/*
 * What the GPU was doing when it stopped (DRED). See gpu_crash.hpp.
 */

#include "gpu_crash.hpp"
#include <dxgi1_4.h>
#include <share.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <ctime>
#include <map>
#include <unordered_map>
#include <vector>

namespace gpu_crash
{
	namespace
	{
		// D3D12_AUTO_BREADCRUMB_OP, in order
		const char *const kOps[] = {
			"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch",
			"CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView",
			"ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData",
			"BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64",
			"ResolveSubresourceRegion", "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2",
			"ProcessFrames1", "BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
			"CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand", "EstimateMotion",
			"ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand", "ExecuteExtensionCommand", "DispatchMesh",
			"EncodeFrame", "ResolveEncoderOutputMetadata", "Barrier", "BeginCommandList" };

		// Command lists in the report at most; of those the GPU hadn't finished, the first kFullLists have all
		// their operations (runs of the same one on a line), the rest only where it stopped
		constexpr int kMaxLists = 24, kFullLists = 4;
		constexpr UINT kBefore = 24, kAfter = 8;
		constexpr int kMaxRunLines = 200;

		const char *op_name(UINT op)
		{
			return op < sizeof(kOps) / sizeof(kOps[0]) ? kOps[op] : "?";
		}

		std::string narrow(const wchar_t *w)
		{
			if (w == nullptr || *w == 0)
				return {};
			const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
			if (n <= 1)
				return {};
			std::string s(size_t(n) - 1, '\0');
			WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
			return s;
		}

		std::string name_of(const char *a, const wchar_t *w)
		{
			std::string s = a != nullptr ? std::string(a) : narrow(w);
			return s.empty() ? "unnamed" : s;
		}

		const char *reason_name(HRESULT hr)
		{
			switch (static_cast<unsigned long>(hr))
			{
			case 0x887A0006: return "DXGI_ERROR_DEVICE_HUNG (the GPU timed out or faulted on a command)";
			case 0x887A0005: return "DXGI_ERROR_DEVICE_REMOVED";
			case 0x887A0007: return "DXGI_ERROR_DEVICE_RESET (bad commands from this process)";
			case 0x887A0020: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
			case 0x887A0001: return "DXGI_ERROR_INVALID_CALL";
			case 0x8007000E: return "E_OUTOFMEMORY";
			default: return "another reason";
			}
		}

		void add(std::string &out, const char *format, ...)
		{
			char line[768];
			va_list args;
			va_start(args, format);
			std::vsnprintf(line, sizeof(line), format, args);
			va_end(args);
			out += line;
		}

		void add_allocations(std::string &out, const char *heading, const D3D12_DRED_ALLOCATION_NODE1 *node)
		{
			if (node == nullptr)
			{
				add(out, "  %s: none\n", heading);
				return;
			}
			add(out, "  %s:\n", heading);
			for (int i = 0; node != nullptr && i < 32; node = node->pNext, ++i)
				add(out, "    %s (allocation type %u)\n", name_of(node->ObjectNameA, node->ObjectNameW).c_str(), static_cast<unsigned>(node->AllocationType));
		}

		// The DXGI adapter a device runs on (found by its LUID), or nullptr. Through Windows' own dxgi.dll: in the game's
		// folder that name is HiFi XI's starter.
		IDXGIAdapter3 *adapter_of(ID3D12Device *device)
		{
			static const HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			using create_fn = HRESULT(WINAPI *)(REFIID, void **);
			const auto create = dxgi != nullptr ? reinterpret_cast<create_fn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
			IDXGIFactory4 *factory = nullptr;
			if (create == nullptr || FAILED(create(IID_PPV_ARGS(&factory))))
				return nullptr;
			IDXGIAdapter3 *adapter = nullptr;
			if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
				adapter = nullptr;
			factory->Release();
			return adapter;
		}

		unsigned long long megabytes(UINT64 bytes)
		{
			return static_cast<unsigned long long>((bytes + 524288) / 1048576);
		}

		struct watch_state
		{
			ID3D12Device *device = nullptr;
			ID3D12Fence *fence = nullptr;
			HANDLE event = nullptr;
			HANDLE wait = nullptr;
			std::wstring path;
			void (*log)(const char *line) = nullptr;
			std::string (*context)() = nullptr;
		};
		watch_state *g_watch = nullptr;

