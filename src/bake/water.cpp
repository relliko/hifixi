/*
 * Water (bake.hpp): reshade-worldfx's tools/bake_water.py, in C++. A zone's water from its collision: the triangles
 * whose terrain is shallow (8) or deep (9) water, and the water's surface over them.
 *
 * The collision is what characters stand on: in a river or a lake that's the bed (West Ronfaure's deep middle is half
 * a yalm under its shallow edges, Jugner's nearly three), while the sea's and the city canals' are flat, the surface
 * itself. The game's water doesn't write depth, so the surface is rebuilt here from where the water meets its edges:
 *
 * - The water is laid on a 1-yalm grid (a point a grid square, a column holding stacked water apart).
 * - Its edge points are where the surface meets the bank. An edge point around a rock or at a wall's foot isn't at the
 *   waterline, so each takes the highest edge within 20 yalms along the water, less 0.06 a yalm (a river's own slope
 *   down is less than that, so it isn't lifted).
 * - The surface between them is the smoothest one through them (Laplace), and never under the bed.
 * - Where it slopes, the water flows downhill (for the ripples); the distance to the nearest edge fades the water in at
 *   the shore.
 *
 * The Python does this with numpy; the sums here are added as numpy adds them (its pairwise sum, its sums along an
 * axis in order), a column's heights averaged with CPython's sum(), and the conjugate gradients' dot products (BLAS's
 * there) as OpenBLAS's AVX2 kernel adds them on one thread, so the file is the same bytes. (On several threads OpenBLAS
 * splits a long one, a sea zone's, and the Python's own file then depends on the PC's thread count: a few surface
 * heights a float's last bit apart in Al'Taieu.) A sea zone's million points are kept lean here, in arrays rather than
 * dicts: a 32-bit process.
 *
 * Output (little-endian), the grass file's layout with its own tag and two float4s a corner:
 *     char  magic[4] 'WTR2'
 *     u32   zone
 *     f32   cell size (yalms)
 *     u32   cell count
 *     u32   corner count (three a triangle)
 *     cells:   i32 cx, i32 cz, u32 first corner, u32 corner count   (sorted by cz, then cx)
 *     corners: f32 x, y, z, kind      (the collision: y down positive; kind 8 shallow, 9 deep, +100 steep)
 *              f32 surface y, edge distance (yalms, at most 8), flow x, flow z (yalms a second)
 *     (grouped by cell, three a triangle; a triangle goes in the cell of its middle)
 */

#include "bake.hpp"
#include "collision.hpp"
#include "dat.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <vector>

namespace hifi::bake
{
	namespace
	{
		constexpr double kCell = 32.0;     // drawing cells (yalms)
		constexpr double kGrid = 1.0;      // the surface grid (yalms)
		constexpr double kSteep = 0.766044443118978; // cos(radians(40)): water steeper than this is a fall: no surface
		constexpr double kLayer = 5.0;     // heights in one grid column further apart than this are separate water (stacked)
		constexpr double kStep = 4.0;      // neighbouring grid points further apart in height than this aren't joined
		constexpr double kSlope = 0.06;    // an edge point's waterline: the highest edge within kReach along the water, less this a yalm
		constexpr double kReach = 20.0;
		constexpr double kLift = 4.5;      // ...and only from edges at most this far over the point's own bed (not a fall's top)
		constexpr double kSide = 4.0;      // steep water joins the surface only this tall (a channel's side, not a fall)
		constexpr double kEdgeCap = 8.0;   // edge distance kept up to this (yalms)
		constexpr double kFlow = 15.0;     // flow speed (yalms a second) per unit of the surface's slope
		constexpr double kFlowCap = 1.5;
		constexpr double kInf = std::numeric_limits<double>::infinity();
		constexpr double kSqrt2 = 1.4142135623730951; // (math.sqrt(2.0))

