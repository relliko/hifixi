/*
 * Each character model the game is about to draw, recorded (charmodel.hpp; research/ffxi-re/notes-characters.md).
 *
 * The site (02B7DB: test al, al; je ...; then 02B7E3: mov ecx, imm32; call ...), reached after posing on both of
 * 02B6E0's paths (posed now, or the throttled frame's jump straight there). There ebp = the model (actor + 0x674),
 * esi = argument 1 (the actor), and the colour (argument 7) is at [esp + 0xA8]. The 5 bytes become jmp stub:
 *   pushad; lea eax, [esp+20h]; push eax; push esi; push ebp; call on_model; add esp, 0Ch; test eax, eax; popad
 *   jnz leave; mov ecx, imm32 (as it was); jmp 02B7E8
 *   leave: jmp 02B7B0 (the draw's early return: model +0xB4 set, ret 1Ch)
 * on_model is cdecl with copies of what it needs as its arguments (popad restores the registers from pushad's own
 * slots, never from the arguments).
 *
 * The live chain (notes "A live actor", checked live 2026-10-24): model +0x44 the first skeleton instance (next at
 * +4); an instance's Sk2 handle at +0xC ([handle] = the resource, bone count u16 at +0x32), its palette at +0x14 (bone
 * count x 64 bytes, world space), its mesh list at +0x20 (next at +4); a mesh node's +8 the Os2 resource's handle.
 * A resource: +0x14 its own handle, +0x20 the chunk header copy (+0x24: kind | size << 7, size in 16s), +0x28 its
 * folder's handle, +0x30 the data; +8 the next resource's handle (file order). A live Os2 resource's section offsets
 * are pointers (02D8F0); a drawn texture command is 0x8001 with the texture object in the name's first dword (object
 * +8: its resource).
 *
 * The hot path (palettes and the node walk, every model every frame) reads the game's memory directly under SEH; a
 * mesh or texture seen the first time is copied with game::read (can't crash).
 */

#include <windows.h>
#include "charmodel.hpp"
#include "charslot.hpp"
#include "../chars/cache.hpp"
#include "../chars/charframe.hpp"
#include "../chars/os2.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

namespace hifi::game::charmodel
{
	namespace
	{
		namespace ch = hifi::chars;

		constexpr const char *kSite = "84C00F84??????????????????E8????????C705????????000080BF"; // 02B7DB; the mov (B9 imm32) at +8
		constexpr intptr_t kSiteAt = 8;
		constexpr const char *kReturn = "A1????????5F8985B40000005E5D5B83C47CC21C00";            // 02B7B0
		constexpr const char *kLights = "8A9408A8000000B8????????84D20F95442412";                 // 02E0E5: mov eax, the table (+8)
		constexpr const char *kAmbient = "A1????????25FF00000033F6";                              // 02E1C5: mov eax, [AMBIENT] (+1)
		constexpr const char *kFactor = "A1????????8B0D????????668B35";                           // 02255D / 02DE5C: the colour factor at +7
		constexpr const char *kRenderer = "8B0D????????85C9744B83C044";                           // 013C81: mov ecx, [the renderer] (+2)
		// 02B6FB: mov ecx, [the device wrapper] (+2); its +0xC the IDirect3DDevice8 the character pass draws with
		constexpr const char *kDevice = "8B0D????????83C40833DB53891D????????E8";
		// IDirect3DDevice8::GetRenderState, and the fog's states (FOGCOLOR, FOGSTART, FOGEND)
		constexpr uint32_t kGetRenderState = 51, kFogColour = 34, kFogStart = 36, kFogEnd = 37;
		using get_state_fn = long(__stdcall *)(uintptr_t self, uint32_t state, uint32_t *value);

		constexpr uint32_t kColourAt = 0xA8; // (argument 7 at the site's esp)
		constexpr uint32_t kMaxInstancesPerModel = 8, kMaxMeshesPerInstance = 32, kMaxBonesPerInstance = 255;
		constexpr size_t kStubSize = 0x30;
		constexpr size_t kCallAt = 0x08, kMovAt = 0x15, kGoOnAt = 0x1A, kLeaveAt = 0x1F;

