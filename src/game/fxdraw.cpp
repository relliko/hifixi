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
 *   0426F0 / 043110 (CMoDist, CMoVuDist), and the 042D00 distortions 043320 / 043590 / 043720 (a D3M, a morph, rings
 *     drawn with a copy of the world): record_dist, record_dist_model, record_morph's dist, record_dist_ring below.
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
		// 0426F0 (CMoDist): mov eax, [VA 048E666C] at +0x19F (the app: +0x194 the world's texture); 043110 (CMoVuDist)
		constexpr const char *kDistDraw = "81ECA40000005355568BF18A0D????????B00184C85789742448";
		constexpr intptr_t kDistApp = 0x19F;
		// 0548D0 (CMoLfdElem's own draw): its call of 03EBF0 at +0x2B1 (054B81); 075690: mov ecx, [VA 0490B970] at +8
		constexpr const char *kFlareDraw = "83EC3C8D442410568BF15751D98638010000D88E34010000";
		constexpr intptr_t kFlareCallAt = 0x2B1;
		constexpr const char *kFlarePieces = "83EC2855568BF18B0D????????8974241C0FBE4636";
		constexpr const char *kTextureByName = "538BD9833B0075488B0D????????55565733ED";
		constexpr intptr_t kTextureManager = 0xA;
		constexpr const char *kVuDistDraw = "51A1????????538BD9558B88900A000056578BAC88100A0000418988900A00008BCBE8";
		// The 042D00 distortions: CMoDistModelElem 043320, CMoDistMorphElem 043590, CMoDistRingElem 043720
		constexpr const char *kDistModelDraw = "83EC1C535556578BF9897C24148B87FC00000085C074048B00EB0233C08A88DE00000084C97916E8";
		constexpr const char *kDistMorphDraw = "83EC08558BE9568B850401000085C00F84????????85C07408";
		constexpr const char *kDistRingDraw = "83EC2C53558BE956570FBE85B40100008D4C00020FBE85B5010000";
		// The specular D3M 0412B0; 044250 (mov ecx, the global environment 'cubemap spec' VA 0490BB90 at +0xB)
		constexpr const char *kSpecDraw = "83EC1053558BE95733FF39BD040100000F84????????8A857901000084C0";
		constexpr const char *kSpecCubemap = "8B4424088B4C24045051B9????????E8????????C3";
		constexpr intptr_t kSpecCubemapAt = 0xB;
		// The battle numbers: 041AF0; in 041C20 the glyph bind (call 1207B0 at +0xB, the font object mov ecx, imm at +6) and
		// draw (call 00CD00 at +0xE)
		constexpr const char *kNumberDraw = "83EC4056578BF98B87FC00000085C074048B00EB0233C08D4830E8";
		constexpr const char *kGlyphBind = "0FBE14295052B9????????E8????????84C074548B5424288D8424D4000000B904000000";
		constexpr const char *kGlyphDraw = "8D9424C40000006A1C526A026A05E8????????8B442414";
		constexpr intptr_t kGlyphBindAt = 0xB, kGlyphDrawAt = 0xE;
		// The light spots: 040E70 (vtable 0x32B3B8's class draw), its calls of 054D70 (the 256 x 256 target and the
		// look-at), 044C40 (the element's point) and 054F80 (-> 182700, the ground)
		constexpr const char *kSpotDraw = "83EC28538BD98B830401000085C00F84????????A1????????5556578B88900A0000";
		constexpr intptr_t kSpotTargetCall = 0x4C, kSpotPointCall = 0x88, kSpotGroundCall = 0x107;
		// 054D70: mov eax, [the target VA 0490BB88] (+1); fsub [2.0] (+0x12D); the up vector's z -1 (+0xDC, mov [VA], imm);
		// call 2D7FF9 (+0x156, D3DXMatrixLookAtRH); mov eax, 0.25 (+0x193, the scale)
		constexpr uint8_t kSpotTargetHead[] = { 0xA1 }, kSpotScale[] = { 0xB8, 0x00, 0x00, 0x80, 0x3E };
		constexpr intptr_t kSpotTargetLift = 0x12D, kSpotTargetUp = 0xDC, kSpotTargetLookAt = 0x156, kSpotTargetScale = 0x193;
		// 054F80: mov ecx, [the target] (+0x12); push 10.0 (+0x1C); mov ecx, [the renderer] (+0x2B); call 182700 (+0x31)
		constexpr intptr_t kSpotMapTarget = 0x12, kSpotMapSize = 0x1C, kSpotMapRenderer = 0x2B, kSpotMapCall = 0x31;
		// 182700: its statics (the map's centre VA 04807338/3C/40 at +0x2C8/+0x2CE/+0x2D3, its height scale VA 04807348 at
		// +0x22F), the cells' scale 0.25 (+0x395), lea ecx, [renderer + 0x178] (+0x36A) and call 167940 (+0x370), mov edx,
		// [renderer + 0x17C] (+0x388), cmp eax, 1500 (+0x75E)
		constexpr uint8_t kGroundHead[] = { 0xD9, 0x44, 0x24, 0x10, 0xD8, 0x1D };
		constexpr uint8_t kGroundGrid[] = { 0x8D, 0x8F, 0x78, 0x01, 0x00, 0x00 }, kGroundSkip[] = { 0x8B, 0x97, 0x7C, 0x01, 0x00, 0x00 };
		constexpr uint8_t kGroundLimit[] = { 0x3D, 0xDC, 0x05, 0x00, 0x00 };
		constexpr uint8_t kGridOf[] = { 0x8B, 0x41, 0x60, 0x85, 0xC0, 0x75, 0x03, 0x33, 0xC0, 0xC3, 0x8D, 0x41, 0x08, 0xC3 }; // 167940
		constexpr intptr_t kGroundCentre[3] = { 0x2C8, 0x2CE, 0x2D3 }, kGroundHeight = 0x22F, kGroundCell = 0x395, kGroundGridAt = 0x36A, kGroundGridCall = 0x370,
			kGroundSkipAt = 0x388, kGroundLimitAt = 0x75E;
		constexpr uint32_t kGroundMost = 1501; // (a collision mesh's triangles 182700 draws at most)
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
			uintptr_t dist_draw = 0, vudist_draw = 0, dist_app = 0; // (0426F0, 043110, VA 048E666C)
			uintptr_t distmodel_draw = 0, distmorph_draw = 0, distring_draw = 0; // (043320, 043590, 043720; 0 without 0426F0's)
			uintptr_t spec_draw = 0, spec_cubemap = 0; // (0412B0, VA 0490BB90)
			// (the battle numbers: 041AF0; 041C20's two calls, patched when both are as the game has them, and their targets)
			uintptr_t number_draw = 0, glyph_bind_call = 0, glyph_bind = 0, glyph_draw_call = 0, glyph_draw = 0;
			uint8_t saved_bind[5] = {}, ours_bind[5] = {}, saved_glyph[5] = {}, ours_glyph[5] = {};
			uintptr_t flare_call = 0, flare_pieces = 0, flare_textures = 0; // (054B81 patched when all found; 03EBF0, VA 0490B970)
			uint8_t saved_flare[5] = {}, ours_flare[5] = {};
			// (the light spots: 040E70, 044C40, 2D7FF9, VA 0490BB88; 054D70's 2.0 and 054F80's 10.0; 182700's statics: the map's
			// centre and height scale, the cells' scale; 0 any: the game's)
			uintptr_t spot_draw = 0, spot_point = 0, spot_look_at = 0, spot_target = 0;
			float spot_lift = 0.0f, spot_size = 0.0f;
			uintptr_t spot_centre[3] = {}, spot_height = 0, spot_cell = 0;
			uint32_t fog_raw = 0; // (common()'s fog colour before H's black: the ground's, by its own letter)
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
				g.fog_raw = fog_colour;
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

		// A light spot's ground (182700, R; thiscall on the zone renderer: the point, the 256 x 256 target's texture, the
		// map's size 10.0), as the game makes it, into the frame's vertices after the ones there: a planar map M = T(-p) x
		// S(1 / size, the static VA 04807348 (1.0), 1 / size) x T(the statics VA 04807338/3C/40: 0.5, 0, 0.5) (16C060 three
		// times); the collision grid (167940 on renderer + 0x178: renderer + 0x180 when + 0x1D8 is set, else nothing
		// drawn); its cells round the point: cx = int((grid + 0x38 + p.x) x 0.25), cz the same with + 0x3C and p.z, cx - 1
		// .. cx + 1 and cz - 1 .. cz + 1 each kept in 0 .. grid + 0x40 - 1 (+ 0x44 for z); a cell's list (grid + 0x58)[W
		// z + x] a chain of groups (a dword whose low 11 bits count the (instance, mesh) pairs after it, then the next
		// group); a pair skipped when the instance's + 0xBC is the renderer's + 0x17C or the mesh's word + 0xE is set;
		// WORLD the instance's matrix (its first 64 bytes), C = it x M; each triangle (mesh + 8, four words: corners in the
		// low 15 / 14 / 14 bits, the normal's in the low 15; + 0xC of them) whose normal (mesh + 4) has (C's column 1) <= 0
		// as three vertices (the position (mesh + 0), 0xFF808080, UV = (x, y, z, 1) by C's columns 0 and 2), at most 1501 a
		// pair (the 1502nd is written, then the loop leaves); a mesh in two cells is drawn twice, as the game does. Each
		// position is put through the instance's matrix here (the ground's WORLD is then the camera's VIEW); the sums in
		// the game's order. False: no room.
		bool spot_ground(const float p[3], uint32_t &added)
		{
			fx::frame &f = *g.f;
			added = 0;
			const uintptr_t renderer = at<uint32_t>(g.renderer);
			if (renderer == 0 || at<uint32_t>(renderer + 0x178 + 0x60) == 0)
				return true;
			const uintptr_t grid = renderer + 0x178 + 8;
			// (16C060: out = a x b, each sum from 0 in the order k = 0 .. 3)
			const auto multiply = [](const float *a, const float *b, float *out) {
				float r[16];
				for (int i = 0; i < 4; ++i)
					for (int j = 0; j < 4; ++j)
					{
						float s = 0.0f;
						for (int k = 0; k < 4; ++k)
							s += a[4 * i + k] * b[4 * k + j];
						r[4 * i + j] = s;
					}
				std::memcpy(out, r, sizeof(r));
			};
			const float inv = g.spot_size != 0.0f ? 1.0f / g.spot_size : 1.0f;
			float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
			float t[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -p[0], -p[1], -p[2], 1 };
			multiply(m, t, m);
			const float s[16] = { inv, 0, 0, 0, 0, at<float>(g.spot_height), 0, 0, 0, 0, inv, 0, 0, 0, 0, 1 };
			multiply(m, s, m);
			const float c[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, at<float>(g.spot_centre[0]), at<float>(g.spot_centre[1]), at<float>(g.spot_centre[2]), 1 };
			multiply(m, c, m);
			const float cell = at<float>(g.spot_cell);
			const int32_t cx = int32_t((at<float>(grid + 0x38) + p[0]) * cell), cz = int32_t((at<float>(grid + 0x3C) + p[2]) * cell);
			const int32_t w = at<int32_t>(grid + 0x40), h = at<int32_t>(grid + 0x44);
			const auto keep = [](int32_t v, int32_t n) { return v < 0 ? 0 : std::min(v, n - 1); };
			const int32_t x0 = keep(cx - 1, w), x1 = keep(cx + 1, w), z0 = keep(cz - 1, h), z1 = keep(cz + 1, h);
			const uint32_t skip = at<uint32_t>(renderer + 0x17C);
			const uintptr_t cells = at<uint32_t>(grid + 0x58);
			for (int32_t z = z0; z <= z1; ++z)
				for (int32_t x = x0; x <= x1; ++x)
				{
					uintptr_t group = at<uint32_t>(cells + 4 * uintptr_t(w * z + x));
					for (int guard = 0; group != 0 && guard < 65536; ++guard)
					{
						const uint32_t pairs = at<uint32_t>(group) & 0x7FF;
						for (uint32_t k = 0; k < pairs; ++k)
						{
							const uintptr_t inst = at<uint32_t>(group + 4 + 8 * k), mesh = at<uint32_t>(group + 8 + 8 * k);
							if (skip == at<uint32_t>(inst + 0xBC) || at<uint16_t>(mesh + 0xE) != 0)
								continue;
							float W[16], C[16];
							std::memcpy(W, reinterpret_cast<const void *>(inst), 64);
							multiply(W, m, C);
							const uintptr_t pos = at<uint32_t>(mesh), nrm = at<uint32_t>(mesh + 4), tri = at<uint32_t>(mesh + 8);
							const int32_t count = at<int16_t>(mesh + 0xC);
							uint32_t drawn = 0;
							for (int32_t i = 0; i < count && drawn < kGroundMost; ++i)
							{
								const uintptr_t tw = tri + 8 * uintptr_t(i);
								const uintptr_t n = nrm + 12 * uintptr_t(at<uint16_t>(tw + 6) & 0x7FFF);
								const float d = (C[9] * at<float>(n + 8) + C[5] * at<float>(n + 4)) + C[1] * at<float>(n);
								if (d > 0.0f)
									continue;
								if (f.vertex_count + added + 3 > fx::kMaxVertices)
									return false;
								const uint32_t corner[3] = { at<uint16_t>(tw) & 0x7FFFu, at<uint16_t>(tw + 2) & 0x3FFFu, at<uint16_t>(tw + 4) & 0x3FFFu };
								for (int v = 0; v < 3; ++v)
								{
									const uintptr_t q = pos + 12 * uintptr_t(corner[v]);
									const float px = at<float>(q), py = at<float>(q + 4), pz = at<float>(q + 8);
									fx::vertex &o = f.vertices[f.vertex_count + added + uint32_t(v)];
									o = fx::vertex();
									for (int k2 = 0; k2 < 3; ++k2)
										o.pos[k2] = px * W[k2] + py * W[4 + k2] + pz * W[8 + k2] + W[12 + k2];
									o.colour = 0xFF808080u;
									// (the first corner's sums x, z, y; the others' z, y, x: as 182700 adds them)
									if (v == 0)
									{
										o.uv[0] = ((C[0] * px + C[8] * pz) + C[4] * py) + C[12];
										o.uv[1] = ((C[2] * px + C[10] * pz) + C[6] * py) + C[14];
									}
									else
									{
										o.uv[0] = ((C[8] * pz + C[4] * py) + C[0] * px) + C[12];
										o.uv[1] = ((C[10] * pz + C[6] * py) + C[2] * px) + C[14];
									}
								}
								added += 3;
								drawn++;
							}
						}
						group = at<uint32_t>(group + 4 + 8 * uintptr_t(pairs));
					}
				}
			return true;
		}

		// A light spot (vtable 0x32B3B8, its class draw 040E70; R) into the frame; false: the game's. 040E70: no model,
		// nothing. WORLD = the local matrix +0x60 (06A480); 054D70: the 256 x 256 target (VA 0490BB88: +4 its texture, +8
		// its surface, +0xC its own depth; none: it answers 0 and the D3M would go to the world, so the game's) made the
		// render target and cleared (colour 0, z 1.0), VIEW and PROJECTION pushed, the viewport 256 x 256, VIEW =
		// D3DXMatrixLookAtRH(eye = WORLD's translation moved by -2.0 in y, at = that translation, up (0, 0, -1)) (2D7FF9,
		// called here), WORLD = scale 0.25 x WORLD (math vt+0x24, called here); then the D3M as 040220 draws it (record_d3m:
		// its states the first 0467E0's, PROJECTION the element's own) into that target; an MMB (040C20) or a lit element
		// (its lights would be the look-at's) left to the game. 0472F0, the transforms and the target put back, then 0467E0
		// again with vt+0x3C's letter (+0x198: the ground's blend, fog black for H; z write and PROJECTION as before);
		// 054F80: VIEW = the camera's (gfx +0xB50), 182700 on the element's point (044C40, called here): spot_ground. The
		// ground's texture stages are the last the D3M left (TEXTUREFACTOR too: 0467E0 doesn't set it), stage 0 the target
		// with ADDRESS CLAMP. Recorded as two elements: the D3M into the light-spot target (into_spot), then the ground
		// with it (kSpotTexture). A D3M with no vertex buffer (040220 sets no states then) is the game's.
		using letter_fn = uint32_t(__fastcall *)(uintptr_t element, uintptr_t edx);
		using point_of_fn = float *(__fastcall *)(uintptr_t element, uintptr_t edx, float *out);
		using look_at_fn = float *(__stdcall *)(float *out, const float *eye, const float *at, const float *up);

		bool record_spot(uintptr_t e)
		{
			fx::frame &f = *g.f;
			const uint32_t handle = at<uint32_t>(e + kModel);
			if (handle == 0)
			{
				g.n.empty++;
				return true;
			}
			const uintptr_t target = at<uint32_t>(g.spot_target), math = at<uint32_t>(g.math);
			const uint32_t flags = at<uint32_t>(e + kFlags);
			if (target == 0 || at<uint32_t>(target + 4) == 0 || math == 0 || at<uint8_t>(e + kMmb) != 0)
			{
				g.n.unsupported++;
				return false;
			}
			if ((flags & (1u << 16)) != 0)
			{
				g.n.lit++;
				return false;
			}
			const uint32_t resource = at<uint32_t>(handle);
			if (resource == 0)
			{
				g.n.unsupported++;
				return false;
			}
			// (the ground's letter: vt+0x3C, the class's own +0x198)
			const uint16_t ground_letter = uint16_t(reinterpret_cast<letter_fn>(uintptr_t(at<uint32_t>(at<uint32_t>(e) + 0x3C)))(e, 0));
			uint32_t unused = 0;
			uint8_t ground_blend = fx::blend_add;
			if (!letter_blend(ground_letter, unused, ground_blend))
			{
				g.n.letter++;
				return false;
			}
			// The D3M as 040220 draws it (its own counts moved to the spot's after)
			const uint32_t e0 = f.element_count, d0 = f.draw_count, v0 = f.vertex_count;
			const uint64_t empty0 = g.n.empty;
			if (!record_d3m(e, resource))
				return false;
			const auto undo = [&]() {
				f.element_count = e0;
				f.draw_count = d0;
				f.vertex_count = v0;
			};
			if (f.element_count == e0)
			{
				g.n.empty = empty0;
				g.n.unsupported++;
				return false;
			}
			g.n.taken[fx::class_d3m]--;
			fx::element &src = f.elements[e0];
			if (src.draws == 0)
			{
				// (a D3M with nothing in it: nothing in the picture, so nothing on the ground)
				undo();
				g.n.empty++;
				return true;
			}
			// 054D70's look-at from WORLD = +0x60, and the scale
			float local[16], scale[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, scaled[16], view[16];
			std::memcpy(local, reinterpret_cast<const void *>(e + 0x60), 64);
			const float eye[3] = { local[12], local[13] - g.spot_lift, local[14] }, to[3] = { local[12], local[13], local[14] }, up[3] = { 0.0f, 0.0f, -1.0f };
			reinterpret_cast<look_at_fn>(g.spot_look_at)(view, eye, to, up);
			scale[0] = scale[5] = scale[10] = 0.25f;
			const uintptr_t mvt = at<uint32_t>(math);
			reinterpret_cast<product_fn>(uintptr_t(at<uint32_t>(mvt + kProductB)))(math, 0, scaled, scale, local);
			// (WORLD x VIEW in one: the shader's world is the view space's)
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 4; ++j)
					src.world[4 * i + j] = scaled[4 * i] * view[j] + scaled[4 * i + 1] * view[4 + j] + scaled[4 * i + 2] * view[8 + j] + scaled[4 * i + 3] * view[12 + j];
			src.into_spot = 1;
			src.cls = fx::class_spot;
			const uint32_t ground_stages = f.draws[src.first_draw + src.draws - 1].stages | fx::stage_clamp;
			// The ground: 182700 on the element's point
			float point[3] = {};
			reinterpret_cast<point_of_fn>(g.spot_point)(e, 0, point);
			uint32_t added = 0;
			if (!spot_ground(point, added) || f.element_count + 2 > fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws)
			{
				undo();
				g.n.full++;
				return false;
			}
			if (added == 0)
			{
				// (nothing under it: the game draws the D3M into its target and nothing more)
				undo();
				g.n.empty++;
				return true;
			}
			fx::element &out = f.elements[e0 + 1];
			out = src;
			std::memcpy(out.world, f.view, 64);
			out.into_spot = 0;
			out.blend = ground_blend;
			out.fog_colour = (ground_letter & 0xFF) == 0x48 ? 0 : g.fog_raw;
			out.uv[0] = out.uv[1] = 0.0f;
			out.light = out.env = out.spec = fx::kNone;
			out.zone = nullptr;
			out.first_draw = f.draw_count;
			out.draws = 1;
			f.draws[f.draw_count++] = { f.vertex_count, added, fx::kSpotTexture, ground_stages };
			f.vertex_count += added;
			f.element_count = e0 + 2;
			g.n.taken[fx::class_spot]++;
			g.n.spot_triangles += added / 3;
			return true;
		}

		// A specular D3M (CMoD3mSpecularElem, 0412B0) into the frame; false: the game's. Its pre-draw (vt+0x4C 041160, run
		// before the setup, so before this) clears flags bit 16 (unlit) and sets bit 25 (no fog), sets the stages +0x1BC 0
		// and +0x1BD 1 when the D3M has no textures, and binds the environment at stage +0x1BD with 0759C0: the element's
		// own (+0x1A8's resource +0x30, when its first dword is set) else the global 'cubemap spec' (VA 0490BB90, 044250),
		// tinted by +0x1AC (unless +0x198 bit 0, its R G B x VA 0490BC0C's / 128, truncated to their low bytes): 0x80808080
		// binds it as it is, anything else a copy of it x the tint x 2 (0755A0's target): done here in the shader. 0412B0
		// (R): an MMB goes to 040C20 (record_mmb); no vertex buffer: nothing; ZENABLE 1, TEXTUREFACTOR VA 0490BC0C as it
		// is; the texture matrix of stage +0x1BD: math vt+0x58 of (0, -pi/2, 0), or with +0x198 bit 0 of (-+0x19C,
		// -(+0x1A0 + pi/2), -+0x1A4), times (with bit 1 too) the camera's matrix (015230: [VA 048E68FC] +0xD4) without its
		// translation; no textures: 3529B0 / 3528F0 (NORMALIZENORMALS 1; stage 0 CURRENT x TFACTOR x 2, alpha x 4; stage 1
		// the camera-space normal through it, COUNT3, colour CURRENT + CURRENT's alpha x TEXTURE, alpha CURRENT), ADDRESS
		// of stage 1 CLAMP with +0x198 bit 3, else WRAP; the triangles in one draw. Textures: +0x1B8 bit 1 352B24 /
		// 3529C8 (three stages, the environment at 2) else 352C60 / 352B40 (two), the alpha of the stage after the
		// texture's SELECTARG1 with letter d or flags bit 28, else MODULATE4X with TFACTOR; the UV scroll of stage +0x1BC
		// (+0x17B: the TEXTURE0 offset); each texture run (075700 at stage +0x1BC: a wave of it when +0x148, the game's)
		// and the rest with 352C78 (the untextured table, no environment) when +5. Taken only when the stages are where
		// the tables put them (+0x1BC 0, +0x1BD 1, or 2 with three stages).
		using euler_fn = void(__fastcall *)(uintptr_t math, uintptr_t edx, float *out, float x, float y, float z);

		bool record_spec(uintptr_t e, uintptr_t handle)
		{
			fx::frame &f = *g.f;
			const uintptr_t math = at<uint32_t>(g.math), camera = at<uint32_t>(g.camera);
			if (g.spec_cubemap == 0 || math == 0)
			{
				g.n.unsupported++;
				return false;
			}
			const uintptr_t resource = at<uint32_t>(handle);
			if (resource == 0)
			{
				g.n.unsupported++;
				return false;
			}
			const uintptr_t data = resource + 0x30;
			const uint32_t textures = at<uint8_t>(data + 4), tail = at<uint8_t>(data + 5), triangles = at<uint16_t>(data + 6);
			const uintptr_t names = d3m_names(data), vertices = d3m_vertices(data);
			if (at<uint32_t>(names + 4) == 0)
			{
				g.n.empty++;
				return true; // (no vertex buffer: 0412B0 draws nothing)
			}
			const uint8_t mode = at<uint8_t>(e + 0x198);
			const bool three = textures != 0 && (at<uint8_t>(e + 0x1B8) & 2) != 0;
			const uint8_t stage0 = at<uint8_t>(e + 0x1BC), stage_env = at<uint8_t>(e + 0x1BD);
			if (textures > 13 || (textures != 0 && (stage0 != 0 || stage_env != (three ? 2 : 1) || at<float>(e + 0x148) != 0.0f)) || (textures == 0 && stage_env != 1))
			{
				g.n.unsupported++;
				return false;
			}
			// The environment and its tint, as the pre-draw bound them
			uintptr_t entry = g.spec_cubemap;
			if (const uint32_t h = at<uint32_t>(e + 0x1A8); h != 0)
				if (const uint32_t r = at<uint32_t>(h); r != 0 && at<uint32_t>(r + 0x30) != 0)
					entry = r + 0x30;
			const uint32_t object = at<uint32_t>(entry);
			if (object == 0 || (at<uint32_t>(object + 0x40) == 0 && at<uint32_t>(object + 0x44) == 0))
			{
				g.n.unsupported++;
				return false;
			}
			uint32_t tint = at<uint32_t>(e + 0x1AC);
			if ((mode & 1) == 0)
			{
				const uint32_t colour = at<uint32_t>(g.colour);
				for (int k = 0; k < 3; ++k)
				{
					const uint32_t p = uint32_t(float(int32_t((colour >> (8 * k)) & 0xFF) * int32_t((tint >> (8 * k)) & 0xFF)) * 0.0078125f) & 0xFF;
					tint = (tint & ~(0xFFu << (8 * k))) | p << (8 * k);
				}
			}
			const uint32_t env = texture_of(object);
			if (env == fx::kNone)
			{
				g.n.texture++;
				return false;
			}
			// The draws, as 0412B0 makes them
			const uint16_t letter = at<uint16_t>(e + kLetter);
			const uint32_t flags = at<uint32_t>(e + kFlags);
			uint32_t stages = fx::stage_spec | ((mode & 8) != 0 ? fx::stage_spec_clamp : 0u) | (tint != 0x80808080u ? fx::stage_spec_tint : 0u);
			fx::draw draws[16];
			uint32_t count = 0, used = 0;
			bool patch_y = false;
			float first_y_value = 0.0f;
			if (textures == 0)
			{
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
				draws[count++] = { 0, triangles * 3, fx::kNone, stages };
				used = triangles * 3;
			}
			else
			{
				stages |= fx::stage_texture | (three ? fx::stage_spec_three : 0u) | ((letter & 0xFF) == 0x64 || (flags & (1u << 28)) != 0 ? fx::stage_spec_keep : 0u);
				uint32_t first = 0;
				for (uint32_t i = 0; i < textures; ++i)
				{
					const uint32_t id = texture_of(at<uint32_t>(names + 16 * i));
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
			if (f.element_count >= fx::kMaxElements || f.draw_count + count > fx::kMaxDraws || f.vertex_count + used > fx::kMaxVertices || f.light_count >= fx::kMaxLit)
			{
				g.n.full++;
				return false;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out) || !lit_ok(e, out))
				return false;
			// The texture matrix (math vt+0x58, vt+0x24 as 0412B0 calls them)
			alignas(16) float tm[16];
			const auto euler = reinterpret_cast<euler_fn>(uintptr_t(at<uint32_t>(at<uint32_t>(math) + 0x58)));
			if ((mode & 1) != 0)
			{
				euler(math, 0, tm, -at<float>(e + 0x19C), -(at<float>(e + 0x1A0) + 1.5707964f), -at<float>(e + 0x1A4));
				if ((mode & 2) != 0)
				{
					if (camera == 0)
					{
						g.n.unsupported++;
						return false;
					}
					alignas(16) float cm[16];
					std::memcpy(cm, reinterpret_cast<const void *>(camera + 0xD4), 64);
					cm[12] = cm[13] = cm[14] = 0.0f;
					cm[15] = 1.0f;
					reinterpret_cast<product_fn>(uintptr_t(at<uint32_t>(at<uint32_t>(math) + kProductB)))(math, 0, tm, tm, cm);
				}
			}
			else
				euler(math, 0, tm, 0.0f, -1.5707964f, 0.0f);
			// The camera-space normals: WORLD's 3x3 inverted and transposed (VIEW is identity)
			const float *w = out.world;
			const float det = w[0] * (w[5] * w[10] - w[6] * w[9]) - w[1] * (w[4] * w[10] - w[6] * w[8]) + w[2] * (w[4] * w[9] - w[5] * w[8]);
			if (!std::isfinite(det) || std::fabs(det) < 1e-20f)
			{
				g.n.unsupported++;
				return false;
			}
			const float inv[9] = { (w[5] * w[10] - w[6] * w[9]) / det, (w[2] * w[9] - w[1] * w[10]) / det, (w[1] * w[6] - w[2] * w[5]) / det,
				(w[6] * w[8] - w[4] * w[10]) / det, (w[0] * w[10] - w[2] * w[8]) / det, (w[2] * w[4] - w[0] * w[6]) / det,
				(w[4] * w[9] - w[5] * w[8]) / det, (w[1] * w[8] - w[0] * w[9]) / det, (w[0] * w[5] - w[1] * w[4]) / det };
			fx::lighting &l = f.lights[f.light_count];
			l = fx::lighting();
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					l.normal[i][j] = inv[j * 3 + i];
			for (int r = 0; r < 4; ++r)
			{
				l.toward[0][r] = tm[4 * r + 0];
				l.toward[1][r] = tm[4 * r + 1];
			}
			for (int c = 0; c < 4; ++c)
				l.diffuse[0][c] = float((tint >> (c == 3 ? 24 : 16 - 8 * c)) & 0xFF) / 255.0f;
			out.spec = f.light_count++;
			out.env = env;
			out.tfactor = at<uint32_t>(g.colour);
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
			out.cls = fx::class_spec;
			f.vertex_count += used;
			f.element_count++;
			g.n.taken[fx::class_spec]++;
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

		// A distortion quad (CMoDist 0426F0; CMoVuDist 043110 draws it once a point of its walk) into the frame; false: the
		// game's. 0426F0 (R): the draw matrix (045D40), the diamond (0, 0, 0), (-1, 0, 0), (0, -1, 0), (1, 0, 0), (0, 1, 0)
		// projected by math vt+0x80 (the draw matrix, gfx +0xB10, gfx +0xDC0 / +0xDC4: x = (x/|w| + 1) x +0xDC0, y = (1 -
		// y/|w|) x +0xDC4) into pixels of the game's target (009E00: W +8, H +0xC), UV = them / (W, H). Nothing without the
		// world's texture ([VA 048E666C] +0x194). The points' box (the centre too) moved to 0, each side scaled by 255 /
		// size when 256 or more: the grab (3530D0 / 353028, into the 256 x 256 target VA 0490BB88) is the strip of points
		// 1, 2, 4, 3 at those places with their UVs. Then the fan (353178 / 353108: SRCALPHA / INVSRCALPHA, z test,
		// LIGHTING and fog off; colour TEXTURE x DIFFUSE x 2, alpha DIFFUSE x TFACTOR x 4, TFACTOR 0x80808080 as 3530D0
		// left it, ADDRESS MIRROR as 353028 left it) of the centre (colour VA 0490BC0C) and points 1 2 3 4 1 (0x00808080),
		// UVs the moved places / 255, WORLD the draw matrix with +0x194 added to x and y. BLENDOP is 046070's / 0467E0's:
		// a subtractive letter keeps REVSUBTRACT: the game's. `repeat`: the quads drawn one after the other (VuDist's
		// points: 0430D0 sets the point into the draw matrix, but 0426F0 builds it again first, so every one is drawn at
		// the element's own place; R).
		using project_fn = void(__fastcall *)(uintptr_t math, uintptr_t edx, const float *in, float *out, int32_t count, const float *m, const float *p, float hw, float hh);
		constexpr float kDiamond[5][3] = { { 0, 0, 0 }, { -1, 0, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };

		bool record_dist(uintptr_t e, uint32_t repeat)
		{
			fx::frame &f = *g.f;
			const uintptr_t gfx = at<uint32_t>(g.gfx), math = at<uint32_t>(g.math);
			const uintptr_t wrapper = gfx != 0 ? at<uint32_t>(gfx + 0xDC8) : 0;
			const uintptr_t app = g.dist_app != 0 ? at<uint32_t>(g.dist_app) : 0;
			if (gfx == 0 || math == 0 || wrapper == 0 || app == 0)
			{
				g.n.unsupported++;
				return false;
			}
			if (at<uint32_t>(app + 0x194) == 0 || repeat == 0)
			{
				g.n.empty++;
				return true; // (no world texture: 0426F0 draws nothing)
			}
			if (f.element_count >= fx::kMaxElements || f.draw_count + repeat > fx::kMaxDraws || f.vertex_count + 12 > fx::kMaxVertices ||
				f.grab_count + repeat > fx::kMaxGrabs)
			{
				g.n.full++;
				return false;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out))
				return false;
			if (out.blend == fx::blend_subtract)
			{
				g.n.unsupported++;
				return false;
			}
			// The diamond in the game's target's pixels (math vt+0x80, as 0426F0 calls it)
			const uintptr_t rt = wrapper + 0x98 + uintptr_t(at<uint8_t>(wrapper + 0x158)) * 0x18;
			const int32_t W = at<int32_t>(rt + 8), H = at<int32_t>(rt + 0xC);
			if (W <= 0 || H <= 0)
			{
				g.n.unsupported++;
				return false;
			}
			float s[5][3];
			const auto project = reinterpret_cast<project_fn>(uintptr_t(at<uint32_t>(at<uint32_t>(math) + 0x80)));
			project(math, 0, &kDiamond[0][0], &s[0][0], 5, reinterpret_cast<const float *>(e + kWorld), reinterpret_cast<const float *>(gfx + 0xB10),
				at<float>(gfx + 0xDC0), at<float>(gfx + 0xDC4));
			const float iw = 1.0f / float(W), ih = 1.0f / float(H);
			float lo[2] = { s[0][0], s[0][1] }, hi[2] = { s[0][0], s[0][1] };
			for (int k = 1; k < 5; ++k)
				for (int c = 0; c < 2; ++c)
				{
					lo[c] = std::min(lo[c], s[k][c]);
					hi[c] = std::max(hi[c], s[k][c]);
				}
			float scale[2];
			for (int c = 0; c < 2; ++c)
				scale[c] = hi[c] - lo[c] < 256.0f ? 1.0f : 255.0f / (hi[c] - lo[c]);
			float moved[5][2];
			for (int k = 0; k < 5; ++k)
				for (int c = 0; c < 2; ++c)
					moved[k][c] = (s[k][c] - lo[c]) * scale[c];
			// The grab: points 1, 2, 4, 3
			fx::grab gr;
			const int order[4] = { 1, 2, 4, 3 };
			for (int k = 0; k < 4; ++k)
			{
				gr.pos[k][0] = moved[order[k]][0];
				gr.pos[k][1] = moved[order[k]][1];
				gr.uv[k][0] = iw * s[order[k]][0];
				gr.uv[k][1] = ih * s[order[k]][1];
			}
			// The fan as a list: (centre, k, k + 1) for k 1 .. 4 (point 5 is point 1)
			fx::vertex fan[6];
			for (int k = 0; k < 6; ++k)
			{
				const int p = k == 5 ? 1 : k;
				fan[k] = fx::vertex();
				fan[k].pos[0] = kDiamond[p][0];
				fan[k].pos[1] = kDiamond[p][1];
				fan[k].colour = k == 0 ? out.tfactor : 0x00808080u;
				fan[k].uv[0] = moved[p][0] * (1.0f / 255.0f);
				fan[k].uv[1] = moved[p][1] * (1.0f / 255.0f);
			}
			uint32_t w = f.vertex_count;
			for (int k = 1; k <= 4; ++k)
			{
				f.vertices[w++] = fan[0];
				f.vertices[w++] = fan[k];
				f.vertices[w++] = fan[k + 1];
			}
			const float shift = at<float>(e + 0x194);
			out.world[12] += shift;
			out.world[13] += shift;
			out.tfactor = 0x80808080u;
			out.blend = fx::blend_alpha;
			out.fog = 0;
			out.uv[0] = out.uv[1] = 0.0f;
			out.first_draw = f.draw_count;
			out.draws = repeat;
			out.cls = fx::class_dist;
			for (uint32_t r = 0; r < repeat; ++r)
			{
				f.grabs[f.grab_count] = gr;
				fx::draw d;
				d.first = f.vertex_count;
				d.count = 12;
				d.texture = fx::kDistTexture;
				d.stages = fx::stage_dist;
				d.grab = f.grab_count++;
				f.draws[f.draw_count++] = d;
			}
			f.vertex_count = w;
			f.element_count++;
			g.n.taken[fx::class_dist]++;
			return true;
		}

		// A CMoVuDist (043110): the walk 063C80 (ecx the element's +0x198, its count +0x1DC) with 0430D0 at each point,
		// which draws 0426F0 there: the walk run here with a callback of ours that only counts (the clock moves on as in
		// the game), then that many quads (record_dist). The walk's points don't matter (above); WORLD's cache isn't read
		// for anything drawn.
		void __cdecl vu_count(uintptr_t, uintptr_t, uintptr_t)
		{
		}

		bool record_vudist(uintptr_t e)
		{
			const int32_t points = at<int16_t>(e + 0x1DC);
			uint32_t bound = points > 0 ? uint32_t(points) : 0;
			if (points >= 20 && (at<uint8_t>(e + 0x198 + 0x46) & 1) != 0)
			{
				const float k = reinterpret_cast<weather_fn>(g.weather)();
				if (!(k >= 0.0f && k < 64.0f))
				{
					g.n.unsupported++;
					return false;
				}
				bound = std::max(bound, uint32_t(std::ceil(float(points) * k * 0.5f)) + 1);
			}
			fx::frame &f = *g.f;
			const uintptr_t gfx = at<uint32_t>(g.gfx), math = at<uint32_t>(g.math), app = g.dist_app != 0 ? at<uint32_t>(g.dist_app) : 0;
			if (g.vu_walk == 0 || gfx == 0 || math == 0 || app == 0 || points < 0 || bound > 64)
			{
				g.n.unsupported++;
				return false;
			}
			if (f.element_count >= fx::kMaxElements || f.draw_count + bound > fx::kMaxDraws || f.vertex_count + 12 > fx::kMaxVertices || f.grab_count + bound > fx::kMaxGrabs)
			{
				g.n.full++;
				return false;
			}
			// (what record_dist checks before it draws, so the walk below runs only when the quads are taken)
			fx::element probe;
			if (!common(e, probe) || probe.blend == fx::blend_subtract)
			{
				if (probe.blend == fx::blend_subtract)
					g.n.unsupported++;
				return false;
			}
			const int32_t n = reinterpret_cast<walk_fn>(g.vu_walk)(e + 0x198, 0, reinterpret_cast<uintptr_t>(&vu_count), e);
			if (n <= 0 || uint32_t(n) > bound)
			{
				g.n.empty++;
				return true;
			}
			return record_dist(e, uint32_t(n));
		}

		// The 042D00 distortions (CMoDistModelElem 043320, CMoDistMorphElem 043590, CMoDistRingElem 043720). 042D00 (R;
		// thiscall, (vertices, count, M, stage), ret 0x10): every vertex's screen point by math vt+0x84 (M, gfx +0xB10, the
		// half sizes gfx +0xDC0 / +0xDC4; written into the 36-byte vertex's UV), then UV = ((x + 0.5) / W, (H - 0.5 - y) /
		// H) in the game's target (009E00: W +8, H +0xC); false (nothing drawn) when W or H is 0 or without the world's
		// texture ([VA 048E666C] +0x194). Then the grab of the whole world, flipped, into the 256 x 256 target (VA 0490BB88):
		// the strip (0, 0) (256, 0) (0, 256) (256, 256) with UVs (-0.5/W, (H-0.5)/H), ((W-0.5)/W, (H-0.5)/H), (-0.5/W,
		// -0.5/H), ((W-0.5)/W, -0.5/H) (35322C / 3531C0: no blend, no z, colour the texture, alpha TFACTOR 0x80, MIRROR);
		// then ALPHABLENDENABLE and ZENABLE 1 and the target as the texture of the stage handed in. The vertices are drawn
		// by the class after with that texture.
		using project_list_fn = void(__fastcall *)(uintptr_t math, uintptr_t edx, float *box, void *vertices, int32_t count, const float *m, const float *p, float hw, float hh);

		// The game's target's size (009E00) once 042D00 would draw (the world's texture there); false: it draws nothing
		bool dist_target(int32_t &W, int32_t &H)
		{
			const uintptr_t gfx = at<uint32_t>(g.gfx);
			const uintptr_t wrapper = gfx != 0 ? at<uint32_t>(gfx + 0xDC8) : 0;
			const uintptr_t app = g.dist_app != 0 ? at<uint32_t>(g.dist_app) : 0;
			if (wrapper == 0 || app == 0 || at<uint32_t>(app + 0x194) == 0)
				return false;
			const uintptr_t rt = wrapper + 0x98 + uintptr_t(at<uint8_t>(wrapper + 0x158)) * 0x18;
			W = at<int32_t>(rt + 8);
			H = at<int32_t>(rt + 0xC);
			return W > 0 && H > 0;
		}

		// 042D00's UVs for count vertices: math vt+0x84 through M, as 042D00 calls it
		void dist_uvs(fx::vertex *v, uint32_t count, const float *m, int32_t W, int32_t H)
		{
			if (count == 0)
				return;
			const uintptr_t gfx = at<uint32_t>(g.gfx), math = at<uint32_t>(g.math);
			float box[4];
			const auto project = reinterpret_cast<project_list_fn>(uintptr_t(at<uint32_t>(at<uint32_t>(math) + 0x84)));
			project(math, 0, box, v, int32_t(count), m, reinterpret_cast<const float *>(gfx + 0xB10), at<float>(gfx + 0xDC0), at<float>(gfx + 0xDC4));
			const float iw = 1.0f / float(W), ih = -1.0f / float(H), top = float(H) - 0.5f;
			for (uint32_t i = 0; i < count; ++i)
			{
				v[i].uv[0] = (v[i].uv[0] + 0.5f) * iw;
				v[i].uv[1] = (v[i].uv[1] - top) * ih;
			}
		}

		// 042D00's grab: the whole world, flipped
		fx::grab dist_full_grab(int32_t W, int32_t H)
		{
			const float w = float(W), h = float(H);
			const float l = -0.5f / w, r = (w - 0.5f) / w, t = (h - 0.5f) / h, b = -0.5f / h;
			fx::grab gr;
			const float pos[4][2] = { { 0, 0 }, { 256, 0 }, { 0, 256 }, { 256, 256 } }, uv[4][2] = { { l, t }, { r, t }, { l, b }, { r, b } };
			std::memcpy(gr.pos, pos, sizeof(pos));
			std::memcpy(gr.uv, uv, sizeof(uv));
			return gr;
		}

		// The matrix (identity moved by s in x and y) x m, as 043320 and 043590 make it with math vt+0x24
		void shifted(const float *m, float s, float out[16])
		{
			std::memcpy(out, m, 64);
			for (int c = 0; c < 4; ++c)
				out[12 + c] = s * m[c] + s * m[4 + c] + m[12 + c];
		}

		// The element's draw: the grab first, then its vertices with the distortion target
		void dist_draw_with_grab(fx::element &out, const fx::grab &gr, uint32_t first, uint32_t count, uint32_t stages)
		{
			fx::frame &f = *g.f;
			f.grabs[f.grab_count] = gr;
			fx::draw d;
			d.first = first;
			d.count = count;
			d.texture = fx::kDistTexture;
			d.stages = stages;
			d.grab = f.grab_count++;
			out.first_draw = f.draw_count;
			out.draws = 1;
			f.draws[f.draw_count++] = d;
		}

		// A CMoDistModelElem (043320) into the frame; false: the game's. 043320 (R): nothing for a weather element 002D80
		// hides or an MMB (+0x179); the D3M's vertices (03FD70, triangles x 3) copied, M = (identity moved by +0x194 in x and
		// y) x WORLD (the draw matrix, 06A4A0's cache), 042D00 (the copy, from vertex 2 with count - 2 when 03FCB0 answers:
		// no textures and kinds 5-7, whose first vertex the load overwrote; M; stage 1); WORLD the plain draw matrix again
		// (06A480), the copy into the distortion buffer (VA 0490BBB0) and 040670 (the resource, that buffer, VA 0490BC0C,
		// the element, 0): ALPHATEST GREATER 0x60 with the data's +2 bit 0; TEXTUREFACTOR the colour (D: alpha >= 0x7F
		// full); 352740 (stage 0 CURRENT x TFACTOR x 2, colour and alpha; stage 1 TEXCOORDINDEX 0, colour TEXTURE, alpha
		// TEXTURE x CURRENT x 4, WRAP; D: 352848, alpha x 2), stage 1's UV offset (+0x120 when +0x17B bits 0-1); no texture
		// set (its 0 is skipped): each texture run drawn from the buffer, then the rest when +5 (without textures: only when
		// +5, and from triangle 1 when 03FCB0 answers). All of it as one draw here.
		bool record_dist_model(uintptr_t e, uintptr_t resource)
		{
			fx::frame &f = *g.f;
			if (at<uint8_t>(e + kMmb) != 0)
			{
				g.n.empty++;
				return true;
			}
			const uintptr_t data = resource + 0x30;
			const uint32_t textures = at<uint8_t>(data + 4), tail = at<uint8_t>(data + 5), triangles = at<uint16_t>(data + 6), kind = at<uint8_t>(data) & 0xF;
			const uintptr_t gfx = at<uint32_t>(g.gfx), math = at<uint32_t>(g.math);
			if (gfx == 0 || math == 0)
			{
				g.n.unsupported++;
				return false;
			}
			int32_t W = 0, H = 0;
			if (!dist_target(W, H))
			{
				g.n.empty++;
				return true; // (042D00 answers 0: nothing drawn)
			}
			// What 040670 draws from the buffer
			uint32_t first = 0, count = 0;
			if (textures != 0)
			{
				for (uint32_t i = 0; i < textures; ++i)
					count += uint32_t(at<uint16_t>(data + 8 + 2 * i)) * 3;
				if (tail != 0 || count > triangles * 3)
					count = triangles * 3;
			}
			else if (tail != 0)
			{
				const bool overwritten = kind >= 5 && kind <= 7;
				first = overwritten ? 3 : 0;
				count = triangles > (overwritten ? 1u : 0u) ? (triangles - (overwritten ? 1 : 0)) * 3 : 0;
			}
			const uintptr_t names = d3m_names(data), vertices = d3m_vertices(data);
			if (at<uint32_t>(names + 4) == 0)
				count = 0; // (no vertex buffer: 040670 draws nothing; the grab is made)
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + count > fx::kMaxVertices || f.grab_count + 1 > fx::kMaxGrabs)
			{
				g.n.full++;
				return false;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out) || !lit_ok(e, out))
				return false;
			const uint16_t letter = at<uint16_t>(e + kLetter);
			if ((letter & 0xFF) == 0x44 && (out.tfactor >> 24) >= 0x7F)
				out.tfactor |= 0xFF000000u;
			std::memcpy(f.vertices + f.vertex_count, reinterpret_cast<const void *>(vertices + uintptr_t(first) * sizeof(fx::vertex)), size_t(count) * sizeof(fx::vertex));
			float m[16];
			shifted(out.world, at<float>(e + 0x194), m);
			dist_uvs(f.vertices + f.vertex_count, count, m, W, H);
			const uint32_t stages = fx::stage_dist_model | ((letter & 0xFF) == 0x44 ? fx::stage_alpha_2x : 0u) | ((at<uint8_t>(data + 2) & 1) != 0 ? fx::stage_alpha_test : 0u);
			dist_draw_with_grab(out, dist_full_grab(W, H), f.vertex_count, count, stages);
			out.cls = fx::class_dist_model;
			f.vertex_count += count;
			f.element_count++;
			g.n.taken[fx::class_dist_model]++;
			return true;
		}

		// A CMoDistRingElem (043720) into the frame; false: the game's. 043720 (R): +0x1B4 segments n, +0x1B5 rings (signed
		// bytes; the game's stack has room for 4); each ring's colour its bytes +0x1A4 + 4i (R G B A) x VA 0490BC0C's >> 7,
		// each kept to its low byte (no clamp); per band between rings b and b + 1 (radii +0x194 + 4b) 2n + 2 vertices: at
		// angles 0, d .. (n - 1) d (d = 2 pi / n, added up) the pair (sin a x r, cos a x r, 0) inner then outer, then the
		// first pair again; their normals not written (a lit one is the game's). 042D00 (all of them, M the draw matrix: no
		// shift; stage 0), 353378 (colour CURRENT x TEXTURE x 2, alpha DIFFUSE x TEXTURE x 4, MIRROR, stage 1 off), and a
		// DrawPrimitiveUP strip of 2n triangles per band.
		bool record_dist_ring(uintptr_t e)
		{
			fx::frame &f = *g.f;
			const int32_t segments = at<int8_t>(e + 0x1B4), rings = at<int8_t>(e + 0x1B5);
			const uintptr_t gfx = at<uint32_t>(g.gfx), math = at<uint32_t>(g.math);
			if (gfx == 0 || math == 0 || rings > 4)
			{
				g.n.unsupported++;
				return false;
			}
			if ((at<uint8_t>(e + kFlags + 2) & 1) != 0)
			{
				g.n.lit++;
				return false;
			}
			int32_t W = 0, H = 0;
			if (!dist_target(W, H))
			{
				g.n.empty++;
				return true;
			}
			const uint32_t bands = rings > 1 ? uint32_t(rings - 1) : 0, n = segments > 0 ? uint32_t(segments) : 0;
			const uint32_t count = bands * n * 2 * 3;
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + count > fx::kMaxVertices || f.grab_count + 1 > fx::kMaxGrabs)
			{
				g.n.full++;
				return false;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			if (!common(e, out))
				return false;
			const uint32_t colour = at<uint32_t>(g.colour);
			uint32_t col[4] = {};
			for (int32_t i = 0; i < rings; ++i)
			{
				const uintptr_t c = e + 0x1A4 + 4 * uintptr_t(i);
				const uint32_t r = (at<uint8_t>(c) * ((colour >> 16) & 0xFF)) >> 7, gr = (at<uint8_t>(c + 1) * ((colour >> 8) & 0xFF)) >> 7;
				const uint32_t b = (at<uint8_t>(c + 2) * (colour & 0xFF)) >> 7, a = (at<uint8_t>(c + 3) * (colour >> 24)) >> 7;
				col[i] = (a & 0xFF) << 24 | (r & 0xFF) << 16 | (gr & 0xFF) << 8 | (b & 0xFF);
			}
			const float step = 6.283184f / float(segments);
			std::vector<fx::vertex> strip(size_t(n) * 2 + 2);
			uint32_t w = f.vertex_count;
			for (uint32_t b = 0; b < bands && n != 0; ++b)
			{
				const float r[2] = { at<float>(e + 0x194 + 4 * b), at<float>(e + 0x198 + 4 * b) };
				float a = 0.0f;
				for (uint32_t k = 0; k < n; ++k, a += step)
					for (int j = 0; j < 2; ++j)
					{
						fx::vertex &v = strip[2 * k + j];
						v = fx::vertex();
						v.pos[0] = std::sin(a) * r[j];
						v.pos[1] = std::cos(a) * r[j];
						v.colour = col[b + j];
					}
				strip[2 * n] = strip[0];
				strip[2 * n + 1] = strip[1];
				dist_uvs(strip.data(), uint32_t(strip.size()), out.world, W, H);
				for (uint32_t k = 0; k < 2 * n; ++k)
					for (uint32_t j = 0; j < 3; ++j)
						f.vertices[w++] = strip[k + j];
			}
			dist_draw_with_grab(out, dist_full_grab(W, H), f.vertex_count, w - f.vertex_count, fx::stage_dist_ring);
			out.cls = fx::class_dist_ring;
			f.vertex_count = w;
			f.element_count++;
			g.n.taken[fx::class_dist_ring]++;
			return true;
		}

		// A lens flare's pieces (03EBF0, thiscall on the flare D3A's resource, called by the flare's own draw 0548D0 at
		// 054B81 with (the visibility, the light's x and y, W - x, H - y, 0) once its visibility test 06C280 has passed:
		// World FX's HideFlare (054AF1) and VisTest (054AE2) stay where they are, before it) into the frame; false: the
		// game's. 03EBF0 (R): every vertex of the pieces (resource +0x35 of them from +0x48: a 4-byte head, +2 its quads;
		// the float t at +4; then quads x 6 vertices of 24 bytes: x y, colour, u v) pre-transformed: x = W / 16 x vx + t x
		// ((W - x) - x) + x, y the same with H; colour the vertex's with alpha min(255, its alpha x visibility >> 8); the
		// texture by the name +0x44 points at (075690: its first dword a texture object, else the texture manager's [VA
		// 0490B970] +8 list searched by name bytes 8-15 at +0x14, next +0x38); 352180 / 3520A8: SRCALPHA / ONE, no z,
		// LIGHTING and fog off; colour DIFFUSE x TEXTURE x 2, alpha that x TFACTOR x 2 (TFACTOR VA 0490BC0C, 0548D0's colour);
		// WRAP; quads x 2 triangles.
		bool record_flare(uintptr_t resource, int32_t vis, float x, float y, float fx_, float fy)
		{
			fx::frame &f = *g.f;
			const uintptr_t data = resource + 0x30;
			const uintptr_t gfx = at<uint32_t>(g.gfx), mgr = g.flare_textures != 0 ? at<uint32_t>(g.flare_textures) : 0;
			const uintptr_t wrapper = gfx != 0 ? at<uint32_t>(gfx + 0xDC8) : 0;
			if (wrapper == 0)
			{
				g.n.unsupported++;
				return false;
			}
			const uintptr_t rt = wrapper + 0x98 + uintptr_t(at<uint8_t>(wrapper + 0x158)) * 0x18;
			const int32_t W = at<int32_t>(rt + 8), H = at<int32_t>(rt + 0xC);
			const int32_t pieces = at<int8_t>(data + 5), quads = at<int8_t>(data + 6);
			if (W <= 0 || H <= 0 || pieces < 0 || quads < 0)
			{
				g.n.unsupported++;
				return false;
			}
			// The texture, as 075690 finds it
			uint8_t name[16] = {};
			if (const uintptr_t np = at<uint32_t>(resource + 0x44); np != 0)
				std::memcpy(name, reinterpret_cast<const void *>(np), 16);
			uint32_t object = 0;
			std::memcpy(&object, name, 4);
			if (object == 0 && mgr != 0)
				for (uintptr_t node = at<uint32_t>(mgr + 8), n = 0; node != 0 && n < 65536; node = at<uint32_t>(node + 0x38), ++n)
					if (std::memcmp(reinterpret_cast<const void *>(node + 0x14), name + 8, 8) == 0)
					{
						object = uint32_t(node);
						break;
					}
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
			const uint32_t n = uint32_t(quads) * 6;
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + n > fx::kMaxVertices ||
				f.runs.load(std::memory_order_relaxed) + 2 >= fx::kMaxRuns)
			{
				g.n.full++;
				return false;
			}
			const float sw = float(W) * 0.0625f, sh = float(H) * 0.0625f, dx = fx_ - x, dy = fy - y;
			uint32_t w = f.vertex_count;
			uintptr_t p = resource + 0x48;
			for (int32_t k = 0; k < pieces; ++k)
			{
				const float t = at<float>(p + 4);
				const uint32_t m = uint32_t(at<int8_t>(p + 2)) * 6;
				uintptr_t v = p + 0x14;
				for (uint32_t i = 0; i < m; ++i, v += 24)
				{
					if (w >= f.vertex_count + n)
					{
						g.n.unsupported++;
						return false; // (more vertices in the pieces than +0x36 says: the game draws +0x36's)
					}
					const uint32_t c = at<uint32_t>(v + 12);
					const uint32_t a = std::min<uint32_t>((c >> 24) * uint32_t(vis) >> 8, 0xFF);
					fx::vertex &o = f.vertices[w++];
					o = fx::vertex();
					o.pos[0] = sw * at<float>(v) + dx * t + x;
					o.pos[1] = sh * at<float>(v + 4) + dy * t + y;
					o.colour = (c & 0xFFFFFFu) | a << 24;
					o.uv[0] = at<float>(v + 16);
					o.uv[1] = at<float>(v + 20);
				}
				p = v;
			}
			fx::element &out = f.elements[f.element_count];
			out = fx::element();
			// (WORLD takes D3D9's pixels, centres on whole numbers, to clip space; PROJECTION identity)
			const float world[16] = { 2.0f / float(W), 0, 0, 0, 0, -2.0f / float(H), 0, 0, 0, 0, 1, 0, 1.0f / float(W) - 1.0f, 1.0f - 1.0f / float(H), 0, 1 };
			std::memcpy(out.world, world, 64);
			out.projection = fx::proj_identity;
			out.tfactor = at<uint32_t>(g.colour);
			out.blend = fx::blend_add;
			out.z_write = 0;
			out.fog = 0;
			out.first_draw = f.draw_count;
			out.draws = 1;
			out.cls = fx::class_flare;
			f.draws[f.draw_count++] = { f.vertex_count, w - f.vertex_count, id, fx::stage_flare };
			f.vertex_count = w;
			f.element_count++;
			g.n.taken[fx::class_flare]++;
			return true;
		}

		bool record_flare_guarded(uintptr_t resource, int32_t vis, float x, float y, float fx_, float fy)
		{
			__try
			{
				return pointer(uint32_t(resource)) && record_flare(resource, vis, x, y, fx_, fy);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g.n.faulted++;
				return false;
			}
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
		//
		// `dist`: a CMoDistMorphElem (043590, R): the blend made first (03F8A0 into +0x1BC unless one's there: then the
		// game's), the draw matrix (045D40), 042D00 on the blend's P positions (M the plain draw matrix; stage 0: the
		// distortion target is the morph's texture; when it answers 0, nothing drawn and +0x1BC kept: the game's), WORLD =
		// (identity moved by +0x1C0 in x and y) x the draw matrix (06A480), then 03F440 with the blend and its flag 1: no
		// texture set, the colours as they are (not doubled), each corner's UV its position's screen UV; then +0x1BC
		// cleared and +0x194 copied to +0x1A8 as 03FBB0 does.
		bool record_morph(uintptr_t e, uintptr_t resource, bool dist = false)
		{
			fx::frame &f = *g.f;
			const uintptr_t data = resource + 0x30;
			const size_t size = ((at<uint32_t>(resource + 0x24) >> 7) & 0x7FFFF) * 16;
			if (at<uint32_t>(e + 0x1BC) != 0 || (!dist && at<float>(e + 0x148) != 0.0f) || size < 0x30)
			{
				g.n.unsupported++;
				return false;
			}
			int32_t W = 0, H = 0;
			if (dist && (at<uint32_t>(g.gfx) == 0 || at<uint32_t>(g.math) == 0 || !dist_target(W, H)))
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
			if (f.element_count >= fx::kMaxElements || f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + n > fx::kMaxVertices || (dist && f.grab_count + 1 > fx::kMaxGrabs))
			{
				g.n.full++;
				return false;
			}
			uint32_t id = fx::kDistTexture;
			if (!dist)
			{
				const uint32_t object = at<uint32_t>(data + 0x10);
				if (object == 0 || (at<uint32_t>(object + 0x40) == 0 && at<uint32_t>(object + 0x44) == 0))
				{
					g.n.unsupported++;
					return false;
				}
				id = texture_of(object);
				if (id == fx::kNone)
				{
					g.n.texture++;
					return false;
				}
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
			if (dist)
			{
				// (042D00 on the blend's positions: their screen UVs)
				g_vu.assign(size_t(positions), fx::vertex());
				for (int32_t p = 0; p < positions; ++p)
					for (int32_t k = 0; k < targets; ++k)
						for (int c = 0; c < 3; ++c)
							g_vu[size_t(p)].pos[c] = at<float>(data + 0x20 + stride * uintptr_t(k) + 12 * uintptr_t(p) + 4 * uintptr_t(c)) * w[k] + g_vu[size_t(p)].pos[c];
				dist_uvs(g_vu.data(), uint32_t(positions), out.world, W, H);
			}
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
				if (dist)
				{
					v.colour = c;
					v.uv[0] = g_vu[a].uv[0];
					v.uv[1] = g_vu[a].uv[1];
					continue;
				}
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
			const uint32_t stages = fx::stage_texture | ((head & 0x80) != 0 ? fx::stage_alpha_test : 0u);
			if (dist)
			{
				float m[16];
				shifted(out.world, at<float>(e + 0x1C0), m);
				std::memcpy(out.world, m, 64);
				dist_draw_with_grab(out, dist_full_grab(W, H), f.vertex_count, n, stages);
			}
			else
			{
				out.first_draw = f.draw_count;
				out.draws = 1;
				f.draws[f.draw_count++] = { f.vertex_count, n, id, stages };
			}
			out.cls = dist ? fx::class_dist_morph : fx::class_morph;
			f.vertex_count += n;
			f.element_count++;
			g.n.taken[out.cls]++;
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

		// Battle numbers (kind 0x5C: research/ffxi-re/notes-world-passes.md 5). Reachable on this client: 09E090 starts
		// the 'damv' / 'misv' / 'curv' schedulers in the field for a battle message whose byte in the table VA 047ECAE0 is
		// 1, 2 or 3 (168 messages, the melee hit 1 among them), once the chat filter lets it through (0ADE80) and config
		// 0x13 isn't 0: conf3win's damage display ("Display damage on the game screen and in the log window." 1, "... as
		// pop-up graphics ... only" 2; 0 "in the log window only"). 041AF0 (R): the element's source actor (03B700), its
		// point (044C40), the name-visibility test 0830C0, TEXTUREFACTOR VA 0490BC0C, then 041C20: a frame around the point
		// projected (0277E0 .. 027050), the string (+0x198, at most 16 characters) measured with the font object (VA
		// 04971BF8, sprite set 'menu    dmgnum', 120730) and centred, and for each glyph: 1207B0 binds its texture (the
		// font's +0x30 / +0x34 by the code's range +0x38 / +0x3C) and 00CD00 draws its XYZRHW quad (FVF 0x144, a strip of
		// 2; the colour, UVs from the sprite set), stages 352D30 (DIFFUSE x TEXTURE x 2, colour and alpha; x TFACTOR x 2,
		// alpha x 4: stage_texture), WRAP, 0467E0's blend and z. The projection is the game's, so the element isn't taken
		// at its setup: 0467E0 and 041AF0 run as before (they set states, they don't draw), and the two calls in 041C20
		// go to us: on_glyph notes the texture object 1207B0 binds, on_glyph_draw records the quad (pre-transformed, as
		// the lens flares: WORLD takes the game's pixels to clip space, z as it is) into an element of the run, made at
		// its first glyph with what common() worked out at the setup. No fog (pre-transformed: RHW 1, table fog sees w 1).
		// A glyph whose texture isn't up, or anything not as expected: the run closed and the game draws it.
		struct number_state
		{
			bool pending = false;     // a number's setup seen in this flush, its glyphs to come
			bool made = false;        // its element made in the frame (its first glyph recorded)
			uint32_t element = 0;     // (its index in the frame)
			fx::element out;          // what the setup worked out (common)
			uint32_t object = 0;      // the texture object 1207B0 bound for the glyph to be drawn
		};
		number_state g_number;

		void end_number()
		{
			g_number.pending = g_number.made = false;
			g_number.object = 0;
		}

		// At a number's setup (on_setup, before 0467E0): its states, as common() works them out; false: the game's
		bool begin_number(uintptr_t e)
		{
			end_number();
			fx::element out;
			if (!common(e, out))
				return false;
			out.fog = 0;
			out.cls = fx::class_number;
			out.tfactor = at<uint32_t>(g.colour);
			out.projection = fx::proj_identity;
			g_number.out = out;
			g_number.pending = true;
			return true;
		}

		bool begin_number_guarded(uintptr_t e)
		{
			__try
			{
				return begin_number(e);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g.n.faulted++;
				end_number();
				return false;
			}
		}

		// A glyph's quad (00CD00's strip of 2: 4 vertices of FVF 0x144, x y z rhw, colour, u v) into the number's element
		bool record_glyph(const uint8_t *data, uint32_t type, uint32_t count, uint32_t stride)
		{
			fx::frame &f = *g.f;
			if (type != 5 || count != 2 || stride != 0x1C || g_number.object == 0)
			{
				g.n.unsupported++;
				return false;
			}
			const uint32_t id = texture_of(g_number.object);
			if (id == fx::kNone)
			{
				g.n.texture++;
				return false;
			}
			if (f.draw_count + 1 > fx::kMaxDraws || f.vertex_count + 6 > fx::kMaxVertices || (!g_number.made && f.element_count >= fx::kMaxElements) ||
				f.runs.load(std::memory_order_relaxed) + 2 >= fx::kMaxRuns)
			{
				g.n.full++;
				return false;
			}
			// (an element made by an earlier glyph must still be the run's last: draws stay contiguous)
			if (g_number.made && (g_number.element + 1 != f.element_count || g_number.element < g.run_first ||
									 f.elements[g_number.element].first_draw + f.elements[g_number.element].draws != f.draw_count))
				g_number.made = false;
			if (!g_number.made)
			{
				const uintptr_t gfx = at<uint32_t>(g.gfx);
				const uintptr_t wrapper = gfx != 0 ? at<uint32_t>(gfx + 0xDC8) : 0;
				if (wrapper == 0)
				{
					g.n.unsupported++;
					return false;
				}
				const uintptr_t rt = wrapper + 0x98 + uintptr_t(at<uint8_t>(wrapper + 0x158)) * 0x18;
				const int32_t W = at<int32_t>(rt + 8), H = at<int32_t>(rt + 0xC);
				if (W <= 0 || H <= 0)
				{
					g.n.unsupported++;
					return false;
				}
				fx::element &out = f.elements[f.element_count];
				out = g_number.out;
				const float world[16] = { 2.0f / float(W), 0, 0, 0, 0, -2.0f / float(H), 0, 0, 0, 0, 1, 0, 1.0f / float(W) - 1.0f, 1.0f - 1.0f / float(H), 0, 1 };
				std::memcpy(out.world, world, 64);
				out.first_draw = f.draw_count;
				out.draws = 0;
				g_number.element = f.element_count++;
				g_number.made = true;
				g.n.taken[fx::class_number]++;
			}
			fx::vertex q[4];
			for (int k = 0; k < 4; ++k)
			{
				const uint8_t *s = data + 0x1C * k;
				q[k] = fx::vertex();
				std::memcpy(q[k].pos, s, 12);
				std::memcpy(&q[k].colour, s + 0x10, 4);
				std::memcpy(q[k].uv, s + 0x14, 8);
			}
			const int order[6] = { 0, 1, 2, 1, 3, 2 };
			for (int k = 0; k < 6; ++k)
				f.vertices[f.vertex_count + k] = q[order[k]];
			f.draws[f.draw_count++] = { f.vertex_count, 6, id, fx::stage_texture };
			f.vertex_count += 6;
			f.elements[g_number.element].draws++;
			return true;
		}

		bool record_glyph_guarded(const void *data, uint32_t type, uint32_t count, uint32_t stride)
		{
			__try
			{
				return data != nullptr && record_glyph(static_cast<const uint8_t *>(data), type, count, stride);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g.n.faulted++;
				return false;
			}
		}

		// The texture object 1207B0 binds for code c (the font's +0x30 + 4 x its range: 1 from +0x3C, else 0; a negative
		// code +0x30), after it has made them (120880)
		uint32_t glyph_object(uintptr_t font, int32_t c)
		{
			__try
			{
				int32_t k = 0;
				if (c >= 0 && uint32_t(c) >= at<uint32_t>(font + 0x3C))
					k = 1;
				const uint32_t object = at<uint32_t>(font + 0x30 + 4 * uintptr_t(k));
				return object != 0 && (at<uint32_t>(object + 0x40) != 0 || at<uint32_t>(object + 0x44) != 0) ? object : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return 0;
			}
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
			if (cls == g.dist_draw && g.dist_draw != 0)
				return record_dist(e, 1);
			if (cls == g.vudist_draw && g.vudist_draw != 0)
				return record_vudist(e);
			if (cls == g.spec_draw && g.spec_draw != 0)
			{
				// (0412B0: no model, nothing; an MMB, 040C20 as the plain D3M's)
				const uint32_t handle = at<uint32_t>(e + kModel);
				if (handle == 0)
				{
					g.n.empty++;
					return true;
				}
				return at<uint8_t>(e + kMmb) != 0 ? record_mmb(e, handle) : record_spec(e, handle);
			}
			if (cls == g.distring_draw && g.distring_draw != 0)
				return record_dist_ring(e);
			if (cls == g.spot_draw && g.spot_draw != 0)
				return record_spot(e);
			if ((cls == g.distmodel_draw && g.distmodel_draw != 0) || (cls == g.distmorph_draw && g.distmorph_draw != 0))
			{
				// (043320: a weather element 002D80 hides: nothing drawn; 043590 has no such check. Without a model 043590 draws
				// nothing; 043320 would read through it: never so)
				const uint32_t gh = at<uint32_t>(e + kGen);
				const uint32_t gen = gh != 0 ? at<uint32_t>(gh) : 0;
				const uint32_t handle = at<uint32_t>(e + kModel);
				const bool model = cls == g.distmodel_draw;
				if (model && gen != 0 && (at<uint8_t>(gen + 0xDE) & 0x80) != 0 && reinterpret_cast<weather_fn>(g.weather)() == 0.0f)
				{
					g.n.empty++;
					return true;
				}
				const uint32_t resource = handle != 0 ? at<uint32_t>(handle) : 0;
				if (!model && handle == 0)
				{
					g.n.empty++;
					return true;
				}
				if (resource == 0)
				{
					g.n.unsupported++;
					return false;
				}
				return model ? record_dist_model(e, resource) : record_morph(e, resource, true);
			}
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

		// Whether an element's class draw (its vtable's +0x48) is `draw`, under a guard
		bool drawn_by(uintptr_t e, uintptr_t draw)
		{
			__try
			{
				if (!pointer(uint32_t(e)))
					return false;
				const uint32_t vt = at<uint32_t>(e);
				return vt != 0 && at<uint32_t>(vt + 0x48) == draw;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
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
			g.f->element_count = g.f->draw_count = g.f->vertex_count = g.f->light_count = g.f->grab_count = 0;
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
		end_number();
		g.in_flush = false;
		g.taking = false;
		g.f = nullptr;
	}

	void __cdecl on_entry(uintptr_t callback, uintptr_t arg)
	{
		end_number();
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

	void __fastcall on_flare(uintptr_t resource, uintptr_t, int32_t visibility, float x, float y, float far_x, float far_y, uint32_t zero)
	{
		if (g.in_flush && g.taking)
		{
			if (record_flare_guarded(resource, visibility, x, y, far_x, far_y))
				return;
			close_run();
		}
		using pieces_fn = void(__fastcall *)(uintptr_t, uintptr_t, int32_t, float, float, float, float, uint32_t);
		reinterpret_cast<pieces_fn>(g.flare_pieces)(resource, 0, visibility, x, y, far_x, far_y, zero);
	}

	uint32_t __fastcall on_glyph(uintptr_t font, uintptr_t, int32_t c, uint32_t *colour, uint32_t *other)
	{
		using bind_fn = uint32_t(__fastcall *)(uintptr_t, uintptr_t, int32_t, uint32_t *, uint32_t *);
		const uint32_t ok = reinterpret_cast<bind_fn>(g.glyph_bind)(font, 0, c, colour, other);
		if (g_number.pending)
			g_number.object = (ok & 0xFF) != 0 ? glyph_object(font, c) : 0;
		return ok;
	}

	void __fastcall on_glyph_draw(uintptr_t wrapper, uintptr_t, uint32_t type, uint32_t count, const void *data, uint32_t stride)
	{
		if (g.in_flush && g.taking)
		{
			if (g_number.pending && record_glyph_guarded(data, type, count, stride))
				return;
			// (the game draws this one: what was recorded so far first)
			g_number.made = false;
			close_run();
		}
		using draw_fn = void(__fastcall *)(uintptr_t, uintptr_t, uint32_t, uint32_t, const void *, uint32_t);
		reinterpret_cast<draw_fn>(g.glyph_draw)(wrapper, 0, type, count, data, stride);
	}

	uint32_t __fastcall on_setup(uintptr_t element, uintptr_t, uint32_t arg)
	{
		if (g.in_flush && g.taking && arg == 0xFFFF)
		{
			// (a battle number: its setup and class draw run as before, its glyphs come to on_glyph_draw)
			end_number();
			if (g.number_draw != 0 && drawn_by(element, g.number_draw))
			{
				if (!begin_number_guarded(element))
					close_run();
				return reinterpret_cast<setup_fn>(g.setup)(element, 0, arg);
			}
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
		if (const uintptr_t d = find(kDistDraw); d != 0)
		{
			uint8_t b = 0;
			if (read(d + kDistApp, b) && b == 0xA1)
			{
				n.dist_draw = d;
				n.dist_app = word(d + kDistApp + 1);
			}
		}
		{
			// (the flare's call of 03EBF0, patched below when it's there as the game has it)
			const uintptr_t flare = find(kFlareDraw), pieces = find(kFlarePieces), byname = find(kTextureByName);
			uint8_t call[5] = {};
			if (flare != 0 && pieces != 0 && byname != 0 && read(flare + kFlareCallAt, call, 5) && call[0] == 0xE8 && rel_target(flare + kFlareCallAt) == pieces)
			{
				n.flare_call = flare + kFlareCallAt;
				n.flare_pieces = pieces;
				n.flare_textures = word(byname + kTextureManager);
				std::memcpy(n.saved_flare, call, 5);
			}
		}
		if (const uintptr_t v = find(kVuDistDraw); v != 0 && n.dist_draw != 0)
			n.vudist_draw = v;
		if (const uintptr_t sp = find(kSpecDraw), cube = find(kSpecCubemap); sp != 0 && cube != 0)
		{
			n.spec_draw = sp;
			n.spec_cubemap = word(cube + kSpecCubemapAt);
		}
		if (const uintptr_t sp = find(kSpotDraw); sp != 0)
		{
			// (040E70's three calls; 054D70's target, 2.0, up z -1, look-at call and 0.25; 054F80's target, 10.0, renderer
			// and call of 182700; 182700's statics, grid call, skip and limit: each by its opcode, else the game's)
			uint8_t b[10] = {};
			const auto call_at = [&](uintptr_t a) { return read(a, b, 1) && b[0] == 0xE8 ? uintptr_t(rel_target(a)) : uintptr_t(0); };
			const uintptr_t target_fn = call_at(sp + kSpotTargetCall), point_fn = call_at(sp + kSpotPointCall), ground_fn = call_at(sp + kSpotGroundCall);
			const uintptr_t map_fn = ground_fn != 0 ? call_at(ground_fn + kSpotMapCall) : 0;
			bool ok = target_fn != 0 && point_fn != 0 && map_fn != 0 && code_is(target_fn, reinterpret_cast<const char *>(kSpotTargetHead), sizeof(kSpotTargetHead)) &&
				code_is(target_fn + kSpotTargetScale, reinterpret_cast<const char *>(kSpotScale), sizeof(kSpotScale)) &&
				read(target_fn + kSpotTargetUp, b, 10) && b[0] == 0xC7 && b[1] == 0x05 && word(target_fn + kSpotTargetUp + 6) == 0xBF800000u &&
				read(target_fn + kSpotTargetLift, b, 2) && b[0] == 0xD8 && b[1] == 0x25 && call_at(target_fn + kSpotTargetLookAt) != 0 &&
				read(ground_fn + kSpotMapTarget, b, 2) && b[0] == 0x8B && b[1] == 0x0D && word(ground_fn + kSpotMapTarget + 2) == word(target_fn + 1) &&
				read(ground_fn + kSpotMapSize, b, 1) && b[0] == 0x68 && read(ground_fn + kSpotMapRenderer, b, 2) && b[0] == 0x8B && b[1] == 0x0D &&
				word(ground_fn + kSpotMapRenderer + 2) == n.renderer && code_is(map_fn, reinterpret_cast<const char *>(kGroundHead), sizeof(kGroundHead)) &&
				code_is(map_fn + kGroundGridAt, reinterpret_cast<const char *>(kGroundGrid), sizeof(kGroundGrid)) &&
				code_is(map_fn + kGroundSkipAt, reinterpret_cast<const char *>(kGroundSkip), sizeof(kGroundSkip)) &&
				code_is(map_fn + kGroundLimitAt, reinterpret_cast<const char *>(kGroundLimit), sizeof(kGroundLimit)) &&
				code_is(call_at(map_fn + kGroundGridCall), reinterpret_cast<const char *>(kGridOf), sizeof(kGridOf));
			// (the statics' operands: 8B 15 / A1 / 8B 0D for the centre, A1 for the height, D8 0D for the cells' scale)
			const intptr_t centre_operand[3] = { 2, 1, 2 };
			ok = ok && read(map_fn + kGroundHeight, b, 1) && b[0] == 0xA1 && read(map_fn + kGroundCell, b, 2) && b[0] == 0xD8 && b[1] == 0x0D;
			for (int k = 0; ok && k < 3; ++k)
				ok = read(map_fn + kGroundCentre[k], b, 1) && b[0] == (k == 1 ? 0xA1 : 0x8B);
			if (ok)
			{
				n.spot_draw = sp;
				n.spot_point = point_fn;
				n.spot_look_at = call_at(target_fn + kSpotTargetLookAt);
				n.spot_target = word(target_fn + 1);
				const uint32_t lift = word(word(target_fn + kSpotTargetLift + 2)), size = word(ground_fn + kSpotMapSize + 1);
				std::memcpy(&n.spot_lift, &lift, 4);
				std::memcpy(&n.spot_size, &size, 4);
				for (int k = 0; k < 3; ++k)
					n.spot_centre[k] = word(map_fn + kGroundCentre[k] + centre_operand[k]);
				n.spot_height = word(map_fn + kGroundHeight + 1);
				n.spot_cell = word(map_fn + kGroundCell + 2);
			}
		}
		if (n.dist_draw != 0)
		{
			n.distmodel_draw = find(kDistModelDraw);
			n.distmorph_draw = find(kDistMorphDraw);
			n.distring_draw = find(kDistRingDraw);
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
		// (the battle numbers' glyph calls in 041C20: both or neither, each where the game has it)
		if (const uintptr_t nd = find(kNumberDraw), bind = find(kGlyphBind), draw = find(kGlyphDraw); nd != 0 && bind != 0 && draw != 0)
		{
			uint8_t b[5] = {}, d[5] = {};
			if (read(bind + kGlyphBindAt, b, 5) && b[0] == 0xE8 && read(draw + kGlyphDrawAt, d, 5) && d[0] == 0xE8)
			{
				n.number_draw = nd;
				n.glyph_bind_call = bind + kGlyphBindAt;
				n.glyph_draw_call = draw + kGlyphDrawAt;
				n.glyph_bind = rel_target(n.glyph_bind_call);
				n.glyph_draw = rel_target(n.glyph_draw_call);
				std::memcpy(n.saved_bind, b, 5);
				std::memcpy(n.saved_glyph, d, 5);
				put_call(n.ours_bind, n.glyph_bind_call, reinterpret_cast<uintptr_t>(&on_glyph));
				put_call(n.ours_glyph, n.glyph_draw_call, reinterpret_cast<uintptr_t>(&on_glyph_draw));
				if (!write_code(n.glyph_bind_call, n.ours_bind, 5))
					n.number_draw = n.glyph_bind_call = n.glyph_draw_call = 0;
				else if (!write_code(n.glyph_draw_call, n.ours_glyph, 5))
				{
					write_code(n.glyph_bind_call, n.saved_bind, 5);
					n.number_draw = n.glyph_bind_call = n.glyph_draw_call = 0;
				}
			}
		}
		// (the flare's pieces: only when the rest is in; without it the game draws them)
		if (n.flare_call != 0)
		{
			put_call(n.ours_flare, n.flare_call, reinterpret_cast<uintptr_t>(&on_flare));
			if (!write_code(n.flare_call, n.ours_flare, 5))
				n.flare_call = 0;
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
		if (!code_is(g.setup_call, g.ours_setup, 5) || !code_is(g.flush_call, g.ours_flush, 5) || !code_is(g.entry_site, g.ours_entry, 7) ||
			(g.flare_call != 0 && !code_is(g.flare_call, g.ours_flare, 5)) ||
			(g.glyph_bind_call != 0 && (!code_is(g.glyph_bind_call, g.ours_bind, 5) || !code_is(g.glyph_draw_call, g.ours_glyph, 5))))
		{
			why = "something else has changed the effects' flush or element draw since: left as they are";
			return false;
		}
		if (!write_code(g.setup_call, g.saved_setup, 5) || !write_code(g.flush_call, g.saved_flush, 5) || !write_code(g.entry_site, kEntryBytes, 7) ||
			(g.flare_call != 0 && !write_code(g.flare_call, g.saved_flare, 5)) ||
			(g.glyph_bind_call != 0 && (!write_code(g.glyph_bind_call, g.saved_bind, 5) || !write_code(g.glyph_draw_call, g.saved_glyph, 5))))
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
		char line[1400];
		std::snprintf(line, sizeof(line),
			"effects: %s; %llu flushes, %llu elements drawn by HiFi XI (D3M %llu, MMB meshes %llu, D3A %llu, VuAnm %llu, morphs %llu, rings %llu, weapon trails %llu, distortion %llu (meshes %llu, morphs %llu, rings %llu), specular %llu, battle numbers %llu, lens flares %llu, light spots %llu on %llu ground triangles; %llu lit), %llu with nothing to draw, "
			"%llu water meshes World FX hides; the last flush %u elements in %u runs; "
			"left to the game: %llu (not taking), %llu (class not handled), %llu (lit, its lights not to be had), %llu (blend letter), %llu (texture not up yet), %llu (no room), %llu (unreadable), "
			"%llu (untextured D3M, its first vertex overwritten), %llu (MMB mesh not in the zone's data); "
			"%llu markers sent%s",
			g.native ? "HiFi XI draws what it can" : g.probe ? "the game draws them (waiting for the markers to come through)" : "the game draws them",
			static_cast<unsigned long long>(g.n.flushes), static_cast<unsigned long long>(taken), static_cast<unsigned long long>(g.n.taken[fx::class_d3m]),
			static_cast<unsigned long long>(g.n.taken[fx::class_mmb]), static_cast<unsigned long long>(g.n.taken[fx::class_d3a]), static_cast<unsigned long long>(g.n.taken[fx::class_vuanm]),
			static_cast<unsigned long long>(g.n.taken[fx::class_morph]),
			static_cast<unsigned long long>(g.n.taken[fx::class_ring]), static_cast<unsigned long long>(g.n.taken[fx::class_trail]),
			static_cast<unsigned long long>(g.n.taken[fx::class_dist]), static_cast<unsigned long long>(g.n.taken[fx::class_dist_model]),
			static_cast<unsigned long long>(g.n.taken[fx::class_dist_morph]), static_cast<unsigned long long>(g.n.taken[fx::class_dist_ring]),
			static_cast<unsigned long long>(g.n.taken[fx::class_spec]), static_cast<unsigned long long>(g.n.taken[fx::class_number]),
			static_cast<unsigned long long>(g.n.taken[fx::class_flare]), static_cast<unsigned long long>(g.n.taken[fx::class_spot]),
			static_cast<unsigned long long>(g.n.spot_triangles),
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
		s.dist_draw = g.dist_draw;
		s.vudist_draw = g.vudist_draw;
		s.dist_app = g.dist_app;
		s.distmodel_draw = g.distmodel_draw;
		s.distmorph_draw = g.distmorph_draw;
		s.distring_draw = g.distring_draw;
		s.spec_draw = g.spec_draw;
		s.spec_cubemap = g.spec_cubemap;
		s.spot_draw = g.spot_draw;
		s.spot_target = g.spot_target;
		s.number_draw = g.number_draw;
		s.glyph_bind_call = g.glyph_bind_call;
		s.glyph_bind = g.glyph_bind;
		s.glyph_draw_call = g.glyph_draw_call;
		s.glyph_draw = g.glyph_draw;
		s.flare_call = g.flare_call;
		s.flare_pieces = g.flare_pieces;
		s.flare_textures = g.flare_textures;
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

	void set_glyph_calls(uintptr_t bind, uintptr_t draw)
	{
		g.glyph_bind = bind;
		g.glyph_draw = draw;
	}
}
