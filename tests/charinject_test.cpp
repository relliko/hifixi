/*
 * The native character renderer on a list recorded as dgVoodoo records its own (src/chars/charinject.cpp,
 * chardraw.cpp), on WARP with the debug layer, as tests/zoneinject_test.cpp does for the zone: dgVoodoo's world target
 * and depth (typeless resources, UNORM and D24S8 views), its own root signature, heap, pipeline and vertex buffer bound
 * once; the depth plane in DEPTH_READ and the stencil plane in DEPTH_WRITE at the markers, as dgVoodoo leaves them.
 *
 * The Mithra NPC (ROM2/0/52.DAT) as the cache has it from the DAT, posed by research/ffxi-re/tools/chars.py (idl0, its
 * palette from tests/chars_pose.py), three times side by side in one frame: in the middle drawn by HiFi XI at the
 * characters' end marker (opaque), on the right see-through (colour alpha 0x40) at its own marker, on the left one the
 * game drew (not kNative: HiFi XI leaves it alone). Then dgVoodoo's next draw with nothing bound again.
 *   - the middle one in the target and its depth, inside its skinned vertices' projected box and filling it;
 *   - the see-through one blended about halfway between the sky and what the opaque one shows, its depth written;
 *   - the left one not drawn;
 *   - dgVoodoo's own draw as it meant it; the debug layer says nothing; the counts;
 *   - the zone's lamps (set_lamps): a lamp 1.5 yalms in front of the middle one lights it (against the sun alone), and
 *     its light then stands in for the game's own lamp slot (the lamp turned directional, left out): about as bright as
 *     the game's own, not both added; without the zone's lamps the game's slot as it is;
 *   - the Mithra let go between a frame's recording and its run, a new texture uploaded meanwhile: it gets a pair of
 *     descriptors the list doesn't read (those wait for the GPU: retire::then), the picture is unchanged, the debug layer
 *     says nothing (HIFIXI_GBV=1: with GPU-based validation).
 *   - the texture pack (docs/texture-pack/characters.md; the streamer on this device, src/chars/charpack driven as the
 *     module drives it, made-up packs under build\test\charinject): the Mithra again from its original file's bytes,
 *     each texture keyed as its DAT chunk. A flat pack (level 0 green, the rest blue) with one key left out and one
 *     no_pack: the hits ready through the second pair, BC2 with the record's levels down to the game's last; the two
 *     spent; every changed pixel the pack's, the two's groups unchanged; four times as far a lower level sampled; the
 *     casters' map byte for byte the game's. The FSR bias (-1): more of level 0 on the pack's textures, the game's own
 *     (compare) and the casters' map as without it. Evicted under the streamer's pressure while a frame is in flight: that
 *     frame as recorded, the next the game's, the pack's again once the pressure's been gone. Off between a frame's recording and its run, on again, 50 times (every
 *     fifth with the copies held on the GPU and let go while held): each frame as recorded, no pair twice, the debug
 *     layer silent. A pack of the Mithra's own pictures 4x: the cut-outs' coverage within 2% of the game's near and four
 *     times as far, the colours within 2 of brightness. The real pack (read only; skipped without): the original bytes'
 *     keys in it; the overlay's copies' (what the game holds while hifi-textures is listed) none, and the line says why.
 *     Per pixel too: the shine groups' highlight (by the texture's alpha) as bright with the pack as with the game's.
 *     CHARS_DETAIL=1: the pack's pairs' second view of each detail map too. M9: a pack with heights (a third colour and
 *     height, a third a height alone, a third a colour alone): off none asked or drawn; on each asked, its relief scale
 *     in the pair, per vertex unchanged, per pixel changed with the light within 3%, compare none, off again byte for
 *     byte, on again the same, clear_all lets every height go.
 *   ZONE_GPU=1 runs it on the first GPU in place of WARP. For a look, not the checks: CHARS_DATS="ROM\a\b.DAT;..." draws
 *   those DATs read as one (a race's base file, then its gear) in place of the Mithra (the pose: tests/chars_pose.py on
 *   the base file), and CHARS_PICTURES=<folder> writes game, pack and overlay BMPs there (whole and close up: the game's
 *   textures, the real pack's, the hifi-textures overlay's copies). M9's relief A/B: HIFIXI_PACK=<folder> another pack
 *   for "the real pack", CHARS_PACK_HEIGHT=0/1 its heights off or on, CHARS_SUN="x y z" the sun toward there (y down).
 *
 *   build\charinject_test.exe <pose.bin> [out.bmp]
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include "../src/chars/cache.hpp"
#include "../src/chars/charframe.hpp"
#include "../src/chars/charinject.hpp"
#include "../src/chars/charpack.hpp"
#include "../src/chars/os2.hpp"
#include "../src/chars/tex.hpp"
#include "../src/host/retire.hpp"
#include "../src/pack/bc2.hpp"
#include "../src/pack/pack.hpp"
#include "../src/pack/pack_write.hpp"
#include "../src/pack/stream.hpp"
#include "../src/pack/validate.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace
{
	namespace ch = hifi::chars;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	constexpr UINT W = 800, H = 450;
	const float kSky[4] = { 0.55f, 0.65f, 0.78f, 1.0f };
	const float kShift = 1.1f; // (world x between the three)

	const char *kGame = R"(
Texture2D t : register(t0);
SamplerState s : register(s0);
struct v { float2 p : POSITION; float2 uv : TEXCOORD; };
struct o { float4 p : SV_Position; float2 uv : TEXCOORD; };
o vs(v i) { o r; r.p = float4(i.p, 0.0, 1.0); r.uv = i.uv; return r; }
float4 ps(o i) : SV_Target { return t.Sample(s, i.uv); }
)";

	ID3DBlob *compile(const char *src, const char *entry, const char *target)
	{
		using compile_fn = decltype(&D3DCompile);
		static const auto c = reinterpret_cast<compile_fn>(GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile"));
		ID3DBlob *code = nullptr, *errors = nullptr;
		if (c == nullptr || FAILED(c(src, std::strlen(src), "game", nullptr, nullptr, entry, target, 0, 0, &code, &errors)))
			return nullptr;
		return code;
	}

	ID3D12Resource *g_marked = nullptr; // (the depth the marker named: the only one followed, as the renderer's)
	// The depth's planes as the test moves them (a new depth: both as the first barrier found them), handed to the
	// renderer as our d3d8.dll hands them at each mark
	void follow_planes(ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		static ID3D12Resource *of = nullptr;
		static D3D12_RESOURCE_STATES planes[2] = {};
		if (r != of)
		{
			of = r;
			planes[0] = planes[1] = from;
		}
		if (sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
			planes[0] = planes[1] = to;
		else if (sub < 2)
			planes[sub] = to;
		ch::inject::set_depth_planes(r, planes);
	}

	void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Transition.pResource = r;
		b.Transition.Subresource = sub;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
		// (the marked depth's planes handed over as our d3d8.dll hands them at each mark)
		if (r == g_marked)
			follow_planes(r, sub, from, to);
	}

	struct mat4
	{
		float m[16];
	};
	mat4 mul(const mat4 &a, const mat4 &b)
	{
		mat4 o = {};
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				for (int k = 0; k < 4; ++k)
					o.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
		return o;
	}
	// The game's camera: y down, looking from eye to at
	mat4 camera(const float eye[3], const float at[3])
	{
		float z[3] = { eye[0] - at[0], eye[1] - at[1], eye[2] - at[2] }, up[3] = { 0, -1, 0 }, x[3], y[3];
		auto norm = [](float *v) {
			const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			for (int k = 0; k < 3; ++k)
				v[k] /= l;
		};
		auto cross = [](const float *a, const float *b, float *o) {
			o[0] = a[1] * b[2] - a[2] * b[1];
			o[1] = a[2] * b[0] - a[0] * b[2];
			o[2] = a[0] * b[1] - a[1] * b[0];
		};
		auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
		norm(z);
		cross(up, z, x);
		norm(x);
		cross(z, x, y);
		const mat4 v = { { x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1 } };
		const float ys = 1.0f / std::tan(0.4f), xs = ys / (float(W) / H), zn = 0.1f, zf = 500.0f;
		const mat4 p = { { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zn - zf), -1, 0, 0, zn * zf / (zn - zf), 0 } };
		return mul(v, p);
	}

	uint32_t debug_messages(ID3D12InfoQueue *q, std::string &first)
	{
		if (q == nullptr)
			return 0;
		const UINT64 n = q->GetNumStoredMessages();
		uint32_t errors = 0;
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T size = 0;
			q->GetMessage(i, nullptr, &size);
			std::vector<uint8_t> buf(size);
			D3D12_MESSAGE *m = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
			q->GetMessage(i, m, &size);
			if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && m->ID != D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE &&
				m->ID != D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE)
			{
				if (errors++ == 0)
					first = m->pDescription;
			}
		}
		q->ClearStoredMessages();
		return errors;
	}

	// ------------------------------------------------------------------------------------------ the Mithra from its DAT
	struct dat_chunk
	{
		uint32_t kind;
		size_t folder; // (the index of the folder chunk it's in)
		std::vector<uint8_t> data;
	};
	std::vector<dat_chunk> read_dat_at(const std::string &path)
	{
		std::ifstream f(path, std::ios::binary);
		std::vector<uint8_t> dat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::vector<dat_chunk> out;
		std::vector<size_t> folders;
		size_t off = 0;
		while (off + 16 <= dat.size())
		{
			uint32_t w;
			std::memcpy(&w, dat.data() + off + 4, 4);
			const size_t size = size_t((w >> 7) & 0x7FFFF) * 16;
			if (size < 16 || off + size > dat.size())
				break;
			dat_chunk c;
			c.kind = w & 0x7F;
			c.folder = folders.empty() ? 0 : folders.back();
			c.data.assign(dat.begin() + off + 16, dat.begin() + off + size);
			if (c.kind == 1)
				folders.push_back(out.size());
			else if (c.kind == 0 && !folders.empty())
				folders.pop_back();
			out.push_back(std::move(c));
			off += size;
		}
		return out;
	}
	std::vector<dat_chunk> read_dat(const char *rel)
	{
		return read_dat_at(std::string("C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\") + rel);
	}

	// CHARS_DATS (for pictures, not the checks: they're the Mithra's): DATs (';' between, relative to the game folder) read
	// one after another as one, each one's folders shifted by where it starts (a race's base file for nothing but its
	// skeleton, then the gear: face, head, body, hands, legs, feet, weapons); in_overlay reads each from the hifi-textures
	// overlay instead where it has a copy (what the game holds while it's listed)
	std::vector<dat_chunk> read_dats(const std::string &list, bool in_overlay)
	{
		std::vector<dat_chunk> out;
		size_t at = 0;
		while (at < list.size())
		{
			const size_t end = std::min(list.find(';', at), list.size());
			const std::string rel = list.substr(at, end - at);
			at = end + 1;
			if (rel.empty())
				continue;
			std::vector<dat_chunk> one;
			if (in_overlay)
				one = read_dat_at("C:\\Games\\PhoenixXI\\polplugins\\DATs\\hifi-textures\\" + rel);
			if (one.empty() && in_overlay)
				std::printf("      (no overlay copy of %s: the original's)\n", rel.c_str());
			if (one.empty())
				one = read_dat(rel.c_str());
			const size_t base = out.size();
			for (dat_chunk &c : one)
			{
				c.folder += base;
				out.push_back(std::move(c));
			}
		}
		return out;
	}

	// A frame read back (RGBA, W x H) as a 32-bit top-down BMP
	void write_bmp(const std::string &path, const std::vector<uint8_t> &px)
	{
		FILE *fo = nullptr;
		if (fopen_s(&fo, path.c_str(), "wb") != 0 || fo == nullptr)
			return;
		uint8_t head[54] = { 'B', 'M' };
		auto put = [&](int at, uint32_t v) { std::memcpy(head + at, &v, 4); };
		put(2, 54 + W * H * 4);
		put(10, 54);
		put(14, 40);
		put(18, W);
		put(22, uint32_t(-int32_t(H)));
		head[26] = 1;
		head[28] = 32;
		fwrite(head, 1, 54, fo);
		for (size_t i = 0; i < size_t(W) * H && i * 4 + 3 < px.size(); ++i)
		{
			const uint8_t bgra[4] = { px[i * 4 + 2], px[i * 4 + 1], px[i * 4 + 0], 255 };
			fwrite(bgra, 1, 4, fo);
		}
		fclose(fo);
	}

	struct loaded
	{
		std::vector<uint32_t> meshes;          // cache ids
		std::vector<ch::os2::mesh> decoded;    // (for the box)
		uint32_t textures = 0;
	};
	// Each mesh into the cache with the textures its commands name (the first of that name in its folder, from the
	// folder's start, as 03AF10 finds it)
	loaded load_into_cache(const std::vector<dat_chunk> &dat)
	{
		loaded L;
		std::map<size_t, uint32_t> texture_of_chunk;
		for (size_t i = 0; i < dat.size(); ++i)
		{
			if (dat[i].kind != 0x2A)
				continue;
			ch::os2::sections s;
			ch::os2::sections_of_chunk(dat[i].data.data(), dat[i].data.size(), s);
			std::vector<ch::os2::texture_ref> refs;
			ch::os2::texture_commands(dat[i].data.data(), dat[i].data.size(), s, refs);
			std::vector<uint32_t> ids;
			for (const auto &r : refs)
			{
				uint32_t id = ch::cache::kNone;
				int depth = 0;
				for (size_t j = dat[i].folder + 1; j < dat.size(); ++j)
				{
					if (dat[j].kind == 1)
						depth++;
					if (dat[j].kind == 0 && --depth < 0)
						break;
					if (dat[j].kind == 0x20 && dat[j].data.size() > 17 && std::memcmp(dat[j].data.data() + 1, r.name, 16) == 0)
					{
						auto known = texture_of_chunk.find(j);
						if (known == texture_of_chunk.end())
						{
							ch::cache::key k;
							k.resource = 0x70000000u + uint32_t(j) * 16;
							std::vector<uint8_t> bytes = dat[j].data;
							known = texture_of_chunk.emplace(j, ch::cache::add_texture(k, std::move(bytes))).first;
							L.textures++;
						}
						id = known->second;
						break;
					}
				}
				ids.push_back(id);
			}
			ch::cache::key k;
			k.resource = 0x60000000u + uint32_t(i) * 16;
			std::vector<uint8_t> bytes = dat[i].data;
			L.meshes.push_back(ch::cache::add_mesh(k, std::move(bytes), 0, std::move(ids)));
			ch::os2::mesh m;
			std::string why;
			ch::os2::decode(dat[i].data.data(), dat[i].data.size(), s, m, why);
			L.decoded.push_back(std::move(m));
		}
		return L;
	}

	// The skinned vertices' world box (both halves), shifted by dx
	void box_of(const loaded &L, const std::vector<ch::bone> &pal, float lo[3], float hi[3])
	{
		for (int k = 0; k < 3; ++k)
		{
			lo[k] = 1e30f;
			hi[k] = -1e30f;
		}
		auto apply = [&](uint32_t b, const float p[3], float w, float out[3]) {
			const float *m = pal[std::min<size_t>(b, pal.size() - 1)].m;
			for (int c = 0; c < 3; ++c)
				out[c] += p[0] * m[c] + p[1] * m[3 + c] + p[2] * m[6 + c] + w * m[9 + c];
		};
		for (const auto &m : L.decoded)
			for (int half = 0; half < (m.mirrored ? 2 : 1); ++half)
				for (const auto &v : m.vertices)
				{
					float p[3] = { 0, 0, 0 };
					float p0[3] = { v.p0[0], v.p0[1], v.p0[2] }, p1[3] = { v.p1[0], v.p1[1], v.p1[2] };
					if (half == 1)
					{
						if (v.axis[0] != 0)
							p0[v.axis[0] - 1] = -p0[v.axis[0] - 1];
						if (v.axis[1] != 0)
							p1[v.axis[1] - 1] = -p1[v.axis[1] - 1];
					}
					apply(half ? v.mirror_bone[0] : v.bone[0], p0, v.w0, p);
					apply(half ? v.mirror_bone[1] : v.bone[1], p1, v.w1, p);
					for (int k = 0; k < 3; ++k)
					{
						lo[k] = std::min(lo[k], p[k]);
						hi[k] = std::max(hi[k], p[k]);
					}
				}
	}

	// A world box's screen rectangle (all 8 corners projected)
	struct rect
	{
		float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
		bool inside(float x, float y, float margin) const { return x >= x0 - margin && x <= x1 + margin && y >= y0 - margin && y <= y1 + margin; }
	};
	rect project(const mat4 &vp, const float lo[3], const float hi[3], float dx)
	{
		rect r;
		for (int c = 0; c < 8; ++c)
		{
			const float p[4] = { ((c & 1) ? hi[0] : lo[0]) + dx, (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2], 1.0f };
			float o[4] = {};
			for (int j = 0; j < 4; ++j)
				for (int k = 0; k < 4; ++k)
					o[j] += p[k] * vp.m[k * 4 + j];
			const float sx = (o[0] / o[3] * 0.5f + 0.5f) * W, sy = (0.5f - o[1] / o[3] * 0.5f) * H;
			r.x0 = std::min(r.x0, sx);
			r.x1 = std::max(r.x1, sx);
			r.y0 = std::min(r.y0, sy);
			r.y1 = std::max(r.y1, sy);
		}
		return r;
	}

	// ------------------------------------------------------------------------------------------ the texture pack
	void remove_tree(const std::wstring &folder)
	{
		WIN32_FIND_DATAW f;
		HANDLE h = FindFirstFileW((folder + L"*").c_str(), &f);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
				if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
					DeleteFileW((folder + f.cFileName).c_str());
			while (FindNextFileW(h, &f));
			FindClose(h);
		}
		RemoveDirectoryW(folder.substr(0, folder.size() - 1).c_str());
	}

	// A made-up pack of the Mithra's textures (each key once, with its chunk's bytes): left_out not in it, no_pack flagged
	// so (the game's drawn). flat: level 0 green and every other level blue, alpha 0x88 (passing the alpha test
	// everywhere); else the game's own picture 4x (each texel 4 x 4), its mips by bc2::full_chain (a cut-out's keeping
	// level 0's coverage, as the converter makes them)
	// A BC4 level (M9's heights) of stripes along u: block columns 0 and 255 in turn (4 texels each), down every row
	std::vector<uint8_t> height_stripes(uint32_t w, uint32_t h)
	{
		const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
		std::vector<uint8_t> b(size_t(bw) * bh * 8, 0);
		for (uint32_t y = 0; y < bh; ++y)
			for (uint32_t x = 0; x < bw; ++x)
			{
				uint8_t *p = b.data() + (size_t(y) * bw + x) * 8;
				p[0] = p[1] = (x & 1) != 0 ? 255 : 0; // (r0 == r1: every code 0 is r0)
			}
		return b;
	}

	// kinds (M9, not nullptr): heights too, each texture in turn (sides multiples of 4; else a colour alone) 0 a colour
	// and a height, 1 a height alone (no colour record: the game's colour drawn with the pack's relief), 2 a colour alone;
	// the heights stripes 2x the game's size (a model's half), span kHeightSpan; *kinds says which each texture got
	constexpr float kHeightSpan = 0.25f;
	bool make_mithra_pack(const std::wstring &folder, const std::vector<std::pair<hifi::pack::key, const std::vector<uint8_t> *>> &textures,
		const hifi::pack::key &left_out, const hifi::pack::key &no_pack, bool flat, std::string &why, std::vector<int> *kinds = nullptr)
	{
		namespace pk = hifi::pack;
		remove_tree(folder);
		pk::writer w;
		pk::writer_options o;
		o.folder = folder;
		o.source = "charinject_test";
		if (!w.open(o, why))
			return false;
		if (kinds != nullptr)
			kinds->clear();
		uint32_t turn = 0;
		for (const auto &[k, bytes] : textures)
		{
			int kind = 2;
			if (kinds != nullptr && k.w % 4 == 0 && k.h % 4 == 0)
				kind = int(turn++ % 3);
			if (kinds != nullptr)
				kinds->push_back(kind);
			if (k == left_out)
				continue;
			pk::entry e;
			e.hash = k.hash;
			e.bytes = k.bytes;
			e.w = k.w;
			e.h = k.h;
			e.orig_format = (*bytes)[0];
			e.source_kind = pk::kSourceModel;
			std::memcpy(e.name, bytes->data() + 1, 16);
			if (k == no_pack)
				e.flags = pk::kNoPack;
			pk::map_in m;
			m.format = pk::kBC2;
			m.w = 4 * k.w;
			m.h = 4 * k.h;
			if (flat)
			{
				for (uint32_t l = 0, n = pk::bc2::chain_levels(m.w, m.h); l < n; ++l)
				{
					const uint32_t lw = m.w >> l, lh = m.h >> l;
					const uint8_t texel[4] = { 0, uint8_t(l == 0 ? 255 : 0), uint8_t(l == 0 ? 0 : 255), 0x88 };
					std::vector<uint8_t> px(size_t(lw) * lh * 4);
					for (size_t i = 0; i < px.size(); i += 4)
						std::memcpy(px.data() + i, texel, 4);
					m.levels.push_back(pk::bc2::encode_bc2(px.data(), lw, lh));
				}
			}
			else
			{
				ch::tex::texture t;
				std::string bad;
				if (!ch::tex::decode(bytes->data(), bytes->size(), t, bad) || t.w != k.w || t.h != k.h)
				{
					why = "a texture couldn't be decoded: " + bad;
					return false;
				}
				std::vector<uint8_t> px(size_t(m.w) * m.h * 4);
				bool cut = false;
				for (uint32_t y = 0; y < m.h; ++y)
					for (uint32_t x = 0; x < m.w; ++x)
					{
						const uint8_t *s = t.levels[0].rgba.data() + (size_t(y / 4) * t.w + x / 4) * 4;
						std::memcpy(px.data() + (size_t(y) * m.w + x) * 4, s, 4);
						cut = cut || s[3] <= 0x22;
					}
				e.alpha_kind = cut ? pk::kAlphaCut : pk::kAlphaNone;
				const std::vector<uint8_t> top = pk::bc2::encode_bc2(px.data(), m.w, m.h);
				m.levels = pk::bc2::full_chain(top.data(), m.w, m.h, e.alpha_kind, pk::bc2::kCharsCutThreshold);
			}
			std::vector<pk::map_in> maps;
			if (kind != 1)
				maps.push_back(std::move(m));
			if (kind != 2)
			{
				pk::map_in hm;
				hm.kind = pk::kHeight;
				hm.format = pk::kBC4;
				hm.w = 2 * k.w;
				hm.h = 2 * k.h;
				std::memcpy(&hm.param, &kHeightSpan, 4);
				for (uint32_t l = 0, n = pk::full_levels(hm.w, hm.h); l < n; ++l)
					hm.levels.push_back(height_stripes(std::max(1u, hm.w >> l), std::max(1u, hm.h >> l)));
				maps.push_back(std::move(hm));
			}
			if (w.add(e, maps, why) != pk::add_result::ok)
				return false;
		}
		const bool ok = w.commit(why);
		w.close();
		return ok;
	}
}

// A crash says where (code, address, module and offset) before the process goes, and what the debug layer said
ID3D12InfoQueue *g_info = nullptr;
LONG WINAPI report_crash(EXCEPTION_POINTERS *e)
{
	if (g_info != nullptr)
		for (UINT64 i = 0, n = g_info->GetNumStoredMessages(); i < n; ++i)
		{
			SIZE_T size = 0;
			g_info->GetMessage(i, nullptr, &size);
			std::vector<uint8_t> buf(size);
			D3D12_MESSAGE *m = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
			g_info->GetMessage(i, m, &size);
			if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
				std::printf("DEBUG LAYER #%d: %s\n", int(m->ID), m->pDescription);
		}
	const void *at = e->ExceptionRecord->ExceptionAddress;
	HMODULE m = nullptr;
	char name[MAX_PATH] = "?";
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(at), &m))
		GetModuleFileNameA(m, name, sizeof(name));
	std::printf("CRASH: exception %08lX at %p (%s +%lX)\n", e->ExceptionRecord->ExceptionCode, at, name,
		static_cast<unsigned long>(reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(m)));
	return EXCEPTION_EXECUTE_HANDLER;
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }
namespace hifi::backend
{
	void log(int, const char *) {} // (src/chars/chartest.cpp's lines; the backend isn't linked here)
}

int main(int argc, char **argv)
{
	namespace inj = ch::inject;
	std::setvbuf(stdout, nullptr, _IONBF, 0); // (each line out as it's written: a crash keeps what came before it)
	SetUnhandledExceptionFilter(report_crash);
	if (argc < 2)
	{
		std::printf("usage: charinject_test <pose.bin> [out.bmp]\n");
		return 2;
	}
	// The pose (chars.py's palette: 4x4 row vectors, translation in the last row)
	std::vector<ch::bone> pose;
	{
		std::ifstream f(argv[1], std::ios::binary);
		uint32_t n = 0;
		f.read(reinterpret_cast<char *>(&n), 4);
		for (uint32_t i = 0; i < n && f; ++i)
		{
			float m[16];
			f.read(reinterpret_cast<char *>(m), 64);
			ch::bone b;
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 3; ++c)
					b.m[r * 3 + c] = m[r * 4 + c];
			pose.push_back(b);
		}
	}
	if (getenv("CHARS_GROUPS") != nullptr)
	{
		const std::vector<dat_chunk> d0 = read_dat("ROM2\\0\\52.DAT");
		for (const auto &c : d0)
			if (c.kind == 0x2A)
			{
				ch::os2::sections s0;
				ch::os2::sections_of_chunk(c.data.data(), c.data.size(), s0);
				ch::os2::mesh m0;
				std::string w0;
				ch::os2::decode(c.data.data(), c.data.size(), s0, m0, w0);
				uint32_t mg = 0;
				for (const auto &g : m0.groups)
					mg += g.mirrored;
				std::printf("      mesh: mirrored %d, %zu groups (%u mirrored), %zu corners\n", int(m0.mirrored), m0.groups.size(), mg, m0.corners.size());
			}
	}
	check(pose.size() == 108, "the pose: " + std::to_string(pose.size()) + " bones (chars.py, idl0)");

	ID3D12Debug *debug = nullptr;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
	{
		debug->EnableDebugLayer();
		// (HIFIXI_GBV=1: GPU-based validation as well, as the add-on's debug layer takes it: what the shaders read, the
		// states resources are really in)
		ID3D12Debug1 *debug1 = nullptr;
		if (GetEnvironmentVariableA("HIFIXI_GBV", nullptr, 0) != 0 && SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))))
		{
			debug1->SetEnableGPUBasedValidation(TRUE);
			debug1->Release();
			std::printf("      (GPU-based validation on)\n");
		}
	}
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	ID3D12Device *device = nullptr;
	// (ZONE_GPU=1: the first GPU in place of WARP, as zoneinject_test has it: the pack's BC2 levels and the FSR bias as a
	// real card samples them)
	const bool on_gpu = GetEnvironmentVariableA("ZONE_GPU", nullptr, 0) != 0;
	if (FAILED(D3D12CreateDevice(on_gpu ? nullptr : warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
	{
		check(false, on_gpu ? "a device on the GPU" : "a WARP device");
		return 1;
	}
	if (on_gpu)
		std::printf("      (on the GPU)\n");
	ID3D12InfoQueue *info = nullptr;
	device->QueryInterface(IID_PPV_ARGS(&info));
	g_info = info;
	ID3D12CommandQueue *direct = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&direct));

	// The Mithra into the cache, on the GPU
	const char *dats = getenv("CHARS_DATS");
	const std::vector<dat_chunk> dat = dats != nullptr ? read_dats(dats, false) : read_dat("ROM2\\0\\52.DAT");
	if (dats != nullptr)
		std::printf("      CHARS_DATS: %s (%zu chunks; the checks are the Mithra's, the pictures this one's)\n", dats, dat.size());
	ch::cache::set_detail(getenv("CHARS_DETAIL") != nullptr); // (the detail maps made only with the option on, as the plugin has it)
	const loaded L = load_into_cache(dat);
	ch::cache::frame_tick(device, 1.0);
	bool settled = ch::cache::settle(60000);
	for (int i = 0; i < 400 && ch::cache::get_counts().meshes_ready < L.meshes.size(); ++i)
		Sleep(50);
	settled = settled && ch::cache::settle(60000);
	bool all_ready = true;
	for (uint32_t id : L.meshes)
		all_ready = all_ready && ch::cache::mesh_ready(id);
	check(settled && all_ready, "the Mithra's " + std::to_string(L.meshes.size()) + " meshes and " + std::to_string(L.textures) + " textures on the GPU: " +
		ch::cache::status()[0]);
	std::string why;
	check(inj::prepare(device, why), "the renderer made" + (why.empty() ? "" : ": " + why));
	inj::set_options(getenv("CHARS_PERPIXEL") != nullptr, getenv("CHARS_DETAIL") != nullptr); // (the per-pixel option: CHARS_PERPIXEL=1; the detail maps: CHARS_DETAIL=1)
	// (with it the zone's sun, as the plugin passes it: the models' light 0 below is that sun, for its highlight)
	// (CHARS_SUN="x y z": toward the sun from elsewhere, y down: e.g. a low sun for M9's relief pictures)
	float toward_sun[3] = { -0.31f, -0.951f, 0.0f };
	if (const char *s = getenv("CHARS_SUN"); s != nullptr)
		std::sscanf(s, "%f %f %f", &toward_sun[0], &toward_sun[1], &toward_sun[2]);
	{
		const float sun_l = std::sqrt(toward_sun[0] * toward_sun[0] + toward_sun[1] * toward_sun[1] + toward_sun[2] * toward_sun[2]);
		for (float &c : toward_sun)
			c /= sun_l;
	}
	inj::set_sun(getenv("CHARS_PERPIXEL") != nullptr ? toward_sun : nullptr);

	// The camera on the three of them
	float lo[3], hi[3];
	box_of(L, pose, lo, hi);
	const float centre[3] = { (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2 };
	// (CHARS_YAW: another side, for a look)
	const float yaw = getenv("CHARS_YAW") != nullptr ? float(atof(getenv("CHARS_YAW"))) * 3.14159265f / 180.0f : 0.0f;
	const float eye[3] = { centre[0] - 4.2f * std::sin(yaw), centre[1] - 0.3f, centre[2] - 4.2f * std::cos(yaw) };
	mat4 vp = camera(eye, centre);
	if (getenv("CHARS_ORTHO") != nullptr)
	{
		// (chars.py's view for a side-by-side look: orthographic along +z, fitted to 90% of the height, y down)
		const float scale = 0.9f / std::max(hi[0] - lo[0], hi[1] - lo[1]) * 2.0f;
		const mat4 o = { { scale * float(H) / W, 0, 0, 0, 0, -scale, 0, 0, 0, 0, 0.001f, 0, -centre[0] * scale * float(H) / W, centre[1] * scale, 0.5f - centre[2] * 0.001f, 1 } };
		vp = o;
	}
	const rect middle = project(vp, lo, hi, 0.0f), right = project(vp, lo, hi, kShift), left = project(vp, lo, hi, -kShift),
			   fogged = project(vp, lo, hi, 2 * kShift);
	if (getenv("CHARS_GROUPS") != nullptr)
	{
		// (where each half's skinned vertices land on screen)
		for (int half = 0; half < 2; ++half)
		{
			float x0 = 1e9f, x1 = -1e9f;
			for (const auto &m : L.decoded)
				for (const auto &v : m.vertices)
				{
					float p[3] = { 0, 0, 0 };
					float p0[3] = { v.p0[0], v.p0[1], v.p0[2] }, p1[3] = { v.p1[0], v.p1[1], v.p1[2] };
					if (half == 1)
					{
						if (v.axis[0] != 0)
							p0[v.axis[0] - 1] = -p0[v.axis[0] - 1];
						if (v.axis[1] != 0)
							p1[v.axis[1] - 1] = -p1[v.axis[1] - 1];
					}
					const uint32_t b0 = half ? v.mirror_bone[0] : v.bone[0], b1 = half ? v.mirror_bone[1] : v.bone[1];
					for (int c = 0; c < 3; ++c)
						p[c] = p0[0] * pose[b0].m[c] + p0[1] * pose[b0].m[3 + c] + p0[2] * pose[b0].m[6 + c] + v.w0 * pose[b0].m[9 + c] + p1[0] * pose[b1].m[c] +
							p1[1] * pose[b1].m[3 + c] + p1[2] * pose[b1].m[6 + c] + v.w1 * pose[b1].m[9 + c];
					float o[4] = {};
					const float q[4] = { p[0], p[1], p[2], 1 };
					for (int j = 0; j < 4; ++j)
						for (int k = 0; k < 4; ++k)
							o[j] += q[k] * vp.m[k * 4 + j];
					const float sx = (o[0] / o[3] * 0.5f + 0.5f) * W;
					x0 = std::min(x0, sx);
					x1 = std::max(x1, sx);
				}
			std::printf("      half %d on screen x %.0f..%.0f\n", half, x0, x1);
		}
		uint32_t axes[4] = {};
		for (const auto &m : L.decoded)
			for (const auto &v : m.vertices)
				axes[v.axis[0] & 3]++;
		std::printf("      mirror axes: none %u, x %u, y %u, z %u\n", axes[0], axes[1], axes[2], axes[3]);
	}
	std::printf("      the Mithra's box %.2f..%.2f, %.2f..%.2f, %.2f..%.2f; on screen x %.0f..%.0f y %.0f..%.0f\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], middle.x0, middle.x1,
		middle.y0, middle.y1);

	// The frame, as the game side publishes it
	const uint32_t seq = 21;
	ch::frame &f = ch::begin_frame(seq);
	std::memcpy(f.view_proj, vp.m, 64);
	f.camera = true;
	auto add_model = [&](float dx, uint32_t colour, uint32_t flags) {
		ch::model_entry &m = f.model[f.models];
		m = ch::model_entry();
		m.actor = 0xAC700000u + f.models;
		m.colour = colour;
		m.flags = flags;
		ch::light8 sun = {};
		sun.type = 3;
		sun.diffuse[0] = 1.48f;
		sun.diffuse[1] = 1.47f;
		sun.diffuse[2] = 1.42f;
		sun.direction[0] = 0.31f;
		sun.direction[1] = 0.951f;
		m.lights[0] = sun;
		m.ambient = 0xFF606060u;
		m.tfactor = 0x80808080u;
		m.first_instance = f.instances;
		m.instances = 1;
		ch::instance_entry &ie = f.instance[f.instances++];
		ie.first_bone = f.bones;
		ie.bones = uint32_t(pose.size());
		for (size_t b = 0; b < pose.size(); ++b)
		{
			f.palette[f.bones + b] = pose[b];
			f.palette[f.bones + b].m[9] += dx;
		}
		f.bones += ie.bones;
		ie.first_mesh = f.meshes;
		ie.meshes = uint32_t(L.meshes.size());
		uint32_t hidden = 0;
		for (size_t k = 0; k < L.meshes.size(); ++k)
		{
			f.mesh[f.meshes + k].mesh = L.meshes[k];
			hidden |= ch::os2::hidden_by(ch::cache::mesh_hides(L.meshes[k]));
		}
		m.hidden = getenv("CHARS_NOHIDE") != nullptr ? 0 : hidden;
		f.meshes += ie.meshes;
		f.models++;
	};
	add_model(0.0f, 0x80808080u, ch::kNative);   // the middle: HiFi XI's
	add_model(-kShift, 0x80808080u, 0);          // the left: the game's
	add_model(2 * kShift, 0x80808080u, ch::kNative); // the far right: HiFi XI's, in its own fog (all of it: red)
	{
		ch::model_entry &m = f.model[f.models - 1];
		m.fog_read = true;
		m.fog_colour = 0xFFFF0000u;
		m.fog_start = 0.0f;
		m.fog_end = 0.5f;
	}
	ch::publish_opaque(f);
	add_model(kShift, 0x40808080u, ch::kNative | ch::kFromFlush); // the right: see-through, from the flush
	ch::publish_see_through(f);
	ch::cache::publish_palettes(f, 0, f.bones);

	// dgVoodoo's world: typeless resources, UNORM / D24S8 views; CHARS_MSAA=n multisampled (its depth then isn't read back:
	// what's drawn stands for it)
	const UINT samples = getenv("CHARS_MSAA") != nullptr ? UINT(atoi(getenv("CHARS_MSAA"))) : 1u;
	if (samples > 1)
		std::printf("      the world multisampled: %u samples\n", samples);
	D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, up = { D3D12_HEAP_TYPE_UPLOAD }, rb = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = W;
	td.Height = H;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.SampleDesc.Count = samples;
	td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	ID3D12Resource *A = nullptr, *Az = nullptr;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&A));
	td.Format = DXGI_FORMAT_R24G8_TYPELESS;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&Az));
	ID3D12DescriptorHeap *rtvh = nullptr, *dsvh = nullptr, *srvh = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh));
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh));
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvh));
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvh->GetCPUDescriptorHandleForHeapStart(), dsv = dsvh->GetCPUDescriptorHandleForHeapStart();
	D3D12_RENDER_TARGET_VIEW_DESC rvd = {};
	rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rvd.ViewDimension = samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(A, &rvd, rtv);
	D3D12_DEPTH_STENCIL_VIEW_DESC dvd = {};
	dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dvd.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
	dvd.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
	device->CreateDepthStencilView(Az, &dvd, dsv);
	ID3D12DescriptorHeap *dsvh2 = nullptr;
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh2));
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv_clear = dsvh2->GetCPUDescriptorHandleForHeapStart();
	dvd.Flags = D3D12_DSV_FLAG_NONE;
	device->CreateDepthStencilView(Az, &dvd, dsv_clear);

	// dgVoodoo's own draw: a textured quad (a 2x2 of one colour) in the lower right corner
	ID3D12Resource *tex = nullptr, *tex_up = nullptr, *vb = nullptr;
	td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = 2;
	td.Height = 2;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.SampleDesc.Count = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = 1024;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tex_up));
	device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vb));
	{
		uint8_t *p = nullptr;
		tex_up->Map(0, nullptr, reinterpret_cast<void **>(&p));
		const uint32_t px = 0xFF2080E0u;
		for (int row = 0; row < 2; ++row)
			for (int x = 0; x < 2; ++x)
				std::memcpy(p + row * 256 + x * 4, &px, 4);
		tex_up->Unmap(0, nullptr);
		float *v = nullptr;
		vb->Map(0, nullptr, reinterpret_cast<void **>(&v));
		const float quad[] = { 0.8f, -0.8f, 0, 0, 0.98f, -0.8f, 1, 0, 0.8f, -0.98f, 0, 1, 0.98f, -0.98f, 1, 1 };
		std::memcpy(v, quad, sizeof(quad));
		vb->Unmap(0, nullptr);
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
	sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sv.Texture2D.MipLevels = 1;
	const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_CPU_DESCRIPTOR_HANDLE srv = srvh->GetCPUDescriptorHandleForHeapStart();
	srv.ptr += 2 * step;
	device->CreateShaderResourceView(tex, &sv, srv);
	D3D12_GPU_DESCRIPTOR_HANDLE table = srvh->GetGPUDescriptorHandleForHeapStart();
	table.ptr += 2 * step;
	D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0 };
	D3D12_ROOT_PARAMETER rp[2] = {};
	rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rp[0].Constants.Num32BitValues = 1;
	rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rp[1].DescriptorTable.NumDescriptorRanges = 1;
	rp[1].DescriptorTable.pDescriptorRanges = &range;
	D3D12_STATIC_SAMPLER_DESC ss = {};
	ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
	ID3DBlob *blob = nullptr, *err = nullptr;
	ID3D12RootSignature *game_root = nullptr;
	D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
	device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&game_root));
	ID3DBlob *gvs = compile(kGame, "vs", "vs_5_0"), *gps = compile(kGame, "ps", "ps_5_0");
	const D3D12_INPUT_ELEMENT_DESC il[] = { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
	pd.pRootSignature = game_root;
	pd.VS = { gvs->GetBufferPointer(), gvs->GetBufferSize() };
	pd.PS = { gps->GetBufferPointer(), gps->GetBufferSize() };
	pd.InputLayout = { il, 2 };
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1;
	pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	pd.SampleDesc.Count = samples;
	pd.RasterizerState.MultisampleEnable = samples > 1;
	pd.SampleMask = UINT_MAX;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.DepthStencilState.DepthEnable = TRUE;
	pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	ID3D12PipelineState *game_pso = nullptr;
	check(gvs != nullptr && gps != nullptr && SUCCEEDED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&game_pso))), "dgVoodoo's stand-in pipeline");

	// The list, as dgVoodoo records a frame round the characters' markers
	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *list = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
	{
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = tex;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.pResource = tex_up;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 256 };
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		barrier(list, tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	inj::on_marker(Az);
	g_marked = Az;
	list->ClearRenderTargetView(rtv, kSky, 0, nullptr);
	list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
	list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	const D3D12_VIEWPORT vpt = { 0.5f, 0.5f, float(W), float(H), 0, 1 };
	const D3D12_RECT sc = { 0, 0, LONG(W), LONG(H) };
	list->RSSetViewports(1, &vpt);
	list->RSSetScissorRects(1, &sc);
	list->SetDescriptorHeaps(1, &srvh);
	list->SetGraphicsRootSignature(game_root);
	list->SetGraphicsRoot32BitConstant(0, 7, 0);
	list->SetGraphicsRootDescriptorTable(1, table);
	list->SetPipelineState(game_pso);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGPUVirtualAddress(), 64, 16 };
	list->IASetVertexBuffers(0, 1, &vbv);
	grass::list_state bound;
	bound.rtvs[0] = rtv;
	bound.rtv_count = 1;
	bound.dsv = dsv;
	bound.pipeline = game_pso;
	bound.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
	bound.viewports[0] = vpt;
	bound.viewport_count = 1;
	bound.scissors[0] = sc;
	bound.scissor_count = 1;
	bound.set_root(game_root);
	bound.arg(1).kind = grass::root_arg::table;
	bound.arg(1).value = table.ptr;
	const uint32_t seven = 7;
	bound.set_constants(0, 0, 1, &seven);
	inj::world w;
	w.rt = A;
	w.ds = Az;
	w.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	w.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;

	// The characters' end marker: the planes as dgVoodoo leaves them (depth read, stencil written for the clear)
	barrier(list, Az, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	inj::set_motion_wanted(samples == 1); // (the characters' motion for FSR, as while it draws: chardraw.hpp "Motion")
	inj::on_chars_end(list, w, seq & 15);
	inj::on_chars_end(list, w, seq & 15); // (the same seq again: nothing)
	// A draw of dgVoodoo's in between, writing depth (the stencil plane back to read; then to write for the marker)
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
	barrier(list, Az, 0, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	inj::on_see_through(list, w, seq & 15);
	inj::on_see_through(list, w, seq & 15); // (a second see-through marker: there's no second model)
	// World FX's casters' pass (its shading pass calls record_casters): the sun straight overhead, a D32 map
	// kCasterSize square looking down on all four (x across, z down the map, depth by height: the top of the box
	// nearest), then the game's state put back as World FX does after its pass
	constexpr UINT kCasterSize = 2048;
	const float c_reach = 0.5f * ((hi[0] + 2 * kShift) - (lo[0] - kShift)) + 0.5f, c_x = 0.5f * ((hi[0] + 2 * kShift) + (lo[0] - kShift)), c_z = 0.5f * (lo[2] + hi[2]);
	const float c_top = lo[1] - 1.0f, c_span = (hi[1] - lo[1]) + 2.0f;
	const float caster_m[16] = { 1.0f / c_reach, 0, 0, 0, 0, 0, 1.0f / c_span, 0, 0, -1.0f / c_reach, 0, 0, -c_x / c_reach, c_z / c_reach, -c_top / c_span, 1 };
	ID3D12Resource *caster_map = nullptr, *caster_back = nullptr;
	ID3D12DescriptorHeap *caster_heap = nullptr;
	{
		D3D12_RESOURCE_DESC cd = {};
		cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		cd.Width = kCasterSize;
		cd.Height = kCasterSize;
		cd.DepthOrArraySize = 1;
		cd.MipLevels = 1;
		cd.Format = DXGI_FORMAT_D32_FLOAT;
		cd.SampleDesc.Count = 1;
		cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		D3D12_CLEAR_VALUE cv = {};
		cv.Format = DXGI_FORMAT_D32_FLOAT;
		cv.DepthStencil.Depth = 1.0f;
		device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&caster_map));
		D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
		device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&caster_heap));
		device->CreateDepthStencilView(caster_map, nullptr, caster_heap->GetCPUDescriptorHandleForHeapStart());
		D3D12_RESOURCE_DESC cb = bd;
		cb.Width = UINT64(kCasterSize) * kCasterSize * 4;
		device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &cb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&caster_back));
	}
	const D3D12_CPU_DESCRIPTOR_HANDLE caster_dsv = caster_heap->GetCPUDescriptorHandleForHeapStart();
	list->ClearDepthStencilView(caster_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	const bool casters_drawn = inj::record_casters(list, caster_m, caster_dsv, kCasterSize);
	list->SetDescriptorHeaps(1, &srvh); // (as our d3d8.dll does after a mark: its heaps, then its state)
	bound.restore(list);
	barrier(list, caster_map, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	{
		D3D12_TEXTURE_COPY_LOCATION cdst = {}, csrc = {};
		cdst.pResource = caster_back;
		cdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		cdst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_TYPELESS, kCasterSize, kCasterSize, 1, kCasterSize * 4 };
		csrc.pResource = caster_map;
		csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		list->CopyTextureRegion(&cdst, 0, 0, 0, &csrc, nullptr);
	}
	// dgVoodoo's next draw, nothing bound again
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
	list->DrawInstanced(4, 1, 0, 0);
	// The picture and depth out
	ID3D12Resource *picture = A;
	if (samples > 1)
	{
		D3D12_RESOURCE_DESC rd = A->GetDesc();
		rd.SampleDesc.Count = 1;
		rd.Alignment = 0; // (a multisampled resource's 4 MB alignment isn't a plain one's)
		rd.Flags = D3D12_RESOURCE_FLAG_NONE;
		device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&picture));
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		list->ResolveSubresource(picture, 0, A, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
		barrier(list, picture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
	}
	else
	{
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	}
	ID3D12Resource *readback = nullptr, *depth_back = nullptr;
	bd.Width = UINT64(W) * 4 * H;
	device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
	device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&depth_back));
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	dst.pResource = readback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, W, H, 1, W * 4 };
	src.pResource = picture;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	if (samples == 1)
	{
		dst.pResource = depth_back;
		dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_TYPELESS, W, H, 1, W * 4 };
		src.pResource = Az;
		src.SubresourceIndex = 0;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}
	// (the characters' motion, read back: R16G16B16A16_FLOAT)
	ID3D12Resource *motion_tex = nullptr, *motion_back = nullptr;
	uint64_t motion_frames = 0;
	const bool have_motion = inj::motion(motion_tex, motion_frames);
	if (have_motion)
	{
		bd.Width = UINT64(W) * 8 * H;
		device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&motion_back));
		barrier(list, motion_tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		dst.pResource = motion_back;
		dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 1, W * 8 };
		src.pResource = motion_tex;
		src.SubresourceIndex = 0;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		barrier(list, motion_tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	list->Close();
	ID3D12CommandList *lists[] = { list };
	direct->ExecuteCommandLists(1, lists);
	ID3D12Fence *fence = nullptr;
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	direct->Signal(fence, 1);
	HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	fence->SetEventOnCompletion(1, e);
	WaitForSingleObject(e, 60000);
	check(device->GetDeviceRemovedReason() == S_OK, "the device still there");
	std::string first;
	const uint32_t messages = debug_messages(info, first);
	check(messages == 0, "the debug layer: nothing (" + std::to_string(messages) + (messages != 0 ? ": " + first : std::string()) + ")");

	// What came out
	uint8_t *px = nullptr;
	uint32_t *dz = nullptr;
	readback->Map(0, nullptr, reinterpret_cast<void **>(&px));
	depth_back->Map(0, nullptr, reinterpret_cast<void **>(&dz));
	const int sky8[3] = { int(kSky[0] * 255 + 0.5f), int(kSky[1] * 255 + 0.5f), int(kSky[2] * 255 + 0.5f) };
	auto is_sky = [&](const uint8_t *p) { return std::abs(p[0] - sky8[0]) <= 2 && std::abs(p[1] - sky8[1]) <= 2 && std::abs(p[2] - sky8[2]) <= 2; };
	uint32_t in_middle = 0, in_right = 0, in_left = 0, outside = 0, depth_middle = 0, depth_right = 0, in_fogged = 0, fog_red = 0;
	double off_sky_middle = 0, off_sky_right = 0;
	for (UINT y = 0; y < H; ++y)
		for (UINT x = 0; x < W; ++x)
		{
			const uint8_t *p = px + (size_t(y) * W + x) * 4;
			const bool nearer = samples == 1 ? (dz[size_t(y) * W + x] & 0xFFFFFF) < 0xFFFFFF : !is_sky(px + (size_t(y) * W + x) * 4);
			const float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
			const bool quad = x >= 720 && y >= 405; // (dgVoodoo's own quad)
			if (quad)
				continue;
			const double off = std::abs(p[0] - sky8[0]) + std::abs(p[1] - sky8[1]) + std::abs(p[2] - sky8[2]);
			if (middle.inside(cx, cy, 2))
			{
				if (!is_sky(p))
				{
					in_middle++;
					off_sky_middle += off;
				}
				depth_middle += nearer;
			}
			else if (right.inside(cx, cy, 2))
			{
				if (!is_sky(p))
				{
					in_right++;
					off_sky_right += off;
				}
				depth_right += nearer;
			}
			else if (left.inside(cx, cy, 2))
				in_left += !is_sky(p) || nearer;
			else if (fogged.inside(cx, cy, 2))
			{
				if (!is_sky(p))
				{
					in_fogged++;
					fog_red += p[0] >= 250 && p[1] <= 5 && p[2] <= 5;
				}
			}
			else
				outside += !is_sky(p) || nearer;
		}
	const double box = (middle.x1 - middle.x0) * (middle.y1 - middle.y0);
	char line[300];
	std::snprintf(line, sizeof(line), "the middle Mithra drawn at the end marker: %u pixels (%.0f%% of its box), its depth %u, nothing outside the boxes (%u)", in_middle,
		100.0 * in_middle / box, depth_middle, outside);
	check(in_middle > box * 0.15 && depth_middle >= in_middle * 9 / 10 && outside == 0, line);
	// Its motion (for FSR): on each of its pixels its view depth and no move of its own (no last frame: this is the first);
	// nothing elsewhere but the other opaque one HiFi XI draws
	if (samples == 1)
	{
		uint32_t on_middle = 0, still_middle = 0, sane_depth = 0, stray = 0;
		const uint16_t *mo = nullptr;
		if (motion_back != nullptr)
			motion_back->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint16_t **>(&mo)));
		for (UINT y = 0; y < H && mo != nullptr; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const uint16_t *q = mo + (size_t(y) * W + x) * 4;
				const float mx = DirectX::PackedVector::XMConvertHalfToFloat(q[0]), my = DirectX::PackedVector::XMConvertHalfToFloat(q[1]),
					mz = DirectX::PackedVector::XMConvertHalfToFloat(q[2]), mw = DirectX::PackedVector::XMConvertHalfToFloat(q[3]);
				const float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
				if (middle.inside(cx, cy, 2) && !is_sky(px + (size_t(y) * W + x) * 4) && mw > 0.0f)
				{
					on_middle++;
					still_middle += mx == 0.0f && my == 0.0f && mz == 0.0f ? 1 : 0;
					sane_depth += mw > 0.05f && mw < 100.0f ? 1 : 0;
				}
				stray += !middle.inside(cx, cy, 2) && !fogged.inside(cx, cy, 2) && mw != 0.0f ? 1 : 0;
			}
		check(have_motion && motion_frames == 1 && on_middle >= in_middle * 9 / 10 && still_middle == on_middle && sane_depth == on_middle && stray == 0,
			"its motion for FSR: " + std::to_string(on_middle) + " of its " + std::to_string(in_middle) + " pixels with its depth (" + std::to_string(sane_depth) +
			" sane), none moved on a first frame (" + std::to_string(still_middle) + "), nothing outside HiFi XI's opaque ones (" + std::to_string(stray) + ")");
	}
	const double mean_middle = in_middle ? off_sky_middle / in_middle : 0, mean_right = in_right ? off_sky_right / in_right : 0;
	std::snprintf(line, sizeof(line), "the see-through one at its marker: %u pixels, %.0f%% as far from the sky as the opaque one's (about half: alpha 0x40), its depth written %u",
		in_right, mean_middle > 0 ? 100.0 * mean_right / mean_middle : 0.0, depth_right);
	check(in_right > box * 0.1 && mean_right > 0.3 * mean_middle && mean_right < 0.7 * mean_middle && depth_right >= in_right * 9 / 10, line);
	check(in_left == 0, "the game's own (not kNative) left alone: " + std::to_string(in_left) + " pixels");
	check(in_fogged > box * 0.1 && (samples == 1 ? fog_red == in_fogged : fog_red >= in_fogged * 8 / 10), "one in its own fog (the actor's, red, all of it): " + std::to_string(fog_red) + " of " + std::to_string(in_fogged) +
		" pixels the fog's colour");
	// Every skinned vertex of the middle one (both halves) on a drawn pixel (or one next to it): what a missing or
	// misplaced half, bone or mesh would show
	{
		uint32_t total = 0, on = 0, half_on[2] = {}, half_total[2] = {};
		for (const auto &m : L.decoded)
			for (int half = 0; half < (m.mirrored ? 2 : 1); ++half)
				for (const auto &v : m.vertices)
				{
					float p0[3] = { v.p0[0], v.p0[1], v.p0[2] }, p1[3] = { v.p1[0], v.p1[1], v.p1[2] };
					if (half == 1)
					{
						if (v.axis[0] != 0)
							p0[v.axis[0] - 1] = -p0[v.axis[0] - 1];
						if (v.axis[1] != 0)
							p1[v.axis[1] - 1] = -p1[v.axis[1] - 1];
					}
					const uint32_t b0 = std::min<uint32_t>(half ? v.mirror_bone[0] : v.bone[0], uint32_t(pose.size() - 1));
					const uint32_t b1 = std::min<uint32_t>(half ? v.mirror_bone[1] : v.bone[1], uint32_t(pose.size() - 1));
					float q[4] = { 0, 0, 0, 1 };
					for (int c = 0; c < 3; ++c)
						q[c] = p0[0] * pose[b0].m[c] + p0[1] * pose[b0].m[3 + c] + p0[2] * pose[b0].m[6 + c] + v.w0 * pose[b0].m[9 + c] + p1[0] * pose[b1].m[c] +
							p1[1] * pose[b1].m[3 + c] + p1[2] * pose[b1].m[6 + c] + v.w1 * pose[b1].m[9 + c];
					float o[4] = {};
					for (int j = 0; j < 4; ++j)
						for (int k = 0; k < 4; ++k)
							o[j] += q[k] * vp.m[k * 4 + j];
					const int sx = int((o[0] / o[3] * 0.5f + 0.5f) * W), sy = int((0.5f - o[1] / o[3] * 0.5f) * H);
					bool drawn = false;
					for (int dy = -1; dy <= 1 && !drawn; ++dy)
						for (int dx = -1; dx <= 1 && !drawn; ++dx)
						{
							const int x = sx + dx, y = sy + dy;
							if (x >= 0 && y >= 0 && x < int(W) && y < int(H))
								drawn = !is_sky(px + (size_t(y) * W + x) * 4);
						}
					total++;
					on += drawn;
					half_total[half]++;
					half_on[half] += drawn;
				}
		std::snprintf(line, sizeof(line), "the middle one's skinned vertices on drawn pixels: %u of %u (%.0f%%; this half %.0f%%, the mirrored half %.0f%%)", on, total,
			100.0 * on / std::max(total, 1u), 100.0 * half_on[0] / std::max(half_total[0], 1u), 100.0 * half_on[1] / std::max(half_total[1], 1u));
		check(on >= total * 9 / 10 && half_on[0] >= half_total[0] * 9 / 10 && half_on[1] >= half_total[1] * 9 / 10, line);
	}
	const uint8_t *c = px + (size_t(430) * W + 760) * 4;
	std::snprintf(line, sizeof(line), "dgVoodoo's next draw as it meant it: its texture's colour (%u %u %u)", c[0], c[1], c[2]);
	check(c[0] == 0xE0 && c[1] == 0x80 && c[2] == 0x20, line);
	const inj::counts n = inj::get_counts();
	check(n.frames == 1 && n.models == 2 && n.see_through == 1 && n.failed == 0, "drawn: 1 frame, 2 opaque models, 1 see-through: " + inj::status()[0]);
	// The casters' map: each of HiFi XI's three (the middle, the see-through one, the fogged one) seen from above in its
	// own x band, the game's own (left) not; the nearest texel the top of the box (the head), within a texel or two
	{
		float *cz = nullptr;
		caster_back->Map(0, nullptr, reinterpret_cast<void **>(&cz));
		uint32_t band[4] = {};
		float nearest = 1.0f;
		const float dxs[4] = { 0.0f, -kShift, kShift, 2 * kShift }; // (middle, left, see-through, fogged)
		for (UINT y = 0; y < kCasterSize; ++y)
			for (UINT x = 0; x < kCasterSize; ++x)
			{
				const float d = cz[size_t(y) * kCasterSize + x];
				if (!(d < 1.0f))
					continue;
				nearest = std::min(nearest, d);
				const float wx = c_x + ((float(x) + 0.5f) / kCasterSize * 2.0f - 1.0f) * c_reach;
				for (int k = 0; k < 4; ++k)
					if (wx >= lo[0] + dxs[k] - 0.05f && wx <= hi[0] + dxs[k] + 0.05f)
						band[k]++;
			}
		const float head = (lo[1] - c_top) / c_span, texel_depth = 2.0f * c_reach / kCasterSize / c_span;
		std::snprintf(line, sizeof(line), "the casters' map from above: %u, %u, %u texels under HiFi XI's three, %u under the game's own; nearest %.4f (the box's top %.4f)",
			band[0], band[2], band[3], band[1], nearest, head);
		check(casters_drawn && band[0] > 2000 && band[2] > 2000 && band[3] > 2000 && band[1] == 0 && std::fabs(nearest - head) < 3.0f * texel_depth + 0.002f, line);
		const inj::counts nc = inj::get_counts();
		check(nc.caster_frames == 1 && nc.caster_models == 3, "counted: 1 casters' frame, 3 models (" + std::to_string(nc.caster_models) + ")");
		caster_back->Unmap(0, nullptr);
	}
	// The zone's lamps (chardraw.hpp "Lamps"): the middle one alone again (seqs 22-24), a lamp 1.5 yalms in front of it
	// toward the camera at its middle's height. 22: the game's own lamp slot (the lamp turned directional from the actor's
	// root, its colour times its weight there), no zone lamps; 23: the same slot and the zone's lamp handed over; 24: the
	// sun alone. Its pixels' mean brightness: 23 above 24, and within half again of 22 (the slot left out, not added to).
	if (samples == 1)
	{
		const ch::model_entry &mm = f.model[0];
		const ch::instance_entry &mi = f.instance[mm.first_instance];
		const float *root = f.palette[mi.first_bone].m + 9;
		float way[3] = { eye[0] - centre[0], 0.0f, eye[2] - centre[2] };
		const float wl = std::sqrt(way[0] * way[0] + way[2] * way[2]);
		const float lamp_at[3] = { centre[0] + 1.5f * way[0] / wl, centre[1], centre[2] + 1.5f * way[2] / wl };
		const float lamp_colour[3] = { 0.6f, 0.5f, 0.3f };
		const float to_actor[3] = { root[0] - lamp_at[0], root[1] - lamp_at[1], root[2] - lamp_at[2] };
		const float d2 = to_actor[0] * to_actor[0] + to_actor[1] * to_actor[1] + to_actor[2] * to_actor[2];
		const float weight = 1.0f / std::max(d2 * 0.5f, 1e-4f);
		auto lamp_frame = [&](uint32_t s, bool game_lamp) {
			ch::frame &lf = ch::begin_frame(s);
			std::memcpy(lf.view_proj, vp.m, 64);
			lf.camera = true;
			lf.model[0] = mm;
			lf.model[0].first_instance = 0;
			lf.model[0].actor = 0xAC710000u;
			if (game_lamp)
			{
				ch::light8 l = {};
				l.type = 3;
				for (int k = 0; k < 3; ++k)
				{
					l.diffuse[k] = lamp_colour[k] * weight;
					l.direction[k] = to_actor[k] / std::sqrt(d2);
				}
				lf.model[0].lights[1] = l;
			}
			lf.instance[0] = mi;
			lf.instance[0].first_bone = 0;
			lf.instance[0].first_mesh = 0;
			for (uint32_t b = 0; b < mi.bones; ++b)
				lf.palette[b] = f.palette[mi.first_bone + b];
			for (uint32_t k = 0; k < mi.meshes; ++k)
				lf.mesh[k] = f.mesh[mi.first_mesh + k];
			lf.models = 1;
			lf.instances = 1;
			lf.bones = mi.bones;
			lf.meshes = mi.meshes;
			ch::publish_opaque(lf);
			ch::publish_see_through(lf);
			ch::cache::publish_palettes(lf, 0, lf.bones);
		};
		const float sun_toward[3] = { -0.31f / 1.00025f, -0.951f / 1.00025f, 0.0f };
		inj::set_sun(sun_toward);
		const float zone_lamp[8] = { lamp_at[0], lamp_at[1], lamp_at[2], 6.0f, lamp_colour[0], lamp_colour[1], lamp_colour[2], 0.5f };
		alloc->Reset();
		list->Reset(alloc, nullptr);
		list->SetDescriptorHeaps(1, &srvh);
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		ID3D12Resource *outs[3] = {};
		bd.Width = UINT64(W) * 4 * H;
		for (int k = 0; k < 3; ++k)
		{
			device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outs[k]));
			const uint32_t s = 22 + k;
			lamp_frame(s, k < 2);
			inj::set_lamps(k == 1 ? zone_lamp : nullptr, k == 1 ? 1 : 0);
			list->ClearRenderTargetView(rtv, kSky, 0, nullptr);
			list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			inj::on_chars_end(list, w, s & 15);
			barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
			to.pResource = outs[k];
			to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			to.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, W, H, 1, W * 4 };
			from.pResource = A;
			from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
			barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}
		inj::set_lamps(nullptr, 0);
		inj::set_sun(nullptr);
		list->Close();
		direct->ExecuteCommandLists(1, lists);
		direct->Signal(fence, 2);
		fence->SetEventOnCompletion(2, e);
		WaitForSingleObject(e, 60000);
		check(device->GetDeviceRemovedReason() == S_OK, "the device still there (the lamps' frames)");
		const uint32_t lamp_messages = debug_messages(info, first);
		check(lamp_messages == 0, "the debug layer: nothing (" + std::to_string(lamp_messages) + (lamp_messages != 0 ? ": " + first : std::string()) + ")");
		double mean[3] = {};
		uint32_t count[3] = {};
		for (int k = 0; k < 3; ++k)
		{
			uint8_t *img = nullptr;
			outs[k]->Map(0, nullptr, reinterpret_cast<void **>(&img));
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					const uint8_t *p = img + (size_t(y) * W + x) * 4;
					if (middle.inside(float(x) + 0.5f, float(y) + 0.5f, 2) && !is_sky(p))
					{
						mean[k] += 0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2];
						count[k]++;
					}
				}
			mean[k] /= std::max(count[k], 1u);
			outs[k]->Unmap(0, nullptr);
			outs[k]->Release();
		}
		std::snprintf(line, sizeof(line), "a lamp before the middle one (%u, %u, %u pixels): the game's lamp slot %.1f, the zone's lamp in its stead %.1f, the sun alone %.1f",
			count[0], count[1], count[2], mean[0], mean[1], mean[2]);
		check(count[1] > 1000 && mean[1] > mean[2] + 3.0, line);
		check(mean[1] - mean[2] < 1.5 * (mean[0] - mean[2]) && mean[1] - mean[2] > 0.4 * (mean[0] - mean[2]),
			"  the zone's lamp stands in for the game's lamp slot (about as bright: the slot left out, not added to)");

		// The sun-alone frame again (seq 25), its meshes and textures let go after it's recorded and before it runs (not
		// named for kForgetSeconds), and a red texture put on the GPU meanwhile: the pairs of descriptors the list reads
		// aren't given to it until the GPU is past the list (retire::then, two signals of the fence on), the picture is the
		// same as before, the debug layer says nothing
		std::vector<UINT64> in_flight;
		for (uint32_t id : L.meshes)
		{
			ch::cache::mesh_view mv;
			if (!ch::cache::mesh_view_of(id, mv))
				continue;
			in_flight.push_back(mv.views.ptr);
			for (uint32_t t : mv.textures)
				if (D3D12_GPU_DESCRIPTOR_HANDLE th = {}; t != ch::cache::kNone && ch::cache::texture_view_of(t, th))
					in_flight.push_back(th.ptr);
		}
		std::sort(in_flight.begin(), in_flight.end());
		in_flight.erase(std::unique(in_flight.begin(), in_flight.end()), in_flight.end()); // (a texture named by several meshes: one pair)
		alloc->Reset();
		list->Reset(alloc, nullptr);
		list->SetDescriptorHeaps(1, &srvh);
		lamp_frame(25, false);
		inj::set_sun(sun_toward);
		list->ClearRenderTargetView(rtv, kSky, 0, nullptr);
		list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		inj::on_chars_end(list, w, 25 & 15);
		inj::set_sun(nullptr);
		ID3D12Resource *again = nullptr;
		device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&again));
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		{
			D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
			to.pResource = again;
			to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			to.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, W, H, 1, W * 4 };
			from.pResource = A;
			from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
		}
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		list->Close();
		ch::cache::frame_tick(device, 1.0 + ch::cache::kForgetSeconds + 1.0);
		const ch::cache::counts let_go = ch::cache::get_counts();
		check(let_go.meshes == 0 && let_go.textures == 0 && let_go.slots_retiring == in_flight.size(),
			"the Mithra let go after its frame was recorded: " + std::to_string(let_go.slots_retiring) + " pairs of descriptors waiting for the GPU");
		std::vector<uint8_t> red(69 + 16 * 16 * 8, 0);
		{
			red[0] = 0x20; // (layout 2, DXT only: 64 x 64, every block colour 0 pure red)
			std::memcpy(red.data() + 1, "charinject red  ", 16);
			const uint16_t side = 64, colour = 0xF800;
			std::memcpy(red.data() + 21, &side, 2);
			std::memcpy(red.data() + 25, &side, 2);
			std::memcpy(red.data() + 57, "1TXD", 4);
			for (size_t b = 69; b < red.size(); b += 8)
				std::memcpy(red.data() + b, &colour, 2);
		}
		ch::cache::key rk;
		rk.resource = 0x7F000000u;
		const uint32_t fresh = ch::cache::add_texture(rk, std::move(red));
		D3D12_GPU_DESCRIPTOR_HANDLE fh = {};
		const bool fresh_up = ch::cache::settle(60000) && ch::cache::texture_view_of(fresh, fh);
		check(fresh_up && std::find(in_flight.begin(), in_flight.end(), fh.ptr) == in_flight.end(),
			"  a red texture on the GPU meanwhile, in a pair the list doesn't read");
		direct->ExecuteCommandLists(1, lists);
		direct->Signal(fence, 3);
		fence->SetEventOnCompletion(3, e);
		WaitForSingleObject(e, 60000);
		const uint32_t forget_messages = debug_messages(info, first);
		check(device->GetDeviceRemovedReason() == S_OK && forget_messages == 0,
			"  the frame run: the debug layer says nothing (" + std::to_string(forget_messages) + (forget_messages != 0 ? ": " + first : std::string()) + ")");
		double again_mean = 0.0;
		uint32_t again_count = 0;
		{
			uint8_t *img = nullptr;
			again->Map(0, nullptr, reinterpret_cast<void **>(&img));
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					const uint8_t *p = img + (size_t(y) * W + x) * 4;
					if (middle.inside(float(x) + 0.5f, float(y) + 0.5f, 2) && !is_sky(p))
					{
						again_mean += 0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2];
						again_count++;
					}
				}
			again_mean /= std::max(again_count, 1u);
			again->Unmap(0, nullptr);
			again->Release();
		}
		std::snprintf(line, sizeof(line), "  the picture as before it was let go: %u pixels, brightness %.2f (before: %u, %.2f)", again_count, again_mean, count[2], mean[2]);
		check(again_count == count[2] && std::fabs(again_mean - mean[2]) < 0.01, line);
		hifi::retire::on_present(direct);
		const uint32_t after_one = ch::cache::get_counts().slots_retiring;
		int presents = 1;
		for (; presents < 40 && ch::cache::get_counts().slots_retiring != 0; ++presents)
		{
			Sleep(20);
			hifi::retire::on_present(direct);
		}
		check(after_one == in_flight.size() && ch::cache::get_counts().slots_retiring == 0,
			"  the pairs not given back at the next present, all back " + std::to_string(presents) + " presents on");

		// ---- The texture pack (docs/texture-pack/characters.md): the Mithra into the cache again from its original
		// file's bytes (as the game holds it once the hifi-textures overlay is unlisted), the streamer on this device,
		// charpack driven as the module drives it (each present: the streamer's on_present, then its frame; then
		// retire::on_present), made-up packs under build\test\charinject
		{
			namespace pk = hifi::pack;
			namespace st = hifi::pack::stream;
			namespace cp = hifi::chars::charpack;
			wchar_t exe[MAX_PATH];
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			const std::wstring build = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\') + 1), here = build + L"test\\charinject\\";
			CreateDirectoryW((build + L"test").c_str(), nullptr);
			CreateDirectoryW(here.c_str(), nullptr);
			double pnow = 23.0;
			uint64_t fv = 10;
			uint32_t next_seq = 40;
			ID3D12Resource *pic = nullptr;
			bd.Width = UINT64(W) * 4 * H;
			device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pic));
			const loaded P = load_into_cache(dat);
			ch::cache::frame_tick(device, pnow);
			ch::cache::settle(60000);
			for (int i = 0; i < 400 && ch::cache::get_counts().meshes_ready < P.meshes.size(); ++i)
				Sleep(50);
			ch::cache::settle(60000);
			// (its textures, each once: their ids, keys and chunks)
			std::vector<uint32_t> tids;
			for (uint32_t id : P.meshes)
				if (ch::cache::mesh_view mv; ch::cache::mesh_view_of(id, mv))
					for (uint32_t t : mv.textures)
						if (t != ch::cache::kNone && std::find(tids.begin(), tids.end(), t) == tids.end())
							tids.push_back(t);
			std::map<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>, const std::vector<uint8_t> *> chunk_of;
			for (const dat_chunk &dc : dat)
				if (dc.kind == 0x20)
				{
					const pk::key k = pk::key_of(dc.data.data(), dc.data.size());
					chunk_of.emplace(std::make_tuple(k.hash, k.bytes, k.w, k.h), &dc.data);
				}
			std::vector<pk::key> keys;
			std::vector<std::pair<pk::key, const std::vector<uint8_t> *>> textures;
			for (uint32_t t : tids)
			{
				const pk::key k = ch::cache::test::pack_key_of(t);
				keys.push_back(k);
				const auto found = chunk_of.find(std::make_tuple(k.hash, k.bytes, k.w, k.h));
				if (found != chunk_of.end() && std::none_of(textures.begin(), textures.end(), [&](const auto &p) { return p.first == k; }))
					textures.emplace_back(k, found->second);
			}
			std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>> unique;
			for (const pk::key &k : keys)
				unique.emplace(k.hash, k.bytes, k.w, k.h);
			check(tids.size() >= 3 && textures.size() == unique.size() && unique.size() == tids.size(), "the texture pack: the Mithra again, its " + std::to_string(tids.size()) + " textures keyed from the bytes given, each its DAT chunk's key");
			if (tids.size() < 3)
				return failures;
			const pk::key left_out = keys[0], no_pack = keys[1];
			static ID3D12Device *s_device = nullptr;
			static bool s_overlay = false;
			s_device = device;
			cp::sources sources;
			sources.device = []() { return s_device; };
			sources.overlay_listed = []() { return s_overlay; };
			cp::set_sources(sources);

			// The frame: the middle one alone (as the lamp frames), its meshes the new ones (or, for the pictures, the
			// overlay's copies)
			const std::vector<uint32_t> *frame_meshes = &P.meshes;
			auto pack_frame = [&](uint32_t s, const mat4 &m) {
				ch::frame &pf = ch::begin_frame(s);
				std::memcpy(pf.view_proj, m.m, 64);
				pf.camera = true;
				pf.model[0] = mm;
				pf.model[0].first_instance = 0;
				pf.model[0].actor = 0xAC720000u;
				pf.instance[0] = mi;
				pf.instance[0].first_bone = 0;
				pf.instance[0].first_mesh = 0;
				pf.instance[0].meshes = uint32_t(frame_meshes->size());
				for (uint32_t b = 0; b < mi.bones; ++b)
					pf.palette[b] = f.palette[mi.first_bone + b];
				for (uint32_t k = 0; k < frame_meshes->size(); ++k)
					pf.mesh[k].mesh = (*frame_meshes)[k];
				pf.models = 1;
				pf.instances = 1;
				pf.bones = mi.bones;
				pf.meshes = uint32_t(frame_meshes->size());
				ch::publish_opaque(pf);
				ch::publish_see_through(pf);
				ch::cache::publish_palettes(pf, 0, pf.bones);
			};
			// One frame recorded, run and read back (the casters' map too, when asked); between: called after the list is
			// closed and before it runs (a toggle while it's in flight)
			auto render = [&](const mat4 &m, std::vector<uint8_t> &out, std::vector<float> *casters = nullptr, const std::function<void()> &between = nullptr) {
				const uint32_t s = next_seq++;
				pack_frame(s, m);
				alloc->Reset();
				list->Reset(alloc, nullptr);
				list->SetDescriptorHeaps(1, &srvh);
				inj::set_sun(sun_toward);
				list->ClearRenderTargetView(rtv, kSky, 0, nullptr);
				list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
				inj::on_chars_end(list, w, s & 15);
				inj::set_sun(nullptr);
				if (casters != nullptr)
				{
					barrier(list, caster_map, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					list->ClearDepthStencilView(caster_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
					inj::record_casters(list, caster_m, caster_dsv, kCasterSize);
					list->SetDescriptorHeaps(1, &srvh);
					barrier(list, caster_map, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
					D3D12_TEXTURE_COPY_LOCATION cdst = {}, csrc = {};
					cdst.pResource = caster_back;
					cdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					cdst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_TYPELESS, kCasterSize, kCasterSize, 1, kCasterSize * 4 };
					csrc.pResource = caster_map;
					csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					list->CopyTextureRegion(&cdst, 0, 0, 0, &csrc, nullptr);
				}
				barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
				D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
				to.pResource = pic;
				to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				to.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, W, H, 1, W * 4 };
				from.pResource = A;
				from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
				barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				list->Close();
				if (between)
					between();
				direct->ExecuteCommandLists(1, lists);
				direct->Signal(fence, ++fv);
				fence->SetEventOnCompletion(fv, e);
				WaitForSingleObject(e, 60000);
				uint8_t *img = nullptr;
				pic->Map(0, nullptr, reinterpret_cast<void **>(&img));
				out.assign(img, img + size_t(W) * H * 4);
				pic->Unmap(0, nullptr);
				if (casters != nullptr)
				{
					float *cz = nullptr;
					caster_back->Map(0, nullptr, reinterpret_cast<void **>(&cz));
					casters->assign(cz, cz + size_t(kCasterSize) * kCasterSize);
					caster_back->Unmap(0, nullptr);
				}
			};
			// The module's present: the cache's tick, the streamer's present, the glue's frame, then backend::present's
			// retire::on_present (the GPU idle: each frame above is waited for)
			auto pump = [&](int n) {
				for (int i = 0; i < n; ++i)
				{
					pnow += 0.002;
					ch::cache::frame_tick(device, pnow);
					st::on_present();
					cp::frame(pnow, st::epoch());
					hifi::retire::on_present(direct);
					Sleep(1);
				}
			};
			auto pump_settled = [&] {
				for (int i = 0; i < 2000; ++i)
				{
					pump(1);
					if (i > 2 && ch::cache::get_counts().pack_asked == 0)
						return true;
				}
				return false;
			};
			auto open = [&](const std::wstring &folder) {
				const uint64_t n = st::open(folder);
				for (int i = 0; i < 4000 && st::posts_done() < n; ++i)
					Sleep(5);
				return st::current() != nullptr;
			};
			auto quiet_check = [&](const std::string &part) {
				const uint32_t n = debug_messages(info, first);
				check(n == 0 && device->GetDeviceRemovedReason() == S_OK, part + ": the debug layer says nothing (" + std::to_string(n) + (n != 0 ? ": " + first : std::string()) + ")");
			};
			// (the Mithra's pixels: inside its box, not the sky)
			const float eye_far[3] = { centre[0] + 4.0f * (eye[0] - centre[0]), centre[1] + 4.0f * (eye[1] - centre[1]), centre[2] + 4.0f * (eye[2] - centre[2]) };
			const mat4 vp_far = camera(eye_far, centre);
			const rect far_box = project(vp_far, lo, hi, 0.0f);
			auto mithra = [&](const std::vector<uint8_t> &img, const rect &r, size_t i) {
				return r.inside(float(i % W) + 0.5f, float(i / W) + 0.5f, 2) && !is_sky(img.data() + i * 4);
			};
			auto covered = [&](const std::vector<uint8_t> &img, const rect &r) {
				uint32_t n = 0;
				for (size_t i = 0; i < size_t(W) * H; ++i)
					n += mithra(img, r, i);
				return n;
			};
			// (the flat pack's texels have no red: green level 0, blue below it, a mix of the two between levels)
			auto pack_coloured = [](const uint8_t *p) { return int(p[0]) + 24 < int(std::max(p[1], p[2])); };
			auto blue = [](const uint8_t *p) { return int(p[2]) > int(p[0]) + 24 && int(p[2]) > int(p[1]) + 24; };

			// The game's own textures (no pack yet): near, far and the casters' map
			std::vector<uint8_t> p0, p0_far, p1, p1_far;
			std::vector<float> c0, c1;
			render(vp, p0, &c0);
			render(vp_far, p0_far);
			quiet_check("the Mithra again, the game's textures");

			// A hit: the pack's BC2 with the record's levels down to the game's last, through the second pair; a miss (left
			// out) and a no_pack entry: the game's, those groups' pixels unchanged
			std::string pack_why;
			const bool made = make_mithra_pack(here + L"flat\\", textures, left_out, no_pack, true, pack_why);
			const bool started = st::start(device);
			const bool opened = made && open(here + L"flat\\");
			check(made && started && opened, "a made-up pack of the Mithra's keys (one left out, one no_pack; level 0 green, the rest blue), the streamer on this device: opened" +
				(pack_why.empty() ? std::string() : ": " + pack_why));
			{
				pk::modes m;
				m.chars = true;
				cp::set_mode(m);
			}
			const bool flat_settled = pump_settled();
			bool states = true, views = true, levels = true;
			std::string level_text;
			for (size_t i = 0; i < tids.size(); ++i)
			{
				const bool hit = !(keys[i] == left_out) && !(keys[i] == no_pack);
				states = states && std::strcmp(ch::cache::test::pack_state_of(tids[i]), hit ? "ready" : "spent") == 0;
				D3D12_GPU_DESCRIPTOR_HANDLE h = {}, g = {};
				bool is_pack = false, g_pack = true;
				views = views && ch::cache::texture_view_of(tids[i], h, true, &is_pack) && is_pack == hit && ch::cache::texture_view_of(tids[i], g, false, &g_pack) && !g_pack &&
					(h.ptr != g.ptr) == hit;
				if (!hit)
					continue;
				// (the streamer's count: from the record down to the first level at most the game's last, at most its chain)
				const uint32_t game_levels = ch::tex::mip_count(keys[i].w, keys[i].h), dw = keys[i].w >> (game_levels - 1), dh = keys[i].h >> (game_levels - 1);
				const uint32_t stored = pk::bc2::chain_levels(4 * keys[i].w, 4 * keys[i].h);
				uint32_t want = 1;
				while (want < stored && ((4 * keys[i].w) >> (want - 1) > dw || (4 * keys[i].h) >> (want - 1) > dh))
					++want;
				st::view v;
				const bool got = st::get(ch::cache::test::pack_ticket_of(tids[i]), v);
				levels = levels && got && v.format == DXGI_FORMAT_BC2_UNORM && v.w == 4 * keys[i].w && v.h == 4 * keys[i].h && v.levels == want;
				level_text += (level_text.empty() ? "" : ", ") + std::to_string(keys[i].w) + "x" + std::to_string(keys[i].h) + ": " + std::to_string(v.w) + "x" +
					std::to_string(v.h) + " " + std::to_string(v.levels) + " levels (game " + std::to_string(game_levels) + ")";
			}
			const cp::counts pc = cp::get_counts();
			check(flat_settled && states && pc.not_in_pack >= 2, "  " + std::to_string(tids.size() - 2) + " ready, the one left out and the no_pack one spent as not in it: " +
				ch::cache::status().back());
			check(views, "  texture_view_of: the pack's pair (the second) for the hits, the game's for the two, the game's always with the pack refused");
			check(levels, "  the hits' views: BC2 " + level_text);
			const inj::counts before_casters = inj::get_counts();
			render(vp, p1, &c1);
			const inj::counts after_casters = inj::get_counts();
			render(vp_far, p1_far);
			quiet_check("the pack's textures drawn");
			{
				uint32_t changed = 0, unexplained = 0, kept = 0, mithra_px = 0, far_changed = 0, far_blue = 0;
				for (size_t i = 0; i < size_t(W) * H; ++i)
				{
					const uint8_t *a = p0.data() + i * 4, *b = p1.data() + i * 4;
					const bool same = std::memcmp(a, b, 4) == 0;
					mithra_px += mithra(p1, middle, i);
					if (!same)
					{
						changed++;
						unexplained += !pack_coloured(b);
					}
					else
						kept += mithra(p1, middle, i);
					const uint8_t *fa = p0_far.data() + i * 4, *fb = p1_far.data() + i * 4;
					if (std::memcmp(fa, fb, 4) != 0)
					{
						far_changed++;
						far_blue += blue(fb);
					}
				}
				std::snprintf(line, sizeof(line), "  the picture: %u of the Mithra's %u pixels the pack's (green or blue), %u not (%u unchanged: the two kept the game's); %u changed otherwise",
					changed - unexplained, mithra_px, mithra_px - (changed - unexplained), kept, unexplained);
				check(changed > mithra_px / 3 && unexplained == 0 && kept > 100, line);
				std::snprintf(line, sizeof(line), "  four times as far: %u of %u changed pixels blue (a lower level sampled: the view has the levels)", far_blue, far_changed);
				check(far_changed > 100 && far_blue >= far_changed * 9 / 10, line);
				check(c0 == c1 && !c0.empty(), "  the casters' map byte for byte as with the game's textures (the casters keep the game's views)");
				// (the map alone can't tell: the flat records' alpha passes the cut-out test wherever the game's does here)
				check(after_casters.caster_frames == before_casters.caster_frames + 1 && after_casters.caster_pack_draws == before_casters.caster_pack_draws,
					"  the casters drew with none of the pack's views bound (" + std::to_string(after_casters.caster_pack_draws - before_casters.caster_pack_draws) +
						" pack draws in the casters' frame, the pack's " + std::to_string(cp::get_counts().views) + " views ready)");
			}

			// The FSR bias (inject::set_texture_bias, as pack_wiring sets it: -1 at most): closer than the test's distance, the pack's
			// textures sampled a level finer (more of level 0's green); the game's own (compare: every texture the game's)
			// and the casters' map as without it (flag 2 only on the pack's)
			{
				std::vector<uint8_t> pb, p_unbiased, pg_far, pg0_far;
				std::vector<float> cb;
				// (lit and fogged, level 0's green and the lower levels' blue aren't pure: pixels leaning green (green over blue
				// by 8) counted. At the test's own distance the 4x textures are sampled at level 1 and below already, all one
				// blue; closer, from 0.3 to 0.75 of it, level 0 shows, and one level finer shows more of it)
				auto leaning_green = [](const std::vector<uint8_t> &img) {
					uint32_t n = 0;
					for (size_t i = 0; i < size_t(W) * H; ++i)
						n += int(img[i * 4 + 1]) > int(img[i * 4 + 2]) + 8;
					return n;
				};
				int64_t more = 0;
				bool never_less = true;
				std::string by_distance;
				for (const float k : { 0.3f, 0.4f, 0.5f, 0.6f, 0.75f })
				{
					const float eye_k[3] = { centre[0] + k * (eye[0] - centre[0]), centre[1] + k * (eye[1] - centre[1]), centre[2] + k * (eye[2] - centre[2]) };
					const mat4 vp_k = camera(eye_k, centre);
					inj::set_texture_bias(0.0f);
					render(vp_k, p_unbiased);
					inj::set_texture_bias(-1.0f);
					render(vp_k, pb);
					const int64_t d = int64_t(leaning_green(pb)) - int64_t(leaning_green(p_unbiased));
					more += d;
					never_less = never_less && d >= 0;
					by_distance += (by_distance.empty() ? "" : ", ") + std::to_string(d);
				}
				render(vp, pb, &cb);
				cp::set_compare(true);
				render(vp_far, pg_far);
				inj::set_texture_bias(0.0f);
				render(vp_far, pg0_far);
				cp::set_compare(false);
				check(never_less && more > 200, "the FSR bias -1 on the pack's textures: more of level 0's green from 0.3 to 0.75 of the distance (pixels leaning green, more by " +
					by_distance + ")");
				check(pg_far == pg0_far && pg_far == p0_far, "  the game's textures (compare) with the bias: the same picture as without, and as before the pack (no flag 2: their own six levels as drawn)");
				check(cb == c0, "  the casters' map with the bias: byte for byte the game's");
				quiet_check("  the FSR bias");
			}

			// Eviction with a list in flight: the video memory over 95% (set_limits) between a frame's recording and its run,
			// the glue's frame there evicting every texture (the round's bytes past them all); the frame drawn as recorded,
			// then the game's picture; the pressure gone, back once it's been gone kEvictHoldS; the debug layer silent
			{
				cp::test::set_evict_bytes(1ull << 40);
				st::limits full;
				full.vram_usage = 99ull << 20;
				full.vram_budget = 100ull << 20;
				std::vector<uint8_t> pe, pe_after;
				uint32_t evicted_in_flight = 0;
				render(vp, pe, nullptr, [&] {
					st::set_limits(full);
					pnow += 1.0;
					cp::frame(pnow, st::epoch());
					evicted_in_flight = ch::cache::get_counts().pack_evicted;
				});
				render(vp, pe_after);
				std::string w2;
				const bool consistent = ch::cache::test::slots_consistent(w2);
				check(pe == p1 && evicted_in_flight == tids.size() - 2 && pe_after == p0 && consistent,
					"evicted under the pressure while a frame was in flight: " + std::to_string(evicted_in_flight) + " back to the game's; that frame as recorded (the pack's), the next the game's" +
						(w2.empty() ? std::string() : ": " + w2));
				st::set_limits(st::limits {});
				pump(3);
				pnow += cp::kEvictHoldS + 1.0;
				const bool back = pump_settled() && ch::cache::get_counts().pack_ready == tids.size() - 2;
				std::vector<uint8_t> pe_back;
				render(vp, pe_back);
				check(back && pe_back == p1, "  the pressure gone " + std::to_string(int(cp::kEvictHoldS)) + " s: asked again, the pack's picture again");
				cp::test::set_evict_bytes(0);
				quiet_check("  the eviction");
			}

			// Off between a frame's recording and its run, on again, 50 times (every fifth with the copies held on the GPU
			// and let go while held): the frame drawn as recorded (the pack's), no pair twice, the debug layer silent
			{
				ID3D12Fence *gate = nullptr;
				device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate));
				uint64_t gate_value = 0;
				uint32_t same = 0, consistent = 0, ready_again = 0, held_drops = 0;
				std::string bad;
				std::vector<uint8_t> p;
				for (int k = 0; k < 50; ++k)
				{
					render(vp, p, nullptr, [] { cp::clear_all(); });
					same += p == p1;
					std::string w2;
					consistent += ch::cache::test::slots_consistent(w2);
					if (!w2.empty() && bad.empty())
						bad = w2;
					if (k % 5 == 4 && gate != nullptr)
					{
						st::test::gate_copies(gate, ++gate_value);
						pump(3);
						const uint32_t coming = ch::cache::get_counts().pack_asked;
						cp::clear_all();
						held_drops += coming != 0 && ch::cache::get_counts().pack_asked == 0;
						gate->Signal(gate_value);
						st::test::gate_copies(nullptr, 0);
					}
					ready_again += pump_settled() && ch::cache::get_counts().pack_ready == tids.size() - 2;
				}
				if (gate != nullptr)
					gate->Release();
				std::snprintf(line, sizeof(line), "off while a frame is in flight, on again, 50 times: %u frames as recorded, %u with no pair twice, %u ready again, %u let go with the copies held",
					same, consistent, ready_again, held_drops);
				check(same == 50 && consistent == 50 && ready_again == 50 && held_drops == 10, line + (bad.empty() ? std::string() : ": " + bad));
				quiet_check("  the toggles");
			}

			// The game's own picture 4x: the cut-outs (hair, lace) cover what the game's do, near and far (a lower level
			// sampled: the coverage kept in the mips), and the colours (a shine's mask in alpha too) as the game's
			{
				cp::clear_all();
				const bool content = make_mithra_pack(here + L"content\\", textures, pk::key {}, pk::key {}, false, pack_why) && open(here + L"content\\");
				const bool ready = pump_settled() && ch::cache::get_counts().pack_ready == tids.size();
				std::vector<uint8_t> pc_near, pc_far;
				render(vp, pc_near);
				render(vp_far, pc_far);
				const uint32_t n0 = covered(p0, middle), n1 = covered(pc_near, middle), f0 = covered(p0_far, far_box), f1 = covered(pc_far, far_box);
				double mean0 = 0.0, mean1 = 0.0;
				uint32_t both = 0;
				for (size_t i = 0; i < size_t(W) * H; ++i)
					if (mithra(p0, middle, i) && mithra(pc_near, middle, i))
					{
						const uint8_t *a = p0.data() + i * 4, *b = pc_near.data() + i * 4;
						mean0 += 0.3 * a[0] + 0.59 * a[1] + 0.11 * a[2];
						mean1 += 0.3 * b[0] + 0.59 * b[1] + 0.11 * b[2];
						both++;
					}
				mean0 /= std::max(both, 1u);
				mean1 /= std::max(both, 1u);
				check(content && ready, "a made-up pack of the Mithra's own pictures 4x (its cut-outs' mips keeping their coverage): all " + std::to_string(tids.size()) + " ready" +
					(pack_why.empty() ? std::string() : ": " + pack_why));
				std::snprintf(line, sizeof(line), "  the cut-outs' coverage: near %u pixels against the game's %u (%.2f%%), four times as far %u against %u (%.2f%%)", n1, n0,
					100.0 * (double(n1) - n0) / std::max(n0, 1u), f1, f0, 100.0 * (double(f1) - f0) / std::max(f0, 1u));
				check(std::fabs(double(n1) - n0) <= 0.02 * n0 && std::fabs(double(f1) - f0) <= 0.02 * f0 && n0 > 1000, line);
				std::snprintf(line, sizeof(line), "  the colours: brightness %.2f against the game's %.2f over %u pixels (within 2)", mean1, mean0, both);
				check(std::fabs(mean1 - mean0) <= 2.0, line);
				// (per pixel, the sun's highlight on the shine groups (flags 1) by the texture's alpha, kSpec * saturate(2 * t.a):
				// the shine pixels alone, the game's (compare) against the pack's, so a mask lost in the pack's alpha shows)
				{
					inj::set_options(true, getenv("CHARS_DETAIL") != nullptr);
					inj::set_sun(sun_toward);
					std::vector<uint8_t> ps_game, ps_pack;
					cp::set_compare(true);
					render(vp, ps_game);
					cp::set_compare(false);
					render(vp, ps_pack);
					inj::set_options(getenv("CHARS_PERPIXEL") != nullptr, getenv("CHARS_DETAIL") != nullptr);
					inj::set_sun(getenv("CHARS_PERPIXEL") != nullptr ? toward_sun : nullptr);
					// (over the pixels all four pictures cover: the highlight in it, a mask lost or doubled moves it)
					double hl_game = 0.0, hl_pack = 0.0;
					uint32_t lit_px = 0;
					for (size_t i = 0; i < size_t(W) * H; ++i)
						if (mithra(p0, middle, i) && mithra(pc_near, middle, i) && mithra(ps_game, middle, i) && mithra(ps_pack, middle, i))
						{
							const uint8_t *a = ps_game.data() + i * 4, *b = ps_pack.data() + i * 4;
							hl_game += 0.3 * a[0] + 0.59 * a[1] + 0.11 * a[2];
							hl_pack += 0.3 * b[0] + 0.59 * b[1] + 0.11 * b[2];
							lit_px++;
						}
					hl_game /= std::max(lit_px, 1u);
					hl_pack /= std::max(lit_px, 1u);
					std::snprintf(line, sizeof(line), "  per pixel (the shine groups' highlight by the texture's alpha): brightness %.2f against the game's %.2f over %u pixels (within 2)",
						hl_pack, hl_game, lit_px);
					check(lit_px > 1000 && std::fabs(hl_pack - hl_game) <= 2.0, line);
				}
				quiet_check("  the Mithra's own pictures");
			}

			// M9 (docs/texture-pack/characters.md "Pack relief"): a pack of the Mithra's own pictures with heights, a third a
			// colour and a height, a third a height alone (the game's colour, the pack's relief), a third a colour alone.
			// Heights off: none asked, none drawn. On: each asked once, the pair's detail descriptor the height (flags 4, its
			// relief scale); per vertex the picture as without; per pixel it changes, the light on the whole within 3%; compare
			// shows none; off again byte for byte the picture without, on again the same as before
			{
				cp::clear_all();
				std::vector<int> kinds;
				const bool hmade = make_mithra_pack(here + L"heights\\", textures, pk::key {}, pk::key {}, false, pack_why, &kinds) && open(here + L"heights\\");
				uint32_t with = 0, alone = 0, colour_only = 0;
				for (int k : kinds)
					(k == 0 ? with : k == 1 ? alone : colour_only)++;
				check(hmade && kinds.size() == tids.size() && with != 0 && alone != 0, "M9: a made-up pack of the Mithra's own pictures with heights: " + std::to_string(with) +
					" with a colour and a height, " + std::to_string(alone) + " a height alone, " + std::to_string(colour_only) + " a colour alone" +
					(pack_why.empty() ? std::string() : ": " + pack_why));
				const bool detail = getenv("CHARS_DETAIL") != nullptr;
				inj::set_sun(sun_toward);
				pk::modes hm;
				hm.chars = true;
				hm.heights = false;
				cp::set_mode(hm);
				const uint64_t asked_before = cp::get_counts().heights_asked;
				const bool off_settled = pump_settled();
				const ch::cache::counts off_counts = ch::cache::get_counts();
				std::vector<uint8_t> h_off, v_off, c_off;
				inj::set_options(true, detail);
				render(vp, h_off);
				cp::set_compare(true);
				render(vp, c_off);
				cp::set_compare(false);
				inj::set_options(false, detail);
				render(vp, v_off);
				check(off_settled && cp::get_counts().heights_asked == asked_before && off_counts.pack_relief == 0 && off_counts.pack_heights_asked == 0 &&
					off_counts.pack_ready == with + colour_only, "  heights off: none asked, none drawn, the colours ready (" + std::to_string(off_counts.pack_ready) + ")");
				// (on: every pack part let go and asked again, the heights beside the colours)
				hm.heights = true;
				cp::set_mode(hm);
				const bool on_settled = pump_settled();
				const ch::cache::counts on_counts = ch::cache::get_counts();
				bool relief_views = true;
				std::string relief_text;
				for (size_t i = 0; i < tids.size(); ++i)
				{
					D3D12_GPU_DESCRIPTOR_HANDLE h = {};
					bool is_pack = false;
					float relief = -1.0f;
					const bool got = ch::cache::texture_view_of(tids[i], h, true, &is_pack, &relief);
					// (4 x span x the map's texels a game texel: 2x)
					const float want = kinds[i] != 2 ? 4.0f * kHeightSpan * 2.0f : 0.0f;
					const bool ok = got && is_pack == (kinds[i] != 1) && std::fabs(relief - want) < 1e-6f;
					relief_views = relief_views && ok;
					if (!ok && relief_text.empty())
						relief_text = ": texture " + std::to_string(i) + " (kind " + std::to_string(kinds[i]) + ") is_pack " + std::to_string(is_pack) + ", relief " + std::to_string(relief);
					float casters_relief = -1.0f;
					bool casters_pack = true;
					relief_views = relief_views && ch::cache::texture_view_of(tids[i], h, false, &casters_pack, &casters_relief) && !casters_pack && casters_relief == 0.0f;
				}
				std::snprintf(line, sizeof(line), "  heights on: %llu asked, %u pairs with the pack's relief (%u on the game's colour), %u colours from the pack",
					static_cast<unsigned long long>(cp::get_counts().heights_asked - asked_before), on_counts.pack_relief, on_counts.pack_relief_only, on_counts.pack_ready);
				check(on_settled && cp::get_counts().heights_asked - asked_before == with + alone && on_counts.pack_relief == with + alone && on_counts.pack_relief_only == alone &&
					on_counts.pack_ready == with + colour_only && on_counts.pack_heights_asked == 0, line);
				check(relief_views, "  texture_view_of: the relief scale (4 x span x 2) on exactly the textures with a height, the colour the pack's but for the heights alone; none with the pack refused (the casters)" + relief_text);
				std::vector<uint8_t> v_on, h_on, c_on;
				render(vp, v_on);
				check(v_on == v_off, "  per vertex: the picture byte for byte as with heights off (no relief without per-pixel lighting)");
				inj::set_options(true, detail);
				const uint64_t draws_before = inj::get_counts().relief_draws;
				render(vp, h_on);
				const uint64_t relief_draws = inj::get_counts().relief_draws - draws_before;
				{
					uint32_t changed = 0, both = 0;
					double l_off = 0.0, l_on = 0.0;
					for (size_t i = 0; i < size_t(W) * H; ++i)
					{
						const uint8_t *a = h_off.data() + i * 4, *b = h_on.data() + i * 4;
						if (!mithra(h_off, middle, i) || !mithra(h_on, middle, i))
							continue;
						both++;
						changed += std::memcmp(a, b, 4) != 0;
						l_off += 0.3 * a[0] + 0.59 * a[1] + 0.11 * a[2];
						l_on += 0.3 * b[0] + 0.59 * b[1] + 0.11 * b[2];
					}
					const double ratio = l_off > 0.0 ? l_on / l_off : 0.0;
					std::snprintf(line, sizeof(line), "  per pixel: %llu draws with relief, %u of the Mithra's %u pixels changed, the light on the whole %.2f%% of it (within 3%%)",
						static_cast<unsigned long long>(relief_draws), changed, both, 100.0 * ratio);
					check(relief_draws != 0 && changed > both / 50 && std::fabs(ratio - 1.0) <= 0.03, line);
				}
				cp::set_compare(true);
				render(vp, c_on);
				cp::set_compare(false);
				check(c_on == c_off, "  compare: the game's pairs, no relief (byte for byte as with heights off)");
				// (off again: every pair written again with the detail map's view, the picture as without heights)
				hm.heights = false;
				cp::set_mode(hm);
				const bool off_again = pump_settled();
				std::vector<uint8_t> h_off2, h_on2;
				render(vp, h_off2);
				check(off_again && h_off2 == h_off && ch::cache::get_counts().pack_relief == 0, "  off again: byte for byte the picture with heights off, no pair with relief");
				hm.heights = true;
				cp::set_mode(hm);
				const bool on_again = pump_settled();
				render(vp, h_on2);
				std::string bad;
				check(on_again && h_on2 == h_on && ch::cache::test::slots_consistent(bad), "  on again: the same picture with relief; no pair twice" + (bad.empty() ? std::string() : ": " + bad));
				cp::clear_all();
				const ch::cache::counts cleared = ch::cache::get_counts();
				bool no_height_tickets = true;
				for (uint32_t t : tids)
					no_height_tickets = no_height_tickets && ch::cache::test::pack_height_ticket_of(t).generation == 0;
				check(cleared.pack_relief == 0 && cleared.pack_heights_asked == 0 && no_height_tickets, "  clear_all: every height let go");
				hm.heights = false;
				cp::set_mode(hm);
				inj::set_options(getenv("CHARS_PERPIXEL") != nullptr, detail);
				inj::set_sun(getenv("CHARS_PERPIXEL") != nullptr ? toward_sun : nullptr);
				quiet_check("  the heights");
			}

			// The real pack (read only; skipped without it): the Mithra's original bytes' keys in it; the overlay's copies'
			// (the game's bytes while hifi-textures is listed) none, and the line says pack_why
			{
				cp::clear_all();
				// (CHARS_PACK_HEIGHT=0/1: the pictures' pack with its heights off or on, M9's A/B; HIFIXI_PACK: another pack)
				if (const char *ph = getenv("CHARS_PACK_HEIGHT"); ph != nullptr)
				{
					pk::modes rm;
					rm.chars = true;
					rm.heights = std::atoi(ph) != 0;
					cp::set_mode(rm);
				}
				const std::wstring real = pk::folder_in(L"C:\\Games\\PhoenixXI\\bootloader");
				const std::vector<dat_chunk> overlay = dats != nullptr ? read_dats(dats, true) : read_dat_at("C:\\Games\\PhoenixXI\\polplugins\\DATs\\hifi-textures\\ROM2\\0\\52.DAT");
				// (CHARS_PICTURES=<folder>: the game's textures, the real pack's and the overlay's copies drawn whole and close
				// up, as BMPs there: for a look, no check reads them)
				const char *pictures = getenv("CHARS_PICTURES");
				const float close_at[3] = { centre[0], lo[1] + 0.3f * (hi[1] - lo[1]), centre[2] };
				const float eye_close[3] = { close_at[0] + 0.3f * (eye[0] - centre[0]), close_at[1] + 0.3f * (eye[1] - centre[1]), close_at[2] + 0.3f * (eye[2] - centre[2]) };
				const mat4 vp_close = camera(eye_close, close_at);
				auto picture = [&](const char *name) {
					if (pictures == nullptr)
						return;
					std::vector<uint8_t> whole, close;
					render(vp, whole);
					render(vp_close, close);
					write_bmp(std::string(pictures) + "\\" + name + ".bmp", whole);
					write_bmp(std::string(pictures) + "\\" + name + "-close.bmp", close);
					std::printf("      (pictures: %s\\%s.bmp, %s-close.bmp)\n", pictures, name, name);
				};
				picture("game");
				if (GetFileAttributesW((real + L"pack.idx").c_str()) == INVALID_FILE_ATTRIBUTES)
					std::printf("      the real pack: skipped (none at %ls)\n", real.c_str());
				else if (!open(real))
					check(false, "the real pack opened (read only)");
				else
				{
					const bool settled_real = pump_settled();
					picture("pack");
					uint32_t hits = 0, misses = 0;
					for (uint32_t t : tids)
					{
						hits += std::strcmp(ch::cache::test::pack_state_of(t), "ready") == 0;
						misses += std::strcmp(ch::cache::test::pack_state_of(t), "spent") == 0;
					}
					check(settled_real && hits != 0 && hits + misses == tids.size(), "the real pack (read only): " + std::to_string(hits) + " of the Mithra's " +
						std::to_string(tids.size()) + " textures, keyed from its original file's bytes, from the pack (" + std::to_string(misses) + " not in it)");
					if (overlay.empty())
						std::printf("      the overlay's copy: skipped (no hifi-textures\\ROM2\\0\\52.DAT)\n");
					else
					{
						const cp::counts before = cp::get_counts();
						const loaded O = load_into_cache(overlay);
						ch::cache::frame_tick(device, pnow);
						ch::cache::settle(120000);
						for (int i = 0; i < 400 && ch::cache::get_counts().meshes_ready < P.meshes.size() + O.meshes.size(); ++i)
							Sleep(50);
						const bool settled_overlay = pump_settled();
						frame_meshes = &O.meshes;
						picture("overlay");
						frame_meshes = &P.meshes;
						const cp::counts after = cp::get_counts();
						std::vector<uint32_t> otids;
						for (uint32_t id : O.meshes)
							if (ch::cache::mesh_view mv; ch::cache::mesh_view_of(id, mv))
								for (uint32_t t : mv.textures)
									if (t != ch::cache::kNone && std::find(otids.begin(), otids.end(), t) == otids.end())
										otids.push_back(t);
						uint32_t spent = 0;
						for (uint32_t t : otids)
							spent += ch::cache::test::pack_key_of(t).bytes != 0 && std::strcmp(ch::cache::test::pack_state_of(t), "spent") == 0;
						check(settled_overlay && !otids.empty() && after.not_in_pack - before.not_in_pack >= otids.size() && spent == otids.size() && after.views == before.views,
							"  the overlay's copies of its " + std::to_string(otids.size()) + " textures (what the game holds while it's listed): every key missed, not in the pack");
						s_overlay = true;
						cp::clear_all();
						std::vector<std::string> out;
						cp::lines(out);
						check(!out.empty() && out[0].find("the hifi-textures overlay is still installed") != std::string::npos, "  the line: " + (out.empty() ? std::string() : out[0]));
						s_overlay = false;
					}
				}
				quiet_check("  the real pack");
			}
			cp::clear_all();
			st::stop();
			pump(4);
			pic->Release();
		}
	}
	if (argc > 2)
	{
		FILE *fo = nullptr;
		if (fopen_s(&fo, argv[2], "wb") == 0 && fo != nullptr)
		{
			uint8_t head[54] = { 'B', 'M' };
			auto put = [&](int at, uint32_t v) { std::memcpy(head + at, &v, 4); };
			put(2, 54 + W * H * 4);
			put(10, 54);
			put(14, 40);
			put(18, W);
			put(22, uint32_t(-int32_t(H)));
			head[26] = 1;
			head[28] = 32;
			fwrite(head, 1, 54, fo);
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const uint8_t bgra[4] = { px[i * 4 + 2], px[i * 4 + 1], px[i * 4 + 0], 255 };
				fwrite(bgra, 1, 4, fo);
			}
			fclose(fo);
		}
	}
	inj::release();
	ch::cache::stop();
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
