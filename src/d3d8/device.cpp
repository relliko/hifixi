// IDirect3DDevice8 (device.hpp): its making, state, resources, shaders, state blocks and copies. The draws, clears and
// the GPU side of the state are in device_draw.cpp.

#include "device.hpp"
#include "present.hpp"
#include <algorithm>

namespace hifi::d3d8
{
	namespace
	{
		const D3DMATRIX kIdentity = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

		DWORD fbits(float f)
		{
			DWORD d;
			std::memcpy(&d, &f, 4);
			return d;
		}

		// The render and stage states each state block type has (Direct3D 8's lists)
		const DWORD kPixelRs[] = { D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_LINEPATTERN, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE,
			D3DRS_LASTPIXEL, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_DITHERENABLE, D3DRS_FOGSTART,
			D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_ALPHABLENDENABLE, D3DRS_ZBIAS, D3DRS_STENCILENABLE, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL,
			D3DRS_STENCILPASS, D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_TEXTUREFACTOR, D3DRS_WRAP0,
			D3DRS_WRAP1, D3DRS_WRAP2, D3DRS_WRAP3, D3DRS_WRAP4, D3DRS_WRAP5, D3DRS_WRAP6, D3DRS_WRAP7, D3DRS_COLORWRITEENABLE, D3DRS_BLENDOP, D3DRS_EDGEANTIALIAS };
		const DWORD kVertexRs[] = { D3DRS_SHADEMODE, D3DRS_SPECULARENABLE, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE,
			D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_RANGEFOGENABLE, D3DRS_AMBIENT, D3DRS_COLORVERTEX, D3DRS_FOGVERTEXMODE,
			D3DRS_CLIPPING, D3DRS_LIGHTING, D3DRS_NORMALIZENORMALS, D3DRS_LOCALVIEWER, D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE,
			D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE, D3DRS_VERTEXBLEND, D3DRS_CLIPPLANEENABLE, D3DRS_SOFTWAREVERTEXPROCESSING,
			D3DRS_POINTSIZE, D3DRS_POINTSIZE_MIN, D3DRS_POINTSPRITEENABLE, D3DRS_POINTSCALEENABLE, D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B,
			D3DRS_POINTSCALE_C, D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK, D3DRS_PATCHEDGESTYLE, D3DRS_PATCHSEGMENTS, D3DRS_POINTSIZE_MAX,
			D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_TWEENFACTOR, D3DRS_POSITIONORDER, D3DRS_NORMALORDER };
		const DWORD kVertexTss[] = { D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS };

		// What each render state changes on the GPU side
		uint32_t dirty_of_rs(DWORD s)
		{
			switch (s)
			{
			case D3DRS_TEXTUREFACTOR: case D3DRS_FOGCOLOR: case D3DRS_ALPHAREF:
				return device::d_psc;
			case D3DRS_FOGSTART: case D3DRS_FOGEND: case D3DRS_FOGDENSITY:
				return device::d_psc | device::d_ffvs;
			case D3DRS_AMBIENT:
				return device::d_ffvs;
			case D3DRS_STENCILREF:
				return 0;
			default:
				return device::d_pipeline | device::d_vs_key | device::d_ps_key | device::d_ffvs | device::d_vsc;
			}
		}
	}

	DWORD default_render_state(DWORD s, bool auto_depth)
	{
		switch (s)
		{
		case D3DRS_ZENABLE: return auto_depth ? D3DZB_TRUE : D3DZB_FALSE;
		case D3DRS_FILLMODE: return D3DFILL_SOLID;
		case D3DRS_SHADEMODE: return D3DSHADE_GOURAUD;
		case D3DRS_ZWRITEENABLE: return TRUE;
		case D3DRS_LASTPIXEL: return TRUE;
		case D3DRS_SRCBLEND: return D3DBLEND_ONE;
		case D3DRS_DESTBLEND: return D3DBLEND_ZERO;
		case D3DRS_CULLMODE: return D3DCULL_CCW;
		case D3DRS_ZFUNC: return D3DCMP_LESSEQUAL;
		case D3DRS_ALPHAFUNC: return D3DCMP_ALWAYS;
		case D3DRS_FOGTABLEMODE: return D3DFOG_NONE;
		case D3DRS_FOGEND: return fbits(1.0f);
		case D3DRS_FOGDENSITY: return fbits(1.0f);
		case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL: case D3DRS_STENCILPASS: return D3DSTENCILOP_KEEP;
		case D3DRS_STENCILFUNC: return D3DCMP_ALWAYS;
		case D3DRS_STENCILMASK: case D3DRS_STENCILWRITEMASK: case D3DRS_TEXTUREFACTOR: case D3DRS_MULTISAMPLEMASK: return 0xFFFFFFFF;
		case D3DRS_CLIPPING: case D3DRS_LIGHTING: case D3DRS_COLORVERTEX: case D3DRS_LOCALVIEWER: case D3DRS_MULTISAMPLEANTIALIAS: return TRUE;
		case D3DRS_DIFFUSEMATERIALSOURCE: return D3DMCS_COLOR1;
		case D3DRS_SPECULARMATERIALSOURCE: return D3DMCS_COLOR2;
		case D3DRS_AMBIENTMATERIALSOURCE: case D3DRS_EMISSIVEMATERIALSOURCE: return D3DMCS_MATERIAL;
		case D3DRS_POINTSIZE: case D3DRS_POINTSCALE_A: case D3DRS_PATCHSEGMENTS: return fbits(1.0f);
		case D3DRS_POINTSIZE_MAX: return fbits(64.0f);
		case D3DRS_COLORWRITEENABLE: return 0xF;
		case D3DRS_BLENDOP: return D3DBLENDOP_ADD;
		case D3DRS_POSITIONORDER: return D3DORDER_CUBIC;
		case D3DRS_NORMALORDER: return D3DORDER_LINEAR;
		default: return 0;
		}
	}

	DWORD default_stage_state(DWORD stage, DWORD t)
	{
		switch (t)
		{
		case D3DTSS_COLOROP: return stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
		case D3DTSS_ALPHAOP: return stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
		case D3DTSS_COLORARG1: case D3DTSS_ALPHAARG1: return D3DTA_TEXTURE;
		case D3DTSS_COLORARG2: case D3DTSS_ALPHAARG2: return D3DTA_CURRENT;
		case D3DTSS_COLORARG0: case D3DTSS_ALPHAARG0: case D3DTSS_RESULTARG: return D3DTA_CURRENT;
		case D3DTSS_TEXCOORDINDEX: return stage;
		case D3DTSS_ADDRESSU: case D3DTSS_ADDRESSV: case D3DTSS_ADDRESSW: return D3DTADDRESS_WRAP;
		case D3DTSS_MAGFILTER: case D3DTSS_MINFILTER: return D3DTEXF_POINT;
		case D3DTSS_MIPFILTER: return D3DTEXF_NONE;
		case D3DTSS_MAXANISOTROPY: return 1;
		case D3DTSS_TEXTURETRANSFORMFLAGS: return D3DTTFF_DISABLE;
		default: return 0;
		}
	}

	device::device(direct3d *p, UINT a, HWND f, DWORD b) : parent(p), adapter(a), focus(f), behaviour(b)
	{
		interface_of(parent)->AddRef();
	}

	device::~device()
	{
		for (auto &b : blocks)
			if (b != nullptr)
				b->let_go();
		for (IDirect3DBaseTexture8 *&t : cur.textures)
			release(t);
		for (IDirect3DVertexBuffer8 *&v : cur.streams)
			release(v);
		release(cur.indices);
		drop_surface(rt);
		drop_surface(ds);
		rt = ds = nullptr;
		if (g.device != nullptr)
			g.wait_idle();
		for (auto &p : psos)
			release(p.second.pso);
		chain.reset();
		if (auto_ds != nullptr)
			delete auto_ds;
		g.shutdown();
		interface_of(parent)->Release();
	}

	void device::make_auto_depth()
	{
		if (auto_ds != nullptr)
		{
			delete auto_ds;
			auto_ds = nullptr;
		}
		if (!params.EnableAutoDepthStencil)
			return;
		image *img = new image(this);
		if (!img->make(D3DRTYPE_SURFACE, params.BackBufferWidth, params.BackBufferHeight, 1, 1, 1, D3DUSAGE_DEPTHSTENCIL, params.AutoDepthStencilFormat, D3DPOOL_DEFAULT, params.MultiSampleType))
		{
			delete img;
			return;
		}
		auto_ds = new surface(this, img, 0, 0, static_cast<IDirect3DDevice8 *>(this), true);
	}

