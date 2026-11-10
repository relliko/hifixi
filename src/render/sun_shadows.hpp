#pragma once
// The sun's shadows looked up by HiFi XI's native passes (research notes-native-shadows.md): World FX draws the sun's
// cascades (grass_render.cpp draw_sun_map), the casters' map (draw_casters) and the clouds' shadows (cloud_shadow_ps),
// all into one buffer, its copy (the clouds' map copied to its end); each frame it shades with them it publishes them
// here (publish), in world space, so the next frame's native zone (src/zone/draw.cpp) and characters
// (src/chars/chardraw.cpp) can light each pixel with its shadow: the sun's term alone darkened, before the texture,
// lamps and ambient untouched, rather than World FX darkening the finished colour afterwards.
//
// Frame order: the native passes draw before World FX in a frame, so they read what World FX drew the frame before (the
// cascades are held and faded anyway; the casters' map, the characters' shadows, is a frame behind). On the GPU that's
// in order: the zone's and characters' lists of frame N + 1 run after World FX's of frame N and before its N + 1, which
// is what writes the copy again (always after them on the game's one queue, in submission order). The copy is a buffer
// World FX leaves in NON_PIXEL | PIXEL_SHADER_RESOURCE (or decayed to COMMON, which promotes to a read): no barrier here.
//
// Lifetime: the block holds a reference to the copy while it's the latest (publish AddRefs it, the next publish or
// release() lets it go); latest() hands the caller one more, which the caller hands to retire::later once its list is
// recorded (the +2 rule: docs/d3d12/sync.md), so a renderer World FX remakes or a stale block let go never frees the
// copy under a list in flight. A block older than kFreshMs isn't used; one older than kDropMs is let go.
//
// The light split's mark (light_split.hpp "Marks"): while native() is on, every word the native passes write has the
// sun's share in 6 bits (25-30) and bit 31 set where the sun's and clouds' shadows were applied already; World FX (told
// by zinfo.w bit 128) then leaves those pixels' sun and cloud shadows alone (only its short contact march stays) and
// reads the share as what's left of the sun's light after them. The pixels the game still draws itself, the blades and
// anything not marked keep World FX's old path. native() and native_lamps() both off: every word as before, bit for
// bit, and nothing applied. (With the lamps the mark says a little more: "The mark, with lamps" below.)
//
// The lamps (notes-lighting-improvements.md "2. One lamp system"; [zone] nativelamps, native_lamps()): the same block
// carries World FX's lamp list in world space as grass::look hands it (up to 16: the zone's lit lamps and the effects'
// glows), the lamps' shadow cubes (their depth copied into the published buffer: the sun's copy's end, or a buffer of
// their own when there's no sun map: at night, indoors) and up to 4 characters' capsules. The native passes then add
// World FX's tuned glow themselves (kHlsl's sl_glow, grass.hlsl's lamp_light ported): by the pixel's real facing, every
// lamp that has a cube shadowed by it, the characters' capsules shadowing a lamp in closed form. The block's flags
// (mode.z) say what it holds: kHasSun the sun's and clouds' shadows, kHasLamps the lamps; either may be there without
// the other, and a block with lamps and no cube has no buffer at all (nothing is read then).
//
// The mark, with lamps: while native() or native_lamps() is on the words are written marked (the share in 6 bits), and
// bit 31 means "this pixel was lit from the published block", whatever it held: a native pass sets it only where it
// applied all the block holds (after its options: constants_for). World FX, the block's producer, knows what the block
// the native passes read held (published(), asked before it publishes this frame's) and tells its shaders in zinfo.w:
// bit 512 the words are marked, 128 the block had the sun's shadows, 256 it had the lamps. On a marked pixel it leaves
// the sun's and clouds' shadows alone only with 128, and adds no lamp glow only with 256.
//
// Header only: World FX's renderer (the engine) publishes, the zone's and characters' renderers read, main.cpp sets
// native() from [zone] nativeshadows and native_lamps() from [zone] nativelamps.
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace hifi::sun
{
	constexpr ULONGLONG kFreshMs = 250, kDropMs = 2000;

	// The constants the native passes read (b2 in both: the HLSL's cbuffer sun_shadow below, the same layout).
	// Cascade k (0 near, 1 far, 2 farthest): rows[6k..6k+2] its last drawing's rows taking a world point (x, y, z, 1) to
	// its texel's column, row and depth, rows[6k+3..+5] its drawing before's; cascade[k]: depth a yalm, the one before's,
	// a texel (yalms), how far the last has faded in over the one before (1: wholly); where[k]: where its last drawing
	// starts in the copy (uints), the one before's, the row pitch (uints), texels a side (0: the cascade not drawn).
	// The casters' map the same (one drawing, the near cascade's size and pitch). sun: toward the sun (world), w how much
	// of the sun's term a whole shadow takes (0: none applied). cloud: its map's corner (world x, z), texel (yalms), the
	// shadows' strength (0: none); cloud_light: toward the light (world), the map's ground height; cloud_where: where the
	// map starts (uints), texels a side. mode: x 1 the light split's words marked (above), 0 as before; y the frame's turn
	// of the taps (float bits); z what the block holds (kHasSun, kHasLamps).
	// The lamps: lamp_head x how many of lamp_pos/lamp_col are used, y how much of a lamp's glow its shadow takes (World
	// FX's lamp shadows, 0 none), z how many bodies; lamp_pos[i] where its light is (world) and how far its glow reaches;
	// lamp_col[i] its colour times its glow's strength, w 0 a glow (never shadowed), 1 a zone's lamp (casts), 2 + k a
	// zone's lamp with cube k; lamp_where: x where the cubes start in the buffer (uints), y texels a face (cube k's face f
	// at x + (6k + f) y y; grass.hlsl kLampFaces' order); lamp_map: x the cubes' near plane, y their faces' scale, z how
	// many times its lamp's reach a cube's far plane is (kLampCubeReach; under 1 read as 1), w the look (0 tuned, 1
	// faithful: a small room's, or lamp_look()'s); body[j]: a character's feet (world) and size (grass::look's actors).
	constexpr uint32_t kHasSun = 1, kHasLamps = 2;
	constexpr uint32_t kMaxLamps = 16, kMaxLampCubes = 4, kMaxBodies = 4;
	struct gpu_block
	{
		float rows[18][4];
		float cascade[3][4];
		uint32_t where[3][4];
		float caster_rows[3][4];
		float caster[4];
		uint32_t caster_where[4];
		float sun[4];
		float cloud[4];
		float cloud_light[4];
		uint32_t cloud_where[4];
		uint32_t mode[4];
		float lamp_head[4];
		float lamp_pos[kMaxLamps][4];
		float lamp_col[kMaxLamps][4];
		uint32_t lamp_where[4];
		float lamp_map[4];
		float body[kMaxBodies][4];
	};
	static_assert(sizeof(gpu_block) == 73 * 16, "the HLSL's cbuffer sun_shadow");
	constexpr UINT64 kBlockBytes = 1280; // (a root CBV's slot: 256-aligned, at least the block)

	struct block
	{
		ID3D12Device *device = nullptr;
		ID3D12Resource *buffer = nullptr; // (World FX's copy: the cascades, the casters' map, the farthest cascade, the clouds')
		gpu_block gpu = {};
	};

	namespace detail
	{
		struct state
		{
			std::mutex mutex;
			block latest;
			bool has = false;
			ULONGLONG at = 0;
			uint64_t published = 0, taken = 0; // (status)
		};
		inline state g;
		inline std::atomic<bool> g_native { false };
		inline std::atomic<bool> g_native_lamps { false };
	}

	// [zone] nativeshadows (main.cpp, each present): the native passes apply the shadows and mark the light split
	inline void set_native(bool on) { detail::g_native.store(on, std::memory_order_relaxed); }
	inline bool native() { return detail::g_native.load(std::memory_order_relaxed); }
	// [zone] nativelamps (the same): the native passes add the lamps' glow and mark the light split
	inline void set_native_lamps(bool on) { detail::g_native_lamps.store(on, std::memory_order_relaxed); }
	inline bool native_lamps() { return detail::g_native_lamps.load(std::memory_order_relaxed); }
	// The native lamps' look (kHlslLamps): tuned, the default; faithful: World FX's glow as it is, ported (the tests hold
	// the port to World FX's with it, and zone_view draws either). Not a setting: nothing in the plugin sets it.
	constexpr int kLookTuned = 0, kLookFaithful = 1;
	inline std::atomic<int> g_lamp_look { kLookTuned };
	inline void set_lamp_look(int look) { g_lamp_look.store(look, std::memory_order_relaxed); }
	inline int lamp_look() { return g_lamp_look.load(std::memory_order_relaxed); }
	// (how far past a lamp's reach, as World FX has it, its cube is drawn: the tuned look's pool, kHlslLamps' kSlReach)
	constexpr float kLampCubeReach = 1.6f;
	// (the light split's words written marked: either is on)
	inline bool marked() { return native() || native_lamps(); }
	// (what the options leave of a block's flags)
	inline uint32_t allowed(uint32_t flags) { return flags & ((native() ? kHasSun : 0u) | (native_lamps() ? kHasLamps : 0u)); }

	// World FX, each frame it shades with the cascades or the lamps (b.gpu.mode[2] says which; b.buffer, when there is
	// one, AddRef'd here; the last one let go)
	inline void publish(const block &b)
	{
		using namespace detail;
		if (b.buffer != nullptr)
			b.buffer->AddRef();
		ID3D12Resource *old = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			old = g.latest.buffer;
			g.latest = b;
			// (the sun's shadows always read the buffer; lamps alone may have none: no cube, nothing read)
			g.has = b.device != nullptr && b.gpu.mode[2] != 0 && (b.buffer != nullptr || (b.gpu.mode[2] & kHasSun) == 0);
			g.at = GetTickCount64();
			g.published++;
		}
		// (the lists recorded with it hold their own reference: latest's, handed to retire::later)
		if (old != nullptr)
			old->Release();
	}
	// ... and each frame it shades without (the sun down, the shadows off): the native passes apply none
	inline void publish_none()
	{
		using namespace detail;
		ID3D12Resource *old = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			old = g.latest.buffer;
			g.latest = block();
			g.has = false;
			g.at = GetTickCount64();
		}
		if (old != nullptr)
			old->Release();
	}

	// World FX, before it publishes this frame's: what the block the native passes of this frame read held, as far as the
	// options let them apply it (kHasSun, kHasLamps): what bit 31 of the words they wrote means (above)
	inline uint32_t published()
	{
		using namespace detail;
		std::lock_guard<std::mutex> lock(g.mutex);
		return g.has ? allowed(g.latest.gpu.mode[2]) : 0u;
	}

	// A native pass on device: the latest block if fresh and its device's (out.buffer, when it has one, AddRef'd: hand it
	// to retire::later once the list is recorded). False: none to apply.
	inline bool latest(ID3D12Device *device, block &out)
	{
		using namespace detail;
		out = block();
		if (!marked() || device == nullptr)
			return false;
		ID3D12Resource *stale = nullptr;
		bool ok = false;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			const ULONGLONG age = GetTickCount64() - g.at;
			if (g.has && age <= kFreshMs && g.latest.device == device && allowed(g.latest.gpu.mode[2]) != 0)
			{
				out = g.latest;
				if (out.buffer != nullptr)
					out.buffer->AddRef();
				g.taken++;
				ok = true;
			}
			else if (age > kDropMs && g.latest.buffer != nullptr)
			{
				stale = g.latest.buffer;
				g.latest = block();
				g.has = false;
			}
		}
		if (stale != nullptr)
			stale->Release();
		return ok;
	}

	// (status: blocks published, taken by the native passes)
	inline void counts(uint64_t &published, uint64_t &taken)
	{
		using namespace detail;
		std::lock_guard<std::mutex> lock(g.mutex);
		published = g.published;
		taken = g.taken;
	}

	// A device going or the plugin's unload: the block let go
	inline void release()
	{
		using namespace detail;
		ID3D12Resource *old = nullptr;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			old = g.latest.buffer;
			g.latest = block();
			g.has = false;
		}
		if (old != nullptr)
			old->Release();
	}

	// The lookup in HLSL for the native passes (after light_split.hpp's kHlsl): the cbuffer at b2 and the copy at the
	// given register ("t6" for the zone, "t7" for the characters): hlsl(register). One copy of grass.hlsl's map_drawing,
	// cascade_shadow, sun_from_map, casters_from_map and cloud_shadow_at, in world space and for a facing that's known
	// (the mesh's own): keep the two alike (their constants are the same: kSunMapEdge, kSunSpread, kPenumbraMin/Max,
	// kPlaneSlack). Where grass.hlsl spins its taps by a 4 x 4 block of the half-size pass's pixels (averaged after),
	// these are spun by a hash of the point on the cascade's texel grid (a quarter texel: fixed in the world, so the
	// pattern doesn't crawl as the camera moves) and the frame's turn (mode.y), which FSR's history averages out.
	inline const char *kHlslHead = R"(
