/*
 * The game's memory watched for the texture pack (memwatch.hpp), and the reports' section built from it.
 */

#include <windows.h>
#include <tlhelp32.h>
#include "memwatch.hpp"
#include "stream_crumbs.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <thread>
#include <vector>

namespace hifi::pack::memwatch
{
	namespace
	{
		// (64-bit atomics in this 32-bit process: lock-free on x86, so the reports' threads never wait on one)
		static_assert(std::atomic<uint64_t>::is_always_lock_free);

		struct published
		{
			std::atomic<uint64_t> avail_virtual { 0 }, total_virtual { 0 }, avail_phys { 0 };
			std::atomic<uint64_t> free_va { 0 }, largest_free { 0 }, walk_us { 0 };
			std::atomic<uint64_t> low_avail_virtual { 0 }, low_largest_free { 0 };
			std::atomic<uint8_t> bake { kBakeNotChecked };
			std::atomic<uint64_t> status_at { 0 }, walk_at { 0 }, bake_at { 0 };
			std::atomic<uint32_t> fall_walks { 0 };
			std::atomic<uint64_t> vram_usage { 0 }, vram_budget { 0 }, chars_bytes { 0 }, zone_bytes { 0 };
		};
		published p;

		struct thread_state
		{
			std::mutex mutex;
			std::condition_variable wake;
			std::thread thread;
			bool quit = false;
			// (left running at exit only when stop() wasn't called: let go rather than end the process)
			~thread_state()
			{
				if (thread.joinable())
					thread.detach();
			}
		};
		thread_state t;

		// The lower of a low-water mark and v (0: none yet)
		void lower(std::atomic<uint64_t> &mark, uint64_t v)
		{
			uint64_t now = mark.load();
			while ((now == 0 || v < now) && !mark.compare_exchange_weak(now, v))
			{
			}
		}

		// (the address space free, 0 when it couldn't be read)
		uint64_t sample_status()
		{
			MEMORYSTATUSEX m = {};
			m.dwLength = sizeof(m);
			if (!GlobalMemoryStatusEx(&m))
				return 0;
			p.avail_virtual.store(m.ullAvailVirtual);
			p.total_virtual.store(m.ullTotalVirtual);
			p.avail_phys.store(m.ullAvailPhys);
			lower(p.low_avail_virtual, m.ullAvailVirtual);
			p.status_at.store(GetTickCount64());
			return m.ullAvailVirtual;
		}

		void sample_walk()
		{
			const walk_result w = walk();
			p.free_va.store(w.free);
			p.largest_free.store(w.largest);
			p.walk_us.store(uint64_t(w.ms * 1000.0));
			lower(p.low_largest_free, w.largest);
			p.walk_at.store(GetTickCount64());
		}

		uint8_t bake_running()
		{
			const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (snap == INVALID_HANDLE_VALUE)
				return kBakeUnknown;
			uint8_t out = kBakeNo;
			PROCESSENTRY32W e = {};
			e.dwSize = sizeof(e);
			for (BOOL more = Process32FirstW(snap, &e); more && out != kBakeYes; more = Process32NextW(snap, &e))
			{
				std::wstring line;
				const bool python = _wcsicmp(e.szExeFile, L"python.exe") == 0;
				const bool read = python && command_line(e.th32ProcessID, line);
				const uint8_t one = bake_of(e.szExeFile, read ? line.c_str() : nullptr);
				if (one == kBakeYes || one == kBakeUnknown)
					out = one;
			}
			CloseHandle(snap);
			return out;
		}

		void sample_bake()
		{
			p.bake.store(bake_running());
			p.bake_at.store(GetTickCount64());
		}

		void run()
		{
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			uint64_t next_status = 0, next_walk = 0, next_bake = 0;
			uint64_t high = 0; // (the most address space free since the last walk)
			std::unique_lock<std::mutex> lock(t.mutex);
			while (!t.quit)
			{
				lock.unlock();
				const uint64_t now = GetTickCount64();
				bool by_fall = false;
				if (now >= next_status)
				{
					// (a zone load's peak lasts a second or two, mostly between two 5 s walks: a fall walked at once, so the
					// largest block's low-water mark sees it)
					const uint64_t avail = sample_status();
					by_fall = avail != 0 && fell(avail, high);
					next_status = now + 500;
				}
				if (by_fall || now >= next_walk)
				{
					sample_walk();
					high = p.avail_virtual.load();
					next_walk = now + 5000;
					if (by_fall)
						p.fall_walks.fetch_add(1);
				}
				if (now >= next_bake)
				{
					sample_bake();
					next_bake = now + 10000;
				}
				lock.lock();
				t.wake.wait_for(lock, std::chrono::milliseconds(100), [] { return t.quit; });
			}
		}

