/*
 * Glows (glows.hpp): the worldfx Ashita addon's glows (worldfx.lua 0.16: send_glows and what it uses), in C++. Lights
 * for glowing things the game draws but lights nothing with (reshade-worldfx's client notes, "Effects that light": the
 * game's own light element is only on zone fixtures, so a Home Point crystal, a spell or a fire lights nothing round
 * it). Every other frame they go to World FX (worldfx_glows), 10 words each as the lamps go: an id, the kind, colour
 * r g b, where (the game's world, y down), reach and brightness (as a lamp's). Two kinds:
 * - Fixtures (kind 2): NPCs the game draws glowing (Home Point crystals, Survival Guides, waypoints...), found in
 *   Ashita's entity list by name (kGlowFixtureScan entities a frame); each a light of its kind's colour at its glow's
 *   height, pulsing slowly.
 * - Effects (kind 3): the game's effect elements alive now. Each is a task in the game's task list (the task
 *   manager's global, found by the code that links a task: mov ecx, [manager]; its +0xD50 the first node, a node's
 *   +0xC the next; a task's node callback +4 is the one found by its code, the others are bare function nodes or the
 *   priority markers). Elements are the tasks whose vtable's draw (+0x2C) is the element draw the rain patch also
 *   finds (046070), not the light element (its vtable from its constructor). The ones that glow: blend letter H, I or
 *   h at +0x16C (additive, as the game starts every element), not hidden (+0x17A), not suspended (+0x22), faded in
 *   (+0x138 at least 0.01), with some colour (+0xF8, 0xAARRGGBB, 0x80 = 1), not placed by the camera, a sky layer or
 *   occlusion-tested (+0x10C bits 2, 22, 18), not a weather's (its generator, [[+0xFC]], +0xDE bit 7: rain and snow
 *   are additive too), not a footstep's (footstep): each step on any ground starts the zone's footstep effect (its
 *   folder fses, fser in rain, the generators in its fefs / fefr) and the walker's own fvib, and they're additive:
 *   every step lit the ground white round your feet (in game, 2026-10-22); not a melee hit's or miss's (melee: the
 *   hit sparks in syst/effe/hit.., the damage and miss flashes in damv / misv). Their weight (brightest channel x alpha x fade x +0x134) is added up by the generator that
 *   made them (one light per emitter: it stays put while its particles come and go); those within kGlowMerge yalms
 *   are merged, and the nearest kGlowEffectsMax within kGlowReach of the camera kept. Where: the game's own view-space
 *   position of this frame (+0x188), back into the world through the effects' view matrix (gfx +0xB50, gfx found by
 *   the effects' frame set-up, 06B480: it's a look-at, so its inverse is its transpose).
 * At Present the game's frame is over (its task list isn't changing: Ashita's present callback runs on its thread), so
 * every pointer is as the game left it. The walk reads the game's memory directly (up to kGlowNodesMax nodes every
 * other frame: ReadProcessMemory for each field would cost too much), each pointer tested as the addon tested it, and
 * the whole walk under a structured exception guard: a pointer that looks good but isn't abandons that walk (no
 * effects sent for it) instead of crashing the game. Floats are decoded from their bits, never read as floats. Read
 * only: nothing is written to the game.
 */