cbuffer sun_shadow : register(b2)
{
	float4 ss_rows[18];
	float4 ss_cascade[3];
	uint4 ss_where[3];
	float4 ss_caster_rows[3];
	float4 ss_caster;
	uint4 ss_caster_where;
	float4 ss_sun;
	float4 ss_cloud;
	float4 ss_cloud_light;
	uint4 ss_cloud_where;
	uint4 ss_mode;
	float4 ss_lamp_head;
	float4 ss_lamp_pos[16];
	float4 ss_lamp_col[16];
	uint4 ss_lamp_where;
	float4 ss_lamp_map;
	float4 ss_body[4];
};
)";

	inline const char *kHlsl = R"(
// (sun_shadows.hpp) The light split's word as the mode says: marked (6-bit share, bit 31 where the pass applied what the
// block holds: the sun's shadows, the lamps' glow, both; `shadowed`) or as before
uint light_word_ss(float depth, float3 n, float share, bool shadowed)
{
	if (ss_mode.x == 0)
		return light_word(depth, n, share);
	const uint w = light_word(depth, n, 0);
	return w | (uint)round(saturate(share) * 63.0) << 25 | (shadowed ? 1u << 31 : 0u);
}
bool ss_on() { return ss_sun.w > 0; }

static const float kSsEdge = 0.15, kSsSpread = 0.006545, kSsPenumbraMin = 2.0, kSsPenumbraMax = 16.0, kSsPlaneSlack = 0.05;
static const float kSsGolden = 2.3999632;