	bool device::init(D3DPRESENT_PARAMETERS *pp, std::string &why)
	{
		params = *pp;
		HWND window = params.hDeviceWindow != nullptr ? params.hDeviceWindow : focus;
		if (!g.init(factory_of(parent), adapter_of(parent, adapter), debug_of(parent), why))
			return false;
		g.on_reset = [this] { bound.epoch = 0; };
		check_formats(g.device);
		chain = std::make_unique<swap_chain>(this);
		if (!chain->make(params, window, why))
			return false;
		params = chain->params;
		*pp = params;
		make_auto_depth();
		reset_state();
		for (int i = 0; i < 256; ++i)
		{
			gamma.red[i] = gamma.green[i] = gamma.blue[i] = WORD(i * 257);
			for (int p = 0; p < 256; ++p)
				palettes[p][i] = { BYTE(i), BYTE(i), BYTE(i), 255 };
		}
		log(0, "device: %ux%u back buffer (format %u, %s), depth %u, behaviour 0x%X", params.BackBufferWidth, params.BackBufferHeight, unsigned(params.BackBufferFormat),
			params.Windowed ? "windowed" : "full screen", params.EnableAutoDepthStencil ? unsigned(params.AutoDepthStencilFormat) : 0u, unsigned(behaviour));
		return true;
	}

	void device::reset_state()
	{
		// (Direct3D 8's defaults: Reset puts every state back to them)
		for (IDirect3DBaseTexture8 *&t : cur.textures)
			release(t);
		for (IDirect3DVertexBuffer8 *&v : cur.streams)
			release(v);
		release(cur.indices);
		cur = device_state();
		for (DWORD s = 0; s < kRenderStates; ++s)
			cur.rs[s] = default_render_state(s, params.EnableAutoDepthStencil != FALSE);
		for (DWORD st = 0; st < kMaxStages; ++st)
			for (DWORD t = 0; t < kStageStates; ++t)
				cur.tss[st][t] = default_stage_state(st, t);
		for (D3DMATRIX &m : cur.world)
			m = kIdentity;
		cur.view = cur.proj = kIdentity;
		for (D3DMATRIX &m : cur.texm)
			m = kIdentity;
		drop_surface(rt);
		drop_surface(ds);
		rt = chain->back.get();
		hold_surface(rt);
		ds = auto_ds;
		hold_surface(ds);
		cur.viewport = { 0, 0, rt->width(), rt->height(), 0.0f, 1.0f };
		dirty = d_all;
		vs_cb_frame = ps_cb_frame = ~0ull;
	}

	void device::hold_surface(surface *s)
	{
		if (s != nullptr && s->container != static_cast<IUnknown *>(static_cast<IDirect3DDevice8 *>(this)))
			s->AddRef();
	}

	void device::drop_surface(surface *s)
	{
		if (s != nullptr && s->container != static_cast<IUnknown *>(static_cast<IDirect3DDevice8 *>(this)))
			s->Release();
	}

	void device::forget(image *img)
	{
		const auto it = std::find(images.begin(), images.end(), img);
		if (it != images.end())
			images.erase(it);
	}

	void device::unbind(IUnknown *) {}

	// ---- IUnknown ----

