#pragma once

// The game's memory watched for the texture pack (docs/texture-pack-plan.md §5), on a thread of its own so the game's
// thread never waits on a probe:
//   - GlobalMemoryStatusEx every half second: the address space left (phoenix-loader.exe is large-address-aware: 4 GB,
//     shared by the game, Ashita, Direct3D 12 and the driver) and the physical memory free;
//   - a VirtualQuery walk every 5 s: all the free address space and its largest block (what a big allocation needs);
//     and at once when the address space free has fallen kFallWalk since the last walk (a zone load's peak lasts a
//     second or two: between two 5 s walks the largest block's low-water mark wouldn't see it);
//   - every 10 s, whether a texture bake is running (realesrgan-ncnn-vulkan.exe, or a python.exe running bakeall.py):
//     sessions played meanwhile are kept out of the hang analysis. A python.exe whose command line can't be read makes
//     it "can't tell", not "no";
//   - every 3 s, once the module has named the pack folder (set_pack_folder): its progress.txt as a texture pack being
//     made there wrote it (hifixi-enhanced-textures: texpack make), and whether pack.lock is there (a builder holds it
//     while it runs; deleted when it ends, however it ends). Only the text: the module reads it (pack::build_line).
// Each value is an atomic (read with no lock: the GPU crash and hang reports read them from their own threads). The
// lowest free address space and largest block are kept since the last reset_low_water (the module resets them at each
// zone change, after logging the zone's). Video memory isn't read here: the module reads it on the game's thread
// (gpu_crash::video_memory, its adapter kept) and hands it over with the characters' and the zone's bytes.

#include <cstdint>
#include <string>

namespace hifi::pack::memwatch
{
	// The thread: start() once (again after stop() starts it anew); stop() joins it, within 100 ms of a probe's end (the
	// thread raised above normal for it)
	void start();
	void stop();

	// How far the address space free falls before the thread walks at once (fell)
	constexpr uint64_t kFallWalk = 32ull << 20;

	enum : uint8_t
	{
		kBakeNotChecked = 0,
		kBakeNo,
		kBakeYes,
		kBakeUnknown, // (a python.exe's command line couldn't be read)
	};

	struct values
	{
		uint64_t avail_virtual = 0, total_virtual = 0, avail_phys = 0; // (GlobalMemoryStatusEx)
		uint64_t free_va = 0, largest_free = 0;                        // (the walk)
		double walk_ms = 0.0;
		uint64_t low_avail_virtual = 0, low_largest_free = 0;         // (lowest since reset_low_water; 0 before a sample)
		uint8_t bake = kBakeNotChecked;
		uint64_t status_at = 0, walk_at = 0, bake_at = 0;            // (GetTickCount64 of each probe's last run; 0 never)
		uint32_t fall_walks = 0;                                     // (walks made early, the address space having fallen)
		uint64_t vram_usage = 0, vram_budget = 0, chars_bytes = 0, zone_bytes = 0; // (from the module, set_gpu)
	};
	// The last of each, lock-free (any thread)
	values read();
	// The low-water marks started again from the values now (the module, at a zone change)
	void reset_low_water();
	// The module's numbers, mirrored for the reports: video memory used and its budget, the characters' and the zone's
	// bytes on the GPU
	void set_gpu(uint64_t vram_usage, uint64_t vram_budget, uint64_t chars_bytes, uint64_t zone_bytes);

	// The pack folder whose progress.txt the thread reads (with its slash; the module, at each look). "" reads none
	void set_pack_folder(const std::wstring &folder);
	// A pack being made there, as the thread last read it: progress.txt's text (at most kBuildTextMax bytes) and
	// whether pack.lock was there. there false: no progress.txt (or no folder named yet)
	constexpr size_t kBuildTextMax = 16384;
	struct build_file
	{
		bool there = false, locked = false;
		std::string text;
		uint64_t at = 0; // (GetTickCount64 of the read; 0 never)
	};
	build_file read_build();

	// For the tests: every probe once (progress.txt too), now, on the calling thread (published as the thread would)
	void sample_now();
	// The walk alone: the free address space, its largest block, the time it took
	struct walk_result
	{
		uint64_t free = 0, largest = 0;
		double ms = 0.0;
	};
	walk_result walk();
	// The thread's rule for an early walk: high kept as the most address space free seen since the last walk (the caller
	// sets it to what's free after each walk); true when avail is more than kFallWalk below it
	bool fell(uint64_t avail, uint64_t &high);
	// One process's part in the bake check: its exe's name and, for a python.exe, its command line (nullptr when it
	// couldn't be read): kBakeYes, kBakeUnknown or kBakeNo
	uint8_t bake_of(const wchar_t *exe, const wchar_t *command_line);
	// A process's command line (NtQueryInformationProcess, PROCESS_QUERY_LIMITED_INFORMATION); false when it can't be read
	bool command_line(uint32_t pid, std::wstring &out);
}

namespace hifi::pack
{
	// The GPU crash report's section (gpu_crash::add_section) and the hang report's lines: "Texture pack:" and what the
	// watch last saw. Reads atomics only: no lock, no device or DXGI call (it runs on the report's thread pool thread,
	// the game's thread perhaps stuck).
	std::string crash_section();
}