		unsigned long long megabytes(uint64_t bytes)
		{
			return static_cast<unsigned long long>((bytes + 524288) / 1048576);
		}
	}

	void start()
	{
		std::lock_guard<std::mutex> lock(t.mutex);
		if (t.thread.joinable())
			return;
		t.quit = false;
		t.thread = std::thread(run);
	}

	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(t.mutex);
			t.quit = true;
		}
		t.wake.notify_all();
		if (t.thread.joinable())
		{
			// (below normal, a probe of its own still running waits for a core while the CPU is busy, a bake or the suites
			// side by side: seconds, the caller the game's thread. Raised for its last steps)
			SetThreadPriority(t.thread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
			t.thread.join();
		}
	}

	values read()
	{
		values v;
		v.avail_virtual = p.avail_virtual.load();
		v.total_virtual = p.total_virtual.load();
		v.avail_phys = p.avail_phys.load();
		v.free_va = p.free_va.load();
		v.largest_free = p.largest_free.load();
		v.walk_ms = double(p.walk_us.load()) / 1000.0;
		v.low_avail_virtual = p.low_avail_virtual.load();
		v.low_largest_free = p.low_largest_free.load();
		v.bake = p.bake.load();
		v.status_at = p.status_at.load();
		v.walk_at = p.walk_at.load();
		v.bake_at = p.bake_at.load();
		v.fall_walks = p.fall_walks.load();
		v.vram_usage = p.vram_usage.load();
		v.vram_budget = p.vram_budget.load();
		v.chars_bytes = p.chars_bytes.load();
		v.zone_bytes = p.zone_bytes.load();
		return v;
	}

	void reset_low_water()
	{
		p.low_avail_virtual.store(p.avail_virtual.load());
		p.low_largest_free.store(p.largest_free.load());
	}

	void set_gpu(uint64_t vram_usage, uint64_t vram_budget, uint64_t chars_bytes, uint64_t zone_bytes)
	{
		p.vram_usage.store(vram_usage);
		p.vram_budget.store(vram_budget);
		p.chars_bytes.store(chars_bytes);
		p.zone_bytes.store(zone_bytes);
	}

	void sample_now()
	{
		sample_status();
		sample_walk();
		sample_bake();
	}

	bool fell(uint64_t avail, uint64_t &high)
	{
		high = std::max(high, avail);
		return avail + kFallWalk < high;
	}

	walk_result walk()
	{
		LARGE_INTEGER t0, t1, f;
		QueryPerformanceCounter(&t0);
		SYSTEM_INFO si = {};
		GetSystemInfo(&si);
		walk_result out;
		uintptr_t at = 0x10000;
		const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
		MEMORY_BASIC_INFORMATION m = {};
		while (at < end && VirtualQuery(reinterpret_cast<const void *>(at), &m, sizeof(m)) == sizeof(m))
		{
			if (m.State == MEM_FREE)
			{
				out.free += m.RegionSize;
				out.largest = std::max<uint64_t>(out.largest, m.RegionSize);
			}
			const uintptr_t next = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
			if (next <= at)
				break; // (the top of the address space: wrapped)
			at = next;
		}
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&f);
		out.ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
		return out;
	}

	uint8_t bake_of(const wchar_t *exe, const wchar_t *command_line)
	{
		if (exe == nullptr)
			return kBakeNo;
		if (_wcsicmp(exe, L"realesrgan-ncnn-vulkan.exe") == 0)
			return kBakeYes;
		if (_wcsicmp(exe, L"python.exe") != 0)
			return kBakeNo;
		if (command_line == nullptr)
			return kBakeUnknown;
		std::wstring lower(command_line);
		for (wchar_t &c : lower)
			c = wchar_t(towlower(c));
		return lower.find(L"bakeall.py") != std::wstring::npos ? kBakeYes : kBakeNo;
	}

	bool command_line(uint32_t pid, std::wstring &out)
	{
		out.clear();
		// (ProcessCommandLineInformation, 60: a UNICODE_STRING and its characters after it; Windows 8.1 on)
		using query_fn = LONG(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
		static const query_fn query = reinterpret_cast<query_fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
		if (query == nullptr)
			return false;
		const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (h == nullptr)
			return false;
		struct unicode_string
		{
			USHORT length, maximum;
			const wchar_t *buffer;
		};
		std::vector<uint8_t> buffer(4096);
		ULONG need = 0;
		LONG status = query(h, 60, buffer.data(), ULONG(buffer.size()), &need);
		if (status == LONG(0xC0000004) && need > buffer.size() && need <= (1u << 20)) // (STATUS_INFO_LENGTH_MISMATCH)
		{
			buffer.resize(need);
			status = query(h, 60, buffer.data(), ULONG(buffer.size()), &need);
		}
		CloseHandle(h);
		if (status < 0 || buffer.size() < sizeof(unicode_string))
			return false;
		const unicode_string *s = reinterpret_cast<const unicode_string *>(buffer.data());
		const uint8_t *first = reinterpret_cast<const uint8_t *>(s->buffer);
		if (s->buffer == nullptr || first < buffer.data() || first + s->length > buffer.data() + buffer.size())
			return false;
		out.assign(s->buffer, s->length / sizeof(wchar_t));
		return true;
	}
}