	HRESULT device::QueryInterface(REFIID riid, void **ppv)
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3DDevice8)
		{
			*ppv = this;
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG device::AddRef() { return rc.add(); }
	ULONG device::Release()
	{
		const ULONG n = rc.sub();
		if (n == 0)
		{
			// (the back buffer and the auto depth buffer forward their references here: kept from reaching it again)
			rc.refs = 1 << 30;
			delete this;
		}
		return n;
	}

	// ---- The device itself ----

	HRESULT device::TestCooperativeLevel() { return D3D_OK; }
	UINT device::GetAvailableTextureMem() { return 2047u << 20; }
	HRESULT device::ResourceManagerDiscardBytes(DWORD) { return D3D_OK; }
	HRESULT device::GetDirect3D(IDirect3D8 **pp)
	{
		if (pp == nullptr)
			return D3DERR_INVALIDCALL;
		*pp = interface_of(parent);
		(*pp)->AddRef();
		return D3D_OK;
	}
	HRESULT device::GetDeviceCaps(D3DCAPS8 *c)
	{
		if (c == nullptr)
			return D3DERR_INVALIDCALL;
		fill_caps(*c, adapter);
		return D3D_OK;
	}
	HRESULT device::GetDisplayMode(D3DDISPLAYMODE *m)
	{
		if (m == nullptr)
			return D3DERR_INVALIDCALL;
		const HRESULT hr = interface_of(parent)->GetAdapterDisplayMode(adapter, m);
		if (SUCCEEDED(hr) && !params.Windowed)
		{
			m->Width = params.BackBufferWidth;
			m->Height = params.BackBufferHeight;
		}
		return hr;
	}
	HRESULT device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *p)
	{
		if (p == nullptr)
			return D3DERR_INVALIDCALL;
		p->AdapterOrdinal = adapter;
		p->DeviceType = D3DDEVTYPE_HAL;
		p->hFocusWindow = focus;
		p->BehaviorFlags = behaviour;
		return D3D_OK;
	}
	HRESULT device::SetCursorProperties(UINT, UINT, IDirect3DSurface8 *) { return D3D_OK; }
	void device::SetCursorPosition(int, int, DWORD) {}
	BOOL device::ShowCursor(BOOL) { return FALSE; }
	HRESULT device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pp, IDirect3DSwapChain8 **out)
	{
		if (pp == nullptr || out == nullptr)
			return D3DERR_INVALIDCALL;
		swap_chain *s = new swap_chain(this);
		std::string why;
		if (!s->make(*pp, pp->hDeviceWindow != nullptr ? pp->hDeviceWindow : focus, why))
		{
			log(2, "CreateAdditionalSwapChain: %s", why.c_str());
			s->Release();
			return D3DERR_NOTAVAILABLE;
		}
		// (its back buffer forwards to the swap chain, not the device)
		s->back->container = s;
		*out = s;
		return D3D_OK;
	}

	HRESULT device::Reset(D3DPRESENT_PARAMETERS *pp)
	{
		if (pp == nullptr)
			return D3DERR_INVALIDCALL;
		g.submit(true);
		uint32_t default_pool = 0;
		for (image *i : images)
			if (i->pool == D3DPOOL_DEFAULT && i != chain->back_image && (auto_ds == nullptr || i != auto_ds->img))
				default_pool++;
		if (default_pool != 0)
			log(1, "Reset with %u default-pool resources still made (Direct3D 8 would refuse; they're kept)", default_pool);
		drop_surface(rt);
		drop_surface(ds);
		rt = ds = nullptr;
		params = *pp;
		std::string why;
		chain->drop();
		if (!chain->make(params, params.hDeviceWindow != nullptr ? params.hDeviceWindow : chain->window, why))
		{
			log(2, "Reset: %s", why.c_str());
			return D3DERR_INVALIDCALL;
		}
		params = chain->params;
		*pp = params;
		make_auto_depth();
		reset_state();
		log(0, "Reset: %ux%u", params.BackBufferWidth, params.BackBufferHeight);
		return D3D_OK;
	}

	HRESULT device::Present(CONST RECT *src, CONST RECT *dst, HWND w, CONST RGNDATA *)
	{
		present_frame(w, src, dst);
		return D3D_OK;
	}

	HRESULT device::GetBackBuffer(UINT b, D3DBACKBUFFER_TYPE t, IDirect3DSurface8 **pp) { return chain->GetBackBuffer(b, t, pp); }
	HRESULT device::GetRasterStatus(D3DRASTER_STATUS *s)
	{
		if (s == nullptr)
			return D3DERR_INVALIDCALL;
		s->InVBlank = FALSE;
		s->ScanLine = 0;
		return D3D_OK;
	}
	void device::SetGammaRamp(DWORD, CONST D3DGAMMARAMP *r)
	{
		if (r == nullptr)
			return;
		gamma = *r;
		gamma_set = true;
	}
	void device::GetGammaRamp(D3DGAMMARAMP *r)
	{
		if (r != nullptr)
			*r = gamma;
	}

	// ---- Resources ----

	HRESULT device::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DTexture8 **out)
	{
		if (out == nullptr || w == 0 || h == 0)
			return D3DERR_INVALIDCALL;
		*out = nullptr;
		texture *t = new texture(this);
		t->img = new image(this);
		if (!t->img->make(D3DRTYPE_TEXTURE, w, h, 1, levels, 1, usage, f, pool, D3DMULTISAMPLE_NONE))
		{
			t->Release();
			return D3DERR_INVALIDCALL;
		}
		for (UINT l = 0; l < t->img->levels; ++l)
			t->levels.push_back(std::make_unique<surface>(this, t->img, 0, l, static_cast<IDirect3DTexture8 *>(t), false));
		*out = t;
		return D3D_OK;
	}

	HRESULT device::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DVolumeTexture8 **out)
	{
		if (out == nullptr || w == 0 || h == 0 || d == 0)
			return D3DERR_INVALIDCALL;
		*out = nullptr;
		volume_texture *t = new volume_texture(this);
		t->img = new image(this);
		if (!t->img->make(D3DRTYPE_VOLUMETEXTURE, w, h, d, levels, 1, usage, f, pool, D3DMULTISAMPLE_NONE))
		{
			t->Release();
			return D3DERR_INVALIDCALL;
		}
		for (UINT l = 0; l < t->img->levels; ++l)
			t->levels.push_back(std::make_unique<volume>(this, t->img, l, static_cast<IDirect3DVolumeTexture8 *>(t)));
		*out = t;
		return D3D_OK;
	}

	HRESULT device::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DCubeTexture8 **out)
	{
		if (out == nullptr || edge == 0)
			return D3DERR_INVALIDCALL;
		*out = nullptr;
		cube_texture *t = new cube_texture(this);
		t->img = new image(this);
		if (!t->img->make(D3DRTYPE_CUBETEXTURE, edge, edge, 1, levels, 6, usage, f, pool, D3DMULTISAMPLE_NONE))
		{
			t->Release();
			return D3DERR_INVALIDCALL;
		}
		for (UINT fc = 0; fc < 6; ++fc)
			for (UINT l = 0; l < t->img->levels; ++l)
				t->faces.push_back(std::make_unique<surface>(this, t->img, fc, l, static_cast<IDirect3DCubeTexture8 *>(t), false));
		*out = t;
		return D3D_OK;
	}

	HRESULT device::CreateVertexBuffer(UINT n, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8 **out)
	{
		if (out == nullptr || n == 0)
			return D3DERR_INVALIDCALL;
		vertex_buffer *v = new vertex_buffer(this);
		v->buf.make(n, usage, pool);
		v->fvf = fvf;
		*out = v;
		return D3D_OK;
	}

	HRESULT device::CreateIndexBuffer(UINT n, DWORD usage, D3DFORMAT f, D3DPOOL pool, IDirect3DIndexBuffer8 **out)
	{
		if (out == nullptr || n == 0 || (f != D3DFMT_INDEX16 && f != D3DFMT_INDEX32))
			return D3DERR_INVALIDCALL;
		index_buffer *i = new index_buffer(this);
		i->buf.make(n, usage, pool);
		i->format = f;
		*out = i;
		return D3D_OK;
	}

	HRESULT device::CreateRenderTarget(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, BOOL, IDirect3DSurface8 **out)
	{
		if (out == nullptr || !is_target_format(f))
			return D3DERR_INVALIDCALL;
		image *img = new image(this);
		if (!img->make(D3DRTYPE_SURFACE, w, h, 1, 1, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, ms))
		{
			delete img;
			return D3DERR_INVALIDCALL;
		}
		*out = new surface(this, img, 0, 0, nullptr, true);
		return D3D_OK;
	}

	HRESULT device::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, IDirect3DSurface8 **out)
	{
		if (out == nullptr || !is_depth_format(f))
			return D3DERR_INVALIDCALL;
		image *img = new image(this);
		if (!img->make(D3DRTYPE_SURFACE, w, h, 1, 1, 1, D3DUSAGE_DEPTHSTENCIL, f, D3DPOOL_DEFAULT, ms))
		{
			delete img;
			return D3DERR_INVALIDCALL;
		}
		*out = new surface(this, img, 0, 0, nullptr, true);
		return D3D_OK;
	}

	HRESULT device::CreateImageSurface(UINT w, UINT h, D3DFORMAT f, IDirect3DSurface8 **out)
	{
		if (out == nullptr || !info(f).known || info(f).depth)
			return D3DERR_INVALIDCALL;
		image *img = new image(this);
		if (!img->make(D3DRTYPE_SURFACE, w, h, 1, 1, 1, 0, f, D3DPOOL_SYSTEMMEM, D3DMULTISAMPLE_NONE))
		{
			delete img;
			return D3DERR_INVALIDCALL;
		}
		*out = new surface(this, img, 0, 0, nullptr, true);
		return D3D_OK;
	}

	HRESULT device::CopyRects(IDirect3DSurface8 *src, CONST RECT *rects, UINT n, IDirect3DSurface8 *dst, CONST POINT *points)
	{
		if (src == nullptr || dst == nullptr)
			return D3DERR_INVALIDCALL;
		surface *s = static_cast<surface *>(src), *d = static_cast<surface *>(dst);
		if (s->img->format != d->img->format)
			return D3DERR_INVALIDCALL;
		if (rects == nullptr || n == 0)
		{
			const RECT whole = { 0, 0, LONG(s->width()), LONG(s->height()) };
			const POINT at = points != nullptr ? points[0] : POINT { 0, 0 };
			return copy_rect(s, whole, d, at);
		}
		for (UINT i = 0; i < n; ++i)
		{
			const POINT at = points != nullptr ? points[i] : POINT { rects[i].left, rects[i].top };
			const HRESULT hr = copy_rect(s, rects[i], d, at);
			if (FAILED(hr))
				return hr;
		}
		return D3D_OK;
	}

	HRESULT device::copy_rect(surface *s, const RECT &r0, surface *d, POINT at)
	{
		// (clipped to both surfaces)
		RECT r = r0;
		r.left = std::max<LONG>(r.left, 0);
		r.top = std::max<LONG>(r.top, 0);
		r.right = std::min<LONG>(r.right, LONG(s->width()));
		r.bottom = std::min<LONG>(r.bottom, LONG(s->height()));
		LONG w = r.right - r.left, h = r.bottom - r.top;
		w = std::min<LONG>(w, LONG(d->width()) - at.x);
		h = std::min<LONG>(h, LONG(d->height()) - at.y);
		if (w <= 0 || h <= 0)
			return D3D_OK;
		image *si = s->img, *di = d->img;
		const format_info &fi = info(si->format);
		const bool src_gpu = si->kind == image_kind::target, dst_gpu = di->kind == image_kind::target;
		const UINT bpp = fi.bpp != 0 ? fi.bpp : 4;

		if (src_gpu && !dst_gpu)
		{
			// From a target into memory: copied on the GPU into a readback buffer now, read into the copy at the next
			// access (the game locks it next: that waits for the GPU, as with dgVoodoo)
			ID3D12Resource *res = si->sample_resource();
			gpu_resource single;
			gpu_resource &sr = res == si->gpu.res ? si->gpu : single;
			if (res != si->gpu.res)
			{
				single.res = res;
				single.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			}
			const UINT row = align_up(UINT(w) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
			image::pending_read p;
			p.rb = g.new_readback(UINT64(row) * UINT(h));
			if (p.rb.buffer == nullptr)
				return D3DERR_OUTOFVIDEOMEMORY;
			g.transition(sr, D3D12_RESOURCE_STATE_COPY_SOURCE);
			g.flush_barriers();
			D3D12_TEXTURE_COPY_LOCATION to = {};
			to.pResource = p.rb.buffer;
			to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			to.PlacedFootprint.Footprint = { kTargetView, UINT(w), UINT(h), 1, row };
			D3D12_TEXTURE_COPY_LOCATION from = {};
			from.pResource = res;
			from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			from.SubresourceIndex = s->sub();
			const D3D12_BOX box = { UINT(r.left), UINT(r.top), 0, UINT(r.left + w), UINT(r.top + h), 1 };
			g.list()->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
			if (res != si->gpu.res)
				g.transition(single, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			p.rb.fence = g.last_submitted() + 1;
			p.sub = d->sub();
			p.x = UINT(at.x);
			p.y = UINT(at.y);
			p.w = UINT(w);
			p.h = UINT(h);
			p.row_pitch = row;
			p.from = si->format;
			p.target = true;
			di->copy(d->sub()); // (made, and older reads done)
			di->reads.push_back(p);
			stats.readbacks++;
			return D3D_OK;
		}
		if (src_gpu && dst_gpu)
		{
			// Target to target: on the GPU
			ID3D12Resource *res = si->sample_resource();
			gpu_resource single;
			gpu_resource &sr = res == si->gpu.res ? si->gpu : single;
			if (res != si->gpu.res)
			{
				single.res = res;
				single.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			}
			g.transition(sr, D3D12_RESOURCE_STATE_COPY_SOURCE);
			g.transition(di->gpu, D3D12_RESOURCE_STATE_COPY_DEST);
			g.flush_barriers();
			D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
			to.pResource = di->gpu.res;
			to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			to.SubresourceIndex = d->sub();
			from.pResource = res;
			from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			from.SubresourceIndex = s->sub();
			const D3D12_BOX box = { UINT(r.left), UINT(r.top), 0, UINT(r.left + w), UINT(r.top + h), 1 };
			g.list()->CopyTextureRegion(&to, UINT(at.x), UINT(at.y), 0, &from, &box);
			if (res != si->gpu.res)
				g.transition(single, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			return D3D_OK;
		}
		// From memory (a texture's or an image's copy) into memory or a target
		std::vector<uint8_t> &sc = si->copy(s->sub());
		const UINT sp = si->pitch(s->level);
		if (dst_gpu)
		{
			// (through the target's copy: the rect written up)
			std::vector<uint8_t> &dc = di->cpu[d->sub()];
			dc.assign(size_t(di->slice(d->level)), 0);
			const UINT dp = di->pitch(d->level);
			for (LONG y = 0; y < h; ++y)
				std::memcpy(dc.data() + size_t(at.y + y) * dp + size_t(at.x) * bpp, sc.data() + size_t(r.top + y) * sp + size_t(r.left) * bpp, size_t(w) * bpp);
			const RECT wr = { at.x, at.y, at.x + w, at.y + h };
			di->write_target(d->sub(), &wr);
			return D3D_OK;
		}
		std::vector<uint8_t> &dc = di->copy(d->sub());
		const UINT dp = di->pitch(d->level);
		if (fi.block_bytes != 0)
		{
			const UINT bytes = UINT((w + 3) / 4) * fi.block_bytes;
			for (LONG y = 0; y < (h + 3) / 4; ++y)
				std::memcpy(dc.data() + size_t(at.y / 4 + y) * dp + size_t(at.x / 4) * fi.block_bytes, sc.data() + size_t(r.top / 4 + y) * sp + size_t(r.left / 4) * fi.block_bytes, bytes);
		}
		else
			for (LONG y = 0; y < h; ++y)
				std::memcpy(dc.data() + size_t(at.y + y) * dp + size_t(at.x) * bpp, sc.data() + size_t(r.top + y) * sp + size_t(r.left) * bpp, size_t(w) * bpp);
		if (di->kind == image_kind::texture)
			di->dirty[d->sub()] = 1;
		return D3D_OK;
	}

	HRESULT device::UpdateTexture(IDirect3DBaseTexture8 *src, IDirect3DBaseTexture8 *dst)
	{
		base_texture *s = texture_of(src), *d = texture_of(dst);
		if (s == nullptr || d == nullptr || s->img->format != d->img->format || s->img->faces != d->img->faces)
			return D3DERR_INVALIDCALL;
		image *si = s->img, *di = d->img;
		// (levels matched from the top: the source may have more)
		const UINT skip = si->levels > di->levels ? si->levels - di->levels : 0;
		for (UINT f = 0; f < di->faces; ++f)
			for (UINT l = 0; l < di->levels && l + skip < si->levels; ++l)
			{
				std::vector<uint8_t> &sc = si->copy(si->subresource(f, l + skip));
				std::vector<uint8_t> &dc = di->cpu[di->subresource(f, l)];
				dc = sc;
				di->dirty[di->subresource(f, l)] = 1;
			}
		return D3D_OK;
	}

	HRESULT device::GetFrontBuffer(IDirect3DSurface8 *dst)
	{
		if (dst == nullptr)
			return D3DERR_INVALIDCALL;
		surface *d = static_cast<surface *>(dst);
		if (d->img->format != D3DFMT_A8R8G8B8)
			return D3DERR_INVALIDCALL;
		// (the last frame shown is the back buffer's picture till the next is drawn: read from it, as A8R8G8B8)
		surface *b = chain->back.get();
		const D3DFORMAT was = b->img->format;
		b->img->format = D3DFMT_A8R8G8B8;
		const RECT whole = { 0, 0, LONG(b->width()), LONG(b->height()) };
		const HRESULT hr = copy_rect(b, whole, d, POINT { 0, 0 });
		d->img->finish_reads();
		b->img->format = was;
		// (the X formats' alpha read as opaque)
		std::vector<uint8_t> &c = d->img->cpu[d->sub()];
		for (size_t i = 3; i < c.size(); i += 4)
			c[i] = 255;
		return hr;
	}

	HRESULT device::SetRenderTarget(IDirect3DSurface8 *new_rt, IDirect3DSurface8 *new_ds)
	{
		surface *r = static_cast<surface *>(new_rt), *z = static_cast<surface *>(new_ds);
		if (r != nullptr && r->img->kind != image_kind::target)
			return D3DERR_INVALIDCALL;
		if (z != nullptr && z->img->kind != image_kind::depth)
			return D3DERR_INVALIDCALL;
		if (r != nullptr && r != rt)
		{
			hold_surface(r);
			drop_surface(rt);
			rt = r;
		}
		if (z != ds)
		{
			hold_surface(z);
			drop_surface(ds);
			ds = z;
		}
		// (Direct3D 8 sets the viewport to the whole of the new target)
		cur.viewport = { 0, 0, rt->width(), rt->height(), 0.0f, 1.0f };
		dirty |= d_targets | d_viewport | d_pipeline | d_ffvs | d_vsc;
		return D3D_OK;
	}

	HRESULT device::GetRenderTarget(IDirect3DSurface8 **pp)
	{
		if (pp == nullptr || rt == nullptr)
			return D3DERR_INVALIDCALL;
		*pp = rt;
		rt->AddRef();
		return D3D_OK;
	}

	HRESULT device::GetDepthStencilSurface(IDirect3DSurface8 **pp)
	{
		if (pp == nullptr)
			return D3DERR_INVALIDCALL;
		*pp = ds;
		if (ds == nullptr)
			return D3DERR_NOTFOUND;
		ds->AddRef();
		return D3D_OK;
	}

	HRESULT device::BeginScene()
	{
		in_scene = true;
		return D3D_OK;
	}

	HRESULT device::EndScene()
	{
		in_scene = false;
		return D3D_OK;
	}

	HRESULT device::Clear(DWORD n, CONST D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
	{
		return clear(n, rects, flags, color, z, stencil);
	}

	// ---- State ----

	HRESULT device::SetTransform(D3DTRANSFORMSTATETYPE t, CONST D3DMATRIX *m)
	{
		if (m == nullptr)
			return D3DERR_INVALIDCALL;
		device_state &s = target();
		const DWORD v = DWORD(t);
		if (v == D3DTS_VIEW)
		{
			s.view = *m;
			if (recording)
				recording->mask.view = 1;
		}
		else if (v == D3DTS_PROJECTION)
		{
			s.proj = *m;
			if (recording)
				recording->mask.proj = 1;
		}
		else if (v >= D3DTS_TEXTURE0 && v <= D3DTS_TEXTURE7)
		{
			s.texm[v - D3DTS_TEXTURE0] = *m;
			if (recording)
				recording->mask.texm[v - D3DTS_TEXTURE0] = 1;
		}
		else if (v >= 256 && v < 260)
		{
			s.world[v - 256] = *m;
			if (recording)
				recording->mask.world[v - 256] = 1;
		}
		else if (v >= 260 && v < 512)
		{
			log_once(1, "world matrices past 3 (indexed vertex blending) are ignored");
			return D3D_OK;
		}
		else
			return D3DERR_INVALIDCALL;
		dirty |= d_ffvs;
		return D3D_OK;
	}

	HRESULT device::GetTransform(D3DTRANSFORMSTATETYPE t, D3DMATRIX *m)
	{
		if (m == nullptr)
			return D3DERR_INVALIDCALL;
		const DWORD v = DWORD(t);
		if (v == D3DTS_VIEW)
			*m = cur.view;
		else if (v == D3DTS_PROJECTION)
			*m = cur.proj;
		else if (v >= D3DTS_TEXTURE0 && v <= D3DTS_TEXTURE7)
			*m = cur.texm[v - D3DTS_TEXTURE0];
		else if (v >= 256 && v < 260)
			*m = cur.world[v - 256];
		else if (v >= 260 && v < 512)
			*m = kIdentity;
		else
			return D3DERR_INVALIDCALL;
		return D3D_OK;
	}

	HRESULT device::MultiplyTransform(D3DTRANSFORMSTATETYPE t, CONST D3DMATRIX *m)
	{
		if (m == nullptr)
			return D3DERR_INVALIDCALL;
		D3DMATRIX a;
		if (FAILED(GetTransform(t, &a)))
			return D3DERR_INVALIDCALL;
		D3DMATRIX r;
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				r.m[i][j] = a.m[i][0] * m->m[0][j] + a.m[i][1] * m->m[1][j] + a.m[i][2] * m->m[2][j] + a.m[i][3] * m->m[3][j];
		return SetTransform(t, &r);
	}

	HRESULT device::SetViewport(CONST D3DVIEWPORT8 *v)
	{
		if (v == nullptr)
			return D3DERR_INVALIDCALL;
		// (one reaching past the render target is refused, the viewport kept: Direct3D 8's rule, and dgVoodoo's)
		if (rt != nullptr && (UINT64(v->X) + v->Width > rt->width() || UINT64(v->Y) + v->Height > rt->height()))
			return D3DERR_INVALIDCALL;
		target().viewport = *v;
		if (recording)
			recording->mask.viewport = 1;
		dirty |= d_viewport | d_ffvs | d_vsc;
		return D3D_OK;
	}

	HRESULT device::GetViewport(D3DVIEWPORT8 *v)
	{
		if (v == nullptr)
			return D3DERR_INVALIDCALL;
		*v = cur.viewport;
		return D3D_OK;
	}

	HRESULT device::SetMaterial(CONST D3DMATERIAL8 *m)
	{
		if (m == nullptr)
			return D3DERR_INVALIDCALL;
		target().material = *m;
		if (recording)
			recording->mask.material = 1;
		dirty |= d_ffvs;
		return D3D_OK;
	}

	HRESULT device::GetMaterial(D3DMATERIAL8 *m)
	{
		if (m == nullptr)
			return D3DERR_INVALIDCALL;
		*m = cur.material;
		return D3D_OK;
	}

	HRESULT device::SetLight(DWORD i, CONST D3DLIGHT8 *l)
	{
		if (l == nullptr || i >= kMaxLights)
			return D3DERR_INVALIDCALL;
		light_slot &s = target().lights[i];
		s.light = *l;
		s.set = true;
		if (recording)
			recording->mask.light[i] = 1;
		dirty |= d_ffvs | d_vs_key;
		return D3D_OK;
	}

	HRESULT device::GetLight(DWORD i, D3DLIGHT8 *l)
	{
		if (l == nullptr || i >= kMaxLights || !cur.lights[i].set)
			return D3DERR_INVALIDCALL;
		*l = cur.lights[i].light;
		return D3D_OK;
	}

	HRESULT device::LightEnable(DWORD i, BOOL on)
	{
		if (i >= kMaxLights)
			return D3DERR_INVALIDCALL;
		light_slot &s = target().lights[i];
		if (!s.set)
		{
			// (Direct3D 8: a light enabled before it's set is the default white light down +z)
			s.light = {};
			s.light.Type = D3DLIGHT_DIRECTIONAL;
			s.light.Diffuse = { 1, 1, 1, 0 };
			s.light.Direction = { 0, 0, 1 };
			s.set = true;
		}
		s.enabled = on != FALSE;
		if (recording)
			recording->mask.light_enable[i] = 1;
		dirty |= d_ffvs | d_vs_key;
		return D3D_OK;
	}

	HRESULT device::GetLightEnable(DWORD i, BOOL *on)
	{
		if (on == nullptr || i >= kMaxLights || !cur.lights[i].set)
			return D3DERR_INVALIDCALL;
		*on = cur.lights[i].enabled;
		return D3D_OK;
	}

	HRESULT device::SetClipPlane(DWORD i, CONST float *p)
	{
		if (p == nullptr || i >= 6)
			return D3DERR_INVALIDCALL;
		std::memcpy(target().clip[i], p, 16);
		if (recording)
			recording->mask.clip[i] = 1;
		dirty |= d_ffvs | d_vsc;
		return D3D_OK;
	}

	HRESULT device::GetClipPlane(DWORD i, float *p)
	{
		if (p == nullptr || i >= 6)
			return D3DERR_INVALIDCALL;
		std::memcpy(p, cur.clip[i], 16);
		return D3D_OK;
	}

	HRESULT device::SetRenderState(D3DRENDERSTATETYPE s, DWORD v)
	{
		if (DWORD(s) >= kRenderStates)
			return D3DERR_INVALIDCALL;
		device_state &t = target();
		if (recording)
			recording->mask.rs[s] = 1;
		else if (t.rs[s] == v)
			return D3D_OK;
		t.rs[s] = v;
		dirty |= dirty_of_rs(s);
		return D3D_OK;
	}

	HRESULT device::GetRenderState(D3DRENDERSTATETYPE s, DWORD *v)
	{
		if (v == nullptr || DWORD(s) >= kRenderStates)
			return D3DERR_INVALIDCALL;
		*v = cur.rs[s];
		return D3D_OK;
	}

	void device::set_texture_ref(IDirect3DBaseTexture8 *&slot, IDirect3DBaseTexture8 *t)
	{
		if (slot == t)
			return;
		if (t != nullptr)
			t->AddRef();
		if (slot != nullptr)
			slot->Release();
		slot = t;
	}

	HRESULT device::SetTexture(DWORD stage, IDirect3DBaseTexture8 *t)
	{
		if (stage >= kMaxStages)
			return D3DERR_INVALIDCALL;
		device_state &s = target();
		if (recording)
			recording->mask.textures[stage] = 1;
		set_texture_ref(s.textures[stage], t);
		dirty |= d_textures | d_ps_key | d_vs_key;
		return D3D_OK;
	}

	HRESULT device::GetTexture(DWORD stage, IDirect3DBaseTexture8 **pp)
	{
		if (pp == nullptr || stage >= kMaxStages)
			return D3DERR_INVALIDCALL;
		*pp = cur.textures[stage];
		if (*pp != nullptr)
			(*pp)->AddRef();
		return D3D_OK;
	}

	HRESULT device::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v)
	{
		if (stage >= kMaxStages || DWORD(t) >= kStageStates)
			return D3DERR_INVALIDCALL;
		device_state &s = target();
		if (recording)
			recording->mask.tss[stage][t] = 1;
		else if (s.tss[stage][t] == v)
			return D3D_OK;
		s.tss[stage][t] = v;
		switch (t)
		{
		case D3DTSS_BUMPENVMAT00: case D3DTSS_BUMPENVMAT01: case D3DTSS_BUMPENVMAT10: case D3DTSS_BUMPENVMAT11: case D3DTSS_BUMPENVLSCALE: case D3DTSS_BUMPENVLOFFSET:
			dirty |= d_psc;
			break;
		case D3DTSS_ADDRESSU: case D3DTSS_ADDRESSV: case D3DTSS_ADDRESSW: case D3DTSS_BORDERCOLOR: case D3DTSS_MAGFILTER: case D3DTSS_MINFILTER:
		case D3DTSS_MIPFILTER: case D3DTSS_MIPMAPLODBIAS: case D3DTSS_MAXMIPLEVEL: case D3DTSS_MAXANISOTROPY:
			dirty |= d_textures;
			break;
		default:
			dirty |= d_ps_key | d_vs_key | d_pipeline | d_ffvs;
			break;
		}
		return D3D_OK;
	}

	HRESULT device::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD *v)
	{
		if (v == nullptr || stage >= kMaxStages || DWORD(t) >= kStageStates)
			return D3DERR_INVALIDCALL;
		*v = cur.tss[stage][t];
		return D3D_OK;
	}

	HRESULT device::ValidateDevice(DWORD *passes)
	{
		if (passes != nullptr)
			*passes = 1;
		return D3D_OK;
	}

	HRESULT device::GetInfo(DWORD, void *, DWORD) { return S_FALSE; }

	HRESULT device::SetPaletteEntries(UINT n, CONST PALETTEENTRY *e)
	{
		if (e == nullptr || n >= 256)
			return D3DERR_INVALIDCALL;
		std::memcpy(palettes[n], e, sizeof(palettes[n]));
		return D3D_OK;
	}
	HRESULT device::GetPaletteEntries(UINT n, PALETTEENTRY *e)
	{
		if (e == nullptr || n >= 256)
			return D3DERR_INVALIDCALL;
		std::memcpy(e, palettes[n], sizeof(palettes[n]));
		return D3D_OK;
	}
	HRESULT device::SetCurrentTexturePalette(UINT n)
	{
		if (n >= 256)
			return D3DERR_INVALIDCALL;
		target().palette = n;
		if (recording)
			recording->mask.palette = 1;
		return D3D_OK;
	}
	HRESULT device::GetCurrentTexturePalette(UINT *n)
	{
		if (n == nullptr)
			return D3DERR_INVALIDCALL;
		*n = cur.palette;
		return D3D_OK;
	}

	HRESULT device::SetClipStatus(CONST D3DCLIPSTATUS8 *) { return D3D_OK; }
	HRESULT device::GetClipStatus(D3DCLIPSTATUS8 *s)
	{
		if (s == nullptr)
			return D3DERR_INVALIDCALL;
		s->ClipUnion = 0;
		s->ClipIntersection = 0;
		return D3D_OK;
	}

	// ---- Draws ----

	HRESULT device::DrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count)
	{
		return draw(t, count, false, start, 0, 0, 0, nullptr, 0, nullptr, D3DFMT_UNKNOWN);
	}
	HRESULT device::DrawIndexedPrimitive(D3DPRIMITIVETYPE t, UINT min_index, UINT vertices, UINT start, UINT count)
	{
		return draw(t, count, true, start, INT(cur.base_vertex), min_index, vertices, nullptr, 0, nullptr, D3DFMT_UNKNOWN);
	}
	HRESULT device::DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, CONST void *v, UINT stride)
	{
		if (v == nullptr || stride == 0)
			return D3DERR_INVALIDCALL;
		return draw(t, count, false, 0, 0, 0, 0, v, stride, nullptr, D3DFMT_UNKNOWN);
	}
	HRESULT device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT min_index, UINT vertices, UINT count, CONST void *i, D3DFORMAT f, CONST void *v, UINT stride)
	{
		if (v == nullptr || i == nullptr || stride == 0)
			return D3DERR_INVALIDCALL;
		return draw(t, count, true, 0, 0, min_index, vertices, v, stride, i, f);
	}
	HRESULT device::ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer8 *, DWORD)
	{
		log_once(1, "ProcessVertices isn't supported");
		return D3DERR_INVALIDCALL;
	}
	HRESULT device::DrawRectPatch(UINT, CONST float *, CONST D3DRECTPATCH_INFO *)
	{
		log_once(1, "patches aren't supported");
		return D3D_OK;
	}
	HRESULT device::DrawTriPatch(UINT, CONST float *, CONST D3DTRIPATCH_INFO *)
	{
		log_once(1, "patches aren't supported");
		return D3D_OK;
	}
	HRESULT device::DeletePatch(UINT) { return D3D_OK; }

	// ---- Streams ----

	HRESULT device::SetStreamSource(UINT n, IDirect3DVertexBuffer8 *v, UINT stride)
	{
		if (n >= kMaxStreams)
			return D3DERR_INVALIDCALL;
		device_state &s = target();
		if (recording)
			recording->mask.streams[n] = 1;
		if (s.streams[n] != v)
		{
			if (v != nullptr)
				v->AddRef();
			if (s.streams[n] != nullptr)
				s.streams[n]->Release();
			s.streams[n] = v;
		}
		s.strides[n] = stride;
		return D3D_OK;
	}
	HRESULT device::GetStreamSource(UINT n, IDirect3DVertexBuffer8 **pp, UINT *stride)
	{
		if (n >= kMaxStreams || pp == nullptr || stride == nullptr)
			return D3DERR_INVALIDCALL;
		*pp = cur.streams[n];
		if (*pp != nullptr)
			(*pp)->AddRef();
		*stride = cur.strides[n];
		return D3D_OK;
	}
	HRESULT device::SetIndices(IDirect3DIndexBuffer8 *i, UINT base)
	{
		device_state &s = target();
		if (recording)
			recording->mask.indices = 1;
		if (s.indices != i)
		{
			if (i != nullptr)
				i->AddRef();
			if (s.indices != nullptr)
				s.indices->Release();
			s.indices = i;
		}
		s.base_vertex = base;
		return D3D_OK;
	}
	HRESULT device::GetIndices(IDirect3DIndexBuffer8 **pp, UINT *base)
	{
		if (pp == nullptr || base == nullptr)
			return D3DERR_INVALIDCALL;
		*pp = cur.indices;
		if (*pp != nullptr)
			(*pp)->AddRef();
		*base = cur.base_vertex;
		return D3D_OK;
	}

	// ---- Shaders ----

	namespace
	{
		UINT type_size(uint8_t t)
		{
			switch (t)
			{
			case D3DVSDT_FLOAT1: return 4;
			case D3DVSDT_FLOAT2: return 8;
			case D3DVSDT_FLOAT3: return 12;
			case D3DVSDT_FLOAT4: return 16;
			case D3DVSDT_D3DCOLOR: return 4;
			case D3DVSDT_UBYTE4: return 4;
			case D3DVSDT_SHORT2: return 4;
			case D3DVSDT_SHORT4: return 8;
			default: return 0;
			}
		}
	}

	void declaration::finish()
	{
		layout.clear();
		streams = 0;
		std::memset(inputs, 0, sizeof(inputs));
		for (const element &e : elements)
		{
			D3D12_INPUT_ELEMENT_DESC d = {};
			d.SemanticName = "ATTR";
			d.SemanticIndex = e.reg;
			d.InputSlot = e.stream;
			d.AlignedByteOffset = e.offset;
			d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
			uint8_t kind = in_none;
			switch (e.type)
			{
			case D3DVSDT_FLOAT1: d.Format = DXGI_FORMAT_R32_FLOAT; kind = in_float1; break;
			case D3DVSDT_FLOAT2: d.Format = DXGI_FORMAT_R32G32_FLOAT; kind = in_float2; break;
			case D3DVSDT_FLOAT3: d.Format = DXGI_FORMAT_R32G32B32_FLOAT; kind = in_float3; break;
			case D3DVSDT_FLOAT4: d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; kind = in_float4; break;
			case D3DVSDT_D3DCOLOR: d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; kind = in_color; break;
			case D3DVSDT_UBYTE4: d.Format = DXGI_FORMAT_R8G8B8A8_UINT; kind = in_ubyte4; break;
			case D3DVSDT_SHORT2: d.Format = DXGI_FORMAT_R16G16_SINT; kind = in_short2; break;
			case D3DVSDT_SHORT4: d.Format = DXGI_FORMAT_R16G16B16A16_SINT; kind = in_short4; break;
			default: continue;
			}
			if (e.reg >= 16 || inputs[e.reg] != in_none)
				continue;
			inputs[e.reg] = kind;
			streams |= uint16_t(1u << e.stream);
			layout.push_back(d);
		}
		hash = hash_bytes(elements.data(), elements.size() * sizeof(element), rhw ? 7 : 3);
	}

	const declaration *device::fvf_declaration(DWORD fvf)
	{
		const auto it = fvf_decls.find(fvf);
		if (it != fvf_decls.end())
			return it->second.get();
		auto d = std::make_unique<declaration>();
		uint16_t at = 0;
		auto add = [&](uint8_t reg, uint8_t type) {
			d->elements.push_back({ 0, reg, type, at });
			at = uint16_t(at + type_size(type));
		};
		const DWORD pos = fvf & D3DFVF_POSITION_MASK;
		if (pos == D3DFVF_XYZRHW)
		{
			add(D3DVSDE_POSITION, D3DVSDT_FLOAT4);
			d->rhw = true;
		}
		else if (pos != 0)
		{
			add(D3DVSDE_POSITION, D3DVSDT_FLOAT3);
			const int betas = pos == D3DFVF_XYZ ? 0 : int((pos - D3DFVF_XYZB1) / 2) + 1;
			const bool indices = (fvf & D3DFVF_LASTBETA_UBYTE4) != 0 && betas > 0;
			const int weights = std::min(betas - (indices ? 1 : 0), 4);
			if (weights > 0)
				add(D3DVSDE_BLENDWEIGHT, uint8_t(D3DVSDT_FLOAT1 + weights - 1));
			for (int extra = weights; extra < betas - (indices ? 1 : 0); ++extra)
				at = uint16_t(at + 4);
			if (indices)
				add(D3DVSDE_BLENDINDICES, D3DVSDT_UBYTE4);
		}
		if (fvf & D3DFVF_NORMAL)
			add(D3DVSDE_NORMAL, D3DVSDT_FLOAT3);
		if (fvf & D3DFVF_PSIZE)
			add(D3DVSDE_PSIZE, D3DVSDT_FLOAT1);
		if (fvf & D3DFVF_DIFFUSE)
			add(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR);
		if (fvf & D3DFVF_SPECULAR)
			add(D3DVSDE_SPECULAR, D3DVSDT_D3DCOLOR);
		const UINT sets = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
		for (UINT i = 0; i < sets && i < 8; ++i)
		{
			const DWORD f = (fvf >> (16 + 2 * i)) & 3;
			const uint8_t type = f == D3DFVF_TEXTUREFORMAT1 ? D3DVSDT_FLOAT1 : f == D3DFVF_TEXTUREFORMAT3 ? D3DVSDT_FLOAT3 : f == D3DFVF_TEXTUREFORMAT4 ? D3DVSDT_FLOAT4 : D3DVSDT_FLOAT2;
			add(uint8_t(D3DVSDE_TEXCOORD0 + i), type);
		}
		d->finish();
		const declaration *r = d.get();
		fvf_decls[fvf] = std::move(d);
		return r;
	}

	vertex_shader *device::vshader(DWORD h)
	{
		if ((h & 1) == 0)
			return nullptr;
		const DWORD i = (h >> 1);
		return i < vshaders.size() ? vshaders[i].get() : nullptr;
	}

	pixel_shader *device::pshader(DWORD h)
	{
		return h != 0 && h - 1 < pshaders.size() ? pshaders[h - 1].get() : nullptr;
	}

	const declaration *device::current_declaration(const vertex_shader **vs)
	{
		const DWORD h = cur.vs;
		if (const vertex_shader *v = vshader(h))
		{
			if (vs != nullptr)
				*vs = v;
			return &v->decl;
		}
		if (vs != nullptr)
			*vs = nullptr;
		return fvf_declaration(h);
	}

	HRESULT device::CreateVertexShader(CONST DWORD *decl, CONST DWORD *function, DWORD *handle, DWORD)
	{
		if (decl == nullptr || handle == nullptr)
			return D3DERR_INVALIDCALL;
		auto v = std::make_unique<vertex_shader>();
		// The declaration: stream selectors, registers, skips, constants
		uint8_t stream = 0;
		uint16_t at = 0;
		const DWORD *t = decl;
		for (int guard = 0; guard < 4096 && *t != D3DVSD_END(); ++guard)
		{
			const DWORD tok = *t;
			const DWORD type = (tok & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;
			v->decl_tokens.push_back(tok);
			++t;
			switch (type)
			{
			case D3DVSD_TOKEN_STREAM:
				stream = uint8_t(tok & 0xF);
				at = 0;
				break;
			case D3DVSD_TOKEN_STREAMDATA:
				if (tok & 0x10000000)
					at = uint16_t(at + 4 * ((tok >> 16) & 0xF)); // D3DVSD_SKIP
				else
				{
					const uint8_t dt = uint8_t((tok >> 16) & 0xF), reg = uint8_t(tok & 0x1F);
					v->decl.elements.push_back({ stream, reg, dt, at });
					at = uint16_t(at + type_size(dt));
				}
				break;
			case D3DVSD_TOKEN_CONSTMEM:
			{
				const DWORD count = (tok >> 25) & 0xF, addr = tok & 0x7F;
				std::vector<float> values(count * 4);
				for (DWORD k = 0; k < count * 4; ++k)
				{
					values[k] = as_float(t[k]);
					v->decl_tokens.push_back(t[k]);
				}
				t += count * 4;
				v->constants.emplace_back(addr, std::move(values));
				break;
			}
			case D3DVSD_TOKEN_EXT:
			{
				const DWORD count = (tok >> 24) & 0x1F;
				for (DWORD k = 0; k < count; ++k)
					v->decl_tokens.push_back(t[k]);
				t += count;
				break;
			}
			default:
				break;
			}
		}
		v->decl_tokens.push_back(D3DVSD_END());
		v->decl.finish();
		if (function != nullptr)
		{
			if (!read_sm1(function, v->code))
			{
				log(2, "CreateVertexShader: %s", v->code.why.c_str());
				return D3DERR_INVALIDCALL;
			}
			v->programmable = true;
		}
		// (handles are odd, as Direct3D 8's: an even value is an FVF)
		vshaders.push_back(std::move(v));
		*handle = DWORD(((vshaders.size() - 1) << 1) | 1);
		return D3D_OK;
	}

	HRESULT device::SetVertexShader(DWORD h)
	{
		device_state &s = target();
		if (recording)
			recording->mask.vs = 1;
		s.vs = h;
		// (the declaration's constants go into the registers as the shader is set)
		if (const vertex_shader *v = vshader(h))
			for (const auto &c : v->constants)
				SetVertexShaderConstant(c.first, c.second.data(), DWORD(c.second.size() / 4));
		dirty |= d_pipeline | d_vs_key | d_ps_key | d_ffvs | d_vsc;
		return D3D_OK;
	}

	HRESULT device::GetVertexShader(DWORD *h)
	{
		if (h == nullptr)
			return D3DERR_INVALIDCALL;
		*h = cur.vs;
		return D3D_OK;
	}

	HRESULT device::DeleteVertexShader(DWORD h)
	{
		const DWORD i = h >> 1;
		if ((h & 1) == 0 || i >= vshaders.size() || vshaders[i] == nullptr)
			return D3DERR_INVALIDCALL;
		// (kept: pipelines made with it stay in the cache; its slot isn't used again)
		return D3D_OK;
	}

	HRESULT device::SetVertexShaderConstant(DWORD r, CONST void *data, DWORD n)
	{
		if (data == nullptr || r + n > kVsConstRegs)
			return D3DERR_INVALIDCALL;
		device_state &s = target();
		std::memcpy(s.vsc[r], data, size_t(n) * 16);
		if (recording)
			for (DWORD k = 0; k < n; ++k)
				recording->mask.vsc[r + k] = 1;
		dirty |= d_vsc;
		return D3D_OK;
	}

	HRESULT device::GetVertexShaderConstant(DWORD r, void *data, DWORD n)
	{
		if (data == nullptr || r + n > kVsConstRegs)
			return D3DERR_INVALIDCALL;
		std::memcpy(data, cur.vsc[r], size_t(n) * 16);
		return D3D_OK;
	}

	HRESULT device::GetVertexShaderDeclaration(DWORD h, void *data, DWORD *size)
	{
		const vertex_shader *v = vshader(h);
		if (v == nullptr || size == nullptr)
			return D3DERR_INVALIDCALL;
		const DWORD bytes = DWORD(v->decl_tokens.size() * 4);
		if (data == nullptr)
		{
			*size = bytes;
			return D3D_OK;
		}
		if (*size < bytes)
			return D3DERR_MOREDATA;
		std::memcpy(data, v->decl_tokens.data(), bytes);
		return D3D_OK;
	}

	HRESULT device::GetVertexShaderFunction(DWORD h, void *data, DWORD *size)
	{
		const vertex_shader *v = vshader(h);
		if (v == nullptr || size == nullptr)
			return D3DERR_INVALIDCALL;
		const DWORD bytes = DWORD(v->code.tokens.size() * 4);
		if (data == nullptr)
		{
			*size = bytes;
			return D3D_OK;
		}
		if (*size < bytes)
			return D3DERR_MOREDATA;
		std::memcpy(data, v->code.tokens.data(), bytes);
		return D3D_OK;
	}

	HRESULT device::CreatePixelShader(CONST DWORD *function, DWORD *handle)
	{
		if (function == nullptr || handle == nullptr)
			return D3DERR_INVALIDCALL;
		auto p = std::make_unique<pixel_shader>();
		if (!read_sm1(function, p->code) || !p->code.pixel)
		{
			log(2, "CreatePixelShader: %s", p->code.why.c_str());
			return D3DERR_INVALIDCALL;
		}
		pshaders.push_back(std::move(p));
		*handle = DWORD(pshaders.size());
		return D3D_OK;
	}

	HRESULT device::SetPixelShader(DWORD h)
	{
		device_state &s = target();
		if (recording)
			recording->mask.ps = 1;
		s.ps = h;
		dirty |= d_pipeline | d_ps_key | d_psc;
		return D3D_OK;
	}

	HRESULT device::GetPixelShader(DWORD *h)
	{
		if (h == nullptr)
			return D3DERR_INVALIDCALL;
		*h = cur.ps;
		return D3D_OK;
	}

	HRESULT device::DeletePixelShader(DWORD h)
	{
		return pshader(h) != nullptr ? D3D_OK : D3DERR_INVALIDCALL;
	}

	HRESULT device::SetPixelShaderConstant(DWORD r, CONST void *data, DWORD n)
	{
		if (data == nullptr || r + n > 8)
			return D3DERR_INVALIDCALL;
		device_state &s = target();
		std::memcpy(s.psc[r], data, size_t(n) * 16);
		if (recording)
			for (DWORD k = 0; k < n; ++k)
				recording->mask.psc[r + k] = 1;
		dirty |= d_psc;
		return D3D_OK;
	}

	HRESULT device::GetPixelShaderConstant(DWORD r, void *data, DWORD n)
	{
		if (data == nullptr || r + n > 8)
			return D3DERR_INVALIDCALL;
		std::memcpy(data, cur.psc[r], size_t(n) * 16);
		return D3D_OK;
	}

	HRESULT device::GetPixelShaderFunction(DWORD h, void *data, DWORD *size)
	{
		const pixel_shader *p = pshader(h);
		if (p == nullptr || size == nullptr)
			return D3DERR_INVALIDCALL;
		const DWORD bytes = DWORD(p->code.tokens.size() * 4);
		if (data == nullptr)
		{
			*size = bytes;
			return D3D_OK;
		}
		if (*size < bytes)
			return D3DERR_MOREDATA;
		std::memcpy(data, p->code.tokens.data(), bytes);
		return D3D_OK;
	}

	// ---- State blocks ----

	void state_block::hold()
	{
		for (int i = 0; i < int(kMaxStages); ++i)
			if (mask.textures[i] && values.textures[i] != nullptr)
				values.textures[i]->AddRef();
		for (int i = 0; i < int(kMaxStreams); ++i)
			if (mask.streams[i] && values.streams[i] != nullptr)
				values.streams[i]->AddRef();
		if (mask.indices && values.indices != nullptr)
			values.indices->AddRef();
	}

	void state_block::let_go()
	{
		for (int i = 0; i < int(kMaxStages); ++i)
			if (mask.textures[i])
				release(values.textures[i]);
		for (int i = 0; i < int(kMaxStreams); ++i)
			if (mask.streams[i])
				release(values.streams[i]);
		if (mask.indices)
			release(values.indices);
	}

	void device::make_state_mask(D3DSTATEBLOCKTYPE type, state_mask &m)
	{
		m = state_mask();
		const bool all = type == D3DSBT_ALL, pixel = all || type == D3DSBT_PIXELSTATE, vertex = all || type == D3DSBT_VERTEXSTATE;
		if (all)
		{
			std::memset(m.rs, 1, sizeof(m.rs));
			std::memset(m.tss, 1, sizeof(m.tss));
			std::memset(m.world, 1, sizeof(m.world));
			m.view = m.proj = 1;
			std::memset(m.texm, 1, sizeof(m.texm));
			m.material = m.viewport = 1;
			std::memset(m.clip, 1, sizeof(m.clip));
			std::memset(m.textures, 1, sizeof(m.textures));
			std::memset(m.streams, 1, sizeof(m.streams));
			m.indices = m.palette = 1;
		}
		if (pixel)
		{
			for (DWORD s : kPixelRs)
				m.rs[s] = 1;
			for (DWORD st = 0; st < kMaxStages; ++st)
				for (DWORD t = 1; t < kStageStates; ++t)
					if (t != D3DTSS_TEXCOORDINDEX && t != D3DTSS_TEXTURETRANSFORMFLAGS)
						m.tss[st][t] = 1;
			m.ps = 1;
			std::memset(m.psc, 1, sizeof(m.psc));
		}
		if (vertex)
		{
			for (DWORD s : kVertexRs)
				m.rs[s] = 1;
			for (DWORD st = 0; st < kMaxStages; ++st)
				for (DWORD t : kVertexTss)
					m.tss[st][t] = 1;
			for (DWORD l = 0; l < kMaxLights; ++l)
				if (cur.lights[l].set)
					m.light[l] = m.light_enable[l] = 1;
			m.vs = 1;
			std::memset(m.vsc, 1, sizeof(m.vsc));
		}
	}

	void device::capture(state_block &b)
	{
		b.let_go();
		const state_mask &m = b.mask;
		device_state &v = b.values;
		for (DWORD s = 0; s < kRenderStates; ++s)
			if (m.rs[s])
				v.rs[s] = cur.rs[s];
		for (DWORD st = 0; st < kMaxStages; ++st)
			for (DWORD t = 0; t < kStageStates; ++t)
				if (m.tss[st][t])
					v.tss[st][t] = cur.tss[st][t];
		for (int i = 0; i < 4; ++i)
			if (m.world[i])
				v.world[i] = cur.world[i];
		if (m.view)
			v.view = cur.view;
		if (m.proj)
			v.proj = cur.proj;
		for (DWORD i = 0; i < kMaxStages; ++i)
			if (m.texm[i])
				v.texm[i] = cur.texm[i];
		if (m.material)
			v.material = cur.material;
		for (DWORD l = 0; l < kMaxLights; ++l)
		{
			if (m.light[l])
			{
				v.lights[l].light = cur.lights[l].light;
				v.lights[l].set = cur.lights[l].set;
			}
			if (m.light_enable[l])
				v.lights[l].enabled = cur.lights[l].enabled;
		}
		for (int i = 0; i < 6; ++i)
			if (m.clip[i])
				std::memcpy(v.clip[i], cur.clip[i], 16);
		if (m.viewport)
			v.viewport = cur.viewport;
		for (DWORD i = 0; i < kMaxStages; ++i)
			if (m.textures[i])
				v.textures[i] = cur.textures[i];
		for (DWORD i = 0; i < kMaxStreams; ++i)
			if (m.streams[i])
			{
				v.streams[i] = cur.streams[i];
				v.strides[i] = cur.strides[i];
			}
		if (m.indices)
		{
			v.indices = cur.indices;
			v.base_vertex = cur.base_vertex;
		}
		if (m.vs)
			v.vs = cur.vs;
		if (m.ps)
			v.ps = cur.ps;
		for (DWORD i = 0; i < kVsConstRegs; ++i)
			if (m.vsc[i])
				std::memcpy(v.vsc[i], cur.vsc[i], 16);
		for (DWORD i = 0; i < 8; ++i)
			if (m.psc[i])
				std::memcpy(v.psc[i], cur.psc[i], 16);
		if (m.palette)
			v.palette = cur.palette;
		b.hold();
	}

	void device::apply(const state_block &b)
	{
		const state_mask &m = b.mask;
		const device_state &v = b.values;
		for (DWORD s = 0; s < kRenderStates; ++s)
			if (m.rs[s])
				SetRenderState(D3DRENDERSTATETYPE(s), v.rs[s]);
		for (DWORD st = 0; st < kMaxStages; ++st)
			for (DWORD t = 0; t < kStageStates; ++t)
				if (m.tss[st][t])
					SetTextureStageState(st, D3DTEXTURESTAGESTATETYPE(t), v.tss[st][t]);
		for (DWORD i = 0; i < 4; ++i)
			if (m.world[i])
				SetTransform(D3DTRANSFORMSTATETYPE(256 + i), &v.world[i]);
		if (m.view)
			SetTransform(D3DTS_VIEW, &v.view);
		if (m.proj)
			SetTransform(D3DTS_PROJECTION, &v.proj);
		for (DWORD i = 0; i < kMaxStages; ++i)
			if (m.texm[i])
				SetTransform(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + i), &v.texm[i]);
		if (m.material)
			SetMaterial(&v.material);
		for (DWORD l = 0; l < kMaxLights; ++l)
		{
			if (m.light[l] && v.lights[l].set)
				SetLight(l, &v.lights[l].light);
			if (m.light_enable[l])
				LightEnable(l, v.lights[l].enabled);
		}
		for (DWORD i = 0; i < 6; ++i)
			if (m.clip[i])
				SetClipPlane(i, v.clip[i]);
		if (m.viewport)
			SetViewport(&v.viewport);
		for (DWORD i = 0; i < kMaxStages; ++i)
			if (m.textures[i])
				SetTexture(i, v.textures[i]);
		for (DWORD i = 0; i < kMaxStreams; ++i)
			if (m.streams[i])
				SetStreamSource(i, v.streams[i], v.strides[i]);
		if (m.indices)
			SetIndices(v.indices, v.base_vertex);
		if (m.vs)
			SetVertexShader(v.vs);
		if (m.ps)
			SetPixelShader(v.ps);
		for (DWORD i = 0; i < kVsConstRegs; ++i)
			if (m.vsc[i])
				SetVertexShaderConstant(i, v.vsc[i], 1);
		for (DWORD i = 0; i < 8; ++i)
			if (m.psc[i])
				SetPixelShaderConstant(i, v.psc[i], 1);
		if (m.palette)
			SetCurrentTexturePalette(v.palette);
	}

	HRESULT device::BeginStateBlock()
	{
		if (recording != nullptr)
			return D3DERR_INVALIDCALL;
		recording = new state_block();
		// (what isn't set while recording keeps the device's values: copied in, the mask says what counts)
		recording->values = cur;
		for (IDirect3DBaseTexture8 *&t : recording->values.textures)
			t = nullptr;
		for (IDirect3DVertexBuffer8 *&s : recording->values.streams)
			s = nullptr;
		recording->values.indices = nullptr;
		return D3D_OK;
	}

	HRESULT device::EndStateBlock(DWORD *token)
	{
		if (recording == nullptr || token == nullptr)
			return D3DERR_INVALIDCALL;
		// (the references the recording took, held for the block's life)
		state_block *b = recording;
		recording = nullptr;
		for (DWORD i = 0; i < kMaxStages; ++i)
			if (!b->mask.textures[i])
				b->values.textures[i] = nullptr;
		for (DWORD i = 0; i < kMaxStreams; ++i)
			if (!b->mask.streams[i])
				b->values.streams[i] = nullptr;
		if (!b->mask.indices)
			b->values.indices = nullptr;
		blocks.emplace_back(b);
		*token = DWORD(blocks.size());
		return D3D_OK;
	}

	HRESULT device::ApplyStateBlock(DWORD token)
	{
		if (token == 0 || token > blocks.size() || blocks[token - 1] == nullptr || recording != nullptr)
			return D3DERR_INVALIDCALL;
		apply(*blocks[token - 1]);
		return D3D_OK;
	}

	HRESULT device::CaptureStateBlock(DWORD token)
	{
		if (token == 0 || token > blocks.size() || blocks[token - 1] == nullptr || recording != nullptr)
			return D3DERR_INVALIDCALL;
		capture(*blocks[token - 1]);
		return D3D_OK;
	}

	HRESULT device::DeleteStateBlock(DWORD token)
	{
		if (token == 0 || token > blocks.size() || blocks[token - 1] == nullptr)
			return D3DERR_INVALIDCALL;
		blocks[token - 1]->let_go();
		blocks[token - 1].reset();
		// (the slot is used again: tokens stay small, as the game's are)
		return D3D_OK;
	}

	HRESULT device::CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD *token)
	{
		if (token == nullptr || recording != nullptr)
			return D3DERR_INVALIDCALL;
		auto b = std::make_unique<state_block>();
		make_state_mask(type, b->mask);
		capture(*b);
		// (a free slot first)
		for (size_t i = 0; i < blocks.size(); ++i)
			if (blocks[i] == nullptr)
			{
				blocks[i] = std::move(b);
				*token = DWORD(i + 1);
				return D3D_OK;
			}
		blocks.push_back(std::move(b));
		*token = DWORD(blocks.size());
		return D3D_OK;
	}
}
