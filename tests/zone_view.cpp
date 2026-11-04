/*
 * An offline picture of a zone drawn by the native zone renderer itself (src/zone/draw: the shaders and pipelines the
 * game gets), from the map's data (src/bake/zonemesh) uploaded through gpu_zone on a copy queue as in the game. The
 * objects are the MZB's placements (in the game, the ones it would draw this frame); the light and fog are made up:
 * a day's sun, ambient and fog. It shows the data and the renderer whole.
 *
 *   build\zone_view.exe <map> <out.bmp> [view] [--perpixel] [--night] [--warp]
 *     view: "top" (the whole map from above; the default), "street [yaw]" (from 2.5 yalms over the placements' median
 *     point, looking a little down), or "x y z yaw pitch" (an eye in the game's coordinates, y down; yaw in degrees
 *     from +z toward +x, pitch down)
 *     --perpixel: the renderer's per-pixel option; --night: a dark ambient and the sun low
 *     --alpha1: the pixels whose alpha came out 1 (of 255) painted magenta and counted: World FX takes those for names
 *     facelift drew (grass.hlsl name_at) and leaves them unshaded
 *
 * Needs the game closed (the GPU; or --warp) and the FFXiMain dump (the key tables). Exit code: 0 when written. It
 * prints the load's memory (its peak commit above what was committed before it, the free address space and its largest
 * block before and after) for the texture pack's baseline (docs/texture-pack-plan.md M1).
 *
 * The texture pack (docs/texture-pack/zones.md; M4's pictures):
 *   ZONE_VIEW_PACK=<folder>: the map read in the pack's mode (pivot.ini's overlays but hifi-textures, keys only for the
 *     game's own file: zm::load's pack) and every keyed texture streamed from that pack (read only) as the plugin's
 *     glue asks for it (src/zone/zonepack: the colour record down to the game's size), its view written into the zone's
 *     pack slot and its word set drawn before the picture; it prints what came of them. A map another overlay serves
 *     (Zi'Tah's canopy) gets no pack region: the picture is the one without the pack. With it (M9) every keyed texture
 *     whose entry has a height record gets it too, its whole chain, as the glue asks it (zonepack: the view into the
 *     zone's height block with its relief scale, gpu_zone::height_written), drawn as relief with --perpixel
 *   ZONE_VIEW_HEIGHT=0: no height asked ([textures] height=0): the picture as before M9
 *   ZONE_VIEW_PARALLAX=<depth>: parallax occlusion on the relief at that depth ([textures] parallax_depth; zone::set_parallax)
 *   ZONE_VIEW_SUN="x y z": the day's sun toward there (y down), e.g. "0.9 -0.15 0.4" a low sun for the relief A/B
 *   ZONE_VIEW_BIAS=<b>: the colour maps' extra mip bias (draw_options::texture_bias), for the FSR A/B: e.g. Quality's
 *     -0.585 at ZONE_VIEW_SIZE 2/3 of the full size
 *   HIFIXI_PIVOT_INI: the overlays (zonemesh's dat_core; unset: config\pivot\pivot.ini beside build\, none here)
 */

