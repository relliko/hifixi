/*
 * HiFi XI's own 2D renderer against dgVoodoo itself (src/game/uidraw.cpp, src/ui2d; docs/own-frame/2d.md "Tests").
 *
 * dgVoodoo's own d3d8.dll (the game's, beside this program with the game's dgVoodoo.conf but OutputAPI d3d11warp: no
 * GPU) is the reference. Each case is one frame of Direct3D 8 calls made through dgVoodoo's device: a background (a
 * gradient and a few rectangles, the "world"), then the window as the game's loop tail opens it, the case's 2D calls
 * made through the device as the game makes them (so through uidraw's vtable), the window closed:
 *   - "shadow": the recorder records and dgVoodoo draws everything; its back buffer is the reference. The recorded
 *     segments are then drawn by render2d on a Direct3D 12 WARP device with the debug layer, into a target holding the
 *     same background, on a list set up as dgVoodoo leaves its own (its root signature, heaps, pipeline, viewport,
 *     target), and dgVoodoo's next draw made after with nothing bound again. Pixels compared with dgVoodoo's;
 *   - "native": the same calls with native drawing on: dgVoodoo's back buffer is the background alone (every draw
 *     recorded was left out of its stream), the recorded frame the same as the shadow one's.
 * Cases:
 *   - the d3dlog captures' interface sections (C:\Games\PhoenixXI\d3dlog\*.txt, from just after the resolve to EndScene):
 *     every state, texture size and format, viewport, state block and draw in the game's order, the vertices and the
 *     textures' pixels made up (the capture has neither: deterministic, the quads where the game's would be likely);
 *   - every blend 11F300 maps the PS2's (A, B, D) to (REVSUBTRACT, SUBTRACT, DESTCOLOR ...), every alpha test
 *     function, both shade modes, the cull modes, MIRROR, TFACTOR, the null texture, FVF 0x44, lists and fans, a
 *     viewport clip, a pixel shader-free stage 1 left on (the game's then), a render-target texture (the game's).
 *
 *   build\dgv\ui2d_dgv.exe [capture ...]       (from build\dgv: dgVoodoo reads its dgVoodoo.conf there)
 *   UI2D_SAVE=1: writes <case>-dgv.bmp, <case>-ours.bmp, <case>-diff.bmp
 */