float ss_texel_at(uint base, int2 at, uint pitch) { return asfloat(ss_map.Load((base + at.y * pitch + at.x) * 4)); }

// One drawing at q (rows r0..r2): how much of it is blocked (grass.hlsl map_drawing's .x, wide 1: the facing known)
float ss_drawing(float4 q, float3 n, float4 r0, float4 r1, float4 r2, float per_yalm, uint base, float texel, float size, uint pitch, float spin)
{
	const float2 t = float2(dot(q, r0), dot(q, r1));
	const float depth = dot(q, r2) - (0.03 + texel) * per_yalm;
	const float2 f = t - 0.5;
	const int2 corner = int2(floor(f)) - 1;
	const float2 w = f - floor(f);
	float wx[4] = { 1.0 - w.x, 1.0, 1.0, w.x }, wy[4] = { 1.0 - w.y, 1.0, 1.0, w.y };
	float r = 0.0, found = 0.0, between = 0.0;
	[unroll] for (int j = 0; j < 16; ++j)
	{
		const int2 at = clamp(corner + int2(j & 3, j >> 2), int2(0, 0), int2(size, size) - 1);
		const float d = ss_texel_at(base, at, pitch);
		const bool blocked = d < depth && d < 1.0;
		r += blocked ? wx[j & 3] * wy[j >> 2] : 0.0;
		found += blocked ? 1.0 : 0.0;
		between += blocked ? depth - d : 0.0;
	}
	r = smoothstep(kSsEdge, 1.0 - kSsEdge, r / 9.0);
	// The blocker search (contact hardening): the surface's plane in the map from its facing, 8 taps out to
	// kSsPenumbraMax texels; how far above P what blocks is, on average, makes the penumbra
	const float3 a0 = r0.xyz, a1 = r1.xyz, a2 = r2.xyz;
	const float3 m = float3(dot(cross(a1, a2), n), dot(cross(a2, a0), n), dot(cross(a0, a1), n));
	const float unit = texel * per_yalm;
	const float den = m.z * unit;
	const float2 slope = -m.xy / ((den < 0.0 ? -1.0 : 1.0) * max(abs(den), length(m.xy) / 8.0 + 1e-30));
	const float slack = kSsPlaneSlack * (1.0 + dot(slope, slope));
	[unroll] for (int i = 0; i < 8; ++i)
	{
		const float a = spin + kSsGolden * i, reach = kSsPenumbraMax * sqrt((i + 0.5) / 8.0);
		const int2 at = clamp(int2(floor(t + reach * float2(cos(a), sin(a)))), int2(0, 0), int2(size, size) - 1);
		const float d = ss_texel_at(base, at, pitch);
		const float2 o = float2(at) + 0.5 - t;
		const float expect = depth + (dot(slope, o) - slack * length(o)) * unit;
		const bool blocked = d < expect && d < 1.0;
		found += blocked ? 1.0 : 0.0;
		between += blocked ? expect - d : 0.0;
	}
	const float radius = found > 0.0 ? min(between / (found * per_yalm) * kSsSpread / texel, kSsPenumbraMax) : 0.0;
	const float wider = saturate(radius - kSsPenumbraMin);
	[branch] if (wider <= 0.0 || found >= 24.0)
		return r;
	float disc = 0.0;
	[unroll] for (int i2 = 0; i2 < 16; ++i2)
	{
		const float a = spin + 1.3 + kSsGolden * i2, reach = radius * sqrt((i2 + 0.5) / 16.0);
		const int2 at = clamp(int2(floor(t + reach * float2(cos(a), sin(a)))), int2(0, 0), int2(size, size) - 1);
		const float d = ss_texel_at(base, at, pitch);
		const float2 o = float2(at) + 0.5 - t;
		const float expect = depth + (dot(slope, o) - slack * length(o)) * unit;
		disc += d < expect && d < 1.0 ? 1.0 : 0.0;
	}
	return lerp(r, disc / 16.0, wider);
}

