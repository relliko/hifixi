#include "device.hpp"
#include <algorithm>

namespace hifi::d3d8
{
	// ---- Private data ----

	HRESULT private_data::set(REFGUID g, const void *data, DWORD size, DWORD flags)
	{
		drop(g);
		if (flags & D3DSPD_IUNKNOWN)
		{
			if (size != sizeof(IUnknown *) || data == nullptr)
				return D3DERR_INVALIDCALL;
			IUnknown *u = *static_cast<IUnknown *const *>(data);
			if (u != nullptr)
				u->AddRef();
			objects[g] = u;
			return D3D_OK;
		}
		const uint8_t *p = static_cast<const uint8_t *>(data);
		items[g].assign(p, p + size);
		return D3D_OK;
	}

	HRESULT private_data::get(REFGUID g, void *data, DWORD *size)
	{
		if (size == nullptr)
			return D3DERR_INVALIDCALL;
		const auto o = objects.find(g);
		if (o != objects.end())
		{
			if (data == nullptr)
			{
				*size = sizeof(IUnknown *);
				return D3D_OK;
			}
			if (*size < sizeof(IUnknown *))
				return D3DERR_MOREDATA;
			*static_cast<IUnknown **>(data) = o->second;
			if (o->second != nullptr)
				o->second->AddRef();
			return D3D_OK;
		}
		const auto it = items.find(g);
		if (it == items.end())
			return D3DERR_NOTFOUND;
		if (data == nullptr)
		{
			*size = DWORD(it->second.size());
			return D3D_OK;
		}
		if (*size < it->second.size())
		{
			*size = DWORD(it->second.size());
			return D3DERR_MOREDATA;
		}
		std::memcpy(data, it->second.data(), it->second.size());
		*size = DWORD(it->second.size());
		return D3D_OK;
	}

	HRESULT private_data::drop(REFGUID g)
	{
		const auto o = objects.find(g);
		if (o != objects.end())
		{
			if (o->second != nullptr)
				o->second->Release();
			objects.erase(o);
			return D3D_OK;
		}
		return items.erase(g) != 0 ? D3D_OK : D3DERR_NOTFOUND;
	}

	private_data::~private_data()
	{
		for (auto &o : objects)
			if (o.second != nullptr)
				o.second->Release();
	}

	// ---- Images ----

