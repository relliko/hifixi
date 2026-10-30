/*
 * The effect elements handed to the native effect renderer (fxdraw.hpp; research/ffxi-re/notes-effects-draw.md).
 *
 * The flush (task priority 12): 055620: call 046380; mov ecx, [list]; call 055520 (at +0xB); mov ecx, [list]; jmp 0554B0.
 * The call's rel32 is pointed at a stub of ours (ecx, the list, kept for the walk):
 *   push ecx; push 0; call on_flush; add esp, 4; pop ecx; call 055520; push 1; call on_flush; add esp, 4; ret
 * The walk 055520 calls each queued item's callback with its argument:
 *   055533: mov eax, [esi+0Ch]; mov edi, [esi+4]; push eax; call [esi+8]; add esp, 4 (at +6, 7 bytes)
 * Those 7 bytes become call entry; nop; nop, the stub (esi the node, eax the argument; esi and edi kept by on_entry):
 *   push eax; push [esi+8]; call on_entry; add esp, 8; ret
 * An element's callback is 0555A0 (mov ecx, [esp+4]; mov eax, [ecx]; jmp [eax+2Ch]), its argument the element.
 *
 * The element draw 046070 (vt+0x2C of most classes): its colour (03E330 -> VA 0490BC0C), the hidden check (04609D,
 * gamerain's), BLENDOP ADD, vt+0x4C, then at 04612E: mov ecx, ebp; call 0467E0 (thiscall, the word 0xFFFF on the
 * stack, ret 4); test al, al; jne 046358 (past the depth pull, the class draw and 0472F0's restore). That call goes to
 * on_setup (a __fastcall with the same stack), which answers 1 for an element it recorded.
 *
 * What's recorded for an element (046070, 0467E0, the class draw), read straight from the game's memory (no
 * ReadProcessMemory per field: a busy flush has hundreds of elements) under a structured exception guard (an element whose memory faults is left to the game, nothing of it kept):
 *   - WORLD: +0xA0 after 045D40 (called here as 0467E0 calls it: the draw matrix is built only in the setup path);
 *   - TEXTUREFACTOR: VA 0490BC0C (046070's colour, alpha x fade x brightness) with 0467E0's letter changes (d, h: alpha
 *     = min(255, the letter's high byte x 2); b: the high byte) and the class's (the D3M's D: alpha >= 0x7F -> 0xFF;
 *     I: | 0xFFFFFF);
 *   - the blend by the letter (+0x16C), z write flags bit 12, PROJECTION: the depth pull (not flags bit 27, not a sky
 *     layer bit 22, +0x128 not 0, within 32 of the eye: near and far both less (1 - |z|/32)^3 x 0.03 x +0x128) or
 *     screen-fixed (bit 0, bits 6-8 clear, bits 2 and 3: field of view 2 atan2(192, 350)), each with the renderer's
 *     near/far shift (+0x39594 when +0x39598) as 046070 adds it;
 *   - fog unless flags bit 25: 18BA20 on the environment [VA 04AECB04] for the generator's area ([[+0xFC]]+0xC4) as
 *     0467E0 asks it (colour, end, start); table fog (wrapper +0x92D) by the view depth, else vertex fog (+0x92E) by
 *     the depth or the distance (RANGEFOG, +0x92F); H: the fog colour black;
 *   - TEXTURE0's offset (+0x120, +0x124) when +0x17B bits 0-1;
 *   - the class's geometry and stages.
 *
 * Classes (by the vtable's class draw, vt+0x48):
 *   040BA0 (the D3M element, vtable 0x32AD80): no model (+0x104) or a weather element (generator +0xDE bit 7) that
 *     002D80 hides: nothing drawn (taken, as the game's own early return). The model an MMB (+0x179): 040C20 ->
 *     183760 (record_mmb below: the zone's own model, drawn from the native zone renderer's data). Else the D3M
 *     (040220, the resource [[+0x104]], data at +0x30): its vertices as the load left them
 *     (03FEE0 doubled their colours in place), no vertex buffer (03FCF0 + 4): nothing; no textures: one untextured
 *     draw (352348, D: alpha x 2); for kinds 5-7 without textures the first vertex's y comes from the DAT (their names
 *     begin where their vertices do, and the load stores the vertex buffer object over it once it has copied them:
 *     dat_first_y below; left to the game when it can't be found); else a draw per texture run (3524A8, bit 28: 3523D0; I: 352590), the texture object
 *     in each name's first dword (its resource at +8), and the untextured tail (352648) when +5.
 *   03EB30 (CMoD3aElem): the frame +0x170 of the D3A (record_d3a below), lit or not (03E9A0 sets LIGHTING 0).
 *   0565A0 (the rings, kind 0x24): record_ring below.
 *   03FBB0 (the morphs, chunk 0x25): record_morph below (03F440 with its weights 03F8A0).
 * Lit elements (flags bit 16; D3M, MMB meshes and rings: the D3A sets LIGHTING 0 itself) get their lights as 0467E0
 * works them out (record_light below), the game's own functions called for each step.
 *   Anything else: left to the game.
 * Weapon trails (CYyAfterImage) aren't elements: their flush callback 01C930 is recognised in the walk's stub and the
 * ring of samples recorded as one more element of the run (record_trail below).
 */

