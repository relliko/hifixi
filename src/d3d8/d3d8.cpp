/*
 * HiFi XI's own d3d8.dll: Direct3D 8 on Direct3D 12, in dgVoodoo's place (docs/own-frame/d3d8.md). Direct3DCreate8 and
 * IDirect3D8: adapters and their modes from DXGI, the caps the game chooses its paths by (dgVoodoo 2.87's own, as the
 * tests read them through dgVoodoo: vs.1.1 and ps.1.4, table, vertex and range fog, 8 lights, 8 stages), and the device.
 *
 * Before the device's Direct3D 12 device is made a DXGI factory is asked for through the dxgi.dll beside the game
 * (HiFi XI's starter, bootloader\dxgi.dll), as dgVoodoo asks: HiFi XI then starts before the device, its hooks in from
 * the first list.
 *
 * Settings (environment, for the tests): D3D8_LOG (the log's path), D3D8_WARP=1 (WARP instead of the GPU),
 * D3D8_DEBUG=1 (Direct3D 12's debug layer), D3D8_SHADER_CACHE (the compiled shaders' folder).
 */

#include <initguid.h> // (Direct3D 8's interface IDs defined here, once)
#include "device.hpp"
#include "present.hpp"
#include <share.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>

namespace hifi::d3d8
{
	namespace
	{
		std::mutex log_mutex;
		FILE *log_file = nullptr;
		bool log_tried = false;
		std::set<std::string> said;