		struct raw_instance
		{
			uint32_t bones = 0, first_bone = 0, meshes = 0;
			uint32_t mesh[kMaxMeshesPerInstance];
			ch::cache::key key[kMaxMeshesPerInstance];
		};
		struct raw_model
		{
			uint32_t colour = 0, ambient = 0, tfactor = 0x80808080u, instances = 0;
			ch::light8 lights[4];
			raw_instance instance[kMaxInstancesPerModel];
		};

		struct state
		{
			uintptr_t site = 0, early_return = 0, stub = 0, lights = 0, ambient = 0, tfactor = 0, renderer = 0, device = 0;
			uintptr_t go_on = 0, leave = 0; // (the tests' exits; 0: the game's)
			uint8_t saved[5] = {}, ours[5] = {};
			bool in = false;
			bool game_draws = true, recording = true, native = false;
			ch::frame *f = nullptr; // the frame the walk's begin started (nullptr: none yet)
			bool in_walk = false;   // between the walk's begin and end
			raw_model raw;          // (scratch for the model being recorded)
			counts n;
		};
		state g;

		// A resource's key (its handle, header word and first section pointer: what tells it from another one at the
		// same address later)
		bool key_of(uintptr_t res, ch::cache::key &k)
		{
			k.resource = res;
			k.handle = *reinterpret_cast<const uint32_t *>(res + 0x14);
			k.size = *reinterpret_cast<const uint32_t *>(res + 0x24);
			k.data = *reinterpret_cast<const uint32_t *>(res + 0x30 + 6);
			return true;
		}
		uint32_t kind_of_header(uint32_t header) { return header & 0x7F; }
		uint32_t data_size(uint32_t header) { const uint32_t s = ((header >> 7) & 0x7FFFF) * 16; return s > 16 ? s - 16 : 0; }

