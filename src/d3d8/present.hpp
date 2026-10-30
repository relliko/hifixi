#pragma once

// The swap chain: the game's back buffer is a render target of the device's own (R8G8B8A8, as dgVoodoo's), copied into
// a DXGI flip swap chain's buffer at Present (the back buffer to COPY_SOURCE, as HiFi XI sees dgVoodoo's go). DXGI
// stretches it to the window when the two differ. A gamma ramp other than the identity is drawn through on the way
// (a pass of its own instead of the copy). Full screen is a window on the screen, never an exclusive mode.

#include "resource.hpp"

namespace hifi::d3d8
{
	class swap_chain final : public IDirect3DSwapChain8
	{
	public:
		swap_chain(device *d) : dev(d) {}
		~swap_chain();
		bool make(const D3DPRESENT_PARAMETERS &pp, HWND window, std::string &why);
		void drop();
		HRESULT present(HWND override_window);

		device *dev;
		refcount rc;
		D3DPRESENT_PARAMETERS params = {};
		HWND window = nullptr;
		IDXGISwapChain3 *chain = nullptr;
		ID3D12Resource *buffers[4] = {};
		UINT buffer_count = 0;
		UINT width = 0, height = 0;
		bool tearing = false;
		image *back_image = nullptr;
		std::unique_ptr<surface> back;
		// The gamma pass
		ID3D12RootSignature *gamma_root = nullptr;
		ID3D12PipelineState *gamma_pso = nullptr;
		ID3D12DescriptorHeap *gamma_rtvs = nullptr;
		UINT rtv_step = 0;
		bool make_gamma(std::string &why);

		STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override;
		STDMETHOD_(ULONG, AddRef)() override;
		STDMETHOD_(ULONG, Release)() override;
		STDMETHOD(Present)(CONST RECT *pSourceRect, CONST RECT *pDestRect, HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion) override;
		STDMETHOD(GetBackBuffer)(UINT BackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface8 **ppBackBuffer) override;
	};
}
