/*
 * The texture pack's glue for the zones and the model actors (zonepack.hpp; docs/texture-pack/zones.md "Requests: order,
 * pacing and the quiet window", "Destruction points"). The game's thread only: no lock of its own.
 *
 * Each gpu_zone with an owner has an entry here: the order its textures are asked in, how far it got, its tickets by
 * slot, what came of them. The entries live in a map made once and never freed, so a gpu_zone destroyed during the
 * DLL's static destruction (after this file's) still finds it from the drop hook.
 */

#include <windows.h>
#include "zonepack.hpp"
#include "actordata.hpp"
#include "native.hpp"
#include "zone_gpu.hpp"
#include "../pack/stream.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace hifi::zone::zonepack
{
	namespace
	{
		namespace zm = bake::zonemesh;
		namespace st = hifi::pack::stream;

		struct entry
		{
			int32_t base = 0;    // (its priorities' base: the zone's, the second map's, the model actors')
			const char *what = "";
			uint32_t map = 0;    // (its map or file id, for the log)
			double since = 0.0;  // (when it got its owner)
			bool ordered = false;
			std::vector<uint32_t> order; // (its keyed textures, most used first, then nearest)
			size_t next = 0;             // (the next of them to ask for)
			std::unordered_map<uint32_t, uint32_t> by_slot; // (outstanding tickets: slot -> texture)
			uint32_t drawn = 0, not_in_pack = 0, failed = 0, gone = 0;
			bool logged = false;
		};

		struct state
		{
			bool zones = false, actors = false;
			camera_fn camera = nullptr;
			log_fn log = nullptr;
			double now = 0.0; // (the last frame's)
			uint32_t cap = 0; // (set_request_cap: the tests')
			counts n;
		};
		state g;

		// (made once, never freed: see the file's comment)
		std::unordered_map<gpu_zone *, entry> &entries()
		{
			static auto *const m = new std::unordered_map<gpu_zone *, entry>();
			return *m;
		}

		void log(int level, const std::string &line)
		{
			if (g.log != nullptr)
				g.log(level, ("HiFi XI: " + line).c_str());
		}

		// A zone's (not a model actor DAT's) coverage, once: when its requests have settled, or as it's let go
		void log_entry(const gpu_zone &z, entry &e, double now, const char *how)
		{
			if (e.logged || e.base == kActorPriority)
				return;
			e.logged = true;
			char line[300];
			std::snprintf(line, sizeof(line), "texture pack in %s %u: %u of %zu textures from the pack, %u not in it, %u failed, %u gone; %.0f MB; %s %.1f s after it was ready",
				e.what, e.map, e.drawn, z.data().textures.size(), e.not_in_pack, e.failed, e.gone, double(z.pack().bytes) / 1048576.0, how, now - e.since);
			log(0, line);
		}

		// The drop hook (gpu_zone::pack_let_go, release, the destructor) and clear_all's: the owner dropped (no wait: the
		// zone may go right after), every word spent, the visit done, out of the list
		void drop_zone(gpu_zone &z)
		{
			gpu_zone::pack_data &p = z.pack();
			auto &m = entries();
			const auto it = m.find(&z);
			if (it != m.end())
				log_entry(z, it->second, g.now, "let go");
			if (p.owner != 0)
			{
				st::drop_owner(p.owner);
				g.n.dropped++;
			}
			for (uint32_t i = 0; i < z.data().textures.size(); ++i)
				if (z.pack_word(i) != gpu_zone::kPackSpent)
					z.set_pack_word(i, gpu_zone::kPackSpent);
			p.owner = 0;
			p.done = true;
			p.bytes = 0;
			if (it != m.end())
				m.erase(it);
			g.n.zones = uint32_t(m.size());
		}

		// The textures' order: most used first (triangles naming it times its model's placements, by powers of two), then
		// the nearest placement of a model using it to the camera (none known: by use only)
		void make_order(const gpu_zone &z, entry &e, const float *camera)
		{
			const zm::zone &d = z.data();
			const size_t n = d.textures.size();
			std::vector<uint64_t> placed(d.models.size(), 0);
			std::vector<float> near2(d.models.size(), 3.0e38f);
			for (const zm::placement &pl : d.placements)
			{
				if (pl.model >= d.models.size())
					continue;
				placed[pl.model]++;
				if (camera != nullptr)
				{
					const float dx = pl.t[0] - camera[0], dy = pl.t[1] - camera[1], dz = pl.t[2] - camera[2];
					near2[pl.model] = std::min(near2[pl.model], dx * dx + dy * dy + dz * dz);
				}
			}
			std::vector<uint64_t> uses(n, 0);
			std::vector<float> dist(n, 3.0e38f);
			for (size_t m = 0; m < d.models.size(); ++m)
			{
				const zm::model &mo = d.models[m];
				for (uint32_t k = mo.first_piece; k < mo.first_piece + mo.pieces && k < d.pieces.size(); ++k)
				{
					const zm::piece &pc = d.pieces[k];
					if (pc.texture >= n)
						continue;
					uses[pc.texture] += uint64_t(pc.triangles) * std::max<uint64_t>(placed[m], 1);
					dist[pc.texture] = std::min(dist[pc.texture], near2[m]);
				}
			}
			e.order.clear();
			for (uint32_t i = 0; i < n; ++i)
				if (d.textures[i].key.bytes != 0)
					e.order.push_back(i);
			auto tier = [&](uint32_t i) { return int(std::floor(std::log2(1.0 + double(uses[i])))); };
			std::stable_sort(e.order.begin(), e.order.end(), [&](uint32_t a, uint32_t b) {
				const int ta = tier(a), tb = tier(b);
				if (ta != tb)
					return ta > tb;
				return dist[a] < dist[b];
			});
			e.ordered = true;
		}

		// A gpu_zone given an owner (once a visit: not again once done)
		void take(gpu_zone *z, int32_t base, const char *what, uint32_t epoch, double now)
		{
			if (z == nullptr || !z->has_pack() || z->pack().done || z->pack().owner != 0)
				return;
			gpu_zone::set_pack_drop(&drop_zone);
			char name[48];
			std::snprintf(name, sizeof(name), "%s %u", what, z->data().map);
			z->pack().owner = st::add_owner(name);
			z->pack().epoch = epoch;
			entry &e = entries()[z];
			e = entry {};
			e.base = base;
			e.what = what;
			e.map = z->data().map;
			e.since = now;
			g.n.zones = uint32_t(entries().size());
		}

		// A ticket handed over by take_done: a ready one's view written, then drawn; else dropped and spent
		void settle(gpu_zone &z, entry &e, st::ticket t, st::state s, uint32_t &views)
		{
			const auto at = e.by_slot.find(t.slot);
			if (at == e.by_slot.end())
				return;
			const uint32_t i = at->second;
			if (i >= z.pack().tickets.size() || !(z.pack().tickets[i] == t))
				return;
			e.by_slot.erase(at);
			if (s == st::state::ready && z.pack_word(i) == gpu_zone::kPackAsked)
			{
				// (the slot written once in this gpu_zone's life, before its word says drawn: no list can be reading it)
				st::view v;
				if (st::get(t, v) && st::write_view(t, z.pack_cpu(i)))
				{
					z.pack_written(i);
					z.pack().bytes += v.bytes;
					z.set_pack_word(i, gpu_zone::kPackDrawn);
					e.drawn++;
					g.n.drawn++;
					views++;
					return;
				}
				s = st::state::gone; // (gone between take_done and here)
			}
			const st::miss why = s == st::state::missed ? st::reason(t) : st::miss::none;
			st::drop(t);
			z.pack().tickets[i] = st::ticket {};
			z.set_pack_word(i, gpu_zone::kPackSpent);
			if (s == st::state::missed && (why == st::miss::not_in_pack || why == st::miss::no_pack_flag))
				e.not_in_pack++, g.n.not_in_pack++;
			else if (s == st::state::missed)
				e.failed++, g.n.failed++;
			else
				e.gone++, g.n.gone++;
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
		std::vector<gpu_zone *> zs;
		for (auto &[z, e] : entries())
			zs.push_back(z);
		for (gpu_zone *z : zs)
			drop_zone(*z);
	}

	void set_mode(bool zones, bool actors)
	{
		g.zones = zones;
		g.actors = actors;
		native::set_pack_mode(zones);
		actordata::set_pack_mode(actors);
	}

	void frame(double now, uint32_t epoch)
	{
		g.now = now;
		// Owners for the gpu_zones read in pack mode that are up
		if (g.zones)
		{
			take(native::ready_zone(0), kZonePriority, "zone", epoch, now);
			take(native::ready_zone(1), kSecondPriority, "second map", epoch, now);
		}
		if (g.actors)
		{
			std::vector<gpu_zone *> files;
			actordata::ready_files(files);
			for (gpu_zone *z : files)
				take(z, kActorPriority, "model DAT", epoch, now);
		}
		auto &m = entries();
		if (m.empty())
		{
			g.n.last_requests = g.n.last_views = 0;
			return;
		}
		// (the zone's first, then its second map's, then the model actors': by base, the most first)
		std::vector<std::pair<gpu_zone *, entry *>> list;
		for (auto &[z, e] : m)
			list.emplace_back(z, &e);
		std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.second->base != b.second->base ? a.second->base > b.second->base : a.first < b.first; });

		// Each one's order, once: a zone's from a camera of its own (the last zone pass's, when that was this zone), or
		// after kCameraWaitS without one by use only; a model actor DAT's at once (no placements)
		float camera[3] = {};
		uint32_t camera_map = 0;
		const bool have_camera = g.camera != nullptr && g.camera(camera, camera_map);
		const gpu_zone *const zone0 = native::ready_zone(0);
		for (auto &[z, e] : list)
		{
			if (e->ordered)
				continue;
			const bool own_camera = have_camera && e->base != kActorPriority && zone0 != nullptr && camera_map == zone0->data().map;
			if (e->base != kActorPriority && !own_camera && now - e->since < kCameraWaitS)
				continue;
			make_order(*z, *e, own_camera ? camera : nullptr);
		}

		// Requests: at most kRequestsPerPresent in all, the rest at the next present (they wait in the streamer while the
		// area settles: kQuiet)
		uint32_t asked = 0;
		const uint32_t cap = g.cap != 0 ? g.cap : kRequestsPerPresent;
		for (auto &[z, e] : list)
		{
			const size_t n = e->order.size();
			while (e->ordered && e->next < n && asked < cap)
			{
				const size_t rank = e->next++;
				const uint32_t i = e->order[rank];
				if (z->pack_word(i) != gpu_zone::kPackNone)
					continue;
				const zm::texture &t = z->data().textures[i];
				st::want w;
				w.k = t.key;
				w.kind = hifi::pack::kColour;
				w.first = 0;
				w.count = 0;
				w.down_to_w = t.key.w;
				w.down_to_h = t.key.h;
				w.priority = e->base + int32_t(n - rank);
				w.epoch = epoch;
				const st::ticket tk = st::request(z->pack().owner, w);
				asked++;
				g.n.asked++;
				if (tk.generation == 0)
				{
					z->set_pack_word(i, gpu_zone::kPackSpent); // (no owner any more, or the table full)
					e->failed++;
					g.n.failed++;
					continue;
				}
				z->pack().tickets[i] = tk;
				e->by_slot[tk.slot] = i;
				z->set_pack_word(i, gpu_zone::kPackAsked);
			}
		}
		g.n.last_requests = asked;
		g.n.most_in_a_present = std::max(g.n.most_in_a_present, asked);

		// The tickets that changed: views for the ready ones, the rest spent
		uint32_t views = 0;
		std::vector<std::pair<st::ticket, st::state>> done;
		for (auto &[z, e] : list)
		{
			if (e->by_slot.empty())
				continue;
			done.clear();
			st::take_done(z->pack().owner, done);
			for (const auto &[t, s] : done)
				settle(*z, *e, t, s, views);
			if (e->next >= e->order.size() && e->by_slot.empty() && e->ordered)
				log_entry(*z, *e, now, "settled");
		}
		g.n.last_views = views;
	}

	void set_compare(bool on)
	{
		gpu_zone::set_compare(on);
	}

	void lines(std::vector<std::string> &out)
	{
		char line[300];
		if (const gpu_zone *const z = native::ready_zone(0))
		{
			const zm::zone &d = z->data();
			if (!d.pack)
				out.push_back("This area was entered before the pack was on: its textures switch at the next area.");
			else if (d.overlaid)
				out.push_back("This area is read from another XIPivot overlay: the game's own textures are drawn.");
			else if (z->pack().done)
				out.push_back("This area keeps the game's own textures until the next (the pack was turned off or reloaded here).");
			else if (z->has_pack())
			{
				const gpu_zone::pack_counts c = z->pack_summary();
				const auto it = entries().find(const_cast<gpu_zone *>(z));
				const uint32_t not_in = it != entries().end() ? it->second.not_in_pack : 0, failed = it != entries().end() ? it->second.failed : 0;
				const uint32_t coming = c.asked + (c.keyed - std::min(c.keyed, c.asked + c.drawn + c.spent));
				std::snprintf(line, sizeof(line), "This area: %u of %u scenery textures from the pack (%u not in it%s); %u coming", c.drawn, c.textures,
					not_in + (c.textures - c.keyed), failed != 0 ? (", " + std::to_string(failed) + " failed").c_str() : "", coming);
				out.push_back(line);
			}
		}
		uint32_t files = 0, textures = 0, drawn = 0;
		for (const auto &[z, e] : entries())
			if (e.base == kActorPriority)
			{
				files++;
				textures += uint32_t(z->data().textures.size());
				drawn += e.drawn;
			}
		if (files != 0)
		{
			std::snprintf(line, sizeof(line), "Ships and furniture: %u of %u textures from the pack (%u files).", drawn, textures, files);
			out.push_back(line);
		}
	}

	void set_camera_source(camera_fn fn)
	{
		g.camera = fn;
	}

	void set_log(log_fn fn)
	{
		g.log = fn;
	}

	counts get_counts()
	{
		counts c = g.n;
		c.zones = uint32_t(entries().size());
		return c;
	}

	std::vector<uint32_t> order_of(const gpu_zone *z)
	{
		const auto it = entries().find(const_cast<gpu_zone *>(z));
		return it != entries().end() ? it->second.order : std::vector<uint32_t>();
	}

	void set_request_cap(uint32_t n)
	{
		g.cap = n;
	}
}
