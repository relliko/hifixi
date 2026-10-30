/*
 * The native character renderer's drawing (chardraw.hpp). One root signature:
 *   0  CBV b0         the frame's constants (the upload ring's slot for its seq)
 *   1  8 constants b1 the draw: model, light scale, flags, vertex count, bone count, the composite's colour
 *   2  SRV t3         the instance's palette rows (48 bytes a bone)        3  SRV t4  the frame's models (the ring)
 *   4  table t0-t1    the mesh's vertices (68 bytes) and corners (12)       5  table t2 the group's texture (or T)
 *   s0                linear, clamped, mips linear (the character pass's sampler, 02BC12)
 * Pipelines are made as targets of new formats or sample counts come: the model draw, the composite.
 * Every index into a root SRV or table is clamped first (a root SRV has no bounds: a wild read can hang the GPU).
 */

#include <windows.h>
#include <d3dcompiler.h>
#include "chardraw.hpp"
#include "cache.hpp"
#include "../zone/hemi_spec.hpp"
#include "../zone/light_split.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>


namespace hifi::chars
{
	namespace
	{
		const char *kShader = R"(
cbuffer frame_constants : register(b0)
{
	row_major float4x4 view_proj;
	float4 fog;          // rgb; w: 1 / (end - start), 0 for none
	float4 fog_end;      // x: end
	float4 options;      // x: per-pixel lighting; y: the frame's models
	float4 sun_dir;      // toward the zone's sun (world), w: 1 when it's known (the light split's share: which light is it)
	float4 camera;       // the camera (world), w: 1 when it's known (the sun's highlight)
};
cbuffer draw_constants : register(b1)
{
	uint model_index;
	float light_scale;   // the material's dword 4
	uint flags;          // 1: shine (the alpha always passes)
	uint vertex_count;
	uint bone_count;
	uint composite;      // the actor colour (0xAARRGGBB) for the composite
	uint first_corner;   // the group's first corner (SV_VertexID starts at 0 for each draw, whatever its start)
	uint corner_count;   // the mesh's corners (every read clamped below it)
	uint has_last;       // 1: last_palette is this instance's in the last frame (its motion); 0: it moved nowhere of its own
};
struct vertex_t { float3 p0; float w0; float3 p1; float w1; float3 n0; float3 n1; uint bones; uint mirror_bones; uint axes; };
struct corner_t { uint vertex; float2 uv; };
struct bone_t { float4 a; float4 b; float4 c; }; // (rows x y z of the 3x3, then the translation: m0..m11)
struct model_t
{
	float4 light_dir[4];     // toward each light (world), w: 1 when the light is on
	float4 light_diffuse[4];
	float4 light_ambient[4];
	float4 ambient;          // AMBIENT
	float4 material_ambient; // the actor colour / 128, its bytes in the game's order
	float4 tfactor;          // TEXTUREFACTOR (alpha 0x80)
	float4 fog;              // the actor's own fog: rgb; w: 1 / (end - start), 0 for none; or all 0 (w -1): the frame's
	float4 fog_end;          // x: end
	float4 lamp_pos[4];      // the zone's lamps near it (chardraw.hpp "Lamps"): where, w the range (0: none)
	float4 lamp_colour[4];   // ... their colour, w the attenuation (a2)
	float4 overlay;          // the overlay pass (chardraw.hpp "Extra passes"): xy its texture's offset
	float4 overlay_colour;   // ... its TEXTUREFACTOR
	float4 cloak;            // the cloak pass: xy the shift in clip space (the offsets through PROJECTION)
	float4 cloak_colour;     // ... its TEXTUREFACTOR (alpha 0x80 less the actor's)
};
StructuredBuffer<vertex_t> vertices : register(t0);
StructuredBuffer<corner_t> corners : register(t1);
Texture2D colour_map : register(t2);
StructuredBuffer<bone_t> palette : register(t3);
StructuredBuffer<model_t> models : register(t4);
StructuredBuffer<bone_t> last_palette : register(t5);
SamplerState clamp_linear : register(s0);
SamplerState wrap_linear : register(s1);   // (the extra passes: WRAP addressing)

static const float kMaterialDiffuse = 0.5;
// HiFi XI's own look per pixel (hemi_spec.hpp). The ambient from a sky as the zone's, x kSkyAbove on what faces up,
// x kSkyBelow on what faces down: a body faces sideways mostly, so as bright as the game's on the whole. The sun's
// highlight: kSpec on a shine's metal by its texture's alpha (the game's own mask for its reflection), kSpecMatte on
// everything else, broad (cloth and skin only take the sheen of it).
static const float kSkyAbove = 1.2, kSkyBelow = 0.8, kSkyTint = 0.2;
static const float kSpec = 0.08, kSpecMatte = 0.015, kSpecBroad = 8, kSpecSharp = 32;

float3 bone_point(uint b, float4 p)
{
	const bone_t m = palette[min(b, bone_count - 1)];
	return p.x * float3(m.a.x, m.a.y, m.a.z) + p.y * float3(m.a.w, m.b.x, m.b.y) + p.z * float3(m.b.z, m.b.w, m.c.x) + p.w * float3(m.c.y, m.c.z, m.c.w);
}
float3 last_bone_point(uint b, float4 p)
{
	const bone_t m = last_palette[min(b, bone_count - 1)];
	return p.x * float3(m.a.x, m.a.y, m.a.z) + p.y * float3(m.a.w, m.b.x, m.b.y) + p.z * float3(m.b.z, m.b.w, m.c.x) + p.w * float3(m.c.y, m.c.z, m.c.w);
}
float3 bone_dir(uint b, float3 n)
{
	const bone_t m = palette[min(b, bone_count - 1)];
	return n.x * float3(m.a.x, m.a.y, m.a.z) + n.y * float3(m.a.w, m.b.x, m.b.y) + n.z * float3(m.b.z, m.b.w, m.c.x);
}
float3 flip(uint axis)
{
	return float3(axis == 1 ? -1 : 1, axis == 2 ? -1 : 1, axis == 3 ? -1 : 1);
}

// The sun among the model's lights (the one from the zone's sun, as lit takes it): its colour as it lights (0: none
// there, or the sun not known) and toward it
float3 sun_light(model_t m, out float3 dir)
{
	float3 colour = 0;
	dir = sun_dir.xyz;
	[unroll] for (int k = 0; k < 4; ++k)
		if (sun_dir.w > 0 && m.light_dir[k].w > 0 && dot(m.light_dir[k].xyz, sun_dir.xyz) > 0.985)
		{
			colour = m.light_diffuse[k].rgb * light_scale;
			dir = m.light_dir[k].xyz;
		}
	return colour;
}
// The ambient a from the zone's sky at the facing n (world), per pixel only (the fixed-function colour is the game's)
float3 sky_ambient(model_t m, float3 a, float3 n)
{
	if (options.x <= 0)
		return a;
	float3 dir;
	return hemi_ambient(a, n, kSkyBelow, kSkyAbove, kSkyTint, fog.rgb, sun_light(m, dir));
}
// The sun's highlight at the facing n, world and texel t, per pixel only (added after the texture)
float3 char_spec(model_t m, float3 n, float3 world, float4 t)
{
	if (options.x <= 0 || camera.w <= 0)
		return 0;
	float3 dir;
	const float3 sun = sun_light(m, dir);
	const bool shine = (flags & 1) != 0;
	return sun_spec(n, dir, camera.xyz - world, sun, shine ? kSpecSharp : kSpecBroad, shine ? kSpec * saturate(2 * t.a) : kSpecMatte);
}

// Direct3D 8's fixed-function colour for a normal (the game's states: directional lights, no specular, no emissive); share:
// the sun's share of it (light_share), the sun being the light that comes from the zone's sun (the game eases an actor's
// sun toward it: within about 10 degrees; a lamp is turned into a light from the lamp). The zone's lamps near it
// (world: the point lit) light it as they light the zone, through the diffuse as the game's own lamp slot would: never
// the sun's share.
float4 lit(model_t m, float3 n, float3 world, out float share)
{
	float3 diffuse = 0, light_ambient = 0, sun = 0;
	[unroll] for (int k = 0; k < 4; ++k)
	{
		const float3 d = m.light_dir[k].w * m.light_diffuse[k].rgb * light_scale * max(0, dot(n, m.light_dir[k].xyz));
		diffuse += d;
		sun += sun_dir.w > 0 && dot(m.light_dir[k].xyz, sun_dir.xyz) > 0.985 ? d : 0;
		light_ambient += m.light_dir[k].w * m.light_ambient[k].rgb;
	}
	[unroll] for (int j = 0; j < 4; ++j)
	{
		const float3 d = m.lamp_pos[j].xyz - world;
		const float d2 = dot(d, d), r2 = m.lamp_pos[j].w * m.lamp_pos[j].w;
		const float edge = d2 < r2 ? 1.0 - smoothstep(0.36, 1.0, d2 / max(r2, 1e-6)) : 0.0;
		diffuse += edge * m.lamp_colour[j].rgb * light_scale * max(0, dot(n, d * rsqrt(max(d2, 1e-6)))) / max(m.lamp_colour[j].w * d2, 1e-3);
	}
	const float3 c = m.material_ambient.rgb * sky_ambient(m, m.ambient.rgb * light_scale + light_ambient, n) + kMaterialDiffuse * diffuse;
	share = light_share(saturate(c), saturate(c - kMaterialDiffuse * sun));
	return float4(saturate(c), kMaterialDiffuse);
}

struct v2p
{
	float4 pos : SV_Position;
	float4 colour : COLOR0;
	float2 uv : TEXCOORD0;
	float fog : TEXCOORD1;
	float3 normal : TEXCOORD2;
	float share : TEXCOORD4;    // the sun's share of colour (lit)
	float4 motion : TEXCOORD3; // xyz the move since the last frame (world), w the view depth
	float3 world : TEXCOORD5;  // (for the lamps per pixel and the sun's highlight)
	float4 clip : TEXCOORD6;   // (the clip position: the cloak's screen place)
};

v2p vs(uint vid : SV_VertexID)
{
	const corner_t c = corners[min(first_corner + vid, corner_count - 1)];
	const bool mirrored = (c.vertex >> 31) != 0;
	const vertex_t v = vertices[min(c.vertex & 0x7FFFFFFFu, vertex_count - 1)];
	const uint bones = mirrored ? v.mirror_bones : v.bones;
	const uint b0 = bones & 0xFFFF, b1 = bones >> 16;
	const float3 f0 = mirrored ? flip(v.axes & 0xFF) : 1, f1 = mirrored ? flip((v.axes >> 8) & 0xFF) : 1;
	const float3 world = bone_point(b0, float4(v.p0 * f0, v.w0)) + bone_point(b1, float4(v.p1 * f1, v.w1));
	const float3 n = bone_dir(b0, v.n0 * f0) + bone_dir(b1, v.n1 * f1);
	const float l2 = dot(n, n);
	const float3 nn = l2 > 0 ? n * rsqrt(l2) : float3(0, -1, 0);
	v2p r;
	r.pos = mul(float4(world, 1), view_proj);
	const float3 before = has_last != 0 ? last_bone_point(b0, float4(v.p0 * f0, v.w0)) + last_bone_point(b1, float4(v.p1 * f1, v.w1)) : world;
	r.motion = float4(world - before, r.pos.w);
	const model_t mo = models[min(model_index, (uint)options.y - 1)];
	const float4 f = mo.fog.w >= 0 ? mo.fog : fog;
	const float e = mo.fog.w >= 0 ? mo.fog_end.x : fog_end.x;
	r.fog = f.w > 0 ? saturate((e - r.pos.w) * f.w) : 1;
	r.uv = c.uv;
	r.normal = nn;
	r.world = world;
	r.clip = r.pos;
	r.colour = lit(models[min(model_index, (uint)options.y - 1)], nn, world, r.share);
	return r;
}

float4 shade(v2p i, out float share)
{
	const model_t m = models[min(model_index, (uint)options.y - 1)];
	const float4 t = colour_map.Sample(clamp_linear, i.uv);
	share = i.share;
	const float4 colour = options.x > 0 ? lit(m, normalize(i.normal), i.world, share) : i.colour;
	const float3 s0 = saturate(2 * t.rgb * colour.rgb);
	const float a0 = saturate(2 * t.a * m.tfactor.a);
	float3 rgb;
	float a;
	if ((flags & 1) != 0)
	{
		// (a shine: stage 1 adds the shared cube map by the texture's alpha, and its alpha (about 0.5) to the alpha: it
		// always passes the test. The cube map, nearly black but its -Z face, isn't drawn here.)
		rgb = s0;
		a = saturate(a0 + 0.5);
	}
	else
	{
		rgb = saturate(2 * s0 * m.tfactor.rgb);
		a = saturate(4 * a0 * m.tfactor.a);
		if (a * 255 <= 69.5)
			discard; // (ALPHAFUNC GREATER, ALPHAREF 0x45)
	}
	// (the sun's highlight after the texture, the sun's all of it)
	const float3 spec = char_spec(m, i.normal, i.world, t);
	share = spec_share(share, rgb, spec);
	return float4(lerp(m.fog.w >= 0 ? m.fog.rgb : fog.rgb, rgb + spec, i.fog), a);
}
float4 ps(v2p i) : SV_Target
{
	float share;
	return shade(i, share);
}
// (with the motion target as well: the motion of what's drawn there)
struct motion_out
{
	float4 colour : SV_Target0;
	float4 motion : SV_Target1;
};
motion_out ps_motion(v2p i)
{
	motion_out o;
	float share;
	o.colour = shade(i, share);
	o.motion = i.motion;
	return o;
}
// (with the light split as well (light_split.hpp), and the motion after it)
struct light_out
{
	float4 colour : SV_Target0;
	uint light : SV_Target1;
};
light_out ps_light(v2p i)
{
	light_out o;
	float share;
	o.colour = shade(i, share);
	o.light = light_word(i.motion.w, i.normal, share);
	return o;
}
struct light_motion_out
{
	float4 colour : SV_Target0;
	uint light : SV_Target1;
	float4 motion : SV_Target2;
};
light_motion_out ps_light_motion(v2p i)
{
	light_motion_out o;
	float share;
	o.colour = shade(i, share);
	o.light = light_word(i.motion.w, i.normal, share);
	o.motion = i.motion;
	return o;
}

// The extra passes (chardraw.hpp "Extra passes"), each the mesh again, added (SRCALPHA, ONE: alpha too), no depth
// written, no alpha test, fogged as the mesh. The fog as Direct3D 8 fogs a pixel before it's blended.
float4 fogged(model_t m, v2p i, float3 rgb, float a)
{
	return float4(lerp(m.fog.w >= 0 ? m.fog.rgb : fog.rgb, rgb, i.fog), a);
}
// The overlay (025C40): its texture at the normal's place (TCI CAMERASPACENORMAL through gGame +0x294, the camera's own
// rotation: the world normal), x 0.5 + 0.5, + the offset; stage 0 MODULATE2X(texture, diffuse: lighting off and no
// vertex colour, white), stage 1 MODULATE2X(current, TFACTOR), alpha alike; each clamped
float4 ps_overlay(v2p i) : SV_Target
{
	const model_t m = models[min(model_index, (uint)options.y - 1)];
	const float3 n = normalize(i.normal);
	const float4 t = colour_map.Sample(wrap_linear, 0.5 * n.xy + 0.5 + m.overlay.xy);
	const float3 c0 = saturate(2 * t.rgb);
	const float a0 = saturate(2 * t.a);
	return fogged(m, i, saturate(2 * c0 * m.overlay_colour.rgb), saturate(2 * a0 * m.overlay_colour.a));
}
// The cloak (025330): the scene's copy (t2) at the pixel's own screen place (TCI CAMERASPACEPOSITION through
// PROJECTION x the NDC-to-UV bias, projected), shifted by the offsets through PROJECTION (over w); stage 0
// SELECTARG1(texture), its alpha the mesh's MODULATE2X(texture, TFACTOR) with the copy's alpha opaque; stage 1
// MODULATE2X(current, TFACTOR), alpha MODULATE4X(current, TFACTOR); each clamped
float4 ps_cloak(v2p i) : SV_Target
{
	const model_t m = models[min(model_index, (uint)options.y - 1)];
	const float w = abs(i.clip.w) > 1e-6 ? i.clip.w : 1e-6;
	const float2 uv = float2(0.5 * i.clip.x / w + 0.5, -0.5 * i.clip.y / w + 0.5) + m.cloak.xy / w;
	const float4 t = colour_map.Sample(wrap_linear, uv);
	const float a0 = saturate(2 * m.cloak_colour.a);
	return fogged(m, i, saturate(2 * t.rgb * m.cloak_colour.rgb), saturate(4 * a0 * m.cloak_colour.a));
}

// Depth only into a sun shadow map (record_casters): the model draw's alpha test, nothing written but depth
void sps(v2p i)
{
	if ((flags & 1) == 0)
	{
		const model_t m = models[min(model_index, (uint)options.y - 1)];
		const float a0 = saturate(2 * colour_map.Sample(clamp_linear, i.uv).a * m.tfactor.a);
		if (saturate(4 * a0 * m.tfactor.a) * 255 <= 69.5)
			discard;
	}
}

// The see-through composite: one full-target triangle; T's texel MODULATE2X the actor colour
float4 unpack(uint c) { return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0; }
struct c2p { float4 pos : SV_Position; };
c2p cvs(uint vid : SV_VertexID)
{
	c2p r;
	const float2 p = float2((vid << 1) & 2, vid & 2);
	r.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
	return r;
}
float4 over(float4 c)
{
	const float4 k = unpack(composite);
	const float4 o = float4(saturate(2 * c.rgb * k.rgb), saturate(2 * c.a * k.a));
	if (o.a <= 0)
		discard; // (ALPHAFUNC GREATER, ALPHAREF 0)
	return o;
}
float4 cps(c2p i) : SV_Target
{
	return over(colour_map.Load(int3(i.pos.xy, 0)));
}
Texture2DMS<float4> colour_ms : register(t2);
float4 cps_ms(c2p i, uint s : SV_SampleIndex) : SV_Target
{
	return over(colour_ms.Load(int2(i.pos.xy), s));
}
)";