// (a turn for the taps from where q falls on the drawing's grid, a quarter texel: fixed in the world; and the frame's)
float ss_spin(float4 q, float4 r0, float4 r1)
{
	uint2 c = uint2(int2(floor(float2(dot(q, r0), dot(q, r1)) * 4.0)) + 0x8000);
	uint h = c.x * 0x8DA6B343u ^ c.y * 0xD8163841u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	return 6.2831853 * frac(float(h & 0xFFFFu) / 65536.0 + asfloat(ss_mode.y));
}

// How far inside cascade k P is (texels in from its nearest edge), and q: P off its surface by a texel and a half and
// toward the sun by one (grass.hlsl cascade_inside)
float ss_inside(uint k, float3 P, float3 n, out float4 q)
{
	const float texel = ss_cascade[k].z;
	q = float4(P + n * (1.5 * texel) + ss_sun.xyz * texel, 1.0);
	const float2 t = float2(dot(q, ss_rows[6 * k]), dot(q, ss_rows[6 * k + 1]));
	return min(min(t.x, t.y), float(ss_where[k].w) - max(t.x, t.y));
}

// Cascade k's last drawing at q, faded in over the one before as far as it says (grass.hlsl cascade_shadow)
float ss_cascade_shadow(uint k, float4 q, float3 n)
{
	const float size = float(ss_where[k].w);
	const uint pitch = ss_where[k].z;
	const float texel = ss_cascade[k].z;
	float r = ss_drawing(q, n, ss_rows[6 * k], ss_rows[6 * k + 1], ss_rows[6 * k + 2], ss_cascade[k].x, ss_where[k].x, texel, size, pitch,
		ss_spin(q, ss_rows[6 * k], ss_rows[6 * k + 1]));
	[branch] if (ss_cascade[k].w < 1.0)
	{
		const float2 tp = float2(dot(q, ss_rows[6 * k + 3]), dot(q, ss_rows[6 * k + 4]));
		if (min(min(tp.x, tp.y), size - max(tp.x, tp.y)) > 2.0)
			r = lerp(ss_drawing(q, n, ss_rows[6 * k + 3], ss_rows[6 * k + 4], ss_rows[6 * k + 5], ss_cascade[k].y, ss_where[k].y, texel, size, pitch,
				ss_spin(q, ss_rows[6 * k + 3], ss_rows[6 * k + 4])), r, ss_cascade[k].w);
	}
	return r;
}
)" R"(
// How much of the sun the cascades block at P (world; n its facing, toward the sun's side): the near cascade where P is
// well inside it, else the far one, handing over to the farthest over the far one's last tenth (grass.hlsl sun_from_map)
float ss_from_map(float3 P, float3 n, out float covered)
{
	static const float kMargin = kSsPenumbraMax + 2.0;
	float4 q;
	float r = 0.0;
	[branch] if (ss_inside(0, P, n, q) > kMargin)
	{
		r = ss_cascade_shadow(0, q, n);
		covered = 1.0;
	}
	else
	{
		float4 q2 = q;
		const float in1 = ss_inside(1, P, n, q);
		const bool farthest = ss_where[2].w > 0;
		const float in2 = farthest ? ss_inside(2, P, n, q2) : -1.0;
		const float size1 = float(ss_where[1].w), size2 = float(ss_where[2].w);
		const float handed = farthest ? saturate(1.0 - (in1 - kMargin) / (0.1 * size1)) : 0.0;
		covered = farthest ? (handed < 1.0 ? 1.0 : saturate(in2 / (0.05 * size2))) : saturate(in1 / (0.05 * size1));
		[branch] if (handed < 1.0 && in1 > 0.0)
			r = ss_cascade_shadow(1, q, n);
		[branch] if (handed > 0.0 && in2 > 0.0)
			r = lerp(r, ss_cascade_shadow(2, q2, n), handed);
	}
	return r;
}

// How much of the sun the casters' map (the characters, monsters and the zone's moving layer) blocks at P; out: 1
// well inside it, fading over its last twentieth (grass.hlsl casters_from_map). bias: texels more off the surface
// (the characters on their own bodies: else acne on low-poly limbs)
float ss_from_casters(float3 P, float3 n, float bias)
{
	if (ss_caster_where.w == 0)
		return 0.0;
	const float texel = ss_caster.y, size = float(ss_caster_where.z);
	const float4 q = float4(P + n * ((1.5 + bias) * texel) + ss_sun.xyz * ((1.0 + bias) * texel), 1.0);
	const float2 t = float2(dot(q, ss_caster_rows[0]), dot(q, ss_caster_rows[1]));
	const float covered = saturate(min(min(t.x, t.y), size - max(t.x, t.y)) / (0.05 * size));
	float shadow = 0.0;
	[branch] if (covered > 0.0)
		shadow = ss_drawing(q, n, ss_caster_rows[0], ss_caster_rows[1], ss_caster_rows[2], ss_caster.x, ss_caster_where.x, texel, size,
			ss_caster_where.y, ss_spin(q, ss_caster_rows[0], ss_caster_rows[1]));
	return shadow * covered;
}

// How much sunlight the clouds let through at W (world: grass.hlsl cloud_shadow_at)
float ss_cloud_lit(float3 W)
{
	const uint n = ss_cloud_where.y;
	const float3 L = ss_cloud_light.xyz;
	const float along = L.y < -0.03 ? clamp((ss_cloud_light.w - W.y) / L.y, -300.0, 300.0) : 0.0;
	const float2 s = (W.xz + L.xz * along - ss_cloud.xy) / ss_cloud.z - 0.5;
	const float2 i = floor(s), f = s - i;
	float sum = 0.0;
	[unroll] for (int k = 0; k < 4; ++k)
	{
		const int2 p = clamp(int2(i) + int2(k & 1, k >> 1), int2(0, 0), int2(n, n) - 1);
		const float wk = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
		sum += wk * asfloat(ss_map.Load((ss_cloud_where.x + uint(p.y) * n + uint(p.x)) * 4));
	}
	const float2 edge = min(s, float(n) - 1.0 - s) / (0.125 * n);
	return lerp(1.0, sum, saturate(min(edge.x, edge.y)));
}

// How much of the sun's term reaches P (world; n the mesh's own facing): 1 - the cascades' and casters' shadow at
// ss_sun.w of it, less the clouds'. 1 when none is applied (ss_on false: never reads the copy). bias: the casters'
// (ss_from_casters)
float ss_sun_seen(float3 P, float3 n, float bias)
{
	[branch] if (!ss_on())
		return 1.0;
	const float3 ns = dot(n, ss_sun.xyz) < 0.0 ? -n : n; // (off the side toward the sun: a two-sided card's other face)
	float covered = 0.0;
	const float mapped = ss_from_map(P, ns, covered);
	const float blocked = max(mapped * covered, ss_from_casters(P, ns, bias));
	float seen = 1.0 - ss_sun.w * saturate(blocked);
	[branch] if (ss_cloud.w > 0.0)
		seen *= 1.0 - ss_cloud.w * (1.0 - ss_cloud_lit(P));
	return seen;
}
)";

	// The lamps' glow for the native passes ([zone] nativelamps): grass.hlsl's lamp_light, lamp_blocked and lamp_from_map
	// in world space, for a facing that's known. Keep the two alike: the falloff, the wrapped facing, the fixture and
	// near distances (kLampFixture, kLampNear), the cube's compare, offsets and bias, and how the glow goes onto the
	// colour (sl_lit). What differs, on purpose: every lamp with a cube is shadowed by it (there, only the strongest at
	// the pixel), a lamp without one isn't marched toward (nothing of the screen is read here), and a character's capsule
	// hides a zone's lamp (there the march left characters out: it found them at another depth each step).
	//
	// Two looks (ss_lamp_map.w: lamp_look()). kLookFaithful is that port, World FX's glow as it is. kLookTuned, the
	// default (Alex, 2026-11-10: "the lights do not have to match today's size or look; the best-looking result"), uses
	// what the native pass has that World FX lacked:
	// - the pool: a lamp reaches kSlReach (1.6) x as far as World FX's glow, still falling to nothing at its edge with
	//   no slope there ((1 - d^2 / R^2)^2: no ring, no disc), and inside it a core by the inverse square of the distance
	//   over kSlCoreAt (a quarter) of the old reach: light = window x (kSlPool 0.5 + kSlCore 0.8 / (1 + (d / (0.25 R0))^2)).
	//   Inside the old reach that is about what World FX's glow was (0.88 at a quarter of it, 0.54 at half: 0.88 and
	//   0.56 there), 1.3 x it at the lamp; at the old edge, where the old glow ended, still 0.2, and on out to 1.6 x;
	// - the facing: a real N.L from the facing the pixel is lit with (its normal map's or the pack's relief), wrapped a
	//   little (kSlWrap 0.25: a wall side on to a lamp keeps a fifth) over a floor of kSlFloor 0.1 for what's turned away
	//   (World FX: 0.3 on everything, the depth's guessed facing);
	// - the shadows: the cube read with a 4 x 4 tent between its texels and smoothed (World FX: a 3 x 3 of hard taps), and
	//   a capsule's edge softer the further the surface is behind the character;
	// - onto the colour as World FX puts it (x (2.5 c + 0.03) x saturate(1.2 - luma(c)): in proportion to what's drawn,
	//   so it mostly brightens what the game's own lamp light reaches, and no more by day than today), and a second
	//   term for what is dark: x kSlAlbedo (1.0) x the surface's albedo (its texture and baked vertex shade, as the
	//   pass's own light goes onto it) x saturate(kSlFadeAt 1.3 - kSlFadeBy 1.6 x luma(light)), the light being what the
	//   pixel has already, before its texture: all of it in the dark, none where the light is past 0.8 (the sun, a game
	//   lamp close by: nothing more by day, no doubling near a lamp). That term is the pool on the dark ground and walls
	//   round a lamp, which World FX's glow, being in proportion to the colour drawn, hardly lit; it is eased off as the
	//   glow in all gets bright (over 1 + kSlKnee 1.0 x its luma), so a lamp at arm's length doesn't bleach. (Tried first on its
	//   own, with 0.6 and then 1.5 of the albedo and no World FX term: the walls right by a lantern came out darker
	//   than today, where the game's own lamp light had the fade take the glow away.)
	// A small room (a Mog House: grass::look's lamp_small_room, ss_lamp_map.w 1) always gets the faithful look: its glow
	// is World FX's 0.3 one already (worldfx_core kMogHouseGlow, in the list's colours), and a wider pool would light the
	// whole room more than today. The user's LampGlow, LampReach and LampShadows reach both looks: they are in the list's
	// colours, reaches and the shadows' amount as World FX hands them.
	inline const char *kHlslLamps = R"(
