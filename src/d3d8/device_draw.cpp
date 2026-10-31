// The device's GPU side (device.hpp): at each draw the shaders and pipeline for the state, the constants, textures,
// samplers and buffers, the resources' states, then only what changed bound again on the list; clears; Present.

#include "device.hpp"
#include "present.hpp"
#include <algorithm>
#include <cmath>

namespace hifi::d3d8
{
	namespace
	{
		// ---- Matrices (row vectors, as Direct3D) ----
		using mat = D3DMATRIX;
		mat mul(const mat &a, const mat &b)
		{
			mat r;
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 4; ++j)
					r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
			return r;
		}
		// The inverse's transpose of the upper 3x3 (normals through it)
		mat normal_matrix(const mat &a)
		{
			const float a00 = a.m[0][0], a01 = a.m[0][1], a02 = a.m[0][2];
			const float a10 = a.m[1][0], a11 = a.m[1][1], a12 = a.m[1][2];
			const float a20 = a.m[2][0], a21 = a.m[2][1], a22 = a.m[2][2];
			const float c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22, c02 = a10 * a21 - a11 * a20;
			const float det = a00 * c00 + a01 * c01 + a02 * c02;
			const float inv = det != 0.0f ? 1.0f / det : 0.0f;
			mat r = {};
			// (inverse transposed = cofactors / det)
			r.m[0][0] = c00 * inv;
			r.m[0][1] = c01 * inv;
			r.m[0][2] = c02 * inv;
			r.m[1][0] = (a02 * a21 - a01 * a22) * inv;
			r.m[1][1] = (a00 * a22 - a02 * a20) * inv;
			r.m[1][2] = (a01 * a20 - a00 * a21) * inv;
			r.m[2][0] = (a01 * a12 - a02 * a11) * inv;
			r.m[2][1] = (a02 * a10 - a00 * a12) * inv;
			r.m[2][2] = (a00 * a11 - a01 * a10) * inv;
			r.m[3][3] = 1.0f;
			return r;
		}
		void put(float *dst, const mat &m) { std::memcpy(dst, &m, 64); }
		void colour(float *dst, const D3DCOLORVALUE &c) { dst[0] = c.r; dst[1] = c.g; dst[2] = c.b; dst[3] = c.a; }
		void colour(float *dst, D3DCOLOR c)
		{
			dst[0] = ((c >> 16) & 255) / 255.0f;
			dst[1] = ((c >> 8) & 255) / 255.0f;
			dst[2] = (c & 255) / 255.0f;
			dst[3] = (c >> 24) / 255.0f;
		}

		D3D12_BLEND blend_of(DWORD b, bool alpha, bool x_target)
		{
			switch (b)
			{
			case D3DBLEND_ZERO: return D3D12_BLEND_ZERO;
			case D3DBLEND_ONE: return D3D12_BLEND_ONE;
			case D3DBLEND_SRCCOLOR: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
			case D3DBLEND_INVSRCCOLOR: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
			case D3DBLEND_SRCALPHA: return D3D12_BLEND_SRC_ALPHA;
			case D3DBLEND_INVSRCALPHA: return D3D12_BLEND_INV_SRC_ALPHA;
			case D3DBLEND_DESTALPHA: return x_target ? D3D12_BLEND_ONE : D3D12_BLEND_DEST_ALPHA;
			case D3DBLEND_INVDESTALPHA: return x_target ? D3D12_BLEND_ZERO : D3D12_BLEND_INV_DEST_ALPHA;
			case D3DBLEND_DESTCOLOR: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
			case D3DBLEND_INVDESTCOLOR: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
			case D3DBLEND_SRCALPHASAT: return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
			default: return D3D12_BLEND_ONE;
			}
		}

		D3D12_FILTER_TYPE filter_type(DWORD f) { return f == D3DTEXF_POINT || f == D3DTEXF_NONE ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR; }

		UINT vertex_count(D3DPRIMITIVETYPE t, UINT n)
		{
			switch (t)
			{
			case D3DPT_POINTLIST: return n;
			case D3DPT_LINELIST: return n * 2;
			case D3DPT_LINESTRIP: return n + 1;
			case D3DPT_TRIANGLELIST: return n * 3;
			case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n + 2;
			default: return 0;
			}
		}

		D3D_PRIMITIVE_TOPOLOGY topology_of(D3DPRIMITIVETYPE t)
		{
			switch (t)
			{
			case D3DPT_POINTLIST: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
			case D3DPT_LINELIST: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
			case D3DPT_LINESTRIP: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
			case D3DPT_TRIANGLESTRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
			default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			}
		}

		D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(D3DPRIMITIVETYPE t)
		{
			switch (t)
			{
			case D3DPT_POINTLIST: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
			case D3DPT_LINELIST: case D3DPT_LINESTRIP: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
			default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			}
		}

