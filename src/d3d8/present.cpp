#include "present.hpp"
#include "device.hpp"
#include <algorithm>
#include <cstdio>

namespace hifi::d3d8
{
	namespace
	{
		const char *kGammaHlsl = R"(
cbuffer lut : register(b0) { float4 ramp[256]; };
Texture2D<float4> src : register(t0);
struct V { float4 pos : SV_Position; };
V vs(uint id : SV_VertexID)
{
	V o;
	const float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}
float4 ps(V i) : SV_Target
{
	const float4 c = src.Load(int3(i.pos.xy, 0));
	const uint3 n = uint3(round(saturate(c.rgb) * 255.0));
	return float4(ramp[n.r].r, ramp[n.g].g, ramp[n.b].b, 1.0);
}
)";
	}

	swap_chain::~swap_chain()
	{
		drop();
		release(gamma_pso);
		release(gamma_root);
		release(gamma_rtvs);
	}

	void swap_chain::drop()
	{
		if (chain == nullptr)
			return;
		dev->g.wait_idle();
		for (ID3D12Resource *&b : buffers)
			release(b);
		back.reset();
		if (back_image != nullptr)
		{
			delete back_image;
			back_image = nullptr;
		}
	}

	bool swap_chain::make(const D3DPRESENT_PARAMETERS &pp, HWND w, std::string &why)
	{
		params = pp;
		window = w;
		if (params.BackBufferWidth == 0 || params.BackBufferHeight == 0)
		{
			RECT r = {};
			GetClientRect(window, &r);
			params.BackBufferWidth = std::max<UINT>(1, UINT(r.right - r.left));
			params.BackBufferHeight = std::max<UINT>(1, UINT(r.bottom - r.top));
		}
		if (params.BackBufferFormat == D3DFMT_UNKNOWN)
			params.BackBufferFormat = D3DFMT_X8R8G8B8;
		if (params.BackBufferCount == 0)
			params.BackBufferCount = 1;
		width = params.BackBufferWidth;
		height = params.BackBufferHeight;

		IDXGIFactory5 *f5 = nullptr;
		BOOL allow = FALSE;
		if (SUCCEEDED(dev->g.factory->QueryInterface(IID_PPV_ARGS(&f5))))
		{
			if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
				allow = FALSE;
			f5->Release();
		}
		tearing = allow != FALSE;

		if (chain != nullptr)
		{
			if (FAILED(chain->ResizeBuffers(buffer_count, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)))
			{
				why = "the swap chain couldn't be resized";
				return false;
			}
		}
		else
		{
			DXGI_SWAP_CHAIN_DESC1 sd = {};
			sd.Width = width;
			sd.Height = height;
			sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			sd.SampleDesc.Count = 1;
			sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			sd.BufferCount = buffer_count = std::clamp<UINT>(params.BackBufferCount + 1, 2, 3);
			sd.Scaling = DXGI_SCALING_STRETCH;
			sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
			sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
			IDXGISwapChain1 *c1 = nullptr;
			HRESULT hr = dev->g.factory->CreateSwapChainForHwnd(dev->g.queue, window, &sd, nullptr, nullptr, &c1);
			if (FAILED(hr) || c1 == nullptr)
			{
				char text[96];
				std::snprintf(text, sizeof(text), "no swap chain for the window (0x%08lX)", static_cast<unsigned long>(hr));
				why = text;
				return false;
			}
			hr = c1->QueryInterface(IID_PPV_ARGS(&chain));
			c1->Release();
			if (FAILED(hr))
			{
				why = "the swap chain has no IDXGISwapChain3";
				return false;
			}
			dev->g.factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
		}
		for (UINT i = 0; i < buffer_count; ++i)
			chain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));

		// The game's back buffer: a target of our own
		back_image = new image(dev);
		if (!back_image->make(D3DRTYPE_SURFACE, width, height, 1, 1, 1, D3DUSAGE_RENDERTARGET, params.BackBufferFormat, D3DPOOL_DEFAULT, params.MultiSampleType))
		{
			why = "the back buffer couldn't be made";
			return false;
		}
		back = std::make_unique<surface>(dev, back_image, 0, 0, static_cast<IDirect3DDevice8 *>(dev), false);
		if (!params.Windowed)
			log(0, "full screen %ux%u asked for: presented in the window (no exclusive mode)", width, height);
		return true;
	}

	bool swap_chain::make_gamma(std::string &why)
	{
		if (gamma_pso != nullptr)
			return true;
		ID3D12Device *d = dev->g.device;
		D3D12_DESCRIPTOR_RANGE range = {};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		D3D12_ROOT_PARAMETER p[2] = {};
		p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		p[1].DescriptorTable.NumDescriptorRanges = 1;
		p[1].DescriptorTable.pDescriptorRanges = &range;
		p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC rd = {};
		rd.NumParameters = 2;
		rd.pParameters = p;
		ID3DBlob *blob = nullptr, *err = nullptr;
		if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
			FAILED(d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gamma_root))))
		{
			release(blob);
			release(err);
			why = "no root signature for the gamma pass";
			return false;
		}
		release(blob);
		release(err);
		std::string hlsl = kGammaHlsl;
		// (the compiler takes "main": two texts with the entry renamed)
		std::string vs = hlsl, ps = hlsl;
		vs.replace(vs.find("V vs("), 5, "V main(");
		ps.replace(ps.find("float4 ps("), 10, "float4 main(");
		const std::vector<uint8_t> &vb = compile(vs, "vs_5_0", why), &pb = compile(ps, "ps_5_0", why);
		if (vb.empty() || pb.empty())
			return false;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
		pd.pRootSignature = gamma_root;
		pd.VS = { vb.data(), vb.size() };
		pd.PS = { pb.data(), pb.size() };
		pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pd.SampleMask = 0xFFFFFFFF;
		pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pd.RasterizerState.DepthClipEnable = TRUE;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 1;
		pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		pd.SampleDesc.Count = 1;
		if (FAILED(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&gamma_pso))))
		{
			why = "no pipeline for the gamma pass";
			return false;
		}
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 4;
		d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gamma_rtvs));
		rtv_step = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		return true;
	}

	HRESULT swap_chain::present(HWND override_window)
	{
		if (chain == nullptr || back_image == nullptr)
			return D3DERR_INVALIDCALL;
		(void)override_window;
		gpu &g = dev->g;
		ID3D12GraphicsCommandList *l = g.list();
		const UINT i = chain->GetCurrentBackBufferIndex();
		ID3D12Resource *dst = buffers[i];
		gpu_resource sw;
		sw.res = dst;
		sw.state = D3D12_RESOURCE_STATE_PRESENT;
		// (a multisampled back buffer is resolved into its single-sample copy first, left in PIXEL_SHADER_RESOURCE)
		const bool resolved = back_image->multisample > D3DMULTISAMPLE_NONE;
		gpu_resource single;
		if (resolved)
		{
			single.res = back_image->sample_resource();
			single.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		}
		gpu_resource &b = resolved ? single : back_image->gpu;

		bool identity = true;
		if (dev->gamma_set)
			for (int k = 0; k < 256 && identity; ++k)
				identity = dev->gamma.red[k] >> 8 == k && dev->gamma.green[k] >> 8 == k && dev->gamma.blue[k] >> 8 == k;
		std::string why;
		if (!identity && make_gamma(why))
		{
			// The ramp drawn through: the back buffer read, the swap chain's buffer drawn
			g.transition(b, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			g.transition(sw, D3D12_RESOURCE_STATE_RENDER_TARGET);
			g.flush_barriers();
			upload_alloc lut = g.upload(256 * 16, 256);
			float *f = reinterpret_cast<float *>(lut.cpu);
			for (int k = 0; k < 256; ++k)
			{
				f[k * 4 + 0] = dev->gamma.red[k] / 65535.0f;
				f[k * 4 + 1] = dev->gamma.green[k] / 65535.0f;
				f[k * 4 + 2] = dev->gamma.blue[k] / 65535.0f;
				f[k * 4 + 3] = 1.0f;
			}
			D3D12_CPU_DESCRIPTOR_HANDLE view = back_image->srv;
			const D3D12_GPU_DESCRIPTOR_HANDLE table = g.srv_table(&view, 1);
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = gamma_rtvs->GetCPUDescriptorHandleForHeapStart();
			rtv.ptr += SIZE_T(i) * rtv_step;
			g.device->CreateRenderTargetView(dst, nullptr, rtv);
			l = g.list();
			l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			const D3D12_VIEWPORT vp = { 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
			const D3D12_RECT sc = { 0, 0, LONG(width), LONG(height) };
			l->RSSetViewports(1, &vp);
			l->RSSetScissorRects(1, &sc);
			l->SetGraphicsRootSignature(gamma_root);
			l->SetPipelineState(gamma_pso);
			l->SetGraphicsRootConstantBufferView(0, lut.gpu);
			l->SetGraphicsRootDescriptorTable(1, table);
			l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			l->DrawInstanced(3, 1, 0, 0);
			dev->bound.epoch = 0;
			g.transition(sw, D3D12_RESOURCE_STATE_PRESENT);
		}
		else
		{
			// The copy (as dgVoodoo: the back buffer to COPY_SOURCE)
			g.transition(b, D3D12_RESOURCE_STATE_COPY_SOURCE);
			g.transition(sw, D3D12_RESOURCE_STATE_COPY_DEST);
			g.flush_barriers();
			g.list()->CopyResource(dst, b.res);
			g.transition(sw, D3D12_RESOURCE_STATE_PRESENT);
			if (resolved)
				g.transition(single, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		g.submit(false);
		UINT interval = 0, flags = 0;
		if (!params.Windowed)
		{
			const UINT pi = params.FullScreen_PresentationInterval;
			interval = pi == D3DPRESENT_INTERVAL_IMMEDIATE ? 0 : pi == D3DPRESENT_INTERVAL_TWO ? 2 : pi == D3DPRESENT_INTERVAL_THREE ? 3 : pi == D3DPRESENT_INTERVAL_FOUR ? 4 : 1;
		}
		if (interval == 0 && tearing)
			flags |= DXGI_PRESENT_ALLOW_TEARING;
		const HRESULT hr = chain->Present(interval, flags);
		if (FAILED(hr))
		{
			log_once(2, "Present failed (0x%08lX)", static_cast<unsigned long>(hr));
			if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
				log(2, "the device was removed (0x%08lX)", static_cast<unsigned long>(g.device->GetDeviceRemovedReason()));
		}
		g.end_frame();
		return D3D_OK;
	}

	HRESULT swap_chain::QueryInterface(REFIID riid, void **ppv)
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3DSwapChain8)
		{
			*ppv = this;
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG swap_chain::AddRef() { return rc.add(); }
	ULONG swap_chain::Release()
	{
		const ULONG n = rc.sub();
		if (n == 0)
			delete this;
		return n;
	}
	HRESULT swap_chain::Present(CONST RECT *, CONST RECT *, HWND hDestWindowOverride, CONST RGNDATA *)
	{
		return present(hDestWindowOverride);
	}
	HRESULT swap_chain::GetBackBuffer(UINT BackBuffer, D3DBACKBUFFER_TYPE, IDirect3DSurface8 **pp)
	{
		if (pp == nullptr || BackBuffer >= params.BackBufferCount || back == nullptr)
			return D3DERR_INVALIDCALL;
		*pp = back.get();
		back->AddRef();
		return D3D_OK;
	}
}