	bool image::make(D3DRESOURCETYPE t, UINT w, UINT h, UINT d, UINT l, UINT f, DWORD use, D3DFORMAT fm, D3DPOOL p, D3DMULTISAMPLE_TYPE ms)
	{
		type = t;
		width = std::max<UINT>(w, 1);
		height = std::max<UINT>(h, 1);
		depth = std::max<UINT>(d, 1);
		faces = f;
		usage = use;
		format = fm;
		pool = p;
		multisample = ms;
		const format_info &fi = info(fm);
		if (!fi.known)
		{
			log_once(1, "format %u isn't supported", unsigned(fm));
			return false;
		}
		// (no DXT texture whose top level isn't whole 4x4 blocks: dgVoodoo makes none either)
		if (fi.block_bytes != 0 && ((width & 3) != 0 || (height & 3) != 0))
		{
			log_once(1, "a DXT texture of %ux%u (not whole blocks) refused, as dgVoodoo refuses it", width, height);
			return false;
		}
		// (all levels down to 1x1 when 0 asked)
		UINT most = 1;
		for (UINT s = std::max(std::max(width, height), depth); s > 1; s >>= 1)
			most++;
		levels = l == 0 ? most : std::min(l, most);
		if (fi.depth || (use & D3DUSAGE_DEPTHSTENCIL))
			kind = image_kind::depth;
		else if (use & D3DUSAGE_RENDERTARGET)
			kind = image_kind::target;
		else if (p == D3DPOOL_SYSTEMMEM)
			kind = image_kind::sysmem;
		else
			kind = image_kind::texture;
		const UINT subs = levels * faces;
		cpu.resize(subs);
		dirty.assign(subs, 0);
		cpu_stale.assign(subs, kind == image_kind::target ? 1 : 0);
		if (kind == image_kind::sysmem)
		{
			dev->images.push_back(this);
			return true;
		}

		d3d8::gpu &g = dev->g;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension = t == D3DRTYPE_VOLUMETEXTURE ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.Width = width;
		rd.Height = height;
		rd.DepthOrArraySize = UINT16(t == D3DRTYPE_VOLUMETEXTURE ? depth : faces);
		rd.MipLevels = UINT16(levels);
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
		D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COPY_DEST;
		D3D12_CLEAR_VALUE clear = {};
		const D3D12_CLEAR_VALUE *cv = nullptr;
		DXGI_FORMAT view = fi.view;
		UINT mapping = fi.mapping;
		if (kind == image_kind::target)
		{
			rd.Format = kTargetResource;
			rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			if (ms > D3DMULTISAMPLE_NONE)
			{
				rd.SampleDesc.Count = UINT(ms);
				rd.MipLevels = 1;
				levels = 1;
			}
			initial = D3D12_RESOURCE_STATE_RENDER_TARGET;
			view = kTargetView;
			mapping = target_mapping(fm);
			gpu_format = kTargetView;
		}
		else if (kind == image_kind::depth)
		{
			rd.Format = fi.resource;
			rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
			if (ms > D3DMULTISAMPLE_NONE)
				rd.SampleDesc.Count = UINT(ms);
			rd.MipLevels = 1;
			levels = 1;
			initial = D3D12_RESOURCE_STATE_DEPTH_READ;
			clear.Format = fi.view;
			clear.DepthStencil.Depth = 1.0f;
			cv = &clear;
			gpu.depth = true;
			gpu.planes = fi.view == DXGI_FORMAT_D24_UNORM_S8_UINT ? 2 : 1;
			gpu_format = fi.view;
		}
		else
		{
			decode = is_dxt(fm) && ((width & 3) != 0 || (height & 3) != 0);
			rd.Format = decode ? DXGI_FORMAT_B8G8R8A8_UNORM : fi.resource;
			if (decode)
			{
				view = DXGI_FORMAT_B8G8R8A8_UNORM;
				mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			}
			gpu_format = rd.Format;
		}
		if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial, cv, IID_PPV_ARGS(&gpu.res))))
		{
			log(2, "a %ux%u texture of format %u (%u levels, usage 0x%X) couldn't be made", width, height, unsigned(fm), levels, unsigned(use));
			return false;
		}
		gpu.state = initial;
		if (kind == image_kind::depth)
			gpu.plane[0] = gpu.plane[1] = initial;

		// Views
		if (kind == image_kind::depth)
		{
			dsvs.resize(1);
			dsvs[0] = g.new_dsv();
			D3D12_DEPTH_STENCIL_VIEW_DESC dd = {};
			dd.Format = fi.view;
			dd.ViewDimension = ms > D3DMULTISAMPLE_NONE ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
			g.device->CreateDepthStencilView(gpu.res, &dd, dsvs[0]);
		}
		else
		{
			if (kind == image_kind::target)
			{
				rtvs.resize(levels * faces);
				for (UINT fc = 0; fc < faces; ++fc)
					for (UINT lv = 0; lv < levels; ++lv)
					{
						D3D12_CPU_DESCRIPTOR_HANDLE &slot = rtvs[subresource(fc, lv)];
						slot = g.new_rtv();
						D3D12_RENDER_TARGET_VIEW_DESC vd = {};
						vd.Format = kTargetView;
						if (ms > D3DMULTISAMPLE_NONE)
							vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
						else if (faces > 1)
						{
							vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
							vd.Texture2DArray.MipSlice = lv;
							vd.Texture2DArray.FirstArraySlice = fc;
							vd.Texture2DArray.ArraySize = 1;
						}
						else
						{
							vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
							vd.Texture2D.MipSlice = lv;
						}
						g.device->CreateRenderTargetView(gpu.res, &vd, slot);
					}
				if (ms > D3DMULTISAMPLE_NONE)
				{
					// (the single-sample copy the target is resolved into when it's read)
					D3D12_RESOURCE_DESC sd = rd;
					sd.SampleDesc.Count = 1;
					if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&resolved))))
						resolved = nullptr;
				}
			}
			srv = g.new_srv();
			D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = view;
			sd.Shader4ComponentMapping = mapping;
			if (t == D3DRTYPE_VOLUMETEXTURE)
			{
				sd.ViewDimension = srv_dim = D3D12_SRV_DIMENSION_TEXTURE3D;
				sd.Texture3D.MipLevels = levels;
			}
			else if (t == D3DRTYPE_CUBETEXTURE)
			{
				sd.ViewDimension = srv_dim = D3D12_SRV_DIMENSION_TEXTURECUBE;
				sd.TextureCube.MipLevels = levels;
			}
			else
			{
				sd.ViewDimension = srv_dim = D3D12_SRV_DIMENSION_TEXTURE2D;
				sd.Texture2D.MipLevels = levels;
			}
			g.device->CreateShaderResourceView(resolved != nullptr ? resolved : gpu.res, &sd, srv);
		}
		dev->images.push_back(this);
		return true;
	}

	void image::drop_gpu()
	{
		d3d8::gpu &g = dev->g;
		if (gpu.res != nullptr)
			g.defer_release(gpu.res);
		if (resolved != nullptr)
			g.defer_release(resolved);
		gpu.res = nullptr;
		resolved = nullptr;
		const auto s = srv;
		const auto r = rtvs;
		const auto d = dsvs;
		d3d8::gpu *gp = &g;
		g.defer([gp, s, r, d] {
			gp->free_srv(s);
			for (const auto &h : r)
				gp->free_rtv(h);
			for (const auto &h : d)
				gp->free_dsv(h);
		});
		srv = {};
		rtvs.clear();
		dsvs.clear();
	}

	image::~image()
	{
		for (pending_read &p : reads)
			dev->g.free_readback(p.rb);
		drop_gpu();
		dev->forget(this);
	}

	std::vector<uint8_t> &image::copy(UINT sub)
	{
		finish_reads();
		std::vector<uint8_t> &c = cpu[sub];
		const UINT level = sub % levels;
		const size_t bytes = size_t(slice(level)) * level_depth(level);
		if (c.size() != bytes)
			c.assign(bytes, 0);
		if (kind == image_kind::target && gpu.res != nullptr)
			read_back(sub);
		return c;
	}

	void image::finish_reads()
	{
		if (reads.empty())
			return;
		d3d8::gpu &g = dev->g;
		for (pending_read &p : reads)
		{
			if (p.rb.fence > g.last_submitted())
				g.submit(true);
			g.wait_for(p.rb.fence);
			std::vector<uint8_t> &c = cpu[p.sub];
			const UINT level = p.sub % levels;
			const size_t bytes = size_t(slice(level)) * level_depth(level);
			if (c.size() != bytes)
				c.assign(bytes, 0);
			uint8_t *mapped = nullptr;
			const D3D12_RANGE whole = { 0, SIZE_T(p.rb.size) };
			if (SUCCEEDED(p.rb.buffer->Map(0, &whole, reinterpret_cast<void **>(&mapped))))
			{
				const UINT pt = pitch(level);
				const UINT bpp = info(format).bpp != 0 ? info(format).bpp : 4;
				uint8_t *dst = c.data() + size_t(p.y) * pt + size_t(p.x) * bpp;
				if (p.target)
					from_target(format, mapped, p.row_pitch, dst, pt, p.w, p.h);
				else
					for (UINT y = 0; y < p.h; ++y)
						std::memcpy(dst + size_t(y) * pt, mapped + size_t(y) * p.row_pitch, size_t(p.w) * bpp);
				const D3D12_RANGE none = { 0, 0 };
				p.rb.buffer->Unmap(0, &none);
			}
			g.free_readback(p.rb);
			if (kind != image_kind::sysmem)
				dirty[p.sub] = 1;
		}
		reads.clear();
	}

	void image::upload()
	{
		if (gpu.res == nullptr || kind != image_kind::texture)
			return;
		finish_reads();
		for (UINT s = 0; s < UINT(dirty.size()); ++s)
			if (dirty[s])
				upload(s);
	}

	void image::upload(UINT sub)
	{
		d3d8::gpu &g = dev->g;
		std::vector<uint8_t> &c = cpu[sub];
		dirty[sub] = 0;
		if (c.empty() || gpu.res == nullptr)
			return;
		const UINT level = sub % levels;
		const UINT w = level_width(level), h = level_height(level), d = level_depth(level);
		const D3D12_RESOURCE_DESC rd = gpu.res->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
		UINT rows = 0;
		UINT64 row_bytes = 0, total = 0;
		g.device->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_bytes, &total);
		upload_alloc a = g.upload(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		if (a.cpu == nullptr)
			return;
		const UINT src_pitch = pitch(level), src_slice = slice(level);
		const PALETTEENTRY *palette = dev->palettes[dev->cur.palette & 255];
		for (UINT z = 0; z < d; ++z)
		{
			const uint8_t *src = c.data() + size_t(z) * src_slice;
			uint8_t *dst = a.cpu + size_t(z) * fp.Footprint.RowPitch * rows;
			if (decode)
				decode_dxt(format, src, src_pitch, dst, fp.Footprint.RowPitch, w, h);
			else
				to_gpu(format, src, src_pitch, dst, fp.Footprint.RowPitch, w, h, palette);
		}
		fp.Offset = a.offset;
		g.transition(gpu, D3D12_RESOURCE_STATE_COPY_DEST);
		g.flush_barriers();
		D3D12_TEXTURE_COPY_LOCATION to = {};
		to.pResource = gpu.res;
		to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		to.SubresourceIndex = sub;
		D3D12_TEXTURE_COPY_LOCATION from = {};
		from.pResource = a.resource;
		from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		from.PlacedFootprint = fp;
		g.list()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
		dev->stats.uploads++;
		dev->stats.upload_bytes += total;
		// (the default pool's textures can't be locked: their copy isn't kept)
		if (pool == D3DPOOL_DEFAULT && (usage & D3DUSAGE_DYNAMIC) == 0)
		{
			c.clear();
			c.shrink_to_fit();
		}
	}

	ID3D12Resource *image::sample_resource()
	{
		if (resolved == nullptr)
			return gpu.res;
		d3d8::gpu &g = dev->g;
		gpu_resource r;
		r.res = resolved;
		r.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		g.transition(gpu, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		g.transition(r, D3D12_RESOURCE_STATE_RESOLVE_DEST);
		g.flush_barriers();
		g.list()->ResolveSubresource(resolved, 0, gpu.res, 0, kTargetView);
		g.transition(r, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		return resolved;
	}

	void image::read_back(UINT sub)
	{
		d3d8::gpu &g = dev->g;
		ID3D12Resource *src = sample_resource();
		gpu_resource single;
		gpu_resource &s = src == gpu.res ? gpu : single;
		if (src != gpu.res)
		{
			single.res = src;
			single.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		}
		const UINT level = sub % levels;
		const D3D12_RESOURCE_DESC rd = src->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
		UINT rows = 0;
		UINT64 row_bytes = 0, total = 0;
		g.device->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_bytes, &total);
		readback rb = g.new_readback(total);
		if (rb.buffer == nullptr)
			return;
		g.transition(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
		g.flush_barriers();
		D3D12_TEXTURE_COPY_LOCATION to = {};
		to.pResource = rb.buffer;
		to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		to.PlacedFootprint = fp;
		D3D12_TEXTURE_COPY_LOCATION from = {};
		from.pResource = src;
		from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		from.SubresourceIndex = sub;
		g.list()->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
		if (src != gpu.res)
			g.transition(single, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		g.submit(true);
		dev->stats.readbacks++;
		uint8_t *mapped = nullptr;
		const D3D12_RANGE whole = { 0, SIZE_T(total) };
		if (SUCCEEDED(rb.buffer->Map(0, &whole, reinterpret_cast<void **>(&mapped))))
		{
			from_target(format, mapped, fp.Footprint.RowPitch, cpu[sub].data(), pitch(level), level_width(level), level_height(level));
			const D3D12_RANGE none = { 0, 0 };
			rb.buffer->Unmap(0, &none);
		}
		g.free_readback(rb);
	}

	void image::write_target(UINT sub, const RECT *r)
	{
		d3d8::gpu &g = dev->g;
		if (gpu.res == nullptr || cpu[sub].empty())
			return;
		const UINT level = sub % levels;
		RECT whole = { 0, 0, LONG(level_width(level)), LONG(level_height(level)) };
		const RECT rc = r != nullptr ? *r : whole;
		const UINT w = UINT(rc.right - rc.left), h = UINT(rc.bottom - rc.top);
		if (w == 0 || h == 0)
			return;
		const UINT row = align_up(w * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
		upload_alloc a = g.upload(UINT64(row) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		if (a.cpu == nullptr)
			return;
		const UINT bpp = info(format).bpp != 0 ? info(format).bpp : 4;
		to_target(format, cpu[sub].data() + size_t(rc.top) * pitch(level) + size_t(rc.left) * bpp, pitch(level), a.cpu, row, w, h);
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
		fp.Offset = a.offset;
		fp.Footprint.Format = kTargetView;
		fp.Footprint.Width = w;
		fp.Footprint.Height = h;
		fp.Footprint.Depth = 1;
		fp.Footprint.RowPitch = row;
		g.transition(gpu, D3D12_RESOURCE_STATE_COPY_DEST);
		g.flush_barriers();
		D3D12_TEXTURE_COPY_LOCATION to = {};
		to.pResource = gpu.res;
		to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		to.SubresourceIndex = sub;
		D3D12_TEXTURE_COPY_LOCATION from = {};
		from.pResource = a.resource;
		from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		from.PlacedFootprint = fp;
		g.list()->CopyTextureRegion(&to, UINT(rc.left), UINT(rc.top), 0, &from, nullptr);
	}

	// ---- Surfaces ----

	surface::surface(device *d, image *i, UINT f, UINT l, IUnknown *c, bool o) : img(i), face(f), level(l), container(c), owns(o), dev(d) {}

	surface::~surface()
	{
		if (owns)
			delete img;
	}

	HRESULT surface::QueryInterface(REFIID riid, void **ppv)
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3DSurface8)
		{
			*ppv = this;
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	ULONG surface::AddRef()
	{
		if (container != nullptr)
			return container->AddRef();
		return rc.add();
	}

	ULONG surface::Release()
	{
		if (container != nullptr)
			return container->Release();
		const ULONG n = rc.sub();
		if (n == 0)
			delete this;
		return n;
	}

	HRESULT surface::GetDevice(IDirect3DDevice8 **pp)
	{
		if (pp == nullptr)
			return D3DERR_INVALIDCALL;
		*pp = dev;
		dev->AddRef();
		return D3D_OK;
	}
	HRESULT surface::SetPrivateData(REFGUID g, CONST void *p, DWORD n, DWORD f) { return common.data.set(g, p, n, f); }
	HRESULT surface::GetPrivateData(REFGUID g, void *p, DWORD *n) { return common.data.get(g, p, n); }
	HRESULT surface::FreePrivateData(REFGUID g) { return common.data.drop(g); }

	HRESULT surface::GetContainer(REFIID riid, void **pp)
	{
		if (pp == nullptr)
			return D3DERR_INVALIDCALL;
		if (container != nullptr)
			return container->QueryInterface(riid, pp);
		return dev->QueryInterface(riid, pp);
	}

	HRESULT surface::GetDesc(D3DSURFACE_DESC *d)
	{
		if (d == nullptr)
			return D3DERR_INVALIDCALL;
		d->Format = img->format;
		d->Type = D3DRTYPE_SURFACE;
		d->Usage = img->kind == image_kind::target ? D3DUSAGE_RENDERTARGET : img->kind == image_kind::depth ? D3DUSAGE_DEPTHSTENCIL : img->usage;
		d->Pool = img->pool;
		d->Size = img->slice(level);
		d->MultiSampleType = img->multisample;
		d->Width = width();
		d->Height = height();
		return D3D_OK;
	}

	HRESULT surface::LockRect(D3DLOCKED_RECT *lr, CONST RECT *r, DWORD flags)
	{
		if (lr == nullptr || locked || !img->lockable())
			return D3DERR_INVALIDCALL;
		std::vector<uint8_t> &c = img->copy(sub());
		const UINT pt = img->pitch(level);
		const format_info &fi = info(img->format);
		size_t at = 0;
		lock_rect = r != nullptr ? *r : RECT { 0, 0, LONG(width()), LONG(height()) };
		if (r != nullptr)
		{
			if (fi.block_bytes != 0)
				at = size_t(r->top / 4) * pt + size_t(r->left / 4) * fi.block_bytes;
			else
				at = size_t(r->top) * pt + size_t(r->left) * fi.bpp;
		}
		lr->pBits = c.data() + at;
		lr->Pitch = INT(pt);
		locked = true;
		lock_flags = flags;
		return D3D_OK;
	}

	HRESULT surface::UnlockRect()
	{
		if (!locked)
			return D3DERR_INVALIDCALL;
		locked = false;
		if (lock_flags & D3DLOCK_READONLY)
			return D3D_OK;
		if (img->kind == image_kind::texture)
			img->dirty[sub()] = 1;
		else if (img->kind == image_kind::target)
			img->write_target(sub(), &lock_rect);
		return D3D_OK;
	}

	// ---- Volumes ----

	volume::volume(device *d, image *i, UINT l, IUnknown *c) : img(i), level(l), container(c), dev(d) {}

	HRESULT volume::QueryInterface(REFIID riid, void **ppv)
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3DVolume8)
		{
			*ppv = this;
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG volume::AddRef() { return container->AddRef(); }
	ULONG volume::Release() { return container->Release(); }
	HRESULT volume::GetDevice(IDirect3DDevice8 **pp)
	{
		if (pp == nullptr)
			return D3DERR_INVALIDCALL;
		*pp = dev;
		dev->AddRef();
		return D3D_OK;
	}
	HRESULT volume::SetPrivateData(REFGUID g, CONST void *p, DWORD n, DWORD f) { return common.data.set(g, p, n, f); }
	HRESULT volume::GetPrivateData(REFGUID g, void *p, DWORD *n) { return common.data.get(g, p, n); }
	HRESULT volume::FreePrivateData(REFGUID g) { return common.data.drop(g); }
	HRESULT volume::GetContainer(REFIID riid, void **pp) { return pp != nullptr ? container->QueryInterface(riid, pp) : D3DERR_INVALIDCALL; }
	HRESULT volume::GetDesc(D3DVOLUME_DESC *d)
	{
		if (d == nullptr)
			return D3DERR_INVALIDCALL;
		d->Format = img->format;
		d->Type = D3DRTYPE_VOLUME;
		d->Usage = img->usage;
		d->Pool = img->pool;
		d->Size = img->slice(level) * img->level_depth(level);
		d->Width = img->level_width(level);
		d->Height = img->level_height(level);
		d->Depth = img->level_depth(level);
		return D3D_OK;
	}
	HRESULT volume::LockBox(D3DLOCKED_BOX *lb, CONST D3DBOX *b, DWORD flags)
	{
		if (lb == nullptr || locked)
			return D3DERR_INVALIDCALL;
		std::vector<uint8_t> &c = img->copy(level);
		const UINT pt = img->pitch(level), sl = img->slice(level);
		const UINT bpp = info(img->format).bpp != 0 ? info(img->format).bpp : 4;
		const size_t at = b != nullptr ? size_t(b->Front) * sl + size_t(b->Top) * pt + size_t(b->Left) * bpp : 0;
		lb->pBits = c.data() + at;
		lb->RowPitch = INT(pt);
		lb->SlicePitch = INT(sl);
		locked = (flags & D3DLOCK_READONLY) == 0;
		if (!locked)
			return D3D_OK;
		return D3D_OK;
	}
	HRESULT volume::UnlockBox()
	{
		if (locked && img->kind == image_kind::texture)
			img->dirty[level] = 1;
		locked = false;
		return D3D_OK;
	}

	// ---- Textures ----

	base_texture *texture_of(IDirect3DBaseTexture8 *t)
	{
		if (t == nullptr)
			return nullptr;
		switch (t->GetType())
		{
		case D3DRTYPE_TEXTURE: return static_cast<texture *>(static_cast<IDirect3DTexture8 *>(t));
		case D3DRTYPE_CUBETEXTURE: return static_cast<cube_texture *>(static_cast<IDirect3DCubeTexture8 *>(t));
		case D3DRTYPE_VOLUMETEXTURE: return static_cast<volume_texture *>(static_cast<IDirect3DVolumeTexture8 *>(t));
		default: return nullptr;
		}
	}

#define HIFI_D3D8_RESOURCE_IMPL(cls, iface, rtype) \
	HRESULT cls::QueryInterface(REFIID riid, void **ppv) \
	{ \
		if (ppv == nullptr) \
			return E_POINTER; \
		if (riid == IID_IUnknown || riid == IID_IDirect3DResource8 || riid == IID_##iface || (rtype != D3DRTYPE_VERTEXBUFFER && rtype != D3DRTYPE_INDEXBUFFER && riid == IID_IDirect3DBaseTexture8)) \
		{ \
			*ppv = static_cast<iface *>(this); \
			AddRef(); \
			return S_OK; \
		} \
		*ppv = nullptr; \
		return E_NOINTERFACE; \
	} \
	ULONG cls::AddRef() { return rc.add(); } \
	ULONG cls::Release() \
	{ \
		const ULONG n = rc.sub(); \
		if (n == 0) \
			delete this; \
		return n; \
	} \
	HRESULT cls::GetDevice(IDirect3DDevice8 **pp) \
	{ \
		if (pp == nullptr) \
			return D3DERR_INVALIDCALL; \
		*pp = dev; \
		dev->AddRef(); \
		return D3D_OK; \
	} \
	HRESULT cls::SetPrivateData(REFGUID g, CONST void *p, DWORD n, DWORD f) { return common.data.set(g, p, n, f); } \
	HRESULT cls::GetPrivateData(REFGUID g, void *p, DWORD *n) { return common.data.get(g, p, n); } \
	HRESULT cls::FreePrivateData(REFGUID g) { return common.data.drop(g); } \
	DWORD cls::SetPriority(DWORD p) { const DWORD o = common.priority; common.priority = p; return o; } \
	DWORD cls::GetPriority() { return common.priority; } \
	void cls::PreLoad() {} \
	D3DRESOURCETYPE cls::GetType() { return rtype; }

#define HIFI_D3D8_BASE_TEXTURE_IMPL(cls) \
	DWORD cls::SetLOD(DWORD l) { const DWORD o = lod; lod = std::min<DWORD>(l, img->levels - 1); return o; } \
	DWORD cls::GetLOD() { return lod; } \
	DWORD cls::GetLevelCount() { return img->levels; }

	HIFI_D3D8_RESOURCE_IMPL(texture, IDirect3DTexture8, D3DRTYPE_TEXTURE)
	HIFI_D3D8_BASE_TEXTURE_IMPL(texture)
	HIFI_D3D8_RESOURCE_IMPL(cube_texture, IDirect3DCubeTexture8, D3DRTYPE_CUBETEXTURE)
	HIFI_D3D8_BASE_TEXTURE_IMPL(cube_texture)
	HIFI_D3D8_RESOURCE_IMPL(volume_texture, IDirect3DVolumeTexture8, D3DRTYPE_VOLUMETEXTURE)
	HIFI_D3D8_BASE_TEXTURE_IMPL(volume_texture)
	HIFI_D3D8_RESOURCE_IMPL(vertex_buffer, IDirect3DVertexBuffer8, D3DRTYPE_VERTEXBUFFER)
	HIFI_D3D8_RESOURCE_IMPL(index_buffer, IDirect3DIndexBuffer8, D3DRTYPE_INDEXBUFFER)

	texture::~texture()
	{
		levels.clear();
		delete img;
	}

	HRESULT texture::GetLevelDesc(UINT l, D3DSURFACE_DESC *d)
	{
		if (l >= levels.size())
			return D3DERR_INVALIDCALL;
		return levels[l]->GetDesc(d);
	}
	HRESULT texture::GetSurfaceLevel(UINT l, IDirect3DSurface8 **pp)
	{
		if (pp == nullptr || l >= levels.size())
			return D3DERR_INVALIDCALL;
		*pp = levels[l].get();
		AddRef();
		return D3D_OK;
	}
	HRESULT texture::LockRect(UINT l, D3DLOCKED_RECT *lr, CONST RECT *r, DWORD f)
	{
		if (l >= levels.size())
			return D3DERR_INVALIDCALL;
		return levels[l]->LockRect(lr, r, f);
	}
	HRESULT texture::UnlockRect(UINT l)
	{
		if (l >= levels.size())
			return D3DERR_INVALIDCALL;
		return levels[l]->UnlockRect();
	}
	HRESULT texture::AddDirtyRect(CONST RECT *)
	{
		std::fill(img->dirty.begin(), img->dirty.end(), uint8_t(img->kind == image_kind::texture));
		for (UINT s = 0; s < img->dirty.size(); ++s)
			if (img->cpu[s].empty())
				img->dirty[s] = 0;
		return D3D_OK;
	}

	cube_texture::~cube_texture()
	{
		faces.clear();
		delete img;
	}
	HRESULT cube_texture::GetLevelDesc(UINT l, D3DSURFACE_DESC *d)
	{
		if (l >= img->levels)
			return D3DERR_INVALIDCALL;
		return faces[l]->GetDesc(d);
	}
	HRESULT cube_texture::GetCubeMapSurface(D3DCUBEMAP_FACES f, UINT l, IDirect3DSurface8 **pp)
	{
		if (pp == nullptr || l >= img->levels || UINT(f) >= 6)
			return D3DERR_INVALIDCALL;
		*pp = faces[img->subresource(UINT(f), l)].get();
		AddRef();
		return D3D_OK;
	}
	HRESULT cube_texture::LockRect(D3DCUBEMAP_FACES f, UINT l, D3DLOCKED_RECT *lr, CONST RECT *r, DWORD fl)
	{
		if (l >= img->levels || UINT(f) >= 6)
			return D3DERR_INVALIDCALL;
		return faces[img->subresource(UINT(f), l)]->LockRect(lr, r, fl);
	}
	HRESULT cube_texture::UnlockRect(D3DCUBEMAP_FACES f, UINT l)
	{
		if (l >= img->levels || UINT(f) >= 6)
			return D3DERR_INVALIDCALL;
		return faces[img->subresource(UINT(f), l)]->UnlockRect();
	}
	HRESULT cube_texture::AddDirtyRect(D3DCUBEMAP_FACES, CONST RECT *) { return D3D_OK; }

	volume_texture::~volume_texture()
	{
		levels.clear();
		delete img;
	}
	HRESULT volume_texture::GetLevelDesc(UINT l, D3DVOLUME_DESC *d)
	{
		if (l >= levels.size())
			return D3DERR_INVALIDCALL;
		return levels[l]->GetDesc(d);
	}
	HRESULT volume_texture::GetVolumeLevel(UINT l, IDirect3DVolume8 **pp)
	{
		if (pp == nullptr || l >= levels.size())
			return D3DERR_INVALIDCALL;
		*pp = levels[l].get();
		AddRef();
		return D3D_OK;
	}
	HRESULT volume_texture::LockBox(UINT l, D3DLOCKED_BOX *lb, CONST D3DBOX *b, DWORD f)
	{
		if (l >= levels.size())
			return D3DERR_INVALIDCALL;
		return levels[l]->LockBox(lb, b, f);
	}
	HRESULT volume_texture::UnlockBox(UINT l)
	{
		if (l >= levels.size())
			return D3DERR_INVALIDCALL;
		return levels[l]->UnlockBox();
	}
	HRESULT volume_texture::AddDirtyBox(CONST D3DBOX *) { return D3D_OK; }

	// ---- Buffers ----

	bool buffer::make(UINT n, DWORD u, D3DPOOL p)
	{
		size = std::max<UINT>(n, 4);
		usage = u;
		pool = p;
		bytes.assign(size, 0);
		streaming = (u & D3DUSAGE_DYNAMIC) != 0 || p == D3DPOOL_SYSTEMMEM;
		dirty_lo = 0;
		dirty_hi = size;
		dev->buffers_alive++;
		return true;
	}

	buffer::~buffer()
	{
		if (gpu.res != nullptr)
			dev->g.defer_release(gpu.res);
		dev->buffers_alive--;
	}

	void buffer::lock(UINT offset, UINT n, DWORD flags)
	{
		if (offset >= size)
			return;
		if (n == 0 || offset + n > size)
			n = size - offset;
		if (flags & D3DLOCK_READONLY)
			return;
		if (dirty_lo == dirty_hi)
		{
			dirty_lo = offset;
			dirty_hi = offset + n;
		}
		else
		{
			dirty_lo = std::min(dirty_lo, offset);
			dirty_hi = std::max(dirty_hi, offset + n);
		}
		if ((flags & D3DLOCK_NOOVERWRITE) == 0)
			all_noov = false;
		// (a static buffer locked again after it was drawn with this list: it streams from now on)
		if (!streaming && gpu.res != nullptr && last_draw_epoch == dev->g.epoch && ++uploads > 2)
		{
			streaming = true;
			log_once(0, "vertex/index buffers the game rewrites each frame stream from upload memory (first: %u bytes)", size);
		}
	}

	D3D12_GPU_VIRTUAL_ADDRESS buffer::address(D3D12_RESOURCE_STATES use)
	{
		d3d8::gpu &g = dev->g;
		last_draw_epoch = g.epoch;
		if (streaming)
		{
			if (stream_frame != g.frame_number || stream_cpu == nullptr || (dirty_hi > dirty_lo && !all_noov))
			{
				upload_alloc a = g.upload(size, 256);
				if (a.cpu == nullptr)
					return 0;
				std::memcpy(a.cpu, bytes.data(), size);
				stream_at = a.gpu;
				stream_cpu = a.cpu;
				stream_frame = g.frame_number;
			}
			else if (dirty_hi > dirty_lo)
				std::memcpy(stream_cpu + dirty_lo, bytes.data() + dirty_lo, dirty_hi - dirty_lo);
			dirty_lo = dirty_hi = 0;
			all_noov = true;
			return stream_at;
		}
		if (state_epoch != g.epoch)
		{
			// (buffers decay to COMMON when a list has run)
			state_epoch = g.epoch;
			state = D3D12_RESOURCE_STATE_COMMON;
		}
		if (gpu.res == nullptr)
		{
			D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = align_up(size, 4);
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&gpu.res))))
			{
				streaming = true;
				return address(use);
			}
			state = D3D12_RESOURCE_STATE_COMMON;
			dirty_lo = 0;
			dirty_hi = size;
		}
		auto move = [&](D3D12_RESOURCE_STATES to) {
			if (state == to)
				return;
			if (state != D3D12_RESOURCE_STATE_COMMON)
			{
				gpu_resource r;
				r.res = gpu.res;
				r.state = state;
				g.transition(r, to);
			}
			state = to;
		};
		if (dirty_hi > dirty_lo)
		{
			const UINT lo = dirty_lo & ~3u, hi = std::min(align_up(dirty_hi, 4), align_up(size, 4));
			upload_alloc a = g.upload(hi - lo, 256);
			if (a.cpu != nullptr)
			{
				std::memcpy(a.cpu, bytes.data() + lo, std::min(hi, size) - lo);
				move(D3D12_RESOURCE_STATE_COPY_DEST);
				g.flush_barriers();
				g.list()->CopyBufferRegion(gpu.res, lo, a.resource, a.offset, hi - lo);
				dev->stats.upload_bytes += hi - lo;
			}
			dirty_lo = dirty_hi = 0;
			all_noov = true;
		}
		move(use);
		return gpu.res->GetGPUVirtualAddress();
	}

	HRESULT vertex_buffer::Lock(UINT o, UINT n, BYTE **pp, DWORD f)
	{
		if (pp == nullptr || o > buf.size)
			return D3DERR_INVALIDCALL;
		buf.lock(o, n, f);
		*pp = buf.bytes.data() + o;
		return D3D_OK;
	}
	HRESULT vertex_buffer::Unlock() { return D3D_OK; }
	HRESULT vertex_buffer::GetDesc(D3DVERTEXBUFFER_DESC *d)
	{
		if (d == nullptr)
			return D3DERR_INVALIDCALL;
		d->Format = D3DFMT_VERTEXDATA;
		d->Type = D3DRTYPE_VERTEXBUFFER;
		d->Usage = buf.usage;
		d->Pool = buf.pool;
		d->Size = buf.size;
		d->FVF = fvf;
		return D3D_OK;
	}

	HRESULT index_buffer::Lock(UINT o, UINT n, BYTE **pp, DWORD f)
	{
		if (pp == nullptr || o > buf.size)
			return D3DERR_INVALIDCALL;
		buf.lock(o, n, f);
		*pp = buf.bytes.data() + o;
		return D3D_OK;
	}
	HRESULT index_buffer::Unlock() { return D3D_OK; }
	HRESULT index_buffer::GetDesc(D3DINDEXBUFFER_DESC *d)
	{
		if (d == nullptr)
			return D3DERR_INVALIDCALL;
		d->Format = format;
		d->Type = D3DRTYPE_INDEXBUFFER;
		d->Usage = buf.usage;
		d->Pool = buf.pool;
		d->Size = buf.size;
		return D3D_OK;
	}
}
