/*
 * drawcull (drawcull.hpp): the Ashita addon drawcull 1.5 in C++ (its load, apply and unload; its /drawcull command is
 * HiFi XI's [drawcull] options now, followed every half second).
 *
 * How it works (the addon's notes):
 *   World and entity draw distance: the client multiplies its far clip, fog and entity cull distance by two floats
 *   (world and entity multipliers, 1.0 stock). These are the same floats the drawdistance addon writes; they're set
 *   here from the options and put back to 1.0 at stop.
 *
 *   Zone object culling: objects that carry their own view range (buildings, trees, props) are culled with
 *     distance^2 > scale * range^2
 *   where scale is the world multiplier (times the client's own distance setting). The multiplier is applied to a
 *   squared distance, so a world multiplier of 10 only pushes those objects out about 3.2x (sqrt(10)) while the far
 *   clip and fog move 10x, and they pop in well inside the visible range. The scale is computed once per frame by a
 *   call that stores it to [renderer+0x39428]; that call is pointed at a small stub that multiplies the result by the
 *   world multiplier once more, so object ranges grow by the multiplier itself. At a multiplier of 1 nothing changes.
 *
 *   Zone object detail: each object has three meshes and picks one by comparing its squared distance against two fixed
 *   squared thresholds ([obj+0xCC], [obj+0xC8]). Nothing scales those, so trees and props swap to a coarser mesh at the
 *   same distance whatever the draw distance is. Each "fld [dist2]; fcomp [obj+thr]" pair is replaced by a call to a
 *   stub that multiplies the distance by 1 / lod^2 before comparing, which moves both switch points out by the lod
 *   multiplier.
 *
 *   Zone visibility lists: zones carry precomputed lists of which objects can be seen from each area. When the camera's
 *   area has one, the renderer draws only those objects, so terrain that the zone's author didn't expect to be visible
 *   is never drawn however far the draw distance goes, and it pops in when the camera moves into the next area. With
 *   vis on, the read of the current list ([renderer+0x3912C]) is replaced by 0, and the renderer falls back to its
 *   quadtree walk, which culls by frustum and distance only.
 *
 *   Square pixels: the client builds every world projection with aspect = 1 / (a * k), where a = [gGame+0x2F0] =
 *   height / (0.75 * width) (aspect writes it from the window size) and k = [gGame+0x2E8] is a constant 0.791. So the
 *   aspect is 0.948 x the window's, and the 3D picture is about 5.5% wider than it should be: people look broad, round
 *   things oval. With square on, k is 0.75, the aspect is exactly the window's, and the view shows a little more at the
 *   sides. Only the four projection builders read k (camera, decals, effects), so the whole world changes together; the
 *   UI doesn't. Traced in client-notes.md, "Projection and depth, complete".
 *
 *   Frustum: zone objects are culled against a frustum of their own, built each frame from the camera's zoom F as
 *   y = 2F / H. The camera's projection is y = F / 192 (H = 384), but the culling frustum takes H from the background
 *   resolution (renderer+0x355B4, set once at startup): 2880 at 5120x2880, a vertical half-angle of about 80 degrees
 *   against the camera's 38 at the widest zoom. So objects above and below the view are never culled and get drawn
 *   anyway. With frustum on, the per-frame build gets 384.0 instead ("mov eax, [ebp+0x355B4]" -> "mov eax, 384.0; nop");
 *   the field itself and its other reader (the second zone path's screen scale) are left alone. The box test is
 *   conservative (culled only when all 8 corners are outside one plane), so nothing that reaches the screen is cut.
 *
 * Addresses and the traced code are in research/ffxi-re/client-notes.md ("Draw distance and culling").
 *
 * Unlike the addon: each patch goes in only over the game's own bytes and comes out only over ours (the addon flipped
 * the visibility read and the frustum's bytes whoever had put them there); the culling call is taken only while it
 * goes to the client's distance scale (the function the multipliers are read in), not someone's stub; the multipliers
 * and the square-pixel constant are put back only while they're still what we wrote; the stubs' memory is freed at
 * stop once every call into it is out (stop runs on the game's thread, between frames); the square-pixel check runs at
 * each update (twice a second) instead of every 60th present; a multiplier of 0 or less in the options is taken as 1,
 * a detail below 0 as 0 (the command refused them).
 */