#include "glows.hpp"
#include <windows.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace hifi::game::glows
{
	namespace
	{
		// The game's code the effect walk needs (and where in each pattern its address is)
		constexpr const char *kGlowTasks = "8B44240450518B0D????????E8????????C20400";                                         // +8
		constexpr const char *kGlowTaskCallback = "568B7424088BCE8B06FF502084C075088B168BCE5EFF6224";                         // +0
		constexpr const char *kGlowGfx = "8B413483E800742F48752F8B0D????????E8????????8B0D????????E8????????8B0D????????6A006A006A006A00E8"; // +0xD
		constexpr const char *kGlowDraw = "8D8DF8000000D91C2450E8????????8B00A3????????8A857A01000084C00F85";                  // -0x17
		constexpr const char *kGlowLight = "C706????????66C78676010000B401";                                                    // +2
		constexpr uint32_t kGlowsMax = 64, kGlowWords = 10, kGlowsEvery = 2;
		constexpr uint32_t kGlowEffectsMax = 16, kGlowNodesMax = 8192;
		constexpr double kGlowReach = 40.0, kGlowMerge = 2.0;
		constexpr uint32_t kGlowFixtureScan = 64, kGlowFixtureLast = 1023; // (entities looked at a frame; the zone's NPCs are 0..1023)
		constexpr double kPi = 3.141592653589793;
		constexpr uint32_t kGlowFolders = 4, kGlowSeenMax = 24; // (folders kept per generator; sources /hifi glows recalls)

		// The glowing fixtures, by the start of their name: colour, the glow's height over their feet, reach, brightness,
		// how deep the pulse goes and its period (seconds)
		struct fixture_kind
		{
			const char *name;
			double colour[3];
			double height, range, brightness, pulse, period;
		};
		const fixture_kind kGlowFixtures[] = {
			{ "Home Point",         { 0.35, 0.75, 1.00 }, 1.6, 7, 6, 0.25, 4.0 },
			{ "Survival Guide",     { 1.00, 0.85, 0.55 }, 1.2, 5, 4, 0.15, 5.0 },
			{ "Waypoint",           { 0.45, 1.00, 0.85 }, 1.6, 7, 6, 0.25, 4.0 },
			{ "Telepoint",          { 0.70, 0.90, 1.00 }, 2.0, 8, 6, 0.20, 6.0 },
			{ "Veridical Conflux",  { 0.40, 0.90, 1.00 }, 1.5, 7, 6, 0.25, 4.0 },
			{ "Geomagnetic Fount",  { 0.55, 1.00, 0.60 }, 1.0, 6, 5, 0.25, 3.0 },
			{ "Eschan Portal",      { 0.75, 0.55, 1.00 }, 1.5, 7, 5, 0.20, 5.0 },
			{ "Dimensional Portal", { 0.60, 0.65, 1.00 }, 1.5, 7, 5, 0.20, 5.0 },
			{ "Runic Portal",       { 0.65, 0.85, 1.00 }, 1.5, 6, 5, 0.20, 5.0 },
			{ "Rune of Transfer",   { 0.50, 0.70, 1.00 }, 0.5, 6, 5, 0.20, 4.0 },
			{ "Rune of Release",    { 0.50, 0.70, 1.00 }, 0.5, 6, 5, 0.20, 4.0 },
		};

		// A fixture found: its entity, its kind, its name
		struct fixture
		{
			uint32_t i;
			const fixture_kind *kind;
			std::string name;
		};

		// One generator's glowing elements (or one element's, when nothing made it) as the walk adds them up: their
		// weight, and their view-space positions and colours times their weights; the key (the generator, else the
		// element), the generator and its name (4 characters in a word)
		struct group
		{
			double w, x, y, z, r, g, b;
			uint32_t key, gen, name;
			uint32_t folders[kGlowFolders]; // (the generator's folders' names, nearest first, 0 past the last)
		};

		// What a walk found, in plain memory only (walk_tasks' guard can't unwind objects): how many nodes, elements and
		// glowing ones, where it stopped (0: the list's end), the effects' view matrix (as bits) and the groups in the
		// order they were first met
		struct walk
		{
			uint32_t nodes, elements, glowing, steps, hits, next, groups;
			uint32_t view[16];
			group list[kGlowNodesMax];
		};
		walk g_walk;
		// The groups by key: open addressing, an index into g_walk.list plus 1 (0: empty), twice as many slots as there
		// can be groups
		constexpr uint32_t kSlots = kGlowNodesMax * 2;
		uint16_t g_slots[kSlots];

		// An effect's light: where (the game's world), its colour, its weight, its id (the group's key) and what made it
		struct effect
		{
			double x, y, z, r, g, b, weight;
			uint32_t key;
			std::string name;
		};

		// A source of glowing effects met lately (for /hifi glows: effects that last a moment, a hit's spark, are gone
		// before it can be typed): its generator's name and folders, how many walks it glowed in, its heaviest weight,
		// the walk it last glowed in
		struct seen
		{
			uint32_t name;
			uint32_t folders[kGlowFolders];
			uint32_t walks;
			double weight;
			uint32_t last;
			double at; // (the frame's clock, seconds)
		};

		// The game's code found (find_code): the task manager's and gfx's globals (where they are), the task callback,
		// the element draw, the light element's vtable
		struct code
		{
			uint32_t tasks, callback, gfx, draw, light;
		};

		struct state
		{
			uint32_t frames = 0, sent = 0, n = 0;
			uint32_t words[kGlowsMax * kGlowWords] = {};
			std::vector<fixture> fixtures, found;
			uint32_t scan_at = 0, fixtures_sent = 0;
			bool have_fx = false, looked = false;
			code fx = {};
			std::string why = "not looked for yet";
			std::vector<effect> effects;
			uint32_t elements = 0, glowing = 0, steps = 0, hits = 0, nodes = 0, abandoned = 0;
			bool capped = false;
			double ms = 0.0, ms_most = 0.0;
			uint32_t walks = 0;
			std::vector<group *> order, kept; // (read_effects': kept between walks for their memory)
			std::vector<seen> recent;
			double now = 0.0;
		};
		state g;

		// A word of the game's memory, read directly (only under walk_tasks' guard)
		inline uint32_t at32(uint32_t a)
		{
			return *reinterpret_cast<const uint32_t *>(uintptr_t(a));
		}

		inline uint8_t at8(uint32_t a)
		{
			return *reinterpret_cast<const uint8_t *>(uintptr_t(a));
		}

		inline double sq(double v)
		{
			return v * v;
		}

		// The glowing fixtures: kGlowFixtureScan entities a frame looked at by name, the list swapped in at each full sweep
		void scan_fixtures(const entities &e)
		{
			const uint32_t from = g.scan_at;
			const uint32_t to = std::min(from + kGlowFixtureScan - 1, kGlowFixtureLast);
			for (uint32_t i = from; i <= to; ++i)
			{
				const char *name = e.name(i);
				if (name == nullptr || name[0] == '\0')
					continue;
				for (const fixture_kind &f : kGlowFixtures)
					if (std::strncmp(name, f.name, std::strlen(f.name)) == 0)
					{
						g.found.push_back({ i, &f, name });
						break;
					}
			}
			if (to >= kGlowFixtureLast)
			{
				g.fixtures.swap(g.found);
				g.found.clear();
				g.scan_at = 0;
			}
			else
				g.scan_at = to + 1;
		}

		// A glow into the buffer
		void put_glow(uint32_t id, uint32_t kind, double r, double gr, double b, double x, double y, double z, double range, double brightness)
		{
			if (g.n >= kGlowsMax)
				return;
			uint32_t *w = g.words + g.n * kGlowWords;
			w[0] = id;
			w[1] = kind;
			const double f[8] = { r, gr, b, x, y, z, range, brightness };
			for (int k = 0; k < 8; ++k)
				w[2 + k] = std::bit_cast<uint32_t>(float(f[k]));
			g.n++;
		}

		// The fixtures shown now, as glows: at their feet (Ashita's x east, y north, z down: the game's x, z, y) plus
		// their glow's height, each pulsing on its own phase
		void put_fixtures(const entities &e, double now)
		{
			uint32_t n = 0;
			for (const fixture &f : g.fixtures)
			{
				float x = 0.0f, y = 0.0f, z = 0.0f;
				if (!e.position(f.i, x, y, z) || (x == 0.0f && y == 0.0f) || !e.shown(f.i))
					continue;
				const fixture_kind &k = *f.kind;
				const double pulse = 1 + k.pulse * std::sin(2 * kPi * now / k.period + f.i * 0.7);
				put_glow(0x46000000 + f.i, 2, k.colour[0], k.colour[1], k.colour[2], x, z - k.height, y, k.range, k.brightness * pulse);
				n++;
			}
			g.fixtures_sent = n;
		}

		// The group for a key, made (with its generator and that one's name) the first time it's met
		group &group_of(walk &out, uint32_t key, uint32_t gen)
		{
			uint32_t s = (((key >> 2) * 2654435761u) >> 18) & (kSlots - 1);
			while (g_slots[s] != 0)
			{
				group &grp = out.list[g_slots[s] - 1];
				if (grp.key == key)
					return grp;
				s = (s + 1) & (kSlots - 1);
			}
			group &grp = out.list[out.groups++];
			g_slots[s] = uint16_t(out.groups);
			grp = {};
			grp.key = key;
			grp.gen = gen;
			grp.name = pointer(gen) ? at32(gen + 0x20) : 0;
			// (its folders, as footstep() walks them: each [handle] up the +0x28 parents)
			uint32_t at = gen;
			for (uint32_t up = 0; up < kGlowFolders && pointer(at); ++up)
			{
				const uint32_t handle = at32(at + 0x28);
				const uint32_t parent = pointer(handle) ? at32(handle) : 0;
				if (!pointer(parent))
					break;
				grp.folders[up] = at32(parent + 0x20);
				at = parent;
			}
			return grp;
		}

		// Whether a generator is a footstep's (client notes, "The ripples at a wading character's feet": the footstep code
		// 0D4BC0 starts, each step, the zone's footstep effect from its fses folder (fser in rain), the generators in its
		// subfolder fefs / fefr, and restarts the walker's own fvib; on some ground its footmark fmrk): by its name, or a
		// folder's up its +0x28 parents (each [handle]: a clone's source, its folder, that one's), a few steps up.
		// Only under walk_tasks' guard.
		constexpr uint32_t four(const char (&s)[5])
		{
			return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
		}
		bool footstep(uint32_t gen)
		{
			constexpr uint32_t folders[] = { four("fses"), four("fser"), four("fefs"), four("fefr") };
			const uint32_t name = at32(gen + 0x20);
			if (name == four("fvib") || name == four("fmrk"))
				return true;
			uint32_t at = gen;
			for (int up = 0; up < 4; ++up)
			{
				const uint32_t handle = at32(at + 0x28);
				const uint32_t parent = pointer(handle) ? at32(handle) : 0;
				if (!pointer(parent))
					return false;
				const uint32_t folder = at32(parent + 0x20);
				for (const uint32_t f : folders)
					if (folder == f)
						return true;
				at = parent;
			}
			return false;
		}

		// Whether a generator is a melee hit's or miss's (in game, 2026-10-23, /hifi glows' recent sources while
		// fighting): the hit sparks are clones in ROM/0/0.DAT's syst/effe/hit1..hit9 and hi17, hi26..hi30 (a subfolder
		// g0xx between), the damage and miss flashes in ROM/0/27.DAT's damv / misv (the schedulers 072ED0 looks up).
		// syst/effe also holds other effects (sb.., ei.., geom...), so only those names under it. Only under walk_tasks'
		// guard.
		bool melee(uint32_t gen)
		{
			uint32_t chain[6] = {}, n = 0, at = gen;
			while (n < 6)
			{
				const uint32_t handle = at32(at + 0x28);
				const uint32_t parent = pointer(handle) ? at32(handle) : 0;
				if (!pointer(parent))
					break;
				chain[n++] = at32(parent + 0x20);
				at = parent;
			}
			for (uint32_t i = 0; i < n; ++i)
			{
				const uint32_t f = chain[i];
				if (f == four("damv") || f == four("misv"))
					return true;
				const char c0 = char(f), c1 = char(f >> 8), c2 = char(f >> 16), c3 = char(f >> 24);
				const bool digit2 = c2 >= '0' && c2 <= '9', digit3 = c3 >= '0' && c3 <= '9';
				if (i + 2 < n && chain[i + 1] == four("effe") && chain[i + 2] == four("syst") && c0 == 'h' && c1 == 'i' &&
					((c2 == 't' && digit3) || (digit2 && digit3)))
					return true;
			}
			return false;
		}

		// The walk itself (see above): the game's memory read directly, each pointer tested before it's followed. 1:
		// walked; 0: no task manager or gfx yet.
		int walk_unguarded(const code &fx, walk &out)
		{
			const uint32_t manager = pointer(fx.tasks) ? at32(fx.tasks) : 0;
			const uint32_t gfx = pointer(fx.gfx) ? at32(fx.gfx) : 0;
			if (!pointer(manager) || !pointer(gfx))
				return 0;
			std::memset(g_slots, 0, sizeof(g_slots));
			out.groups = 0;
			uint32_t node = at32(manager + 0xD50);
			uint32_t nodes = 0, elements = 0, glowing = 0, steps = 0, hits = 0;
			while (node != 0 && nodes < kGlowNodesMax && pointer(node))
			{
				nodes++;
				const uint32_t *p = reinterpret_cast<const uint32_t *>(uintptr_t(node));
				const uint8_t *b = reinterpret_cast<const uint8_t *>(p);
				const uint32_t vt = p[0];
				if (p[1] == fx.callback && vt != fx.light && pointer(vt) && at32(vt + 0x2C) == fx.draw)
				{
					elements++;
					const uint8_t letter = b[0x16C];
					const uint32_t fade = p[0x138 / 4];
					if ((letter == 'H' || letter == 'I' || letter == 'h') && b[0x17A] == 0 && *reinterpret_cast<const uint16_t *>(b + 0x22) == 0 &&
						fade >= 0x3C23D70A && fade < 0x7F800000 && (p[0x10C / 4] & 0x00440004) == 0)
					{
						const uint32_t c = p[0xF8 / 4];
						const uint32_t a = c >> 24, r = (c >> 16) & 0xFF, gr = (c >> 8) & 0xFF, bl = c & 0xFF;
						const uint32_t top = std::max(r, std::max(gr, bl));
						const uint32_t handle = p[0xFC / 4];
						const uint32_t gen = pointer(handle) ? at32(handle) : 0;
						const bool weather = pointer(gen) && (at8(gen + 0xDE) & 0x80) != 0;
						const bool step = pointer(gen) && !weather && footstep(gen);
						steps += step ? 1 : 0;
						const bool hit = pointer(gen) && !weather && !step && melee(gen);
						hits += hit ? 1 : 0;
						float v[3];
						if (top >= 0x10 && a > 0 && !weather && !step && !hit && float_of(p[0x188 / 4], v[0]) && float_of(p[0x18C / 4], v[1]) && float_of(p[0x190 / 4], v[2]))
						{
							const double vx = v[0], vy = v[1], vz = v[2];
							if (vx * vx + vy * vy + vz * vz <= kGlowReach * kGlowReach)
							{
								float more_f = 1.0f, fade_f = 0.0f;
								const double more = float_of(p[0x134 / 4], more_f) ? double(more_f) : 1.0;
								float_of(fade, fade_f); // (a number: tested above)
								const double w = (double(top) / 128) * (double(a) / 128) * double(fade_f) * std::min(std::max(more, 0.0), 4.0);
								if (w > 0)
								{
									glowing++;
									group &grp = group_of(out, gen != 0 ? gen : node, gen);
									grp.w = grp.w + w;
									grp.x = grp.x + w * vx;
									grp.y = grp.y + w * vy;
									grp.z = grp.z + w * vz;
									grp.r = grp.r + w * r / 128;
									grp.g = grp.g + w * gr / 128;
									grp.b = grp.b + w * bl / 128;
								}
							}
						}
					}
				}
				node = p[3];
			}
			out.nodes = nodes;
			out.elements = elements;
			out.glowing = glowing;
			out.steps = steps;
			out.hits = hits;
			out.next = node;
			for (uint32_t i = 0; i < 16; ++i)
				out.view[i] = at32(gfx + 0xB50 + i * 4);
			return 1;
		}

		// The walk under a guard: memory gone under it (a pointer that looks good but isn't) abandons it (-1) instead of
		// crashing the game. (Nothing here needs unwinding: the guard can't be in a function that has such objects.)
		int walk_tasks(const code &fx, walk &out)
		{
			__try
			{
				return walk_unguarded(fx, out);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return -1;
			}
		}

		// A group's source counted in g.recent (the one met longest ago makes room)
		void remember(const group &grp)
		{
			for (seen &s : g.recent)
				if (s.name == grp.name && std::memcmp(s.folders, grp.folders, sizeof(s.folders)) == 0)
				{
					if (s.last != g.walks)
						s.walks++;
					s.last = g.walks;
					s.weight = std::max(s.weight, grp.w);
					s.at = g.now;
					return;
				}
			seen s = { grp.name, {}, 1, grp.w, g.walks, g.now };
			std::memcpy(s.folders, grp.folders, sizeof(s.folders));
			if (g.recent.size() < kGlowSeenMax)
				g.recent.push_back(s);
			else
				*std::min_element(g.recent.begin(), g.recent.end(), [](const seen &a, const seen &b) { return a.last < b.last; }) = s;
		}

		// The effects that glow now (see above): the task list walked, its glowing elements grouped by generator, close
		// groups merged, the nearest kGlowEffectsMax into g.effects (none when there's no task list yet, the walk was
		// abandoned or the view matrix isn't numbers)
		void read_effects()
		{
			g.effects.clear();
			const int walked = walk_tasks(g.fx, g_walk);
			if (walked <= 0)
			{
				if (walked < 0)
					g.abandoned++;
				return;
			}
			g.nodes = g_walk.nodes;
			g.elements = g_walk.elements;
			g.glowing = g_walk.glowing;
			g.steps = g_walk.steps;
			g.hits = g_walk.hits;
			g.capped = g_walk.next != 0;
			// (heaviest first, each taking in the lighter ones within kGlowMerge of it)
			g.order.clear();
			for (uint32_t k = 0; k < g_walk.groups; ++k)
			{
				group &grp = g_walk.list[k];
				grp.x = grp.x / grp.w;
				grp.y = grp.y / grp.w;
				grp.z = grp.z / grp.w;
				g.order.push_back(&grp);
				remember(grp);
			}
			std::stable_sort(g.order.begin(), g.order.end(), [](const group *a, const group *b) { return a->w > b->w; });
			g.kept.clear();
			for (group *grp : g.order)
			{
				group *into = nullptr;
				for (group *k : g.kept)
					if (sq(k->x - grp->x) + sq(k->y - grp->y) + sq(k->z - grp->z) < kGlowMerge * kGlowMerge)
					{
						into = k;
						break;
					}
				if (into == nullptr)
				{
					g.kept.push_back(grp);
					continue;
				}
				const double w = into->w + grp->w;
				into->x = (into->x * into->w + grp->x * grp->w) / w;
				into->y = (into->y * into->w + grp->y * grp->w) / w;
				into->z = (into->z * into->w + grp->z * grp->w) / w;
				into->r = into->r + grp->r;
				into->g = into->g + grp->g;
				into->b = into->b + grp->b;
				into->w = w;
			}
			// (nearest the camera: the view space's origin)
			std::stable_sort(g.kept.begin(), g.kept.end(), [](const group *a, const group *b) {
				return a->x * a->x + a->y * a->y + a->z * a->z < b->x * b->x + b->y * b->y + b->z * b->z;
			});
			// Back into the world: view = world x VIEW (row vectors), VIEW a rotation and a move, so
			// world[j] = sum over i of (view[i] - VIEW[3][i]) x VIEW[j][i]
			double m[16];
			for (int i = 0; i < 16; ++i)
			{
				float f = 0.0f;
				if (!float_of(g_walk.view[i], f))
					return;
				m[i] = f;
			}
			const size_t n = std::min<size_t>(g.kept.size(), kGlowEffectsMax);
			for (size_t k = 0; k < n; ++k)
			{
				const group &grp = *g.kept[k];
				const double dx = grp.x - m[12], dy = grp.y - m[13], dz = grp.z - m[14];
				effect e;
				e.x = dx * m[0] + dy * m[1] + dz * m[2];
				e.y = dx * m[4] + dy * m[5] + dz * m[6];
				e.z = dx * m[8] + dy * m[9] + dz * m[10];
				e.r = grp.r / grp.w;
				e.g = grp.g / grp.w;
				e.b = grp.b / grp.w;
				e.weight = grp.w;
				e.key = grp.key;
				e.name = pointer(grp.gen) ? name_text(grp.name) : std::string("-");
				g.effects.push_back(std::move(e));
			}
		}

		// The effects as glows: colour scaled to its brightest channel, brightness and reach growing with the weight and
		// levelling off (one faint sprite a little, a spell's burst of them about as a lamp: World FX lights with 1 -
		// e^(-brightness / 4) of the colour, so at most 0.39 here, a lit lamp 0.6 or so at LampGlow 1). The addon's were
		// four times as bright and a third further: a white burst lit the ground round a fight white (in game,
		// 2026-10-22).
		constexpr double kEffectBrightness = 2.0, kEffectReach = 2.0, kEffectReachMore = 3.0;
		void put_effects()
		{
			for (const effect &e : g.effects)
			{
				const double top = std::max(std::max(e.r, e.g), std::max(e.b, 1e-6));
				const double s = 1 - std::exp(-e.weight / 4);
				put_glow(e.key, 3, e.r / top, e.g / top, e.b / top, e.x, e.y, e.z, kEffectReach + kEffectReachMore * s, kEffectBrightness * s);
			}
		}
	}

	void find_code()
	{
		if (g.have_fx || g.looked)
			return;
		if (!image_known())
			return; // (FFXiMain.dll isn't there yet: looked for next time)
		g.looked = true;
		const uintptr_t tasks = find(kGlowTasks, 8), callback = find(kGlowTaskCallback, 0), gfx = find(kGlowGfx, 0xD);
		const uintptr_t draw = find(kGlowDraw, -0x17), light = find(kGlowLight, 2);
		if (tasks == 0 || callback == 0 || gfx == 0 || draw == 0 || light == 0)
		{
			g.why = "the game's effect code wasn't found";
			return;
		}
		g.fx = { word(tasks), uint32_t(callback), word(gfx), uint32_t(draw), word(light) };
		g.have_fx = true;
		g.why.clear();
	}

	void stop()
	{
		g = state();
	}

	void zone_changed()
	{
		g.fixtures.clear();
		g.found.clear();
		g.scan_at = 0;
	}

	void frame(const entities &e, double now)
	{
		g.frames++;
		g.now = now;
		scan_fixtures(e);
		if (g.frames % kGlowsEvery != 0)
			return;
		g.n = 0;
		put_fixtures(e, now);
		if (g.have_fx)
		{
			const auto t0 = std::chrono::steady_clock::now();
			read_effects();
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			g.ms = g.ms * 0.95 + ms * 0.05;
			g.ms_most = std::max(g.ms_most, ms);
			g.walks++;
			put_effects();
		}
		else
			g.effects.clear();
		worldfx_glows(g.words, g.n);
		g.sent++;
	}

	std::string status()
	{
		char line[320];
		std::snprintf(line, sizeof(line), "%u glowing fixtures shown", g.fixtures_sent);
		std::string s = line;
		if (!g.have_fx)
			return s + "; effects not read: " + g.why;
		std::snprintf(line, sizeof(line), "; effects: %u lights from %u glowing of %u elements (%u tasks%s), %.2f ms a read (%.2f at most)",
			unsigned(g.effects.size()), g.glowing, g.elements, g.nodes, g.capped ? ", stopped there" : "", g.ms, g.ms_most);
		s += line;
		if (g.steps != 0)
		{
			std::snprintf(line, sizeof(line), "; %u footsteps' elements left out", g.steps);
			s += line;
		}
		if (g.hits != 0)
		{
			std::snprintf(line, sizeof(line), "; %u melee hits' and misses' elements left out", g.hits);
			s += line;
		}
		if (g.abandoned != 0)
		{
			std::snprintf(line, sizeof(line), "; a walk abandoned %u times (memory gone under it)", g.abandoned);
			s += line;
		}
		return s;
	}

	std::vector<std::string> list(const entities &e)
	{
		std::vector<std::string> out;
		char line[320];
		for (const fixture &f : g.fixtures)
		{
			float x = 0.0f, y = 0.0f, z = 0.0f;
			if (!e.position(f.i, x, y, z))
				x = y = z = 0.0f;
			std::snprintf(line, sizeof(line), "%s at %.1f %.1f %.1f", f.name.c_str(), x, z - f.kind->height, y);
			out.push_back(line);
		}
		const size_t n = std::min<size_t>(g.effects.size(), 12);
		for (size_t k = 0; k < n; ++k)
		{
			const effect &ef = g.effects[k];
			std::snprintf(line, sizeof(line), "effect %s weight %.2f, colour %.2f %.2f %.2f at %.1f %.1f %.1f", ef.name.c_str(), ef.weight, ef.r, ef.g, ef.b, ef.x, ef.y, ef.z);
			out.push_back(line);
		}
		// (the sources met lately, the latest first: a hit's spark is gone before /hifi glows can be typed)
		std::vector<const seen *> recent;
		for (const seen &s : g.recent)
			recent.push_back(&s);
		std::sort(recent.begin(), recent.end(), [](const seen *a, const seen *b) { return a->last > b->last; });
		for (const seen *s : recent)
		{
			std::string folders;
			for (uint32_t f : s->folders)
				if (f != 0)
					folders += (folders.empty() ? "" : " < ") + name_text(f);
			const double ago = g.now - s->at;
			std::snprintf(line, sizeof(line), "lately: %s in %s, glowed in %u reads, weight %.2f at most, %.0f s ago", name_text(s->name).c_str(),
				folders.empty() ? "-" : folders.c_str(), s->walks, s->weight, ago);
			out.push_back(line);
		}
		return out;
	}
}