// (sun_shadows.hpp) The lamps' glow, when the block has them
bool sl_on() { return (ss_mode.z & 2) != 0; }
bool sl_tuned() { return ss_lamp_map.w < 0.5; }
static const float kSlFixture = 0.75, kSlNear = 1.25;
static const float kSlReach = 1.6, kSlPool = 0.5, kSlCore = 0.8, kSlCoreAt = 0.25, kSlWrap = 0.25, kSlFloor = 0.1;
static const float kSlAlbedo = 1.0, kSlFadeAt = 1.3, kSlFadeBy = 1.6, kSlKnee = 1.0;
static const float3 kSlFaces[18] = {
	float3(1, 0, 0), float3(0, 0, 1), float3(0, 1, 0),
	float3(-1, 0, 0), float3(0, 0, -1), float3(0, 1, 0),
	float3(0, 1, 0), float3(1, 0, 0), float3(0, 0, 1),
	float3(0, -1, 0), float3(1, 0, 0), float3(0, 0, -1),
	float3(0, 0, 1), float3(-1, 0, 0), float3(0, 1, 0),
	float3(0, 0, -1), float3(1, 0, 0), float3(0, 1, 0) };
float sl_luma(float3 c) { return dot(c, float3(0.3, 0.59, 0.11)); } // (grass.hlsl's luma)