		const int kOffsets[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

		struct tri
		{
			double p[3][3];
			uint32_t kind;
		};

		bool steep(const double *a, const double *b, const double *c)
		{
			const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
			const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
			const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			const double n = std::sqrt(nx * nx + ny * ny + nz * nz);
			return n < 1e-9 || std::fabs(ny) / n < kSteep;
		}

		// numpy's maximum and minimum (a NaN in either is the answer; the first when they're equal)
		double np_max(double a, double b) { return (a >= b || std::isnan(a)) ? a : b; }
		double np_min(double a, double b) { return (a <= b || std::isnan(a)) ? a : b; }

		// The Python's x @ y (numpy hands it to BLAS's ddot): OpenBLAS's AVX2 kernel on one thread, as the Python runs
		// here: sixteen running sums (four registers of four) fused-multiply-added over each block of 16, the registers'
		// halves added, then the registers, then the two lanes; the rest added one by one after
		double blas_dot(size_t n, const double *x, const double *y)
		{
			const size_t n1 = n & ~size_t(15);
			double dot = 0.0;
			if (n1 != 0)
			{
				double acc[16] = {};
				for (size_t i = 0; i < n1; i += 16)
					for (size_t k = 0; k < 16; ++k)
						acc[k] = std::fma(x[i + k], y[i + k], acc[k]);
				double t[4][2];
				for (int r = 0; r < 4; ++r)
				{
					t[r][0] = acc[r * 4] + acc[r * 4 + 2];
					t[r][1] = acc[r * 4 + 1] + acc[r * 4 + 3];
				}
				const double u0 = (t[0][0] + t[1][0]) + (t[2][0] + t[3][0]);
				const double u1 = (t[0][1] + t[1][1]) + (t[2][1] + t[3][1]);
				dot = u0 + u1;
			}
			for (size_t i = n1; i < n; ++i)
				dot = std::fma(y[i], x[i], dot);
			return 0.0 + dot; // (numpy's sum of the chunks, from 0)
		}

		// The grid points: in the order the Python makes them (its columns in the order the triangles first reach them,
		// each column's points from the lowest bed up), and the columns by (ix, iz) for the lookups (the Python's
		// `where`: a column's points follow each other)
		struct column
		{
			int32_t ix, iz;
			uint32_t first, count; // (its samples while they're gathered; then its points)
		};
		struct points
		{
			std::vector<int32_t> ix, iz;
			std::vector<double> bed;
			std::vector<column> columns; // (sorted by ix, then iz)

			const column *find(int64_t x, int64_t z) const
			{
				const auto it = std::lower_bound(columns.begin(), columns.end(), std::make_pair(x, z),
					[](const column &c, const std::pair<int64_t, int64_t> &k) { return std::make_pair(int64_t(c.ix), int64_t(c.iz)) < k; });
				return it != columns.end() && it->ix == x && it->iz == z ? &*it : nullptr;
			}
		};

		// rasterize: each joined triangle's height at the grid squares' middles it covers (handed to f(ix, iz, y), in the
		// Python's order); false when a square's index won't fit 32 bits
		template <typename F>
		bool rasterize(const std::vector<tri> &tris, const std::vector<uint8_t> &joined, F &&f)
		{
			for (size_t k = 0; k < tris.size(); ++k)
			{
				if (!joined[k])
					continue;
				const double *a = tris[k].p[0], *b = tris[k].p[1], *c = tris[k].p[2];
				const double minx = std::min({ a[0], b[0], c[0] }), maxx = std::max({ a[0], b[0], c[0] });
				const double minz = std::min({ a[2], b[2], c[2] }), maxz = std::max({ a[2], b[2], c[2] });
				const double ix0 = std::ceil(minx / kGrid - 0.5), ix1 = std::floor(maxx / kGrid - 0.5);
				const double iz0 = std::ceil(minz / kGrid - 0.5), iz1 = std::floor(maxz / kGrid - 0.5);
				const double d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
				if (ix0 > ix1 || iz0 > iz1 || std::fabs(d) < 1e-9)
					continue;
				if (ix0 < -1e9 || ix1 > 1e9 || iz0 < -1e9 || iz1 > 1e9)
					return false;
				for (int32_t ix = int32_t(ix0); ix <= int32_t(ix1); ++ix)
				{
					const double px = (double(ix) + 0.5) * kGrid;
					for (int32_t iz = int32_t(iz0); iz <= int32_t(iz1); ++iz)
					{
						const double pz = (double(iz) + 0.5) * kGrid;
						const double l1 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / d;
						const double l2 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / d;
						const double l3 = 1.0 - l1 - l2;
						if (l1 < -1e-6 || l2 < -1e-6 || l3 < -1e-6)
							continue;
						f(ix, iz, l1 * a[1] + l2 * b[1] + l3 * c[1]);
					}
				}
			}
			return true;
		}

		// The grid points from the joined triangles: each column's heights sorted, those within kLayer of the one below
		// averaged into one point (bed y)
		bool make_points(const std::vector<tri> &tris, const std::vector<uint8_t> &joined, points &pt)
		{
			// The heights each square's middle gets, in the order they come (counted first: no growing)
			size_t n = 0;
			if (!rasterize(tris, joined, [&](int32_t, int32_t, double) { n++; }))
				return false;
			struct sample
			{
				int32_t ix, iz;
				uint32_t seq;
				double y;
			};
			std::vector<sample> samples;
			samples.reserve(n);
			rasterize(tris, joined, [&](int32_t ix, int32_t iz, double y) { samples.push_back(sample{ ix, iz, uint32_t(samples.size()), y }); });
			// (by column, each column's in the order they came)
			std::sort(samples.begin(), samples.end(), [](const sample &a, const sample &b) {
				return a.ix != b.ix ? a.ix < b.ix : (a.iz != b.iz ? a.iz < b.iz : a.seq < b.seq);
			});
			for (size_t i = 0; i < samples.size();)
			{
				size_t j = i + 1;
				while (j < samples.size() && samples[j].ix == samples[i].ix && samples[j].iz == samples[i].iz)
					j++;
				pt.columns.push_back(column{ samples[i].ix, samples[i].iz, uint32_t(i), uint32_t(j - i) });
				i = j;
			}
			// The columns in the order the Python's dict has them (where each was first reached)
			std::vector<uint32_t> order(pt.columns.size());
			for (uint32_t c = 0; c < order.size(); ++c)
				order[c] = c;
			std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return samples[pt.columns[a].first].seq < samples[pt.columns[b].first].seq; });
			std::vector<double> ys, group;
			for (const uint32_t c : order)
			{
				column &col = pt.columns[c];
				ys.clear();
				for (uint32_t s = 0; s < col.count; ++s)
					ys.push_back(samples[col.first + s].y);
				std::stable_sort(ys.begin(), ys.end());
				col.first = uint32_t(pt.bed.size());
				group.assign(1, ys[0]);
				for (size_t k = 1; k <= ys.size(); ++k)
				{
					if (k == ys.size() || ys[k] - group.back() > kLayer)
					{
						pt.ix.push_back(col.ix);
						pt.iz.push_back(col.iz);
						pt.bed.push_back(collision::python_sum(group.data(), group.size()) / double(group.size()));
						if (k < ys.size())
							group.assign(1, ys[k]);
					}
					else
						group.push_back(ys[k]);
				}
				col.count = uint32_t(pt.bed.size()) - col.first;
			}
			return true;
		}

