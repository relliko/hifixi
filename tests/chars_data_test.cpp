/*
 * Offline test of the native character renderer's decoders (src/chars/os2, src/chars/tex) against
 * research/ffxi-re/tools/chars.py and chartex.py, through tests/chars_oracle.py's text (made first: build.cmd runs it):
 *   - every mesh of the oracle's DATs: the counts, each vertex's bones and floats, each triangle's corners and UVs (the
 *     non-mirrored half), its texture's name and its material; the mirrored half: the same triangles, two corners
 *     swapped, marked mirrored;
 *   - the same meshes as the game holds them loaded (section offsets made pointers, a drawn texture command rewritten
 *     0x8000 -> 0x8001 with the texture object in the name's first dword): the same mesh, the object read;
 *   - every texture: its size and an FNV-1a hash of its RGBA rows; the mips: 6 levels down by halves (1 under 32);
 *   - the hide groups a header byte 3 switches on.
 *
 *   build\chars_data_test.exe <oracle.txt>
 */

#include "../src/chars/os2.hpp"
#include "../src/chars/tex.hpp"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		if (!ok || what.rfind("  ", 0) != 0)
			std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	const char *kGame = "C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\";

	struct chunk
	{
		uint8_t name[4];
		uint32_t kind;
		std::vector<uint8_t> data;
	};
	std::vector<chunk> read_chunks(const std::string &rel)
	{
		std::string path = kGame + rel;
		for (char &c : path)
			if (c == '/')
				c = '\\';
		std::ifstream f(path, std::ios::binary);
		std::vector<uint8_t> dat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::vector<chunk> out;
		size_t off = 0;
		while (off + 16 <= dat.size())
		{
			uint32_t w;
			std::memcpy(&w, dat.data() + off + 4, 4);
			const size_t size = size_t((w >> 7) & 0x7FFFF) * 16;
			if (size < 16)
				break;
			chunk c;
			std::memcpy(c.name, dat.data() + off, 4);
			c.kind = w & 0x7F;
			const size_t end = std::min(off + size, dat.size());
			c.data.assign(dat.begin() + off + 16, dat.begin() + end);
			out.push_back(std::move(c));
			off += size;
		}
		return out;
	}

	std::string hex(const uint8_t *p, size_t n)
	{
		std::string s;
		char b[3];
		for (size_t i = 0; i < n; ++i)
		{
			std::snprintf(b, sizeof(b), "%02x", p[i]);
			s += b;
		}
		return s;
	}
	uint32_t bits(float f)
	{
		uint32_t u;
		std::memcpy(&u, &f, 4);
		return u;
	}
	uint64_t fnv(const std::vector<uint8_t> &b)
	{
		uint64_t h = 0xCBF29CE484222325ull;
		for (uint8_t x : b)
		{
			h ^= x;
			h *= 0x100000001B3ull;
		}
		return h;
	}

	// One oracle mesh's lines
	struct oracle_mesh
	{
		std::string dat, name;
		int index = 0;
		bool error = false;
		uint32_t n1 = 0, n2 = 0, tris = 0;
		bool mirrored = false;
		std::vector<std::string> v, t;
	};

	// The non-mirrored half's triangles of a decoded mesh, as the oracle writes them
	std::vector<std::string> triangles_of(const hifi::chars::os2::mesh &m)
	{
		std::vector<std::string> out;
		for (const auto &g : m.groups)
		{
			if (g.mirrored)
				continue;
			for (uint32_t k = 0; k < g.count; k += 3)
			{
				const auto *c = &m.corners[g.first + k];
				char line[512];
				int n = std::snprintf(line, sizeof(line), "t %u %u %u %08X %08X %08X %08X %08X %08X ", c[0].vertex, c[1].vertex, c[2].vertex, bits(c[0].u), bits(c[0].v),
					bits(c[1].u), bits(c[1].v), bits(c[2].u), bits(c[2].v));
				std::string s(line, n);
				s += g.texture < 0 ? "-" : hex(m.textures[g.texture].name, 16);
				if (g.has_material)
				{
					float light;
					std::memcpy(&light, &g.material[4], 4);
					std::snprintf(line, sizeof(line), " %08X %u %08X", g.material[0], unsigned(g.kind()), bits(light));
					s += line;
				}
				else
					s += " -";
				out.push_back(s);
			}
		}
		return out;
	}
	std::vector<std::string> vertices_of(const hifi::chars::os2::mesh &m)
	{
		std::vector<std::string> out;
		for (uint32_t i = 0; i < m.vertices.size(); ++i)
		{
			const auto &v = m.vertices[i];
			char line[512];
			int n;
			if (i < m.one_bone)
				n = std::snprintf(line, sizeof(line), "v %u %u %u %u %u %u %08X %08X %08X %08X %08X %08X", v.bone[0], v.mirror_bone[0], v.axis[0], v.bone[1], v.mirror_bone[1],
					v.axis[1], bits(v.p0[0]), bits(v.p0[1]), bits(v.p0[2]), bits(v.n0[0]), bits(v.n0[1]), bits(v.n0[2]));
			else
				n = std::snprintf(line, sizeof(line), "v %u %u %u %u %u %u %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X", v.bone[0], v.mirror_bone[0],
					v.axis[0], v.bone[1], v.mirror_bone[1], v.axis[1], bits(v.p0[0]), bits(v.p1[0]), bits(v.p0[1]), bits(v.p1[1]), bits(v.p0[2]), bits(v.p1[2]), bits(v.w0),
					bits(v.w1), bits(v.n0[0]), bits(v.n1[0]), bits(v.n0[1]), bits(v.n1[1]), bits(v.n0[2]), bits(v.n1[2]));
			out.push_back(std::string(line, n));
		}
		return out;
	}

	std::string first_difference(const std::vector<std::string> &a, const std::vector<std::string> &b)
	{
		if (a.size() != b.size())
			return "counts " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
		for (size_t i = 0; i < a.size(); ++i)
			if (a[i] != b[i])
				return "#" + std::to_string(i) + ": [" + a[i] + "] vs oracle [" + b[i] + "]";
		return "";
	}

	// The chunk as the game holds it loaded at `base`: section offsets made pointers; the first texture command drawn
	void make_live(std::vector<uint8_t> &d, uint32_t base, uint32_t object)
	{
		hifi::chars::os2::sections s;
		hifi::chars::os2::sections_of_chunk(d.data(), d.size(), s);
		for (int k = 0; k < 6; ++k)
		{
			const uint32_t p = base + s.at[k];
			std::memcpy(d.data() + 6 + 6 * k, &p, 4);
		}
		hifi::chars::os2::mesh m;
		std::string why;
		if (hifi::chars::os2::decode(d.data(), d.size(), [&] {
				hifi::chars::os2::sections t;
				hifi::chars::os2::sections_of_live(d.data(), d.size(), base, t);
				return t;
			}(), m, why) && !m.textures.empty())
		{
			const uint32_t at = m.textures[0].at;
			d[at] |= 1;
			std::memcpy(d.data() + at + 2, &object, 4);
		}
	}
}

