// keeptexels.py in C++ (keeptexels.hpp). The script's steps, in its order, on doubles:
//   lum = out @ (0.3, 0.59, 0.11); gy, gx = gradient(enlarge(shrink(lum, s), s)); grad = hypot(gx, gy) * s;
//   flat = exp(-(grad / EDGE)^2); g = GRAIN when both sides divide by it, else 1; target = shrink(orig, g);
//   ROUNDS times: out = clip(out + flat * enlarge(target - shrink(out, s*g), s*g), 0, 255); round half to even.

#include "keeptexels.hpp"
#include <cmath>
#include <vector>

namespace hifi::texpack::keeptexels
{
	namespace
	{
		// Each S x S block averaged (c channels)
		std::vector<double> shrink(const std::vector<double> &a, uint32_t w, uint32_t h, uint32_t c, uint32_t S)
		{
			const uint32_t sw = w / S, sh = h / S;
			std::vector<double> out(size_t(sw) * sh * c, 0.0);
			for (uint32_t y = 0; y < sh * S; ++y)
				for (uint32_t x = 0; x < sw * S; ++x)
					for (uint32_t k = 0; k < c; ++k)
						out[(size_t(y / S) * sw + x / S) * c + k] += a[(size_t(y) * w + x) * c + k];
			const double n = double(S) * double(S);
			for (double &v : out)
				v /= n;
			return out;
		}

		// Bilinear S x, wrapping at the edges (the textures tile)
		std::vector<double> enlarge(const std::vector<double> &a, uint32_t w, uint32_t h, uint32_t c, uint32_t S)
		{
			const uint32_t W = w * S, H = h * S;
			std::vector<double> out(size_t(W) * H * c);
			std::vector<uint32_t> xa(W), xb(W);
			std::vector<double> fxs(W);
			for (uint32_t X = 0; X < W; ++X)
			{
				const double x = (X + 0.5) / S - 0.5, x0 = std::floor(x);
				fxs[X] = x - x0;
				xa[X] = uint32_t(((int64_t(x0) % int64_t(w)) + w) % w);
				xb[X] = uint32_t(((int64_t(x0) + 1) % int64_t(w) + w) % w);
			}
			for (uint32_t Y = 0; Y < H; ++Y)
			{
				const double y = (Y + 0.5) / S - 0.5, y0 = std::floor(y), fy = y - y0;
				const uint32_t ya = uint32_t(((int64_t(y0) % int64_t(h)) + h) % h), yb = uint32_t(((int64_t(y0) + 1) % int64_t(h) + h) % h);
				for (uint32_t X = 0; X < W; ++X)
				{
					const double fx = fxs[X];
					for (uint32_t k = 0; k < c; ++k)
					{
						const double a00 = a[(size_t(ya) * w + xa[X]) * c + k], a01 = a[(size_t(ya) * w + xb[X]) * c + k];
						const double a10 = a[(size_t(yb) * w + xa[X]) * c + k], a11 = a[(size_t(yb) * w + xb[X]) * c + k];
						out[(size_t(Y) * W + X) * c + k] = a00 * (1 - fx) * (1 - fy) + a01 * fx * (1 - fy) + a10 * (1 - fx) * fy + a11 * fx * fy;
					}
				}
			}
			return out;
		}
	}

	bool apply(const uint8_t *orig, uint32_t w, uint32_t h, uint8_t *up, uint32_t W, uint32_t H, int rounds, double edge, uint32_t grain)
	{
		if (w == 0 || h == 0 || W % w != 0 || H % h != 0 || W / w != H / h)
			return false;
		const uint32_t s = W / w;
		const size_t N = size_t(W) * H;
		std::vector<double> out(N * 3);
		for (size_t i = 0; i < N; ++i)
			for (int k = 0; k < 3; ++k)
				out[i * 3 + k] = up[i * 4 + k];

		// (the fade: the luma's gradient, numpy.gradient's central differences inside and one-sided at the edges)
		std::vector<double> lum(N);
		for (size_t i = 0; i < N; ++i)
			lum[i] = out[i * 3] * 0.3 + out[i * 3 + 1] * 0.59 + out[i * 3 + 2] * 0.11;
		const std::vector<double> E = enlarge(shrink(lum, W, H, 1, s), w, h, 1, s);
		std::vector<double> flat(N);
		for (uint32_t y = 0; y < H; ++y)
			for (uint32_t x = 0; x < W; ++x)
			{
				auto at = [&](uint32_t yy, uint32_t xx) { return E[size_t(yy) * W + xx]; };
				const double gy = H < 2 ? 0.0 : y == 0 ? at(1, x) - at(0, x) : y == H - 1 ? at(H - 1, x) - at(H - 2, x) : (at(y + 1, x) - at(y - 1, x)) / 2;
				const double gx = W < 2 ? 0.0 : x == 0 ? at(y, 1) - at(y, 0) : x == W - 1 ? at(y, W - 1) - at(y, W - 2) : (at(y, x + 1) - at(y, x - 1)) / 2;
				const double grad = std::hypot(gx, gy) * s, r = grad / edge;
				flat[size_t(y) * W + x] = std::exp(-(r * r));
			}

		const uint32_t g = (h % grain == 0 && w % grain == 0) ? grain : 1, S = s * g;
		std::vector<double> o(size_t(w) * h * 3);
		for (size_t i = 0; i < size_t(w) * h; ++i)
			for (int k = 0; k < 3; ++k)
				o[i * 3 + k] = orig[i * 4 + k];
		const std::vector<double> target = shrink(o, w, h, 3, g);
		const uint32_t tw = w / g, th = h / g;
		for (int round = 0; round < rounds; ++round)
		{
			std::vector<double> diff = shrink(out, W, H, 3, S);
			for (size_t i = 0; i < diff.size(); ++i)
				diff[i] = target[i] - diff[i];
			const std::vector<double> e = enlarge(diff, tw, th, 3, S);
			for (size_t i = 0; i < N; ++i)
				for (int k = 0; k < 3; ++k)
				{
					const double v = out[i * 3 + k] + flat[i] * e[i * 3 + k];
					out[i * 3 + k] = v < 0 ? 0 : v > 255 ? 255 : v;
				}
		}
		for (size_t i = 0; i < N; ++i)
			for (int k = 0; k < 3; ++k)
				up[i * 4 + k] = uint8_t(std::nearbyint(out[i * 3 + k]));
		return true;
	}
}
