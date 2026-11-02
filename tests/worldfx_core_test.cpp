/*
 * World FX's core (src\render\worldfx_core.cpp) pinned on WARP with the debug layer and GPU-based validation, fed as
 * HiFi XI's backend feeds it (src\backend\backend.cpp), minus the hooks: a characterization test, so that the core can
 * be rewritten against it. The test records frames as our d3d8.dll does (src\host\frameapi.hpp's marks: the zone's
 * begin, the zone's end, the world's end) and goes through one adapter to the core's API (WFX_CORE_API: how far the
 * re-architecture's step 4 has come: 1 the stream events, 2 the marks, 3 no lock while recording, 4 the shaders compiled
 * on their own thread at attach; each level's changes on purpose behind it). A stand-in FFXiMain.dll (tests\fake_ffximain.cpp) holds the game's
 * camera (looking away from every blade); the host's camera source gives the grass's.
 *
 *   build\worldfx_core_test.exe [--quick]      (from this folder; --quick: no GPU-based validation, for working on it)
 *
 * Checked (C1..C18 as the design has them; "today" marks what the rewrite changes on purpose):
 *   C1  attached, nothing drawn, no world before any mark
 *   C2  a present with no marks: "no world target yet"; the zone's blades read (their status line exact)
 *   C9  (today) the first frame with the depth still DEPTH_READ: the core assumes DEPTH_WRITE (errors reported, not failed)
 *   C3  a full frame: blades at the zone's end, pixels (changed, at the centre, none behind the wall, alpha untouched),
 *       counts, status lines (last frame, the fog, the camera, the world), no debug-layer errors
 *   C4  the host's camera source off: the game's (away) camera, nothing drawn (today counted as drawn)
 *   C5  not in the field; then the camera not set
 *   C6  out of the game: nothing counted (today the first frame still draws), then "not in the game" counted
 *   C7  the zone guard: nothing drawn till the new zone's files are read; zone 104's statuses; "no blades for this zone"
 *   C8  Grass off: "off"; get_setting and the port at once; on again: drawn
 *   C10 a D32 world depth: no blades at the zone's end, all of it at the world's end
 *   C11 three world ends on one list: the second and third ignored; three lists (level 1: the kMaxPasses warning; 2 on:
 *       one zone's begin before them, drawn at the first alone)
 *   C12 (today) the world's view held by another resource: skipped
 *   C13 no zone markers: the world's end not marked; marked() for 8 presents
 *   C14 worldfx.hlsl the same as the built-in shaders: said so within 31 presents
 *   C16 FloatWorld: the world remade as RGBA16F, drawn, "light past white on"
 *   C15 release behind the fence: the queue held on a fence of the test's, the zone changed: the old blades kept while
 *       the GPU is behind, freed once it catches up
 *   thr three threads (the game's, a feed thread calling every entry the addon and the menu call, a copy-queue loader),
 *       a watchdog; a probe inside the blades' draw: today a feed call waits for the core's lock (ok false, the overlay
 *       empty: asserted as today's behaviour)
 *   C17 detach: nothing of World FX's left on the device
 *   C18 every present in the game counted once, drawn or skipped
 * Any debug-layer error fails the test (C9 today aside). A deadlock fails it through the watchdog (180 s).
 */

#include <windows.h>
#include "worldfx_core.hpp" // (src\render) worldfx::*, grass::*
#include "frameapi.hpp"     // (src\host) hifi_frame_mark
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WFX_CORE_API
#define WFX_CORE_API 1
#endif

extern "C" {
void __cdecl worldfx_zone(uint32_t zone, uint32_t in_game);
uint32_t __cdecl worldfx_drawing();
void __cdecl worldfx_last_draw(uint32_t out[5]);
void __cdecl worldfx_world(uint32_t out[4]);
void __cdecl worldfx_grass_actors2(const float *xyzr, const uint32_t *flags, uint32_t count);
void __cdecl worldfx_grass_cut(const float *swaths, uint32_t count);
void __cdecl worldfx_lamps(const uint32_t *words, uint32_t count);
void __cdecl worldfx_glows(const uint32_t *words, uint32_t count);
void __cdecl worldfx_glow_stats(uint32_t out[5]);
void __cdecl worldfx_weather(const uint32_t *words, uint32_t count);
void __cdecl worldfx_moon(uint32_t clock);
uint32_t __cdecl worldfx_weather_drawn();
uint32_t __cdecl worldfx_casts_characters();
uint32_t __cdecl worldfx_set_setting(const char *key, float value);
}