int main(int argc, char **argv)
{
	namespace os2 = hifi::chars::os2;
	namespace tex = hifi::chars::tex;
	if (argc < 2)
	{
		std::printf("usage: chars_data_test <oracle.txt>\n");
		return 2;
	}
	std::ifstream in(argv[1]);
	std::string line;
	std::vector<oracle_mesh> meshes;
	std::vector<std::string> textures;
	while (std::getline(in, line))
	{
		if (line.rfind("mesh ", 0) == 0)
		{
			oracle_mesh m;
			std::istringstream s(line.substr(5));
			std::string rest;
			s >> m.dat >> m.index >> m.name >> rest;
			if (rest == "error")
				m.error = true;
			else
			{
				m.n1 = uint32_t(std::stoul(rest));
				int mir = 0;
				s >> m.n2 >> mir >> m.tris;
				m.mirrored = mir != 0;
			}
			meshes.push_back(m);
		}
		else if (line.rfind("v ", 0) == 0 && !meshes.empty())
			meshes.back().v.push_back(line);
		else if (line.rfind("t ", 0) == 0 && !meshes.empty())
			meshes.back().t.push_back(line);
		else if (line.rfind("tex ", 0) == 0)
			textures.push_back(line);
	}
	check(meshes.size() > 20 && textures.size() > 10, "oracle read: " + std::to_string(meshes.size()) + " meshes, " + std::to_string(textures.size()) + " textures");

	std::map<std::string, std::vector<chunk>> dats;
	auto chunks_of = [&](const std::string &rel) -> const std::vector<chunk> & {
		auto it = dats.find(rel);
		if (it == dats.end())
			it = dats.emplace(rel, read_chunks(rel)).first;
		return it->second;
	};

	int mesh_ok = 0, live_ok = 0, mirrored_ok = 0;
	for (const oracle_mesh &om : meshes)
	{
		const auto &ch = chunks_of(om.dat);
		const std::string what = om.dat + " #" + std::to_string(om.index);
		if (om.index >= int(ch.size()))
		{
			check(false, what + ": no such chunk");
			continue;
		}
		const chunk &c = ch[om.index];
		os2::sections s;
		os2::mesh m;
		std::string why;
		const bool ok = os2::sections_of_chunk(c.data.data(), c.data.size(), s) && os2::decode(c.data.data(), c.data.size(), s, m, why);
		if (om.error)
		{
			check(!ok, "  " + what + ": the oracle can't read it, nor can we (" + why + ")");
			mesh_ok += !ok;
			live_ok += !ok;
			continue;
		}
		if (!ok)
		{
			check(false, what + ": decode failed: " + why);
			continue;
		}
		bool good = m.one_bone == om.n1 && m.two_bone == om.n2 && m.mirrored == om.mirrored;
		const std::string dv = first_difference(vertices_of(m), om.v), dt = first_difference(triangles_of(m), om.t);
		check(good && dv.empty() && dt.empty(), "  " + what + " " + om.name + ": " + std::to_string(om.n1) + "+" + std::to_string(om.n2) + " vertices, " +
			std::to_string(om.tris) + " triangles as chars.py" + (dv.empty() ? "" : "; vertices " + dv) + (dt.empty() ? "" : "; triangles " + dt));
		mesh_ok += good && dv.empty() && dt.empty();

		// The mirrored half
		if (m.mirrored)
		{
			bool same = true;
			size_t plain = 0, mirror = 0;
			std::vector<const os2::group *> p, q;
			for (const auto &g : m.groups)
				(g.mirrored ? q : p).push_back(&g);
			same = p.size() == q.size();
			for (size_t i = 0; same && i < p.size(); ++i)
			{
				plain += p[i]->count;
				mirror += q[i]->count;
				same = p[i]->count == q[i]->count && p[i]->texture == q[i]->texture;
				for (uint32_t k = 0; same && k < p[i]->count; k += 3)
				{
					const auto *a = &m.corners[p[i]->first + k], *b = &m.corners[q[i]->first + k];
					same = b[0].vertex == (a[1].vertex | os2::kMirrored) && b[1].vertex == (a[0].vertex | os2::kMirrored) && b[2].vertex == (a[2].vertex | os2::kMirrored) &&
						b[0].u == a[1].u && b[1].v == a[0].v;
				}
			}
			check(same && plain == mirror, "  " + what + ": the mirrored half, two corners swapped");
			mirrored_ok += same;
		}

		// As the game holds it loaded
		std::vector<uint8_t> live = c.data;
		const uint32_t base = 0x2A000030, object = 0x0BADF00D;
		make_live(live, base, object);
		os2::sections ls;
		os2::mesh lm;
		const bool lok = os2::sections_of_live(live.data(), live.size(), base, ls) && os2::decode(live.data(), live.size(), ls, lm, why);
		bool lsame = lok && vertices_of(lm) == vertices_of(m) && lm.corners.size() == m.corners.size() && lm.groups.size() == m.groups.size();
		if (lsame && !lm.textures.empty())
			lsame = lm.textures[0].cached && lm.textures[0].object == object && std::memcmp(lm.textures[0].name + 4, m.textures[0].name + 4, 12) == 0 &&
				(lm.textures.size() < 2 || !lm.textures[1].cached);
		check(lsame, "  " + what + ": the same as the game holds it loaded" + (lok ? "" : ": " + why));
		live_ok += lsame;
	}
	check(mesh_ok == int(meshes.size()), "meshes as chars.py: " + std::to_string(mesh_ok) + " of " + std::to_string(meshes.size()) + " (mirrored halves " +
		std::to_string(mirrored_ok) + ")");
	check(live_ok == int(meshes.size()), "the same meshes as the game holds them loaded: " + std::to_string(live_ok));

	int tex_ok = 0, none = 0;
	for (const std::string &l : textures)
	{
		std::istringstream s(l.substr(4));
		std::string dat, name, rest;
		int index = 0;
		s >> dat >> index >> name >> rest;
		const auto &ch = chunks_of(dat);
		const chunk &c = ch[index];
		tex::texture t;
		std::string why;
		const bool ok = tex::decode(c.data.data(), c.data.size(), t, why);
		if (rest == "none")
		{
			check(!ok, "  " + dat + " #" + std::to_string(index) + ": chartex.py has no picture, nor have we");
			tex_ok += !ok;
			none++;
			continue;
		}
		uint32_t w = uint32_t(std::stoul(rest)), h = 0;
		std::string hash;
		s >> h >> hash;
		char ours[17];
		std::snprintf(ours, sizeof(ours), "%016llX", ok ? static_cast<unsigned long long>(fnv(t.levels[0].rgba)) : 0ull);
		const bool good = ok && t.w == w && t.h == h && hash == ours;
		check(good, "  " + dat + " #" + std::to_string(index) + " " + name + ": " + std::to_string(w) + "x" + std::to_string(h) + (good ? "" : " ours " + std::string(ours) + " vs " + hash + " " + why));
		tex_ok += good;
		if (good)
		{
			tex::make_mips(t);
			const uint32_t want = tex::mip_count(w, h);
			bool mips = t.levels.size() == want;
			for (size_t k = 1; mips && k < t.levels.size(); ++k)
				mips = t.levels[k].w == std::max(t.levels[k - 1].w / 2, 1u) && t.levels[k].rgba.size() == size_t(t.levels[k].w) * t.levels[k].h * 4;
			check(mips, "  " + std::to_string(want) + " mip levels");
		}
	}
	check(tex_ok == int(textures.size()), "textures as chartex.py: " + std::to_string(tex_ok) + " of " + std::to_string(textures.size()) + " (" + std::to_string(none) + " with nothing to show)");

	check(os2::hidden_by(2) == 2u && os2::hidden_by(5) == (2u | 4u | 8u | 16u | 256u) && os2::hidden_by(0x12) == 32u && os2::hidden_by(0x22) == 128u &&
			os2::hidden_by(0x32) == 64u && os2::hidden_by(0x11) == 0u && os2::hidden_by(1) == 0u,
		"hide groups: 2 -> 1; 5 -> 1-4, 8; 0x12 -> 5; 0x22 -> 7; 0x32 -> 6; 0x11, 1 -> none");

	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
