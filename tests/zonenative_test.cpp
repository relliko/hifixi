/*
 * The native zone renderer's per-zone data as the plugin runs it (src/zone/native.cpp), on WARP: presents with a
 * zone, until it's on the GPU; another zone (the first kept until it's let go); not wanted (let go); stop.
 *
 * Then the texture pack's zones (docs/texture-pack/zones.md: src/zone/zonepack, the gpu_zone's pack region, the
 * streamer) with the debug layer, a made-up pack under build\test\zonenative from Southern San d'Oria's (230) and West
 * Ronfaure's (100) own textures (each a 2x copy: level 0 red, the levels after it blue; one key left out, one no_pack),
 * driven as the module drives the glue (set_mode, frame each present before retire::on_present, clear_all before every
 * close and open), each picture drawn by the zone renderer (src/zone/draw) and read back:
 *   - the zone read in pack mode has a pack region; its game textures first (the picture as the game's while the
 *     copies are held), then the pack's (another picture); each view of the record's 2 levels down to the game's size;
 *     the zone's bytes take the pack's in; the most used textures asked first; nothing read while the area settles
 *     (kQuiet); the coverage as Alex reads it (/hifi status's pack line, the menu's "This area" line, the zone's log
 *     line once its requests settle) with those numbers;
 *   - compare: the game's picture, nothing freed; the FSR bias: more of the red level 0 with -1, the picture back at 0;
 *   - a zone change: the old zone's owner dropped as it's handed to the retire list, its textures let go behind it;
 *   - off mid-stream (copies in flight): the game's picture the next frame, every word spent, nothing written after
 *     (the menu says the area keeps the game's textures); on again: nothing asked for the zone in view, the next zone asks;
 *   - 50 switches (off/on, reload, zone changes) with copies in flight: the slot-write log shows each slot written once
 *     at most per gpu_zone, always before its word says drawn, never after it's drawn or spent; nothing left over; the
 *     glue's worst present (requests, take_done, the views written) timed;
 *   - the request cap and the priorities: 230 with 100 as its second map, asked together under a cap of 40 a present
 *     (zonepack::set_request_cap: a zone here has fewer keyed textures than kRequestsPerPresent): 40, 40, then the
 *     rest, the zone's all before its second map's; the streamer takes none of the second map's while one of the
 *     zone's waits;
 *   - stop during streaming (clear_all, stream::stop with copies held by a gate opened 5 ms in, retire::release_all):
 *     nothing leaked, the device's references back;
 *   - the device removed mid-copy (a child process): stream::device_lost alone misses every ticket in flight (lost),
 *     before the glue's clear_all spends the words; stop() fast, no crash;
 *   - the debug layer says nothing.
 *
 * Then the texture pack's heights (M9, zones.md "Pack normals"): a made-up pack under build\test\zonenative-heights of
 * 230's keyed textures, a third with a colour and a height (BC4 stripes, 4 texels wide), a third a height alone (a
 * materials-only entry: the game's colour), a third a colour alone, drawn per pixel with the light split, in a process
 * of its own ("zonenative_test heights" runs it alone; the main run starts it so after the rest, its checks counted):
 *   - heights off ([textures] height=0): none asked; then on: each height asked once and drawn (the colour-less ones
 *     too, their colour words spent as not in the pack), the zone's bytes take them in, the menu's relief line;
 *   - the picture per pixel changes on the relief's pixels and stays within 3% of the light on the whole; per pixel off
 *     it is byte for byte the one without heights; the light split's words differ only where the relief lit them, each
 *     with its depth as it was and its facing turned away from the camera (the mark), and nowhere else;
 *   - heights off again: the picture and the light split byte for byte the ones without heights; on again: the relief's
 *     picture at once; compare: the game's look, relief and all;
 *   - clear_all: every height spent; the slot-write log valid for the heights' words as for the colours'; the debug
 *     layer says nothing.
 *
 *   build\zonenative_test.exe      (ZONE_GPU=1: the GPU instead of WARP for the texture pack's part; HIFIXI_GBV=1:
 *                                   GPU-based validation too)
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include "../src/zone/native.hpp"
#include "../src/zone/actordata.hpp"
#include "../src/zone/draw.hpp"
#include "../src/zone/frame.hpp"
#include "../src/zone/zone_gpu.hpp"
#include "../src/zone/zonepack.hpp"
#include "../src/host/retire.hpp"
#include "../src/host/shadercache.hpp"
#include "../src/pack/pack.hpp"
#include "../src/pack/pack_write.hpp"
#include "../src/pack/stream.hpp"
#include "../src/pack/validate.hpp"
#include "game_dump.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{
	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}
	void note(const std::string &what) { std::printf("--    %s\n", what.c_str()); }

	// Presents until the zone is on the GPU (or 2000 of them, 2 ms apart)
	hifi::zone::gpu_zone *present_until_ready(ID3D12Device *device, uint32_t zone, int &presents)
	{
		for (presents = 1; presents <= 2000; ++presents)
		{
			hifi::zone::native::frame(device, zone, 0, true);
			if (hifi::zone::gpu_zone *z = hifi::zone::native::ready_zone(); z != nullptr && z->data().map == zone)
				return z;
			Sleep(2);
		}
		return nullptr;
	}

	std::string lines()
	{
		std::string s;
		for (const std::string &l : hifi::zone::native::status())
			s += "\n        " + l;
		return s;
	}

	double now_ms()
	{
		LARGE_INTEGER t, f;
		QueryPerformanceCounter(&t);
		QueryPerformanceFrequency(&f);
		return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
	}

	ULONG refs(IUnknown *p)
	{
		p->AddRef();
		return p->Release();
	}

	void remove_tree(const std::wstring &folder)
	{
		WIN32_FIND_DATAW f;
		HANDLE h = FindFirstFileW((folder + L"*").c_str(), &f);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
			{
				const std::wstring name = f.cFileName;
				if (name == L"." || name == L"..")
					continue;
				if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					remove_tree(folder + name + L"\\");
				else
					DeleteFileW((folder + name).c_str());
			} while (FindNextFileW(h, &f));
			FindClose(h);
		}
		RemoveDirectoryW(folder.substr(0, folder.size() - 1).c_str());
	}

	uint32_t debug_messages(ID3D12InfoQueue *q, std::string &first)
	{
		if (q == nullptr)
			return 0;
		const UINT64 n = q->GetNumStoredMessages();
		uint32_t errors = 0;
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T size = 0;
			q->GetMessage(i, nullptr, &size);
			std::vector<uint8_t> buf(size);
			D3D12_MESSAGE *m = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
			q->GetMessage(i, m, &size);
			if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && m->ID != D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE &&
				m->ID != D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE)
			{
				if (errors++ == 0)
					first = m->pDescription;
			}
		}
		q->ClearStoredMessages();
		return errors;
	}

	// ---- The texture pack's part

	namespace zm = hifi::bake::zonemesh;
	namespace pk = hifi::pack;
	namespace st = hifi::pack::stream;
	namespace zp = hifi::zone::zonepack;
	namespace native = hifi::zone::native;
	using hifi::zone::gpu_zone;

	constexpr UINT W = 256, H = 160;
	const wchar_t *const kFolder = L"build\\test\\zonenative\\";

	// The slot-write log (gpu_zone::set_test_log): each owner's (a gpu_zone's visit) events, a texture's in order
	std::map<st::owner, std::map<uint32_t, std::string>> g_log;
	void on_log(const gpu_zone &z, uint32_t texture, char what)
	{
		g_log[z.pack().owner][texture] += what;
	}
	// Every texture's events: [a [w d]] [s] (written once at most, before drawn, never after drawn or spent); false
	// with the first that isn't
	bool log_valid(std::string &bad)
	{
		for (const auto &[o, ts] : g_log)
			for (const auto &[t, ev] : ts)
			{
				size_t k = 0;
				if (k < ev.size() && ev[k] == 'a')
					k++;
				if (k + 1 < ev.size() && ev[k] == 'w' && ev[k + 1] == 'd')
					k += 2;
				if (k < ev.size() && ev[k] == 's')
					k++;
				if (k != ev.size())
				{
					bad = "owner " + std::to_string(o) + " texture " + std::to_string(t) + ": \"" + ev + "\"";
					return false;
				}
			}
		return true;
	}
	size_t log_count(char what)
	{
		size_t n = 0;
		for (const auto &[o, ts] : g_log)
			for (const auto &[t, ev] : ts)
				n += size_t(std::count(ev.begin(), ev.end(), what));
		return n;
	}

	// The glue's log lines (zonepack::set_log; the plugin's backend::log), one a line
	std::string g_logged;
	void on_glue_log(int, const char *line)
	{
		g_logged += line;
		g_logged += '\n';
	}

	// The camera the glue orders a zone's textures from (as zonedraw::camera: the last pass's)
	float g_eye[3] = { -27.1f, -4.5f, 35.1f };
	uint32_t g_camera_map = 0;
	bool camera_source(float out[3], uint32_t &map)
	{
		if (g_camera_map == 0)
			return false;
		std::memcpy(out, g_eye, 12);
		map = g_camera_map;
		return true;
	}

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
	mat4 camera(const float eye[3], float yaw, float tilt)
	{
		const float at[3] = { eye[0] + std::sin(yaw) * std::cos(tilt), eye[1] + std::sin(tilt), eye[2] + std::cos(yaw) * std::cos(tilt) };
		float z[3] = { eye[0] - at[0], eye[1] - at[1], eye[2] - at[2] }, up[3] = { 0, -1, 0 }, x[3], y[3];
		auto norm = [](float *v) {
			const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			for (int k = 0; k < 3; ++k)
				v[k] /= l;
		};
		auto cross = [](const float *a, const float *b, float *o) {
			o[0] = a[1] * b[2] - a[2] * b[1];
			o[1] = a[2] * b[0] - a[0] * b[2];
			o[2] = a[0] * b[1] - a[1] * b[0];
		};
		auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
		norm(z);
		cross(up, z, x);
		norm(x);
		cross(z, x, y);
		const mat4 v = { { x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1 } };
		const float ys = 1.0f / std::tan(0.5f), xs = ys / (float(W) / H), zn = 0.5f, zf = 2000.0f;
		const mat4 p = { { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zn - zf), -1, 0, 0, zn * zf / (zn - zf), 0 } };
		return mul(v, p);
	}

	// A BC2 level of one colour (565), opaque
	std::vector<uint8_t> solid(uint32_t w, uint32_t h, uint16_t c565)
	{
		const size_t blocks = size_t((w + 3) / 4) * ((h + 3) / 4);
		std::vector<uint8_t> b(blocks * 16);
		for (size_t i = 0; i < blocks; ++i)
		{
			uint8_t *p = b.data() + i * 16;
			std::memset(p, 0xFF, 8);
			std::memcpy(p + 8, &c565, 2);
			std::memcpy(p + 10, &c565, 2);
			std::memset(p + 12, 0, 4);
		}
		return b;
	}

	// The made-up pack: every keyed texture of the zones given, 2x its size, level 0 red and the levels after it blue;
	// the first zone's first keyed texture left out, its second no_pack
	bool make_pack(const std::vector<const zm::zone *> &zones, pk::key &left_out, pk::key &no_pack, uint32_t &added)
	{
		remove_tree(kFolder);
		pk::writer w;
		pk::writer_options o;
		o.folder = kFolder;
		o.source = "zonenative_test";
		std::string why;
		if (!w.open(o, why))
		{
			note("the made-up pack couldn't be opened: " + why);
			return false;
		}
		added = 0;
		uint32_t keyed = 0;
		for (const zm::zone *z : zones)
			for (const zm::texture &t : z->textures)
			{
				if (t.key.bytes == 0)
					continue;
				const uint32_t n = keyed++;
				if (n == 0)
				{
					left_out = t.key;
					continue;
				}
				if (t.key == left_out)
					continue;
				pk::entry e;
				e.hash = t.key.hash;
				e.bytes = t.key.bytes;
				e.w = t.key.w;
				e.h = t.key.h;
				e.orig_format = t.format == zm::kBC2 ? 0xA1 : 0xB1;
				e.source_kind = pk::kSourceZone;
				e.file_id = z->map;
				std::memcpy(e.name, t.name, 16);
				if (n == 1)
				{
					e.flags = pk::kNoPack;
					no_pack = t.key;
				}
				pk::map_in m;
				m.format = pk::kBC2;
				m.w = t.key.w * 2;
				m.h = t.key.h * 2;
				const uint32_t levels = pk::full_levels(m.w, m.h);
				for (uint32_t l = 0; l < levels; ++l)
					m.levels.push_back(solid(std::max(1u, m.w >> l), std::max(1u, m.h >> l), l == 0 ? 0xF800 : 0x001F));
				const pk::add_result a = w.add(e, { m }, why);
				if (a == pk::add_result::refused)
				{
					note("a made-up entry refused: " + why);
					return false;
				}
				added += a == pk::add_result::ok;
			}
		const bool ok = w.commit(why);
		w.close();
		if (!ok)
			note("the made-up pack couldn't be written: " + why);
		return ok;
	}

	// The GPU side: the game's queue (retire's fence), a target and depth, the zone renderer, a readback
	struct rig
	{
		ID3D12Device *device = nullptr;
		ID3D12InfoQueue *info = nullptr;
		ID3D12CommandQueue *direct = nullptr;
		ID3D12Fence *fence = nullptr;
		uint64_t fence_value = 0;
		HANDLE event = nullptr;
		ID3D12CommandAllocator *alloc = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12Resource *rt = nullptr, *ds = nullptr, *back = nullptr;
		ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
		hifi::zone::zone_renderer renderer;
		uint64_t frame_number = 0;
		double now = 100.0;
		bool on = false;     // (the module in use: the glue's frame each present)
		uint32_t epoch = 0;  // (the epoch in use)
		uint32_t map = 230;  // (the zone the presents are for)
		uint32_t second = 0; // (and its second map: none)
		double worst_ms = 0.0; // (the glue's frame, the longest: requests, take_done and the views it wrote)
		uint32_t worst_views = 0;
	};

	bool make_rig(rig &r)
	{
		D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
		if (FAILED(r.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&r.direct))) || FAILED(r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r.fence))) ||
			FAILED(r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&r.alloc))) ||
			FAILED(r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.alloc, nullptr, IID_PPV_ARGS(&r.list))))
			return false;
		r.list->Close();
		r.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
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
		D3D12_CLEAR_VALUE cv = {};
		cv.Format = td.Format;
		if (FAILED(r.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&r.rt))))
			return false;
		td.Format = DXGI_FORMAT_D32_FLOAT;
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		cv.Format = td.Format;
		cv.DepthStencil.Depth = 1.0f;
		if (FAILED(r.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&r.ds))))
			return false;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = uint64_t(W) * 4 * H;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(r.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r.back))))
			return false;
		D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
		r.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&r.rtv_heap));
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		r.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&r.dsv_heap));
		r.device->CreateRenderTargetView(r.rt, nullptr, r.rtv_heap->GetCPUDescriptorHandleForHeapStart());
		r.device->CreateDepthStencilView(r.ds, nullptr, r.dsv_heap->GetCPUDescriptorHandleForHeapStart());
		std::string why;
		if (!r.renderer.init(r.device, why))
		{
			note("the zone renderer: " + why);
			return false;
		}
		return true;
	}

	void wait_gpu(rig &r)
	{
		r.direct->Signal(r.fence, ++r.fence_value);
		if (r.fence->GetCompletedValue() < r.fence_value)
		{
			r.fence->SetEventOnCompletion(r.fence_value, r.event);
			WaitForSingleObject(r.event, 20000);
		}
	}

	// One of the plugin's presents: the zone's data, the module's present (the streamer's pacing, then the glue's
	// frame), retire::on_present
	void present(rig &r)
	{
		native::frame(r.device, r.map, r.second, true);
		st::on_present();
		if (r.on)
		{
			const double t0 = now_ms();
			zp::frame(r.now, r.epoch);
			const double ms = now_ms() - t0;
			if (ms > r.worst_ms)
			{
				r.worst_ms = ms;
				r.worst_views = zp::get_counts().last_views;
			}
		}
		r.now += 0.016;
		hifi::retire::on_present(r.direct);
		Sleep(1);
	}

	// The light split's target and readback (made at its first use): R32_UINT, the picture's size
	ID3D12Resource *g_light_rt = nullptr, *g_light_back = nullptr;
	ID3D12DescriptorHeap *g_light_heap = nullptr;
	bool light_target(rig &r)
	{
		if (g_light_rt != nullptr)
			return true;
		D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, rb = { D3D12_HEAP_TYPE_READBACK };
		D3D12_RESOURCE_DESC td = {};
		td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		td.Width = W;
		td.Height = H;
		td.DepthOrArraySize = 1;
		td.MipLevels = 1;
		td.SampleDesc.Count = 1;
		td.Format = DXGI_FORMAT_R32_UINT;
		td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		D3D12_CLEAR_VALUE cv = {};
		cv.Format = td.Format;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = uint64_t(W) * 4 * H;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
		if (FAILED(r.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&g_light_rt))) ||
			FAILED(r.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_light_back))) ||
			FAILED(r.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_light_heap))))
			return false;
		r.device->CreateRenderTargetView(g_light_rt, nullptr, g_light_heap->GetCPUDescriptorHandleForHeapStart());
		return true;
	}
	void drop_light_target()
	{
		for (IUnknown *p : { static_cast<IUnknown *>(g_light_rt), static_cast<IUnknown *>(g_light_back), static_cast<IUnknown *>(g_light_heap) })
			if (p != nullptr)
				p->Release();
		g_light_rt = g_light_back = nullptr;
		g_light_heap = nullptr;
	}

	// The zone drawn from the street (every placement), read back; per_pixel: draw_options' (M9's heights are drawn only
	// so); light: the light split as well, read back into it
	std::vector<uint32_t> picture(rig &r, gpu_zone *z, float bias = 0.0f, bool per_pixel = false, std::vector<uint32_t> *light = nullptr)
	{
		std::vector<uint32_t> px;
		if (z == nullptr || !z->ready())
			return px;
		hifi::zone::frame &f = hifi::zone::frame_slot(1);
		f.native = true;
		f.no_bump = false;
		f.zone = z;
		f.second = nullptr;
		f.map = z->data().map;
		const mat4 vp = camera(g_eye, 1.5708f, 0.12f);
		std::memcpy(f.view_proj, vp.m, 64);
		std::memcpy(f.camera, g_eye, 12);
		const float sd[3] = { 0.36f, -0.83f, 0.42f }, sun[3] = { 1.26f, 1.25f, 1.1f }, amb[3] = { 0.49f, 0.47f, 0.49f }, fog[3] = { 0.55f, 0.65f, 0.78f };
		std::memcpy(f.sun_dir, sd, 12);
		std::memcpy(f.sun, sun, 12);
		std::memcpy(f.sun_bump, sun, 12);
		std::memcpy(f.ambient, amb, 12);
		std::memcpy(f.ambient_bump, amb, 12);
		std::memcpy(f.fog, fog, 12);
		f.fog_start = 150.0f;
		f.fog_end = 600.0f;
		f.fog_scale = 1.0f;
		f.env_count = 1;
		f.count = 0;
		f.lamp_count = 0;
		for (const zm::placement &p : z->data().placements)
		{
			if (f.count >= hifi::zone::kMaxObjects)
				break;
			hifi::zone::frame_object &o = f.objects[f.count++];
			o = hifi::zone::frame_object();
			for (int k = 0; k < 3; ++k)
			{
				o.world[k * 4 + 0] = p.m[k];
				o.world[k * 4 + 1] = p.m[3 + k];
				o.world[k * 4 + 2] = p.m[6 + k];
				o.world[k * 4 + 3] = p.t[k];
			}
			o.model = p.model;
			o.flags = p.flags;
		}
		r.alloc->Reset();
		r.list->Reset(r.alloc, nullptr);
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = r.rtv_heap->GetCPUDescriptorHandleForHeapStart(), dsv = r.dsv_heap->GetCPUDescriptorHandleForHeapStart();
		const float sky[4] = { 0.55f, 0.65f, 0.78f, 1.0f };
		r.list->ClearRenderTargetView(rtv, sky, 0, nullptr);
		r.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		hifi::zone::draw_target t;
		t.rtv = rtv;
		t.dsv = dsv;
		t.rtv_format = DXGI_FORMAT_R8G8B8A8_UNORM;
		t.dsv_format = DXGI_FORMAT_D32_FLOAT;
		t.width = float(W);
		t.height = float(H);
		const bool with_light = light != nullptr && light_target(r);
		if (with_light)
		{
			t.light_rtv = g_light_heap->GetCPUDescriptorHandleForHeapStart();
			const UINT none[4] = {};
			r.list->ClearRenderTargetView(t.light_rtv, reinterpret_cast<const float *>(none), 0, nullptr);
		}
		hifi::zone::draw_options o;
		o.texture_bias = bias;
		o.per_pixel = per_pixel;
		r.renderer.record(r.list, t, f, o, ++r.frame_number);
		if (with_light)
		{
			D3D12_RESOURCE_BARRIER lb = {};
			lb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			lb.Transition.pResource = g_light_rt;
			lb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			lb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			lb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			r.list->ResourceBarrier(1, &lb);
			D3D12_TEXTURE_COPY_LOCATION ldst = {}, lsrc = {};
			ldst.pResource = g_light_back;
			ldst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			ldst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, W, H, 1, W * 4 };
			lsrc.pResource = g_light_rt;
			lsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			r.list->CopyTextureRegion(&ldst, 0, 0, 0, &lsrc, nullptr);
			std::swap(lb.Transition.StateBefore, lb.Transition.StateAfter);
			r.list->ResourceBarrier(1, &lb);
		}
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = r.rt;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		r.list->ResourceBarrier(1, &b);
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = r.back;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, W * 4 };
		src.pResource = r.rt;
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
		r.list->ResourceBarrier(1, &b);
		r.list->Close();
		ID3D12CommandList *lists[] = { r.list };
		r.direct->ExecuteCommandLists(1, lists);
		wait_gpu(r);
		void *p = nullptr;
		const D3D12_RANGE all = { 0, size_t(W) * 4 * H };
		if (SUCCEEDED(r.back->Map(0, &all, &p)))
		{
			px.assign(static_cast<const uint32_t *>(p), static_cast<const uint32_t *>(p) + size_t(W) * H);
			const D3D12_RANGE none = { 0, 0 };
			r.back->Unmap(0, &none);
		}
		if (with_light && SUCCEEDED(g_light_back->Map(0, &all, &p)))
		{
			light->assign(static_cast<const uint32_t *>(p), static_cast<const uint32_t *>(p) + size_t(W) * H);
			const D3D12_RANGE none = { 0, 0 };
			g_light_back->Unmap(0, &none);
		}
		return px;
	}

	size_t differ(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b)
	{
		if (a.size() != b.size() || a.empty())
			return SIZE_MAX;
		size_t n = 0;
		for (size_t i = 0; i < a.size(); ++i)
			n += a[i] != b[i];
		return n;
	}

	// Red over blue in all (the pack's level 0 is red, the levels after it blue)
	double redness(const std::vector<uint32_t> &a)
	{
		double s = 0.0;
		for (uint32_t p : a)
			s += double(p & 0xFF) - double((p >> 16) & 0xFF);
		return s / double(std::max<size_t>(a.size(), 1));
	}

	// The words of a gpu_zone
	gpu_zone::pack_counts words(const gpu_zone *z) { return z != nullptr ? z->pack_summary() : gpu_zone::pack_counts(); }

	// Presents until the zone `map` is up (a new gpu_zone)
	gpu_zone *zone_up(rig &r, uint32_t map, const gpu_zone *not_this = nullptr)
	{
		r.map = map;
		for (int i = 0; i < 3000; ++i)
		{
			present(r);
			gpu_zone *z = native::ready_zone(0);
			if (z != nullptr && z->data().map == map && z != not_this)
				return z;
			Sleep(1);
		}
		return nullptr;
	}

	// Presents until every keyed texture of z is drawn or spent (or n presents)
	bool settle(rig &r, gpu_zone *z, int n = 4000)
	{
		for (int i = 0; i < n; ++i)
		{
			const gpu_zone::pack_counts c = words(z);
			if (c.keyed != 0 && c.drawn + c.spent == c.keyed)
				return true;
			present(r);
		}
		return false;
	}

	// The module's open as it does it: every owner dropped first, the loaders out of pack mode, the open posted; then
	// (later in the module: its next update) the epoch seen raised, the loaders in pack mode again
	bool open_pack(rig &r, const wchar_t *folder = kFolder)
	{
		zp::clear_all();
		zp::set_mode(false, false);
		const uint32_t before = st::epoch();
		st::open(std::wstring(folder));
		st::resume(st::kOff);
		for (int i = 0; i < 5000 && st::epoch() == before; ++i)
			Sleep(1);
		r.on = st::current() != nullptr && st::epoch() != before;
		r.epoch = st::epoch();
		zp::set_mode(r.on, false);
		return r.on;
	}

	// The module's off: every owner dropped, the loaders out of pack mode, the pack closed, kOff
	void close_pack(rig &r)
	{
		zp::clear_all();
		zp::set_mode(false, false);
		zp::set_compare(false);
		st::close();
		st::pause(st::kOff);
		r.on = false;
	}

	uint64_t in_flight_now()
	{
		const st::counts c = st::get_counts();
		return c.in_flight + c.queued;
	}

	// The device removed mid-copy, in a child process (RemoveDevice ends this process's one device on the adapter)
	int lost_child()
	{
		uintptr_t base = 0;
		if (dump::map(dump::kDumps[0], base) == 0)
			return 2;
		IDXGIFactory4 *factory = nullptr;
		IDXGIAdapter *warp = nullptr;
		CreateDXGIFactory1(IID_PPV_ARGS(&factory));
		factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
		rig r;
		if (FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&r.device))) || !make_rig(r))
			return 3;
		gpu_zone::set_test_log(on_log);
		zp::set_camera_source(camera_source);
		st::start(r.device, 32);
		if (!open_pack(r))
			return 4;
		ID3D12Fence *gate = nullptr;
		r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate));
		st::test::gate_copies(gate, 1);
		gpu_zone *z = zone_up(r, 230);
		if (z == nullptr || !z->has_pack())
			return 5;
		for (int i = 0; i < 3000 && st::get_counts().in_flight == 0; ++i)
			present(r);
		if (st::get_counts().in_flight == 0)
			return 6;
		ID3D12Device5 *d5 = nullptr;
		if (FAILED(r.device->QueryInterface(IID_PPV_ARGS(&d5))))
			return 7;
		d5->RemoveDevice();
		// (the module's follow_lost: the streamer told, every owner dropped)
		const double t0 = now_ms();
		st::device_lost();
		// (the streamer's part alone, before the glue's clear_all: every ticket the zone's words wait on missed, lost; one
		// that missed for another reason before the loss, not yet handed over by take_done, left out)
		uint32_t waiting = 0, missed_lost = 0;
		for (uint32_t i = 0; i < z->data().textures.size(); ++i)
			if (z->pack_word(i) == gpu_zone::kPackAsked)
			{
				const st::ticket t = z->pack().tickets[i];
				const st::state s = st::poll(t);
				const st::miss why = s == st::state::missed ? st::reason(t) : st::miss::none;
				if (s == st::state::missed && why != st::miss::lost)
					continue;
				waiting++;
				missed_lost += s == st::state::missed && why == st::miss::lost;
			}
		const uint64_t refused_lost = st::get_counts().refused[size_t(st::miss::lost)];
		zp::clear_all();
		const gpu_zone::pack_counts c = words(z);
		const double t1 = now_ms();
		st::stop();
		const double stop_ms = now_ms() - t1;
		const st::counts n = st::get_counts();
		std::string bad;
		const bool valid = log_valid(bad);
		std::printf("lost: %u tickets waited on, %u of them missed (lost) before clear_all (%llu in all); words %u asked, %u drawn, %u spent of %u keyed; lost %.1f ms, "
			"stop %.1f ms; log %s; owners %u\n", waiting, missed_lost, static_cast<unsigned long long>(refused_lost), c.asked, c.drawn, c.spent, c.keyed, t1 - t0, stop_ms,
			valid ? "valid" : bad.c_str(), n.owners);
		const bool ok = waiting > 0 && missed_lost == waiting && refused_lost >= waiting && c.asked == 0 && c.drawn == 0 && c.spent >= c.keyed && stop_ms < 100.0 &&
			valid && n.owners == 0;
		std::printf("lost: %s\n", ok ? "ok" : "not ok");
		std::fflush(stdout);
		// (the removed device's objects left for the process's end)
		ExitProcess(ok ? 0 : 1);
	}

	DWORD run_child(const char *arg, std::string &out);

	void pack_part(bool on_gpu, bool gbv)
	{
		std::printf("\nThe texture pack's zones (%s%s)\n", on_gpu ? "the GPU" : "WARP", gbv ? ", GPU-based validation" : "");
		// (no pivot.ini: the game's own files, so every DAT is the game's and gets keys)
		SetEnvironmentVariableW(L"HIFIXI_PIVOT_INI", L"build\\test\\zonenative\\no-pivot.ini");
		zm::zone z230, z100, plain;
		hifi::bake::result res;
		const auto t0 = now_ms();
		const bool read = zm::load(230, z230, res, true) && zm::load(100, z100, res, true);
		const double pack_ms = now_ms() - t0;
		const auto t1 = now_ms();
		zm::load(230, plain, res, false);
		const double plain_ms = now_ms() - t1;
		uint32_t keyed = 0;
		for (const zm::texture &t : z230.textures)
			keyed += t.key.bytes != 0;
		check(read && z230.pack && !z230.overlaid && keyed == z230.textures.size() && keyed > 2,
			"230 and 100 read in pack mode: every texture of 230 keyed (" + std::to_string(keyed) + "), 230 and 100 in " + std::to_string(int(pack_ms)) +
				" ms (230 alone without the pack " + std::to_string(int(plain_ms)) + " ms)");
		pk::key left_out, no_pack;
		uint32_t added = 0;
		const bool made = make_pack({ &z230, &z100 }, left_out, no_pack, added);
		check(made, "the made-up pack under build\\test\\zonenative: " + std::to_string(added) + " entries");

		IDXGIFactory4 *factory = nullptr;
		IDXGIAdapter *warp = nullptr;
		CreateDXGIFactory1(IID_PPV_ARGS(&factory));
		factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
		rig r;
		if (FAILED(D3D12CreateDevice(on_gpu ? nullptr : warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&r.device))))
		{
			check(false, "a device for the texture pack's part");
			return;
		}
		r.device->QueryInterface(IID_PPV_ARGS(&r.info));
		if (r.info != nullptr)
			r.info->SetMuteDebugOutput(TRUE);
		hifi::shadercache::set_folder(L"build\\shadercache_test\\");
		check(make_rig(r), "the zone renderer, a target and the game's queue on the device");
		std::string first;
		gpu_zone::set_test_log(on_log);
		zp::set_camera_source(camera_source);
		zp::set_log(on_glue_log);
		ID3D12Fence *gate = nullptr;
		r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate));
		uint64_t gate_value = 1;
		// (the device's references before the streamer: retire's fence made (kept, as the game's queue keeps it), and the
		// renderer's pipelines (made at the first draw) by a picture of 230 read without the pack, let go again)
		{
			gpu_zone *const plain_zone = zone_up(r, 230);
			picture(r, plain_zone);
			native::stop();
			for (int i = 0; i < 3; ++i)
			{
				wait_gpu(r);
				hifi::retire::on_present(r.direct);
			}
		}
		wait_gpu(r);
		const ULONG refs0 = refs(r.device);

		// The streamer started once (the module's begin_open), the pack opened
		st::start(r.device, 32);
		const bool opened = open_pack(r);
		check(opened, "the pack opened on the streamer's worker (epoch " + std::to_string(r.epoch) + "); the loaders in pack mode");

		// The zone, read in pack mode: its game textures first (the copies held on the GPU), then the pack's
		g_camera_map = 230;
		st::test::gate_copies(gate, gate_value);
		r.on = false; // (no glue's frame till the game picture is taken)
		gpu_zone *z = zone_up(r, 230);
		check(z != nullptr && z->has_pack() && words(z).keyed == keyed && words(z).asked == 0, "230 up in pack mode: a pack region, every word none");
		if (z == nullptr || !z->has_pack())
			return;
		bool same_slots = true;
		for (uint32_t i = 0; i < z->data().textures.size(); ++i)
			same_slots = same_slots && z->texture_srv(i).ptr == z->game_srv(i).ptr;
		const uint64_t bytes0 = z->bytes();
		const std::vector<uint32_t> game = picture(r, z);
		check(game.size() == size_t(W) * H && same_slots, "  the game's picture; texture_srv names the game's slots");
		r.on = true;
		for (int i = 0; i < 400 && (st::get_counts().in_flight == 0 || words(z).asked + words(z).spent < keyed); ++i)
			present(r);
		const std::vector<uint32_t> held = picture(r, z);
		const gpu_zone::pack_counts c0 = words(z);
		check(c0.asked > 0 && c0.drawn == 0 && differ(held, game) == 0 && st::get_counts().in_flight > 0,
			"  asked, the copies held on the GPU: " + std::to_string(c0.asked) + " asked, " + std::to_string(c0.spent) + " spent, none drawn, the game's picture (" +
				std::to_string(st::get_counts().in_flight) + " copies in flight)");
		// (the order: the most used first, by powers of two)
		{
			const std::vector<uint32_t> order = zp::order_of(z);
			const zm::zone &d = z->data();
			std::vector<uint64_t> placed(d.models.size(), 0), uses(d.textures.size(), 0);
			for (const zm::placement &p : d.placements)
				if (p.model < placed.size())
					placed[p.model]++;
			for (size_t m = 0; m < d.models.size(); ++m)
				for (uint32_t k = d.models[m].first_piece; k < d.models[m].first_piece + d.models[m].pieces; ++k)
					if (d.pieces[k].texture < uses.size())
						uses[d.pieces[k].texture] += uint64_t(d.pieces[k].triangles) * std::max<uint64_t>(placed[m], 1);
			bool down = order.size() == keyed;
			for (size_t i = 1; down && i < order.size(); ++i)
				down = std::floor(std::log2(1.0 + double(uses[order[i]]))) <= std::floor(std::log2(1.0 + double(uses[order[i - 1]])));
			check(down, "  asked in order: " + std::to_string(order.size()) + " keyed textures, the most used first");
		}
		// (the area settling: nothing read or copied while kQuiet holds it)
		st::pause(st::kQuiet);
		const uint64_t copies_before = st::get_counts().copies;
		gate->Signal(gate_value);
		for (int i = 0; i < 100; ++i)
			present(r);
		const uint64_t copies_quiet = st::get_counts().copies - copies_before;
		st::resume(st::kQuiet);
		check(copies_quiet == 0, "  while the area settles (kQuiet) no copy starts (" + std::to_string(copies_quiet) + " in 100 presents)");
		const bool settled = settle(r, z);
		const gpu_zone::pack_counts c1 = words(z);
		const zp::counts n1 = zp::get_counts();
		check(settled && c1.drawn == keyed - 2 && c1.spent == 2 && n1.not_in_pack == 2, "  then drawn: " + std::to_string(c1.drawn) + " drawn, " +
			std::to_string(c1.spent) + " spent (" + std::to_string(n1.not_in_pack) + " not in it: one left out, one no_pack)");
		bool levels = true, pack_slots = true;
		uint64_t pack_bytes = 0;
		for (uint32_t i = 0; i < z->data().textures.size(); ++i)
		{
			if (z->pack_word(i) != gpu_zone::kPackDrawn)
				continue;
			st::view v;
			levels = levels && st::get(z->pack().tickets[i], v) && v.levels == 2 && v.w == 2 * z->data().textures[i].key.w;
			pack_bytes += v.bytes;
			bool from_pack = false;
			pack_slots = pack_slots && z->texture_srv(i, &from_pack).ptr != z->game_srv(i).ptr && from_pack;
		}
		check(levels && pack_slots, "  each view the record's 2 levels down to the game's size (2x); texture_srv names the pack's slots, game_srv the game's");
		check(z->bytes() == bytes0 + pack_bytes, "  the zone's bytes take the pack's in (" + std::to_string(pack_bytes / 1024) + " KB)");
		const std::vector<uint32_t> packed = picture(r, z);
		check(differ(packed, game) > size_t(W) * H / 4, "  the pack's picture: " + std::to_string(differ(packed, game)) + " of " + std::to_string(W * H) + " pixels changed");
		zp::set_compare(true);
		const std::vector<uint32_t> compared = picture(r, z);
		zp::set_compare(false);
		check(differ(compared, game) == 0 && differ(picture(r, z), packed) == 0 && words(z).drawn == c1.drawn,
			"compare: the game's picture, then the pack's again; nothing freed");
		const std::vector<uint32_t> biased = picture(r, z, -1.0f);
		check(differ(biased, packed) > 0 && redness(biased) > redness(packed) && differ(picture(r, z, 0.0f), packed) == 0,
			"the FSR bias -1: more of the red level 0 (" + std::to_string(redness(packed)) + " -> " + std::to_string(redness(biased)) + "), at 0 the picture as before");
		std::string bad;
		check(log_valid(bad) && log_count('w') == c1.drawn, "the slot-write log: each slot written once, before its word says drawn (" + std::to_string(log_count('w')) +
			" writes)" + (bad.empty() ? "" : ": " + bad));
		check(debug_messages(r.info, first) == 0, "  the debug layer said nothing" + (first.empty() ? "" : ": " + first));
		// (the coverage as Alex reads it: /hifi status, the menu's "This area" line, the zone's log line once settled)
		{
			const std::string want_status = "native zone data: zone: texture pack: " + std::to_string(c1.drawn) + " of " + std::to_string(c1.textures) + " textures from it, " +
				std::to_string(c1.spent) + " not (not in it, or failed), 0 waiting, " + std::to_string(c1.textures - c1.keyed) + " with no key";
			std::vector<std::string> menu;
			zp::lines(menu);
			const std::string want_menu = "This area: " + std::to_string(c1.drawn) + " of " + std::to_string(c1.textures) + " scenery textures from the pack (2 not in it); 0 coming";
			const std::string want_log = "texture pack in zone 230: " + std::to_string(c1.drawn) + " of " + std::to_string(c1.textures) + " textures from the pack, 2 not in it, 0 failed, 0 gone";
			const bool logged = g_logged.find(want_log) != std::string::npos;
			check(lines().find(want_status) != std::string::npos && !menu.empty() && menu[0] == want_menu && logged,
				"the coverage: /hifi status \"" + want_status.substr(18) + "\", the menu \"" + (menu.empty() ? std::string("(none)") : menu[0]) + "\", the log " +
					(logged ? "\"" + want_log + "...\"" : "without the zone's line: " + g_logged));
		}

		// A zone change: the old zone's owner dropped as it's handed to the retire list
		const uint32_t owners_before = st::get_counts().owners;
		const uint64_t retired_before = st::get_counts().retired;
		g_camera_map = 100;
		gpu_zone *const up100 = zone_up(r, 100, z);
		const st::counts after_change = st::get_counts();
		check(up100 != nullptr && up100->has_pack() && after_change.owners <= owners_before && after_change.retired >= retired_before + c1.drawn,
			"zone 100 in its place: 230's owner dropped as it went to the retire list, its " + std::to_string(c1.drawn) + " textures let go behind it (" +
				std::to_string(after_change.retired - retired_before) + ")");
		const bool settled100 = up100 != nullptr && settle(r, up100);
		check(settled100 && words(up100).drawn > 0, "  100's textures from the pack: " + std::to_string(words(up100).drawn) + " drawn");

		// Off mid-stream: 230 again (a new visit, a new gpu_zone), some drawn, the rest held on the GPU; then off
		g_camera_map = 230;
		gate_value++;
		st::test::gate_copies(gate, gate_value);
		z = zone_up(r, 230, up100);
		for (int i = 0; i < 3000 && st::get_counts().in_flight == 0; ++i)
			present(r);
		gate_value++;
		st::test::gate_copies(gate, gate_value); // (the copies after these held further)
		gate->Signal(gate_value - 1);
		for (int i = 0; i < 2000 && (words(z).drawn == 0 || st::get_counts().in_flight == 0); ++i)
			present(r);
		const gpu_zone::pack_counts mid = words(z);
		const size_t writes_before_off = log_count('w');
		check(z != nullptr && mid.drawn > 0 && st::get_counts().in_flight > 0, "230 again: " + std::to_string(mid.drawn) + " drawn, " + std::to_string(mid.asked) +
			" asked, " + std::to_string(st::get_counts().in_flight) + " copies in flight");
		close_pack(r);
		const std::vector<uint32_t> off = picture(r, z);
		const gpu_zone::pack_counts spent = words(z);
		check(differ(off, game) == 0 && spent.drawn == 0 && spent.asked == 0 && spent.spent == spent.textures && z->pack().owner == 0 && z->pack().done,
			"off mid-stream: the game's picture the next frame; every word spent, the owner dropped, the visit done");
		gate->Signal(gate_value);
		for (int i = 0; i < 200; ++i)
			present(r);
		check(log_count('w') == writes_before_off && in_flight_now() == 0, "  the held copies done: nothing written after, nothing queued or in flight");
		{
			std::vector<std::string> menu;
			zp::lines(menu);
			const bool said = !menu.empty() && menu[0].find("keeps the game's own textures until the next") != std::string::npos;
			check(said && lines().find("texture pack: 0 of") != std::string::npos, "  the menu says so: \"" + (menu.empty() ? std::string("(none)") : menu[0]) + "\"");
		}
		const uint64_t asked_before_on = zp::get_counts().asked;
		const bool reopened = open_pack(r);
		check(reopened, "on again (the pack opened, epoch " + std::to_string(r.epoch) + ")");
		for (int i = 0; i < 100; ++i)
			present(r);
		check(zp::get_counts().asked == asked_before_on && differ(picture(r, z), game) == 0, "  nothing asked for the zone in view: the game's picture till the next");
		g_camera_map = 100;
		gpu_zone *const next = zone_up(r, 100, z);
		for (int i = 0; i < 50; ++i)
			present(r);
		check(next != nullptr && zp::get_counts().asked > asked_before_on, "  the next zone asks (" + std::to_string(zp::get_counts().asked - asked_before_on) + ")");
		check(debug_messages(r.info, first) == 0, "  the debug layer said nothing" + (first.empty() ? "" : ": " + first));

		// 50 switches with copies in flight: off and on, reload, zone changes
		{
			const double c_t0 = now_ms();
			uint32_t with_copies = 0;
			gpu_zone *cur = next;
			for (int k = 0; k < 50; ++k)
			{
				const uint32_t map = k % 2 == 0 ? 230 : 100;
				g_camera_map = map;
				gate_value++;
				st::test::gate_copies(gate, gate_value);
				cur = zone_up(r, map, cur);
				for (int i = 0; i < 3000 && st::get_counts().in_flight == 0; ++i)
					present(r);
				with_copies += st::get_counts().in_flight > 0;
				switch (k % 3)
				{
				case 0:
					close_pack(r);
					present(r);
					open_pack(r);
					break;
				case 1:
					open_pack(r); // (a reload)
					break;
				default:
					break; // (the zone change next does it)
				}
				gate->Signal(gate_value);
				present(r);
			}
			zp::clear_all();
			for (int i = 0; i < 200 && in_flight_now() != 0; ++i)
				present(r);
			for (int i = 0; i < 5; ++i)
				present(r);
			const st::counts c = st::get_counts();
			bad.clear();
			const bool valid = log_valid(bad);
			check(valid && c.leaked == 0 && c.owners == 0 && in_flight_now() == 0 && with_copies >= 40,
				"50 switches (" + std::to_string(with_copies) + " with copies in flight) in " + std::to_string(int(now_ms() - c_t0)) +
					" ms: each slot written once at most per visit, before drawn; nothing left (owners " + std::to_string(c.owners) + ", leaked " +
					std::to_string(c.leaked) + ", held " + std::to_string(c.held) + ")" + (bad.empty() ? "" : ": " + bad));
			check(debug_messages(r.info, first) == 0, "  the debug layer said nothing" + (first.empty() ? "" : ": " + first));
			char worst[200];
			std::snprintf(worst, sizeof(worst), "the glue's worst present so far: %.2f ms (%u views written in it; at most %u requests in one present)", r.worst_ms,
				r.worst_views, zp::get_counts().most_in_a_present);
			check(r.worst_ms < 100.0 && zp::get_counts().most_in_a_present <= zp::kRequestsPerPresent, worst);
		}

		// The request cap and the priorities: 230 with 100 as its second map, both new visits, asked together under a cap
		// of 40 a present while the area settles (kQuiet: nothing read); then the streamer's first picks, the copies held
		{
			open_pack(r);
			r.on = false; // (no glue's frame till both are up)
			st::pause(st::kQuiet);
			gate_value++;
			st::test::gate_copies(gate, gate_value);
			g_camera_map = 230;
			r.map = 230;
			r.second = 100;
			gpu_zone *z0 = nullptr, *z1 = nullptr;
			for (int i = 0; i < 3000; ++i)
			{
				present(r);
				z0 = native::ready_zone(0);
				z1 = native::ready_zone(1);
				if (z0 != nullptr && z1 != nullptr && z0->data().map == 230 && z1->data().map == 100 && z0->has_pack() && z1->has_pack() && !z0->pack().done &&
					!z1->pack().done)
					break;
				z0 = z1 = nullptr;
				Sleep(1);
			}
			const uint32_t k0 = words(z0).keyed, k1 = words(z1).keyed;
			check(z0 != nullptr && z1 != nullptr && k0 > 40 && k0 + k1 > 80 && k0 + k1 <= 120, "230 with 100 as its second map up in pack mode, new visits (" +
				std::to_string(k0) + " and " + std::to_string(k1) + " keyed)");
			if (z0 != nullptr && z1 != nullptr)
			{
				// (each word asked for: asked, or already spent or drawn; counted from before the first request)
				auto taken = [](const gpu_zone *z) { const gpu_zone::pack_counts c = words(z); return c.asked + c.drawn + c.spent; };
				const uint32_t b0 = taken(z0), b1 = taken(z1);
				zp::set_request_cap(40);
				r.on = true;
				uint32_t per[3] = {}, a0[3] = {}, a1[3] = {};
				for (int p = 0; p < 3; ++p)
				{
					present(r);
					per[p] = zp::get_counts().last_requests;
					a0[p] = taken(z0) - b0;
					a1[p] = taken(z1) - b1;
				}
				char line[300];
				std::snprintf(line, sizeof(line), "the cap: %u, %u, %u requests in three presents; the zone's %u, %u, %u, its second map's %u, %u, %u (the zone's all first)",
					per[0], per[1], per[2], a0[0], a0[1], a0[2], a1[0], a1[1], a1[2]);
				check(per[0] == 40 && per[1] == 40 && per[2] == k0 + k1 - 80 && a0[0] == 40 && a1[0] == 0 && a0[1] == k0 && a1[1] == 80 - k0 && a0[2] == k0 &&
					a1[2] == k1, line);
				// (the streamer's picks: never one of the second map's while one of the zone's waits; polled with its mutex
				// held, so no pick moves between the two zones' polls)
				st::resume(st::kQuiet);
				for (int i = 0; i < 3000 && st::get_counts().in_flight == 0; ++i)
					present(r);
				uint32_t zone_waiting = 0, zone_picked = 0, second_picked = 0;
				{
					const std::unique_lock<std::mutex> hold = st::test::hold();
					for (uint32_t i = 0; i < z0->data().textures.size(); ++i)
						if (z0->pack_word(i) == gpu_zone::kPackAsked)
							(st::poll(z0->pack().tickets[i]) == st::state::queued ? zone_waiting : zone_picked)++;
					for (uint32_t i = 0; i < z1->data().textures.size(); ++i)
						if (z1->pack_word(i) == gpu_zone::kPackAsked)
							second_picked += st::poll(z1->pack().tickets[i]) != st::state::queued;
				}
				check(zone_picked > 0 && !(zone_waiting > 0 && second_picked > 0), "  the streamer's picks, the copies held: the zone's " + std::to_string(zone_picked) +
					" taken, " + std::to_string(zone_waiting) + " waiting; its second map's " + std::to_string(second_picked) + " taken");
				zp::set_request_cap(0);
				gate->Signal(gate_value);
				const bool both = settle(r, z0) && settle(r, z1);
				check(both && words(z0).drawn > 0 && words(z1).drawn > 0, "  then both drawn from the pack (" + std::to_string(words(z0).drawn) + " and " +
					std::to_string(words(z1).drawn) + ")");
			}
			zp::set_request_cap(0);
			st::resume(st::kQuiet);
			gate->Signal(gate_value);
			zp::clear_all();
			r.second = 0;
			for (int i = 0; i < 5; ++i)
				present(r);
			check(debug_messages(r.info, first) == 0, "  the debug layer said nothing" + (first.empty() ? "" : ": " + first));
		}

		// Stop during streaming: a new visit, copies held by a gate opened 5 ms into stop; then retire::release_all
		{
			gate_value++;
			st::test::gate_copies(gate, gate_value);
			open_pack(r);
			gpu_zone *const was = native::ready_zone(0);
			g_camera_map = was != nullptr && was->data().map == 230 ? 100 : 230;
			gpu_zone *const s = zone_up(r, g_camera_map, was);
			for (int i = 0; i < 3000 && st::get_counts().in_flight == 0; ++i)
				present(r);
			const uint64_t flying = st::get_counts().in_flight;
			// (the plugin's Release: the module's stop first (clear_all, stream::stop); native's data stays)
			std::thread opener([&] {
				Sleep(5);
				gate->Signal(gate_value);
			});
			const double s0 = now_ms();
			zp::clear_all();
			st::stop();
			const double stop_ms = now_ms() - s0;
			opener.join();
			const st::counts c = st::get_counts();
			check(s != nullptr && flying > 0 && stop_ms < 200.0 && c.leaked == 0 && !c.started,
				"stop with " + std::to_string(flying) + " copies in flight (the gate opened 5 ms in): " + std::to_string(int(stop_ms)) + " ms, nothing left");
			// (then the zone's data let go, the presents the plugin would have made, and everything handed over)
			native::stop();
			for (int i = 0; i < 3; ++i)
			{
				wait_gpu(r);
				hifi::retire::on_present(r.direct);
			}
			wait_gpu(r);
			hifi::retire::release_all();
			const ULONG after = refs(r.device);
			check(hifi::retire::status().held == 0 && after == refs0, "  retire::release_all: nothing held, the device's references back (" + std::to_string(refs0) +
				", " + std::to_string(after) + ")");
		}
		check(debug_messages(r.info, first) == 0, "  the debug layer said nothing" + (first.empty() ? "" : ": " + first));
		gpu_zone::set_test_log(nullptr);
		gate->Release();

		// The device removed mid-copy: a child process
		{
			std::string out;
			const DWORD code = run_child("lost", out);
			while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
				out.pop_back();
			check(code == 0 && out.find("lost: ok") != std::string::npos, "the device removed mid-copy (a child): " + out);
		}
	}

	// This exe again with one argument, its output and exit code (99: not started)
	DWORD run_child(const char *arg, std::string &out)
	{
		{
			char exe[MAX_PATH] = "";
			GetModuleFileNameA(nullptr, exe, MAX_PATH);
			std::string cmd = std::string("\"") + exe + "\" " + arg;
			SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
			HANDLE rd = nullptr, wr = nullptr;
			CreatePipe(&rd, &wr, &sa, 0);
			SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
			STARTUPINFOA si = { sizeof(si) };
			si.dwFlags = STARTF_USESTDHANDLES;
			si.hStdOutput = wr;
			si.hStdError = wr;
			PROCESS_INFORMATION pi = {};
			DWORD code = 99;
			if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
			{
				CloseHandle(wr);
				char buf[512];
				DWORD got = 0;
				while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got != 0)
					out.append(buf, got);
				WaitForSingleObject(pi.hProcess, 60000);
				GetExitCodeProcess(pi.hProcess, &code);
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
			}
			else
				CloseHandle(wr);
			CloseHandle(rd);
			return code;
		}
	}
}

namespace
{
	// ---- The texture pack's heights (M9)
	const wchar_t *const kHeightsFolder = L"build\\test\\zonenative-heights\\";

	// A BC4 level of stripes along u: blocks a column 0 and 255 in turn (4 texels each), down every row
	std::vector<uint8_t> stripes(uint32_t w, uint32_t h)
	{
		const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
		std::vector<uint8_t> b(size_t(bw) * bh * 8, 0);
		for (uint32_t y = 0; y < bh; ++y)
			for (uint32_t x = 0; x < bw; ++x)
			{
				uint8_t *p = b.data() + (size_t(y) * bw + x) * 8;
				p[0] = p[1] = (x & 1) != 0 ? 255 : 0; // (r0 == r1: every code 0 is r0)
			}
		return b;
	}

	// The made-up pack of 230's keyed textures (sides multiples of 4), in turn: a colour and a height, a height alone (a
	// materials-only entry), a colour alone. The colours as make_pack's; the heights the textures' own size (a 2x zone's
	// half), stripes, span 1
	bool make_height_pack(const zm::zone &z, uint32_t &with_height, uint32_t &height_only, uint32_t &colour_only)
	{
		remove_tree(kHeightsFolder);
		pk::writer w;
		pk::writer_options o;
		o.folder = kHeightsFolder;
		o.source = "zonenative_test heights";
		std::string why;
		if (!w.open(o, why))
		{
			note("the made-up heights pack couldn't be opened: " + why);
			return false;
		}
		with_height = height_only = colour_only = 0;
		uint32_t n = 0;
		for (const zm::texture &t : z.textures)
		{
			if (t.key.bytes == 0 || t.key.w % 4 != 0 || t.key.h % 4 != 0)
				continue;
			const uint32_t kind = n++ % 3;
			pk::entry e;
			e.hash = t.key.hash;
			e.bytes = t.key.bytes;
			e.w = t.key.w;
			e.h = t.key.h;
			e.orig_format = t.format == zm::kBC2 ? 0xA1 : 0xB1;
			e.source_kind = pk::kSourceZone;
			e.file_id = z.map;
			std::memcpy(e.name, t.name, 16);
			std::vector<pk::map_in> maps;
			if (kind != 1)
			{
				pk::map_in m;
				m.format = pk::kBC2;
				m.w = t.key.w * 2;
				m.h = t.key.h * 2;
				const uint32_t levels = pk::full_levels(m.w, m.h);
				for (uint32_t l = 0; l < levels; ++l)
					m.levels.push_back(solid(std::max(1u, m.w >> l), std::max(1u, m.h >> l), l == 0 ? 0xF800 : 0x001F));
				maps.push_back(std::move(m));
			}
			if (kind != 2)
			{
				pk::map_in m;
				m.kind = pk::kHeight;
				m.format = pk::kBC4;
				m.w = t.key.w;
				m.h = t.key.h;
				const float span = 1.0f;
				std::memcpy(&m.param, &span, 4);
				const uint32_t levels = pk::full_levels(m.w, m.h);
				for (uint32_t l = 0; l < levels; ++l)
					m.levels.push_back(stripes(std::max(1u, m.w >> l), std::max(1u, m.h >> l)));
				maps.push_back(std::move(m));
			}
			if (w.add(e, maps, why) != pk::add_result::ok)
			{
				note("a made-up entry refused: " + why);
				return false;
			}
			(kind == 0 ? with_height : kind == 1 ? height_only : colour_only)++;
		}
		const bool ok = w.commit(why);
		w.close();
		if (!ok)
			note("the made-up heights pack couldn't be written: " + why);
		return ok;
	}

	// A light split word's facing (light_split.hpp, as grass.hlsl light_at decodes it)
	void facing_of(uint32_t v, float n[3])
	{
		float ox = float((v >> 11) & 127) / 127.0f * 2.0f - 1.0f, oy = float((v >> 18) & 127) / 127.0f * 2.0f - 1.0f;
		float z = 1.0f - std::fabs(ox) - std::fabs(oy);
		const float t = std::clamp(-z, 0.0f, 1.0f);
		ox += ox >= 0.0f ? -t : t;
		oy += oy >= 0.0f ? -t : t;
		const float l = std::sqrt(ox * ox + oy * oy + z * z);
		n[0] = ox / l;
		n[1] = oy / l;
		n[2] = z / l;
	}

	double light_mean(const std::vector<uint32_t> &a)
	{
		double s = 0.0;
		for (uint32_t p : a)
			s += 0.2126 * double(p & 0xFF) + 0.7152 * double((p >> 8) & 0xFF) + 0.0722 * double((p >> 16) & 0xFF);
		return s / double(std::max<size_t>(a.size(), 1));
	}

	void heights_part(bool on_gpu, bool gbv)
	{
		std::printf("\nThe texture pack's heights (%s%s)\n", on_gpu ? "the GPU" : "WARP", gbv ? ", GPU-based validation" : "");
		SetEnvironmentVariableW(L"HIFIXI_PIVOT_INI", L"build\\test\\zonenative\\no-pivot.ini");
		zm::zone z230;
		hifi::bake::result res;
		const bool read = zm::load(230, z230, res, true);
		uint32_t with_height = 0, height_only = 0, colour_only = 0;
		const bool made = read && make_height_pack(z230, with_height, height_only, colour_only);
		const uint32_t heights = with_height + height_only;
		check(made && with_height > 0 && height_only > 0 && colour_only > 0, "the made-up heights pack under build\\test\\zonenative-heights: " +
			std::to_string(with_height) + " with a colour and a height, " + std::to_string(height_only) + " a height alone, " + std::to_string(colour_only) +
			" a colour alone");
		if (!made)
			return;

		IDXGIFactory4 *factory = nullptr;
		IDXGIAdapter *warp = nullptr;
		CreateDXGIFactory1(IID_PPV_ARGS(&factory));
		factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
		rig r;
		if (FAILED(D3D12CreateDevice(on_gpu ? nullptr : warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&r.device))))
		{
			check(false, "a device for the heights' part");
			return;
		}
		r.device->QueryInterface(IID_PPV_ARGS(&r.info));
		if (r.info != nullptr)
			r.info->SetMuteDebugOutput(TRUE);
		check(make_rig(r), "the zone renderer, a target and the game's queue on the device");
		std::string first;
		g_log.clear();
		gpu_zone::set_test_log(on_log);
		zp::set_camera_source(camera_source);
		zp::set_log(on_glue_log);
		g_camera_map = 230;
		st::start(r.device, 32);
		const bool opened = open_pack(r, kHeightsFolder);
		check(opened, "the heights pack opened");

		// [textures] height=0 first: no height asked
		zp::set_mode(true, false, false);
		gpu_zone *const z = zone_up(r, 230);
		const bool settled = z != nullptr && settle(r, z);
		for (int i = 0; i < 20; ++i)
			present(r);
		const gpu_zone::pack_counts c0 = words(z);
		const uint64_t bytes0 = z != nullptr ? z->bytes() : 0;
		check(settled && c0.drawn == with_height + colour_only && c0.spent == c0.keyed - c0.drawn && c0.heights_asked + c0.heights_drawn + c0.heights_spent == 0 &&
				zp::get_counts().heights_asked == 0,
			"heights off: 230's colours settled (" + std::to_string(c0.drawn) + " from the pack, " + std::to_string(c0.spent) + " spent), no height asked");
		std::vector<uint32_t> light_none, light_h, light_off;
		const std::vector<uint32_t> pp_none = picture(r, z, 0.0f, true, &light_none), flat_none = picture(r, z, 0.0f, false);

		// On: each height asked once and drawn, the colour-less ones too
		zp::set_mode(true, false, true);
		gpu_zone::pack_counts c1;
		for (int i = 0; i < 4000; ++i)
		{
			c1 = words(z);
			if (c1.heights_drawn + c1.heights_spent == heights && c1.heights_asked == 0)
				break;
			present(r);
		}
		const zp::counts n1 = zp::get_counts();
		check(c1.heights_drawn == heights && c1.heights_spent == 0 && n1.heights_asked == heights && n1.heights_drawn == heights && c1.drawn == c0.drawn &&
				z->bytes() > bytes0,
			"heights on: " + std::to_string(c1.heights_drawn) + " of " + std::to_string(heights) + " drawn (" + std::to_string(height_only) +
				" without a colour of their own), each asked once (" + std::to_string(n1.heights_asked) + "); the zone's bytes " + std::to_string(bytes0) + " -> " +
				std::to_string(z->bytes()));
		std::vector<std::string> area;
		zp::lines(area);
		bool relief_line = false;
		for (const std::string &l : area)
			relief_line = relief_line || l == "Relief from the pack on " + std::to_string(heights) + " of them.";
		check(relief_line, "  the menu's relief line" + (area.size() > 1 ? ": " + area[1] : std::string()));

		const std::vector<uint32_t> pp_h = picture(r, z, 0.0f, true, &light_h), flat_h = picture(r, z, 0.0f, false);
		const size_t changed = differ(pp_h, pp_none);
		const double m0 = light_mean(pp_none), m1 = light_mean(pp_h);
		check(changed != SIZE_MAX && changed > pp_h.size() / 50 && std::fabs(m1 - m0) <= 0.03 * m0,
			"per pixel: the relief changes " + std::to_string(changed) + " of " + std::to_string(pp_h.size()) + " pixels, the light on the whole " +
				std::to_string(m0) + " -> " + std::to_string(m1) + " (within 3%)");
		check(differ(flat_h, flat_none) == 0, "per pixel off: byte for byte the picture without heights");
		// The light split: words differ only where the relief lit them, each with its depth, its facing turned away
		size_t words_changed = 0, marked = 0, bad = 0;
		for (size_t i = 0; i < light_h.size() && i < light_none.size(); ++i)
		{
			if (light_h[i] == light_none[i])
				continue;
			words_changed++;
			float a[3], b[3];
			facing_of(light_h[i], a);
			facing_of(light_none[i], b);
			const bool same_depth = (light_h[i] & 0x7FF) == (light_none[i] & 0x7FF);
			const bool turned = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] < -0.9f;
			if (same_depth && turned)
				marked++;
			else if (((light_h[i] >> 11) & 0x3FFF) != ((light_none[i] >> 11) & 0x3FFF) || !same_depth)
				bad++;
		}
		check(light_h.size() == size_t(W) * H && marked > 0 && bad == 0 && marked == words_changed,
			"  the light split: " + std::to_string(words_changed) + " words differ, every one the relief's: its depth as it was, its facing turned away (" +
				std::to_string(marked) + " marked, " + std::to_string(bad) + " otherwise)");

		// Off again: byte for byte the pictures without heights; on again: the relief at once
		zp::set_mode(true, false, false);
		const std::vector<uint32_t> pp_off = picture(r, z, 0.0f, true, &light_off);
		check(differ(pp_off, pp_none) == 0 && light_off == light_none && words(z).heights_drawn == heights,
			"heights off again: the picture and the light split byte for byte the ones without heights (the heights kept for the visit)");
		zp::set_mode(true, false, true);
		check(differ(picture(r, z, 0.0f, true), pp_h) == 0, "  on again: the relief at once");
		zp::set_compare(true);
		const std::vector<uint32_t> cmp_h = picture(r, z, 0.0f, true);
		zp::set_mode(true, false, false);
		const std::vector<uint32_t> cmp_off = picture(r, z, 0.0f, true);
		zp::set_mode(true, false, true);
		zp::set_compare(false);
		check(differ(cmp_h, cmp_off) == 0 && differ(cmp_h, pp_h) != 0, "  compare: the game's look, without the relief");

		// clear_all: every height spent; the log valid for the heights' words too
		zp::clear_all();
		const gpu_zone::pack_counts c2 = words(z);
		std::string why_bad;
		const bool valid = log_valid(why_bad);
		size_t height_events = 0;
		for (const auto &[o, ts] : g_log)
			for (const auto &[t, ev] : ts)
				height_events += (t & gpu_zone::kHeightLog) != 0 ? 1 : 0;
		check(c2.heights_drawn == 0 && c2.heights_spent == heights && valid && height_events == heights,
			"clear_all: every height spent; the slot-write log valid (" + std::to_string(height_events) + " heights' words: a, w, d, s)" +
				(valid ? std::string() : ": " + why_bad));
		check(debug_messages(r.info, first) == 0, "  the debug layer said nothing" + (first.empty() ? "" : ": " + first));

		// Let go as the plugin would
		close_pack(r);
		st::stop();
		native::stop();
		for (int i = 0; i < 3; ++i)
		{
			wait_gpu(r);
			hifi::retire::on_present(r.direct);
		}
		wait_gpu(r);
		hifi::retire::release_all();
		gpu_zone::set_test_log(nullptr);
		drop_light_target();
		g_camera_map = 0;
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc > 1 && std::strcmp(argv[1], "lost") == 0)
		return lost_child();
	// (the debug layer for all of it, before the first device; HIFIXI_GBV=1: GPU-based validation too)
	const bool gbv = GetEnvironmentVariableA("HIFIXI_GBV", nullptr, 0) != 0;
	if (ID3D12Debug *debug = nullptr; SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
	{
		debug->EnableDebugLayer();
		ID3D12Debug1 *debug1 = nullptr;
		if (gbv && SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))))
		{
			debug1->SetEnableGPUBasedValidation(TRUE);
			debug1->Release();
		}
		debug->Release();
	}
	// ("heights": the heights' part alone, in a process of its own: the main run starts it so, after the rest)
	if (argc > 1 && std::strcmp(argv[1], "heights") == 0)
	{
		uintptr_t base = 0;
		if (dump::map(dump::kDumps[0], base) == 0)
		{
			check(false, "the FFXiMain dump mapped");
			return 1;
		}
		heights_part(GetEnvironmentVariableA("ZONE_GPU", nullptr, 0) != 0, GetEnvironmentVariableA("HIFIXI_GBV", nullptr, 0) != 0);
		dump::unmap(base);
		std::printf("\n%s (%d)\n", failures == 0 ? "all passed" : "failed", failures == 0 ? passes : failures);
		return failures;
	}
	uintptr_t base = 0;
	if (dump::map(dump::kDumps[0], base) == 0)
	{
		check(false, "the FFXiMain dump mapped");
		return 1;
	}
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	ID3D12Device *device = nullptr;
	if (FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
	{
		check(false, "a WARP device");
		return 1;
	}

	native::frame(device, 100, 0, false);
	check(native::ready_zone() == nullptr, "not wanted: nothing loaded" + lines());
	int presents = 0;
	hifi::zone::gpu_zone *z = present_until_ready(device, 100, presents);
	check(z != nullptr && z->data().models.size() == 137, "zone 100 on the GPU after " + std::to_string(presents) + " presents" + lines());
	check(z != nullptr && z->data().vertices.empty() && z->data().textures.size() == 32 && z->data().textures[0].data.empty(),
		"  the big arrays let go on the CPU, the tables kept");
	check(z != nullptr && !z->has_pack() && !z->data().pack && z->data().textures[0].key.bytes == 0, "  not in the texture pack's mode: no keys, no pack region");
	native::frame(device, 100, 0, true);
	check(native::ready_zone() == z, "  the same zone again: kept as it is");

	hifi::zone::gpu_zone *y = present_until_ready(device, 230, presents);
	check(y != nullptr && y != z && y->data().models.size() == 252, "zone 230 in its place after " + std::to_string(presents) + " presents" + lines());
	check(hifi::retire::status().held == 1, "  zone 100 kept (the GPU may still read it: host/retire.hpp)");
	{
		// (the game's queue: the fence signalled at each present, as backend::present does)
		ID3D12CommandQueue *queue = nullptr;
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
		for (int i = 0; i < 300 && hifi::retire::status().held != 0; ++i)
		{
			native::frame(device, 230, 0, true);
			hifi::retire::on_present(queue);
			Sleep(2);
		}
		// (handed over before the fence's first signal: counted from it, so let go once it has passed 2)
		check(hifi::retire::status().held == 0 && hifi::retire::status().signalled >= 2,
			"  and let go once the fence passed it (" + std::to_string(hifi::retire::status().signalled) + " presents)");
		queue->Release();
	}

	native::frame(device, 5000, 0, true); // (no such map)
	for (int i = 0; i < 2000 && lines().find("not loaded") == std::string::npos; ++i)
	{
		Sleep(2);
		native::frame(device, 5000, 0, true);
	}
	check(lines().find("zone 5000 not loaded") != std::string::npos, "a map with no DAT: said why, and not tried again" + lines());
	check(native::ready_zone() == nullptr, "  the zone before isn't handed out for it");

	// A second map (the renderer's slot 1): Port Windurst and its Fishermen's Guild (map 412), each in its own slot
	hifi::zone::gpu_zone *pw = nullptr, *guild = nullptr;
	for (presents = 1; presents <= 3000; ++presents)
	{
		native::frame(device, 240, 412, true);
		pw = native::ready_zone(0);
		guild = native::ready_zone(1);
		if (pw != nullptr && guild != nullptr)
			break;
		Sleep(2);
	}
	check(pw != nullptr && pw->data().map == 240 && guild != nullptr && guild->data().map == 412,
		"Port Windurst and map 412 on the GPU together after " + std::to_string(presents) + " presents" + lines());
	check(lines().find("second map 412 on the GPU") != std::string::npos, "  the status names the second map");
	native::frame(device, 240, 0, true);
	check(native::ready_zone(0) == pw && native::ready_zone(1) == nullptr, "  the second map left: slot 1 empty, the zone kept");
	native::frame(device, 0, 412, true);
	check(native::ready_zone(1) == nullptr, "  no zone: no second map either");

	native::frame(device, 230, 0, false);
	check(native::ready_zone() == nullptr, "not wanted any more: let go");
	native::stop();
	check(native::status().size() == 1 && native::status()[0].find("not loaded") != std::string::npos, "stop(): nothing left" + lines());
	device->Release();
	hifi::retire::release_all();

	pack_part(GetEnvironmentVariableA("ZONE_GPU", nullptr, 0) != 0, gbv);
	{
		// (the heights' part in a process of its own, as "zonenative_test heights" runs it: its streamer and loaders started
		// afresh; its lines passed on, each check of it counted here)
		std::string out;
		const DWORD code = run_child("heights", out);
		std::printf("%s", out.c_str());
		size_t at = 0;
		int child_ok = 0, child_failed = 0;
		while ((at = out.find('\n', at)) != std::string::npos)
		{
			++at;
			child_ok += out.compare(at, 4, "ok  ") == 0;
			child_failed += out.compare(at, 4, "FAIL") == 0;
		}
		child_ok += out.compare(0, 4, "ok  ") == 0;
		passes += child_ok;
		failures += child_failed;
		check(code == 0 && child_failed == 0 && child_ok > 0, "the heights' part (a child): exit " + std::to_string(code));
	}
	dump::unmap(base);
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
