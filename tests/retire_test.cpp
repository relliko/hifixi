/*
 * Offline test of GPU objects let go behind the GPU (src/host/retire.hpp), on WARP:
 *   - held before the first present (no fence yet), then counted from the fence's signals;
 *   - the queue held back (waiting on a fence of the test's own): nothing let go however many presents pass; let go once
 *     the GPU has caught up;
 *   - one handed over is kept past the next signal and let go once the fence passes the one after;
 *   - an owner (unique_ptr) destroyed then, not before; let_go sets the pointer to nullptr;
 *   - a new device: the last one's objects let go at its first present;
 *   - release_all (the unload): one more signal waited for; one handed over before the last present let go, one since
 *     left, never freed.
 *
 *   build\retire_test.exe     (built by build.cmd)
 */

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../src/host/retire.hpp"
#include <cstdio>
#include <memory>
#include <string>

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

	// A stand-in COM object that counts its Release
	struct probe : IUnknown
	{
		int released = 0;
		HRESULT __stdcall QueryInterface(REFIID, void **out) override
		{
			*out = nullptr;
			return E_NOINTERFACE;
		}
		ULONG __stdcall AddRef() override { return 2; }
		ULONG __stdcall Release() override
		{
			released++;
			return 1;
		}
	};

	struct owner
	{
		int *destroyed;
		explicit owner(int *d) : destroyed(d) {}
		~owner() { ++*destroyed; }
	};

	struct gpu
	{
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		// (WARP; or the first hardware adapter: D3D12 hands out one device per adapter, so a second device is another's)
		bool make(IDXGIFactory4 *factory, bool hardware = false)
		{
			IDXGIAdapter1 *adapter = nullptr;
			if (hardware ? FAILED(factory->EnumAdapters1(0, &adapter)) : FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))))
				return false;
			DXGI_ADAPTER_DESC1 ad = {};
			adapter->GetDesc1(&ad);
			const bool ok = !(hardware && (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) &&
				SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
			adapter->Release();
			D3D12_COMMAND_QUEUE_DESC qd = {};
			qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			return ok && SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
		}
	};

	// The GPU past the fence's last signal (at most 5 s)
	bool caught_up()
	{
		for (int i = 0; i < 500; ++i)
		{
			const hifi::retire::counts c = hifi::retire::status();
			if (c.completed >= c.signalled)
				return true;
			Sleep(10);
		}
		return false;
	}
}

int main()
{
	namespace rt = hifi::retire;
	IDXGIFactory4 *factory = nullptr;
	gpu a, b;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !a.make(factory))
	{
		std::printf("FAIL  no WARP device\n");
		return 1;
	}
	const bool second = b.make(factory, true);

	// Before the first present
	probe early;
	rt::later(&early);
	check(rt::status().held == 1 && early.released == 0, "held before the first present (no fence yet)");

	// The queue held back: the fence's signals stay behind it
	ID3D12Fence *gate = nullptr;
	a.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate));
	a.queue->Wait(gate, 1);
	for (int i = 0; i < 6; ++i)
		rt::on_present(a.queue);
	Sleep(50);
	rt::on_present(a.queue);
	check(early.released == 0 && rt::status().signalled == 7 && rt::status().completed == 0,
		"the queue held back: 7 presents, nothing let go (the fence at " + std::to_string(rt::status().completed) + ")");
	gate->Signal(1);
	check(caught_up(), "  the gate opened: the GPU catches up");
	rt::on_present(a.queue);
	check(early.released == 1 && rt::status().held == 0, "  then let go at the next present");

	// Kept past the next signal, let go behind the one after
	check(caught_up(), "the GPU idle");
	probe one;
	rt::later(&one);
	const uint64_t at = rt::status().signalled;
	rt::on_present(a.queue);
	check(one.released == 0, "handed over at signal " + std::to_string(at) + ": kept through the next present");
	check(caught_up(), "  the GPU past signal " + std::to_string(rt::status().signalled));
	rt::on_present(a.queue);
	rt::on_present(a.queue);
	check(caught_up(), "  two more signals");
	rt::on_present(a.queue);
	check(one.released == 1, "  let go once the fence passed signal " + std::to_string(at + 2));

	// An owner, and let_go
	int destroyed = 0;
	rt::later(std::make_unique<owner>(&destroyed));
	probe slot_object;
	IUnknown *slot = &slot_object;
	rt::let_go(slot);
	check(slot == nullptr && destroyed == 0 && slot_object.released == 0, "an owner and a slot handed over: the slot nullptr, nothing gone yet");
	for (int i = 0; i < 4; ++i)
	{
		rt::on_present(a.queue);
		caught_up();
	}
	check(destroyed == 1 && slot_object.released == 1, "  the owner destroyed and the slot's object released behind the GPU");

	// A new device (the GPU's): the last one's objects let go at once
	ID3D12CommandQueue *last_queue = a.queue;
	if (second)
	{
		probe old_device;
		rt::later(&old_device);
		a.queue->Wait(gate, 2); // (held back: only the device switch can let it go)
		rt::on_present(a.queue);
		check(old_device.released == 0, "a new device's first present: before it, held");
		rt::on_present(b.queue);
		check(old_device.released == 1 && rt::status().signalled == 1, "  at it, the last device's objects let go, its own fence counting from 1");
		gate->Signal(2);
		last_queue = b.queue;
	}
	else
		std::printf("--    no hardware adapter: the new device's check skipped\n");

	// release_all (the unload): one more signal waited for; what was handed over since the last present left
	probe before, after;
	rt::later(&before);
	rt::on_present(last_queue);
	rt::later(&after);
	rt::release_all();
	check(before.released == 1 && after.released == 0 && rt::status().held == 0,
		"release_all: one handed over before the last present let go, one since left (the open list may hold it)");

	gate->Release();
	for (gpu *g : { &a, &b })
	{
		if (g->queue != nullptr)
			g->queue->Release();
		if (g->device != nullptr)
			g->device->Release();
	}
	factory->Release();
	std::printf(failures == 0 ? "all passed (%d)\n" : "%d failed\n", failures == 0 ? passes : failures);
	return failures == 0 ? 0 : 1;
}