namespace
{
	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (ok)
			passes++;
		else
			failures++;
	}

	template <typename T>
	void release(T *&p)
	{
		if (p != nullptr)
			p->Release();
		p = nullptr;
	}

	double seconds_since(std::chrono::steady_clock::time_point t)
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
	}

	// ---- the watchdog: where each thread is and its heartbeat; no progress for kStuckSeconds fails the test
	constexpr int kStuckSeconds = 180;
	std::atomic<const char *> a_at{ "start" }, b_at{ "not started" };
	std::atomic<uint64_t> a_beat{ 0 }, b_beat{ 0 };
	std::atomic<bool> b_running{ false }, watch_stop{ false };

	void watchdog()
	{
		uint64_t a_last = ~0ull, b_last = ~0ull;
		auto a_since = std::chrono::steady_clock::now(), b_since = a_since;
		while (!watch_stop)
		{
			Sleep(250);
			const auto now = std::chrono::steady_clock::now();
			if (a_beat != a_last)
				a_last = a_beat, a_since = now;
			if (b_beat != b_last || !b_running)
				b_last = b_beat, b_since = now;
			if (seconds_since(a_since) > kStuckSeconds || seconds_since(b_since) > kStuckSeconds)
			{
				std::printf("FAIL deadlock: game thread at %s, feed thread at %s\n", a_at.load(), b_at.load());
				std::fflush(stdout);
				TerminateProcess(GetCurrentProcess(), 3);
			}
		}
	}

	void at(const char *where)
	{
		a_at = where;
		a_beat++;
	}

	// ---- the port (thread-safe: set_* comes from any thread, under the core's lock)
	std::mutex port_mutex;
	std::map<std::string, std::string> settings;
	std::vector<std::string> log_lines;
	struct view
	{
		ID3D12Resource *res;
		DXGI_FORMAT fmt;
	};
	std::map<uint64_t, view> views;

	void p_log(int, const char *t)
	{
		std::lock_guard<std::mutex> l(port_mutex);
		log_lines.push_back(t);
	}
	bool p_value(const char *k, std::string &out)
	{
		std::lock_guard<std::mutex> l(port_mutex);
		const auto i = settings.find(k);
		if (i == settings.end())
			return false;
		out = i->second;
		return true;
	}
	bool p_get_bool(const char *k, bool &v)
	{
		std::string s;
		if (!p_value(k, s))
			return false;
		v = std::atoi(s.c_str()) != 0;
		return true;
	}
	bool p_get_float(const char *k, float &v)
	{
		std::string s;
		if (!p_value(k, s))
			return false;
		v = std::strtof(s.c_str(), nullptr);
		return true;
	}
	bool p_get_int(const char *k, int &v)
	{
		std::string s;
		if (!p_value(k, s))
			return false;
		v = std::atoi(s.c_str());
		return true;
	}
	void p_set(const char *k, const std::string &v)
	{
		std::lock_guard<std::mutex> l(port_mutex);
		settings[k] = v;
	}
	void p_set_bool(const char *k, bool v) { p_set(k, v ? "1" : "0"); }
	void p_set_float(const char *k, float v)
	{
		char s[32];
		std::snprintf(s, sizeof(s), "%g", double(v));
		p_set(k, s);
	}
	void p_set_int(const char *k, int v) { p_set(k, std::to_string(v)); }
	ID3D12Resource *p_resource_of_view(uint64_t v)
	{
		std::lock_guard<std::mutex> l(port_mutex);
		const auto i = views.find(v);
		return i != views.end() ? i->second.res : nullptr;
	}
	DXGI_FORMAT p_format_of_view(uint64_t v)
	{
		std::lock_guard<std::mutex> l(port_mutex);
		const auto i = views.find(v);
		return i != views.end() ? i->second.fmt : DXGI_FORMAT_UNKNOWN;
	}
	void set_view(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource *res, DXGI_FORMAT fmt)
	{
		std::lock_guard<std::mutex> l(port_mutex);
		views[h.ptr] = { res, fmt };
	}
	size_t log_count()
	{
		std::lock_guard<std::mutex> l(port_mutex);
		return log_lines.size();
	}
	// A line logged since index from that has text in it
	bool logged_since(size_t from, const char *text)
	{
		std::lock_guard<std::mutex> l(port_mutex);
		for (size_t i = from; i < log_lines.size(); ++i)
			if (log_lines[i].find(text) != std::string::npos)
				return true;
		return false;
	}

	// ---- the camera: the stand-in FFXiMain's (away from the blades) and the host's source (the grass's view)
	using set_camera_fn = void (*)(const float *view, const float *proj, int in_field);
	set_camera_fn fake_set_camera = nullptr;
	float grass_view[16], grass_proj[16], away_view[16];
	std::atomic<bool> source_on{ true };
	bool fake_in_field = true, fake_proj_zero = false;
	bool camera_source(void *, float v[16], float p[16])
	{
		if (!source_on)
			return false;
		std::memcpy(v, grass_view, 64);
		std::memcpy(p, grass_proj, 64);
		return true;
	}
	void set_game_camera()
	{
		static const float zero[16] = {};
		fake_set_camera(away_view, fake_proj_zero ? zero : grass_proj, fake_in_field ? 1 : 0);
	}

	struct vec3
	{
		float x, y, z;
	};
	vec3 sub(vec3 a, vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	vec3 cross(vec3 a, vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	vec3 normalize(vec3 a)
	{
		const float l = std::sqrt(dot(a, a));
		return { a.x / l, a.y / l, a.z / l };
	}
	// Right-handed look-at, row vectors (D3DXMatrixLookAtRH), as grass_test.cpp
	void look_at(vec3 eye, vec3 to, vec3 up, float m[16])
	{
		const vec3 z = normalize(sub(eye, to));
		const vec3 x = normalize(cross(up, z));
		const vec3 y = cross(z, x);
		const float v[16] = { x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1 };
		std::memcpy(m, v, sizeof(v));
	}
	// The game's projection: infinite far plane, near 0.1
	void projection(float aspect, float m[16])
	{
		const float b = 1.26f;
		const float p[16] = { b / aspect, 0, 0, 0, 0, b, 0, 0, 0, 0, -1, -1, 0, 0, -0.1f, 0 };
		std::memcpy(m, p, sizeof(p));
	}

	// ---- the host's callbacks; the probe (the threads part) inside the blades' draw
	HANDLE ev_inside = nullptr, ev_feed_done = nullptr;
	std::atomic<bool> probe_armed{ false };
	std::atomic<int> probe_hits{ 0 };
	bool probe_ok = false;
	bool bounds_probe(void *, float *, float *, uint64_t *)
	{
		if (probe_armed.exchange(false))
		{
			probe_hits++;
			SetEvent(ev_inside);
			probe_ok = WaitForSingleObject(ev_feed_done, 3000) == WAIT_OBJECT_0;
		}
		return false;
	}
	uint32_t zone_draw_none(void *, ID3D12GraphicsCommandList *, const float *, D3D12_CPU_DESCRIPTOR_HANDLE, UINT) { return 0; }
	uint32_t floor_none(void *, ID3D12GraphicsCommandList *, const float *, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, UINT) { return 0; }

	// ---- the status, captured (each thread its own lines)
	thread_local std::vector<std::string> *sink = nullptr;
	void ui_text(const char *t)
	{
		if (sink != nullptr)
			sink->push_back(t);
	}
	void ui_text_colored(const float *, const char *t) { ui_text(t); }
	bool ui_checkbox(const char *, bool *) { return false; }
	bool ui_slider_float(const char *, float *, float, float, const char *) { return false; }
	bool ui_slider_int(const char *, int *, int, int) { return false; }
	bool ui_color_edit3(const char *, float *) { return false; }
	bool ui_button(const char *) { return false; }
	bool ui_header(const char *, bool) { return true; }
	worldfx::ui capture_ui()
	{
		worldfx::ui u;
		u.checkbox = ui_checkbox;
		u.slider_float = ui_slider_float;
		u.slider_int = ui_slider_int;
		u.color_edit3 = ui_color_edit3;
		u.button = ui_button;
		u.header = ui_header;
		u.text = ui_text;
		u.text_colored = ui_text_colored;
		u.text_disabled = ui_text;
		u.text_wrapped = ui_text;
		return u;
	}
	std::vector<std::string> status()
	{
		std::vector<std::string> lines;
		sink = &lines;
		worldfx::draw_status(capture_ui());
		sink = nullptr;
		return lines;
	}
	// The first status line starting with prefix ("" none)
	std::string line_of(const std::vector<std::string> &lines, const char *prefix)
	{
		for (const std::string &l : lines)
			if (l.rfind(prefix, 0) == 0)
				return l;
		return "";
	}

	// ---- frame_counts parsed: "World FX's passes: N frames in the game drawn; K not: <reason>; ..." (a reason may hold
	// "; " itself: the next one starts at "; <digits> not: ")
	struct counts
	{
		uint64_t drawn = 0;
		std::map<std::string, uint64_t> skipped;
		uint64_t skipped_total() const
		{
			uint64_t n = 0;
			for (const auto &k : skipped)
				n += k.second;
			return n;
		}
	};
	counts parse_counts(const std::string &s)
	{
		counts c;
		const char head[] = "World FX's passes: ";
		if (s.rfind(head, 0) != 0)
			return c;
		c.drawn = std::strtoull(s.c_str() + sizeof(head) - 1, nullptr, 10);
		std::vector<size_t> starts;
		for (size_t i = 0; i + 3 < s.size(); ++i)
			if (s[i] == ';' && s[i + 1] == ' ' && s[i + 2] >= '0' && s[i + 2] <= '9')
			{
				size_t k = i + 2;
				while (k < s.size() && s[k] >= '0' && s[k] <= '9')
					++k;
				if (s.compare(k, 6, " not: ") == 0)
					starts.push_back(i);
			}
		for (size_t n = 0; n < starts.size(); ++n)
		{
			const size_t i = starts[n] + 2;
			const uint64_t v = std::strtoull(s.c_str() + i, nullptr, 10);
			const size_t r = s.find(" not: ", i) + 6;
			const size_t end = n + 1 < starts.size() ? starts[n + 1] : s.size();
			c.skipped[s.substr(r, end - r)] = v;
		}
		return c;
	}
	counts counts_now()
	{
		const std::vector<std::string> v = worldfx::frame_counts();
		return parse_counts(v.empty() ? std::string() : v[0]);
	}
	counts delta(const counts &before, const counts &after)
	{
		counts d;
		d.drawn = after.drawn - before.drawn;
		for (const auto &k : after.skipped)
		{
			const auto b = before.skipped.find(k.first);
			const uint64_t n = k.second - (b != before.skipped.end() ? b->second : 0);
			if (n != 0)
				d.skipped[k.first] = n;
		}
		return d;
	}
	// Exactly one frame skipped, for this reason
	bool one_skip(const counts &d, const char *reason) { return d.drawn == 0 && d.skipped_total() == 1 && d.skipped.count(reason) == 1; }
	std::string describe(const counts &d)
	{
		std::string s = std::to_string(d.drawn) + " drawn";
		for (const auto &k : d.skipped)
			s += "; " + std::to_string(k.second) + " not: " + k.first;
		return s;
	}

	// ---- the GPU
	constexpr uint32_t kRing = 6;
	struct gpu
	{
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		ID3D12InfoQueue *info = nullptr;
		ID3D12DebugDevice *dbg = nullptr;
		ID3D12CommandAllocator *alloc[kRing] = {};
		ID3D12GraphicsCommandList *list[kRing] = {};
		ID3D12Fence *fence = nullptr;
		uint64_t done[kRing] = {}, value = 0;
		uint32_t next = 0;
		HANDLE event = nullptr;
		ID3D12DescriptorHeap *rtv_heap = nullptr, *dsv_heap = nullptr;
		UINT rtv_step = 0, dsv_step = 0, rtvs = 0, dsvs = 0;
		// (the debug layer's messages so far: errors, objects deleted while in use, the live objects' lines)
		int errors = 0, deleted_in_use = 0;
		std::vector<std::string> live;

		void wait(uint64_t v)
		{
			if (fence->GetCompletedValue() >= v)
				return;
			fence->SetEventOnCompletion(v, event);
			while (WaitForSingleObject(event, 1000) == WAIT_TIMEOUT)
				if (fence->GetCompletedValue() >= v)
					break;
		}
		void idle()
		{
			queue->Signal(fence, ++value);
			wait(value);
		}
		void drain(bool print = true)
		{
			if (info == nullptr)
				return;
			const UINT64 n = info->GetNumStoredMessages();
			for (UINT64 i = 0; i < n; ++i)
			{
				SIZE_T size = 0;
				info->GetMessage(i, nullptr, &size);
				std::vector<char> mem(size);
				auto *m = reinterpret_cast<D3D12_MESSAGE *>(mem.data());
				info->GetMessage(i, m, &size);
				if (m->ID == D3D12_MESSAGE_ID_OBJECT_DELETED_WHILE_STILL_IN_USE)
					deleted_in_use++;
				if (std::strstr(m->pDescription, "Live ") != nullptr)
					live.push_back(m->pDescription);
				else if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
				{
					if (print && errors < 20)
						std::printf("  debug layer: %s\n", m->pDescription);
					errors++;
				}
			}
			info->ClearStoredMessages();
		}
		// The errors since the last take (the GPU idle first: GPU-based validation reports after the work)
		int take_errors(bool print = true)
		{
			drain(print);
			const int e = errors;
			errors = 0;
			return e;
		}
		// Live objects whose name starts with name
		uint32_t live_named(const char *name)
		{
			drain();
			live.clear();
			dbg->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
			drain();
			const std::string key = std::string("Name: ") + name;
			uint32_t n = 0;
			for (const std::string &l : live)
				n += l.find(key) != std::string::npos ? 1 : 0;
			live.clear();
			return n;
		}

		ID3D12Resource *texture(DXGI_FORMAT format, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear)
		{
			D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = w;
			desc.Height = h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.Flags = flags;
			ID3D12Resource *res = nullptr;
			device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&res));
			return res;
		}
		ID3D12Resource *buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
		{
			D3D12_HEAP_PROPERTIES heap = { type };
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = size;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ID3D12Resource *res = nullptr;
			device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
			return res;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE new_rtv() { return { rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(rtvs++) * rtv_step }; }
		D3D12_CPU_DESCRIPTOR_HANDLE new_dsv() { return { dsv_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(dsvs++) * dsv_step }; }
	};

	// Resources let go only at the end (the core may hold a pointer to an old world until its next zone begin)
	std::vector<IUnknown *> graveyard;

	void barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
	{
		D3D12_RESOURCE_BARRIER x = {};
		x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		x.Transition.pResource = r;
		x.Transition.Subresource = sub;
		x.Transition.StateBefore = a;
		x.Transition.StateAfter = b;
		l->ResourceBarrier(1, &x);
	}

	// ---- the world as our d3d8.dll keeps it
	struct depth_t
	{
		ID3D12Resource *res = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
		DXGI_FORMAT fmt = DXGI_FORMAT_D24_UNORM_S8_UINT;
		UINT planes = 2;
		D3D12_RESOURCE_STATES state[2] = { D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_READ };
	};
	struct world_t
	{
		ID3D12Resource *rt = nullptr, *screen = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = {}, screen_rtv = {};
		DXGI_FORMAT rt_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
		UINT w = 0, h = 0;
		depth_t *depth = nullptr;
		ID3D12Resource *readback = nullptr;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
	};
	const float kFog[4] = { 0.35f, 0.30f, 0.20f, 0.50f };

	ID3D12Resource *make_rt(gpu &g, DXGI_FORMAT fmt, UINT w, UINT h)
	{
		return g.texture(fmt, w, h, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
	}
	// A depth as our d3d8.dll makes it (resource.cpp:162-176): typeless, made in DEPTH_READ, readable by shaders
	depth_t make_depth(gpu &g, bool d32, UINT w, UINT h)
	{
		depth_t d;
		d.fmt = d32 ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D24_UNORM_S8_UINT;
		d.planes = d32 ? 1 : 2;
		D3D12_CLEAR_VALUE clear = { d.fmt };
		clear.DepthStencil.Depth = 1.0f;
		d.res = g.texture(d32 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS, w, h, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
			D3D12_RESOURCE_STATE_DEPTH_READ, &clear);
		d.dsv = g.new_dsv();
		D3D12_DEPTH_STENCIL_VIEW_DESC vd = {};
		vd.Format = d.fmt;
		vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		g.device->CreateDepthStencilView(d.res, &vd, d.dsv);
		set_view(d.dsv, d.res, d.fmt);
		return d;
	}
	world_t make_world(gpu &g, depth_t *depth, UINT w, UINT h)
	{
		world_t x;
		x.w = w;
		x.h = h;
		x.depth = depth;
		x.rt = make_rt(g, x.rt_fmt, w, h);
		x.screen = make_rt(g, DXGI_FORMAT_R8G8B8A8_UNORM, w, h);
		x.rtv = g.new_rtv();
		x.screen_rtv = g.new_rtv();
		g.device->CreateRenderTargetView(x.rt, nullptr, x.rtv);
		g.device->CreateRenderTargetView(x.screen, nullptr, x.screen_rtv);
		set_view(x.rtv, x.rt, x.rt_fmt);
		set_view(x.screen_rtv, x.screen, DXGI_FORMAT_R8G8B8A8_UNORM);
		return x;
	}
	// The world's target made again in another format, its view kept (as HifiFrameSetWorldFormat: the GPU idle here)
	void remake_world(gpu &g, world_t &w, DXGI_FORMAT fmt)
	{
		g.idle();
		ID3D12Resource *const old = w.rt;
		w.rt_fmt = fmt;
		w.rt = make_rt(g, fmt, w.w, w.h);
		g.device->CreateRenderTargetView(w.rt, nullptr, w.rtv);
		set_view(w.rtv, w.rt, fmt);
		graveyard.push_back(old);
	}

	// ---- the adapter (one frame API, two core APIs)
#if WFX_CORE_API == 1
	// v1: as backend.cpp feeds today's core (ev_reset, the barrier filter, ev_clear_target, the marks' handler)
	std::map<ID3D12GraphicsCommandList *, worldfx::list_data> data;
	void a_reset(ID3D12GraphicsCommandList *l) { worldfx::on_reset(data[l]); }
	void a_barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES after)
	{
		if (sub == 0 || sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
			worldfx::on_barrier(l, data[l], 1, &r, &after);
	}
	void a_clear(ID3D12GraphicsCommandList *l, uint64_t rtv, const float c[4]) { worldfx::on_clear_target(data[l], rtv, c, 0, nullptr); }
	void a_mark(uint32_t kind, uint32_t, const hifi_frame_mark &m)
	{
		worldfx::list_data &d = data[m.list];
		if (m.dsv.ptr == 0 && kind != 13)
			return;
		if (kind == 13)
		{
			worldfx::on_world_end(m.list, d);
			return;
		}
		if (kind == 0 && m.ds != nullptr && m.rt != nullptr)
			worldfx::on_world_marker(m.rt, m.ds, m.rtv.ptr, m.dsv.ptr);
		if (kind == 1 && m.ds != nullptr)
		{
			const D3D12_RESOURCE_STATES p[2] = { m.ds_planes[0], m.ds_plane_count > 1 ? m.ds_planes[1] : m.ds_planes[0] };
			worldfx::on_zone_end(m.list, d, m.dsv.ptr, p);
		}
	}
	bool a_float_world() { return worldfx::float_world(); }
#else
	// v2 and on (the marks): as backend.cpp feeds the core now (world_of: the world from the mark, its views and formats
	// when it's the one bound; the fog as our d3d8.dll says it, the colour of the world's last whole clear). List Reset
	// and barriers are no-ops: the core follows no stream.
	float g_fog[4] = {};
	bool g_fog_known = false;
	void a_reset(ID3D12GraphicsCommandList *) {}
	void a_barrier(ID3D12GraphicsCommandList *, ID3D12Resource *, UINT, D3D12_RESOURCE_STATES) {}
	void a_clear(ID3D12GraphicsCommandList *, uint64_t, const float *c)
	{
		std::memcpy(g_fog, c, sizeof(g_fog));
		g_fog_known = true;
	}
	worldfx::world_mark world_of(const hifi_frame_mark &fm)
	{
		worldfx::world_mark w;
		w.list = fm.list;
		w.rt = fm.world_rt;
		w.ds = fm.world_ds;
		w.rt_state = fm.world_rt_state;
		w.ds_planes[0] = fm.world_ds_planes[0];
		w.ds_planes[1] = fm.world_ds_plane_count > 1 ? fm.world_ds_planes[1] : fm.world_ds_planes[0];
		if (fm.rt != nullptr && fm.rt == fm.world_rt && fm.ds != nullptr && fm.ds == fm.world_ds)
		{
			w.rtv = uint64_t(fm.rtv.ptr);
			w.dsv = uint64_t(fm.dsv.ptr);
			w.rt_format = fm.rt_format;
			w.ds_format = fm.ds_format;
			w.width = fm.width;
			w.height = fm.height;
		}
		w.fog_known = g_fog_known;
		for (int i = 0; i < 3; ++i)
			w.fog[i] = g_fog[i];
		return w;
	}
	void a_mark(uint32_t kind, uint32_t, const hifi_frame_mark &m)
	{
		if (m.dsv.ptr == 0 && kind != 13)
			return;
		if (kind == 13)
			worldfx::on_world_end(world_of(m));
		else if (kind == 0 && m.ds != nullptr && m.rt != nullptr)
			worldfx::on_world_marker(world_of(m));
		else if (kind == 1 && m.ds != nullptr)
			worldfx::on_zone_end(world_of(m));
	}
	bool a_float_world() { return worldfx::float_world(); }
#endif

	// A barrier the test records (and the adapter sees), one subresource
	void rec_barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
	{
		barrier(l, r, sub, a, b);
		a_barrier(l, r, sub, b);
	}

	uint32_t mark_seq = 0, zone_begins = 0;
	// The mark as d3d8.dll fills it (device.cpp:2190-2236): kind 0/1 with the world bound, 13 (the world's end) with the
	// screen bound and no depth
	hifi_frame_mark mark_of(const world_t &w, ID3D12GraphicsCommandList *l, bool world_bound)
	{
		const depth_t &d = *w.depth;
		hifi_frame_mark m = {};
		m.list = l;
		m.rt = world_bound ? w.rt : w.screen;
		m.ds = world_bound ? d.res : nullptr;
		m.rtv = world_bound ? w.rtv : w.screen_rtv;
		m.dsv = world_bound ? d.dsv : D3D12_CPU_DESCRIPTOR_HANDLE {};
		m.rt_format = world_bound ? w.rt_fmt : DXGI_FORMAT_R8G8B8A8_UNORM;
		m.ds_format = world_bound ? d.fmt : DXGI_FORMAT_UNKNOWN;
		m.rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		if (world_bound)
		{
			m.ds_planes[0] = d.state[0];
			m.ds_planes[1] = d.planes > 1 ? d.state[1] : d.state[0];
			m.ds_plane_count = d.planes;
		}
		m.width = w.w;
		m.height = w.h;
		m.world_rt = w.rt;
		m.world_ds = d.res;
		m.world_rt_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		m.world_ds_planes[0] = d.state[0];
		m.world_ds_planes[1] = d.planes > 1 ? d.state[1] : d.state[0];
		m.world_ds_plane_count = d.planes;
		return m;
	}
	void mark(uint32_t kind, const hifi_frame_mark &m)
	{
		if (kind == 0)
			zone_begins++;
		a_mark(kind, ++mark_seq, m);
	}

	// ---- presents (C18: the ones made while the game side says a character is in)
	bool told_in = false;
	uint64_t presents_in_game = 0, presents = 0;
	void zone(uint32_t z, bool in)
	{
		told_in = in;
		worldfx_zone(z, in ? 1u : 0u);
	}
	void present(gpu &g)
	{
		at("present");
		presents++;
		if (told_in)
			presents_in_game++;
		worldfx::on_present(g.queue);
	}

	// ---- one frame (the design's step list)
	struct frame_opts
	{
		bool readback = false, gated = false, skip_first_barrier = false, no_marks = false;
		int world_ends_on_list = 1, world_end_lists = 1;
	};
	void frame(gpu &g, world_t &w, const frame_opts &o = {})
	{
		at("frame");
		set_game_camera();
		// (the float world asked for: the target made again before the zone's begin)
		const DXGI_FORMAT want = a_float_world() ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
		if (want != w.rt_fmt && !o.gated)
			remake_world(g, w, want);
		std::vector<uint32_t> used;
		auto begin_list = [&]() {
			const uint32_t s = g.next++ % kRing;
			if (g.fence->GetCompletedValue() < g.done[s])
			{
				if (o.gated)
					check(false, "a gated frame found its list still on the GPU (the test's ring is too small)");
				at("frame: waiting for its list");
				g.wait(g.done[s]);
				at("frame");
			}
			g.alloc[s]->Reset();
			g.list[s]->Reset(g.alloc[s], nullptr);
			a_reset(g.list[s]);
			used.push_back(s);
			return g.list[s];
		};
		depth_t &d = *w.depth;
		ID3D12GraphicsCommandList *l = begin_list();
		// 2. the depth's planes to DEPTH_WRITE, one barrier each
		if (!o.skip_first_barrier)
			for (UINT k = 0; k < d.planes; ++k)
				if (d.state[k] != D3D12_RESOURCE_STATE_DEPTH_WRITE)
				{
					rec_barrier(l, d.res, k, d.state[k], D3D12_RESOURCE_STATE_DEPTH_WRITE);
					d.state[k] = D3D12_RESOURCE_STATE_DEPTH_WRITE;
				}
		// 3. the world bound and cleared to the fog; the depth to 1, a wall over the left quarter at 0
		l->OMSetRenderTargets(1, &w.rtv, FALSE, &d.dsv);
		l->ClearRenderTargetView(w.rtv, kFog, 0, nullptr);
		a_clear(l, w.rtv.ptr, kFog);
		if (d.state[0] == D3D12_RESOURCE_STATE_DEPTH_WRITE)
		{
			l->ClearDepthStencilView(d.dsv, d.planes > 1 ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			const D3D12_RECT wall = { 0, 0, LONG(w.w / 4), LONG(w.h) };
			l->ClearDepthStencilView(d.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 1, &wall);
		}
		// 4, 5. the zone's begin and end
		if (!o.no_marks)
		{
			mark(0, mark_of(w, l, true));
			l->OMSetRenderTargets(1, &w.rtv, FALSE, &d.dsv);
			mark(1, mark_of(w, l, true));
		}
		// 6. the depth's plane 0 read (the stencil's stays DEPTH_WRITE)
		if (d.state[0] == D3D12_RESOURCE_STATE_DEPTH_WRITE)
		{
			rec_barrier(l, d.res, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
			d.state[0] = D3D12_RESOURCE_STATE_DEPTH_READ;
		}
		// 7. the world's end, with the screen bound
		if (!o.no_marks)
		{
			l->OMSetRenderTargets(1, &w.screen_rtv, FALSE, nullptr);
			for (int i = 0; i < o.world_ends_on_list; ++i)
				mark(13, mark_of(w, l, false));
			for (int n = 1; n < o.world_end_lists; ++n)
			{
				l->Close();
				l = begin_list();
				l->OMSetRenderTargets(1, &w.screen_rtv, FALSE, nullptr);
				mark(13, mark_of(w, l, false));
			}
		}
		// 8. the world read back
		if (o.readback)
		{
			const D3D12_RESOURCE_DESC desc = w.rt->GetDesc();
			UINT64 total = 0;
			g.device->GetCopyableFootprints(&desc, 0, 1, 0, &w.fp, nullptr, nullptr, &total);
			release(w.readback);
			w.readback = g.buffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
			rec_barrier(l, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION dst = { w.readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
			dst.PlacedFootprint = w.fp;
			D3D12_TEXTURE_COPY_LOCATION src = { w.rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			rec_barrier(l, w.rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}
		// 9. the stencil's plane read too (v1 ignores it, as the backend does)
		if (d.planes > 1 && d.state[1] == D3D12_RESOURCE_STATE_DEPTH_WRITE)
		{
			rec_barrier(l, d.res, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
			d.state[1] = D3D12_RESOURCE_STATE_DEPTH_READ;
		}
		l->Close();
		// 10. presented (Ashita's present comes before d3d8.dll submits), then submitted
		present(g);
		std::vector<ID3D12CommandList *> lists;
		for (uint32_t s : used)
			lists.push_back(g.list[s]);
		g.queue->ExecuteCommandLists(UINT(lists.size()), lists.data());
		g.queue->Signal(g.fence, ++g.value);
		for (uint32_t s : used)
			g.done[s] = g.value;
	}

	// The world's last read back (RGBA8: 4 bytes a pixel), the GPU idle first
	std::vector<uint8_t> read_world(gpu &g, world_t &w)
	{
		g.idle();
		std::vector<uint8_t> out;
		if (w.readback == nullptr)
			return out;
		out.resize(size_t(w.w) * w.h * 4);
		void *p = nullptr;
		w.readback->Map(0, nullptr, &p);
		for (UINT y = 0; y < w.h; ++y)
			std::memcpy(&out[size_t(y) * w.w * 4], static_cast<uint8_t *>(p) + size_t(y) * w.fp.Footprint.RowPitch, size_t(w.w) * 4);
		w.readback->Unmap(0, nullptr);
		release(w.readback);
		return out;
	}

	struct picture_diff
	{
		uint32_t changed = 0, centre = 0, wall = 0, alpha = 0, total = 0;
	};
	picture_diff compare(const std::vector<uint8_t> &a, const std::vector<uint8_t> &ref, UINT w, UINT h)
	{
		picture_diff d;
		if (a.size() != ref.size() || a.size() != size_t(w) * h * 4)
		{
			d.changed = d.wall = d.alpha = ~0u;
			return d;
		}
		d.total = w * h;
		for (UINT y = 0; y < h; ++y)
			for (UINT x = 0; x < w; ++x)
			{
				const uint8_t *p = &a[(size_t(y) * w + x) * 4], *q = &ref[(size_t(y) * w + x) * 4];
				if (p[3] != q[3])
					d.alpha++;
				if (p[0] == q[0] && p[1] == q[1] && p[2] == q[2])
					continue;
				d.changed++;
				if (x < w / 4)
					d.wall++;
				if (std::abs(int(x) - int(w / 2)) <= 4 && std::abs(int(y) - int(h / 2)) <= 4)
					d.centre++;
			}
		return d;
	}

	// ---- the feed thread (the threads part): every entry the addon, the game side and the menu call off the game's thread
	std::atomic<bool> a_done{ false };
	std::atomic<uint64_t> b_calls{ 0 };
	std::atomic<int> probe_gen{ 0 }, b_parked_gen{ 0 }, b_handled_gen{ 0 };
	constexpr int kProbes = 5;
	bool probe_overlay_empty[kProbes + 1] = {};
	size_t probe_overlay_lines[kProbes + 1] = {};
	// What the feed thread saw (checked by the game thread after the join)
	uint32_t b_blades_expected = 0, b_cells_expected = 0;
	std::atomic<uint32_t> b_bad_blades{ 0 }, b_blades_seen{ 0 }, b_bad_pushing{ 0 }, b_pushing_seen{ 0 }, b_bad_glows{ 0 }, b_bad_height{ 0 };
	std::string b_bad_example;
	vec3 grass_at = {};

	uint32_t bits(float f)
	{
		uint32_t u;
		std::memcpy(&u, &f, 4);
		return u;
	}

	void feed_once(uint64_t i, std::vector<std::string> &lines)
	{
		b_at = "actors";
		const uint32_t n_actors = uint32_t(i % 3) + 1;
		float xyzr[12];
		uint32_t flags[3];
		for (uint32_t k = 0; k < n_actors; ++k)
		{
			xyzr[k * 4 + 0] = grass_at.x + float(k);
			xyzr[k * 4 + 1] = grass_at.y;
			xyzr[k * 4 + 2] = grass_at.z;
			xyzr[k * 4 + 3] = 0.6f;
			flags[k] = uint32_t((i + k) & 1);
		}
		worldfx_grass_actors2(xyzr, flags, n_actors);
		const float swath[7] = { grass_at.x, grass_at.y, grass_at.z, 1.5f, grass_at.x + 3.0f, grass_at.y, grass_at.z };
		worldfx_grass_cut(swath, 1);
		b_at = "lamps";
		{
			uint32_t words[20];
			for (int k = 0; k < 2; ++k)
			{
				uint32_t *w = words + k * 10;
				w[0] = 0x31706D6Cu + uint32_t(k); // ("lmp1", "lmp2")
				w[1] = 1;
				const float f[8] = { 1.0f, 0.8f, 0.5f, grass_at.x + 2.0f * k, grass_at.y - 2.0f, grass_at.z, 10.0f, 5.0f };
				for (int j = 0; j < 8; ++j)
					w[2 + j] = bits(f[j]);
			}
			worldfx_lamps(words, 2);
		}
		b_at = "glows";
		{
			const uint32_t count = (i & 1) ? 5u : 3u;
			uint32_t words[50];
			for (uint32_t k = 0; k < count; ++k)
			{
				uint32_t *w = words + k * 10;
				w[0] = 100 + k;
				w[1] = (k & 1) ? 3u : 2u;
				const float f[8] = { 0.4f, 0.7f, 1.0f, grass_at.x, grass_at.y - 1.0f, grass_at.z + float(k), 6.0f, 4.0f };
				for (int j = 0; j < 8; ++j)
					w[2 + j] = bits(f[j]);
			}
			worldfx_glows(words, count);
			uint32_t out[5] = {};
			worldfx_glow_stats(out);
			if (out[0] != 3 && out[0] != 5)
				b_bad_glows++;
		}
		b_at = "weather";
		{
			uint32_t words[grass::kWeatherWords] = {};
			words[1] = grass::weather_name("fine");
			words[3] = 0x3F800000u; // (change 1.0)
			words[7] = 4;           // (the weather object read)
			worldfx_weather(words, grass::kWeatherWords);
			worldfx_moon(700000000u + uint32_t(i));
		}
		b_at = "settings";
		{
			const float h = (i & 1) ? 0.6f : 0.5f;
			worldfx_set_setting("GrassHeight", h);
			float v[3] = {};
			worldfx::get_setting("GrassHeight", v);
			if (std::fabs(v[0] - 0.5f) > 1e-6f && std::fabs(v[0] - 0.6f) > 1e-6f)
				b_bad_height++;
		}
		b_at = "stats";
		{
			uint32_t out[5];
			(void)worldfx_weather_drawn();
			(void)worldfx_casts_characters();
			(void)worldfx_drawing();
			worldfx_last_draw(out);
			uint32_t world[4];
			worldfx_world(world);
		}
		b_at = "frame_counts";
		(void)worldfx::frame_counts();
		b_at = "overlay_lines";
		(void)worldfx::overlay_lines();
		b_at = "gpu_times";
		(void)worldfx::gpu_times();
		b_at = "draw_status";
		lines.clear();
		sink = &lines;
		worldfx::draw_status(capture_ui());
		b_at = "draw_panel";
		if (i % 4 == 0)
			worldfx::draw_panel(capture_ui());
		sink = nullptr;
		{
			char want[96];
			std::snprintf(want, sizeof(want), "Blades: zone 100: %u blades in %u cells", b_blades_expected, b_cells_expected);
			const std::string blades = line_of(lines, "Blades: ");
			if (!blades.empty())
			{
				b_blades_seen++;
				if (blades != want)
				{
					b_bad_blades++;
					b_bad_example = blades;
				}
			}
			const std::string pushing = line_of(lines, "Pushing the grass: ");
			unsigned k = 0, f = 0;
			if (pushing.rfind("Pushing the grass: nobody", 0) != 0 && !pushing.empty())
			{
				b_pushing_seen++;
				if (sscanf_s(pushing.c_str(), "Pushing the grass: %u characters and monsters near, %u of them fighting", &k, &f) != 2 || k < 1 || k > 3 || f > k)
				{
					b_bad_pushing++;
					b_bad_example = pushing;
				}
			}
		}
		b_at = "forced_weather_text";
		(void)worldfx::forced_weather_text();
		b_at = "blade_motion";
		{
			ID3D12Resource *t = nullptr;
			uint64_t frames = 0;
			(void)worldfx::blade_motion(t, frames);
		}
		b_calls += 22;
	}

	void feed_loop()
	{
		std::vector<std::string> lines;
		uint64_t i = 0;
		while (!a_done)
		{
			b_beat++;
			const int gen = probe_gen;
			if (gen > b_handled_gen)
			{
				// (the probe: parked between calls, so the game thread's probe finds this thread waiting, not in a call)
				b_at = "parked for the probe";
				b_parked_gen = gen;
				while (WaitForSingleObject(ev_inside, 50) == WAIT_TIMEOUT && !a_done)
					b_beat++;
				b_at = "probe: overlay_lines";
				const std::vector<std::string> o = worldfx::overlay_lines();
				probe_overlay_empty[gen] = o.empty();
				probe_overlay_lines[gen] = o.size();
				b_at = "probe: worldfx_glows";
				const uint32_t words[10] = { 1, 2, bits(1.0f), bits(1.0f), bits(1.0f), bits(grass_at.x), bits(grass_at.y), bits(grass_at.z), bits(5.0f), bits(5.0f) };
				worldfx_glows(words, 1);
				b_at = "probe: frame_counts";
				(void)worldfx::frame_counts();
				b_at = "probe: draw_status";
				sink = &lines;
				worldfx::draw_status(capture_ui());
				sink = nullptr;
				SetEvent(ev_feed_done);
				b_at = "after the probe";
				b_handled_gen = gen;
				continue;
			}
			feed_once(i++, lines);
		}
		b_at = "done";
	}

	// ---- the loader thread (the threads part): a copy queue of its own uploading 64 KB again and again (the characters' cache worker's)
	std::atomic<uint64_t> c_uploads{ 0 };
	std::atomic<bool> c_failed{ false };
	void loader_loop(ID3D12Device *device)
	{
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
		ID3D12CommandQueue *queue = nullptr;
		ID3D12CommandAllocator *alloc = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12Fence *fence = nullptr;
		ID3D12Resource *up = nullptr, *dst = nullptr;
		HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&alloc));
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, alloc, nullptr, IID_PPV_ARGS(&list));
		device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
		if (list != nullptr)
			list->Close();
		auto buffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
			D3D12_HEAP_PROPERTIES heap = { type };
			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = 65536;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ID3D12Resource *res = nullptr;
			device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res));
			return res;
		};
		up = buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
		dst = buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
		if (queue == nullptr || alloc == nullptr || list == nullptr || fence == nullptr || up == nullptr || dst == nullptr)
			c_failed = true;
		uint64_t v = 0;
		while (!a_done && !c_failed)
		{
			void *p = nullptr;
			if (SUCCEEDED(up->Map(0, nullptr, &p)))
			{
				std::memset(p, int(v & 0xFF), 65536);
				up->Unmap(0, nullptr);
			}
			alloc->Reset();
			list->Reset(alloc, nullptr);
			list->CopyBufferRegion(dst, 0, up, 0, 65536);
			list->Close();
			ID3D12CommandList *lists[] = { list };
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(fence, ++v);
			fence->SetEventOnCompletion(v, event);
			if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0 && fence->GetCompletedValue() < v)
			{
				c_failed = true; // (leaked: never freed under the copy)
				break;
			}
			c_uploads++;
		}
		if (!c_failed)
		{
			release(up);
			release(dst);
			release(list);
			release(alloc);
			release(fence);
			release(queue);
		}
		CloseHandle(event);
	}
}

int main(int argc, char **argv)
{
	const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
	std::setvbuf(stdout, nullptr, _IONBF, 0); // (each line out as it's printed: a crash shows where it was)
	const auto t_start = std::chrono::steady_clock::now();
	std::thread watch(watchdog);

	// 1. The device: WARP, the debug layer and GPU-based validation (not with --quick)
	at("device");
	HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
	HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
	const auto get_debug = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d12, "D3D12GetDebugInterface"));
	const auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
	using create_factory_fn = HRESULT(WINAPI *)(REFIID, void **);
	const auto create_factory = reinterpret_cast<create_factory_fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
	ID3D12Debug *debug = nullptr;
	if (get_debug != nullptr && SUCCEEDED(get_debug(IID_PPV_ARGS(&debug))))
	{
		debug->EnableDebugLayer();
		ID3D12Debug1 *debug1 = nullptr;
		if (!quick && SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))))
		{
			debug1->SetEnableGPUBasedValidation(TRUE);
			release(debug1);
		}
	}
	check(debug != nullptr, quick ? "debug layer on (--quick: no GPU-based validation)" : "debug layer and GPU-based validation on");
	gpu g;
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	if (create_factory == nullptr || FAILED(create_factory(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
		FAILED(create_device(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
	{
		std::printf("FAIL  no WARP device\n");
		return 1;
	}
	g.device->QueryInterface(IID_PPV_ARGS(&g.info));
	g.device->QueryInterface(IID_PPV_ARGS(&g.dbg));
	if (g.info != nullptr)
		g.info->SetMessageCountLimit(UINT64(-1));
	check(g.info != nullptr && g.dbg != nullptr, "the info queue and the debug device");
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
	for (uint32_t i = 0; i < kRing; ++i)
	{
		g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc[i]));
		g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[i], nullptr, IID_PPV_ARGS(&g.list[i]));
		g.list[i]->Close();
	}
	g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
	g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	{
		D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 16 };
		g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtv_heap));
		hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 16 };
		g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.dsv_heap));
		g.rtv_step = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		g.dsv_step = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	}

	// The runtime folder: build\test\worldfx_core\ with zone 100's blades, zone 104's water and rain map (no sunlit list, no
	// clear sky, no worldfx.hlsl but in C14); the shader cache kept there between runs
	wchar_t full[MAX_PATH];
	GetFullPathNameW(L"build\\test\\worldfx_core\\", MAX_PATH, full, nullptr);
	const std::wstring folder = full;
	CreateDirectoryW(L"build\\test", nullptr);
	CreateDirectoryW(folder.c_str(), nullptr);
	bool copied = true;
	for (const wchar_t *part : { L"grass\\100.bin", L"water\\104.bin", L"rain\\104.bin" })
	{
		const std::wstring p = part, kind = p.substr(0, p.find(L'\\'));
		CreateDirectoryW((folder + L"worldfx-" + kind).c_str(), nullptr);
		copied = CopyFileW((L"build\\testdata\\" + p).c_str(), (folder + L"worldfx-" + p).c_str(), FALSE) && copied;
	}
	DeleteFileW((folder + L"worldfx.hlsl").c_str());
	check(copied, "the zone files copied from build\\testdata into build\\test\\worldfx_core");