		// En-spells (record_enspells). b0: 32 root constants; t3 the instance's palette; t0-t1 the blade's mesh.
		const char *kEnShader = R"(
cbuffer en_constants : register(b0)
{
	row_major float4x4 view_proj;
	float time;
	uint element;        // charframe.hpp kEn*: 1 fire, 2 ice, 3 wind, 4 earth, 5 thunder, 6 water, 7 light, 8 dark
	uint bone_count;
	uint vertex_count;
	uint corner_count;
	uint bone;           // the blade's bone
	float half_width;
	float strength;      // 0..1 (the model's fade)
	float3 base;         // the blade, in its bone's space: where it leaves the guard
	uint edge_axis;      // the bone's axis across the blade's flat (0 x, 1 y, 2 z)
	float3 tip;
	uint blade_kind;     // os2::blade's kind: 0 a blade (the line base..tip), 1 a head, 2 the whole piece
};
struct vertex_t { float3 p0; float w0; float3 p1; float w1; float3 n0; float3 n1; uint bones; uint mirror_bones; uint axes; };
struct corner_t { uint vertex; float2 uv; };
struct bone_t { float4 a; float4 b; float4 c; };
StructuredBuffer<vertex_t> vertices : register(t0);
StructuredBuffer<corner_t> corners : register(t1);
StructuredBuffer<bone_t> palette : register(t3);

static const float kPi = 3.14159265;
static const float kSubtle = 0.6; // (every element's light, kept below the game's own spell effects)

float3 bone_point(float3 p)
{
	const bone_t m = palette[min(bone, bone_count - 1)];
	return p.x * float3(m.a.x, m.a.y, m.a.z) + p.y * float3(m.a.w, m.b.x, m.b.y) + p.z * float3(m.b.z, m.b.w, m.c.x) + float3(m.c.y, m.c.z, m.c.w);
}
float3 bone_dir(float3 n)
{
	const bone_t m = palette[min(bone, bone_count - 1)];
	return n.x * float3(m.a.x, m.a.y, m.a.z) + n.y * float3(m.a.w, m.b.x, m.b.y) + n.z * float3(m.b.z, m.b.w, m.c.x);
}

float hash3(float3 p)
{
	p = frac(p * 0.3183099 + 0.1);
	p *= 17.0;
	return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float noise3(float3 x)
{
	const float3 i = floor(x);
	float3 f = frac(x);
	f = f * f * (3 - 2 * f);
	return lerp(lerp(lerp(hash3(i), hash3(i + float3(1, 0, 0)), f.x), lerp(hash3(i + float3(0, 1, 0)), hash3(i + float3(1, 1, 0)), f.x), f.y),
		lerp(lerp(hash3(i + float3(0, 0, 1)), hash3(i + float3(1, 0, 1)), f.x), lerp(hash3(i + float3(0, 1, 1)), hash3(i + float3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm(float3 p)
{
	float a = 0.5, s = 0;
	[unroll] for (int k = 0; k < 4; ++k)
	{
		s += a * noise3(p);
		p = p * 2.03 + float3(1.7, 9.2, 3.1);
		a *= 0.5;
	}
	return s;
}
float hash1(uint n)
{
	n = (n << 13) ^ n;
	n = n * (n * n * 15731u + 789221u) + 1376312589u;
	return float(n & 0x7FFFFFFFu) / 2147483647.0;
}
// Heat 0..1 to fire's colours: dark red, orange, yellow-white
float3 fire_ramp(float v)
{
	float3 c = lerp(float3(0, 0, 0), float3(0.55, 0.06, 0.0), saturate(v * 3));
	c = lerp(c, float3(1.0, 0.38, 0.04), saturate(v * 3 - 1));
	return lerp(c, float3(1.0, 0.86, 0.5), saturate(v * 3 - 2));
}
// Toward the camera's forward (clip w is the view depth: view_proj's 4th column)
float3 forward() { return normalize(float3(view_proj._14, view_proj._24, view_proj._34)); }

// The blade in the world: its base B, its direction A (base to tip) and length, E across its flat, F through it
struct blade_t
{
	float3 B, T, A, E, F;
	float L;
};
blade_t blade_now()
{
	blade_t b;
	b.B = bone_point(base);
	b.T = bone_point(tip);
	b.L = max(length(b.T - b.B), 1e-4);
	b.A = (b.T - b.B) / b.L;
	const float3 e = bone_dir(float3(edge_axis == 0 ? 1 : 0, edge_axis == 1 ? 1 : 0, edge_axis == 2 ? 1 : 0));
	b.E = normalize(e - b.A * dot(e, b.A) + 1e-6);
	b.F = cross(b.A, b.E);
	return b;
}

// Where a sprite starts: on the line base..tip at t (a blade), else on one of the piece's own vertices (a head's: past
// its base, found by a few tries)
float3 at(blade_t b, uint n, float t)
{
	if (blade_kind == 0)
		return lerp(b.B, b.T, t);
	const float3 axis = tip - base;
	for (uint k = 0; k < 6; ++k)
	{
		const float3 p = vertices[min(uint(hash1(n * 13u + k) * vertex_count), vertex_count - 1)].p0;
		if (blade_kind == 2 || dot(p - base, axis) >= 0)
			return bone_point(p);
	}
	return lerp(b.B, b.T, t);
}

// ------------------------------------------------------------------------------------------------ the blade's shell
// The blade's own mesh, swollen along its normals, from the guard to the tip; each element's surface on it
struct s2p
{
	float4 pos : SV_Position;
	float3 world : TEXCOORD0;
	float facing : TEXCOORD1;
	float fade : TEXCOORD2;
	float along : TEXCOORD3;
};
s2p svs(uint vid : SV_VertexID)
{
	const corner_t c = corners[min(vid, corner_count - 1)];
	const vertex_t v = vertices[min(c.vertex & 0x7FFFFFFFu, vertex_count - 1)];
	const float3 axis = tip - base;
	const float along = dot(v.p0 - base, axis) / max(dot(axis, axis), 1e-6);
	float3 n = bone_dir(v.n0);
	n = dot(n, n) > 0 ? normalize(n) : float3(0, -1, 0);
	const float swell = element == 1 ? half_width * (0.5 + 0.2 * sin(time * 9.0 + along * 21.0)) : half_width * 0.3;
	s2p r;
	r.world = bone_point(v.p0) + n * swell * saturate(along * 6 + 0.3);
	r.pos = mul(float4(r.world, 1), view_proj);
	r.facing = abs(dot(n, forward()));
	r.fade = (blade_kind == 2 ? 1.0 : saturate(along * 10) * saturate((1.03 - along) * 12)) * strength;
	r.along = along;
	return r;
}
// (premultiplied: rgb added, alpha how much of what's behind is covered)
float4 sps(s2p i) : SV_Target
{
	const float rim = pow(1 - saturate(i.facing), 1.5);
	const float3 p = i.world;
	float3 c = 0;
	float a = 0;
	switch (element)
	{
	case 1: // fire: noise climbing the blade, bright at the rim
	{
		float3 q = p * 7.0;
		q.y += time * 3.4; // (y is down: the pattern climbs)
		c = fire_ramp(saturate((fbm(q) - 0.32) * 2.0) * (0.3 + 0.9 * rim)) * 1.1;
		break;
	}
	case 2: // ice: frost facets, a cold rim, the steel frosted over
	{
		const float n = noise3(p * 38.0);
		const float facets = smoothstep(0.55, 0.62, n);
		const float shimmer = 0.75 + 0.25 * sin(time * 3.0 + n * 12.0);
		c = float3(0.3, 0.65, 1.0) * (0.2 + 0.8 * rim) + float3(0.75, 0.92, 1.0) * facets * 0.4 * shimmer;
		a = 0.25;
		break;
	}
	case 3: // wind: thin streaks racing along the blade
	{
		const float n = noise3(float3(i.along * 10.0 - time * 7.0, (p.x + p.z) * 30.0, p.y * 30.0));
		c = float3(0.5, 1.0, 0.65) * (pow(saturate(n), 6) * 3.0 + 0.2 * rim);
		break;
	}
	case 4: // earth: glowing cracks over a dulled blade, slowly pulsing
	{
		const float cracks = 1 - saturate(abs(noise3(p * 30.0) - 0.5) * 14.0);
		const float pulse = 0.7 + 0.3 * sin(time * 2.5);
		c = float3(1.0, 0.55, 0.15) * cracks * pulse * 0.9 + float3(0.6, 0.4, 0.15) * rim * 0.35;
		a = 0.3;
		break;
	}
	case 5: // thunder: crackling lines, a new pattern many times a second
	{
		const float step = floor(time * 18.0);
		const float n = noise3(p * 30.0 + step * 7.3);
		const float line_ = saturate(1 - abs(n - 0.5) * 25.0);
		c = float3(0.8, 0.7, 1.0) * line_ * 1.6 + float3(0.5, 0.4, 1.0) * rim * (0.35 + 0.3 * hash3(float3(step, 1, 2)));
		break;
	}
	case 6: // water: caustics flowing toward the tip over a wet blue sheen
	{
		const float n = fbm(float3(i.along * 8.0 - time * 2.5, (p.x + p.z) * 12.0, p.y * 12.0));
		const float caustic = pow(saturate(1 - abs(sin(n * 14.0))), 5);
		c = float3(0.15, 0.45, 1.0) * (0.25 + 0.6 * rim) * 0.6 + float3(0.7, 0.9, 1.0) * caustic * 0.6;
		a = 0.15;
		break;
	}
	case 7: // light: a warm white glow, pulsing, glittering
	{
		const float pulse = 0.8 + 0.2 * sin(time * 4.0);
		const float glitter = pow(noise3(p * 60.0 + time * 2.0), 8) * 4.0;
		c = float3(1.0, 0.9, 0.62) * ((0.3 + 0.9 * rim) * pulse + glitter);
		break;
	}
	default: // dark: the blade swallowed, a violet rim smouldering
	{
		const float n = fbm(p * 6.0 + float3(0, time * 1.5, 0));
		c = float3(0.35, 0.08, 0.6) * rim * (0.5 + n);
		a = 0.6;
		break;
	}
	}
	return float4(c * kSubtle, a * 0.8) * i.fade;
}

)"
									R"(
// ------------------------------------------------------------------------------------------------ the sprites
// kSprites quads per blade, each element's kinds of sprite. A sprite faces the camera (round ones), or lies along a
// direction (streaks, shards, bolts, rays) turned to the camera round it. Stateless: a sprite's life comes from the
// time and its number, a new place each life.
static const uint kSprites = 186;
static const float2 kQuad[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };

static const uint kFlame = 0, kGlow = 1, kStar = 2, kSoft = 3, kStreak = 4, kShard = 5, kRock = 6, kBolt = 7, kDrop = 8, kRing = 9, kSmoke = 10, kRay = 11;

struct sprite
{
	float3 c;      // its middle
	float3 dir;    // along: its length's direction
	float len, wid; // half length (camera up, or dir), half width
	bool along;
	uint kind;
	float4 col;    // premultiplied colour and cover
	float age, seed;
};

// A sprite's life: its age 0..1 and a seed for this life
void cycle(uint i, float shortest, float longest, out float age, out uint n)
{
	const float life = shortest + (longest - shortest) * hash1(i * 7 + 1);
	const float c = time / life + hash1(i * 7 + 2);
	age = frac(c);
	n = i * 7919u + uint(floor(c)) * 104729u;
}
float3 ring_dir(blade_t b, float th) { return b.E * cos(th) + b.F * sin(th); }
float bump(float age) { return sin(saturate(age) * kPi); }

sprite fire_sprite(uint i, blade_t b)
{
	sprite s = (sprite)0;
	const bool ember = i >= 150;
	float age;
	uint n;
	cycle(i, ember ? 1.3 : 0.42, ember ? 2.2 : 0.75, age, n);
	const float t = lerp(0.03, 1.0, hash1(n + 3));
	const float3 jitter = (float3(hash1(n + 4), hash1(n + 5), hash1(n + 6)) - 0.5) * half_width * 2.2;
	const float rise = (ember ? 0.85 : 0.30) * (0.6 + 0.8 * hash1(n + 9));
	const float3 sway = float3(sin(time * 3.1 + hash1(n + 7) * 6.28), 0, cos(time * 2.3 + hash1(n + 8) * 6.28)) * (ember ? 0.12 : 0.035) * age;
	s.c = at(b, n, t) + jitter + float3(0, -rise * age, 0) + sway;
	const float size = ember ? 0.010 : half_width * (0.9 + 0.8 * hash1(n + 10)) * (1 - age * 0.5) * smoothstep(0.0, 0.12, age);
	s.wid = size;
	s.len = ember ? size : size * 1.6;
	s.kind = ember ? kGlow : kFlame;
	s.col = ember ? float4(1.6, 0.88, 0.24, 0) * (1 - age) * (0.6 + 0.4 * sin(time * 23.0 + hash1(n + 11) * 40.0)) : float4(1, 1, 1, 0);
	s.age = age;
	s.seed = hash1(n + 11);
	return s;
}

)"
									R"(
sprite ice_sprite(uint i, blade_t b)
{
	sprite s = (sprite)0;
	float age;
	uint n;
	if (i < 60) // sparkles on the blade
	{
		cycle(i, 0.5, 1.1, age, n);
		s.c = at(b, n, lerp(0.05, 1.0, hash1(n + 3))) + ring_dir(b, hash1(n + 4) * 6.283) * half_width * 1.1;
		s.wid = s.len = 0.035 * (0.6 + 0.8 * hash1(n + 5)) * bump(age);
		s.kind = kStar;
		s.col = float4(0.75, 0.95, 1.2, 0);
	}
	else if (i < 156) // frost motes drifting down
	{
		cycle(i, 1.4, 2.4, age, n);
		const float th = hash1(n + 4) * 6.283;
		s.c = at(b, n, lerp(0.0, 1.05, hash1(n + 3))) + ring_dir(b, th) * half_width * (1.5 + 3.0 * hash1(n + 5)) +
			float3(sin(time * 1.3 + th) * 0.03, 0.18 * age, cos(time * 1.1 + th) * 0.03);
		s.wid = s.len = 0.009 * (0.6 + 0.8 * hash1(n + 6));
		s.kind = kGlow;
		s.col = float4(0.7, 0.9, 1.1, 0) * bump(age) * 1.3;
	}
	else // ice shards growing off the edges toward the tip, then breaking off
	{
		cycle(i, 1.8, 2.8, age, n);
		const float t = lerp(0.08, 0.95, hash1(n + 3));
		const float side = hash1(n + 4) < 0.5 ? -1.0 : 1.0;
		const float3 out_dir = normalize(b.E * side + b.A * (0.25 + 0.5 * hash1(n + 5)) + b.F * (hash1(n + 6) - 0.5) * 0.6);
		const float grow = smoothstep(0.0, 0.25, age) * (1 - smoothstep(0.85, 1.0, age));
		const float len = half_width * (0.4 + 0.7 * hash1(n + 7)) * (1.1 - 0.5 * t) * grow;
		s.c = at(b, n, t) + b.E * side * half_width * 0.8 + out_dir * len;
		s.dir = out_dir;
		s.along = true;
		s.len = len;
		s.wid = len * 0.3;
		s.kind = kShard;
		s.col = float4(0.5, 0.8, 1.0, 0.5);
	}
	s.age = age;
	s.seed = hash1(n + 11);
	return s;
}

sprite wind_sprite(uint i, blade_t b)
{
	sprite s = (sprite)0;
	float age;
	uint n;
	if (i < 130) // wisps on three helices round the blade, racing toward the tip
	{
		cycle(i, 0.6, 1.1, age, n);
		const float band = float(i % 3);
		const float t = lerp(-0.08, 1.0, hash1(n + 3)) + age * 0.25;
		const float th = band * 2.094 + t * 11.0 - time * 9.0;
		const float r = half_width * (1.6 + 0.5 * sin(t * 5.0 + band));
		const float3 out_ = ring_dir(b, th);
		s.c = b.B + b.A * b.L * t + out_ * r;
		s.dir = normalize(-b.E * sin(th) + b.F * cos(th) - b.A * 0.2);
		s.along = true;
		s.len = 0.07 + 0.06 * hash1(n + 4);
		s.wid = 0.004;
		s.kind = kStreak;
		const float front = 0.45 + 0.55 * saturate(-dot(out_, forward()) * 0.5 + 0.5);
		s.col = float4(0.55, 1.0, 0.7, 0) * bump(age) * front * 0.9;
	}
	else // motes caught in it
	{
		cycle(i, 0.8, 1.4, age, n);
		const float t = hash1(n + 3);
		const float th = hash1(n + 4) * 6.283 - time * 6.0 + t * 8.0;
		s.c = at(b, n, t) + ring_dir(b, th) * half_width * (2.0 + 2.5 * hash1(n + 5)) + float3(0, -0.12 * age, 0);
		s.wid = s.len = 0.008;
		s.kind = kGlow;
		s.col = float4(0.6, 1.1, 0.6, 0) * bump(age);
	}
	s.age = age;
	s.seed = hash1(n + 11);
	return s;
}

sprite earth_sprite(uint i, blade_t b)
{
	sprite s = (sprite)0;
	float age;
	uint n;
	if (i < 30) // rock chips circling slowly, bobbing
	{
		cycle(i, 3.0, 4.5, age, n);
		const float th = hash1(n + 4) * 6.283 + time * 1.2 * (hash1(n + 5) < 0.5 ? -1.0 : 1.0);
		s.c = at(b, n, lerp(0.05, 1.0, hash1(n + 3))) + ring_dir(b, th) * half_width * (2.5 + 2.0 * hash1(n + 6)) +
			float3(0, sin(time * 2.0 + hash1(n + 7) * 6.28) * 0.02, 0);
		s.wid = s.len = (0.016 + 0.02 * hash1(n + 8)) * smoothstep(0.0, 0.1, age) * (1 - smoothstep(0.9, 1.0, age));
		s.kind = kRock;
		s.col = float4(float3(0.42, 0.32, 0.22) * (0.8 + 0.4 * hash1(n + 9)), 1);
	}
	else if (i < 150) // dust: amber glints and dark grit drifting down and out
	{
		cycle(i, 1.2, 2.0, age, n);
		const float3 out_ = ring_dir(b, hash1(n + 4) * 6.283);
		s.c = at(b, n, hash1(n + 3)) + out_ * half_width * (1.0 + 2.5 * hash1(n + 5) + age) + float3(0, 0.1 * age, 0);
		const bool grit = (i & 1) != 0;
		s.wid = s.len = grit ? 0.007 : 0.012;
		s.kind = kSoft;
		s.col = (grit ? float4(0.12, 0.09, 0.05, 0.5) : float4(0.9, 0.6, 0.3, 0) * 0.45) * bump(age);
	}
	else // glints on the blade
	{
		cycle(i, 0.6, 1.2, age, n);
		s.c = at(b, n, hash1(n + 3)) + ring_dir(b, hash1(n + 4) * 6.283) * half_width;
		s.wid = s.len = 0.025 * bump(age);
		s.kind = kStar;
		s.col = float4(1.0, 0.7, 0.3, 0);
	}
	s.age = age;
	s.seed = hash1(n + 11);
	return s;
}

)"
									R"(
sprite thunder_sprite(uint i, blade_t b)
{
	sprite s = (sprite)0;
	float age;
	uint n;
	if (i < 38) // arcs jumping along the blade, and some into the air
	{
		cycle(i, 0.10, 0.22, age, n);
		const float t0 = hash1(n + 3) * 0.85;
		const float3 p0 = at(b, n, t0) + b.E * (hash1(n + 4) < 0.5 ? -1.0 : 1.0) * half_width;
		float3 p1;
		if (i < 30)
			p1 = at(b, n + 977u, min(t0 + 0.1 + 0.25 * hash1(n + 5), 1.0)) + b.E * (hash1(n + 6) < 0.5 ? -1.0 : 1.0) * half_width;
		else
			p1 = p0 + normalize(ring_dir(b, hash1(n + 5) * 6.283) + float3(0, -0.6, 0)) * (0.2 + 0.25 * hash1(n + 6));
		s.c = 0.5 * (p0 + p1);
		s.dir = normalize(p1 - p0 + 1e-6);
		s.along = true;
		s.len = 0.5 * length(p1 - p0);
		s.wid = s.len * 0.25 + half_width * 0.5;
		s.kind = kBolt;
		s.col = float4(0.6, 0.45, 1.0, 0) * (hash1(n + 20) < 0.5 ? 0.0 : 1.0); // (some blink out)
	}
	else // sparks flying off
	{
		cycle(i, 0.15, 0.35, age, n);
		const float3 v = normalize(ring_dir(b, hash1(n + 4) * 6.283) + float3(0, -0.5, 0) + b.A * (hash1(n + 5) - 0.5));
		s.c = at(b, n, hash1(n + 3)) + v * (half_width + 0.25 * age) + float3(0, 0.15 * age * age, 0);
		s.dir = v;
		s.along = true;
		s.len = 0.02;
		s.wid = 0.004;
		s.kind = kStreak;
		s.col = float4(0.8, 0.75, 1.2, 0) * (1 - age) * 1.5;
	}
	s.age = age;
	s.seed = hash1(n + 11);
	return s;
}

sprite water_sprite(uint i, blade_t b)
{
	sprite s = (sprite)0;
	float age;
	uint n;
	if (i < 50) // drops running off the edges and falling
	{
		cycle(i, 0.5, 0.9, age, n);
		const float side = hash1(n + 4) < 0.5 ? -1.0 : 1.0;
		s.c = at(b, n, lerp(0.05, 1.0, hash1(n + 3))) + b.E * side * half_width * (0.8 + age * 0.6) + float3(0, 0.9 * age * age, 0);
		s.wid = (0.010 + 0.008 * hash1(n + 5)) * smoothstep(0.0, 0.15, age);
		s.len = s.wid * 1.25;
		s.kind = kDrop;
		s.col = float4(0.35, 0.6, 1.0, 0.5);
	}
	else if (i < 150) // a stream wound twice round the blade, flowing to the tip
	{
		cycle(i, 0.7, 1.1, age, n);
		const float band = float(i & 1);
		const float t = lerp(-0.05, 1.0, hash1(n + 3)) + age * 0.2;
		const float th = band * kPi + t * 8.0 - time * 5.0;
		const float3 out_ = ring_dir(b, th);
		s.c = b.B + b.A * b.L * t + out_ * half_width * 1.9;
		s.dir = normalize(-b.E * sin(th) + b.F * cos(th) - b.A * 0.4);
		s.along = true;
		s.len = 0.07;
		s.wid = 0.012;
		s.kind = kStreak;
		const float front = 0.4 + 0.6 * saturate(-dot(out_, forward()) * 0.5 + 0.5);
		s.col = float4(0.25, 0.55, 1.0, 0.2) * bump(age) * front;
	}
	else // bubbles rising
	{
		cycle(i, 1.0, 1.8, age, n);
		const float th = hash1(n + 4) * 6.283;
		s.c = at(b, n, hash1(n + 3)) + ring_dir(b, th) * half_width * (1.5 + 2.0 * hash1(n + 5)) +
			float3(sin(time * 4.0 + th) * 0.02, -0.15 * age, 0);
		s.wid = s.len = 0.012 + 0.01 * hash1(n + 6);
		s.kind = kRing;
		s.col = float4(0.6, 0.85, 1.0, 0) * bump(age);
	}
	s.age = age;
	s.seed = hash1(n + 11);
	return s;
}

sprite light_sprite(uint i, blade_t b)
{
	sprite s = (sprite)0;
	float age;
	uint n;
	if (i < 50) // stars rising off it
	{
		cycle(i, 0.8, 1.4, age, n);
		s.c = at(b, n, hash1(n + 3)) + ring_dir(b, hash1(n + 4) * 6.283) * half_width * (1.0 + 2.0 * hash1(n + 5)) + float3(0, -0.15 * age, 0);
		s.wid = s.len = (0.03 + 0.03 * hash1(n + 6)) * bump(age);
		s.kind = kStar;
		s.col = float4(1.2, 1.1, 0.8, 0);
	}
	else if (i < 140) // motes drifting up
	{
		cycle(i, 1.0, 1.8, age, n);
		s.c = at(b, n, hash1(n + 3)) + ring_dir(b, hash1(n + 4) * 6.283) * half_width * (1.0 + 3.0 * hash1(n + 5)) + float3(0, -0.35 * age, 0);
		s.wid = s.len = 0.008;
		s.kind = kGlow;
		s.col = float4(1.0, 0.85, 0.45, 0) * bump(age) * 1.2;
	}
	else // rays fanning out, turning slowly
	{
		cycle(i, 1.5, 2.5, age, n);
		const float3 p = at(b, n, lerp(0.1, 0.95, hash1(n + 3)));
		const float3 d = normalize(ring_dir(b, hash1(n + 4) * 6.283 + time * 0.6) + b.A * (hash1(n + 5) - 0.5) * 0.5);
		const float len = (0.08 + 0.14 * hash1(n + 6)) * bump(age);
		s.c = p + d * len;
		s.dir = d;
		s.along = true;
		s.len = len;
		s.wid = 0.012;
		s.kind = kRay;
		s.col = float4(1.0, 0.92, 0.7, 0) * 0.7;
	}
	s.age = age;
	s.seed = hash1(n + 11);
	return s;
}

sprite dark_sprite(uint i, blade_t b)
{
	sprite s = (sprite)0;
	float age;
	uint n;
	if (i < 100) // smoke welling up off it
	{
		cycle(i, 1.0, 1.8, age, n);
		const float3 jitter = (float3(hash1(n + 4), hash1(n + 5), hash1(n + 6)) - 0.5) * half_width * 2.0;
		s.c = at(b, n, hash1(n + 3)) + jitter + float3(sin(time * 1.7 + hash1(n + 7) * 6.28) * 0.03 * age, -0.25 * age, 0);
		s.wid = s.len = half_width * (1.5 + 1.5 * hash1(n + 8)) * (0.6 + 0.8 * age);
		s.kind = kSmoke;
		s.col = float4(0.04, 0.0, 0.07, 0.7) * bump(age);
	}
	else if (i < 150) // violet wisps in it
	{
		cycle(i, 0.8, 1.4, age, n);
		s.c = at(b, n, hash1(n + 3)) + (float3(hash1(n + 4), hash1(n + 5), hash1(n + 6)) - 0.5) * half_width * 2.0 + float3(0, -0.35 * age, 0);
		s.wid = s.len = half_width * (0.6 + 0.6 * hash1(n + 8));
		s.kind = kSmoke;
		s.col = float4(0.35, 0.08, 0.6, 0) * 0.28 * bump(age);
	}
	else // motes
	{
		cycle(i, 1.0, 1.8, age, n);
		s.c = at(b, n, hash1(n + 3)) + ring_dir(b, hash1(n + 4) * 6.283) * half_width * (1.0 + 2.5 * hash1(n + 5)) + float3(0, -0.25 * age, 0);
		s.wid = s.len = 0.008;
		s.kind = kGlow;
		s.col = float4(0.8, 0.4, 1.3, 0) * bump(age);
	}
	s.age = age;
	s.seed = hash1(n + 11);
	return s;
}

)"
									R"(
struct p2p
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	float age : TEXCOORD1;
	float seed : TEXCOORD2;
	nointerpolation uint kind : TEXCOORD3;
	float4 col : TEXCOORD4;
};
p2p pvs(uint vid : SV_VertexID)
{
	const uint i = vid / 6;
	const float2 q = kQuad[vid % 6];
	const blade_t b = blade_now();
	sprite s;
	switch (element)
	{
	case 1: s = fire_sprite(i, b); break;
	case 2: s = ice_sprite(i, b); break;
	case 3: s = wind_sprite(i, b); break;
	case 4: s = earth_sprite(i, b); break;
	case 5: s = thunder_sprite(i, b); break;
	case 6: s = water_sprite(i, b); break;
	case 7: s = light_sprite(i, b); break;
	default: s = dark_sprite(i, b); break;
	}
	// (a third of them left out, the rest a little smaller: kept subtle)
	if (i % 3 == 2)
		s.wid = s.len = 0;
	s.wid *= 0.85;
	if (!s.along)
		s.len *= 0.85;
	float3 world;
	if (s.along)
	{
		// (lying along dir, turned round it to face the camera)
		float3 side = cross(s.dir, forward());
		side = dot(side, side) > 1e-8 ? normalize(side) : normalize(float3(view_proj._11, view_proj._21, view_proj._31));
		world = s.c + s.dir * s.len * q.y + side * s.wid * q.x;
	}
	else
	{
		const float3 right = normalize(float3(view_proj._11, view_proj._21, view_proj._31));
		const float3 up = normalize(float3(view_proj._12, view_proj._22, view_proj._32));
		world = s.c + right * s.wid * q.x + up * s.len * q.y;
	}
	p2p r;
	r.pos = mul(float4(world, 1), view_proj);
	r.uv = q;
	r.age = s.age;
	r.seed = s.seed;
	r.kind = s.kind;
	r.col = s.col;
	return r;
}
float4 pps(p2p i) : SV_Target
{
	const float2 d = i.uv; // (-1..1; y along the sprite's length, or up)
	const float r2 = dot(d, d);
	float4 o = 0;
	switch (i.kind)
	{
	case kFlame: // a tongue of fire, narrower toward its top
	{
		const float2 e = float2(d.x * (1.0 + 0.7 * (d.y * 0.5 + 0.5)), d.y);
		const float n = fbm(float3(e * 1.9, i.seed * 13.0 + time * 2.2));
		float shape = saturate(1 - length(e) - (n - 0.5) * 0.7);
		shape *= shape;
		o = float4(fire_ramp(saturate(shape * (1 - i.age) * 1.3 + 0.1 * (1 - i.age))) * shape * (1 - 0.7 * i.age) * 0.5, 0);
		break;
	}
	case kGlow:
	{
		const float e = saturate(1 - r2);
		o = i.col * e * e;
		break;
	}
	case kStar: // four points and a core, twinkling
	{
		const float ax = abs(d.x), ay = abs(d.y);
		const float star = saturate(1 - (ax * 7 + ay)) + saturate(1 - (ay * 7 + ax)) + saturate(1 - r2 * 5) * 0.6;
		o = float4(i.col.rgb * star * (0.55 + 0.45 * sin(time * 9.0 + i.seed * 31.0)), 0);
		break;
	}
	case kSoft:
	{
		const float e = saturate(1 - r2);
		o = i.col * e * e;
		break;
	}
	case kStreak: // soft across, fading at both ends
	{
		const float across = saturate(1 - abs(d.x));
		o = i.col * across * across * saturate(1 - d.y * d.y);
		break;
	}
	case kShard: // a sliver of ice: wide at its root, a point at its tip, edges bright
	{
		const float m = abs(d.x) / max((1 - d.y) * 0.5, 1e-3);
		if (m > 1)
			discard;
		const float edge = smoothstep(0.55, 1.0, m);
		o = float4(lerp(i.col.rgb, float3(0.9, 0.97, 1.0), edge) * (0.55 + 0.45 * edge), i.col.a);
		break;
	}
	case kRock: // a lumpy stone, lit from above, an amber rim
	{
		const float ang = atan2(d.y, d.x);
		const float edge = 0.7 + 0.3 * noise3(float3(cos(ang) * 2.0, sin(ang) * 2.0, i.seed * 10.0));
		const float r = sqrt(r2);
		if (r > edge)
			discard;
		const float3 nrm = normalize(float3(d, sqrt(saturate(edge * edge - r2)) + 0.2));
		const float shade = 0.45 + 0.55 * saturate(dot(nrm, normalize(float3(-0.4, 0.7, 0.6))));
		const float facet = 0.85 + 0.15 * noise3(float3(d * 5.0, i.seed * 7.0));
		const float rim = smoothstep(edge - 0.15, edge, r);
		o = float4(i.col.rgb * shade * facet + float3(1.0, 0.55, 0.15) * rim * 0.5, 1);
		break;
	}
	case kBolt: // a jagged line across the sprite's length, a new shape many times a second
	{
		const float step = floor(time * 24.0);
		const float x = d.y;
		const float y = ((noise3(float3(x * 3.0 + i.seed * 20.0, step, 0)) - 0.5) * 1.2 + (noise3(float3(x * 9.0, i.seed * 7.0, step)) - 0.5) * 0.5) * (1 - x * x);
		const float dist = abs(d.x - y);
		const float core = saturate(1 - dist * 14.0), glow = saturate(1 - dist * 3.0);
		o = float4(float3(1, 1, 1) * core * any(i.col.rgb) + i.col.rgb * glow * glow, 0) * saturate((1 - abs(x)) * 6.0);
		break;
	}
	case kDrop: // a drop: rounder below, a highlight, clear in the middle
	{
		const float2 e = float2(d.x * (1.0 + max(d.y, 0.0) * 0.8), d.y);
		const float r = length(e);
		if (r > 1)
			discard;
		const float rim = smoothstep(0.55, 1.0, r);
		const float highlight = saturate(1 - length(d - float2(-0.3, 0.35)) * 4.0);
		const float cover = (0.3 + 0.5 * rim) * i.col.a;
		o = float4(i.col.rgb * cover + highlight * 0.8, cover);
		break;
	}
	case kRing: // a bubble
	{
		const float r = sqrt(r2);
		const float ring = saturate(1 - abs(r - 0.8) * 8.0) + saturate(1 - length(d - float2(-0.35, 0.35)) * 6.0);
		o = float4(i.col.rgb * ring, 0);
		break;
	}
	case kSmoke: // a puff, ragged at its edge
	{
		const float n = fbm(float3(d * 1.5, i.seed * 9.0 + time * 0.4));
		float m = saturate(1 - r2 - (n - 0.5) * 0.9);
		o = i.col * m * m;
		break;
	}
	default: // a ray: soft across, fading toward its far end
	{
		const float across = saturate(1 - abs(d.x));
		const float t = d.y * 0.5 + 0.5;
		o = i.col * across * across * across * (1 - t) * smoothstep(0.0, 0.1, t);
		break;
	}
	}
	return float4(o.rgb * kSubtle, o.a * (i.kind == kRock ? 1.0 : 0.8)) * strength;
}
)";
		constexpr uint32_t kEnSpriteCount = 186; // (kSprites in the shader)