#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include "../src/bake/zonemesh.hpp"
#include "../src/host/retire.hpp"
#include "../src/pack/memwatch.hpp"
#include "../src/pack/stream.hpp"
#include "../src/zone/draw.hpp"
#include "../src/zone/frame.hpp"
#include "../src/zone/zone_gpu.hpp"
#include "game_dump.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	namespace zm = hifi::bake::zonemesh;

	UINT W = 1600, H = 900; // (ZONE_VIEW_SIZE=WxH: another size, e.g. a player's 2560x1440)

	struct mat4
	{
		float m[16];
	};
	mat4 mul(const mat4 &a, const mat4 &b)
	{
		mat4 o = {};
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				for (int k = 0; k < 4; ++k)
					o.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
		return o;
	}
	// The camera as the game's (right-handed, row vectors, the world's y down: as World FX's end-to-end test's stand-in
	// for the game has it): the view looks down its -z, the projection maps -z_view = near..far to 0..1
	mat4 look_at(const float eye[3], const float at[3], const float up[3])
	{
		float z[3] = { eye[0] - at[0], eye[1] - at[1], eye[2] - at[2] };
		auto norm = [](float *v) {
			const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			v[0] /= l;
			v[1] /= l;
			v[2] /= l;
		};
		auto cross = [](const float *a, const float *b, float *o) {
			o[0] = a[1] * b[2] - a[2] * b[1];
			o[1] = a[2] * b[0] - a[0] * b[2];
			o[2] = a[0] * b[1] - a[1] * b[0];
		};
		norm(z);
		float x[3], y[3];
		cross(up, z, x);
		norm(x);
		cross(z, x, y);
		auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
		return { { x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1 } };
	}
	mat4 perspective(float fov_y, float aspect, float zn, float zf)
	{
		const float ys = 1.0f / std::tan(fov_y / 2), xs = ys / aspect;
		return { { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zn - zf), -1, 0, 0, zn * zf / (zn - zf), 0 } };
	}

	bool write_bmp(const char *path, const uint8_t *rgba, UINT w, UINT h, UINT pitch)
	{
		FILE *f = nullptr;
		if (fopen_s(&f, path, "wb") != 0 || f == nullptr)
			return false;
		uint8_t head[54] = { 'B', 'M' };
		auto put = [&](int at, uint32_t v) { std::memcpy(head + at, &v, 4); };
		put(2, 54 + w * h * 4);
		put(10, 54);
		put(14, 40);
		put(18, w);
		put(22, uint32_t(-int32_t(h))); // (top-down)
		head[26] = 1;
		head[28] = 32;
		put(34, w * h * 4);
		fwrite(head, 1, 54, f);
		std::vector<uint8_t> row(w * 4);
		for (UINT y = 0; y < h; ++y)
		{
			for (UINT x = 0; x < w; ++x)
			{
				const uint8_t *p = rgba + size_t(y) * pitch + x * 4;
				row[x * 4 + 0] = p[2];
				row[x * 4 + 1] = p[1];
				row[x * 4 + 2] = p[0];
				row[x * 4 + 3] = 255;
			}
			fwrite(row.data(), 1, row.size(), f);
		}
		fclose(f);
		return true;
	}

	void norm3(float *v)
	{
		const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		for (int k = 0; k < 3; ++k)
			v[k] /= l;
	}

	namespace st = hifi::pack::stream;

	// The zone's keyed textures from the pack in folder (ZONE_VIEW_PACK), asked as the plugin's glue asks (zonepack.cpp
	// frame and settle: the colour record from its top down to the game's size, the most used first): a ready one's view
	// written into its pack slot, then its word drawn; a missed one's spent. A present every 2 ms (the streamer's pacing)
	// until none is out. The owner is kept, so the views stay valid for the picture. False when the pack couldn't be
	// opened
	bool stream_pack(ID3D12Device *device, hifi::zone::gpu_zone &gz, const std::wstring &folder, st::owner &o)
	{
		using clock = std::chrono::steady_clock;
		const zm::zone &d = gz.data();
		st::start(device, st::kRingDefaultMb);
		const uint32_t before = st::epoch();
		auto t0 = clock::now();
		st::open(folder);
		while (st::epoch() == before && clock::now() - t0 < std::chrono::seconds(60))
			Sleep(5);
		const std::shared_ptr<const hifi::pack::index> idx = st::current();
		if (idx == nullptr)
		{
			std::printf("the texture pack: %s\n", st::open_status().empty() ? "not opened in 60 s" : st::open_status().c_str());
			return false;
		}
		std::printf("the texture pack: %zu entries (zones %u, models %u), %.1f GB of colour, opened in %.0f ms%s%s\n", idx->entries.size(), idx->by_source[1],
			idx->by_source[2], double(idx->colour_bytes) / 1073741824.0, std::chrono::duration<double, std::milli>(clock::now() - t0).count(),
			idx->status.empty() ? "" : "; ", idx->status.c_str());
		if (!gz.has_pack())
		{
			std::printf("map %u: no pack region (%s): the game's or the overlay's textures as without the pack\n", d.map,
				d.overlaid ? "read from another overlay's copy" : "no keyed texture");
			return true;
		}
		const uint32_t epoch = st::epoch();
		o = st::add_owner("zone_view");
		gz.pack().owner = o;
		gz.pack().epoch = epoch;
		gz.pack().tickets.assign(d.textures.size(), st::ticket {});
		gz.pack().height_tickets.assign(d.textures.size(), st::ticket {});
		char hv[8] = "";
		const bool heights = GetEnvironmentVariableA("ZONE_VIEW_HEIGHT", hv, sizeof(hv)) == 0 || hv[0] != '0';
		hifi::zone::gpu_zone::set_heights(heights);
		if (const char *pd = std::getenv("ZONE_VIEW_PARALLAX"); pd != nullptr)
			hifi::zone::set_parallax(float(std::atof(pd)));

		// (the order: the most used first, by the triangles naming each texture times its model's placements)
		std::vector<uint64_t> placed(d.models.size(), 0), uses(d.textures.size(), 0);
		for (const zm::placement &p : d.placements)
			if (p.model < placed.size())
				placed[p.model]++;
		for (size_t m = 0; m < d.models.size(); ++m)
			for (uint32_t k = d.models[m].first_piece; k < d.models[m].first_piece + d.models[m].pieces && k < d.pieces.size(); ++k)
				if (d.pieces[k].texture < uses.size())
					uses[d.pieces[k].texture] += uint64_t(d.pieces[k].triangles) * std::max<uint64_t>(placed[m], 1);
		std::vector<uint32_t> order;
		for (uint32_t i = 0; i < d.textures.size(); ++i)
			if (d.textures[i].key.bytes != 0)
				order.push_back(i);
		std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return uses[a] > uses[b]; });
		t0 = clock::now();
		std::vector<std::pair<uint32_t, uint32_t>> by_slot; // (ticket slot, texture)
		for (size_t rank = 0; rank < order.size(); ++rank)
		{
			const uint32_t i = order[rank];
			const zm::texture &t = d.textures[i];
			st::want w;
			w.k = t.key;
			w.kind = hifi::pack::kColour;
			w.down_to_w = t.key.w;
			w.down_to_h = t.key.h;
			w.priority = int32_t(order.size() - rank);
			w.epoch = epoch;
			const st::ticket tk = st::request(o, w);
			if (tk.generation == 0)
			{
				gz.set_pack_word(i, hifi::zone::gpu_zone::kPackSpent);
				continue;
			}
			gz.pack().tickets[i] = tk;
			by_slot.emplace_back(tk.slot, i);
			gz.set_pack_word(i, hifi::zone::gpu_zone::kPackAsked);
		}
		// (M9: then the heights, as zonepack asks them: an entry with a height record, its whole chain)
		uint32_t heights_asked = 0, heights_drawn = 0;
		for (size_t rank = 0; heights && rank < order.size(); ++rank)
		{
			const uint32_t i = order[rank];
			const zm::texture &t = d.textures[i];
			const hifi::pack::entry *e = idx->find(t.key);
			if (e == nullptr || e->map(hifi::pack::kHeight) == nullptr)
				continue;
			st::want w;
			w.k = t.key;
			w.kind = hifi::pack::kHeight;
			w.priority = int32_t(order.size() - rank);
			w.epoch = epoch;
			const st::ticket tk = st::request(o, w);
			if (tk.generation == 0)
				continue;
			gz.pack().height_tickets[i] = tk;
			by_slot.emplace_back(tk.slot, i | hifi::zone::gpu_zone::kHeightLog);
			gz.set_height_word(i, hifi::zone::gpu_zone::kPackAsked);
			heights_asked++;
		}

		// The tickets as they come: views for the ready ones (the levels counted), the rest spent by why
		uint32_t drawn = 0, levels[8] = {}, bigger = 0;
		uint64_t reasons[size_t(st::miss::count_)] = {}, gone = 0;
		std::vector<std::pair<st::ticket, st::state>> done;
		size_t outstanding = by_slot.size();
		while (outstanding != 0 && clock::now() - t0 < std::chrono::seconds(120))
		{
			st::on_present();
			done.clear();
			st::take_done(o, done);
			for (const auto &[tk, s] : done)
			{
				const auto at = std::find_if(by_slot.begin(), by_slot.end(), [&](const auto &e) { return e.first == tk.slot; });
				if (at == by_slot.end())
					continue;
				if ((at->second & hifi::zone::gpu_zone::kHeightLog) != 0)
				{
					// (a height: its relief scale as zonepack's settle_height makes it)
					const uint32_t i = at->second & ~hifi::zone::gpu_zone::kHeightLog;
					if (!(gz.pack().height_tickets[i] == tk))
						continue;
					outstanding--;
					st::view v;
					float span = 0.0f;
					if (s == st::state::ready && st::get(tk, v))
						std::memcpy(&span, &v.param, 4);
					const float relief = d.textures[i].key.w != 0 ? 4.0f * span * float(v.w) / float(d.textures[i].key.w) : 0.0f;
					if (std::isfinite(relief) && relief > 0.0f && st::write_view(tk, gz.height_cpu(i)))
					{
						gz.height_written(i, relief);
						gz.pack().bytes += v.bytes;
						gz.set_height_word(i, hifi::zone::gpu_zone::kPackDrawn);
						heights_drawn++;
						continue;
					}
					st::drop(tk);
					gz.set_height_word(i, hifi::zone::gpu_zone::kPackSpent);
					continue;
				}
				if (!(gz.pack().tickets[at->second] == tk))
					continue;
				const uint32_t i = at->second;
				outstanding--;
				st::view v;
				if (s == st::state::ready && st::get(tk, v) && st::write_view(tk, gz.pack_cpu(i)))
				{
					gz.pack_written(i);
					gz.pack().bytes += v.bytes;
					gz.set_pack_word(i, hifi::zone::gpu_zone::kPackDrawn);
					drawn++;
					levels[std::min<uint32_t>(v.levels, 7)]++;
					bigger += v.w > d.textures[i].w || v.h > d.textures[i].h;
					continue;
				}
				if (s == st::state::missed)
					reasons[size_t(st::reason(tk))]++;
				else
					gone++;
				st::drop(tk);
				gz.pack().tickets[i] = st::ticket {};
				gz.set_pack_word(i, hifi::zone::gpu_zone::kPackSpent);
			}
			Sleep(2);
		}
		const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
		std::string missed;
		for (size_t m = 1; m < size_t(st::miss::count_); ++m)
			if (reasons[m] != 0)
				missed += std::string(missed.empty() ? "" : ", ") + std::to_string(reasons[m]) + " " + st::text(st::miss(m));
		std::printf("map %u from the pack: %u of %zu textures (%zu keyed) drawn from it, %u of them bigger than the game's; views of 1/2/3/4+ levels %u/%u/%u/%u; missed: %s; "
			"%llu gone, %zu not back; heights %u of %u asked drawn%s; %.1f MB in %.0f ms\n", d.map, drawn, d.textures.size(), order.size(), bigger, levels[1], levels[2], levels[3],
			levels[4] + levels[5] + levels[6] + levels[7], missed.empty() ? "none" : missed.c_str(), static_cast<unsigned long long>(gone), outstanding,
			heights_drawn, heights_asked, heights ? "" : " (ZONE_VIEW_HEIGHT=0)", double(gz.pack().bytes) / 1048576.0, ms);
		for (const std::string &l : st::status())
			std::printf("  %s\n", l.c_str());
		return true;
	}
}