#if WFX_CORE_API >= 4
	// (C1 after the rewrite: attach doesn't wait for the shaders, the cache emptied first)
	{
		WIN32_FIND_DATAW fd;
		const std::wstring cache = folder + L"worldfx-shader-cache\\";
		HANDLE h = FindFirstFileW((cache + L"*").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
				DeleteFileW((cache + fd.cFileName).c_str());
			while (FindNextFileW(h, &fd));
			FindClose(h);
		}
	}
#endif

	// 2. The stand-in FFXiMain.dll (before the first world end: the core scans for the camera once)
	HMODULE fake = LoadLibraryW(L"build\\test\\fake\\FFXiMain.dll");
	fake_set_camera = fake != nullptr ? reinterpret_cast<set_camera_fn>(GetProcAddress(fake, "set_camera")) : nullptr;
	check(fake_set_camera != nullptr, "the stand-in FFXiMain.dll loaded (build\\test\\fake)");
	if (fake_set_camera == nullptr)
		return 1;
	{
		using set_light_fn = void (*)(const float *);
		const auto set_light = reinterpret_cast<set_light_fn>(GetProcAddress(fake, "set_light"));
		const float down[3] = { 0.3f, 0.9f, 0.3f };
		if (set_light != nullptr)
			set_light(down);
	}

	// 3. The blades: the grass's view looks at the busiest cell's first blade (grass_test.cpp), the game's away from all
	grass::blade_file file, water_file;
	std::string error;
	check(grass::read_blade_file(L"build\\testdata\\grass\\100.bin", file, error) && file.blade_count() != 0, "zone 100's blade file read");
	check(grass::read_blade_file(L"build\\testdata\\water\\104.bin", water_file, error, "WTR2", 8), "zone 104's water file read");
	if (file.blade_count() == 0)
		return 1;
	{
		const grass::blade_file::cell *busiest = &file.cells[0];
		for (const grass::blade_file::cell &c : file.cells)
			if (c.count > busiest->count)
				busiest = &c;
		const float *b = &file.blades[size_t(busiest->first) * 4];
		grass_at = { b[0], b[1], b[2] };
		look_at({ grass_at.x + 4.0f, grass_at.y - 2.0f, grass_at.z + 3.0f }, grass_at, { 0.0f, -1.0f, 0.0f }, grass_view);
		projection(640.0f / 360.0f, grass_proj);
		// (far off over the zone's edge, looking level: no blade within any grass distance. Right over them, the renderer
		// still draws the cells under the camera, off the screen)
		const vec3 high = { grass_at.x + 100000.0f, grass_at.y - 100.0f, grass_at.z + 100000.0f };
		look_at(high, { high.x + 10.0f, high.y, high.z }, { 0.0f, -1.0f, 0.0f }, away_view);
	}
	b_blades_expected = file.blade_count();
	b_cells_expected = uint32_t(file.cells.size());

	// 4. The port, lean settings (only the blades), the host's sources
	{
		const char *lean[][2] = { { "Grass", "1" }, { "AOStrength", "0" }, { "SunShadows", "0" }, { "MoonShadows", "0" }, { "LampGlow", "0" },
			{ "FixtureGlow", "0" }, { "EffectGlow", "0" }, { "WetGround", "0" }, { "SnowCover", "0" }, { "Rain", "0" }, { "Snow", "0" }, { "Sky", "0" },
			{ "WeatherOwn", "0" }, { "VolLight", "0" }, { "VolFog", "0" }, { "Fireflies", "0" }, { "Water", "0" }, { "WaterSea", "0" },
			{ "Tonemap", "0" }, { "Bloom", "0" }, { "ColourGrade", "0" }, { "FloatWorld", "0" }, { "GpuCrashReports", "0" },
			{ "D3D12DebugLayer", "0" }, { "TestVanadielHour", "12" }, { "TestMoon", "0.5" }, { "TestShadeMaxMs", "100000" } };
		for (const auto &kv : lean)
			settings[kv[0]] = kv[1];
	}
	worldfx::port p;
	p.log = p_log;
	p.get_bool = p_get_bool;
	p.get_float = p_get_float;
	p.get_int = p_get_int;
	p.set_bool = p_set_bool;
	p.set_float = p_set_float;
	p.set_int = p_set_int;
