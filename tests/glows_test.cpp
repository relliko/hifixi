/*
 * Offline test of the glows HiFi XI sends World FX (src/game/glows.cpp, worldfx_glows), as the worldfx addon's
 * t_glows.py tested its Lua: an FFXiMain dump mapped at its own base, with a made-up game state the dump has no copy of
 * (its heap): the task list and the effects' view matrix, and Ashita's entity list. Each dump in a process of its own
 * (the code is looked for once a process, and the two dumps' images overlap).
 *
 *   build\glows_test.exe           both dumps (game_dump.hpp's), each in a child process
 *   build\glows_test.exe <dump>    one
 *
 * - The game's code the glows look for (the task manager's and gfx's globals, the task callback, the element draw, the
 *   light element's vtable: client-notes "Effects that light") is there once in the dump's code, and found. The two
 *   globals are pointed at a made-up task manager and gfx object in memory mapped here.
 * - The task list: a priority marker, a bare function node (no vtable), a task that isn't an element, three glowing
 *   elements of one generator (blue, a Home Point's colour) round a spot, elements that mustn't count (see-through 'D',
 *   hidden, a weather generator's, suspended, faded out, a sky layer, placed by the camera, the light element itself,
 *   footsteps' (in the zone's fses / fefs, a clone of one in fser / fefr, the walker's fvib and fmrk), one 60 yalms
 *   off), and two more glowing generators a yalm apart (an orange fire). Sent: two effect glows (kind 3),
 *   the blue one where its three were in the world (worked back from their view-space positions through the view
 *   matrix), the two orange ones merged, nearest first, each its generator's id; colours scaled to their brightest
 *   channel, brightness and reach from the weight.
 * - A list that loops back on itself: the walk stops at its cap (no hang) and says so.
 * - Memory gone under the walk (a next node, a generator's handle, a generator, a vtable in a page given back): that
 *   walk abandoned, not a crash, the fixtures still sent; the next walk whole again.
 * - No task manager or gfx yet (their globals 0), or a view matrix that isn't numbers: only the fixtures sent.
 * - Twenty glowing elements made by nothing, apart from each other: each its own light, its id the element, the
 *   nearest 16 sent, nearest first.
 * - Fixtures: a Home Point and a Survival Guide among other NPCs in the entity list, found by name within one sweep:
 *   sent as kind 2 at their feet plus their glow's height (the game's axes: Ashita's x, z, y), their colour, pulsing
 *   over time; not shown, not sent; after a zone change, looked for again. With 70 more, 64 glows sent at most.
 * - Every other frame a send; status and list say what's sent; the made-up memory and the dump's image unchanged (read
 *   only).
 */

#include "game_dump.hpp"
#include "../src/game/glows.hpp"
#include <process.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace glows = hifi::game::glows;

namespace
{
	int failures = 0, passes = 0;
	void check(bool ok, const char *what, ...)
	{
		char text[2048];
		va_list args;
		va_start(args, what);
		std::vsnprintf(text, sizeof(text), what, args);
		va_end(args);
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", text);
		if (ok)
			passes++;
		else
			failures++;
	}

	// What went to World FX: each send's glows
	struct glow
	{
		uint32_t id, kind;
		float colour[3], at[3], range, brightness;
	};
	std::vector<std::vector<glow>> g_sends;

	const std::vector<glow> &last_sent()
	{
		static const std::vector<glow> none;
		return g_sends.empty() ? none : g_sends.back();
	}

	std::vector<glow> of_kind(const std::vector<glow> &sent, uint32_t kind)
	{
		std::vector<glow> out;
		for (const glow &x : sent)
			if (x.kind == kind)
				out.push_back(x);
		return out;
	}

	using v3 = std::array<double, 3>;

