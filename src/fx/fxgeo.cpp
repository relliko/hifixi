// The effects' geometry chunks (fxgeo.hpp; the reference: research/ffxi-re/tools/effects_geo.py)

#include "fxgeo.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hifi::fx::geo
{
	namespace
	{
		template <typename T>
		T get(const uint8_t *d, size_t at)
		{
			T v;
			std::memcpy(&v, d + at, sizeof(T));
			return v;
		}
	}

	uint32_t doubled(uint32_t c)
	{
		uint32_t out = 0;
		for (int s = 0; s < 32; s += 8)
		{
			const uint32_t b = std::min<uint32_t>(((c >> s) & 0xFF) * 2, 0xFF);
			out |= b << s;
		}
		return out;
	}

	size_t d3m_names_at(const uint8_t *d)
	{
		const uint32_t kind = d[0] & 0xF;
		if (kind < 5 || kind > 7)
			return 0x10;
		const uint32_t textures = d[4];
		if (kind == 7 && textures == 0)
			return 8;
		const uint32_t n = textures + d[5];
		return n % 4 != 0 ? 8 + (n - n % 4) * 2 + 6 : 8 + n * 2;
	}

	size_t d3m_vertices_at(const uint8_t *d)
	{
		const uint32_t kind = d[0] & 0xF;
		if (kind < 5 || kind > 7)
			return 0x50;
		return d3m_names_at(d) + size_t(d[4]) * 16;
	}

	bool parse_d3m(const uint8_t *d, size_t size, d3m &out, std::string &why, bool double_colours)
	{
		out = d3m();
		if (size < 8)
		{
			why = "short";
			return false;
		}
		out.kind = d[0] & 0xF;
		out.textures = d[4];
		out.tail = d[5];
		out.triangles = get<uint16_t>(d, 6);
		if ((out.kind < 5 || out.kind > 7) && out.textures > 4)
		{
			why = "a kind without its own names with more than 4 textures";
			return false;
		}
		const size_t names = d3m_names_at(d), verts = d3m_vertices_at(d);
		const size_t end = verts + size_t(out.triangles) * 3 * sizeof(vertex);
		if (end > size || names + 16 * size_t(out.textures) > size || 8 + 2 * size_t(out.textures) > size)
		{
			why = "past the end";
			return false;
		}
		for (uint32_t i = 0; i < out.textures; ++i)
			out.names.emplace_back(reinterpret_cast<const char *>(d + names + 16 * i), 16);
		out.vertices.resize(size_t(out.triangles) * 3);
		std::memcpy(out.vertices.data(), d + verts, out.vertices.size() * sizeof(vertex));
		if (double_colours)
			for (vertex &v : out.vertices)
				v.colour = doubled(v.colour);
		// (040220's draws)
		if (out.textures == 0)
		{
			out.runs.push_back({ 0, out.triangles * 3, -1 });
			return true;
		}
		uint32_t first = 0;
		for (uint32_t i = 0; i < out.textures; ++i)
		{
			const uint32_t n = uint32_t(get<uint16_t>(d, 8 + 2 * i)) * 3;
			if (first + n > out.triangles * 3)
			{
				why = "texture runs past the triangles";
				return false;
			}
			out.runs.push_back({ first, n, int32_t(i) });
			first += n;
		}
		if (out.tail != 0 && first < out.triangles * 3)
			out.runs.push_back({ first, out.triangles * 3 - first, -1 });
		return true;
	}

	bool parse_d3a(const uint8_t *d, size_t size, d3a &out, std::string &why)
	{
		out = d3a();
		if (size < 0x18)
		{
			why = "short";
			return false;
		}
		out.units = get<int16_t>(d, 0);
		out.frames = get<int16_t>(d, 2);
		out.flare = d[4];
		out.pieces = d[5];
		out.quads = d[6];
		out.uvs_unit = d[7];
		out.name.assign(reinterpret_cast<const char *>(d + 8), 16);
		const int count = out.flare != 0 ? out.pieces : out.frames;
		size_t p = 0x18;
		for (int i = 0; i < count; ++i)
		{
			if (p + 4 > size)
			{
				why = "a frame's head past the end";
				return false;
			}
			d3a_frame f;
			std::memcpy(f.head, d + p, 4);
			const size_t n = size_t(d[p + 2]) * 6;
			const size_t v0 = p + (out.flare != 0 ? 20 : 4);
			if (v0 + n * 24 > size)
			{
				why = "a frame past the end";
				return false;
			}
			if (out.flare != 0)
				f.t = get<float>(d, p + 4);
			f.vertices.resize(n);
			for (size_t k = 0; k < n; ++k)
			{
				const uint8_t *s = d + v0 + k * 24;
				vertex &v = f.vertices[k];
				std::memcpy(v.pos, s, 12);
				v.normal[0] = v.normal[1] = v.normal[2] = 0.0f;
				v.colour = get<uint32_t>(s, 12);
				std::memcpy(v.uv, s + 16, 8);
			}
			out.frame_list.push_back(std::move(f));
			p = v0 + n * 24;
		}
		return true;
	}

	bool parse_morph(const uint8_t *d, size_t size, morph &out, std::string &why, bool double_colours)
	{
		out = morph();
		if (size < 0x20)
		{
			why = "short";
			return false;
		}
		const uint8_t b2 = d[2], b3 = d[3];
		const int k = (b2 & 0x40) != 0 ? int(b2 & 0x7F) - 0x80 : int(b2 & 0x7F); // ((char)(b2 x 2) >> 1, 03F8A0)
		const int16_t positions = get<int16_t>(d, 4), normals = get<int16_t>(d, 6);
		size_t idx = get<uint16_t>(d, 8) + (size_t(b3) << 16);
		const uint32_t triangles = get<uint16_t>(d, 0xA);
		const size_t coff = get<uint16_t>(d, 0xC);
		size_t uoff = get<uint16_t>(d, 0xE);
		if (uoff <= coff)
			uoff += 0x10000;
		if (k < 1 || positions < 0 || normals < 0)
		{
			why = "no targets";
			return false;
		}
		out.targets = uint32_t(k);
		out.positions = uint32_t(positions);
		out.normals = uint32_t(normals);
		out.triangles = triangles;
		out.alpha_test = (b2 & 0x80) != 0;
		out.name.assign(reinterpret_cast<const char *>(d + 0x10), 16);
		const size_t stride = (size_t(positions) * 3 + normals) * 4;
		if (0x20 + out.targets * stride > size || coff + triangles * 12 > size || uoff + triangles * 24 > size || idx + triangles * 12 > size)
		{
			why = "past the end";
			return false;
		}
		for (uint32_t t = 0; t < out.targets; ++t)
		{
			const size_t o = 0x20 + t * stride;
			for (int i = 0; i < positions * 3; ++i)
				out.pos.push_back(get<float>(d, o + 4 * size_t(i)));
			for (int i = 0; i < normals; ++i)
			{
				const uint32_t pk = get<uint32_t>(d, o + size_t(positions) * 12 + 4 * size_t(i));
				for (int s = 0; s < 30; s += 10)
				{
					const int v = int(((pk >> s) & 0x3FF) ^ 0x200) - 0x200;
					out.nrm.push_back(float(v) / 512.0f);
				}
			}
		}
		for (uint32_t i = 0; i < triangles * 3; ++i)
			out.colours.push_back(double_colours ? doubled(get<uint32_t>(d, coff + 4 * i)) : get<uint32_t>(d, coff + 4 * i));
		for (uint32_t i = 0; i < triangles * 6; ++i)
			out.uv.push_back(get<float>(d, uoff + 4 * i));
		for (uint32_t i = 0; i < triangles * 3; ++i)
		{
			out.pi.push_back(get<uint16_t>(d, idx + 2 * i));
			out.ni.push_back(get<uint16_t>(d, idx + triangles * 6 + 2 * i));
		}
		for (uint32_t i = 0; i < triangles * 3; ++i)
			if (out.pi[i] >= out.positions || out.ni[i] >= out.normals)
			{
				why = "an index out of range";
				return false;
			}
		return true;
	}

	void morph::blend(const float *weights, std::vector<vertex> &out) const
	{
		float w[8] = { 1.0f };
		float sum = 0.0f;
		for (uint32_t t = 0; t < targets && t < 8 && weights != nullptr; ++t)
			sum += weights[t];
		if (weights != nullptr && sum > 0.0f)
			for (uint32_t t = 0; t < targets && t < 8; ++t)
				w[t] = weights[t] / sum;
		out.resize(size_t(triangles) * 3);
		for (uint32_t i = 0; i < triangles * 3; ++i)
		{
			vertex &v = out[i];
			for (int c = 0; c < 3; ++c)
			{
				float p = 0.0f, n = 0.0f;
				for (uint32_t t = 0; t < targets && t < 8; ++t)
				{
					p += w[t] * pos[(size_t(t) * positions + pi[i]) * 3 + c];
					n += w[t] * nrm[(size_t(t) * normals + ni[i]) * 3 + c];
				}
				v.pos[c] = p;
				v.normal[c] = n;
			}
			v.colour = colours[i];
			v.uv[0] = uv[size_t(i) * 2];
			v.uv[1] = uv[size_t(i) * 2 + 1];
		}
	}
}
