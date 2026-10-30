#pragma once
// d3d8_frame.cpp's replay of a d3dlog capture (C:\Games\PhoenixXI\d3dlog\*.txt, the game's own Direct3D 8 calls of
// one frame, 2026-10-05): every call in its order, through the d3d8.dll under test.
//
// From the capture as it was: the render targets and depth buffers (their sizes halved: the 5120x2880 world target
// is 2560x1440 here, the back buffer 1280x720), viewports, clears, every render and texture stage state it logs
// (state blocks' contents aren't in the log: ApplyStateBlock is left out on both sides alike), lights, materials,
// the projection (section 5's matrices), light enables, textures by their size, format and levels, which render
// target a draw samples (the one last drawn into that isn't bound: the resolve reads A, the ground shadows the
// silhouette target, the visibility tests' downsample their target), the vertex formats, and each draw's kind,
// primitive type and counts. Its visibility tests are read back as the game reads them (CopyRects + LockRect).
// Stand-ins: the geometry (made up per draw from its number: in front of the camera with the frame's projection, or
// over the viewport for pretransformed vertices), the textures' texels, vertex shaders (fixed function in their place,
// pixel shader 0: the zone's own shaders are compared with real data by d3d8_compare look), the world and view
// matrices (identity). Both d3d8.dlls get the very same calls.
//
// Writes <out>-world.bmp (the world target at the frame's end) and <out>-screen.bmp (the back buffer before Present).