	double dist(const float a[3], const v3 &b)
	{
		const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	bool close_to(const float a[3], const v3 &b, double within)
	{
		return std::fabs(a[0] - b[0]) < within && std::fabs(a[1] - b[1]) < within && std::fabs(a[2] - b[2]) < within;
	}

	std::string described(const std::vector<glow> &sent)
	{
		std::string s;
		char line[256];
		for (const glow &x : sent)
		{
			std::snprintf(line, sizeof(line), "%skind %u colour %.2f %.2f %.2f at %.2f %.2f %.2f range %.2f brightness %.2f", s.empty() ? "" : "; ", x.kind,
				x.colour[0], x.colour[1], x.colour[2], x.at[0], x.at[1], x.at[2], x.range, x.brightness);
			s += line;
		}
		return s;
	}

	// Ashita's entity list: names, positions (x east, y north, z down), render flags (0x200 shown)
	struct npc
	{
		std::string name;
		float x, y, z;
		uint32_t flags;
	};

	struct fake_entities : hifi::game::entities
	{
		std::map<uint32_t, npc> npcs;
		const char *name(uint32_t i) const override
		{
			const auto it = npcs.find(i);
			return it != npcs.end() ? it->second.name.c_str() : "";
		}
		bool position(uint32_t i, float &x, float &y, float &z) const override
		{
			x = y = z = 0.0f;
			const auto it = npcs.find(i);
			if (it == npcs.end())
				return false;
			x = it->second.x;
			y = it->second.y;
			z = it->second.z;
			return true;
		}
		bool shown(uint32_t i) const override
		{
			const auto it = npcs.find(i);
			return it != npcs.end() && (it->second.flags & 0x200) != 0;
		}
		uint32_t server_id(uint32_t) const override { return 0; }
		uint32_t status(uint32_t) const override { return 0; }
		float model_size(uint32_t) const override { return 1.0f; }
	};

	// The plugin's present as feed.cpp calls the glows (and the addon did): each frame the glows' frame, every half
	// second the code looked for and the zone looked at (a new one: the fixtures looked for again)
	struct game_loop
	{
		double clock = 0.0, next_check = 0.0;
		uint32_t zone = 230, last = 0;
		void present(const fake_entities &e, int n = 1)
		{
			for (int k = 0; k < n; ++k)
			{
				clock += 0.05;
				glows::frame(e, clock);
				if (clock < next_check)
					continue;
				next_check = clock + 0.5;
				glows::find_code();
				if (zone != 0 && zone != last)
				{
					last = zone;
					glows::zone_changed();
				}
			}
		}
	};

	// The effect code's patterns, as glows.cpp has them (and where in each its address is)
	struct pattern
	{
		const char *name, *bytes;
		int offset;
	};
	const pattern kPatterns[] = {
		{ "tasks", "8B44240450518B0D????????E8????????C20400", 8 },
		{ "callback", "568B7424088BCE8B06FF502084C075088B168BCE5EFF6224", 0 },
		{ "gfx", "8B413483E800742F48752F8B0D????????E8????????8B0D????????E8????????8B0D????????6A006A006A006A00E8", 0xD },
		{ "draw", "8D8DF8000000D91C2450E8????????8B00A3????????8A857A01000084C00F85", -0x17 },
		{ "light", "C706????????66C78676010000B401", 2 },
	};

	// Every place a pattern is in [base, base + size)
	std::vector<uintptr_t> find_all(uintptr_t base, size_t size, const char *bytes)
	{
		std::vector<int> want;
		for (size_t i = 0; bytes[i] != '\0' && bytes[i + 1] != '\0'; i += 2)
			want.push_back(bytes[i] == '?' ? -1 : int(std::strtoul(std::string(bytes + i, 2).c_str(), nullptr, 16)));
		std::vector<uintptr_t> out;
		const auto p = reinterpret_cast<const uint8_t *>(base);
		for (size_t at = 0; at + want.size() <= size; ++at)
		{
			size_t k = 0;
			while (k < want.size() && (want[k] < 0 || p[at + k] == want[k]))
				++k;
			if (k == want.size())
				out.push_back(base + at);
		}
		return out;
	}

	template <typename T>
	void poke(uintptr_t at, T value)
	{
		std::memcpy(reinterpret_cast<void *>(at), &value, sizeof(T));
	}

	template <typename T>
	T peek(uintptr_t at)
	{
		T value;
		std::memcpy(&value, reinterpret_cast<const void *>(at), sizeof(T));
		return value;
	}

	constexpr uintptr_t kHeap = 0x30000000;      // (the made-up game objects)
	constexpr size_t kHeapSize = 0x100000, kHeapUsed = 0x30000;
	constexpr uintptr_t kGone = kHeap + 0x80000; // (a page given back: memory gone under the walk)
	constexpr double kPi = 3.141592653589793;

	// The camera: at kEye, looking along look (the game's world: x east, y down, z north); its view matrix as the game's
	// (row vectors: view = world x VIEW; right, up and back as its columns, then the move)
	const v3 kEye = { 100.0, -12.0, 200.0 };

	void view_matrix(double m[16], v3 &look)
	{
		const double rad = kPi / 180.0;
		look = { std::cos(30 * rad) * std::cos(-20 * rad), std::sin(20 * rad), std::sin(30 * rad) * std::cos(-20 * rad) };
		double n = std::sqrt(look[0] * look[0] + look[1] * look[1] + look[2] * look[2]);
		const v3 back = { -look[0] / n, -look[1] / n, -look[2] / n };
		const v3 down = { 0.0, 1.0, 0.0 };
		v3 right = { down[1] * back[2] - down[2] * back[1], down[2] * back[0] - down[0] * back[2], down[0] * back[1] - down[1] * back[0] };
		n = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
		right = { right[0] / n, right[1] / n, right[2] / n };
		const v3 up = { back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2], back[0] * right[1] - back[1] * right[0] };
		for (int i = 0; i < 16; ++i)
			m[i] = 0.0;
		for (int j = 0; j < 3; ++j)
		{
			m[j * 4 + 0] = right[j];
			m[j * 4 + 1] = up[j];
			m[j * 4 + 2] = back[j];
		}
		m[12] = -(right[0] * kEye[0] + right[1] * kEye[1] + right[2] * kEye[2]);
		m[13] = -(up[0] * kEye[0] + up[1] * kEye[1] + up[2] * kEye[2]);
		m[14] = -(back[0] * kEye[0] + back[1] * kEye[1] + back[2] * kEye[2]);
		m[15] = 1.0;
	}

