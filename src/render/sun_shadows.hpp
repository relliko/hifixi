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
// anything not marked keep World FX's old path. native() off: every word as before, bit for bit, and nothing applied.
//
// Header only: World FX's renderer (the engine) publishes, the zone's and characters' renderers read, main.cpp sets
// native() from [zone] nativeshadows.
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
	// of the taps (float bits).
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
	};
	static_assert(sizeof(gpu_block) == 34 * 16, "the HLSL's cbuffer sun_shadow");
	constexpr UINT64 kBlockBytes = 768; // (a root CBV's slot: 256-aligned, at least the block)

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
	}

	// [zone] nativeshadows (main.cpp, each present): the native passes apply the shadows and mark the light split
	inline void set_native(bool on) { detail::g_native.store(on, std::memory_order_relaxed); }
	inline bool native() { return detail::g_native.load(std::memory_order_relaxed); }

	// World FX, each frame it shades with the cascades (b.buffer AddRef'd here; the last one let go)
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
			g.has = b.buffer != nullptr && b.device != nullptr;
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

	// A native pass on device: the latest block if fresh and its device's (out.buffer AddRef'd: hand it to retire::later
	// once the list is recorded). False: none to apply.
	inline bool latest(ID3D12Device *device, block &out)
	{
		using namespace detail;
		out = block();
		if (!native() || device == nullptr)
			return false;
		ID3D12Resource *stale = nullptr;
		bool ok = false;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			const ULONGLONG age = GetTickCount64() - g.at;
			if (g.has && age <= kFreshMs && g.latest.device == device)
			{
				out = g.latest;
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
};
)";

	inline const char *kHlsl = R"(
// (sun_shadows.hpp) The light split's word as the mode says: marked (6-bit share, bit 31 the shadows applied) or as before
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

	// The whole of it, the copy at map_register
	inline std::string hlsl(const char *map_register)
	{
		return std::string(kHlslHead) + "ByteAddressBuffer ss_map : register(" + map_register + ");\n" + kHlsl;
	}

	// The block's constants for a native pass, as it'll apply them: have (a fresh block: latest) or not (nothing applied,
	// never read); the words marked while native() is on; turn: the frame's turn of the taps (0..1)
	inline gpu_block constants_for(bool have, const block &b, float turn)
	{
		gpu_block c = have ? b.gpu : gpu_block();
		if (!have)
			std::memset(&c, 0, sizeof(c));
		c.mode[0] = native() ? 1u : 0u;
		std::memcpy(&c.mode[1], &turn, 4);
		return c;
	}
}