#include "drawcull.hpp"
#include <windows.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hifi::game::drawcull
{
	namespace
	{
		// fld [cfg]; ...; fmul [world] / fmul [entity] inside the client's distance scale function: the world
		// multiplier's address at +7, the entity's at +0xF
		constexpr const char *kDistance = "8BC1487408D80D";
		// mov ecx, [cam]; fstp st(0); call <scale>; fstp dword [ebp+0x39428]: the call at +8
		constexpr const char *kCulling = "8B0D????????DDD8E8????????D99D28940300";
		constexpr intptr_t kCullingAt = 8;
		// (the call's own target is the distance scale: the multipliers are read this far into it at most)
		constexpr uintptr_t kScaleReach = 0x400;
		// Mesh detail compares: fld dword [esp+d]; fcomp dword [reg+0xCC or 0xC8]
		constexpr const char *kLod[4] = { "D9442414D899CC000000", "D9442414D899C8000000", "D9442458D89FCC000000", "D9442458D89FC8000000" };
		// mov eax, [ebp+0x3912C]; lea ecx, [ebp+0x98] (the zone's visibility list read), and the same made 0
		constexpr const char *kVis = "8B852C9103008D8D98000000";
		constexpr const char *kVisZeroed = "31C0909090908D8D98000000";
		constexpr uint8_t kVisStock[6] = { 0x8B, 0x85, 0x2C, 0x91, 0x03, 0x00 };
		constexpr uint8_t kVisZero[6] = { 0x31, 0xC0, 0x90, 0x90, 0x90, 0x90 }; // xor eax, eax
		// mov eax, [gGame]; add eax, 0x94; ret (the view matrix getter): gGame's global at +1
		constexpr const char *kGame = "A1????????0594000000C3";
		// fdiv dword [eax+0x2F0]; mov eax, [ebp+0x355B4]; push ecx; push 0.2; push ecx; lea esi, [ebp+0x38E5C] (the
		// per-frame culling frustum's arguments; the mov at +6), and the same with 384.0 in it
		constexpr const char *kFrustum = "D8B0F00200008B85B45503005168CDCC4C3E518DB55C8E0300";
		constexpr const char *kFrustumFixed = "D8B0F0020000B80000C043905168CDCC4C3E518DB55C8E0300";
		constexpr intptr_t kFrustumAt = 6;
		constexpr uint8_t kFrustumStock[6] = { 0x8B, 0x85, 0xB4, 0x55, 0x03, 0x00 };
		constexpr uint8_t kFrustumHeight[6] = { 0xB8, 0x00, 0x00, 0xC0, 0x43, 0x90 }; // mov eax, 384.0; nop
		// [gGame+0x2E8], as bits (game floats are read and written as their bits here)
		constexpr uint32_t kAspectK = 0x2E8;
		constexpr uint32_t kAspectStock = 0x3F4A7EFA;  // 0.791
		constexpr uint32_t kAspectSquare = 0x3F400000; // 0.75
		constexpr uint32_t kOne = 0x3F800000;
		// Our memory: +0 the culling stub, +32 the mesh detail factor, +48 the mesh detail stubs (24 bytes each)
		constexpr uint32_t kMemSize = 160, kFactor = 32, kLodStubs = 48, kLodStub = 24;

		// The options, as the addon's settings were
		struct options
		{
			double world = 1.0, entity = 1.0, lod = 0.0; // (lod 0: the same as world)
			bool vis = true, square = false, frustum = false;
		};

		// A call of ours in the game's code (the culling call, a mesh detail compare): where, the game's bytes and ours,
		// whether ours are in, why not
		struct site
		{
			uintptr_t at = 0;
			uint8_t stock[10] = {}, ours[10] = {};
			size_t size = 0;
			bool in = false;
			std::string why;
		};

		// A patch the options put in and take out (the visibility read, the frustum's height): where, the game's bytes
		// and ours, whether ours there were put there by us, why it isn't as asked
		struct toggle
		{
			uintptr_t at = 0;
			const uint8_t *stock = nullptr, *ours = nullptr;
			size_t size = 0;
			bool mine = false;
			std::string why;
		};

		struct state
		{
			bool looked = false;
			std::string why = "not started yet";      // why nothing is done (empty: running)
			uintptr_t world = 0, entity = 0;          // the multipliers
			uintptr_t mem = 0, scale = 0;             // our memory; the distance scale the culling call goes to
			site culling, lod[4];
			toggle vis, frustum;
			uintptr_t game = 0;                       // gGame's global
			std::string game_why;
			uint32_t square_game = 0;                 // the gGame we put 0.75 in (0: none)
			std::string square_why;
			bool world_set = false, entity_set = false;
			uint32_t world_bits = 0, entity_bits = 0; // what we last wrote
			options o;
		};
		state g;
		std::string g_left; // what the last stop left as it was (said until the next stop)

		// A word of the game's data written (WriteProcessMemory on this process: false when it can't be)
		bool put_word(uintptr_t at, uint32_t v)
		{
			SIZE_T n = 0;
			return at >= 0x10000 && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(at), &v, sizeof(v), &n) != 0 && n == sizeof(v);
		}

		uint32_t bits(double v)
		{
			return std::bit_cast<uint32_t>(float(v));
		}

		// A number option as the addon's settings held it: a double, read from its text
		double number(const char *key, double fallback)
		{
			const std::string text = option_text("drawcull", key, "");
			if (text.empty())
				return fallback;
			char *end = nullptr;
			const double v = std::strtod(text.c_str(), &end);
			return end != text.c_str() && std::isfinite(v) ? v : fallback;
		}

		options read_options()
		{
			options o;
			o.world = number("world", 1.0);
			o.entity = number("entity", 1.0);
			o.lod = number("lod", 0.0);
			// (as the addon's command took them: multipliers above 0, a detail of 0 or more)
			if (!(o.world > 0))
				o.world = 1.0;
			if (!(o.entity > 0))
				o.entity = 1.0;
			if (!(o.lod >= 0))
				o.lod = 0.0;
			o.vis = option_on("drawcull", "vis", true);
			o.square = option_on("drawcull", "square", false);
			o.frustum = option_on("drawcull", "frustum", false);
			return o;
		}

		// How much further the detailed meshes are kept: lod, or the world multiplier when it's 0; never below 1
		double lod_value(const options &o)
		{
			const double l = o.lod > 0 ? o.lod : o.world;
			return std::max(l, 1.0);
		}

		// The culling call, taken only while it goes to the distance scale (the function at `distance`'s multiplies)
		void find_culling(uintptr_t distance)
		{
			const uintptr_t m = find(kCulling);
			if (m == 0)
			{
				g.culling.why = "the object culling code wasn't found: zone objects still pop in early";
				return;
			}
			const uintptr_t at = m + kCullingAt;
			const uint32_t target = rel_target(at);
			if (!(target < distance && distance - target < kScaleReach) || !read(at, g.culling.stock, 5))
			{
				g.culling.why = "the object culling call goes elsewhere already (another addon's stub, the drawcull addon's?): left alone";
				return;
			}
			g.culling.at = at;
			g.culling.size = 5;
			g.scale = target;
		}

		// The mesh detail compares, each where it's still the game's own
		void find_lod()
		{
			for (int i = 0; i < 4; ++i)
			{
				site &s = g.lod[i];
				const uintptr_t at = find(kLod[i]);
				if (at == 0 || !read(at, s.stock, 10))
				{
					s.why = "not found (changed by something else: the drawcull addon's?)";
					continue;
				}
				s.at = at;
				s.size = 10;
			}
		}

		// A toggle's site, found as the game's own or as ours (someone else's then: left alone while it is)
		void find_toggle(toggle &t, const char *stock_pattern, const char *ours_pattern, intptr_t offset, const uint8_t *stock, const uint8_t *ours, const char *what)
		{
			t.stock = stock;
			t.ours = ours;
			t.size = 6;
			uintptr_t at = find(stock_pattern, offset);
			if (at == 0)
				at = find(ours_pattern, offset);
			if (at == 0)
			{
				t.why = std::string(what) + " wasn't found";
				return;
			}
			t.at = at;
		}

		void find_game()
		{
			const uintptr_t p = find(kGame);
			const uint32_t global = p != 0 ? word(p + 1) : 0;
			if (!pointer(global))
			{
				g.game_why = "the camera object wasn't found: square is unavailable";
				return;
			}
			g.game = global;
		}

		// Ours in, only over the game's own bytes
		void put_in(site &s)
		{
			if (!code_is(s.at, s.stock, s.size))
				s.why = "changed by something else: left alone";
			else if (!write_code(s.at, s.ours, s.size))
				s.why = "couldn't write the game's code";
			else
			{
				s.in = true;
				s.why.clear();
			}
		}

		// Our memory, the stubs in it, and the calls to them put in
		void patch_calls()
		{
			bool any = g.culling.at != 0;
			for (const site &s : g.lod)
				any = any || s.at != 0;
			if (!any)
				return;
			const uintptr_t mem = alloc_code(kMemSize);
			if (mem == 0)
			{
				const char *why = "couldn't get memory for its stubs: only the draw distance values are applied";
				g.culling.why = why;
				for (site &s : g.lod)
					s.why = why;
				return;
			}
			g.mem = mem;
			uint8_t *m = reinterpret_cast<uint8_t *>(mem);
			put32(m + kFactor, kOne);
			if (g.culling.at != 0)
			{
				// push dword [esp+4]; call <scale>; fmul dword [world]; ret 4
				const uint8_t stub[18] = { 0xFF, 0x74, 0x24, 0x04, 0xE8, 0, 0, 0, 0, 0xD8, 0x0D, 0, 0, 0, 0, 0xC2, 0x04, 0x00 };
				std::memcpy(m, stub, sizeof(stub));
				put32(m + 5, uint32_t(g.scale - (mem + 9)));
				put32(m + 11, uint32_t(g.world));
				g.culling.ours[0] = 0xE8;
				put32(g.culling.ours + 1, uint32_t(mem - (g.culling.at + 5)));
				put_in(g.culling);
			}
			for (uint32_t i = 0; i < 4; ++i)
			{
				site &s = g.lod[i];
				if (s.at == 0)
					continue;
				// fld dword [esp+d+4]; fmul dword [factor]; fcomp dword [reg+thr]; ret (+4: the call pushed a return
				// address)
				const uintptr_t stub = mem + kLodStubs + i * kLodStub;
				uint8_t *p = reinterpret_cast<uint8_t *>(stub);
				p[0] = 0xD9;
				p[1] = 0x44;
				p[2] = 0x24;
				p[3] = uint8_t(s.stock[3] + 4);
				p[4] = 0xD8;
				p[5] = 0x0D;
				put32(p + 6, uint32_t(mem + kFactor));
				std::memcpy(p + 10, s.stock + 4, 6);
				p[16] = 0xC3;
				s.ours[0] = 0xE8;
				put32(s.ours + 1, uint32_t(stub - (s.at + 5)));
				std::memset(s.ours + 5, 0x90, 5);
				put_in(s);
			}
		}

		// The game's code looked for (once FFXiMain.dll is there), our stubs put in
		void start()
		{
			if (g.looked)
				return;
			if (!image_known())
			{
				g.why = "FFXiMain.dll isn't there yet";
				return;
			}
			g.looked = true;
			const uintptr_t p = find(kDistance);
			const uint32_t world = p != 0 ? word(p + 0x07) : 0, entity = p != 0 ? word(p + 0x0F) : 0;
			if (!pointer(world) || !pointer(entity))
			{
				g.why = "the draw distance values weren't found: nothing done";
				return;
			}
			g.world = world;
			g.entity = entity;
			g.why.clear();
			find_culling(p);
			find_lod();
			find_toggle(g.vis, kVis, kVisZeroed, 0, kVisStock, kVisZero, "the zone visibility list's read");
			find_toggle(g.frustum, kFrustum, kFrustumFixed, kFrustumAt, kFrustumStock, kFrustumHeight, "the culling frustum's setup");
			find_game();
			patch_calls();
		}

		// A toggle as asked: ours in only over the game's bytes, the game's back only over ours
		void set_toggle(toggle &t, bool on)
		{
			if (t.at == 0)
				return;
			const bool stock = code_is(t.at, t.stock, t.size), ours = code_is(t.at, t.ours, t.size);
			if (t.mine && !ours)
				t.mine = false; // (changed since by something else)
			t.why.clear();
			if (on)
			{
				if (stock)
				{
					if (write_code(t.at, t.ours, t.size))
						t.mine = true;
					else
						t.why = "couldn't write the game's code";
				}
				else if (!ours)
					t.why = "the game's code there is changed by something else: left alone";
				else if (!t.mine)
					t.why = "on through something else's patch (the drawcull addon's?), left as it is";
			}
			else if (ours)
			{
				if (!t.mine)
					t.why = "something else's patch is in (the drawcull addon's?): left alone";
				else if (write_code(t.at, t.stock, t.size))
					t.mine = false;
				else
					t.why = "couldn't write the game's code";
			}
			else if (!stock)
				t.why = "the game's code there is changed by something else: left alone";
		}

		// [gGame+0x2E8] set to 0.75 (on) or the game's 0.791. Only a value that is one of the two is changed, 0.75 put
		// back only where we put it, so anything else that set it is left alone. gGame may not exist yet (or be a new
		// one: the game sets 0.791 in each); the next update tries again.
		void apply_square(bool on)
		{
			g.square_why.clear();
			if (g.game == 0)
				return;
			const uint32_t game = word(g.game);
			uint32_t now = 0;
			if (!pointer(game) || !read(game + kAspectK, now))
			{
				if (on)
					g.square_why = "waiting for the camera object";
				return;
			}
			if (g.square_game != 0 && (g.square_game != game || now != kAspectSquare))
				g.square_game = 0; // (a new camera object, or changed since: not ours any more)
			if (on)
			{
				if (now == kAspectStock)
				{
					if (put_word(game + kAspectK, kAspectSquare))
						g.square_game = game;
				}
				else if (now != kAspectSquare)
					g.square_why = "the camera's aspect constant is changed by something else: left alone";
			}
			else if (now == kAspectSquare)
			{
				if (g.square_game != game)
					g.square_why = "0.75 set by something else (the drawcull addon?): left alone";
				else if (put_word(game + kAspectK, kAspectStock))
					g.square_game = 0;
			}
		}

		// A multiplier written when its option changed (and at the first update), as the addon wrote them at load and
		// at each change
		void set_multiplier(uintptr_t at, double value, bool &set, uint32_t &written)
		{
			const uint32_t want = bits(value);
			if (set && want == written)
				return;
			if (put_word(at, want))
			{
				set = true;
				written = want;
			}
		}

		// A multiplier back to 1.0 while it's still ours
		void reset_multiplier(uintptr_t at, bool set, uint32_t written)
		{
			uint32_t now = 0;
			if (set && read(at, now) && now == written)
				put_word(at, kOne);
		}

		// A call of ours taken out, only over our own bytes (false: left as it was)
		bool take_out(site &s, const char *what, std::vector<std::string> &left)
		{
			if (!s.in)
				return true;
			if (code_is(s.at, s.ours, s.size) && write_code(s.at, s.stock, s.size))
			{
				s.in = false;
				return true;
			}
			left.push_back(std::string(what) + " was changed by something else since: left as it was");
			return false;
		}

		void toggle_off(toggle &t, const char *what, std::vector<std::string> &left)
		{
			if (!t.mine)
				return;
			if (code_is(t.at, t.ours, t.size) && write_code(t.at, t.stock, t.size))
				t.mine = false;
			else
				left.push_back(std::string(what) + " was changed by something else since: left as it was");
		}

		std::string value_text(uintptr_t at)
		{
			uint32_t b = 0;
			float f = 0.0f;
			char text[32] = "?";
			if (read(at, b) && float_of(b, f))
				std::snprintf(text, sizeof(text), "%.2f", f);
			return text;
		}
	}

	void update()
	{
		start();
		if (g.world == 0)
			return;
		g.o = read_options();
		set_toggle(g.vis, g.o.vis);
		apply_square(g.o.square);
		set_toggle(g.frustum, g.o.frustum);
		set_multiplier(g.world, g.o.world, g.world_set, g.world_bits);
		set_multiplier(g.entity, g.o.entity, g.entity_set, g.entity_bits);
		if (g.mem != 0)
		{
			const double l = lod_value(g.o);
			*reinterpret_cast<volatile uint32_t *>(g.mem + kFactor) = bits(1.0 / (l * l));
		}
	}

	void frame(double)
	{
	}

	void stop()
	{
		std::vector<std::string> left;
		bool out = true;
		// (the calls, the last put in first, as the addon's unload)
		for (int i = 3; i >= 0; --i)
			out = take_out(g.lod[i], "a mesh detail compare", left) && out;
		out = take_out(g.culling, "the object culling call", left) && out;
		toggle_off(g.vis, "the zone visibility list's read", left);
		if (g.square_game != 0)
			apply_square(false);
		toggle_off(g.frustum, "the culling frustum's setup", left);
		if (g.world != 0)
		{
			reset_multiplier(g.world, g.world_set, g.world_bits);
			reset_multiplier(g.entity, g.entity_set, g.entity_bits);
		}
		if (g.mem != 0)
		{
			if (out)
				free_code(g.mem);
			else
				left.push_back("its stubs' memory is left (something may still call into it)");
		}
		g = state();
		g_left.clear();
		for (const std::string &l : left)
			g_left += (g_left.empty() ? "" : "; ") + l;
	}

	std::vector<std::string> status()
	{
		std::vector<std::string> out;
		char line[320];
		if (g.world == 0)
			out.push_back("drawcull: " + g.why);
		else
		{
			const options &o = g.o;
			std::snprintf(line, sizeof(line), "drawcull: world %.2f, entity %.2f, detail %.2f%s, vis %s, square %s, frustum %s", o.world, o.entity,
				lod_value(o), o.lod <= 0 ? " (same as world)" : "", o.vis ? "on" : "off", o.square ? "on" : "off", o.frustum ? "on" : "off");
			out.push_back(line);
			uint32_t now = 0;
			if (g.world_set && read(g.world, now) && now != g.world_bits)
				out.push_back("drawcull: the world multiplier is " + value_text(g.world) + " now (changed by something else since)");
			if (g.entity_set && read(g.entity, now) && now != g.entity_bits)
				out.push_back("drawcull: the entity multiplier is " + value_text(g.entity) + " now (changed by something else since)");
			if (!g.culling.why.empty())
				out.push_back("drawcull: object ranges not scaled: " + g.culling.why);
			int lod_in = 0;
			std::string lod_why;
			for (const site &s : g.lod)
			{
				lod_in += s.in ? 1 : 0;
				if (!s.in && lod_why.empty())
					lod_why = s.why;
			}
			if (lod_in < 4)
			{
				std::snprintf(line, sizeof(line), "drawcull: %d of 4 mesh detail sites not patched (some objects still drop detail early): ", 4 - lod_in);
				out.push_back(line + lod_why);
			}
			if (!g.vis.why.empty())
				out.push_back("drawcull: vis: " + g.vis.why);
			if (!g.game_why.empty())
				out.push_back("drawcull: " + g.game_why);
			else if (!g.square_why.empty())
				out.push_back("drawcull: square: " + g.square_why);
			if (!g.frustum.why.empty())
				out.push_back("drawcull: frustum: " + g.frustum.why);
		}
		if (!g_left.empty())
			out.push_back("drawcull: at its last stop, " + g_left);
		return out;
	}

	sites where()
	{
		sites s;
		s.world = g.world;
		s.entity = g.entity;
		s.memory = g.mem;
		s.culling = g.culling.at;
		s.scale = g.scale;
		s.vis = g.vis.at;
		s.frustum = g.frustum.at;
		s.game = g.game;
		for (int i = 0; i < 4; ++i)
			s.lod[i] = g.lod[i].at;
		return s;
	}
}
