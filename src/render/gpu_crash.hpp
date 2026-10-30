/*
 * What the GPU was doing when it stopped (the device removed: hung, a page fault, a reset), from Direct3D
 * 12's own record (DRED): turned on before the device is created, it notes each command list's operations
 * as the GPU finishes them (the add-on's own work is marked "worldfx: ...") and, on a page fault, the address
 * and what memory is or was there. When the device is lost the record goes to a text file next to the add-on.
 */

#pragma once

#include <d3d12.h>
#include <string>

namespace gpu_crash
{
	// Before D3D12CreateDevice (ReShade's create_device event): turns the recording on for the devices
	// created after. False (and why in status) when it can't be.
	bool enable(std::string &status);

	// Once the device exists: when it's removed, the report goes to path and a line to log (on a thread
	// pool thread, once), with what context() says (the add-on's state) at its end. Again for another
	// device moves the watch there.
	void watch(ID3D12Device *device, const std::wstring &path, void (*log)(const char *line), std::string (*context)());
	// Before the device is released
	void unwatch();

	// The report for a removed device (the reason, the GPU and its video memory then, the page fault, the command
	// lists the GPU hadn't finished)
	std::string report(ID3D12Device *device);

	// The GPU a device runs on, for the log and the report: its name, PCI ids and driver version ("NVIDIA GeForce RTX
	// 4080 (vendor 10DE, device 2704), driver 32.0.15.6094 (NVIDIA 560.94)"); and its video memory now: what this
	// process uses of the budget Windows gives it, on the card and shared. Each says so when DXGI can't tell.
	std::string gpu_name(ID3D12Device *device);
	std::string gpu_memory(ID3D12Device *device);
	// What the device supports that HiFi XI's passes lean on, for the log and reports from other GPUs ("feature level
	// 12_1, shader model 6.7, resource binding tier 3, typed UAV loads yes, ...")
	std::string gpu_features(ID3D12Device *device);
	// Whether the game is over the video memory Windows gives it on the card now (line: as gpu_memory says it; false
	// and line empty when DXGI can't tell). The adapter is kept between calls: cheap enough every few seconds.
	bool memory_over(ID3D12Device *device, std::string &line);

	// Direct3D 12's debug layer (a diagnostic: it checks every call, the game's and the add-on's, and costs
	// CPU time), before D3D12CreateDevice like enable(). False when it isn't installed (Windows' optional
	// "Graphics Tools").
	bool enable_debug_layer(std::string &status);
	// At each present: the debug layer's new messages to path (the first few of each kind in full, then
	// counts), written as they come so they survive a crash
	void write_debug_messages(ID3D12Device *device, const std::wstring &path);

	// A marker in a command list, shown in the report where the GPU got to ("worldfx: ...")
	inline void mark(ID3D12GraphicsCommandList *list, const wchar_t *text)
	{
		// (metadata 0: a UTF-16 string, ending in its 0)
		UINT n = 0;
		while (text[n] != 0)
			++n;
		list->SetMarker(0, text, (n + 1) * sizeof(wchar_t));
	}
}