// (HiFi XI's options and World FX's settings: not looked at)
extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		std::printf("zone_view <map> <out.bmp> [top | street [yaw] | x y z yaw pitch] [--perpixel] [--night] [--alpha1] [--warp]\n");
		return 1;
	}
	const uint32_t map = uint32_t(std::strtoul(argv[1], nullptr, 10));
	if (const char *size = std::getenv("ZONE_VIEW_SIZE"); size != nullptr && std::sscanf(size, "%ux%u", &W, &H) != 2)
		W = 1600, H = 900;
	const char *out = argv[2];
	bool warp = false, per_pixel = false, night = false, top = true, street = false, alpha1 = false;
	float eye[3] = {}, yaw = 0, tilt = 0;
	std::vector<std::string> rest;
	for (int i = 3; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--warp")
			warp = true;
		else if (a == "--perpixel")
			per_pixel = true;
		else if (a == "--night")
			night = true;
		else if (a == "--alpha1")
			alpha1 = true;
		else if (a == "street")
			street = true;
		else if (a != "top")
			rest.push_back(a);
	}
	if (rest.size() == 5)
	{
		top = false;
		for (int k = 0; k < 3; ++k)
			eye[k] = std::strtof(rest[k].c_str(), nullptr);
		yaw = std::strtof(rest[3].c_str(), nullptr) * 0.0174533f;
		tilt = std::strtof(rest[4].c_str(), nullptr) * 0.0174533f;
	}

	uintptr_t base = 0;
	// (the other dump when the first's address is taken in this process)
	if (dump::map(dump::kDumps[0], base) == 0 && dump::map(dump::kDumps[1], base) == 0)
	{
		std::printf("the FFXiMain dump couldn't be mapped\n");
		return 1;
	}
	zm::zone z;
	hifi::bake::result r;
	// (the load's memory, for the texture pack's baseline: its peak commit above what the process had committed before it
	// (the CRT, the FFXiMain dump), and the largest free block before and after)
	const hifi::pack::memwatch::walk_result va_before = hifi::pack::memwatch::walk();
	PROCESS_MEMORY_COUNTERS pm_before = {};
	pm_before.cb = sizeof(pm_before);
	GetProcessMemoryInfo(GetCurrentProcess(), &pm_before, sizeof(pm_before));
	// (ZONE_VIEW_PACK: the texture pack's mode; without it the load as it always was)
	wchar_t pack_folder[MAX_PATH] = L"";
	const bool pack = GetEnvironmentVariableW(L"ZONE_VIEW_PACK", pack_folder, MAX_PATH) != 0;
	if (!(pack ? zm::load(map, z, r, true) : zm::load(map, z, r)))
	{
		std::printf("map %u: %s\n", map, r.why.c_str());
		return 1;
	}
	{
		const hifi::pack::memwatch::walk_result va_after = hifi::pack::memwatch::walk();
		PROCESS_MEMORY_COUNTERS pm = {};
		pm.cb = sizeof(pm);
		GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm));
		// (the process's peak is since it started: the load's own only when it came during the load, which it says)
		const bool in_load = pm.PeakPagefileUsage > pm_before.PeakPagefileUsage;
		std::printf("loaded: the load's peak commit %.0f MB above the %.0f MB before it%s (now %.0f MB above; the process's peak %.0f MB); free address space "
			"%.0f -> %.0f MB, largest block %.0f -> %.0f MB\n", (double(pm.PeakPagefileUsage) - double(pm_before.PagefileUsage)) / 1048576.0,
			pm_before.PagefileUsage / 1048576.0, in_load ? "" : " (at most: the process's peak came before the load)",
			(double(pm.PagefileUsage) - double(pm_before.PagefileUsage)) / 1048576.0, pm.PeakPagefileUsage / 1048576.0, va_before.free / 1048576.0,
			va_after.free / 1048576.0, va_before.largest / 1048576.0, va_after.largest / 1048576.0);
	}

	// The device and queues
	IDXGIFactory4 *factory = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	IDXGIAdapter *adapter = nullptr;
	if (warp)
		factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
	ID3D12Device *device = nullptr;
	if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
	{
		std::printf("no Direct3D 12 device\n");
		return 1;
	}
	ID3D12CommandQueue *direct = nullptr, *copy = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&direct));
	qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&copy));

	// The zone, as the game side will upload it; the renderer
	hifi::zone::gpu_zone gz;
	std::string why;
	if (!gz.upload(device, copy, std::move(z), why) || !gz.wait(10000))
	{
		std::printf("upload: %s\n", why.empty() ? "not finished in 10 s" : why.c_str());
		return 1;
	}
	st::owner pack_owner = 0;
	if (pack && !stream_pack(device, gz, pack_folder, pack_owner))
		return 1;
	const zm::zone &d = gz.data();