#if WFX_CORE_API == 1
	p.resource_of_view = p_resource_of_view;
	p.format_of_view = p_format_of_view;
#endif
	p.folder = folder;
	worldfx::set_port(p);
	worldfx::load_settings();
	worldfx::set_camera_source(camera_source, nullptr);
	worldfx::set_zone_casters(bounds_probe, zone_draw_none, nullptr);
	worldfx::set_floor_source(floor_none, nullptr);
	ev_inside = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	ev_feed_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	// The world, its depth (D24S8; a D32 one for C10, a fresh one for C9) and the screen, 640 x 360
	depth_t depth = make_depth(g, false, 640, 360), depth32 = make_depth(g, true, 640, 360), depth_c9 = make_depth(g, false, 640, 360);
	world_t w = make_world(g, &depth, 640, 360);
	const UINT W = w.w, H = w.h;

	// C1. Attached: nothing drawn, no world
	at("attach");
	{
		const auto t0 = std::chrono::steady_clock::now();
		worldfx::attach(g.device);
		const double s = seconds_since(t0);
		std::printf("  attach took %.2f s\n", s);
#if WFX_CORE_API >= 4
		check(s < 0.5, "C1 attach returns within 0.5 s, the shader cache empty (the shaders compiled on their own thread)");
#endif
		uint32_t out[4] = { 9, 9, 9, 9 };
		worldfx_world(out);
		check(worldfx::attached() == g.device && worldfx_drawing() == 0 && out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0,
			"C1 attached; nothing drawn and no world before any mark");
	}
