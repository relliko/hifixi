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
#include <cmath>
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

		// Cloth (research/ffxi-re/notes-actor-extras.md 1). 0224D0, a cloth mesh's draw, at 02251C: the work buffer
		// aligned into its global (+9), the model's posed-once bit, the pose frame's flag (+0x1B), then the step 023290
		// called (+0x31) as (ecx the resource; the actor, the model, the instance, the buffer, the node)
		constexpr const char *kClothDraw = "8B464083C00F24F0A3????????8A5130F6C2100F84????????803D????????01751F8B54242C56508B44242C5251508BCFE8";
		constexpr intptr_t kClothBufferAt = 9, kClothFlagAt = 0x1B, kClothStepAt = 0x31, kClothDrawAt = 0x4C;
		// 023290's start (sub esp, 2Ch; push ebx, ebp, esi; mov esi, ecx; xor eax, eax; mov ebx, [the palette pointer]): the
		// pointer it skins with at +0xC ([0x48EEC20], the posed instance's palette, set by 034330)
		constexpr uint8_t kClothStep[12] = { 0x83, 0xEC, 0x2C, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x33, 0xC0, 0x8B, 0x1D };
		constexpr intptr_t kClothPaletteAt = 0xC;
		// 025030, the cloth node's draw (its vtable +0x1C), its call of 0224D0 at +0x26
		constexpr const char *kClothNode = "8B41088B54240C518B4C2414518B4C2410528B542410518B0D????????5250E8????????8BC8E8????????C21800";
		constexpr intptr_t kClothNodeCallAt = 0x26;
		// (023290 is thiscall with 5 arguments, ret 14h: as fastcall with edx unused)
		using step_fn = void(__fastcall *)(uintptr_t resource, uintptr_t edx, uintptr_t actor, uintptr_t model, uintptr_t instance, uintptr_t buffer, uintptr_t node);
		constexpr uint32_t kClothStride = 60; // (a work vertex: +0x0C position, +0x18 the last, +0x24 scratch, +0x30 normal)

		// The actor's extra passes (research/ffxi-re/notes-actor-extras.md 2), as 0D6D30 sets them up before 02B6E0 and
		// 02B6E0 reads them. 02B955: the overlay's flag ([0x48E70FC], +2); 02B9E8: its v offset ([0x48E70F8], +2), u
		// offset ([0x48E7100], +7), the texture's handle ([0x48E7268], +0xD), its TEXTUREFACTOR ([0x48E74B0], +0x14);
		// 02BA17: the cloak's flag ([0x48E72B0], +2), PROJECTION ([0x48E7468], +0xD), its offsets ([0x48E726C] +0x1D,
		// [0x48E7458] +0x22: both actor +0x744); 02BAEB: its TEXTUREFACTOR ([0x48E74B4], +0xF)
		constexpr const char *kOverlayFlag = "381D????????0F84????????B9????????E8????????E8????????50B9";
		constexpr const char *kOverlaySetup = "8B15????????A1????????8B0D????????528B15????????5051528D8D8C000000E8";
		constexpr const char *kCloakSetup = "381D????????0F84????????68????????B9????????E8????????8B0D????????A1????????";
		constexpr const char *kCloakFactor = "8B15????????8B0D????????528B15????????5150528D8D90000000E8";

		constexpr uint32_t kColourAt = 0xA8; // (argument 7 at the site's esp)
		constexpr uint32_t kMaxInstancesPerModel = 8, kMaxMeshesPerInstance = 32, kMaxBonesPerInstance = 255;
		constexpr uint32_t kMaxClothPerModel = 8, kMaxClothVertices = 1024;
		constexpr size_t kStubSize = 0x30;
		constexpr size_t kCallAt = 0x08, kMovAt = 0x15, kGoOnAt = 0x1A, kLeaveAt = 0x1F;

		struct raw_instance
		{
			uint32_t bones = 0, first_bone = 0, meshes = 0;
			uintptr_t self = 0; // (the instance)
			uint32_t mesh[kMaxMeshesPerInstance];
			ch::cache::key key[kMaxMeshesPerInstance];
			uintptr_t node[kMaxMeshesPerInstance];   // (each mesh's node)
			uint32_t cloth[kMaxMeshesPerInstance];   // a cloth mesh's vertex count (a node the game made for it), else 0
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
			// The cloth's (0: not found, a model with cloth is the game's): 023290, the cloth node's draw 025030, the
			// globals 0224D0 and the posing leave for it (the aligned buffer, the pose frame's flag, the palette pointer)
			uintptr_t cloth_step = 0, cloth_node = 0, cloth_buffer = 0, cloth_flag = 0, cloth_palette = 0;
			// The extra passes' globals (0: not found, a model with one of them is the game's)
			uintptr_t overlay_flag = 0, overlay_u = 0, overlay_v = 0, overlay_handle = 0, overlay_colour = 0;
			uintptr_t cloak_flag = 0, cloak_projection = 0, cloak_a = 0, cloak_b = 0, cloak_factor = 0;
			uintptr_t go_on = 0, leave = 0; // (the tests' exits; 0: the game's)
			uint8_t saved[5] = {}, ours[5] = {};
			bool in = false;
			bool game_draws = true, recording = true, native = false;
			uintptr_t enspell_actor = 0; // (set_enspell's)
			uint8_t enspell = ch::kEnNone;
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

		// A cloth mesh's vertex count, or kBadCloth when this one can't be drawn here (the cloth's sites not found, the
		// node not the game's cloth node, its work buffer not as big as the mesh). The node (029F70: 0x1C8 bytes,
		// vtable 0x32A430): +0x3C the reset flag, +0x40 the work buffer (60 bytes a vertex, aligned to 16 when used), +0x44
		// the vertices + 1. The live resource's counts: section 2, a pointer at data + 0x12. Inside gather's __try.
		constexpr uint32_t kBadCloth = 0x80000000u;
		uint32_t cloth_vertices(uintptr_t node, uintptr_t res)
		{
			if (g.cloth_step == 0)
				return kBadCloth;
			const uintptr_t vtable = *reinterpret_cast<const uint32_t *>(node);
			if (vtable < 0x10000 || *reinterpret_cast<const uint32_t *>(vtable + 0x1C) != g.cloth_node)
				return kBadCloth;
			const uintptr_t counts = *reinterpret_cast<const uint32_t *>(res + 0x30 + 0x12);
			if (counts < 0x10000)
				return kBadCloth;
			const uint32_t n = uint32_t(*reinterpret_cast<const uint16_t *>(counts)) + *reinterpret_cast<const uint16_t *>(counts + 2);
			if (n == 0 || n > kMaxClothVertices || *reinterpret_cast<const uint32_t *>(node + 0x44) != n + 1 || *reinterpret_cast<const uint32_t *>(node + 0x40) < 0x10000)
				return kBadCloth;
			return n;
		}

		// The game's own cloth step for one mesh, as 0224D0 runs it on a pose frame ([pose flag] = 1, the model posed
		// once: model +0x30 bit 4): the aligned work buffer into its global, the palette pointer the instance's
		// (023290 skins the pinned vertices with it; the posing left it at the last instance posed: put back after),
		// then 023290 (pinned skinned, the free ones moved with the model, damped, gravity, the environment's wind, the
		// constraints 4 times, the bone spheres, the normals from the triangles), then the reset flag cleared. The
		// game draws a model it leaves through the early return no more, so the step is ours to run, once a pose frame
		// as its draw would. False when it faulted.
		bool step_cloth(uintptr_t res, uintptr_t actor, uintptr_t model, uintptr_t instance, uintptr_t node)
		{
			uint32_t *const palette = reinterpret_cast<uint32_t *>(g.cloth_palette);
			uint32_t was = 0;
			bool set = false;
			__try
			{
				const uintptr_t buffer = (*reinterpret_cast<const uint32_t *>(node + 0x40) + 15u) & ~uintptr_t(15);
				was = *palette;
				*palette = *reinterpret_cast<const uint32_t *>(instance + 0x14);
				set = true;
				*reinterpret_cast<uint32_t *>(g.cloth_buffer) = uint32_t(buffer);
				reinterpret_cast<step_fn>(g.cloth_step)(res, 0, actor, model, instance, buffer, node);
				*palette = was;
				if (*reinterpret_cast<const uint8_t *>(node + 0x3C) != 0)
					*reinterpret_cast<uint8_t *>(node + 0x3C) = 0;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				if (set)
					*palette = was;
				return false;
			}
		}

		// A cloth mesh's work vertices as palette rows (os2.hpp mesh::cloth): row 0 the normal, the translation the
		// position, the rest 0. False when it faulted, or the step never ran on it (the reset flag still set: the
		// buffer holds nothing yet).
		bool cloth_rows(uintptr_t node, uint32_t n, ch::bone *out)
		{
			__try
			{
				if (*reinterpret_cast<const uint8_t *>(node + 0x3C) != 0)
					return false;
				const float *w = reinterpret_cast<const float *>((*reinterpret_cast<const uint32_t *>(node + 0x40) + 15u) & ~uintptr_t(15));
				for (uint32_t i = 0; i < n; ++i, w += kClothStride / 4)
				{
					float *m = out[i].m;
					m[0] = w[12], m[1] = w[13], m[2] = w[14];
					m[3] = m[4] = m[5] = m[6] = m[7] = m[8] = 0.0f;
					m[9] = w[3], m[10] = w[4], m[11] = w[5];
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Whether this is a pose frame for the actor being drawn ([0x48E7460]: 1 set per actor at 0CBEBF, 0 by the pose
		// throttle) and the model was posed once (model +0x30 bit 4: 0224D0 draws nothing of a cloth before)
		bool pose_frame(uintptr_t model, bool &posed)
		{
			__try
			{
				posed = (*reinterpret_cast<const uint8_t *>(model + 0x30) & 0x10) != 0;
				return *reinterpret_cast<const uint8_t *>(g.cloth_flag) == 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				posed = false;
				return false;
			}
		}

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
					ri.self = node;
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
								ri.node[ri.meshes] = mesh;
								ri.cloth[ri.meshes] = (*reinterpret_cast<const uint8_t *>(res + 0x32) & 0x7F) == 1 ? cloth_vertices(mesh, res) : 0;
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
			// (its chunk's name, from the resource's header copy at +0x20: a weapon's slot)
			const uint32_t name_word = word(k.resource + 0x20);
			char name[4];
			std::memcpy(name, &name_word, 4);
			return ch::cache::add_mesh(k, std::move(bytes), base, std::move(textures), name);
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
			// (PROJECTION as the character pass keeps it: the cloak's)
			if (g.cloak_projection != 0 && read(g.cloak_projection, f.projection, 64))
			{
				bool ok = true;
				for (float x : f.projection)
					ok = ok && std::isfinite(x);
				f.projection_read = ok;
			}
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

		// The actor's extra passes as 0D6D30 set them up for this draw (model_entry::passes): the overlay with its
		// texture (handle [0x48E7268] -> its kind-10 reference -> +0x30 the texture object -> +8 its resource, 025C85 /
		// 075960), offsets and factor; the cloak with its offsets and factor. A pass that adds nothing isn't kept (the
		// overlay's factor alpha 0, its default till ops 46/47 raise it; the cloak's on an opaque actor: 0x80 less 0x80).
		// False when one that adds something can't be drawn here (its texture not there or not on the GPU yet,
		// PROJECTION not read, a global that can't be read): the model is then the game's. (With the globals not found
		// at all, as on a client this doesn't know, none is seen: the models are drawn without them, as before.)
		bool read_passes(ch::model_entry &m, const ch::frame &f)
		{
			m.passes = 0;
			if (g.overlay_flag == 0)
				return true;
			uint8_t on = 0;
			if (!read(g.overlay_flag, on))
				return false;
			if (on != 0)
			{
				const uint32_t colour = word(g.overlay_colour), handle = word(g.overlay_handle);
				// (no handle: 025C40 draws no overlay at all)
				if (handle != 0 && (colour >> 24) != 0)
				{
					const uint32_t ref = pointer(handle) ? word(handle) : 0;
					const uint32_t object = pointer(ref) ? word(ref + 0x30) : 0;
					const uint32_t res = pointer(object) ? word(object + 8) : 0;
					const uint32_t id = pointer(res) ? texture_id(res) : ch::cache::kNone;
					D3D12_GPU_DESCRIPTOR_HANDLE h;
					if (id == ch::cache::kNone || !ch::cache::texture_view_of(id, h))
						return false;
					float u = 0.0f, v = 0.0f;
					if (!float_of(word(g.overlay_u), u) || !float_of(word(g.overlay_v), v))
						return false;
					m.passes |= ch::kPassOverlay;
					m.overlay_texture = id;
					m.overlay_offset[0] = u;
					m.overlay_offset[1] = v;
					m.overlay_colour = colour;
				}
			}
			if (!read(g.cloak_flag, on))
				return false;
			if (on != 0)
			{
				const uint32_t factor = word(g.cloak_factor);
				if ((factor >> 24) != 0)
				{
					float a = 0.0f, b = 0.0f;
					if (!f.projection_read || !float_of(word(g.cloak_a), a) || !float_of(word(g.cloak_b), b))
						return false;
					m.passes |= ch::kPassCloak;
					m.cloak_offset[0] = a;
					m.cloak_offset[1] = b;
					m.cloak_factor = factor;
				}
			}
			return true;
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
			if (f->models >= ch::kMaxModels || f->instances + kMaxInstancesPerModel + kMaxClothPerModel > ch::kMaxInstances ||
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
			m.enspell = actor != 0 && actor == g.enspell_actor ? g.enspell : ch::kEnNone;
			// (each cloth mesh is drawn as an instance of its own after the skeletons', its vertices its rows: os2.hpp
			// mesh::cloth)
			struct cloth_ref
			{
				uint32_t instance, k, id;
			};
			cloth_ref cloth[kMaxClothPerModel];
			uint32_t cloths = 0, cloth_bones = 0;
			bool cloth_ok = true;
			for (uint32_t i = 0; i < r.instances; ++i)
			{
				const raw_instance &ri = r.instance[i];
				ch::instance_entry &ie = f->instance[f->instances + i];
				ie.first_bone = ri.first_bone;
				ie.bones = ri.bones;
				ie.first_mesh = f->meshes;
				ie.meshes = 0;
				for (uint32_t k = 0; k < ri.meshes; ++k)
				{
					const uint32_t id = mesh_id(ri.key[k]);
					if (id != ch::cache::kNone)
						m.hidden |= ch::os2::hidden_by(ch::cache::mesh_hides(id));
					if (ri.cloth[k] != 0)
					{
						if (ri.cloth[k] == kBadCloth || cloths == kMaxClothPerModel)
							cloth_ok = false;
						else
						{
							cloth[cloths++] = { i, k, id };
							cloth_bones += ri.cloth[k];
						}
						continue;
					}
					f->mesh[f->meshes++].mesh = id;
					ie.meshes++;
				}
			}
			// HiFi XI draws it when it can: its meshes and their textures all on the GPU (and something to draw); its
			// cloth stepped as the game's draw would have (its vertices drawn from the game's own work buffer)
			bool posed = false;
			const bool pose = cloths != 0 && pose_frame(model, posed);
			// (a cloak that adds something on a model of the opaque walk: drawn at the walk's end in one batch here, with
			// no scene copy of its own: the game's)
			const bool passes_ok = read_passes(m, *f) && ((m.passes & ch::kPassCloak) == 0 || flush);
			if (m.passes & ch::kPassOverlay)
				g.n.overlays++;
			if (m.passes & ch::kPassCloak)
				g.n.cloaks++;
			if (g.native && f->camera && (r.colour >> 24) != 0)
			{
				bool ready = r.instances != 0;
				if (!passes_ok)
				{
					if (ready)
						g.n.passes_refused++;
					ready = false;
				}
				for (uint32_t i = 0; i < r.instances && ready; ++i)
				{
					const ch::instance_entry &ie = f->instance[f->instances + i];
					for (uint32_t k = 0; k < ie.meshes && ready; ++k)
						ready = ch::cache::mesh_ready(f->mesh[ie.first_mesh + k].mesh);
				}
				for (uint32_t c = 0; c < cloths && ready; ++c)
					ready = ch::cache::mesh_ready(cloth[c].id);
				if (!cloth_ok || f->bones + cloth_bones > ch::kMaxBones)
				{
					if (ready)
						g.n.cloth_refused++;
					ready = false;
				}
				for (uint32_t c = 0; c < cloths && ready && pose && posed; ++c)
				{
					const raw_instance &ri = r.instance[cloth[c].instance];
					const uint32_t k = cloth[c].k;
					if (step_cloth(ri.key[k].resource, actor, model, ri.self, ri.node[k]))
						g.n.cloth_steps++;
					else
					{
						// (left to the game, which steps it again in its own draw)
						g.n.cloth_faults++;
						ready = false;
					}
				}
				if (ready)
				{
					m.flags |= ch::kNative;
					leave = 1;
					g.n.native++;
					g.n.cloth += cloths;
				}
				else
					g.n.not_ready++;
			}
			for (uint32_t c = 0; c < cloths; ++c)
			{
				const raw_instance &ri = r.instance[cloth[c].instance];
				const uint32_t k = cloth[c].k, n = ri.cloth[k];
				ch::instance_entry &ce = f->instance[f->instances + r.instances + c];
				ce.first_mesh = f->meshes;
				ce.meshes = 1;
				f->mesh[f->meshes++].mesh = cloth[c].id;
				ce.first_bone = f->bones;
				ce.bones = 0;
				// (none before the model was posed once: 0224D0 draws nothing of it then; none for a model the game draws)
				if ((m.flags & ch::kNative) != 0 && posed && f->bones + n <= ch::kMaxBones && cloth_rows(ri.node[k], n, &f->palette[f->bones]))
				{
					ce.bones = n;
					f->bones += n;
				}
			}
			m.instances = r.instances + cloths;
			f->instances += r.instances + cloths;
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

		// The cloth's sites, each checked against the others (0224D0's call of 023290, 025030's call of 0224D0); left 0
		// when any isn't as expected: models with cloth are then the game's, as before
		// The extra passes' globals, each read out of 02B6E0's code (left 0 when any isn't found)
		void find_passes()
		{
			g.overlay_flag = g.overlay_u = g.overlay_v = g.overlay_handle = g.overlay_colour = 0;
			g.cloak_flag = g.cloak_projection = g.cloak_a = g.cloak_b = g.cloak_factor = 0;
			const uintptr_t flag = find(kOverlayFlag), setup = find(kOverlaySetup), cloak = find(kCloakSetup), factor = find(kCloakFactor);
			if (flag == 0 || setup == 0 || cloak == 0 || factor == 0)
				return;
			g.overlay_flag = word(flag + 2);
			g.overlay_v = word(setup + 2);
			g.overlay_u = word(setup + 7);
			g.overlay_handle = word(setup + 0xD);
			g.overlay_colour = word(setup + 0x14);
			g.cloak_flag = word(cloak + 2);
			g.cloak_projection = word(cloak + 0xD);
			g.cloak_b = word(cloak + 0x1D);
			g.cloak_a = word(cloak + 0x22);
			g.cloak_factor = word(factor + 0xF);
		}

		void find_cloth()
		{
			g.cloth_step = g.cloth_node = g.cloth_buffer = g.cloth_flag = g.cloth_palette = 0;
			const uintptr_t at = find(kClothDraw), node = find(kClothNode);
			if (at == 0 || node == 0)
				return;
			const uintptr_t step = rel_target(at + kClothStepAt);
			if (rel_target(node + kClothNodeCallAt) != at - kClothDrawAt || !code_is(step, kClothStep, sizeof(kClothStep)))
				return;
			g.cloth_step = step;
			g.cloth_node = node;
			g.cloth_buffer = word(at + kClothBufferAt);
			g.cloth_flag = word(at + kClothFlagAt);
			g.cloth_palette = word(step + kClothPaletteAt);
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
		find_cloth();
		find_passes();
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

	void set_enspell(uintptr_t actor, uint8_t element)
	{
		g.enspell_actor = element != ch::kEnNone ? actor : 0;
		g.enspell = element;
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
		char line[1000];
		std::snprintf(line, sizeof(line),
			"character models: %llu recorded (%llu see-through; %llu left to HiFi XI's draw, %llu to the game's as their data wasn't up), the last frame %u models and %u bones; "
			"%llu new meshes, %llu new textures (%llu not found); "
			"%llu couldn't be read, %llu didn't fit, %llu drawn outside the world's passes; "
			"cloth: %llu meshes in HiFi XI's models, %llu steps, %llu faulted, %llu models left to the game for theirs%s; "
			"extra passes: %llu overlays, %llu cloaks seen, %llu models left to the game for theirs%s%s",
			static_cast<unsigned long long>(g.n.models), static_cast<unsigned long long>(g.n.see_through), static_cast<unsigned long long>(g.n.native),
			static_cast<unsigned long long>(g.n.not_ready), g.n.last_models, g.n.last_bones,
			static_cast<unsigned long long>(g.n.new_meshes), static_cast<unsigned long long>(g.n.new_textures), static_cast<unsigned long long>(g.n.textures_missing),
			static_cast<unsigned long long>(g.n.faults), static_cast<unsigned long long>(g.n.dropped), static_cast<unsigned long long>(g.n.outside),
			static_cast<unsigned long long>(g.n.cloth), static_cast<unsigned long long>(g.n.cloth_steps), static_cast<unsigned long long>(g.n.cloth_faults),
			static_cast<unsigned long long>(g.n.cloth_refused), g.cloth_step != 0 ? "" : " (its sites not found: the game's)",
			static_cast<unsigned long long>(g.n.overlays), static_cast<unsigned long long>(g.n.cloaks), static_cast<unsigned long long>(g.n.passes_refused),
			g.overlay_flag != 0 ? "" : " (their globals not found: not drawn)",
			g.game_draws ? "" : "; the game's own character drawing OFF");
		return line;
	}

	sites where()
	{
		return { g.site, g.early_return, g.in ? g.stub : 0, g.lights, g.ambient, g.tfactor, g.renderer, g.device,
			g.cloth_step, g.cloth_node, g.cloth_buffer, g.cloth_flag, g.cloth_palette,
			g.overlay_flag, g.overlay_u, g.overlay_v, g.overlay_handle, g.overlay_colour, g.cloak_flag, g.cloak_projection, g.cloak_a, g.cloak_b, g.cloak_factor };
	}

	void set_exits(uintptr_t go_on, uintptr_t leave)
	{
		g.go_on = go_on;
		g.leave = leave;
		if (g.in)
			write_stub();
	}
}