// How much the lamp at L (world; its glow's reach the cube's far plane) is hidden from P (n its facing) by what its
// cube k holds (grass.hlsl lamp_from_map: the point off the surface by a texel and a half, the depth compared a few
// centimetres and two texels short, a 3 x 3 of texels each hidden or not). Every index clamped: a root SRV has no bounds.
float sl_from_map(float3 P, float3 n, float3 L, float far_plane, uint k)
{
	const float near_plane = ss_lamp_map.x, scale = ss_lamp_map.y;
	const uint size = max(ss_lamp_where.y, 1u);
	float3 d = P - L;
	const float reach = max(abs(d.x), max(abs(d.y), abs(d.z)));
	const float texel = 2.0 * reach / (scale * float(size));
	d = P + n * (1.5 * texel) - L;
	const float3 ad = abs(d);
	const uint face = ad.x >= ad.y && ad.x >= ad.z ? (d.x > 0.0 ? 0 : 1) : ad.y >= ad.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5);
	const float3 middle = kSlFaces[face * 3], right = kSlFaces[face * 3 + 1], up = kSlFaces[face * 3 + 2];
	const float w = max(dot(d, middle), 1e-3);
	const float2 c = float2(scale * dot(d, right) / w * 0.5 + 0.5, 0.5 - scale * dot(d, up) / w * 0.5) * float(size);
	const float compared = w - (0.04 + 2.0 * texel);
	const uint base = ss_lamp_where.x + (min(k, 3u) * 6 + face) * size * size;
	float hidden = 0.0;
	[branch] if (sl_tuned())
	{
		// (the tuned look: a 4 x 4 tent between the texels, as the sun's map_drawing, then smoothed: a soft edge about a
		// texel and a half wide that doesn't step as the surface crosses the cube's grid)
		const float2 f = c - 0.5;
		const int2 corner = int2(floor(f)) - 1;
		const float2 fw = f - floor(f);
		const float wx[4] = { 1.0 - fw.x, 1.0, 1.0, fw.x }, wy[4] = { 1.0 - fw.y, 1.0, 1.0, fw.y };
		[unroll] for (int j = 0; j < 16; ++j)
		{
			const int2 q = clamp(corner + int2(j & 3, j >> 2), int2(0, 0), int2(size, size) - 1);
			const float z = asfloat(ss_map.Load((base + uint(q.y) * size + uint(q.x)) * 4));
			const float drawn = near_plane * far_plane / max(far_plane - z * (far_plane - near_plane), 1e-4);
			hidden += z < 1.0 && drawn < compared ? wx[j & 3] * wy[j >> 2] : 0.0;
		}
		return smoothstep(0.1, 0.9, hidden / 9.0);
	}
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const int2 q = clamp(int2(c) + int2(x, y), int2(0, 0), int2(size, size) - 1);
			const float z = asfloat(ss_map.Load((base + uint(q.y) * size + uint(q.x)) * 4));
			// (the depth back to the distance along the face's middle: z = f / (f - n) x (1 - n / w))
			const float drawn = near_plane * far_plane / max(far_plane - z * (far_plane - near_plane), 1e-4);
			hidden += z < 1.0 && drawn < compared ? 1.0 : 0.0;
		}
	return hidden / 9.0;
}