#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace replay
{
	struct call
	{
		int n = 0;
		std::string op, args;
		std::vector<uint32_t> chain;
	};

	inline uint32_t hash32(uint32_t x)
	{
		x ^= x >> 16;
		x *= 0x7FEB352Du;
		x ^= x >> 15;
		x *= 0x846CA68Bu;
		x ^= x >> 16;
		return x;
	}
	inline UINT align8(UINT v) { return (v + 7) & ~7u; }
	inline float rnd(uint32_t &s) { s = hash32(s + 0x9E3779B9u); return float(s & 0xFFFFFF) / float(0xFFFFFF); }

	inline const std::map<std::string, DWORD> &names()
	{
		static const std::map<std::string, DWORD> m = {
			// render states
			{ "ZENABLE", 7 }, { "FILLMODE", 8 }, { "SHADEMODE", 9 }, { "ZWRITEENABLE", 14 }, { "ALPHATESTENABLE", 15 }, { "LASTPIXEL", 16 },
			{ "SRCBLEND", 19 }, { "DESTBLEND", 20 }, { "CULLMODE", 22 }, { "ZFUNC", 23 }, { "ALPHAREF", 24 }, { "ALPHAFUNC", 25 },
			{ "DITHERENABLE", 26 }, { "ALPHABLENDENABLE", 27 }, { "FOGENABLE", 28 }, { "SPECULARENABLE", 29 }, { "FOGCOLOR", 34 },
			{ "FOGTABLEMODE", 35 }, { "FOGSTART", 36 }, { "FOGEND", 37 }, { "FOGDENSITY", 38 }, { "ZBIAS", 47 }, { "RANGEFOGENABLE", 48 },
			{ "STENCILENABLE", 52 }, { "STENCILFAIL", 53 }, { "STENCILZFAIL", 54 }, { "STENCILPASS", 55 }, { "STENCILFUNC", 56 },
			{ "STENCILREF", 57 }, { "STENCILMASK", 58 }, { "STENCILWRITEMASK", 59 }, { "TEXTUREFACTOR", 60 }, { "CLIPPING", 136 },
			{ "LIGHTING", 137 }, { "AMBIENT", 139 }, { "FOGVERTEXMODE", 140 }, { "COLORVERTEX", 141 }, { "LOCALVIEWER", 142 },
			{ "NORMALIZENORMALS", 143 }, { "DIFFUSEMATERIALSOURCE", 145 }, { "SPECULARMATERIALSOURCE", 146 }, { "AMBIENTMATERIALSOURCE", 147 },
			{ "EMISSIVEMATERIALSOURCE", 148 }, { "VERTEXBLEND", 151 }, { "CLIPPLANEENABLE", 152 }, { "COLORWRITEENABLE", 168 }, { "BLENDOP", 171 },
		};
		return m;
	}
	inline const std::map<std::string, DWORD> &stage_names()
	{
		static const std::map<std::string, DWORD> m = {
			{ "COLOROP", 1 }, { "COLORARG1", 2 }, { "COLORARG2", 3 }, { "ALPHAOP", 4 }, { "ALPHAARG1", 5 }, { "ALPHAARG2", 6 },
			{ "BUMPENVMAT00", 7 }, { "BUMPENVMAT01", 8 }, { "BUMPENVMAT10", 9 }, { "BUMPENVMAT11", 10 }, { "TEXCOORDINDEX", 11 },
			{ "ADDRESSU", 13 }, { "ADDRESSV", 14 }, { "BORDERCOLOR", 15 }, { "MAGFILTER", 16 }, { "MINFILTER", 17 }, { "MIPFILTER", 18 },
			{ "MIPMAPLODBIAS", 19 }, { "MAXMIPLEVEL", 20 }, { "MAXANISOTROPY", 21 }, { "BUMPENVLSCALE", 22 }, { "BUMPENVLOFFSET", 23 },
			{ "TEXTURETRANSFORMFLAGS", 24 }, { "ADDRESSW", 25 }, { "COLORARG0", 26 }, { "ALPHAARG0", 27 }, { "RESULTARG", 28 },
		};
		return m;
	}
	// A value as logged: an enum's name (by the state it's for), TRUE/FALSE, hex, a float (the float states), a number
	inline bool value_of(const std::string &state, const std::string &v, DWORD &out)
	{
		static const std::map<std::string, DWORD> plain = {
			{ "TRUE", 1 }, { "FALSE", 0 }, { "NEVER", 1 }, { "LESS", 2 }, { "EQUAL", 3 }, { "LESSEQUAL", 4 }, { "GREATER", 5 }, { "NOTEQUAL", 6 },
			{ "GREATEREQUAL", 7 }, { "ALWAYS", 8 }, { "ZERO", 1 }, { "ONE", 2 }, { "SRCCOLOR", 3 }, { "INVSRCCOLOR", 4 }, { "SRCALPHA", 5 },
			{ "INVSRCALPHA", 6 }, { "DESTALPHA", 7 }, { "INVDESTALPHA", 8 }, { "DESTCOLOR", 9 }, { "INVDESTCOLOR", 10 }, { "SRCALPHASAT", 11 },
			{ "ADD", 1 }, { "SUBTRACT", 2 }, { "REVSUBTRACT", 3 }, { "MIN", 4 }, { "MAX", 5 }, { "CW", 2 }, { "CCW", 3 },
			{ "FLAT", 1 }, { "GOURAUD", 2 }, { "PHONG", 3 }, { "POINT", 1 }, { "WIREFRAME", 2 }, { "SOLID", 3 }, { "EXP", 1 }, { "EXP2", 2 }, { "LINEAR", 3 },
			{ "MATERIAL", 0 }, { "COLOR1", 1 }, { "COLOR2", 2 }, { "KEEP", 1 }, { "REPLACE", 3 }, { "INCRSAT", 4 }, { "DECRSAT", 5 }, { "INVERT", 6 },
			{ "INCR", 7 }, { "DECR", 8 },
		};
		static const std::map<std::string, DWORD> ops = {
			{ "DISABLE", 1 }, { "SELECTARG1", 2 }, { "SELECTARG2", 3 }, { "MODULATE", 4 }, { "MODULATE2X", 5 }, { "MODULATE4X", 6 }, { "ADD", 7 },
			{ "ADDSIGNED", 8 }, { "ADDSIGNED2X", 9 }, { "SUBTRACT", 10 }, { "ADDSMOOTH", 11 }, { "BLENDDIFFUSEALPHA", 12 }, { "BLENDTEXTUREALPHA", 13 },
			{ "BLENDFACTORALPHA", 14 }, { "BLENDTEXTUREALPHAPM", 15 }, { "BLENDCURRENTALPHA", 16 }, { "PREMODULATE", 17 }, { "MODULATEALPHA_ADDCOLOR", 18 },
			{ "MODULATECOLOR_ADDALPHA", 19 }, { "MODULATEINVALPHA_ADDCOLOR", 20 }, { "MODULATEINVCOLOR_ADDALPHA", 21 }, { "BUMPENVMAP", 22 },
			{ "BUMPENVMAPLUMINANCE", 23 }, { "DOTPRODUCT3", 24 }, { "MULTIPLYADD", 25 }, { "LERP", 26 },
		};
		static const std::map<std::string, DWORD> args = { { "DIFFUSE", 0 }, { "CURRENT", 1 }, { "TEXTURE", 2 }, { "TFACTOR", 3 }, { "SPECULAR", 4 }, { "TEMP", 5 },
			{ "COMPLEMENT", 0x10 }, { "ALPHAREPLICATE", 0x20 } };
		static const std::map<std::string, DWORD> tex = { { "NONE", 0 }, { "POINT", 1 }, { "LINEAR", 2 }, { "ANISOTROPIC", 3 }, { "WRAP", 1 }, { "MIRROR", 2 },
			{ "CLAMP", 3 }, { "BORDER", 4 }, { "MIRRORONCE", 5 }, { "DISABLE", 0 }, { "COUNT1", 1 }, { "COUNT2", 2 }, { "COUNT3", 3 }, { "COUNT4", 4 },
			{ "PROJECTED", 256 }, { "CAMERASPACENORMAL", 0x10000 }, { "CAMERASPACEPOSITION", 0x20000 }, { "CAMERASPACEREFLECTIONVECTOR", 0x30000 } };
		const bool is_op = state == "COLOROP" || state == "ALPHAOP";
		const bool is_arg = state.find("ARG") != std::string::npos || state == "RESULTARG";
		const bool is_tex = state.find("FILTER") != std::string::npos || state.find("ADDRESS") != std::string::npos || state == "TEXTURETRANSFORMFLAGS" || state == "TEXCOORDINDEX";
		if (v == "NONE" && !is_tex)
		{
			// (CULL NONE is 1; the fog modes' NONE 0)
			out = state == "CULLMODE" ? 1 : 0;
			return true;
		}
		if (state == "FOGSTART" || state == "FOGEND" || state == "FOGDENSITY" || state == "MIPMAPLODBIAS")
		{
			const float f = std::strtof(v.c_str(), nullptr);
			std::memcpy(&out, &f, 4);
			return true;
		}
		if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
		{
			out = DWORD(std::strtoul(v.c_str(), nullptr, 16));
			return true;
		}
		if (!v.empty() && (std::isdigit(static_cast<unsigned char>(v[0])) || v[0] == '-') && v.find('|') == std::string::npos)
		{
			out = DWORD(std::strtol(v.c_str(), nullptr, 10));
			return true;
		}
		// (names, possibly joined by |)
		out = 0;
		std::stringstream ss(v);
		std::string part;
		while (std::getline(ss, part, '|'))
		{
			const std::map<std::string, DWORD> &m = is_op ? ops : is_arg ? args : is_tex ? tex : plain;
			const auto it = m.find(part);
			if (it != m.end())
				out |= it->second;
			else if (!part.empty() && std::isdigit(static_cast<unsigned char>(part[0])))
				out |= DWORD(std::strtoul(part.c_str(), nullptr, 10));
			else
				return false;
		}
		return true;
	}

	inline D3DFORMAT format_named(const std::string &s)
	{
		static const std::map<std::string, D3DFORMAT> m = { { "A8R8G8B8", D3DFMT_A8R8G8B8 }, { "X8R8G8B8", D3DFMT_X8R8G8B8 }, { "R5G6B5", D3DFMT_R5G6B5 },
			{ "A1R5G5B5", D3DFMT_A1R5G5B5 }, { "A4R4G4B4", D3DFMT_A4R4G4B4 }, { "DXT1", D3DFMT_DXT1 }, { "DXT3", D3DFMT_DXT3 }, { "DXT5", D3DFMT_DXT5 },
			{ "A8", D3DFMT_A8 }, { "L8", D3DFMT_L8 }, { "P8", D3DFMT_P8 }, { "D24S8", D3DFMT_D24S8 }, { "D16", D3DFMT_D16 } };
		const auto it = m.find(s);
		return it != m.end() ? it->second : D3DFMT_A8R8G8B8;
	}

	inline DWORD fvf_named(const std::string &s)
	{
		static const std::map<std::string, DWORD> m = { { "XYZ", D3DFVF_XYZ }, { "XYZRHW", D3DFVF_XYZRHW }, { "NORMAL", D3DFVF_NORMAL },
			{ "DIFFUSE", D3DFVF_DIFFUSE }, { "SPECULAR", D3DFVF_SPECULAR }, { "TEX1", D3DFVF_TEX1 }, { "TEX2", D3DFVF_TEX2 } };
		if (s.rfind("shader:", 0) == 0)
			return D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
		DWORD v = 0;
		std::stringstream ss(s);
		std::string part;
		while (std::getline(ss, part, '|'))
		{
			const auto it = m.find(part);
			if (it != m.end())
				v |= it->second;
		}
		return v != 0 ? v : D3DFVF_XYZ | D3DFVF_DIFFUSE;
	}

	inline UINT fvf_stride(DWORD f)
	{
		UINT s = (f & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW ? 16 : 12;
		if (f & D3DFVF_NORMAL) s += 12;
		if (f & D3DFVF_DIFFUSE) s += 4;
		if (f & D3DFVF_SPECULAR) s += 4;
		s += 8 * ((f & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);
		return s;
	}

	struct state
	{
		std::vector<call> calls;
		std::map<int, D3DMATRIX> matrices;          // section 5: by call number
		std::map<std::string, std::pair<UINT, UINT>> sizes; // surface id -> size (from SetRenderTarget lines)
		std::map<std::string, D3DFORMAT> formats;
		std::map<std::string, IDirect3DTexture8 *> target_tex;
		std::map<std::string, IDirect3DSurface8 *> surfaces;
		std::map<std::string, IDirect3DTexture8 *> textures; // by "WxH FORMAT Ln"
		std::string current_rt, last_drawn, world;
		std::vector<std::string> drawn_order;        // targets in the order they were last drawn into
		DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
		IDirect3DVertexBuffer8 *vb = nullptr;
		IDirect3DIndexBuffer8 *ib = nullptr;
		UINT vb_at = 0, ib_at = 0;
		IDirect3DSurface8 *sys16 = nullptr;
		uint32_t readbacks = 0, skipped = 0, draws = 0;
		double readback_ms = 0, present_ms = 0; // this frame's time in the visibility tests' CopyRects + LockRect, and in Present
		std::map<int, std::string> pending_tex;
		std::set<std::string> unknown; // stage -> sampled render target (resolved at the draw)
	};

	inline std::vector<std::string> split(const std::string &s)
	{
		std::vector<std::string> v;
		std::stringstream ss(s);
		std::string w;
		while (ss >> w)
			v.push_back(w);
		return v;
	}

	inline bool parse(const char *path, state &st)
	{
		FILE *f = std::fopen(path, "rb");
		if (f == nullptr)
			return false;
		char line[4096];
		int section = 0, matrix_call = -1, matrix_row = 0;
		D3DMATRIX m = {};
		while (std::fgets(line, sizeof(line), f) != nullptr)
		{
			std::string s = line;
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
				s.pop_back();
			if (s.rfind("==== ", 0) == 0)
			{
				section = std::atoi(s.c_str() + 5);
				continue;
			}
			if (section == 3 && s.find("SetRenderTarget rt ") != std::string::npos)
			{
				// "SetRenderTarget rt ID WxH FMT ...  z ID WxH FMT ..."
				const std::vector<std::string> w = split(s.substr(s.find("SetRenderTarget rt ") + 19));
				if (w.size() >= 3)
				{
					const UINT x = UINT(std::atoi(w[1].c_str())), y = UINT(std::atoi(w[1].c_str() + w[1].find('x') + 1));
					st.sizes[w[0]] = { x, y };
					st.formats[w[0]] = format_named(w[2]);
				}
				const size_t z = s.find(" z ");
				if (z != std::string::npos)
				{
					const std::vector<std::string> u = split(s.substr(z + 3));
					if (u.size() >= 3 && u[0] != "null")
					{
						st.sizes[u[0]] = { UINT(std::atoi(u[1].c_str())), UINT(std::atoi(u[1].c_str() + u[1].find('x') + 1)) };
						st.formats[u[0]] = format_named(u[2]);
					}
				}
			}
			if (section == 5)
			{
				if (s.find("SetTransform") != std::string::npos)
				{
					matrix_call = std::atoi(s.c_str());
					matrix_row = 0;
					continue;
				}
				if (matrix_call >= 0 && s.find('[') != std::string::npos && matrix_row < 4)
				{
					const std::vector<std::string> w = split(s.substr(s.find('[') + 1));
					for (int c = 0; c < 4 && c < int(w.size()); ++c)
						m.m[matrix_row][c] = std::strtof(w[size_t(c)].c_str(), nullptr);
					if (++matrix_row == 4)
					{
						st.matrices[matrix_call] = m;
						matrix_call = -1;
					}
				}
				continue;
			}
			if (section == 7)
			{
				const std::vector<std::string> w = split(s);
				if (w.size() < 2 || !std::isdigit(static_cast<unsigned char>(w[0][0])))
					continue;
				call c;
				c.n = std::atoi(w[0].c_str());
				c.op = w[1];
				const size_t b = s.rfind('[');
				const size_t a = s.find(w[1]) + w[1].size();
				c.args = b != std::string::npos && b > a ? s.substr(a, b - a) : s.substr(a);
				if (b != std::string::npos)
					for (const std::string &x : split(s.substr(b + 1)))
						if (x.rfind("0x", 0) == 0)
							c.chain.push_back(uint32_t(std::strtoul(x.c_str(), nullptr, 16)));
				st.calls.push_back(c);
			}
		}
		std::fclose(f);
		return !st.calls.empty();
	}

	inline IDirect3DSurface8 *surface_of(state &st, const std::string &id, bool is_depth)
	{
		const auto it = st.surfaces.find(id);
		if (it != st.surfaces.end())
			return it->second;
		const auto sz = st.sizes.find(id);
		UINT w = sz != st.sizes.end() ? sz->second.first / 2 : 64, h = sz != st.sizes.end() ? sz->second.second / 2 : 64;
		w = std::max<UINT>(w, 1);
		h = std::max<UINT>(h, 1);
		IDirect3DSurface8 *s = nullptr;
		if (is_depth)
			dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, &s);
		else if (w == 1280 && h == 720)
			dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &s); // (the capture's 2560x1440 back buffer)
		else
		{
			IDirect3DTexture8 *t = nullptr;
			dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t);
			if (t != nullptr)
			{
				t->GetSurfaceLevel(0, &s);
				st.target_tex[id] = t;
			}
		}
		st.surfaces[id] = s;
		return s;
	}

	inline IDirect3DTexture8 *texture_named(state &st, const std::string &desc)
	{
		const auto it = st.textures.find(desc);
		if (it != st.textures.end())
			return it->second;
		// "WxH FORMAT Ln"
		const std::vector<std::string> w = split(desc);
		UINT tw = 16, th = 16, levels = 1;
		D3DFORMAT f = D3DFMT_A8R8G8B8;
		if (w.size() >= 2)
		{
			tw = std::clamp<UINT>(UINT(std::atoi(w[0].c_str())), 4, 256);
			th = std::clamp<UINT>(UINT(std::atoi(w[0].c_str() + w[0].find('x') + 1)), 4, 256);
			f = format_named(w[1]);
		}
		if (w.size() >= 3 && w[2][0] == 'L')
			levels = UINT(std::atoi(w[2].c_str() + 1));
		IDirect3DTexture8 *t = make_texture(f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5 || f == D3DFMT_A8R8G8B8 || f == D3DFMT_R5G6B5 || f == D3DFMT_A4R4G4B4 ||
			f == D3DFMT_A1R5G5B5 || f == D3DFMT_L8 || f == D3DFMT_A8 ? f : D3DFMT_A8R8G8B8, std::max(tw, th), levels, UINT(st.textures.size()));
		st.textures[desc] = t;
		return t;
	}

	// One made-up draw's vertices in the current format
	inline std::vector<uint8_t> vertices(state &st, UINT count, uint32_t seed)
	{
		const DWORD f = st.fvf;
		const UINT stride = fvf_stride(f);
		std::vector<uint8_t> v(size_t(count) * stride);
		D3DVIEWPORT8 vp;
		dev->GetViewport(&vp);
		uint32_t r = seed * 7919u + 17u;
		// (a small patch: consecutive vertices near each other, so strips and lists make triangles of some size, and the frame's
		// fill stays near the game's: GPU-bound frames would hide what the calling thread does)
		float cx = rnd(r) * 2 - 1, cy = rnd(r) * 2 - 1, cz = 3.0f + rnd(r) * 40.0f;
		for (UINT i = 0; i < count; ++i)
		{
			float *p = reinterpret_cast<float *>(v.data() + size_t(i) * stride);
			if ((f & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW)
			{
				p[0] = float(vp.X) + (cx * 0.5f + 0.5f) * float(vp.Width) + (rnd(r) - 0.5f) * 0.04f * float(vp.Width);
				p[1] = float(vp.Y) + (cy * 0.5f + 0.5f) * float(vp.Height) + (rnd(r) - 0.5f) * 0.04f * float(vp.Height);
				p[2] = rnd(r);
				p[3] = 1.0f / (1.0f + 10 * rnd(r));
				p += 4;
			}
			else
			{
				const float z = cz + (rnd(r) - 0.5f) * 4.0f;
				p[0] = (cx + (rnd(r) - 0.5f) * 0.06f) * z * 0.9f;
				p[1] = (cy + (rnd(r) - 0.5f) * 0.06f) * z * 0.6f;
				p[2] = z;
				p += 3;
			}
			if (f & D3DFVF_NORMAL)
			{
				float n[3] = { rnd(r) - 0.5f, rnd(r) - 0.5f, -1.0f };
				const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + 1.0f);
				p[0] = n[0] / l;
				p[1] = n[1] / l;
				p[2] = n[2] / l;
				p += 3;
			}
			DWORD *c = reinterpret_cast<DWORD *>(p);
			if (f & D3DFVF_DIFFUSE)
				*c++ = 0x40000000u | (hash32(seed + i / 3) & 0xBFFFFFFFu) | 0x80000000u * (i & 1);
			if (f & D3DFVF_SPECULAR)
				*c++ = hash32(seed * 3 + i) & 0xFF3F3F3F;
			float *t = reinterpret_cast<float *>(c);
			for (UINT k = 0; k < ((f & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT); ++k)
			{
				t[0] = rnd(r) * 2;
				t[1] = rnd(r) * 2;
				t += 2;
			}
		}
		return v;
	}

	inline UINT verts_for(D3DPRIMITIVETYPE t, UINT prims)
	{
		return t == D3DPT_TRIANGLELIST ? prims * 3 : t == D3DPT_TRIANGLESTRIP || t == D3DPT_TRIANGLEFAN ? prims + 2 : t == D3DPT_LINELIST ? prims * 2 :
			t == D3DPT_LINESTRIP ? prims + 1 : prims;
	}

	inline D3DPRIMITIVETYPE prim_named(const std::string &s)
	{
		return s == "TRIANGLESTRIP" ? D3DPT_TRIANGLESTRIP : s == "TRIANGLEFAN" ? D3DPT_TRIANGLEFAN : s == "LINELIST" ? D3DPT_LINELIST : s == "LINESTRIP" ? D3DPT_LINESTRIP :
			s == "POINTLIST" ? D3DPT_POINTLIST : D3DPT_TRIANGLELIST;
	}

	// The render targets sampled this draw, bound now (the one last drawn into of the same size that isn't the target)
	inline void bind_sampled(state &st)
	{
		for (const auto &p : st.pending_tex)
		{
			std::string pick;
			for (auto it = st.drawn_order.rbegin(); it != st.drawn_order.rend(); ++it)
				if (*it != st.current_rt && st.target_tex.count(*it) != 0)
				{
					pick = *it;
					break;
				}
			dev->SetTexture(DWORD(p.first), pick.empty() ? nullptr : st.target_tex[pick]);
		}
		st.pending_tex.clear();
	}

	inline void draw(state &st, const call &c)
	{
		// "#n TYPE prims P verts V [stride S]"
		const std::vector<std::string> w = split(c.args);
		if (w.size() < 6)
			return;
		const D3DPRIMITIVETYPE t = prim_named(w[1]);
		const UINT prims = UINT(std::atoi(w[3].c_str())), verts = UINT(std::atoi(w[5].c_str()));
		if (prims == 0)
			return;
		bind_sampled(st);
		st.draws++;
		const uint32_t seed = uint32_t(std::atoi(w[0].c_str() + 1));
		const UINT stride = fvf_stride(st.fvf);
		dev->SetVertexShader(st.fvf);
		if (c.op == "DPUP")
		{
			const UINT n = verts_for(t, prims);
			const std::vector<uint8_t> v = vertices(st, n, seed);
			dev->DrawPrimitiveUP(t, prims, v.data(), stride);
		}
		else
		{
			// Through a dynamic vertex buffer (appended, NOOVERWRITE; DISCARD when full), as the game's streams are
			const bool indexed = c.op == "DIP";
			const UINT n = indexed ? std::max(verts, 3u) : verts_for(t, prims);
			const std::vector<uint8_t> v = vertices(st, n, seed);
			const UINT bytes = UINT(v.size());
			if (st.vb_at + bytes > 4u << 20)
				st.vb_at = 0;
			BYTE *p = nullptr;
			st.vb->Lock(st.vb_at, bytes, &p, st.vb_at == 0 ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE);
			std::memcpy(p, v.data(), bytes);
			st.vb->Unlock();
			dev->SetStreamSource(0, st.vb, stride);
			const UINT first = (st.vb_at + stride - 1) / stride;
			if (first * stride != st.vb_at)
			{
				// (the start a whole vertex in: copied again there)
				st.vb_at = first * stride;
				if (st.vb_at + bytes > 4u << 20)
					st.vb_at = 0;
				st.vb->Lock(st.vb_at, bytes, &p, st.vb_at == 0 ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE);
				std::memcpy(p, v.data(), bytes);
				st.vb->Unlock();
			}
			const UINT base = st.vb_at / stride;
			st.vb_at += bytes;
			if (!indexed)
				dev->DrawPrimitive(t, base, prims);
			else
			{
				const UINT ni = verts_for(t, prims);
				std::vector<WORD> ix(ni);
				uint32_t r = seed;
				for (UINT i = 0; i < ni; ++i)
					ix[i] = WORD(t == D3DPT_TRIANGLELIST ? (i < n ? i : UINT(rnd(r) * float(n - 1))) : std::min(i, n - 1));
				if (st.ib_at + ni * 2 > 1u << 20)
					st.ib_at = 0;
				st.ib->Lock(st.ib_at, ni * 2, &p, st.ib_at == 0 ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE);
				std::memcpy(p, ix.data(), ni * 2);
				st.ib->Unlock();
				dev->SetIndices(st.ib, base);
				dev->DrawIndexedPrimitive(t, 0, n, st.ib_at / 2, prims);
				st.ib_at += align8(ni * 2);
			}
		}
		if (std::find(st.drawn_order.begin(), st.drawn_order.end(), st.current_rt) != st.drawn_order.end())
			st.drawn_order.erase(std::find(st.drawn_order.begin(), st.drawn_order.end(), st.current_rt));
		st.drawn_order.push_back(st.current_rt);
		// The visibility test's downsample: read back as the game reads it
		bool vis = false;
		for (uint32_t a : c.chain)
			vis = vis || a == 0x06C867;
		if (vis && st.sys16 != nullptr)
		{
			IDirect3DSurface8 *rt = st.surfaces[st.current_rt];
			const RECT r16 = { 0, 0, 8, 8 };
			const POINT p0 = { 0, 0 };
			LARGE_INTEGER t0, t1, qf;
			QueryPerformanceCounter(&t0);
			if (rt != nullptr && SUCCEEDED(dev->CopyRects(rt, &r16, 1, st.sys16, &p0)))
			{
				D3DLOCKED_RECT lr;
				if (SUCCEEDED(st.sys16->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
				{
					st.sys16->UnlockRect();
					st.readbacks++;
				}
			}
			QueryPerformanceCounter(&t1);
			QueryPerformanceFrequency(&qf);
			st.readback_ms += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(qf.QuadPart);
		}
	}

	inline void light(const call &c, bool &ok)
	{
		// "N TYPE diff(r g b a) spec(...) amb(...) dir(x y z) pos(x y z) range r att(a b c) ..."
		D3DLIGHT8 l = {};
		const std::vector<std::string> w = split(c.args);
		if (w.size() < 2)
		{
			ok = false;
			return;
		}
		const DWORD index = DWORD(std::atoi(w[0].c_str()));
		l.Type = w[1] == "POINT" ? D3DLIGHT_POINT : w[1] == "SPOT" ? D3DLIGHT_SPOT : D3DLIGHT_DIRECTIONAL;
		auto group = [&](const char *name, float *out, int n) {
			const size_t at = c.args.find(std::string(name) + "(");
			if (at == std::string::npos)
				return;
			std::stringstream ss(c.args.substr(at + std::strlen(name) + 1));
			for (int i = 0; i < n; ++i)
				ss >> out[i];
		};
		group("diff", &l.Diffuse.r, 4);
		group("spec", &l.Specular.r, 4);
		group("amb", &l.Ambient.r, 4);
		group("dir", &l.Direction.x, 3);
		group("pos", &l.Position.x, 3);
		group("att", &l.Attenuation0, 3);
		const size_t rg = c.args.find("range ");
		l.Range = rg != std::string::npos ? std::strtof(c.args.c_str() + rg + 6, nullptr) : 1000.0f;
		if (l.Type != D3DLIGHT_DIRECTIONAL && l.Attenuation0 == 0 && l.Attenuation1 == 0 && l.Attenuation2 == 0)
			l.Attenuation0 = 1.0f;
		dev->SetLight(index, &l);
	}

	inline int run(const char *capture, const char *out, int frames)
	{
		state st;
		if (!parse(capture, st))
		{
			std::printf("replay: %s not read\n", capture);
			return 1;
		}
		dev->CreateVertexBuffer(4u << 20, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &st.vb);
		dev->CreateIndexBuffer(1u << 20, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &st.ib);
		dev->CreateImageSurface(16, 16, D3DFMT_A8R8G8B8, &st.sys16);
		const D3DMATRIX id = identity();
		std::vector<uint32_t> world_px, screen_px;
		UINT ww = 0, wh = 0;
		std::vector<double> ms;
		double rb_sum = 0, present_sum = 0;
		LARGE_INTEGER qf;
		QueryPerformanceFrequency(&qf);
		for (int frame = 0; frame < frames; ++frame)
		{
			LARGE_INTEGER f0, f1;
			QueryPerformanceCounter(&f0);
			st.readbacks = st.skipped = st.draws = 0;
			st.readback_ms = st.present_ms = 0;
			dev->SetTransform(D3DTS_WORLD, &id);
			dev->SetTransform(D3DTS_VIEW, &id);
			// (REPLAY_STOP=n: the frame cut after its n-th draw, to find where two runs part)
			const char *stop = std::getenv("REPLAY_STOP");
			for (const call &c : st.calls)
			{
				if (stop != nullptr && st.draws >= uint32_t(std::atoi(stop)))
					break;
				const std::vector<std::string> w = split(c.args);
				if (c.op == "RS" || c.op == "TSS")
				{
					// "NAME = VALUE" / "STAGE NAME = VALUE"
					const size_t eq = c.args.find(" = ");
					const std::vector<std::string> left = split(c.args.substr(0, eq));
					const std::string value = eq != std::string::npos ? split(c.args.substr(eq + 3)).front() : std::string();
					const std::string name = left.empty() ? std::string() : left.back();
					DWORD v = 0;
					if (!value_of(name, value, v))
					{
						if (st.unknown.insert(name + " = " + value).second && frame == 0)
							std::printf("  (not replayed: %s %s)\n", c.op.c_str(), (name + " = " + value).c_str());
						st.skipped++;
						continue;
					}
					if (c.op == "RS")
					{
						const auto it = names().find(name);
						if (it != names().end())
							dev->SetRenderState(D3DRENDERSTATETYPE(it->second), v);
						else
							st.skipped++;
					}
					else
					{
						const auto it = stage_names().find(name);
						if (it != stage_names().end() && left.size() >= 2)
							dev->SetTextureStageState(DWORD(std::atoi(left[0].c_str())), D3DTEXTURESTAGESTATETYPE(it->second), v);
						else
							st.skipped++;
					}
				}
				else if (c.op == "SetRenderTarget" && w.size() >= 3)
				{
					IDirect3DSurface8 *rt = surface_of(st, w[0], false), *z = w[2] == "00000000" ? nullptr : surface_of(st, w[2], true);
					dev->SetRenderTarget(rt, z);
					st.current_rt = w[0];
					if (st.world.empty())
						st.world = w[0];
				}
				else if (c.op == "SetViewport" && w.size() >= 4)
				{
					D3DVIEWPORT8 vp = { DWORD(std::atoi(w[0].c_str()) / 2), DWORD(std::atoi(w[1].c_str()) / 2), DWORD(std::max(1, std::atoi(w[2].c_str()) / 2)),
						DWORD(std::max(1, std::atoi(w[3].c_str()) / 2)), 0.0f, 1.0f };
					if (w.size() >= 6)
					{
						vp.MinZ = std::strtof(w[5].c_str(), nullptr);
						vp.MaxZ = std::strtof(w[5].c_str() + w[5].find("..") + 2, nullptr);
					}
					dev->SetViewport(&vp);
				}
				else if (c.op == "Clear" && w.size() >= 6)
				{
					const DWORD flags = DWORD(std::strtoul(w[1].c_str(), nullptr, 16));
					dev->Clear(0, nullptr, flags, D3DCOLOR(std::strtoul(w[3].c_str(), nullptr, 16)), std::strtof(w[5].c_str(), nullptr), 0);
				}
				else if (c.op == "BeginScene")
					dev->BeginScene();
				else if (c.op == "EndScene")
					dev->EndScene();
				else if (c.op == "Xform" && !w.empty())
				{
					// (the projection and texture matrices as they were; world and view left identity: the geometry is made up in view space)
					const auto m = st.matrices.find(c.n);
					if (m != st.matrices.end() && w[0] == "PROJECTION")
						dev->SetTransform(D3DTS_PROJECTION, &m->second);
					else if (m != st.matrices.end() && w[0].rfind("TEXTURE", 0) == 0)
						dev->SetTransform(D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + std::atoi(w[0].c_str() + 7)), &m->second);
				}
				else if (c.op == "SetLight")
				{
					bool ok = true;
					light(c, ok);
				}
				else if (c.op == "LightEnable" && w.size() >= 2)
					dev->LightEnable(DWORD(std::atoi(w[0].c_str())), w[1] == "on");
				else if (c.op == "SetMaterial")
				{
					D3DMATERIAL8 m = {};
					auto group = [&](const char *name, float *o) {
						const size_t at = c.args.find(std::string(name) + "(");
						if (at != std::string::npos)
						{
							std::stringstream ss(c.args.substr(at + std::strlen(name) + 1));
							ss >> o[0] >> o[1] >> o[2] >> o[3];
						}
					};
					group("diff", &m.Diffuse.r);
					group("amb", &m.Ambient.r);
					group("spec", &m.Specular.r);
					group("emis", &m.Emissive.r);
					const size_t pw = c.args.find("power ");
					m.Power = pw != std::string::npos ? std::strtof(c.args.c_str() + pw + 6, nullptr) : 0.0f;
					dev->SetMaterial(&m);
				}
				else if (c.op == "Tex" && w.size() >= 2)
				{
					const DWORD stage = DWORD(std::atoi(w[0].c_str()));
					if (w[1] == "null")
						dev->SetTexture(stage, nullptr);
					else if (w.back() == "RT")
						st.pending_tex[int(stage)] = "rt";
					else
					{
						// "tex WxH FMT Ln"
						std::string desc;
						for (size_t i = 2; i < w.size(); ++i)
							desc += (i > 2 ? " " : "") + w[i];
						dev->SetTexture(stage, texture_named(st, desc));
					}
				}
				else if (c.op == "VS" && !w.empty())
					st.fvf = fvf_named(w[0]);
				else if (c.op == "DP" || c.op == "DIP" || c.op == "DPUP")
					draw(st, c);
				// (ApplyStateBlock, VSConst, PS, Stream, Indices, Present: left out on both sides; the frame is presented below)
			}
			// The world and the screen read back on the last frame
			if (frame == frames - 1)
			{
				auto read = [&](IDirect3DSurface8 *s, std::vector<uint32_t> &px, UINT &w, UINT &h) {
					D3DSURFACE_DESC d;
					s->GetDesc(&d);
					w = d.Width;
					h = d.Height;
					IDirect3DSurface8 *sys = nullptr;
					dev->CreateImageSurface(w, h, d.Format, &sys); // (CopyRects copies between one format only)
					px = read_target(s, w, h, sys);
					sys->Release();
				};
				if (!st.world.empty() && st.surfaces[st.world] != nullptr)
					read(st.surfaces[st.world], world_px, ww, wh);
				IDirect3DSurface8 *bb = nullptr;
				dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
				UINT sw = 0, sh = 0;
				read(bb, screen_px, sw, sh);
				bb->Release();
				write_bmp(std::string(out) + "-world.bmp", ww, wh, world_px);
				write_bmp(std::string(out) + "-screen.bmp", sw, sh, screen_px);
			}
			LARGE_INTEGER p0;
			QueryPerformanceCounter(&p0);
			dev->Present(nullptr, nullptr, nullptr, nullptr);
			QueryPerformanceCounter(&f1);
			st.present_ms = double(f1.QuadPart - p0.QuadPart) * 1000.0 / double(qf.QuadPart);
			if (frame >= 2 && frame + 1 < frames)
			{
				rb_sum += st.readback_ms;
				present_sum += st.present_ms;
			}
			ms.push_back(double(f1.QuadPart - f0.QuadPart) * 1000.0 / double(qf.QuadPart));
			MSG msg;
			while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
				DispatchMessageA(&msg);
		}
		// (the game thread's time a frame, the frames after the first two, but the last, read back: the calls and Present, the readbacks' waits in them)
		double sum = 0;
		int counted = 0;
		for (size_t i = 2; i + 1 < ms.size(); ++i)
		{
			sum += ms[i];
			counted++;
		}
		if (counted != 0)
			std::printf("replay %s: %.2f ms a frame on the calling thread (%d frames); in the visibility tests' readbacks %.3f ms, in Present %.3f ms\n", capture, sum / counted, counted, rb_sum / counted, present_sum / counted);
		std::printf("replay %s: %zu calls, %u draws, %u readbacks, %u states not replayed\n", capture, st.calls.size(), st.draws, st.readbacks, st.skipped);
		return 0;
	}
}