#ifdef ZONE_GBUF
	// (with the buffers: each piece's first index, texture and model, for the mock-up's materials)
	if (FILE *pf = nullptr; fopen_s(&pf, (std::string(out) + ".pieces.txt").c_str(), "w") == 0 && pf != nullptr)
	{
		for (const zm::model &m : d.models)
			for (uint32_t k = m.first_piece; k < m.first_piece + m.pieces; ++k)
			{
				const zm::piece &p = d.pieces[k];
				std::fprintf(pf, "%u\t%.16s\t%.16s\t%u\n", p.first_index, p.texture < d.textures.size() ? d.textures[p.texture].name : "-", m.name, p.bits);
			}
		std::fclose(pf);
	}
#endif
	hifi::zone::zone_renderer renderer;
	auto t0 = std::chrono::steady_clock::now();
	if (!renderer.init(device, why))
	{
		std::printf("renderer: %s\n", why.c_str());
		return 1;
	}
	const double init_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

	// The picture's target and depth (the world's formats: R8G8B8A8, D24S8), and a readback
	D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, rb = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = W;
	td.Height = H;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.SampleDesc.Count = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	float sky[4] = { 0.55f, 0.65f, 0.78f, 1.0f };
	D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_R8G8B8A8_UNORM, { sky[0], sky[1], sky[2], sky[3] } };
	ID3D12Resource *target = nullptr, *depth = nullptr, *readback = nullptr;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&target));
	td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	D3D12_CLEAR_VALUE dv = {};
	dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dv.DepthStencil.Depth = 1.0f;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv, IID_PPV_ARGS(&depth));
	const UINT pitch = W * 4;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = UINT64(pitch) * H;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
	ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));
	hifi::zone::draw_target t;
	t.rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	t.dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(target, nullptr, t.rtv);
	device->CreateDepthStencilView(depth, nullptr, t.dsv);
	t.rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	t.dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	t.width = float(W);
	t.height = float(H);

	// The camera: from above the whole map, from the street, or the given eye
	float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
	for (const zm::placement &p : d.placements)
		for (int k = 0; k < 3; ++k)
		{
			lo[k] = std::min(lo[k], p.t[k]);
			hi[k] = std::max(hi[k], p.t[k]);
		}
	float at[3];
	const float up[3] = { 0, -1, 0 };
	if (street)
	{
		// (the placements' median, 2.5 yalms over it, looking along the yaw a little down)
		std::vector<float> c[3];
		for (const zm::placement &p : d.placements)
			for (int k = 0; k < 3; ++k)
				c[k].push_back(p.t[k]);
		for (int k = 0; k < 3; ++k)
		{
			std::nth_element(c[k].begin(), c[k].begin() + c[k].size() / 2, c[k].end());
			eye[k] = c[k][c[k].size() / 2];
		}
		eye[1] -= 2.5f;
		top = false;
		yaw = std::strtof(rest.size() == 1 ? rest[0].c_str() : "90", nullptr) * 0.0174533f;
		tilt = 0.12f;
	}
	if (top)
	{
		const float cx = (lo[0] + hi[0]) / 2, cz = (lo[2] + hi[2]) / 2, span = std::max(hi[0] - lo[0], hi[2] - lo[2]);
		at[0] = cx;
		at[1] = (lo[1] + hi[1]) / 2;
		at[2] = cz;
		eye[0] = cx;
		eye[1] = at[1] - span * 0.75f;
		eye[2] = cz - span * 0.55f;
	}
	else
	{
		at[0] = eye[0] + std::sin(yaw) * std::cos(tilt);
		at[1] = eye[1] + std::sin(tilt);
		at[2] = eye[2] + std::cos(yaw) * std::cos(tilt);
	}
	const mat4 vp = mul(look_at(eye, at, up), perspective(top ? 0.9f : 1.0f, float(W) / H, 0.5f, 20000.0f));

	// The frame: every placement (the nearest first when there are more than a pass holds), a day's light and fog
	hifi::zone::frame &f = hifi::zone::frame_slot(1);
	f.native = true;
	f.zone = &gz;
	f.map = map;
	std::memcpy(f.view_proj, vp.m, sizeof(vp.m));
	std::memcpy(f.camera, eye, sizeof(eye));
	float sun_dir[3] = { 0.35f, night ? -0.25f : -0.85f, 0.4f };
	// (ZONE_VIEW_SUN="x y z": the day's sun from elsewhere, toward it, y down: e.g. a low sun for M9's relief A/B)
	if (const char *s = std::getenv("ZONE_VIEW_SUN"); s != nullptr)
		std::sscanf(s, "%f %f %f", &sun_dir[0], &sun_dir[1], &sun_dir[2]);
	norm3(sun_dir);
	std::memcpy(f.sun_dir, sun_dir, sizeof(sun_dir));
	float sun[3] = { 1.26f, 1.25f, 1.1f }, dusk[3] = { 0.35f, 0.38f, 0.55f };
	float ambient[3] = { 0.49f, 0.47f, 0.49f }, dark[3] = { 0.18f, 0.2f, 0.3f };