		// solve_surface: per point the surface y (the game's, down positive), edge distance, flow (x, z a point)
		void solve_surface(const points &pt, std::vector<double> &surf, std::vector<double> &dist, std::vector<double> &flow)
		{
			const size_t n = pt.bed.size();
			auto up = [&](size_t i) { return -pt.bed[i]; }; // (the game's y is down positive)
			std::vector<int32_t> nb(n * 8, -1);
			for (size_t i = 0; i < n; ++i)
				for (int k = 0; k < 8; ++k)
				{
					int32_t best = -1;
					double best_d = kStep;
					if (const column *c = pt.find(int64_t(pt.ix[i]) + kOffsets[k][0], int64_t(pt.iz[i]) + kOffsets[k][1]))
						for (uint32_t j = c->first; j < c->first + c->count; ++j)
						{
							const double dd = std::fabs(pt.bed[j] - pt.bed[i]);
							if (dd < best_d)
							{
								best = int32_t(j);
								best_d = dd;
							}
						}
					nb[i * 8 + k] = best;
				}
			std::vector<uint8_t> edge(n);
			for (size_t i = 0; i < n; ++i)
				edge[i] = nb[i * 8] < 0 || nb[i * 8 + 1] < 0 || nb[i * 8 + 2] < 0 || nb[i * 8 + 3] < 0;
			const double cost[8] = { kGrid, kGrid, kGrid, kGrid, kGrid * kSqrt2, kGrid * kSqrt2, kGrid * kSqrt2, kGrid * kSqrt2 };

			// Each edge point's waterline: the highest edge within kReach along the water, less kSlope a yalm
			std::vector<double> v(n), cand(n);
			for (size_t i = 0; i < n; ++i)
				v[i] = edge[i] ? up(i) : -kInf;
			for (int step = 0; step < int(kReach / kGrid); ++step)
				for (int k = 0; k < 8; ++k)
				{
					const double less = kSlope * cost[k];
					for (size_t i = 0; i < n; ++i)
					{
						const int32_t j = nb[i * 8 + k];
						double cv = j >= 0 ? v[j] - less : -kInf;
						if (cv > up(i) + kLift)
							cv = -kInf;
						cand[i] = cv;
					}
					for (size_t i = 0; i < n; ++i)
						v[i] = np_max(v[i], cand[i]);
				}

			// The smoothest surface through them (Laplace, by conjugate gradients over the inner points). v becomes the
			// surface: the edges' waterlines, the beds under the inner points till they're solved.
			std::vector<uint32_t> inner;
			for (size_t i = 0; i < n; ++i)
				if (!edge[i])
					inner.push_back(uint32_t(i));
			const size_t m = inner.size();
			if (m != 0)
			{
				std::vector<double> x(m), r(m), p(m + 1, 0.0), ap(m);
				for (size_t s = 0; s < m; ++s)
					x[s] = v[inner[s]];
				size_t edges = 0;
				for (size_t i = 0; i < n; ++i)
					if (edge[i])
						cand[edges++] = v[i];
				const double mean = edges != 0 ? collision::numpy_sum(cand.data(), edges) / double(edges) : 0.0;
				for (double &e : x)
					if (!std::isfinite(e))
						e = mean;
				for (size_t i = 0; i < n; ++i)
					if (!edge[i])
						v[i] = up(i);
				// (an edge neighbour: its slot is m, the zero at the end; fixed: the edges' values only)
				std::vector<uint32_t> slot(n, uint32_t(m));
				for (size_t s = 0; s < m; ++s)
					slot[inner[s]] = uint32_t(s);
				auto fixed = [&](int32_t j) { return edge[j] ? v[j] : 0.0; };
				// (4 x - the neighbours' sum, the neighbours added in order from 0)
				auto apply = [&](const std::vector<double> &xs, size_t s) {
					const int32_t *q = &nb[size_t(inner[s]) * 8];
					return 4.0 * xs[s] - (0.0 + (((xs[slot[q[0]]] + xs[slot[q[1]]]) + xs[slot[q[2]]]) + xs[slot[q[3]]]));
				};
				p.assign(m + 1, 0.0);
				std::copy(x.begin(), x.end(), p.begin());
				for (size_t s = 0; s < m; ++s)
				{
					const int32_t *q = &nb[size_t(inner[s]) * 8];
					const double rhs = 0.0 + (((fixed(q[0]) + fixed(q[1])) + fixed(q[2])) + fixed(q[3]));
					r[s] = rhs - apply(p, s);
				}
				std::copy(r.begin(), r.end(), p.begin());
				p[m] = 0.0;
				double rr = blas_dot(m, r.data(), r.data());
				for (int it = 0; it < 5000; ++it)
				{
					if (rr < 1e-8 * double(m))
						break;
					for (size_t s = 0; s < m; ++s)
						ap[s] = apply(p, s);
					const double a = rr / blas_dot(m, p.data(), ap.data());
					for (size_t s = 0; s < m; ++s)
						x[s] += a * p[s];
					for (size_t s = 0; s < m; ++s)
						r[s] -= a * ap[s];
					const double rr2 = blas_dot(m, r.data(), r.data());
					const double beta = rr2 / rr;
					for (size_t s = 0; s < m; ++s)
						p[s] = r[s] + beta * p[s];
					rr = rr2;
				}
				for (size_t s = 0; s < m; ++s)
					v[inner[s]] = x[s];
			}
			inner = std::vector<uint32_t>();
			std::vector<double> &surface = v;
			for (size_t i = 0; i < n; ++i)
				surface[i] = np_max(surface[i], up(i));

			// How far each point is from the water's edge, along the water
			dist.assign(n, 0.0);
			for (size_t i = 0; i < n; ++i)
				dist[i] = edge[i] ? 0.0 : kInf;
			for (int step = 0; step < int(kEdgeCap / kGrid) + 2; ++step)
				for (int k = 0; k < 8; ++k)
				{
					for (size_t i = 0; i < n; ++i)
					{
						const int32_t j = nb[i * 8 + k];
						cand[i] = j >= 0 ? dist[j] + cost[k] : kInf;
					}
					for (size_t i = 0; i < n; ++i)
						dist[i] = np_min(dist[i], cand[i]);
				}
			for (size_t i = 0; i < n; ++i)
				dist[i] = np_min(dist[i], kEdgeCap);
			cand = std::vector<double>();
			edge = std::vector<uint8_t>();

			// The flow: downhill on the surface, faster the steeper, smoothed
			auto nan_to_num = [](double e) {
				if (std::isnan(e))
					return 0.0;
				if (std::isinf(e))
					return e > 0 ? std::numeric_limits<double>::max() : -std::numeric_limits<double>::max();
				return e;
			};
			// (nanmean of the two one-sided slopes: the NaNs, where a neighbour is missing, left out)
			auto slope = [&](size_t i, int k_plus, int k_minus) {
				const int32_t jp = nb[i * 8 + k_plus], jm = nb[i * 8 + k_minus];
				const double a0 = jp >= 0 ? surface[jp] - surface[i] : std::nan("");
				const double a1 = jm >= 0 ? surface[i] - surface[jm] : std::nan("");
				const double c = double(!std::isnan(a0)) + double(!std::isnan(a1));
				const double tot = (0.0 + (std::isnan(a0) ? 0.0 : a0)) + (std::isnan(a1) ? 0.0 : a1);
				return nan_to_num(tot / c / kGrid);
			};
			flow.assign(n * 2, 0.0);
			for (size_t i = 0; i < n; ++i)
			{
				const double gx = slope(i, 0, 1), gz = slope(i, 2, 3);
				flow[i * 2] = -gx * kFlow;
				flow[i * 2 + 1] = -gz * kFlow;
			}
			std::vector<double> acc(n * 2);
			for (int pass = 0; pass < 4; ++pass)
			{
				for (size_t i = 0; i < n; ++i)
				{
					double ax = flow[i * 2], az = flow[i * 2 + 1];
					for (int k = 0; k < 8; ++k)
					{
						const int32_t j = nb[i * 8 + k];
						if (j >= 0)
						{
							ax += flow[size_t(j) * 2];
							az += flow[size_t(j) * 2 + 1];
						}
					}
					acc[i * 2] = ax;
					acc[i * 2 + 1] = az;
				}
				for (size_t i = 0; i < n; ++i)
				{
					double c = 1.0;
					for (int k = 0; k < 8; ++k)
						if (nb[i * 8 + k] >= 0)
							c += 1;
					flow[i * 2] = acc[i * 2] / c;
					flow[i * 2 + 1] = acc[i * 2 + 1] / c;
				}
			}
			for (size_t i = 0; i < n; ++i)
			{
				double &fx = flow[i * 2], &fz = flow[i * 2 + 1];
				const double speed = std::sqrt(0.0 + (fx * fx + fz * fz));
				const double f = np_min(speed, kFlowCap) / np_max(speed, 1e-9);
				fx = fx * f;
				fz = fz * f;
				if (speed < 0.05)
					fx = fz = 0.0;
			}
			for (size_t i = 0; i < n; ++i)
				surface[i] = -surface[i];
			surf = std::move(surface);
		}