#include <windows.h>
#include "fxdraw.hpp"
#include "gamewater.hpp"
#include "slotmark.hpp"
#include "../fx/fxframe.hpp"
#include "../fx/fxtextures.hpp"
#include "../bake/dat.hpp"
#include "../zone/zone_gpu.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace hifi::game::fxdraw
{
	namespace
	{
		namespace fx = hifi::fx;
		namespace zm = hifi::bake::zonemesh;

		// 04610E: mov eax, [math]; push 0FFFFh; ... mov ecx, ebp; call 0467E0 (at +0x20)
		constexpr const char *kSetup = "A1????????68FFFF00008B88900A00008B9C88100A0000418988900A00008BCDE8";
		constexpr intptr_t kSetupCallAt = 0x20, kSetupMath = 1, kSetupGfx = 0x3B, kSetupMatrixCall = 0x5A, kSetupCamera = 0x8C, kSetupRenderer = 0x91;
		constexpr intptr_t kElementDrawFromSetup = 0x9E; // (046070 = 04610E - 0x9E)
		constexpr const char *kElementDraw = "83EC508D44240C558BE951";
		// 055620: call 046380; mov ecx, [list]; call 055520 (at +0xB); mov ecx, [list]; jmp 0554B0
		constexpr const char *kFlush = "E8????????8B0D????????E8????????8B0D????????E975FEFFFF";
		constexpr intptr_t kFlushCallAt = 0xB;
		constexpr const char *kWalk = "8B41045685C074268B7108C1E60503F0"; // 055520
		constexpr const char *kEntry = "8B460C8B7E0450FF560883C404C7461400000000"; // 055533, the 7 bytes at +6
		constexpr intptr_t kEntryAt = 6;
		constexpr uint8_t kEntryBytes[7] = { 0x50, 0xFF, 0x56, 0x08, 0x83, 0xC4, 0x04 };
		constexpr const char *kElementCallback = "8B4C24048B01FF602C"; // 0555A0
		constexpr const char *kD3mDraw = "568BF18B860401000085C074688B86FC000000"; // 040BA0: push colour at +0x58, call 002D80 at +0x27
		constexpr const char *kD3aDraw = "8B810401000085C0741E85C074048B00EB0233C0518B0D????????68????????518BC8E8"; // 03EB30
		constexpr const char *kTrailCallback = "8B4C240485C97405E903000000C3"; // 01C930 (-> 01C940)
		constexpr const char *kRingDraw = "83EC40535556578BF98A87B401000084C0"; // 0565A0
		constexpr const char *kMorphDraw = "538BD98B83FC00000085C074048B00EB0233C08A88DE000000"; // 03FBB0
		// 03F130 (CMoVuAnmElem): movsx esi, byte [ecx + 352218] (+0xCB), call 0473F0 (+0x1DF), mov [VA 0490BF34], edi
		// (+0x219), push 03EFC0 (+0x259), mov [VA 0490B9D4], eax (+0x264), call 063C80 (+0x269)
		constexpr const char *kVuAnmDraw = "83EC1053558BE933DB8B85040100003BC30F84????????568B308A85DC010000";
		constexpr intptr_t kVuTable = 0xCB, kVuAttachCall = 0x1DF, kVuAttachStore = 0x219, kVuCallback = 0x259, kVuCursorStore = 0x264, kVuWalkCall = 0x269;
		constexpr uint8_t kVuWalk[] = { 0x81, 0xEC, 0x4C, 0x0A, 0x00, 0x00, 0x53, 0x8B, 0xD9, 0x66, 0x8B, 0x43, 0x44 }; // 063C80's first bytes
		constexpr uint32_t kMaxVuPoints = 2048;
		constexpr intptr_t kD3mColour = 0x58, kD3mWeatherCall = 0x27;
		constexpr intptr_t kD3mMmbJump = 0x46; // (040BE6: jmp 040C20)
		constexpr uint8_t kMmbDraw[] = { 0x57, 0x8B, 0xF9, 0xF6, 0x87, 0x7B, 0x01, 0x00, 0x00, 0x03 }; // 040C20's first bytes
		// 047018: mov ecx, [env]; push 0; push fog start; push fog end; push fog colour; call 18BA20 (at +0x17); call 0464D0
		constexpr const char *kFog = "8B0D????????6A0068????????68????????68????????E8????????E8";
		constexpr intptr_t kFogEnv = 2, kFogCall = 0x17;
		// The client's IDirect3DDevice8 (Ashita's wrapper object): [renderer + 8] (as zoneslot and charslot find it)
		constexpr const char *kDevicePat = "8BC8E8????????85C0A3????????0F84????????8B5C241C";

		constexpr size_t kCodeSize = 0x40, kEntryStubAt = 0x20;
		constexpr size_t kFlushMark1 = 0x03, kFlushWalk = 0x0C, kFlushMark2 = 0x13, kEntryCall = 0x24;

		constexpr uint32_t kRelease = 2, kGetDepthStencilSurface = 33, kClear = 36, kSurfaceGetDesc = 8;
		constexpr uint32_t kClearStencil = 4; // D3DCLEAR_STENCIL
		using release_fn = unsigned long(__stdcall *)(uintptr_t self);
		using get_surface_fn = long(__stdcall *)(uintptr_t self, uintptr_t *out);
		using get_desc_fn = long(__stdcall *)(uintptr_t self, uint32_t desc[8]);
		using clear_fn = long(__stdcall *)(uintptr_t self, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);

		using setup_fn = uint32_t(__fastcall *)(uintptr_t element, uintptr_t edx, uint32_t arg);
		using callback_fn = uint32_t(__cdecl *)(uintptr_t arg);
		using matrix_fn = void(__fastcall *)(uintptr_t element, uintptr_t edx);
		using weather_fn = float(__cdecl *)();
		using fog_fn = void(__fastcall *)(uintptr_t env, uintptr_t edx, uint32_t *colour, float *end, float *start, uint32_t none, uint32_t area);
		// The lighting's (0467E0 046A72-046D9E, 04719C): the element's frame matrix and attach matrix, the attach fix-up,
		// the math object's turn (vt+0x78), products (vt+0x20, vt+0x24) and point transform (vt+0x50), the environment's
		// one light, two lights (with an area, without) and ambient
		using matrix_of_fn = uintptr_t(__fastcall *)(uintptr_t element, uintptr_t edx);
		using fix_fn = void(__cdecl *)(float *m);
		using turn_fn = void(__fastcall *)(uintptr_t math, uintptr_t edx, float *out, float *in, float angle);
		using product_fn = void(__fastcall *)(uintptr_t math, uintptr_t edx, float *out, float *a, float *b);
		using point_fn = void(__fastcall *)(uintptr_t math, uintptr_t edx, float *out, float *m, float *in);
		using light_fn = void(__fastcall *)(uintptr_t env, uintptr_t edx, void *light, uint32_t zero, uint32_t area);
		using lights_fn = void(__fastcall *)(uintptr_t env, uintptr_t edx, void *light0, void *light1, uint32_t area);
		using lights_default_fn = void(__fastcall *)(uintptr_t env, uintptr_t edx, void *light0, void *light1);
		using ambient_fn = void(__fastcall *)(uintptr_t env, uintptr_t edx, uint32_t *colour, uint32_t zero, uint32_t area);
		// Their calls in 0467E0 (from its start), and the material's push (0471C1: push VA 0490BC28)
		constexpr intptr_t kFrameOfCall = 0x294, kAttachCall = 0x2F0, kFixCall = 0x339, kLightCall = 0x474, kLightsCall = 0x510, kLightsDefaultCall = 0x52A,
			kAmbientCall = 0x9BC, kMaterialPush = 0x9E1;
		constexpr uint32_t kTurn = 0x78, kProductA = 0x20, kProductB = 0x24, kPoint = 0x50;
		constexpr uint32_t kThreeHalfPi = 0x4096CBE2, kPi = 0x40490FD8; // (the angles 0467E0 pushes)

		// Element fields (notes-effects-update.md "Fields the frame touches")
		constexpr uintptr_t kWorld = 0xA0, kGen = 0xFC, kModel = 0x104, kFlags = 0x10C, kUv = 0x120, kBias = 0x128, kLetter = 0x16C, kMmb = 0x179, kUvBits = 0x17B;

		struct state
		{
			// The sites
			uintptr_t setup_call = 0, setup = 0, flush_call = 0, walk = 0, entry_site = 0, element_cb = 0, element_draw = 0, d3m_draw = 0;
			uintptr_t draw_matrix = 0, weather = 0, fog_env = 0, d3a_draw = 0, trail_cb = 0, ring_draw = 0, mmb_draw = 0, morph_draw = 0;
			uintptr_t colour = 0, gfx = 0, renderer = 0, camera = 0, env = 0, math = 0;
			uintptr_t frame_of = 0, attach = 0, fix = 0, light1 = 0, lights2 = 0, lights2_default = 0, ambient = 0, material = 0; // (0 any: lit elements the game's)
			uintptr_t vuanm_draw = 0, vu_walk = 0, vu_callback = 0, vu_table = 0, vu_attach = 0, vu_cursor = 0; // (03F130's; 0 any: VuAnm the game's)
			uintptr_t code = 0;
			uint8_t saved_setup[5] = {}, ours_setup[5] = {}, saved_flush[5] = {}, ours_flush[5] = {}, ours_entry[7] = {};
			bool in = false;
			// The plugin's say
			volatile bool native = false, wanted = false, probe = false;
			double clock = 0.0;
			// The marker device
			uintptr_t renderer_ptr = 0, device_override = 0;
			void (*sink)(uint32_t, uint32_t) = nullptr;
			// The flush under way
			bool in_flush = false, taking = false, can_mark = false;
			uint32_t seq = 0;
			fx::frame *f = nullptr;
			uint32_t run_first = 0;
			counts n;
		};
		state g;

		// The native zone renderer's data of the map slots (set each present), and each live MMB mesh's model in it
		// (kNone: not there, or not as the game has it), for the data it was found in
		struct mesh_model
		{
			const zone::gpu_zone *zone = nullptr;
			uint32_t model = zm::kNone;
		};
		struct zones_state
		{
			zone::gpu_zone *data[2] = {};
			std::unordered_map<uintptr_t, mesh_model> of;
		};
		zones_state g_zones;

		template <typename T>
		T at(uintptr_t a)
		{
			T v;
			std::memcpy(&v, reinterpret_cast<const void *>(a), sizeof(T));
			return v;
		}

		uintptr_t client_device()
		{
			if (g.device_override != 0)
				return g.device_override;
			if (g.renderer_ptr == 0)
			{
				const uintptr_t p = find(kDevicePat);
				if (p == 0)
					return 0;
				g.renderer_ptr = word(p + 10);
			}
			const uint32_t r = word(g.renderer_ptr);
			return r != 0 ? word(r + 8) : 0;
		}

		template <typename F>
		F method(uintptr_t object, uint32_t index)
		{
			return reinterpret_cast<F>(uintptr_t(*reinterpret_cast<const uint32_t *>(*reinterpret_cast<const uint32_t *>(object) + 4 * index)));
		}

		bool depth_has_stencil(uintptr_t dev)
		{
			uintptr_t surface = 0;
			if (method<get_surface_fn>(dev, kGetDepthStencilSurface)(dev, &surface) < 0 || surface == 0)
				return false;
			uint32_t desc[8] = {};
			const bool got = method<get_desc_fn>(surface, kSurfaceGetDesc)(surface, desc) >= 0;
			method<release_fn>(surface, kRelease)(surface);
			return got && (desc[0] == 73 || desc[0] == 75 || desc[0] == 79); // D15S1, D24S8, D24X4S4
		}

		// Whether this flush's markers can go out (a sink, or the device with a stencil on the bound depth)
		bool markers_possible()
		{
			if (g.sink != nullptr)
				return true;
			const uintptr_t dev = client_device();
			return dev != 0 && depth_has_stencil(dev);
		}

		void send_marker()
		{
			if (g.sink != nullptr)
			{
				g.sink(slotmark::fx_run, g.seq);
				g.n.markers++;
				return;
			}
			const uintptr_t dev = client_device();
			if (dev == 0)
			{
				g.n.not_sent++;
				return;
			}
			const int32_t x = int32_t(slotmark::x_of(slotmark::fx_run, g.seq));
			const int32_t rect[4] = { x, 0, x + 1, 1 };
			if (method<clear_fn>(dev, kClear)(dev, 1, rect, kClearStencil, 0, 1.0f, 0) >= 0)
				g.n.markers++;
			else
				g.n.not_sent++;
		}

		// The run of elements recorded since the last one published, and its marker (an empty run too, when asked)
		void close_run(bool even_empty = false)
		{
			if (g.f == nullptr || !g.can_mark)
				return;
			const uint32_t count = g.f->element_count - g.run_first;
			if (count == 0 && !even_empty)
				return;
			const uint32_t r = g.f->runs.load(std::memory_order_relaxed);
			if (r >= fx::kMaxRuns)
				return; // (never with elements in it: record() stops taking before)
			g.f->run_list[r].first = g.run_first;
			g.f->run_list[r].count = count;
			g.f->runs.store(r + 1, std::memory_order_release);
			g.run_first = g.f->element_count;
			g.n.runs++;
			send_marker();
		}

		// 0467E0's blend and its letter changes (fx::letter_blend); the subtractive letters only with the device's blend
		// ops (046550 otherwise draws a copy of the frame back: left to the game)
		bool letter_blend(uint16_t letter, uint32_t &colour, uint8_t &blend)
		{
			if (!fx::letter_blend(letter, colour, blend))
				return false;
			if (blend != fx::blend_subtract)
				return true;
			const uintptr_t gfx = at<uint32_t>(g.gfx);
			return gfx != 0 && (at<uint32_t>(gfx + 0xDEC) & 0x800) != 0;
		}

		// The D3M's names and vertices (03FCF0, 03FD70) from its data (resource + 0x30)
		uintptr_t d3m_names(uintptr_t data)
		{
			const uint32_t kind = at<uint8_t>(data) & 0xF;
			if (kind < 5 || kind > 7)
				return data + 0x10;
			const uintptr_t counts = data + 8;
			const uint32_t textures = at<uint8_t>(data + 4);
			if (kind == 7 && textures == 0)
				return counts;
			const uint32_t n = textures + at<uint8_t>(data + 5);
			return n % 4 != 0 ? counts + (n - n % 4) * 2 + 6 : counts + n * 2;
		}
		uintptr_t d3m_vertices(uintptr_t data)
		{
			const uint32_t kind = at<uint8_t>(data) & 0xF;
			if (kind < 5 || kind > 7)
				return data + 0x50;
			return d3m_names(data) + uintptr_t(at<uint8_t>(data + 4)) * 16;
		}

		// A texture object's texture id once it's on the GPU (kNone: not yet, or not to be had); new ones queued
		uint32_t texture_of(uintptr_t object)
		{
			if (!pointer(uint32_t(object)))
				return fx::kNone;
			const uint32_t res = at<uint32_t>(object + 8);
			if (!pointer(res))
				return fx::kNone;
			fx::textures::key k;
			k.resource = res;
			k.handle = at<uint32_t>(res + 0x14);
			k.header = at<uint32_t>(res + 0x24);
			k.data = at<uint32_t>(res + 0x30 + 6);
			uint32_t id = fx::textures::find(k, g.clock);
			if (id == fx::kNone)
			{
				const uint32_t size = ((k.header >> 7) & 0x7FFFF) * 16;
				if ((k.header & 0x7F) != 0x20 || size <= 16 || size > (8u << 20))
					return fx::kNone;
				std::vector<uint8_t> bytes(size - 16);
				if (!read(res + 0x30, bytes.data(), bytes.size()))
					return fx::kNone;
				id = fx::textures::add(k, std::move(bytes), g.clock);
			}
			return id != fx::kNone && fx::textures::ready(id) ? id : fx::kNone;
		}

		// What every class shares: WORLD, TEXTUREFACTOR, blend, z write, projection, fog, the UV offset. False: the
		// game's (a blend letter without a case)
		bool common(uintptr_t e, fx::element &out)
		{
			const uint32_t flags = at<uint32_t>(e + kFlags);
			const uint16_t letter = at<uint16_t>(e + kLetter);
			uint32_t colour = at<uint32_t>(g.colour);
			uint8_t blend = fx::blend_add;
			if (!letter_blend(letter, colour, blend))
			{
				g.n.letter++;
				return false;
			}
			// (the draw matrix, as 0467E0 builds it)
			reinterpret_cast<matrix_fn>(g.draw_matrix)(e, 0);
			std::memcpy(out.world, reinterpret_cast<const void *>(e + kWorld), 64);
			out.tfactor = colour;
			out.blend = blend;
			out.z_write = (flags >> 12) & 1;
			// PROJECTION (046070): the depth pull, else screen-fixed
			const uintptr_t renderer = at<uint32_t>(g.renderer), camera = at<uint32_t>(g.camera);
			const float bias = renderer != 0 && at<uint8_t>(renderer + 0x39598) != 0 ? at<float>(renderer + 0x39594) : 0.0f;
			const float cam_near = camera != 0 ? at<float>(camera + 0x2DC) : 0.0f, cam_far = camera != 0 ? at<float>(camera + 0x2E0) : 0.0f;
			const float pull = at<float>(e + kBias);
			out.projection = fx::proj_camera;
			if ((flags & (1u << 27)) == 0 && pull != 0.0f && (flags & (1u << 22)) == 0)
			{
				const float z = std::fabs(out.world[14]);
				if (z < 32.0f && camera != 0)
				{
					const float k = 1.0f - z * 0.03125f;
					const float s = k * k * k * 0.03f * pull;
					out.projection = fx::proj_near_far;
					out.near_z = cam_near - s + bias;
					out.far_z = cam_far - s + bias;
				}
			}
			else if ((flags & 1) != 0 && (flags & 0x1C0) == 0 && (flags & 4) != 0 && (flags & 8) != 0 && camera != 0)
			{
				// (D3DXMatrixPerspectiveFovRH: _22 = cot(fov / 2) = 350 / 192, _11 = _22 / aspect, aspect = 1 / (+0x2E8 x +0x2F0))
				out.projection = fx::proj_screen;
				out.near_z = cam_near + bias;
				out.far_z = cam_far + bias;
				out.scale_y = 350.0f / 192.0f;
				out.scale_x = out.scale_y * at<float>(camera + 0x2E8) * at<float>(camera + 0x2F0);
			}
			// Fog
			out.fog = 0;
			if ((flags & (1u << 25)) == 0)
			{
				const uintptr_t gfx = at<uint32_t>(g.gfx);
				const uintptr_t wrapper = gfx != 0 ? at<uint32_t>(gfx + 0xDC8) : 0;
				const uint8_t table = wrapper != 0 ? at<uint8_t>(wrapper + 0x92D) : 0, vertex = wrapper != 0 ? at<uint8_t>(wrapper + 0x92E) : 0;
				const uint8_t range = wrapper != 0 ? at<uint8_t>(wrapper + 0x92F) : 0;
				const uint32_t handle = at<uint32_t>(e + kGen);
				const uint32_t gen = handle != 0 ? at<uint32_t>(handle) : 0;
				const uint32_t area = gen != 0 ? at<uint32_t>(gen + 0xC4) : 0;
				uint32_t fog_colour = 0;
				float fog_end = 0.0f, fog_start = 0.0f;
				const uintptr_t env = at<uint32_t>(g.env);
				if (env != 0)
				{
					reinterpret_cast<fog_fn>(g.fog_env)(env, 0, &fog_colour, &fog_end, &fog_start, 0, area);
					out.fog = table != 0 ? 1 : vertex != 0 ? (range != 0 ? 2 : 1) : 0;
				}
				out.fog_colour = (letter & 0xFF) == 0x48 ? 0 : fog_colour;
				out.fog_start = fog_start;
				out.fog_end = fog_end;
			}
			out.uv[0] = out.uv[1] = 0.0f;
			if ((at<uint8_t>(e + kUvBits) & 3) != 0)
			{
				out.uv[0] = at<float>(e + kUv);
				out.uv[1] = at<float>(e + kUv + 4);
			}
			return true;
		}

		// A lit element's lights (flags bit 16) into the frame, as 0467E0 works them out for D3D8's lighting (046A72-
		// 046D9E, 04719C-0471CC): L = the element's frame (045C50) without its translation; unless an MMB (+0x179) or the
		// generator's +0xDF bit 1, with an attach matrix (0473F0): L turned by 3 pi / 2 (vt+0x78), the attach matrix fixed
		// up (045800) and L = L x it (vt+0x20); bits 6-8 1 or 2 with bit 0: VIEW without its translation, vt+0x24 (L,
		// VIEW, L), L = that VIEW, turned by pi. The lights: one (18B9A0, light 1 off) when the renderer's path
		// +0x3949C, else two (18B960 with the generator's area, 188B70 without), each direction through L (vt+0x50);
		// AMBIENT 18B9E0; the material VA 0490BC28. All read under record()'s guard; false: the game's (none of the
		// calls found, no environment or math object, WORLD not invertible).
		struct d3dlight
		{
			uint32_t type;
			float diffuse[4], specular[4], ambient[4];
			float position[3], direction[3];
			float range, falloff, attenuation[3], theta, phi;
		};
		static_assert(sizeof(d3dlight) == 104, "D3DLIGHT8");

		bool record_light(uintptr_t e, fx::element &out)
		{
			fx::frame &f = *g.f;
			const uintptr_t env = at<uint32_t>(g.env), math = at<uint32_t>(g.math), renderer = at<uint32_t>(g.renderer), gfx = at<uint32_t>(g.gfx);
			if (g.frame_of == 0 || g.attach == 0 || g.fix == 0 || g.light1 == 0 || g.lights2 == 0 || g.lights2_default == 0 || g.ambient == 0 || g.material == 0 ||
				env == 0 || math == 0 || renderer == 0 || gfx == 0 || f.light_count >= fx::kMaxLit)
				return false;
			const uintptr_t vt = at<uint32_t>(math);
			const auto turn = reinterpret_cast<turn_fn>(uintptr_t(at<uint32_t>(vt + kTurn)));
			const auto product_a = reinterpret_cast<product_fn>(uintptr_t(at<uint32_t>(vt + kProductA)));
			const auto product_b = reinterpret_cast<product_fn>(uintptr_t(at<uint32_t>(vt + kProductB)));
			const auto point = reinterpret_cast<point_fn>(uintptr_t(at<uint32_t>(vt + kPoint)));
			float three_half_pi, pi;
			std::memcpy(&three_half_pi, &kThreeHalfPi, 4);
			std::memcpy(&pi, &kPi, 4);
			const uint32_t flags = at<uint32_t>(e + kFlags);
			// L, the frame the lights are turned into
			alignas(16) float L[16];
			std::memcpy(L, reinterpret_cast<const void *>(reinterpret_cast<matrix_of_fn>(g.frame_of)(e, 0)), 64);
			L[12] = L[13] = L[14] = 0.0f;
			L[15] = 1.0f;
			const uint32_t handle = at<uint32_t>(e + kGen);
			const uint32_t gen = handle != 0 ? at<uint32_t>(handle) : 0;
			const bool own_frame = gen != 0 && ((at<uint8_t>(gen + 0xDF) >> 1) & 1) != 0;
			if (at<uint8_t>(e + kMmb) == 0 && !own_frame)
			{
				const uintptr_t a = reinterpret_cast<matrix_of_fn>(g.attach)(e, 0);
				if (a != 0)
				{
					alignas(16) float t[16];
					std::memcpy(t, reinterpret_cast<const void *>(a), 64);
					turn(math, 0, L, L, three_half_pi);
					reinterpret_cast<fix_fn>(g.fix)(t);
					product_a(math, 0, L, L, t);
				}
			}
			const uint32_t facing = (flags >> 6) & 7;
			if (facing >= 1 && facing <= 2 && (flags & 1) != 0)
			{
				alignas(16) float t[16];
				std::memcpy(t, reinterpret_cast<const void *>(gfx + 0xB50), 64);
				t[12] = t[13] = t[14] = 0.0f;
				t[15] = 1.0f;
				product_b(math, 0, L, t, L);
				std::memcpy(L, t, 64);
				turn(math, 0, L, L, pi);
			}
			// The lights and the ambient, from the environment for the generator's area
			const uint32_t area = gen != 0 ? at<uint32_t>(gen + 0xC4) : 0;
			d3dlight lights[2] = {};
			const bool one = at<uint32_t>(renderer + 0x3949C) != 0;
			if (one)
				reinterpret_cast<light_fn>(g.light1)(env, 0, &lights[0], 0, area);
			else if (gen != 0)
				reinterpret_cast<lights_fn>(g.lights2)(env, 0, &lights[0], &lights[1], area);
			else
				reinterpret_cast<lights_default_fn>(g.lights2_default)(env, 0, &lights[0], &lights[1]);
			for (int k = 0; k < (one ? 1 : 2); ++k)
				point(math, 0, lights[k].direction, L, lights[k].direction);
			uint32_t ambient = 0;
			reinterpret_cast<ambient_fn>(g.ambient)(env, 0, &ambient, 0, area);
			// Normals: by WORLD's 3x3, inverted and transposed
			const float *w = out.world;
			const float det = w[0] * (w[5] * w[10] - w[6] * w[9]) - w[1] * (w[4] * w[10] - w[6] * w[8]) + w[2] * (w[4] * w[9] - w[5] * w[8]);
			if (!std::isfinite(det) || std::fabs(det) < 1e-20f)
				return false;
			const float inv[9] = { (w[5] * w[10] - w[6] * w[9]) / det, (w[2] * w[9] - w[1] * w[10]) / det, (w[1] * w[6] - w[2] * w[5]) / det,
				(w[6] * w[8] - w[4] * w[10]) / det, (w[0] * w[10] - w[2] * w[8]) / det, (w[2] * w[4] - w[0] * w[6]) / det,
				(w[4] * w[9] - w[5] * w[8]) / det, (w[1] * w[8] - w[0] * w[9]) / det, (w[0] * w[5] - w[1] * w[4]) / det };
			fx::lighting &l = f.lights[f.light_count];
			l = fx::lighting();
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					l.normal[i][j] = inv[j * 3 + i];
			for (int k = 0; k < (one ? 1 : 2); ++k)
			{
				const float *d = lights[k].direction;
				const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				for (int c = 0; c < 3; ++c)
				{
					l.toward[k][c] = len > 0.0f ? -d[c] / len : 0.0f;
					l.diffuse[k][c] = lights[k].diffuse[c];
					l.ambient[c] += lights[k].ambient[c];
				}
			}
			for (int c = 0; c < 3; ++c)
			{
				l.ambient[c] += float((ambient >> (16 - 8 * c)) & 0xFF) / 255.0f;
				l.emissive[c] = at<float>(g.material + 0x30 + 4 * uintptr_t(c));
			}
			out.light = f.light_count++;
			return true;
		}

		// An element's lighting when it's lit (flags bit 16): false, counted, when it can't be had (the game's)
		bool lit_ok(uintptr_t e, fx::element &out)
		{
			if ((at<uint8_t>(e + kFlags + 2) & 1) == 0)
				return true;
			if (record_light(e, out))
			{
				g.n.lit_taken++;
				return true;
			}
			g.n.lit++;
			return false;
		}

		// The first vertex's y of an untextured D3M of kinds 5-7 (the load wrote the vertex buffer object over it in
		// memory; record_d3m), from its DAT: the file id at its root (up the parents +0x28, as 072240; the root's +0x34),
		// the chunk of its name (resource +0x20), type 0x1F and size whose other vertex positions are the ones in memory.
		// Cached per resource (a failure too); false when it can't be had.
		struct first_y
		{
			uint32_t handle = 0, header = 0;
			bool ok = false;
			float y = 0.0f;
		};
		std::map<uintptr_t, first_y> g_first_y;

		bool dat_first_y(uintptr_t resource, float &y)
		{
			const uint32_t handle = at<uint32_t>(resource + 0x14), header = at<uint32_t>(resource + 0x24);
			const auto known = g_first_y.find(resource);
			if (known != g_first_y.end() && known->second.handle == handle && known->second.header == header)
			{
				y = known->second.y;
				return known->second.ok;
			}
			first_y &out = g_first_y[resource];
			out = first_y();
			out.handle = handle;
			out.header = header;
			uintptr_t r = resource;
			for (int depth = 0; depth < 64; ++depth)
			{
				const uint32_t h = at<uint32_t>(r + 0x28);
				const uint32_t parent = h != 0 ? at<uint32_t>(h) : 0;
				if (parent == 0 || parent == r)
					break;
				r = parent;
			}
			const uint32_t file = at<uint32_t>(r + 0x34);
			std::string why;
			const std::wstring path = file < 0x40000 ? bake::dat_path(file, why) : std::wstring();
			std::vector<uint8_t> dat;
			if (path.empty() || !bake::read_file(path, dat))
				return false;
			const uint32_t name = at<uint32_t>(resource + 0x20);
			const size_t size = ((header >> 7) & 0x7FFFF) * 16;
			const uintptr_t data = resource + 0x30;
			const uint32_t triangles = at<uint16_t>(data + 6);
			const size_t verts = d3m_vertices(data) - data;
			for (size_t off = 0; off + 16 <= dat.size();)
			{
				uint32_t w = 0, n = 0;
				std::memcpy(&n, &dat[off], 4);
				std::memcpy(&w, &dat[off + 4], 4);
				const size_t s = ((w >> 7) & 0x7FFFF) * 16;
				if (s < 16 || off + s > dat.size())
					break;
				if (n == name && (w & 0x7F) == 0x1F && s == size && verts + size_t(triangles) * 3 * 36 <= s - 16)
				{
					const uint8_t *d = &dat[off + 16];
					bool same = std::memcmp(d + 4, reinterpret_cast<const void *>(data + 4), 4) == 0;
					for (uint32_t i = 0; same && i < triangles * 3; ++i)
					{
						const uint8_t *fv = d + verts + size_t(i) * 36;
						const uintptr_t mv = data + verts + uintptr_t(i) * 36;
						same = i == 0 ? std::memcmp(fv, reinterpret_cast<const void *>(mv), 4) == 0 && std::memcmp(fv + 8, reinterpret_cast<const void *>(mv + 8), 4) == 0
									  : std::memcmp(fv, reinterpret_cast<const void *>(mv), 12) == 0;
					}
					if (same)
					{
						std::memcpy(&out.y, d + verts + 4, 4);
						out.ok = std::isfinite(out.y);
						y = out.y;
						return out.ok;
					}
				}
				off += s;
			}
			return false;
		}

		// The D3M (040220) into the frame; false: the game's
		bool record_d3m(uintptr_t e, uintptr_t resource)
		{
			fx::frame &f = *g.f;
			const uintptr_t data = resource + 0x30;
			const uint32_t textures = at<uint8_t>(data + 4), tail = at<uint8_t>(data + 5), triangles = at<uint16_t>(data + 6);
			const uintptr_t names = d3m_names(data), vertices = d3m_vertices(data);
			if (at<uint32_t>(names + 4) == 0)
			{
				g.n.empty++;
				return true; // (no vertex buffer: 040220 draws nothing)
			}
			const uint16_t letter = at<uint16_t>(e + kLetter);
			const uint32_t flags = at<uint32_t>(e + kFlags);
			// The draws, as 040220 makes them
			fx::draw draws[16];
			uint32_t count = 0, used = 0;
			bool patch_y = false;
			float first_y_value = 0.0f;
			if (textures == 0)
			{
				// (kinds 5-7 without textures: the names (03FCF0) begin where the vertices do, and the load stores the
				// vertex buffer object at names + 4 after copying them: the first vertex's y is gone from the chunk)
				const uint32_t kind = at<uint8_t>(data) & 0xF;
				if (kind >= 5 && kind <= 7)
				{
					if (!dat_first_y(resource, first_y_value))
					{
						g.n.overwritten++;
						return false;
					}
					patch_y = true;
				}
				draws[count++] = { 0, triangles * 3, fx::kNone, uint32_t((letter & 0xFF) == 0x44 ? fx::stage_alpha_2x : 0) };
				used = triangles * 3;
			}
			else
			{
				if (textures > 13)
				{
					g.n.unsupported++;
					return false;
				}
				const bool i_letter = (letter & 0xFF) == 0x49;
				const uint32_t stages = i_letter ? fx::stage_texture | fx::stage_colour_diffuse | fx::stage_colour_tfactor
					: fx::stage_texture | ((flags & (1u << 28)) != 0 ? fx::stage_alpha_diffuse : 0);
				uint32_t first = 0;
				for (uint32_t i = 0; i < textures; ++i)
				{
					const uint32_t object = at<uint32_t>(names + 16 * i);
					const uint32_t id = texture_of(object);
					if (id == fx::kNone)
					{
						g.n.texture++;
						return false;
					}
					const uint32_t n = uint32_t(at<uint16_t>(data + 8 + 2 * i)) * 3;
					draws[count++] = { first, n, id, stages };
					first += n;
				}
				used = first;
				if (tail != 0)
				{
					const uint32_t rest = first / 3 <= triangles ? (triangles - first / 3) * 3 : 0;
					draws[count++] = { first, rest, fx::kNone, 0 };
					used = first + rest;
				}
			}
			if (used > triangles * 3)
				used = triangles * 3;
			if (f.element_count >= fx::kMaxElements || f.draw_count + count > fx::kMaxDraws || f.vertex_count + used > fx::kMaxVertices)
			{
				g.n.full++;
				return false;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out) || !lit_ok(e, out))
				return false;
			// (the D3M's TEXTUREFACTOR: D's alpha from 0x7F up full; I's colour white, blended DESTCOLOR / SRCALPHA)
			if ((letter & 0xFF) == 0x44 && (out.tfactor >> 24) >= 0x7F)
				out.tfactor |= 0xFF000000u;
			if (textures != 0 && (letter & 0xFF) == 0x49)
			{
				out.tfactor |= 0x00FFFFFFu;
				out.blend = fx::blend_multiply;
			}
			std::memcpy(f.vertices + f.vertex_count, reinterpret_cast<const void *>(vertices), size_t(used) * sizeof(fx::vertex));
			if (patch_y && used != 0)
				f.vertices[f.vertex_count].pos[1] = first_y_value;
			out.first_draw = f.draw_count;
			for (uint32_t i = 0; i < count; ++i)
			{
				fx::draw d = draws[i];
				if (d.first + d.count > used)
					d.count = d.first < used ? used - d.first : 0;
				d.first += f.vertex_count;
				f.draws[f.draw_count++] = d;
			}
			out.draws = count;
			out.cls = fx::class_d3m;
			f.vertex_count += used;
			f.element_count++;
			g.n.taken[fx::class_d3m]++;
			return true;
		}

		// An MMB mesh's model in the zone's data (cached by the mesh; kNone: the game's). 183420 found the mesh as the
		// model of that name in the renderer's map slots (0, then 1: 170F30 on the slot's model list), so it's the
		// same model the native zone renderer has by its name (mesh +8, 16 characters, as the zone's objects' models).
		// Taken only when the mesh is as that model was read: FVF 0x152 in 36-byte strips (+0x20, +0x24, +0x2C 5: kind
		// 1; not the bump shader's), as many parts as the MMB has pieces (+0x60 groups of 0x24 bytes, +0x44 of them;
		// +0 a group's parts of 0xC0 bytes at +4), each part's vertex buffer made (+0x28), a texture object's (+4)
		// Direct3D texture there (+0x40, else +0x44), and no texture the DAT doesn't have.
		mesh_model model_of(uintptr_t mesh)
		{
			const auto known = g_zones.of.find(mesh);
			if (known != g_zones.of.end() && (known->second.zone == nullptr || known->second.zone == g_zones.data[0] || known->second.zone == g_zones.data[1]))
				return known->second;
			mesh_model out;
			char name[16];
			std::memcpy(name, reinterpret_cast<const void *>(mesh + 8), 16);
			for (zone::gpu_zone *z : g_zones.data)
			{
				if (z == nullptr)
					continue;
				const zm::zone &d = z->data();
				uint32_t found = zm::kNone;
				for (uint32_t i = 0; i < uint32_t(d.models.size()); ++i)
					if (std::memcmp(d.models[i].name, name, 16) == 0)
					{
						found = i;
						break;
					}
				if (found == zm::kNone)
					continue;
				out.zone = z;
				out.model = found;
				break;
			}
			if (out.zone == nullptr)
				return out; // (not cached: the data may come up later)
			const zm::model &m = out.zone->data().models[out.model];
			bool same = m.kind == 1 && !m.bump && m.missing == 0 && at<uint32_t>(mesh + 0x20) == 0x152 && at<uint32_t>(mesh + 0x24) == 0x24 && at<uint32_t>(mesh + 0x2C) == 5;
			const uintptr_t groups = at<uint32_t>(mesh + 0x60);
			const int32_t group_count = at<int32_t>(mesh + 0x44);
			uint32_t parts = 0;
			for (int32_t gi = 0; same && groups != 0 && gi < group_count && gi < 4096; ++gi)
			{
				const int32_t n = at<int32_t>(groups + 0x24 * uintptr_t(gi));
				const uintptr_t list = at<uint32_t>(groups + 0x24 * uintptr_t(gi) + 4);
				for (int32_t k = 0; same && k < n && k < 4096; ++k)
				{
					const uintptr_t part = list + 0xC0 * uintptr_t(k);
					const uint32_t object = at<uint32_t>(part + 4);
					same = at<uint32_t>(part + 0x28) != 0 && (object == 0 || at<uint32_t>(object + 0x40) != 0 || at<uint32_t>(object + 0x44) != 0);
					parts++;
				}
			}
			if (!same || parts != m.raw_pieces)
				out.model = zm::kNone;
			g_zones.of[mesh] = out;
			return out;
		}

		// An MMB effect mesh (040C20 -> 183760) into the frame; false: the game's. 183760 (thiscall on the zone
		// renderer: the draw matrix +0xA0, the mesh +0x104, the textured mode: letter d, else flags bit 28) culls the
		// mesh's box (+0x70) against the projection (182070: only when all eight corners are past one side, the near
		// side at 0.2; not done here: what it would cull draws nothing on the GPU either, but for a mesh wholly within
		// 0.2 of the eye), sets WORLD = the draw matrix with the mesh's +0xE0..+0xEC added to its last row, ADDRESS
		// WRAP, and draws each part with its vertex buffer: untextured CURRENT x TFACTOR x 2, alpha x 4; textured by the
		// mode (fx::stage_mmb_textured, _d). Blend, z write, fog and projection are 0467E0's; the UV scroll 040C20's.
		// World FX's gamewater list (by the mesh's 4-character name +0x18) is honoured: such a mesh is taken with
		// nothing drawn, as its routine would skip 183760.
		bool record_mmb(uintptr_t e, uintptr_t mesh)
		{
			fx::frame &f = *g.f;
			if (game::gamewater::hides_mesh(at<uint32_t>(mesh + 0x18)))
			{
				g.n.water++;
				return true;
			}
			if (at<uint32_t>(mesh + 0x60) == 0)
			{
				g.n.empty++;
				return true; // (no groups: 183760 draws nothing)
			}
			const mesh_model mm = model_of(mesh);
			if (mm.zone == nullptr || mm.model == zm::kNone)
			{
				g.n.mesh++;
				return false;
			}
			const zm::zone &d = mm.zone->data();
			const zm::model &m = d.models[mm.model];
			const uint16_t letter = at<uint16_t>(e + kLetter);
			const uint32_t flags = at<uint32_t>(e + kFlags);
			const bool mode = (letter & 0xFF) == 0x64 || (flags & (1u << 28)) != 0;
			// The draws: the pieces in file order, a run of them with the same texture as one
			uint32_t count = 0;
			const uint32_t first_draw = f.draw_count;
			for (uint32_t i = m.first_piece; i < m.first_piece + m.pieces && i < d.pieces.size(); ++i)
			{
				const zm::piece &pc = d.pieces[i];
				const uint32_t stages = fx::stage_zone_mesh | (pc.texture == zm::kNone ? 0u : mode ? fx::stage_mmb_textured_d : fx::stage_mmb_textured);
				const uint32_t texture = pc.texture == zm::kNone ? fx::kNone : pc.texture;
				if (count != 0)
				{
					fx::draw &last = f.draws[first_draw + count - 1];
					if (last.texture == texture && last.stages == stages && last.first + last.count == pc.first_index)
					{
						last.count += pc.triangles * 3;
						continue;
					}
				}
				if (f.element_count >= fx::kMaxElements || first_draw + count + 1 > fx::kMaxDraws)
				{
					g.n.full++;
					return false;
				}
				f.draws[first_draw + count++] = { pc.first_index, pc.triangles * 3, texture, stages };
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out) || !lit_ok(e, out))
				return false;
			for (int k = 0; k < 4; ++k)
				out.world[12 + k] += at<float>(mesh + 0xE0 + 4 * uintptr_t(k));
			out.zone = mm.zone;
			out.first_draw = first_draw;
			out.draws = count;
			out.cls = fx::class_mmb;
			f.draw_count += count;
			f.element_count++;
			g.n.taken[fx::class_mmb]++;
			return true;
		}

		// The D3A (03EB30 -> 03E910) into the frame; false: the game's. The resource keeps the texture object in the
		// name's first dword (data +8, 075520), the vertex buffer object at +0xC and the frame table at +0x14 (03E6C0:
		// per frame u16 quads, s16 first vertex); the frames' vertices stay in the chunk as the DAT has them (24 bytes:
		// x y z, D3DCOLOR, u v), the load putting them in the vertex buffer with a normal (0, 0, 1) and the UVs times 1 /
		// the texture's width and height when +0 >= 1 and +7 is 0 (the texture object's +0x24, +0x26: 075FB0, 075FD0).
		// The draw: the frame +0x170 (wrapped to 0 at the frame count, written back as 03E910 does), table 351FA0 (bit
		// 28: 351EC8; @: 352084, but @ has no blend of its own in 0467E0, so never here), TEXTUREFACTOR the colour,
		// LIGHTING 0, FOGENABLE 0, ZWRITEENABLE 0, quads x 2 triangles from the frame's first vertex.
		bool record_d3a(uintptr_t e, uintptr_t resource)
		{
			fx::frame &f = *g.f;
			const uintptr_t data = resource + 0x30;
			if (at<uint32_t>(data + 0xC) == 0)
			{
				g.n.empty++;
				return true; // (no vertex buffer: 03E910 draws nothing)
			}
			const int16_t frames = at<int16_t>(data + 2), units = at<int16_t>(data);
			uint16_t frame = at<uint16_t>(e + 0x170);
			if (int32_t(frame) >= int32_t(frames))
			{
				frame = 0;
				*reinterpret_cast<uint16_t *>(e + 0x170) = 0;
			}
			const uintptr_t table = at<uint32_t>(data + 0x14);
			if (table == 0 || frames <= 0)
			{
				g.n.unsupported++;
				return false;
			}
			const int32_t quads = at<int16_t>(table + 4 * uintptr_t(frame)), first = at<int16_t>(table + 4 * uintptr_t(frame) + 2);
			const uint32_t object = at<uint32_t>(data + 8);
			const uint32_t id = texture_of(object);
			if (id == fx::kNone)
			{
				g.n.texture++;
				return false;
			}
			// (the frame's vertices in the chunk: the heads walked to it; its first vertex must be the table's)
			uintptr_t p = data + 0x18;
			int32_t at_vertex = 0;
			for (uint32_t i = 0; i < frame; ++i)
			{
				const uint32_t q = at<uint8_t>(p + 2);
				at_vertex += int32_t(q) * 6;
				p += 4 + uintptr_t(q) * 6 * 24;
			}
			const uint32_t n = uint32_t(quads) * 6;
			if (at_vertex != first || quads < 0 || at<uint8_t>(p + 2) != uint32_t(quads))
			{
				g.n.unsupported++;
				return false;
			}
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + n > fx::kMaxVertices)
			{
				g.n.full++;
				return false;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out))
				return false;
			float su = 1.0f, sv = 1.0f;
			if (units >= 1 && at<uint8_t>(data + 7) == 0)
			{
				const uint16_t w = at<uint16_t>(object + 0x24), h = at<uint16_t>(object + 0x26);
				su = w != 0 ? 1.0f / float(w) : 1.0f;
				sv = h != 0 ? 1.0f / float(h) : 1.0f;
			}
			const uintptr_t v0 = p + 4;
			for (uint32_t k = 0; k < n; ++k)
			{
				const uintptr_t s = v0 + uintptr_t(k) * 24;
				fx::vertex &v = f.vertices[f.vertex_count + k];
				std::memcpy(v.pos, reinterpret_cast<const void *>(s), 12);
				v.normal[0] = v.normal[1] = 0.0f;
				v.normal[2] = 1.0f;
				v.colour = at<uint32_t>(s + 12);
				v.uv[0] = at<float>(s + 16) * su;
				v.uv[1] = at<float>(s + 20) * sv;
			}
			const uint32_t flags = at<uint32_t>(e + kFlags);
			out.z_write = 0;
			out.fog = 0;
			out.first_draw = f.draw_count;
			out.draws = 1;
			out.cls = fx::class_d3a;
			f.draws[f.draw_count++] = { f.vertex_count, n, id, fx::stage_texture | ((flags & (1u << 28)) != 0 ? fx::stage_alpha_diffuse : 0u) };
			f.vertex_count += n;
			f.element_count++;
			g.n.taken[fx::class_d3a]++;
			return true;
		}

		// The point walk of a CMoVuAnmElem (063C80 as 03F130 calls it: ecx the element's +0x194, the callback 03EFC0 and
		// the element), with what it reads set as 03F130 and 0467E0 leave it: the cached WORLD (the wrapper's +0x93C,
		// 06A4A0's: the element's draw matrix, as 0467E0 sets it), VA 0490BF34 the attach matrix without its translation,
		// VA 0490B9D4 where the corners go (03EFC0 writes 6 vertices a point there and moves it on). The cache and the
		// cursor are put back after (fault or not); 063C80 clears 0490BF34 itself. The points walked (its answer).
		using walk_fn = int32_t(__fastcall *)(uintptr_t vu, uintptr_t edx, uintptr_t callback, uintptr_t element);
		int32_t vu_walk(uintptr_t e, uintptr_t wrapper, const float *world, const float *attach, fx::vertex *out)
		{
			float kept[16];
			std::memcpy(kept, reinterpret_cast<const void *>(wrapper + 0x93C), 64);
			const uint32_t cursor = at<uint32_t>(g.vu_cursor);
			__try
			{
				std::memcpy(reinterpret_cast<void *>(wrapper + 0x93C), world, 64);
				*reinterpret_cast<uint32_t *>(g.vu_attach) = uint32_t(reinterpret_cast<uintptr_t>(attach));
				*reinterpret_cast<uint32_t *>(g.vu_cursor) = uint32_t(reinterpret_cast<uintptr_t>(out));
				return reinterpret_cast<walk_fn>(g.vu_walk)(e + 0x194, 0, g.vu_callback, e);
			}
			__finally
			{
				std::memcpy(reinterpret_cast<void *>(wrapper + 0x93C), kept, 64);
				*reinterpret_cast<uint32_t *>(g.vu_cursor) = cursor;
			}
		}

		// A CMoVuAnmElem (03F130: a D3A frame's first quad at every point of a walk) into the frame; false: the game's.
		// Its first draw (+0x1DC 0) makes the corner table +0x22D the walk's callback reads and a vertex buffer of frames x
		// (points +0x1D8 x 6) vertices whose template it fills once: each point's 6 vertices take the UV and colour of the
		// frame's first quad's vertices 0, 0, 1, 2, 5, 5 (table 352218; the D3A's own UVs, not scaled), normal (0, 0, -1):
		// left to the game (it makes them). After: the texture (075700 on +0x140: its +0x40, else +0x44; a wave of it when
		// +0x148: the game's), the attach matrix (0473F0, else identity) without its translation into VA 0490BF34, the frame
		// +0x170 wrapped at the count (written back), the frame's vertices locked into VA 0490B9D4, then the walk 063C80
		// (vu_walk: the game's own, called once as 03F130 calls it; it moves the walk's clock +0x198 on and draws its random
		// points from a generator seeded from +0x1A4 each time), 03EFC0 writing each point's 6 vertices (the corners 0, 0,
		// 1, 2, 3, 3 through the point's matrix). Then 03E9A0's D3A states (351FA0 / bit 28 351EC8, LIGHTING 0, FOGENABLE
		// 0, ZWRITEENABLE 0), WORLD math +0x810, and one TRIANGLESTRIP from vertex 1 of n x 4 - 2 triangles (n the points
		// walked: so the strip covers vertices 1 .. 4n of the 6n written, about two points in three; as the game draws it).
		// The strip is cut into a list here, its degenerate triangles (two corners the same) left out.
		std::vector<fx::vertex> g_vu;

		bool record_vuanm(uintptr_t e, uintptr_t resource)
		{
			fx::frame &f = *g.f;
			const uintptr_t data = resource + 0x30;
			const int32_t frames = at<int16_t>(data + 2), points = at<int16_t>(e + 0x1D8);
			const uint32_t object = at<uint32_t>(e + 0x140);
			const uintptr_t gfx = at<uint32_t>(g.gfx), math = at<uint32_t>(g.math);
			const uintptr_t wrapper = gfx != 0 ? at<uint32_t>(gfx + 0xDC8) : 0;
			if (g.vu_walk == 0 || g.attach == 0 || wrapper == 0 || math == 0 || at<uint8_t>(e + 0x1DC) == 0 || frames <= 0 || points < 0 ||
				at<int32_t>(e + 0x229) != points * 6 || object == 0 || (at<uint32_t>(object + 0x40) == 0 && at<uint32_t>(object + 0x44) == 0) || at<float>(e + 0x148) != 0.0f)
			{
				g.n.unsupported++;
				return false;
			}
			// (the most points the walk can give: +0x1D8, or from 20 up with +0x1DA bit 0 that x 002D80's factor x 0.5)
			uint32_t bound = uint32_t(points);
			if (points >= 20 && (at<uint8_t>(e + 0x194 + 0x46) & 1) != 0)
			{
				const float k = reinterpret_cast<weather_fn>(g.weather)();
				if (!(k >= 0.0f && k < 64.0f))
				{
					g.n.unsupported++;
					return false;
				}
				bound = std::max(bound, uint32_t(std::ceil(float(points) * k * 0.5f)) + 1);
			}
			if (bound > kMaxVuPoints)
			{
				g.n.unsupported++;
				return false;
			}
			const uint32_t most = bound != 0 ? (4 * bound - 2) * 3 : 0;
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + most > fx::kMaxVertices)
			{
				g.n.full++;
				return false;
			}
			const uint32_t id = texture_of(object);
			if (id == fx::kNone)
			{
				g.n.texture++;
				return false;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out))
				return false;
			// The frame, wrapped as 03F130 wraps it, and its first quad's template
			uint16_t frame = at<uint16_t>(e + 0x170);
			if (int32_t(frame) >= frames)
			{
				frame = 0;
				*reinterpret_cast<uint16_t *>(e + 0x170) = 0;
			}
			uintptr_t p = data + 0x18;
			for (uint32_t i = 0; i < frame; ++i)
				p += 4 + uintptr_t(at<uint8_t>(p + 2)) * 0x90;
			fx::vertex tpl[6];
			for (int j = 0; j < 6; ++j)
			{
				const uintptr_t s = p + 4 + uintptr_t(int32_t(at<int8_t>(g.vu_table + uintptr_t(j)))) * 0x18;
				tpl[j] = fx::vertex();
				tpl[j].normal[2] = -1.0f;
				tpl[j].colour = at<uint32_t>(s + 12);
				tpl[j].uv[0] = at<float>(s + 16);
				tpl[j].uv[1] = at<float>(s + 20);
			}
			g_vu.resize(size_t(bound) * 6 + 6);
			for (size_t i = 0; i < g_vu.size(); ++i)
				g_vu[i] = tpl[i % 6];
			// The attach matrix without its translation, the walk
			alignas(16) float attach[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
			if (const uintptr_t a = reinterpret_cast<matrix_of_fn>(g.attach)(e, 0); a != 0)
				std::memcpy(attach, reinterpret_cast<const void *>(a), 64);
			attach[12] = attach[13] = attach[14] = 0.0f;
			attach[15] = 1.0f;
			const int32_t n = vu_walk(e, wrapper, out.world, attach, g_vu.data());
			if (n <= 0 || uint32_t(n) > bound)
			{
				// (none walked: 03F130 draws nothing)
				g.n.empty++;
				return true;
			}
			// The strip from vertex 1, n x 4 - 2 triangles, as a list
			uint32_t w = f.vertex_count;
			const auto same = [](const fx::vertex &a, const fx::vertex &b) { return std::memcmp(a.pos, b.pos, 12) == 0; };
			for (uint32_t k = 0; k < 4 * uint32_t(n) - 2; ++k)
			{
				const fx::vertex &a = g_vu[1 + k], &b = g_vu[2 + k], &c = g_vu[3 + k];
				if (same(a, b) || same(b, c) || same(a, c))
					continue;
				f.vertices[w++] = a;
				f.vertices[w++] = b;
				f.vertices[w++] = c;
			}
			std::memcpy(out.world, reinterpret_cast<const void *>(math + 0x810), 64);
			const uint32_t flags = at<uint32_t>(e + kFlags);
			out.z_write = 0;
			out.fog = 0;
			out.first_draw = f.draw_count;
			out.draws = 1;
			out.cls = fx::class_vuanm;
			f.draws[f.draw_count++] = { f.vertex_count, w - f.vertex_count, id, fx::stage_texture | ((flags & (1u << 28)) != 0 ? fx::stage_alpha_diffuse : 0u) };
			f.vertex_count = w;
			f.element_count++;
			g.n.taken[fx::class_vuanm]++;
			return true;
		}

		// A morph (03FBB0 -> 03F440, chunk 0x25 at resource +0x30) into the frame; false: the game's. 03FBB0 hands
		// 03F440 the weights +0x194, the last frame's +0x1A8 and a blend made already (+0x1BC: then the game's), then
		// clears +0x1BC and copies +0x194 to +0x1A8 (done here too once it's taken). The blend (03F8A0): K targets
		// (+2's low 7 bits, signed; bit 7 the alpha test GREATER 0x60), each P positions (+4) then N packed normals
		// (+6; bits 0-9, 10-19, 20-29 signed, / 512) from +0x20; the weights over their sum, or when that's under
		// FLT_MIN the largest of the last frame's set to 1.0 (written back) and taken as they are. 03F440 then makes T
		// (+0xA) x 3 vertices: the position and normal through the index lists (+8, bits 16-23 at +3; normals' after
		// the positions'), the colours (+0xC, T x 3) doubled, the UVs (+0xE, + 0x10000 unless above +0xC); the texture
		// the name's first dword (+0x10, 075700: its +0x40, else +0x44; a wave of it when +0x148: the game's); table
		// 352258 (stage 0 TEXTURE x CURRENT x 2, colour and alpha; stage 1 x TFACTOR x 2, alpha x 4), one draw.
		bool record_morph(uintptr_t e, uintptr_t resource)
		{
			fx::frame &f = *g.f;
			const uintptr_t data = resource + 0x30;
			const size_t size = ((at<uint32_t>(resource + 0x24) >> 7) & 0x7FFFF) * 16;
			if (at<uint32_t>(e + 0x1BC) != 0 || at<float>(e + 0x148) != 0.0f || size < 0x30)
			{
				g.n.unsupported++;
				return false;
			}
			const uint8_t head = at<uint8_t>(data + 2);
			const int32_t targets = int32_t(int8_t(uint8_t(head << 1))) >> 1;
			const int32_t positions = at<int16_t>(data + 4), normals = at<int16_t>(data + 6);
			const uint32_t triangles = at<uint16_t>(data + 0xA);
			const uint32_t index_at = uint32_t(at<uint16_t>(data + 8)) + uint32_t(int32_t(at<int8_t>(data + 3)) * 0x10000);
			const uint32_t colour_at = at<uint16_t>(data + 0xC), uv_word = at<uint16_t>(data + 0xE);
			const uint32_t uv_at = colour_at < uv_word ? uv_word : uv_word + 0x10000;
			const size_t body = size - 16, stride = (size_t(positions) * 3 + size_t(normals)) * 4;
			if (targets < 1 || targets > 5 || positions <= 0 || normals <= 0 || triangles == 0 || 0x20 + stride * size_t(targets) > body ||
				size_t(index_at) + size_t(triangles) * 12 > body || size_t(colour_at) + size_t(triangles) * 12 > body || size_t(uv_at) + size_t(triangles) * 24 > body)
			{
				g.n.unsupported++;
				return false;
			}
			const uint32_t n = triangles * 3;
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + n > fx::kMaxVertices)
			{
				g.n.full++;
				return false;
			}
			const uint32_t object = at<uint32_t>(data + 0x10);
			if (object == 0 || (at<uint32_t>(object + 0x40) == 0 && at<uint32_t>(object + 0x44) == 0))
			{
				g.n.unsupported++;
				return false;
			}
			const uint32_t id = texture_of(object);
			if (id == fx::kNone)
			{
				g.n.texture++;
				return false;
			}
			// The weights (03F8A0)
			float weights[5], last[5], w[5];
			std::memcpy(weights, reinterpret_cast<const void *>(e + 0x194), sizeof(weights));
			std::memcpy(last, reinterpret_cast<const void *>(e + 0x1A8), sizeof(last));
			float sum = 0.0f;
			for (int32_t k = 0; k < targets; ++k)
				sum += weights[k];
			float scale = 0.0f;
			bool set_largest = false;
			if (sum < 1.1754944e-38f)
			{
				int32_t largest = 0;
				for (int32_t k = 1; k < targets; ++k)
					if (last[largest] < last[k])
						largest = k;
				weights[largest] = 1.0f;
				set_largest = true;
				scale = 1.0f;
			}
			else
				scale = 1.0f / sum;
			for (int32_t k = 0; k < targets; ++k)
				w[k] = scale * weights[k];
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out) || !lit_ok(e, out))
				return false;
			// The vertices: each corner's position and normal blended as 03F8A0 blends them
			const uintptr_t pi = data + index_at, ni = pi + uintptr_t(n) * 2, colours = data + colour_at, uvs = data + uv_at;
			for (uint32_t i = 0; i < n; ++i)
			{
				const uint32_t a = at<uint16_t>(pi + 2 * uintptr_t(i)), b = at<uint16_t>(ni + 2 * uintptr_t(i));
				if (a >= uint32_t(positions) || b >= uint32_t(normals))
				{
					g.n.unsupported++;
					return false;
				}
				fx::vertex &v = f.vertices[f.vertex_count + i];
				for (int c = 0; c < 3; ++c)
					v.pos[c] = v.normal[c] = 0.0f;
				for (int32_t k = 0; k < targets; ++k)
				{
					const uintptr_t target = data + 0x20 + stride * uintptr_t(k);
					for (int c = 0; c < 3; ++c)
						v.pos[c] = at<float>(target + 12 * uintptr_t(a) + 4 * uintptr_t(c)) * w[k] + v.pos[c];
					const uint32_t packed = at<uint32_t>(target + 12 * uintptr_t(positions) + 4 * uintptr_t(b));
					v.normal[0] = float(int32_t(packed << 22) >> 22) * w[k] * 0.001953125f + v.normal[0];
					v.normal[1] = float(int32_t(packed << 12) >> 22) * w[k] * 0.001953125f + v.normal[1];
					v.normal[2] = float(int32_t(packed << 2) >> 22) * w[k] * 0.001953125f + v.normal[2];
				}
				const uint32_t c = at<uint32_t>(colours + 4 * uintptr_t(i));
				uint32_t doubled = 0;
				for (int k = 0; k < 4; ++k)
					doubled |= std::min(((c >> (8 * k)) & 0xFFu) * 2u, 0xFFu) << (8 * k);
				v.colour = doubled;
				v.uv[0] = at<float>(uvs + 8 * uintptr_t(i));
				v.uv[1] = at<float>(uvs + 8 * uintptr_t(i) + 4);
			}
			// (03FBB0's side effects: the blend handed in cleared, this frame's weights kept as the last)
			if (set_largest)
				std::memcpy(reinterpret_cast<void *>(e + 0x194), weights, sizeof(weights));
			std::memcpy(reinterpret_cast<void *>(e + 0x1A8), reinterpret_cast<const void *>(e + 0x194), 20);
			*reinterpret_cast<uint32_t *>(e + 0x1BC) = 0;
			out.first_draw = f.draw_count;
			out.draws = 1;
			out.cls = fx::class_morph;
			f.draws[f.draw_count++] = { f.vertex_count, n, id, fx::stage_texture | ((head & 0x80) != 0 ? fx::stage_alpha_test : 0u) };
			f.vertex_count += n;
			f.element_count++;
			g.n.taken[fx::class_morph]++;
			return true;
		}

		// A weapon trail (CYyAfterImage, its flush callback 01C930 -> 01C940) into the frame; false: the game's. Its
		// ring of 80 samples (head +0x14A8, count +0x14AC): tip +0x40[i], base +0x540[i] (x y z w, the world's), colour
		// A +0xA40[i] and B +0xF40[i] (r g b a floats, 0..255, truncated into a D3DCOLOR). From the newest back: +0x151C 0
		// one strip of (tip, A), (base, B) pairs; else two from the middle line (tip + base) / 2: (mid, A), (tip, A) and
		// (mid, A), (base, B). WORLD identity (VIEW the camera's), FVF 0x42, colour and alpha the vertex's, LIGHTING 0,
		// FOGENABLE 0, Z test on, ZWRITEENABLE 0, SRCALPHA / ONE, CULL NONE.
		bool record_trail(uintptr_t t)
		{
			fx::frame &f = *g.f;
			const int32_t head = at<int32_t>(t + 0x14A8), count = at<int32_t>(t + 0x14AC);
			const uint32_t mode = at<uint32_t>(t + 0x151C);
			if (count <= 1 || count > 80 || head < 0 || head > 80)
			{
				g.n.empty++;
				return true; // (fewer than two samples: no triangle; 01C940 draws 2 x count - 2 of them)
			}
			const uint32_t strips = mode != 0 ? 2 : 1;
			const uint32_t n = strips * (2 * uint32_t(count) - 2) * 3;
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + n > fx::kMaxVertices ||
				f.runs.load(std::memory_order_relaxed) + 2 >= fx::kMaxRuns)
			{
				g.n.full++;
				return false;
			}
			const auto colour = [&](uintptr_t c) {
				const auto b = [&](uintptr_t o) { return uint32_t(int32_t(at<float>(c + o))); };
				return b(0xC) << 24 | b(0) << 16 | b(4) << 8 | b(8);
			};
			fx::vertex strip[2][160];
			for (int32_t k = 0, i = head - 1; k < count; ++k, --i)
			{
				if (i < 0)
					i += 80;
				const uintptr_t tip = t + 0x40 + 16 * uintptr_t(i), base = t + 0x540 + 16 * uintptr_t(i);
				const uint32_t a = colour(t + 0xA40 + 16 * uintptr_t(i)), b = colour(t + 0xF40 + 16 * uintptr_t(i));
				float pt[3], pb[3], mid[3];
				std::memcpy(pt, reinterpret_cast<const void *>(tip), 12);
				std::memcpy(pb, reinterpret_cast<const void *>(base), 12);
				for (int c = 0; c < 3; ++c)
					mid[c] = (pt[c] + pb[c]) * 0.5f;
				const auto put = [](fx::vertex &v, const float *p, uint32_t col) {
					v = fx::vertex();
					std::memcpy(v.pos, p, 12);
					v.colour = col;
				};
				if (mode == 0)
				{
					put(strip[0][2 * k], pt, a);
					put(strip[0][2 * k + 1], pb, b);
				}
				else
				{
					put(strip[0][2 * k], mid, a);
					put(strip[0][2 * k + 1], pt, a);
					put(strip[1][2 * k], mid, a);
					put(strip[1][2 * k + 1], pb, b);
				}
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			std::memcpy(out.world, f.view, 64);
			out.blend = fx::blend_add;
			out.z_write = 0;
			out.fog = 0;
			out.projection = fx::proj_camera;
			uint32_t w = f.vertex_count;
			for (uint32_t s = 0; s < strips; ++s)
				for (int32_t k = 0; k + 2 < 2 * count; ++k)
					for (int j = 0; j < 3; ++j)
						f.vertices[w++] = strip[s][k + j];
			out.first_draw = f.draw_count;
			out.draws = 1;
			out.cls = fx::class_trail;
			f.draws[f.draw_count++] = { f.vertex_count, n, fx::kNone, fx::stage_diffuse };
			f.vertex_count += n;
			f.element_count++;
			g.n.taken[fx::class_trail]++;
			return true;
		}

		bool record_trail_guarded(uintptr_t t)
		{
			__try
			{
				return pointer(uint32_t(t)) && record_trail(t);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g.n.faulted++;
				return false;
			}
		}

		// Procedural rings (kind 0x24, 0565A0) into the frame; false: the game's. +0x1B4 segments, +0x1B5 rings (signed
		// bytes), radii +0x194 + 4i, colours +0x1A4 + 4i (bytes R G B A, made a D3DCOLOR by 03E280). Each band between
		// two rings is a strip in the element's XY plane of (sin a x r, cos a x r, 0) pairs, inner then outer, at angles
		// 0, 0, d, 2d .. (n - 2) d, then the first pair again (d = 2 pi / n): DrawPrimitive(TRIANGLESTRIP, 2n) per band,
		// FVF XYZ | DIFFUSE, table 353810 (untextured), TEXTUREFACTOR the element's own colour +0xF8 (not 046070's: no
		// fade, no brightness, no letter change).
		bool record_ring(uintptr_t e)
		{
			fx::frame &f = *g.f;
			const int32_t segments = at<int8_t>(e + 0x1B4), rings = at<int8_t>(e + 0x1B5);
			if (segments <= 0 || rings <= 1)
			{
				g.n.empty++;
				return true;
			}
			if (rings > 4)
			{
				g.n.unsupported++;
				return false;
			}
			const uint32_t bands = uint32_t(rings - 1), per_band = uint32_t(segments) * 2 * 3;
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + bands * per_band > fx::kMaxVertices)
			{
				g.n.full++;
				return false;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out) || !lit_ok(e, out))
				return false;
			out.tfactor = at<uint32_t>(e + 0xF8);
			const float step = 6.283184f / float(segments);
			uint32_t w = f.vertex_count;
			for (uint32_t b = 0; b < bands; ++b)
			{
				const float r[2] = { at<float>(e + 0x194 + 4 * b), at<float>(e + 0x194 + 4 * (b + 1)) };
				uint32_t col[2];
				for (int k = 0; k < 2; ++k)
				{
					const uint32_t c = at<uint32_t>(e + 0x1A4 + 4 * (b + k));
					col[k] = (c & 0xFF000000u) | (c & 0xFF) << 16 | (c & 0xFF00) | ((c >> 16) & 0xFF);
				}
				// (the strip's 2n + 2 points: angle 0, then 0, d .. (n - 2) d, then angle 0 again)
				fx::vertex strip[2 * 128 + 2];
				const uint32_t points = uint32_t(segments) + 1;
				if (points > 129)
				{
					g.n.unsupported++;
					return false;
				}
				for (uint32_t p = 0; p < points; ++p)
				{
					const float a = p == 0 || p + 1 == points ? 0.0f : float(p - 1) * step;
					for (int k = 0; k < 2; ++k)
					{
						fx::vertex &v = strip[2 * p + k];
						v = fx::vertex();
						v.pos[0] = std::sin(a) * r[k];
						v.pos[1] = std::cos(a) * r[k];
						v.colour = col[k];
					}
				}
				for (uint32_t k = 0; k + 2 < 2 * points; ++k)
					for (uint32_t j = 0; j < 3; ++j)
						f.vertices[w++] = strip[k + j];
			}
			const uint32_t n = w - f.vertex_count;
			out.first_draw = f.draw_count;
			out.draws = 1;
			out.cls = fx::class_ring;
			f.draws[f.draw_count++] = { f.vertex_count, n, fx::kNone, 0 };
			f.vertex_count = w;
			f.element_count++;
			g.n.taken[fx::class_ring]++;
			return true;
		}

		// An element into the frame: true when it's taken (the game's setup and draw skipped)
		bool record(uintptr_t e)
		{
			const uintptr_t vt = at<uint32_t>(e);
			if (vt == 0)
				return false;
			const uintptr_t cls = at<uint32_t>(vt + 0x48);
			if (g.f->runs.load(std::memory_order_relaxed) + 2 >= fx::kMaxRuns)
			{
				g.n.full++;
				return false;
			}
			if (cls == g.d3a_draw && g.d3a_draw != 0)
			{
				const uint32_t handle = at<uint32_t>(e + kModel);
				if (handle == 0)
				{
					g.n.empty++;
					return true;
				}
				const uint32_t resource = at<uint32_t>(handle);
				if (resource == 0)
				{
					g.n.unsupported++;
					return false;
				}
				return record_d3a(e, resource);
			}
			if (cls == g.ring_draw && g.ring_draw != 0)
				return record_ring(e);
			if (cls == g.vuanm_draw && g.vuanm_draw != 0)
			{
				const uint32_t handle = at<uint32_t>(e + kModel);
				if (handle == 0)
				{
					g.n.empty++;
					return true; // (03F130: no model, nothing drawn)
				}
				const uint32_t resource = at<uint32_t>(handle);
				if (resource == 0)
				{
					g.n.unsupported++;
					return false;
				}
				return record_vuanm(e, resource);
			}
			if (cls == g.morph_draw && g.morph_draw != 0)
			{
				// (03FBB0: a weather element 002D80 hides, or no model: nothing drawn)
				const uint32_t gh = at<uint32_t>(e + kGen);
				const uint32_t gen = gh != 0 ? at<uint32_t>(gh) : 0;
				const uint32_t handle = at<uint32_t>(e + kModel);
				if ((gen != 0 && (at<uint8_t>(gen + 0xDE) & 0x80) != 0 && reinterpret_cast<weather_fn>(g.weather)() == 0.0f) || handle == 0)
				{
					g.n.empty++;
					return true;
				}
				const uint32_t resource = at<uint32_t>(handle);
				if (resource == 0)
				{
					g.n.unsupported++;
					return false;
				}
				return record_morph(e, resource);
			}
			if (cls == g.d3m_draw)
			{
				const uint32_t handle = at<uint32_t>(e + kModel);
				if (handle == 0)
				{
					g.n.empty++;
					return true;
				}
				const uint32_t gh = at<uint32_t>(e + kGen);
				const uint32_t gen = gh != 0 ? at<uint32_t>(gh) : 0;
				if (gen != 0 && (at<uint8_t>(gen + 0xDE) & 0x80) != 0 && reinterpret_cast<weather_fn>(g.weather)() == 0.0f)
				{
					g.n.empty++;
					return true;
				}
				// (an MMB: +0x104 is the mesh itself, 049570 having kept 183420's answer)
				if (at<uint8_t>(e + kMmb) != 0)
					return record_mmb(e, handle);
				const uint32_t resource = at<uint32_t>(handle);
				if (resource == 0)
				{
					g.n.unsupported++;
					return false;
				}
				return record_d3m(e, resource);
			}
			g.n.unsupported++;
			return false;
		}

		// record() under a guard: an element whose memory faults is the game's (nothing of it kept: the frame's counts
		// move only once it's all read)
		bool record_guarded(uintptr_t e)
		{
			__try
			{
				return record(e);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g.n.faulted++;
				return false;
			}
		}

		// Whether a queued element is drawn through 046070 (its vtable's +0x2C), under a guard
		bool drawn_by_046070(uintptr_t e)
		{
			__try
			{
				if (!pointer(uint32_t(e)))
					return false;
				const uint32_t vt = at<uint32_t>(e);
				return vt != 0 && at<uint32_t>(vt + 0x2C) == g.element_draw;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void put_call(uint8_t *at_, uintptr_t from, uintptr_t to)
		{
			at_[0] = 0xE8;
			put32(at_ + 1, uint32_t(to - (from + 5)));
		}
	}

	void __cdecl on_flush(uint32_t end)
	{
		if (end == 0)
		{
			g.in_flush = true;
			g.n.flushes++;
			if (++g.seq == 0)
				g.seq = 1;
			g.f = &fx::frame_slot(g.seq);
			g.f->runs.store(0, std::memory_order_relaxed);
			g.f->element_count = g.f->draw_count = g.f->vertex_count = g.f->light_count = 0;
			g.run_first = 0;
			const uintptr_t renderer = word(g.renderer);
			g.can_mark = g.wanted && markers_possible();
			g.taking = g.native && g.can_mark && renderer != 0 && word(g.camera) != 0;
			if (renderer != 0)
				std::memcpy(g.f->proj, reinterpret_cast<const void *>(renderer + 0x38DDC), 64);
			if (const uintptr_t gfx = word(g.gfx); gfx != 0)
				std::memcpy(g.f->view, reinterpret_cast<const void *>(gfx + 0xB50), 64);
			g.f->seq.store(g.seq, std::memory_order_release);
			return;
		}
		// The last run; an empty one for the textures waiting to be copied in, or as proof that markers come through
		close_run();
		if (g.f != nullptr && g.f->runs.load(std::memory_order_relaxed) == 0 && (g.probe || fx::textures::waiting() != 0))
			close_run(true);
		if (g.f != nullptr)
		{
			g.n.last_elements = g.f->element_count;
			g.n.last_runs = g.f->runs.load(std::memory_order_relaxed);
		}
		g.in_flush = false;
		g.taking = false;
		g.f = nullptr;
	}

	void __cdecl on_entry(uintptr_t callback, uintptr_t arg)
	{
		if (g.in_flush && g.taking)
		{
			// (a weapon trail: drawn by HiFi XI with the run, else the game's after it)
			if (callback == g.trail_cb && g.trail_cb != 0 && record_trail_guarded(arg))
				return;
			// (anything but an element drawn through 046070 draws now: the run so far first)
			if (callback != g.element_cb || !drawn_by_046070(arg))
				close_run();
		}
		reinterpret_cast<callback_fn>(callback)(arg);
	}

	uint32_t __fastcall on_setup(uintptr_t element, uintptr_t, uint32_t arg)
	{
		if (g.in_flush && g.taking && arg == 0xFFFF)
		{
			if (record_guarded(element))
				return 1;
			close_run();
		}
		else if (g.in_flush)
			g.n.game++;
		return reinterpret_cast<setup_fn>(g.setup)(element, 0, arg);
	}

	bool start(std::string &why)
	{
		if (g.in)
			return true;
		const uintptr_t s = find(kSetup), fl = find(kFlush), walk = find(kWalk), entry = find(kEntry), cb = find(kElementCallback), d3m = find(kD3mDraw), fog = find(kFog);
		if (s == 0 || fl == 0 || walk == 0 || entry == 0 || cb == 0 || d3m == 0 || fog == 0)
		{
			why = "the effects' flush, walk or element draw wasn't found";
			return false;
		}
		const uintptr_t setup_call = s + kSetupCallAt, flush_call = fl + kFlushCallAt, entry_site = entry + kEntryAt, element_draw = s - kElementDrawFromSetup;
		uint8_t now_setup[5] = {}, now_flush[5] = {}, now_entry[7] = {};
		if (!read(setup_call, now_setup, 5) || now_setup[0] != 0xE8 || !read(flush_call, now_flush, 5) || now_flush[0] != 0xE8 || rel_target(flush_call) != walk ||
			!read(entry_site, now_entry, 7) || std::memcmp(now_entry, kEntryBytes, 7) != 0 || !code_is(element_draw, "\x83\xEC\x50\x8D\x44\x24\x0C\x55\x8B\xE9\x51", 11))
		{
			why = "the effects' flush or element draw isn't laid out as expected (another patch there?)";
			return false;
		}
		// The globals and functions the recording reads and calls (each checked by its opcode first)
		uint8_t op[2] = {};
		const auto opcode = [&](uintptr_t a, uint8_t b0, int b1) { return read(a, op, 2) && op[0] == b0 && (b1 < 0 || op[1] == b1); };
		if (!opcode(s + kSetupGfx, 0x8B, 0x0D) || !opcode(s + kSetupCamera, 0xA1, -1) || !opcode(s + kSetupRenderer, 0x8B, 0x0D) || !opcode(s + kSetupMatrixCall, 0xE8, -1) ||
			!opcode(d3m + kD3mColour, 0x68, -1) || !opcode(d3m + kD3mWeatherCall, 0xE8, -1) || !opcode(fog + kFogCall, 0xE8, -1))
		{
			why = "the effects' globals aren't where the element draw reads them";
			return false;
		}
		state n;
		n.setup_call = setup_call;
		n.setup = rel_target(setup_call);
		n.flush_call = flush_call;
		n.walk = walk;
		n.entry_site = entry_site;
		n.element_cb = cb;
		n.element_draw = element_draw;
		n.d3m_draw = d3m;
		n.d3a_draw = find(kD3aDraw);
		n.trail_cb = find(kTrailCallback);
		n.ring_draw = find(kRingDraw);
		n.morph_draw = find(kMorphDraw);
		{
			uint8_t jmp = 0;
			const uintptr_t to = read(d3m + kD3mMmbJump, jmp) && jmp == 0xE9 ? rel_target(d3m + kD3mMmbJump) : 0;
			n.mmb_draw = to != 0 && code_is(to, kMmbDraw, sizeof(kMmbDraw)) ? to : 0;
		}
		n.draw_matrix = rel_target(s + kSetupMatrixCall);
		n.weather = rel_target(d3m + kD3mWeatherCall);
		n.fog_env = rel_target(fog + kFogCall);
		n.math = word(s + kSetupMath);
		n.gfx = word(s + kSetupGfx + 2);
		n.camera = word(s + kSetupCamera + 1);
		n.renderer = word(s + kSetupRenderer + 2);
		n.colour = word(d3m + kD3mColour + 1);
		n.env = word(fog + kFogEnv);
		{
			// (the lighting's calls in 0467E0, each checked as a call; the material's push)
			const auto call = [&](intptr_t at_) { uint8_t b = 0; return read(n.setup + at_, b) && b == 0xE8 ? uintptr_t(rel_target(n.setup + at_)) : uintptr_t(0); };
			n.frame_of = call(kFrameOfCall);
			n.attach = call(kAttachCall);
			n.fix = call(kFixCall);
			n.light1 = call(kLightCall);
			n.lights2 = call(kLightsCall);
			n.lights2_default = call(kLightsDefaultCall);
			n.ambient = call(kAmbientCall);
			uint8_t push = 0;
			n.material = read(n.setup + kMaterialPush, push) && push == 0x68 ? word(n.setup + kMaterialPush + 1) : 0;
		}
		if (const uintptr_t v = find(kVuAnmDraw); v != 0)
		{
			// (03F130's table, globals, callback and walk, each by its opcode; its attach call must be 0467E0's)
			uint8_t b[3] = {};
			const bool ok = read(v + kVuTable, b, 3) && b[0] == 0x0F && b[1] == 0xBE && b[2] == 0xB1 && read(v + kVuAttachStore, b, 2) && b[0] == 0x89 && b[1] == 0x3D &&
				read(v + kVuCallback, b, 1) && b[0] == 0x68 && read(v + kVuCursorStore, b, 1) && b[0] == 0xA3 && read(v + kVuWalkCall, b, 1) && b[0] == 0xE8 &&
				read(v + kVuAttachCall, b, 1) && b[0] == 0xE8 && rel_target(v + kVuAttachCall) == n.attach && code_is(rel_target(v + kVuWalkCall), kVuWalk, sizeof(kVuWalk));
			if (ok)
			{
				n.vuanm_draw = v;
				n.vu_table = word(v + kVuTable + 3);
				n.vu_attach = word(v + kVuAttachStore + 2);
				n.vu_callback = word(v + kVuCallback + 1);
				n.vu_cursor = word(v + kVuCursorStore + 1);
				n.vu_walk = rel_target(v + kVuWalkCall);
			}
		}
		if (g.code == 0)
			g.code = alloc_code(kCodeSize);
		if (g.code == 0)
		{
			why = "no memory for the stubs";
			return false;
		}
		n.code = g.code;
		uint8_t c[kCodeSize] = {
			0x51, 0x6A, 0x00, 0, 0, 0, 0, 0, 0x83, 0xC4, 0x04, 0x59, // push ecx; push 0; call on_flush; add esp, 4; pop ecx
			0, 0, 0, 0, 0,                                           // call the walk
			0x6A, 0x01, 0, 0, 0, 0, 0, 0x83, 0xC4, 0x04,             // push 1; call on_flush; add esp, 4
			0xC3 };                                                  // ret
		const uint8_t stub[] = {
			0x50,             // push eax (the argument)
			0xFF, 0x76, 0x08, // push [esi+8] (the callback)
			0, 0, 0, 0, 0,    // call on_entry
			0x83, 0xC4, 0x08, // add esp, 8
			0xC3 };           // ret
		std::memcpy(c + kEntryStubAt, stub, sizeof(stub));
		put_call(c + kFlushMark1, g.code + kFlushMark1, reinterpret_cast<uintptr_t>(&on_flush));
		put_call(c + kFlushWalk, g.code + kFlushWalk, walk);
		put_call(c + kFlushMark2, g.code + kFlushMark2, reinterpret_cast<uintptr_t>(&on_flush));
		put_call(c + kEntryCall, g.code + kEntryCall, reinterpret_cast<uintptr_t>(&on_entry));
		if (!write_code(g.code, c, kCodeSize))
		{
			why = "couldn't write the stubs";
			return false;
		}
		put_call(n.ours_setup, setup_call, reinterpret_cast<uintptr_t>(&on_setup));
		put_call(n.ours_flush, flush_call, g.code);
		put_call(n.ours_entry, entry_site, g.code + kEntryStubAt);
		n.ours_entry[5] = n.ours_entry[6] = 0x90;
		std::memcpy(n.saved_setup, now_setup, 5);
		std::memcpy(n.saved_flush, now_flush, 5);
		// (the walk's stub first, then the flush's, then the setup: each works without the ones after it)
		if (!write_code(entry_site, n.ours_entry, 7))
		{
			why = "couldn't write the game's code";
			return false;
		}
		if (!write_code(flush_call, n.ours_flush, 5))
		{
			write_code(entry_site, kEntryBytes, 7);
			why = "couldn't write the game's code";
			return false;
		}
		if (!write_code(setup_call, n.ours_setup, 5))
		{
			write_code(flush_call, now_flush, 5);
			write_code(entry_site, kEntryBytes, 7);
			why = "couldn't write the game's code";
			return false;
		}
		n.native = g.native;
		n.wanted = g.wanted;
		n.probe = g.probe;
		n.clock = g.clock;
		n.device_override = g.device_override;
		n.sink = g.sink;
		n.in = true;
		const counts kept = g.n;
		g = n;
		g.n = kept;
		return true;
	}

	bool stop(std::string &why)
	{
		if (!g.in)
			return true;
		if (!code_is(g.setup_call, g.ours_setup, 5) || !code_is(g.flush_call, g.ours_flush, 5) || !code_is(g.entry_site, g.ours_entry, 7))
		{
			why = "something else has changed the effects' flush or element draw since: left as they are";
			return false;
		}
		if (!write_code(g.setup_call, g.saved_setup, 5) || !write_code(g.flush_call, g.saved_flush, 5) || !write_code(g.entry_site, kEntryBytes, 7))
		{
			why = "couldn't write the game's code";
			return false;
		}
		// (on the game's thread, between frames: no flush running)
		free_code(g.code);
		state n;
		n.device_override = g.device_override;
		n.sink = g.sink;
		n.native = g.native;
		n.wanted = g.wanted;
		n.n = g.n;
		g = n;
		fx::clear_frames();
		return true;
	}

	bool in()
	{
		return g.in;
	}

	void set_native(bool on)
	{
		g.native = on;
	}

	void set_wanted(bool wanted)
	{
		g.wanted = wanted;
	}

	void set_probe(bool on)
	{
		g.probe = on;
	}

	void set_clock(double now)
	{
		g.clock = now;
	}

	void set_zones(zone::gpu_zone *zone, zone::gpu_zone *second)
	{
		if (zone != g_zones.data[0] || second != g_zones.data[1])
			g_zones.of.clear();
		g_zones.data[0] = zone;
		g_zones.data[1] = second;
	}

	counts get_counts()
	{
		return g.n;
	}

	std::string status()
	{
		if (!g.in)
			return "effects: drawn by the game";
		uint64_t taken = 0;
		for (const uint64_t t : g.n.taken)
			taken += t;
		char line[800];
		std::snprintf(line, sizeof(line),
			"effects: %s; %llu flushes, %llu elements drawn by HiFi XI (D3M %llu, MMB meshes %llu, D3A %llu, VuAnm %llu, morphs %llu, rings %llu, weapon trails %llu; %llu lit), %llu with nothing to draw, "
			"%llu water meshes World FX hides; the last flush %u elements in %u runs; "
			"left to the game: %llu (not taking), %llu (class not handled), %llu (lit, its lights not to be had), %llu (blend letter), %llu (texture not up yet), %llu (no room), %llu (unreadable), "
			"%llu (untextured D3M, its first vertex overwritten), %llu (MMB mesh not in the zone's data); "
			"%llu markers sent%s",
			g.native ? "HiFi XI draws what it can" : g.probe ? "the game draws them (waiting for the markers to come through)" : "the game draws them",
			static_cast<unsigned long long>(g.n.flushes), static_cast<unsigned long long>(taken), static_cast<unsigned long long>(g.n.taken[fx::class_d3m]),
			static_cast<unsigned long long>(g.n.taken[fx::class_mmb]), static_cast<unsigned long long>(g.n.taken[fx::class_d3a]), static_cast<unsigned long long>(g.n.taken[fx::class_vuanm]),
			static_cast<unsigned long long>(g.n.taken[fx::class_morph]),
			static_cast<unsigned long long>(g.n.taken[fx::class_ring]), static_cast<unsigned long long>(g.n.taken[fx::class_trail]),
			static_cast<unsigned long long>(g.n.lit_taken), static_cast<unsigned long long>(g.n.empty), static_cast<unsigned long long>(g.n.water), g.n.last_elements, g.n.last_runs, static_cast<unsigned long long>(g.n.game),
			static_cast<unsigned long long>(g.n.unsupported), static_cast<unsigned long long>(g.n.lit), static_cast<unsigned long long>(g.n.letter),
			static_cast<unsigned long long>(g.n.texture), static_cast<unsigned long long>(g.n.full), static_cast<unsigned long long>(g.n.faulted),
			static_cast<unsigned long long>(g.n.overwritten), static_cast<unsigned long long>(g.n.mesh),
			static_cast<unsigned long long>(g.n.markers), g.n.not_sent != 0 ? (", not sent " + std::to_string(g.n.not_sent)).c_str() : "");
		return line;
	}

	sites where()
	{
		sites s;
		s.setup_call = g.setup_call;
		s.setup = g.setup;
		s.flush_call = g.flush_call;
		s.walk = g.walk;
		s.entry_site = g.entry_site;
		s.element_cb = g.element_cb;
		s.element_draw = g.element_draw;
		s.d3m_draw = g.d3m_draw;
		s.draw_matrix = g.draw_matrix;
		s.fog_env = g.fog_env;
		s.d3a_draw = g.d3a_draw;
		s.trail_cb = g.trail_cb;
		s.ring_draw = g.ring_draw;
		s.mmb_draw = g.mmb_draw;
		s.morph_draw = g.morph_draw;
		s.vuanm_draw = g.vuanm_draw;
		s.vu_walk = g.vu_walk;
		s.vu_cursor = g.vu_cursor;
		s.frame_of = g.frame_of;
		s.light1 = g.light1;
		s.lights2 = g.lights2;
		s.ambient = g.ambient;
		s.material = g.material;
		s.colour = g.colour;
		s.gfx = g.gfx;
		s.renderer = g.renderer;
		s.camera = g.camera;
		s.env = g.env;
		s.math = g.math;
		s.code = g.in ? g.code : 0;
		return s;
	}

	void set_sink(void (*sink)(uint32_t, uint32_t))
	{
		g.sink = sink;
	}

	void set_device(uintptr_t device)
	{
		g.device_override = device;
	}

	void set_setup(uintptr_t setup)
	{
		g.setup = setup;
	}
}