		std::wstring module_folder()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&module_folder), &self);
			wchar_t path[MAX_PATH] = L"";
			GetModuleFileNameW(self, path, MAX_PATH);
			std::wstring s = path;
			return s.substr(0, s.find_last_of(L"\\/") + 1);
		}

		std::wstring env(const wchar_t *name)
		{
			wchar_t v[MAX_PATH * 2] = L"";
			const DWORD n = GetEnvironmentVariableW(name, v, MAX_PATH * 2);
			return n > 0 && n < MAX_PATH * 2 ? std::wstring(v) : std::wstring();
		}

		void write(int level, const char *text)
		{
			std::lock_guard<std::mutex> lock(log_mutex);
			if (!log_tried)
			{
				log_tried = true;
				std::wstring path = env(L"D3D8_LOG");
				if (path.empty())
					path = module_folder() + L"d3d8.log";
				log_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
			}
			if (log_file == nullptr)
				return;
			SYSTEMTIME t;
			GetLocalTime(&t);
			std::fprintf(log_file, "%02u:%02u:%02u:%03u | %-5s | %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level == 2 ? "ERROR" : level == 1 ? "WARN" : "INFO", text);
			std::fflush(log_file);
		}
	}

	void log(int level, const char *format, ...)
	{
		char text[1024];
		va_list a;
		va_start(a, format);
		vsnprintf(text, sizeof(text), format, a);
		va_end(a);
		write(level, text);
	}

	void log_once(int level, const char *format, ...)
	{
		char text[1024];
		va_list a;
		va_start(a, format);
		vsnprintf(text, sizeof(text), format, a);
		va_end(a);
		{
			std::lock_guard<std::mutex> lock(log_mutex);
			if (!said.insert(text).second)
				return;
		}
		write(level, text);
	}

	// The caps: dgVoodoo 2.87's (internal3D card, as Phoenix's dgVoodoo.conf has it), read through it by d3d8_frame caps
	void fill_caps(D3DCAPS8 &c, UINT adapter)
	{
		std::memset(&c, 0, sizeof(c));
		c.DeviceType = D3DDEVTYPE_HAL;
		c.AdapterOrdinal = adapter;
		c.Caps = 0x00020000;          // D3DCAPS_READ_SCANLINE
		c.Caps2 = 0x200A0000;
		c.Caps3 = 0x00000020;         // ALPHA_FULLSCREEN_FLIP_OR_DISCARD
		c.PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_TWO | D3DPRESENT_INTERVAL_THREE | D3DPRESENT_INTERVAL_FOUR;
		c.CursorCaps = D3DCURSORCAPS_COLOR | D3DCURSORCAPS_LOWRES;
		c.DevCaps = 0x011BBEF0;
		c.PrimitiveMiscCaps = 0x00000EF2;
		c.RasterCaps = 0x00736191;
		c.ZCmpCaps = 0xFF;
		c.SrcBlendCaps = 0x1FFF;
		c.DestBlendCaps = 0x1FFF;
		c.AlphaCmpCaps = 0xFF;
		c.ShadeCaps = 0x00084208;
		c.TextureCaps = 0x0001ECC5;
		c.TextureFilterCaps = 0x07030700;
		c.CubeTextureFilterCaps = 0x07030700;
		c.VolumeTextureFilterCaps = 0x07030700;
		c.TextureAddressCaps = 0x3F;
		c.VolumeTextureAddressCaps = 0x3F;
		c.LineCaps = 0x1F;
		c.MaxTextureWidth = 16384;
		c.MaxTextureHeight = 16384;
		c.MaxVolumeExtent = 16384;
		c.MaxTextureRepeat = 16384;
		c.MaxTextureAspectRatio = 16384;
		c.MaxAnisotropy = 16;
		c.MaxVertexW = 1e11f;
		c.GuardBandLeft = -1e7f;
		c.GuardBandTop = -1e7f;
		c.GuardBandRight = 1e7f;
		c.GuardBandBottom = 1e7f;
		c.ExtentsAdjust = 0.0f;
		c.StencilCaps = 0xFF;
		c.FVFCaps = 0x00180008;
		c.TextureOpCaps = 0x03FEFFFF;
		c.MaxTextureBlendStages = 8;
		c.MaxSimultaneousTextures = 8;
		c.VertexProcessingCaps = 0x0000007B;
		c.MaxActiveLights = 8;
		c.MaxUserClipPlanes = 6;
		c.MaxVertexBlendMatrices = 4;
		c.MaxVertexBlendMatrixIndex = 255;
		c.MaxPointSize = 8192.0f;
		c.MaxPrimitiveCount = 0x01000000;
		c.MaxVertexIndex = 0x0FFFFFFF;
		c.MaxStreams = 16;
		c.MaxStreamStride = 65536;
		c.VertexShaderVersion = D3DVS_VERSION(1, 1);
		c.MaxVertexShaderConst = kVsConstRegs;
		c.PixelShaderVersion = D3DPS_VERSION(1, 4);
		c.MaxPixelShaderValue = 3.402823466e+38f;
	}

	// IDirect3D8
	class direct3d final : public IDirect3D8
	{
	public:
		refcount rc;
		IDXGIFactory2 *factory = nullptr;
		std::vector<IDXGIAdapter1 *> adapters;
		std::vector<std::vector<DXGI_MODE_DESC>> modes;
		std::vector<IDXGIOutput *> outputs;
		bool warp = false, debug = false;

		bool init()
		{
			warp = env(L"D3D8_WARP") == L"1";
			debug = env(L"D3D8_DEBUG") == L"1";
			// (through the dxgi.dll beside the game: HiFi XI's starter starts it here, before the device)
			HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
			using create_fn = HRESULT(WINAPI *)(UINT, REFIID, void **);
			using create1_fn = HRESULT(WINAPI *)(REFIID, void **);
			const auto create2 = dxgi != nullptr ? reinterpret_cast<create_fn>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
			const auto create1 = dxgi != nullptr ? reinterpret_cast<create1_fn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
			HRESULT hr = E_FAIL;
			if (create2 != nullptr)
				hr = create2(0, IID_PPV_ARGS(&factory));
			if (FAILED(hr) && create1 != nullptr)
				hr = create1(IID_PPV_ARGS(&factory));
			if (FAILED(hr) || factory == nullptr)
			{
				log(2, "no DXGI factory (0x%08lX)", static_cast<unsigned long>(hr));
				return false;
			}
			IDXGIFactory4 *f4 = nullptr;
			if (warp && SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f4))))
			{
				IDXGIAdapter1 *a = nullptr;
				if (SUCCEEDED(f4->EnumWarpAdapter(IID_PPV_ARGS(&a))))
					adapters.push_back(a);
				f4->Release();
			}
			for (UINT i = 0; adapters.empty() || (!warp && i < 16); ++i)
			{
				IDXGIAdapter1 *a = nullptr;
				if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
					break;
				DXGI_ADAPTER_DESC1 d;
				a->GetDesc1(&d);
				if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || FAILED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
				{
					a->Release();
					continue;
				}
				adapters.push_back(a);
			}
			if (adapters.empty())
			{
				log(2, "no adapter with Direct3D 12 at feature level 11_0");
				return false;
			}
			// Each adapter's first output and its modes (the primary's when the adapter has none: WARP)
			for (IDXGIAdapter1 *a : adapters)
			{
				IDXGIOutput *o = nullptr;
				if (a->EnumOutputs(0, &o) != S_OK)
					for (IDXGIAdapter1 *b : adapters)
						if (b->EnumOutputs(0, &o) == S_OK)
							break;
				if (o == nullptr)
				{
					IDXGIAdapter1 *first = nullptr;
					if (factory->EnumAdapters1(0, &first) == S_OK)
					{
						first->EnumOutputs(0, &o);
						first->Release();
					}
				}
				outputs.push_back(o);
				std::vector<DXGI_MODE_DESC> list;
				UINT n = 0;
				if (o != nullptr && SUCCEEDED(o->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &n, nullptr)) && n != 0)
				{
					list.resize(n);
					o->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, 0, &n, list.data());
					list.resize(n);
				}
				if (list.empty())
				{
					DXGI_MODE_DESC m = {};
					m.Width = UINT(GetSystemMetrics(SM_CXSCREEN));
					m.Height = UINT(GetSystemMetrics(SM_CYSCREEN));
					m.RefreshRate = { 60, 1 };
					list.push_back(m);
				}
				modes.push_back(list);
			}
			return true;
		}

		~direct3d()
		{
			for (IDXGIOutput *o : outputs)
				release(o);
			for (IDXGIAdapter1 *a : adapters)
				a->Release();
			release(factory);
		}

		static D3DFORMAT mode_format(UINT i) { return i == 0 ? D3DFMT_X8R8G8B8 : D3DFMT_R5G6B5; }
		UINT mode_count(UINT a) const { return a < modes.size() ? UINT(modes[a].size()) * 2 : 0; }

		STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override
		{
			if (ppv == nullptr)
				return E_POINTER;
			if (riid == IID_IUnknown || riid == IID_IDirect3D8)
			{
				*ppv = this;
				AddRef();
				return S_OK;
			}
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		STDMETHOD_(ULONG, AddRef)() override { return rc.add(); }
		STDMETHOD_(ULONG, Release)() override
		{
			const ULONG n = rc.sub();
			if (n == 0)
				delete this;
			return n;
		}
		STDMETHOD(RegisterSoftwareDevice)(void *) override { return D3DERR_INVALIDCALL; }
		STDMETHOD_(UINT, GetAdapterCount)() override { return UINT(adapters.size()); }
		STDMETHOD(GetAdapterIdentifier)(UINT Adapter, DWORD, D3DADAPTER_IDENTIFIER8 *p) override
		{
			if (Adapter >= adapters.size() || p == nullptr)
				return D3DERR_INVALIDCALL;
			DXGI_ADAPTER_DESC1 d;
			adapters[Adapter]->GetDesc1(&d);
			std::memset(p, 0, sizeof(*p));
			std::snprintf(p->Driver, sizeof(p->Driver), "hifixi_d3d8.dll");
			WideCharToMultiByte(CP_ACP, 0, d.Description, -1, p->Description, MAX_DEVICE_IDENTIFIER_STRING, nullptr, nullptr);
			p->VendorId = d.VendorId;
			p->DeviceId = d.DeviceId;
			p->SubSysId = d.SubSysId;
			p->Revision = d.Revision;
			p->DriverVersion.QuadPart = 0x0006000E00000000ll;
			std::memcpy(&p->DeviceIdentifier, &d.AdapterLuid, sizeof(d.AdapterLuid));
			p->WHQLLevel = 1;
			return D3D_OK;
		}
		STDMETHOD_(UINT, GetAdapterModeCount)(UINT Adapter) override { return mode_count(Adapter); }
		STDMETHOD(EnumAdapterModes)(UINT Adapter, UINT Mode, D3DDISPLAYMODE *p) override
		{
			if (Adapter >= modes.size() || Mode >= mode_count(Adapter) || p == nullptr)
				return D3DERR_INVALIDCALL;
			const UINT n = UINT(modes[Adapter].size());
			const DXGI_MODE_DESC &m = modes[Adapter][Mode % n];
			p->Width = m.Width;
			p->Height = m.Height;
			p->RefreshRate = m.RefreshRate.Denominator != 0 ? (m.RefreshRate.Numerator + m.RefreshRate.Denominator / 2) / m.RefreshRate.Denominator : 0;
			p->Format = mode_format(Mode / n);
			return D3D_OK;
		}
		STDMETHOD(GetAdapterDisplayMode)(UINT Adapter, D3DDISPLAYMODE *p) override
		{
			if (Adapter >= adapters.size() || p == nullptr)
				return D3DERR_INVALIDCALL;
			HMONITOR mon = outputs[Adapter] != nullptr ? [&] { DXGI_OUTPUT_DESC d; outputs[Adapter]->GetDesc(&d); return d.Monitor; }() : MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
			MONITORINFOEXW mi = {};
			mi.cbSize = sizeof(mi);
			GetMonitorInfoW(mon, &mi);
			DEVMODEW dm = {};
			dm.dmSize = sizeof(dm);
			EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
			p->Width = dm.dmPelsWidth != 0 ? dm.dmPelsWidth : UINT(mi.rcMonitor.right - mi.rcMonitor.left);
			p->Height = dm.dmPelsHeight != 0 ? dm.dmPelsHeight : UINT(mi.rcMonitor.bottom - mi.rcMonitor.top);
			p->RefreshRate = dm.dmDisplayFrequency;
			p->Format = D3DFMT_X8R8G8B8;
			return D3D_OK;
		}
		STDMETHOD(CheckDeviceType)(UINT Adapter, D3DDEVTYPE, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL) override
		{
			if (Adapter >= adapters.size())
				return D3DERR_INVALIDCALL;
			const bool display = DisplayFormat == D3DFMT_X8R8G8B8 || DisplayFormat == D3DFMT_R5G6B5 || DisplayFormat == D3DFMT_X1R5G5B5;
			const bool back = BackBufferFormat == D3DFMT_UNKNOWN || is_target_format(BackBufferFormat);
			return display && back ? D3D_OK : D3DERR_NOTAVAILABLE;
		}
		STDMETHOD(CheckDeviceFormat)(UINT Adapter, D3DDEVTYPE, D3DFORMAT, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override
		{
			if (Adapter >= adapters.size())
				return D3DERR_INVALIDCALL;
			const format_info &i = info(CheckFormat);
			if (!i.known)
				return D3DERR_NOTAVAILABLE;
			if (Usage & D3DUSAGE_RENDERTARGET)
				return is_target_format(CheckFormat) && RType != D3DRTYPE_VOLUMETEXTURE ? D3D_OK : D3DERR_NOTAVAILABLE;
			if (Usage & D3DUSAGE_DEPTHSTENCIL)
				return i.depth && (RType == D3DRTYPE_SURFACE || RType == D3DRTYPE_TEXTURE) ? D3D_OK : D3DERR_NOTAVAILABLE;
			if (i.depth)
				return RType == D3DRTYPE_SURFACE ? D3D_OK : D3DERR_NOTAVAILABLE;
			// (what dgVoodoo 2.87 turns down, turned down too: the game picks its formats by these answers)
			switch (CheckFormat)
			{
			case D3DFMT_R8G8B8: case D3DFMT_A4L4: case D3DFMT_A8P8: case D3DFMT_UYVY: case D3DFMT_YUY2:
				return D3DERR_NOTAVAILABLE;
			default:
				return D3D_OK;
			}
		}
		STDMETHOD(CheckDeviceMultiSampleType)(UINT Adapter, D3DDEVTYPE, D3DFORMAT SurfaceFormat, BOOL, D3DMULTISAMPLE_TYPE MultiSampleType) override
		{
			if (Adapter >= adapters.size())
				return D3DERR_INVALIDCALL;
			if (MultiSampleType == D3DMULTISAMPLE_NONE)
				return D3D_OK;
			const format_info &i = info(SurfaceFormat);
			const bool ok = (is_target_format(SurfaceFormat) || i.depth) && (MultiSampleType == 2 || MultiSampleType == 4 || MultiSampleType == 8);
			return ok ? D3D_OK : D3DERR_NOTAVAILABLE;
		}
		STDMETHOD(CheckDepthStencilMatch)(UINT Adapter, D3DDEVTYPE, D3DFORMAT, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override
		{
			if (Adapter >= adapters.size())
				return D3DERR_INVALIDCALL;
			return is_target_format(RenderTargetFormat) && is_depth_format(DepthStencilFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
		}
		STDMETHOD(GetDeviceCaps)(UINT Adapter, D3DDEVTYPE, D3DCAPS8 *pCaps) override
		{
			if (Adapter >= adapters.size() || pCaps == nullptr)
				return D3DERR_INVALIDCALL;
			fill_caps(*pCaps, Adapter);
			return D3D_OK;
		}
		STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT Adapter) override
		{
			if (Adapter >= outputs.size() || outputs[Adapter] == nullptr)
				return MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
			DXGI_OUTPUT_DESC d;
			outputs[Adapter]->GetDesc(&d);
			return d.Monitor;
		}
		STDMETHOD(CreateDevice)(UINT Adapter, D3DDEVTYPE, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **ppReturnedDeviceInterface) override
		{
			if (ppReturnedDeviceInterface == nullptr || pp == nullptr || Adapter >= adapters.size())
				return D3DERR_INVALIDCALL;
			*ppReturnedDeviceInterface = nullptr;
			device *d = new device(this, Adapter, hFocusWindow, BehaviorFlags);
			std::string why;
			if (!d->init(pp, why))
			{
				log(2, "CreateDevice: %s", why.c_str());
				d->Release();
				return D3DERR_NOTAVAILABLE;
			}
			*ppReturnedDeviceInterface = d;
			return D3D_OK;
		}
	};

	IDXGIFactory2 *factory_of(direct3d *d) { return d->factory; }
	IDXGIAdapter1 *adapter_of(direct3d *d, UINT a) { return a < d->adapters.size() ? d->adapters[a] : nullptr; }
	bool debug_of(direct3d *d) { return d->debug; }
	IDirect3D8 *interface_of(direct3d *d) { return d; }
	std::wstring dll_folder() { return module_folder(); }
	std::wstring env_value(const wchar_t *name) { return env(name); }
}

extern "C" IDirect3D8 *WINAPI Direct3DCreate8(UINT SDKVersion)
{
	using namespace hifi::d3d8;
	if (SDKVersion != D3D_SDK_VERSION && SDKVersion != 120)
		log(1, "Direct3DCreate8 asked for SDK version %u", SDKVersion);
	direct3d *d = new direct3d();
	if (!d->init())
	{
		d->Release();
		return nullptr;
	}
	const std::wstring cache = env(L"D3D8_SHADER_CACHE");
	set_shader_cache_folder(cache.empty() ? module_folder() + L"d3d8-shader-cache" : cache);
	DXGI_ADAPTER_DESC1 ad;
	d->adapters[0]->GetDesc1(&ad);
	char name[128];
	WideCharToMultiByte(CP_ACP, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);
	log(0, "HiFi XI d3d8: Direct3DCreate8 (SDK %u), %u adapter(s), the first %s%s", SDKVersion, unsigned(d->adapters.size()), name, d->warp ? " (WARP)" : "");
	return d;
}

// (the validators D3DX and old tools look for)
extern "C" HRESULT WINAPI ValidatePixelShader(const DWORD *, const D3DCAPS8 *, BOOL, char **errors)
{
	if (errors != nullptr)
		*errors = nullptr;
	return D3D_OK;
}
extern "C" HRESULT WINAPI ValidateVertexShader(const DWORD *, const DWORD *, const D3DCAPS8 *, BOOL, char **errors)
{
	if (errors != nullptr)
		*errors = nullptr;
	return D3D_OK;
}
extern "C" void WINAPI DebugSetMute() {}