		void put32(std::vector<uint8_t> &out, size_t at, uint32_t v)
		{
			std::memcpy(out.data() + at, &v, 4);
		}
		void putf(std::vector<uint8_t> &out, size_t at, double v)
		{
			const float f = float(v);
			std::memcpy(out.data() + at, &f, 4);
		}

		bool bake_water(uint32_t zone, const std::wstring &out, result &r)
		{
			std::vector<tri> tris;
			{
				std::vector<uint8_t> dat;
				collision::reader rd;
				if (!collision::read_zone(zone, dat, r) || !collision::open(dat, rd, r))
					return false;
				dat = std::vector<uint8_t>();
				collision::each(rd, [&](const collision::triangle &t) {
					if (t.terrain == collision::kShallow || t.terrain == collision::kDeep)
					{
						tri w;
						std::memcpy(w.p, t.p, sizeof(w.p));
						w.kind = t.terrain;
						tris.push_back(w);
					}
				});
			}
			if (tris.empty())
			{
				r.none = true;
				r.why = "no water";
				return false;
			}
			for (const tri &t : tris)
				for (const auto &p : t.p)
					if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
					{
						// (the Python's math.ceil raises on it, or its arrays fill with NaNs: it bakes nothing sound)
						r.why = "a water triangle's corner isn't finite";
						return false;
					}

			// (steep water too, when short: a deep channel's sides join it to its shallows, so they share one surface;
			// a fall's don't join the pools above and below it)
			std::vector<uint8_t> joined(tris.size());
			bool any = false;
			for (size_t k = 0; k < tris.size(); ++k)
			{
				const tri &t = tris[k];
				joined[k] = !steep(t.p[0], t.p[1], t.p[2]) ||
					std::max({ t.p[0][1], t.p[1][1], t.p[2][1] }) - std::min({ t.p[0][1], t.p[1][1], t.p[2][1] }) <= kSide;
				any = any || joined[k];
			}
			points pt;
			std::vector<double> surf, dist, flow;
			if (any)
			{
				if (!make_points(tris, joined, pt))
				{
					r.why = "the water is too far out";
					return false;
				}
				solve_surface(pt, surf, dist, flow);
			}
			joined = std::vector<uint8_t>();

			// surface y, edge distance, flow x, z at a corner: from the grid points around it at its height
			auto at = [&](const double *p, double o[4]) {
				o[0] = p[1];
				o[1] = o[2] = o[3] = 0.0;
				if (!any)
					return;
				const int64_t cx = int64_t(std::floor(p[0] / kGrid)), cz = int64_t(std::floor(p[2] / kGrid));
				double ws = 0.0, s = 0.0, d = 0.0, fx = 0.0, fz = 0.0;
				for (int dx = -1; dx <= 1; ++dx)
					for (int dz = -1; dz <= 1; ++dz)
					{
						const column *c = pt.find(cx + dx, cz + dz);
						if (c == nullptr)
							continue;
						for (uint32_t j = c->first; j < c->first + c->count; ++j)
						{
							if (std::fabs(pt.bed[j] - p[1]) >= kStep)
								continue;
							const double ex = (double(pt.ix[j]) + 0.5) * kGrid - p[0], ez = (double(pt.iz[j]) + 0.5) * kGrid - p[2];
							const double w = 1.0 / (ex * ex + ez * ez + 0.05);
							ws += w;
							s += w * surf[j];
							d += w * dist[j];
							fx += w * flow[size_t(j) * 2];
							fz += w * flow[size_t(j) * 2 + 1];
						}
					}
				if (ws == 0.0)
					return;
				// (Python's min(s / ws, p[1]))
				const double sw = s / ws;
				o[0] = p[1] < sw ? p[1] : sw;
				o[1] = d / ws;
				o[2] = fx / ws;
				o[3] = fz / ws;
			};

			// Each triangle in the cell of its middle; cells sorted by cz, then cx, triangles in their order
			std::map<std::pair<int64_t, int64_t>, std::vector<uint32_t>> cells;
			for (uint32_t k = 0; k < tris.size(); ++k)
			{
				const tri &t = tris[k];
				const double mx = (t.p[0][0] + t.p[1][0] + t.p[2][0]) / 3.0, mz = (t.p[0][2] + t.p[1][2] + t.p[2][2]) / 3.0;
				cells[{ int64_t(std::floor(mz / kCell)), int64_t(std::floor(mx / kCell)) }].push_back(k);
			}
			const size_t corners = 3 * tris.size(), head = 20, table = head + 16 * cells.size();
			std::vector<uint8_t> file(table + 32 * corners);
			std::memcpy(file.data(), "WTR2", 4);
			put32(file, 4, zone);
			putf(file, 8, kCell);
			put32(file, 12, uint32_t(cells.size()));
			put32(file, 16, uint32_t(corners));
			size_t at_cell = head, at_corner = table;
			uint32_t first = 0;
			for (const auto &[key, list] : cells)
			{
				const uint32_t count = uint32_t(3 * list.size());
				put32(file, at_cell, uint32_t(int32_t(key.second)));
				put32(file, at_cell + 4, uint32_t(int32_t(key.first)));
				put32(file, at_cell + 8, first);
				put32(file, at_cell + 12, count);
				at_cell += 16;
				first += count;
				for (const uint32_t k : list)
				{
					const tri &t = tris[k];
					const uint32_t kind = t.kind + (steep(t.p[0], t.p[1], t.p[2]) ? 100 : 0);
					for (const auto &p : t.p)
					{
						double o[4];
						at(p, o);
						const double rec[8] = { p[0], p[1], p[2], double(kind), o[0], o[1], o[2], o[3] };
						for (int e = 0; e < 8; ++e)
							putf(file, at_corner + 4 * e, rec[e]);
						at_corner += 32;
					}
				}
			}
			if (!write_file(out, file.data(), file.size()))
			{
				r.why = "the file couldn't be written";
				return false;
			}
			return true;
		}
	}

	bool water(uint32_t zone, const std::wstring &out, result &r)
	{
		try
		{
			return bake_water(zone, out, r);
		}
		catch (const std::bad_alloc &)
		{
			r = result();
			r.why = "out of memory";
			return false;
		}
	}
}