		// The hot path, no C++ objects (SEH): the lights, the instances' palettes into the frame, their meshes' keys.
		// False when the game's memory faulted (nothing of the model kept then).
		bool gather(uintptr_t model, uintptr_t esp, ch::frame &f, raw_model &r)
		{
			__try
			{
				r.colour = *reinterpret_cast<const uint32_t *>(esp + kColourAt);
				std::memcpy(r.lights, reinterpret_cast<const void *>(g.lights), sizeof(r.lights));
				r.ambient = *reinterpret_cast<const uint32_t *>(g.ambient);
				r.tfactor = *reinterpret_cast<const uint32_t *>(g.tfactor);
				r.instances = 0;
				uintptr_t node = *reinterpret_cast<const uint32_t *>(model + 0x44);
				for (uint32_t ni = 0; node >= 0x10000 && ni < kMaxInstancesPerModel; ++ni)
				{
					raw_instance &ri = r.instance[r.instances];
					ri.bones = ri.meshes = 0;
					const uintptr_t handle = *reinterpret_cast<const uint32_t *>(node + 0xC);
					const uintptr_t sk2 = handle >= 0x10000 ? *reinterpret_cast<const uint32_t *>(handle) : 0;
					const uintptr_t palette = *reinterpret_cast<const uint32_t *>(node + 0x14);
					if (sk2 >= 0x10000 && palette >= 0x10000 && kind_of_header(*reinterpret_cast<const uint32_t *>(sk2 + 0x24)) == 0x29)
					{
						uint32_t bones = *reinterpret_cast<const uint16_t *>(sk2 + 0x32);
						if (bones > kMaxBonesPerInstance)
							bones = kMaxBonesPerInstance;
						if (f.bones + bones > ch::kMaxBones)
							return true; // (the frame's palettes are full: the instances so far)
						ri.first_bone = f.bones;
						const float *src = reinterpret_cast<const float *>(palette);
						for (uint32_t b = 0; b < bones; ++b, src += 16)
						{
							float *dst = f.palette[f.bones + b].m;
							dst[0] = src[0], dst[1] = src[1], dst[2] = src[2];
							dst[3] = src[4], dst[4] = src[5], dst[5] = src[6];
							dst[6] = src[8], dst[7] = src[9], dst[8] = src[10];
							dst[9] = src[12], dst[10] = src[13], dst[11] = src[14];
						}
						f.bones += bones;
						ri.bones = bones;
						uintptr_t mesh = *reinterpret_cast<const uint32_t *>(node + 0x20);
						for (uint32_t mi = 0; mesh >= 0x10000 && mi < 64 && ri.meshes < kMaxMeshesPerInstance; ++mi)
						{
							const uintptr_t mh = *reinterpret_cast<const uint32_t *>(mesh + 8);
							const uintptr_t res = mh >= 0x10000 ? *reinterpret_cast<const uint32_t *>(mh) : 0;
							if (res >= 0x10000 && kind_of_header(*reinterpret_cast<const uint32_t *>(res + 0x24)) == 0x2A)
							{
								key_of(res, ri.key[ri.meshes]);
								ri.meshes++;
							}
							mesh = *reinterpret_cast<const uint32_t *>(mesh + 4);
						}
						r.instances++;
					}
					node = *reinterpret_cast<const uint32_t *>(node + 4);
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// A resource's chunk data, copied with reads that can't crash (empty when it can't be read)
		std::vector<uint8_t> chunk_bytes(uintptr_t res, uint32_t want_kind)
		{
			std::vector<uint8_t> out;
			const uint32_t header = word(res + 0x24);
			const uint32_t size = data_size(header);
			if (kind_of_header(header) != want_kind || size == 0 || size > (32u << 20))
				return out;
			out.resize(size);
			if (!read(res + 0x30, out.data(), size))
				out.clear();
			return out;
		}

		// 072430: from `start` (a folder: what's in it; else start and what follows) to its folder's end, the first
		// resource of a kind; resources follow each other through the handle at +8, kind 1 opens a folder, 0 closes one
		uintptr_t find_kind(uintptr_t start, uint32_t kind)
		{
			int depth = kind_of_header(word(start + 0x24)) == 1 ? -1 : 0;
			uintptr_t r = start;
			for (int n = 0; pointer(uint32_t(r)) && n < 200000; ++n)
			{
				const uint32_t k = kind_of_header(word(r + 0x24));
				if (k == 1)
					depth++;
				if (k == 0)
				{
					if (--depth < 0)
						return 0;
				}
				else if (k == kind)
					return r;
				const uint32_t h = word(r + 8);
				r = pointer(h) ? word(h) : 0;
			}
			return 0;
		}

		// 03AF10: the texture chunk of this name in the mesh's folder (the first in file order), its resource
		uintptr_t find_texture(uintptr_t mesh_res, const uint8_t name[16])
		{
			const uint32_t fh = word(mesh_res + 0x28);
			const uintptr_t folder = kind_of_header(word(mesh_res + 0x24)) == 1 ? mesh_res : (pointer(fh) ? word(fh) : 0);
			uintptr_t r = folder != 0 ? find_kind(folder, 0x20) : 0;
			for (int n = 0; r != 0 && n < 100000; ++n)
			{
				uint8_t got[16];
				if (read(r + 0x31, got, 16) && std::memcmp(got, name, 16) == 0)
					return r;
				const uint32_t h = word(r + 8);
				r = pointer(h) ? word(h) : 0;
				r = r != 0 ? find_kind(r, 0x20) : 0;
			}
			return 0;
		}

		uint32_t texture_id(uintptr_t res)
		{
			ch::cache::key k;
			k.resource = res;
			k.handle = word(res + 0x14);
			k.size = word(res + 0x24);
			k.data = word(res + 0x30 + 6);
			uint32_t id = ch::cache::texture(k);
			if (id != ch::cache::kNone)
				return id;
			std::vector<uint8_t> bytes = chunk_bytes(res, 0x20);
			if (bytes.empty())
				return ch::cache::kNone;
			g.n.new_textures++;
			return ch::cache::add_texture(k, std::move(bytes));
		}

		// A mesh's id: known, else copied, its textures found
		uint32_t mesh_id(const ch::cache::key &k)
		{
			uint32_t id = ch::cache::mesh(k);
			if (id != ch::cache::kNone)
				return id;
			std::vector<uint8_t> bytes = chunk_bytes(k.resource, 0x2A);
			if (bytes.empty())
				return ch::cache::kNone;
			const uint32_t base = uint32_t(k.resource + 0x30);
			ch::os2::sections s;
			std::vector<ch::os2::texture_ref> refs;
			std::vector<uint32_t> textures;
			if (ch::os2::sections_of_live(bytes.data(), bytes.size(), base, s) && ch::os2::texture_commands(bytes.data(), bytes.size(), s, refs))
				for (const ch::os2::texture_ref &t : refs)
				{
					uintptr_t res = 0;
					if (t.cached)
						res = t.object != 0 && pointer(t.object) ? word(t.object + 8) : 0;
					else
						res = find_texture(k.resource, t.name);
					const uint32_t tid = res != 0 && pointer(uint32_t(res)) ? texture_id(res) : ch::cache::kNone;
					if (tid == ch::cache::kNone)
						g.n.textures_missing++;
					textures.push_back(tid);
				}
			g.n.new_meshes++;
			return ch::cache::add_mesh(k, std::move(bytes), base, std::move(textures));
		}

		// The camera and the zone's fog as the zone pass left them in the renderer (renderer +0x38F5C VIEW x PROJECTION,
		// FOGCOLOR +0x39254, FOGEND +0x39258, FOGSTART +0x3925C: notes-replace-slots "Built: the native zone draw")
		void read_camera(ch::frame &f)
		{
			const uint32_t r = g.renderer != 0 ? word(g.renderer) : 0;
			if (!pointer(r) || !read(r + 0x38F5C, f.view_proj, 64))
				return;
			const uint32_t fog = word(r + 0x39254);
			f.fog[0] = float((fog >> 16) & 255) / 255.0f;
			f.fog[1] = float((fog >> 8) & 255) / 255.0f;
			f.fog[2] = float(fog & 255) / 255.0f;
			float e = 0.0f, s = 0.0f;
			if (float_of(word(r + 0x39258), e) && float_of(word(r + 0x3925C), s))
			{
				f.fog_end = e;
				f.fog_start = s;
			}
			f.camera = true;
		}

		// The actor's fog as 0CB4E0 just set it on the device (the proxies forward every setter at once and pass
		// GetRenderState through, so the device has what the game set for this model)
		bool read_fog_states(uintptr_t dev, uint32_t &colour, uint32_t &start, uint32_t &end)
		{
			__try
			{
				const auto get = reinterpret_cast<get_state_fn>(uintptr_t(*reinterpret_cast<const uint32_t *>(*reinterpret_cast<const uint32_t *>(dev) + 4 * kGetRenderState)));
				return get(dev, kFogColour, &colour) >= 0 && get(dev, kFogStart, &start) >= 0 && get(dev, kFogEnd, &end) >= 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
		void read_fog(ch::model_entry &m)
		{
			m.fog_read = false;
			const uint32_t wrapper = g.device != 0 ? word(g.device) : 0;
			const uint32_t dev = pointer(wrapper) ? word(wrapper + 0xC) : 0;
			uint32_t colour = 0, start = 0, end = 0;
			if (!pointer(dev) || !read_fog_states(dev, colour, start, end))
				return;
			float s = 0.0f, e = 0.0f;
			if (!float_of(start, s) || !float_of(end, e))
				return;
			m.fog_colour = colour;
			m.fog_start = s;
			m.fog_end = e;
			m.fog_read = true;
		}

		// From the stub: 1 = leave through the early return (the game doesn't draw it)
		uint32_t __cdecl on_model(uintptr_t model, uintptr_t actor, uintptr_t esp)
		{
			uint32_t leave = g.game_draws ? 0 : 1;
			if (leave)
				g.n.skipped++;
			ch::frame *const f = g.f;
			const bool flush = charslot::from_flush();
			if (!g.recording || f == nullptr)
				return leave;
			// (only the walk's models and the flush's: a model drawn anywhere else, a preview say, isn't the world's)
			if (!g.in_walk && !flush)
			{
				g.n.outside++;
				return leave;
			}
			if (f->models >= ch::kMaxModels || f->instances + kMaxInstancesPerModel > ch::kMaxInstances ||
				f->meshes + kMaxInstancesPerModel * kMaxMeshesPerInstance > ch::kMaxMeshes)
			{
				f->dropped++;
				g.n.dropped++;
				return leave;
			}
			const uint32_t bones_before = f->bones;
			raw_model &r = g.raw;
			if (!gather(model, esp, *f, r))
			{
				f->bones = bones_before;
				g.n.faults++;
				return leave;
			}
			ch::model_entry &m = f->model[f->models];
			m.actor = actor;
			m.model = model;
			m.colour = r.colour;
			m.flags = flush ? ch::kFromFlush : 0;
			std::memcpy(m.lights, r.lights, sizeof(m.lights));
			m.ambient = r.ambient;
			m.tfactor = r.tfactor;
			read_fog(m);
			m.first_instance = f->instances;
			m.instances = r.instances;
			m.hidden = 0;
			for (uint32_t i = 0; i < r.instances; ++i)
			{
				const raw_instance &ri = r.instance[i];
				ch::instance_entry &ie = f->instance[f->instances + i];
				ie.first_bone = ri.first_bone;
				ie.bones = ri.bones;
				ie.first_mesh = f->meshes;
				ie.meshes = ri.meshes;
				for (uint32_t k = 0; k < ri.meshes; ++k)
				{
					const uint32_t id = mesh_id(ri.key[k]);
					f->mesh[f->meshes + k].mesh = id;
					if (id != ch::cache::kNone)
						m.hidden |= ch::os2::hidden_by(ch::cache::mesh_hides(id));
				}
				f->meshes += ri.meshes;
			}
			// HiFi XI draws it when it can: its meshes and their textures all on the GPU (and something to draw)
			if (g.native && f->camera && (r.colour >> 24) != 0)
			{
				bool ready = r.instances != 0;
				for (uint32_t i = 0; i < r.instances && ready; ++i)
				{
					const ch::instance_entry &ie = f->instance[f->instances + i];
					for (uint32_t k = 0; k < ie.meshes && ready; ++k)
						ready = ch::cache::mesh_ready(f->mesh[ie.first_mesh + k].mesh);
				}
				if (ready)
				{
					m.flags |= ch::kNative;
					leave = 1;
					g.n.native++;
				}
				else
					g.n.not_ready++;
			}
			f->instances += r.instances;
			f->models++;
			g.n.models++;
			if (flush)
			{
				// (published before its marker goes: the data the marker names is there first)
				g.n.see_through++;
				ch::publish_see_through(*f);
				ch::cache::publish_palettes(*f, bones_before, f->bones);
				charslot::mark_see_through(actor);
			}
			return leave;
		}

		void put_rel(uint8_t *at, uint8_t op, uintptr_t from, uintptr_t to)
		{
			at[0] = op;
			put32(at + 1, uint32_t(to - (from + 5)));
		}

		bool write_stub()
		{
			uint8_t s[kStubSize] = {
				0x60,                   // pushad
				0x8D, 0x44, 0x24, 0x20, // lea eax, [esp+20h] (esp at the site)
				0x50, 0x56, 0x55,       // push eax; push esi (the actor); push ebp (the model)
				0, 0, 0, 0, 0,          // call on_model
				0x83, 0xC4, 0x0C,       // add esp, 0Ch
				0x85, 0xC0,             // test eax, eax
				0x61,                   // popad
				0x75, 0x0A,             // jnz leave
				0, 0, 0, 0, 0,          // mov ecx, imm32 (the game's)
				0, 0, 0, 0, 0,          // jmp the site + 5
				0, 0, 0, 0, 0,          // leave: jmp the early return
				0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
			put_rel(s + kCallAt, 0xE8, g.stub + kCallAt, reinterpret_cast<uintptr_t>(&on_model));
			std::memcpy(s + kMovAt, g.saved, 5);
			put_rel(s + kGoOnAt, 0xE9, g.stub + kGoOnAt, g.go_on != 0 ? g.go_on : g.site + 5);
			put_rel(s + kLeaveAt, 0xE9, g.stub + kLeaveAt, g.leave != 0 ? g.leave : g.early_return);
			return write_code(g.stub, s, kStubSize);
		}
	}

	bool start(std::string &why)
	{
		if (g.in)
			return true;
		const uintptr_t m = find(kSite), ret = find(kReturn), lights = find(kLights), ambient = find(kAmbient), factor = find(kFactor), renderer = find(kRenderer),
						device = find(kDevice);
		if (m == 0 || ret == 0)
		{
			why = "the game's model draw wasn't found";
			return false;
		}
		if (lights == 0 || ambient == 0 || factor == 0 || renderer == 0 || device == 0)
		{
			why = "the characters' lights weren't found";
			return false;
		}
		const uintptr_t site = m + kSiteAt;
		uint8_t now[5] = {};
		if (!read(site, now, 5) || now[0] != 0xB9)
		{
			why = "the model draw's site isn't as expected (is /hifi gamedraw chars off? put it back on first)";
			return false;
		}
		g.site = site;
		g.early_return = ret;
		g.lights = word(lights + 8);
		g.ambient = word(ambient + 1);
		g.tfactor = word(factor + 7);
		g.renderer = word(renderer + 2);
		g.device = word(device + 2);
		std::memcpy(g.saved, now, 5);
		if (g.stub == 0)
		{
			g.stub = alloc_code(kStubSize);
			if (g.stub == 0)
			{
				why = "no memory for the stub";
				return false;
			}
		}
		if (!write_stub())
		{
			why = "couldn't write the stub";
			return false;
		}
		uint8_t ours[5];
		put_rel(ours, 0xE9, site, g.stub);
		if (!write_code(site, ours, 5))
		{
			why = "couldn't write the game's code";
			return false;
		}
		std::memcpy(g.ours, ours, 5);
		g.in = true;
		g.f = nullptr;
		g.n = counts();
		charslot::set_model_marks(true);
		return true;
	}

	bool stop(std::string &why)
	{
		if (!g.in)
			return true;
		if (!code_is(g.site, g.ours, 5))
		{
			why = "something else has changed the model draw's site since: left as it is";
			return false;
		}
		if (!write_code(g.site, g.saved, 5))
		{
			why = "couldn't write the game's code";
			return false;
		}
		charslot::set_model_marks(false);
		free_code(g.stub);
		const uintptr_t go_on = g.go_on, leave = g.leave;
		g = state();
		g.go_on = go_on;
		g.leave = leave;
		return true;
	}

	bool in()
	{
		return g.in;
	}

	void set_game_draws(bool on)
	{
		g.game_draws = on;
	}

	bool game_draws()
	{
		return g.game_draws;
	}

	void set_recording(bool on)
	{
		g.recording = on;
	}

	void set_native(bool on)
	{
		g.native = on;
	}

	void on_slot(uint32_t kind, uint32_t seq, uintptr_t)
	{
		if (!g.in)
			return;
		if (kind == 2)
		{
			g.f = &ch::begin_frame(seq);
			g.in_walk = true;
			read_camera(*g.f);
		}
		else if (kind == 3 && g.f != nullptr)
		{
			g.in_walk = false;
			ch::publish_opaque(*g.f);
			ch::cache::publish_palettes(*g.f, 0, g.f->bones);
			g.n.last_models = g.f->models;
			g.n.last_bones = g.f->bones;
		}
	}

	counts get_counts()
	{
		return g.n;
	}

	std::string status()
	{
		if (!g.in)
			return "character models: not recorded";
		char line[600];
		std::snprintf(line, sizeof(line),
			"character models: %llu recorded (%llu see-through; %llu left to HiFi XI's draw, %llu to the game's as their data wasn't up), the last frame %u models and %u bones; "
			"%llu new meshes, %llu new textures (%llu not found); "
			"%llu couldn't be read, %llu didn't fit, %llu drawn outside the world's passes%s",
			static_cast<unsigned long long>(g.n.models), static_cast<unsigned long long>(g.n.see_through), static_cast<unsigned long long>(g.n.native),
			static_cast<unsigned long long>(g.n.not_ready), g.n.last_models, g.n.last_bones,
			static_cast<unsigned long long>(g.n.new_meshes), static_cast<unsigned long long>(g.n.new_textures), static_cast<unsigned long long>(g.n.textures_missing),
			static_cast<unsigned long long>(g.n.faults), static_cast<unsigned long long>(g.n.dropped), static_cast<unsigned long long>(g.n.outside),
			g.game_draws ? "" : "; the game's own character drawing OFF");
		return line;
	}

	sites where()
	{
		return { g.site, g.early_return, g.in ? g.stub : 0, g.lights, g.ambient, g.tfactor, g.renderer, g.device };
	}

	void set_exits(uintptr_t go_on, uintptr_t leave)
	{
		g.go_on = go_on;
		g.leave = leave;
		if (g.in)
			write_stub();
	}
}
