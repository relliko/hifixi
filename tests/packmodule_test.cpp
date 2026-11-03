/*
 * The texture pack's module (src/pack/pack_module; docs/texture-pack/zones.md "The module's lifecycle") driven as the
 * plugin drives it, through passes::modules() ("textures": update, present, device, command, stop), on WARP with the
 * debug layer (HIFIXI_GBV=1: GPU-based validation too), its settings in a folder of its own, a made-up pack under
 * build\test\packmodule, and a fake consumer standing in for the zones' glue (it asks for textures and drops them):
 *   - a second fake consumer (the characters' glue's place: main.cpp adds src/chars/charpack after the zones'), the
 *     first one added twice (kept once): [textures] chars on by default for both, their frames, the second's line after
 *     the first's, the hidden chars key off and on, and off clearing the first then the second; the module's device
 *     published while in use (pack::device_in_use);
 *   - the quiet window over made-up timelines (quiet_window), and nothing read while the module holds kQuiet;
 *   - no pack folder: nothing started, nothing in use; the folder made, a reload finds it; pack=0: the streamer not
 *     started (it starts at the first enable with a device);
 *   - on: the streamer started once, the pack opened on its worker, in use in the pack's epoch, the loaders' pack mode,
 *     the Found line with the pack's own counts; a safe start keeps it off until /hifi textures on;
 *   - requests read ready; compare; reload: the consumer cleared before the open, a new epoch, the same streamer;
 *     a reload while the pack still opens (the first open held on the worker after its pack.idx is loaded): not in
 *     use till the second is done, then in its epoch, requests ready (none gone); a device change while it opens:
 *     nothing asked again, in use in the epoch after the change; the FSR bias's switch (pack::fsr_bias_on): on while
 *     in use with [textures] fsr_bias, not with it 0, off or stopped;
 *     off: cleared before the close, the pack closed, kOff, the streamer kept, the folder free to rename (held while
 *     on); on again: the same streamer;
 *   - a miss at the worker (a damaged record) logged from update, through the log;
 *   - a device change (none, then the device again): a new epoch, the pack in use again;
 *   - the plugin's unload with 1,000 queued and four copies in flight (held by a gate opened 5 ms into stop): the
 *     consumer cleared, then stop, retire::release_all: nothing leaked, the device's references back; loaded again:
 *     a second start, in use; unloaded again, the references back;
 *   - folder_exists, menu_lines and folder_text make no file, DXGI or streamer call; the worst present's time;
 *   - last, the device removed (gpu_crash's watch sees it): at the next present the consumer cleared first, the
 *     loaders out of pack mode, not in use, its frame not called again, the status, menu and log say the card stopped.
 *
 *   build\packmodule_test.exe     (ZONE_GPU=1: the GPU instead of WARP)
 */

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../src/backend/backend.hpp"
#include "../src/game/game.hpp"
#include "../src/host/passes.hpp"
#include "../src/host/retire.hpp"
#include "../src/pack/pack.hpp"
#include "../src/pack/pack_module.hpp"
#include "../src/pack/pack_write.hpp"
#include "../src/pack/stream.hpp"
#include "../src/pack/stream_crumbs.hpp"
#include "../src/pack/validate.hpp"
#include "../src/render/gpu_crash.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace st = hifi::pack::stream;
namespace pk = hifi::pack;

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
	void note(const std::string &what) { std::printf("--    %s\n", what.c_str()); }

	double now_ms()
	{
		LARGE_INTEGER t, f;
		QueryPerformanceCounter(&t);
		QueryPerformanceFrequency(&f);
		return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
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

	void write_text(const std::wstring &path, const std::string &text)
	{
		if (FILE *f = nullptr; _wfopen_s(&f, path.c_str(), L"wb") == 0 && f != nullptr)
		{
			fwrite(text.data(), 1, text.size(), f);
			fclose(f);
		}
	}

	// ---- The device, the stand-in for the game's queue (as packstream_test's)

	struct gpu_t
	{
		ID3D12Device *device = nullptr;
		ID3D12InfoQueue *info = nullptr;
		ID3D12CommandQueue *direct = nullptr;
		ID3D12Fence *fence = nullptr;
		uint64_t value = 0;
		HANDLE event = nullptr;
	};
	gpu_t G;

	bool make_gpu(bool hardware)
	{
		IDXGIFactory4 *factory = nullptr;
		IDXGIAdapter1 *warp = nullptr;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
			return false;
		if (!hardware)
			factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
		const bool ok = SUCCEEDED(D3D12CreateDevice(hardware ? nullptr : warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&G.device)));
		if (warp != nullptr)
			warp->Release();
		factory->Release();
		if (!ok)
			return false;
		G.device->QueryInterface(IID_PPV_ARGS(&G.info));
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		G.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&G.direct));
		G.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&G.fence));
		G.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return G.direct != nullptr && G.fence != nullptr;
	}

	void idle()
	{
		G.direct->Signal(G.fence, ++G.value);
		if (G.fence->GetCompletedValue() < G.value)
		{
			G.fence->SetEventOnCompletion(G.value, G.event);
			WaitForSingleObject(G.event, 30000);
		}
	}

	uint32_t debug_messages(std::string &first)
	{
		if (G.info == nullptr)
			return 0;
		const UINT64 n = G.info->GetNumStoredMessages();
		uint32_t errors = 0;
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T size = 0;
			G.info->GetMessage(i, nullptr, &size);
			std::vector<uint8_t> buf(size);
			D3D12_MESSAGE *m = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
			G.info->GetMessage(i, m, &size);
			if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && errors++ == 0)
				first = m->pDescription;
		}
		G.info->ClearStoredMessages();
		return errors;
	}
	void messages_check(const std::string &part)
	{
		std::string first;
		const uint32_t n = debug_messages(first);
		check(n == 0, part + ": the debug layer said nothing (" + std::to_string(n) + (n != 0 ? ": " + first : std::string()) + ")");
	}

	ULONG device_refs()
	{
		const ULONG refs = G.device->AddRef();
		G.device->Release();
		return refs - 1;
	}

	// ---- The made-up pack: small BC2 textures (zone and model entries), a few 2048x1024 for copies held in flight

	struct made_entry
	{
		pk::key k;
		std::string tag;
	};

	uint64_t mix(uint64_t x)
	{
		x += 0x9E3779B97F4A7C15ull;
		x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
		x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
		return x ^ (x >> 31);
	}

	bool make_pack(const std::wstring &folder, std::vector<made_entry> &out, uint32_t &zones)
	{
		remove_tree(folder);
		pk::writer w;
		pk::writer_options o;
		o.folder = folder;
		o.source = "packmodule_test";
		std::string why;
		if (!w.open(o, why))
		{
			note("the made-up pack couldn't be opened: " + why);
			return false;
		}
		zones = 0;
		auto add = [&](uint32_t side_w, uint32_t side_h, const std::string &tag) {
			const uint32_t i = uint32_t(out.size());
			pk::entry e;
			e.hash = mix(i + 7);
			e.bytes = 128 + i;
			e.w = side_w / 2;
			e.h = side_h / 2;
			e.orig_format = 0xA1;
			e.source_kind = i % 3 == 0 ? pk::kSourceModel : pk::kSourceZone;
			zones += e.source_kind == pk::kSourceZone;
			std::snprintf(e.name, sizeof(e.name), "pm%013u", i);
			pk::map_in m;
			m.format = pk::kBC2;
			m.w = side_w;
			m.h = side_h;
			const uint32_t levels = pk::full_levels(side_w, side_h);
			for (uint32_t l = 0; l < levels; ++l)
			{
				const uint32_t lw = std::max(1u, side_w >> l), lh = std::max(1u, side_h >> l);
				std::vector<uint8_t> b(size_t((lw + 3) / 4) * ((lh + 3) / 4) * pk::block_bytes(pk::kBC2));
				uint64_t s = mix(uint64_t(i) * 131 + l);
				for (size_t k = 0; k < b.size(); k += 8)
				{
					s = mix(s);
					std::memcpy(b.data() + k, &s, std::min<size_t>(8, b.size() - k));
				}
				m.levels.push_back(std::move(b));
			}
			if (w.add(e, { m }, why) != pk::add_result::ok)
				return false;
			out.push_back(made_entry { e.the_key(), tag });
			return true;
		};
		bool ok = true;
		for (int i = 0; i < 1100 && ok; ++i)
			ok = add(i % 4 == 0 ? 128u : 64u, 64, "small");
		for (int i = 0; i < 12 && ok; ++i)
			ok = add(2048, 1024, "big");
		ok = ok && add(256, 256, "damaged");
		if (!ok || !w.commit(why))
		{
			note("the made-up pack couldn't be written: " + why);
			return false;
		}
		w.close();
		// (one record damaged after its CRC was taken: the worker misses it, "a CRC mismatch")
		std::shared_ptr<const pk::index> idx = pk::load(folder, nullptr, why);
		const pk::entry *e = idx ? idx->find(out.back().k) : nullptr;
		const pk::record *r = e != nullptr ? e->map(pk::kColour) : nullptr;
		if (r == nullptr)
			return false;
		const std::wstring data = pk::data_path(folder, r->file);
		const HANDLE h = CreateFileW(data.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		LARGE_INTEGER at;
		at.QuadPart = int64_t(r->offset) + 16;
		uint8_t byte = 0;
		DWORD n = 0;
		bool flipped = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, &byte, 1, &n, nullptr) && n == 1;
		byte ^= 0x5A;
		flipped = flipped && SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && WriteFile(h, &byte, 1, &n, nullptr) && n == 1;
		CloseHandle(h);
		return flipped;
	}

	// ---- The fake consumer (the zones' glue): one owner asking for textures; what the module called, in order

	namespace fake
	{
		std::vector<std::string> calls;
		st::owner owner = 0;
		std::vector<st::ticket> tickets;
		bool zones = false, actors = false, chars = false, compare = false;
		int frames = 0;
		uint32_t epoch_at_clear = 0;      // (the streamer's epoch when clear_all was last called)
		bool snapshot_at_clear = false;   // (and whether a pack was open then)

		std::vector<std::string> order; // (both fakes' clear_all and set_mode, in the order the module called them)
		void clear_all()
		{
			calls.push_back("clear");
			order.push_back("1 clear");
			epoch_at_clear = st::epoch();
			snapshot_at_clear = st::current() != nullptr;
			if (owner != 0)
				st::drop_owner(owner);
			owner = 0;
			tickets.clear();
		}
		void set_mode(const pk::modes &m)
		{
			calls.push_back(std::string("mode ") + (m.zones ? "1" : "0") + (m.actors ? "1" : "0"));
			order.push_back("1 mode");
			zones = m.zones;
			actors = m.actors;
			chars = m.chars;
		}
		void frame(double, uint32_t) { frames++; }
		void set_compare(bool on)
		{
			calls.push_back(on ? "compare 1" : "compare 0");
			compare = on;
		}
		void lines(std::vector<std::string> &out) { out.push_back("This area: the test's textures"); }

		// Asked from the pack in use, in its epoch (count from `first` of the made-up entries)
		void ask(const std::vector<made_entry> &made, size_t first, size_t count, int32_t priority = 0)
		{
			if (owner == 0)
				owner = st::add_owner("packmodule_test");
			for (size_t i = first; i < first + count && i < made.size(); ++i)
			{
				st::want w;
				w.k = made[i].k;
				w.priority = priority;
				w.epoch = pk::epoch_in_use();
				tickets.push_back(st::request(owner, w));
			}
		}
		size_t count(st::state s)
		{
			size_t n = 0;
			for (const st::ticket &t : tickets)
				n += st::poll(t) == s;
			return n;
		}
		size_t index_of(const std::string &call)
		{
			for (size_t i = 0; i < calls.size(); ++i)
				if (calls[i] == call)
					return i;
			return SIZE_MAX;
		}
		// Whether every call a in order came before the next b (each pair in turn, none left over): the first consumer
		// called first each time
		bool in_turn(const std::string &a, const std::string &b)
		{
			size_t pairs = 0;
			bool want_a = true;
			for (const std::string &o : order)
				if (o == a || o == b)
				{
					if ((o == a) != want_a)
						return false;
					pairs += o == b;
					want_a = !want_a;
				}
			return want_a && pairs != 0;
		}
	}

	// ---- A second fake consumer (the characters' glue stands there in main.cpp): its calls after the first's
	namespace fake2
	{
		bool chars = false;
		int frames = 0;
		void clear_all() { fake::order.push_back("2 clear"); }
		void set_mode(const pk::modes &m)
		{
			fake::order.push_back("2 mode");
			chars = m.chars;
		}
		void frame(double, uint32_t) { frames++; }
		void lines(std::vector<std::string> &out) { out.push_back("Characters: the test's second line"); }
	}

	// ---- The module, as the plugin calls it

	const hifi::passes::module *M = nullptr;
	double g_now = 100.0;
	double g_worst_present = 0.0;

	void update()
	{
		g_now += 0.5;
		M->update(g_now);
	}
	// A present: the module's, then the retire list's signal on the game's queue (backend::present's)
	void present()
	{
		g_now += 0.004;
		const double t0 = now_ms();
		M->present(g_now, true);
		g_worst_present = std::max(g_worst_present, now_ms() - t0);
		hifi::retire::on_present(G.direct);
	}
	std::vector<std::string> command(const char *what)
	{
		std::vector<std::string> args { "textures" }, out;
		if (what != nullptr)
			args.push_back(what);
		M->command(args, out);
		return out;
	}
	bool has(const std::vector<std::string> &lines, const char *text)
	{
		for (const std::string &l : lines)
			if (l.find(text) != std::string::npos)
				return true;
		return false;
	}
	// Updates until the pack is in use (or the time is up)
	bool wait_in_use(double timeout_ms = 30000)
	{
		const double t0 = now_ms();
		while (now_ms() - t0 < timeout_ms)
		{
			update();
			if (pk::in_use())
				return true;
			Sleep(2);
		}
		return false;
	}
	// Presents until no ticket waits (or the time is up)
	bool wait_tickets(double timeout_ms = 30000)
	{
		const double t0 = now_ms();
		while (now_ms() - t0 < timeout_ms)
		{
			present();
			if (fake::count(st::state::queued) + fake::count(st::state::reading) + fake::count(st::state::copying) == 0)
				return true;
			Sleep(1);
		}
		return false;
	}
	// The worker's open or close done (its epoch past e)
	bool wait_epoch_past(uint32_t e, double timeout_ms = 30000)
	{
		const double t0 = now_ms();
		while (st::epoch() == e && now_ms() - t0 < timeout_ms)
			Sleep(1);
		return st::epoch() != e;
	}
	bool wait_held(uint32_t n, double timeout_ms = 10000)
	{
		namespace sc = hifi::pack::stream_crumbs;
		const double t0 = now_ms();
		while (now_ms() - t0 < timeout_ms)
		{
			present();
			if (sc::the.in_flight.load() >= n && sc::the.state.load() == sc::kFenceWait)
				return true;
			Sleep(1);
		}
		return false;
	}
	// (test::after_load: the worker's nth open held after its pack.idx is loaded till g_hold[n - 1] is set, the third and
	// on by the second's)
	std::atomic<int> g_loads { 0 };
	HANDLE g_hold[2] = {};
	void hold_load()
	{
		const int n = ++g_loads;
		WaitForSingleObject(g_hold[std::min(n, 2) - 1], 30000);
	}
	bool wait_loads(int n, double timeout_ms = 30000)
	{
		const double t0 = now_ms();
		while (g_loads.load() < n && now_ms() - t0 < timeout_ms)
			Sleep(1);
		return g_loads.load() >= n;
	}

	void settle()
	{
		for (int i = 0; i < 4; ++i)
		{
			hifi::retire::on_present(G.direct);
			idle();
		}
	}

	std::mutex g_log_mutex;
	std::vector<std::string> g_log;
	void log_sink(int, const char *text)
	{
		std::lock_guard<std::mutex> lock(g_log_mutex);
		g_log.push_back(text);
	}
	bool logged(const char *text)
	{
		std::lock_guard<std::mutex> lock(g_log_mutex);
		return has(g_log, text);
	}
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const std::wstring build = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\') + 1);
	const std::wstring here = build + L"test\\packmodule\\", pack_folder = here + L"hifixi-pack\\", settings_file = here + L"config\\hifixi\\settings.ini";
	CreateDirectoryW((build + L"test").c_str(), nullptr);
	CreateDirectoryW(here.c_str(), nullptr);
	CreateDirectoryW((here + L"config").c_str(), nullptr);
	CreateDirectoryW((here + L"config\\hifixi").c_str(), nullptr);
	remove_tree(pack_folder);
	DeleteFileW(settings_file.c_str());
	SetEnvironmentVariableW(L"HIFIXI_PACK", pack_folder.c_str());
	// (no pivot.ini: the overlay isn't listed)
	SetEnvironmentVariableW(L"HIFIXI_PIVOT_INI", (here + L"no-pivot.ini").c_str());
	const bool on_gpu = GetEnvironmentVariableA("ZONE_GPU", nullptr, 0) != 0;

	// ---- The quiet window over made-up timelines
	{
		pk::quiet_inputs q;
		q.delay = 2.0;
		q.now = 10.0;
		q.in_since = -1.0;
		const bool zoning = pk::quiet_window(q);
		q.in_since = 9.0;
		const bool just_in = pk::quiet_window(q);
		q.now = 11.0;
		q.ready_presents = 0;
		const bool old_zone_held = pk::quiet_window(q);
		q.ready_presents = pk::kQuietPresents - 1;
		const bool two_presents = pk::quiet_window(q);
		q.ready_presents = pk::kQuietPresents;
		const bool three_presents = pk::quiet_window(q);
		check(zoning && just_in && !old_zone_held && !two_presents && !three_presents,
			"the quiet window: zoning quiet, in for 1 of 2 s quiet; past the delay, the old zone not yet let go but video memory unknown: not quiet");
		q.ready_presents = 0;
		q.vram_usage = 90;
		q.vram_budget = 100;
		const bool full = pk::quiet_window(q);
		q.ready_presents = pk::kQuietPresents;
		const bool full_retired = pk::quiet_window(q);
		q.ready_presents = 0;
		q.vram_usage = 84;
		const bool headroom = pk::quiet_window(q);
		q.vram_usage = 90;
		q.zone_wanted = false;
		const bool no_zone = pk::quiet_window(q);
		check(full && !full_retired && !headroom && !no_zone,
			"at 90% of the video memory: quiet until the new zone has been ready 3 presents (the old one past the retire fence); at 84%, or no zone wanted: not");
		q.zone_wanted = true;
		q.ready_presents = pk::kQuietPresents;
		q.delay = 5.0;
		const bool longer = pk::quiet_window(q);
		q.delay = 0.0;
		q.now = q.in_since;
		const bool none = pk::quiet_window(q);
		check(longer && !none, "[textures] zone_delay_s: 5 s still quiet 2 s in; 0 not quiet the moment the character is in");
		check(pk::fsr_bias(1.0f) == 0.0f && pk::fsr_bias(0.0f) == 0.0f && std::fabs(pk::fsr_bias(1.0f / 1.5f) + 0.585f) < 0.01f && pk::fsr_bias(0.5f) == -1.0f &&
			pk::fsr_bias(1.0f / 3.0f) == -1.0f && pk::fsr_bias(2.0f) == 0.0f, "the FSR bias: 0 at native or unknown, -0.58 at 1.5x, -1 at 2x, clamped at -1 at 3x");
	}

	// ---- The settings, the device, the module
	check(hifi::backend::start_settings_only(settings_file.c_str(), here.c_str()), "the settings alone started (no hooks), in a folder of its own");
	hifi::backend::set_log_sink(log_sink);
	ID3D12Debug *debug = nullptr;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
	{
		debug->EnableDebugLayer();
		ID3D12Debug1 *debug1 = nullptr;
		if (GetEnvironmentVariableA("HIFIXI_GBV", nullptr, 0) != 0 && SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))))
		{
			debug1->SetEnableGPUBasedValidation(TRUE);
			debug1->Release();
			std::printf("      (GPU-based validation on)\n");
		}
	}
	if (!make_gpu(on_gpu))
	{
		check(false, on_gpu ? "a device on the GPU" : "a WARP device");
		return 1;
	}
	note(std::string(on_gpu ? "on the GPU" : "on WARP") + (G.info != nullptr ? ", the debug layer on" : ", NO debug layer (Graphics Tools missing)"));
	for (const hifi::passes::module &m : hifi::passes::modules())
		if (m.name != nullptr && std::strcmp(m.name, "textures") == 0)
			M = &m;
	check(M != nullptr && M->update != nullptr && M->present != nullptr && M->stop != nullptr && M->command != nullptr && M->device != nullptr,
		"the \"textures\" module registered, with update, present, device, command and stop");
	if (M == nullptr)
		return 1;
	pk::consumer c;
	c.clear_all = fake::clear_all;
	c.set_mode = fake::set_mode;
	c.frame = fake::frame;
	c.set_compare = fake::set_compare;
	c.lines = fake::lines;
	pk::test::clear_consumers();
	pk::add_consumer(c);
	pk::add_consumer(c); // (wired twice: added once)
	{
		pk::consumer c2;
		c2.clear_all = fake2::clear_all;
		c2.set_mode = fake2::set_mode;
		c2.frame = fake2::frame;
		c2.lines = fake2::lines;
		pk::add_consumer(c2);
	}
	pk::test::force_quiet(0);
	settle(); // (the retire list's own fence made first: it holds the device too)
	const ULONG refs_before = device_refs();

	// ---- No pack folder: nothing started, nothing shown
	{
		M->device(G.device);
		update();
		present();
		hifi::game::set_option_on("textures", "pack", true);
		update();
		present();
		check(!pk::folder_exists() && !pk::in_use() && pk::test::stream_starts() == 0 && !st::running() &&
			has(command("status"), "no texture pack on this PC"), "no pack folder (pack=1, a device): not shown, nothing started, the status says so");
		check(pk::folder_text().find("packmodule\\hifixi-pack\\") != std::string::npos, "the Files line's folder: " + pk::folder_text());
	}

	// ---- The folder made; pack=0: found by a reload, the streamer not started
	std::vector<made_entry> made;
	uint32_t zones = 0;
	{
		const double t0 = now_ms();
		const bool written = make_pack(pack_folder, made, zones);
		check(written, "the made-up pack written: " + std::to_string(made.size()) + " entries (" + std::to_string(zones) +
			" scenery), one record damaged, in " + std::to_string(int(now_ms() - t0)) + " ms");
		hifi::game::set_option_on("textures", "pack", false);
		update();
		check(!pk::folder_exists(), "not looked for again by itself (only at the first update, on and reload)");
		command("reload");
		update();
		check(pk::folder_exists() && !pk::in_use() && pk::test::stream_starts() == 0 && !st::running() && has(pk::menu_lines(), "Off: turn it on above.") &&
			has(pk::menu_lines(), "To remove the texture pack: turn it off, then delete the hifixi-pack folder"),
			"/hifi textures reload finds it; pack=0: off, the streamer not started; the menu says Off and how to remove it");
	}

	// ---- A safe start keeps it off till /hifi textures on
	{
		write_text(settings_file, "[hifixi]\r\npreset=custom\r\n\r\n[worldfx]\r\nGpuStopped=1\r\n\r\n[textures]\r\npack=1\r\n");
		check(hifi::backend::start_settings_only(settings_file.c_str(), here.c_str()) && hifi::backend::safe_start(), "the settings again after a GPU stop: a safe start");
		update();
		update();
		check(!pk::in_use() && pk::test::stream_starts() == 0 && has(pk::menu_lines(), "Paused after a graphics-card stop") &&
			has(command("status"), "paused after a graphics-card stop"), "pack=1 after a GPU stop: off for the session, said so");
		fake::calls.clear();
		const std::vector<std::string> out = command("on");
		const bool in = wait_in_use();
		check(in && has(out, "despite the graphics card's stop") && pk::test::stream_starts() == 1 && st::running(),
			"/hifi textures on: used this session anyway, the streamer started");
		check(pk::epoch_in_use() == st::epoch() && st::current() != nullptr && fake::zones && fake::actors && fake::index_of("mode 11") != SIZE_MAX &&
			logged("texture pack: ") && logged("opened"), "in use: in the streamer's epoch (" + std::to_string(pk::epoch_in_use()) +
			"), the loaders in pack mode (zones and model actors), the open logged");
		char found[160];
		std::snprintf(found, sizeof(found), "Found: %s textures (scenery %s, the rest %s)", "1,113", std::to_string(zones).c_str(),
			std::to_string(made.size() - zones).c_str());
		const std::vector<std::string> menu = pk::menu_lines();
		check(has(menu, found) && has(menu, "This area: the test's textures") && has(menu, "On the graphics card:"),
			std::string("the menu: \"") + (menu.empty() ? "" : menu[0]) + "\", the consumer's line, the graphics card's");
		// (the second consumer: the characters' mode on by default, its frame called, its line after the first's)
		const int frames2 = fake2::frames;
		present();
		const auto at = [&](const std::string &l) { return size_t(std::find(menu.begin(), menu.end(), l) - menu.begin()); };
		check(fake::chars && fake2::chars && fake2::frames == frames2 + 1 && logged("characters on") &&
				at("Characters: the test's second line") == at("This area: the test's textures") + 1 && pk::device_in_use() == G.device,
			"the second consumer: [textures] chars on by default for both, its frame each present, its line after the first's; the module's device published");
		// ([textures] chars, hidden: off and on again, to both consumers, each time the first one first)
		fake::order.clear();
		hifi::game::set_option_on("textures", "chars", false);
		update();
		const bool chars_off = !fake::chars && !fake2::chars && fake::zones && fake::actors;
		hifi::game::set_option_on("textures", "chars", true);
		update();
		check(chars_off && fake::chars && fake2::chars && fake::in_turn("1 mode", "2 mode") && fake::order.size() == 4,
			"[textures] chars=0: the modes with chars off to both (zones and actors still on), then on again; the first consumer first each time");
		// (the rest of the test as an ordinary session: the safe start gone)
		write_text(settings_file, "[hifixi]\r\npreset=custom\r\n\r\n[textures]\r\npack=1\r\n");
		hifi::backend::start_settings_only(settings_file.c_str(), here.c_str());
	}

	// ---- Requests: held while quiet, ready after; compare
	{
		pk::test::force_quiet(1);
		present();
		const st::counts c0 = st::get_counts();
		fake::ask(made, 0, 200);
		for (int i = 0; i < 20; ++i)
		{
			present();
			Sleep(2);
		}
		const st::counts c1 = st::get_counts();
		check(pk::quiet() && (st::paused() & st::kQuiet) != 0 && c1.reads == c0.reads && c1.made == c0.made && fake::count(st::state::queued) == 200,
			"quiet (the zone settling): kQuiet set, 200 asked, nothing read or made in 20 presents");
		pk::test::force_quiet(0);
		const bool all = wait_tickets();
		check(all && !pk::quiet() && (st::paused() & st::kQuiet) == 0 && fake::count(st::state::ready) == 200 && fake::frames > 0,
			"the window over: kQuiet let go, all 200 ready; the consumer's frame called each present");
		command("compare");
		const bool comparing = pk::comparing() && fake::compare;
		update();
		const bool said = has(pk::menu_lines(), "Comparing: the game's own textures are drawn");
		command("compare");
		check(comparing && said && !pk::comparing() && !fake::compare && fake::count(st::state::ready) == 200,
			"/hifi textures compare: the consumer told at once, the menu says so; again: off; nothing freed");
	}

	// ---- A miss at the worker logged from update
	{
		fake::ask(made, made.size() - 1, 1);
		wait_tickets();
		update();
		check(fake::count(st::state::missed) == 1 && logged("CRC") && logged("pm0000000001112"),
			"the damaged record: missed at the worker, its line in the log (from the module's update) with the entry's name");
	}

	// ---- Reload: cleared before the open, a new epoch, the same streamer
	{
		const uint32_t e0 = pk::epoch_in_use();
		fake::calls.clear();
		const std::vector<std::string> out = command("reload");
		const size_t clear_at = fake::index_of("clear");
		const bool cleared_first = clear_at == 0 && fake::epoch_at_clear == e0 && fake::snapshot_at_clear && fake::owner == 0 && !pk::in_use();
		const bool in = wait_in_use();
		check(has(out, "opening the pack again") && cleared_first, "reload: the consumer cleared first (the epoch then still " + std::to_string(e0) +
			", the pack open), not in use while it opens");
		check(in && pk::epoch_in_use() > e0 && pk::test::stream_starts() == 1, "in use again in epoch " + std::to_string(pk::epoch_in_use()) +
			", the same streamer (started once)");
		fake::ask(made, 200, 100);
		check(wait_tickets() && fake::count(st::state::ready) == 100, "asked in the new epoch: 100 ready");
	}

	// ---- A reload while the pack still opens: the first open held on the worker after its pack.idx is loaded, a second
	// asked meanwhile (done after it, raising the epoch again); then a device change while it opens
	{
		g_hold[0] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_hold[1] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_loads = 0;
		st::test::after_load(hold_load);
		command("reload");
		const bool first_held = wait_loads(1);
		command("reload");
		SetEvent(g_hold[0]);
		const bool second_held = wait_loads(2);
		const uint32_t between = st::epoch();
		update();
		update();
		const bool waited = !pk::in_use() && has(command("status"), "textures: opening");
		SetEvent(g_hold[1]);
		const bool in = wait_in_use();
		check(first_held && second_held && waited && in && pk::epoch_in_use() == st::epoch() && st::epoch() != between,
			"a reload while the first open loads: not in use once that one is done (epoch " + std::to_string(between) + "), in use in the second's (" +
				std::to_string(pk::epoch_in_use()) + ", the streamer's " + std::to_string(st::epoch()) + ")");
		fake::ask(made, 450, 50);
		check(wait_tickets() && fake::count(st::state::ready) == 50 && fake::count(st::state::gone) == 0, "  asked then: 50 ready, none gone");
		ResetEvent(g_hold[0]);
		ResetEvent(g_hold[1]);
		g_loads = 0;
		command("reload");
		const bool held = wait_loads(1);
		M->device(nullptr);
		M->device(G.device);
		SetEvent(g_hold[0]);
		const bool back = wait_in_use();
		for (int i = 0; i < 4; ++i)
		{
			Sleep(20);
			update();
		}
		check(held && back && g_loads.load() == 1 && pk::epoch_in_use() == st::epoch(), "a device change (none, then the device) while it opens: nothing asked again (" +
			std::to_string(g_loads.load()) + " opens loaded), in use in the epoch after the change (" + std::to_string(pk::epoch_in_use()) + ", the streamer's " +
			std::to_string(st::epoch()) + ")");
		SetEvent(g_hold[1]);
		st::test::after_load(nullptr);
		fake::ask(made, 500, 50);
		check(wait_tickets() && fake::count(st::state::ready) == 50 && fake::count(st::state::gone) == 0, "  asked then: 50 ready, none gone");
		CloseHandle(g_hold[0]);
		CloseHandle(g_hold[1]);
		settle();
		messages_check("a reload and a device change while it opens");
	}

	// ---- The FSR bias's switch: on while the pack is in use with [textures] fsr_bias (on by default)
	{
		update();
		const bool on = pk::fsr_bias_on();
		hifi::game::set_option_on("textures", "fsr_bias", false);
		update();
		const bool key_off = !pk::fsr_bias_on();
		hifi::game::set_option_on("textures", "fsr_bias", true);
		update();
		check(on && key_off && pk::fsr_bias_on(), "the FSR bias's switch: on while in use, off with [textures] fsr_bias=0, on again with 1");
	}

	// ---- Off: cleared before the close, kOff, the streamer kept; on again: the same streamer
	{
		const uint32_t e0 = pk::epoch_in_use();
		// (plan §2: turned off, none of the pack's files open, so the folder can be renamed or deleted while the game runs.
		// While on, the worker's reader holds the data file it read last: the rename fails, so the check below can fail)
		const std::wstring folder = pack_folder.substr(0, pack_folder.size() - 1), moved = here + L"hifixi-pack-moved";
		auto rename_and_back = [&] {
			if (!MoveFileW(folder.c_str(), moved.c_str()))
				return false;
			MoveFileW(moved.c_str(), folder.c_str());
			return true;
		};
		const bool held_while_on = !rename_and_back();
		fake::calls.clear();
		fake::order.clear();
		command("off");
		const bool cleared_first = fake::index_of("clear") == 0 && fake::epoch_at_clear == e0 && fake::snapshot_at_clear && fake::index_of("mode 00") == 1;
		check(fake::in_turn("1 clear", "2 clear") && fake::in_turn("1 mode", "2 mode") && fake::order.size() == 4 && fake::order[0] == "1 clear" &&
				fake::order[1] == "2 clear" && pk::device_in_use() == nullptr && !fake2::chars,
			"off: both consumers cleared (the first, then the second, each once: the first wired twice), then both modes off; no device in use");
		const bool closed = wait_epoch_past(e0) && st::current() == nullptr;
		update();
		check(cleared_first && closed && !pk::in_use() && pk::epoch_in_use() == 0 && (st::paused() & st::kOff) != 0 && st::running() &&
			!hifi::game::option_on("textures", "pack", true) && has(pk::menu_lines(), "Off: turn it on above.") && !pk::fsr_bias_on(),
			"off: the consumer cleared and the loaders out of pack mode first, then the pack closed (nothing open), kOff, the streamer kept, saved; no FSR bias");
		// (the worker closes the old reader's files just after it marks the close done: a moment's grace)
		bool renamed = false;
		for (int i = 0; i < 200 && !renamed; ++i)
			if (!(renamed = rename_and_back()))
				Sleep(5);
		check(held_while_on && renamed, std::string("off: the pack folder can be renamed (while on, a data file held: ") + (held_while_on ? "yes" : "no") +
			"; after off: " + (renamed ? "renamed and back" : "still held") + ")");
		command("on");
		check(wait_in_use() && pk::test::stream_starts() == 1 && (st::paused() & st::kOff) == 0, "on again: the same streamer, kOff let go");
		messages_check("on, off, compare, reload");
	}

	// ---- A device change: none, then the device again
	{
		const uint32_t e0 = pk::epoch_in_use();
		fake::ask(made, 300, 50);
		wait_tickets();
		fake::calls.clear();
		M->device(nullptr);
		const bool cleared = fake::index_of("clear") == 0;
		update();
		const bool off = !pk::in_use();
		M->device(G.device);
		const bool back = wait_in_use();
		check(cleared && off && back && pk::epoch_in_use() > e0 && pk::test::stream_starts() == 1,
			"the device gone: cleared at once, off; back: in use again in a new epoch, the same streamer");
		fake::ask(made, 350, 50);
		check(wait_tickets() && fake::count(st::state::ready) == 50, "asked on the device again: 50 ready");
		settle();
		messages_check("a device change");
	}

	// ---- The plugin's unload with 1,000 queued and four copies in flight (a gate opened 5 ms into stop)
	{
		ID3D12Fence *gate = nullptr;
		G.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate));
		// (the textures asked so far let go first: the 1,000 below all queued, none resident already)
		fake::clear_all();
		settle();
		st::test::gate_copies(gate, 1);
		fake::ask(made, made.size() - 13, 12, 10); // (the 2048x1024s first)
		fake::ask(made, 0, 1000);
		const bool held = wait_held(st::kInFlight);
		const st::counts c0 = st::get_counts();
		std::thread opener([&] {
			Sleep(5);
			gate->Signal(1);
		});
		fake::calls.clear();
		const double t0 = now_ms();
		std::string why;
		const bool stopped = M->stop(why);
		const double stop_ms = now_ms() - t0;
		M->device(nullptr);
		opener.join();
		st::test::gate_copies(nullptr, 0);
		// (in the game the d3d8.dll's open list may hold what was let go since the last present, and release_all leaves it;
		// here the queue is idle: presents first, so the references can be counted)
		settle();
		hifi::retire::release_all();
		const st::counts c1 = st::get_counts();
		char line[300];
		std::snprintf(line, sizeof(line), "unload with %llu queued and %llu copies in flight: the consumer cleared first, stop in %.1f ms (under 200, memwatch's join too); %llu objects left",
			static_cast<unsigned long long>(c0.queued), static_cast<unsigned long long>(c0.in_flight), stop_ms, static_cast<unsigned long long>(c1.leaked));
		check(held && c0.queued >= 1000 && stopped && fake::index_of("clear") == 0 && !st::running() && stop_ms < 200.0 && c1.leaked == 0 && !pk::in_use() &&
			!pk::folder_exists() && !pk::fsr_bias_on(), line);
		gate->Release();
		const ULONG refs = device_refs();
		check(refs == refs_before, "everything the streamer made on the device let go: the device's references back to " + std::to_string(refs_before) + " (" +
			std::to_string(refs) + ")");
		messages_check("the unload");
	}

	// ---- Loaded again (the DLL pinned: its statics as left): a second start, in use; unloaded again
	{
		M->device(G.device);
		const bool in = wait_in_use();
		check(in && pk::test::stream_starts() == 2 && pk::folder_exists() && st::running(), "loaded again: the folder found, the streamer started again (once more), in use");
		fake::ask(made, 400, 100);
		check(wait_tickets() && fake::count(st::state::ready) == 100, "100 ready");
		std::string why;
		M->stop(why);
		M->device(nullptr);
		settle();
		hifi::retire::release_all();
		check(st::get_counts().leaked == 0 && device_refs() == refs_before, "unloaded again: nothing left, the device's references back");
	}

	// ---- The menu's calls make no file, DXGI or streamer call; the worst present
	{
		M->device(G.device);
		wait_in_use();
		const uint64_t io0 = pk::test::io_calls();
		for (int i = 0; i < 100; ++i)
		{
			pk::folder_exists();
			pk::menu_lines();
			pk::folder_text();
			pk::in_use();
			pk::quiet();
			pk::comparing();
		}
		check(pk::test::io_calls() == io0 && io0 > 0, "folder_exists, menu_lines, folder_text and the owners' reads: no file, DXGI or streamer call (" +
			std::to_string(io0) + " by the module's own steps)");
		std::string why;
		M->stop(why);
		M->device(nullptr);
		settle();
		hifi::retire::release_all();
		char line[120];
		std::snprintf(line, sizeof(line), "the worst present: %.2f ms (the module's own, the consumer's frame a stub)", g_worst_present);
		check(g_worst_present < 50.0, line);
		messages_check("the end");
	}

	// ---- Last: the device removed, as gpu_crash's watch sees it (the module's follow_lost at the next present)
	{
		M->device(G.device);
		const bool in = wait_in_use();
		fake::ask(made, 600, 50);
		wait_tickets();
		gpu_crash::watch(G.device, here + L"gpu-crash.txt", nullptr, nullptr);
		ID3D12Device5 *d5 = nullptr;
		G.device->QueryInterface(IID_PPV_ARGS(&d5));
		fake::calls.clear();
		const int frames0 = fake::frames;
		if (d5 != nullptr)
			d5->RemoveDevice();
		const double t0 = now_ms();
		while (!gpu_crash::lost() && now_ms() - t0 < 10000)
			Sleep(1);
		const bool lost = gpu_crash::lost();
		present();
		const bool cleared = fake::index_of("clear") == 0 && fake::index_of("mode 00") == 1 && fake::owner == 0 && !fake::zones && !fake::actors;
		for (int i = 0; i < 10; ++i)
			present();
		update();
		command("on");
		update();
		check(d5 != nullptr && in && lost && cleared && !pk::in_use() && pk::epoch_in_use() == 0 && fake::frames == frames0 &&
			has(command("status"), "the graphics card stopped") && has(pk::menu_lines(), "Stopped: the graphics card stopped") &&
			logged("texture pack: stopped, the graphics card stopped"),
			"the device removed: the consumer cleared first, the loaders out of pack mode, not in use, its frame not called again; the status, the menu "
			"and the log say so; /hifi textures on changes nothing");
		std::string why;
		M->stop(why);
		gpu_crash::unwatch();
		if (d5 != nullptr)
			d5->Release();
	}

	std::printf(failures == 0 ? "\nall %d passed\n" : "\n%d failed\n", failures == 0 ? passes : failures);
	return failures == 0 ? 0 : 1;
}