// How near the segment p1-q1 passes the segment p2-q2 (the closest points of two segments, Ericson's)
float sl_apart(float3 p1, float3 q1, float3 p2, float3 q2)
{
	const float3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
	const float a = max(dot(d1, d1), 1e-6), e = max(dot(d2, d2), 1e-6), f = dot(d2, r), c = dot(d1, r), b = dot(d1, d2);
	const float denom = a * e - b * b;
	float s = denom > 1e-6 * a * e ? saturate((b * f - c * e) / denom) : 0.0;
	const float t = (b * s + f) / e;
	s = t < 0.0 ? saturate(-c / a) : t > 1.0 ? saturate((b - c) / a) : s;
	return length(p1 + d1 * s - (p2 + d2 * saturate(t)));
}

// How much of the way from P to the lamp at L (as far as `last` of it: short of its fixture) the characters' capsules
// stand in (ss_body: the standing capsule grass.hlsl's body_in_view makes of each, the world's y down): how near the
// way passes a capsule's axis, in closed form, soft over the outer half of its radius. self (a character's own pixel):
// a capsule the pixel is inside, as wide and tall as grass.hlsl's lamp_body takes it (its weapon and hair with it),
// doesn't count: a body doesn't shadow itself here, the others' do.
float sl_bodies(float3 P, float3 L, float last, bool self)
{
	const uint count = min(uint(ss_lamp_head.z), 4u);
	const float3 up = float3(0, -1, 0);
	const float3 end = P + (L - P) * last;
	float blocked = 0.0;
	[loop] for (uint j = 0; j < count; ++j)
	{
		const float4 b = ss_body[j];
		const float height = 2.3 * b.w, radius = 0.6 * b.w + 0.15;
		const float apart = sl_apart(P, end, b.xyz + up * radius, b.xyz + up * max(height - radius, radius));
		float hides = 1.0 - smoothstep(0.5 * radius, radius, apart);
		if (sl_tuned())
		{
			// (the tuned look: the edge wider the further the surface is behind the body, a shadow sharp at the feet and
			// soft across the street, and a little fainter with it)
			const float3 d0 = P - b.xyz;
			const float behind = length(d0 - up * clamp(dot(d0, up), 0.0, height));
			const float soft = saturate(0.25 + 0.12 * behind);
			hides = (1.0 - smoothstep(radius * (1.0 - soft), radius * (1.0 + 0.5 * soft), apart)) / (1.0 + 0.02 * behind * behind);
		}
		if (self)
		{
			const float3 d = P - b.xyz;
			const float3 off = d - up * clamp(dot(d, up), 0.0, height + 1.5);
			hides *= smoothstep(radius + 0.3, radius + 0.6, length(off));
		}
		blocked = max(blocked, hides);
	}
	return blocked;
}

// How much the zone's lamp at L is hidden from P (grass.hlsl lamp_blocked): nothing within kSlNear of the light (its
// own lantern, bowl or bracket is around it), fading in over the next yalm; its cube (cube >= 0) and the capsules
float sl_blocked(float3 P, float3 n, float3 L, float reach, int cube, bool self)
{
	const float len = length(L - P);
	const float last = (len - kSlFixture) / max(len, 1e-3);
	const float away = smoothstep(kSlNear, kSlNear + 1.0, len);
	float blocked = 0.0;
	[branch] if (last > 0.15 && away > 0.0)
	{
		// (the cube's far plane: the lamp's reach as World FX has it, times what the cubes are drawn out to, ss_lamp_map.z)
		[branch] if (cube >= 0)
			blocked = sl_from_map(P, n, L, reach * max(ss_lamp_map.z, 1.0), uint(cube));
		[branch] if (ss_lamp_head.z > 0.0)
			blocked = max(blocked, sl_bodies(P, L, last, self));
	}
	return blocked * away;
}

