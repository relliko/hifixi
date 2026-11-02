/*
 * The texture pack's memory watch (src/pack/memwatch) and its section in the GPU crash report:
 *   - the probes once, at once: the address space, its free total and largest block, the physical memory;
 *   - 256 MB reserved: the free address space (GlobalMemoryStatusEx and the walk) drops by that, within 16 MB, and back
 *     once it's let go; reserved at the largest block's start, the largest block drops by at most that; the low-water
 *     marks keep the lowest until reset;
 *   - the walk under 5 ms (the best of 5);
 *   - the bake check's matcher on a fixed list of processes; a command line read from this process and from its parent
 *     (a 64-bit one, from this 32-bit one);
 *   - the thread: its first values (within 5 s, the suites running side by side), stop() within 200 ms;
 *   - a fall of the free address space between two walks walked at once (the low-water mark of the largest block);
 *   - gpu_crash: the section added once however often it's added, the report's sections with "Texture pack:" and the
 *     free address space; crash_section() read on another thread while the probes run, never waiting;
 *   - gpu_crash::video_memory: false with no device; on WARP, told, its budget the adapter's own (asked here), and
 *     memory_over saying it.
 *
 *   build\memwatch_test.exe     (built by build.cmd)
 */

#include <windows.h>
#include <tlhelp32.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../src/pack/memwatch.hpp"
#include "../src/render/gpu_crash.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

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

	namespace mw = hifi::pack::memwatch;
	constexpr uint64_t kMB = 1048576, kReserve = 256 * kMB, kSlack = 16 * kMB;

	std::string mb(uint64_t bytes)
	{
		return std::to_string((bytes + kMB / 2) / kMB) + " MB";
	}

	bool close_to(uint64_t a, uint64_t b, uint64_t slack)
	{
		return (a > b ? a - b : b - a) <= slack;
	}

	double ms_since(LARGE_INTEGER t0)
	{
		LARGE_INTEGER t1, f;
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&f);
		return double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
	}

	// The largest free block's start (the walk's own rule: MEM_FREE regions from 64 KB up)
	uintptr_t largest_block_at(uint64_t &size)
	{
		SYSTEM_INFO si = {};
		GetSystemInfo(&si);
		uintptr_t at = 0x10000, best = 0;
		size = 0;
		MEMORY_BASIC_INFORMATION m = {};
		while (at < reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress) && VirtualQuery(reinterpret_cast<const void *>(at), &m, sizeof(m)) == sizeof(m))
		{
			if (m.State == MEM_FREE && m.RegionSize > size)
			{
				size = m.RegionSize;
				best = reinterpret_cast<uintptr_t>(m.BaseAddress);
			}
			const uintptr_t next = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
			if (next <= at)
				break;
			at = next;
		}
		return best;
	}

	DWORD parent_pid()
	{
		const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return 0;
		PROCESSENTRY32W e = {};
		e.dwSize = sizeof(e);
		DWORD parent = 0;
		for (BOOL more = Process32FirstW(snap, &e); more; more = Process32NextW(snap, &e))
			if (e.th32ProcessID == GetCurrentProcessId())
				parent = e.th32ParentProcessID;
		CloseHandle(snap);
		return parent;
	}

	int count_of(const std::string &text, const char *what)
	{
		int n = 0;
		for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
			n++;
		return n;
	}
}