namespace hifi::pack
{
	std::string crash_section()
	{
		namespace mw = memwatch;
		using mw::megabytes;
		const mw::values v = mw::read();
		const uint64_t now = GetTickCount64();
		std::string out = "Texture pack:\n";
		char line[400];
		if (v.status_at == 0)
			out += "  memory: not measured yet\n";
		else
		{
			std::snprintf(line, sizeof(line), "  address space: %llu MB of %llu MB free (lowest this zone %llu MB), %.1f s ago; physical memory free %llu MB\n",
				megabytes(v.avail_virtual), megabytes(v.total_virtual), megabytes(v.low_avail_virtual), double(now - v.status_at) / 1000.0, megabytes(v.avail_phys));
			out += line;
		}
		if (v.walk_at != 0)
		{
			std::snprintf(line, sizeof(line), "  largest free block: %llu MB (lowest this zone %llu MB), free in all %llu MB; walked in %.2f ms, %.1f s ago\n",
				megabytes(v.largest_free), megabytes(v.low_largest_free), megabytes(v.free_va), v.walk_ms, double(now - v.walk_at) / 1000.0);
			out += line;
		}
		if (v.vram_budget != 0)
		{
			std::snprintf(line, sizeof(line), "  video memory: the game used %llu MB of %llu MB (the last half second)\n", megabytes(v.vram_usage), megabytes(v.vram_budget));
			out += line;
		}
		std::snprintf(line, sizeof(line), "  on the GPU: characters %llu MB, this zone %llu MB\n", megabytes(v.chars_bytes), megabytes(v.zone_bytes));
		out += line;
		const char *bake = v.bake == mw::kBakeYes ? "yes (this session is kept out of the hang analysis)" : v.bake == mw::kBakeNo ? "no" :
			v.bake == mw::kBakeUnknown ? "can't tell (a python.exe's command line couldn't be read)" : "not checked yet";
		std::snprintf(line, sizeof(line), "  a texture bake running (bakeall.py, realesrgan-ncnn-vulkan): %s\n", bake);
		out += line;
		// The streamer's crumbs (stream_crumbs.hpp: atomics the worker writes; no lock, no device or fence call)
		namespace sc = stream_crumbs;
		const sc::crumbs &c = sc::the;
		const uint32_t state = c.state.load();
		if (c.started.load() == 0 && state == sc::kNotStarted)
		{
			out += "  streamer: not started\n";
			return out;
		}
		std::snprintf(line, sizeof(line), "  streamer: %s, %s; epoch %u, paused 0x%x; %u queued, %u copies in flight, %u textures resident (%llu MB), %u refused\n",
			c.started.load() != 0 ? "running" : "stopped", sc::state_text(state), c.epoch.load(), c.pause.load(), c.queued.load(), c.in_flight.load(),
			c.resident.load(), megabytes(c.resident_bytes.load()), c.refused.load());
		out += line;
		std::snprintf(line, sizeof(line), "  streamer's last record: key %016llx, pack-%03u.bin at %llu, %llu bytes, levels %u to %u\n",
			static_cast<unsigned long long>(c.last_hash.load()), c.last_file.load(), static_cast<unsigned long long>(c.last_offset.load()),
			static_cast<unsigned long long>(c.last_bytes.load()), c.last_first.load(), c.last_first.load() + (c.last_count.load() != 0 ? c.last_count.load() - 1 : 0));
		out += line;
		const uint64_t tick = c.progress_tick.load();
		std::snprintf(line, sizeof(line), "  streamer's copy fence: signalled %llu, completed %llu (as the worker last read it); upload ring %llu MB, head %llu, tail %llu; last step %.1f s ago\n",
			static_cast<unsigned long long>(c.signalled.load()), static_cast<unsigned long long>(c.completed.load()), megabytes(c.ring_size.load()),
			static_cast<unsigned long long>(c.ring_head.load()), static_cast<unsigned long long>(c.ring_tail.load()), tick != 0 && now >= tick ? double(now - tick) / 1000.0 : 0.0);
		out += line;
		return out;
	}
}