	v3 to_view(const double m[16], const v3 &w)
	{
		v3 out;
		for (int i = 0; i < 3; ++i)
			out[i] = w[0] * m[i] + w[1] * m[4 + i] + w[2] * m[8 + i] + m[12 + i];
		return out;
	}

	// An element's fields (the Python test's keywords): its blend letter, fade, +0x134, flags, hidden, suspended and
	// vtable (0: an element's)
	struct options
	{
		uint16_t letter = 'H';
		float fade = 1.0f, more = 1.0f;
		uint32_t flags = 0;
		uint8_t hidden = 0;
		uint16_t suspended = 0;
		uint32_t vt = 0;
	};

	void put_element(uintptr_t a, uint32_t vt, uint32_t callback, uint32_t handle, uint32_t colour, const options &o, const v3 &view)
	{
		poke<uint32_t>(a, vt);
		poke<uint32_t>(a + 4, callback);
		poke<uint16_t>(a + 0x22, o.suspended);
		poke<uint32_t>(a + 0xF8, colour);
		poke<uint32_t>(a + 0xFC, handle);
		poke<uint32_t>(a + 0x10C, o.flags);
		poke<float>(a + 0x134, o.more);
		poke<float>(a + 0x138, o.fade);
		poke<uint16_t>(a + 0x16C, o.letter);
		poke<uint8_t>(a + 0x17A, o.hidden);
		for (int i = 0; i < 3; ++i)
			poke<float>(a + 0x188 + i * 4, float(view[i]));
	}

	bool has(const std::string &text, const char *what)
	{
		return text.find(what) != std::string::npos;
	}