#ifdef ZONE_GBUF
	// (the mock-ups' light: ZONE_VIEW_LIGHT="sun x y z  sun r g b  ambient r g b  sky r g b")
	if (const char *l = std::getenv("ZONE_VIEW_LIGHT"); l != nullptr)
	{
		std::sscanf(l, "%f %f %f %f %f %f %f %f %f %f %f %f", &sun_dir[0], &sun_dir[1], &sun_dir[2], &sun[0], &sun[1], &sun[2], &ambient[0], &ambient[1], &ambient[2], &sky[0], &sky[1], &sky[2]);
		norm3(sun_dir);
		std::memcpy(f.sun_dir, sun_dir, sizeof(sun_dir));
	}
#endif
	std::memcpy(f.sun, night ? dusk : sun, 12);
	std::memcpy(f.sun_bump, night ? dusk : sun, 12);
	std::memcpy(f.ambient, night ? dark : ambient, 12);
	std::memcpy(f.ambient_bump, night ? dark : ambient, 12);
	std::memcpy(f.fog, sky, 12);
	f.fog_start = top ? 1e5f : 150.0f;
	f.fog_end = top ? 2e5f : 600.0f;
	f.fog_scale = 1.0f;
	std::vector<const zm::placement *> order;
	for (const zm::placement &p : d.placements)
		order.push_back(&p);
	auto d2 = [&](const zm::placement *p) {
		const float a = p->t[0] - eye[0], b = p->t[1] - eye[1], c = p->t[2] - eye[2];
		return a * a + b * b + c * c;
	};
	if (order.size() > hifi::zone::kMaxObjects)
	{
		std::nth_element(order.begin(), order.begin() + hifi::zone::kMaxObjects, order.end(), [&](auto *a, auto *b) { return d2(a) < d2(b); });
		order.resize(hifi::zone::kMaxObjects);
	}
	f.count = 0;
	for (const zm::placement *p : order)
	{
		hifi::zone::frame_object &o = f.objects[f.count++];
		for (int k = 0; k < 3; ++k)
		{
			o.world[k * 4 + 0] = p->m[0 * 3 + k];
			o.world[k * 4 + 1] = p->m[1 * 3 + k];
			o.world[k * 4 + 2] = p->m[2 * 3 + k];
			o.world[k * 4 + 3] = p->t[k];
		}
		o.model = p->model;
		o.flags = p->flags;
		o.sway = 0.0f;
		for (uint32_t &l : o.lamps)
			l = 0xFFFFFFFFu;
	}
	f.lamp_count = 0;
	hifi::zone::publish(f, 1);

	// Drawn
	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *list = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
	list->ClearRenderTargetView(t.rtv, sky, 0, nullptr);
	list->ClearDepthStencilView(t.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	hifi::zone::draw_options o;
	o.per_pixel = per_pixel;
	if (char b[32] = ""; GetEnvironmentVariableA("ZONE_VIEW_BIAS", b, sizeof(b)) != 0)
		o.texture_bias = std::strtof(b, nullptr);
	t0 = std::chrono::steady_clock::now();
	const hifi::zone::draw_stats st = renderer.record(list, t, *hifi::zone::published(1), o, 1);
	const double record_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Transition.pResource = target;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	list->ResourceBarrier(1, &barrier);
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	dst.pResource = readback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, pitch };
	src.pResource = target;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	list->Close();
	ID3D12CommandList *lists[] = { list };
	t0 = std::chrono::steady_clock::now();
	direct->ExecuteCommandLists(1, lists);
	ID3D12Fence *fence = nullptr;
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	direct->Signal(fence, 1);
	HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	fence->SetEventOnCompletion(1, e);
	WaitForSingleObject(e, 30000);
	const double gpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (device->GetDeviceRemovedReason() != S_OK)
	{
		std::printf("the device was removed (0x%08lX)\n", device->GetDeviceRemovedReason());
		return 1;
	}
	uint8_t *pixels = nullptr;
	readback->Map(0, nullptr, reinterpret_cast<void **>(&pixels));
	uint32_t ones = 0;
	if (alpha1)
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				uint8_t *q = pixels + size_t(y) * pitch + x * 4;
				if (q[3] == 1)
				{
					q[0] = 255;
					q[1] = 0;
					q[2] = 255;
					ones++;
				}
			}
	if (alpha1)
		std::printf("%u pixels with alpha 1\n", ones);
	const bool written = write_bmp(out, pixels, W, H, pitch);
	std::printf("map %u from %.1f %.1f %.1f: renderer made in %.0f ms; %u objects, %u draws, %u triangles (%u skipped), recorded in %.1f ms, drawn in %.0f ms -> %s\n",
		map, eye[0], eye[1], eye[2], init_ms, st.objects, st.draws, st.triangles, st.skipped, record_ms, gpu_ms, written ? out : "NOT WRITTEN");
	if (pack)
	{
		// (the frame is done: the owner's textures handed to retire, the streamer joined, all of it freed)
		readback->Unmap(0, nullptr);
		if (pack_owner != 0)
			hifi::pack::stream::drop_owner(pack_owner);
		gz.pack().owner = 0;
		hifi::pack::stream::stop();
		hifi::retire::release_all();
	}
	dump::unmap(base);
	return written ? 0 : 1;
}