#include <windows.h>
#include <d3d8.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include "../src/game/game.hpp"
#include "../src/game/slotmark.hpp"
#include "../src/game/uidraw.hpp"
#include "../src/host/passes.hpp"
#include "../src/ui2d/frame2d.hpp"
#include "../src/ui2d/render2d.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace
{
	namespace u2 = hifi::ui2d;
	namespace ud = hifi::game::uidraw;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	constexpr UINT W = 2560, H = 1440;

	// ---------------------------------------------------------------- the calls a case makes

	struct tex_spec
	{
		uint32_t w = 0, h = 0, format = 0, levels = 1;
		bool rt = false;
		bool pattern = false; // (texels by position: R = 8 x, B and G stripes along x and y)
		bool operator<(const tex_spec &o) const
		{
			return std::tie(w, h, format, levels, rt) < std::tie(o.w, o.h, o.format, o.levels, o.rt);
		}
	};

	struct op
	{
		enum kind_t { rs, tss, tex, vs, ps, draw, viewport, block } kind = rs;
		uint32_t a = 0, b = 0, c = 0; // rs: state, value; tss: stage, type, value; tex: stage, spec index (or ~0); vs/ps: handle; draw: type, prims
		uint32_t v[4] = {};           // viewport
		std::vector<uint8_t> vertices; // draw (made once: the same for every run of the case)
		uint32_t stride = 0;
	};

	struct scenario
	{
		std::string name;
		std::vector<tex_spec> textures;
		std::vector<op> ops;
		uint32_t draws = 0;
		bool game_like = false; // (the vertices made up as the game's: 1:1 sprite parts and tiles; else every scale)
	};

	// ---------------------------------------------------------------- the d3dlog capture

	const std::map<std::string, uint32_t> kRS = {
		{ "ZENABLE", 7 }, { "FILLMODE", 8 }, { "SHADEMODE", 9 }, { "ZWRITEENABLE", 14 }, { "ALPHATESTENABLE", 15 }, { "SRCBLEND", 19 }, { "DESTBLEND", 20 },
		{ "CULLMODE", 22 }, { "ZFUNC", 23 }, { "ALPHAREF", 24 }, { "ALPHAFUNC", 25 }, { "ALPHABLENDENABLE", 27 }, { "FOGENABLE", 28 }, { "SPECULARENABLE", 29 },
		{ "STENCILENABLE", 52 }, { "TEXTUREFACTOR", 60 }, { "LIGHTING", 137 }, { "COLORWRITEENABLE", 168 }, { "BLENDOP", 171 } };
	const std::map<std::string, uint32_t> kTSS = {
		{ "COLOROP", 1 }, { "COLORARG1", 2 }, { "COLORARG2", 3 }, { "ALPHAOP", 4 }, { "ALPHAARG1", 5 }, { "ALPHAARG2", 6 }, { "TEXCOORDINDEX", 11 },
		{ "ADDRESSU", 13 }, { "ADDRESSV", 14 }, { "MAGFILTER", 16 }, { "MINFILTER", 17 }, { "MIPFILTER", 18 }, { "TEXTURETRANSFORMFLAGS", 24 } };
	const std::map<std::string, uint32_t> kValues = {
		{ "FALSE", 0 }, { "TRUE", 1 }, { "ADD", 1 }, { "SUBTRACT", 2 }, { "REVSUBTRACT", 3 }, { "MIN", 4 }, { "MAX", 5 },
		{ "ZERO", 1 }, { "ONE", 2 }, { "SRCCOLOR", 3 }, { "INVSRCCOLOR", 4 }, { "SRCALPHA", 5 }, { "INVSRCALPHA", 6 }, { "DESTALPHA", 7 }, { "INVDESTALPHA", 8 },
		{ "DESTCOLOR", 9 }, { "INVDESTCOLOR", 10 }, { "NEVER", 1 }, { "LESS", 2 }, { "EQUAL", 3 }, { "LESSEQUAL", 4 }, { "GREATER", 5 }, { "NOTEQUAL", 6 },
		{ "GREATEREQUAL", 7 }, { "ALWAYS", 8 }, { "DISABLE", 1 }, { "SELECTARG1", 2 }, { "SELECTARG2", 3 }, { "MODULATE", 4 }, { "MODULATE2X", 5 },
		{ "MODULATE4X", 6 }, { "DIFFUSE", 0 }, { "CURRENT", 1 }, { "TEXTURE", 2 }, { "TFACTOR", 3 }, { "WRAP", 1 }, { "MIRROR", 2 }, { "CLAMP", 3 },
		{ "NONE", 0 }, { "POINT", 1 }, { "LINEAR", 2 }, { "FLAT", 1 }, { "GOURAUD", 2 }, { "SOLID", 3 } };

	uint32_t value_of(const std::string &s, bool &ok)
	{
		ok = true;
		const auto at = kValues.find(s);
		if (at != kValues.end())
			return at->second;
		char *end = nullptr;
		const unsigned long v = std::strtoul(s.c_str(), &end, 0);
		ok = end != nullptr && *end == 0 && !s.empty();
		return uint32_t(v);
	}

	uint32_t format_of(const std::string &s)
	{
		if (s == "DXT1")
			return 0x31545844;
		if (s == "DXT3")
			return 0x33545844;
		if (s == "DXT5")
			return 0x35545844;
		if (s == "A8R8G8B8")
			return 21;
		if (s == "X8R8G8B8")
			return 22;
		if (s == "A4R4G4B4")
			return 26;
		if (s == "A1R5G5B5")
			return 25;
		if (s == "R5G6B5")
			return 23;
		return 0;
	}

	// Its interface section: from just after the resolve (the first draw after the last SetRenderTarget of the back
	// buffer, called from 0126A6) to EndScene. False when it has none.
	bool parse_capture(const std::string &path, scenario &sc, std::string &why)
	{
		std::ifstream in(path);
		if (!in)
		{
			why = "can't read " + path;
			return false;
		}
		std::vector<std::string> lines;
		for (std::string l; std::getline(in, l);)
			lines.push_back(l);
		size_t start = 0;
		for (size_t i = 0; i < lines.size(); ++i)
			if (lines[i].find("SetRenderTarget") != std::string::npos && lines[i].find("0x0126A6]") != std::string::npos && lines[i].find(" @#") == std::string::npos)
				start = i;
		if (start == 0)
		{
			why = "no resolve in " + path;
			return false;
		}
		while (start < lines.size() && lines[start].find(" DPUP ") == std::string::npos)
			start++;
		std::map<tex_spec, uint32_t> specs;
		for (size_t i = start + 1; i < lines.size(); ++i)
		{
			std::istringstream ls(lines[i]);
			std::string seqno, kind;
			ls >> seqno >> kind;
			if (kind == "EndScene" || kind == "Present" || kind == "SetRenderTarget")
				break;
			op o;
			if (kind == "RS" || kind == "TSS")
			{
				std::string stage, name, eq, value;
				if (kind == "TSS")
					ls >> stage;
				ls >> name >> eq >> value;
				const auto &table = kind == "RS" ? kRS : kTSS;
				const auto at = table.find(name);
				bool ok = false;
				// (D3DTTFF_DISABLE is 0, D3DTOP_DISABLE 1)
				const uint32_t v = name == "TEXTURETRANSFORMFLAGS" && value == "DISABLE" ? (ok = true, 0u) : value_of(value, ok);
				if (at == table.end() || !ok)
					continue; // (LIGHTING and the like: nothing a pre-transformed draw reads)
				o.kind = kind == "RS" ? op::rs : op::tss;
				if (kind == "RS")
					o.a = at->second, o.b = v;
				else
					o.a = uint32_t(std::atoi(stage.c_str())), o.b = at->second, o.c = v;
			}
			else if (kind == "Tex")
			{
				std::string stage, what, size, fmt, levels, rt;
				ls >> stage >> what;
				o.kind = op::tex;
				o.a = uint32_t(std::atoi(stage.c_str()));
				o.b = ~0u;
				if (what == "tex")
				{
					ls >> size >> fmt >> levels >> rt;
					tex_spec t;
					std::sscanf(size.c_str(), "%ux%u", &t.w, &t.h);
					t.format = format_of(fmt);
					t.levels = uint32_t(std::atoi(levels.c_str() + 1));
					t.rt = rt == "RT";
					if (t.format == 0 || t.w == 0)
						continue;
					const auto at = specs.find(t);
					if (at == specs.end())
					{
						specs[t] = uint32_t(sc.textures.size());
						o.b = uint32_t(sc.textures.size());
						sc.textures.push_back(t);
					}
					else
						o.b = at->second;
				}
			}
			else if (kind == "VS")
			{
				std::string fvf;
				ls >> fvf;
				o.kind = op::vs;
				o.a = fvf == "XYZRHW|DIFFUSE|TEX1" ? 0x144 : fvf == "XYZRHW|DIFFUSE" ? 0x44 : 0x2;
			}
			else if (kind == "SetViewport")
			{
				o.kind = op::viewport;
				ls >> o.v[0] >> o.v[1] >> o.v[2] >> o.v[3];
			}
			else if (kind == "ApplyStateBlock")
				o.kind = op::block;
			else if (kind == "DPUP")
			{
				std::string number, type, word;
				uint32_t prims = 0, verts = 0, stride = 0;
				ls >> number >> type >> word >> prims >> word >> verts >> word >> stride;
				o.kind = op::draw;
				o.a = type == "TRIANGLESTRIP" ? 5 : type == "TRIANGLELIST" ? 4 : type == "TRIANGLEFAN" ? 6 : 1;
				o.b = prims;
				o.stride = stride;
				sc.draws++;
			}
			else
				continue;
			sc.ops.push_back(std::move(o));
		}
		return sc.draws != 0;
	}

	// ---------------------------------------------------------------- made-up vertices and pixels

	struct rng
	{
		uint32_t s;
		explicit rng(uint32_t seed) : s(seed * 2654435761u + 0x9E3779B9u) {}
		uint32_t next()
		{
			s ^= s << 13;
			s ^= s >> 17;
			s ^= s << 5;
			return s;
		}
		uint32_t below(uint32_t n) { return n == 0 ? 0 : next() % n; }
		float unit() { return float(next() & 0xFFFFFF) / float(0x1000000); }
	};

	uint32_t colour(rng &r)
	{
		static const uint8_t kAlphas[] = { 0x20, 0x40, 0x60, 0x70, 0x7F, 0x80, 0xC0, 0xFF };
		const uint32_t a = kAlphas[r.below(8)];
		const uint32_t rr = 0x40 + r.below(0xC0), gg = 0x40 + r.below(0xC0), bb = 0x40 + r.below(0xC0);
		return a << 24 | rr << 16 | gg << 8 | bb;
	}

	// The vertices of one draw: where the game's would likely be (inside the viewport; a 16-pixel cell of the font for
	// the big text atlas, the texture's own size for the rest), as the game makes them (edges at a pixel - 0.5)
	void make_vertices(op &o, uint32_t index, uint32_t fvf, const tex_spec *t, const uint32_t vp[4], bool game_like)
	{
		rng r(index + 17);
		const uint32_t stride = fvf == 0x44 ? 20 : 28;
		o.stride = stride;
		const uint32_t type = o.a, prims = std::max<uint32_t>(o.b, 1);
		const uint32_t count = type == 4 ? 3 * prims : prims + 2;
		o.vertices.assign(size_t(count) * stride, 0);
		float qw, qh, u0 = 0.0f, v0 = 0.0f, du = 1.0f, dv = 1.0f;
		if (t != nullptr && t->w >= 1024)
		{
			// (a glyph cell of the text atlas: 64 x 128 cells)
			qw = qh = 16.0f;
			const uint32_t cell = 0x20 + r.below(0x60);
			u0 = float(cell & 63) / 64.0f, v0 = float(cell >> 6) / 128.0f, du = 1.0f / 64.0f, dv = 1.0f / 128.0f;
		}
		else if (t != nullptr && game_like && r.below(10) < 9)
		{
			// (as the game draws them: a sprite part cut from an atlas at 1:1, or a window's background tiled at 1:1)
			if (r.below(6) != 0)
			{
				const uint32_t sw = 4 + r.below(std::min<uint32_t>(t->w, 64) - 3), sh = 4 + r.below(std::min<uint32_t>(t->h, 64) - 3);
				const uint32_t ux = r.below(t->w - sw + 1), vy = r.below(t->h - sh + 1);
				qw = float(sw), qh = float(sh);
				u0 = float(ux) / float(t->w), v0 = float(vy) / float(t->h), du = qw / float(t->w), dv = qh / float(t->h);
			}
			else
			{
				qw = float(40 + r.below(400)), qh = float(20 + r.below(200));
				du = qw / float(t->w), dv = qh / float(t->h);
			}
		}
		else if (t != nullptr)
		{
			static const float kScale[] = { 0.5f, 1.0f, 1.0f, 1.5f, 2.0f, 3.0f };
			qw = std::max(4.0f, std::floor(float(t->w) * kScale[r.below(6)]));
			qh = std::max(4.0f, std::floor(float(t->h) * kScale[r.below(6)]));
			if (r.below(4) == 0)
				du = dv = 2.0f; // (tiled: WRAP)
			if (r.below(6) == 0)
				u0 = -0.25f, v0 = 0.5f;
		}
		else
			qw = float(20 + r.below(300)), qh = float(10 + r.below(160));
		const float vx = float(vp[0]), vy = float(vp[1]), vw = float(vp[2]), vh = float(vp[3]);
		// (one in eight reaches over the viewport's edge: the clip)
		const bool over = r.below(8) == 0;
		const float x0 = std::floor(vx + (over ? -qw * 0.5f : 0.0f) + r.unit() * std::max(1.0f, vw - (over ? 0.0f : qw))) - 0.5f;
		const float y0 = std::floor(vy + (over ? -qh * 0.5f : 0.0f) + r.unit() * std::max(1.0f, vh - (over ? 0.0f : qh))) - 0.5f;
		const bool gradient = r.below(3) == 0;
		const uint32_t c0 = colour(r);
		const auto put = [&](uint32_t i, float x, float y, float u, float v) {
			uint8_t *p = o.vertices.data() + size_t(i) * stride;
			const float z = 0.0f, rhw = 1.0f;
			const uint32_t c = gradient ? colour(r) : c0;
			std::memcpy(p, &x, 4), std::memcpy(p + 4, &y, 4), std::memcpy(p + 8, &z, 4), std::memcpy(p + 12, &rhw, 4), std::memcpy(p + 16, &c, 4);
			if (stride == 28)
				std::memcpy(p + 20, &u, 4), std::memcpy(p + 24, &v, 4);
		};
		if (type == 5 && prims == 2)
		{
			// TL, TR, BL, BR; one in ten turned (the cursor hands' rotated corners)
			float cx[4] = { x0, x0 + qw, x0, x0 + qw }, cy[4] = { y0, y0, y0 + qh, y0 + qh };
			float cu[4] = { u0, u0 + du, u0, u0 + du }, cv[4] = { v0, v0, v0 + dv, v0 + dv };
			const uint32_t turn = r.below(10) == 0 ? 1 + r.below(3) : 0;
			static const uint32_t kOrder[4][4] = { { 0, 1, 2, 3 }, { 1, 3, 0, 2 }, { 3, 2, 1, 0 }, { 2, 0, 3, 1 } };
			for (uint32_t i = 0; i < 4; ++i)
				put(i, cx[kOrder[turn][i]], cy[kOrder[turn][i]], cu[i], cv[i]);
		}
		else if (type == 5)
			for (uint32_t i = 0; i < count; ++i)
			{
				const float f = float(i / 2) / float(std::max<uint32_t>(1, (count - 1) / 2));
				put(i, x0 + f * qw, y0 + (i & 1) * qh, u0 + f * du, v0 + (i & 1) * dv);
			}
		else if (type == 6)
		{
			put(0, x0 + qw * 0.5f, y0 + qh * 0.5f, u0 + du * 0.5f, v0 + dv * 0.5f);
			for (uint32_t i = 1; i < count; ++i)
			{
				const float a = 6.2831853f * float(i - 1) / float(count - 1);
				put(i, x0 + qw * 0.5f * (1.0f + std::cos(a)), y0 + qh * 0.5f * (1.0f + std::sin(a)), u0 + du * 0.5f * (1.0f + std::cos(a)), v0 + dv * 0.5f * (1.0f + std::sin(a)));
			}
		}
		else
			for (uint32_t i = 0; i < count; ++i)
				put(i, x0 + r.unit() * qw, y0 + r.unit() * qh, u0 + r.unit() * du, v0 + r.unit() * dv);
	}

	// A texture's level as the game's DAT pictures look: a few shapes with soft edges and alpha from clear to opaque
	// (0x80 = opaque under MODULATE2X), so blends, alpha tests and filters all show
	void fill_level(const tex_spec &t, uint32_t level, uint32_t lw, uint32_t lh, uint8_t *bits, int32_t pitch, uint32_t seed)
	{
		rng r(seed * 31 + level);
		const bool block = t.format == 0x31545844 || t.format == 0x33545844 || t.format == 0x35545844;
		if (block)
		{
			const uint32_t bw = std::max(1u, (lw + 3) / 4), bh = std::max(1u, (lh + 3) / 4), size = t.format == 0x31545844 ? 8 : 16;
			for (uint32_t by = 0; by < bh; ++by)
				for (uint32_t bx = 0; bx < bw; ++bx)
				{
					uint8_t *b = bits + by * pitch + bx * size;
					for (uint32_t k = 0; k < size; ++k)
						b[k] = uint8_t(r.next());
					if (t.format == 0x31545844 && r.below(3) != 0)
					{
						// (mostly 4-colour blocks; the rest keep their transparent index)
						const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
						if (c0 <= c1)
							b[0] = uint8_t(c1), b[1] = uint8_t((c1 >> 8) | 0x80), b[2] = uint8_t(c0), b[3] = uint8_t(c0 >> 8 & 0x7F);
					}
				}
			return;
		}
		if (t.pattern)
		{
			for (uint32_t y = 0; y < lh; ++y)
				for (uint32_t x = 0; x < lw; ++x)
				{
					uint8_t *q = bits + y * pitch + x * 4;
					q[0] = uint8_t(x & 1 ? 0xF0 : 0x10), q[1] = uint8_t(y & 1 ? 0xF0 : 0x10), q[2] = uint8_t(x * 8), q[3] = 0x80;
				}
			return;
		}
		const uint32_t bpp = t.format == 21 || t.format == 22 ? 4 : 2;
		for (uint32_t y = 0; y < lh; ++y)
			for (uint32_t x = 0; x < lw; ++x)
			{
				uint8_t *p = bits + y * pitch + x * bpp;
				// (blobs: alpha falls off from each cell's middle)
				const uint32_t cell = 8;
				const float fx = float(x % cell) / cell - 0.5f, fy = float(y % cell) / cell - 0.5f;
				const float d = std::sqrt(fx * fx + fy * fy) * 2.4f;
				const uint32_t salt = (x / cell * 7 + y / cell * 13 + seed) & 7;
				const uint32_t a = d >= 1.0f ? 0 : uint32_t((1.0f - d) * float(salt < 4 ? 0x80 : 0xFF));
				if (bpp == 4)
				{
					p[0] = uint8_t(r.next()), p[1] = uint8_t(r.next()), p[2] = uint8_t(0x80 + r.below(0x80)), p[3] = uint8_t(a);
				}
				else
				{
					const uint16_t v = uint16_t(r.next() & 0x0FFF) | uint16_t((a >> 4) << 12);
					std::memcpy(p, &v, 2);
				}
			}
	}

	// ---------------------------------------------------------------- synthetic coverage

	op set_rs(uint32_t s, uint32_t v)
	{
		op o;
		o.kind = op::rs, o.a = s, o.b = v;
		return o;
	}
	op set_tss(uint32_t stage, uint32_t t, uint32_t v)
	{
		op o;
		o.kind = op::tss, o.a = stage, o.b = t, o.c = v;
		return o;
	}
	op set_tex(uint32_t spec)
	{
		op o;
		o.kind = op::tex, o.a = 0, o.b = spec;
		return o;
	}
	op set_vs(uint32_t fvf)
	{
		op o;
		o.kind = op::vs, o.a = fvf;
		return o;
	}
	op set_vp(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
	{
		op o;
		o.kind = op::viewport;
		o.v[0] = x, o.v[1] = y, o.v[2] = w, o.v[3] = h;
		return o;
	}
	op draw_op(uint32_t type, uint32_t prims)
	{
		op o;
		o.kind = op::draw, o.a = type, o.b = prims;
		return o;
	}

	scenario coverage()
	{
		scenario sc;
		sc.name = "coverage";
		sc.textures = { { 32, 32, 0x33545844, 1, false }, { 64, 64, 0x31545844, 1, false }, { 256, 256, 21, 1, false }, { 32, 32, 0x31545844, 6, false },
			{ 2048, 4096, 21, 1, false }, { 128, 64, 26, 1, false }, { 512, 256, 21, 1, true } };
		std::vector<op> &o = sc.ops;
		// The UI states (1604B0)
		for (const op &x : { set_rs(7, 0), set_rs(14, 0), set_rs(28, 0), set_rs(15, 1), set_rs(25, 5), set_rs(24, 0), set_rs(22, 1), set_tss(0, 17, 2),
				 set_tss(0, 16, 1), set_tss(0, 13, 1), set_tss(0, 14, 1), set_tss(0, 24, 0), set_tss(0, 1, 5), set_tss(0, 2, 2), set_tss(0, 3, 1),
				 set_tss(0, 4, 5), set_tss(0, 5, 2), set_tss(0, 6, 1), set_tss(1, 1, 1), set_tss(1, 4, 1), set_vs(0x144), set_tex(0) })
			o.push_back(x);
		// Every blend 11F300 sets (with the BLENDOP cap), each over a few draws and textures
		struct blend
		{
			uint32_t on, src, dst, op;
		};
		const blend blends[] = { { 1, 5, 6, 1 }, { 1, 5, 2, 1 }, { 1, 5, 2, 3 }, { 1, 5, 1, 1 }, { 0, 2, 1, 1 }, { 1, 6, 5, 1 }, { 1, 2, 5, 1 }, { 1, 9, 5, 1 },
			{ 1, 1, 5, 1 }, { 1, 2, 5, 2 }, { 1, 1, 6, 1 }, { 1, 2, 2, 1 }, { 1, 10, 1, 1 } };
		uint32_t tex = 0;
		for (const blend &b : blends)
		{
			o.push_back(set_rs(27, b.on));
			o.push_back(set_rs(19, b.src));
			o.push_back(set_rs(20, b.dst));
			o.push_back(set_rs(171, b.op));
			for (int k = 0; k < 3; ++k)
			{
				o.push_back(set_tex(tex++ % 4));
				o.push_back(draw_op(5, 2));
			}
		}
		o.push_back(set_rs(27, 1));
		o.push_back(set_rs(19, 5));
		o.push_back(set_rs(20, 6));
		o.push_back(set_rs(171, 1));
		// Every alpha test function, a few references
		for (uint32_t f = 1; f <= 8; ++f)
			for (const uint32_t ref : { 0x00u, 0x60u, 0x80u })
			{
				o.push_back(set_rs(25, f));
				o.push_back(set_rs(24, ref));
				o.push_back(set_tex(2));
				o.push_back(draw_op(5, 2));
			}
		o.push_back(set_rs(25, 5));
		o.push_back(set_rs(24, 0));
		// Filters and addressing: MAG/MIN point and linear, mirror, clamp, wrap; mips (none, point, linear)
		for (uint32_t mag = 1; mag <= 2; ++mag)
			for (uint32_t min = 1; min <= 2; ++min)
				for (uint32_t addr = 1; addr <= 3; ++addr)
				{
					o.push_back(set_tss(0, 16, mag));
					o.push_back(set_tss(0, 17, min));
					o.push_back(set_tss(0, 13, addr));
					o.push_back(set_tss(0, 14, addr == 2 ? 3 : addr));
					o.push_back(set_tex(addr == 3 ? 5 : 1));
					o.push_back(draw_op(5, 2));
				}
		for (uint32_t mip = 0; mip <= 2; ++mip)
		{
			o.push_back(set_tss(0, 18, mip));
			o.push_back(set_tex(3));
			o.push_back(draw_op(5, 2));
		}
		o.push_back(set_tss(0, 18, 0));
		o.push_back(set_tss(0, 16, 1));
		o.push_back(set_tss(0, 17, 2));
		o.push_back(set_tss(0, 13, 1));
		o.push_back(set_tss(0, 14, 1));
		// Text: the atlas, additive (the text object's states)
		o.push_back(set_tex(4));
		o.push_back(set_rs(19, 2));
		o.push_back(set_rs(20, 2));
		for (int k = 0; k < 24; ++k)
			o.push_back(draw_op(5, 2));
		// The other combiner ops and arguments: SELECTARG1(TEXTURE), SELECTARG2, MODULATE, MODULATE4X, ADD, TFACTOR, complement
		o.push_back(set_rs(19, 5));
		o.push_back(set_rs(20, 6));
		o.push_back(set_rs(60, 0x80FF8040));
		const uint32_t ops[][6] = { { 2, 2, 1, 2, 2, 1 }, { 3, 2, 0, 3, 2, 0 }, { 4, 2, 1, 4, 2, 1 }, { 6, 2, 1, 5, 2, 1 }, { 7, 2, 1, 5, 2, 1 },
			{ 5, 2, 3, 5, 2, 3 }, { 5, 0x12, 1, 5, 2, 1 }, { 5, 0x22, 1, 2, 2, 1 }, { 1, 2, 1, 1, 2, 1 }, { 13, 2, 0, 5, 2, 1 } };
		for (const auto &x : ops)
		{
			o.push_back(set_tss(0, 1, x[0]));
			o.push_back(set_tss(0, 2, x[1]));
			o.push_back(set_tss(0, 3, x[2]));
			o.push_back(set_tss(0, 4, x[3]));
			o.push_back(set_tss(0, 5, x[4]));
			o.push_back(set_tss(0, 6, x[5]));
			o.push_back(set_tex(2));
			o.push_back(draw_op(5, 2));
		}
		o.push_back(set_tss(0, 1, 5));
		o.push_back(set_tss(0, 2, 2));
		o.push_back(set_tss(0, 3, 1));
		o.push_back(set_tss(0, 4, 5));
		o.push_back(set_tss(0, 5, 2));
		o.push_back(set_tss(0, 6, 1));
		// No texture set, TEXTURE asked for (what the null texture reads as)
		o.push_back(set_tex(~0u));
		o.push_back(draw_op(5, 2));
		o.push_back(draw_op(5, 2));
		o.push_back(set_tss(0, 1, 2)); // (SELECTARG1(TEXTURE), then ADD(TEXTURE, DIFFUSE), alpha SELECTARG1(TEXTURE))
		o.push_back(set_tss(0, 4, 2));
		o.push_back(draw_op(5, 2));
		o.push_back(set_tss(0, 1, 7));
		o.push_back(draw_op(5, 2));
		o.push_back(set_tss(0, 1, 5));
		o.push_back(set_tss(0, 4, 5));
		// FVF 0x44 with SELECTARG1(DIFFUSE) (the fade, the credits backdrop), flat and Gouraud
		o.push_back(set_vs(0x44));
		o.push_back(set_tss(0, 1, 2));
		o.push_back(set_tss(0, 2, 0));
		o.push_back(set_tss(0, 4, 2));
		o.push_back(set_tss(0, 5, 0));
		for (const uint32_t shade : { 1u, 2u })
		{
			o.push_back(set_rs(9, shade));
			for (int k = 0; k < 3; ++k)
				o.push_back(draw_op(5, 2));
		}
		o.push_back(set_rs(9, 2));
		// Lists, fans, longer strips; each cull mode
		o.push_back(set_vs(0x144));
		o.push_back(set_tss(0, 1, 5));
		o.push_back(set_tss(0, 2, 2));
		o.push_back(set_tss(0, 4, 5));
		o.push_back(set_tss(0, 5, 2));
		o.push_back(set_tex(0));
		for (const uint32_t cull : { 1u, 2u, 3u })
		{
			o.push_back(set_rs(22, cull));
			o.push_back(draw_op(4, 4));
			o.push_back(draw_op(6, 6));
			o.push_back(draw_op(5, 6));
			o.push_back(draw_op(5, 2));
		}
		o.push_back(set_rs(22, 1));
		// The chat log's clip
		o.push_back(set_vp(16, 1209, 980, 211));
		for (int k = 0; k < 12; ++k)
			o.push_back(draw_op(5, 2));
		o.push_back(set_vp(0, 0, W, H));
		// Left to the game: stage 1 on, Z on, a render-target texture, fog; each a cut, then recorded again
		o.push_back(set_tss(1, 1, 4));
		o.push_back(draw_op(5, 2));
		o.push_back(set_tss(1, 1, 1));
		o.push_back(draw_op(5, 2));
		o.push_back(set_rs(7, 1));
		o.push_back(draw_op(5, 2));
		o.push_back(set_rs(7, 0));
		o.push_back(set_tex(6));
		o.push_back(draw_op(5, 2));
		o.push_back(set_tex(1));
		o.push_back(draw_op(5, 2));
		o.push_back(set_rs(28, 1));
		o.push_back(draw_op(5, 2));
		o.push_back(set_rs(28, 0));
		o.push_back(draw_op(5, 2));
		for (const op &x : o)
			sc.draws += x.kind == op::draw;
		return sc;
	}

	// One textured quad of a known pattern (UI2D_PROBE=mag,min,scale): where texels land
	scenario probe(uint32_t mag, uint32_t min, float scale)
	{
		scenario sc;
		sc.name = "probe";
		tex_spec t;
		t.w = t.h = 32, t.format = 21, t.pattern = true;
		sc.textures.push_back(t);
		for (const op &x : { set_rs(7, 0), set_rs(28, 0), set_rs(15, 0), set_rs(27, 0), set_rs(22, 1), set_tss(0, 16, mag), set_tss(0, 17, min),
				 set_tss(0, 13, 3), set_tss(0, 14, 3), set_tss(0, 1, 5), set_tss(0, 2, 2), set_tss(0, 3, 0), set_tss(0, 4, 5), set_tss(0, 5, 2), set_tss(0, 6, 0),
				 set_tss(1, 1, 1), set_vs(0x144), set_tex(0) })
			sc.ops.push_back(x);
		op d = draw_op(5, 2);
		d.stride = 28;
		const float x0 = 99.5f, y0 = 99.5f, w = 32.0f * scale;
		const float corners[4][4] = { { x0, y0, 0, 0 }, { x0 + w, y0, 1, 0 }, { x0, y0 + w, 0, 1 }, { x0 + w, y0 + w, 1, 1 } };
		d.vertices.resize(4 * 28);
		for (int i = 0; i < 4; ++i)
		{
			uint8_t *q = d.vertices.data() + i * 28;
			const float z = 0.0f, rhw = 1.0f;
			const uint32_t c = 0x80808080;
			std::memcpy(q, &corners[i][0], 4), std::memcpy(q + 4, &corners[i][1], 4), std::memcpy(q + 8, &z, 4), std::memcpy(q + 12, &rhw, 4);
			std::memcpy(q + 16, &c, 4), std::memcpy(q + 20, &corners[i][2], 4), std::memcpy(q + 24, &corners[i][3], 4);
		}
		sc.ops.push_back(d);
		sc.draws = 1;
		return sc;
	}

	// ---------------------------------------------------------------- dgVoodoo

	struct dgv
	{
		HMODULE dll = nullptr;
		IDirect3D8 *d3d = nullptr;
		IDirect3DDevice8 *dev = nullptr;
		HWND hwnd = nullptr;
		DWORD block = 0; // (a state block: the device's defaults, for the captures' ApplyStateBlock)
	};

	bool open_dgv(dgv &d, std::string &why)
	{
		WNDCLASSA wc = {};
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = GetModuleHandleA(nullptr);
		wc.lpszClassName = "ui2d_dgv";
		RegisterClassA(&wc);
		d.hwnd = CreateWindowA("ui2d_dgv", "ui2d dgVoodoo", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
		d.dll = LoadLibraryA(".\\d3d8.dll");
		if (d.dll == nullptr)
		{
			why = "dgVoodoo's d3d8.dll isn't beside this program";
			return false;
		}
		using create_fn = IDirect3D8 *(WINAPI *)(UINT);
		const auto create = reinterpret_cast<create_fn>(GetProcAddress(d.dll, "Direct3DCreate8"));
		d.d3d = create != nullptr ? create(D3D_SDK_VERSION) : nullptr;
		if (d.d3d == nullptr)
		{
			why = "Direct3DCreate8 failed";
			return false;
		}
		D3DPRESENT_PARAMETERS pp = {};
		pp.BackBufferWidth = W;
		pp.BackBufferHeight = H;
		pp.BackBufferFormat = D3DFMT_X8R8G8B8;
		pp.BackBufferCount = 1;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.hDeviceWindow = d.hwnd;
		pp.Windowed = TRUE;
		pp.EnableAutoDepthStencil = TRUE;
		pp.AutoDepthStencilFormat = D3DFMT_D24S8;
		const HRESULT hr = d.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d.hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d.dev);
		if (FAILED(hr) || d.dev == nullptr)
		{
			char t[64];
			std::snprintf(t, sizeof(t), "CreateDevice 0x%08lX", hr);
			why = t;
			return false;
		}
		d.dev->CreateStateBlock(D3DSBT_ALL, &d.block);
		return true;
	}

	std::vector<IDirect3DTexture8 *> make_textures(dgv &d, const scenario &sc)
	{
		std::vector<IDirect3DTexture8 *> out;
		for (size_t i = 0; i < sc.textures.size(); ++i)
		{
			const tex_spec &t = sc.textures[i];
			IDirect3DTexture8 *tx = nullptr;
			if (t.rt)
				d.dev->CreateTexture(t.w, t.h, 1, D3DUSAGE_RENDERTARGET, D3DFORMAT(t.format), D3DPOOL_DEFAULT, &tx);
			else if (SUCCEEDED(d.dev->CreateTexture(t.w, t.h, t.levels, 0, D3DFORMAT(t.format), D3DPOOL_MANAGED, &tx)))
				for (uint32_t l = 0; l < tx->GetLevelCount(); ++l)
				{
					D3DSURFACE_DESC sd;
					D3DLOCKED_RECT lr;
					tx->GetLevelDesc(l, &sd);
					if (SUCCEEDED(tx->LockRect(l, &lr, nullptr, 0)))
					{
						fill_level(t, l, sd.Width, sd.Height, static_cast<uint8_t *>(lr.pBits), lr.Pitch, uint32_t(i) + 1);
						tx->UnlockRect(l);
					}
				}
			out.push_back(tx);
		}
		return out;
	}

	// The back buffer's pixels (B, G, R, X)
	bool read_back(dgv &d, std::vector<uint8_t> &out)
	{
		IDirect3DSurface8 *bb = nullptr, *img = nullptr;
		if (FAILED(d.dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)))
			return false;
		bool ok = SUCCEEDED(d.dev->CreateImageSurface(W, H, D3DFMT_X8R8G8B8, &img)) && SUCCEEDED(d.dev->CopyRects(bb, nullptr, 0, img, nullptr));
		D3DLOCKED_RECT lr;
		if (ok && SUCCEEDED(img->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
		{
			out.resize(size_t(W) * H * 4);
			for (UINT y = 0; y < H; ++y)
				std::memcpy(out.data() + size_t(y) * W * 4, static_cast<const uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch, W * 4);
			img->UnlockRect();
		}
		else
			ok = false;
		if (img != nullptr)
			img->Release();
		bb->Release();
		return ok;
	}

	// The "world": a gradient over the screen and a few solid rectangles (no window open)
	void background(dgv &d)
	{
		struct v
		{
			float x, y, z, rhw;
			DWORD c;
		};
		IDirect3DDevice8 *dev = d.dev;
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
		dev->SetTexture(0, nullptr);
		dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		const v full[4] = { { -0.5f, -0.5f, 0, 1, 0xFF203040 }, { W - 0.5f, -0.5f, 0, 1, 0xFF608050 }, { -0.5f, H - 0.5f, 0, 1, 0xFF905020 }, { W - 0.5f, H - 0.5f, 0, 1, 0xFFC0C0E0 } };
		dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, full, sizeof(v));
		rng r(5);
		for (int k = 0; k < 40; ++k)
		{
			const float x = float(r.below(W - 200)), y = float(r.below(H - 200)), w = float(40 + r.below(300)), h = float(40 + r.below(300));
			const DWORD c = 0xFF000000 | r.next();
			const v q[4] = { { x - 0.5f, y - 0.5f, 0, 1, c }, { x + w - 0.5f, y - 0.5f, 0, 1, c }, { x - 0.5f, y + h - 0.5f, 0, 1, c }, { x + w - 0.5f, y + h - 0.5f, 0, 1, c } };
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(v));
		}
		// (the resolve's states left behind, as the game's tail leaves them: MODULATE2X, LINEAR, CLAMP)
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE2X);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
		dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
		dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
		dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
		dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
		dev->SetRenderState(D3DRS_ALPHAREF, 0x60);
		dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
		dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
		dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		dev->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		dev->SetViewport(nullptr);
		D3DVIEWPORT8 vp = { 0, 0, W, H, 0.0f, 1.0f };
		dev->SetViewport(&vp);
	}

	// The case's calls, through the device (so through the recorder's vtable while the window is open)
	void run_ops(dgv &d, scenario &sc, const std::vector<IDirect3DTexture8 *> &tex)
	{
		IDirect3DDevice8 *dev = d.dev;
		uint32_t fvf = 0x144, vp[4] = { 0, 0, W, H }, index = 0, current = ~0u;
		const char *limit = std::getenv("UI2D_LIMIT");
		const uint32_t most = limit != nullptr ? uint32_t(std::atoi(limit)) : ~0u;
		const char *from = std::getenv("UI2D_FROM");
		const uint32_t first = from != nullptr ? uint32_t(std::atoi(from)) : 0;
		for (op &o : sc.ops)
		{
			if (o.kind == op::draw && index >= most)
				break;
			switch (o.kind)
			{
			case op::rs:
				dev->SetRenderState(D3DRENDERSTATETYPE(o.a), o.b);
				break;
			case op::tss:
				dev->SetTextureStageState(o.a, D3DTEXTURESTAGESTATETYPE(o.b), o.c);
				break;
			case op::tex:
				current = o.b;
				dev->SetTexture(o.a, o.b < tex.size() ? tex[o.b] : nullptr);
				break;
			case op::vs:
				fvf = o.a;
				dev->SetVertexShader(o.a);
				break;
			case op::ps:
				dev->SetPixelShader(o.a);
				break;
			case op::viewport:
			{
				std::memcpy(vp, o.v, 16);
				D3DVIEWPORT8 v = { o.v[0], o.v[1], o.v[2], o.v[3], 0.0f, 1.0f };
				dev->SetViewport(&v);
				break;
			}
			case op::block:
				dev->ApplyStateBlock(d.block);
				// (the captures' blocks are the game's defaults: its own tail sets what it needs after one; these are
				// the device's, so the UI states the game would have are put back)
				dev->SetRenderState(D3DRS_ZENABLE, FALSE);
				dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
				dev->SetRenderState(D3DRS_LIGHTING, FALSE);
				dev->SetVertexShader(fvf);
				break;
			case op::draw:
				if (o.vertices.empty())
					make_vertices(o, index, fvf, current < sc.textures.size() ? &sc.textures[current] : nullptr, vp, sc.game_like);
				if (index++ >= first)
					dev->DrawPrimitiveUP(D3DPRIMITIVETYPE(o.a), std::max<uint32_t>(o.b, 1), o.vertices.data(), o.stride);
				break;
			}
		}
	}

	// ---------------------------------------------------------------- Direct3D 12

	// The module's failures so far (its status line: "... N failed")
	uint64_t ui2d_failed()
	{
		for (const hifi::passes::module &m : hifi::passes::modules())
			if (m.name != nullptr && std::strcmp(m.name, "ui") == 0 && m.status != nullptr)
				for (const std::string &line : m.status())
				{
					const size_t at = line.find(" failed");
					if (line.find("Direct3D 12:") == std::string::npos || at == std::string::npos)
						continue;
					size_t from = at;
					while (from > 0 && line[from - 1] >= '0' && line[from - 1] <= '9')
						--from;
					return std::strtoull(line.c_str() + from, nullptr, 10);
				}
		return ~0ull;
	}

	struct d12
	{
		ID3D12Device *device = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		ID3D12CommandAllocator *alloc = nullptr;
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12Fence *fence = nullptr;
		uint64_t fence_value = 0;
		HANDLE event = nullptr;
		ID3D12InfoQueue *info = nullptr;
		// dgVoodoo's stand-ins: its target, root signature, pipeline, heaps
		ID3D12Resource *target = nullptr, *depth = nullptr, *readback = nullptr, *upload = nullptr;
		ID3D12DescriptorHeap *rtv_heap = nullptr, *game_heap = nullptr, *game_samplers = nullptr;
		ID3D12RootSignature *game_root = nullptr;
		ID3D12PipelineState *game_pso = nullptr;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT spot = {};
	};

	const char *kGame = R"(