		// The pipeline's key (everything Direct3D 12 bakes into a pipeline state)
		struct pso_key
		{
			const void *vs = nullptr, *ps = nullptr;
			uint64_t layout = 0;
			uint8_t topology = 0, blend = 0, src = 0, dst = 0, op = 0, write_mask = 0, x_target = 0, has_ds = 0;
			uint8_t z = 0, z_write = 0, z_func = 0, stencil = 0, s_func = 0, s_fail = 0, s_zfail = 0, s_pass = 0;
			uint8_t s_read = 0, s_write = 0, fill = 0, cull = 0, samples = 1, ds_format = 0;
			int32_t zbias = 0;
		};
	}

	void fill_ff_constants(const device_state &s, const D3DVIEWPORT8 &vp, ff_vs_constants &c)
	{
		std::memset(&c, 0, sizeof(c));
		const mat wv0 = mul(s.world[0], s.view);
		put(c.wvp, mul(wv0, s.proj));
		for (int i = 0; i < 4; ++i)
		{
			const mat wv = i == 0 ? wv0 : mul(s.world[i], s.view);
			put(c.wv[i], wv);
			put(c.nrm[i], normal_matrix(wv));
		}
		put(c.proj, s.proj);
		for (int i = 0; i < 8; ++i)
			put(c.texm[i], s.texm[i]);
		put(c.world, s.world[0]);
		colour(c.mdiffuse, s.material.Diffuse);
		colour(c.mambient, s.material.Ambient);
		colour(c.mspecular, s.material.Specular);
		colour(c.memissive, s.material.Emissive);
		c.mpower[0] = s.material.Power;
		colour(c.gambient, D3DCOLOR(s.rs[D3DRS_AMBIENT]));
		const float start = as_float(s.rs[D3DRS_FOGSTART]), end = as_float(s.rs[D3DRS_FOGEND]);
		c.fogp[0] = start;
		c.fogp[1] = end;
		c.fogp[2] = as_float(s.rs[D3DRS_FOGDENSITY]);
		c.fogp[3] = end != start ? 1.0f / (end - start) : 0.0f;
		c.vp[0] = float(vp.X);
		c.vp[1] = float(vp.Y);
		c.vp[2] = vp.Width != 0 ? 2.0f / float(vp.Width) : 0.0f;
		c.vp[3] = vp.Height != 0 ? 2.0f / float(vp.Height) : 0.0f;
		c.zr[0] = vp.MinZ;
		c.zr[1] = vp.MaxZ - vp.MinZ;
		// (table fog by the eye's depth w for a perspective projection, by z for an orthographic one)
		c.zr[2] = s.proj.m[2][3] != 0.0f ? 1.0f : 0.0f;
		for (int i = 0; i < 6; ++i)
			std::memcpy(c.clipp[i], s.clip[i], 16);
		// The enabled lights, in the order of their indices (as many as 8), in view space
		int n = 0;
		for (DWORD l = 0; l < kMaxLights && n < 8; ++l)
		{
			const light_slot &ls = s.lights[l];
			if (!ls.enabled)
				continue;
			const D3DLIGHT8 &L = ls.light;
			ff_light_c &o = c.lights[n++];
			colour(o.diffuse, L.Diffuse);
			colour(o.specular, L.Specular);
			colour(o.ambient, L.Ambient);
			const D3DVECTOR p = L.Position, d = L.Direction;
			for (int k = 0; k < 3; ++k)
			{
				o.position[k] = p.x * s.view.m[0][k] + p.y * s.view.m[1][k] + p.z * s.view.m[2][k] + s.view.m[3][k];
				o.direction[k] = d.x * s.view.m[0][k] + d.y * s.view.m[1][k] + d.z * s.view.m[2][k];
			}
			const float len = std::sqrt(o.direction[0] * o.direction[0] + o.direction[1] * o.direction[1] + o.direction[2] * o.direction[2]);
			if (len > 0.0f)
				for (int k = 0; k < 3; ++k)
					o.direction[k] /= len;
			o.position[3] = L.Range;
			o.direction[3] = L.Falloff;
			o.atten[0] = L.Attenuation0;
			o.atten[1] = L.Attenuation1;
			o.atten[2] = L.Attenuation2;
			o.spot[0] = std::cos(L.Theta * 0.5f);
			o.spot[1] = std::cos(L.Phi * 0.5f);
		}
	}

	void fill_ps_constants(const device_state &s, ps_constants &c)
	{
		std::memset(&c, 0, sizeof(c));
		colour(c.tfactor, D3DCOLOR(s.rs[D3DRS_TEXTUREFACTOR]));
		colour(c.fogcolor, D3DCOLOR(s.rs[D3DRS_FOGCOLOR]));
		const float start = as_float(s.rs[D3DRS_FOGSTART]), end = as_float(s.rs[D3DRS_FOGEND]);
		c.fogp[0] = start;
		c.fogp[1] = end;
		c.fogp[2] = as_float(s.rs[D3DRS_FOGDENSITY]);
		c.fogp[3] = end != start ? 1.0f / (end - start) : 0.0f;
		c.aref[0] = float(s.rs[D3DRS_ALPHAREF] & 255);
		for (int st = 0; st < 8; ++st)
		{
			c.bumpm[st][0] = as_float(s.tss[st][D3DTSS_BUMPENVMAT00]);
			c.bumpm[st][1] = as_float(s.tss[st][D3DTSS_BUMPENVMAT01]);
			c.bumpm[st][2] = as_float(s.tss[st][D3DTSS_BUMPENVMAT10]);
			c.bumpm[st][3] = as_float(s.tss[st][D3DTSS_BUMPENVMAT11]);
			c.bumpl[st][0] = as_float(s.tss[st][D3DTSS_BUMPENVLSCALE]);
			c.bumpl[st][1] = as_float(s.tss[st][D3DTSS_BUMPENVLOFFSET]);
		}
		std::memcpy(c.pc, s.psc, sizeof(c.pc));
	}

	void device::bind_targets()
	{
		ID3D12GraphicsCommandList *l = g.list();
		if (bound.epoch != g.epoch)
		{
			bound = bound_state();
			bound.epoch = g.epoch;
		}
		if (rt == nullptr)
			return;
		g.transition(rt->img->gpu, D3D12_RESOURCE_STATE_RENDER_TARGET);
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rt->rtv();
		const D3D12_CPU_DESCRIPTOR_HANDLE dsv = ds != nullptr ? ds->dsv() : D3D12_CPU_DESCRIPTOR_HANDLE {};
		if (!bound.targets || bound.rtv.ptr != rtv.ptr || bound.dsv.ptr != dsv.ptr)
		{
			g.flush_barriers();
			l->OMSetRenderTargets(1, &rtv, FALSE, dsv.ptr != 0 ? &dsv : nullptr);
			bound.targets = true;
			bound.rtv = rtv;
			bound.dsv = dsv;
		}
	}

	HRESULT device::clear(DWORD n, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
	{
		if (rt == nullptr)
			return D3DERR_INVALIDCALL;
		if (g.draws_in_list >= kSplitDraws)
			g.submit(false);
		const bool has_stencil = ds != nullptr && info(ds->img->format).stencil;
		if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0 && ds == nullptr)
			flags &= ~(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
		if ((flags & D3DCLEAR_STENCIL) && !has_stencil)
			flags &= ~D3DCLEAR_STENCIL;
		if (flags == 0)
			return D3D_OK;
		stats.clears++;
		// The rects, within the viewport (as Direct3D 8 clears); none when that's the whole target
		const D3DVIEWPORT8 &vp = cur.viewport;
		const RECT view = { LONG(vp.X), LONG(vp.Y), LONG(vp.X + vp.Width), LONG(vp.Y + vp.Height) };
		std::vector<D3D12_RECT> list;
		if (rects == nullptr || n == 0)
		{
			if (view.left > 0 || view.top > 0 || view.right < LONG(rt->width()) || view.bottom < LONG(rt->height()))
				list.push_back(view);
		}
		else
			for (DWORD i = 0; i < n; ++i)
			{
				D3D12_RECT r = { std::max<LONG>(rects[i].x1, view.left), std::max<LONG>(rects[i].y1, view.top), std::min<LONG>(rects[i].x2, view.right), std::min<LONG>(rects[i].y2, view.bottom) };
				if (r.right > r.left && r.bottom > r.top)
					list.push_back(r);
			}
		if (rects != nullptr && n != 0 && list.empty())
			return D3D_OK;
		bind_targets();
		ID3D12GraphicsCommandList *l = g.list();
		if (flags & D3DCLEAR_TARGET)
		{
			float c[4];
			colour(c, color);
			g.flush_barriers();
			l->ClearRenderTargetView(rt->rtv(), c, UINT(list.size()), list.empty() ? nullptr : list.data());
		}
		if (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))
		{
			if (flags & D3DCLEAR_ZBUFFER)
				g.transition_plane(ds->img->gpu, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			if (flags & D3DCLEAR_STENCIL)
				g.transition_plane(ds->img->gpu, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			g.flush_barriers();
			D3D12_CLEAR_FLAGS f = D3D12_CLEAR_FLAGS(0);
			if (flags & D3DCLEAR_ZBUFFER)
				f |= D3D12_CLEAR_FLAG_DEPTH;
			if (flags & D3DCLEAR_STENCIL)
				f |= D3D12_CLEAR_FLAG_STENCIL;
			l->ClearDepthStencilView(ds->dsv(), f, std::clamp(z, 0.0f, 1.0f), UINT8(stencil), UINT(list.size()), list.empty() ? nullptr : list.data());
		}
		return D3D_OK;
	}

	HRESULT device::draw(D3DPRIMITIVETYPE type, UINT count, bool indexed, UINT start, INT base_vertex, UINT min_index, UINT num_vertices,
		const void *up_vertices, UINT up_stride, const void *up_indices, D3DFORMAT up_index_format)
	{
		if (count == 0 || rt == nullptr)
			return D3D_OK;
		const UINT vcount = vertex_count(type, count);
		if (vcount == 0)
			return D3DERR_INVALIDCALL;
		if (g.draws_in_list >= kSplitDraws)
			g.submit(false);
		// (the world's end: this draw reads its target, before anything is bound for it)
		for (IDirect3DBaseTexture8 *t : cur.textures)
			if (const base_texture *bt = texture_of(t))
				mark_world_read(this, bt->img);
		const device_state &s = cur;

		// ---- Shaders ----
		const vertex_shader *vsh = nullptr;
		const declaration *decl = current_declaration(&vsh);
		if (decl == nullptr || decl->layout.empty())
		{
			log_once(1, "a draw with no vertex declaration (shader 0x%X): skipped", unsigned(s.vs));
			return D3D_OK;
		}
		const bool programmable = vsh != nullptr && vsh->programmable;
		const pixel_shader *psh = s.ps != 0 && s.ps - 1 < pshaders.size() ? pshaders[s.ps - 1].get() : nullptr;

		// Textures and stage use (stages the fixed function reads: up to the first disabled one)
		uint8_t tex_kind[8] = {};
		image *tex_img[8] = {};
		for (int st = 0; st < 8; ++st)
			if (base_texture *t = texture_of(s.textures[st]))
			{
				tex_img[st] = t->img;
				tex_kind[st] = t->img->type == D3DRTYPE_CUBETEXTURE ? 2 : t->img->type == D3DRTYPE_VOLUMETEXTURE ? 3 : 1;
			}
		int stages = 0;
		if (psh == nullptr)
			while (stages < 8 && s.tss[stages][D3DTSS_COLOROP] != D3DTOP_DISABLE)
				stages++;
		else
			stages = 8;

		// The vertex shader
		std::string why;
		const std::vector<uint8_t> *vs_code = nullptr;
		uint8_t coords[8] = {};
		const uint8_t clip = s.rs[D3DRS_CLIPPING] ? uint8_t(s.rs[D3DRS_CLIPPLANEENABLE] & 63) : 0;
		if (programmable)
		{
			const uint64_t h = hash_bytes(vsh->code.tokens.data(), vsh->code.tokens.size() * 4, hash_bytes(decl->inputs, 16, clip + 1));
			auto it = vs_cache.find(h);
			if (it == vs_cache.end())
			{
				uint8_t head[17];
				std::memcpy(head, decl->inputs, 16);
				head[16] = clip;
				note_shader_key(key_sm1_vs, head, sizeof(head), vsh->code.tokens.data(), vsh->code.tokens.size() * 4);
				const std::string hlsl = sm1_vs_hlsl(vsh->code, decl->inputs, clip);
				const std::vector<uint8_t> &b = compile(hlsl, "vs_5_0", why);
				if (b.empty())
					log_once(2, "a vs.1.1 shader didn't compile: %s\n%s", why.c_str(), hlsl.c_str());
				it = vs_cache.emplace(h, b.empty() ? nullptr : &b).first;
			}
			vs_code = it->second;
			for (int st = 0; st < 8; ++st)
				coords[st] = 4;
		}
		else
		{
			ff_vs_key k;
			std::memcpy(k.inputs, decl->inputs, 16);
			k.rhw = decl->rhw ? 1 : 0;
			if (!decl->rhw)
			{
				k.lighting = s.rs[D3DRS_LIGHTING] ? 1 : 0;
				// (the local viewer counts for the reflection vector even unlit)
				k.local_viewer = s.rs[D3DRS_LOCALVIEWER] ? 1 : 0;
				if (k.lighting)
				{
					k.normalize = s.rs[D3DRS_NORMALIZENORMALS] ? 1 : 0;
					k.specular = s.rs[D3DRS_SPECULARENABLE] ? 1 : 0;
					k.color_vertex = s.rs[D3DRS_COLORVERTEX] ? 1 : 0;
					k.src_diffuse = uint8_t(s.rs[D3DRS_DIFFUSEMATERIALSOURCE]);
					k.src_ambient = uint8_t(s.rs[D3DRS_AMBIENTMATERIALSOURCE]);
					k.src_specular = uint8_t(s.rs[D3DRS_SPECULARMATERIALSOURCE]);
					k.src_emissive = uint8_t(s.rs[D3DRS_EMISSIVEMATERIALSOURCE]);
					int n = 0;
					for (DWORD l = 0; l < kMaxLights && n < 8; ++l)
						if (s.lights[l].enabled)
							k.light_type[n++] = uint8_t(s.lights[l].light.Type);
				}
				const int weights = decl->inputs[1] >= in_float1 && decl->inputs[1] <= in_float4 ? decl->inputs[1] : 0;
				k.vertex_blend = uint8_t(std::min<DWORD>(s.rs[D3DRS_VERTEXBLEND] & 3, DWORD(weights)));
				if (s.rs[D3DRS_FOGENABLE] && s.rs[D3DRS_FOGTABLEMODE] == D3DFOG_NONE)
				{
					k.fog_vertex = uint8_t(s.rs[D3DRS_FOGVERTEXMODE]);
					k.range_fog = s.rs[D3DRS_RANGEFOGENABLE] ? 1 : 0;
				}
				k.clip_planes = clip;
			}
			for (int st = 0; st < 8; ++st)
			{
				const DWORD tci = s.tss[st][D3DTSS_TEXCOORDINDEX];
				const DWORD tf = s.tss[st][D3DTSS_TEXTURETRANSFORMFLAGS];
				k.stage_index[st] = uint8_t(st < stages ? tci & 7 : st);
				if (st >= stages || decl->rhw)
				{
					const uint8_t in = decl->inputs[7 + (k.stage_index[st] & 7)];
					coords[st] = in >= in_float1 && in <= in_float4 ? in : 2;
					if (st < stages && (tf & D3DTTFF_PROJECTED))
						coords[st] = uint8_t(std::max<int>(coords[st], 2));
					if (st < stages && decl->rhw)
						k.stage_xform[st] = uint8_t((tf & 7) | ((tf & D3DTTFF_PROJECTED) ? 0x80 : 0));
					continue;
				}
				k.stage_gen[st] = uint8_t((tci >> 16) & 3);
				const uint8_t n = uint8_t(tf & 7);
				k.stage_xform[st] = uint8_t(n | ((tf & D3DTTFF_PROJECTED) ? 0x80 : 0));
				const uint8_t in = decl->inputs[7 + (tci & 7)];
				coords[st] = n != 0 ? n : k.stage_gen[st] != 0 ? 3 : (in >= in_float1 && in <= in_float4 ? in : 2);
			}
			const uint64_t h = hash_bytes(&k, sizeof(k), 0x5F5F);
			auto it = vs_cache.find(h);
			if (it == vs_cache.end())
			{
				note_shader_key(key_ff_vs, &k, sizeof(k));
				const std::string hlsl = ff_vs_hlsl(k);
				const std::vector<uint8_t> &b = compile(hlsl, "vs_5_0", why);
				if (b.empty())
					log_once(2, "a fixed-function vertex shader didn't compile: %s\n%s", why.c_str(), hlsl.c_str());
				it = vs_cache.emplace(h, b.empty() ? nullptr : &b).first;
			}
			vs_code = it->second;
		}
		if (vs_code == nullptr)
			return D3D_OK;

		// The pixel shader
		ps_tail tail;
		tail.alpha_func = s.rs[D3DRS_ALPHATESTENABLE] ? uint8_t(s.rs[D3DRS_ALPHAFUNC]) : 0;
		tail.flat = s.rs[D3DRS_SHADEMODE] == D3DSHADE_FLAT ? 1 : 0;
		if (s.rs[D3DRS_FOGENABLE])
		{
			const DWORD table = s.rs[D3DRS_FOGTABLEMODE];
			if (table != D3DFOG_NONE && !programmable)
				tail.fog = uint8_t(table == D3DFOG_EXP ? 2 : table == D3DFOG_EXP2 ? 3 : 4);
			else if (table != D3DFOG_NONE)
				tail.fog = uint8_t(table == D3DFOG_EXP ? 2 : table == D3DFOG_EXP2 ? 3 : 4);
			else if (programmable || decl->rhw || s.rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE)
				tail.fog = 1;
		}
		const std::vector<uint8_t> *ps_code = nullptr;
		{
			uint8_t projected = 0;
			for (int st = 0; st < 8; ++st)
				if (s.tss[st][D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED)
					projected |= uint8_t(1u << st);
			uint64_t h;
			std::string hlsl;
			if (psh != nullptr)
			{
				h = hash_bytes(psh->code.tokens.data(), psh->code.tokens.size() * 4, hash_bytes(tex_kind, 8, hash_bytes(&tail, sizeof(tail), projected + 77)));
				const auto it = ps_cache.find(h);
				if (it != ps_cache.end())
					ps_code = it->second;
				else
				{
					uint8_t head[13];
					std::memcpy(head, tex_kind, 8);
					head[8] = projected;
					std::memcpy(head + 9, &tail, sizeof(tail));
					note_shader_key(key_sm1_ps, head, sizeof(head), psh->code.tokens.data(), psh->code.tokens.size() * 4);
					hlsl = sm1_ps_hlsl(psh->code, tex_kind, projected, tail);
					const std::vector<uint8_t> &b = compile(hlsl, "ps_5_0", why);
					if (b.empty())
						log_once(2, "a ps.1.x shader didn't compile: %s\n%s", why.c_str(), hlsl.c_str());
					ps_code = b.empty() ? nullptr : &b;
					ps_cache.emplace(h, ps_code);
				}
			}
			else
			{
				ff_ps_key k;
				for (int st = 0; st < stages; ++st)
				{
					ff_ps_stage &p = k.stages[st];
					const DWORD *t = s.tss[st];
					p.cop = uint8_t(t[D3DTSS_COLOROP]);
					p.ca1 = uint8_t(t[D3DTSS_COLORARG1]);
					p.ca2 = uint8_t(t[D3DTSS_COLORARG2]);
					p.ca0 = uint8_t(t[D3DTSS_COLORARG0]);
					p.aop = uint8_t(t[D3DTSS_ALPHAOP]);
					p.aa1 = uint8_t(t[D3DTSS_ALPHAARG1]);
					p.aa2 = uint8_t(t[D3DTSS_ALPHAARG2]);
					p.aa0 = uint8_t(t[D3DTSS_ALPHAARG0]);
					p.result = uint8_t(t[D3DTSS_RESULTARG]);
					p.tex = tex_kind[st];
					p.projected = (t[D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED) ? 1 : 0;
					p.coords = p.projected ? coords[st] : 0;
				}
				k.tail = tail;
				k.specular = s.rs[D3DRS_SPECULARENABLE] ? 1 : 0;
				h = hash_bytes(&k, sizeof(k), 0xF00D);
				const auto it = ps_cache.find(h);
				if (it != ps_cache.end())
					ps_code = it->second;
				else
				{
					note_shader_key(key_ff_ps, &k, sizeof(k));
					hlsl = ff_ps_hlsl(k);
					const std::vector<uint8_t> &b = compile(hlsl, "ps_5_0", why);
					if (b.empty())
						log_once(2, "a fixed-function pixel shader didn't compile: %s\n%s", why.c_str(), hlsl.c_str());
					ps_code = b.empty() ? nullptr : &b;
					ps_cache.emplace(h, ps_code);
				}
			}
		}
		if (ps_code == nullptr)
			return D3D_OK;

		// ---- Textures and samplers (first: a table that runs out submits the list) ----
		D3D12_CPU_DESCRIPTOR_HANDLE views[8];
		D3D12_SAMPLER_DESC samplers[8] = {};
		for (int st = 0; st < 8; ++st)
		{
			image *img = tex_img[st];
			const D3D12_SRV_DIMENSION dim = tex_kind[st] == 2 ? D3D12_SRV_DIMENSION_TEXTURECUBE : tex_kind[st] == 3 ? D3D12_SRV_DIMENSION_TEXTURE3D : D3D12_SRV_DIMENSION_TEXTURE2D;
			views[st] = g.null_srv(dim);
			D3D12_SAMPLER_DESC &sd = samplers[st];
			sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sd.MaxAnisotropy = 1;
			sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			sd.MaxLOD = D3D12_FLOAT32_MAX;
			if (img == nullptr || st >= stages)
				continue;
			if (img == rt->img)
			{
				log_once(1, "a texture is read while it's the render target: read as empty");
				continue;
			}
			if (img->kind == image_kind::target)
			{
				if (img->resolved != nullptr)
					img->sample_resource();
				else
					g.transition(img->gpu, read_state);
			}
			else if (img->gpu.res != nullptr)
			{
				img->upload();
				g.transition(img->gpu, read_state);
			}
			else
				continue;
			views[st] = img->srv;
			const DWORD *t = s.tss[st];
			DWORD mn = t[D3DTSS_MINFILTER], mg = t[D3DTSS_MAGFILTER];
			const DWORD mp = t[D3DTSS_MIPFILTER];
			// (a stage that isn't point sampled both ways is filtered linear both ways: dgVoodoo's with Phoenix's conf,
			// Filtering 1 and KeepFilterIfPointSampled, seen in the d3dlog replays' interface)
			if ((mn == D3DTEXF_LINEAR || mg == D3DTEXF_LINEAR) && mn != D3DTEXF_ANISOTROPIC && mg != D3DTEXF_ANISOTROPIC)
				mn = mg = D3DTEXF_LINEAR;
			if (mn == D3DTEXF_ANISOTROPIC || mg == D3DTEXF_ANISOTROPIC)
			{
				sd.Filter = D3D12_FILTER_ANISOTROPIC;
				sd.MaxAnisotropy = std::clamp<UINT>(t[D3DTSS_MAXANISOTROPY], 1, 16);
			}
			else
			{
				// (mips are blended whenever they're on: dgVoodoo draws MIPFILTER POINT as LINEAR, pixel for pixel, with
				// Phoenix's dgVoodoo.conf; NONE stays the base level)
				sd.Filter = D3D12_ENCODE_BASIC_FILTER(filter_type(mn), filter_type(mg), mp != D3DTEXF_NONE ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
			}
			sd.AddressU = D3D12_TEXTURE_ADDRESS_MODE(std::clamp<DWORD>(t[D3DTSS_ADDRESSU], 1, 5));
			sd.AddressV = D3D12_TEXTURE_ADDRESS_MODE(std::clamp<DWORD>(t[D3DTSS_ADDRESSV], 1, 5));
			sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE(std::clamp<DWORD>(t[D3DTSS_ADDRESSW], 1, 5));
			colour(sd.BorderColor, D3DCOLOR(t[D3DTSS_BORDERCOLOR]));
			sd.MipLODBias = as_float(t[D3DTSS_MIPMAPLODBIAS]);
			const base_texture *bt = texture_of(s.textures[st]);
			sd.MinLOD = float(std::max<DWORD>(t[D3DTSS_MAXMIPLEVEL], bt != nullptr ? bt->lod : 0));
			if (mp == D3DTEXF_NONE)
				sd.MaxLOD = sd.MinLOD;
		}
		const D3D12_GPU_DESCRIPTOR_HANDLE srv_table = g.srv_table(views, 8);
		const D3D12_GPU_DESCRIPTOR_HANDLE sampler_table = g.sampler_table(samplers);

		// ---- Vertex and index data ----
		D3D12_VERTEX_BUFFER_VIEW vbs[kMaxStreams] = {};
		uint32_t vb_count = 0;
		UINT first_vertex = start, first_index = start, index_count = vcount;
		INT vertex_base = base_vertex;
		D3D12_INDEX_BUFFER_VIEW ibv = {};
		bool draw_indexed = indexed;
		if (up_vertices != nullptr)
		{
			const UINT n = indexed ? min_index + num_vertices : vcount;
			upload_alloc a = g.upload(UINT64(n) * up_stride, 16);
			if (a.cpu == nullptr)
				return D3DERR_OUTOFVIDEOMEMORY;
			std::memcpy(a.cpu, up_vertices, size_t(n) * up_stride);
			vbs[0] = { a.gpu, n * up_stride, up_stride };
			vb_count = 1;
			first_vertex = 0;
			vertex_base = 0;
			if (indexed)
			{
				const UINT isz = up_index_format == D3DFMT_INDEX32 ? 4 : 2;
				upload_alloc ia = g.upload(UINT64(vcount) * isz, 16);
				std::memcpy(ia.cpu, up_indices, size_t(vcount) * isz);
				ibv = { ia.gpu, vcount * isz, isz == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
				first_index = 0;
			}
		}
		else
		{
			for (UINT st = 0; st < kMaxStreams; ++st)
			{
				if ((decl->streams & (1u << st)) == 0)
					continue;
				vertex_buffer *vb = static_cast<vertex_buffer *>(s.streams[st]);
				if (vb == nullptr)
				{
					log_once(1, "a draw reads stream %u with nothing set: skipped", st);
					return D3D_OK;
				}
				const D3D12_GPU_VIRTUAL_ADDRESS a = vb->buf.address(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
				vbs[st] = { a, vb->buf.size, s.strides[st] };
				vb_count = st + 1;
			}
			if (indexed)
			{
				index_buffer *ib = static_cast<index_buffer *>(s.indices);
				if (ib == nullptr)
				{
					log_once(1, "an indexed draw with no index buffer: skipped");
					return D3D_OK;
				}
				const D3D12_GPU_VIRTUAL_ADDRESS a = ib->buf.address(D3D12_RESOURCE_STATE_INDEX_BUFFER);
				ibv = { a, ib->buf.size, ib->format == D3DFMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT };
			}
		}
		// Fans: made into lists through indices of our own
		if (type == D3DPT_TRIANGLEFAN)
		{
			std::vector<uint32_t> idx(size_t(count) * 3);
			if (draw_indexed)
			{
				const bool i32 = ibv.Format == DXGI_FORMAT_R32_UINT;
				const uint8_t *src = up_indices != nullptr ? static_cast<const uint8_t *>(up_indices) : static_cast<index_buffer *>(s.indices)->buf.bytes.data() + size_t(first_index) * (i32 ? 4 : 2);
				auto at = [&](UINT i) -> uint32_t { if (i32) { uint32_t v; std::memcpy(&v, src + 4 * i, 4); return v; } uint16_t v; std::memcpy(&v, src + 2 * i, 2); return v; };
				for (UINT p = 0; p < count; ++p)
				{
					idx[p * 3] = at(0);
					idx[p * 3 + 1] = at(p + 1);
					idx[p * 3 + 2] = at(p + 2);
				}
			}
			else
				for (UINT p = 0; p < count; ++p)
				{
					idx[p * 3] = first_vertex;
					idx[p * 3 + 1] = first_vertex + p + 1;
					idx[p * 3 + 2] = first_vertex + p + 2;
				}
			upload_alloc ia = g.upload(UINT64(idx.size()) * 4, 16);
			std::memcpy(ia.cpu, idx.data(), idx.size() * 4);
			ibv = { ia.gpu, UINT(idx.size() * 4), DXGI_FORMAT_R32_UINT };
			index_count = UINT(idx.size());
			first_index = 0;
			if (!draw_indexed)
				vertex_base = 0;
			draw_indexed = true;
		}

		// ---- Constants ----
		const int kind = programmable ? 2 : 1;
		const bool fresh = vs_cb == 0 || vs_cb_frame != g.frame_number || vs_cb_kind != kind;
		if (programmable)
		{
			if (fresh || (dirty & d_vsc))
			{
				upload_alloc a = g.upload(sizeof(sm1_vs_constants), 256);
				sm1_vs_constants *c = reinterpret_cast<sm1_vs_constants *>(a.cpu);
				std::memcpy(c->c, s.vsc, sizeof(c->c));
				c->zr[0] = s.viewport.MinZ;
				c->zr[1] = s.viewport.MaxZ - s.viewport.MinZ;
				c->zr[2] = s.proj.m[2][3] != 0.0f || s.proj.m[3][3] == 0.0f ? 1.0f : 0.0f;
				c->zr[3] = 0.0f;
				std::memcpy(c->clipp, s.clip, sizeof(c->clipp));
				vs_cb = a.gpu;
				vs_cb_frame = g.frame_number;
				vs_cb_kind = kind;
				dirty &= ~d_vsc;
			}
		}
		else if (fresh || (dirty & d_ffvs))
		{
			upload_alloc a = g.upload(sizeof(ff_vs_constants), 256);
			fill_ff_constants(s, s.viewport, *reinterpret_cast<ff_vs_constants *>(a.cpu));
			vs_cb = a.gpu;
			vs_cb_frame = g.frame_number;
			vs_cb_kind = kind;
			dirty &= ~d_ffvs;
		}
		if ((dirty & d_psc) || ps_cb_frame != g.frame_number || ps_cb == 0)
		{
			upload_alloc a = g.upload(sizeof(ps_constants), 256);
			fill_ps_constants(s, *reinterpret_cast<ps_constants *>(a.cpu));
			ps_cb = a.gpu;
			ps_cb_frame = g.frame_number;
			dirty &= ~d_psc;
		}

		// ---- The pipeline ----
		const bool has_ds = ds != nullptr;
		const D3DFORMAT rtf = rt->img->format;
		pso_key pk;
		pk.vs = vs_code;
		pk.ps = ps_code;
		pk.layout = decl->hash;
		pk.topology = uint8_t(topology_type(type));
		pk.blend = s.rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0;
		if (pk.blend)
		{
			pk.src = uint8_t(s.rs[D3DRS_SRCBLEND]);
			pk.dst = uint8_t(s.rs[D3DRS_DESTBLEND]);
			pk.op = uint8_t(s.rs[D3DRS_BLENDOP]);
		}
		pk.write_mask = uint8_t(s.rs[D3DRS_COLORWRITEENABLE] & 15);
		pk.x_target = rtf == D3DFMT_X8R8G8B8 || rtf == D3DFMT_X1R5G5B5 || rtf == D3DFMT_R5G6B5 || rtf == D3DFMT_X4R4G4B4 ? 1 : 0;
		pk.has_ds = has_ds ? 1 : 0;
		const bool z_on = has_ds && s.rs[D3DRS_ZENABLE] != D3DZB_FALSE;
		const bool stencil_on = has_ds && info(ds->img->format).stencil && s.rs[D3DRS_STENCILENABLE];
		if (has_ds)
		{
			pk.z = z_on ? 1 : 0;
			pk.z_write = z_on && s.rs[D3DRS_ZWRITEENABLE] ? 1 : 0;
			pk.z_func = uint8_t(z_on ? s.rs[D3DRS_ZFUNC] : 0);
			pk.ds_format = uint8_t(ds->img->gpu_format);
			pk.samples = uint8_t(std::max<UINT>(1, UINT(ds->img->multisample)));
			if (stencil_on)
			{
				pk.stencil = 1;
				pk.s_func = uint8_t(s.rs[D3DRS_STENCILFUNC]);
				pk.s_fail = uint8_t(s.rs[D3DRS_STENCILFAIL]);
				pk.s_zfail = uint8_t(s.rs[D3DRS_STENCILZFAIL]);
				pk.s_pass = uint8_t(s.rs[D3DRS_STENCILPASS]);
				pk.s_read = uint8_t(s.rs[D3DRS_STENCILMASK]);
				// (no write mask when every operation keeps: Direct3D 12 counts a mask as writing, the plane then in DEPTH_WRITE)
				const bool keeps = pk.s_fail == D3DSTENCILOP_KEEP && pk.s_zfail == D3DSTENCILOP_KEEP && pk.s_pass == D3DSTENCILOP_KEEP;
				pk.s_write = keeps ? 0 : uint8_t(s.rs[D3DRS_STENCILWRITEMASK]);
			}
		}
		pk.samples = uint8_t(std::max<UINT>(1, UINT(rt->img->multisample)));
		pk.fill = uint8_t(s.rs[D3DRS_FILLMODE]);
		pk.cull = uint8_t(s.rs[D3DRS_CULLMODE]);
		pk.zbias = INT(s.rs[D3DRS_ZBIAS]);
		const uint64_t ph = hash_bytes(&pk, sizeof(pk));
		pso_entry &pe = psos[ph];
		if (pe.pso == nullptr)
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
			pd.pRootSignature = g.root;
			pd.VS = { vs_code->data(), vs_code->size() };
			pd.PS = { ps_code->data(), ps_code->size() };
			D3D12_RENDER_TARGET_BLEND_DESC &b = pd.BlendState.RenderTarget[0];
			b.BlendEnable = pk.blend;
			const DWORD sb = s.rs[D3DRS_SRCBLEND], db = s.rs[D3DRS_DESTBLEND];
			const bool both = sb == D3DBLEND_BOTHSRCALPHA || sb == D3DBLEND_BOTHINVSRCALPHA;
			const DWORD src = both ? (sb == D3DBLEND_BOTHSRCALPHA ? D3DBLEND_SRCALPHA : D3DBLEND_INVSRCALPHA) : sb;
			const DWORD dst = both ? (sb == D3DBLEND_BOTHSRCALPHA ? D3DBLEND_INVSRCALPHA : D3DBLEND_SRCALPHA) : db;
			b.SrcBlend = blend_of(src, false, pk.x_target != 0);
			b.DestBlend = blend_of(dst, false, pk.x_target != 0);
			b.SrcBlendAlpha = blend_of(src, true, pk.x_target != 0);
			b.DestBlendAlpha = blend_of(dst, true, pk.x_target != 0);
			b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP(std::clamp<DWORD>(s.rs[D3DRS_BLENDOP], 1, 5));
			b.LogicOp = D3D12_LOGIC_OP_NOOP;
			b.RenderTargetWriteMask = pk.write_mask;
			pd.SampleMask = s.rs[D3DRS_MULTISAMPLEMASK];
			pd.RasterizerState.FillMode = pk.fill == D3DFILL_SOLID ? D3D12_FILL_MODE_SOLID : D3D12_FILL_MODE_WIREFRAME;
			pd.RasterizerState.CullMode = pk.cull == D3DCULL_CW ? D3D12_CULL_MODE_FRONT : pk.cull == D3DCULL_CCW ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
			pd.RasterizerState.FrontCounterClockwise = FALSE;
			// (dgVoodoo 2.87's depth bias for ZBIAS, measured: notes-replace-slots.md)
			pd.RasterizerState.DepthBias = INT(-2.5f * float(pk.zbias));
			pd.RasterizerState.SlopeScaledDepthBias = -0.5f * float(pk.zbias);
			pd.RasterizerState.DepthClipEnable = TRUE;
			pd.RasterizerState.MultisampleEnable = pk.samples > 1 && s.rs[D3DRS_MULTISAMPLEANTIALIAS];
			D3D12_DEPTH_STENCIL_DESC &d = pd.DepthStencilState;
			d.DepthEnable = pk.z;
			d.DepthWriteMask = pk.z_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
			d.DepthFunc = pk.z ? D3D12_COMPARISON_FUNC(std::clamp<int>(pk.z_func, 1, 8)) : D3D12_COMPARISON_FUNC_ALWAYS;
			d.StencilEnable = pk.stencil;
			d.StencilReadMask = pk.stencil ? pk.s_read : 0xFF;
			d.StencilWriteMask = pk.stencil ? pk.s_write : 0xFF;
			d.FrontFace.StencilFunc = pk.stencil ? D3D12_COMPARISON_FUNC(std::clamp<int>(pk.s_func, 1, 8)) : D3D12_COMPARISON_FUNC_ALWAYS;
			d.FrontFace.StencilFailOp = pk.stencil ? D3D12_STENCIL_OP(std::clamp<int>(pk.s_fail, 1, 8)) : D3D12_STENCIL_OP_KEEP;
			d.FrontFace.StencilDepthFailOp = pk.stencil ? D3D12_STENCIL_OP(std::clamp<int>(pk.s_zfail, 1, 8)) : D3D12_STENCIL_OP_KEEP;
			d.FrontFace.StencilPassOp = pk.stencil ? D3D12_STENCIL_OP(std::clamp<int>(pk.s_pass, 1, 8)) : D3D12_STENCIL_OP_KEEP;
			d.BackFace = d.FrontFace;
			pd.InputLayout = { decl->layout.data(), UINT(decl->layout.size()) };
			pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
			pd.PrimitiveTopologyType = topology_type(type);
			pd.NumRenderTargets = 1;
			pd.RTVFormats[0] = kTargetView;
			pd.DSVFormat = has_ds ? ds->img->gpu_format : DXGI_FORMAT_UNKNOWN;
			pd.SampleDesc.Count = pk.samples;
			LARGE_INTEGER made_at;
			QueryPerformanceCounter(&made_at);
			const HRESULT hr = g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pe.pso));
			lf.pso_ms += ms_since(made_at);
			lf.psos++;
			if (FAILED(hr))
			{
				log_once(2, "a pipeline couldn't be made (0x%08lX)", static_cast<unsigned long>(hr));
				pe.pso = nullptr;
				psos.erase(ph);
				return D3D_OK;
			}
			stats.pipelines++;
		}

		// ---- States: the depth's planes by what's written; then everything bound ----
		bind_targets();
		if (has_ds)
		{
			image *z = ds->img;
			const bool writes_stencil = stencil_on && pk.s_write != 0 && (pk.s_fail != D3DSTENCILOP_KEEP || pk.s_zfail != D3DSTENCILOP_KEEP || pk.s_pass != D3DSTENCILOP_KEEP);
			g.transition_plane(z->gpu, 0, pk.z_write ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_DEPTH_READ);
			g.transition_plane(z->gpu, 1, writes_stencil ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_DEPTH_READ);
		}
		g.flush_barriers();
		ID3D12GraphicsCommandList *l = g.list();
		{
			const D3DVIEWPORT8 &v = s.viewport;
			const D3D12_VIEWPORT vp = { float(v.X) + 0.5f, float(v.Y) + 0.5f, float(v.Width), float(v.Height), 0.0f, 1.0f };
			const D3D12_RECT sc = { LONG(v.X), LONG(v.Y), LONG(std::min<UINT>(v.X + v.Width, rt->width())), LONG(std::min<UINT>(v.Y + v.Height, rt->height())) };
			if (!bound.vp || std::memcmp(&vp, &bound.viewport, sizeof(vp)) != 0)
			{
				l->RSSetViewports(1, &vp);
				bound.viewport = vp;
			}
			if (!bound.vp || std::memcmp(&sc, &bound.scissor, sizeof(sc)) != 0)
			{
				l->RSSetScissorRects(1, &sc);
				bound.scissor = sc;
			}
			bound.vp = true;
		}
		if (bound.pso != pe.pso)
		{
			l->SetPipelineState(pe.pso);
			bound.pso = pe.pso;
		}
		const D3D_PRIMITIVE_TOPOLOGY topo = type == D3DPT_TRIANGLEFAN ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : topology_of(type);
		if (bound.topology != topo)
		{
			l->IASetPrimitiveTopology(topo);
			bound.topology = topo;
		}
		if (bound.vs_cb != vs_cb)
		{
			l->SetGraphicsRootConstantBufferView(0, vs_cb);
			bound.vs_cb = vs_cb;
		}
		if (bound.ps_cb != ps_cb)
		{
			l->SetGraphicsRootConstantBufferView(1, ps_cb);
			bound.ps_cb = ps_cb;
		}
		if (bound.srvs.ptr != srv_table.ptr)
		{
			l->SetGraphicsRootDescriptorTable(2, srv_table);
			bound.srvs = srv_table;
		}
		if (bound.samplers.ptr != sampler_table.ptr)
		{
			l->SetGraphicsRootDescriptorTable(3, sampler_table);
			bound.samplers = sampler_table;
		}
		if (vb_count != bound.vb_count || std::memcmp(vbs, bound.vbs, sizeof(D3D12_VERTEX_BUFFER_VIEW) * vb_count) != 0)
		{
			l->IASetVertexBuffers(0, vb_count, vbs);
			std::memcpy(bound.vbs, vbs, sizeof(vbs));
			bound.vb_count = vb_count;
		}
		if (draw_indexed && std::memcmp(&ibv, &bound.ib, sizeof(ibv)) != 0)
		{
			l->IASetIndexBuffer(&ibv);
			bound.ib = ibv;
		}
		if (stencil_on && bound.stencil_ref != (s.rs[D3DRS_STENCILREF] & 0xFF))
		{
			l->OMSetStencilRef(s.rs[D3DRS_STENCILREF] & 0xFF);
			bound.stencil_ref = s.rs[D3DRS_STENCILREF] & 0xFF;
		}
		if (draw_indexed)
			l->DrawIndexedInstanced(index_count, 1, first_index, vertex_base, 0);
		else
			l->DrawInstanced(vcount, 1, first_vertex, 0);
		g.draws_in_list++;
		stats.draws++;
		return D3D_OK;
	}

	void device::present_frame(HWND override_window, const RECT *, const RECT *)
	{
		chain->present(override_window);
		stats.presents++;
		async_index.clear();
		if (!async_reads.empty())
			poll_async(false);
		// (answers not asked for in two seconds or so let go)
		if (stats.presents % 120 == 0)
			for (auto it = async_results.begin(); it != async_results.end();)
				it = it->second.frame + 240 < g.frame_number ? async_results.erase(it) : std::next(it);
		if (stats.presents == 1 || stats.presents == 100 || stats.presents % 3600 == 0)
		{
			const compile_stats cs = shader_stats();
			log(0, "frame %llu: %llu draws, %llu clears, %llu readbacks (%llu answered from an earlier frame, %llu not yet), %llu pipelines, %u shaders compiled in play (%.0f ms), %u ahead, %u from the cache folder, %u failed",
				static_cast<unsigned long long>(stats.presents), static_cast<unsigned long long>(stats.draws), static_cast<unsigned long long>(stats.clears),
				static_cast<unsigned long long>(stats.readbacks), static_cast<unsigned long long>(stats.async_answered), static_cast<unsigned long long>(stats.async_unanswered), static_cast<unsigned long long>(stats.pipelines), cs.compiled, cs.ms, cs.ahead, cs.from_disk, cs.failed);
		}
	}
}