		constexpr UINT64 kConstantsBytes = 256;
		struct gpu_model
		{
			float light_dir[4][4];
			float light_diffuse[4][4];
			float light_ambient[4][4];
			float ambient[4];
			float material_ambient[4];
			float tfactor[4];
			float fog[4];
			float fog_end[4];
			float lamp_pos[kModelLamps][4];    // (model_lamps)
			float lamp_colour[kModelLamps][4];
			float overlay[4];          // (the extra passes: model_constants)
			float overlay_colour[4];
			float cloak[4];
			float cloak_colour[4];
		};
		static_assert(sizeof(gpu_model) == 464 && kModelLamps == 4, "the shader's model_t");
		struct gpu_constants
		{
			float view_proj[16];
			float fog[4];
			float fog_end[4];
			float options[4];
			float sun_dir[4];
			float camera[4];
		};
		static_assert(sizeof(gpu_constants) <= kConstantsBytes);

		// The camera (world) a VIEW x PROJECTION (row vectors) looks from: the one point it takes to clip x, y and w 0. False
		// for none (an orthographic one, a matrix not read).
		bool camera_of(const float m[16], float eye[3])
		{
			// (p x M[0..2][j] = -M[3][j] for the columns j 0, 1, 3, by Cramer's rule)
			const int col[3] = { 0, 1, 3 };
			double a[3][3], b[3];
			for (int r = 0; r < 3; ++r)
			{
				for (int i = 0; i < 3; ++i)
					a[r][i] = m[i * 4 + col[r]];
				b[r] = -m[12 + col[r]];
			}
			auto det3 = [](const double x[3][3]) {
				return x[0][0] * (x[1][1] * x[2][2] - x[1][2] * x[2][1]) - x[0][1] * (x[1][0] * x[2][2] - x[1][2] * x[2][0]) + x[0][2] * (x[1][0] * x[2][1] - x[1][1] * x[2][0]);
			};
			const double d = det3(a);
			if (!(std::fabs(d) > 1e-12))
				return false;
			for (int i = 0; i < 3; ++i)
			{
				double t[3][3];
				std::memcpy(t, a, sizeof(t));
				for (int r = 0; r < 3; ++r)
					t[r][i] = b[r];
				eye[i] = float(det3(t) / d);
			}
			return std::isfinite(eye[0]) && std::isfinite(eye[1]) && std::isfinite(eye[2]);
		}
		// (a slot: the frame's draw's constants and models, then its shadow's: record_casters may come while the GPU has yet
		// to read the draw's)
		constexpr UINT64 kHalfSlotBytes = (kConstantsBytes + UINT64(kMaxModels) * sizeof(gpu_model) + 255) & ~UINT64(255);
		constexpr UINT64 kSlotBytes = 2 * kHalfSlotBytes;
		constexpr UINT64 kModelsAt = kConstantsBytes;

		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		// A D3DCOLOR as rgba 0..1
		void colour_of(uint32_t c, float out[4])
		{
			out[0] = float((c >> 16) & 255) / 255.0f;
			out[1] = float((c >> 8) & 255) / 255.0f;
			out[2] = float(c & 255) / 255.0f;
			out[3] = float(c >> 24) / 255.0f;
		}