cbuffer c : register(b0) { float4 rect; float4 colour; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
	float2 p = float2(id & 1 ? rect.z : rect.x, id & 2 ? rect.w : rect.y);
	return float4(p, 0, 1);
}
float4 ps() : SV_Target { return colour; }
)";

	void wait(d12 &g)
	{
		const uint64_t v = ++g.fence_value;
		g.queue->Signal(g.fence, v);
		if (g.fence->GetCompletedValue() < v)
		{
			g.fence->SetEventOnCompletion(v, g.event);
			WaitForSingleObject(g.event, 60000);
		}
	}

	uint32_t debug_messages(ID3D12InfoQueue *q, std::string &first)
	{
		if (q == nullptr)
			return 0;
		const UINT64 n = q->GetNumStoredMessages();
		uint32_t count = 0;
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T size = 0;
			q->GetMessage(i, nullptr, &size);
			std::vector<char> buf(size);
			D3D12_MESSAGE *m = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
			if (FAILED(q->GetMessage(i, m, &size)))
				continue;
			if (m->Severity > D3D12_MESSAGE_SEVERITY_WARNING)
				continue;
			if (count++ == 0)
				first.assign(m->pDescription, m->DescriptionByteLength);
		}
		q->ClearStoredMessages();
		return count;
	}

	bool open_d12(d12 &g, std::string &why)
	{
		ID3D12Debug *debug = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
			debug->EnableDebugLayer();
		IDXGIFactory4 *factory = nullptr;
		IDXGIAdapter *warp = nullptr;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
			FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
		{
			why = "no WARP device";
			return false;
		}
		g.device->QueryInterface(IID_PPV_ARGS(&g.info));
		D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
		g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
		g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc));
		g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, IID_PPV_ARGS(&g.list));
		g.list->Close();
		g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
		g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		// dgVoodoo's back buffer: R8G8B8A8, typeless, a UNORM view (the stream logs)
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.Width = W;
		rd.Height = H;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		rd.SampleDesc.Count = 1;
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&g.target));
		// (and its depth: only named by the markers)
		D3D12_RESOURCE_DESC dd = rd;
		dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
		dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&g.depth));
		UINT rows = 0;
		UINT64 row_bytes = 0, total = 0;
		g.device->GetCopyableFootprints(&rd, 0, 1, 0, &g.spot, &rows, &row_bytes, &total);
		D3D12_HEAP_PROPERTIES rb = { D3D12_HEAP_TYPE_READBACK }, up = { D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = total;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		g.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.readback));
		g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g.upload));
		D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
		g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtv_heap));
		D3D12_RENDER_TARGET_VIEW_DESC rv = {};
		rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		g.device->CreateRenderTargetView(g.target, &rv, g.rtv_heap->GetCPUDescriptorHandleForHeapStart());
		D3D12_DESCRIPTOR_HEAP_DESC gh = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		D3D12_DESCRIPTOR_HEAP_DESC gs = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 16, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		g.device->CreateDescriptorHeap(&gh, IID_PPV_ARGS(&g.game_heap));
		g.device->CreateDescriptorHeap(&gs, IID_PPV_ARGS(&g.game_samplers));
		// dgVoodoo's own root signature and pipeline stand-ins: a table into its heap, root constants
		D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
		D3D12_ROOT_PARAMETER p[2] = {};
		p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		p[0].Constants = { 0, 0, 8 };
		p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		p[1].DescriptorTable = { 1, &range };
		const D3D12_ROOT_SIGNATURE_DESC rsd = { 2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		ID3DBlob *blob = nullptr, *errors = nullptr, *vs = nullptr, *ps = nullptr;
		D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
		g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.game_root));
		D3DCompile(kGame, std::strlen(kGame), "game", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors);
		D3DCompile(kGame, std::strlen(kGame), "game", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors);
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
		pd.pRootSignature = g.game_root;
		pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
		pd.SampleMask = UINT_MAX;
		pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 1;
		pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		pd.SampleDesc.Count = 1;
		if (FAILED(g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g.game_pso))))
		{
			why = "the stand-in pipeline";
			return false;
		}
		return true;
	}

	// The recorded window drawn as HiFi XI draws it at its markers, onto the background; then dgVoodoo's next draw
	// (a 24 x 24 square at the bottom left in 0x10E0F0) with nothing bound again. The pixels as R, G, B, A.
	bool replay_d12(d12 &g, const std::vector<uint8_t> &bg_bgrx, const u2::frame &f, std::vector<uint8_t> &out, std::string &why, uint32_t &drawn)
	{
		// The background into the target
		uint8_t *cpu = nullptr;
		g.upload->Map(0, nullptr, reinterpret_cast<void **>(&cpu));
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const uint8_t *s = bg_bgrx.data() + (size_t(y) * W + x) * 4;
				uint8_t *d = cpu + g.spot.Offset + size_t(y) * g.spot.Footprint.RowPitch + x * 4;
				d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
			}
		g.upload->Unmap(0, nullptr);
		g.alloc->Reset();
		g.list->Reset(g.alloc, nullptr);
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = g.target;
		b.Transition.Subresource = 0;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		g.list->ResourceBarrier(1, &b);
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = g.target;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.pResource = g.upload;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint = g.spot;
		g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
		g.list->ResourceBarrier(1, &b);

		// dgVoodoo's state as it leaves it: its heaps, root signature and arguments, pipeline, target, viewport, scissor
		grass::list_state st;
		ID3D12DescriptorHeap *const heaps[2] = { g.game_heap, g.game_samplers };
		g.list->SetDescriptorHeaps(2, heaps);
		g.list->SetGraphicsRootSignature(g.game_root);
		const D3D12_GPU_DESCRIPTOR_HANDLE table = g.game_heap->GetGPUDescriptorHandleForHeapStart();
		g.list->SetGraphicsRootDescriptorTable(1, table);
		g.list->SetPipelineState(g.game_pso);
		g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
		g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		const D3D12_VIEWPORT vp = { 0.5f, 0.5f, float(W), float(H), 0.0f, 1.0f };
		const D3D12_RECT sc = { 0, 0, LONG(W), LONG(H) };
		g.list->RSSetViewports(1, &vp);
		g.list->RSSetScissorRects(1, &sc);
		st.rtvs[0] = rtv;
		st.rtv_count = 1;
		st.pipeline = g.game_pso;
		st.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
		st.viewports[0] = vp;
		st.viewport_count = 1;
		st.scissors[0] = sc;
		st.scissor_count = 1;
		st.set_root(g.game_root);
		st.arg(1).kind = grass::root_arg::table;
		st.arg(1).value = table.ptr;
		// The markers as the backend hands them on (src/host/passes: the heaps it saw set, the target bound with the
		// back buffer's depth): the window's begin, then each segment's (drawn: they're marked native here, as the
		// gate would have them)
		hifi::passes::on_heaps(g.list, 2, heaps);
		hifi::passes::world w;
		w.ds = g.depth;
		w.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		w.bound_rt = g.target;
		w.bound_ds = g.depth;
		w.bound_rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
		drawn = 0;
		const uint32_t n = f.segments.load(), seq = f.seq.load() & 15u;
		hifi::passes::dispatch(hifi::slotmark::ui_begin, g.list, st, w, seq);
		const uint64_t failed_before = ui2d_failed();
		for (uint32_t k = 0; k < n; ++k)
		{
			const_cast<u2::segment &>(f.seg[k]).native = true;
			if (!hifi::passes::dispatch(f.seg[k].kind, g.list, st, w, seq) || ui2d_failed() != failed_before)
			{
				why = "segment " + std::to_string(k) + " not drawn";
				g.list->Close();
				return false;
			}
			drawn += f.seg[k].count;
		}
		// dgVoodoo's next draw, nothing set again but its constants
		const float c[8] = { -1.0f, -1.0f + 2.0f * 24.0f / H, -1.0f + 2.0f * 24.0f / W, -1.0f, 0x10 / 255.0f, 0xE0 / 255.0f, 0xF0 / 255.0f, 1.0f };
		g.list->SetGraphicsRoot32BitConstants(0, 8, c, 0);
		g.list->DrawInstanced(4, 1, 0, 0);
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		g.list->ResourceBarrier(1, &b);
		D3D12_TEXTURE_COPY_LOCATION rdst = {}, rsrc = {};
		rdst.pResource = g.readback;
		rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		rdst.PlacedFootprint = g.spot;
		rsrc.pResource = g.target;
		rsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		g.list->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, nullptr);
		std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
		g.list->ResourceBarrier(1, &b);
		if (FAILED(g.list->Close()))
		{
			why = "the list didn't close";
			return false;
		}
		ID3D12CommandList *const lists[] = { g.list };
		g.queue->ExecuteCommandLists(1, lists);
		wait(g);
		const uint8_t *rb = nullptr;
		g.readback->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint8_t **>(&rb)));
		out.resize(size_t(W) * H * 4);
		for (UINT y = 0; y < H; ++y)
			std::memcpy(out.data() + size_t(y) * W * 4, rb + g.spot.Offset + size_t(y) * g.spot.Footprint.RowPitch, W * 4);
		g.readback->Unmap(0, nullptr);
		return true;
	}

	void save_bmp(const std::string &path, const std::vector<uint8_t> &px, bool bgr)
	{
		std::ofstream f(path, std::ios::binary);
		BITMAPFILEHEADER fh = {};
		BITMAPINFOHEADER ih = {};
		ih.biSize = sizeof(ih);
		ih.biWidth = W;
		ih.biHeight = -LONG(H);
		ih.biPlanes = 1;
		ih.biBitCount = 32;
		fh.bfType = 0x4D42;
		fh.bfOffBits = sizeof(fh) + sizeof(ih);
		fh.bfSize = fh.bfOffBits + W * H * 4;
		f.write(reinterpret_cast<const char *>(&fh), sizeof(fh));
		f.write(reinterpret_cast<const char *>(&ih), sizeof(ih));
		std::vector<uint8_t> o(px);
		if (!bgr)
			for (size_t i = 0; i < o.size(); i += 4)
				std::swap(o[i], o[i + 2]);
		f.write(reinterpret_cast<const char *>(o.data()), std::streamsize(o.size()));
	}

	// ---------------------------------------------------------------- one case

	uint32_t g_marks = 0;
	void count_marks(uint32_t, uint32_t)
	{
		g_marks++;
	}

	void run_case(dgv &d, d12 &g, scenario &sc)
	{
		std::printf("-- %s: %zu calls, %u draws, %zu textures\n", sc.name.c_str(), sc.ops.size(), sc.draws, sc.textures.size());
		std::vector<IDirect3DTexture8 *> tex = make_textures(d, sc);
		std::vector<uint8_t> bg, ref, native_bb;
		const auto frame = [&](ud::mode m, std::vector<uint8_t> &before, std::vector<uint8_t> &after) {
			d.dev->BeginScene();
			d.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF000000, 1.0f, 0);
			background(d);
			read_back(d, before);
			ud::set_mode(m);
			ud::window_begin(reinterpret_cast<uintptr_t>(d.dev));
			run_ops(d, sc, tex);
			ud::window_flush(hifi::slotmark::ui_end);
			ud::window_flush(hifi::slotmark::frame_end);
			read_back(d, after);
			d.dev->EndScene();
			d.dev->Present(nullptr, nullptr, nullptr, nullptr);
		};
		const ud::counts c0 = ud::get_counts();
		frame(ud::shadow, bg, ref);
		const ud::counts c1 = ud::get_counts();
		const u2::frame *f = u2::published(c1.seq & 15);
		check(f != nullptr && f->seq.load() == c1.seq, sc.name + ": the window's frame published");
		if (f == nullptr)
			return;
		check(c1.windows == c0.windows + 1 && ud::current() == ud::shadow, sc.name + ": one window opened");
		const uint64_t recorded = c1.recorded - c0.recorded, passed = c1.passed - c0.passed;
		std::printf("     recorded %llu draws, %llu left to the game, %u segments\n", static_cast<unsigned long long>(recorded),
			static_cast<unsigned long long>(passed), f->segments.load());
		std::string reasons;
		for (uint32_t r = 1; r < 16; ++r)
			if (c1.why[r] != c0.why[r])
				reasons += std::string(reasons.empty() ? "" : ", ") + ud::reason_name(r) + " " + std::to_string(c1.why[r] - c0.why[r]);
		if (!reasons.empty())
			std::printf("     left to the game: %s\n", reasons.c_str());
		check(recorded + passed == sc.draws, sc.name + ": every draw recorded or left to the game (" + std::to_string(recorded + passed) + " of " + std::to_string(sc.draws) + ")");
		check(c1.markers - c0.markers == f->segments.load() + 0 && c1.not_marked == c0.not_marked, sc.name + ": a marker for each segment");
		check(*reinterpret_cast<uint32_t *>(d.dev) != 0 && ud::get_counts().frame_draws == recorded, sc.name + ": the device's vtable back");
		// (the device's own vtable back: dgVoodoo's)
		{
			const uint32_t vt = *reinterpret_cast<uint32_t *>(d.dev);
			MEMORY_BASIC_INFORMATION mi = {};
			VirtualQuery(reinterpret_cast<void *>(uintptr_t(vt)), &mi, sizeof(mi));
			check(mi.AllocationBase == d.dll, sc.name + ": the device wears dgVoodoo's own vtable again");
		}

		if (const char *s = std::getenv("UI2D_DUMP"); s != nullptr)
			for (uint32_t i = 0; i < f->draw_count && i < uint32_t(std::atoi(s)); ++i)
			{
				const u2::draw &x = f->draws[i];
				const u2::vertex &v = f->vertices[x.first];
				std::printf("     #%u tex %u blend %u %u/%u/%u ops %06X %06X test %u/%u/%u flat %u filt %u%u%u addr %u%u clip %d,%d,%d,%d v0 %.1f,%.1f c %08X uv %.3f,%.3f n %u\n", i, x.texture,
					x.blend.enable, x.blend.src, x.blend.dst, x.blend.op, x.colour.packed(), x.alpha.packed(), x.alpha_test, x.alpha_func, x.alpha_ref, x.flat,
					x.min_filter, x.mag_filter, x.mip_filter, x.address_u, x.address_v, x.clip[0], x.clip[1], x.clip[2], x.clip[3], v.x, v.y, v.colour, v.u, v.v, x.count);
			}
		std::vector<uint8_t> ours;
		std::string why;
		uint32_t drawn = 0;
		const bool ok = replay_d12(g, bg, *f, ours, why, drawn);
		check(ok, sc.name + ": drawn on Direct3D 12" + (ok ? "" : ": " + why));
		std::string first;
		const uint32_t messages = debug_messages(g.info, first);
		check(messages == 0, sc.name + ": the debug layer: nothing (" + std::to_string(messages) + (messages != 0 ? ": " + first : std::string()) + ")");
		if (!ok)
			return;
		// dgVoodoo's next draw came out as it meant
		bool square = true;
		for (UINT y = H - 20; y < H - 4 && square; ++y)
			for (UINT x = 4; x < 20 && square; ++x)
			{
				const uint8_t *p = ours.data() + (size_t(y) * W + x) * 4;
				square = p[0] == 0x10 && p[1] == 0xE0 && p[2] == 0xF0;
			}
		check(square, sc.name + ": dgVoodoo's own draw after ours, with its state put back");
		// The pixels against dgVoodoo's (outside that square)
		uint64_t same = 0, close = 0, near_ = 0, total = 0, changed = 0;
		uint32_t worst = 0, wx = 0, wy = 0;
		std::vector<uint8_t> diff(size_t(W) * H * 4, 0);
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				if (x < 24 && y >= H - 24)
					continue;
				const uint8_t *a = ref.data() + (size_t(y) * W + x) * 4, *b = ours.data() + (size_t(y) * W + x) * 4, *z = bg.data() + (size_t(y) * W + x) * 4;
				uint32_t m = 0;
				for (int k = 0; k < 3; ++k)
					m = std::max<uint32_t>(m, uint32_t(std::abs(int(a[2 - k]) - int(b[k]))));
				changed += a[0] != z[0] || a[1] != z[1] || a[2] != z[2];
				total++;
				same += m == 0;
				close += m <= 1;
				near_ += m <= 4;
				if (m > worst)
					worst = m, wx = x, wy = y;
				uint8_t *dd = diff.data() + (size_t(y) * W + x) * 4;
				dd[0] = dd[1] = dd[2] = uint8_t(std::min<uint32_t>(255, m * 32));
			}
		char line[300];
		std::snprintf(line, sizeof(line), "%s: pixels as dgVoodoo's: %.4f%% the same, %.4f%% within 1, %.4f%% within 4; worst %u at %u,%u (%llu pixels the 2D changed)",
			sc.name.c_str(), 100.0 * same / total, 100.0 * close / total, 100.0 * near_ / total, worst, wx, wy, static_cast<unsigned long long>(changed));
		// (the game's own 1:1 sprites come out the same; what differs is filtered texels, by a few steps: dgVoodoo samples
		// about 0.006 texel off a texel corner at 2:1, so text from the 2048 x 4096 font and scaled quads of noise-like
		// pictures differ in their last bits, and an alpha-tested edge pixel now and then. A wrong blend, op, filter or
		// address mode differs over whole quads.)
		const bool stress = sc.name == "coverage";
		check(sc.name == "probe" || (changed > 10000 && near_ * 1000 >= total * (stress ? 990 : 993) && close * 100 >= total * 95), line);
		if (const char *s = std::getenv("UI2D_SAVE"); s != nullptr && *s == '1')
		{
			save_bmp(sc.name + "-dgv.bmp", ref, true);
			save_bmp(sc.name + "-ours.bmp", ours, false);
			save_bmp(sc.name + "-diff.bmp", diff, true);
		}

		// Native: the same calls, the recorded draws left out of dgVoodoo's stream
		const uint32_t shadow_seq = c1.seq;
		std::vector<u2::draw> shadow_draws(f->draws, f->draws + f->draw_count);
		std::vector<u2::vertex> shadow_vertices(f->vertices, f->vertices + f->vertex_count);
		u2::render::pipeline(u2::blend_key()); // (the target is known now: as the gate wants it)
		std::vector<uint8_t> bg2;
		frame(ud::native, bg2, native_bb);
		const ud::counts c2 = ud::get_counts();
		const u2::frame *f2 = u2::published(c2.seq & 15);
		check(f2 != nullptr && c2.seq == shadow_seq + 1 && c2.native - c1.native == recorded, sc.name + ": native: every recorded draw left out");
		if (f2 == nullptr)
			return;
		bool same_frame = f2->draw_count == shadow_draws.size() && f2->vertex_count == shadow_vertices.size() &&
			std::memcmp(f2->vertices, shadow_vertices.data(), shadow_vertices.size() * sizeof(u2::vertex)) == 0;
		uint32_t differs = ~0u;
		for (uint32_t i = 0; same_frame && i < f2->draw_count; ++i)
			if (!(same_frame = f2->draws[i].same_state(shadow_draws[i]) && f2->draws[i].first == shadow_draws[i].first && f2->draws[i].count == shadow_draws[i].count))
				differs = i;
		check(same_frame, sc.name + ": native: the frame recorded as in shadow" +
			(differs != ~0u ? " (draw " + std::to_string(differs) + ": texture " + std::to_string(f2->draws[differs].texture) + " was " + std::to_string(shadow_draws[differs].texture) + ")"
			: f2->draw_count != shadow_draws.size() ? " (" + std::to_string(f2->draw_count) + " draws, was " + std::to_string(shadow_draws.size()) + ")" : std::string()));
		bool natives = true;
		for (uint32_t k = 0; k < f2->segments.load(); ++k)
			natives = natives && f2->seg[k].native;
		check(natives, sc.name + ": native: every segment marked native");
		// What dgVoodoo drew: the background and only what the game kept
		if (passed == 0)
			check(native_bb == bg2, sc.name + ": native: dgVoodoo drew nothing of it");
		else
		{
			uint64_t kept = 0;
			for (size_t i = 0; i < bg2.size(); i += 4)
				kept += bg2[i] != native_bb[i] || bg2[i + 1] != native_bb[i + 1] || bg2[i + 2] != native_bb[i + 2];
			check(kept < changed, sc.name + ": native: dgVoodoo drew only what was left to the game (" + std::to_string(kept) + " pixels)");
		}
		ud::set_mode(ud::off);
		for (IDirect3DTexture8 *t : tex)
			if (t != nullptr)
				t->Release();
		ud::drop_textures();
	}
}