		void CALLBACK on_removed(void *context, BOOLEAN)
		{
			const watch_state *const w = static_cast<const watch_state *>(context);
			std::string text = report(w->device);
			if (w->context != nullptr)
				text += "\nThe add-on at that moment:\n" + w->context();
			FILE *f = nullptr;
			const bool written = _wfopen_s(&f, w->path.c_str(), L"wb") == 0 && f != nullptr;
			if (written)
			{
				std::fwrite(text.data(), 1, text.size(), f);
				std::fclose(f);
			}
			if (w->log != nullptr)
			{
				std::string line = text.substr(0, text.find('\n'));
				line += written ? " Report: " + narrow(w->path.c_str()) : std::string(" (the report couldn't be written)");
				w->log(line.c_str());
			}
		}
	}

	bool enable(std::string &status)
	{
		HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
		const auto get = d3d12 != nullptr ? reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
		if (get == nullptr)
		{
			status = "off (d3d12.dll isn't loaded)";
			return false;
		}
		ID3D12DeviceRemovedExtendedDataSettings1 *s1 = nullptr;
		if (SUCCEEDED(get(IID_PPV_ARGS(&s1))))
		{
			s1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			s1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			s1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			s1->Release();
			status = "on (operations, page faults, markers)";
			return true;
		}
		ID3D12DeviceRemovedExtendedDataSettings *s = nullptr;
		if (SUCCEEDED(get(IID_PPV_ARGS(&s))))
		{
			s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			s->Release();
			status = "on (operations, page faults)";
			return true;
		}
		status = "off (this Direct3D 12 has no DRED)";
		return false;
	}

	bool enable_debug_layer(std::string &status)
	{
		HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
		const auto get = d3d12 != nullptr ? reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
		ID3D12Debug *debug = nullptr;
		if (get == nullptr || FAILED(get(IID_PPV_ARGS(&debug))))
		{
			status = "off (not installed: Windows' optional feature \"Graphics Tools\")";
			return false;
		}
		debug->EnableDebugLayer();
		debug->Release();
		status = "on (worldfx-d3d12-debug.txt)";
		return true;
	}