int main()
{
	// The probes once
	mw::sample_now();
	mw::values v = mw::read();
	check(v.avail_virtual > 0 && v.total_virtual >= v.avail_virtual && v.avail_phys > 0 && v.status_at != 0,
		"the address space: " + mb(v.avail_virtual) + " of " + mb(v.total_virtual) + " free; physical " + mb(v.avail_phys) + " free");
	check(v.free_va > 0 && v.largest_free > 0 && v.largest_free <= v.free_va && close_to(v.free_va, v.avail_virtual, 64 * kMB),
		"the walk: " + mb(v.free_va) + " free in all (GlobalMemoryStatusEx: " + mb(v.avail_virtual) + "), the largest block " + mb(v.largest_free));
	check(v.low_avail_virtual == v.avail_virtual && v.low_largest_free == v.largest_free, "the low-water marks start at the first values");

	// 256 MB reserved, then let go
	{
		const mw::walk_result before = mw::walk();
		mw::sample_now();
		const uint64_t avail_before = mw::read().avail_virtual;
		void *r = VirtualAlloc(nullptr, kReserve, MEM_RESERVE, PAGE_NOACCESS);
		check(r != nullptr, "256 MB reserved");
		mw::sample_now();
		const mw::values held = mw::read();
		const mw::walk_result during = mw::walk();
		check(close_to(avail_before - held.avail_virtual, kReserve, kSlack), "  the free address space (GlobalMemoryStatusEx) down by " + mb(avail_before - held.avail_virtual));
		check(close_to(before.free - during.free, kReserve, kSlack), "  the walk's free total down by " + mb(before.free - during.free));
		check(held.low_avail_virtual == held.avail_virtual, "  the low-water mark follows it down: " + mb(held.low_avail_virtual));
		VirtualFree(r, 0, MEM_RELEASE);
		mw::sample_now();
		const mw::values freed = mw::read();
		const mw::walk_result after = mw::walk();
		check(close_to(freed.avail_virtual, avail_before, kSlack) && close_to(after.free, before.free, kSlack), "  let go: back to " + mb(freed.avail_virtual) + " (walk " + mb(after.free) + ")");
		check(freed.low_avail_virtual == held.avail_virtual, "  the low-water mark kept at the lowest: " + mb(freed.low_avail_virtual));
		mw::reset_low_water();
		check(mw::read().low_avail_virtual == freed.avail_virtual && mw::read().low_largest_free == freed.largest_free, "  reset: from the values now");
	}
	{
		uint64_t size = 0;
		const uintptr_t start = largest_block_at(size);
		const uintptr_t at = (start + 0xFFFF) & ~uintptr_t(0xFFFF); // (a reservation starts on 64 KB)
		const uint64_t largest_before = mw::walk().largest;
		if (start != 0 && size >= (at - start) + kReserve + kSlack)
		{
			void *r = VirtualAlloc(reinterpret_cast<void *>(at), kReserve, MEM_RESERVE, PAGE_NOACCESS);
			const uint64_t largest_during = mw::walk().largest;
			check(r != nullptr && largest_during < largest_before && largest_before - largest_during <= kReserve + kSlack,
				"256 MB reserved at the largest block's start (" + mb(largest_before) + "): the largest now " + mb(largest_during));
			if (r != nullptr)
				VirtualFree(r, 0, MEM_RELEASE);
			check(mw::walk().largest == largest_before, "  let go: the largest block " + mb(largest_before) + " again");
		}
		else
			std::printf("--    the largest block (%s) too small: its check skipped\n", mb(size).c_str());
	}

	// The walk's time
	double best = 1e9;
	for (int i = 0; i < 5; ++i)
		best = std::min(best, mw::walk().ms);
	char line[200];
	std::snprintf(line, sizeof(line), "the walk takes %.3f ms (the best of 5; under 5)", best);
	check(best < 5.0, line);

	// The early walk's rule
	{
		uint64_t high = 0;
		const bool first = mw::fell(1800 * kMB, high);
		const bool slight = mw::fell(1800 * kMB - mw::kFallWalk, high);
		const bool risen = mw::fell(1900 * kMB, high);
		const bool steep = mw::fell(1900 * kMB - mw::kFallWalk - kMB, high);
		check(!first && !slight && !risen && steep && high == 1900 * kMB,
			"fell: not at the first sample, not by " + mb(mw::kFallWalk) + ", from the most seen since the walk (1900 MB) when by more");
	}

	// The bake check
	struct bake_case
	{
		const wchar_t *exe, *line;
		uint8_t want;
	};
	const bake_case cases[] = {
		{ L"realesrgan-ncnn-vulkan.exe", nullptr, mw::kBakeYes },
		{ L"RealESRGAN-NCNN-Vulkan.EXE", L"realesrgan-ncnn-vulkan -i a.png -o b.png", mw::kBakeYes },
		{ L"python.exe", L"python -I C:\\code\\LSB\\research\\ffxi-re\\tools\\bakeall.py --models", mw::kBakeYes },
		{ L"python.exe", L"\"C:\\Python313\\python.exe\" tools\\BakeAll.py", mw::kBakeYes },
		{ L"python.exe", L"python -I tests\\run_suites.py", mw::kBakeNo },
		{ L"python.exe", nullptr, mw::kBakeUnknown },
		{ L"pythonw.exe", L"pythonw bakeall.py", mw::kBakeNo },
		{ L"notepad.exe", L"notepad bakeall.py", mw::kBakeNo },
		{ L"realesrgan.exe", nullptr, mw::kBakeNo },
	};
	bool all = true;
	for (const bake_case &c : cases)
		if (mw::bake_of(c.exe, c.line) != c.want)
		{
			all = false;
			std::wprintf(L"      %ls \"%ls\": %u, not %u\n", c.exe, c.line != nullptr ? c.line : L"(not read)", unsigned(mw::bake_of(c.exe, c.line)), unsigned(c.want));
		}
	check(all, "the bake check's matcher: " + std::to_string(sizeof(cases) / sizeof(cases[0])) + " processes (the realesrgan exe, python.exe with bakeall.py; unknown when a python.exe's line can't be read)");
	std::wstring own;
	check(mw::command_line(GetCurrentProcessId(), own) && own.find(L"memwatch_test") != std::wstring::npos, "this process's command line read");
	if (const DWORD parent = parent_pid(); parent != 0)
	{
		std::wstring theirs;
		const bool read = mw::command_line(parent, theirs);
		check(read && !theirs.empty(), "the parent's command line read (a 64-bit process): " + std::string(read ? "yes" : "no") + ", " + std::to_string(theirs.size()) + " characters");
	}
	else
		std::printf("--    no parent process found: its check skipped\n");
	check(mw::read().bake != mw::kBakeNotChecked, "the bake checked: " + std::to_string(unsigned(mw::read().bake)) + " (1 no, 2 yes, 3 can't tell)");

	// The thread
	{
		// (past the tick count's step first, so the thread's first sample reads later than the last; up to 5 s for it: a
		// below-normal thread waits its turn while the suites run side by side. stop() timed once all three first probes
		// are done, the thread in its wait: what memwatch.hpp promises)
		const mw::values was = mw::read();
		Sleep(50);
		mw::start();
		bool fresh = false;
		// (up to 20 s: Windows lifts a starved thread after about 4, and the bake check reads every process's command line)
		for (const ULONGLONG until = GetTickCount64() + 20000; !fresh && GetTickCount64() < until;)
		{
			Sleep(10);
			const mw::values now = mw::read();
			fresh = now.status_at > was.status_at && now.walk_at > was.walk_at && now.bake_at > was.bake_at;
		}
		check(fresh, "the thread's first values in time (the status, the walk and the bake check)");
		// (256 MB reserved right after its first walk: the next status sample, half a second on, sees the fall before the
		// next 5 s walk, however late the thread gets its turn, the status sampled first)
		const mw::values first = mw::read();
		void *r = VirtualAlloc(nullptr, kReserve, MEM_RESERVE, PAGE_NOACCESS);
		bool early = false;
		for (int i = 0; i < 1000 && !early && r != nullptr; ++i)
		{
			Sleep(10);
			early = mw::read().fall_walks > first.fall_walks;
		}
		const mw::values walked = mw::read();
		if (r != nullptr)
			VirtualFree(r, 0, MEM_RELEASE);
		check(r != nullptr && early && walked.walk_at > first.walk_at && close_to(first.free_va - walked.free_va, kReserve, kSlack),
			"256 MB reserved: walked " + std::to_string(walked.walk_at - first.walk_at) + " ms after the last walk, not 5 s (free in all down by " +
				mb(first.free_va - walked.free_va) + ")");
		Sleep(150);
		LARGE_INTEGER t0;
		QueryPerformanceCounter(&t0);
		mw::stop();
		const double stop_ms = ms_since(t0);
		std::snprintf(line, sizeof(line), "stop() joined it in %.1f ms (under 200)", stop_ms);
		check(stop_ms < 200.0, line);
		mw::start();
		mw::stop();
		check(true, "started and stopped again");
	}

	// The GPU crash report's section
	gpu_crash::add_section(hifi::pack::crash_section);
	gpu_crash::add_section(hifi::pack::crash_section);
	const std::string sections = gpu_crash::sections_text();
	check(count_of(sections, "Texture pack:") == 1, "added twice, in the report once");
	check(sections.find("address space:") != std::string::npos && sections.find("largest free block:") != std::string::npos &&
			sections.find("a texture bake running") != std::string::npos && sections.find("streamer: none yet") != std::string::npos,
		"the report's sections: the free address space, the largest block, the bake, the streamer");
	std::printf("%s", sections.c_str());
	{
		// (read on another thread over and over while the probes run on theirs and this one: never waiting)
		mw::start();
		std::atomic<bool> done { false };
		double worst = 0.0;
		int reads = 0;
		std::thread reader([&] {
			for (; reads < 300; ++reads)
			{
				LARGE_INTEGER t0;
				QueryPerformanceCounter(&t0);
				const std::string s = hifi::pack::crash_section();
				worst = std::max(worst, ms_since(t0));
				if (s.find("Texture pack:") != 0)
					break;
			}
			done = true;
		});
		while (!done)
		{
			mw::sample_now();
			mw::reset_low_water();
		}
		reader.join();
		mw::stop();
		std::snprintf(line, sizeof(line), "crash_section() on another thread while the probes run: %d reads, the slowest %.2f ms", reads, worst);
		check(reads == 300 && worst < 20.0, line);
	}

	// Video memory through gpu_crash's kept adapter
	uint64_t usage = 0, budget = 0;
	check(!gpu_crash::video_memory(nullptr, usage, budget) && usage == 0 && budget == 0, "video_memory: false with no device");
	{
		IDXGIFactory4 *factory = nullptr;
		IDXGIAdapter *warp = nullptr;
		ID3D12Device *device = nullptr;
		if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
			SUCCEEDED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
		{
			// (against the WARP adapter's own answer, asked here: memory_over reads video_memory, so it can't tell)
			const bool known = gpu_crash::video_memory(device, usage, budget);
			IDXGIAdapter3 *adapter = nullptr;
			DXGI_QUERY_VIDEO_MEMORY_INFO direct = {};
			const bool asked = SUCCEEDED(warp->QueryInterface(IID_PPV_ARGS(&adapter))) && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &direct));
			if (adapter != nullptr)
				adapter->Release();
			check(known && budget > 0 && asked && close_to(budget, direct.Budget, 64 * kMB),
				"video_memory on WARP: " + (known ? mb(usage) + " of " + mb(budget) : std::string("not told")) + "; the adapter itself: " +
					(asked ? mb(direct.CurrentUsage) + " of " + mb(direct.Budget) : std::string("not told")));
			std::string over;
			gpu_crash::memory_over(device, over);
			check(over.find("of the " + mb(budget)) != std::string::npos, "  memory_over with it: " + over);
			device->Release();
		}
		else
			std::printf("--    no WARP device: video_memory's device check skipped\n");
		if (warp != nullptr)
			warp->Release();
		if (factory != nullptr)
			factory->Release();
	}

	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
