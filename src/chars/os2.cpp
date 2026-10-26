// An Os2 skinned mesh decoded for the GPU (os2.hpp; the reference: research/ffxi-re/tools/chars.py Mesh)

#include "os2.hpp"
#include <cstring>

namespace hifi::chars::os2
{
	namespace
	{
		uint16_t u16(const uint8_t *p)
		{
			uint16_t v;
			std::memcpy(&v, p, 2);
			return v;
		}
		uint32_t u32(const uint8_t *p)
		{
			uint32_t v;
			std::memcpy(&v, p, 4);
			return v;
		}
		float f32(const uint8_t *p)
		{
			float v;
			std::memcpy(&v, p, 4);
			return v;
		}

		struct reader
		{
			const uint8_t *data;
			size_t size;
			bool has(size_t at, size_t n) const { return at <= size && n <= size - at; }
		};

		// (bone, mirrored bone, axis) of a 16-bit bone reference: bits 0-6, 7-13, 14-15, through the table when used
		bool bone_of(uint16_t ref, const std::vector<uint16_t> &table, bool uses_table, uint16_t &bone, uint16_t &mirror, uint8_t &axis)
		{
			uint16_t a = ref & 0x7F, b = (ref >> 7) & 0x7F;
			if (uses_table)
			{
				if (a >= table.size() || b >= table.size())
					return false;
				a = table[a];
				b = table[b];
			}
			bone = a;
			mirror = b;
			axis = uint8_t((ref >> 14) & 3);
			return true;
		}
	}

	bool sections_of_chunk(const uint8_t *data, size_t size, sections &out)
	{
		if (size < 0x2A)
			return false;
		for (int k = 0; k < 6; ++k)
		{
			out.at[k] = u32(data + 6 + 6 * k) * 2;
			out.size[k] = uint32_t(u16(data + 10 + 6 * k)) * 2;
		}
		return true;
	}

	bool sections_of_live(const uint8_t *data, size_t size, uint32_t base, sections &out)
	{
		if (size < 0x2A)
			return false;
		for (int k = 0; k < 6; ++k)
		{
			const uint32_t p = u32(data + 6 + 6 * k);
			out.at[k] = p - base; // (an empty section may hold anything: its length says so)
			out.size[k] = uint32_t(u16(data + 10 + 6 * k)) * 2;
		}
		return true;
	}

	bool texture_commands(const uint8_t *data, size_t size, const sections &s, std::vector<texture_ref> &out)
	{
		out.clear();
		const reader r { data, size };
		size_t o = s.at[0];
		while (r.has(o, 2))
		{
			const uint16_t op = u16(data + o);
			if (op == 0xFFFF)
				return true;
			if (op & 0x8000)
			{
				const uint32_t sub = (op >> 4) & 0xF;
				if (sub == 0)
				{
					if (!r.has(o + 2, 16))
						return false;
					texture_ref t;
					t.at = uint32_t(o);
					t.cached = (op & 1) != 0;
					std::memcpy(t.name, data + o + 2, 16);
					if (t.cached)
						t.object = u32(data + o + 2);
					out.push_back(t);
					o += 18;
				}
				else if (sub == 1)
					o += 46;
				else
					return true;
				continue;
			}
			if (!r.has(o + 2, 2))
				return false;
			const size_t n = u16(data + o + 2);
			if (op == 0x0054)
				o += (2 + 15 * n) * 2;
			else if (op == 0x5453)
				o += (5 * n + 12) * 2;
			else if (op == 0x0043)
				o += (2 + 5 * n) * 2;
			else if (op == 0x4353)
				o += (n + 6) * 2;
			else
				return true;
		}
		return false;
	}

	uint32_t hidden_by(uint8_t byte3)
	{
		switch (byte3)
		{
		case 2: return 1u << 1;
		case 3: return 1u << 1 | 1u << 2;
		case 4: return 1u << 1 | 1u << 2 | 1u << 3;
		case 5: return 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 8;
		case 6: return 1u << 1 | 1u << 2 | 1u << 3 | 1u << 8;
		case 0x12: return 1u << 5;
		case 0x22: return 1u << 7;
		case 0x32: return 1u << 6;
		default: return 0;
		}
	}