	void write_debug_messages(ID3D12Device *device, const std::wstring &path)
	{
		// (the first kFull of each kind of message in full, then a count every kCountEvery; a kind is the
		// message with its object addresses left out: the same complaint about another list, state or
		// subresource is another kind)
		constexpr uint32_t kFull = 4, kCountEvery = 2000;
		static ID3D12Device *for_device = nullptr;
		static ID3D12InfoQueue *queue = nullptr;
		static FILE *file = nullptr;
		static std::unordered_map<std::string, uint32_t> seen;
		if (device != for_device)
		{
			for_device = device;
			if (queue != nullptr)
				queue->Release();
			queue = nullptr;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
				queue = nullptr;
			if (queue != nullptr)
			{
				D3D12_MESSAGE_SEVERITY deny[] = { D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE };
				D3D12_INFO_QUEUE_FILTER filter = {};
				filter.DenyList.NumSeverities = 2;
				filter.DenyList.pSeverityList = deny;
				queue->AddStorageFilterEntries(&filter);
				queue->SetMessageCountLimit(8192);
			}
			if (file == nullptr)
				file = _wfsopen(path.c_str(), L"wb", _SH_DENYNO); // (readable while the game runs)
			if (file != nullptr)
			{
				std::fprintf(file, "Direct3D 12 debug layer messages (errors, corruption, warnings), the first %u of each kind in full\n", kFull);
				std::fflush(file);
			}
		}
		if (queue == nullptr || file == nullptr)
			return;
		const UINT64 n = queue->GetNumStoredMessages();
		if (n == 0)
			return;
		SYSTEMTIME t = {};
		GetLocalTime(&t);
		std::vector<char> buffer;
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T size = 0;
			if (FAILED(queue->GetMessage(i, nullptr, &size)) || size == 0)
				continue;
			buffer.resize(size);
			auto *m = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
			if (FAILED(queue->GetMessage(i, m, &size)))
				continue;
			std::string kind = std::to_string(static_cast<int>(m->ID)) + ":";
			for (const char *c = m->pDescription; c != nullptr && *c != 0; ++c)
			{
				// (0x and 8 hex digits: an object's address)
				if (c[0] == '0' && c[1] == 'x')
				{
					int digits = 0;
					while (std::isxdigit(static_cast<unsigned char>(c[2 + digits])))
						++digits;
					if (digits >= 8)
					{
						kind += "0x*";
						c += 1 + digits;
						continue;
					}
				}
				kind += *c;
			}
			uint32_t &times = seen[kind];
			++times;
			const char *severity = m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" : m->Severity == D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "WARNING";
			if (times <= kFull)
				std::fprintf(file, "%02u:%02u:%02u.%03u %s #%d: %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, severity, static_cast<int>(m->ID), m->pDescription);
			else if (times % kCountEvery == 0)
				std::fprintf(file, "%02u:%02u:%02u.%03u %s #%d: %u times so far: %.160s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, severity, static_cast<int>(m->ID), times, m->pDescription);
		}
		queue->ClearStoredMessages();
		std::fflush(file);
	}

	void watch(ID3D12Device *device, const std::wstring &path, void (*log)(const char *line), std::string (*context)())
	{
		unwatch();
		auto *w = new watch_state;
		w->device = device;
		w->path = path;
		w->log = log;
		w->context = context;
		// (a removed device completes every fence to UINT64_MAX: that sets the event)
		w->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (w->event == nullptr || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&w->fence))) ||
			FAILED(w->fence->SetEventOnCompletion(UINT64_MAX, w->event)) ||
			!RegisterWaitForSingleObject(&w->wait, w->event, on_removed, w, INFINITE, WT_EXECUTEONLYONCE))
		{
			w->wait = nullptr;
			g_watch = w;
			unwatch();
			return;
		}
		g_watch = w;
	}

	void unwatch()
	{
		watch_state *const w = g_watch;
		g_watch = nullptr;
		if (w == nullptr)
			return;
		if (w->wait != nullptr)
			UnregisterWaitEx(w->wait, INVALID_HANDLE_VALUE); // (waits for a report being written)
		if (w->fence != nullptr)
			w->fence->Release();
		if (w->event != nullptr)
			CloseHandle(w->event);
		delete w;
	}

	std::string gpu_name(ID3D12Device *device)
	{
		IDXGIAdapter3 *const adapter = adapter_of(device);
		DXGI_ADAPTER_DESC1 desc = {};
		if (adapter == nullptr || FAILED(adapter->GetDesc1(&desc)))
		{
			if (adapter != nullptr)
				adapter->Release();
			return "unknown (DXGI didn't say)";
		}
		char line[320];
		std::snprintf(line, sizeof(line), "%s (vendor %04X, device %04X), driver ", narrow(desc.Description).c_str(), desc.VendorId, desc.DeviceId);
		std::string out = line;
		// (the user-mode driver's version, as DXGI gives it for a Direct3D 10+ device; NVIDIA's own number is its last
		// five digits: 32.0.15.6094 is 560.94)
		LARGE_INTEGER v = {};
		if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v)))
		{
			const unsigned a = HIWORD(v.HighPart), b = LOWORD(v.HighPart), c = HIWORD(v.LowPart), d = LOWORD(v.LowPart);
			std::snprintf(line, sizeof(line), "%u.%u.%u.%u", a, b, c, d);
			out += line;
			if (desc.VendorId == 0x10DE)
			{
				const unsigned n = (c % 10) * 10000 + d % 10000;
				std::snprintf(line, sizeof(line), " (NVIDIA %u.%02u)", n / 100, n % 100);
				out += line;
			}
		}
		else
			out += "version unknown";
		adapter->Release();
		return out;
	}

	std::string gpu_memory(ID3D12Device *device)
	{
		IDXGIAdapter3 *const adapter = adapter_of(device);
		DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, shared = {};
		if (adapter == nullptr || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
			FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
		{
			if (adapter != nullptr)
				adapter->Release();
			return "unknown (DXGI didn't say)";
		}
		adapter->Release();
		char line[320];
		std::snprintf(line, sizeof(line), "the game uses %llu MB of the %llu MB Windows lets it have on the card%s, and %llu MB of %llu MB shared",
			megabytes(local.CurrentUsage), megabytes(local.Budget), local.CurrentUsage > local.Budget ? " (OVER: Windows swaps it out)" : "",
			megabytes(shared.CurrentUsage), megabytes(shared.Budget));
		return line;
	}

	std::string report(ID3D12Device *device)
	{
		std::string out;
		const HRESULT reason = device->GetDeviceRemovedReason();
		char when[32] = "";
		const std::time_t now = std::time(nullptr);
		std::tm local = {};
		if (localtime_s(&local, &now) == 0)
			std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
		add(out, "FFXI World FX: the GPU stopped at %s: %s (0x%08lX).\n", when, reason_name(reason), static_cast<unsigned long>(reason));
		out += "\nThe GPU: " + gpu_name(device) + "\nIts video memory at that moment: " + gpu_memory(device) + "\n";

		ID3D12DeviceRemovedExtendedData1 *dred = nullptr;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
		{
			out += "Direct3D kept no record (DRED wasn't on for this device).\n";
			return out;
		}

		D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
		if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)))
		{
			if (fault.PageFaultVA != 0)
			{
				add(out, "\nPage fault: the GPU read or wrote address 0x%016llX, where there was no memory.\n", static_cast<unsigned long long>(fault.PageFaultVA));
				add_allocations(out, "memory there now", fault.pHeadExistingAllocationNode);
				add_allocations(out, "memory there freed recently", fault.pHeadRecentFreedAllocationNode);
			}
			else
				out += "\nNo page fault (not a bad address: a hang, or a command the GPU couldn't finish in time).\n";
		}
		else
			out += "\nPage faults weren't recorded.\n";

		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs = {};
		if (FAILED(dred->GetAutoBreadcrumbsOutput1(&crumbs)))
		{
			out += "\nOperations weren't recorded.\n";
			dred->Release();
			return out;
		}
		out += "\nCommand lists the GPU had (operations done of recorded; '>' marks the first not done, where it stopped):\n";
		int lists = 0, full = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE1 *n = crumbs.pHeadAutoBreadcrumbNode; n != nullptr; n = n->pNext)
		{
			if (++lists > kMaxLists)
			{
				out += "  (more lists left out)\n";
				break;
			}
			const UINT count = n->BreadcrumbCount;
			const UINT done = n->pLastBreadcrumbValue != nullptr ? *n->pLastBreadcrumbValue : 0;
			add(out, "\n  list %p \"%s\" on queue %p \"%s\": %u of %u done%s\n", static_cast<const void *>(n->pCommandList),
				name_of(n->pCommandListDebugNameA, n->pCommandListDebugNameW).c_str(), static_cast<const void *>(n->pCommandQueue),
				name_of(n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW).c_str(), done, count, done < count ? " (NOT FINISHED)" : "");
			std::map<UINT, std::string> contexts;
			for (UINT i = 0; i < n->BreadcrumbContextsCount; ++i)
				contexts[n->pBreadcrumbContexts[i].BreadcrumbIndex] = narrow(n->pBreadcrumbContexts[i].pContextString);
			if (done < count && full < kFullLists)
			{
				// All of it, runs of the same operation on a line (a run never crosses where it stopped)
				full++;
				int lines_out = 0;
				for (UINT i = 0; i < count;)
				{
					if (++lines_out > kMaxRunLines)
					{
						add(out, "         ... %u more\n", count - i);
						break;
					}
					const UINT op = static_cast<UINT>(n->pCommandHistory[i]);
					const auto c = contexts.find(i);
					UINT j = i + 1;
					if (c == contexts.end() && i != done)
						while (j < count && static_cast<UINT>(n->pCommandHistory[j]) == op && contexts.find(j) == contexts.end() && j != done)
							++j;
					if (j - i == 1)
						add(out, "    %c %5u %s%s%s\n", i == done ? '>' : ' ', i, op_name(op), c != contexts.end() ? "  " : "", c != contexts.end() ? c->second.c_str() : "");
					else
						add(out, "      %5u-%u %s x%u\n", i, j - 1, op_name(op), j - i);
					i = j;
				}
			}
			else if (done < count)
			{
				const UINT from = done > kBefore ? done - kBefore : 0, to = done + kAfter < count ? done + kAfter : count;
				if (from != 0)
					add(out, "         ... %u before\n", from);
				for (UINT i = from; i < to; ++i)
				{
					const UINT op = static_cast<UINT>(n->pCommandHistory[i]);
					const auto c = contexts.find(i);
					add(out, "    %c %5u %s%s%s\n", i == done ? '>' : ' ', i, op_name(op),
						c != contexts.end() ? "  " : "", c != contexts.end() ? c->second.c_str() : "");
				}
				if (to < count)
					add(out, "         ... %u after\n", count - to);
			}
			else if (!contexts.empty())
			{
				// (a finished list: only its markers)
				int shown = 0;
				for (const auto &c : contexts)
				{
					if (++shown > 12)
						break;
					add(out, "      %5u %s\n", c.first, c.second.c_str());
				}
			}
		}
		if (lists == 0)
			out += "  none\n";
		dred->Release();
		return out;
	}
}
