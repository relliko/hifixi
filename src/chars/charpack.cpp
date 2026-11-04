/*
 * The texture pack's glue for the characters (charpack.hpp; docs/texture-pack/characters.md "Requests, levels and
 * pacing", "When tickets are read"). The game's thread only: no lock of its own. Every streamer call is made here, none
 * with the cache's mutex held (the cache's calls take it and leave it before returning); the cache keeps every texture's
 * pack part, so there is no list of tickets here to go stale: a ticket take_done hands over is looked up in the cache,
 * and one the cache let go meanwhile (forgotten, cleared) is no longer anyone's there and is passed over.
 */

#include <windows.h>
#include "charpack.hpp"
#include "cache.hpp"
#include "../pack/stream.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace hifi::chars::charpack
{
	namespace
	{
		namespace st = hifi::pack::stream;

		struct state
		{
			bool chars = false;   // (set_mode's: the characters ask while the pack is in use)
			bool heights = false; // (set_mode's, M9: the heights asked beside the colours, [textures] height)
			st::owner owner = 0;  // (made at the first frame in use, dropped by clear_all)
			sources src;
			counts n;
			bool overlay_logged = false; // (once a session: the overlay listed and every key missed)
			// (eviction: the next round's time (evicting while the pressure holds, giving back once it's been gone the
			// hold), since when it's been gone (below 0: it holds, or nothing is evicted), whether any texture is evicted
			// now, the bytes a round lets go or gives back; the hold now (doubled each time the pressure comes back within
			// kBackoffWindowS of a round given back) and that round's time, below 0: none lately)
			double evict_next = 0.0, clear_since = -1.0;
			bool evicted = false;
			uint64_t evict_bytes = kEvictBytes;
			double hold = kEvictHoldS, unevict_at = -1.0;
		};
		state g;
		// (reused each frame: no allocation once they've grown)
		std::vector<cache::pack_ask> g_asks;
		std::vector<std::pair<st::ticket, st::state>> g_done;

		void log(int level, const std::string &line)
		{
			if (g.src.log != nullptr)
				g.src.log(level, ("HiFi XI: " + line).c_str());
		}

		// Every pack part let go and the owner dropped (asking again only from the next frame in use)
		void let_go()
		{
			cache::set_pack_on(false);
			cache::forget_pack_textures();
			if (g.owner != 0)
				st::drop_owner(g.owner);
			g.owner = 0;
			// (the evicted ones none again with the rest: asked in the next epoch once the pressure allows)
			g.evicted = false;
			g.clear_since = -1.0;
			g.evict_next = 0.0;
			g.hold = kEvictHoldS;
			g.unevict_at = -1.0;
		}

		// Plan section 5: while the streamer's pressure holds (over 95% of the video memory), each half second the
		// textures a model drew longest ago go back to the game's (their pairs and tickets behind the GPU), up to
		// evict_bytes a round; nothing is asked meanwhile. Asked again only once it's been gone the hold (kEvictHoldS: the
		// 95% check reads the usage Windows reports half a second late), and then evict_bytes a half second, the ones
		// drawn last first, so a pressure that comes back stops them part way. One that comes back within kBackoffWindowS
		// of a round given back says they don't fit: the next hold twice as long, up to kEvictHoldMaxS, so a crowd that
		// stays over the card's memory flips between the pack and the game's ever more rarely; back to kEvictHoldS once
		// all are back and the pressure has stayed away kBackoffWindowS
		void follow_pressure(double now, bool pressed)
		{
			if (pressed)
			{
				g.clear_since = -1.0;
				if (g.unevict_at >= 0.0 && now - g.unevict_at < kBackoffWindowS)
				{
					g.unevict_at = -1.0;
					g.hold = std::min(g.hold * 2.0, kEvictHoldMaxS);
					char line[200];
					std::snprintf(line, sizeof(line), "texture pack: video memory over 95%% again just after character textures came back: the next held %.0f s", g.hold);
					log(0, line);
				}
				if (now < g.evict_next)
					return;
				g.evict_next = now + kEvictEveryS;
				uint64_t bytes = 0;
				const uint32_t n = cache::pack_evict(g.evict_bytes, bytes);
				if (n == 0)
					return;
				g.evicted = true;
				g.n.evicted += n;
				g.n.evict_rounds++;
				char line[200];
				std::snprintf(line, sizeof(line), "texture pack: %u character textures back to the game's (video memory over 95%%), %.1f MB", n, double(bytes) / 1048576.0);
				log(0, line);
				return;
			}
			if (!g.evicted)
			{
				if (g.unevict_at >= 0.0 && now - g.unevict_at >= kBackoffWindowS)
				{
					g.unevict_at = -1.0;
					g.hold = kEvictHoldS;
				}
				return;
			}
			if (g.clear_since < 0.0)
				g.clear_since = now;
			if (now - g.clear_since < g.hold || now < g.evict_next)
				return;
			g.evict_next = now + kEvictEveryS;
			uint32_t left = 0;
			cache::pack_unevict(g.evict_bytes, left);
			g.unevict_at = now;
			if (left == 0)
			{
				g.evicted = false;
				g.clear_since = -1.0;
			}
		}

		// A ticket take_done handed over (a texture's colour, or its height: M9): settled in the cache; once both of a
		// texture's are, its pair taken and written, then ready; or spent when neither had anything
		void settle(st::ticket t, st::state s)
		{
			uint32_t key_w = 0;
			const bool height = cache::pack_height_of(t, key_w);
			bool ready = s == st::state::ready;
			float relief = 0.0f;
			cache::pack_miss m = cache::pack_miss::gone;
			if (height)
			{
				// (the relief scale as the zones': 4 x span x the map's texels a game texel, zones.md "Pack normals". A flat
				// map, or a scale that isn't a number, gives nothing to draw: let go as missed)
				st::view v;
				float span = 0.0f;
				if (ready && st::get(t, v))
					std::memcpy(&span, &v.param, 4);
				relief = key_w != 0 ? 4.0f * span * float(v.w) / float(key_w) : 0.0f;
				if (ready && !(std::isfinite(relief) && relief > 0.0f))
					ready = false;
				if (!ready)
					g.n.heights_missed++;
			}
			else if (!ready)
			{
				const st::miss why = s == st::state::missed ? st::reason(t) : st::miss::none;
				if (s == st::state::missed && (why == st::miss::not_in_pack || why == st::miss::no_pack_flag))
					m = cache::pack_miss::not_in_pack, g.n.not_in_pack++;
				else if (s == st::state::missed)
					m = cache::pack_miss::failed, g.n.failed++;
				else
					g.n.gone++;
			}
			// (a ready colour's view bytes: kept while its height is still on its way, the pace pack_evict lets it go at)
			uint64_t colour_bytes = 0;
			if (!height && ready)
			{
				st::view cv;
				if (st::get(t, cv))
					colour_bytes = cv.bytes;
			}
			cache::pack_writing w;
			if (cache::pack_settle(t, ready, m, relief, w, colour_bytes) != cache::pack_step::write)
				return; // (the other on its way, spent, not the cache's any more, or no pair to spare: let go there)
			// (the pair written once, the cache's mutex left: the colour from its ticket (BC2, the levels it got) or a view
			// of the game's texture; the detail the height from its ticket (BC4, its whole chain) or a view of the game
			// texture's own detail map)
			bool ok = true;
			uint64_t bytes = 0;
			if (w.t.generation != 0)
			{
				st::view v;
				ok = st::get(w.t, v) && st::write_view(w.t, w.colour);
				bytes += v.bytes;
			}
			if (ok && w.height.generation != 0)
			{
				st::view v;
				ok = st::get(w.height, v) && st::write_view(w.height, w.detail);
				bytes += v.bytes;
			}
			cache::write_pack_detail(w);
			cache::pack_end(w, ok, bytes);
			if (ok)
			{
				g.n.views++;
				g.n.last_views++;
				g.n.heights += w.height.generation != 0;
			}
			else
				g.n.gone++; // (gone between take_done and here)
		}

		std::string number(uint64_t n)
		{
			return std::to_string(n);
		}
	}

	pack::consumer consumer()
	{
		pack::consumer c;
		c.clear_all = &clear_all;
		c.set_mode = &set_mode;
		c.frame = &frame;
		c.set_compare = &set_compare;
		c.lines = &lines;
		return c;
	}

	void clear_all()
	{
		let_go();
	}

	void set_mode(const pack::modes &m)
	{
		// (M9: heights turned on or off let every pack part go too, so each pair is written again in the new mode at once:
		// with them off, the detail map's view again in every pair, as before M9; characters switch at once both ways)
		const bool heights_changed = m.heights != g.heights;
		g.chars = m.chars;
		g.heights = m.heights;
		if (!m.chars || (heights_changed && g.owner != 0))
			let_go(); // (at once: the game's textures are handed out again from now)
	}

	void frame(double now, uint32_t epoch)
	{
		g.n.last_requests = g.n.last_views = 0;
		if (!g.chars)
			return;
		// Only onto the cache's own device (it never follows a change): a view of one device's texture is never written
		// into another device's heap
		ID3D12Device *const theirs = g.src.device != nullptr ? g.src.device() : nullptr, *const mine = cache::device();
		g.n.other_device = theirs != nullptr && mine != nullptr && mine != theirs;
		if (theirs == nullptr || mine != theirs)
		{
			if (g.owner != 0)
				let_go();
			return;
		}
		cache::set_pack_drop(&st::drop);
		if (g.owner == 0)
			g.owner = st::add_owner("characters");
		cache::set_pack_on(true);
		cache::pack_tick(now);

		// Eviction under the pressure, then requests: none while the card is nearly full (they'd only wait); the cache holds
		// them while the pairs are short
		const bool pressed = st::pressure();
		follow_pressure(now, pressed);
		if (!pressed)
		{
			g_asks.clear();
			cache::take_pack_asks(kRequestsPerPresent, g_asks);
			// (M9: the pack's index, only while heights are on and there are asks: a height is asked only for an entry
			// that has one, so a pack without heights asks nothing more)
			std::shared_ptr<const hifi::pack::index> idx;
			if (g.heights && !g_asks.empty())
				idx = st::current();
			for (const cache::pack_ask &a : g_asks)
			{
				st::want w;
				w.k = a.k;
				w.kind = hifi::pack::kColour;
				w.first = 0;
				w.count = 0;
				w.down_to_w = a.down_to_w;
				w.down_to_h = a.down_to_h;
				w.priority = kPriority;
				w.epoch = epoch;
				const st::ticket t = st::request(g.owner, w);
				g.n.asked++;
				g.n.last_requests++;
				if (t.generation == 0)
				{
					cache::pack_refused(a.id, a.serial); // (no owner any more, or the table full)
					g.n.refused++;
					continue;
				}
				// (M9) Its height beside it, whatever comes of the colour (a materials-only entry has none: the game's
				// texture is drawn with the pack's relief), the whole chain (mip-mapped at a distance, so it doesn't
				// sparkle), at the same priority. Refused: the colour alone
				st::ticket h;
				const hifi::pack::entry *pe = idx ? idx->find(a.k) : nullptr;
				if (pe != nullptr && pe->map(hifi::pack::kHeight) != nullptr)
				{
					st::want hw;
					hw.k = a.k;
					hw.kind = hifi::pack::kHeight;
					hw.first = 0;
					hw.count = 0;
					hw.priority = kPriority;
					hw.epoch = epoch;
					h = st::request(g.owner, hw);
					g.n.heights_asked += h.generation != 0;
				}
				if (!cache::pack_asked(a.id, a.serial, t, epoch, h))
				{
					st::drop(t); // (the texture went meanwhile)
					if (h.generation != 0)
						st::drop(h);
				}
			}
		}

		// The tickets that changed: views for the ready ones, the rest spent
		g_done.clear();
		st::take_done(g.owner, g_done);
		for (const auto &[t, s] : g_done)
			settle(t, s);

		if (!g.overlay_logged && g.n.not_in_pack != 0 && g.n.views == 0 && g.src.overlay_listed != nullptr && g.src.overlay_listed())
		{
			g.overlay_logged = true;
			log(1, "texture pack: the characters' textures aren't in the pack while the hifi-textures XIPivot overlay is installed (the game holds "
				"its copies, and the pack is keyed from the original files): the characters keep the game's textures");
		}
	}

	void set_compare(bool on)
	{
		cache::set_pack_compare(on);
	}

	void lines(std::vector<std::string> &out)
	{
		if (!g.chars)
		{
			out.push_back("Characters: the game's own textures ([textures] chars is off).");
			return;
		}
		if (g.n.other_device)
		{
			out.push_back("Characters: not from the pack until the game is restarted (the graphics device changed).");
			return;
		}
		const cache::counts c = cache::get_counts();
		if (c.pack_ready == 0 && g.src.overlay_listed != nullptr && g.src.overlay_listed())
		{
			out.push_back("Characters: none of " + number(c.textures_ready) + " textures from the pack: the hifi-textures overlay is still installed, so the "
				"game holds its own bigger copies, and the pack is keyed from the game's original files.");
			return;
		}
		std::string line = "Characters: " + number(c.pack_ready) + " of " + number(c.textures_ready) + " textures from the pack (" + number(c.pack_not_in) + " not in it";
		if (c.pack_failed != 0)
			line += ", " + number(c.pack_failed) + " failed";
		if (c.pack_evicted != 0)
			line += ", " + number(c.pack_evicted) + " back to the game's while the graphics card is nearly full";
		line += ", " + number(c.pack_asked) + " coming)";
		// (M9: only once any has the pack's relief, so a pack without heights reads as before)
		if (c.pack_relief != 0)
			line += "; relief from the pack on " + number(c.pack_relief) + (c.pack_relief_only != 0 ? " (" + number(c.pack_relief_only) + " of them on the game's own colour)" : std::string());
		if (c.pack_waiting_slots != 0)
			line += "; " + number(c.pack_waiting_slots) + " skipped (the characters' descriptors are nearly all in use)";
		if (g.src.game_drawn != nullptr)
			line += "; " + number(g.src.game_drawn()) + " models drawn by the game itself";
		out.push_back(line + ".");
	}

	void set_sources(const sources &s)
	{
		g.src = s;
	}

	counts get_counts()
	{
		counts c = g.n;
		c.owner = g.owner != 0;
		c.hold_s = g.hold;
		return c;
	}

	namespace test
	{
		void set_evict_bytes(uint64_t bytes)
		{
			g.evict_bytes = bytes != 0 ? bytes : kEvictBytes;
		}
	}
}