#if WFX_CORE_API >= 4
	// (the built-in shaders compiled on their own thread: presents till one takes them in, at most 3 minutes on WARP)
	{
		const auto t0 = std::chrono::steady_clock::now();
		bool in = false;
		while (!in && seconds_since(t0) < 180.0)
		{
			present(g);
			in = line_of(status(), "Shaders: ").find("compiled in the background") != std::string::npos;
			if (!in)
				Sleep(20);
		}
		std::printf("  the built-in shaders in after %.1f s\n", seconds_since(t0));
		check(in, "C1 the built-in shaders compiled in the background, taken in at a present");
	}
#endif

	// The reference: a frame with no marks before the game side has said anything (nothing of World FX's): the cleared bytes
	frame(g, w, { true, false, false, true });
	const std::vector<uint8_t> reference = read_world(g, w);
	{
		const uint8_t *c = &reference[(size_t(H / 2) * W + W / 2) * 4];
		std::printf("  the cleared world: %u %u %u %u\n", c[0], c[1], c[2], c[3]);
		check(reference.size() == size_t(W) * H * 4 && g.take_errors() == 0, "the reference picture (the world cleared, no marks), no debug-layer errors");
	}
	auto picture = [&](const char *what) {
		const std::vector<uint8_t> px = read_world(g, w);
		const picture_diff d = compare(px, reference, W, H);
		std::printf("  %s: %u of %u pixels changed (%u at the centre, %u behind the wall), %u alphas changed\n", what, d.changed, d.total, d.centre, d.wall, d.alpha);
		return d;
	};
	auto same_as_reference = [&](const picture_diff &d) { return d.changed == 0 && d.alpha == 0; };

	// C2. The zone told, one present with no marks
	{
		zone(100, true);
		const counts before = counts_now();
		present(g);
		const counts d = delta(before, counts_now());
		std::printf("  %s\n", describe(d).c_str());
#if WFX_CORE_API == 1
		check(one_skip(d, "no world target yet (no zone marker from HiFi XI's d3d8.dll)"), "C2 a present with no marks: +1 \"no world target yet\"");
#else
		check(d.drawn == 0 && d.skipped_total() == 1, "C2 a present with no marks: one frame skipped");
#endif
		char want[96];
		std::snprintf(want, sizeof(want), "Blades: zone 100: %u blades in %zu cells", file.blade_count(), file.cells.size());
		const std::vector<std::string> s = status();
		check(line_of(s, "Blades: ") == want, std::string("C2 the status: \"") + want + "\"");
	}

	// C9. The first frame with the world, its depth still DEPTH_READ from its making, no barrier on it yet (its own depth)
	{
		w.depth = &depth_c9;
		frame(g, w, { false, false, true });
		g.idle();
		const int e = g.take_errors(false);
#if WFX_CORE_API == 1
		std::printf("  C9 (today, known): %d debug-layer errors with the depth left in DEPTH_READ (the core takes it for DEPTH_WRITE)\n", e);
		check(true, "C9 (today) the depth's state assumed DEPTH_WRITE before any barrier: errors reported, not failed");
#else
		check(e == 0, "C9 the depth left in DEPTH_READ from its making: no debug-layer errors");
#endif
		w.depth = &depth;
	}

	// C3. A full frame
	{
		const counts before = counts_now();
		frame(g, w, { true });
		const picture_diff px = picture("C3");
		const counts d = delta(before, counts_now());
		uint32_t last[5] = {}, world[4] = {};
		worldfx_last_draw(last);
		worldfx_world(world);
		std::printf("  %s; last drawing %u blades in %u draws; world %u x %u marked %u, %u markers\n", describe(d).c_str(), last[0], last[1], world[0], world[1], world[2], world[3]);
		check(last[0] > 0 && last[1] > 0, "C3 worldfx_last_draw: blades in draws");
		check(px.changed * 200 > px.total, "C3 the blades changed more than 0.5% of the world");
		check(px.centre > 0, "C3 a blade at the centre (where the grass's view looks)");
		check(px.wall == 0, "C3 nothing behind the wall (the world's depth tested)");
		check(px.alpha == 0, "C3 the world's alpha untouched");
		check(worldfx_drawing() == 1, "C3 worldfx_drawing 1");
		check(world[0] == W && world[1] == H && world[2] == 1 && world[3] == zone_begins, "C3 worldfx_world: 640 x 360, marked, one marker for each zone begin");
		check(d.drawn == 1 && d.skipped_total() == 0, "C3 +1 frame drawn");
		const std::vector<std::string> s = status();
		char want[200];
		std::snprintf(want, sizeof(want), "Last frame: %u blades in %u draws, at the zone's end (before characters and effects, in the world's depth).", last[0], last[1]);
		check(line_of(s, "Last frame: ") == want, std::string("C3 the status: \"") + want + "\"");
		check(line_of(s, "Sky along the horizon") == "Sky along the horizon (the zone's fog): 0.35 0.30 0.20", "C3 the status: the fog from the world's clear");
		check(line_of(s, "Camera: ") == "Camera: found", "C3 the status: \"Camera: found\" (the stand-in FFXiMain.dll)");
		check(line_of(s, "The world: ") == "The world: 640x360, found by the game's zone marker", "C3 the status: the world found by the zone marker");
		check(g.take_errors() == 0, "C3 no debug-layer errors");
	}

	// C4. The host's camera source off: the game's own camera (away from every blade)
	{
		source_on = false;
		const counts before = counts_now();
		frame(g, w, { true });
		const picture_diff px = picture("C4");
		const counts d = delta(before, counts_now());
		const std::vector<std::string> s = status();
		std::printf("  %s; %s\n", describe(d).c_str(), line_of(s, "Last frame: ").c_str());
		// (a world's end that drew nothing still counts as drawn: the skip reason is set, passes counted)
		check(d.drawn == 1 && d.skipped_total() == 0 && line_of(s, "Last frame: ") == "Last frame: not drawn: no blades near the camera.",
			"C4 the game's camera: counted as drawn, the status \"not drawn: no blades near the camera\"");
		check(same_as_reference(px), "C4 the world as cleared (the game's camera sees no blades)");
		source_on = true;
		check(g.take_errors() == 0, "C4 no debug-layer errors");
	}

	// C5. Not in the field; then a camera that isn't set (a zero projection, the source off)
	{
		fake_in_field = false;
		counts before = counts_now();
		frame(g, w);
		counts d = delta(before, counts_now());
		check(one_skip(d, "not in the field"), "C5 the game's camera not in the field: +1 \"not in the field\" (" + describe(d) + ")");
		fake_in_field = true;
		fake_proj_zero = true;
		source_on = false;
		before = counts_now();
		frame(g, w);
		d = delta(before, counts_now());
		check(one_skip(d, "the camera isn't set"), "C5 a zero projection: +1 \"the camera isn't set\" (" + describe(d) + ")");
		fake_proj_zero = false;
		source_on = true;
		g.idle();
		check(g.take_errors() == 0, "C5 no debug-layer errors");
	}

	// C6. Out of the game
	{
		zone(100, false);
		counts before = counts_now();
		frame(g, w, { true });
		picture_diff px = picture("C6, the first frame after");
		counts d = delta(before, counts_now());
#if WFX_CORE_API == 1
		// (today "in the game" is the last present's: the frame right after the game side says no is still drawn)
		check(d.drawn == 0 && d.skipped_total() == 0 && px.changed != 0, "C6 (today) out of the game: the next frame still drawn, nothing counted");
#else
		check(d.drawn == 0 && d.skipped_total() == 0, "C6 out of the game: nothing counted");
#endif
		before = counts_now();
		frame(g, w, { true });
		px = picture("C6, the second");
		d = delta(before, counts_now());
		check(d.drawn == 0 && d.skipped_total() == 0 && same_as_reference(px), "C6 out of the game: nothing counted, nothing drawn");
		zone(100, true);
		before = counts_now();
		frame(g, w, { true });
		px = picture("C6, back in");
		d = delta(before, counts_now());
		check(one_skip(d, "not in the game (login, character select, zoning; or the worldfx Ashita addon isn't loaded)") && same_as_reference(px),
			"C6 back in: that frame +1 \"not in the game (...)\", nothing drawn (" + describe(d) + ")");
		before = counts_now();
		frame(g, w);
		d = delta(before, counts_now());
		check(d.drawn == 1 && d.skipped_total() == 0, "C6 the frame after: drawn");
		g.idle();
		check(g.take_errors() == 0, "C6 no debug-layer errors");
	}

	// C7. The zone guard
	{
		zone(104, true);
		counts before = counts_now();
		frame(g, w, { true });
		picture_diff px = picture("C7, the new zone's first frame");
		counts d = delta(before, counts_now());
		check(one_skip(d, "a new zone, its files not read yet (the last zone's never drawn in it)") && same_as_reference(px),
			"C7 a new zone told: +1 \"a new zone, its files not read yet\", nothing drawn (" + describe(d) + ")");
		const std::vector<std::string> s = status();
		char water[96];
		std::snprintf(water, sizeof(water), "Water: zone 104: %u triangles in %zu cells", water_file.blade_count() / 3, water_file.cells.size());
		check(line_of(s, "Blades: ") == "Blades: zone 104: no file for this zone", "C7 zone 104's files read at the present: \"Blades: zone 104: no file for this zone\"");
		check(line_of(s, "Water: ") == water, std::string("C7 \"") + water + "\"");
		const std::string rain = line_of(s, "Rain map: ");
		check(rain.rfind("Rain map: zone 104: ", 0) == 0 && rain.find(" squares of ") != std::string::npos && rain.find(" with a surface") != std::string::npos,
			"C7 \"" + rain + "\"");
		before = counts_now();
		frame(g, w, { true });
		px = picture("C7, the next");
		d = delta(before, counts_now());
		check(one_skip(d, "no blades for this zone") && same_as_reference(px), "C7 then: +1 \"no blades for this zone\" (" + describe(d) + ")");
		zone(100, true);
		before = counts_now();
		frame(g, w);
		d = delta(before, counts_now());
		check(one_skip(d, "a new zone, its files not read yet (the last zone's never drawn in it)"), "C7 back to 100: one frame guarded (" + describe(d) + ")");
		before = counts_now();
		frame(g, w);
		d = delta(before, counts_now());
		check(d.drawn == 1 && d.skipped_total() == 0, "C7 then drawn again");
		g.idle();
		check(g.take_errors() == 0, "C7 no debug-layer errors");
	}

	// C8. Grass off between frames
	{
		const float off[3] = { 0.0f, 0.0f, 0.0f }, on[3] = { 1.0f, 0.0f, 0.0f };
		check(worldfx::set_setting("Grass", off), "C8 set_setting(\"Grass\", 0)");
		float v[3] = { 9, 9, 9 };
		std::string saved;
		check(worldfx::get_setting("Grass", v) && v[0] == 0.0f && p_value("Grass", saved) && saved == "0", "C8 get_setting 0 at once; the port holds Grass=0");
		counts before = counts_now();
		frame(g, w, { true });
		const picture_diff px = picture("C8");
		counts d = delta(before, counts_now());
		check(one_skip(d, "off") && same_as_reference(px), "C8 the next frame: +1 \"off\", nothing drawn (" + describe(d) + ")");
		worldfx::set_setting("Grass", on);
		before = counts_now();
		frame(g, w);
		d = delta(before, counts_now());
		check(d.drawn == 1 && d.skipped_total() == 0, "C8 on again: drawn");
		g.idle();
		check(g.take_errors() == 0, "C8 no debug-layer errors");
	}

	// C10. A D32 world depth: not the blades' depth at the zone's end; all of it at the world's end
	{
		w.depth = &depth32;
		const counts before = counts_now();
		frame(g, w, { true });
		const picture_diff px = picture("C10");
		const counts d = delta(before, counts_now());
		uint32_t last[5] = {};
		worldfx_last_draw(last);
		const std::vector<std::string> s = status();
		char want[160];
		std::snprintf(want, sizeof(want), "Last frame: %u blades in %u draws.", last[0], last[1]);
		check(d.drawn == 1 && last[0] > 0 && px.changed != 0 && px.wall == 0 && px.alpha == 0, "C10 a D32 depth: drawn at the world's end (" + describe(d) + ")");
		check(line_of(s, "Last frame: ") == want, std::string("C10 the status: \"") + want + "\" (not at the zone's end)");
		w.depth = &depth;
		g.idle();
		check(g.take_errors() == 0, "C10 no debug-layer errors");
	}

	// C11. Three world ends on one list; then on three lists in one frame
	{
		size_t from = log_count();
		counts before = counts_now();
		frame_opts o;
		o.world_ends_on_list = 3;
		frame(g, w, o);
		counts d = delta(before, counts_now());
#if WFX_CORE_API == 1
		check(d.drawn == 1 && d.skipped_total() == 0 && !logged_since(from, "the world ended"),
			"C11 (today) three world ends on one list: the second and third ignored (no warning), drawn once");
#else
		check(d.drawn == 1 && d.skipped_total() == 0 && !logged_since(from, "the world ended"),
			"C11 three world ends on one list: drawn at the first alone (once after each zone's begin), no warning");
#endif
		from = log_count();
		before = counts_now();
		o.world_ends_on_list = 1;
		o.world_end_lists = 3;
		frame(g, w, o);
		d = delta(before, counts_now());
#if WFX_CORE_API == 1
		check(logged_since(from, "FFXI World FX: the world ended 3 times between two presents (drawn at the first 2)"),
			"C11 three lists, a world end each: \"the world ended 3 times between two presents (drawn at the first 2)\"");
#else
		// (one zone's begin before them: drawn at the first world's end alone, wherever it is; kMaxPasses never reached)
		check(!logged_since(from, "the world ended"), "C11 three lists, a world end each, one zone's begin: drawn at the first alone, no warning");
#endif
		check(d.drawn == 1 && d.skipped_total() == 0, "C11 three lists: +1 frame drawn (" + describe(d) + ")");
		g.idle();
		check(g.take_errors() == 0, "C11 no debug-layer errors");
	}

