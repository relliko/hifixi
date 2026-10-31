// A character texture chunk decoded (tex.hpp; the reference: research/ffxi-re/tools/chartex.py Tex, chars.py _dxt)

#include "tex.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hifi::chars::tex
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

		// The DXT code at o: 0 DXT1, 2 DXT3, -1 anything else (01D940 takes only those two)
		int fourcc(const uint8_t *data, size_t size, size_t o)
		{
			if (o + 4 > size)
				return -1;
			if (std::memcmp(data + o, "1TXD", 4) == 0)
				return 0;
			if (std::memcmp(data + o, "3TXD", 4) == 0)
				return 2;
			return -1;
		}

		void rgb565(uint16_t c, double out[3])
		{
			out[0] = double(((c >> 11) & 31) * 255 / 31);
			out[1] = double(((c >> 5) & 63) * 255 / 63);
			out[2] = double((c & 31) * 255 / 31);
		}

		// DXT1/DXT3 blocks to RGBA8, rows top down (as chars.py _dxt: interpolated colours truncated)
		void dxt(const uint8_t *buf, size_t size, uint32_t w, uint32_t h, int code, std::vector<uint8_t> &img)
		{
			img.assign(size_t(w) * h * 4, 0);
			const size_t bs = code == 0 ? 8 : 16;
			size_t p = 0;
			for (uint32_t by = 0; by < h; by += 4)
				for (uint32_t bx = 0; bx < w; bx += 4)
				{
					if (p + bs > size)
						return;
					uint8_t alpha[16];
					std::fill(alpha, alpha + 16, uint8_t(255));
					if (code == 2)
					{
						uint64_t a;
						std::memcpy(&a, buf + p, 8);
						for (int k = 0; k < 16; ++k)
							alpha[k] = uint8_t(((a >> (4 * k)) & 15) * 17);
					}
					const size_t c = p + (code == 0 ? 0 : 8);
					const uint16_t c0 = u16(buf + c), c1 = u16(buf + c + 2);
					const uint32_t bits = u32(buf + c + 4);
					double k0[3], k1[3], cols[4][3];
					rgb565(c0, k0);
					rgb565(c1, k1);
					const bool four = c0 > c1 || code != 0;
					for (int j = 0; j < 3; ++j)
					{
						cols[0][j] = k0[j];
						cols[1][j] = k1[j];
						if (four)
						{
							cols[2][j] = (2 * k0[j] + k1[j]) / 3;
							cols[3][j] = (k0[j] + 2 * k1[j]) / 3;
						}
						else
						{
							cols[2][j] = (k0[j] + k1[j]) / 2;
							cols[3][j] = 0;
						}
					}
					for (int k = 0; k < 16; ++k)
					{
						const uint32_t y = by + k / 4, x = bx + k % 4;
						if (y >= h || x >= w)
							continue;
						const uint32_t sel = (bits >> (2 * k)) & 3;
						uint8_t *o = img.data() + (size_t(y) * w + x) * 4;
						for (int j = 0; j < 3; ++j)
							o[j] = uint8_t(cols[sel][j]);
						o[3] = (!four && sel == 3) ? 0 : alpha[k];
					}
					p += bs;
				}
		}

		// A palette entry or a direct pixel as RGBA (039E50; chartex.py _entries)
		void entry(const uint8_t *e, uint32_t bytes, bool direct, uint8_t out[4])
		{
			if (bytes == 4)
			{
				out[0] = e[2];
				out[1] = e[1];
				out[2] = e[0];
				out[3] = e[3];
			}
			else if (bytes == 3)
			{
				out[0] = e[2];
				out[1] = e[1];
				out[2] = e[0];
				out[3] = direct ? 0x80 : 255;
			}
			else
			{
				const uint32_t v = u16(e);
				out[0] = uint8_t(((v >> 10) & 31) << 3);
				out[1] = uint8_t(((v >> 5) & 31) << 3);
				out[2] = uint8_t((v & 31) << 3);
				out[3] = (v & 0x8000) ? (direct ? 0x80 : 255) : 0;
			}
		}
	}

	uint32_t mip_count(uint32_t w, uint32_t h)
	{
		return (w < 32 || h < 32) ? 1 : 6;
	}

	bool decode(const uint8_t *data, size_t size, texture &out, std::string &why)
	{
		out = texture();
		if (size < 57)
		{
			why = "too short for a texture header";
			return false;
		}
		const uint8_t flags = data[0];
		const uint32_t layout = (flags >> 4) & 7;
		std::memcpy(out.name, data + 1, 16);
		const uint32_t w = u16(data + 21), h = u16(data + 25);
		const uint32_t bits = data[31], palbits = data[53];
		if (w == 0 || h == 0 || w > 4096 || h > 4096)
		{
			why = "an odd size";
			return false;
		}
		out.w = w;
		out.h = h;
		level l0;
		l0.w = w;
		l0.h = h;
		size_t p = 57;
		int code = -1;
		size_t blocks = 0;
		if (layout == 2)
		{
			code = fourcc(data, size, p);
			if (code < 0)
			{
				why = "a DXT texture the game doesn't take";
				return false;
			}
			blocks = p + 12;
		}
		else if (layout == 0 || layout == 1 || layout == 3)
		{
			if (layout == 3)
				p += 4;
			const size_t n = size_t(w) * h;
			size_t pal = 0, pix = 0, end = 0;
			bool known = true;
			if (bits == 4)
			{
				pal = p;
				pix = p + 16 * palbits / 8;
				end = pix + (n + 1) / 2;
			}
			else if (bits == 8)
			{
				pal = p;
				pix = p + 256 * palbits / 8;
				end = pix + n;
			}
			else if (bits == 16 || bits == 24 || bits == 32)
			{
				pix = p;
				end = p + n * bits / 8;
			}
			else
				known = false;
			if (known && (flags & 0x80))
			{
				code = fourcc(data, size, end);
				if (code >= 0)
					blocks = end + 12;
			}
			if (code < 0)
			{
				if (!known || end > size)
				{
					why = "the picture is outside the data";
					return false;
				}
				l0.rgba.resize(n * 4);
				if (bits == 4 || bits == 8)
				{
					const uint32_t es = palbits / 8;
					if (es < 2 || es > 4)
					{
						why = "an odd palette";
						return false;
					}
					for (size_t i = 0; i < n; ++i)
					{
						const uint32_t index = bits == 8 ? data[pix + i] : ((i & 1) == 0 ? data[pix + i / 2] >> 4 : data[pix + i / 2] & 15);
						// (rows bottom up: row y of the picture is row h - 1 - y here)
						const size_t y = i / w, x = i % w;
						entry(data + pal + size_t(index) * es, es, false, l0.rgba.data() + ((h - 1 - y) * w + x) * 4);
					}
				}
				else
				{
					const uint32_t es = bits / 8;
					for (size_t i = 0; i < n; ++i)
					{
						const size_t y = i / w, x = i % w;
						entry(data + pix + i * es, es, true, l0.rgba.data() + ((h - 1 - y) * w + x) * 4);
					}
				}
			}
		}
		else
		{
			why = "a layout the game doesn't take";
			return false;
		}
		if (code >= 0)
		{
			if (blocks > size)
			{
				why = "the DXT blocks are outside the data";
				return false;
			}
			dxt(data + blocks, size - blocks, w, h, code, l0.rgba);
			out.from_dxt = true;
		}
		out.levels.push_back(std::move(l0));
		return true;
	}

	void make_mips(texture &t)
	{
		if (t.levels.empty())
			return;
		t.levels.resize(1);
		const uint32_t count = mip_count(t.w, t.h);
		for (uint32_t m = 1; m < count; ++m)
		{
			const level &a = t.levels.back();
			level b;
			b.w = std::max(a.w / 2, 1u);
			b.h = std::max(a.h / 2, 1u);
			b.rgba.resize(size_t(b.w) * b.h * 4);
			for (uint32_t y = 0; y < b.h; ++y)
				for (uint32_t x = 0; x < b.w; ++x)
				{
					const uint32_t x0 = std::min(2 * x, a.w - 1), x1 = std::min(2 * x + 1, a.w - 1);
					const uint32_t y0 = std::min(2 * y, a.h - 1), y1 = std::min(2 * y + 1, a.h - 1);
					for (int c = 0; c < 4; ++c)
					{
						const uint32_t s = a.rgba[(size_t(y0) * a.w + x0) * 4 + c] + a.rgba[(size_t(y0) * a.w + x1) * 4 + c] +
							a.rgba[(size_t(y1) * a.w + x0) * 4 + c] + a.rgba[(size_t(y1) * a.w + x1) * 4 + c];
						b.rgba[(size_t(y) * b.w + x) * 4 + c] = uint8_t((s + 2) / 4);
					}
				}
			t.levels.push_back(std::move(b));
		}
	}

	namespace
	{
		// A separable Gaussian, clamped at the edges (as the character sampler clamps)
		std::vector<float> blur(const std::vector<float> &in, uint32_t w, uint32_t h, float sigma)
		{
			const int r = int(3.0f * sigma) + 1;
			std::vector<float> k(size_t(2 * r + 1));
			float sum = 0.0f;
			for (int i = -r; i <= r; ++i)
				sum += k[size_t(i + r)] = std::exp(-0.5f * (float(i) / sigma) * (float(i) / sigma));
			for (float &v : k)
				v /= sum;
			std::vector<float> tmp(in.size()), out(in.size());
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					float s = 0.0f;
					for (int i = -r; i <= r; ++i)
						s += k[size_t(i + r)] * in[size_t(y) * w + uint32_t(std::clamp(int(x) + i, 0, int(w) - 1))];
					tmp[size_t(y) * w + x] = s;
				}
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					float s = 0.0f;
					for (int i = -r; i <= r; ++i)
						s += k[size_t(i + r)] * tmp[size_t(std::clamp(int(y) + i, 0, int(h) - 1)) * w + x];
					out[size_t(y) * w + x] = s;
				}
			return out;
		}

		float smooth(float e0, float e1, float x)
		{
			const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}
	}

	void make_detail(const texture &colour, texture &out)
	{
		out = texture();
		if (colour.levels.empty())
			return;
		const level &c = colour.levels[0];
		const uint32_t w = c.w, h = c.h;
		const size_t n = size_t(w) * h;
		std::memcpy(out.name, colour.name, 16);
		out.w = w;
		out.h = h;
		std::vector<float> lum(n);
		for (size_t i = 0; i < n; ++i)
			lum[i] = (0.30f * c.rgba[i * 4] + 0.59f * c.rgba[i * 4 + 1] + 0.11f * c.rgba[i * 4 + 2]) / 255.0f;
		// The height: the brightness less its broad shading (the light and shadow painted in), detail at three scales
		const std::vector<float> broad = blur(lum, w, h, 6.0f);
		std::vector<float> hp(n);
		for (size_t i = 0; i < n; ++i)
			hp[i] = lum[i] - broad[i];
		std::vector<float> gx(n, 0.0f), gy(n, 0.0f);
		const float scales[3][2] = { { 0.6f, 1.0f }, { 1.2f, 0.7f }, { 2.5f, 0.4f } };
		for (const auto &sc : scales)
		{
			const std::vector<float> b = blur(hp, w, h, sc[0]);
			auto at = [&](int x, int y) { return b[size_t(std::clamp(y, 0, int(h) - 1)) * w + uint32_t(std::clamp(x, 0, int(w) - 1))]; };
			for (int y = 0; y < int(h); ++y)
				for (int x = 0; x < int(w); ++x)
				{
					const float sx = (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1));
					const float sy = (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1));
					gx[size_t(y) * w + uint32_t(x)] += sc[1] * sx;
					gy[size_t(y) * w + uint32_t(x)] += sc[1] * sy;
				}
		}
		// Metal: bright texels with fine detail around them (the detail scaled by its 95th percentile); cavity: below
		// the brightness around
		const std::vector<float> fine = blur(lum, w, h, 1.5f);
		std::vector<float> d(n);
		for (size_t i = 0; i < n; ++i)
			d[i] = std::fabs(lum[i] - fine[i]);
		d = blur(d, w, h, 1.2f);
		std::vector<float> sorted = d;
		std::nth_element(sorted.begin(), sorted.begin() + ptrdiff_t(n * 95 / 100), sorted.end());
		const float p95 = std::max(sorted[n * 95 / 100], 1e-6f);
		const std::vector<float> around = blur(lum, w, h, 2.0f);
		level l0;
		l0.w = w;
		l0.h = h;
		l0.rgba.resize(n * 4);
		constexpr float kRelief = 2.2f;
		for (size_t i = 0; i < n; ++i)
		{
			float nx = -kRelief * gx[i], ny = -kRelief * gy[i], nz = 1.0f;
			const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
			nx /= len;
			ny /= len;
			const float metal = smooth(0.28f, 0.55f, lum[i]) * smooth(0.25f, 0.7f, std::min(d[i] / p95, 1.0f));
			const float cavity = std::clamp((around[i] - lum[i]) * 5.0f, 0.0f, 1.0f);
			l0.rgba[i * 4] = uint8_t(std::lround(std::clamp(nx * 127.5f + 127.5f, 0.0f, 255.0f)));
			l0.rgba[i * 4 + 1] = uint8_t(std::lround(std::clamp(ny * 127.5f + 127.5f, 0.0f, 255.0f)));
			l0.rgba[i * 4 + 2] = uint8_t(std::lround(metal * 255.0f));
			l0.rgba[i * 4 + 3] = uint8_t(std::lround(cavity * 255.0f));
		}
		out.levels.push_back(std::move(l0));
		make_mips(out);
	}
}
