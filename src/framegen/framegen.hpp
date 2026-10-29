#pragma once
// AMD FSR 3 frame generation (research/ffxi-re/notes-framegen.md; the FidelityFX SDK v1.1.4 in src/framegen/ffx, built
// with HiFi XI). Spike, step 1: dgVoodoo's swap chain for the game's window made as AMD's frame interpolation swap chain
// (its own present queue and pacing threads around a real FLIP_DISCARD swap chain), frame generation off: the game drawn
// and presented through it as before. DXGI's factory's CreateSwapChain and CreateSwapChainForHwnd go through ours from
// HiFi XI's early start (before dgVoodoo makes its device and swap chain); only with HIFIXI_FRAMEGEN=1 for now.
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::framegen
{
	// At HiFi XI's early start, with DXGI's own CreateDXGIFactory1 (the starter's): the factory's swap chain calls through
	// ours, when wanted. False with why when they couldn't be; true (nothing done) when not wanted.
	// log: HiFi XI's log (level 0 info, 1 warning, 2 error), told of each swap chain made through ours.
	bool install(void *create_factory1, std::string &why, void (*log)(int level, const char *text) = nullptr);
	bool wanted();
	std::vector<std::string> status();
}