// The lamps' light at P (world), to go onto the colour with sl_lit (grass.hlsl lamp_light's sum): each lamp's glow
// falling off to nothing at its reach ((1 - d^2 / R^2)^2), a little more on what faces it (wrapped: 30% on what's
// turned away) by n, the facing the pixel is lit with (its normal map's); a zone's lamp (not an effect's glow) loses
// ss_lamp_head.y of its light as far as it's hidden (sl_blocked, from ng: the mesh's own facing). The tuned look: the
// wider pool with its core and the real N.L (above).
float3 sl_glow(float3 P, float3 n, float3 ng, bool self)
{
	const uint count = min(uint(ss_lamp_head.x), 16u);
	const bool tuned = sl_tuned();
	float3 sum = 0.0;
	[loop] for (uint i = 0; i < count; ++i)
	{
		const float4 a = ss_lamp_pos[i], c = ss_lamp_col[i];
		const float3 v = a.xyz - P;
		const float d2 = dot(v, v);
		const float reach = a.w * (tuned ? kSlReach : 1.0);
		const float k = saturate(1.0 - d2 / max(reach * reach, 1e-6));
		[branch] if (k > 0.0)
		{
			const float ndl = dot(n, v) * rsqrt(d2 + 1e-4);
			const float core = kSlCoreAt * a.w;
			const float amount = tuned ? k * k * (kSlPool + kSlCore / (1.0 + d2 / max(core * core, 1e-6))) *
				(kSlFloor + (1.0 - kSlFloor) * saturate((ndl + kSlWrap) / (1.0 + kSlWrap))) : k * k * (0.3 + 0.7 * saturate(ndl));
			float3 add = amount * c.rgb;
			[branch] if (c.w > 0.5 && ss_lamp_head.y > 0.0 && sl_luma(add) > 0.002)
				add *= 1.0 - ss_lamp_head.y * sl_blocked(P, ng, a.xyz, a.w, int(c.w + 0.5) - 2, self);
			sum += add;
		}
	}
	return sum;
}
// The glow on a surface of colour c (what's drawn there before the fog): in proportion to it, a little on top so a
// near-black surface still warms, less on what's bright already (grass.hlsl lamp_light's last line)
// The tuned look: by the surface's albedo (its texture and baked shade, as the pass's own light goes onto it: colour =
// albedo x light) and faded by the light it has already, before the texture (light: the lit colour, 0..1)
float3 sl_lit(float3 glow, float3 c, float3 albedo, float3 light)
{
	const float3 as_before = glow * (2.5 * c + 0.03) * saturate(1.2 - sl_luma(c));
	[flatten] if (sl_tuned())
	{
		// (the pool's term eased off as the glow in all gets bright, kSlKnee: a lamp at arm's length warms a surface, it
		// doesn't bleach it)
		const float3 pool = glow * kSlAlbedo * albedo * saturate(kSlFadeAt - kSlFadeBy * sl_luma(light));
		return as_before + pool / (1.0 + kSlKnee * sl_luma(as_before + pool));
	}
	return as_before;
}
// The sun's share of a colour c (share of it the sun's) once the lamps' glow, none of it the sun's, is added
float sl_share(float share, float3 c, float3 glow)
{
	const float3 k = float3(0.2126, 0.7152, 0.0722);
	const float a = dot(c, k), g = dot(glow, k);
	return g > 0 && a + g > 1e-4 ? saturate(share * a / (a + g)) : share;
}
)";

	// The whole of it, the copy at map_register
	inline std::string hlsl(const char *map_register)
	{
		return std::string(kHlslHead) + "ByteAddressBuffer ss_map : register(" + map_register + ");\n" + kHlsl + kHlslLamps;
	}

	// The block's constants for a native pass, as it'll apply them: have (a fresh block: latest) or not (nothing applied,
	// never read); the words marked while native() is on; turn: the frame's turn of the taps (0..1)
	// What the pass then applies is c.mode[2]: the block's flags as far as the options allow (kHasSun only with native(),
	// kHasLamps only with native_lamps(); the part not allowed is zeroed, so nothing of it is applied or read). A pass
	// that can't apply all of that (the characters while the zone's sun isn't known) takes no block at all: a marked
	// pixel says all of it was applied.
	inline gpu_block constants_for(bool have, const block &b, float turn)
	{
		gpu_block c = have ? b.gpu : gpu_block();
		if (!have)
			std::memset(&c, 0, sizeof(c));
		c.mode[2] = have ? allowed(c.mode[2]) : 0u;
		if ((c.mode[2] & kHasSun) == 0)
		{
			c.sun[3] = 0.0f;
			c.cloud[3] = 0.0f;
			c.caster_where[3] = 0;
		}
		if ((c.mode[2] & kHasLamps) == 0)
		{
			c.lamp_head[0] = c.lamp_head[1] = c.lamp_head[2] = 0.0f;
		}
		// (the look: the block's own word, 1 a small room's, else as lamp_look() says)
		if (lamp_look() == kLookFaithful)
			c.lamp_map[3] = 1.0f;
		c.mode[0] = marked() ? 1u : 0u;
		c.mode[3] = 0;
		std::memcpy(&c.mode[1], &turn, 4);
		return c;
	}
}