		void model_constants(const frame &f, const model_entry &m, gpu_model &g)
		{
			std::memset(&g, 0, sizeof(g));
			for (int k = 0; k < 4; ++k)
			{
				const light8 &l = m.lights[k];
				if (l.type == 0)
					continue;
				// (a directional light shines along its Direction: toward it is minus that; the game's lamps come
				// here already turned directional)
				const float d[3] = { -l.direction[0], -l.direction[1], -l.direction[2] };
				const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				if (!(n > 1e-6f))
					continue;
				g.light_dir[k][0] = d[0] / n;
				g.light_dir[k][1] = d[1] / n;
				g.light_dir[k][2] = d[2] / n;
				g.light_dir[k][3] = 1.0f;
				std::memcpy(g.light_diffuse[k], l.diffuse, 16);
				std::memcpy(g.light_ambient[k], l.ambient, 16);
			}
			colour_of(m.ambient, g.ambient);
			// (02B846-02B8E9: the material's ambient is the colour's bytes / 128, byte 0 (blue) to red)
			g.material_ambient[0] = float(m.colour & 255) / 128.0f;
			g.material_ambient[1] = float((m.colour >> 8) & 255) / 128.0f;
			g.material_ambient[2] = float((m.colour >> 16) & 255) / 128.0f;
			g.material_ambient[3] = float(m.colour >> 24) / 128.0f;
			colour_of((m.tfactor & 0x00FFFFFFu) | 0x80000000u, g.tfactor);
			if (m.fog_read)
			{
				colour_of(m.fog_colour, g.fog);
				const float span = m.fog_end - m.fog_start;
				g.fog[3] = span > 1e-3f ? 1.0f / span : 0.0f;
				g.fog_end[0] = m.fog_end;
			}
			else
				g.fog[3] = -1.0f; // (the frame's)
			// The extra passes: the overlay's offset and factor; the cloak's offsets through PROJECTION (02BA57: (a, b, 0)
			// through it, its x and y added to the texture matrix's translation; a perspective one's _31 _32 _41 _42 are
			// 0 as the game makes it, and kept here as it has them) and factor
			if (m.passes & kPassOverlay)
			{
				g.overlay[0] = m.overlay_offset[0];
				g.overlay[1] = m.overlay_offset[1];
				colour_of(m.overlay_colour, g.overlay_colour);
			}
			if ((m.passes & kPassCloak) && f.projection_read)
			{
				const float *p = f.projection;
				g.cloak[0] = m.cloak_offset[0] * p[0] + m.cloak_offset[1] * p[4] + p[12];
				g.cloak[1] = m.cloak_offset[0] * p[1] + m.cloak_offset[1] * p[5] + p[13];
				colour_of(m.cloak_factor, g.cloak_colour);
			}
		}