	bool decode(const uint8_t *data, size_t size, const sections &s, mesh &out, std::string &why)
	{
		out = mesh();
		const reader r { data, size };
		if (size < 0x2A)
		{
			why = "too short for an Os2 header";
			return false;
		}
		out.version = u16(data);
		out.flags = data[2];
		out.hides = data[3];
		out.mirrored = (data[4] & 1) != 0;
		const bool uses_table = (out.flags & 0x80) != 0;

		// The bone table, the counts, the references
		if (!r.has(s.at[1], s.size[1]) || !r.has(s.at[2], 4))
		{
			why = "the bone table or the counts are outside the data";
			return false;
		}
		std::vector<uint16_t> table(s.size[1] / 2);
		for (size_t i = 0; i < table.size(); ++i)
			table[i] = u16(data + s.at[1] + 2 * i);
		out.one_bone = u16(data + s.at[2]);
		out.two_bone = u16(data + s.at[2] + 2);
		const uint32_t n = out.one_bone + out.two_bone;
		if (!r.has(s.at[3], size_t(n) * 4) || !r.has(s.at[4], size_t(out.one_bone) * 24 + size_t(out.two_bone) * 56))
		{
			why = "the vertices or their bone references are outside the data";
			return false;
		}

		// The vertices
		out.vertices.resize(n);
		const uint8_t *v = data + s.at[4];
		for (uint32_t i = 0; i < n; ++i)
		{
			vertex &o = out.vertices[i];
			std::memset(&o, 0, sizeof(o));
			const uint8_t *ref = data + s.at[3] + 4 * i;
			if (!bone_of(u16(ref), table, uses_table, o.bone[0], o.mirror_bone[0], o.axis[0]))
			{
				why = "a bone reference past the bone table";
				return false;
			}
			if (i < out.one_bone)
			{
				const uint8_t *p = v + 24 * i;
				for (int k = 0; k < 3; ++k)
				{
					o.p0[k] = f32(p + 4 * k);
					o.n0[k] = f32(p + 12 + 4 * k);
				}
				o.w0 = 1.0f;
				o.bone[1] = o.bone[0];
				o.mirror_bone[1] = o.mirror_bone[0];
				o.axis[1] = o.axis[0];
			}
			else
			{
				if (!bone_of(u16(ref + 2), table, uses_table, o.bone[1], o.mirror_bone[1], o.axis[1]))
				{
					why = "a bone reference past the bone table";
					return false;
				}
				// (x1 x2 y1 y2 z1 z2 w1 w2, then n1x n2x n1y n2y n1z n2z)
				const uint8_t *p = v + 24 * out.one_bone + 56 * (i - out.one_bone);
				for (int k = 0; k < 3; ++k)
				{
					o.p0[k] = f32(p + 8 * k);
					o.p1[k] = f32(p + 8 * k + 4);
					o.n0[k] = f32(p + 32 + 8 * k);
					o.n1[k] = f32(p + 32 + 8 * k + 4);
				}
				o.w0 = f32(p + 24);
				o.w1 = f32(p + 28);
			}
			for (int k = 0; k < 2; ++k)
			{
				if (o.bone[k] > out.max_bone)
					out.max_bone = o.bone[k];
				if (out.mirrored && o.mirror_bone[k] > out.max_bone)
					out.max_bone = o.mirror_bone[k];
			}
		}

		// The draw commands (02CB80): triangles into corners, in groups by texture and material
		size_t o = s.at[0];
		int32_t texture = -1;
		uint32_t material[11] = { 0x80808080u };
		bool has_material = false, open = false;
		auto add = [&](uint16_t a, uint16_t b, uint16_t c, const float uv[6]) {
			if (!open)
			{
				group g;
				g.first = uint32_t(out.corners.size());
				g.texture = texture;
				std::memcpy(g.material, material, sizeof(material));
				g.has_material = has_material;
				out.groups.push_back(g);
				open = true;
			}
			out.corners.push_back({ a, uv[0], uv[1] });
			out.corners.push_back({ b, uv[2], uv[3] });
			out.corners.push_back({ c, uv[4], uv[5] });
			out.groups.back().count += 3;
		};
		bool ended = false;
		while (r.has(o, 2))
		{
			const uint16_t op = u16(data + o);
			if (op == 0xFFFF)
			{
				ended = true;
				break;
			}
			if (op & 0x8000)
			{
				const uint32_t sub = (op >> 4) & 0xF;
				if (sub == 0)
				{
					if (!r.has(o + 2, 16))
						break;
					texture_ref t;
					t.at = uint32_t(o);
					t.cached = (op & 1) != 0;
					std::memcpy(t.name, data + o + 2, 16);
					if (t.cached)
						t.object = u32(data + o + 2);
					texture = int32_t(out.textures.size());
					out.textures.push_back(t);
					open = false;
					o += 18;
				}
				else if (sub == 1)
				{
					if (!r.has(o + 2, 44))
						break;
					for (int k = 0; k < 11; ++k)
						material[k] = u32(data + o + 2 + 4 * k);
					has_material = true;
					open = false;
					o += 46;
				}
				else
				{
					ended = true; // (not one the game knows either: it stops there)
					break;
				}
				continue;
			}
			if (!r.has(o + 2, 2))
				break;
			const uint32_t count = u16(data + o + 2);
			if (op == 0x0054)
			{
				if (!r.has(o + 4, size_t(count) * 30))
					break;
				const uint8_t *p = data + o + 4;
				for (uint32_t k = 0; k < count; ++k, p += 30)
				{
					float uv[6];
					for (int j = 0; j < 6; ++j)
						uv[j] = f32(p + 6 + 4 * j);
					add(u16(p), u16(p + 2), u16(p + 4), uv);
				}
				o += (2 + 15 * size_t(count)) * 2;
			}
			else if (op == 0x5453)
			{
				if (count == 0 || !r.has(o + 4, 30 + size_t(count - 1) * 10))
					break;
				const uint8_t *p = data + o + 4;
				uint16_t i[3] = { u16(p), u16(p + 2), u16(p + 4) };
				float uv[6];
				for (int j = 0; j < 6; ++j)
					uv[j] = f32(p + 6 + 4 * j);
				p += 30;
				for (uint32_t k = 0; k < count; ++k)
				{
					if (k > 0)
					{
						i[0] = i[1];
						i[1] = i[2];
						i[2] = u16(p);
						uv[0] = uv[2];
						uv[1] = uv[3];
						uv[2] = uv[4];
						uv[3] = uv[5];
						uv[4] = f32(p + 2);
						uv[5] = f32(p + 6);
						p += 10;
					}
					// (a strip alternates its winding)
					if (k % 2 == 0)
						add(i[0], i[1], i[2], uv);
					else
					{
						const float w[6] = { uv[2], uv[3], uv[0], uv[1], uv[4], uv[5] };
						add(i[1], i[0], i[2], w);
					}
				}
				o += (5 * size_t(count) + 12) * 2;
			}
			else if (op == 0x0043)
				o += (2 + 5 * size_t(count)) * 2;
			else if (op == 0x4353)
				o += (size_t(count) + 6) * 2;
			else
			{
				ended = true; // (an unknown command: the game's walk stops there too)
				break;
			}
		}
		if (!ended)
		{
			why = "the draw commands run past the data";
			out = mesh();
			return false;
		}
		for (const corner &c : out.corners)
			if (c.vertex >= n)
			{
				why = "a triangle names a vertex the mesh doesn't have";
				out = mesh();
				return false;
			}

		// The mirrored half: the same groups again, each triangle's first two corners swapped
		if (out.mirrored)
		{
			const size_t groups = out.groups.size(), corners = out.corners.size();
			out.corners.reserve(corners * 2);
			for (size_t gi = 0; gi < groups; ++gi)
			{
				group g = out.groups[gi];
				const uint32_t from = g.first;
				g.first = uint32_t(out.corners.size());
				g.mirrored = true;
				for (uint32_t k = 0; k < g.count; k += 3)
				{
					corner a = out.corners[from + k], b = out.corners[from + k + 1], c = out.corners[from + k + 2];
					a.vertex |= kMirrored;
					b.vertex |= kMirrored;
					c.vertex |= kMirrored;
					out.corners.push_back(b);
					out.corners.push_back(a);
					out.corners.push_back(c);
				}
				out.groups.push_back(g);
			}
		}
		return true;
	}
}
