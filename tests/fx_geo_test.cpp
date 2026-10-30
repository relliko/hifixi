/*
 * Offline test of the effects' geometry decoders (src/fx/fxgeo) against research/ffxi-re/tools/effects_geo.py, through
 * tests/fx_oracle.py geo: every D3M (0x1F), D3A (0x21) and morph (0x25) chunk of the DATs it read, with its counts,
 * its runs as 040220 draws them (first vertex, vertices, texture name), and the sums of its positions, normals, UVs and
 * colours (the colours as the DAT has them).
 *
 *   build\fx_geo_test.exe build\test\fx_geo.txt
 */

#include "../src/fx/fxgeo.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	namespace geo = hifi::fx::geo;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		if (!ok)
		{
			std::printf("FAIL  %s\n", what.c_str());
			failures++;
		}
		else
			passes++;
	}

	const char *kFfxi = "C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\";

	std::map<std::string, std::vector<uint8_t>> g_dats;
	const std::vector<uint8_t> &dat(const std::string &rel)
	{
		auto it = g_dats.find(rel);
		if (it != g_dats.end())
			return it->second;
		std::string path = std::string(kFfxi) + rel;
		for (char &c : path)
			if (c == '/')
				c = '\\';
		std::vector<uint8_t> bytes;
		std::ifstream f(path, std::ios::binary);
		if (f)
			bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		return g_dats[rel] = bytes;
	}

	std::string hex(const std::string &s)
	{
		std::string out;
		char b[3];
		for (const unsigned char c : s)
		{
			std::snprintf(b, sizeof(b), "%02x", c);
			out += b;
		}
		return out;
	}

	double sum3(const hifi::fx::vertex *v, size_t n, int what)
	{
		double s = 0.0;
		for (size_t i = 0; i < n; ++i)
		{
			const float *p = what == 0 ? v[i].pos : what == 1 ? v[i].normal : v[i].uv;
			for (int k = 0; k < (what == 2 ? 2 : 3); ++k)
				if (std::isfinite(p[k]))
					s += p[k];
		}
		return s;
	}

	bool close(double a, double b)
	{
		return std::fabs(a - b) <= 2e-5 * std::fmax(1.0, std::fabs(b)) + 1e-3;
	}
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const char *path = argc > 1 ? argv[1] : "build\\test\\fx_geo.txt";
	std::ifstream in(path);
	if (!in)
	{
		std::printf("FAIL  %s not there (python tests\\fx_oracle.py geo ...)\n", path);
		return 1;
	}
	std::string line;
	uint32_t chunks[3] = {};
	while (std::getline(in, line))
	{
		std::istringstream s(line);
		std::string tag, rel;
		size_t off = 0;
		s >> tag >> rel >> off;
		const std::vector<uint8_t> &d = dat(rel);
		if (d.empty() || off + 16 > d.size())
		{
			check(false, rel + " read");
			continue;
		}
		const uint32_t header = *reinterpret_cast<const uint32_t *>(&d[off + 4]);
		const size_t size = ((header >> 7) & 0x7FFFF) * 16;
		const uint8_t *data = d.data() + off + 16;
		const size_t n = size > 16 && off + size <= d.size() ? size - 16 : 0;
		const std::string where = rel + " @" + std::to_string(off);
		std::string why;
		if (tag == "bad")
		{
			// (the oracle refused it: so should we, or at least not crash)
			continue;
		}
		uint32_t kind = 0;
		std::string name;
		s >> kind >> name;
		if (kind == 31)
		{
			chunks[0]++;
			uint32_t k = 0, tris = 0, texs = 0;
			double ps = 0, ns = 0, us = 0;
			unsigned long long cs = 0;
			std::string runs;
			s >> k >> tris >> texs >> ps >> ns >> us >> cs >> runs;
			geo::d3m m;
			if (!geo::parse_d3m(data, n, m, why, false))
			{
				check(false, where + " D3M parses: " + why);
				continue;
			}
			unsigned long long c = 0;
			for (const auto &v : m.vertices)
				c += v.colour;
			std::string mine;
			for (const geo::d3m_run &r : m.runs)
			{
				if (!mine.empty())
					mine += ' ';
				mine += std::to_string(r.first) + ":" + std::to_string(r.count) + ":" + (r.texture < 0 ? std::string("-") : hex(m.names[size_t(r.texture)]));
			}
			std::string theirs = runs;
			for (std::string w; s >> w;)
				theirs += " " + w;
			check(m.kind == k && m.triangles == tris && m.textures == texs, where + " D3M counts");
			check(close(sum3(m.vertices.data(), m.vertices.size(), 0), ps) && close(sum3(m.vertices.data(), m.vertices.size(), 1), ns) &&
				close(sum3(m.vertices.data(), m.vertices.size(), 2), us), where + " D3M positions, normals, UVs");
			check(c == cs, where + " D3M colours");
			check(mine == theirs, where + " D3M runs: " + mine + " vs " + theirs);
		}
		else if (kind == 33)
		{
			chunks[1]++;
			int units = 0, frames = 0, flare = 0, count = 0;
			s >> units >> frames >> flare >> count;
			geo::d3a a;
			if (!geo::parse_d3a(data, n, a, why))
			{
				check(false, where + " D3A parses: " + why);
				continue;
			}
			check(a.units == units && a.frames == frames && int(a.flare) == flare && int(a.frame_list.size()) == count, where + " D3A counts");
			for (int i = 0; i < count && i < int(a.frame_list.size()); ++i)
			{
				std::string w;
				s >> w;
				size_t nv = 0;
				double ps = 0, us = 0;
				unsigned long long cs = 0;
				sscanf_s(w.c_str(), "%zu:%lf:%lf:%llu", &nv, &ps, &us, &cs);
				const auto &f = a.frame_list[size_t(i)];
				unsigned long long c = 0;
				for (const auto &v : f.vertices)
					c += v.colour;
				check(f.vertices.size() == nv && close(sum3(f.vertices.data(), nv, 0), ps) && close(sum3(f.vertices.data(), nv, 2), us) && c == cs,
					where + " D3A frame " + std::to_string(i));
			}
		}
		else if (kind == 37)
		{
			chunks[2]++;
			uint32_t k = 0, p = 0, nn = 0, t = 0;
			double ps = 0, ns = 0, us = 0;
			unsigned long long cs = 0;
			s >> k >> p >> nn >> t >> ps >> ns >> us >> cs;
			geo::morph m;
			if (!geo::parse_morph(data, n, m, why, false))
			{
				check(false, where + " morph parses: " + why);
				continue;
			}
			check(m.targets == k && m.positions == p && m.normals == nn && m.triangles == t, where + " morph counts");
			double a = 0, b = 0, u = 0;
			for (uint32_t i = 0; i < p * 3; ++i)
				a += m.pos[i];
			for (uint32_t i = 0; i < nn * 3; ++i)
				b += m.nrm[size_t(k - 1) * nn * 3 + i];
			for (const float x : m.uv)
				if (std::isfinite(x))
					u += x;
			unsigned long long c = 0;
			for (const uint32_t x : m.colours)
				c += x;
			check(close(a, ps) && close(b, ns) && close(u, us) && c == cs, where + " morph targets, UVs, colours");
		}
	}
	std::printf("%u D3M, %u D3A, %u morph chunks; %d checks passed, %d failed\n", chunks[0], chunks[1], chunks[2], passes, failures);
	return failures == 0 && chunks[0] + chunks[1] + chunks[2] > 0 ? 0 : 1;
}