#if WFX_CORE_API == 1
	// C12 (today). The world's view held by another resource now (the view map says the screen): nothing drawn. Today the
	// world's end says "the world's views hold something else now (skipped)" but doesn't count as tried, so the frame is
	// counted as "the world's end wasn't marked this frame".
	{
		set_view(w.rtv, w.screen, DXGI_FORMAT_R8G8B8A8_UNORM);
		const counts before = counts_now();
		frame(g, w, { true });
		const picture_diff px = picture("C12");
		const counts d = delta(before, counts_now());
		check(one_skip(d, "the world's end wasn't marked this frame") && same_as_reference(px),
			"C12 (today) the world's view re-pointed: nothing drawn, +1 \"the world's end wasn't marked this frame\" (" + describe(d) + ")");
		set_view(w.rtv, w.rt, w.rt_fmt);
		frame(g, w);
		g.idle();
		check(g.take_errors() == 0, "C12 no debug-layer errors");
	}
#endif

	// C13. No zone markers: presents only
	{
		const counts before = counts_now();
		uint32_t out[4] = {};
		for (int i = 0; i < 7; ++i)
			present(g);
		worldfx_world(out);
		check(out[2] == 1, "C13 7 presents after the last marker: the world still marked");
		present(g);
		worldfx_world(out);
		check(out[2] == 0, "C13 8: no longer marked (kMarkerPresents)");
		present(g);
		counts d = delta(before, counts_now());
		check(d.drawn == 0 && d.skipped_total() == 9 && d.skipped["the world's end wasn't marked this frame"] == 9,
			"C13 9 presents: +9 \"the world's end wasn't marked this frame\" (" + describe(d) + ")");
		check(line_of(status(), "The world: ") == "The world: 640x360, found (no zone markers lately)", "C13 the status: \"found (no zone markers lately)\"");
	}

	// C14. worldfx.hlsl next to the add-on, the same as the built-in shaders
	{
		const bool put = CopyFileW(L"src\\render\\grass.hlsl", (folder + L"worldfx.hlsl").c_str(), FALSE) != 0;
		int n = 0;
		std::string shaders;
		for (; n < 31; ++n)
		{
			present(g);
			shaders = line_of(status(), "Shaders: ");
			if (shaders.rfind("Shaders: worldfx.hlsl, the same as the shaders in use (", 0) == 0)
				break;
		}
		std::printf("  %s (after %d presents)\n", shaders.c_str(), n + 1);
		check(put && n < 31, "C14 worldfx.hlsl the same as the shaders in use: said so within 31 presents");
		DeleteFileW((folder + L"worldfx.hlsl").c_str());
		frame(g, w);
	}

	// C16. FloatWorld: the world made again as RGBA16F at the next zone begin, drawn
	{
		const float on[3] = { 1.0f, 0.0f, 0.0f }, off[3] = { 0.0f, 0.0f, 0.0f };
		worldfx::set_setting("FloatWorld", on);
		check(worldfx::float_world(), "C16 float_world() true");
		counts before = counts_now();
		frame(g, w);
		counts d = delta(before, counts_now());
		const std::string look = line_of(status(), "The look: ");
		check(w.rt_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT && d.drawn == 1, "C16 the world remade as RGBA16F, drawn (" + describe(d) + ")");
		check(look.find("light past white on") != std::string::npos, "C16 the status: \"light past white on\"");
		worldfx::set_setting("FloatWorld", off);
		before = counts_now();
		frame(g, w);
		d = delta(before, counts_now());
		check(w.rt_fmt == DXGI_FORMAT_R8G8B8A8_UNORM && d.drawn == 1, "C16 back to RGBA8, drawn");
		g.idle();
		check(g.take_errors() == 0, "C16 no debug-layer errors");
	}

	// C15. Release behind the fence: the queue held on the test's own fence while the zone changes
	{
		at("C15");
		frame(g, w);
		g.idle();
		g.take_errors();
		ID3D12Fence *gate = nullptr;
		g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate));
		const uint32_t blades_before = g.live_named("worldfx: blades");
		check(blades_before == 1, "C15 one blade buffer alive, drawing in zone 100 (" + std::to_string(blades_before) + ")");
		g.queue->Wait(gate, 1);
		frame_opts o;
		o.gated = true;
		frame(g, w, o);  // K: blades
		zone(104, true);
		frame(g, w, o);  // K+1: the guard frame; its present reads zone 104's files, the blades let go behind the fence
		bool kept = true, rising = true, none_deleted = true;
		uint32_t last_behind = 0;
		for (int i = 0; i < 3; ++i)
		{
			present(g);
			const uint32_t blades = g.live_named("worldfx: blades"), memory = g.live_named("worldfx: colour memory");
			const std::string pacing = line_of(status(), "Frame pacing: ");
			const size_t at_behind = pacing.find("the GPU ");
			const uint32_t behind = at_behind != std::string::npos ? uint32_t(std::strtoul(pacing.c_str() + at_behind + 8, nullptr, 10)) : 0;
			std::printf("  gated present %d: %u blade buffers, %u colour memories alive; the GPU %u presents behind\n", i + 1, blades, memory, behind);
			kept = kept && blades == 1 && memory >= 1;
			rising = rising && behind > last_behind;
			last_behind = behind;
			none_deleted = none_deleted && g.deleted_in_use == 0;
		}
		check(kept, "C15 the GPU held back: the old blades and colour memory kept at every present");
		check(rising, "C15 the status: the GPU more presents behind at each");
		gate->Signal(1);
		g.wait(g.value);
		int freed_at = 0;
		bool stays = true;
		for (int i = 0; i < 4; ++i)
		{
			present(g);
			const uint32_t blades = g.live_named("worldfx: blades");
			if (blades == 0 && freed_at == 0)
				freed_at = i + 1;
			else if (blades != 0 && freed_at != 0)
				stays = false;
		}
		std::printf("  the GPU caught up: the old blades freed at present %d after\n", freed_at);
		check(freed_at >= 1 && freed_at <= 3 && stays, "C15 once the GPU caught up: freed by the 3rd present, and stays freed");
		g.idle();
		check(g.take_errors() == 0 && none_deleted && g.deleted_in_use == 0, "C15 nothing deleted while in use, no debug-layer errors");
		graveyard.push_back(gate);
		zone(100, true);
		frame(g, w);
		frame(g, w);
		g.idle();
		g.take_errors();
	}

	// The threads part. Three threads: the game's (40 frames at 320 x 180), the feed thread, the copy-queue loader; the probe at 30..34
	{
		at("threads: the little world");
		depth_t depth_small = make_depth(g, false, 320, 180);
		world_t little = make_world(g, &depth_small, 320, 180);
		frame(g, little); // (the core's world moved to it before the old one could go)
		g.idle();
		g.take_errors();
		const counts before = counts_now();
		const uint64_t in_game_before = presents_in_game;
		b_running = true;
		std::thread feed(feed_loop), loader(loader_loop, g.device);
		bool probe_ok_all = true, overlay_all = true;
		int probes_reached = 0;
		counts last_before;
		for (int i = 0; i < 40; ++i)
		{
			const bool probe = i >= 30 && i < 30 + kProbes;
			int gen = 0;
			if (probe)
			{
				at("threads: waiting for the feed thread to park");
				gen = ++probe_gen;
				while (b_parked_gen != gen)
				{
					a_beat++;
					Sleep(1);
				}
				ResetEvent(ev_feed_done);
				probe_ok = false;
				probe_armed = true;
			}
			if (i == 39)
				last_before = counts_now();
			const int hits = probe_hits;
			frame(g, little);
			if (probe)
			{
				const bool reached = probe_hits != hits;
				if (probe_armed.exchange(false))
					SetEvent(ev_inside); // (not reached: the feed thread goes on)
				at("threads: waiting for the feed thread's probe calls");
				while (b_handled_gen != gen)
				{
					a_beat++;
					Sleep(1);
				}
				std::printf("  probe %d: %s; inside the draw a feed call %s, the overlay %zu lines\n", gen, reached ? "reached" : "not reached",
					probe_ok ? "completed" : "waited (3 s timeout)", probe_overlay_lines[gen]);
				if (reached)
				{
					probes_reached++;
#if WFX_CORE_API < 3
					probe_ok_all = probe_ok_all && !probe_ok;
					overlay_all = overlay_all && probe_overlay_empty[gen];
#else
					probe_ok_all = probe_ok_all && probe_ok;
					overlay_all = overlay_all && probe_overlay_lines[gen] == 1;
#endif
				}
			}
		}
		const counts last_d = delta(last_before, counts_now());
		at("threads: joining");
		g.idle();
		a_done = true;
		feed.join();
		loader.join();
		b_running = false;
		const counts d = delta(before, counts_now());
		std::printf("  %s; the feed thread made %llu calls, the loader %llu uploads\n", describe(d).c_str(), static_cast<unsigned long long>(b_calls.load()),
			static_cast<unsigned long long>(c_uploads.load()));
		check(true, "threads: no deadlock (the watchdog)");
		check(g.take_errors() == 0, "threads: no debug-layer errors with the three threads");
		check(d.drawn + d.skipped_total() == presents_in_game - in_game_before && d.drawn >= 35, "threads: every present counted once; drawn (" + describe(d) + ")");
		check(last_d.drawn == 1, "threads: the last frame drawn");
		check(b_calls > 0 && c_uploads > 0 && !c_failed, "threads: the feed thread made calls, the loader uploaded");
		check(b_blades_seen > 0 && b_bad_blades == 0, "threads: the feed thread's status: \"Blades: zone 100: N blades in M cells\" exact" +
			(b_bad_blades != 0 ? " (seen \"" + b_bad_example + "\")" : std::string()));
		check(b_pushing_seen > 0 && b_bad_pushing == 0, "threads: the feed thread's status: \"Pushing the grass: k characters ...\", k from 1 to 3");
		check(b_bad_glows == 0 && b_bad_height == 0, "threads: glow stats one of the counts sent (3 or 5); GrassHeight one of the values set");
		check(probes_reached == kProbes, "threads: the probe reached inside the blades' draw at each of frames 30..34 (" + std::to_string(probes_reached) + ")");
#if WFX_CORE_API < 3
		check(probe_ok_all && overlay_all, "threads: (today) inside the blades' draw a feed call waits for the core's lock (ok false); the overlay empty meanwhile");
#else
		check(probe_ok_all && overlay_all, "threads: inside the blades' draw a feed call completes (no lock while recording); the overlay has its line");
#endif
		worldfx_set_setting("GrassHeight", 0.5f);
		graveyard.push_back(little.rt);
		graveyard.push_back(little.screen);
		graveyard.push_back(depth_small.res);
		w.depth = &depth;
		frame(g, w); // (the core's world back on the big one)
		g.idle();
	}

	// C18. Every present in the game counted once, drawn or skipped
	{
		const counts c = counts_now();
		std::printf("  %llu presents in the game: %s\n", static_cast<unsigned long long>(presents_in_game), describe(c).c_str());
		check(c.drawn + c.skipped_total() == presents_in_game, "C18 every present in the game counted once (drawn + skipped == presents in the game)");
	}

	// C17. Detach, the GPU idle: nothing of World FX's left
	{
		at("detach");
		g.idle();
		g.take_errors();
		worldfx::detach(g.device);
		const uint32_t left = g.live_named("worldfx:");
		check(left == 0 && worldfx::attached() == nullptr, "C17 detached: no object named \"worldfx:\" left (" + std::to_string(left) + "), attached() nullptr");
		check(g.take_errors() == 0, "C17 no debug-layer errors");
	}

	watch_stop = true;
	watch.join();
	for (IUnknown *u : graveyard)
		u->Release();
	std::printf("  %.1f s\n", seconds_since(t_start));
	std::printf(failures == 0 ? "all passed (%d)\n" : "%d failed\n", failures == 0 ? passes : failures);
	return failures == 0 ? 0 : 1;
}
