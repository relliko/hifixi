/*
 * The texture pack's height maps (height.hpp). Blurs are separable Gaussians that wrap at the edges (zone textures
 * tile, and nmmockup.py's gblur wraps); "normalized" blurs (nblur) divide the weighted blur by the blurred weights, so
 * texels that count for nothing neither pull their neighbours' heights nor get a height of their own beyond what's
 * around them.
 */

#include "height.hpp"
#include "bc2.hpp"
#include "bc4.hpp"
#include "pack.hpp"
#include <algorithm>
#include <cmath>

namespace hifi::pack::height
{
	namespace
	{
		std::vector<float> blur(const std::vector<float> &in, uint32_t w, uint32_t h, float sigma)
		{
			const int r = int(3.0f * sigma) + 1;
			std::vector<float> k(size_t(2 * r + 1));
			float sum = 0.0f;
			for (int i = -r; i <= r; ++i)
				sum += k[size_t(i + r)] = std::exp(-0.5f * (float(i) / sigma) * (float(i) / sigma));
			for (float &v : k)
				v /= sum;
			auto wrap = [](int i, int n) { return uint32_t(((i % n) + n) % n); };
			std::vector<float> tmp(in.size()), out(in.size());
			for (uint32_t y = 0; y < h; ++y)
			{
				const float *row = in.data() + size_t(y) * w;
				for (uint32_t x = 0; x < w; ++x)
				{
					float s = 0.0f;
					for (int i = -r; i <= r; ++i)
						s += k[size_t(i + r)] * row[wrap(int(x) + i, int(w))];
					tmp[size_t(y) * w + x] = s;
				}
			}
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					float s = 0.0f;
					for (int i = -r; i <= r; ++i)
						s += k[size_t(i + r)] * tmp[size_t(wrap(int(y) + i, int(h))) * w + x];
					out[size_t(y) * w + x] = s;
				}
			return out;
		}

		// The weighted mean of v (the plain mean when nothing has weight)
		double weighted_mean(const std::vector<float> &v, const std::vector<float> &wt)
		{
			double s = 0, n = 0, plain = 0;
			for (size_t i = 0; i < v.size(); ++i)
				s += double(v[i]) * wt[i], n += wt[i], plain += v[i];
			return n > 1e-6 ? s / n : (v.empty() ? 0.0 : plain / double(v.size()));
		}

		// blur(v * wt) / blur(wt); where the blurred weight is about nothing, the weighted mean
		std::vector<float> nblur(const std::vector<float> &v, const std::vector<float> &wt, uint32_t w, uint32_t h, float sigma)
		{
			std::vector<float> vw(v.size());
			for (size_t i = 0; i < v.size(); ++i)
				vw[i] = v[i] * wt[i];
			const std::vector<float> a = blur(vw, w, h, sigma), b = blur(wt, w, h, sigma);
			const float fallback = float(weighted_mean(v, wt));
			std::vector<float> out(v.size());
			for (size_t i = 0; i < v.size(); ++i)
				out[i] = b[i] > 1e-4f ? a[i] / b[i] : fallback;
			return out;
		}