// (game.cpp's settings: the plugin's, not used here)
extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main(int argc, char **argv)
{
	SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *e) -> LONG {
		std::printf("crash: code %08lX at %p\n", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
		std::fflush(stdout);
		return EXCEPTION_EXECUTE_HANDLER;
	});
	dgv d;
	std::string why;
	if (!open_dgv(d, why))
	{
		std::printf("SKIP  dgVoodoo: %s\n", why.c_str());
		return 0;
	}
	d12 g;
	if (!open_d12(g, why))
	{
		check(false, why);
		return 1;
	}
	// (dgVoodoo's own filtering setting, as the plugin reads it from the game's dgVoodoo.conf)
	{
		std::ifstream conf("dgVoodoo.conf", std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(conf)), std::istreambuf_iterator<char>());
		const u2::render::filtering::setting f = u2::render::filtering::read(text);
		u2::render::set_filtering(f.forced, f.anisotropy, f.keep_point);
		std::printf("dgVoodoo's filtering: %u (anisotropy %u), point-sampled textures kept: %s\n", f.forced, f.anisotropy, f.keep_point ? "yes" : "no");
	}
	const bool made = u2::render::make(g.device, why);
	check(made, "the 2D renderer made on WARP" + (why.empty() ? "" : ": " + why));

	std::vector<scenario> cases;
	std::vector<std::string> captures;
	bool cover = argc < 2;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "coverage") == 0)
			cover = true;
		else
			captures.push_back(argv[i]);
	if (const char *pr = std::getenv("UI2D_PROBE"))
	{
		uint32_t mag = 1, min = 1;
		float scale = 1.0f;
		std::sscanf(pr, "%u,%u,%f", &mag, &min, &scale);
		cases.push_back(probe(mag, min, scale));
		cover = false;
		captures.clear();
	}
	if (cover)
		cases.push_back(coverage());
	if (argc < 2)
		for (const char *n : { "menu", "day_npcs", "night", "spell", "water" })
			captures.push_back(std::string("C:\\Games\\PhoenixXI\\d3dlog\\") + n + ".txt");
	for (const std::string &p : captures)
	{
		scenario sc;
		const size_t slash = p.find_last_of("\\/");
		sc.name = p.substr(slash == std::string::npos ? 0 : slash + 1);
		sc.name = sc.name.substr(0, sc.name.find('.'));
		sc.game_like = true;
		if (parse_capture(p, sc, why))
			cases.push_back(std::move(sc));
		else
			std::printf("SKIP  %s\n", why.c_str());
	}
	for (scenario &sc : cases)
		run_case(d, g, sc);

	std::printf("\n%d passed, %d failed\n", passes, failures);
	u2::render::release();
	return failures == 0 ? 0 : 1;
}
