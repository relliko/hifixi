/*
 * The texture pack's streamer (src/pack/stream.hpp; docs/texture-pack/streamer.md), offline, on WARP or the GPU
 * (ZONE_GPU=1), with the debug layer always on and GPU-based validation with HIFIXI_GBV=1; every part counts the debug
 * layer's messages and wants none. A made-up pack is written under build\test\packstream (BC2, BC7 and BC4 records,
 * 4x4 to 4096x4096); the real pack (HIFIXI_PACK, or C:\Games\PhoenixXI\bootloader\hifixi-pack) is read when it's
 * there, never written. A direct queue stands in for the game's: retire::on_present is called with it.
 *   - the ring's heap: CUSTOM, CPU write-back, L0 (an UPLOAD ring is write-combined on the GPU: WARP can't show it);
 *   - the made-up pack's 2,000 entries streamed and read back: each texture copied on the direct queue into a readback
 *     buffer (promotion to COPY_SOURCE, no barrier) and compared row by row with the record's bytes; on WARP in waves
 *     of at most 256 MB resident; BC2 ones sampled by a compute shader through views write_view made in the test's own
 *     heap; and 2,000 random entries of the real pack the same way (skipped without it);
 *   - an 8 MB ring with mixed sizes (a 5.3 MB record among them): copies held on the GPU by a gate fence
 *     (stream::test::gate_copies) until the worker waits for ring space behind them: none reads ready, then the data
 *     right (ring bytes a held copy reads never written); wraps counted; a 4096x4096 BC2 (22 MB) missed "bigger than
 *     the upload ring", the same from level 2 ready; a first level that isn't a multiple of 4 refused;
 *   - take_done hands a ticket over once across an epoch change (a miss keeps its reason), and keeps nothing for an
 *     owner that only polls;
 *   - set_device and a request of the new epoch between the worker's loop top and its pick (test::before_pick): the
 *     ticket comes, on the new parts; set_device and open both waiting at the loop's top: the open done after the change
 *     (not dropped with it);
 *   - two owners of one key share one texture (its bytes counted as the sharing's saving); it goes to the retire list
 *     when the second drops it;
 *   - gone tickets keep their texture (docs/texture-pack/zones.md decision 1): a reload or a close turns a ready ticket
 *     gone and holds its texture (counted held) till it's dropped, then the retire list lets it go behind the fence; a
 *     device change lets go of the held ones at once;
 *   - want::down_to: a record down to half its size 2 levels, a quarter 3, its own size 1;
 *   - failures at the worker: a level CRC that doesn't match and a data file that can't be opened: missed "a CRC
 *     mismatch" and "a read failed", the texture made for each released at once, a good one between them ready, one
 *     take_log line each with the reader's why, handed over once;
 *   - evict and stream again (a character's key): dropped while a direct-queue list reading it runs, asked again: a
 *     new texture, the old one let go only behind the retire fence (+2), the list's reads right;
 *   - pause and resume: nothing read while paused, everything ready after; the per-present cap: bytes started in one
 *     present at most the cap, a record bigger than it alone;
 *   - a forced low budget: over 85% the texture comes without its top level, over 95% it stays queued (pressure()),
 *     then comes once the budget is raised; the address-space floor the same way; eight asked in one window of a
 *     lagging usage sample (what was made since counts); pressure() the level at each set_limits;
 *   - drop then destroy: 500 requests, the owner dropped mid-stream and its heap released at once; in_flight 0, every
 *     ticket dropped, everything the worker made released or handed to the retire list;
 *   - crash_section() from another thread while the test holds the streamer's mutex: within 10 ms, with its lines;
 *   - stop() under 100 ms with a read stalled on a named pipe, with 1,000 tickets queued (then retire::release_all:
 *     the plugin's unload, M4's test with the plugin), with four copies in flight (held, the gate opened 5 ms in:
 *     nothing left), and while an open reads a slow pack.idx; the pack opened again with a read stalled on a pipe:
 *     the ticket reads gone with the new epoch, not missed as a failed read;
 *   - device loss, in a child process each ("lost", "lost-read", "lost-stop", "lost-switch": D3D12 hands out one device
 *     per adapter and process, so RemoveDevice would take the test's own): copies in flight, then a read stalled on a
 *     pipe, then copies held and stop() or set_device(nullptr) at once; tickets missed, nothing waits, stop() under
 *     100 ms, what a copy may still use left, not released;
 *   - the GPU only (ZONE_GPU=1), in a child process with no debug layer ("va": its own bookkeeping of every object
 *     doubles the figure): address space per 1,000 textures at most 16 MB, the streamer's whole address space at most
 *     ring + index + 5.2 KB a texture, rounded up to 8 MB, and stopped and started again three times at most one ring's
 *     more, then no more (WARP holds textures in the process: skipped);
 *   - PACKSTREAM_SOAK=<minutes> (by hand, on the GPU; a child process "soak" with no debug layer): random requests,
 *     drops, owners, budget squeezes and reloads for that long; everything made let go at the end; the address space
 *     sampled every 30 s, flat within 8 MB after the first two minutes.
 *
 *   build\packstream_test.exe         (built by build.cmd; run_suites runs it on WARP and as packstream_gpu)
 *   build\packstream_test.exe soak    (the soak alone, its lines as they come; PACKSTREAM_SOAK=<minutes>, default 10)
 *
 * Exit code: the number of failures.
 */

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include "../src/host/retire.hpp"
#include "../src/pack/bc2.hpp"
#include "../src/pack/memwatch.hpp"
#include "../src/pack/pack.hpp"
#include "../src/pack/pack_write.hpp"
#include "../src/pack/stream.hpp"
#include "../src/pack/stream_crumbs.hpp"
#include "../src/pack/validate.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
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

	constexpr uint64_t kMB = 1048576;
	std::string mb(uint64_t bytes)
	{
		char s[32];
		std::snprintf(s, sizeof(s), "%.1f MB", double(bytes) / double(kMB));
		return s;
	}

	double now_ms()
	{
		LARGE_INTEGER t, f;
		QueryPerformanceCounter(&t);
		QueryPerformanceFrequency(&f);
		return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
	}

	std::wstring g_build; // build\ (with its slash)
	std::wstring g_exe;
	bool g_on_gpu = false;

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

	uint64_t avail_virtual()
	{
		MEMORYSTATUSEX m = {};
		m.dwLength = sizeof(m);
		GlobalMemoryStatusEx(&m);
		return m.ullAvailVirtual;
	}

	// ---- The device, the stand-in for the game's queue

	struct gpu_t
	{
		ID3D12Device *device = nullptr;
		ID3D12InfoQueue *info = nullptr;
		ID3D12CommandQueue *direct = nullptr;
		ID3D12CommandAllocator *allocator = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
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
		G.direct->SetName(L"packstream_test: the game's queue");
		G.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&G.allocator));
		G.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, G.allocator, nullptr, IID_PPV_ARGS(&G.list));
		G.list->Close();
		G.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&G.fence));
		G.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return G.direct != nullptr && G.list != nullptr && G.fence != nullptr;
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

	// A "present" of the game's: the retire list's signal on the game's queue
	void frame() { hifi::retire::on_present(G.direct); }
	// The GPU idle and enough presents for the retire list to let go of everything handed over so far
	void settle()
	{
		for (int i = 0; i < 4; ++i)
		{
			frame();
			idle();
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

	// ---- Packs

	uint64_t mix(uint64_t x)
	{
		x += 0x9E3779B97F4A7C15ull;
		x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
		x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
		return x ^ (x >> 31);
	}

	struct made_entry
	{
		pk::key k;
		uint32_t format = 0, w = 0, h = 0, levels = 0;
		std::string tag;
	};

	// The made-up pack: 2,000 entries of mixed sizes and formats, and the special ones by tag
	bool make_synthetic(const std::wstring &folder, std::vector<made_entry> &out)
	{
		remove_tree(folder);
		pk::writer w;
		pk::writer_options o;
		o.folder = folder;
		o.source = "packstream_test";
		std::string why;
		if (!w.open(o, why))
		{
			note("the made-up pack couldn't be opened: " + why);
			return false;
		}
		std::mt19937 rng(20261102);
		const uint32_t formats[3] = { pk::kBC2, pk::kBC7, pk::kBC4 };
		const uint32_t sides[8] = { 4, 8, 16, 32, 64, 128, 256, 512 };
		const int weight[8] = { 5, 5, 10, 15, 20, 20, 15, 10 };
		auto add = [&](uint32_t format, uint32_t mw, uint32_t mh, uint32_t levels, const std::string &tag, uint16_t flags = 0) {
			const uint32_t i = uint32_t(out.size());
			pk::entry e;
			e.hash = mix(i + 1);
			e.bytes = 64 + i;
			e.w = std::max(1u, mw / 2);
			e.h = std::max(1u, mh / 2);
			e.orig_format = 0xA1;
			e.alpha_kind = pk::kAlphaBlended;
			e.source_kind = i % 2 ? pk::kSourceZone : pk::kSourceModel;
			e.flags = flags;
			std::snprintf(e.name, sizeof(e.name), "ps%013u", i);
			pk::map_in m;
			m.format = format;
			m.w = mw;
			m.h = mh;
			const uint32_t block = pk::block_bytes(format);
			for (uint32_t l = 0; l < levels; ++l)
			{
				const uint32_t lw = std::max(1u, mw >> l), lh = std::max(1u, mh >> l);
				std::vector<uint8_t> b(size_t((lw + 3) / 4) * ((lh + 3) / 4) * block);
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
			out.push_back(made_entry { e.the_key(), format, mw, mh, levels, tag });
			return true;
		};
		bool ok = true;
		for (int i = 0; i < 2000 && ok; ++i)
		{
			auto side = [&]() {
				int r = int(rng() % 100), k = 0;
				while (r >= weight[k])
					r -= weight[k++];
				return sides[k];
			};
			const uint32_t mw = side(), mh = std::clamp(side(), mw / 8, mw * 8);
			const uint32_t full = pk::full_levels(mw, mh);
			const uint32_t levels = i % 5 == 0 ? 1 + rng() % full : full;
			ok = add(formats[i % 3], mw, std::max(4u, mh), std::min(levels, pk::full_levels(mw, std::max(4u, mh))), "");
		}
		ok = ok && add(pk::kBC2, 2048, 2048, pk::full_levels(2048, 2048), "big2048");
		ok = ok && add(pk::kBC2, 4096, 4096, pk::full_levels(4096, 4096), "big4096");
		ok = ok && add(pk::kBC7, 4096, 4096, 1, "big4096bc7");
		ok = ok && add(pk::kBC2, 12, 12, pk::full_levels(12, 12), "small12");
		ok = ok && add(pk::kBC2, 64, 64, 7, "nopack", pk::kNoPack);
		ok = ok && add(pk::kBC2, 1024, 1024, pk::full_levels(1024, 1024), "budget1");
		ok = ok && add(pk::kBC2, 1024, 1024, pk::full_levels(1024, 1024), "budget2");
		ok = ok && add(pk::kBC2, 512, 256, pk::full_levels(512, 256), "char");
		ok = ok && add(pk::kBC2, 256, 256, pk::full_levels(256, 256), "shared");
		for (int i = 0; i < 12 && ok; ++i)
			ok = add(pk::kBC2, 2048, 1024, pk::full_levels(2048, 1024), "lost");
		for (int i = 0; i < 24 && ok; ++i)
			ok = add(pk::kBC2, i % 3 == 0 ? 1024u : 512u, 1024, i % 3 == 0 ? 11u : 10u, "wrap");
		if (!ok || !w.commit(why))
		{
			note("the made-up pack couldn't be written: " + why);
			return false;
		}
		w.close();
		return true;
	}

	const made_entry *tagged(const std::vector<made_entry> &v, const char *tag, int nth = 0)
	{
		for (const made_entry &e : v)
			if (e.tag == tag && nth-- == 0)
				return &e;
		return nullptr;
	}

	// Limits with no cap a present (once the test has called on_present, the default 64 MB would hold a part that doesn't
	// present back)
	st::limits nocap()
	{
		st::limits l;
		l.per_present = 0;
		return l;
	}

	st::want want_of(const pk::key &k, uint8_t first = 0, uint8_t count = 0, int32_t priority = 0)
	{
		st::want w;
		w.k = k;
		w.first = first;
		w.count = count;
		w.priority = priority;
		return w;
	}

	// Polls until no ticket is queued, reading or copying (or the time is up); the counts of each state
	struct tally
	{
		int ready = 0, missed = 0, waiting = 0, dropped = 0, gone = 0, other = 0;
	};
	tally count_states(const std::vector<st::ticket> &ts)
	{
		tally t;
		for (const st::ticket &k : ts)
			switch (st::poll(k))
			{
			case st::state::ready:
				t.ready++;
				break;
			case st::state::missed:
				t.missed++;
				break;
			case st::state::queued:
			case st::state::reading:
			case st::state::copying:
				t.waiting++;
				break;
			case st::state::dropped:
				t.dropped++;
				break;
			case st::state::gone:
				t.gone++;
				break;
			default:
				t.other++;
			}
		return t;
	}
	tally wait_all(const std::vector<st::ticket> &ts, double timeout_ms = 60000)
	{
		const double t0 = now_ms();
		tally t;
		for (;;)
		{
			t = count_states(ts);
			if (t.waiting == 0 || now_ms() - t0 > timeout_ms)
				return t;
			Sleep(1);
		}
	}
	bool wait_for(st::ticket t, st::state s, double timeout_ms = 30000)
	{
		const double t0 = now_ms();
		while (st::poll(t) != s)
		{
			if (now_ms() - t0 > timeout_ms)
				return false;
			Sleep(1);
		}
		return true;
	}
	// The worker done with everything asked: nothing queued or in flight
	bool wait_worker(double timeout_ms = 30000)
	{
		const double t0 = now_ms();
		for (;;)
		{
			const st::counts c = st::get_counts();
			if (c.queued == 0 && c.in_flight == 0 && hifi::pack::stream_crumbs::the.state.load() != hifi::pack::stream_crumbs::kReading &&
				hifi::pack::stream_crumbs::the.state.load() != hifi::pack::stream_crumbs::kMaking)
				return true;
			if (now_ms() - t0 > timeout_ms)
				return false;
			Sleep(2);
		}
	}
	bool open_and_wait(std::shared_ptr<const pk::index> idx, const std::wstring &folder = L"")
	{
		const auto before = st::current();
		const uint32_t e = st::epoch();
		if (idx)
			st::open(idx);
		else
			st::open(folder);
		const double t0 = now_ms();
		while (st::epoch() == e && now_ms() - t0 < 30000)
			Sleep(1);
		return st::current() != nullptr && st::current() != before;
	}
	bool wait_parts()
	{
		const double t0 = now_ms();
		while (now_ms() - t0 < 30000)
		{
			const st::counts c = st::get_counts();
			if (c.ring_bytes != 0 || !c.failure.empty())
				return c.ring_bytes != 0;
			Sleep(1);
		}
		return false;
	}
	// Until the worker has `n` copies in flight and idles waiting for their fence (held back by a gate), or the time is up
	bool wait_held(uint32_t n, double timeout_ms = 10000)
	{
		namespace sc = hifi::pack::stream_crumbs;
		const double t0 = now_ms();
		while (now_ms() - t0 < timeout_ms)
		{
			if (sc::the.in_flight.load() >= n && sc::the.state.load() == sc::kFenceWait)
				return true;
			Sleep(1);
		}
		return false;
	}
	ID3D12Fence *make_gate()
	{
		ID3D12Fence *f = nullptr;
		G.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
		if (f != nullptr)
			f->SetName(L"packstream_test: the gate");
		return f;
	}
	// A texture's allocation as the streamer sizes it (GetResourceAllocationInfo)
	uint64_t alloc_of(uint32_t w, uint32_t h, uint32_t levels, DXGI_FORMAT format = DXGI_FORMAT_BC2_UNORM)
	{
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		d.Width = w;
		d.Height = h;
		d.DepthOrArraySize = 1;
		d.MipLevels = uint16_t(levels);
		d.Format = format;
		d.SampleDesc.Count = 1;
		return G.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
	}

	// ---- The race of a device change between the worker's loop top and its pick (test::before_pick): set_device and a
	// request made on the worker right there, once
	std::atomic<bool> g_race_armed { false }, g_race_done { false };
	st::owner g_race_owner = 0;
	pk::key g_race_key;
	st::ticket g_race_ticket;
	void race_hook()
	{
		if (!g_race_armed.exchange(false))
			return;
		st::set_device(G.device);
		g_race_ticket = st::request(g_race_owner, want_of(g_race_key));
		g_race_done = true;
	}

	// ---- A device change and an open both waiting at the worker's loop top (made on the worker, test::before_pick): the
	// change is done first, the open on the next pass (it used to be taken with the change and dropped: M4's module saw
	// it on the 4070 Ti, set_device then open in one update, the pack never opened)
	std::atomic<bool> g_both_armed { false }, g_both_done { false };
	std::wstring g_both_folder;
	void both_hook()
	{
		if (!g_both_armed.exchange(false))
			return;
		st::set_device(G.device);
		st::open(g_both_folder);
		g_both_done = true;
	}

	// ---- A slow pack.idx (pk::load_test_window): each piece takes 40 ms
	HANDLE g_load_entered = nullptr;
	std::atomic<int> g_load_pieces { 0 };
	void slow_piece()
	{
		g_load_pieces++;
		SetEvent(g_load_entered);
		Sleep(40);
	}

	// ---- Reading textures back

	struct readback
	{
		ID3D12Resource *buffer = nullptr;
		uint64_t size = 0;
		bool make(uint64_t bytes)
		{
			D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Width = bytes;
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.SampleDesc.Count = 1;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			size = bytes;
			return SUCCEEDED(G.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)));
		}
		~readback()
		{
			if (buffer != nullptr)
				buffer->Release();
		}
	};

	struct item
	{
		st::ticket t;
		st::view v;
		pk::record rec;
	};

	D3D12_RESOURCE_DESC desc_of(const st::view &v)
	{
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		d.Width = v.w;
		d.Height = v.h;
		d.DepthOrArraySize = 1;
		d.MipLevels = uint16_t(v.levels);
		d.Format = v.format;
		d.SampleDesc.Count = 1;
		return d;
	}

	// Every item's texture copied back on the direct queue (no barrier: COMMON promoted to COPY_SOURCE) and compared
	// row by row with its record's bytes as the test's own reader reads them. Returns how many matched
	int read_back(std::vector<item> &items, pk::reader &own, readback &rb, std::string &bad)
	{
		int matched = 0;
		size_t at = 0;
		while (at < items.size())
		{
			// A batch that fits the readback buffer
			struct placed
			{
				size_t i;
				D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[pk::kMaxLevels];
				UINT rows[pk::kMaxLevels];
				UINT64 row_bytes[pk::kMaxLevels];
			};
			std::vector<placed> batch;
			uint64_t used = 0;
			G.allocator->Reset();
			G.list->Reset(G.allocator, nullptr);
			for (; at < items.size(); ++at)
			{
				const D3D12_RESOURCE_DESC d = desc_of(items[at].v);
				placed p = {};
				p.i = at;
				UINT64 total = 0;
				G.device->GetCopyableFootprints(&d, 0, items[at].v.levels, used, p.fp, p.rows, p.row_bytes, &total);
				if (used + total > rb.size)
				{
					if (batch.empty())
					{
						bad = "a texture bigger than the readback buffer";
						return matched;
					}
					break;
				}
				for (uint32_t l = 0; l < items[at].v.levels; ++l)
				{
					D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
					dst.pResource = rb.buffer;
					dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					dst.PlacedFootprint = p.fp[l];
					src.pResource = items[at].v.resource;
					src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					src.SubresourceIndex = l;
					G.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				}
				used = (used + total + 511) / 512 * 512;
				batch.push_back(p);
			}
			G.list->Close();
			ID3D12CommandList *lists[] = { G.list };
			G.direct->ExecuteCommandLists(1, lists);
			idle();
			uint8_t *mapped = nullptr;
			D3D12_RANGE whole = { 0, size_t(used) };
			if (FAILED(rb.buffer->Map(0, &whole, reinterpret_cast<void **>(&mapped))))
			{
				bad = "the readback buffer couldn't be mapped";
				return matched;
			}
			for (const placed &p : batch)
			{
				const item &it = items[p.i];
				pk::layout l;
				pk::footprint(it.rec.format, it.rec.w, it.rec.h, it.rec.levels, l);
				std::vector<uint8_t> want(size_t(pk::reader::levels_span(it.rec, it.v.first, it.v.levels)));
				std::string why;
				if (!own.read_levels(it.rec, it.v.first, it.v.levels, want.data(), why))
				{
					bad = "the test's own read failed: " + why;
					continue;
				}
				bool same = true;
				for (uint32_t i = 0; same && i < it.v.levels; ++i)
				{
					const pk::level_layout &lv = l.level[it.v.first + i];
					const uint64_t rel = lv.offset - l.level[it.v.first].offset;
					same = p.rows[i] == lv.rows && p.row_bytes[i] == lv.row_bytes;
					for (uint32_t r = 0; same && r < lv.rows; ++r)
						same = std::memcmp(mapped + p.fp[i].Offset + uint64_t(r) * p.fp[i].Footprint.RowPitch, want.data() + rel + uint64_t(r) * lv.row_pitch, lv.row_bytes) == 0;
				}
				if (same)
					matched++;
				else if (bad.empty())
				{
					char s[160];
					std::snprintf(s, sizeof(s), "%ux%u format %u levels %u from %u differs", it.v.w, it.v.h, uint32_t(it.v.format), it.v.levels, it.v.first);
					bad = s;
				}
			}
			D3D12_RANGE none = { 0, 0 };
			rb.buffer->Unmap(0, &none);
		}
		return matched;
	}

	// ---- Sampling through write_view's views (a compute shader: Load at 64 places of level 0)

	const char *kSample = R"(
Texture2D<float4> t : register(t0);
RWByteAddressBuffer o : register(u0);
cbuffer c : register(b0) { uint w; uint h; uint base; uint pad; };
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint x = (id.x * 37u + 3u) % w, y = (id.x * 11u + id.x / 7u) % h;
	uint4 u = uint4(round(saturate(t.Load(int3(x, y, 0))) * 255.0));
	o.Store((base + id.x) * 4u, u.r | (u.g << 8) | (u.b << 16) | (u.a << 24));
}
)";

	struct sampler
	{
		ID3D12RootSignature *root = nullptr;
		ID3D12PipelineState *pso = nullptr;
		ID3D12DescriptorHeap *heap = nullptr;
		ID3D12Resource *out = nullptr;
		readback rb;
		uint32_t slots = 0;
		UINT step = 0;
		bool make(uint32_t n)
		{
			ID3DBlob *code = nullptr, *errors = nullptr;
			if (FAILED(D3DCompile(kSample, std::strlen(kSample), "sample", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &errors)))
			{
				if (errors != nullptr)
					note(static_cast<const char *>(errors->GetBufferPointer()));
				return false;
			}
			D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
			D3D12_ROOT_PARAMETER params[3] = {};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[0].DescriptorTable = { 1, &range };
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
			params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[2].Constants.Num32BitValues = 4;
			D3D12_ROOT_SIGNATURE_DESC rd = { 3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
			ID3DBlob *blob = nullptr;
			bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) &&
				SUCCEEDED(G.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
			if (blob != nullptr)
				blob->Release();
			D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
			pd.pRootSignature = root;
			pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
			ok = ok && SUCCEEDED(G.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
			code->Release();
			D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, n, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			ok = ok && SUCCEEDED(G.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
			step = G.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			slots = n;
			D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = uint64_t(n) * 64 * 4;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			ok = ok && SUCCEEDED(G.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)));
			return ok && rb.make(bd.Width);
		}
		D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint32_t i) const { return { heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * step }; }
		D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint32_t i) const { return { heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(i) * step }; }
		// The dispatches for slots 0..n-1 (textures w x h each) recorded on the list (open), out moved to COPY_SOURCE and
		// copied to the readback buffer
		void record(const std::vector<std::pair<uint32_t, uint32_t>> &sizes)
		{
			G.list->SetDescriptorHeaps(1, &heap);
			G.list->SetComputeRootSignature(root);
			G.list->SetPipelineState(pso);
			G.list->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
			for (uint32_t i = 0; i < sizes.size(); ++i)
			{
				const uint32_t c[4] = { sizes[i].first, sizes[i].second, i * 64, 0 };
				G.list->SetComputeRootDescriptorTable(0, gpu(i));
				G.list->SetComputeRoot32BitConstants(2, 4, c, 0);
				G.list->Dispatch(1, 1, 1);
			}
			// (the test's own buffer: promoted to UNORDERED_ACCESS by the dispatches, moved for the copy)
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = out;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			G.list->ResourceBarrier(1, &b);
			G.list->CopyBufferRegion(rb.buffer, 0, out, 0, uint64_t(sizes.size()) * 64 * 4);
		}
		// What sample i of slot s read (RGBA8)
		uint32_t result(uint32_t s, uint32_t i)
		{
			uint32_t *p = nullptr;
			D3D12_RANGE r = { 0, size_t(rb.size) };
			rb.buffer->Map(0, &r, reinterpret_cast<void **>(&p));
			const uint32_t v = p[s * 64 + i];
			D3D12_RANGE none = { 0, 0 };
			rb.buffer->Unmap(0, &none);
			return v;
		}
		~sampler()
		{
			for (IUnknown *p : { static_cast<IUnknown *>(root), static_cast<IUnknown *>(pso), static_cast<IUnknown *>(heap), static_cast<IUnknown *>(out) })
				if (p != nullptr)
					p->Release();
		}
	};

	// Whether the shader's 64 samples of a BC2 texture's level 0 are the CPU's decode (colour within 8: decoders may
	// round the 2/3 mixes differently; alpha exact)
	bool samples_match(sampler &s, uint32_t slot, const item &it, pk::reader &own)
	{
		pk::layout l;
		pk::footprint(it.rec.format, it.rec.w, it.rec.h, it.rec.levels, l);
		const pk::level_layout &lv = l.level[it.v.first];
		std::vector<uint8_t> span(size_t(pk::reader::levels_span(it.rec, it.v.first, 1)));
		std::string why;
		if (!own.read_levels(it.rec, it.v.first, 1, span.data(), why))
			return false;
		std::vector<uint8_t> tight(size_t(lv.rows) * lv.row_bytes);
		for (uint32_t r = 0; r < lv.rows; ++r)
			std::memcpy(tight.data() + size_t(r) * lv.row_bytes, span.data() + size_t(r) * lv.row_pitch, lv.row_bytes);
		const std::vector<uint8_t> px = pk::bc2::decode_bc2(tight.data(), lv.w, lv.h);
		for (uint32_t i = 0; i < 64; ++i)
		{
			const uint32_t x = (i * 37 + 3) % lv.w, y = (i * 11 + i / 7) % lv.h;
			const uint8_t *c = px.data() + (size_t(y) * lv.w + x) * 4;
			const uint32_t got = s.result(slot, i);
			for (int k = 0; k < 4; ++k)
			{
				const int g = int((got >> (8 * k)) & 255), want = c[k];
				if (k == 3 ? g != want : std::abs(g - want) > 8)
					return false;
			}
		}
		return true;
	}

	// ---- A pipe that never answers (stall.cpp's trick): an index whose folder is \\.\pipe\<name>\, one BC2 record

	struct stall_pipe
	{
		std::wstring folder;
		HANDLE pipe = INVALID_HANDLE_VALUE;
		std::thread server;
		std::atomic<bool> connected { false };
		std::shared_ptr<pk::index> idx;
		pk::key k;
		bool make(const wchar_t *name)
		{
			wchar_t f[128];
			std::swprintf(f, 128, L"\\\\.\\pipe\\%s-%lu\\", name, GetCurrentProcessId());
			folder = f;
			idx = std::make_shared<pk::index>();
			idx->head.file_count = 1;
			idx->head.entry_count = 1;
			idx->head.crc_count = 1;
			idx->file_bytes = { 1ull << 30 };
			idx->folder = folder;
			pk::entry e;
			e.hash = 0x5747A11ull;
			e.bytes = 4096;
			e.w = 512;
			e.h = 512;
			pk::record &r = e.maps[0];
			r.kind = pk::kColour;
			r.levels = 1;
			r.w = r.h = 1024;
			r.format = pk::kBC2;
			pk::layout l;
			pk::footprint(r.format, r.w, r.h, 1, l);
			r.bytes = uint32_t(l.total);
			idx->entries = { e };
			idx->level_crcs = { 0 };
			k = e.the_key();
			pipe = CreateNamedPipeW(pk::data_path(folder, 0).c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1 << 20, 0, 0, nullptr);
			if (pipe == INVALID_HANDLE_VALUE)
				return false;
			server = std::thread([this] {
				ConnectNamedPipe(pipe, nullptr); // (the reader's open; then nothing is ever written)
				connected = true;
			});
			return true;
		}
		~stall_pipe()
		{
			if (pipe != INVALID_HANDLE_VALUE)
			{
				// (a server still waiting for its client: a client opens it so ConnectNamedPipe returns)
				if (!connected)
				{
					HANDLE c = CreateFileW(pk::data_path(folder, 0).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
					if (c != INVALID_HANDLE_VALUE)
						CloseHandle(c);
				}
				if (server.joinable())
					server.join();
				CloseHandle(pipe);
			}
		}
	};

	// ---- The device-loss parts, each in a child process (one WARP device a process)

	int child_lost(const std::wstring &synthetic, const std::vector<made_entry> &entries)
	{
		using namespace hifi::pack;
		if (!make_gpu(false))
		{
			check(false, "a WARP device for the device-loss part");
			return 1;
		}
		ID3D12Device5 *d5 = nullptr;
		G.device->QueryInterface(IID_PPV_ARGS(&d5));
		check(d5 != nullptr, "ID3D12Device5 (RemoveDevice)");
		if (d5 == nullptr)
			return failures;
		st::start(G.device, 32);
		check(wait_parts() && open_and_wait(nullptr, synthetic), "the streamer on a WARP device of its own, the made-up pack open");
		const st::owner o = st::add_owner("lost");
		std::vector<st::ticket> ts;
		for (const made_entry &e : entries)
			if (e.tag == "lost" || e.tag == "big4096" || e.tag == "big2048" || e.tag == "wrap")
				ts.push_back(st::request(o, want_of(e.k)));
		// (at the first copy in flight: WARP runs copies quickly, so the moment is caught when it comes)
		const double t0 = now_ms();
		uint32_t in_flight = 0, state = 0;
		while (now_ms() - t0 < 20000)
		{
			in_flight = stream_crumbs::the.in_flight.load();
			state = stream_crumbs::the.state.load();
			if (in_flight != 0)
				break;
		}
		d5->RemoveDevice();
		st::device_lost();
		const tally after = wait_all(ts, 2000);
		char line[300];
		std::snprintf(line, sizeof(line), "device removed with %u copies in flight (the worker %s): within 2 s no ticket waits (%d ready, %d missed, %d gone), %llu missed as lost",
			in_flight, stream_crumbs::state_text(state), after.ready, after.missed, after.gone,
			static_cast<unsigned long long>(st::get_counts().refused[size_t(st::miss::lost)]));
		check(after.waiting == 0 && after.ready == 0 && in_flight != 0 && st::get_counts().refused[size_t(st::miss::lost)] != 0, line);
		{
			Sleep(50);
			const st::counts c = st::get_counts();
			std::snprintf(line, sizeof(line), "what the worker made: %llu textures, %llu released, %llu to the retire list, %llu objects left on purpose (copies not seen done may still run)",
				static_cast<unsigned long long>(c.made), static_cast<unsigned long long>(c.released), static_cast<unsigned long long>(c.retired),
				static_cast<unsigned long long>(c.leaked));
			check(c.in_flight == 0 && c.made <= c.released + c.retired + c.leaked, line);
		}
		const st::ticket late = st::request(o, want_of(entries[0].k));
		check(st::poll(late) == st::state::missed && st::reason(late) == st::miss::lost, "a request after the loss: missed at once (the device was lost)");
		Sleep(50);
		check(stream_crumbs::the.state.load() == stream_crumbs::kLost, std::string("the worker idles: ") + stream_crumbs::state_text(stream_crumbs::the.state.load()));
		const double s0 = now_ms();
		st::stop();
		const double stop_ms = now_ms() - s0;
		std::snprintf(line, sizeof(line), "stop() after the loss in %.2f ms (under 100)", stop_ms);
		check(stop_ms < 100.0, line);
		hifi::retire::release_all();
		d5->Release();
		return failures;
	}

	// The device removed with copies held on the GPU (never seen done), then at once stop() ("lost-stop") or
	// set_device(nullptr) ("lost-switch"), before the worker has handled the loss itself: the fence reads UINT64_MAX
	// there too, and what the copies use must be left, not released (a removed WARP device runs submitted copies on)
	int child_lost_at_once(bool switching, const std::wstring &synthetic, const std::vector<made_entry> &entries)
	{
		using namespace hifi::pack;
		if (!make_gpu(false))
		{
			check(false, "a WARP device for the device-loss part");
			return 1;
		}
		ID3D12Device5 *d5 = nullptr;
		G.device->QueryInterface(IID_PPV_ARGS(&d5));
		ID3D12Fence *gate = make_gate();
		check(d5 != nullptr && gate != nullptr, "ID3D12Device5 (RemoveDevice) and a gate fence");
		if (d5 == nullptr || gate == nullptr)
			return failures;
		st::start(G.device, 32);
		check(wait_parts() && open_and_wait(nullptr, synthetic), "the streamer on a WARP device of its own, the made-up pack open");
		st::test::gate_copies(gate, 1);
		const st::owner o = st::add_owner("lost at once");
		std::vector<st::ticket> ts;
		// (the twelve 2048x1024: four held fill 11 MB of the ring, so the worker waits for their fence, not for space)
		for (const made_entry &e : entries)
			if (e.w == 2048 && e.h == 1024)
				ts.push_back(st::request(o, want_of(e.k)));
		const bool held = wait_held(st::kInFlight);
		const uint32_t in_flight = stream_crumbs::the.in_flight.load();
		const st::counts c0 = st::get_counts();
		d5->RemoveDevice();
		st::device_lost();
		if (switching)
		{
			st::set_device(nullptr);
			Sleep(300);
		}
		else
			st::stop();
		const st::counts c1 = st::get_counts();
		char line[300];
		std::snprintf(line, sizeof(line), "removed with %u copies held, then %s at once: %llu released since, %llu objects left (at least the %u textures)", in_flight,
			switching ? "set_device(nullptr)" : "stop()", static_cast<unsigned long long>(c1.released - c0.released), static_cast<unsigned long long>(c1.leaked - c0.leaked),
			in_flight);
		check(held && c1.released == c0.released && c1.leaked >= c0.leaked + in_flight, line);
		if (switching)
			st::stop();
		hifi::retire::release_all();
		d5->Release();
		return failures;
	}

	int child_lost_read()
	{
		using namespace hifi::pack;
		if (!make_gpu(false))
		{
			check(false, "a WARP device for the device-loss part");
			return 1;
		}
		ID3D12Device5 *d5 = nullptr;
		G.device->QueryInterface(IID_PPV_ARGS(&d5));
		stall_pipe sp;
		check(d5 != nullptr && sp.make(L"hxp-lost"), "ID3D12Device5 and a pipe that never answers");
		if (d5 == nullptr)
			return failures;
		st::start(G.device, 32);
		check(wait_parts() && open_and_wait(sp.idx), "the streamer on a WARP device of its own, the pipe's index open");
		const st::owner o = st::add_owner("lost");
		const st::ticket t = st::request(o, want_of(sp.k));
		const double t0 = now_ms();
		while (!sp.connected && now_ms() - t0 < 10000)
			Sleep(1);
		Sleep(100);
		const uint32_t state = stream_crumbs::the.state.load();
		check(sp.connected && state == stream_crumbs::kReading && st::poll(t) == st::state::reading,
			std::string("the read is pending on the pipe (the worker ") + stream_crumbs::state_text(state) + ", the ticket " + st::text(st::poll(t)) + ")");
		d5->RemoveDevice();
		const double l0 = now_ms();
		st::device_lost();
		const double lost_ms = now_ms() - l0;
		const bool missed = wait_for(t, st::state::missed, 1000);
		char line[200];
		// (it waits on nothing: not the worker, not a fence; 1-3 ms alone, 120 ms seen once with the suites running side by
		// side, this thread waiting for a core)
		std::snprintf(line, sizeof(line), "device_lost() returned in %.2f ms (under 100) with the read pending; the ticket missed (%s)", lost_ms, st::text(st::reason(t)));
		check(lost_ms < 100.0 && missed && st::reason(t) == st::miss::lost, line);
		const double s0 = now_ms();
		st::stop();
		const double stop_ms = now_ms() - s0;
		std::snprintf(line, sizeof(line), "stop() with the read cancelled by the loss in %.2f ms (under 100)", stop_ms);
		check(stop_ms < 100.0, line);
		hifi::retire::release_all();
		d5->Release();
		return failures;
	}

	// The address space the streamer takes on the GPU (the child "va": no debug layer, a fresh process): its parts and
	// the index (opened by folder: loaded on the worker), then 1,000 random textures resident
	int child_va(const std::wstring &folder)
	{
		if (!make_gpu(true))
		{
			check(false, "a device on the GPU");
			return 1;
		}
		const uint64_t a0 = avail_virtual();
		st::start(G.device, 32);
		const bool up = wait_parts() && open_and_wait(nullptr, folder);
		Sleep(50);
		const uint64_t a1 = avail_virtual();
		const std::shared_ptr<const pk::index> va_pack = st::current();
		if (!up || !va_pack)
		{
			check(false, "the streamer started with the pack open");
			return failures;
		}
		std::vector<pk::key> keys;
		for (const pk::entry &e : va_pack->entries)
			if (e.map(pk::kColour) != nullptr && (e.flags & pk::kNoPack) == 0)
				keys.push_back(e.the_key());
		std::shuffle(keys.begin(), keys.end(), std::mt19937(1000));
		keys.resize(std::min<size_t>(keys.size(), 1000));
		const st::owner o = st::add_owner("va");
		std::vector<st::ticket> ts;
		for (const pk::key &k : keys)
			ts.push_back(st::request(o, want_of(k)));
		const tally t = wait_all(ts);
		Sleep(50);
		const uint64_t a2 = avail_virtual();
		const st::counts c = st::get_counts();
		const uint64_t per_1000 = a1 > a2 ? a1 - a2 : 0, whole = a0 > a2 ? a0 - a2 : 0;
		const uint64_t index_bytes = sizeof(pk::header) + va_pack->file_bytes.size() * 8 + va_pack->entries.size() * sizeof(pk::entry) + va_pack->level_crcs.size() * 4;
		const uint64_t bound = (c.ring_bytes + index_bytes + c.resident_peak * 5325 + 8 * kMB - 1) / (8 * kMB) * (8 * kMB);
		char line[400];
		std::snprintf(line, sizeof(line), "address space for %d textures resident (%s of video memory): %s (at most 16 MB)", t.ready, mb(c.resident_bytes).c_str(),
			mb(per_1000).c_str());
		check(t.ready == int(keys.size()) && per_1000 <= 16 * kMB, line);
		std::snprintf(line, sizeof(line), "the streamer's whole address space %s: at most ring %s + index %s + 5.2 KB x %llu textures, rounded up to 8 MB = %s (started and opened: %s)",
			mb(whole).c_str(), mb(c.ring_bytes).c_str(), mb(index_bytes).c_str(), static_cast<unsigned long long>(c.resident_peak), mb(bound).c_str(),
			mb(a0 > a1 ? a0 - a1 : 0).c_str());
		check(whole <= bound, line);
		st::drop_owner(o);
		wait_worker();
		settle();
		const uint64_t a3 = avail_virtual();
		st::stop();
		hifi::retire::release_all();
		const uint64_t a4 = avail_virtual();
		const st::counts after = st::get_counts();
		note("all let go: " + mb(a0 > a3 ? a0 - a3 : 0) + " still taken (the driver keeps its bookkeeping's high-water); after stop " + mb(a0 > a4 ? a0 - a4 : 0) + " (" +
			std::to_string(after.leaked) + " objects left; the test still holds the index)");
		// (started and stopped again, three times: a stopped ring's address space isn't given back on the 4070 Ti, a
		// driver's high-water like its bookkeeping; the next start's ring takes another, and from then on they're used
		// again: at most one ring more than after the first stop, the same after each)
		std::vector<uint64_t> taken;
		bool again = true;
		for (int i = 0; i < 3; ++i)
		{
			st::start(G.device, 32);
			again = wait_parts() && open_and_wait(nullptr, folder) && again;
			st::stop();
			hifi::retire::release_all();
			taken.push_back(a0 > avail_virtual() ? a0 - avail_virtual() : 0);
		}
		const uint64_t first_stop = a0 > a4 ? a0 - a4 : 0;
		char line2[300];
		std::snprintf(line2, sizeof(line2), "started and stopped 3 times more: taken after each stop %s, %s, %s (after the first %s: at most one ring more, then no more)",
			mb(taken[0]).c_str(), mb(taken[1]).c_str(), mb(taken[2]).c_str(), mb(first_stop).c_str());
		check(again && after.leaked == 0 && taken[2] <= first_stop + 32 * kMB + kMB && taken[2] <= taken[1] + kMB && taken[1] <= taken[0] + kMB, line2);
		return failures;
	}

	// Runs this test again with an argument; its lines printed, its failures counted here
	void run_child(const wchar_t *arg, const char *what, DWORD wait_ms = 120000)
	{
		const std::wstring out = g_build + L"test\\packstream\\" + arg + L".txt";
		SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
		HANDLE f = CreateFileW(out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
		STARTUPINFOW si = { sizeof(si) };
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = f;
		si.hStdError = f;
		PROCESS_INFORMATION pi = {};
		std::wstring cmd = L"\"" + g_exe + L"\" " + arg;
		DWORD code = 1;
		const bool ran = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi) != 0;
		if (ran)
		{
			WaitForSingleObject(pi.hProcess, wait_ms);
			GetExitCodeProcess(pi.hProcess, &code);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
		}
		CloseHandle(f);
		std::printf("--    %s (a child process: %s)\n", what, ran ? "ran" : "couldn't start");
		FILE *in = nullptr;
		_wfopen_s(&in, out.c_str(), L"rb");
		int child_fail = 0, child_ok = 0;
		if (in != nullptr)
		{
			char line[600];
			while (std::fgets(line, sizeof(line), in))
			{
				if (std::strncmp(line, "FAIL", 4) == 0)
					child_fail++;
				if (std::strncmp(line, "ok", 2) == 0)
					child_ok++;
				std::printf("   %s", line);
			}
			std::fclose(in);
		}
		check(ran && code == 0 && child_fail == 0 && child_ok > 0, std::string(what) + ": " + std::to_string(child_ok) + " checks passed, exit " + std::to_string(code));
	}

	// ---- The soak (the child "soak": PACKSTREAM_SOAK=<minutes>, the GPU with ZONE_GPU=1, no debug layer: its own
	// bookkeeping of every object would hide the address space's own course)
	//   random requests (some from level 2, three priorities) and drops; an owner dropped whole about every half
	//   minute; the quiet bit on and off; every 20 s 3 s over 85% of a made-up budget (top levels skipped), every 2 min
	//   the pack opened again (a new epoch: every ticket gone, every texture to the retire list). The address space free
	//   sampled every 30 s; after the first two minutes (the first third of a shorter run) within 8 MB

	int child_soak(int minutes, const std::wstring &folder)
	{
		if (!make_gpu(GetEnvironmentVariableA("ZONE_GPU", nullptr, 0) != 0))
		{
			check(false, "a device");
			return 1;
		}
		st::start(G.device, 32);
		if (!wait_parts() || !open_and_wait(nullptr, folder))
		{
			check(false, "the streamer started with the pack open");
			return failures;
		}
		std::shared_ptr<const pk::index> idx = st::current();
		std::vector<pk::key> keys;
		for (const pk::entry &e : idx->entries)
			if (e.map(pk::kColour) != nullptr && (e.flags & pk::kNoPack) == 0)
				keys.push_back(e.the_key());
		std::mt19937 rng(7);
		std::vector<st::owner> owners = { st::add_owner("soak a"), st::add_owner("soak b"), st::add_owner("soak c") };
		std::vector<std::pair<st::owner, st::ticket>> live;
		std::vector<std::pair<double, uint64_t>> samples;
		const double t0 = now_ms(), end = t0 + minutes * 60000.0;
		double next_sample = t0, next_squeeze = t0 + 20000.0, squeeze_end = 0, next_reload = t0 + 120000.0;
		uint64_t asked = 0, dropped = 0, owners_dropped = 0, reloads = 0, squeezes = 0;
		while (now_ms() < end)
		{
			const double now = now_ms();
			st::on_present();
			frame();
			// (asked faster than dropped till about 1,200 are held, then as fast)
			const size_t target = 1200;
			for (uint32_t n = rng() % 9; n > 0; --n)
			{
				const pk::key k = keys[rng() % keys.size()];
				const st::owner o = owners[rng() % owners.size()];
				const pk::record *r = idx->find(k)->map(pk::kColour);
				uint8_t first = 0;
				if (rng() % 4 == 0 && r->levels > 2 && (r->w >> 2) % 4 == 0 && (r->h >> 2) % 4 == 0)
					first = 2;
				live.emplace_back(o, st::request(o, want_of(k, first, 0, int32_t(rng() % 3))));
				asked++;
			}
			for (uint32_t n = live.size() > target ? rng() % 12 : rng() % 3; n > 0 && !live.empty(); --n)
			{
				const size_t i = rng() % live.size();
				st::drop(live[i].second);
				live[i] = live.back();
				live.pop_back();
				dropped++;
			}
			if (rng() % 2000 == 0)
			{
				// (an owner gone with everything it holds, a new one in its place)
				const size_t i = rng() % owners.size();
				st::drop_owner(owners[i]);
				live.erase(std::remove_if(live.begin(), live.end(), [&](const auto &p) { return p.first == owners[i]; }), live.end());
				owners[i] = st::add_owner("soak again");
				owners_dropped++;
			}
			if (rng() % 500 == 0)
				st::pause(st::kQuiet);
			if (rng() % 100 == 0)
				st::resume(st::kQuiet);
			if (now >= next_squeeze)
			{
				st::limits l = nocap();
				l.vram_budget = 4096 * kMB;
				l.vram_usage = 4096 * kMB * 87 / 100;
				st::set_limits(l);
				squeeze_end = now + 3000.0;
				next_squeeze = now + 20000.0;
				squeezes++;
			}
			if (squeeze_end != 0 && now >= squeeze_end)
			{
				st::set_limits(nocap());
				squeeze_end = 0;
			}
			if (now >= next_reload)
			{
				// (the same pack opened again: a new epoch, every ticket gone; the owners drop theirs)
				const uint32_t e = st::epoch();
				st::open(idx);
				while (st::epoch() == e && now_ms() - now < 10000)
					Sleep(1);
				for (const auto &p : live)
					st::drop(p.second);
				live.clear();
				next_reload = now + 120000.0;
				reloads++;
			}
			if (now >= next_sample)
			{
				const st::counts c = st::get_counts();
				const uint64_t va = avail_virtual();
				samples.emplace_back((now - t0) / 60000.0, va);
				std::printf("--    %5.1f min: address space free %s; %llu asked, %zu held, %llu resident (%s), %llu made (%llu without the top level), %s read\n",
					(now - t0) / 60000.0, mb(va).c_str(), static_cast<unsigned long long>(asked), live.size(), static_cast<unsigned long long>(c.resident),
					mb(c.resident_bytes).c_str(), static_cast<unsigned long long>(c.made), static_cast<unsigned long long>(c.skipped_top), mb(c.bytes_read).c_str());
				next_sample += 30000.0;
			}
			Sleep(16);
		}
		st::resume(st::kQuiet);
		st::set_limits(nocap());
		for (const auto &p : live)
			st::drop(p.second);
		for (st::owner o : owners)
			st::drop_owner(o);
		wait_worker();
		settle();
		const st::counts c = st::get_counts();
		const double from = minutes >= 6 ? 2.0 : minutes / 3.0;
		uint64_t lo = UINT64_MAX, hi = 0;
		for (const auto &s : samples)
			if (s.first >= from)
				lo = std::min(lo, s.second), hi = std::max(hi, s.second);
		char line[400];
		std::snprintf(line, sizeof(line), "soak %d min: %llu asked, %llu dropped, %llu owners dropped, %llu reloads, %llu budget squeezes; %llu made, %llu let go, %llu left, %llu resident at the end",
			minutes, static_cast<unsigned long long>(asked), static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(owners_dropped),
			static_cast<unsigned long long>(reloads), static_cast<unsigned long long>(squeezes), static_cast<unsigned long long>(c.made),
			static_cast<unsigned long long>(c.released + c.retired), static_cast<unsigned long long>(c.leaked), static_cast<unsigned long long>(c.resident));
		check(c.made == c.released + c.retired && c.leaked == 0 && c.resident == 0, line);
		std::snprintf(line, sizeof(line), "the address space free from minute %.1f on: between %s and %s (%s apart, within 8 MB)", from, mb(lo).c_str(), mb(hi).c_str(),
			mb(lo != UINT64_MAX ? hi - lo : 0).c_str());
		check(samples.size() >= 2 && lo != UINT64_MAX && hi - lo <= 8 * kMB, line);
		st::stop();
		hifi::retire::release_all();
		return failures;
	}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0); // (each line out as it's printed: a crash shows where it was)
	{
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		g_exe = exe;
		g_build = g_exe.substr(0, g_exe.find_last_of(L'\\') + 1);
		CreateDirectoryW((g_build + L"test").c_str(), nullptr);
		CreateDirectoryW((g_build + L"test\\packstream").c_str(), nullptr);
	}
	const std::wstring synthetic = g_build + L"test\\packstream\\made\\";
	const std::string arg = argc > 1 ? argv[1] : "";
	g_on_gpu = GetEnvironmentVariableA("ZONE_GPU", nullptr, 0) != 0 && arg.empty();

	ID3D12Debug *debug = nullptr;
	if (arg != "va" && arg != "soak" && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
	{
		debug->EnableDebugLayer();
		// (HIFIXI_GBV=1: GPU-based validation as well: the states the textures are really in at GPU time)
		ID3D12Debug1 *debug1 = nullptr;
		if (GetEnvironmentVariableA("HIFIXI_GBV", nullptr, 0) != 0 && SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))))
		{
			debug1->SetEnableGPUBasedValidation(TRUE);
			debug1->Release();
			std::printf("      (GPU-based validation on)\n");
		}
	}

	if (arg == "soak")
	{
		char text[16] = "10";
		GetEnvironmentVariableA("PACKSTREAM_SOAK", text, sizeof(text));
		std::string why_child;
		const std::wstring folder = pk::folder_in(L"C:\\Games\\PhoenixXI\\bootloader");
		return child_soak(std::max(1, std::atoi(text)), pk::load(folder, nullptr, why_child) ? folder : synthetic);
	}
	if (arg == "va")
	{
		std::string why_child;
		const std::wstring folder = pk::folder_in(L"C:\\Games\\PhoenixXI\\bootloader");
		return child_va(pk::load(folder, nullptr, why_child) ? folder : synthetic);
	}
	// The device-loss children: the made-up pack was written by the parent
	if (arg == "lost" || arg == "lost-read" || arg == "lost-stop" || arg == "lost-switch")
	{
		if (arg == "lost-read")
			return child_lost_read();
		std::vector<made_entry> entries;
		std::string why_child;
		auto x = pk::load(synthetic, nullptr, why_child);
		if (!x)
		{
			check(false, "the made-up pack (the parent writes it)");
			return 1;
		}
		for (const pk::entry &e : x->entries)
		{
			const pk::record *r = e.map(pk::kColour);
			made_entry m;
			m.k = e.the_key();
			m.w = r->w;
			m.h = r->h;
			m.levels = r->levels;
			m.tag = r->w >= 2048 && r->h >= 1024 && r->format == pk::kBC2 ? "lost" : "";
			entries.push_back(m);
		}
		if (arg != "lost")
			return child_lost_at_once(arg == "lost-switch", synthetic, entries);
		return child_lost(synthetic, entries);
	}

	if (!make_gpu(g_on_gpu))
	{
		check(false, g_on_gpu ? "a device on the GPU" : "a WARP device");
		return 1;
	}
	{
		DXGI_ADAPTER_DESC d = {};
		IDXGIFactory4 *f = nullptr;
		IDXGIAdapter *a = nullptr;
		if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) && SUCCEEDED(f->EnumAdapterByLuid(G.device->GetAdapterLuid(), IID_PPV_ARGS(&a))))
		{
			a->GetDesc(&d);
			char name[128];
			std::snprintf(name, sizeof(name), "%ls", d.Description);
			note(std::string("on ") + name + (G.info != nullptr ? ", the debug layer on" : ", NO debug layer (Graphics Tools missing)"));
			a->Release();
		}
		if (f != nullptr)
			f->Release();
	}

	// ---- The packs
	std::vector<made_entry> made;
	{
		const double t0 = now_ms();
		const bool written = make_synthetic(synthetic, made);
		check(written, "the made-up pack written: " + std::to_string(made.size()) + " entries (BC2, BC7, BC4; 4x4 to 4096x4096) in " + std::to_string(int(now_ms() - t0)) +
			" ms");
	}
	std::string why;
	auto made_idx = pk::load(synthetic, nullptr, why);
	check(made_idx != nullptr, "the made-up pack loads " + why);
	if (!made_idx)
		return failures;
	std::shared_ptr<const pk::index> real;
	{
		const std::wstring folder = pk::folder_in(L"C:\\Games\\PhoenixXI\\bootloader");
		real = pk::load(folder, nullptr, why);
		char f[300];
		std::snprintf(f, sizeof(f), "%ls", folder.c_str());
		note(real ? std::string("the real pack: ") + f + " (" + std::to_string(real->entries.size()) + " entries, read only)" : std::string("no real pack at ") + f + ": " + why);
	}
	readback rb;
	check(rb.make(64 * kMB), "a 64 MB readback buffer");
	if (G.info != nullptr)
	{
		// (the debug layer listens: a copy of a buffer onto itself, overlapping, is an error it reports; so the parts'
		// "said nothing" below means something)
		G.allocator->Reset();
		G.list->Reset(G.allocator, nullptr);
		G.list->CopyBufferRegion(rb.buffer, 0, rb.buffer, 512, 4096);
		G.list->Close();
		std::string first;
		const uint32_t n = debug_messages(first);
		check(n != 0, "the debug layer reports a bad call made on purpose (" + std::to_string(n) + ": " + first.substr(0, 80) + ")");
	}

	// ---- Address space per 1,000 textures (the GPU only: WARP keeps textures in the process). In a child process with no
	// debug layer (it keeps bookkeeping of its own for every object) and the driver's high-water not yet raised here
	if (g_on_gpu)
		run_child(L"va", "address space per texture (no debug layer)");
	else
		note("address space per texture: skipped on WARP (its textures live in this process); ZONE_GPU=1 checks it");

	// ---- The ring, the made-up pack read back
	settle(); // (the retire list's own fence made first: it holds the device too)
	ULONG device_refs = G.device->AddRef();
	G.device->Release();
	device_refs--;
	check(st::start(G.device, 32) && !st::start(G.device, 32), "started once (a second start refused)");
	check(wait_parts(), "the worker made its copy queue, lists, fence and ring");
	{
		const st::counts c = st::get_counts();
		check(c.ring_bytes == 32 * kMB && c.ring_heap == D3D12_HEAP_TYPE_CUSTOM && c.ring_page == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK && c.ring_pool == D3D12_MEMORY_POOL_L0,
			"the ring: 32 MB, its heap CUSTOM, CPU write-back, L0 (GetHeapProperties: not write-combined, so the CRC over it is quick)");
	}
	{
		const st::owner o = st::add_owner("before open");
		const st::ticket t = st::request(o, want_of(made[0].k));
		check(st::poll(t) == st::state::missed && st::reason(t) == st::miss::no_pack, "a request with no pack open: missed at once (no pack open)");
		st::drop_owner(o);
	}
	check(open_and_wait(nullptr, synthetic) && st::open_status().empty(), "the made-up pack opened on the worker");
	{
		st::open(L"C:\\no such folder\\");
		const double t0 = now_ms();
		while (st::current() != nullptr && now_ms() - t0 < 10000)
			Sleep(1);
		check(st::current() == nullptr && st::open_status().find("no pack.idx") != std::string::npos, "a folder with no pack: none open, \"" + st::open_status() + "\"");
		check(open_and_wait(nullptr, synthetic), "the made-up pack opened again");
	}
	pk::reader own(made_idx);
	{
		// Every entry, in waves of at most 200 MB stored (on WARP the textures live in this process)
		const st::owner o = st::add_owner("all");
		std::vector<item> all;
		int matched = 0, ready = 0, missed = 0, asked = 0;
		std::string bad;
		size_t at = 0;
		const double t0 = now_ms();
		uint64_t bytes = 0;
		std::vector<std::pair<st::ticket, pk::record>> sample_bc2;
		while (at < made.size())
		{
			std::vector<st::ticket> ts;
			std::vector<pk::record> recs;
			uint64_t wave = 0;
			for (; at < made.size() && wave < 200 * kMB; ++at)
			{
				if (made[at].tag == "nopack" || made[at].tag == "lost")
					continue;
				const pk::record *r = made_idx->find(made[at].k)->map(pk::kColour);
				ts.push_back(st::request(o, want_of(made[at].k)));
				recs.push_back(*r);
				wave += r->bytes;
				asked++;
			}
			const tally t = wait_all(ts);
			ready += t.ready;
			missed += t.missed;
			std::vector<item> items;
			for (size_t i = 0; i < ts.size(); ++i)
			{
				item it;
				it.t = ts[i];
				it.rec = recs[i];
				if (st::get(ts[i], it.v))
					items.push_back(it), bytes += recs[i].bytes;
			}
			matched += read_back(items, own, rb, bad);
			for (const st::ticket &t2 : ts)
				st::drop(t2);
			settle();
		}
		char line[300];
		std::snprintf(line, sizeof(line), "the made-up pack: %d entries asked, %d ready, %d missed; %d read back row for row equal to their records (%s in %.0f ms)", asked, ready,
			missed, matched, mb(bytes).c_str(), now_ms() - t0);
		check(asked >= 2000 && ready == asked && matched == asked, line + (bad.empty() ? std::string() : ": " + bad));
		st::drop_owner(o);
		messages_check("the made-up pack read back");
	}
	{
		// BC2 textures sampled by a shader through views write_view made in the test's own heap
		sampler s;
		check(s.make(32), "a compute shader, its root signature and a 32-slot heap of the test's");
		const st::owner o = st::add_owner("sampled");
		std::vector<item> items;
		std::vector<std::pair<uint32_t, uint32_t>> sizes;
		for (const made_entry &e : made)
			if (e.format == pk::kBC2 && e.w >= 16 && e.h >= 16 && e.tag.empty() && items.size() < 32)
			{
				item it;
				it.t = st::request(o, want_of(e.k));
				it.rec = *made_idx->find(e.k)->map(pk::kColour);
				items.push_back(it);
			}
		std::vector<st::ticket> ts;
		for (const item &it : items)
			ts.push_back(it.t);
		wait_all(ts);
		bool views = true;
		for (uint32_t i = 0; i < items.size(); ++i)
		{
			views = st::get(items[i].t, items[i].v) && st::write_view(items[i].t, s.cpu(i)) && views;
			sizes.emplace_back(items[i].v.w, items[i].v.h);
		}
		G.allocator->Reset();
		G.list->Reset(G.allocator, nullptr);
		s.record(sizes);
		G.list->Close();
		ID3D12CommandList *lists[] = { G.list };
		G.direct->ExecuteCommandLists(1, lists);
		idle();
		int good = 0;
		for (uint32_t i = 0; i < items.size(); ++i)
			good += samples_match(s, i, items[i], own);
		check(views && good == int(items.size()), "a shader's Load through write_view's SRVs (COMMON promoted to a shader read, no barrier): " + std::to_string(good) + " of " +
			std::to_string(items.size()) + " BC2 textures read as the CPU decodes them");
		st::drop_owner(o);
		settle();
		messages_check("the shader's reads");
	}

	// ---- The real pack: 2,000 random entries read back
	if (real)
	{
		std::vector<pk::key> keys;
		for (const pk::entry &e : real->entries)
			if (e.map(pk::kColour) != nullptr && (e.flags & pk::kNoPack) == 0)
				keys.push_back(e.the_key());
		std::shuffle(keys.begin(), keys.end(), std::mt19937(2026));
		keys.resize(std::min<size_t>(keys.size(), 2000));
		check(open_and_wait(real), "the real pack opened (read only)");
		pk::reader real_own(real);
		const st::owner o = st::add_owner("real");
		int matched = 0, ready = 0;
		uint64_t bytes = 0;
		std::string bad;
		size_t at = 0;
		const double t0 = now_ms();
		while (at < keys.size())
		{
			std::vector<st::ticket> ts;
			std::vector<pk::record> recs;
			uint64_t wave = 0;
			for (; at < keys.size() && wave < 200 * kMB; ++at)
			{
				const pk::record *r = real->find(keys[at])->map(pk::kColour);
				ts.push_back(st::request(o, want_of(keys[at])));
				recs.push_back(*r);
				wave += r->bytes;
			}
			ready += wait_all(ts).ready;
			std::vector<item> items;
			for (size_t i = 0; i < ts.size(); ++i)
			{
				item it;
				it.t = ts[i];
				it.rec = recs[i];
				if (st::get(ts[i], it.v))
					items.push_back(it), bytes += recs[i].bytes;
			}
			matched += read_back(items, real_own, rb, bad);
			for (const st::ticket &t2 : ts)
				st::drop(t2);
			settle();
		}
		char line[300];
		std::snprintf(line, sizeof(line), "the real pack: %zu random entries, %d ready, %d read back equal to their records (%s in %.0f ms)", keys.size(), ready, matched,
			mb(bytes).c_str(), now_ms() - t0);
		check(ready == int(keys.size()) && matched == ready, line + (bad.empty() ? std::string() : ": " + bad));
		st::drop_owner(o);
		check(open_and_wait(nullptr, synthetic), "the made-up pack opened again");
		messages_check("the real pack read back");
	}
	else
		note("the real pack's 2,000 entries: skipped (no real pack); the made-up pack's 2,000 stood in");

	// ---- Misses at the request
	{
		const st::owner o = st::add_owner("misses");
		pk::key unknown = made[0].k;
		unknown.hash ^= 1;
		const st::ticket t1 = st::request(o, want_of(unknown));
		const st::ticket t2 = st::request(o, want_of(tagged(made, "nopack")->k));
		const st::ticket t3 = st::request(o, want_of(tagged(made, "small12")->k, 1));
		st::want old = want_of(made[0].k);
		old.epoch = st::epoch() - 1;
		const st::ticket t4 = st::request(o, old);
		st::want height = want_of(made[0].k);
		height.kind = pk::kHeight;
		const st::ticket t5 = st::request(o, height);
		check(st::reason(t1) == st::miss::not_in_pack && st::reason(t2) == st::miss::no_pack_flag && st::reason(t3) == st::miss::first_level &&
				  st::poll(t4) == st::state::gone && st::reason(t5) == st::miss::not_in_pack,
			std::string("missed at once: another key (") + st::text(st::reason(t1)) + "), a no_pack entry (" + st::text(st::reason(t2)) + "), 12x12 from level 1 (" +
				st::text(st::reason(t3)) + "); a want of an older epoch gone; a kind the entry hasn't (" + st::text(st::reason(t5)) + ")");
		std::vector<std::pair<st::ticket, st::state>> done;
		st::take_done(o, done);
		check(done.size() == 5, "take_done hands the owner its 5 tickets that ended (" + std::to_string(done.size()) + ")");
		st::drop_owner(o);
	}

	// ---- take_done: each ticket once across an epoch change, a miss keeps its reason; nothing kept for an owner that
	// only polls
	{
		const st::owner o = st::add_owner("done once");
		pk::key unknown = made[0].k;
		unknown.hash ^= 1;
		const st::ticket ready = st::request(o, want_of(made[7].k));
		const st::ticket missed = st::request(o, want_of(unknown));
		const bool came = wait_for(ready, st::state::ready);
		const bool reopened = open_and_wait(made_idx); // (a new epoch: the ready one gone)
		std::vector<std::pair<st::ticket, st::state>> done;
		st::take_done(o, done);
		int of_ready = 0, of_missed = 0;
		for (const auto &d : done)
		{
			of_ready += d.first == ready && d.second == st::state::gone;
			of_missed += d.first == missed && d.second == st::state::missed;
		}
		char line[300];
		std::snprintf(line, sizeof(line), "a ticket ready and one missed, then the pack opened again: take_done hands over %zu (each once: the ready one now gone %d, the missed one still missed %d, \"%s\")",
			done.size(), of_ready, of_missed, st::text(st::reason(missed)));
		check(came && reopened && done.size() == 2 && of_ready == 1 && of_missed == 1 && st::reason(missed) == st::miss::not_in_pack, line);
		const st::owner p = st::add_owner("only polls");
		for (int i = 0; i < 3000; ++i)
			st::drop(st::request(p, want_of(unknown)));
		for (int i = 0; i < 50; ++i)
			st::request(p, want_of(unknown));
		const size_t waiting = st::test::done_waiting(p);
		check(waiting == 50, "an owner that never calls take_done, 3,000 tickets missed and dropped, 50 missed and held: " + std::to_string(waiting) + " kept for it (50)");
		st::drop_owner(o);
		st::drop_owner(p);
		check(open_and_wait(nullptr, synthetic), "the made-up pack opened again");
	}

	// ---- Two owners of one key share one texture
	{
		const hifi::retire::counts r0 = hifi::retire::status();
		const st::owner a = st::add_owner("zone"), b = st::add_owner("model actors");
		const pk::key k = tagged(made, "shared")->k;
		const st::ticket ta = st::request(a, want_of(k));
		const st::ticket tb = st::request(b, want_of(k));
		wait_all({ ta, tb });
		st::view va, vb;
		const bool both = st::get(ta, va) && st::get(tb, vb);
		const st::counts c1 = st::get_counts();
		check(both && va.resource == vb.resource && c1.resident == 1 && c1.shared_bytes == va.bytes,
			"two owners asking one key and levels: one texture (" + std::to_string(c1.resident) + " resident), its " + std::to_string(va.bytes) +
				" bytes counted as not taken twice (" + std::to_string(c1.shared_bytes) + ")");
		st::drop(ta);
		const st::counts c2 = st::get_counts();
		const bool kept = c2.resident == 1 && c2.shared_bytes == 0 && hifi::retire::status().held == r0.held && st::poll(tb) == st::state::ready;
		st::drop(tb);
		const st::counts c3 = st::get_counts();
		check(kept && c3.resident == 0 && c3.retired == c2.retired + 1 && hifi::retire::status().held == r0.held + 1,
			"it stays through the first drop; the second hands it to the retire list (resident 0)");
		const st::ticket t1 = st::request(a, want_of(k, 0, 3));
		const st::ticket t2 = st::request(b, want_of(k, 0, 0));
		wait_all({ t1, t2 });
		st::view v1, v2;
		const bool got = st::get(t1, v1) && st::get(t2, v2);
		check(got && v1.resource != v2.resource && v1.levels == 3 && v2.levels == tagged(made, "shared")->levels,
			"other levels of the same key: another texture (3 levels; all " + std::to_string(v2.levels) + ")");
		st::drop_owner(a);
		st::drop_owner(b);
		settle();
		messages_check("two owners");
	}

	// ---- Gone tickets keep their texture (docs/texture-pack/zones.md decision 1): an open (a reload) or a close turns a
	// ready ticket gone but leaves it its texture till it's dropped (held), then to the retire list as any drop, let go
	// behind the fence; a new device lets go at once, the held ones too
	{
		std::vector<const made_entry *> plain;
		for (const made_entry &m : made)
			if (m.tag.empty() && m.w <= 256 && m.h <= 256 && plain.size() < 3)
				plain.push_back(&m);
		const st::owner o = st::add_owner("gone keeps");
		const st::ticket t = st::request(o, want_of(plain[0]->k));
		const bool came = wait_for(t, st::state::ready);
		st::view v;
		st::get(t, v);
		ID3D12Resource *const res = v.resource;
		if (res != nullptr)
			res->AddRef();
		const bool reopened = open_and_wait(made_idx); // (a reload)
		settle();                                       // (presents: nothing of it let go)
		const st::counts c1 = st::get_counts();
		ULONG alive = 0;
		if (res != nullptr)
		{
			res->AddRef();
			alive = res->Release();
		}
		st::view gone_view;
		const bool refused = !st::get(t, gone_view) && !st::write_view(t, D3D12_CPU_DESCRIPTOR_HANDLE { 0 });
		char line[400];
		std::snprintf(line, sizeof(line), "a reload: the ready ticket %s, its texture held for it (held %llu, %llu bytes; refs %lu after 4 presents); get and write_view refuse it",
			st::text(st::poll(t)), static_cast<unsigned long long>(c1.held), static_cast<unsigned long long>(c1.held_bytes), alive);
		check(came && reopened && st::poll(t) == st::state::gone && c1.held == 1 && c1.held_bytes == v.bytes && c1.resident == 0 && alive >= 2 && refused, line);
		const st::counts c1b = st::get_counts();
		st::drop(t);
		const st::counts c2 = st::get_counts();
		frame(); // (one present: still held by the retire list)
		ULONG one = 0;
		if (res != nullptr)
		{
			res->AddRef();
			one = res->Release();
		}
		idle();
		settle();
		const ULONG last = res != nullptr ? res->Release() : 99;
		std::snprintf(line, sizeof(line), "  dropped: to the retire list (held %llu, retired +%llu), after one present still there (refs %lu), let go behind the fence (refs %lu)",
			static_cast<unsigned long long>(c2.held), static_cast<unsigned long long>(c2.retired - c1b.retired), one, last);
		check(c2.held == 0 && c2.held_bytes == 0 && c2.retired == c1b.retired + 1 && one >= 2 && last == 0, line);
		// (a close the same; then a new device: the held ones let go at once, their tickets' hold forgotten)
		const st::ticket t2 = st::request(o, want_of(plain[1]->k));
		const st::ticket t3 = st::request(o, want_of(plain[2]->k));
		const tally got = wait_all({ t2, t3 });
		const uint32_t e0 = st::epoch();
		st::close();
		for (int i = 0; i < 30000 && st::epoch() == e0; ++i)
			Sleep(1);
		const st::counts c3 = st::get_counts();
		const bool closed_held = got.ready == 2 && st::poll(t2) == st::state::gone && c3.held == 2 && st::current() == nullptr;
		st::drop(t3);
		const st::counts c4 = st::get_counts();
		st::set_device(G.device); // (a device change, as a new device's: the epoch at once)
		const st::counts c5 = st::get_counts();
		st::drop(t2); // (its hold forgotten: nothing let go twice)
		const st::counts c6 = st::get_counts();
		std::snprintf(line, sizeof(line), "a close: both gone and held (%llu); one dropped (held %llu); a device change lets go of the other at once (held %llu, retired +%llu), its drop then nothing (+%llu)",
			static_cast<unsigned long long>(c3.held), static_cast<unsigned long long>(c4.held), static_cast<unsigned long long>(c5.held),
			static_cast<unsigned long long>(c5.retired - c4.retired), static_cast<unsigned long long>(c6.retired - c5.retired));
		check(closed_held && c4.held == 1 && c5.held == 0 && c5.held_bytes == 0 && c5.retired == c4.retired + 1 && c6.retired == c5.retired, line);
		st::drop_owner(o);
		check(wait_parts() && open_and_wait(nullptr, synthetic), "the made-up pack opened again on the device's new parts");
		settle();
		messages_check("gone tickets keep their texture");
	}

	// ---- want::down_to: the levels from the top down to the first at most a size (a zone's: the game's own), counted
	// by the streamer from the record: a 2x record 2 levels, 4x 3, the same size 1; down_to wider than tall: both sides
	{
		const made_entry *m = nullptr;
		for (const made_entry &e : made)
			if (e.tag.empty() && e.format == pk::kBC2 && e.w >= 64 && e.h >= 64 && e.w <= 512 && e.h <= 512 && e.levels >= 5)
			{
				m = &e;
				break;
			}
		const st::owner o = st::add_owner("down to");
		auto ask = [&](uint32_t dw, uint32_t dh) {
			st::want w = want_of(m->k);
			w.down_to_w = dw;
			w.down_to_h = dh;
			return st::request(o, w);
		};
		const st::ticket t2 = m != nullptr ? ask(m->w / 2, m->h / 2) : st::ticket {};
		const st::ticket t4 = m != nullptr ? ask(m->w / 4, m->h / 4) : st::ticket {};
		const st::ticket t1 = m != nullptr ? ask(m->w, m->h) : st::ticket {};
		const st::ticket tw = m != nullptr ? ask(m->w / 4, m->h) : st::ticket {};
		wait_all({ t2, t4, t1, tw });
		st::view v2, v4, v1, vw;
		const bool got = st::get(t2, v2) && st::get(t4, v4) && st::get(t1, v1) && st::get(tw, vw);
		char line[300];
		std::snprintf(line, sizeof(line), "down_to: a %ux%u record down to half its size %u levels, a quarter %u, its own size %u, a quarter wide and its own height %u",
			m != nullptr ? m->w : 0, m != nullptr ? m->h : 0, v2.levels, v4.levels, v1.levels, vw.levels);
		check(m != nullptr && got && v2.levels == 2 && v4.levels == 3 && v1.levels == 1 && vw.levels == 3 && v2.w == m->w, line);
		st::drop_owner(o);
		settle();
		messages_check("down_to");
	}

	// ---- Failures at the worker: a level CRC that doesn't match (changed in a copy of the index) and a data file that
	// can't be opened (a copy whose folder has none). Missed "a CRC mismatch" and "a read failed", the texture made for
	// each released at once (no queue ran on it), one line each from take_log with the reader's why; a good texture
	// asked between them still comes (the ring space the failed read took given back)
	{
		std::vector<const made_entry *> plain;
		for (const made_entry &m : made)
			if (m.tag.empty() && m.w <= 256 && m.h <= 256 && plain.size() < 3)
				plain.push_back(&m);
		std::vector<std::string> lines;
		st::take_log(lines);
		lines.clear();
		auto bad_crc = std::make_shared<pk::index>(*made_idx);
		const pk::entry *be = plain.size() == 3 ? bad_crc->find(plain[0]->k) : nullptr;
		const pk::record *br = be != nullptr ? be->map(pk::kColour) : nullptr;
		if (br != nullptr)
			bad_crc->level_crcs[br->level_crc] ^= 1;
		const st::counts c0 = st::get_counts();
		const bool opened = br != nullptr && open_and_wait(bad_crc);
		const st::owner o = st::add_owner("failures");
		const st::ticket tc = opened ? st::request(o, want_of(plain[0]->k)) : st::ticket {};
		const st::ticket tg = opened ? st::request(o, want_of(plain[1]->k)) : st::ticket {};
		wait_all({ tc, tg });
		const std::wstring empty = g_build + L"test\\packstream\\no-files\\";
		CreateDirectoryW(empty.substr(0, empty.size() - 1).c_str(), nullptr);
		auto no_files = std::make_shared<pk::index>(*made_idx);
		no_files->folder = empty;
		const st::state good = st::poll(tg);
		const bool opened2 = opened && open_and_wait(no_files);
		const st::ticket tr = opened2 ? st::request(o, want_of(plain[2]->k)) : st::ticket {};
		wait_all({ tr });
		const st::counts c1 = st::get_counts();
		st::take_log(lines);
		const bool crc_line = lines.size() == 2 && lines[0].find("a CRC mismatch (CRC mismatch (level 0))") != std::string::npos;
		const bool read_line = lines.size() == 2 && lines[1].find("a read failed (pack-000.bin can't be opened") != std::string::npos;
		char line[400];
		std::snprintf(line, sizeof(line),
			"at the worker: a damaged level %s (%s), a good one between %s, a data file missing %s (%s); %llu made for them released at once; take_log %zu lines",
			st::text(st::poll(tc)), st::text(st::reason(tc)), st::text(good), st::text(st::poll(tr)), st::text(st::reason(tr)),
			static_cast<unsigned long long>(c1.released - c0.released), lines.size());
		check(opened && opened2 && st::reason(tc) == st::miss::crc && good == st::state::ready && st::reason(tr) == st::miss::read &&
				  c1.refused[size_t(st::miss::crc)] == c0.refused[size_t(st::miss::crc)] + 1 &&
				  c1.refused[size_t(st::miss::read)] == c0.refused[size_t(st::miss::read)] + 1 && c1.released - c0.released == 2 && c1.leaked == c0.leaked,
			line);
		check(crc_line && read_line, "the log's lines name the miss and the reader's why: " + (lines.empty() ? std::string("(none)") : lines[0] + (lines.size() > 1 ? " | " + lines[1] : "")));
		lines.clear();
		st::take_log(lines);
		check(lines.empty(), "take_log hands each line over once (" + std::to_string(lines.size()) + " the second time)");
		st::drop_owner(o);
		check(open_and_wait(nullptr, synthetic), "the made-up pack opened again");
		settle();
		messages_check("failures at the worker");
	}

	// ---- Evict and stream again (a character's key): dropped while a list reading it runs
	{
		sampler s;
		s.make(2);
		const st::owner o = st::add_owner("characters");
		const made_entry *e = tagged(made, "char");
		item it;
		it.t = st::request(o, want_of(e->k));
		it.rec = *made_idx->find(e->k)->map(pk::kColour);
		wait_for(it.t, st::state::ready);
		st::get(it.t, it.v);
		ID3D12Resource *old = it.v.resource;
		old->AddRef();
		st::write_view(it.t, s.cpu(0));
		G.allocator->Reset();
		G.list->Reset(G.allocator, nullptr);
		s.record({ { it.v.w, it.v.h } });
		G.list->Close();
		ID3D12CommandList *lists[] = { G.list };
		G.direct->ExecuteCommandLists(1, lists); // (not waited for: the list may still read it)
		const size_t held0 = hifi::retire::status().held;
		st::drop(it.t);
		const bool to_retire = hifi::retire::status().held == held0 + 1;
		item again;
		again.t = st::request(o, want_of(e->k));
		again.rec = it.rec;
		wait_for(again.t, st::state::ready);
		st::get(again.t, again.v);
		frame(); // (one present: the old texture still held)
		old->AddRef();
		const ULONG still = old->Release();
		idle();
		settle();
		const ULONG last = old->Release();
		const bool read_right = samples_match(s, 0, it, own);
		char line[300];
		std::snprintf(line, sizeof(line), "dropped while a list reads it: to the retire list (%s); asked again, a new texture (%s); the old one held after one present (refs %lu), let go after the fence (refs %lu)",
			to_retire ? "yes" : "no", again.v.resource != old ? "yes" : "no", still, last);
		check(to_retire && again.v.resource != nullptr && again.v.resource != old && still >= 2 && last == 0, line);
		check(read_right, "the list in flight read the old texture right");
		st::drop_owner(o);
		settle();
		messages_check("evict and stream again");
	}

	// ---- Pause and resume
	{
		const st::owner o = st::add_owner("paused");
		st::pause(st::kTest);
		const st::counts c0 = st::get_counts();
		std::vector<st::ticket> ts;
		for (size_t i = 100; i < 160; ++i)
			ts.push_back(st::request(o, want_of(made[i].k)));
		Sleep(300);
		const tally during = count_states(ts);
		const st::counts c1 = st::get_counts();
		check(during.waiting == int(ts.size()) && c1.reads == c0.reads && c1.made == c0.made && (st::paused() & st::kTest) != 0,
			"paused: 60 asked, after 300 ms all still queued, nothing read or made");
		st::resume(st::kTest);
		const tally after = wait_all(ts);
		check(after.ready == int(ts.size()), "resumed: all 60 ready (" + std::to_string(after.ready) + ")");
		st::drop_owner(o);
		settle();
	}

	// ---- The per-present cap
	{
		st::limits l;
		l.per_present = 4 * kMB;
		st::set_limits(l);
		const st::counts c0 = st::get_counts();
		const st::owner o = st::add_owner("paced");
		std::vector<st::ticket> ts;
		uint64_t total = 0;
		st::on_present(); // (the cap counts from the first present; before it there's none)
		// (the 5.3 MB record first: over the cap, it must start alone in its present)
		ts.push_back(st::request(o, want_of(tagged(made, "big2048")->k)));
		total += made_idx->find(tagged(made, "big2048")->k)->map(pk::kColour)->bytes;
		for (size_t i = 0; i < made.size() && total < 40 * kMB; ++i)
			if (made[i].tag.empty())
			{
				ts.push_back(st::request(o, want_of(made[i].k)));
				total += made_idx->find(made[i].k)->map(pk::kColour)->bytes;
			}
		std::atomic<bool> done { false };
		std::thread presents([&] {
			while (!done)
			{
				st::on_present();
				Sleep(4);
			}
		});
		const tally t = wait_all(ts);
		done = true;
		presents.join();
		const st::counts c1 = st::get_counts();
		char line[300];
		std::snprintf(line, sizeof(line), "a 4 MB cap a present: %d of %zu ready (%s) over %llu presents; at most %s started in one, %llu over the cap alone (a 5.3 MB record), %llu over it with others",
			t.ready, ts.size(), mb(total).c_str(), static_cast<unsigned long long>(c1.presents - c0.presents), mb(c1.present_max_bytes).c_str(),
			static_cast<unsigned long long>(c1.present_alone - c0.present_alone), static_cast<unsigned long long>(c1.present_breaks - c0.present_breaks));
		check(t.ready == int(ts.size()) && c1.present_breaks == c0.present_breaks && c1.present_alone > c0.present_alone && c1.presents - c0.presents >= total / (4 * kMB),
			line);
		st::drop_owner(o);
		st::set_limits(nocap());
		settle();
	}

	// ---- A forced low budget
	{
		const st::owner o = st::add_owner("budget");
		const made_entry *e1 = tagged(made, "budget1"), *e2 = tagged(made, "budget2");
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		d.Width = 1024;
		d.Height = 1024;
		d.DepthOrArraySize = 1;
		d.MipLevels = uint16_t(e1->levels);
		d.Format = DXGI_FORMAT_BC2_UNORM;
		d.SampleDesc.Count = 1;
		const uint64_t full = G.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
		st::limits l = nocap();
		l.vram_budget = 1000 * kMB;
		l.vram_usage = 1000 * kMB * 86 / 100;
		st::set_limits(l);
		const st::ticket t1 = st::request(o, want_of(e1->k));
		wait_for(t1, st::state::ready);
		st::view v1;
		st::get(t1, v1);
		check(v1.first == 1 && v1.w == 512 && v1.h == 512 && v1.levels == e1->levels - 1 && v1.bytes < full,
			"over 85% of the budget (86% used): made from level 1, 512x512, one level fewer (" + mb(v1.bytes) + " of " + mb(full) + ")");
		l.vram_usage = 1000 * kMB * 96 / 100;
		st::set_limits(l);
		const st::ticket t2 = st::request(o, want_of(e2->k));
		Sleep(300);
		check(st::poll(t2) == st::state::queued && st::pressure() && (st::paused() & st::kVideoMemory) != 0, std::string("over 95% (96% used): still queued after 300 ms (") +
			st::text(st::poll(t2)) + "), pressure() true");
		l.vram_usage = 100 * kMB;
		st::set_limits(l);
		const bool came = wait_for(t2, st::state::ready, 10000);
		st::view v2;
		st::get(t2, v2);
		check(came && !st::pressure() && v2.first == 0 && v2.w == 1024, "the budget raised: it comes whole (1024x1024), pressure() false");
		st::limits low = nocap();
		low.avail_virtual = 300 * kMB;
		st::set_limits(low);
		const st::ticket t3 = st::request(o, want_of(made[300].k));
		Sleep(200);
		const bool held = st::poll(t3) == st::state::queued && (st::paused() & st::kAddressSpace) != 0;
		low.avail_virtual = 0;
		low.largest_free = 32 * kMB;
		st::set_limits(low);
		Sleep(100);
		const bool held_block = st::poll(t3) == st::state::queued && (st::paused() & st::kAddressSpace) != 0;
		st::set_limits(nocap());
		check(held && held_block && wait_for(t3, st::state::ready, 10000) && (st::paused() & st::kAddressSpace) == 0,
			"the address-space floor: 300 MB free, then a 32 MB largest block: queued (kAddressSpace); measured again as unknown: ready");
		st::drop_owner(o);
		settle();

		// The usage lags: eight 1024x1024 asked in one set_limits window, 70% of a budget of ten of them used. What the
		// worker made since the sample counts: the second comes without its top level, then the 95% stop. The order
		// expected is worked out here with the same rule and the device's allocation sizes
		{
			const st::owner q = st::add_owner("lagging usage");
			const uint64_t whole = alloc_of(1024, 1024, 11), top = alloc_of(512, 512, 10);
			st::limits lag = nocap();
			lag.vram_budget = whole * 10;
			lag.vram_usage = lag.vram_budget * 70 / 100;
			st::set_limits(lag);
			st::pause(st::kTest);
			std::vector<st::ticket> ts;
			std::vector<int> expect; // (0 whole, 1 from level 1, -1 queued)
			uint64_t used = lag.vram_usage;
			bool stopped = false;
			for (int i = 0; i < 8; ++i)
			{
				ts.push_back(st::request(q, want_of(tagged(made, "wrap", 3 * i)->k, 0, 0, 100 - i)));
				uint64_t a = whole;
				int first = 0;
				if ((used + whole) * 100 > lag.vram_budget * st::kSkipPercent)
					a = top, first = 1;
				if (stopped || (used + a) * 100 > lag.vram_budget * st::kStopPercent)
					stopped = true, first = -1;
				else
					used += a;
				expect.push_back(first);
			}
			st::resume(st::kTest);
			std::string got_text, want_text;
			bool same = true, slept = false;
			for (size_t i = 0; i < ts.size(); ++i)
			{
				int got = -2;
				st::view v;
				if (expect[i] >= 0)
				{
					if (wait_for(ts[i], st::state::ready, 10000) && st::get(ts[i], v))
						got = int(v.first);
				}
				else
				{
					if (!slept)
						Sleep(300), slept = true;
					if (st::poll(ts[i]) == st::state::queued)
						got = -1;
				}
				same = same && got == expect[i];
				got_text += got == -1 ? "q" : std::to_string(got);
				want_text += expect[i] == -1 ? "q" : std::to_string(expect[i]);
			}
			const bool all_three = std::count(expect.begin(), expect.end(), 0) != 0 && std::count(expect.begin(), expect.end(), 1) != 0 &&
				std::count(expect.begin(), expect.end(), -1) != 0;
			check(same && all_three && st::pressure(), "a lagging usage sample: 8 asked in one window, first levels " + got_text + " (q: queued), the same rule says " +
				want_text + "; pressure()");
			st::set_limits(nocap());
			wait_all(ts);
			st::drop_owner(q);
			settle();
		}

		// pressure() is the level, not "a texture was refused since the last sample": set_limits over 95% with nothing
		// queued says so at once, every time; a texture refused for its own size keeps it through set_limits under which
		// it still wouldn't fit; under 95% again it's false
		{
			const st::owner q = st::add_owner("pressure");
			st::limits hi = nocap();
			hi.vram_budget = 1000 * kMB;
			hi.vram_usage = 1000 * kMB * 96 / 100;
			bool over = true;
			for (int i = 0; i < 3; ++i)
			{
				st::set_limits(hi);
				over = over && st::pressure();
			}
			hi.vram_usage = 500 * kMB;
			st::set_limits(hi);
			const bool under = !st::pressure();
			const made_entry *ch = tagged(made, "char");
			const uint64_t smaller = alloc_of(ch->w / 2, ch->h / 2, ch->levels - 1);
			hi.vram_usage = hi.vram_budget * 95 / 100 - smaller / 2; // (under 95% alone; not with the texture from level 1)
			st::set_limits(hi);
			const bool before = !st::pressure();
			const st::ticket t = st::request(q, want_of(ch->k));
			Sleep(300);
			const bool refused = st::poll(t) == st::state::queued && st::pressure();
			st::set_limits(hi);
			const bool kept = st::pressure();
			Sleep(200);
			const bool still = st::poll(t) == st::state::queued;
			st::set_limits(nocap());
			const bool arrived = wait_for(t, st::state::ready, 10000) && !st::pressure();
			check(over && under && before && refused && kept && still && arrived,
				std::string("pressure(): over 95% with nothing queued, 3 samples in a row (") + (over ? "true" : "false") + "); at 50% false; a texture refused for its own size keeps it through the next sample (" +
					(kept ? "true" : "false") + "), then comes");
			st::drop_owner(q);
			settle();
		}
		const st::counts c = st::get_counts();
		check(c.skipped_top >= 1 && c.requeued >= 1, "counted: " + std::to_string(c.skipped_top) + " made without the top level, " + std::to_string(c.requeued) + " put back");
		messages_check("the budget");
	}

	// ---- Drop then destroy: the owner gone mid-stream, its heap released at once
	{
		const st::counts c0 = st::get_counts();
		ID3D12DescriptorHeap *heap = nullptr;
		D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
		G.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
		const UINT step = G.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		const st::owner o = st::add_owner("dropped");
		std::vector<st::ticket> ts;
		for (size_t i = 400; i < 900; ++i)
			ts.push_back(st::request(o, want_of(made[i].k)));
		int viewed = 0;
		const double t0 = now_ms();
		while (viewed < 8 && now_ms() - t0 < 10000)
			for (const st::ticket &t : ts)
				if (viewed < 8 && st::poll(t) == st::state::ready)
					viewed += st::write_view(t, { heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(viewed) * step });
		const tally mid = count_states(ts);
		st::drop_owner(o);
		heap->Release(); // (at once: the worker writes nothing of the owner's)
		const uint32_t left = st::in_flight(o);
		const tally after = count_states(ts);
		wait_worker();
		const st::counts c1 = st::get_counts();
		const uint64_t made_now = c1.made - c0.made, let_go = (c1.released - c0.released) + (c1.retired - c0.retired);
		char line[300];
		std::snprintf(line, sizeof(line), "500 asked, dropped with %d ready and %d on their way: in_flight %u, %d of 500 read dropped; the worker made %llu, let go %llu (resident %llu)",
			mid.ready, mid.waiting, left, after.dropped, static_cast<unsigned long long>(made_now), static_cast<unsigned long long>(let_go),
			static_cast<unsigned long long>(c1.resident));
		check(viewed == 8 && mid.waiting > 0 && left == 0 && after.dropped == 500 && made_now == let_go && c1.resident == 0, line);
		settle();
		check(hifi::retire::status().held == 0, "the retire list let go of everything after the fence");
		messages_check("drop then destroy");
	}

	// ---- set_device: a device change and a request of the new epoch landing between the worker's loop top and its pick
	// (made on the worker itself, test::before_pick): the ticket must not start on the old parts (the device change would
	// let them go under it and leave it "copying" for good); it comes on the new ones
	{
		const st::counts c0 = st::get_counts();
		const st::owner o = st::add_owner("device change");
		g_race_owner = o;
		g_race_key = made[1500].k;
		g_race_done = false;
		g_race_armed = true;
		st::test::before_pick(race_hook);
		const st::ticket waker = st::request(o, want_of(made[1501].k));
		const double t0 = now_ms();
		while (!g_race_done && now_ms() - t0 < 10000)
			Sleep(1);
		const bool ran = g_race_done;
		const bool came = ran && wait_for(g_race_ticket, st::state::ready, 10000);
		st::test::before_pick(nullptr);
		wait_all({ waker });
		wait_worker();
		const st::counts c1 = st::get_counts();
		char line[300];
		std::snprintf(line, sizeof(line), "set_device and a request between the worker's loop top and its pick: the new epoch's ticket %s, %llu objects left, epoch %u -> %u",
			ran ? st::text(st::poll(g_race_ticket)) : "never asked", static_cast<unsigned long long>(c1.leaked - c0.leaked), c0.epoch, c1.epoch);
		check(came && c1.leaked == c0.leaked && c1.epoch > c0.epoch && c1.in_flight == 0, line);
		st::drop_owner(o);
		settle();
		messages_check("set_device");
	}

	// ---- set_device and open both waiting at the worker's loop top: the open isn't lost to the device change
	{
		const auto before = st::current();
		const uint32_t e0 = st::epoch();
		const st::owner o = st::add_owner("device change and open");
		g_both_folder = synthetic;
		g_both_done = false;
		g_both_armed = true;
		st::test::before_pick(both_hook);
		const st::ticket waker = st::request(o, want_of(made[1502].k));
		double t0 = now_ms();
		while (!g_both_done && now_ms() - t0 < 10000)
			Sleep(1);
		st::test::before_pick(nullptr);
		t0 = now_ms();
		while ((st::current() == before || st::current() == nullptr) && now_ms() - t0 < 10000)
			Sleep(1);
		const auto after = st::current();
		const uint32_t e1 = st::epoch();
		char line[200];
		std::snprintf(line, sizeof(line), "set_device and open both waiting at the worker's loop top: the change done, then the open (a new snapshot %s; epoch %u -> %u)",
			after != nullptr && after != before ? "open" : "NOT opened", e0, e1);
		check(g_both_done && after != nullptr && after != before && e1 >= e0 + 2, line);
		st::drop(waker);
		st::drop_owner(o);
		wait_worker();
		settle();
		messages_check("set_device and open");
	}

	// ---- The ring: 8 MB, mixed sizes, wraps; too big; first levels
	{
		st::stop();
		hifi::retire::release_all();
		check(st::start(G.device, 4) && wait_parts() && st::get_counts().ring_bytes == 8 * kMB, "started again with ring_mb 4: clamped to 8 MB");
		check(open_and_wait(nullptr, synthetic), "the made-up pack opened");
		const st::owner o = st::add_owner("ring");
		// (the copies held on the GPU by a gate first: the 5.3 MB record and two 2.7 MB ones asked first can't all be in
		// the ring at once, so the worker must wait for space behind a copy that hasn't run. Nothing may read ready, and
		// no ring byte a held copy still reads may be written, until the gate opens)
		ID3D12Fence *gate = make_gate();
		st::test::gate_copies(gate, 1);
		const pk::key first3[3] = { tagged(made, "big2048")->k, tagged(made, "lost", 0)->k, tagged(made, "lost", 1)->k };
		std::vector<st::ticket> ts;
		for (int i = 0; i < 3; ++i)
			ts.push_back(st::request(o, want_of(first3[i], 0, 0, 1000 - i)));
		for (int i = 0; i < 24; ++i)
		{
			ts.push_back(st::request(o, want_of(tagged(made, "wrap", i)->k)));
			if (i % 6 == 3)
				ts.push_back(st::request(o, want_of(tagged(made, "big2048")->k, 0, 0, -i)));
			if (i % 4 == 1)
				ts.push_back(st::request(o, want_of(made[1000 + i].k)));
		}
		{
			const double w0 = now_ms();
			while (st::get_counts().ring_waits == 0 && now_ms() - w0 < 10000)
				Sleep(1);
			namespace sc = hifi::pack::stream_crumbs;
			const st::counts c = st::get_counts();
			const tally held = count_states(ts);
			const uint64_t signalled = sc::the.signalled.load(), completed = sc::the.completed.load();
			char line[300];
			std::snprintf(line, sizeof(line), "copies held on the GPU: the worker waits for ring space (%llu waits), %llu copies submitted, completed %llu of %llu; %d ready (none), the first %s",
				static_cast<unsigned long long>(c.ring_waits), static_cast<unsigned long long>(c.in_flight), static_cast<unsigned long long>(completed),
				static_cast<unsigned long long>(signalled), held.ready, st::text(st::poll(ts[0])));
			check(c.ring_waits != 0 && c.in_flight != 0 && held.ready == 0 && st::poll(ts[0]) == st::state::copying && completed < signalled, line);
			gate->Signal(1);
			st::test::gate_copies(nullptr, 0);
		}
		const tally t = wait_all(ts);
		// (each ticket's record, in the order asked)
		std::vector<item> items;
		{
			size_t k = 0;
			for (int i = 0; i < 3; ++i)
			{
				item it;
				it.t = ts[k++];
				it.rec = *made_idx->find(first3[i])->map(pk::kColour);
				if (st::get(it.t, it.v))
					items.push_back(it);
			}
			for (int i = 0; i < 24; ++i)
			{
				auto push = [&](const pk::key &key) {
					item it;
					it.t = ts[k++];
					it.rec = *made_idx->find(key)->map(pk::kColour);
					if (st::get(it.t, it.v))
						items.push_back(it);
				};
				push(tagged(made, "wrap", i)->k);
				if (i % 6 == 3)
					push(tagged(made, "big2048")->k);
				if (i % 4 == 1)
					push(made[1000 + i].k);
			}
		}
		std::string bad;
		const int matched = read_back(items, own, rb, bad);
		const st::counts c = st::get_counts();
		char line[300];
		std::snprintf(line, sizeof(line), "an 8 MB ring, %zu mixed records (0.7-5.3 MB): %d ready, %d read back equal; wrapped %llu times (%s skipped at the end), %llu waits for space",
			ts.size(), t.ready, matched, static_cast<unsigned long long>(c.ring_wraps), mb(c.ring_skipped).c_str(), static_cast<unsigned long long>(c.ring_waits));
		check(t.ready == int(ts.size()) && matched == int(items.size()) && items.size() == ts.size() && c.ring_wraps > 0 && c.ring_waits > 0,
			line + (bad.empty() ? std::string() : ": " + bad));
		const st::ticket big = st::request(o, want_of(tagged(made, "big4096")->k));
		const st::ticket part = st::request(o, want_of(tagged(made, "big4096")->k, 2));
		const st::ticket bc7 = st::request(o, want_of(tagged(made, "big4096bc7")->k));
		wait_all({ big, part, bc7 });
		st::view vp;
		check(st::reason(big) == st::miss::too_big && st::reason(bc7) == st::miss::too_big && st::get(part, vp) && vp.w == 1024 && vp.first == 2,
			std::string("4096x4096 BC2 (22 MB) and BC7 (16 MB): missed, ") + st::text(st::reason(big)) + "; the BC2 from level 2: ready, 1024x1024");
		const st::ticket odd = st::request(o, want_of(tagged(made, "small12")->k, 1));
		const st::ticket odd0 = st::request(o, want_of(tagged(made, "small12")->k, 0));
		wait_all({ odd, odd0 });
		check(st::reason(odd) == st::miss::first_level && st::poll(odd0) == st::state::ready, "12x12 from level 1 (6x6): refused; from level 0: ready");
		st::drop_owner(o);
		settle();
		messages_check("the 8 MB ring");
		st::stop();
		hifi::retire::release_all();
		gate->Release(); // (every copy that waited on it has run)
		st::start(G.device, 32);
		wait_parts();
		open_and_wait(nullptr, synthetic);
	}

	// ---- The crash section while the streamer's mutex is held
	{
		std::string section;
		double took = 0;
		std::atomic<bool> back { false };
		{
			auto lock = st::test::hold();
			std::thread t([&] {
				const double t0 = now_ms();
				section = hifi::pack::crash_section();
				took = now_ms() - t0;
				back = true;
			});
			const double t0 = now_ms();
			while (!back && now_ms() - t0 < 2000)
				Sleep(1);
			if (!back)
				lock.unlock(); // (it waits on the mutex: let it go, so the join below returns, and fail)
			t.join();
		}
		check(took < 10.0 && section.find("streamer: running") != std::string::npos && section.find("copy fence") != std::string::npos,
			"crash_section() with the streamer's mutex held by another thread: " + std::to_string(took).substr(0, 5) + " ms, with the streamer's lines");
		std::printf("%s", section.c_str());
	}

	// ---- stop() with copies in flight, not paused: four copies held by a gate, the worker idle waiting for the oldest;
	// the gate opens 5 ms into stop(). Its wait must last till the last copy is done, not end at the oldest's (the idle
	// wait's registration on the auto-reset event, left behind when the stop event woke it): nothing left
	{
		ID3D12Fence *gate = make_gate();
		st::test::gate_copies(gate, 1);
		const st::counts c0 = st::get_counts();
		const st::owner o = st::add_owner("stop in flight");
		std::vector<st::ticket> ts;
		for (int i = 0; i < 12; ++i)
			ts.push_back(st::request(o, want_of(tagged(made, "lost", i)->k)));
		const bool held = wait_held(st::kInFlight);
		Sleep(20);
		const tally before = count_states(ts);
		std::thread opener([&] {
			Sleep(5);
			gate->Signal(1);
		});
		const double t0 = now_ms();
		st::stop();
		const double stop_ms = now_ms() - t0;
		opener.join();
		st::test::gate_copies(nullptr, 0);
		hifi::retire::release_all();
		const st::counts c1 = st::get_counts();
		char line[300];
		std::snprintf(line, sizeof(line), "stop() with 4 copies in flight (held, %d ready), the gate opened 5 ms in: %.2f ms (under 100), %llu released, %llu objects left (none)",
			before.ready, stop_ms, static_cast<unsigned long long>(c1.released - c0.released), static_cast<unsigned long long>(c1.leaked - c0.leaked));
		check(held && before.ready == 0 && stop_ms < 100.0 && c1.leaked == c0.leaked && c1.released - c0.released >= st::kInFlight, line);
		gate->Release();
		st::start(G.device, 32);
		check(wait_parts() && open_and_wait(nullptr, synthetic), "started again, the made-up pack open");
	}

	// ---- stop() with 1,000 queued (the plugin's unload: then retire::release_all)
	{
		const st::owner o = st::add_owner("unload");
		st::pause(st::kTest);
		std::vector<st::ticket> ts;
		for (size_t i = 0; i < 1000; ++i)
			ts.push_back(st::request(o, want_of(made[i].k)));
		const double t0 = now_ms();
		st::stop();
		const double stop_ms = now_ms() - t0;
		hifi::retire::release_all();
		const tally t = count_states(ts);
		char line[200];
		std::snprintf(line, sizeof(line), "stop() with 1,000 queued: %.2f ms (under 100); then retire::release_all; all 1,000 read gone (%d)", stop_ms, t.gone);
		check(stop_ms < 100.0 && t.gone == 1000, line);
		ULONG refs = G.device->AddRef();
		G.device->Release();
		refs--;
		check(refs == device_refs, "everything the streamer made on the device released: the device's references back to " + std::to_string(device_refs) + " (" +
			std::to_string(refs) + ")");
		messages_check("stop with 1,000 queued");
	}

	// ---- The pack opened again (a reload) while a read is stalled on a pipe: the read is cancelled, and its ticket reads
	// gone with the new epoch, not missed as a failed read (no "a read failed" counted, no log line)
	{
		stall_pipe sp;
		check(sp.make(L"hxp-reload"), "a pipe that never answers");
		st::start(G.device, 32);
		wait_parts();
		check(open_and_wait(sp.idx), "its index open");
		const st::owner o = st::add_owner("reloaded");
		const st::ticket t = st::request(o, want_of(sp.k));
		const double w0 = now_ms();
		while (!sp.connected && now_ms() - w0 < 10000)
			Sleep(1);
		Sleep(50);
		const st::state during = st::poll(t);
		std::vector<std::string> lines;
		st::take_log(lines);
		lines.clear();
		const st::counts c0 = st::get_counts();
		const double t0 = now_ms();
		const bool reopened = open_and_wait(nullptr, synthetic);
		const double reload_ms = now_ms() - t0;
		const st::counts c1 = st::get_counts();
		st::take_log(lines);
		char line[300];
		std::snprintf(line, sizeof(line), "the pack opened again with a read pending on the pipe (the ticket %s): %.2f ms, the ticket now %s (%s), %llu more failed reads, %zu log lines",
			st::text(during), reload_ms, st::text(st::poll(t)), st::text(st::reason(t)),
			static_cast<unsigned long long>(c1.refused[size_t(st::miss::read)] - c0.refused[size_t(st::miss::read)]), lines.size());
		check(sp.connected && during == st::state::reading && reopened && reload_ms < 2000.0 && st::poll(t) == st::state::gone &&
				  c1.refused[size_t(st::miss::read)] == c0.refused[size_t(st::miss::read)] && lines.empty() && c1.released == c0.released + 1,
			line);
		st::drop_owner(o);
		st::stop();
		hifi::retire::release_all();
		messages_check("a reload during a stalled read");
	}

	// ---- stop() with a read stalled on a pipe
	{
		stall_pipe sp;
		check(sp.make(L"hxp-stop"), "a pipe that never answers");
		st::start(G.device, 32);
		wait_parts();
		check(open_and_wait(sp.idx), "its index open");
		const st::owner o = st::add_owner("stalled");
		const st::ticket t = st::request(o, want_of(sp.k));
		const double w0 = now_ms();
		while (!sp.connected && now_ms() - w0 < 10000)
			Sleep(1);
		Sleep(150);
		const uint32_t state = hifi::pack::stream_crumbs::the.state.load();
		const st::state ts = st::poll(t);
		const double t0 = now_ms();
		st::stop();
		const double stop_ms = now_ms() - t0;
		hifi::retire::release_all();
		char line[200];
		std::snprintf(line, sizeof(line), "stop() with the read pending on the pipe for 150 ms (the worker %s, the ticket %s): %.2f ms (under 100)",
			hifi::pack::stream_crumbs::state_text(state), st::text(ts), stop_ms);
		check(sp.connected && ts == st::state::reading && state == hifi::pack::stream_crumbs::kReading && stop_ms < 100.0, line);
		messages_check("stop during a stalled read");
	}

	// ---- stop() while an open loads pack.idx on the worker: a 4 MB pack.idx (not a real one: it's never parsed), each
	// 256 KB piece taking 40 ms (pk::load_test_window, a slow disk): stop() gives up the load at the next piece
	{
		const std::wstring junk = g_build + L"test\\packstream\\slow-index\\";
		CreateDirectoryW(junk.substr(0, junk.size() - 1).c_str(), nullptr);
		{
			std::vector<uint8_t> zeros(4 * kMB);
			HANDLE f = CreateFileW((junk + L"pack.idx").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			DWORD wrote = 0;
			if (f != INVALID_HANDLE_VALUE)
			{
				WriteFile(f, zeros.data(), DWORD(zeros.size()), &wrote, nullptr);
				CloseHandle(f);
			}
		}
		g_load_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		g_load_pieces = 0;
		st::start(G.device, 32);
		wait_parts();
		pk::load_test_window = slow_piece;
		st::open(junk);
		const bool loading = WaitForSingleObject(g_load_entered, 10000) == WAIT_OBJECT_0;
		const double t0 = now_ms();
		st::stop();
		const double stop_ms = now_ms() - t0;
		pk::load_test_window = nullptr;
		hifi::retire::release_all();
		CloseHandle(g_load_entered);
		const int pieces = g_load_pieces.load();
		char line[200];
		std::snprintf(line, sizeof(line), "stop() while an open reads a 4 MB pack.idx at 40 ms a piece: %.2f ms (under 100), %d of 16 pieces read", stop_ms, pieces);
		check(loading && stop_ms < 100.0 && pieces < 16, line);
	}

	// ---- Device loss (children)
	run_child(L"lost", "device loss with copies in flight");
	run_child(L"lost-read", "device loss with a read pending");
	run_child(L"lost-stop", "device loss with copies held, stop() at once");
	run_child(L"lost-switch", "device loss with copies held, set_device(nullptr) at once");

	// ---- The soak (PACKSTREAM_SOAK=<minutes>: a child process with no debug layer; or run "packstream_test soak" alone to
	// see its lines as they come)
	if (GetEnvironmentVariableA("PACKSTREAM_SOAK", nullptr, 0) != 0)
		run_child(L"soak", "the soak (no debug layer)", INFINITE);

	{
		const std::string section = hifi::pack::crash_section();
		check(section.find("streamer: stopped") != std::string::npos, "after stop the section says \"streamer: stopped\"");
	}

	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