		// The colour's brightness (nmmockup's lum_of: 0.30, 0.59, 0.11 on 0..1) and weights, box-filtered by weight onto
		// w x h (cw, ch whole multiples of them)
		bool lum_and_weights(const uint8_t *rgba, uint32_t cw, uint32_t ch, uint32_t w, uint32_t h, uint8_t alpha_kind, float cut, std::vector<float> &lum,
			std::vector<float> &wt, std::string &why)
		{
			if (rgba == nullptr || w == 0 || h == 0 || cw < w || ch < h || cw % w != 0 || ch % h != 0)
			{
				why = "the colour's size isn't a whole multiple of the map's";
				return false;
			}
			const uint32_t fx = cw / w, fy = ch / h;
			lum.assign(size_t(w) * h, 0.0f);
			wt.assign(size_t(w) * h, 0.0f);
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x)
				{
					double s = 0, n = 0, plain = 0;
					for (uint32_t j = 0; j < fy; ++j)
						for (uint32_t i = 0; i < fx; ++i)
						{
							const uint8_t *p = rgba + ((size_t(y) * fy + j) * cw + size_t(x) * fx + i) * 4;
							const double l = (0.30 * p[0] + 0.59 * p[1] + 0.11 * p[2]) / 255.0;
							const double a = weight_of(p[3], alpha_kind, cut);
							s += l * a, n += a, plain += l;
						}
					const double count = double(fx) * fy;
					lum[size_t(y) * w + x] = float(n > 1e-9 ? s / n : plain / count);
					wt[size_t(y) * w + x] = float(n / count);
				}
			return true;
		}

		// The field centred and scaled into 0..1 (0.5 the weighted mean); span as encode says
		std::vector<float> normalised(const field &f, float &span)
		{
			const double mean = weighted_mean(f.v, f.wt);
			std::vector<float> a;
			a.reserve(f.v.size());
			for (size_t i = 0; i < f.v.size(); ++i)
				if (f.wt[i] >= 0.5f)
					a.push_back(std::fabs(float(f.v[i] - mean)));
			if (a.empty())
				for (float v : f.v)
					a.push_back(std::fabs(float(v - mean)));
			float p = 0;
			if (!a.empty())
			{
				const size_t at = std::min(a.size() - 1, size_t(double(a.size()) * 0.999));
				std::nth_element(a.begin(), a.begin() + ptrdiff_t(at), a.end());
				p = a[at];
			}
			span = p > 1e-6f ? 2.0f * p : 0.0f;
			std::vector<float> s(f.v.size(), 0.5f);
			if (span > 0)
				for (size_t i = 0; i < s.size(); ++i)
					s[i] = std::clamp(0.5f + float(f.v[i] - mean) / span, 0.0f, 1.0f);
			return s;
		}

		uint8_t to8(float s) { return uint8_t(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f)); }
	}

	const char *name(scale s)
	{
		return s == scale::full ? "full" : s == scale::game ? "game" : "half";
	}

	bool parse(const std::string &text, scale &out)
	{
		if (text == "half")
			out = scale::half;
		else if (text == "full")
			out = scale::full;
		else if (text == "game")
			out = scale::game;
		else
			return false;
		return true;
	}

	bool size_for(scale s, uint32_t cw, uint32_t ch, uint32_t w, uint32_t h, uint32_t &out_w, uint32_t &out_h)
	{
		out_w = out_h = 0;
		if (w == 0 || h == 0 || cw < w || ch < h)
			return false;
		uint32_t mw = cw, mh = ch;
		if (s == scale::game)
			mw = w, mh = h;
		else if (s == scale::half && cw / 2 >= w && ch / 2 >= h && cw % 2 == 0 && ch % 2 == 0)
			mw = cw / 2, mh = ch / 2;
		if (mw % 4 != 0 || mh % 4 != 0 || mw > kMaxSide || mh > kMaxSide || cw % mw != 0 || ch % mh != 0)
			return false;
		out_w = mw, out_h = mh;
		return true;
	}

	float weight_of(uint8_t alpha, uint8_t alpha_kind, float cut_threshold)
	{
		switch (alpha_kind)
		{
		case kAlphaCut:
		case kAlphaMixed:
			return float(alpha) > cut_threshold ? 1.0f : 0.0f;
		case kAlphaBlended:
			return std::min(1.0f, float(alpha) / 128.0f);
		default:
			return 1.0f;
		}
	}

	bool from_colour(const uint8_t *rgba, uint32_t cw, uint32_t ch, uint32_t orig_w, uint32_t w, uint32_t h, uint8_t alpha_kind, float cut_threshold,
		field &out, std::string &why)
	{
		out = field {};
		std::vector<float> lum, wt;
		if (orig_w == 0 || !lum_and_weights(rgba, cw, ch, w, h, alpha_kind, cut_threshold, lum, wt, why))
			return false;
		const float r = float(w) / float(orig_w); // (map texels an original texel)
		const std::vector<float> broad = nblur(lum, wt, w, h, kBroad * r);
		std::vector<float> hp(lum.size());
		for (size_t i = 0; i < hp.size(); ++i)
			hp[i] = lum[i] - broad[i];
		std::vector<float> v(hp.size(), 0.0f);
		for (const auto &[sigma, weight] : { std::pair{ 0.6f, 1.0f }, std::pair{ 1.2f, 0.7f }, std::pair{ 2.5f, 0.4f } })
		{
			const std::vector<float> b = nblur(hp, wt, w, h, sigma);
			for (size_t i = 0; i < v.size(); ++i)
				v[i] += weight * b[i];
		}
		// (nmap_multi's slopes are a map texel's: in original texels the same tilt is H / r)
		for (float &x : v)
			x *= kRelief / r;
		out.w = w, out.h = h;
		out.v = std::move(v);
		out.wt = std::move(wt);
		return true;
	}

	bool from_game(const uint8_t *bytes, uint32_t gw, uint32_t gh, const uint8_t *rgba, uint32_t cw, uint32_t ch, uint32_t orig_w, uint32_t w, uint32_t h,
		uint8_t alpha_kind, float cut_threshold, field &out, std::string &why)
	{
		out = field {};
		if (bytes == nullptr || gw == 0 || gh == 0 || orig_w == 0)
		{
			why = "no height bytes";
			return false;
		}
		std::vector<float> lum, wt;
		if (!lum_and_weights(rgba, cw, ch, w, h, alpha_kind, cut_threshold, lum, wt, why))
			return false;
		std::vector<float> v(size_t(w) * h);
		for (uint32_t y = 0; y < h; ++y)
		{
			const float sy = (float(y) + 0.5f) * float(gh) / float(h) - 0.5f;
			const int y0 = int(std::floor(sy));
			const float fy = sy - float(y0);
			const uint32_t ya = uint32_t(((y0 % int(gh)) + int(gh)) % int(gh)), yb = (ya + 1) % gh;
			for (uint32_t x = 0; x < w; ++x)
			{
				const float sx = (float(x) + 0.5f) * float(gw) / float(w) - 0.5f;
				const int x0 = int(std::floor(sx));
				const float fx = sx - float(x0);
				const uint32_t xa = uint32_t(((x0 % int(gw)) + int(gw)) % int(gw)), xb = (xa + 1) % gw;
				const float a = bytes[size_t(ya) * gw + xa], b = bytes[size_t(ya) * gw + xb], c = bytes[size_t(yb) * gw + xa], d = bytes[size_t(yb) * gw + xb];
				v[size_t(y) * w + x] = ((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy) / 255.0f;
			}
		}
		const float r = float(w) / float(orig_w);
		const std::vector<float> broad = nblur(v, wt, w, h, kBroad * r);
		for (size_t i = 0; i < v.size(); ++i)
			v[i] -= broad[i];
		out.w = w, out.h = h;
		out.v = std::move(v);
		out.wt = std::move(wt);
		return true;
	}

	std::vector<uint8_t> texels(const field &f, float &span)
	{
		const std::vector<float> s = normalised(f, span);
		std::vector<uint8_t> out(s.size());
		for (size_t i = 0; i < s.size(); ++i)
			out[i] = to8(s[i]);
		return out;
	}

	made encode(const field &f)
	{
		made m;
		const uint32_t levels = bc2::chain_levels(f.w, f.h);
		if (levels == 0 || f.v.size() != size_t(f.w) * f.h || f.wt.size() != f.v.size())
			return m;
		std::vector<float> s = normalised(f, m.span), wt = f.wt;
		uint32_t w = f.w, h = f.h;
		m.w = w, m.h = h;
		for (uint32_t level = 0; level < levels; ++level)
		{
			if (level > 0)
			{
				// (coverage-aware 2x2: the texels the game shows decide; none shown: the plain mean)
				const uint32_t hw = w / 2, hh = h / 2;
				std::vector<float> s2(size_t(hw) * hh), w2(size_t(hw) * hh);
				for (uint32_t y = 0; y < hh; ++y)
					for (uint32_t x = 0; x < hw; ++x)
					{
						const size_t i[4] = { size_t(2 * y) * w + 2 * x, size_t(2 * y) * w + 2 * x + 1, size_t(2 * y + 1) * w + 2 * x, size_t(2 * y + 1) * w + 2 * x + 1 };
						float sw = 0, n = 0, plain = 0;
						for (size_t k : i)
							sw += s[k] * wt[k], n += wt[k], plain += s[k];
						s2[size_t(y) * hw + x] = n > 1e-6f ? sw / n : plain / 4;
						w2[size_t(y) * hw + x] = n / 4;
					}
				s.swap(s2), wt.swap(w2);
				w = hw, h = hh;
			}
			std::vector<uint8_t> t(s.size());
			for (size_t i = 0; i < s.size(); ++i)
				t[i] = to8(s[i]);
			m.levels.push_back(bc4::encode(t.data(), w, h));
		}
		return m;
	}

	void normals(const float *v, uint32_t w, uint32_t h, float gain, std::vector<float> &out)
	{
		out.assign(size_t(w) * h * 3, 0.0f);
		auto at = [&](int x, int y) { return v[size_t(((y % int(h)) + int(h)) % int(h)) * w + uint32_t(((x % int(w)) + int(w)) % int(w))]; };
		for (int y = 0; y < int(h); ++y)
			for (int x = 0; x < int(w); ++x)
			{
				// (zonemesh::sobel: gy = the row above's 1 2 1 - the row below's, gx = the left column's - the right's)
				const float gy = (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1)) - (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1));
				const float gx = (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1)) - (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1));
				const float a = gy * gain, b = gx * gain, len = std::sqrt(a * a + b * b + 1.0f);
				float *o = out.data() + (size_t(y) * w + x) * 3;
				o[0] = a / len, o[1] = b / len, o[2] = 1.0f / len;
			}
	}
}