	// The test on one dump; the failures
	int run(const char *path)
	{
		const char *slash = std::strrchr(path, '\\');
		const std::string file = slash != nullptr ? slash + 1 : path;
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		check(size != 0, "%s mapped at its base (0x%08X)", file.c_str(), unsigned(base));
		if (size == 0)
			return failures;

		// The game's code: once each in the dump's code (its first 0x340000 bytes)
		uintptr_t at[5] = {};
		bool unique = true;
		std::string hits;
		for (int k = 0; k < 5; ++k)
		{
			const std::vector<uintptr_t> h = find_all(base, std::min<size_t>(size, 0x340000), kPatterns[k].bytes);
			unique = unique && h.size() == 1;
			hits += std::string(k != 0 ? ", " : "") + kPatterns[k].name;
			for (uintptr_t x : h)
			{
				char text[16];
				std::snprintf(text, sizeof(text), " %06X", unsigned(x - base));
				hits += text;
			}
			at[k] = h.empty() ? 0 : uintptr_t(intptr_t(h[0]) + kPatterns[k].offset);
		}
		check(unique, "the effect code's patterns are unique in %s (%s)", file.c_str(), hits.c_str());
		if (!unique)
			return failures;
		const auto in_image = [&](uint32_t a, uint32_t n) { return a >= base && a + n <= base + size; };
		const uint32_t tasks_global = peek<uint32_t>(at[0]), gfx_global = peek<uint32_t>(at[2]), light_vt = peek<uint32_t>(at[4]);
		const uint32_t callback = uint32_t(at[1]), draw = uint32_t(at[3]);
		check(in_image(tasks_global, 4) && in_image(gfx_global, 4), "the task manager's and gfx's globals in the image (%08X, %08X)", tasks_global, gfx_global);
		check(in_image(light_vt, 0x30) && peek<uint32_t>(light_vt + 0x2C) == draw,
			"the light element's vtable draws through the element draw too (so the glows have to leave it out by its vtable)");
		if (!in_image(tasks_global, 4) || !in_image(gfx_global, 4))
			return failures;

		// The made-up game's memory, with a page of it given back
		void *heap = VirtualAlloc(reinterpret_cast<void *>(kHeap), kHeapSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		check(heap == reinterpret_cast<void *>(kHeap), "the made-up game's memory mapped at 0x%08X", unsigned(kHeap));
		if (heap != reinterpret_cast<void *>(kHeap))
			return failures;
		VirtualFree(reinterpret_cast<void *>(kGone), 0x1000, MEM_DECOMMIT);

		// The made-up game: the two globals at a task manager and a gfx object; vtables; generators and their handles
		const uintptr_t manager = kHeap + 0x0000, gfx = kHeap + 0x2000;
		const uint32_t tasks_was = peek<uint32_t>(tasks_global), gfx_was = peek<uint32_t>(gfx_global);
		poke<uint32_t>(tasks_global, uint32_t(manager));
		poke<uint32_t>(gfx_global, uint32_t(gfx));
		double m[16];
		v3 look;
		view_matrix(m, look);
		for (int i = 0; i < 16; ++i)
			poke<float>(gfx + 0xB50 + i * 4, float(m[i]));
		const uintptr_t element_vt = kHeap + 0x4000, other_vt = kHeap + 0x4100;
		poke<uint32_t>(element_vt + 0x2C, draw);
		poke<uint32_t>(other_vt + 0x2C, uint32_t(kHeap + 0x4800));
		struct generator
		{
			const char *name;
			uint8_t weather;
		};
		const generator generators[] = { { "hpgl", 0 }, { "rain", 0x83 }, { "fir1", 0 }, { "fir2", 0 }, { "far1", 0 } };
		std::map<std::string, uintptr_t> gens, handles;
		for (int k = 0; k < 5; ++k)
		{
			const std::string name = generators[k].name;
			gens[name] = kHeap + 0x3000 + k * 0x100;
			handles[name] = kHeap + 0x3800 + k * 4;
			std::memcpy(reinterpret_cast<void *>(gens[name] + 0x20), name.data(), 4);
			poke<uint8_t>(gens[name] + 0xDE, generators[k].weather);
			poke<uint32_t>(handles[name], uint32_t(gens[name]));
		}
		const uintptr_t gone_handle = kHeap + 0x3800 + 5 * 4; // (a handle to a generator in the page given back)
		poke<uint32_t>(gone_handle, uint32_t(kGone + 0x200));
		// Folders and footsteps' generators (from kHeap + 0x5000: a resource each 0x100, its handle at + 0xF0): each
		// resource's +0x20 its name, +0x28 its parent's handle. The blue one's generator is in an everyday folder; the
		// zone's footstep effects are in fses / fefs (fser / fefr in rain), started by a clone as often as not; the
		// walker's own fvib and fmrk are in its own data.
		const auto resource = [&](int k, const char *name, uintptr_t parent_handle) {
			const uintptr_t r = kHeap + 0x5000 + k * 0x100;
			std::memcpy(reinterpret_cast<void *>(r + 0x20), name, 4);
			poke<uint32_t>(r + 0x28, uint32_t(parent_handle));
			poke<uint32_t>(r + 0xF0, uint32_t(r));
			gens[name] = r;
			handles[name] = r + 0xF0;
			return r + 0xF0;
		};
		const uintptr_t magic = resource(0, "magi", 0), fses = resource(1, "fses", 0), fser = resource(2, "fser", 0);
		const uintptr_t chr = resource(3, "chr0", 0);
		const uintptr_t fefs = resource(4, "fefs", fses), fefr = resource(5, "fefr", fser);
		resource(6, "0200", fefs);                                  // a step on a path
		const uintptr_t splash = resource(7, "w800", fefr);         // a step in shallow water, in rain
		resource(8, "clon", splash);                                // a clone of it
		resource(9, "fvib", chr);
		resource(10, "fmrk", chr);
		poke<uint32_t>(gens["hpgl"] + 0x28, uint32_t(magic));

		// The task list
		std::vector<uintptr_t> nodes;
		const auto node = [&](uint32_t vt, uint32_t cb) {
			const uintptr_t a = kHeap + 0x10000 + nodes.size() * 0x200;
			nodes.push_back(a);
			poke<uint32_t>(a, vt);
			poke<uint32_t>(a + 4, cb);
			return a;
		};
		const auto element = [&](const char *gen, const v3 &world, uint32_t colour, const options &o) {
			const uintptr_t a = node(0, 0);
			put_element(a, o.vt != 0 ? o.vt : uint32_t(element_vt), callback, uint32_t(handles[gen]), colour, o, to_view(m, world));
			return a;
		};
		const v3 spot = { kEye[0] + 6.0 * look[0], kEye[1] + 1.5, kEye[2] + 6.0 * look[2] }; // (6 yalms ahead, below the eye)
		const v3 fire = { spot[0] + 5.0, spot[1], spot[2] - 1.0 };
		node(0xDEADBEEF, 0);                          // a priority marker (no callback)
		node(0xDEADBEEF, uint32_t(kHeap + 0x4900));   // a bare function node (no vtable)
		node(uint32_t(other_vt), callback);           // a task that isn't an element
		const uint32_t blue = 0x80206080;             // (A R G B: 0x80 alpha, R 0x20 G 0x60 B 0x80)
		const v3 hp[3] = { { spot[0] + 0.3, spot[1] + 0.0, spot[2] + 0.0 }, { spot[0] - 0.3, spot[1] + 0.2, spot[2] + 0.1 },
			{ spot[0] + 0.0, spot[1] - 0.2, spot[2] - 0.1 } };
		uintptr_t hp_nodes[3];
		for (int k = 0; k < 3; ++k)
			hp_nodes[k] = element("hpgl", hp[k], blue, {});
		const v3 off = { spot[0] + 9, spot[1], spot[2] };
		const uintptr_t see_through = element("hpgl", off, blue, { .letter = 'D' }); // see-through, not a glow
		element("hpgl", off, blue, { .hidden = 1 });                                 // hidden
		element("rain", off, 0x80808080, {});                                        // a weather's (rain is additive)
		element("hpgl", off, blue, { .suspended = 1 });                              // suspended
		element("hpgl", off, blue, { .fade = 0.005f });                              // faded out
		element("hpgl", off, blue, { .flags = 0x400000 });                           // a sky layer
		element("hpgl", off, blue, { .flags = 0x4 });                                // placed by the camera
		element("hpgl", off, blue, { .vt = light_vt });                              // the light element itself
		for (const char *step : { "0200", "clon", "fvib", "fmrk" })                  // footsteps' (additive, white)
			element(step, off, 0x80808080, {});
		element("far1", { kEye[0] + 60, kEye[1], kEye[2] }, blue, {});               // 60 yalms off
		element("fir1", fire, 0x40805010, { .letter = 'h', .fade = 0.5f });          // weight 1 x 0.5 x 0.5 = 0.25
		const v3 fire2 = { fire[0] + 0.6, fire[1] - 0.3, fire[2] + 0.5 };
		element("fir2", fire2, 0x80804008, { .letter = 'I', .more = 2.0f });         // weight 2: the two merged
		for (size_t k = 0; k < nodes.size(); ++k)
			poke<uint32_t>(nodes[k] + 0xC, k + 1 < nodes.size() ? uint32_t(nodes[k + 1]) : 0u);
		poke<uint32_t>(manager + 0xD50, uint32_t(nodes[0]));

		// A second list: twenty glowing elements made by nothing (no generator: each its own light), 2.5 yalms or more
		// apart, listed out of their order by distance (many[d]: the d-th nearest), placed in view space
		uintptr_t many[20];
		v3 many_view[20];
		for (int k = 0; k < 20; ++k)
		{
			const int d = k % 2 == 0 ? 19 - k / 2 : k / 2;
			const uintptr_t a = kHeap + 0x20000 + k * 0x200;
			many[d] = a;
			many_view[d] = { d % 2 == 0 ? -1.5 : 1.5, 0.0, -(3.0 + 1.25 * d) };
			put_element(a, uint32_t(element_vt), callback, 0, blue, {}, many_view[d]);
			poke<uint32_t>(a + 0xC, k + 1 < 20 ? uint32_t(kHeap + 0x20000 + (k + 1) * 0x200) : 0u);
		}
		const std::vector<uint8_t> heap_before(reinterpret_cast<const uint8_t *>(kHeap), reinterpret_cast<const uint8_t *>(kHeap) + kHeapUsed);

		// Ashita's entity list
		fake_entities ent;
		ent.npcs[5] = { "Home Point #1", 10, 20, -3, 0x200 };
		ent.npcs[7] = { "Moogle", 11, 21, -3, 0x200 };
		ent.npcs[900] = { "Survival Guide", -4, 6, 1, 0x200 };
		ent.npcs[1500] = { "Home Point #9", 1, 2, 0, 0x200 }; // (not a zone NPC's index)

		game_loop game;
		check(has(glows::status(), "effects not read: not looked for yet"), "before the first present, the effect code not looked for yet (%s)", glows::status().c_str());
		game.present(ent, 1);
		check(has(glows::status(), "; effects: "), "the effect code found in %s (%s)", file.c_str(), glows::status().c_str());

		// A sweep of the entity list (16 frames of 64) and a few more
		const size_t before = g_sends.size();
		game.present(ent, 20);
		check(g_sends.size() - before == 10, "glows sent every other frame (%u sends in 20 frames)", unsigned(g_sends.size() - before));
		std::printf("  sent: %u glows: %s\n", unsigned(last_sent().size()), described(last_sent()).c_str());

		// Fixtures
		const auto home_of = [](const std::vector<glow> &sent) {
			std::vector<glow> out;
			for (const glow &x : of_kind(sent, 2))
				if (std::fabs(x.at[0] - 10.0f) < 1e-4f)
					out.push_back(x);
			return out;
		};
		{
			const std::vector<glow> fixtures = of_kind(last_sent(), 2);
			const std::vector<glow> home = home_of(last_sent());
			std::vector<glow> guide;
			for (const glow &x : fixtures)
				if (std::fabs(x.at[0] + 4.0f) < 1e-4f)
					guide.push_back(x);
			check(fixtures.size() == 2 && home.size() == 1 && guide.size() == 1,
				"the Home Point and the Survival Guide found by name (not the Moogle, nor a name at an index past the zone's NPCs)");
			if (home.size() == 1)
			{
				const glow &h = home[0];
				check(close_to(h.at, { 10.0, -3.0 - 1.6, 20.0 }, 1e-4), "  the Home Point's glow 1.6 yalms over its feet, in the game's axes (Ashita's x, z, y): %.4f %.4f %.4f",
					h.at[0], h.at[1], h.at[2]);
				check(close_to(h.colour, { 0.35, 0.75, 1.0 }, 1e-4) && std::fabs(h.range - 7.0f) < 1e-4f, "  blue-cyan, reaching 7 yalms");
				check(h.brightness >= 4.5f - 1e-4f && h.brightness <= 7.5f + 1e-4f, "  its brightness within its pulse (6, +-25%%): %.2f", h.brightness);
				check(h.id == 0x46000000 + 5, "  its id 0x46000000 + its entity (%08X)", h.id);
				std::set<long> seen;
				for (int k = 0; k < 20; ++k)
				{
					game.present(ent, 2);
					const std::vector<glow> now = home_of(last_sent());
					if (!now.empty())
						seen.insert(std::lround(now[0].brightness * 1000.0));
				}
				const double lo = seen.empty() ? 0.0 : *seen.begin() / 1000.0, hi = seen.empty() ? 0.0 : *seen.rbegin() / 1000.0;
				check(seen.size() > 10 && hi - lo > 1.0, "  and pulsing over time (%.2f to %.2f)", lo, hi);
			}
			if (guide.size() == 1)
				check(close_to(guide[0].at, { -4.0, 1.0 - 1.2, 6.0 }, 1e-4) && close_to(guide[0].colour, { 1.0, 0.85, 0.55 }, 1e-4) && std::fabs(guide[0].range - 5.0f) < 1e-4f &&
					guide[0].id == 0x46000000 + 900, "  the Survival Guide's 1.2 yalms over its feet, warm white, reaching 5, its id 0x46000000 + 900");
		}
		ent.npcs[5].flags = 0;
		game.present(ent, 2);
		check(home_of(last_sent()).empty(), "a fixture not shown: no glow");
		ent.npcs[5].flags = 0x200;
		game.zone = 231;
		game.present(ent, 12); // (the zone looked at twice a second: by then it's changed and the sweep started over)
		const size_t after_zone = of_kind(last_sent(), 2).size();
		game.present(ent, 40);
		const size_t again = of_kind(last_sent(), 2).size();
		check(after_zone == 0 && again == 2, "after a zone change the fixtures are looked for again and found (%u mid-sweep, %u after)", unsigned(after_zone), unsigned(again));

		// Effects
		{
			const std::vector<glow> effects = of_kind(last_sent(), 3);
			check(effects.size() == 2, "two effect glows: the blue generator's three and the two fires merged; none of the others (%u)", unsigned(effects.size()));
			if (effects.size() == 2)
			{
				const v3 mean = { (hp[0][0] + hp[1][0] + hp[2][0]) / 3, (hp[0][1] + hp[1][1] + hp[2][1]) / 3, (hp[0][2] + hp[1][2] + hp[2][2]) / 3 };
				const glow &e0 = effects[0], &e1 = effects[1];
				const double d0 = dist(e0.at, kEye), d1 = dist(e1.at, kEye);
				check(d0 <= d1, "  nearest first (%.1f, %.1f yalms off)", d0, d1);
				check(dist(e0.at, mean) < 1e-3, "  the blue one where its elements are in the world (worked back from view space): %.3f %.3f %.3f vs %.3f %.3f %.3f",
					e0.at[0], e0.at[1], e0.at[2], mean[0], mean[1], mean[2]);
				check(close_to(e0.colour, { 0.25, 0.75, 1.0 }, 1e-4), "  its colour scaled to its brightest channel (0.25 0.75 1): %.4f %.4f %.4f", e0.colour[0], e0.colour[1], e0.colour[2]);
				const double s = 1 - std::exp(-3.0 / 4);
				check(std::fabs(e0.range - (2 + 3 * s)) < 1e-4 && std::fabs(e0.brightness - 2 * s) < 1e-4, "  reach and brightness from its weight (3), a lamp's at most: %.2f, %.2f",
					e0.range, e0.brightness);
				check(e0.id == gens["hpgl"] && e1.id == gens["fir2"], "  each its generator's id (the heavier fire's for the two merged): %08X, %08X", e0.id, e1.id);
				const double w1 = 0.25, w2 = 2.0;
				const v3 merged = { (fire[0] * w1 + fire2[0] * w2) / (w1 + w2), (fire[1] * w1 + fire2[1] * w2) / (w1 + w2), (fire[2] * w1 + fire2[2] * w2) / (w1 + w2) };
				check(dist(e1.at, merged) < 1e-3, "  the fires merged at their weighted middle: %.3f %.3f %.3f vs %.3f %.3f %.3f", e1.at[0], e1.at[1], e1.at[2], merged[0], merged[1], merged[2]);
				const double r = 1.0 * w1 + 1.0 * w2, gg = 0.625 * w1 + 0.5 * w2, b = 0.125 * w1 + 0.0625 * w2;
				check(close_to(e1.colour, { r / r, gg / r, b / r }, 1e-4), "  orange (%.3f %.3f %.3f)", e1.colour[0], e1.colour[1], e1.colour[2]);
			}
		}
		{
			std::string text = glows::status();
			bool effect_line = false, home_line = false;
			for (const std::string &line : glows::list(ent))
			{
				text += " | " + line;
				effect_line = effect_line || has(line, "effect hpgl weight 3.00");
				home_line = home_line || has(line, "Home Point #1 at 10.0 -4.6 20.0");
			}
			check(has(glows::status(), "2 glowing fixtures shown; effects: 2 lights from 5 glowing of 17 elements (21 tasks)") &&
				has(glows::status(), "; 4 footsteps' elements left out") && effect_line && home_line, "status and list say so: %s", text.c_str());
		}

		// A list that loops: the walk stops at its cap
		poke<uint32_t>(nodes.back() + 0xC, uint32_t(nodes[0]));
		game.present(ent, 2);
		check(has(glows::status(), "stopped there") && has(glows::status(), "8192 tasks"), "a list looping on itself: the walk stops at 8192 nodes and says so (%s)",
			glows::status().c_str());
		poke<uint32_t>(nodes.back() + 0xC, 0);

		// Memory gone under the walk: that walk abandoned, not a crash
		struct gone_case
		{
			const char *what;
			uintptr_t at;
			uint32_t value;
		};
		const gone_case gone[] = {
			{ "a next node", nodes[5] + 0xC, uint32_t(kGone) },
			{ "a glowing element's generator handle", hp_nodes[1] + 0xFC, uint32_t(kGone + 0x40) },
			{ "a glowing element's generator", hp_nodes[1] + 0xFC, uint32_t(gone_handle) },
			{ "an element's vtable", see_through, uint32_t(kGone + 0x100) },
		};
		int abandoned = 0;
		for (const gone_case &c : gone)
		{
			const uint32_t was = peek<uint32_t>(c.at);
			poke<uint32_t>(c.at, c.value);
			game.present(ent, 2);
			abandoned++;
			char times[64];
			std::snprintf(times, sizeof(times), "a walk abandoned %d times", abandoned);
			check(of_kind(last_sent(), 3).empty() && of_kind(last_sent(), 2).size() == 2 && has(glows::status(), times),
				"memory gone under the walk (%s in a page given back): that walk abandoned, no crash, the fixtures still sent (%u glows)", c.what, unsigned(last_sent().size()));
			poke<uint32_t>(c.at, was);
		}
		game.present(ent, 2);
		check(of_kind(last_sent(), 3).size() == 2 && has(glows::status(), "2 lights from 5 glowing of 17 elements (21 tasks)"), "  and the next walk whole again (%s)",
			glows::status().c_str());

		// No task manager or gfx yet (their globals 0), a view matrix that isn't numbers: only the fixtures
		poke<uint32_t>(tasks_global, 0);
		game.present(ent, 2);
		check(last_sent().size() == 2 && of_kind(last_sent(), 2).size() == 2, "with no task manager (0) only the fixtures are sent (%u)", unsigned(last_sent().size()));
		poke<uint32_t>(tasks_global, uint32_t(manager));
		poke<uint32_t>(gfx_global, 0);
		game.present(ent, 2);
		check(last_sent().size() == 2 && of_kind(last_sent(), 2).size() == 2, "with no gfx (0) only the fixtures are sent (%u)", unsigned(last_sent().size()));
		poke<uint32_t>(gfx_global, uint32_t(gfx));
		{
			const uint32_t was = peek<uint32_t>(gfx + 0xB50 + 4 * 5);
			poke<uint32_t>(gfx + 0xB50 + 4 * 5, 0x7FC00000);
			game.present(ent, 2);
			check(last_sent().size() == 2 && of_kind(last_sent(), 2).size() == 2 && has(glows::status(), "0 lights from 5 glowing of 17 elements (21 tasks)"),
				"a view matrix that isn't numbers (a NaN): the walk counted, no effects sent (%s)", glows::status().c_str());
			poke<uint32_t>(gfx + 0xB50 + 4 * 5, was);
		}

		// More effects than are sent: the nearest 16
		poke<uint32_t>(manager + 0xD50, uint32_t(kHeap + 0x20000));
		game.present(ent, 2);
		{
			const std::vector<glow> effects = of_kind(last_sent(), 3);
			bool ids = effects.size() == 16, where = effects.size() == 16, how = effects.size() == 16;
			const double s = 1 - std::exp(-1.0 / 4);
			for (size_t k = 0; k < effects.size() && k < 16; ++k)
			{
				ids = ids && effects[k].id == many[k];
				const v3 v = to_view(m, { effects[k].at[0], effects[k].at[1], effects[k].at[2] });
				where = where && std::fabs(v[0] - many_view[k][0]) < 1e-3 && std::fabs(v[1] - many_view[k][1]) < 1e-3 && std::fabs(v[2] - many_view[k][2]) < 1e-3;
				how = how && std::fabs(effects[k].brightness - 2 * s) < 1e-4 && std::fabs(effects[k].range - (2 + 3 * s)) < 1e-4;
			}
			check(ids, "twenty lights made by nothing, apart: the nearest 16 sent, nearest first, each its id the element (%u sent)", unsigned(effects.size()));
			check(where && how, "  each where it is in the world, none merged (each weight 1)");
			check(has(glows::status(), "16 lights from 20 glowing of 20 elements (20 tasks)"), "  status: %s", glows::status().c_str());
		}
		poke<uint32_t>(manager + 0xD50, uint32_t(nodes[0]));

		// At most 64 glows: 70 more Home Points, the fixtures first
		for (uint32_t i = 100; i < 170; ++i)
			ent.npcs[i] = { "Home Point #" + std::to_string(i), float(i), 5, -1, 0x200 };
		game.present(ent, 40);
		check(last_sent().size() == 64 && of_kind(last_sent(), 2).size() == 64 && has(glows::status(), "72 glowing fixtures shown"),
			"72 fixtures shown: 64 glows sent at most, the fixtures first (%u sent; %s)", unsigned(last_sent().size()), glows::status().c_str());
		for (uint32_t i = 100; i < 170; ++i)
			ent.npcs.erase(i);
		game.present(ent, 40);
		check(last_sent().size() == 4 && of_kind(last_sent(), 2).size() == 2, "  and back to 2 fixtures and 2 effects once they're gone (%u)", unsigned(last_sent().size()));

		// Read only
		poke<uint32_t>(tasks_global, tasks_was); // (the dump's own values back)
		poke<uint32_t>(gfx_global, gfx_was);
		const bool heap_same = std::memcmp(heap_before.data(), reinterpret_cast<const void *>(kHeap), kHeapUsed) == 0;
		check(heap_same && dump::same_as_file(path, base, size), "the made-up game and the dump's image as they were: the glows only read them");
		VirtualFree(heap, 0, MEM_RELEASE);
		dump::unmap(base);
		return failures;
	}
}

// World FX's functions the code calls: the glows recorded, no settings, no folder
extern "C" void __cdecl worldfx_glows(const uint32_t *words, uint32_t count)
{
	std::vector<glow> sent;
	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t *w = words + i * 10;
		glow x;
		x.id = w[0];
		x.kind = w[1];
		std::memcpy(x.colour, w + 2, 12);
		std::memcpy(x.at, w + 5, 12);
		std::memcpy(&x.range, w + 8, 4);
		std::memcpy(&x.brightness, w + 9, 4);
		sent.push_back(x);
	}
	g_sends.push_back(sent);
}

extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t)
{
	return 0;
}

// (HiFi XI's own options: none set here, each its default)
extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}

extern "C" const char *__cdecl worldfx_folder()
{
	return "";
}

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc > 1)
	{
		run(argv[1]);
		std::printf("%d ok, %d failed on %s\n", passes, failures, argv[1]);
		return failures;
	}
	// Each dump in a process of its own
	char self[MAX_PATH] = "";
	GetModuleFileNameA(nullptr, self, MAX_PATH);
	const std::string quoted_self = std::string("\"") + self + "\"";
	int failed = 0;
	for (const char *path : dump::kDumps)
	{
		std::printf("%s\n", path);
		const std::string quoted_path = std::string("\"") + path + "\"";
		const intptr_t code = _spawnl(_P_WAIT, self, quoted_self.c_str(), quoted_path.c_str(), nullptr);
		if (code < 0)
		{
			std::printf("FAIL  the test on %s couldn't be started\n", path);
			failed++;
		}
		else
			failed += int(code);
		std::printf("\n");
	}
	std::printf(failed == 0 ? "all passed\n" : "%d failed\n", failed);
	return failed;
}