		// The zone's lamps whose range reaches the model's bones' box (its instances' palette rows' places, half a yalm
		// round for the skin), up to kModelLamps, the strongest at the box first; and the game's own lamp slots left out
		// where they're one of those (chardraw.hpp "Lamps"): a slot not the sun's (as lit's share tells it) pointing from
		// the model's root (its first instance's first row: the actor's place) toward one of them within about 18 degrees.
		// The model's place is the palette's, world space (WORLD is identity for characters).
		void model_lamps(const frame &f, const model_entry &m, const char_options &o, gpu_model &g)
		{
			if (o.lamp_count == 0 || !o.sun_known)
				return;
			float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, root[3] = {};
			bool any = false;
			for (uint32_t ii = 0; ii < m.instances && m.first_instance + ii < kMaxInstances; ++ii)
			{
				const instance_entry &ie = f.instance[m.first_instance + ii];
				for (uint32_t b = 0; b < ie.bones && ie.first_bone + b < kMaxBones; ++b)
				{
					const float *t = f.palette[ie.first_bone + b].m + 9;
					if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2]))
						continue;
					if (!any)
						std::memcpy(root, t, sizeof(root));
					any = true;
					for (int k = 0; k < 3; ++k)
					{
						lo[k] = std::min(lo[k], t[k] - 0.5f);
						hi[k] = std::max(hi[k], t[k] + 0.5f);
					}
				}
			}
			if (!any)
				return;
			uint32_t index[kFrameLamps];
			float strength[kFrameLamps];
			uint32_t n = 0;
			for (uint32_t i = 0; i < std::min(o.lamp_count, kFrameLamps); ++i)
			{
				const char_lamp &l = o.lamps[i];
				if (!(l.range > 0.0f) || !std::isfinite(l.range))
					continue;
				float d2 = 0.0f;
				for (int k = 0; k < 3; ++k)
				{
					const float out_by = std::max(lo[k] - l.pos[k], l.pos[k] - hi[k]);
					d2 += out_by > 0.0f ? out_by * out_by : 0.0f;
				}
				if (!(d2 < l.range * l.range))
					continue;
				index[n] = i;
				strength[n] = (0.2126f * l.colour[0] + 0.7152f * l.colour[1] + 0.0722f * l.colour[2]) / std::max(l.attenuation2 * d2, 1e-3f);
				n++;
			}
			for (uint32_t a = 0; a < n && a < kModelLamps; ++a)
			{
				uint32_t best = a;
				for (uint32_t b = a + 1; b < n; ++b)
					best = strength[b] > strength[best] ? b : best;
				std::swap(index[a], index[best]);
				std::swap(strength[a], strength[best]);
				const char_lamp &l = o.lamps[index[a]];
				std::memcpy(g.lamp_pos[a], l.pos, 12);
				g.lamp_pos[a][3] = l.range;
				std::memcpy(g.lamp_colour[a], l.colour, 12);
				g.lamp_colour[a][3] = l.attenuation2 > 0.0f ? l.attenuation2 : 0.5f;
			}
			// (the game's lamp slot: its light is one of these now)
			for (int k = 0; k < 4; ++k)
			{
				const float *d = g.light_dir[k];
				if (d[3] == 0.0f || d[0] * o.sun[0] + d[1] * o.sun[1] + d[2] * o.sun[2] > 0.985f)
					continue;
				for (uint32_t a = 0; a < n && a < kModelLamps; ++a)
				{
					const float v[3] = { g.lamp_pos[a][0] - root[0], g.lamp_pos[a][1] - root[1], g.lamp_pos[a][2] - root[2] };
					const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
					if (len > 1e-3f && (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / len > 0.95f)
					{
						g.light_dir[k][3] = 0.0f;
						break;
					}
				}
			}
		}

		enum kind
		{
			kModel = 0,
			kComposite = 1,
			kEnShell = 2,
			kEnSprites = 3,
			kModelMotion = 4, // (the model with the motion target)
			kModelLight = 5,  // (... with the light split's: light_split.hpp)
			kModelLightMotion = 6, // (... with both: the light split second, the motion third)
			kOverlay = 7, // (the extra passes: chardraw.hpp)
			kCloak = 8,
		};

		struct en_constants
		{
			float view_proj[16];
			float time;
			uint32_t element, bone_count, vertex_count, corner_count, bone;
			float half_width, strength;
			float base[3];
			uint32_t edge_axis;
			float tip[3];
			uint32_t blade_kind;
		};
		static_assert(sizeof(en_constants) == 32 * 4, "the en-spell shader's b0");
	}

	char_renderer::~char_renderer()
	{
		release();
	}

	void char_renderer::release()
	{
		for (auto &p : _pipelines)
			if (p.second != nullptr)
				p.second->Release();
		_pipelines.clear();
		drop(_shadow_pso);
		if (_ring != nullptr && _mapped != nullptr)
			_ring->Unmap(0, nullptr);
		_mapped = nullptr;
		drop(_ring);
		drop(_root);
		drop(_en_root);
		for (void **b : { &_vs, &_ps, &_ps_motion, &_ps_light, &_ps_light_motion, &_cvs, &_cps, &_cps_ms, &_sps, &_en_svs, &_en_sps, &_en_pvs, &_en_pps, &_ps_overlay, &_ps_cloak })
		{
			if (*b != nullptr)
				static_cast<ID3DBlob *>(*b)->Release();
			*b = nullptr;
		}
		_device = nullptr;
	}

	bool char_renderer::init(ID3D12Device *device, std::string &why)
	{
		release();
		_device = device;
		using compile_fn = decltype(&D3DCompile);
		HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
		const auto compile = compiler != nullptr ? reinterpret_cast<compile_fn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
		if (compile == nullptr)
		{
			why = "d3dcompiler_47.dll isn't there";
			return false;
		}
		struct entry
		{
			const char *name, *target;
			void **out;
		};
		const entry entries[] = { { "vs", "vs_5_0", &_vs }, { "ps", "ps_5_0", &_ps }, { "ps_motion", "ps_5_0", &_ps_motion }, { "ps_light", "ps_5_0", &_ps_light }, { "ps_light_motion", "ps_5_0", &_ps_light_motion }, { "cvs", "vs_5_0", &_cvs }, { "cps", "ps_5_0", &_cps }, { "cps_ms", "ps_5_0", &_cps_ms }, { "sps", "ps_5_0", &_sps },
			{ "ps_overlay", "ps_5_0", &_ps_overlay }, { "ps_cloak", "ps_5_0", &_ps_cloak },
			{ "svs", "vs_5_0", &_en_svs }, { "sps", "ps_5_0", &_en_sps }, { "pvs", "vs_5_0", &_en_pvs }, { "pps", "ps_5_0", &_en_pps } };
		for (const entry &e : entries)
		{
			ID3DBlob *code = nullptr, *errors = nullptr;
			const bool en = e.out == &_en_svs || e.out == &_en_sps || e.out == &_en_pvs || e.out == &_en_pps;
			const std::string src = en ? std::string(kEnShader) : std::string(light::kHlsl) + light::kHemiSpecHlsl + kShader;
			if (FAILED(compile(src.data(), src.size(), en ? "hifixi_enspells" : "hifixi_chars", nullptr, nullptr, e.name, e.target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
			{
				why = std::string("the character shader didn't compile: ") + (errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
				if (errors != nullptr)
					errors->Release();
				release();
				return false;
			}
			if (errors != nullptr)
				errors->Release();
			*e.out = code;
		}

		D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2 } };
		D3D12_ROOT_PARAMETER p[7] = {};
		p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		p[0].Descriptor.ShaderRegister = 0;
		p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		p[1].Constants.ShaderRegister = 1;
		p[1].Constants.Num32BitValues = 9;
		p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		p[2].Descriptor.ShaderRegister = 3;
		p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		p[3].Descriptor.ShaderRegister = 4;
		for (UINT i = 0; i < 2; ++i)
		{
			p[4 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			p[4 + i].DescriptorTable.NumDescriptorRanges = 1;
			p[4 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
		}
		p[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // (the last frame's palette rows for the instance: t5)
		p[6].Descriptor.ShaderRegister = 5;
		D3D12_STATIC_SAMPLER_DESC s[2] = {};
		s[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		s[0].AddressU = s[0].AddressV = s[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		s[0].MaxLOD = D3D12_FLOAT32_MAX;
		s[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		// (s1: the extra passes', WRAP as 025C40 and 025330 set it)
		s[1] = s[0];
		s[1].ShaderRegister = 1;
		s[1].AddressU = s[1].AddressV = s[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		const D3D12_ROOT_SIGNATURE_DESC rd = { 7, p, 2, s, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		using serialize_fn = decltype(&D3D12SerializeRootSignature);
		const auto serialize = reinterpret_cast<serialize_fn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
		ID3DBlob *blob = nullptr, *errors = nullptr;
		if (serialize == nullptr || FAILED(serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
			FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_root))))
		{
			why = "the characters' root signature couldn't be made";
			if (blob != nullptr)
				blob->Release();
			if (errors != nullptr)
				errors->Release();
			release();
			return false;
		}
		blob->Release();

		// The en-spells' root signature: 0 32 constants b0, 1 SRV t3 (the palette), 2 table t0-t1 (the blade's mesh)
		{
			D3D12_ROOT_PARAMETER q[3] = {};
			q[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			q[0].Constants.ShaderRegister = 0;
			q[0].Constants.Num32BitValues = 32;
			q[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			q[1].Descriptor.ShaderRegister = 3;
			q[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			q[2].DescriptorTable.NumDescriptorRanges = 1;
			q[2].DescriptorTable.pDescriptorRanges = &ranges[0];
			const D3D12_ROOT_SIGNATURE_DESC ed = { 3, q, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
			blob = nullptr;
			errors = nullptr;
			if (FAILED(serialize(&ed, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
				FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_en_root))))
			{
				why = "the en-spells' root signature couldn't be made";
				if (blob != nullptr)
					blob->Release();
				if (errors != nullptr)
					errors->Release();
				release();
				return false;
			}
			blob->Release();
		}

		// The shadow pipeline: depth only into a D32 map, LESS, no culling (as the character pass)
		D3D12_GRAPHICS_PIPELINE_STATE_DESC sd = {};
		sd.pRootSignature = _root;
		sd.VS = { static_cast<ID3DBlob *>(_vs)->GetBufferPointer(), static_cast<ID3DBlob *>(_vs)->GetBufferSize() };
		sd.PS = { static_cast<ID3DBlob *>(_sps)->GetBufferPointer(), static_cast<ID3DBlob *>(_sps)->GetBufferSize() };
		sd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		sd.NumRenderTargets = 0;
		sd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		sd.SampleDesc.Count = 1;
		sd.SampleMask = UINT_MAX;
		sd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		sd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		sd.RasterizerState.DepthClipEnable = TRUE;
		sd.DepthStencilState.DepthEnable = TRUE;
		sd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		sd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		if (FAILED(device->CreateGraphicsPipelineState(&sd, IID_PPV_ARGS(&_shadow_pso))))
		{
			why = "the characters' shadow pipeline couldn't be made";
			release();
			return false;
		}

		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = kSlotBytes * kFrames;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		const D3D12_RANGE none = { 0, 0 };
		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_ring))) ||
			FAILED(_ring->Map(0, &none, reinterpret_cast<void **>(&_mapped))))
		{
			why = "the characters' upload ring couldn't be made";
			release();
			return false;
		}
		return true;
	}

	ID3D12PipelineState *char_renderer::pipeline(const char_target &t, int k)
	{
		const bool model = k == kModel || k == kModelMotion || k == kModelLight || k == kModelLightMotion;
		const bool motion = k == kModelMotion || k == kModelLightMotion, light = k == kModelLight || k == kModelLightMotion;
		const uint64_t key = uint64_t(t.rtv_format) | uint64_t(t.dsv_format) << 8 | uint64_t(t.samples) << 16 | uint64_t(k) << 24 |
			(motion ? uint64_t(t.motion_format) << 32 : 0);
		const auto known = _pipelines.find(key);
		if (known != _pipelines.end())
			return known->second;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		const bool en = k == kEnShell || k == kEnSprites, extra = k == kOverlay || k == kCloak;
		d.pRootSignature = en ? _en_root : _root;
		ID3DBlob *const vs = static_cast<ID3DBlob *>(model || extra ? _vs : k == kEnShell ? _en_svs : k == kEnSprites ? _en_pvs : _cvs);
		ID3DBlob *const ps = static_cast<ID3DBlob *>(k == kModel ? _ps : k == kModelMotion ? _ps_motion : k == kModelLight ? _ps_light : k == kModelLightMotion ? _ps_light_motion :
			k == kOverlay ? _ps_overlay : k == kCloak ? _ps_cloak : k == kEnShell ? _en_sps : k == kEnSprites ? _en_pps : t.samples > 1 ? _cps_ms : _cps);
		d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		d.NumRenderTargets = 1 + (light ? 1 : 0) + (motion ? 1 : 0);
		d.RTVFormats[0] = t.rtv_format;
		if (light)
			d.RTVFormats[1] = light::kFormat;
		if (motion)
			d.RTVFormats[light ? 2 : 1] = t.motion_format;
		d.SampleDesc.Count = t.samples;
		d.SampleMask = UINT_MAX;
		d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // (the character pass: CULL NONE)
		d.RasterizerState.DepthClipEnable = TRUE;
		d.RasterizerState.MultisampleEnable = t.samples > 1;
		D3D12_RENDER_TARGET_BLEND_DESC &b = d.BlendState.RenderTarget[0];
		b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		if (model)
		{
			d.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			d.BlendState.RenderTarget[2].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			d.DSVFormat = t.dsv_format;
			d.DepthStencilState.DepthEnable = TRUE;
			d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		}
		else if (extra)
		{
			// (the extra passes: added, alpha too (Direct3D 8 blends it with the same factors); tested against the
			// depth, none written)
			d.DSVFormat = t.dsv_format;
			d.DepthStencilState.DepthEnable = TRUE;
			d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			b.BlendEnable = TRUE;
			b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
			b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_ONE;
			b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		}
		else if (en)
		{
			// (premultiplied: added on where alpha is 0 (glows), covering where it isn't (smoke, rock, ice); colour only;
			// tested against the world's depth, none written)
			d.DSVFormat = t.dsv_format;
			d.DepthStencilState.DepthEnable = TRUE;
			d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
			d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
			b.BlendEnable = TRUE;
			b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ONE;
			b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
		}
		else
		{
			// (the composite: no depth; blended as Direct3D 8 blends, alpha as colour)
			d.DSVFormat = DXGI_FORMAT_UNKNOWN;
			b.BlendEnable = TRUE;
			b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
			b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		}
		ID3D12PipelineState *pso = nullptr;
		if (FAILED(_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))))
			pso = nullptr;
		_pipelines[key] = pso;
		return pso;
	}

	// The frame's constants (seen through view_proj) and its models' into half `part` of its seq's slot; its GPU address
	D3D12_GPU_VIRTUAL_ADDRESS char_renderer::write_frame(const frame &f, const char_options &o, const float view_proj[16], uint32_t part)
	{
		const uint32_t seq = f.seq.load(std::memory_order_acquire);
		const UINT64 slot = UINT64(seq % kFrames) * kSlotBytes + UINT64(part) * kHalfSlotBytes;
		gpu_constants c = {};
		std::memcpy(c.view_proj, view_proj, sizeof(c.view_proj));
		const float span = f.fog_end - f.fog_start;
		c.fog[0] = f.fog[0];
		c.fog[1] = f.fog[1];
		c.fog[2] = f.fog[2];
		c.fog[3] = span > 1e-3f ? 1.0f / span : 0.0f;
		c.fog_end[0] = f.fog_end;
		c.options[0] = o.per_pixel ? 1.0f : 0.0f;
		c.options[1] = float(std::min(f.models, kMaxModels));
		if (o.sun_known)
		{
			std::memcpy(c.sun_dir, o.sun, 12);
			c.sun_dir[3] = 1.0f;
		}
		// (the frame's own camera, whatever view_proj the draw's seen through)
		if (f.camera && camera_of(f.view_proj, c.camera))
			c.camera[3] = 1.0f;
		std::memcpy(_mapped + slot, &c, sizeof(c));
		gpu_model *gm = reinterpret_cast<gpu_model *>(_mapped + slot + kModelsAt);
		for (uint32_t i = 0; i < std::min(f.models, kMaxModels); ++i)
		{
			model_constants(f, f.model[i], gm[i]);
			model_lamps(f, f.model[i], o, gm[i]);
		}
		return _ring->GetGPUVirtualAddress() + slot;
	}

	bool char_renderer::begin(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, const char_options &o)
	{
		ID3D12DescriptorHeap *const heap = cache::heap();
		const bool motion = t.motion_rtv.ptr != 0 && t.samples == 1, light = t.light_rtv.ptr != 0 && t.samples == 1;
		ID3D12PipelineState *const pso = pipeline(t, light ? (motion ? kModelLightMotion : kModelLight) : motion ? kModelMotion : kModel);
		if (_root == nullptr || heap == nullptr || pso == nullptr || !f.camera || f.models == 0)
			return false;
		// The frame's constants (rewritten for each draw of the frame: the same values) and its models'
		const D3D12_GPU_VIRTUAL_ADDRESS base = write_frame(f, o, f.view_proj, 0);

		const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = { t.rtv, light ? t.light_rtv : t.motion_rtv, t.motion_rtv };
		list->OMSetRenderTargets(1 + (light ? 1 : 0) + (motion ? 1 : 0), rtvs, FALSE, &t.dsv);
		const D3D12_VIEWPORT vp = { t.offset, t.offset, t.width, t.height, 0.0f, 1.0f };
		const D3D12_RECT sc = { 0, 0, LONG(t.width), LONG(t.height) };
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sc);
		list->SetDescriptorHeaps(1, &heap);
		list->SetGraphicsRootSignature(_root);
		list->SetGraphicsRootConstantBufferView(0, base);
		list->SetGraphicsRootShaderResourceView(3, base + kModelsAt);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->SetPipelineState(pso);
		return true;
	}

	void char_renderer::draw_model(ID3D12GraphicsCommandList *list, const char_target &, const frame &f, uint32_t mi, char_stats &st, const frame *last, int pass,
		D3D12_GPU_DESCRIPTOR_HANDLE pass_texture)
	{
		const model_entry &m = f.model[mi];
		const D3D12_GPU_VIRTUAL_ADDRESS palettes = cache::palettes(f.seq.load(std::memory_order_relaxed));
		if (palettes == 0)
			return;
		if (pass == 0)
			st.models++;
		else
			list->SetGraphicsRootDescriptorTable(5, pass_texture);
		// (the same actor's same model in the last frame, its skeleton as now: its palette rows, still in the ring, give
		// each point's place then)
		const model_entry *was = nullptr;
		D3D12_GPU_VIRTUAL_ADDRESS last_palettes = 0;
		if (last != nullptr)
		{
			const uint32_t n = std::min(last->opaque.load(std::memory_order_acquire), kMaxModels);
			for (uint32_t i = 0; i < n && was == nullptr; ++i)
				if (last->model[i].actor == m.actor && last->model[i].model == m.model && last->model[i].instances == m.instances)
					was = &last->model[i];
			last_palettes = was != nullptr ? cache::palettes(last->seq.load(std::memory_order_relaxed)) : 0;
		}
		for (uint32_t ii = 0; ii < m.instances && m.first_instance + ii < kMaxInstances; ++ii)
		{
			const instance_entry &ie = f.instance[m.first_instance + ii];
			if (ie.bones == 0)
				continue;
			list->SetGraphicsRootShaderResourceView(2, palettes + UINT64(ie.first_bone) * sizeof(bone));
			uint32_t has_last = 0;
			D3D12_GPU_VIRTUAL_ADDRESS last_rows = palettes + UINT64(ie.first_bone) * sizeof(bone);
			if (was != nullptr && last_palettes != 0 && was->first_instance + ii < kMaxInstances)
			{
				const instance_entry &le = last->instance[was->first_instance + ii];
				if (le.bones == ie.bones && le.first_bone + le.bones <= kMaxBones)
				{
					last_rows = last_palettes + UINT64(le.first_bone) * sizeof(bone);
					has_last = 1;
				}
			}
			list->SetGraphicsRootShaderResourceView(6, last_rows);
			for (uint32_t k = 0; k < ie.meshes && ie.first_mesh + k < kMaxMeshes; ++k)
			{
				cache::mesh_view mv;
				if (!cache::mesh_view_of(f.mesh[ie.first_mesh + k].mesh, mv) || mv.vertices == 0)
				{
					st.not_ready++;
					continue;
				}
				list->SetGraphicsRootDescriptorTable(4, mv.views);
				// (a texture command whose texture wasn't found keeps the one bound before it in the mesh)
				D3D12_GPU_DESCRIPTOR_HANDLE bound = {};
				int32_t bound_texture = -2;
				for (const os2::group &gr : mv.groups)
				{
					if (gr.count == 0 || gr.first + gr.count > mv.corners)
						continue;
					const uint8_t kind = gr.kind();
					if (kind != 0 && (m.hidden & (1u << kind)) != 0)
					{
						st.hidden++;
						continue;
					}
					if (gr.texture != bound_texture)
					{
						D3D12_GPU_DESCRIPTOR_HANDLE h;
						const uint32_t tid = gr.texture >= 0 && size_t(gr.texture) < mv.textures.size() ? mv.textures[gr.texture] : cache::kNone;
						if (tid != cache::kNone && cache::texture_view_of(tid, h))
						{
							bound = h;
							if (pass == 0)
								list->SetGraphicsRootDescriptorTable(5, h);
						}
						bound_texture = gr.texture;
					}
					if (bound.ptr == 0)
					{
						st.untextured++;
						continue;
					}
					float scale;
					std::memcpy(&scale, &gr.material[4], 4);
					if (!gr.has_material || !(scale > 0.0f && scale < 16.0f))
						scale = 1.0f;
					float env;
					std::memcpy(&env, &gr.material[10], 4);
					const uint32_t shine = gr.has_material && env != 0.0f ? 1u : 0u;
					uint32_t dc[9] = { mi, 0, shine, mv.vertices, ie.bones, 0, gr.first, mv.corners, has_last };
					std::memcpy(&dc[1], &scale, 4);
					list->SetGraphicsRoot32BitConstants(1, 9, dc, 0);
					list->DrawInstanced(gr.count, 1, 0, 0);
					st.draws++;
					if (pass == 0)
						st.triangles += gr.count / 3;
				}
			}
		}
	}

	char_stats char_renderer::record_opaque(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, const char_options &o)
	{
		char_stats st;
		const uint32_t opaque = std::min(f.opaque.load(std::memory_order_acquire), kMaxModels);
		bool any = false;
		for (uint32_t i = 0; i < opaque && !any; ++i)
			any = (f.model[i].flags & kNative) != 0 && (f.model[i].colour >> 24) != 0;
		if (!any || !begin(list, t, f, o))
			return st;
		// (the last frame's record, for each model's motion: only with the motion target, and only the walk just before)
		const frame *last = nullptr;
		if (t.motion_rtv.ptr != 0)
		{
			const uint32_t seq = f.seq.load(std::memory_order_relaxed);
			last = published((seq - 1) & 15u);
			if (last != nullptr && last->seq.load(std::memory_order_acquire) != seq - 1)
				last = nullptr;
		}
		for (uint32_t i = 0; i < opaque; ++i)
			if ((f.model[i].flags & kNative) != 0 && (f.model[i].colour >> 24) != 0)
				draw_model(list, t, f, i, st, last);
		draw_passes(list, t, f, 0, opaque, {}, st);
		return st;
	}

	char_stats char_renderer::record_model(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, uint32_t model, const char_options &o,
		D3D12_GPU_DESCRIPTOR_HANDLE scene)
	{
		char_stats st;
		if (model >= std::min(f.models, kMaxModels) || !begin(list, t, f, o))
			return st;
		draw_model(list, t, f, model, st);
		draw_passes(list, t, f, model, model + 1, scene, st);
		return st;
	}

	void char_renderer::draw_passes(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, uint32_t from, uint32_t to, D3D12_GPU_DESCRIPTOR_HANDLE scene,
		char_stats &st)
	{
		bool bound = false;
		for (int pass = 1; pass <= 2; ++pass)
		{
			ID3D12PipelineState *pso = nullptr;
			for (uint32_t i = from; i < to && i < kMaxModels; ++i)
			{
				const model_entry &m = f.model[i];
				if ((m.flags & kNative) == 0 || (m.colour >> 24) == 0)
					continue;
				D3D12_GPU_DESCRIPTOR_HANDLE tex = {};
				if (pass == 1)
				{
					if ((m.passes & kPassOverlay) == 0 || !cache::texture_view_of(m.overlay_texture, tex))
						continue;
				}
				else
				{
					if ((m.passes & kPassCloak) == 0 || scene.ptr == 0 || !f.projection_read)
						continue;
					tex = scene;
				}
				if (pso == nullptr)
				{
					pso = pipeline(t, pass == 1 ? kOverlay : kCloak);
					if (pso == nullptr)
						break;
					// (the world's target alone: the motion and the light split stay as the meshes left them)
					if (!bound)
						list->OMSetRenderTargets(1, &t.rtv, FALSE, &t.dsv);
					bound = true;
					list->SetPipelineState(pso);
				}
				draw_model(list, t, f, i, st, nullptr, pass, tex);
				if (pass == 1)
					st.overlays++;
				else
					st.cloaks++;
			}
		}
	}

	char_stats char_renderer::record_casters(ID3D12GraphicsCommandList *list, const frame &f, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, uint32_t size)
	{
		char_stats st;
		ID3D12DescriptorHeap *const heap = cache::heap();
		const uint32_t models = std::min(f.models, kMaxModels);
		if (_root == nullptr || _shadow_pso == nullptr || heap == nullptr || models == 0)
			return st;
		bool any = false;
		for (uint32_t i = 0; i < models && !any; ++i)
			any = (f.model[i].flags & kNative) != 0 && (f.model[i].colour >> 24) != 0;
		if (!any)
			return st;
		const D3D12_GPU_VIRTUAL_ADDRESS base = write_frame(f, char_options(), world_to_clip, 1);
		list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		const D3D12_VIEWPORT vp = { 0.0f, 0.0f, float(size), float(size), 0.0f, 1.0f };
		const D3D12_RECT sc = { 0, 0, LONG(size), LONG(size) };
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sc);
		list->SetDescriptorHeaps(1, &heap);
		list->SetGraphicsRootSignature(_root);
		list->SetGraphicsRootConstantBufferView(0, base);
		list->SetGraphicsRootShaderResourceView(3, base + kModelsAt);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->SetPipelineState(_shadow_pso);
		const char_target none;
		for (uint32_t i = 0; i < models; ++i)
			if ((f.model[i].flags & kNative) != 0 && (f.model[i].colour >> 24) != 0)
				draw_model(list, none, f, i, st);
		return st;
	}

	void char_renderer::record_composite(ID3D12GraphicsCommandList *list, const char_target &world, D3D12_GPU_DESCRIPTOR_HANDLE srv, uint32_t colour)
	{
		ID3D12DescriptorHeap *const heap = cache::heap();
		ID3D12PipelineState *const pso = pipeline(world, kComposite);
		if (_root == nullptr || heap == nullptr || pso == nullptr)
			return;
		list->OMSetRenderTargets(1, &world.rtv, FALSE, nullptr);
		const D3D12_VIEWPORT vp = { 0.0f, 0.0f, world.width, world.height, 0.0f, 1.0f };
		const D3D12_RECT sc = { 0, 0, LONG(world.width), LONG(world.height) };
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sc);
		list->SetDescriptorHeaps(1, &heap);
		list->SetGraphicsRootSignature(_root);
		list->SetPipelineState(pso);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		const uint32_t dc[9] = { 0, 0, 0, 0, 0, colour, 0, 0, 0 };
		list->SetGraphicsRoot32BitConstants(1, 9, dc, 0);
		list->SetGraphicsRootDescriptorTable(5, srv);
		list->DrawInstanced(3, 1, 0, 0);
	}
	char_stats char_renderer::record_enspells(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, double seconds)
	{
		char_stats st;
		const uint32_t opaque = std::min(f.opaque.load(std::memory_order_acquire), kMaxModels);
		bool any = false;
		for (uint32_t i = 0; i < opaque && !any; ++i)
			any = f.model[i].enspell != kEnNone && f.model[i].enspell <= kEnDark && (f.model[i].colour >> 24) != 0;
		ID3D12DescriptorHeap *const heap = cache::heap();
		const D3D12_GPU_VIRTUAL_ADDRESS palettes = cache::palettes(f.seq.load(std::memory_order_relaxed));
		if (!any || _en_root == nullptr || heap == nullptr || palettes == 0 || !f.camera)
			return st;
		ID3D12PipelineState *const shell = pipeline(t, kEnShell), *const sprites = pipeline(t, kEnSprites);
		if (shell == nullptr || sprites == nullptr)
			return st;
		list->OMSetRenderTargets(1, &t.rtv, FALSE, &t.dsv);
		const D3D12_VIEWPORT vp = { t.offset, t.offset, t.width, t.height, 0.0f, 1.0f };
		const D3D12_RECT sc = { 0, 0, LONG(t.width), LONG(t.height) };
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sc);
		list->SetDescriptorHeaps(1, &heap);
		list->SetGraphicsRootSignature(_en_root);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		// (the time wrapped, so the shader's floats keep fine steps)
		const float time = float(std::fmod(seconds, 3600.0));
		for (uint32_t mi = 0; mi < opaque; ++mi)
		{
			const model_entry &m = f.model[mi];
			if (m.enspell == kEnNone || m.enspell > kEnDark || (m.colour >> 24) == 0)
				continue;
			st.models++;
			for (uint32_t ii = 0; ii < m.instances && m.first_instance + ii < kMaxInstances; ++ii)
			{
				const instance_entry &ie = f.instance[m.first_instance + ii];
				if (ie.bones == 0)
					continue;
				for (uint32_t k = 0; k < ie.meshes && ie.first_mesh + k < kMaxMeshes; ++k)
				{
					cache::mesh_view mv;
					if (!cache::mesh_view_of(f.mesh[ie.first_mesh + k].mesh, mv) || mv.vertices == 0 || !mv.blade.valid || mv.blade.bone >= ie.bones)
						continue;
					en_constants c = {};
					std::memcpy(c.view_proj, f.view_proj, sizeof(c.view_proj));
					c.time = time;
					c.element = m.enspell;
					c.bone_count = ie.bones;
					c.vertex_count = mv.vertices;
					c.corner_count = mv.corners;
					c.bone = mv.blade.bone;
					c.half_width = mv.blade.half_width;
					c.strength = std::min(1.0f, float(m.colour >> 24) / 128.0f);
					std::memcpy(c.base, mv.blade.base, sizeof(c.base));
					c.edge_axis = mv.blade.edge_axis;
					c.blade_kind = mv.blade.kind;
					std::memcpy(c.tip, mv.blade.tip, sizeof(c.tip));
					list->SetGraphicsRoot32BitConstants(0, 32, &c, 0);
					list->SetGraphicsRootShaderResourceView(1, palettes + UINT64(ie.first_bone) * sizeof(bone));
					list->SetGraphicsRootDescriptorTable(2, mv.views);
					list->SetPipelineState(shell);
					list->DrawInstanced(mv.corners, 1, 0, 0);
					list->SetPipelineState(sprites);
					list->DrawInstanced(kEnSpriteCount * 6, 1, 0, 0);
					st.draws += 2;
					st.triangles += mv.corners / 3 + kEnSpriteCount * 2;
				}
			}
		}
		return st;
	}
}
