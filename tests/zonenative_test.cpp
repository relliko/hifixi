/*
 * The native zone renderer's per-zone data as the plugin runs it (src/zone/native.cpp), on WARP: presents with a
 * zone, until it's on the GPU; another zone (the first kept until it's let go); not wanted (let go); stop.
 *
 *   build\zonenative_test.exe
 */

#include <d3d12.h>
#include <dxgi1_4.h>
#include "../src/zone/native.hpp"
#include "../src/zone/zone_gpu.hpp"
#include "../src/host/retire.hpp"
#include "game_dump.hpp"
#include <cstdio>
#include <string>

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
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main()
{
	namespace native = hifi::zone::native;
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
	dump::unmap(base);
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
