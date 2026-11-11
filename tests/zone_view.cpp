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
 *   ZONE_VIEW_FX=old|native: World FX's shading over the zone as in game (src/render: the sun's cascades from the zone's
 *     own meshes, src/zone/zoneshadow; the light split): "old" its sun shadows darkened onto the finished picture by its
 *     shading pass, "native" looked up by the zone pass itself ([zone] nativeshadows, src/render/sun_shadows.hpp). Two
 *     frames, the second the picture (the zone pass reads what World FX drew the frame before, as in game). No ambient
 *     occlusion or relief; ZONE_VIEW_SHADOW=<strength> the sun shadows' strength (0.55, the High preset's);
 *     ZONE_VIEW_CLOUDS: unused (no weather here, so no clouds' shadows). With ZONE_VIEW_LAMPS, "native" also has the
 *     zone pass add World FX's lamp glow itself ([zone] nativelamps, sun_shadows.hpp's lamps; no characters here: no
 *     capsules), and both modes run 28 frames before the picture's, so the lamps' cubes are drawn (a face a frame) as
 *     they are in game after a moment; "native-shadows" and "native-lamps": only the one of the two; "native-faithful":
 *     both, the lamps' glow as World FX's own, ported (sun_shadows.hpp's kLookFaithful; "native" is the tuned look).
 *     ZONE_VIEW_BODY="x y z size": a character standing there (its capsule: the lamps' shadows); ZONE_VIEW_SMALLROOM=1:
 *     lit as a Mog House; ZONE_VIEW_FXLAMPS=0: World FX given no lamps; ZONE_VIEW_REPEAT=<n>: the zone pass n times over
 *   ZONE_VIEW_GRASS=<folder> (with ZONE_VIEW_FX): the zone's baked grass (<map>.bin: bootloader\worldfx-grass) drawn by
 *     World FX as in game, the blades at the zone's end and the rest after (their colour from the ground's map, drawn
 *     from the zone with the frame's light); with a "native" mode they light themselves from World FX's block and write
 *     the light split's words (sun_shadows.hpp "The grass"). ZONE_VIEW_GRASS_AFTER=1: the blades after the shading pass
 *     in one draw; ZONE_VIEW_GRASS_DISTANCE=<yalms>. The blades' GPU time is printed with the passes'.
 */

#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include "../src/bake/zonemesh.hpp"
#include "../src/host/retire.hpp"
#include "../src/pack/memwatch.hpp"
#include "../src/pack/stream.hpp"
#include "../src/render/grass_render.hpp"
#include "../src/render/lamp_glow.hpp"
#include "../src/render/sun_shadows.hpp"
#include "../src/zone/draw.hpp"
#include "../src/zone/frame.hpp"
#include "../src/zone/light_split.hpp"
#include "../src/zone/zone_gpu.hpp"
#include "../src/zone/zoneshadow.hpp"
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
	const mat4 view = look_at(eye, at, up), proj = perspective(top ? 0.9f : 1.0f, float(W) / H, 0.5f, 20000.0f);
	const mat4 vp = mul(view, proj);

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
	// (ZONE_VIEW_LAMPS=<folder>: the zone's lamps from its bake there, <map>.bin (LMP2: bootloader\worldfx-lamps), all
	// fully lit as at night. The zone pass gets the 64 nearest (the game's light: colour x brightness, Attenuation2 0.5),
	// each object the 4 strongest whose range reaches its box (the game names each object's own: not known here);
	// World FX the 16 nearest of range 20 or less, as worldfx_core's pick_lamps makes them at the default settings:
	// reach clamp(range, 1, 12) x 1.5, colour x lamp_glow_strength(0.5), lamp shadows 0.5)
	std::vector<float> fx_lamps;
	if (wchar_t lf[MAX_PATH] = L""; GetEnvironmentVariableW(L"ZONE_VIEW_LAMPS", lf, MAX_PATH) != 0)
	{
		grass::blade_file file;
		std::string lamps_why;
		const std::wstring path = std::wstring(lf) + L"\\" + std::to_wstring(map) + L".bin";
		if (!grass::read_blade_file(path.c_str(), file, lamps_why, "LMP2", 12))
		{
			std::printf("ZONE_VIEW_LAMPS: %s\n", lamps_why.c_str());
			return 1;
		}
		struct baked { float x, y, z, range, r, g, b, brightness; uint32_t id, slot, flags; float window; };
		std::vector<baked> all;
		for (uint32_t i = 0; i < file.blade_count(); ++i)
		{
			baked b;
			std::memcpy(&b, &file.blades[size_t(i) * 12], sizeof(b));
			if ((b.flags & 1) == 0 && std::isfinite(b.x) && std::isfinite(b.range) && b.range > 0.0f && b.brightness > 0.0f)
				all.push_back(b);
		}
		std::sort(all.begin(), all.end(), [&](const baked &a, const baked &b) {
			const float da = (a.x - eye[0]) * (a.x - eye[0]) + (a.y - eye[1]) * (a.y - eye[1]) + (a.z - eye[2]) * (a.z - eye[2]);
			const float db = (b.x - eye[0]) * (b.x - eye[0]) + (b.y - eye[1]) * (b.y - eye[1]) + (b.z - eye[2]) * (b.z - eye[2]);
			return da < db;
		});
		for (const baked &b : all)
		{
			if (f.lamp_count < hifi::zone::kMaxLamps)
			{
				hifi::zone::frame_lamp &l = f.lamps[f.lamp_count++];
				l.pos[0] = b.x, l.pos[1] = b.y, l.pos[2] = b.z;
				l.range = b.range;
				l.colour[0] = b.r * b.brightness, l.colour[1] = b.g * b.brightness, l.colour[2] = b.b * b.brightness;
				l.attenuation2 = 0.5f;
			}
			if (b.range <= 20.0f && fx_lamps.size() < 16 * 8)
			{
				const float s = grass::lamp_glow_strength(0.5f, b.brightness, b.brightness);
				const float o[8] = { b.x, b.y, b.z, std::clamp(b.range, 1.0f, 12.0f) * 1.5f, b.r * s, b.g * s, b.b * s, 1.0f };
				fx_lamps.insert(fx_lamps.end(), o, o + 8);
			}
		}
		for (uint32_t i = 0; i < f.count; ++i)
		{
			hifi::zone::frame_object &o = f.objects[i];
			const zm::model &mo = d.models[o.model];
			std::pair<float, uint32_t> best[4] = { { 0.0f, 0xFFFFFFFFu }, { 0.0f, 0xFFFFFFFFu }, { 0.0f, 0xFFFFFFFFu }, { 0.0f, 0xFFFFFFFFu } };
			for (uint32_t k = 0; k < f.lamp_count; ++k)
			{
				const hifi::zone::frame_lamp &l = f.lamps[k];
				float gap2 = 0.0f;
				for (int a = 0; a < 3; ++a)
				{
					const float *row = o.world + a * 4;
					const float c[3] = { 0.5f * (mo.lo[0] + mo.hi[0]), 0.5f * (mo.lo[1] + mo.hi[1]), 0.5f * (mo.lo[2] + mo.hi[2]) };
					const float h[3] = { 0.5f * (mo.hi[0] - mo.lo[0]), 0.5f * (mo.hi[1] - mo.lo[1]), 0.5f * (mo.hi[2] - mo.lo[2]) };
					const float centre = c[0] * row[0] + c[1] * row[1] + c[2] * row[2] + row[3];
					const float half = std::fabs(row[0]) * h[0] + std::fabs(row[1]) * h[1] + std::fabs(row[2]) * h[2];
					const float gap = std::max(0.0f, std::fabs(l.pos[a] - centre) - half);
					gap2 += gap * gap;
				}
				if (gap2 >= l.range * l.range)
					continue;
				std::pair<float, uint32_t> c = { (0.2126f * l.colour[0] + 0.7152f * l.colour[1] + 0.0722f * l.colour[2]) / std::max(0.5f * gap2, 1e-3f), k };
				for (auto &b : best)
					if (c.first > b.first)
						std::swap(c, b);
			}
			for (int k = 0; k < 4; ++k)
				o.lamps[k] = best[k].second;
		}
		std::printf("ZONE_VIEW_LAMPS: %zu lamps baked, %u to the zone pass, %zu to World FX\n", all.size(), f.lamp_count, fx_lamps.size() / 8);
		// (the nearest few, for placing a camera by one: x y z, range)
		for (size_t k = 0; k < all.size() && k < 6; ++k)
			std::printf("  lamp %zu: %.1f %.1f %.1f, range %.1f\n", k, all[k].x, all[k].y, all[k].z, all[k].range);
	}
	hifi::zone::publish(f, 1);

	// (ZONE_VIEW_FX: World FX over it, its cascades from the zone's own meshes, the light split for its shading pass)
	std::string fx_mode;
	if (char fm[16] = ""; GetEnvironmentVariableA("ZONE_VIEW_FX", fm, sizeof(fm)) != 0)
		fx_mode = fm;
	const bool fx = fx_mode == "old" || fx_mode == "native" || fx_mode == "native-shadows" || fx_mode == "native-lamps" || fx_mode == "native-faithful";
	grass::renderer wfx;
	hifi::zone::zone_shadow casters;
	struct caster_context
	{
		hifi::zone::gpu_zone *zone;
		hifi::zone::zone_shadow *shadow;
		float env[12];
	} cc = { &gz, &casters, {} };
	if (fx)
	{
		std::string error;
		if (!wfx.init(device, error) || !casters.init(device, why))
		{
			std::printf("World FX: %s%s\n", error.c_str(), why.c_str());
			return 1;
		}
		// (ZONE_VIEW_SHADER=<file>: World FX's shaders from that source instead of the built-in ones, for trying a change)
		if (const char *sf = std::getenv("ZONE_VIEW_SHADER"); sf != nullptr)
		{
			std::string source;
			if (FILE *file = nullptr; fopen_s(&file, sf, "rb") == 0 && file != nullptr)
			{
				char buffer[65536];
				size_t n;
				while ((n = fread(buffer, 1, sizeof(buffer), file)) != 0)
					source.append(buffer, n);
				fclose(file);
			}
			if (source.empty() || !wfx.set_shader_source(source, error))
			{
				std::printf("ZONE_VIEW_SHADER: %s\n", source.empty() ? "not read" : error.c_str());
				return 1;
			}
		}
		wfx.set_zone_casters(
			[](void *c, float lo[3], float hi[3], uint64_t *generation) {
				*generation = 1;
				return static_cast<caster_context *>(c)->shadow->bounds(*static_cast<caster_context *>(c)->zone, lo, hi);
			},
			[](void *c, ID3D12GraphicsCommandList *l, const float m[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size) {
				return static_cast<caster_context *>(c)->shadow->record(l, *static_cast<caster_context *>(c)->zone, m, dsv, size);
			},
			&cc);
		wfx.set_light_source([](void *) { return hifi::light::take(); }, nullptr);
		// (the ground's colour map for the grass, from the zone as the plugin's floor_hook draws it: the frame's light)
		for (int k = 0; k < 3; ++k)
		{
			cc.env[k] = f.sun_dir[k];
			cc.env[4 + k] = f.sun[k];
			cc.env[8 + k] = f.ambient[k];
		}
		wfx.set_floor_source(
			[](void *c, ID3D12GraphicsCommandList *l, const float m[16], D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size) {
				caster_context *const x = static_cast<caster_context *>(c);
				return x->shadow->record(l, *x->zone, m, dsv, size, false, x->env, rtv);
			},
			&cc);
		hifi::sun::set_native(fx_mode == "native" || fx_mode == "native-shadows" || fx_mode == "native-faithful");
		hifi::sun::set_native_lamps(fx_mode == "native" || fx_mode == "native-lamps" || fx_mode == "native-faithful");
		// ("native-faithful": the lamps' glow as World FX's, ported; "native": the tuned look, the plugin's)
		hifi::sun::set_lamp_look(fx_mode == "native-faithful" ? hifi::sun::kLookFaithful : hifi::sun::kLookTuned);
		(void)hifi::light::take(); // (asked: the zone pass draws it from the first frame)
	}
	grass::camera cam;
	std::memcpy(cam.view, view.m, sizeof(cam.view));
	std::memcpy(cam.proj, proj.m, sizeof(cam.proj));
	grass::look fl;
	fl.grass = false;
	fl.water = false;
	fl.sea = false;
	fl.fireflies = 0.0f;
	// (ZONE_VIEW_FXLAMPS=0: World FX given no lamps, the zone pass's own kept: what the glow adds, against the same with it)
	if (char fxl[8] = ""; GetEnvironmentVariableA("ZONE_VIEW_FXLAMPS", fxl, sizeof(fxl)) != 0 && fxl[0] == '0')
		fx_lamps.clear();
	if (!fx_lamps.empty())
	{
		fl.lamps = fx_lamps.data();
		fl.lamp_count = uint32_t(fx_lamps.size() / 8);
		fl.lamp_shadows = 0.5f;
	}
	// (ZONE_VIEW_BODY="x y z size": a character standing there for World FX, its feet and size as the add-on hands them:
	// nothing of it is drawn, its capsule alone shadows the lamps for the zone pass, sun_shadows.hpp's lamps)
	float body[4] = {};
	if (const char *bv = std::getenv("ZONE_VIEW_BODY"); bv != nullptr && std::sscanf(bv, "%f %f %f %f", &body[0], &body[1], &body[2], &body[3]) == 4)
	{
		fl.actors = body;
		fl.actor_count = 1;
	}
	// (ZONE_VIEW_SMALLROOM=1: as World FX has a Mog House: its glow 0.3 as strong, worldfx_core's kMogHouseGlow, and the
	// native passes told to keep that glow's own size and look)
	if (GetEnvironmentVariableA("ZONE_VIEW_SMALLROOM", nullptr, 0) != 0)
	{
		fl.lamp_small_room = true;
		for (size_t k = 0; k + 7 < fx_lamps.size(); k += 8)
			for (int c = 4; c < 7; ++c)
				fx_lamps[k + c] *= 0.3f;
	}
	std::memcpy(fl.sun, sun_dir, sizeof(fl.sun));
	fl.sun_shadow = 0.55f;
	if (char sv[16] = ""; GetEnvironmentVariableA("ZONE_VIEW_SHADOW", sv, sizeof(sv)) != 0)
		fl.sun_shadow = std::strtof(sv, nullptr);
	fl.sun_map = true;
	fl.sun_characters = true;
	fl.night = night ? 0.5f : 0.0f;
	// (ZONE_VIEW_GRASS=<folder>: the zone's baked grass from there, <map>.bin (GRS2: bootloader\worldfx-grass), drawn by
	// World FX as in game: the blades at the zone's end into the world and its depth, with the light split for their
	// words while the native passes' block is in use, then the rest of World FX over all of it. ZONE_VIEW_GRASS_AFTER=1:
	// the blades after the shading pass instead, in the one draw (no marker). ZONE_VIEW_GRASS_DISTANCE=<yalms>, 40.)
	bool grass_on = false, grass_after = GetEnvironmentVariableA("ZONE_VIEW_GRASS_AFTER", nullptr, 0) != 0;
	if (wchar_t gf[MAX_PATH] = L""; fx && GetEnvironmentVariableW(L"ZONE_VIEW_GRASS", gf, MAX_PATH) != 0)
	{
		grass::blade_file file;
		std::string grass_why;
		const std::wstring path = std::wstring(gf) + L"\\" + std::to_wstring(map) + L".bin";
		if (!grass::read_blade_file(path.c_str(), file, grass_why) || !wfx.upload(file, grass_why))
		{
			std::printf("ZONE_VIEW_GRASS: %s\n", grass_why.c_str());
			return 1;
		}
		grass_on = true;
		fl.grass = true;
		fl.time = 3.0f;
		if (const char *gd = std::getenv("ZONE_VIEW_GRASS_DISTANCE"); gd != nullptr)
			fl.distance = std::strtof(gd, nullptr);
		std::printf("ZONE_VIEW_GRASS: %u points baked\n", file.blade_count());
	}
	// The blades at the zone's end (after the zone pass, before World FX's own draw): grass::look::phase_t::blades
	uint32_t blade_native = 0, blades_drawn = 0;
	bool blade_words = false;
	auto blades_at_zone_end = [&](ID3D12GraphicsCommandList *l) {
		if (!grass_on || grass_after)
			return;
		grass::look bl = fl;
		bl.phase = grass::look::phase_t::blades;
		grass::target bt;
		bt.rt = target;
		bt.ds = depth;
		bt.rtv = t.rtv;
		bt.rtv_format = t.rtv_format;
		bt.dsv_format = t.dsv_format;
		bt.blades_into_world_depth = true;
		const bool split = hifi::sun::marked() && hifi::light::begin(l, device, target, bt.light_rtv);
		if (!split)
			bt.light_rtv = {};
		std::string error;
		const grass::draw_stats bs = wfx.draw(l, bt, cam, bl, error);
		if (split)
			hifi::light::end(l);
		if (!error.empty())
			std::printf("the blades: %s\n", error.c_str());
		blade_native = bs.blade_native;
		blade_words = bs.blade_words;
		blades_drawn = bs.blades;
	};
	if (grass_on && !grass_after)
		fl.phase = grass::look::phase_t::rest;
	grass::target gt;
	gt.rt = target;
	gt.ds = depth;
	gt.rtv = t.rtv;
	gt.rtv_format = t.rtv_format;
	gt.dsv_format = t.dsv_format;
	gt.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
	gt.ds_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;

	// Drawn (with World FX: two frames, the picture the second's)
	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *list = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
	hifi::zone::draw_options o;
	o.per_pixel = per_pixel;
	if (char b[32] = ""; GetEnvironmentVariableA("ZONE_VIEW_BIAS", b, sizeof(b)) != 0)
		o.texture_bias = std::strtof(b, nullptr);
	ID3D12Fence *fence = nullptr;
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	hifi::zone::draw_stats st;
	double record_ms = 0.0;
	// (with lamps: frames enough for World FX to keep room for the cubes, then draw the four nearest lamps' six faces)
	const uint64_t last = fx ? (fx_lamps.empty() ? 2 : 30) : 1;
	uint32_t lamp_faces = 0, lamp_copies = 0;
	// (each frame timed on the GPU: timestamps before the zone pass, after it, after World FX; the frames before the
	// picture's are the same frame over again, so the least of them is the pass's time with nothing else in the way)
	ID3D12QueryHeap *stamps = nullptr;
	ID3D12Resource *stamps_back = nullptr;
	UINT64 stamp_hz = 0;
	{
		D3D12_QUERY_HEAP_DESC qd = {};
		qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		qd.Count = 4;
		D3D12_HEAP_PROPERTIES qh = {};
		qh.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC qb = {};
		qb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		qb.Width = 4 * sizeof(UINT64);
		qb.Height = 1;
		qb.DepthOrArraySize = 1;
		qb.MipLevels = 1;
		qb.SampleDesc.Count = 1;
		qb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(direct->GetTimestampFrequency(&stamp_hz)) || stamp_hz == 0 || FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&stamps))) ||
			FAILED(device->CreateCommittedResource(&qh, D3D12_HEAP_FLAG_NONE, &qb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&stamps_back))))
			stamps = nullptr;
	}
	std::vector<double> zone_ms, blade_ms, fx_ms; // (the blades at the zone's end between the two: ZONE_VIEW_GRASS)
	auto stamps_read = [&] {
		UINT64 *v = nullptr;
		const D3D12_RANGE all = { 0, 4 * sizeof(UINT64) };
		if (stamps == nullptr || FAILED(stamps_back->Map(0, &all, reinterpret_cast<void **>(&v))))
			return;
		zone_ms.push_back(1000.0 * double(v[1] - v[0]) / double(stamp_hz));
		blade_ms.push_back(1000.0 * double(v[2] - v[1]) / double(stamp_hz));
		fx_ms.push_back(1000.0 * double(v[3] - v[2]) / double(stamp_hz));
		stamps_back->Unmap(0, nullptr);
	};
	for (uint64_t n = 1; fx && n < last; ++n)
	{
		list->ClearRenderTargetView(t.rtv, sky, 0, nullptr);
		list->ClearDepthStencilView(t.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		hifi::zone::draw_target ft = t;
		const bool split = hifi::light::begin(list, device, target, ft.light_rtv);
		if (stamps != nullptr)
			list->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 0);
		renderer.record(list, ft, *hifi::zone::published(1), o, n);
		if (split)
			hifi::light::end(list);
		if (stamps != nullptr)
			list->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 1);
		blades_at_zone_end(list);
		if (stamps != nullptr)
			list->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 2);
		std::string error;
		const grass::draw_stats fs = wfx.draw(list, gt, cam, fl, error);
		if (stamps != nullptr)
		{
			list->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 3);
			list->ResolveQueryData(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, stamps_back, 0);
		}
		lamp_faces += fs.lamp_map_faces;
		lamp_copies += fs.lamp_native_copies;
		if (n == 1 || n + 1 == last || !error.empty())
			std::printf("World FX frame %llu: %s%s, sun map %s (%u cascades), light split %s; lamps %u, their cubes %u (%u faces drawn so far, %u cubes copied for the zone pass)\n",
				(unsigned long long)n, fs.sun ? "shaded" : "not shaded", error.empty() ? "" : (" (" + error + ")").c_str(), fs.sun_map ? "read" : "not read",
				fs.sun_map_cascades, split ? "drawn" : "not drawn", fs.lamps, fs.lamp_maps, lamp_faces, lamp_copies);
		list->Close();
		ID3D12CommandList *one[] = { list };
		direct->ExecuteCommandLists(1, one);
		direct->Signal(fence, n);
		fence->SetEventOnCompletion(n, e);
		WaitForSingleObject(e, 60000);
		stamps_read();
		alloc->Reset();
		list->Reset(alloc, nullptr);
		wfx.release_old(0);
		// (the block World FX published counts as fresh for the next frame however long WARP took)
		{
			std::lock_guard<std::mutex> lock(hifi::sun::detail::g.mutex);
			hifi::sun::detail::g.at = GetTickCount64();
		}
	}
	list->ClearRenderTargetView(t.rtv, sky, 0, nullptr);
	list->ClearDepthStencilView(t.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	{
		hifi::zone::draw_target ft = t;
		if (fx)
			(void)hifi::light::take(); // (asked again: however long the first frame took)
		const bool split = fx && hifi::light::begin(list, device, target, ft.light_rtv);
		// (ZONE_VIEW_REPEAT=<n>: the zone pass recorded n times over in this frame, the target and depth cleared before
		// each, for timing it: "drawn in" at n less at 1, over n - 1, is one pass's GPU time)
		uint32_t repeat = 1;
		if (const char *rv = std::getenv("ZONE_VIEW_REPEAT"); rv != nullptr)
			repeat = std::clamp<uint32_t>(uint32_t(std::strtoul(rv, nullptr, 10)), 1u, 200u);
		for (uint32_t k = 1; k < repeat; ++k)
		{
			renderer.record(list, ft, *hifi::zone::published(1), o, last);
			list->ClearRenderTargetView(t.rtv, sky, 0, nullptr);
			list->ClearDepthStencilView(t.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		}
		t0 = std::chrono::steady_clock::now();
		if (stamps != nullptr)
			list->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 0);
		st = renderer.record(list, ft, *hifi::zone::published(1), o, last);
		record_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (split)
			hifi::light::end(list);
		if (stamps != nullptr)
			list->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 1);
		if (fx)
			blades_at_zone_end(list);
		if (stamps != nullptr)
			list->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 2);
		if (fx)
		{
			std::string error;
			const grass::draw_stats fs = wfx.draw(list, gt, cam, fl, error);
			if (grass_on)
			{
				const uint32_t lit = grass_after ? fs.blade_native : blade_native;
				std::printf("  the grass: %u blades %s; lit from the native passes' block: %s%s%s; the light split's words %s\n", grass_after ? fs.blades : blades_drawn,
					grass_after ? "after the shading pass" : "at the zone's end", (lit & hifi::sun::kHasSun) != 0 ? "the sun's shadows " : "",
					(lit & hifi::sun::kHasLamps) != 0 ? "the lamps" : "", lit == 0 ? "nothing" : "", blade_words ? "written" : "not written");
			}
			std::printf("World FX frame %llu: %s%s, sun map %s, light split %s; the zone pass %s the sun's shadows itself, and %s the lamps' glow (%u lamps, %u cubes)\n",
				(unsigned long long)last, fs.sun ? "shaded" : "not shaded", error.empty() ? "" : (" (" + error + ")").c_str(), fs.sun_map ? "read" : "not read",
				split ? "drawn" : "not drawn", st.shadowed ? "applied" : "didn't apply", st.lamps ? "added" : "didn't add", fs.lamps, fs.lamp_maps);
			{
				// (what World FX published for the next frame's native passes: sun_shadows.hpp)
				std::lock_guard<std::mutex> lock(hifi::sun::detail::g.mutex);
				const hifi::sun::gpu_block &pb = hifi::sun::detail::g.latest.gpu;
				uint32_t with_cube = 0;
				for (uint32_t k = 0; k < hifi::sun::kMaxLamps; ++k)
					with_cube += pb.lamp_col[k][3] >= 2.0f ? 1 : 0;
				std::printf("  the block published: %s%s; %.0f lamps (%u with a cube), lamp shadows %.2f, %.0f characters' capsules\n",
					hifi::sun::detail::g.has ? "" : "none", (pb.mode[2] & hifi::sun::kHasSun) != 0 ? "the sun's shadows" : "no sun shadows", pb.lamp_head[0], with_cube,
					pb.lamp_head[1], pb.lamp_head[2]);
			}
			uint64_t taken = 0, cleared = 0;
			hifi::light::counts(taken, cleared);
			std::printf("  World FX read the light split: %s (taken %llu, cleared %llu)\n", fs.light_split ? "yes" : "no", (unsigned long long)taken,
				(unsigned long long)cleared);
		}
		if (stamps != nullptr)
		{
			list->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 3);
			list->ResolveQueryData(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, stamps_back, 0);
		}
	}
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
	direct->Signal(fence, last);
	fence->SetEventOnCompletion(last, e);
	WaitForSingleObject(e, 60000);
	const double gpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	stamps_read();
	if (!zone_ms.empty())
	{
		// (the last ten frames: the cubes drawn, the block applied; the least and the middle one)
		const size_t from = zone_ms.size() > 10 ? zone_ms.size() - 10 : 0;
		std::vector<double> z(zone_ms.begin() + from, zone_ms.end()), w(fx_ms.begin() + from, fx_ms.end());
		std::sort(z.begin(), z.end());
		std::sort(w.begin(), w.end());
		std::printf("GPU time over the last %zu frames: the zone pass %.2f ms at least, %.2f the middle one; World FX after it %.2f at least, %.2f the middle one\n",
			z.size(), z.front(), z[z.size() / 2], w.front(), w[w.size() / 2]);
		if (grass_on && !grass_after)
		{
			std::vector<double> b(blade_ms.begin() + from, blade_ms.end());
			std::sort(b.begin(), b.end());
			std::printf("  the blades at the zone's end: %.3f ms at least, %.3f the middle one\n", b.front(), b[b.size() / 2]);
		}
	}
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
