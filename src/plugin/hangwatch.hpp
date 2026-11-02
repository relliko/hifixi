/*
 * The hang report: a thread of its own watches the game's presents. When the game has shown nothing for kHangSeconds
 * (after it had been running), it writes hifixi-hang.txt next to the log: where each of the game's threads is (its
 * instruction and the return addresses up its frame chain, as module + offset: build\hifixi.map and build\d3d8\d3d8.map
 * name HiFi XI's), whether Direct3D still has the device, and what the caller adds. Once a stall; a line in the log when
 * it's written and when the presents come back.
 */

#pragma once

#include <string>

namespace hifi::hangwatch
{
	// folder: where the report goes (bootloader\); log: HiFi XI's log (level 2 error); context: more lines for the
	// report, called on the watch thread (nothing in it may wait on a lock the game thread holds)
	void start(const std::wstring &folder, void (*log)(int level, const char *text), std::string (*context)());
	void stop();
	// Each present, on the game thread
	void present();
}
