// The grass and world shading shaders (Direct3D 12, SM 5.0), compiled by the add-on: built in from this
// file (tools/embed.py), or reloaded from worldfx.hlsl next to the add-on while the game runs.

cbuffer grass : register(b0)
{
	row_major float4x4 view_proj;
	float4 eye;    // xyz: the camera, w: draw distance
	float4 color;  // rgb: the grass's hue, w: seconds (wind)
	float4 blade;  // x: height, y: half the base, z: blades per tuft, w: first point of this draw (uint bits)
	uint4 ground;  // x: row pitch in uints, y, z: size, w: 0 none, 1 RGBA8, 2 BGRA8
	uint4 crowd;   // x: actors in t2 (low 16 bits), colour rules on (bits 16+: 1 only the ground, 2 outliers),
	               // 4 characters cast sun shadows, 8 the shadow map's parameters in t2, 16 lamps in t2 (after them),
	               // 32 the game's fog in t2 (after those); y, z, w: toward the sun, view space (float bits)
	uint4 zinfo;   // the depth copy: x: row pitch in uints, y, z: size, w: 0 none, 1 D24 (low 24 bits), 2 D32 float
	float4 lin;    // x: projection _33 / _34, y: _43 (distance along the view w = y / (d - x)); z, w: _11 and _22
	float4 up_row; // xyz: the view matrix's second row (a view-space point's world height = eye.y + dot(it, this)),
	               // w: projection _34 (view z = w * it: the game's is right-handed, -1)
	float4 shade;  // the shading pass: x: ambient occlusion strength, y: its radius (yalms), z: surface relief strength,
	               // w: sun shadow strength (0: none, the sun down or no sky)
};
StructuredBuffer<float4> blades : register(t0);
StructuredBuffer<float4> actors : register(t2); // characters and monsters near: x, y, z (their feet), radius
ByteAddressBuffer world : register(t1); // the world as drawn, copied before the blades
ByteAddressBuffer zbuf : register(t3);  // and its depth

static const uint kSegments = 4;

// The game's own fog, as its weather and the hour have it now (crowd.x rule bit 32; t2 after the lamps: [kHaze] its
// colour, [kHaze + 1] where it starts and ends, yalms of view depth, and 1 / (end - start)). Linear by view depth,
// as the game's fog render states have it: what's drawn at a depth shows clear_at(depth) of itself, the rest fog.
// Read only behind a [branch] on hazy(): without the fog, t2 may be a small buffer in the actors' place, and a root
// view has no bounds (a plain if is flattened, its reads made anyway).
static const uint kHaze = 73;
bool hazy() { return ((crowd.x >> 16) & 32) != 0; }
float3 haze_colour() { return actors[kHaze].rgb; }
float clear_at(float depth)
{
	float4 h = actors[kHaze + 1];
	return saturate((h.y - depth) * h.z);
}

float hash(float x) { return frac(sin(x * 91.3458) * 47453.5453); }

// A hash of a whole-number point (0..1), steady at any distance from the zone's middle (sin-based ones
// aren't, far out)
float hash2(int2 p)
{
	uint n = uint(p.x) * 1597334677u ^ uint(p.y) * 3812015801u;
	n = (n ^ (n >> 16)) * 2246822519u;
	n ^= n >> 13;
	return float(n & 0xFFFFFFu) / 16777215.0;
}

// Smooth noise (0..1) at p
float noise2(float2 p)
{
	int2 i = int2(floor(p));
	float2 f = p - floor(p);
	float2 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(hash2(i), hash2(i + int2(1, 0)), u.x), lerp(hash2(i + int2(0, 1)), hash2(i + int2(1, 1)), u.x), u.y);
}

// The same repeating every `period` (a whole number) across both axes: what drifts on it (the clouds, the fog banks)
// drifts without end, its offset wrapped by the period on the CPU
float noise_wrap(float2 p, float period)
{
	float2 i = floor(p), f = p - i;
	float2 u = f * f * (3.0 - 2.0 * f);
	float2 a = i - period * floor(i / period), b = a + 1.0;
	b -= period * floor(b / period);
	return lerp(lerp(hash2(int2(a)), hash2(int2(b.x, a.y)), u.x), lerp(hash2(int2(a.x, b.y)), hash2(int2(b)), u.x), u.y);
}

float3 texel(int2 p)
{
	p = clamp(p, int2(0, 0), int2(ground.yz) - 1);
	uint v = world.Load((p.y * ground.x + p.x) * 4);
	float3 c = float3(v & 255, (v >> 8) & 255, (v >> 16) & 255) / 255.0;
	return ground.w == 2 ? c.bgr : c;
}

// A pixel of a name facelift drew into the world (its core/namedepth marks the solid ones with alpha 1:
// the game's own textures can't make that value). Not part of the scene: left as drawn when shading.
bool name_at(int2 p)
{
	p = clamp(p, int2(0, 0), int2(ground.yz) - 1);
	return (world.Load((p.y * ground.x + p.x) * 4) >> 24) == 1u; // (alpha is the top byte, RGBA or BGRA)
}

// How far from the camera what's drawn at a pixel is (view depth), or a long way when depth isn't known
float drawn_depth(int2 p)
{
	if (zinfo.w == 0)
		return 1e9;
	if (any(zinfo.yz != ground.yz)) // (as a rule the same size: no divides then, at every step of a march)
		p = p * int2(zinfo.yz) / int2(ground.yz);
	p = clamp(p, int2(0, 0), int2(zinfo.yz) - 1);
	uint v = zbuf.Load((p.y * zinfo.x + p.x) * 4);
	float d = zinfo.w == 1 ? (v & 0xFFFFFF) / 16777215.0 : asfloat(v);
	return d >= 1.0 ? 1e9 : lin.y / (d - lin.x); // (the game's: 0.1 / (1 - d))
}

// The ground's colour under p on screen: the zone's own texture, lighting, fog and time of day, from
// the taps around it that show the ground (not something nearer the camera than p: a character standing
// there, a name, a branch). Returns how many did (0..8; 0 off screen too, colour then a fixed guess).
uint ground_at(float3 p, out float3 colour)
{
	colour = color.rgb * 0.5;
	float4 c = mul(float4(p, 1.0), view_proj);
	if (ground.w == 0 || c.w <= 0.0 || any(abs(c.xy) > c.w))
		return 0;
	int2 px = int2((c.x / c.w * 0.5 + 0.5) * ground.y, (0.5 - c.y / c.w * 0.5) * ground.z);
	// Eight taps in a ring; of those that see the ground, the middle ones by brightness (up to four) are
	// averaged, so a few on a shadow's edge or a bright speck don't sway it
	int r = max(3, ground.y / 400);
	static const int2 ring[8] = { int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2), int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1) };
	float3 tap[8];
	float lum[8];
	bool seen[8];
	uint n = 0;
	[unroll] for (int i = 0; i < 8; ++i)
	{
		int2 q = px + ring[i] * r;
		tap[i] = texel(q);
		lum[i] = dot(tap[i], float3(0.3, 0.59, 0.11));
		// (the ground: what's drawn there, put back in the world from its depth, lies at the root's height;
		// a tiger's flank, a trunk, a name or the background behind a crest doesn't)
		if (((crowd.x >> 16) & 1) != 0)
		{
			float z = drawn_depth(q);
			float2 ndc = float2((q.x + 0.5) / ground.y * 2.0 - 1.0, 1.0 - (q.y + 0.5) / ground.z * 2.0);
			float3 v = float3(ndc.x * z / lin.z, ndc.y * z / lin.w, z * up_row.w);
			seen[i] = z < 1e8 && abs(eye.y + dot(v, up_row.xyz) - p.y) < 0.4 + 0.01 * z;
		}
		else
			seen[i] = true;
		n += seen[i] ? 1u : 0u;
	}
	if (n == 0)
		return 0;
	// Then those far brighter or darker than the median of the rest aren't the ground either: a name's glyphs
	// (facelift draws names into the world, about as far as the ground around the character), a hard shadow
	float median = 0.0;
	[unroll] for (int a = 0; a < 8; ++a)
	{
		// (the seen tap ranked in the middle: as many darker as fit below it; ties go to the earlier tap)
		uint below = 0;
		[unroll] for (int b = 0; b < 8; ++b)
			below += (seen[b] && (lum[b] < lum[a] || (lum[b] == lum[a] && b < a))) ? 1u : 0u;
		if (seen[a] && below == (n - 1) / 2)
			median = lum[a];
	}
	n = 0;
	[unroll] for (int o = 0; o < 8; ++o)
	{
		seen[o] = seen[o] && (((crowd.x >> 16) & 2) == 0 || (lum[o] <= median * 1.6 + 0.04 && lum[o] >= median * 0.5 - 0.02));
		n += seen[o] ? 1u : 0u;
	}
	if (n == 0)
		return 0;
	uint take = min(n, 4u), lo = (n - take) / 2;
	float3 sum = 0.0;
	[unroll] for (int k = 0; k < 8; ++k)
	{
		// (its rank among the seen taps: how many are darker; ties go to the earlier tap)
		uint darker = 0;
		[unroll] for (int j = 0; j < 8; ++j)
			darker += (seen[j] && (lum[j] < lum[k] || (lum[j] == lum[k] && j < k))) ? 1u : 0u;
		if (seen[k] && darker >= lo && darker < lo + take)
			sum += tap[k];
	}
	colour = sum / take;
	return n;
}

// Each baked point's ground colour, remembered (12 bytes: f16 r, g | f16 b, f16 how far it was seen from,
// 0 while not known yet | how much darker the ground under it was in the last sample, unorm16): a frame's
// sample moves the colour only kFollow of the way, so what changes from frame to frame under a root as the
// camera moves (texture detail, shadows, things passing over) averages out, while day, night and weather
// still come through. Points whose ground isn't on screen keep theirs.
// The darkness isn't learned: it's the last sample's alone, and the blades are darkened by it at once. A
// shadow passing over (someone's own, which the game draws on the ground under the grass) shows on the
// grass and leaves with them, where the colour leaves it out on purpose (a moving shadow would stamp dark
// patches into it). Only darker counts: a flash doesn't light the grass.
// Then (kMemoryStride bytes a point in all) the point's wear: see wear_of.
RWByteAddressBuffer memory : register(u0);
static const uint kMemoryStride = 20;
static const float kFollow = 0.03;

// seen_from: how far (view depth) the colour returned was seen from, the fog of that distance in it (0: a guess)
float3 ground_colour(uint point_index, float3 p, bool first_vertex, out float seen_from)
{
	uint3 m = memory.Load3(point_index * kMemoryStride);
	bool known = (m.y >> 16) != 0;
	float3 kept = float3(f16tof32(m.x), f16tof32(m.x >> 16), f16tof32(m.y));
	float kept_from = f16tof32(m.y >> 16);
	float shaded = 1.0 - (m.z & 0xFFFF) / 65535.0;
	seen_from = kept_from;
	if (known && !first_vertex)
		return kept * shaded;
	float3 now;
	uint seen = ground_at(p, now);
	// Known: only a good view (half the taps on the ground) updates it. Not yet: any view of the ground at
	// all is remembered (better than guessing again every frame); none, a fixed guess, not remembered.
	// (without a good view, no darkness either: a body standing on the point doesn't darken its grass)
	if (known ? seen < 4 : seen == 0)
	{
		if (known && first_vertex)
			memory.Store(point_index * kMemoryStride + 8, 0u);
		seen_from = known ? kept_from : 0.0;
		return known ? kept * shaded : now;
	}
	// A sample much brighter or darker than what's remembered (a shadow passing over, a flash) moves it only a
	// little: a stray frame, or someone's shadow for a while, can't stamp a patch. Gradual light (dusk,
	// dawn, weather) comes through as usual.
	float lk = dot(kept, float3(0.3, 0.59, 0.11)), ln = dot(now, float3(0.3, 0.59, 0.11));
	// (darker than remembered by more than the ground's own flicker under a moving camera: in proportion,
	// down to 0.3)
	float ratio = ln / max(lk, 0.01);
	float darkness = known && ratio < 0.92 ? 1.0 - max(ratio / 0.92, 0.3) : 0.0;
	bool sudden = ln > lk * 1.3 + 0.02 || ln < lk * 0.7 - 0.02;
	float follow = (known && sudden) ? kFollow * 0.25 : kFollow; // (not slower: a wrong colour has to recover too)
	// A clearly closer view than the one it learned from takes over within a few frames: from afar the
	// ground is lightened by the fog, and blurred over more of it, so ground first seen from a distance
	// stayed light as you came up to it
	float from = max(mul(float4(p, 1.0), view_proj).w, 0.1);
	if (known && from < kept_from * 0.8)
		follow = sudden ? 0.2 : 0.3;
	// (a first view through only a few taps is a start: the next good views correct it)
	float3 next = known ? lerp(kept, now, follow) : now;
	float next_from = known ? lerp(kept_from, from, follow) : from;
	// (one write a point a frame; every vertex draws with what was remembered, so a tuft is one colour and
	// a frame's update shows from the next)
	if (first_vertex)
		memory.Store3(point_index * kMemoryStride, uint3(f32tof16(next.r) | (f32tof16(next.g) << 16), f32tof16(next.b) | (f32tof16(next_from) << 16),
			uint(darkness * 65535.0 + 0.5)));
	seen_from = known ? kept_from : from;
	return known ? kept * shaded : next;
}

// The grass's wear (crowd.x rule bit 256; t2 after the sky: [kWear] the memory's clock now (seconds since it was
// zeroed, from 1), the frame's length, how much the wear shows (0 none, up to 2), who's fighting (bits: actor i);
// [kWear + 1] x: how many cuts; then two float4 a cut: where its swath starts and its half-width, where it ends and
// when it was cut). Read only behind a [branch] on wearing(), as hazy().
static const uint kWear = kHaze + 2 + 6 + 24; // (after the wet look's and the sky's: kWet, kSky, below)
static const uint kMaxCuts = 8;
bool wearing() { return ((crowd.x >> 16) & 256) != 0; }

// The weather of its own (crowd.x rule bit 512; t2 after the shadow map's drawings before the last, kSunMapPrev below:
// grass_render.cpp kWxParams, look::weather_fx): [kWx] x, y: toward where the wind blows (the world's x, z) times how
// hard, z: how hard (0 calm .. 1 wind .. 2 gales); [kWx + 1] the view's first row (a view-space point's world x is eye.x
// + dot(it, the point)) and w: the ground's height under the camera (the game's y, down positive); [kWx + 2] its third
// row (world z) and w: the time (seconds, as color.w); [kWx + 3] the clouds: how much of the sky they cover, how opaque at
// most, the veil over all of it (mist), w: the stars' boost (1 none); [kWx + 4] how far the clouds have drifted (the
// noise's units: the broad shapes' x, z, the detail's x, z); [kWx + 5] their colour where lit, w: how much darker the sky;
// [kWx + 6] underneath, w: the aurora; [kWx + 7] the veil's colour; [kWx + 8] the lightning: x the flash now, y the
// bolt's brightness, z its bearing (radians from north toward east), w how high it starts (radians); [kWx + 9] x its
// shape (a number); [kWx + 10] the dust's colour, w how thick; [kWx + 11] x its streaks, y how fast they blow (yalms a
// second); [kWx + 12] gloom's colour, w how thick; [kWx + 13] heat shimmer: x how strong, y, z from and whole by (yalms);
// [kWx + 14] xy: how far the fog banks have drifted (the noise's units, 48 yalms each).
// Read only behind a [branch] on weather_fx(), as the fog.
static const uint kWx = kWear + 2 + 2 * kMaxCuts + 8;
bool weather_fx() { return ((crowd.x >> 16) & 512) != 0; }

// Each point's wear, after its colour in the memory (+12, +16: when its trampling will have worn off, and when it'll
// have grown back from a cut; seconds on the memory's clock, 0 never). Whoever stands within their radius of it (its
// own height within 3 yalms) pushes the first on by the frame's length, more the nearer: full (kWearHold ahead, the
// most it goes) after kWearFill seconds stood on, three times as fast and over half again the radius for one who's
// fighting; so a path walked again and again wears in, a step across it hardly. A cut whose swath covers the point
// sets the second kRegrow after the cut (again changes nothing). Only the tuft's first vertex writes, as the colour,
// and only what changed. Returns how trodden (0..1) and how freshly cut (1 just cut, 0 grown back).
static const float kWearHold = 240.0, kWearFill = 20.0, kRegrow = 240.0;
float2 wear_of(uint point_index, float3 p, bool first_vertex)
{
	uint at = point_index * kMemoryStride + 12;
	float2 until = asfloat(memory.Load2(at));
	float4 w = actors[kWear];
	float now = w.x;
	if (first_vertex)
	{
		float gain = 0.0;
		uint fighting = asuint(w.w);
		for (uint i = 0; i < (crowd.x & 0xFFFF); ++i)
		{
			float4 ac = actors[i];
			bool fights = ((fighting >> i) & 1u) != 0;
			float r = ac.w * (fights ? 1.5 : 1.0);
			float dist = length(p.xz - ac.xz);
			if (dist < r && abs(p.y - ac.y) < 3.0)
				gain += (1.0 - dist / r) * (fights ? 3.0 : 1.0);
		}
		float2 next = until;
		if (gain > 0.0)
			next.x = min(max(until.x, now) + w.y * gain * (kWearHold / kWearFill), now + kWearHold);
		uint cuts = min(uint(actors[kWear + 1].x), kMaxCuts);
		for (uint c = 0; c < cuts; ++c)
		{
			float4 a = actors[kWear + 2 + 2 * c], e = actors[kWear + 3 + 2 * c];
			float2 ab = e.xz - a.xz;
			float s = saturate(dot(p.xz - a.xz, ab) / max(dot(ab, ab), 1e-6));
			if (length(p.xz - (a.xz + ab * s)) < a.w && abs(p.y - lerp(a.y, e.y, s)) < 3.0)
				next.y = max(next.y, e.w + kRegrow);
		}
		if (any(next != until))
			memory.Store2(at, asuint(next));
		until = next;
	}
	return float2(saturate((until.x - now) / kWearHold), saturate((until.y - now) / kRegrow));
}

struct v2p
{
	float4 pos : SV_Position;
	float3 col : COLOR;
};

v2p vs_main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	v2p o;
	o.col = 0.0;
	uint tuft = max(1, uint(blade.z));
	float4 b = blades[iid / tuft + asuint(blade.w)];
	uint k = iid % tuft;
	float seed = frac(b.w + k * 0.61803);
	float fade = saturate((eye.w - distance(b.xz, eye.xz)) / (0.25 * eye.w));
	if (fade <= 0.0)
	{
		o.pos = float4(0.0, 0.0, -1.0, 1.0); // in front of the near plane: clipped
		return o;
	}

	// Where in the blade: row 0 is the root, kSegments the tip; side -1 or 1 (0 at the tip)
	uint row, side;
	if (vid < (kSegments - 1) * 6)
	{
		uint q = vid / 6, c = vid % 6;
		row = q + ((c == 2 || c == 3 || c == 5) ? 1 : 0);
		side = (c == 1 || c == 4 || c == 5) ? 1 : 0;
	}
	else
	{
		uint c = vid - (kSegments - 1) * 6;
		row = c == 2 ? kSegments : kSegments - 1;
		side = c == 1 ? 1 : 0;
	}
	float t = row / float(kSegments);

	// The tuft: blades spread around the point, shorter towards its edge
	float a = seed * 6.2831853;
	float spread = sqrt(hash(seed * 3.1 + 0.7)) * 0.3;
	float3 root = b.xyz + float3(cos(a * 2.7) * spread, 0.0, sin(a * 2.7) * spread);
	root.y += 0.03; // a little into the ground (y is height, down is positive)

	float seen_from;
	float3 g = ground_colour(iid / tuft + asuint(blade.w), b.xyz + float3(0.0, 0.03, 0.0), k == 0 && vid == 0, seen_from);
	// The ground's colour has the game's fog of the distance it was seen from in it: taken out here, the blade lit from
	// what's left, and the fog at the blade's own distance put back over it below. (Lit straight from the fogged colour,
	// a far blade's own shading darkened and greened the fog too: a darker patch on the haze-washed ground around it.)
	// Seen from far into the fog, little of the ground is left in its colour: no more than a quarter is taken as the
	// ground, so what's left of it isn't blown up.
	[branch] if (hazy())
	{
		float clear = max(clear_at(seen_from), 0.25);
		g = max((g - (1.0 - clear) * haze_colour()) / clear, 0.0);
	}

	// Trodden down and cut (wear_of): a cut blade is a stub kStub of its height, growing back slowly at first; a trodden
	// one shorter, flattened along its lean and taking the ground's colour, so a worn-in patch reads as bare ground
	float worn = 0.0, cut = 0.0;
	[branch] if (wearing())
	{
		float2 wc = wear_of(iid / tuft + asuint(blade.w), b.xyz, k == 0 && vid == 0);
		worn = saturate(wc.x * actors[kWear].z);
		cut = wc.y;
	}
	static const float kStub = 0.15;
	float flat = 0.8 * worn;

	float h = blade.x * (0.55 + 0.7 * hash(seed * 13.17)) * (1.0 - 0.4 * spread / 0.3) * fade
		* lerp(kStub, 1.0, smoothstep(0.0, 1.0, 1.0 - cut)) * (1.0 - 0.4 * worn);

	// Pushed aside by whoever stands in it: away from each actor within its radius (and a few yalms of its
	// height), harder the nearer, and flattened as much; back up as soon as they move on
	float2 push = 0.0;
	for (uint i = 0; i < (crowd.x & 0xFFFF); ++i)
	{
		float4 ac = actors[i];
		float2 d = root.xz - ac.xz;
		float dist = length(d);
		if (dist < ac.w && abs(root.y - ac.y) < 3.0)
		{
			float s = 1.0 - dist / ac.w;
			push += (dist > 0.001 ? d / dist : float2(1.0, 0.0)) * s * s;
		}
	}
	float pushed = min(length(push), 1.0); // (also darkens the blade a little: bent over, its brighter tip faces up)
	if (pushed > 0.0)
		push *= pushed / length(push);
	float2 across = float2(cos(a), sin(a));
	float2 lean = float2(-across.y, across.x) * (hash(seed * 5.3) < 0.5 ? -1.0 : 1.0);
	float bend = 0.2 + 0.45 * hash(seed * 7.31);
	float2 wind = float2(0.8, 0.6) * (0.5 + 0.5 * sin(color.w * 1.6 + dot(root.xz, float2(0.23, 0.17))))
		+ 0.25 * sin(color.w * 3.7 + seed * 6.28);
	// The weather's wind (weather_fx: [kWx] toward where it blows times how hard): the blades lean the way it blows, in
	// gusts that roll across the grass along it, a tip bent over coming down as much as it goes across (about half a
	// radian in a wind, a radian in gales); they flutter the more the harder it blows
	float2 blown = 0.0;
	float bowed = 0.0;
	[branch] if (weather_fx())
	{
		float4 wv = actors[kWx];
		float2 way = wv.z > 0.0 ? wv.xy / wv.z : float2(1.0, 0.0);
		float gust = 0.65 + 0.35 * sin(color.w * 1.9 - dot(root.xz, way) * 0.3) + 0.12 * sin(color.w * 5.3 + seed * 6.28);
		float tilt = min(0.55 * wv.z * gust, 1.15);
		blown = way * sin(tilt);
		bowed = 1.0 - cos(tilt);
		wind *= 1.0 + 0.8 * wv.z;
	}

	float w = blade.y * (row == kSegments ? 0.0 : pow(1.0 - t, 0.8)) * (side ? 1.0 : -1.0);
	float3 p = root;
	float down = max(pushed, flat);
	p.xz += across * w + (lean * (bend + 1.2 * flat) + (wind * 0.15 + blown) * (1.0 - down)) * h * t * t + push * h * (0.5 * t + 0.6 * t * t);
	p.y -= h * t * (1.0 - 0.25 * bend * t) * (1.0 - 0.6 * down) * (1.0 - bowed * t);
	o.pos = mul(float4(p, 1.0), view_proj);

	// Colour: the ground's, turned towards the grass's hue at the ground's brightness; dark at the root
	float lum = dot(g, float3(0.3, 0.59, 0.11));
	float3 hue = color.rgb / max(dot(color.rgb, float3(0.3, 0.59, 0.11)), 0.01);
	float3 grassy = lum * hue * (0.85 + 0.3 * hash(seed * 2.9)) + float3(0.04, 0.03, 0.0) * hash(seed * 4.1) * lum * 4.0;
	float3 base = lerp(g, grassy, 0.55) * 0.6;
	float3 tip = lerp(g, grassy, 0.8) * 1.25;
	o.col = lerp(g, lerp(base, tip, t), fade) * (1.0 - 0.2 * down); // far blades sink into the ground's colour as they shrink
	// (worn grass takes the ground's colour; a fresh cut's end paler, yellower)
	o.col = lerp(o.col, g * 0.85, 0.55 * worn);
	o.col = lerp(o.col, o.col * float3(1.15, 1.1, 0.75) + 0.015, 0.6 * cut * t);
	// Into the game's fog as the ground around it is (o.pos.w: the view depth)
	[branch] if (hazy())
		o.col = lerp(haze_colour(), o.col, clear_at(o.pos.w));
	return o;
}

// The shading pass, before the grass: ambient occlusion from the depth, and surface relief from the
// texture's own light and dark detail, on the world only (the interface comes later)
float3 view_at(int2 q)
{
	float z = drawn_depth(q);
	float2 ndc = float2((q.x + 0.5) / ground.y * 2.0 - 1.0, 1.0 - (q.y + 0.5) / ground.z * 2.0);
	return float3(ndc.x * z / lin.z, ndc.y * z / lin.w, z * up_row.w);
}

float luma(float3 c) { return dot(c, float3(0.3, 0.59, 0.11)); }

// The surface's facing at p (view space, toward the camera), from the depth around it: of each pair of
// neighbours, the nearer in depth (across an edge, the other side isn't this surface)
float3 surface_normal(int2 p, float3 P)
{
	float3 r = view_at(p + int2(1, 0)), l = view_at(p - int2(1, 0)), d = view_at(p + int2(0, 1)), u = view_at(p - int2(0, 1));
	float3 dx = abs(r.z - P.z) < abs(P.z - l.z) ? r - P : P - l;
	float3 dy = abs(d.z - P.z) < abs(P.z - u.z) ? d - P : P - u;
	float3 n = normalize(cross(dx, dy));
	return dot(n, -P) < 0.0 ? -n : n;
}

// How many yalms a pixel of the world spans at a view depth
float pixel_width(float depth) { return depth * 2.0 / (lin.z * ground.y); }

// The same, and how steeply the surface is seen: how much further the nearer neighbour is, in pixel widths at that
// depth, the larger of across and down (1: face on; above 4, within 14 degrees of edge on, or a sliver between
// other surfaces, where a normal from the depth can point anywhere)
float3 surface_normal(int2 p, float3 P, out float steep)
{
	float3 r = view_at(p + int2(1, 0)), l = view_at(p - int2(1, 0)), d = view_at(p + int2(0, 1)), u = view_at(p - int2(0, 1));
	float zx = min(abs(r.z - P.z), abs(P.z - l.z)), zy = min(abs(d.z - P.z), abs(P.z - u.z));
	float3 dx = abs(r.z - P.z) < abs(P.z - l.z) ? r - P : P - l;
	float3 dy = abs(d.z - P.z) < abs(P.z - u.z) ? d - P : P - u;
	steep = max(zx, zy) / pixel_width(abs(P.z));
	float3 n = normalize(cross(dx, dy));
	return dot(n, -P) < 0.0 ? -n : n;
}

// How much of a surface's light is the sun's, by how squarely it faces it (facing: the cosine): the game lights
// with an ambient part and the sun's by that cosine, about 0.49 and 1.25 by day. A shadow takes away only the sun's
// share, scaled so a surface facing the sun square loses the slider's whole strength: none at all on a surface
// edge on to the sun or turned away (the game's light already leaves those to the ambient), a little more at each
// step toward square. (It was all of it from a quarter of square and nothing below 0.05: in a hill's long shadow at
// a low sun, the facets a little toward the sun came out darker than those turned away, in slabs along the
// facets' edges that moved with the camera.)
float sun_share(float facing) { return facing > 0.0 ? 1.39 * facing / (0.39 + facing) : 0.0; }

float3 sun_in_view() { return float3(asfloat(crowd.y), asfloat(crowd.z), asfloat(crowd.w)); }

// The sun's shadows are worked out at half the world's size (sun_ps), into u1: a uint for each, its row
// half the world's width (rounded up). (u1: a pixel shader's UAVs share slots with its outputs, and the
// shading pass has one)
RWByteAddressBuffer sun_mask : register(u1);
int2 sun_mask_size() { return int2((ground.yz + 1u) >> 1); }

float4 shade_vs(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// Where a view-space point is on screen (pixels of the world)
float2 screen_of(float3 v)
{
	float2 ndc = v.xy * float2(lin.z, lin.w) / max(v.z * up_row.w, 0.2);
	return float2((ndc.x * 0.5 + 0.5) * ground.y, (0.5 - ndc.y * 0.5) * ground.z);
}

// Lamps lit near the camera (t2 after the shadow map's parameters, when crowd.x rule bit 16 says they're there;
// the add-on picks the nearest lit ones in view and grass_render.cpp puts them in view space): [40] how many, and
// how much of a lamp's glow a surface it can't see loses; then two float4s a lamp: where its light is and how far
// its glow reaches (yalms), its colour times how strong its glow is (rgb). The rest lie far off with no glow: all
// kMaxLamps (grass_render.hpp's) are read, a fixed run.
static const uint kLamps = 40;
static const uint kMaxLamps = 16;

// A character or monster near (t2, as many as crowd.x's low bits) as the lamps' march sees it: the standing capsule
// body_in_view (after the shading pass, below) makes of it, its feet in view space, made 1.5 yalms taller and 0.3 wider
// to take in the name and icon facelift draws over it (in the depth too)
void lamp_body(uint i, out float3 feet, out float height, out float radius)
{
	float4 a = actors[i];
	float4 c = mul(float4(a.xyz, 1.0), view_proj);
	feet = float3(c.x / lin.z, c.y / lin.w, c.w / up_row.w);
	height = 2.3 * a.w + 1.5;
	radius = 0.6 * a.w + 0.45;
}

bool in_lamp_body(float3 x, float3 feet, float height, float radius, float3 up)
{
	float3 d = x - feet;
	float3 r = d - up * clamp(dot(d, up), 0.0, height);
	return dot(r, r) < radius * radius;
}

// The march toward a lamp (lamp_blocked): kLampSteps steps along the path from a tenth of the way to `last` of it;
// where a step is just behind what's drawn there (within 0.3 yalms, 1% of the distance and a step's length), something
// stands between. A hit fades in over its first 5 cm and out toward the back of that window, so no one step turns it
// on or off outright. What's off screen can't hide the lamp, nor can a name's pixel (name_at), nor a step inside
// either of the two capsules given (radius 0: none), nor what's drawn within kLampBody of the light (L): the lamp's
// own post, hook, cage or bowl (they shadowed a ring of the wall behind a lantern, just outside kLampNear where its
// shadows start: in game, 2026-10-21). The steps are a fixed run, each read on its own, as the sun's.
static const int kLampSteps = 12;
static const float kLampBody = 1.0;
float lamp_march(float3 from, float3 path, float last, float3 L, float3 f0, float h0, float r0, float3 f1, float h1, float r1)
{
	float3 up = -up_row.xyz; // (the world's up in view space: its y is down)
	float step_length = length(path) * (last - 0.1) / kLampSteps;
	float blocked = 0.0;
	[unroll] for (int k = 0; k < kLampSteps; ++k)
	{
		float3 x = from + path * (0.1 + (last - 0.1) * (k + 0.5) / kLampSteps);
		float depth = x.z * up_row.w;
		float2 s = screen_of(x);
		int2 q = int2(s);
		bool on = depth > 0.2 && all(s >= 0.0) && all(s < float2(ground.yz));
		float behind = depth - drawn_depth(q);
		float thickness = 0.3 + 0.01 * depth + step_length;
		float hit = saturate(behind / 0.05) * saturate((thickness - behind) / (0.25 * thickness));
		float3 drawn = view_at(clamp(q, int2(0, 0), int2(ground.yz) - 1));
		bool lamps_own = dot(drawn - L, drawn - L) < kLampBody * kLampBody;
		bool theirs = name_at(q) || lamps_own || in_lamp_body(x, f0, h0, r0, up) || in_lamp_body(x, f1, h1, r1, up);
		blocked = max(blocked, on && !theirs ? hit : 0.0);
	}
	return blocked;
}

// How much a lamp at L (view space) is hidden from the surface at P (n its facing). Only the world hides a lamp here:
// - Its own lantern, bowl or bracket is around the light: the march stops kLampFixture short of it, and a surface
//   within kLampNear of the light isn't shaded by it at all, fading in over the next yalm. (Marching to 85% of the way,
//   the wall behind a lantern found the lantern itself: a dark disc in a ring of glow, in game on 2026-10-18.)
// - Characters and monsters don't hide one: the game casts a character's own shadow from a lamp near it, and from here
//   the steps each found the character at another distance and laid a copy of its shadow on the ground, a row of them
//   toward the lamp (in game the same evening; a name's icon the same). So a hit inside either of the two characters
//   whose capsules come nearest the path doesn't count, nor one on a name facelift drew into the world. The march runs
//   once as it is; only where it hit something is it run again with those left out (the characters are looked at only
//   there: most pixels have no hit).
static const float kLampFixture = 0.75, kLampNear = 1.25;
float lamp_blocked(float3 P, float3 n, float3 L)
{
	float3 from = P + n * (2.0 * pixel_width(abs(P.z)));
	float3 path = L - from;
	float len = length(path);
	float last = (len - kLampFixture) / max(len, 1e-3);
	float away = smoothstep(kLampNear, kLampNear + 1.0, len);
	float blocked = 0.0;
	[branch] if (last > 0.15 && away > 0.0)
	{
		blocked = lamp_march(from, path, last, L, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
		uint count = min(crowd.x & 0xFFFF, 32u);
		[branch] if (blocked > 0.0 && count > 0)
		{
			float3 up = -up_row.xyz;
			float3 f0 = 0.0, f1 = 0.0;
			float h0 = 0.0, h1 = 0.0, r0 = 0.0, r1 = 0.0, best0 = 1e9, best1 = 1e9;
			[unroll] for (uint i = 0; i < 32; ++i)
			{
				float3 feet;
				float height, radius;
				lamp_body(i, feet, height, radius);
				float3 centre = feet + up * (0.5 * height);
				float k = saturate(dot(centre - from, path) / max(len * len, 1e-6));
				float apart = length(from + path * k - centre) - (0.5 * height + radius);
				bool first = i < count && apart < best0;
				bool second = i < count && !first && apart < best1;
				f1 = first ? f0 : second ? feet : f1;
				h1 = first ? h0 : second ? height : h1;
				r1 = first ? r0 : second ? radius : r1;
				best1 = first ? best0 : second ? apart : best1;
				f0 = first ? feet : f0;
				h0 = first ? height : h0;
				r0 = first ? radius : r0;
				best0 = first ? apart : best0;
			}
			blocked = lamp_march(from, path, last, L, f0, h0, r0, f1, h1, r1);
		}
	}
	return blocked * away;
}

// The lamps' light on the surface at P (view space; n its facing, steep as surface_normal gives it, col what the
// game drew there), to add to it. The game lights only the meshes that name a lamp, fully within its range and not
// at all past it; this lights every surface near a lamp, falling off smoothly to nothing at the edge of its glow's
// reach ((1 - d^2 / R^2)^2), a little more on what faces it (wrapped: a surface turned away still gets 30%, one whose
// facing the depth can't tell 65%). The light adds in proportion to the surface's colour (as light on it would),
// with a little on top so a near-black surface still warms, and less on what's bright already (lit by the game's own
// lamp light, or a light texture). With lamp shadows on, the strongest lamp at the pixel is marched toward
// (lamp_blocked) and its light taken away as far as it's hidden; the others are left as they are. Only the zone's
// own lights cast them (c.w 1): a Home Point's or an effect's glow (c.w 0) has its light inside a crystal or a
// spell's own shape, which the march's fixed steps found again and again and laid down as a row of shadows that slid
// with the camera (in game, 2026-10-20), so those light without a shadow.
float3 lamp_light(float3 P, float3 n, float steep, float3 col)
{
	float4 head = actors[kLamps];
	uint count = min(uint(head.x), kMaxLamps);
	bool sure = steep < 4.0;
	float3 sum = 0.0, strongest = 0.0, strongest_at = 0.0;
	float most = 0.0;
	[unroll] for (uint i = 0; i < kMaxLamps; ++i)
	{
		float4 a = actors[kLamps + 1 + 2 * i], c = actors[kLamps + 2 + 2 * i];
		float3 v = a.xyz - P;
		float d2 = dot(v, v);
		float k = saturate(1.0 - d2 / (a.w * a.w));
		float facing = sure ? 0.3 + 0.7 * saturate(dot(n, v) * rsqrt(d2 + 1e-4)) : 0.65;
		float3 add = (i < count ? k * k * facing : 0.0) * c.rgb;
		sum += add;
		float amount = c.w > 0.5 ? luma(add) : 0.0;
		strongest = amount > most ? add : strongest;
		strongest_at = amount > most ? a.xyz : strongest_at;
		most = max(most, amount);
	}
	[branch] if (head.y > 0.0 && most > 0.002)
		sum -= strongest * head.y * lamp_blocked(P, n, strongest_at);
	return sum * (2.5 * col + 0.03) * saturate(1.2 - luma(col));
}

// Wet ground in rain (crowd.x rule bit 64; t2 after the fog: [kWet] the view's first row (a view-space point's world x
// is eye.x + dot(it, the point)) and how wet, [kWet + 1] its third row (world z), [kWet + 2] the wet mask's corner (x, z)
// and its squares a yalm, [kWet + 3] its width and height (squares) and words a row, [kWet + 4] the game's light's
// colour, [kWet + 5] the sheen's (the overcast sky's)). The mask is t0 in the shading pass (tools/bake_wet.py: a bit a
// square, set where the topmost surface facing up is path or stone). Read only behind a [branch] on wet(), as the fog.
ByteAddressBuffer wet_mask : register(t0);
static const uint kWet = kHaze + 2;
bool wet() { return ((crowd.x >> 16) & 64) != 0; }

// (a square off the mask isn't wet; the read is clamped to the mask all the same: a plain if around it is flattened,
// and a root view has no bounds)
float wet_bit(int2 s)
{
	float4 size = actors[kWet + 3];
	int2 last = int2(size.xy) - 1;
	bool inside = all(s >= 0) && all(s <= last);
	int2 c = clamp(s, int2(0, 0), last);
	uint w = wet_mask.Load((uint(c.y) * uint(size.z) + (uint(c.x) >> 5)) * 4);
	return inside ? float((w >> (uint(c.x) & 31u)) & 1u) : 0.0;
}

// How much the ground at P (view space) is path or stone: the mask's four squares around it, blended (soft edges)
float wet_here(float3 P)
{
	float4 a = actors[kWet], b = actors[kWet + 1], corner = actors[kWet + 2];
	float2 xz = float2(eye.x + dot(P, a.xyz), eye.z + dot(P, b.xyz));
	float2 c = (xz - corner.xy) * corner.z - 0.5;
	int2 i = int2(floor(c));
	float2 f = c - floor(c);
	return lerp(lerp(wet_bit(i), wet_bit(i + int2(1, 0)), f.x), lerp(wet_bit(i + int2(0, 1)), wet_bit(i + int2(1, 1)), f.x), f.y);
}

// A sky of its own (crowd.x rule bit 128; t2 after the wet look's: grass_render.cpp kSkyParams), on the pixels the zone
// left: the game's dome doesn't write depth (its state block turns Z writes off: client-notes "Sky"), so they keep the
// cleared depth, and nothing the zone draws is this far. [kSky + i] the record's stop i: its colour (bytes) and height
// (0 the horizon .. 1 overhead); [kSky + 8] the clear colour (bytes) and the dome's segments around; [kSky + 9], [kSky +
// 10].xyz how many strips each band between two stops is drawn in; [kSky + 11] the view's first row (a view-space
// direction's world x) and how bright the stars, [kSky + 12] its third row (world z) and how strong the sun's (or moon's)
// disc and glow; [kSky + 13] toward the game's light (view space) and how much it's the moon; [kSky + 14] their colour
// and the stars' turn about the pole (radians); [kSky + 15] x the time (seconds), y 1 for the debug view (what's repainted
// tinted magenta, sky kept as the game drew it red), z and w how near a pixel must be to the game's dome to be repainted
// (bytes: whole within z, none past w); [kSky + 16 + i] stop i's slope (bytes per height), the first three's .w the
// moon's lit side (view space), [kSky + 19].w how much of it is lit (the game's phase: 0 new .. 1 full).
// Read only behind a [branch] on own_sky(), as the fog.
static const uint kSky = kWet + 6;
static const float kSkyDepth = 30000.0; // (yalms: the game's far plane is 65,535, its dome 52,400 out and depthless)
bool own_sky() { return ((crowd.x >> 16) & 128) != 0; }

// The colour (bytes) the game's dome gives along a world direction (x east, y down, z north), as it draws it (03D640,
// grass_render.cpp dome_colour has the same in C++): each band between two stops cut into strips, each strip a ring of
// flat facets between vertices at azimuth k * 6.283 / segments from north, its edges' colours whole bytes; Gouraud
// across the facet the direction meets (linear in its height there); the clear colour below the horizon
float3 dome_colour(float3 w)
{
	float up = -w.y, horiz = length(w.xz);
	float4 info = actors[kSky + 8];
	if (up <= 0.0)
		return info.rgb;
	float segments = info.w, seg = 6.283 / segments;
	float az = atan2(w.x, w.z);
	az = az < 0.0 ? az + 6.2831853 : az;
	float k = min(floor(az / seg), segments - 1.0);
	float a0 = k * seg, a1 = k >= segments - 1.0 ? 6.2831853 : (k + 1.0) * seg;
	float phi = az - 0.5 * (a0 + a1), half_seg = 0.5 * (a1 - a0);
	float2 ray = float2(horiz * cos(phi), up * cos(half_seg));
	float height = atan2(ray.y, ray.x) / 1.570796;
	float4 strips_a = actors[kSky + 9], strips_b = actors[kSky + 10];
	float strips[7] = { strips_a.x, strips_a.y, strips_a.z, strips_a.w, strips_b.x, strips_b.y, strips_b.z };
	float4 s0 = actors[kSky], s1 = actors[kSky + 1];
	float n = strips[0];
	[unroll] for (int i = 1; i < 7; ++i)
	{
		float4 a = actors[kSky + i], b = actors[kSky + i + 1];
		bool here = height >= a.w && b.w > a.w; // (the last band starting at or below the height; none of no height)
		s0 = here ? a : s0;
		s1 = here ? b : s1;
		n = here ? strips[i] : n;
	}
	float f = s1.w > s0.w ? saturate((height - s0.w) / (s1.w - s0.w)) : 0.0;
	float m = clamp(floor(f * n), 0.0, n - 1.0);
	float f1 = m / n, f2 = (m + 1.0) / n;
	float e_lo = (f1 * s1.w + (1.0 - f1) * s0.w) * 1.570796, e_hi = (f2 * s1.w + (1.0 - f2) * s0.w) * 1.570796;
	float2 lo = float2(cos(e_lo), sin(e_lo)), d = float2(cos(e_hi), sin(e_hi)) - lo;
	float den = ray.x * d.y - ray.y * d.x;
	float s = den != 0.0 ? saturate((ray.y * lo.x - ray.x * lo.y) / den) : 0.0;
	float3 c_lo = floor(s0.rgb * (1.0 - f1)) + floor(s1.rgb * f1), c_hi = floor(s0.rgb * (1.0 - f2)) + floor(s1.rgb * f2);
	return lerp(c_lo, c_hi, s);
}

// The add-on's own gradient at a height (0 the horizon .. 1 overhead; bytes): through each stop's colour at its height,
// a monotone cubic between them (the slopes from grass_render.cpp sky_slopes: no overshoot, no kink at the stops)
float3 sky_gradient(float height)
{
	float4 s0 = actors[kSky], s1 = actors[kSky + 1];
	float3 m0 = actors[kSky + 16].rgb, m1 = actors[kSky + 17].rgb;
	[unroll] for (int i = 1; i < 7; ++i)
	{
		float4 a = actors[kSky + i], b = actors[kSky + i + 1];
		bool here = height >= a.w && b.w > a.w;
		s0 = here ? a : s0;
		s1 = here ? b : s1;
		m0 = here ? actors[kSky + 16 + i].rgb : m0;
		m1 = here ? actors[kSky + 17 + i].rgb : m1;
	}
	float dh = s1.w - s0.w;
	float t = dh > 0.0 ? saturate((height - s0.w) / dh) : 0.0, t2 = t * t, t3 = t2 * t;
	return (2.0 * t3 - 3.0 * t2 + 1.0) * s0.rgb + (t3 - 2.0 * t2 + t) * dh * m0 + (3.0 * t2 - 2.0 * t3) * s1.rgb + (t3 - t2) * dh * m1;
}

// Stars on a turning sphere (d: a direction fixed to the stars): a cube's faces cut into cells, a star in about one cell
// in four (times many: more of them in stellar glare), somewhere in its middle, most of them faint; each a soft dot about
// a pixel wide (px: radians a pixel) that twinkles a little. Light added (0..1 each).
static const float kStarCells = 90.0;
uint star_hash(uint x) // (rain_hash's, defined further down)
{
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}
float3 star_field(float3 d, float px, float time, float many)
{
	float3 a = abs(d);
	float face;
	float2 uv;
	float3 axis_u, axis_v, axis_n;
	if (a.x >= a.y && a.x >= a.z)
	{
		face = d.x > 0.0 ? 0.0 : 1.0;
		axis_n = float3(sign(d.x), 0.0, 0.0);
		axis_u = float3(0.0, 1.0, 0.0);
		axis_v = float3(0.0, 0.0, 1.0);
		uv = d.yz / a.x;
	}
	else if (a.y >= a.z)
	{
		face = d.y > 0.0 ? 2.0 : 3.0;
		axis_n = float3(0.0, sign(d.y), 0.0);
		axis_u = float3(1.0, 0.0, 0.0);
		axis_v = float3(0.0, 0.0, 1.0);
		uv = d.xz / a.y;
	}
	else
	{
		face = d.z > 0.0 ? 4.0 : 5.0;
		axis_n = float3(0.0, 0.0, sign(d.z));
		axis_u = float3(1.0, 0.0, 0.0);
		axis_v = float3(0.0, 1.0, 0.0);
		uv = d.xy / a.z;
	}
	float2 g = (uv * 0.5 + 0.5) * kStarCells;
	float2 cell = clamp(floor(g), 0.0, kStarCells - 1.0);
	uint h = star_hash(uint(cell.x) + uint(cell.y) * 128u + uint(face) * 16384u + 7919u);
	uint h2 = star_hash(h ^ 0x9E3779B9u);
	float r0 = float(h & 0xFFFFu) / 65535.0, r1 = float(h >> 16) / 65535.0;
	float r2 = float(h2 & 0xFFFFu) / 65535.0, r3 = float(h2 >> 16) / 65535.0;
	float2 star_uv = ((cell + 0.2 + 0.6 * float2(r1, r2)) / kStarCells) * 2.0 - 1.0;
	float3 star = normalize(axis_n + axis_u * star_uv.x + axis_v * star_uv.y);
	float apart = length(cross(d, star)); // (radians, for so small an angle)
	float bright = r0 < 0.27 * many ? 0.05 + 0.95 * pow(r3, 7.0) : 0.0;
	float size = px * (0.6 + 0.7 * bright);
	float twinkle = 0.75 + 0.25 * sin(time * (1.7 + 3.1 * r1) + 6.2831853 * r2);
	float r4 = frac(r0 * 37.17 + r3 * 11.31);
	float3 tint = lerp(float3(0.78, 0.86, 1.0), float3(1.0, 0.9, 0.76), r4 * r4);
	return tint * bright * twinkle * exp(-0.5 * apart * apart / (size * size));
}

// Clouds of their own (crowd.x rule bit 1024, with the weather of its own and the sky): a slab of them some hundreds of
// yalms over the camera, on a shell round a world kCloudEarth yalms across (so a ray toward the horizon meets them a few
// thousand yalms off rather than never), seen through by cloud_ps at a quarter of the world's width and height into the
// sun mask's buffer (u1, after the mask: two pictures, this frame's and the last's, a uint2 a pixel: their light and how
// much of what's behind them shows, as f16s; 2 there: no sky to work out). A quarter of the picture's pixels are marched
// each frame (in a 2 x 2 order), the rest carried over from the last frame's picture turned with the camera (the clouds
// are far enough off that its turning is all that moves them on the screen). The shading pass reads the picture between
// its pixels where the sky shows (cloud_at). t2 after the weather's (grass_render.cpp kCloudParams):
// [kCloud] .. [kCloud + 2] the last frame's view rows (a world direction into its view space: its first, second and
// third), w: its projection's _11, _22, then this frame's turn in the 2 x 2 order (0..3); [kCloud + 3] x: 1 when the last
// frame's picture can be carried over (0: every pixel marched now), y, z: where this frame's and the last's pictures
// start in u1 (bytes, uint bits), w: the picture's width (uint bits); [kCloud + 4] x: its height (uint bits), y: steps
// along a view ray, z: steps toward the light, w: the frames so far (the steps' jitter turns with them); [kCloud + 5] x, y:
// the slab's bottom and top (yalms over the camera), z: its shape (0 heaps .. 1 a flat deck), w: how strongly the sun or
// moon lights them; [kCloud + 6] toward the light (the world's, y down), w: how much the fine noise eats their edges;
// [kCloud + 7] x: how dense they are (light taken out a yalm, in the thickest), y: their style (0 game-like: thin, soft,
// stretched layers in the sky's own colours, as the game's are; 1 volumetric heaps, lit by the sun); [kCloud + 8],
// [kCloud + 9]: their shadows' map (cloud_shadow_ps, below).
static const uint kCloud = kWx + 16;
bool own_clouds() { return ((crowd.x >> 16) & 1024) != 0; }

// The clouds' noise (t0 in cloud_ps, grass_render.cpp make_cloud_noise): the broad one, kNoiseBase texels a side (a
// Perlin-Worley mix for billowing masses), then the fine one, kNoiseDetail a side (Worley: what eats away their edges),
// both repeating. Each word is a texel and its neighbours along x, z and both, a byte each, so a blend of the eight
// texels round a point is two reads. A texel of the broad one is kBaseTexel yalms (across, up, across), of the fine one
// kDetailTexel.
ByteAddressBuffer cloud_noise : register(t0);
static const uint kNoiseBase = 64, kNoiseDetail = 32, kNoiseDetailAt = 64 * 64 * 64;
static const float3 kBaseTexel = float3(25.0, 15.0, 25.0);
static const float kDetailTexel = 5.0;
// The shell's world (yalms from its middle to the camera), and how far along a view ray the march goes at most
static const float kCloudEarth = 12000.0, kCloudReach = 2400.0;
// ... and how long a step along it is at most (yalms: further in than the steps reach, a low ray sees no more)
static const float kCloudStep = 35.0;

// One of the noises at t (in its texels, repeating every n: a power of two), between its texels
float cloud_tex(float3 t, uint n, uint at)
{
	float3 i = floor(t), f = t - i;
	uint3 a = uint3(int3(i) & int(n - 1));
	uint w0 = cloud_noise.Load((at + (a.y * n + a.z) * n + a.x) * 4);
	uint w1 = cloud_noise.Load((at + (((a.y + 1) & (n - 1)) * n + a.z) * n + a.x) * 4);
	float4 v = lerp(float4(w0 & 255, (w0 >> 8) & 255, (w0 >> 16) & 255, w0 >> 24), float4(w1 & 255, (w1 >> 8) & 255, (w1 >> 16) & 255, w1 >> 24), f.y);
	return lerp(lerp(v.x, v.y, f.x), lerp(v.z, v.w, f.x), f.z) / 255.0;
}

// How far along a view ray (up: its height's sine) the shell `over` yalms above the camera is (the root of the sphere's
// equation, written so nothing large is taken from something large)
float shell_at(float up, float over)
{
	float k = over * (2.0 * kCloudEarth + over);
	return k / (kCloudEarth * up + sqrt(kCloudEarth * kCloudEarth * up * up + k));
}

// How high over the camera a point d from it is (the world's axes, y down), on the shell's curve
float altitude(float3 d)
{
	float r = length(float3(d.x, d.y - kCloudEarth, d.z));
	return (dot(d, d) - 2.0 * kCloudEarth * d.y) / (r + kCloudEarth);
}

// Where the clouds gather: a broad noise over the ground (800 yalms a cell), drifting with the broad shapes and
// repeating where their drift does (the drift wraps at 64 of the broad noise's units, 16 texels each)
float cloud_patches(float2 xz, float4 drift)
{
	float2 a = (xz - drift.xy * (16.0 * kBaseTexel.x)) / 800.0;
	return 0.65 * noise_wrap(a, 32.0) + 0.35 * noise_wrap(a * 2.0 + 17.0, 64.0);
}

// How much cloud the broad noise makes at t (its texels), h of the way up the slab (0 its bottom .. 1 its top), where
// the clouds cover c of the sky: the noise shaped by the height (heaps: a flat base, thinning toward rounded tops; a
// deck: nearly square), then cut by the cover, so only its strongest parts are cloud when there's little (small heaps
// apart), and nearly all of it when the sky is covered
float cloud_shape(float3 t, float h, float c, float shape)
{
	float profile = saturate(h / lerp(0.06, 0.12, shape)) * saturate((1.0 - h) / lerp(0.8, 0.3, shape));
	float x = cloud_tex(t, kNoiseBase, 0) * profile;
	return saturate((x - (1.0 - c)) / max(0.7 * c, 0.25));
}

// The same eaten away by the fine noise: wisps low down, billows (its cells turned inside out: rounded bulges) higher up;
// the thinnest of it gone, the thick hardly touched; inside, denser in the billows' middles than between them, so they
// shade each other (cauliflower)
float cloud_erode(float d, float detail, float h, float erosion)
{
	float billow = lerp(detail, 1.0 - detail, saturate(h * 3.0));
	float e = billow * erosion * 0.85;
	return saturate((d - e) / max(1.0 - e, 1e-3)) * lerp(1.0, 0.4 + 1.2 * (1.0 - billow), 0.6 * erosion);
}

// The slab as this frame has it ([kCloud + 5], [kCloud + 6], the weather's): its bottom and depth (yalms over the
// camera), its shape (0 heaps .. 1 a deck) and how much of a rain deck it is (deck: 0 none .. 1 a solid deck, with rolls
// and a ragged base), the cover and how far it varies, the broad noise's drift (its texels) and the same in yalms, and
// across which way the deck's rolls lie (the wind's way: they run across it). In the game-like style ([kCloud + 7].y 0)
// half as deep, a third as dense (a rain deck as it is) and softer-edged, the broad noise stretched two and a half times along the wind
// (streaks and veils rather than heaps).
struct cloud_slab
{
	float bottom, depth, shape, deck, cover, spread, dense, erosion;
	float3 drift_base;
	float2 drift_yalms, along;
	bool game;
};
// How far a deck's ragged base hangs below the slab at most (yalms)
static const float kScud = 70.0;

cloud_slab slab_now()
{
	cloud_slab s;
	float4 c5 = actors[kCloud + 5], drift = actors[kWx + 4], wind = actors[kWx];
	s.bottom = c5.x;
	s.depth = max(c5.y - c5.x, 1.0);
	s.shape = c5.z;
	s.cover = actors[kWx + 3].x;
	s.deck = saturate((c5.z - 0.55) / 0.3) * saturate(s.cover * 2.0 - 1.0);
	s.spread = 0.7 * (1.0 - 0.6 * s.cover); // (an overcast sky opens up less)
	s.dense = actors[kCloud + 7].x;
	s.erosion = actors[kCloud + 6].w;
	s.drift_base = float3(drift.x, 0.0, drift.y) * 16.0;
	s.drift_yalms = drift.xy * (16.0 * kBaseTexel.x);
	float wl = length(wind.xy);
	s.along = wl > 1e-3 ? wind.xy / wl : float2(1.0, 0.0);
	s.game = actors[kCloud + 7].y < 0.5;
	[flatten] if (s.game) // (a rain deck stays a deck)
	{
		s.depth *= lerp(0.5, 1.0, s.deck);
		s.dense *= lerp(0.35, 1.0, s.deck);
		s.erosion *= 0.4;
	}
	return s;
}

// The weather's cover where the clouds are over xz, as the broad noise has it: a little of it cloud at the least, all
// of it overcast; more or less where the clouds gather
float cover_at(cloud_slab s, float2 xz, float4 drift)
{
	return saturate(0.15 + 0.85 * s.cover + (cloud_patches(xz, drift) - 0.5) * s.spread);
}

// The broad cloud at d from the camera (the world's axes, y down; c the cover there): h how far up the slab it is
// (from a deck's ragged base: in patches it hangs down to kScud below it, scud under the deck), P its point (the world's
// x, z; y its height over the camera). A deck lies in rolls across the wind, 400 yalms apart and wavering: denser and
// darker, then thinner and lighter (roll: 1 the densest). Its drifting ground coordinates repeat with the drift (the patches' periods).
float cloud_base(cloud_slab s, float3 d, float c, out float h, out float3 P, out float roll)
{
	float alt = altitude(d);
	P = float3(eye.x + d.x, alt, eye.z + d.z);
	float lowest = s.bottom;
	roll = 0.5;
	[branch] if (s.deck > 0.0)
	{
		float2 g = P.xz - s.drift_yalms;
		float rag = 0.6 * noise_wrap(g / 160.0, 160.0) + 0.4 * noise_wrap(g / 80.0 + 7.3, 320.0);
		lowest -= s.deck * kScud * saturate(1.8 * rag - 0.6);
		roll = 0.5 + 0.5 * sin(6.2831853 * dot(g, s.along) / 400.0 + 3.0 * noise_wrap(g / 400.0 + 3.1, 64.0));
	}
	h = (alt - lowest) / (s.depth + s.bottom - lowest);
	if (h <= 0.0 || h >= 1.0)
		return 0.0;
	float cc = saturate(c * lerp(1.0, 0.85 + 0.25 * roll, s.deck));
	float3 tb = P / kBaseTexel - s.drift_base;
	[flatten] if (s.game)
		tb.xz = float2(dot(tb.xz, s.along) * 0.4, dot(tb.xz, float2(-s.along.y, s.along.x)));
	return cloud_shape(tb, h, cc, s.shape) * lerp(1.0, 0.45 + 1.1 * roll, s.deck);
}

// The Henyey-Greenstein phase (1 for light scattered evenly every way): how much of the light comes on toward the eye
// at mu, the cosine between the view ray and the way to the light; g > 0 forward
float phase_hg(float mu, float g)
{
	float k = 1.0 + g * g - 2.0 * g * mu;
	return (1.0 - g * g) / (k * sqrt(k));
}

// What the clouds along a view ray w (the world's, y down; up: its height's sine) give: their light (rgb) and how much
// of what's behind them still shows (a).
// Marched from where the ray enters the slab (a deck's ragged base below it) to where it leaves (at most kCloudReach
// on), each step's point a share `jitter` (0..1) into it. At each point with cloud, a few steps toward the sun or moon,
// growing in length, for how much of its light gets there (Beer-Lambert, with two fainter terms for light scattered many
// times over, which reaches deeper; the first step's cloud with its fine noise, so the billows shade each other), times
// the phase toward the eye (mostly forward, with a silver lining on what's thin toward the light; on an overcast deck
// light comes through spread out, without the glow toward the sun) and a powder term (light gets into the dense parts
// more than the wisps: they darken looking away from the light); and the sky's light round it, the record's gradient's,
// more at the top than underneath. Lit as the weather lights them (their colour where lit, [kWx + 5], underneath,
// [kWx + 6]: the record's gradient and sun colour); by the moon's light ([kSky + 13].w) a cool grey. Far off they fade
// toward the sky's own colour, and at the horizon into it.
float4 cloud_march(float3 w, float up, float jitter)
{
	cloud_slab sl = slab_now();
	float4 c4 = actors[kCloud + 4], c5 = actors[kCloud + 5], c6 = actors[kCloud + 6];
	float4 drift = actors[kWx + 4];
	float darker = actors[kWx + 5].w;
	float3 lit = actors[kWx + 5].rgb, shade = actors[kWx + 6].rgb;
	float moon = actors[kSky + 13].w;
	lit = lerp(lit, luma(lit) * float3(0.84, 0.9, 1.0), 0.85 * moon);
	shade = lerp(shade, luma(shade) * float3(0.86, 0.91, 1.0), 0.7 * moon);
	// (under a veil, mist's, the light on them dimmer: a haze, not bright heaps)
	float strength = c5.w * (1.0 - 0.5 * actors[kWx + 3].z);
	float3 L = c6.xyz;
	uint steps = max(uint(c4.y), 1u), light_steps = uint(c4.z);
	float t0 = shell_at(up, sl.bottom - kScud * sl.deck), t1 = min(shell_at(up, sl.bottom + sl.depth), t0 + min(kCloudReach, steps * kCloudStep));
	float cover0 = cover_at(sl, eye.xz + w.xz * t0, drift), cover1 = cover_at(sl, eye.xz + w.xz * t1, drift);
	float mu = dot(w, L);
	float forward = 0.8 * phase_hg(mu, 0.55) + 0.2 * phase_hg(mu, -0.25) + 0.025 * phase_hg(mu, 0.9);
	float softer = 0.7 * phase_hg(mu, 0.3) + 0.3 * phase_hg(mu, -0.15);
	float flat = max(sl.deck, saturate(sl.cover * 1.5 - 0.5)); // (how much of an overcast it is)
	forward = lerp(forward, softer, 0.85 * flat);
	float toward = saturate(mu * 0.5 + 0.5);
	float3 sky_high = sky_gradient(0.35) / 255.0 * (1.0 - darker);
	// (the sky's light on them: the weather's underside colour, some of the sky's own hue in it, brighter higher up)
	float3 sky_hue = sky_high / max(luma(sky_high), 0.03) * luma(shade);
	sky_hue = lerp(sky_hue, luma(sky_hue) * float3(0.86, 0.91, 1.0), 0.7 * moon);
	float hue = lerp(0.3, 0.12, sl.deck);
	float3 base_light = lerp(shade, sky_hue, hue) * lerp(0.5, 0.75, sl.deck), top_light = lerp(shade, sky_hue, hue) * 0.85;
	float3 drift_fine = float3(drift.z, 0.0, drift.w) * 64.0;
	// Two speeds: long steps through clear air, and once one lands in cloud, back most of a step and on in quarter steps
	// (a heap's light is all in its first few yalms), back to long steps after six quarter steps clear
	float dt = (t1 - t0) / steps, dt_fine = 0.25 * dt;
	float3 sum = 0.0;
	float T = 1.0, far = 0.0, seen = 0.0; // (how far off the cloud seen is: each step's distance by how much of it shows)
	float t = t0 + dt * jitter;
	bool fine = false;
	uint clear_run = 0;
	[loop] for (uint i = 0; i < steps * 4 && t < t1; ++i)
	{
		float3 d = w * t;
		float c = lerp(cover0, cover1, saturate((t - t0) / max(t1 - t0, 1e-3)));
		float h, roll;
		float3 P;
		float dens = cloud_base(sl, d, c, h, P, roll);
		[branch] if (!fine)
		{
			[branch] if (dens > 0.0)
			{
				fine = true;
				clear_run = 0;
				t = max(t - 0.75 * dt, t0);
			}
			else
				t += dt;
			continue;
		}
		[branch] if (dens > 0.0)
			dens = cloud_erode(dens, cloud_tex(P / kDetailTexel - drift_fine, kNoiseDetail, kNoiseDetailAt), h, sl.erosion);
		[branch] if (dens > 0.0)
		{
			clear_run = 0;
			float sigma = dens * sl.dense;
			float od = 0.0, s = 0.0, ds = 8.0;
			[loop] for (uint k = 0; k < light_steps; ++k)
			{
				float3 q = d + L * (s + 0.5 * ds);
				float qh, qroll;
				float3 qP;
				float qd = cloud_base(sl, q, c, qh, qP, qroll);
				[branch] if (k == 0 && qd > 0.0)
					qd = cloud_erode(qd, cloud_tex(qP / kDetailTexel - drift_fine, kNoiseDetail, kNoiseDetailAt), qh, sl.erosion);
				else
					qd *= 1.0 - 0.3 * sl.erosion;
				od += qd * sl.dense * ds;
				s += ds;
				ds *= 2.2;
			}
			float through = exp(-od) * forward + 0.5 * exp(-0.4 * od) * softer + 0.25 * exp(-0.15 * od);
			float powder = lerp(1.0 - exp(-sigma * 45.0), 1.0, 0.25 + 0.75 * toward * toward);
			// (the sky's light reaches less far in where much cloud lies toward the light: a deck's dense rolls and a heap's
			// creases darker)
			float sky_in = lerp(0.65, 1.0, exp(-0.06 * od));
			// (a deck's thinner rolls lighter underneath, its dense ones darker)
			float rolls = lerp(1.0, 1.45 - 0.9 * roll, sl.deck);
			float3 S = min((1.5 * lit * strength * through * powder + lerp(base_light, top_light, saturate(h)) * sky_in) * rolls, 8.0);
			float a = exp(-sigma * dt_fine);
			sum += T * S * (1.0 - a);
			far += T * (1.0 - a) * t;
			seen += T * (1.0 - a);
			T *= a;
			if (T < 0.015)
				break;
		}
		else if (++clear_run >= 6)
			fine = false;
		t += dt_fine;
	}
	far = seen > 1e-4 ? far / seen : t0;
	float fade = exp(-max(far - 300.0, 0.0) / 3500.0) * smoothstep(0.0, 0.06, up);
	float3 sky_at = sky_gradient(asin(saturate(up)) / 1.5707963) / 255.0 * (1.0 - darker);
	float a = 1.0 - T;
	// The game-like style: the sky's own colour lifted a little with some of their own light in it (more by day, and a
	// rain deck's grey most), never much brighter than the sky round them (by night hardly at all), and never quite
	// opaque (the sun, moon and stars glimmer through)
	[branch] if (sl.game)
	{
		float3 own = a > 1e-3 ? sum / a : 0.0;
		float mix_own = lerp(lerp(0.6, 0.2, moon), 0.85, sl.deck);
		float3 most = lerp(sky_at * 1.6 + 0.15, sky_at * 1.2 + 0.02, moon);
		float3 base = lerp(sky_at * 1.08 + 0.02, sky_at * 1.04 + 0.008, moon);
		float3 tinted = min(lerp(base, own, mix_own), most);
		a *= lerp(0.85, 1.0, sl.deck);
		sum = tinted * a;
		T = 1.0 - a;
	}
	// By the moon's light, either style: no brighter than the night sky round them by much (they don't glow)
	sum = lerp(sum, min(sum, (sky_at * 1.4 + 0.03) * a), moon);
	// (the brightest of them eased toward white rather than cut off at it: a soft knee from 0.75)
	float3 over = max(sum - 0.75, 0.0);
	sum = min(sum, 0.75) + 0.25 * (1.0 - exp(-over / 0.25));
	return float4(lerp(sky_at * (1.0 - T), sum, fade), T);
}

// The clouds' picture at u1 + at: a pixel (its light and how much of what's behind shows: two words of f16s), or
// between its pixels at s (its pixels' units, their middles at whole numbers), from those of them that hold clouds (none
// worked out there: 2), weighed by nearness; held: how much of that weight there was (0: none, the clear sky returned)
float4 cloud_read(uint at, uint2 q, uint width)
{
	uint2 v = sun_mask.Load2(at + (q.y * width + q.x) * 8);
	return float4(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y), f16tof32(v.y >> 16));
}

float4 cloud_between(uint at, float2 s, uint2 size, out float held)
{
	float2 i = floor(s), f = s - i;
	float4 sum = 0.0;
	held = 0.0;
	[unroll] for (int k = 0; k < 4; ++k)
	{
		int2 p = int2(i) + int2(k & 1, k >> 1);
		float wk = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
		bool on = all(p >= 0) && all(p < int2(size));
		float4 v = cloud_read(at, uint2(clamp(p, int2(0, 0), int2(size) - 1)), size.x);
		bool ok = on && v.a <= 1.5;
		sum += ok ? v * wk : 0.0;
		held += ok ? wk : 0.0;
	}
	return held > 1e-4 ? sum / held : float4(0.0, 0.0, 0.0, 1.0);
}

// The same through a Catmull-Rom spline (sharper: the last frame's picture is carried over again and again, and a blend
// between four pixels each time would blur it as the camera turns), kept within the four nearest's range; between four
// as above where any of the sixteen holds none
float4 cloud_sharp(uint at, float2 s, uint2 size, out float held)
{
	float2 i = floor(s), f = s - i;
	float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f), w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
	float4 sum = 0.0, lo = 1e9, hi = -1e9;
	bool whole = true;
	[unroll] for (int k = 0; k < 16; ++k)
	{
		int2 o = int2(k & 3, k >> 2);
		int2 p = int2(i) + o - 1;
		bool on = all(p >= 0) && all(p < int2(size));
		float4 v = cloud_read(at, uint2(clamp(p, int2(0, 0), int2(size) - 1)), size.x);
		whole = whole && on && v.a <= 1.5;
		float wx = o.x == 0 ? w0.x : o.x == 1 ? w1.x : o.x == 2 ? w2.x : w3.x;
		float wy = o.y == 0 ? w0.y : o.y == 1 ? w1.y : o.y == 2 ? w2.y : w3.y;
		sum += v * (wx * wy);
		bool inner = (o.x == 1 || o.x == 2) && (o.y == 1 || o.y == 2);
		lo = inner ? min(lo, v) : lo;
		hi = inner ? max(hi, v) : hi;
	}
	[branch] if (!whole)
		return cloud_between(at, s, size, held);
	held = 1.0;
	return clamp(sum, lo, hi);
}

// Value noise (0..1) at p, from a hash of the lattice's corners: the clouds' edges' finest grain, worked out per pixel
float hash3(int3 p)
{
	uint n = uint(p.x) * 1597334677u ^ uint(p.y) * 3812015801u ^ uint(p.z) * 2798796415u;
	n = (n ^ (n >> 16)) * 2246822519u;
	n ^= n >> 13;
	return float(n & 0xFFFFFFu) / 16777215.0;
}

float value_noise3(float3 p)
{
	int3 i = int3(floor(p));
	float3 f = p - floor(p);
	float3 u = f * f * (3.0 - 2.0 * f);
	float a = lerp(hash3(i), hash3(i + int3(1, 0, 0)), u.x), b = lerp(hash3(i + int3(0, 1, 0)), hash3(i + int3(1, 1, 0)), u.x);
	float c = lerp(hash3(i + int3(0, 0, 1)), hash3(i + int3(1, 0, 1)), u.x), d = lerp(hash3(i + int3(0, 1, 1)), hash3(i + int3(1, 1, 1)), u.x);
	return lerp(lerp(a, b, u.y), lerp(c, d, u.y), u.z);
}

// The clouds over the sky at a pixel of the world (xy: its middle, w its view ray in the world), from this frame's
// picture; in the volumetric style, at their edges (neither clear nor whole) sharpened at the world's own size: how much of what's behind shows
// pushed toward clear or whole by a fine noise where this pixel's own ray meets the slab's bottom (drifting with their
// fine noise; the same frame to frame), so the quarter-size picture's soft edges come out crisp and ragged, their light
// kept in step
float4 cloud_at(float2 xy, float3 w)
{
	float4 c3 = actors[kCloud + 3];
	float held;
	float4 c = cloud_between(asuint(c3.y), xy / 4.0 - 0.5, uint2(asuint(c3.w), asuint(actors[kCloud + 4].x)), held);
	float a = 1.0 - c.a;
	[branch] if (a > 0.003 && a < 0.997 && actors[kCloud + 7].y > 0.5) // (the volumetric style's: the game-like's stay soft)
	{
		float4 drift = actors[kWx + 4];
		float3 P = eye.xyz + w * shell_at(max(-w.y, 0.0), actors[kCloud + 5].x) - float3(drift.z, 0.0, drift.w) * (64.0 * kDetailTexel);
		float n = 0.6 * value_noise3(P / 9.0) + 0.4 * value_noise3(P / 4.0 + 31.7);
		float sharp = saturate((a - 0.5 + (n - 0.5) * 0.6) * 1.5 + 0.5);
		c.rgb *= min(sharp / a, 3.0);
		c.a = 1.0 - sharp;
	}
	return c;
}

// The clouds' picture, a pixel for each 4 x 4 of the world's (the pass's viewport that size): none where those show no
// sky (2); else carried over from the last frame's picture where the camera's turn puts this direction in it, and marched
// again on this pixel's turn (one frame in four: the 2 x 2 order) or where there's nothing to carry over (just come into
// view, or the picture new), then blended half and half with what was carried over (each march's jitter differs: over a
// few frames they average out)
void cloud_ps(float4 pos : SV_Position)
{
	int2 q = int2(pos.xy);
	float4 c3 = actors[kCloud + 3], c4 = actors[kCloud + 4];
	uint2 size = uint2(asuint(c3.w), asuint(c4.x));
	if (any(uint2(q) >= size))
		return;
	uint now_at = asuint(c3.y);
	bool sky = false;
	[unroll] for (int k = 0; k < 16; ++k)
		sky = sky || drawn_depth(q * 4 + int2(k & 3, k >> 2)) > kSkyDepth;
	float2 xy = (float2(q) + 0.5) * 4.0;
	float2 ndc = float2(xy.x / ground.y * 2.0 - 1.0, 1.0 - xy.y / ground.z * 2.0);
	float3 v = normalize(float3(ndc.x / lin.z, ndc.y / lin.w, up_row.w));
	float3 w = normalize(float3(dot(actors[kSky + 11].xyz, v), dot(up_row.xyz, v), dot(actors[kSky + 12].xyz, v)));
	float up = -w.y;
	float4 c = float4(0.0, 0.0, 0.0, 2.0);
	[branch] if (sky && up > -0.03)
	{
		float4 r0 = actors[kCloud], r1 = actors[kCloud + 1], r2 = actors[kCloud + 2];
		float3 was = w.x * r0.xyz + w.y * r1.xyz + w.z * r2.xyz; // (into the last frame's view space)
		float ahead = was.z * up_row.w;
		float held = 0.0;
		float4 kept = float4(0.0, 0.0, 0.0, 1.0);
		[branch] if (c3.x > 0.5 && ahead > 0.05)
		{
			float2 n = was.xy * float2(r0.w, r1.w) / ahead;
			float2 s = float2((n.x * 0.5 + 0.5) * ground.y, (0.5 - n.y * 0.5) * ground.z) / 4.0 - 0.5;
			kept = cloud_sharp(asuint(c3.z), s, size, held);
		}
		static const uint kOrder[4] = { 0, 2, 3, 1 };
		bool turn = kOrder[(uint(q.x) & 1u) | ((uint(q.y) & 1u) << 1)] == uint(r2.w);
		c = kept;
		[branch] if (held < 0.5 || turn)
		{
			float jitter = frac(frac(52.9829189 * frac(dot(float2(q), float2(0.06711056, 0.00583715)))) + asuint(c4.w) * 0.618034);
			float4 m = cloud_march(w, up, jitter);
			c = held >= 0.5 ? lerp(kept, m, 0.5) : m;
		}
	}
	sun_mask.Store2(now_at + (uint(q.y) * size.x + uint(q.x)) * 8, uint2(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | (f32tof16(c.a) << 16)));
}

// The clouds' shadows on the ground (crowd.x rule bit 2048, with the clouds): a map of how much sunlight (or moonlight)
// gets through them, [kCloud + 9].x texels a side round the camera, worked out each frame by cloud_shadow_ps after the
// clouds' pictures in u1 (a float a texel), for the ground at the camera's ground height ([kWx + 1].w), each texel's ray
// toward the light marched through the slab's broad cloud in ten steps. [kCloud + 8] x, y: its corner (the world's x and
// z, least), z: its texel (yalms), w: where it starts in u1 (bytes, uint bits); [kCloud + 9] x: its texels a side (uint
// bits), y: how strongly a shadow takes the sun's share of the light (the sun's shadows' strength, less the more the
// sky is covered: an overcast's light is the game's dim light already).
bool cloud_shadows() { return ((crowd.x >> 16) & 2048) != 0; }

void cloud_shadow_ps(float4 pos : SV_Position)
{
	uint2 q = uint2(pos.xy);
	float4 m = actors[kCloud + 8], m2 = actors[kCloud + 9];
	uint n = asuint(m2.x);
	if (any(q >= n))
		return;
	cloud_slab sl = slab_now();
	float4 drift = actors[kWx + 4];
	float3 L = actors[kCloud + 6].xyz;
	float3 d0 = float3(m.x + (q.x + 0.5) * m.z, actors[kWx + 1].w, m.y + (q.y + 0.5) * m.z) - eye.xyz;
	float lit = 1.0;
	[branch] if (L.y < -0.03)
	{
		float a0 = altitude(d0), rise = -L.y;
		float s0 = max((sl.bottom - kScud * sl.deck - a0) / rise, 0.0);
		float s1 = min((sl.bottom + sl.depth - a0) / rise, s0 + 1500.0);
		float ds = max(s1 - s0, 0.0) / 10.0;
		float c = cover_at(sl, eye.xz + d0.xz + L.xz * (0.5 * (s0 + s1)), drift);
		float od = 0.0;
		[loop] for (uint k = 0; k < 10; ++k)
		{
			float h, roll;
			float3 P;
			od += cloud_base(sl, d0 + L * (s0 + (k + 0.5) * ds), c, h, P, roll) * sl.dense * (1.0 - 0.3 * sl.erosion) * ds;
		}
		lit = exp(-od);
	}
	sun_mask.Store(asuint(m.w) + (q.y * n + q.x) * 4, asuint(lit));
}

// How much sunlight the clouds let through to a point of the world drawn (P in view space), from their shadow map: the
// point on its ray toward the light at the map's ground height (within 300 yalms), between the map's texels; whole
// past the map's edge (faded over its last eighth)
float cloud_shadow_at(float3 P)
{
	float4 m = actors[kCloud + 8], m2 = actors[kCloud + 9];
	uint n = asuint(m2.x);
	float3 L = actors[kCloud + 6].xyz;
	float3 W = eye.xyz + float3(dot(actors[kWx + 1].xyz, P), dot(up_row.xyz, P), dot(actors[kWx + 2].xyz, P));
	float along = L.y < -0.03 ? clamp((actors[kWx + 1].w - W.y) / L.y, -300.0, 300.0) : 0.0;
	float2 s = (W.xz + L.xz * along - m.xy) / m.z - 0.5;
	float2 i = floor(s), f = s - i;
	float sum = 0.0;
	[unroll] for (int k = 0; k < 4; ++k)
	{
		int2 p = clamp(int2(i) + int2(k & 1, k >> 1), int2(0, 0), int2(n, n) - 1);
		float wk = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
		sum += wk * asfloat(sun_mask.Load(asuint(m.w) + (uint(p.y) * n + uint(p.x)) * 4));
	}
	float2 edge = min(s, float(n) - 1.0 - s) / (0.125 * n);
	return lerp(1.0, sum, saturate(min(edge.x, edge.y)));
}

// An aurora's light along w (the world, y down; up: how high it is): curtains hanging in the sky, sheets that wave slowly
// across it, seen through at eight heights from their foot up (green low down, violet higher, fainter as they rise); their
// light in rays that hang straight down the sky (by the ray's bearing alone: the same up a column of the screen), slowly
// shifting. Light added (before the night and the setting's strength).
float3 aurora_light(float3 w, float up, float t)
{
	float3 sum = 0.0;
	float2 dir = w.xz / max(up, 0.03);
	float2 around = normalize(w.xz + 1e-5) * 60.0; // (the bearing on a circle: no seam)
	float rays = 0.25 + 0.75 * saturate(noise2(around + float2(t * 0.05, 0.0)) * 0.6 + noise2(around * 2.7 - float2(0.0, t * 0.08)) * 0.6 - 0.1);
	[unroll] for (int k = 0; k < 8; ++k)
	{
		float2 q = dir * (1.0 + 0.16 * k) * 0.35; // (where the ray is at that height, in the curtains' units)
		float wave = q.y + 0.8 * sin(q.x * 0.45 + t * 0.021) + 0.3 * sin(q.x * 1.3 - t * 0.037 + 1.7);
		float across = abs(frac(wave * 0.22) - 0.5) * 2.0; // (0 on a sheet .. 1 halfway to the next)
		float sheet = exp(-across * across * 30.0);
		float3 c = lerp(float3(0.1, 0.95, 0.4), float3(0.6, 0.15, 0.85), smoothstep(1.0, 6.0, float(k)));
		sum += c * sheet * (1.0 - k / 9.0);
	}
	return sum * rays * 0.35 * smoothstep(0.04, 0.25, up);
}

// A lightning bolt's sideways offset (radians) at an elevation: kinks of three sizes, its own (seed)
float bolt_jag(float e, float seed)
{
	return 0.12 * (noise2(float2(e * 18.0, seed)) - 0.5) + 0.05 * (noise2(float2(e * 60.0, seed + 17.0)) - 0.5) +
		0.018 * (noise2(float2(e * 200.0, seed + 41.0)) - 0.5);
}

float bolt_line(float d, float px, float thin)
{
	float wd = 1.1 * px * thin;
	return exp(-d * d / (wd * wd)) + 0.3 * exp(-d / (8.0 * px)) + 0.08 * exp(-d / 0.03);
}

// Its light along a ray at bearing az and elevation el (radians; px radians a pixel): a jagged line from where it starts
// (top, coming out of the cloud) down to the horizon at its bearing, three branches forking off it downward and dying
// out; a white core a pixel or two wide and a glow round it
float bolt_light(float az, float el, float bearing, float top, float seed, float px)
{
	float light = 0.0;
	[branch] if (el < top && el > -0.01)
	{
		float a = az - bearing - bolt_jag(el, seed);
		a -= 6.2831853 * round(a / 6.2831853);
		light = bolt_line(abs(a) * cos(el), px, 1.0);
		[unroll] for (int b = 0; b < 3; ++b)
		{
			float h = frac(sin(seed * 12.9898 + b * 78.233) * 43758.5453);
			float fork = top * (0.35 + 0.5 * h);                   // (where it forks off)
			float len = fork * (0.3 + 0.4 * frac(h * 7.13));       // (how far down it reaches)
			float slope = (frac(h * 3.71) < 0.5 ? -1.0 : 1.0) * (0.5 + 0.9 * frac(h * 5.17)); // (radians across for each radian down)
			float rest = (el - (fork - len)) / len;                // (1 at the fork .. 0 where it dies out)
			float ab = az - bearing - bolt_jag(fork, seed) - slope * (fork - el) - 0.6 * bolt_jag(el, seed + 5.3 * (b + 1));
			ab -= 6.2831853 * round(ab / 6.2831853);
			light += el < fork && rest > 0.0 ? 0.8 * sqrt(rest) * bolt_line(abs(ab) * cos(el), px, 0.75) : 0.0;
		}
		light *= smoothstep(top, top - 0.03, el);
	}
	return light;
}

// The sky's weather (weather_fx) over the gradient at this pixel (sky; w its view ray in the world, y down, up its
// height's sine, v in view space, xy the pixel's middle; behind: the sun or moon and the stars, which the clouds hide): an
// aurora's curtains, a veil over all of it (mist), the clouds (own_clouds: cloud_ps's, lit by the sun or moon), dust and
// gloom over them low down, the lightning's flash lighting the clouds most and its bolt in front of them
float3 weather_sky(float2 xy, float3 w, float3 v, float up, float3 sky, float3 behind, float px)
{
	float4 clouds = actors[kWx + 3], shade_c = actors[kWx + 6], veil_c = actors[kWx + 7];
	float4 flash = actors[kWx + 8], bolt = actors[kWx + 9], dust = actors[kWx + 10], gloom = actors[kWx + 12];
	float t = actors[kWx + 2].w;
	float3 colour = sky + behind;
	[branch] if (shade_c.w > 0.0)
		colour += aurora_light(w, up, t) * shade_c.w * saturate(1.2 - 4.0 * luma(sky));
	float veil = clouds.z * (0.55 + 0.45 * (1.0 - up));
	colour = lerp(colour, veil_c.rgb, veil);
	// The clouds (cloud_ps's picture, read between its pixels): what's behind them (the sky, the sun or moon, the stars)
	// showing through as much as they let it, their own light over it
	float cover = 0.0;
	[branch] if (own_clouds())
	{
		float4 c = cloud_at(xy, w);
		colour = colour * c.a + c.rgb;
		cover = 1.0 - c.a;
	}
	[branch] if (dust.w > 0.0)
		colour = lerp(colour, dust.rgb, saturate(dust.w * (0.2 + 0.5 * (1.0 - up) * (1.0 - up))));
	[branch] if (gloom.w > 0.0)
		colour = lerp(colour, gloom.rgb, saturate(gloom.w * 0.6 * (1.0 - smoothstep(0.0, 0.3, up))));
	// (the flash lights the clouds from within, the clear sky a little)
	[branch] if (flash.x > 0.0)
		colour = colour * (1.0 + flash.x * (0.15 + 0.6 * cover + 0.2 * veil)) + flash.x * (0.03 + 0.06 * cover) * float3(0.78, 0.85, 1.0);
	[branch] if (flash.y > 0.0)
		colour += flash.y * bolt_light(atan2(w.x, w.z), asin(saturate(up)), flash.z, flash.w, bolt.x, px) * float3(0.85, 0.9, 1.0);
	return colour;
}

// The sky at a pixel the zone left, as the add-on draws it (col: what the game drew there): its own gradient by the view
// ray's elevation with the sun or moon and the stars over it, where the pixel is still the game's dome (within
// [kSky + 15].z bytes of dome_colour, fading out by .w); what the game drew over its dome later in the frame (clouds, its
// sun and moon, effects) stays. Dithered by under half a byte, so the gradient doesn't come out in steps of one. With the
// weather of its own (weather_fx): the sky darker as the weather has it, the stars brighter and more of them in stellar
// glare, and the weather's clouds and the rest over it (weather_sky).
float3 sky_paint(float2 xy, float3 col)
{
	float2 ndc = float2(xy.x / ground.y * 2.0 - 1.0, 1.0 - xy.y / ground.z * 2.0);
	float3 v = normalize(float3(ndc.x / lin.z, ndc.y / lin.w, up_row.w)); // (view space)
	float4 row0 = actors[kSky + 11], row2 = actors[kSky + 12], light = actors[kSky + 13], disc = actors[kSky + 14], misc = actors[kSky + 15];
	float3 w = normalize(float3(dot(row0.xyz, v), dot(up_row.xyz, v), dot(row2.xyz, v))); // (the world's: y down)
	float up = -w.y;
	if (up <= 0.0)
		return col; // (below the horizon: the game's clear colour, as it is)
	float3 game = col * 255.0;
	float3 off = abs(game - dome_colour(w));
	float bare = 1.0 - smoothstep(misc.z, misc.w, max(off.r, max(off.g, off.b)));
	if (bare <= 0.0)
		return misc.y > 0.5 ? lerp(col, float3(1.0, 0.0, 0.0), 0.3) : col;
	float height = asin(saturate(up)) / 1.5707963;
	float3 sky = sky_gradient(height) / 255.0;
	float px = 2.0 / (lin.w * ground.z); // (radians a pixel, near the middle of the screen)
	bool weather = weather_fx();
	float boost = 1.0;
	[branch] if (weather)
	{
		sky *= 1.0 - actors[kWx + 5].w;
		boost = actors[kWx + 3].w;
	}
	// The sun (or the moon): a disc about one and a half times the real one's, and a glow around it. The moon as the game
	// draws its own: big (about seven times the sun's, 6 degrees across), soft-edged, its face mottled with darker seas
	// (a noise on the disc), tinted by the sky round it (pink at dusk, blue at night) and a soft glow about it. Cut by the
	// horizon (soft by a pixel). The moon in its phase (the game's: [kSky + 16..18].w its lit side in view space, [kSky +
	// 19].w how much of it is lit, 0 new .. 1 full): a sphere lit from the side by as much, the terminator soft, the dark
	// part just a faint earthshine; its glow as bright as its lit share.
	float3 add = 0.0;
	[branch] if (row2.w > 0.0)
	{
		float apart = acos(clamp(dot(v, light.xyz), -1.0, 1.0));
		float radius = lerp(0.008, 0.055, light.w);
		float soft = max(px, lerp(0.0, 0.12 * radius, light.w));
		float body = 1.0 - smoothstep(radius - soft, radius + px, apart);
		float lit = actors[kSky + 19].w;
		float phase = 1.0, face = 1.0;
		[branch] if (light.w > 0.0 && body > 0.0)
		{
			float3 side = float3(actors[kSky + 16].w, actors[kSky + 17].w, actors[kSky + 18].w);
			side -= light.xyz * dot(side, light.xyz);
			float len = length(side);
			side = len > 1e-4 ? side / len : float3(1.0, 0.0, 0.0); // (straight at the moon: any side)
			float3 q = v - light.xyz * dot(v, light.xyz); // (on the disc, out from its middle)
			float x = dot(q, side) / radius, y = dot(q, cross(light.xyz, side)) / radius;
			float z = sqrt(saturate(1.0 - x * x - y * y)); // (toward the eye)
			// (the sun from the side by the phase angle: a share lit of (1 + cos a) / 2 = lit)
			float c = 2.0 * saturate(lit) - 1.0, s = sqrt(saturate(1.0 - c * c));
			float edge = max(1.5 * px / radius, 0.08);
			phase = lerp(0.04, 1.0, smoothstep(-edge, edge, x * s + z * c));
			// (its seas: broad darker patches, finer mottling in them)
			float2 m = float2(x, y);
			float seas = 0.55 * noise2(m * 2.3 + 5.1) + 0.3 * noise2(m * 5.7 + 11.3) + 0.15 * noise2(m * 13.0 + 2.7);
			face = 1.0 - 0.38 * smoothstep(0.42, 0.68, seas);
		}
		float glow_share = lerp(1.0, 0.08 + 0.92 * saturate(lit), light.w);
		float beyond = max(apart - radius, 0.0);
		float glow = lerp(0.45 * exp(-apart / 0.035) + 0.16 * exp(-apart / 0.25), 0.16 * exp(-beyond / 0.03) + 0.05 * exp(-beyond / 0.15), light.w);
		// (the moon's light the sky's hue a little: kept to a sensible range, so a near-black sky doesn't make it garish)
		float3 hue = clamp(sky / max(luma(sky), 0.03), 0.6, 1.5);
		float3 tint = lerp(1.0, hue, 0.3 * light.w);
		add += disc.rgb * tint * (body * lerp(1.0, 0.85 * phase * face, light.w) + glow * glow_share) * row2.w * smoothstep(-px, px, up);
	}
	// The stars, turned about the pole (35 degrees up in the north) by the hour; fewer near the horizon and none on a sky
	// still bright (twilight)
	[branch] if (row0.w > 0.0)
	{
		const float3 pole = float3(0.0, -0.5735764, 0.8191520);
		float c = cos(disc.w), s = sin(disc.w);
		float3 turned = w * c + cross(pole, w) * s + pole * dot(pole, w) * (1.0 - c);
		float dark = saturate(1.3 + 0.25 * (boost - 1.0) - 4.0 * luma(sky));
		add += star_field(turned, px, misc.x, min(boost, 2.0)) * row0.w * boost * dark * smoothstep(0.0, 0.25, up);
	}
	float3 seen = sky + add;
	[branch] if (weather)
		seen = weather_sky(xy, w, v, up, sky, add, px);
	float noise = frac(52.9829189 * frac(dot(xy, float2(0.06711056, 0.00583715)))) - 0.5;
	float3 painted = lerp(col, seen + noise / 255.0, bare);
	return misc.y > 0.5 ? lerp(painted, lerp(float3(1.0, 0.0, 0.0), float3(1.0, 0.0, 1.0), bare), 0.3) : painted;
}

// The world's colour at a point between pixels (from the copy: the four around, blended)
float3 texel_at(float2 s)
{
	float2 f = s - 0.5;
	int2 a = int2(floor(f));
	float2 k = f - floor(f);
	return lerp(lerp(texel(a), texel(a + int2(1, 0)), k.x), lerp(texel(a + int2(0, 1)), texel(a + int2(1, 1)), k.x), k.y);
}

// Heat shimmer (weather_fx: [kWx + 13] x how strong, y, z the distance it starts at and is whole by, yalms): what's drawn
// far off over the ground taken from a little way off on the screen (between pixels: no stepping), the offset wavering
// as hot rising air bends the light, mostly up and down; more with distance, none near the camera, the more the nearer
// the ground under the camera (none 14 yalms over it: far hills, treetops). The wavering is fixed to the view's
// direction in the world, so it stays put as the camera turns. Never from the sky, a name, or what stands much nearer
// (the edge of a tree in front stays clean).
float3 shimmered(int2 p, float3 P, float z, float3 col)
{
	float4 h = actors[kWx + 13], r0 = actors[kWx + 1], r2 = actors[kWx + 2];
	float over = r0.w - (eye.y + dot(P, up_row.xyz)); // (y is down)
	float k = h.x * smoothstep(h.y, h.z, z) * (1.0 - smoothstep(3.0, 14.0, over));
	[branch] if (k > 0.0)
	{
		float3 d = normalize(float3(dot(r0.xyz, P), dot(up_row.xyz, P), dot(r2.xyz, P)));
		float t = r2.w;
		float2 at = d.xz * 140.0 + float2(-d.y * 140.0 - t * 1.7, -d.y * 90.0 + t * 1.1);
		float2 wob = float2(noise2(at) - 0.5, noise2(at * 1.6 + 23.0) - 0.5) * float2(0.5, 1.0);
		float2 s = float2(p) + 0.5 + wob * k * 0.0016 * ground.y;
		int2 q = clamp(int2(s), int2(0, 0), int2(ground.yz) - 1);
		float zq = drawn_depth(q);
		if (zq < 1e8 && zq > 0.7 * z && !name_at(q))
			col = texel_at(s);
	}
	return col;
}

float4 shade_ps(float4 pos : SV_Position) : SV_Target
{
	int2 p = int2(pos.xy);
	float3 col = texel(p);
	float z = drawn_depth(p);
	[branch] if (z > kSkyDepth && own_sky() && !name_at(p))
		return float4(sky_paint(pos.xy, col), 1.0);
	if (z > 1e8 || name_at(p))
		return float4(col, 1.0); // (the sky; a name facelift drew into the world)
	float px_per_yalm = lin.z * 0.5 * ground.y / z;

	// The surface's point and facing, from the depth around it (for the ambient occlusion, the sun and the lamps)
	float3 P = view_at(p);
	[branch] if (weather_fx())
		col = shimmered(p, P, z, col);
	float3 n = float3(0.0, 0.0, 1.0);
	float steep = 1.0;
	bool lamps = ((crowd.x >> 16) & 16) != 0;
	bool wetting = wet();
	bool cloud_shade = cloud_shadows();
	if (shade.x > 0.0 || shade.w > 0.0 || lamps || wetting || cloud_shade)
		n = surface_normal(p, P, steep);
	float noise = frac(52.9829189 * frac(dot(pos.xy, float2(0.06711056, 0.00583715))));

	// Ambient occlusion: twelve points within the radius, turned per pixel; each one above the surface and
	// near enough shades it
	float ao = 1.0;
	float radius_px = min(shade.y * px_per_yalm, 96.0); // (capped: near the camera it would reach across the screen)
	if (shade.x > 0.0 && radius_px >= 2.0)
	{
		float spin = noise * 6.2831853;
		float occ = 0.0;
		[unroll] for (int i = 0; i < 12; ++i)
		{
			float a = spin + i * 2.3999632;
			float rr = radius_px * sqrt((i + 0.5) / 12.0);
			float3 v = view_at(p + int2(float2(cos(a), sin(a)) * rr)) - P;
			float d2 = dot(v, v);
			occ += saturate(dot(n, v) * rsqrt(d2 + 1e-6) - 0.15) * saturate(1.0 - d2 / (shade.y * shade.y));
		}
		ao = saturate(1.0 - shade.x * occ / 12.0);
	}

	// Surface relief: at about 0.12 yalms, a point darker than the ground around it (a joint between stones,
	// a crack) gets darker and a lighter one lighter, with a little light from above; only from taps on the
	// same surface (a similar depth), and fading out where that's under a few pixels
	float relief = 1.0;
	float rr = min(0.12 * px_per_yalm, 40.0);
	if (shade.z > 0.0 && rr >= 1.5)
	{
		static const int2 ring[8] = { int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2), int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1) };
		float sum = 0.0, n = 0.0;
		[unroll] for (int i = 0; i < 8; ++i)
		{
			int2 q = p + int2(float2(ring[i]) * 0.5 * rr);
			if (abs(drawn_depth(q) - z) < 0.03 * z + 0.1)
			{
				sum += luma(texel(q));
				n += 1.0;
			}
		}
		if (n >= 4.0)
		{
			float mean = sum / n, c = luma(col);
			int2 up = p - int2(0, int(rr * 0.5)), down = p + int2(0, int(rr * 0.5));
			float lit = (luma(texel(up)) - luma(texel(down))) / (mean + 0.04);
			float detail = (c - mean) / (mean + 0.04);
			relief = 1.0 + shade.z * clamp(0.6 * detail + 0.2 * lit, -0.45, 0.45) * saturate((rr - 1.5) / 3.0);
		}
	}
	// Sun shadows: worked out for one pixel in four (sun_ps, below), each starting its march at its own one of
	// sixteen points within the first step; here the 4 x 4 of those around this pixel are averaged, so every
	// pixel sees all sixteen starts and the shadow comes out soft and whole rather than in a pattern (one
	// march a pixel, all or nothing, showed as hatching). Only those on this pixel's surface count (a similar
	// depth), so a shadow doesn't bleed across an edge. Each holds two amounts: the shadow (the march's and the
	// map's), which takes the sun's share of this surface's light (sun_share: none on a surface turned away, which the
	// game's own lighting already leaves to the ambient light); and the map's shadow from something well above (over
	// 2 yalms toward the sun: a canopy, a crown, a cliff) on a surface seen too nearly edge on for its facing to be
	// told from the depth, already at the share a surface facing up would lose (a ledge's steep face seen edge on
	// under trees stayed bright, and came and went with the camera's angle), and a character's (sun_ps), both taken
	// as they are.
	float sunlit = 1.0;
	if (shade.w > 0.0)
	{
		float share = sun_share(dot(n, sun_in_view()));
		int2 size = sun_mask_size();
		int2 first = (p - 3) >> 1;
		float2 sum = 0.0, nearest_shadow = 0.0;
		float weight = 0.0, nearest = 1e9;
		[unroll] for (int j = 0; j < 16; ++j)
		{
			int2 h = clamp(first + int2(j & 3, j >> 2), int2(0, 0), size - 1);
			uint m = sun_mask.Load((h.y * size.x + h.x) * 4);
			float2 s = float2(m & 0xFF, (m >> 8) & 0xFF) / 255.0;
			float apart = abs(f16tof32(m >> 16) - z);
			float same = apart < 0.03 * z + 0.1 ? 1.0 : 0.0;
			sum += same * s;
			weight += same;
			nearest_shadow = apart < nearest ? s : nearest_shadow;
			nearest = min(nearest, apart);
		}
		// (none at this depth: ground seen at a grazing angle far off, where each row of pixels is a yalm or more
		// further than the last and the pass's rows skip every other one: the nearest in depth, rather than none,
		// which left every other row there unshaded)
		float2 s = weight > 0.0 ? sum / weight : nearest_shadow;
		sunlit = 1.0 - shade.w * max(s.x * share, s.y);
	}
	// The clouds' shadows (cloud_shadow_at): what they keep off the sun's share of this surface's light, as strong as
	// [kCloud + 9].y says
	[branch] if (cloud_shade)
		sunlit *= 1.0 - actors[kCloud + 9].y * sun_share(dot(n, sun_in_view())) * (1.0 - cloud_shadow_at(P));
	// Lamps lit near (lamp_light): their glow on top, darkened by the ambient occlusion like the rest
	float3 glow = 0.0;
	[branch] if (lamps)
		glow = lamp_light(P, n, steep, col) * ao;
	// Wet path and stone in rain (wet_here), on what faces up: darker (soaked), the overcast sky mirrored more the more
	// grazing the view (water's own reflectance on the film), and a little highlight toward the game's light. A surface
	// seen too nearly edge on for its facing to be told (far ground) is taken as facing up.
	float wet_dark = 1.0;
	float3 wet_add = 0.0;
	[branch] if (wetting)
	{
		const float3 world_up = -up_row.xyz; // (the world's up in view space: its y is down)
		const bool sure = steep < 4.0;
		float k = actors[kWet].w * (sure ? saturate((dot(n, world_up) - 0.5) / 0.3) : 1.0);
		[branch] if (k > 0.0)
			k *= wet_here(P);
		float3 wn = sure ? n : world_up;
		float3 v = normalize(-P);
		float fresnel = 0.04 + 0.96 * pow(1.0 - saturate(dot(wn, v)), 5.0);
		float spec = pow(saturate(dot(wn, normalize(v + sun_in_view()))), 60.0);
		wet_dark = 1.0 - 0.3 * k;
		wet_add = k * (actors[kWet + 5].rgb * fresnel * 0.35 + actors[kWet + 4].rgb * spec * 0.3);
	}
	// The game's fog over what's drawn here (its colour times how much of it there is at this depth) isn't the scene:
	// what's darkened is the scene under it, so a shadow or a crevice far off in mist fades into the haze as the rest
	// of the scene there does (darkened whole, the fog over them went dark too); a lamp's glow is seen through it as
	// the surface is
	float3 haze = 0.0;
	float clear = 1.0;
	[branch] if (hazy())
	{
		clear = clear_at(z);
		haze = haze_colour() * (1.0 - clear);
	}
	return float4(haze + (col - haze) * (ao * relief * sunlit * wet_dark) + (glow + wet_add) * clear, 1.0);
}

// Characters and monsters near (t2, from the Ashita addon: their feet, the game's world, and their size):
// each a standing capsule, from the feet up 2.3 times its size, 0.6 times its size and a bit around
struct body
{
	float3 feet; // (view space)
	float height, radius;
};

body body_in_view(uint i)
{
	float4 a = actors[i];
	float4 c = mul(float4(a.xyz, 1.0), view_proj);
	body b;
	b.feet = float3(c.x / lin.z, c.y / lin.w, c.w / up_row.w); // (the projection's _11, _22 and _34 undone)
	b.height = 2.3 * a.w;
	b.radius = 0.6 * a.w + 0.15;
	return b;
}

bool inside(body b, float3 x, float3 up)
{
	float3 d = x - b.feet;
	float3 r = d - up * clamp(dot(d, up), 0.0, b.height);
	return dot(r, r) < b.radius * b.radius;
}

// How near the ray o + d t (t >= 0, d of unit length) comes to the segment from a to b, and the ray's t there
// (the closest points of two segments, Ericson's, the ray taken as one 100 yalms long)
float ray_to_segment(float3 o, float3 d, float3 a, float3 b, out float t)
{
	static const float kLong = 100.0;
	float3 ab = b - a, r = o - a;
	float aa = kLong * kLong, e = dot(ab, ab), f = dot(ab, r), c = kLong * dot(d, r), bb = kLong * dot(d, ab);
	float denom = aa * e - bb * bb;
	float s = denom > 1e-6 * aa * e ? saturate((bb * f - c * e) / denom) : 0.0;
	float u = (bb * s + f) / e;
	s = u < 0.0 ? saturate(-c / aa) : u > 1.0 ? saturate((bb - c) / aa) : s;
	t = s * kLong;
	return length(o + d * t - (a + ab * saturate(u)));
}

// Where the ray o + d t (d of unit length) is inside the capsule of radius r around the segment from a to b: from t0
// to t1 (t1 < t0: nowhere). The capsule is the two balls at the ends and the cylinder between them; it's convex, so
// the ray is inside it from the first of their entries to the last of their exits.
void capsule_chord(float3 o, float3 d, float3 a, float3 b, float r, out float t0, out float t1)
{
	float3 ab = b - a, oa = o - a, ob = o - b;
	float ha = dot(d, oa), ka = ha * ha - dot(oa, oa) + r * r;
	float hb = dot(d, ob), kb = hb * hb - dot(ob, ob) + r * r;
	t0 = min(ka >= 0.0 ? -ha - sqrt(max(ka, 0.0)) : 1e9, kb >= 0.0 ? -hb - sqrt(max(kb, 0.0)) : 1e9);
	t1 = max(ka >= 0.0 ? -ha + sqrt(max(ka, 0.0)) : -1e9, kb >= 0.0 ? -hb + sqrt(max(kb, 0.0)) : -1e9);
	// (the cylinder: none apart from the balls when the ray runs along the axis)
	float abab = dot(ab, ab), abd = dot(ab, d), aboa = dot(ab, oa);
	float k = abab - abd * abd, m = abab * ha - aboa * abd, n = abab * dot(oa, oa) - aboa * aboa - r * r * abab;
	float h = m * m - k * n;
	if (k > 1e-6 * abab && h >= 0.0)
	{
		float c0 = (-m - sqrt(h)) / k, c1 = (-m + sqrt(h)) / k;
		// (between the ends: 0 <= aboa + t abd <= abab)
		bool across = abs(abd) < 1e-6;
		float e0 = across ? (aboa >= 0.0 && aboa <= abab ? -1e9 : 1e9) : min(-aboa / abd, (abab - aboa) / abd);
		float e1 = across ? (aboa >= 0.0 && aboa <= abab ? 1e9 : -1e9) : max(-aboa / abd, (abab - aboa) / abd);
		float lo = max(c0, e0), hi = min(c1, e1);
		t0 = lo <= hi ? min(t0, lo) : t0;
		t1 = lo <= hi ? max(t1, hi) : t1;
	}
}

// A character's or monster's shadow is worked out from them as they stand, not by the march (sun_ps, below):
// - The body as a smooth slim capsule (body_shape): from the feet, a ball of kSlim times its size and a bit across
//   resting on the ground, up kTall times its size; how near the ray toward the sun comes to it gives the shadow, whole
//   within its radius and fading to none over a soft edge that grows a little with the distance along the ray (a
//   penumbra). It's worked out at once for the whole ray: as dark at the head's end of the shadow as at the feet.
// - Their real shape where it's on screen (silhouette): along the stretch of the ray inside the body's capsule (the
//   larger one, body_in_view's), 2 x kSilhouetteSteps points evenly spaced (a twelfth of a yalm apart or less at
//   size 2: a thin arm, a tail or a staff isn't stepped over) are looked at in the depth. Where what's drawn in front of one
//   is them (a point inside that capsule, above the ground at their feet) and the point is just behind it (at most
//   kThick times their size and a bit: a body's depth), the ray passes through them; where what's drawn is further
//   than the point, it passes beside them. Only where the point can't be seen (off screen, behind something else in
//   front, behind a name facelift drew) does the slim capsule answer for it. Walls and other things never cast here:
//   they aren't inside the capsule (and what is, the ground at their feet aside, is taken as them).
// The march leaves characters and monsters out (it found them only where a step happened to land inside them: the
// shadow came out whole at the feet but dotted and lighter toward the head, and as the march's step grows, a thin
// limb or the neck went missing for some start phases).
static const int kSilhouetteSteps = 16;
static const float kSlim = 0.2, kTall = 1.9, kThick = 0.35;

// The slim capsule's segment (view space) and radius for actor i
void body_shape(uint i, body b, float3 up, out float3 a, out float3 c, out float r)
{
	float size = actors[i].w;
	r = kSlim * size + 0.06;
	a = b.feet + up * r;
	c = b.feet + up * max(kTall * size - r, r + 0.01);
}

// How much of the slim capsule a ray's point at distance `apart` from its segment, t along the ray, is in
float body_cover(float apart, float r, float t)
{
	float soft = 0.3 * r + 0.02 * t;
	return 1.0 - smoothstep(r - soft, r + soft, apart);
}

// The shadow of actor i on the ray o + d t (from t_start on) by their shape on screen, the slim capsule where
// that isn't seen (above): half of the 2 x kSilhouetteSteps points along it (half 0 or 1: every other one)
float silhouette(float3 o, float3 d, uint i, float t_start, float3 up, uint half)
{
	body b = body_in_view(i);
	float3 sa, sb;
	float sr;
	body_shape(i, b, up, sa, sb, sr);
	float thick = kThick * actors[i].w + 0.15;
	float t0, t1;
	capsule_chord(o, d, b.feet, b.feet + up * b.height, b.radius, t0, t1);
	t0 = max(t0, t_start);
	float shadow = 0.0;
	[branch] if (t1 > t0)
	{
		[unroll] for (int k = 0; k < kSilhouetteSteps; ++k)
		{
			float t = lerp(t0, t1, (2 * k + half + 0.5) / (2 * kSilhouetteSteps));
			float3 S = o + d * t;
			float w = S.z * up_row.w;
			float2 s = screen_of(S);
			int2 q = clamp(int2(s), int2(0, 0), int2(ground.yz) - 1);
			bool seen = w > 0.2 && all(s >= 0.0) && all(s < float2(ground.yz)) && !name_at(q);
			float drawn = drawn_depth(q);
			float in_front = w - drawn; // (how far behind what's drawn there the point is)
			// What's drawn there, put back in view space: them if inside their capsule and above the ground at their
			// feet (more so the further out from the middle: on a slope the ground within the capsule rises)
			float3 x = float3(S.xy * drawn / max(w, 0.2), drawn * up_row.w) - b.feet;
			float height = dot(x, up);
			float3 out_from_middle = x - up * height;
			float3 off_axis = x - up * clamp(height, 0.0, b.height);
			bool theirs = dot(off_axis, off_axis) < b.radius * b.radius && height > 0.15 + 0.5 * length(out_from_middle);
			// (just behind their surface: past the depth's precision, within a body's depth)
			float bias = 0.01 + 0.001 * w;
			float through = smoothstep(bias, 2.0 * bias, in_front) * (1.0 - smoothstep(0.75 * thick, thick, in_front));
			// (hidden: off screen, under a name, or behind something else in front of the point)
			bool hidden = !seen || (!theirs && in_front > 0.05 + 0.005 * w);
			float3 sx = S - sa, sab = sb - sa;
			float guess = body_cover(length(sx - sab * saturate(dot(sx, sab) / dot(sab, sab))), sr, t);
			shadow = max(shadow, hidden ? guess : theirs ? through : 0.0);
		}
	}
	return shadow;
}

// The shadow of the characters and monsters near (t2) on the ray o + d t (from t_start on), but `self`'s (the one
// the point is on: none for 32 and over): the two whose capsules the ray passes deepest through by their shape
// (silhouette), the rest by their slim capsules (body_cover)
float actor_shadow(float3 o, float3 d, uint self, float t_start)
{
	float3 up = -up_row.xyz; // (the world's up in view space: its y is down)
	uint count = min(crowd.x & 0xFFFF, 32u);
	float shadow = 0.0;
	uint k0 = 32u, k1 = 32u;
	float best0 = 1.0, best1 = 1.0, cover0 = 0.0, cover1 = 0.0;
	// (as many as were sent, the same for every pixel, 32 at most: no pixel's own reads decide how long it runs. Not
	// unrolled: 32 copies of this made the shaders take five times as long to compile, and the add-on compiles them
	// as the game starts)
	[loop] for (uint i = 0; i < count; ++i)
	{
		if (i != self)
		{
			body b = body_in_view(i);
			// (a ray passing well clear of the capsule's middle can't touch it)
			float3 m = b.feet + up * (0.5 * b.height) - o;
			float along = max(dot(m, d), 0.0), reach = 0.5 * b.height + b.radius;
			[branch] if (dot(m, m) - along * along < reach * reach)
			{
				float3 sa, sb;
				float sr, t, unused;
				body_shape(i, b, up, sa, sb, sr);
				float cover = body_cover(ray_to_segment(o, d, sa, sb, t), sr, t);
				float deep = ray_to_segment(o, d, b.feet, b.feet + up * b.height, unused) / b.radius;
				bool first = deep < best0, second = !first && deep < best1;
				// (one pushed out of the two by a deeper one, or never in them, by its slim capsule)
				shadow = max(shadow, first ? cover1 : second ? cover1 : cover);
				k1 = first ? k0 : second ? i : k1;
				best1 = first ? best0 : second ? deep : best1;
				cover1 = first ? cover0 : second ? cover : cover1;
				k0 = first ? i : k0;
				best0 = first ? deep : best0;
				cover0 = first ? cover : cover0;
			}
		}
	}
	// (each of the two in two halves: a fixed run of four, one copy of the steps; a half isn't looked at once the
	// shadow is whole)
	[loop] for (uint j = 0; j < 4; ++j)
	{
		uint k = j < 2 ? k0 : k1;
		[branch] if (k < 32u && shadow < 1.0)
			shadow = max(shadow, silhouette(o, d, k, t_start, up, j & 1));
	}
	return shadow;
}

// The sun's shadow map (grass_render.cpp, draw_sun_map): the zone's own meshes seen down the sun's rays, in two
// cascades around the camera (40 and 160 yalms each way across the rays), each texel how far from the sun the
// nearest of them is there (0..1 over the zone's whole depth along the rays). Its copy is t0 in the sun pass, a
// float a texel, the far cascade after the near one; its parameters follow the actors in t2 when it's there
// (crowd.x rule bit 8): [32..34] the near cascade's column, row and depth for a view-space point (x, y, z, 1),
// [35..37] the far one's; [38] texels a side, depth a yalm near and far, the copy's row pitch (uints); [39] a
// texel's size (yalms) near and far, where in the copy each cascade's last drawing starts (uints, as their bits: far,
// near). Each cascade's drawing before its last, which the last fades in over (grass_render.cpp draw_sun_map), after
// the wear's: [kSunMapPrev..+2] the near one's rows, [+3..+5] the far one's, [+6] how far each last drawing has faded in
// (near, far; 1: wholly, the one before not read), where each one before starts (uints, as bits: near, far), [+7]
// their depth a yalm (near, far).
ByteAddressBuffer sun_map : register(t0);
static const uint kSunMap = 32;
static const uint kSunMapPrev = kWear + 2 + 2 * kMaxCuts;

// How much of the sun the map says is blocked at P (view space, n its facing), from the near cascade where P is
// well inside it, else the far one; covered: 1 inside them, fading out over the far cascade's last twentieth,
// 0 beyond it (nothing read then). P is moved off its surface by a texel and a half and toward the sun by one,
// and a texel and 3 cm more is allowed in depth, so a surface doesn't shade itself; then the 4 x 4 texels around
// are tested, weighted as three bilinear taps a side (a soft edge a texel or two wide), its middle stretched by
// kSunMapEdge (below). above: the part of it blocked by something over 2 yalms nearer the sun than P (not the
// surface's own other side)
// kSunMapEdge: 0 leaves the filtered edge as it is (10 to 90 % across about 2.5 texels: 10 cm near, and as 1 / sin
// of the light's height more where the edge runs across its rays); over 0, smoothstep(kSunMapEdge, 1 - kSunMapEdge)
// of it: at 0.15 about half as wide, still placed between texels (no stair steps), for no measurable time. The
// shadow of something a texel or less across (4 cm near, 16 cm far) comes out lighter then (a 4 cm pole's: 17 % of
// a whole shadow's darkness, 33 % at 0), of one two texels across darker (66 to 82 %). On at 0.15 since 2026-10-21:
// the shadows near the camera looked soft in game.
static const float kSunMapEdge = 0.15;

// One drawing of a cascade at q (its rows from actors[rows]; per_yalm its depth a yalm, base where it starts in the
// copy): the 4 x 4 texels around, weighted as three bilinear taps a side: (blocked, blocked from well above)
float2 map_drawing(float4 q, uint rows, float per_yalm, uint base, float texel, float size, uint pitch)
{
	float2 t = float2(dot(q, actors[rows]), dot(q, actors[rows + 1]));
	float depth = dot(q, actors[rows + 2]) - (0.03 + texel) * per_yalm;
	float well_above = depth - 2.0 * per_yalm; // (2 yalms nearer the sun)
	float2 f = t - 0.5;
	int2 corner = int2(floor(f)) - 1;
	float2 w = f - floor(f);
	float wx[4] = { 1.0 - w.x, 1.0, 1.0, w.x }, wy[4] = { 1.0 - w.y, 1.0, 1.0, w.y };
	float2 r = 0.0;
	[unroll] for (int j = 0; j < 16; ++j)
	{
		int2 at = clamp(corner + int2(j & 3, j >> 2), int2(0, 0), int2(size, size) - 1);
		// (a texel nothing was drawn into keeps the clear's 1: never in the way, even of a point further
		// from the sun than anything in the zone, past the map's depth)
		float d = asfloat(sun_map.Load((base + at.y * pitch + at.x) * 4));
		float k = wx[j & 3] * wy[j >> 2];
		r.x += (d < depth && d < 1.0 ? 1.0 : 0.0) * k;
		r.y += (d < well_above && d < 1.0 ? 1.0 : 0.0) * k;
	}
	return r / 9.0;
}

// The cascade's last drawing, faded in over the one before as far as kSunMapPrev+6 says (where the one before covers
// the point, two texels in from its edge; else the last alone)
float sun_from_map(float3 P, float3 n, float3 to_sun, out float covered, out float above)
{
	float4 info = actors[kSunMap + 6], info2 = actors[kSunMap + 7], fade = actors[kSunMapPrev + 6], prev_per_yalm = actors[kSunMapPrev + 7];
	float size = info.x;
	float4 q = float4(P + n * (1.5 * info2.x) + to_sun * info2.x, 1.0);
	float2 t = float2(dot(q, actors[kSunMap]), dot(q, actors[kSunMap + 1]));
	bool near = min(min(t.x, t.y), size - max(t.x, t.y)) > 4.0;
	float texel = near ? info2.x : info2.y;
	uint c = near ? kSunMap : kSunMap + 3;
	q = float4(P + n * (1.5 * texel) + to_sun * texel, 1.0);
	t = float2(dot(q, actors[c]), dot(q, actors[c + 1]));
	covered = near ? 1.0 : saturate(min(min(t.x, t.y), size - max(t.x, t.y)) / (0.05 * size));
	float shadow = 0.0;
	above = 0.0;
	[branch] if (covered > 0.0)
	{
		uint pitch = uint(info.w);
		float2 r = map_drawing(q, c, near ? info.y : info.z, asuint(near ? info2.w : info2.z), texel, size, pitch);
		float faded = near ? fade.x : fade.y;
		[branch] if (faded < 1.0)
		{
			uint pc = near ? kSunMapPrev : kSunMapPrev + 3;
			float2 tp = float2(dot(q, actors[pc]), dot(q, actors[pc + 1]));
			if (min(min(tp.x, tp.y), size - max(tp.x, tp.y)) > 2.0)
				r = lerp(map_drawing(q, pc, near ? prev_per_yalm.x : prev_per_yalm.y, asuint(near ? fade.z : fade.w), texel, size, pitch), r, faded);
		}
		shadow = r.x;
		above = r.y;
		if (kSunMapEdge > 0.0)
		{
			shadow = smoothstep(kSunMapEdge, 1.0 - kSunMapEdge, shadow);
			above = smoothstep(kSunMapEdge, 1.0 - kSunMapEdge, above);
		}
	}
	return shadow;
}

// Sun shadows, for the world's even rows and columns (a pixel in four), into u1 for shade_ps to blur, a uint each:
// how much of the sun is blocked (8 bits, 0..1: the march's and the map's, to be taken by the surface's sun share)
// | the map's shadow from well above on a surface seen too nearly edge on to tell its facing (8 bits, taken whole)
// << 8 | f16 how far the pixel is (view depth) << 16.
// With the shadow map (above), what the zone's meshes cast comes from it, on screen or off it; the march below
// then only looks for contact shadows where the map covers the point: its first 16 steps (to 1.7 yalms), which
// the map can't draw finely enough (its texels and bias are a few centimetres) and which come from what isn't in
// it (the grass, objects the zone's object list doesn't place). The two are put together by the larger.
// From the surface toward the sun in 32 steps, each 1.25 times the last, from 0.06 yalms to about 60 (16
// steps to 9 yalms never reached a tree's trunk past head height, let alone its crown); where a step is just
// behind what's drawn there (a crown, a trunk, a wall, a rock), the sun is blocked. Just behind: within 0.4
// yalms, 1% of the distance and 0.6 of the step's own length; further behind, the march passes behind the
// thing, not through it. A hit fades in and out at both ends of that window and the march's last 30% fades
// out, so no one step turns a shadow on or off outright. What's off screen can't cast.
// Characters and monsters never cast in the march: from here they cast a ghost on their far side (the march passing
// behind them) and came out dotted (above actor_shadow). So a step that lands on one of them doesn't count: the two
// whose outline on screen comes nearest the march's path are checked at every step. Their own shadows come from
// actor_shadow, when asked for (crowd.x rule bit 4: the game draws its own otherwise, a silhouette under the player
// and blobs under the rest, which is why it's off unless asked).
// The steps are a fixed run, each read on its own and the results put together after: no loop that waits on a
// read to decide whether to go on.
void sun_ps(float4 pos : SV_Position)
{
	int2 h = int2(pos.xy);
	int2 size = sun_mask_size();
	if (any(h >= size))
		return;
	int2 p = h * 2;
	float z = drawn_depth(p);
	float shadow = 0.0, whole = 0.0;
	float3 to_sun = sun_in_view();
	float3 P = view_at(p);
	float steep;
	float3 n = surface_normal(p, P, steep);
	// A character or monster here (inside one of their capsules, t2): its shadow is worked out once, from its
	// chest, as if facing the sun, and taken whole at kActorShare of its light (below, and shade_ps). The facing
	// built from the depth is a facet's on the game's low-poly models: under a canopy it split every face, limb
	// and knee pad down the middle into a shaded half and a lit one. (The march from the chest leaves out the
	// character's own body, as for any.)
	static const float kActorShare = 0.6;
	const float3 world_up = -up_row.xyz; // (the world's up in view space: its y is down)
	bool on_actor = false;
	float3 chest = P;
	uint self = 32u; // (which one: none)
	[unroll] for (uint k = 0; k < 32; ++k)
	{
		if (k < min(crowd.x & 0xFFFF, 32u))
		{
			body b = body_in_view(k);
			// (above its feet: the ground around them is ground, with its own shadow, the character's included)
			bool here = !on_actor && z < 1e8 && inside(b, P, world_up) && dot(P - b.feet, world_up) > 0.15;
			chest = here ? b.feet + world_up * (0.6 * b.height) : chest;
			self = here ? k : self;
			on_actor = on_actor || here;
		}
	}
	if (on_actor)
	{
		P = chest;
		n = to_sun;
		steep = 1.0;
	}
	float facing = dot(n, to_sun);
	const bool unsure = steep > 4.0; // (seen too nearly edge on for its facing to be told)
	float covered = 0.0, mapped = 0.0, above = 0.0;
	[branch] if (z < 1e8 && (facing > 0.0 || unsure) && ((crowd.x >> 16) & 8) != 0)
		mapped = sun_from_map(P, n, to_sun, covered, above);
	// (at the share a surface facing up would lose: not all of it. Ground seen at a grazing angle is "unsure" from
	// about 10 yalms off when the camera is low, so taken whole it was shaded twice as dark as the same ground nearer
	// at a low sun, along a line that moved with the camera)
	whole = unsure ? above * covered * sun_share(dot(world_up, to_sun)) : 0.0;
	// (any surface the sun reaches at all: what a shadow takes from it comes down to nothing as the sun grazes it)
	if (z < 1e8 && facing > 0.0)
	{
		static const float kGrow = 1.25;
		static const float kLast = 60.58; // (the 32nd step's t: 0.06 x 1.25^31)
		// (the start within the first step: each of a 4 x 4 block of these pixels its own sixteenth, which
		// shade_ps's 4 x 4 average puts together)
		static const float bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
		float t = 0.06 * pow(kGrow, (bayer[(h.y & 3) * 4 + (h.x & 3)] + 0.5) / 16.0);
		static const float kContact = 1.705; // (the 16th step's t: 0.06 x 1.25^15)
		// The march starts two pixel widths off the surface along its facing, and takes no hit nearer than two
		// pixel widths times how steeply the surface is seen: on a rough wall seen at an angle, the first steps
		// caught the wall's own bumps (dark slabs that slid about with the camera)
		const float width = pixel_width(z);
		const float3 from = P + n * (2.0 * width);
		const float t_min = 2.0 * width * clamp(steep, 1.0, 20.0);
		const bool contact = covered >= 1.0;
		const float last = contact ? kContact : kLast;
		const int steps = contact ? 16 : 32;

		// The two characters or monsters whose outline comes nearest the march's path on screen (from here to
		// its last step, or to where it'd reach the camera)
		float3 up = -up_row.xyz; // (the world's up in view space: its y is down)
		uint count = min(crowd.x & 0xFFFF, 32u);
		float w0 = P.z * up_row.w, dw = to_sun.z * up_row.w;
		float t_end = dw < 0.0 ? min(last, max(w0 - 0.3, 0.0) / -dw) : last;
		float2 a = on_actor ? screen_of(P) : float2(p) + 0.5, ab = screen_of(P + to_sun * t_end) - a;
		body b0 = (body)0, b1 = (body)0; // (radius 0: nothing is inside)
		float best0 = 0.0, best1 = 0.0;
		[unroll] for (uint i = 0; i < 32; ++i)
		{
			if (i < count)
			{
				body b = body_in_view(i);
				float3 centre = b.feet + up * (0.5 * b.height);
				float wc = centre.z * up_row.w;
				float2 c = screen_of(centre);
				float k = saturate(dot(c - a, ab) / max(dot(ab, ab), 1e-4));
				float apart = length(a + ab * k - c) - (0.5 * b.height + b.radius) * lin.z * 0.5 * ground.y / max(wc, 0.3);
				if (wc > 0.3 && apart < best0)
				{
					b1 = b0;
					best1 = best0;
					b0 = b;
					best0 = apart;
				}
				else if (wc > 0.3 && apart < best1)
				{
					b1 = b;
					best1 = apart;
				}
			}
		}

		const bool any_body = b0.radius > 0.0;

		// (the march ends where it leaves the screen, goes behind the camera, or crosses half the screen)
		int reach = int(ground.y / 2);
		bool going = true;
		[unroll] for (int s = 0; s < 32; ++s)
		{
			[branch] if (s >= steps)
				break;
			float3 S = from + to_sun * t;
			float w = S.z * up_row.w;
			float2 ndc = float2(S.x * lin.z, S.y * lin.w) / max(w, 0.2);
			int2 q = int2((ndc.x * 0.5 + 0.5) * ground.y, (0.5 - ndc.y * 0.5) * ground.z);
			going = going && w > 0.2 && all(abs(ndc) <= 1.0) && all(abs(q - p) <= reach);
			float drawn = drawn_depth(clamp(q, int2(0, 0), int2(ground.yz) - 1));
			float in_front = w - drawn;
			float bias = 0.05 + 0.005 * w, thick = 0.4 + 0.01 * w + 0.6 * t * (kGrow - 1.0);
			float hit = smoothstep(bias, 2.0 * bias, in_front) * (1.0 - smoothstep(0.7 * thick, thick, in_front)) * smoothstep(t_min, 1.5 * t_min, t);
			// (what's drawn there, put back in view space: one of the two, or not)
			bool on_body = false;
			[branch] if (any_body)
			{
				float3 there = float3(S.xy * drawn / max(w, 0.2), drawn * up_row.w);
				on_body = inside(b0, there, up) || inside(b1, there, up);
			}
			shadow = max(shadow, going && !on_body ? hit * (1.0 - smoothstep(0.7 * last, last, t)) : 0.0);
			t *= kGrow;
		}
		shadow = max(shadow, mapped * covered);
		// The characters' and monsters' own shadows, when asked for (actor_shadow): from where the march starts, past
		// the surface's own bumps; on someone, from their chest, all but their own
		[branch] if (((crowd.x >> 16) & 4) != 0 && count > 0)
			shadow = max(shadow, actor_shadow(from, to_sun, self, t_min));
	}
	if (on_actor)
	{
		whole = saturate(shadow) * kActorShare; // (taken whole: not by the facet's facing)
		shadow = 0.0;
	}
	sun_mask.Store((h.y * size.x + h.x) * 4, uint(saturate(shadow) * 255.0 + 0.5) | (uint(saturate(whole) * 255.0 + 0.5) << 8) |
		(f32tof16(min(z, 60000.0)) << 16));
}

// The blades' depth test against the world is here, on the copy of its depth (the game's depth buffer isn't
// drawn into; the blades have their own, for each other): as the game's own test would, LESS_EQUAL at the
// depth buffer's precision
// The water: the surface tools/bake_water.py rebuilt over the zone's water collision (in t0, two float4s a
// corner: the collision's x, y, z, kind 8 shallow / 9 deep (+100 steep), then the surface's y, how far the
// water's edge is in yalms, and its flow, x and z in yalms a second). The game's own water doesn't write
// depth: what's drawn under the surface is the bed, seen through it. Each pixel is worked out whole from the
// copies: the game's colour there (bent a little by the ripples, darker the deeper the water), the world
// reflected in it (marched along the depth copy; the sky where nothing on screen is hit) by how much water
// reflects at that angle, and the sun's glints. The game's own sea sheets past the zone's water (the sea: SEA1, the same
// corners, kind 200, its edge 8 yalms off: open water, no flow, its bed 30 under it) are drawn with the same shaders.
// The pass's parameters, in t2 (where the grass has its actors): [0] toward the sun (world), glint strength;
// [1] the sky's colour when none is on screen, reflection strength; [2] ripples, depth tint, seconds, the
// water's draw distance; [3] the sun's colour, w flags: 1 when [4] holds the zone's fog colour (the world's
// clear), 2 when there are rings (older add-ons send 0 and nothing past [3]), 4 when [6] and [7] hold the game's
// fog (its colour; where it starts and ends, yalms of view depth, and 1 / (end - start)); [5] how many rings, how
// strong; from [8], two each (see rings)
struct water_v2p
{
	float4 pos : SV_Position;
	float3 world : WORLD; // on the surface
	float edge : EDGE;    // yalms to the water's edge
	float2 flow : FLOW;   // yalms a second, across the ground
};

water_v2p water_vs(uint vid : SV_VertexID)
{
	// (SV_VertexID counts from 0 in every draw, whatever its start vertex: the run's first corner is in blade.w)
	uint corner = vid + asuint(blade.w);
	float4 c = blades[corner * 2], s = blades[corner * 2 + 1];
	water_v2p o;
	o.world = float3(c.x, s.x, c.z);
	o.pos = mul(float4(o.world, 1.0), view_proj);
	o.edge = s.y;
	o.flow = s.zw;
	return o;
}

// A well-mixed hash of a whole number (the rain's: every bit of it random)
uint rain_hash(uint x)
{
	x ^= x >> 16;
	x *= 0x7FEB352Du;
	x ^= x >> 15;
	x *= 0x846CA68Bu;
	x ^= x >> 16;
	return x;
}

// Smooth noise's slope at p (its value isn't needed)
float2 noise_slope(float2 p)
{
	int2 i = int2(floor(p));
	float2 f = p - floor(p);
	float2 u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);
	float a = hash2(i), b = hash2(i + int2(1, 0)), c = hash2(i + int2(0, 1)), d = hash2(i + int2(1, 1));
	float k = a - b - c + d;
	return du * float2(b - a + k * u.y, c - a + k * u.x);
}

// The ripples' slope (height up per yalm, x and z) at a point: four sizes, each two layers drifting apart
// (wind across still water); sizes under a few pixels left out (they'd only sparkle)
float2 ripple_field(float2 xz, float t, float yalms_per_px)
{
	static const float4 sizes[4] = { // wavelength (yalms), steepness, drift (yalms a second), turn (radians)
		float4(3.1, 0.060, 0.30, 0.0), float4(1.45, 0.045, 0.42, 1.9), float4(0.66, 0.030, 0.55, 3.7), float4(0.29, 0.020, 0.70, 5.3) };
	float2 slope = 0.0;
	[unroll] for (int k = 0; k < 4; ++k)
	{
		float4 s = sizes[k];
		float keep = saturate(s.x / (yalms_per_px * 8.0) - 0.5);
		if (keep <= 0.0)
			continue;
		float2 dir = float2(cos(s.w), sin(s.w));
		float2x2 turn = float2x2(dir.x, -dir.y, dir.y, dir.x);
		float2 p = mul(turn, xz) / s.x;
		float2 g = noise_slope(p + float2(t * s.z / s.x, 0.0)) + noise_slope(p * 1.31 + float2(17.3, -t * s.z * 0.8 / s.x));
		slope += mul(g, turn) * (s.y * keep); // (back to the world's axes: the transpose turns back)
	}
	return slope;
}

// The ripples where the water flows: the field carried downstream, two phases of it in turn so it never
// stretches (each fades out as it's put back). Two unrelated fields mixed half and half are flatter than
// either: divided by the mix's own size, or the ripples calm and swell twice a cycle (a pulse in the
// reflections).
float2 ripples(float2 xz, float2 flow, float t, float yalms_per_px)
{
	if (dot(flow, flow) < 1e-4)
		return ripple_field(xz, t, yalms_per_px);
	static const float kCycle = 2.0;
	float a = frac(t / kCycle), b = frac(t / kCycle + 0.5);
	float wa = 1.0 - abs(2.0 * a - 1.0), wb = 1.0 - abs(2.0 * b - 1.0);
	return (ripple_field(xz - flow * a * kCycle, t, yalms_per_px) * wa +
		ripple_field(xz - flow * b * kCycle + float2(31.7, 12.9), t, yalms_per_px) * wb) * rsqrt(wa * wa + wb * wb);
}

// Where a world point is on screen (pixel), and how far from the camera (view depth); false off screen or
// behind the camera
bool on_screen(float3 w, out int2 q, out float depth, out float2 ndc)
{
	float4 c = mul(float4(w, 1.0), view_proj);
	depth = c.w;
	ndc = c.xy / max(c.w, 1e-4);
	q = int2((ndc.x * 0.5 + 0.5) * ground.y, (0.5 - ndc.y * 0.5) * ground.z);
	return c.w > 0.2 && all(abs(ndc) < 1.0);
}

// The pixel at ndc, or at the screen's edge where ndc is past it (for a point ahead of the camera)
int2 edge_texel(float2 ndc)
{
	float2 e = clamp(ndc, -1.0, 1.0);
	return clamp(int2((e.x * 0.5 + 0.5) * ground.y, (0.5 - e.y * 0.5) * ground.z), int2(0, 0), int2(ground.yz) - 1);
}

// The sky the add-on guesses toward r when the screen doesn't show it: from the time of day; with the
// zone's fog colour (the world's clear, when the add-on sends it) along the horizon
float3 sky_guess(float3 r)
{
	float3 sky = actors[1].rgb;
	return (uint(actors[3].w + 0.5) & 1u) != 0 ? lerp(actors[4].rgb, sky, smoothstep(0.02, 0.4, -r.y)) : sky;
}

// What's far off toward r (past the reflections' march): what's drawn that way on screen, if it's the sky or
// well off in the distance (from there the camera and the water see the same thing); otherwise the guess.
// Blended in toward the screen's edges and from 80 to 160 yalms, so nothing switches outright as the
// camera turns. (It used to fall back on the sky along the top of the screen, one average for all the
// water: each time a treetop crossed one of its points, all of the water's sky changed at once.)
// Toward and just past the screen's sides, what's drawn at the side stands in: a guess faded in toward the sides
// made a band of the sky's colour down them, over water mirroring a cliff, that moved with the camera (in game,
// 2026-10-26).
float3 sky_toward(float3 r)
{
	float3 guess = sky_guess(r);
	int2 q;
	float d;
	float2 ndc;
	on_screen(eye.xyz + r * 4000.0, q, d, ndc);
	if (d <= 0.2 || abs(ndc.y) >= 1.0)
		return guess;
	q = edge_texel(ndc);
	float z = drawn_depth(q);
	// (only what's far off: nearer, the march would have found it. Faded toward the top and bottom, where what's
	// past the edge is unknown (looking down at water, its sky far above the screen's top); not toward the sides:
	// just past one, the edge's own pixel, fading to the guess by half a screen past)
	float far = z > 1e8 ? 1.0 : smoothstep(80.0, 160.0, z);
	float sides = 1.0 - smoothstep(1.0, 1.5, abs(ndc.x));
	return lerp(guess, texel(q), far * sides * saturate((1.0 - abs(ndc.y)) / 0.15));
}

// The rings spreading from characters and monsters moving through the water. From the add-on, two float4s
// each from [8]: where it started (x, the surface's y, z) and how strong (0..1); how old (seconds) and its
// first radius (yalms: bigger for bigger bodies). Each is a short train of waves a third of a yalm apart
// running out at 1.1 yalms a second, wider and lower as it goes, gone in about two and a half seconds; only
// on water at the height it started on (not a pool above or below). Returns the slope they add (height up
// a yalm, x and z); crest: how far the waves' tops lift the surface here (0..1).
float2 rings(float3 at, out float crest)
{
	crest = 0.0;
	float2 slope = 0.0;
	if ((uint(actors[3].w + 0.5) & 2u) == 0)
		return slope;
	uint count = min(uint(actors[5].x), 40u);
	float strength = actors[5].y;
	[unroll] for (uint k = 0; k < 40; ++k)
	{
		if (k < count)
		{
			float4 a = actors[8 + 2 * k], b = actors[9 + 2 * k];
			float2 d = at.xz - a.xz;
			float r = length(d);
			float front = b.y + 1.1 * b.x;
			float sigma = 0.16 + 0.22 * b.x;
			float x = r - front;
			if (abs(x) < 3.0 * sigma && abs(at.y - a.y) < 0.6 && r > 1e-3)
			{
				// (a wave a third of a yalm long under a bell around the front: its slope along the radius)
				float env = exp(-(x * x) / (sigma * sigma));
				float amp = a.w * strength * 0.026 * exp(-b.x / 0.8) * saturate(b.x / 0.12) * sqrt(b.y / front);
				float kx = 19.6 * x;
				slope += amp * env * (-19.6 * sin(kx) - 2.0 * x / (sigma * sigma) * cos(kx)) * d / r;
				crest = max(crest, env * saturate(cos(kx)) * amp / 0.026);
			}
		}
	}
	return slope;
}

// Rain on the water: rings where drops land, on two lattices of spots 0.7 yalms apart (the second offset), each spot
// a ring every 0.8 to 1.4 seconds at its own time, running out to about a third of a yalm and gone in half a second.
// How hard it rains (times the rings setting: the water's parameters, [5].z) is the share of the spots with drops.
// Returns the slope they add (height up a yalm, x and z).
float2 rain_rings(float2 xz, float t, float strength)
{
	float2 slope = 0.0;
	[branch] if (strength > 0.0)
	{
		[unroll] for (int g = 0; g < 2; ++g)
		{
			float2 p = xz / 0.7 + (g == 1 ? float2(0.5, 0.37) : float2(0.0, 0.0));
			int2 base = int2(floor(p));
			[unroll] for (int k = 0; k < 9; ++k)
			{
				int2 cell = base + int2(k % 3 - 1, k / 3 - 1);
				uint h = star_hash(uint(cell.x) * 73856093u ^ rain_hash(uint(cell.y) * 19349663u + uint(g) * 2654435761u));
				float4 r = float4(h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF, h >> 24) / 255.0;
				float there = r.x < saturate(strength * 0.8) ? 1.0 : 0.0;
				float period = 0.8 + 0.6 * r.y;
				float age = frac(t / period + r.z) * period;
				float2 centre = float2(cell) + 0.2 + 0.6 * float2(r.w, frac(r.w * 7.31 + r.y));
				float2 d = (p - centre) * 0.7;
				float dist = length(d);
				float x = dist - (0.02 + 0.6 * age);
				float sigma = 0.035 + 0.05 * age;
				float env = exp(-(x * x) / (sigma * sigma));
				float amp = 0.014 * exp(-age / 0.22) * there * min(strength, 1.5); // (slopes up to about half: the moving rings')
				slope += amp * env * (-40.0 * sin(40.0 * x)) * d / max(dist, 1e-3);
			}
		}
	}
	return slope;
}

float4 water_ps(water_v2p i) : SV_Target
{
	int2 p = int2(i.pos.xy);
	float w = i.pos.w;
	// Something in front of the surface here (a log, a rock, someone standing in it, the bank): it stays as
	// drawn. (More than a few hundredths of a yalm nearer; more far off, where depth is coarse.)
	float drawn = drawn_depth(p);
	if (drawn < w - (0.05 + 0.002 * w))
		discard;
	float4 sun = actors[0], sky = actors[1], knobs = actors[2], sun_colour = actors[3];
	float3 seen = texel(p);

	// How far the eye looks through the water here to what's drawn under it (along the view, at most 20
	// yalms), and how deep that is straight down
	float3 to_eye = eye.xyz - i.world;
	float range = length(to_eye);
	float3 v = to_eye / range; // toward the camera
	float under_depth = clamp(drawn - w, 0.0, 20.0) * range / max(w, 0.01);
	// (straight down from the whole of it, not those 20 yalms: far out, looked at along the surface, 20 yalms through is only
	// a few hundredths of a yalm deep, and the open sea faded out into the game's own toward the horizon)
	float deep = clamp(drawn - w, 0.0, 10000.0) * range / max(w, 0.01) * abs(v.y);

	// Fades: in from the water's edge over a yalm; in over the last few hundredths of a yalm of water around
	// what stands in it (a rock, a log, someone's legs: no hard edge, and nothing flickers there between
	// drawn and not as the depth's last bit changes); out toward the draw distance
	float along = distance(i.world.xz, eye.xz);
	float fade = saturate(i.edge / 1.0) * smoothstep(0.0, 0.08, deep) * saturate((knobs.w - along) / (0.15 * knobs.w));
	if (fade <= 0.0)
		discard;
	// In a zone with a sea (grass_render.cpp water_pipeline_for) the baked water is drawn in two parts, blade.z saying
	// which: 1 its pixels at their whole (the rest left: the sea is drawn where these aren't), 2 those it fades in at,
	// blended over what's there by then (the sea, or the game's own). 0 (no sea, and the sea itself): all of it.
	uint part = uint(blade.z + 0.5);
	if ((part == 1 && fade < 0.999) || (part == 2 && fade >= 0.999))
		discard;
	// (far out, past kNearWater yalms, no march toward the sun's glints or along the reflection: what's on screen that far
	// off is too thin to be found by either; their results faded out over the last fifth before it, so there's no line)
	static const float kNearWater = 250.0;
	float near_water = 1.0 - smoothstep(0.8 * kNearWater, kNearWater, w);

	// The surface's facing: up, tipped by the ripples (the world's y is down: up is -y)
	float yalms_per_px = w / (lin.z * 0.5 * ground.y);
	float crest;
	float2 slope = ripples(i.world.xz, i.flow, knobs.z, yalms_per_px) * knobs.x + rings(i.world, crest) + rain_rings(i.world.xz, knobs.z, actors[5].z);
	float3 n = normalize(float3(-slope.x, -1.0, -slope.y));
	float facing = saturate(dot(n, v));

	// Under the surface: the game's colour, shifted where the ripples bend the view, and murkier the more water
	// the eye looks through. The shift is what water does (the bed moves by about a quarter of the surface's
	// tilt times the depth), doubled so it shows, and at most 0.6% of the screen: it was 16 times that,
	// uncapped, and near the camera it smeared the bed into swirls.
	float px_per_yalm = 1.0 / yalms_per_px;
	float2 shift = clamp(slope * min(deep, 1.0) * 0.5 * px_per_yalm, -0.006 * ground.y, 0.006 * ground.y);
	// And the surface itself bends the view: what's under water looks nearer the surface than it is (its depth
	// over 1.33), so the colour shown here is what's drawn a third of its depth further down. A leg in the
	// water looks shortened below the waterline, the bed a little raised. (At most 3% of the screen.)
	float3 drawn_at = eye.xyz - v * (range * min(drawn, w + 20.0) / max(w, 0.01));
	float4 low = mul(float4(drawn_at + float3(0.0, 0.33 * min(deep, 3.0), 0.0), 1.0), view_proj); // (y is down)
	float2 lowered = low.w > 0.01 ? float2((low.x / low.w * 0.5 + 0.5) * ground.y, (0.5 - low.y / low.w * 0.5) * ground.z) - (float2(p) + 0.5) : 0.0;
	lowered = clamp(lowered, -0.03 * ground.y, 0.03 * ground.y);
	int2 bent = clamp(p + int2(shift + lowered), int2(0, 0), int2(ground.yz) - 1);
	float3 game_under = drawn_depth(bent) > w ? texel(bent) : seen;
	// (murk: the water's own colour, greenish and lit by the sky, which what's under it fades into a little
	// over a yalm of water looked through; it was a dark teal at a third of the rate, and a leg a few tenths of
	// a yalm under the surface came through as if through glass)
	float3 body = float3(0.08, 0.14, 0.12) * (0.3 + 1.4 * luma(sky.rgb));
	float through = exp(-1.0 * under_depth);
	float3 under = lerp(game_under, lerp(body, game_under, through), saturate(knobs.y));
	float kept = 1.0 - saturate(knobs.y) * (1.0 - through); // (how much of that is the game's colour)

	// Reflected: the ray off the surface marched along the depth copy, sixteen steps each 1.42 times the last
	// from 0.4 yalms (to some 80); the first step just behind what's drawn there is a hit, narrowed down with
	// five halvings between it and the step before. How much it's trusted fades where the ray, narrowed down,
	// still passes well behind what's drawn (behind a trunk, not into it), toward the screen's top and bottom (at its
// sides only over the last 1%: a wider fade there was a band down each side that followed the camera) and far
	// out, so a reflection fades in and out as the camera moves instead of switching. (No start turned per
	// pixel any more: a pattern fixed to the screen that the water slid under, a shimmer as the camera moved.)
	// Fixed runs, each read on its own (no loop that waits on a read to go on).
	float3 r = reflect(-v, n);
	r.y = min(r.y, -0.02); // (always up and away from the water)
	r = normalize(r);
	float t = 0.4, last = 0.0, lo = 0.0, hi = 0.0;
	bool going = true, found = false;
	[branch] if (near_water > 0.0)
	{
		[unroll] for (int k = 0; k < 16; ++k)
		{
			int2 q;
			float d;
			float2 ndc;
			going = going && on_screen(i.world + r * t, q, d, ndc);
			float behind = d - drawn_depth(clamp(q, int2(0, 0), int2(ground.yz) - 1));
			bool now = going && !found && behind > 0.0 && behind < max(0.5 + 0.02 * d, t * 0.5);
			lo = now ? last : lo;
			hi = now ? t : hi;
			found = found || now;
			last = t;
			t *= 1.42;
		}
	}
	float3 mirrored = sky_toward(r);
	[branch] if (found)
	{
		[unroll] for (int b = 0; b < 5; ++b)
		{
			float mid = 0.5 * (lo + hi);
			int2 q;
			float d;
			float2 ndc;
			bool beyond = on_screen(i.world + r * mid, q, d, ndc) && d > drawn_depth(clamp(q, int2(0, 0), int2(ground.yz) - 1));
			lo = beyond ? lo : mid;
			hi = beyond ? mid : hi;
		}
		int2 q;
		float d;
		float2 ndc;
		on_screen(i.world + r * hi, q, d, ndc);
		q = clamp(q, int2(0, 0), int2(ground.yz) - 1);
		float thick = 0.5 + 0.02 * d;
		float trust = (1.0 - smoothstep(0.5 * thick, thick, d - drawn_depth(q))) *
			saturate((1.0 - abs(ndc.y)) / 0.1) * saturate((1.0 - abs(ndc.x)) / 0.02) * (1.0 - smoothstep(40.0, 80.0, hi)) * near_water;
		mirrored = lerp(mirrored, texel(q), trust);
	}
	// (how much water reflects at this angle: 2% looking straight down, all of it along the surface)
	float fresnel = saturate((0.02 + 0.98 * pow(1.0 - facing, 5.0)) * sky.w);

	// The sun's glints: where the rippled surface mirrors the sun, unless something on screen stands between
	// (twelve steps toward the sun, only there; each step's hit fades in and out at both ends of its window,
	// so a glint near a shadow's edge doesn't blink)
	float3 h = normalize(v + sun.xyz);
	float glint = sun.w > 0.0 ? pow(saturate(dot(n, h)), 500.0) * 25.0 * sun.w : 0.0;
	[branch] if (glint > 0.002 && near_water > 0.0)
	{
		float s = 0.15, shade = 0.0;
		[unroll] for (int j = 0; j < 12; ++j)
		{
			int2 q;
			float d;
			float2 ndc;
			bool in_view = on_screen(i.world + sun.xyz * s, q, d, ndc);
			float behind = d - drawn_depth(clamp(q, int2(0, 0), int2(ground.yz) - 1));
			float bias = 0.1 + 0.005 * d, thick = 0.5 + 0.02 * d + s * 0.45;
			shade = max(shade, in_view ? smoothstep(bias, 2.0 * bias, behind) * (1.0 - smoothstep(0.7 * thick, thick, behind)) : 0.0);
			s *= 1.45;
		}
		glint *= 1.0 - shade * near_water;
	}

	// (the rings' tops catch a little more light, so they show where little is reflected: near, from above)
	float3 water = (lerp(under, mirrored, fresnel) + sun_colour.rgb * glint) * (1.0 + 0.18 * crest);
	// The game's fog over the water, as over the scenery around it (flag 4). What's seen through the surface is the
	// game's own colour, fogged already (at the bed's depth, about the surface's): that part isn't fogged twice.
	if ((uint(sun_colour.w + 0.5) & 4u) != 0)
	{
		float3 fog = actors[6].rgb;
		float4 h = actors[7];
		float clear = saturate((h.y - w) * h.z);
		water = lerp(fog, water, clear) + (1.0 - clear) * (1.0 - fresnel) * kept * (game_under - fog);
	}
	return part == 2 ? float4(water, fade) : float4(lerp(seen, water, fade), 1.0);
}

// The water view (a debug overlay): the zone's water collision itself (not the surface), each spot tinted
// by whether what the game drew there is at it or behind it (blue deep, cyan shallow, yellow steep) or in
// front of it (red: a log, a rock, a bank, someone)
struct water_debug_v2p
{
	float4 pos : SV_Position;
	float kind : KIND;
};

water_debug_v2p water_debug_vs(uint vid : SV_VertexID)
{
	float4 c = blades[(vid + asuint(blade.w)) * 2]; // (the run's first corner in blade.w, as in water_vs)
	water_debug_v2p o;
	o.pos = mul(float4(c.xyz, 1.0), view_proj);
	o.kind = c.w;
	return o;
}

float4 water_debug_ps(water_debug_v2p i) : SV_Target
{
	// (in front: more than a few hundredths of a yalm nearer than the water, more far off, where depth is coarse)
	float drawn = drawn_depth(int2(i.pos.xy));
	bool hidden = drawn < i.pos.w - (0.05 + 0.002 * i.pos.w);
	float3 tint = hidden ? float3(1.0, 0.15, 0.1) : i.kind > 99.5 ? float3(1.0, 0.85, 0.1) : i.kind > 8.5 ? float3(0.15, 0.35, 1.0) : float3(0.2, 0.95, 0.9);
	return float4(tint, hidden ? 0.35 : 0.45);
}

float4 ps_main(v2p i) : SV_Target
{
	int2 p = clamp(int2(i.pos.xy) * int2(zinfo.yz) / int2(ground.yz), int2(0, 0), int2(zinfo.yz) - 1);
	uint v = zbuf.Load((p.y * zinfo.x + p.x) * 4);
	float d = zinfo.w == 1 ? (v & 0xFFFFFF) / 16777215.0 : asfloat(v);
	if (i.pos.z > d + (zinfo.w == 1 ? 1.0 / 16777215.0 : 0.0))
		discard;
	return float4(i.col, 1.0);
}

// The sun's shadow map is drawn from the zone's own meshes (tools/bake_shadow_geo.py): t0 their triangles, three
// float4s each (a corner's x, y, z, and w: 0 on a solid triangle, else u | v << 12 | mask << 24, 12-bit texture
// coordinates and the cut-out texture's index + 1), view_proj taking the world to the cascade's clip space (x, y
// across the sun's rays, z from the sun); t1 the cut-out textures' alpha as bits (4 words a texture: width,
// height, its first word counted from the buffer's start, words a row; a bit a texel, 1 opaque); blade.w the
// draw's first corner (SV_VertexID counts from 0 in each draw, whatever its start vertex). Depth only.
struct shadow_v2p
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	nointerpolation uint mask : TEXCOORD1;
};

shadow_v2p shadow_vs(uint vid : SV_VertexID)
{
	float4 c = blades[vid + asuint(blade.w)];
	uint w = asuint(c.w);
	shadow_v2p o;
	o.pos = mul(float4(c.xyz, 1.0), view_proj);
	o.uv = float2(w & 0xFFF, (w >> 12) & 0xFFF) / 4095.0;
	o.mask = w >> 24;
	return o;
}

// kSunMapCutout: 0 tests a cut-out's mask texel by texel (the nearest bit: square texels, which a 4096 map shows
// as blocky dapples under trees); over 0, the 4 bits around weighted bilinearly (texel centres at (t + 0.5) / size,
// clamped at the edge) and drawn where that's over it: outlines as round as the game's own leaves. 0.5 lets as
// much sun through a canopy as texel by texel; 0.1875 is the game's own test (4 x alpha over 0x60, the leaves'
// alpha 0 or about 0x80: within 1 % of its drawn area, where texel by texel misses 2 to 8 %, the rims), but
// layered leaves then let through about half the sun spots. At most 0.07 ms more a near-cascade redraw at 4096.
// kSunMapLeafArea (on, with kSunMapCutout over 0): where the mask's texels are finer than the shadow map's on the card
// (a leaf texture's bits a centimetre apart, the near cascade's texels 4 cm), the bits over the map texel's own
// footprint (its uv extent, ddx/ddy) are counted, up to 4 x 4 of them, and it's drawn where they cover over
// kSunMapCutout of it. A single bit, as before, gave a canopy's dapples a new pattern each time the map was drawn again
// (the sun turning: about ten times a second, each time on a slightly different grid): under trees they flickered in
// place (in game, 2026-10-21). Where the mask is as coarse as the map, or coarser, the one bilinear test above.
static const float kSunMapCutout = 0.5;
static const bool kSunMapLeafArea = true;

uint mask_bit(uint4 m, int2 t)
{
	uint2 c = uint2(clamp(t, 0, int2(m.xy) - 1));
	return (world.Load((m.z + c.y * m.w + (c.x >> 5)) * 4) >> (c.x & 31)) & 1;
}

void shadow_ps(shadow_v2p i)
{
	[branch] if (i.mask != 0) // (a real branch: t1 holds no table for a solid triangle's mask 0, and a root
	{                         // view has no bounds to keep a read past its end harmless)
		uint4 m = world.Load4((i.mask - 1) * 16);
		// (the map texel's footprint on the mask, in its texels)
		float2 dx = ddx(i.uv) * float2(m.xy), dy = ddy(i.uv) * float2(m.xy);
		float across = max(length(dx), length(dy));
		if (kSunMapCutout > 0.0 && kSunMapLeafArea && across > 1.5)
		{
			uint n = uint(clamp(ceil(across), 2.0, 4.0));
			float2 at = saturate(i.uv) * float2(m.xy);
			uint covered = 0;
			[loop] for (uint b = 0; b < n; ++b)
				[loop] for (uint a = 0; a < n; ++a)
				{
					float2 s = at + ((a + 0.5) / n - 0.5) * dx + ((b + 0.5) / n - 0.5) * dy;
					covered += mask_bit(m, int2(floor(s)));
				}
			if (covered <= kSunMapCutout * float(n * n))
				discard;
		}
		else if (kSunMapCutout > 0.0)
		{
			float2 f = saturate(i.uv) * float2(m.xy) - 0.5;
			float2 w = f - floor(f);
			int2 a = int2(floor(f));
			float4 bit = float4(mask_bit(m, a), mask_bit(m, a + int2(1, 0)), mask_bit(m, a + int2(0, 1)), mask_bit(m, a + int2(1, 1)));
			if (lerp(lerp(bit.x, bit.y, w.x), lerp(bit.z, bit.w, w.x), w.y) <= kSunMapCutout)
				discard;
		}
		else
		{
			uint2 t = min(uint2(saturate(i.uv) * float2(m.xy)), m.xy - 1);
			if (((world.Load((m.z + t.y * m.w + (t.x >> 5)) * 4) >> (t.x & 31)) & 1) == 0)
				discard;
		}
	}
}

// Fireflies at night, over the zone's grass: a few of the baked grass points (every blade.z-th from the draw's
// first, blade.w) are each a firefly's home, in patches (smooth noise a dozen yalms across leaves some ground
// empty). Each drifts slowly around its home, half a yalm to a yalm and a half above the ground, flashes for a
// second and a half or so every few seconds and is dark between, and drifts away from anyone walking through (the actors in
// t2). A glowing sprite facing the camera, added to the world; hidden by what the game drew in front of it (the
// depth copy) and by the blades (their depth buffer, read only). The shading constants are this pass's own:
// shade.x how bright (how deep into the night; the panel's amount sets blade.z, how many there are), y the glow's radius (yalms), z how far
// they're drawn (yalms).
struct firefly_v2p
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;               // across the sprite, -1..1
	nointerpolation float glow : GLOW;   // its brightness this frame
};

firefly_v2p firefly_vs(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	firefly_v2p o;
	o.uv = 0.0;
	o.glow = 0.0;
	o.pos = float4(0.0, 0.0, -1.0, 1.0); // (in front of the near plane: clipped)
	uint index = asuint(blade.w) + iid * max(1u, uint(blade.z));
	float4 b = blades[index];
	float s = frac(b.w * 7.13 + index * 0.6180339);
	float r1 = hash(s * 11.3 + 0.17), r2 = hash(s * 5.71 + 0.53), r3 = hash(s * 3.37 + 0.91), r4 = hash(s * 17.9 + 0.29);
	// In patches: where the noise is low, none
	float patch = noise2(b.xz / 12.0);
	if (r1 > saturate(patch * 1.6 - 0.3))
		return o;

	// Flashes: a second and a half or so lit in every 3.5 to 6.5 seconds, each at its own time
	float t = color.w;
	float period = 3.5 + 3.0 * r2;
	float phase = frac(t / period + r3);
	float lit = phase < 0.4 ? sin(3.14159265 * phase / 0.4) : 0.0;
	lit *= lit;
	if (lit <= 0.002)
		return o;

	// Drifting around its home, slowly, and higher or lower over time
	float3 p = b.xyz;
	p.x += 1.1 * sin(t * (0.21 + 0.12 * r2) + r3 * 6.28) + 0.35 * sin(t * (0.67 + 0.2 * r4) + r1 * 6.28);
	p.z += 1.1 * cos(t * (0.19 + 0.13 * r4) + r2 * 6.28) + 0.35 * cos(t * (0.71 + 0.2 * r3) + r4 * 6.28);
	p.y -= 0.5 + 0.9 * r4 + 0.25 * sin(t * (0.4 + 0.3 * r1) + r2 * 6.28); // (y is down)
	// Away from anyone near: within 2.5 yalms, harder the nearer, and up a little
	for (uint i = 0; i < (crowd.x & 0xFFFF); ++i)
	{
		float4 ac = actors[i];
		float2 d = p.xz - ac.xz;
		float dist = length(d);
		float reach = 2.0 + ac.w;
		if (dist < reach && abs(p.y - ac.y) < 4.0)
		{
			float k = 1.0 - dist / reach;
			p.xz += (dist > 0.001 ? d / dist : float2(1.0, 0.0)) * k * 1.5;
			p.y -= k * 0.6;
		}
	}

	float far = distance(p, eye.xyz);
	float fade = saturate((shade.z - far) / (0.3 * shade.z)) * saturate((far - 0.6) / 0.6);
	o.glow = shade.x * lit * fade;
	if (o.glow <= 0.002)
		return o;

	// The sprite: two triangles facing the camera, at least 3 of the world's pixels across, dimmer when it had
	// to be made bigger than it is (a far one stays a point of light rather than a blot)
	float4 c = mul(float4(p, 1.0), view_proj);
	// (their light seen through the game's fog, as anything that far off)
	[branch] if (hazy())
		o.glow *= clear_at(c.w);
	if (c.w <= 0.1 || o.glow <= 0.002)
	{
		o.glow = 0.0;
		return o;
	}
	static const float2 corner[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };
	float2 uv = corner[vid % 6];
	float radius = shade.y * (0.8 + 0.4 * r3);
	float least = 3.0 * c.w / (lin.z * ground.y); // (yalms: 1.5 world pixels at this depth)
	if (radius < least)
	{
		o.glow *= max(radius / least, 0.35);
		radius = least;
	}
	c.xy += uv * radius * float2(lin.z, lin.w);
	o.pos = c;
	o.uv = uv;
	return o;
}

float4 firefly_ps(firefly_v2p i) : SV_Target
{
	// Behind what the game drew there: hidden
	int2 q = clamp(int2(i.pos.xy) * int2(zinfo.yz) / int2(ground.yz), int2(0, 0), int2(zinfo.yz) - 1);
	uint v = zbuf.Load((q.y * zinfo.x + q.x) * 4);
	float d = zinfo.w == 1 ? (v & 0xFFFFFF) / 16777215.0 : asfloat(v);
	if (i.pos.z > d + (zinfo.w == 1 ? 1.0 / 16777215.0 : 0.0))
		discard;
	// A small hot core in a soft yellow-green glow, nothing at the sprite's edge
	float r2 = dot(i.uv, i.uv);
	if (r2 >= 1.0)
		discard;
	float edge = 1.0 - r2;
	float core = exp(-r2 * 45.0);
	float halo = exp(-r2 * 5.0) * edge * edge;
	float3 light = float3(1.0, 0.95, 0.6) * core * 1.4 + float3(0.55, 0.85, 0.2) * halo * 0.45;
	return float4(light * i.glow, 0.0);
}

// Rain (rain_vs, rain_ps) and its splashes (splash_vs, splash_ps). Drops fall in columns: points shade.z yalms apart
// across the ground, slanted by the wind (a drop at height y is at its column's point plus blade.yz times y: yalms
// across x and z for each yalm fallen), so they stay put in the world as the camera moves and turns. Each column's
// drop comes round every kRainPeriod yalms of fall, at its own speed (about blade.x yalms a second) and time; of its
// copies the one in the volume from kRainAbove yalms over the camera to kRainPeriod below that is drawn, a short
// streak along its fall. How hard it rains (shade.x) is the share of the columns with a drop (kRainLayers of them).
// A drop ends at the first surface under it: the zone's rain map, its window around the camera in t0 (kRainWindow
// squares a side, from the corner crowd.yz, crowd.w yalms each; a uint a square: the surface's y as float bits, what
// it is in the low 2 bits: 0 nothing (the drop falls on through), 1 something that stops rain with no splash (steep,
// leaves), 2 ground facing up, 3 the add-on's water), or the head and shoulders of someone near (t2: the actors). A
// splash shows near the camera where and when each column's drop ends (shade.y the share of them), on ground, water
// and people: a crown of droplets, and a ring (a wide one on water). Hidden by what's in front: the depth copy (t3)
// and the blades (their depth buffer, read only). eye.w the radius drawn in (yalms, across the ground), blade.w the
// columns a side (uint), color.rgb the light the drops catch, color.w seconds, shade.w how bright.
static const uint kRainWindow = 256;
static const float kRainPeriod = 40.0;
static const float kRainAbove = 22.0;

// What the rain map says over xz (a square's uint; 0, nothing, off its window)
uint rain_cell(float2 xz)
{
	float2 corner = float2(asfloat(crowd.y), asfloat(crowd.z));
	int2 i = int2(floor((xz - corner) / asfloat(crowd.w)));
	bool in_window = all(i >= 0) && all(i < int(kRainWindow));
	i = clamp(i, int2(0, 0), int2(kRainWindow - 1, kRainWindow - 1)); // (a root view has no bounds: never read past it)
	uint k = uint(i.y) * kRainWindow + uint(i.x);
	float4 four = blades[k >> 2];
	uint c = asuint(four[k & 3u]);
	return in_window ? c : 0u;
}

// The height of a square's surface (y, down positive), or far below when there's none
float rain_top(uint c)
{
	return (c & 3u) == 0u ? 1e9 : asfloat(c & ~3u);
}

// The top of someone near over xz (t2: their head and shoulders, a dome 0.45 times their size across, its top twice
// their size over their feet), or far below when no one is there. A fixed run over all the actors' places (those past
// crowd.x's count left out after the read: the buffer always holds that many).
float person_top(float2 xz)
{
	float top = 1e9;
	uint count = crowd.x & 0xFFFF;
	[unroll] for (uint i = 0; i < 32; ++i)
	{
		float4 a = actors[i];
		float r = 0.45 * a.w;
		float2 d = xz - a.xz;
		float dd = dot(d, d);
		float y = a.y - 2.0 * a.w + r - sqrt(max(r * r - dd, 0.0));
		top = (i < count && dd < r * r) ? min(top, y) : top;
	}
	return top;
}

// Four random numbers (0..1) of a column's
float4 rain_random(int2 cell, uint layer, uint salt)
{
	uint h = star_hash(uint(cell.x) * 73856093u ^ rain_hash(uint(cell.y) * 19349663u ^ (layer * 83492791u + salt)));
	uint h2 = rain_hash(h + 0x9E3779B9u);
	return float4(h & 0xFFFF, h >> 16, h2 & 0xFFFF, h2 >> 16) / 65535.0;
}

struct rain_drop
{
	bool falls;   // the column has a drop now (how hard it rains)
	float2 u;     // its point: where it is at y = 0 (x, z)
	float speed;  // how fast it falls (yalms a second)
	float phase;  // how far it had fallen at time 0 (yalms, within a period)
	float4 r;     // random numbers of its own
};

// The column an instance draws: the columns a side (blade.w) around the one through the camera at the height `level` (y,
// down positive: the camera's own for the rain and snow, the ground's under it for the leaves, blown far across), a
// layer after another
rain_drop rain_column_at(uint iid, float level)
{
	uint n = asuint(blade.w);
	uint per = n * n;
	uint layer = iid / per;
	uint k = iid - layer * per;
	float spacing = shade.z;
	float2 at_eye = eye.xz - blade.yz * level; // (the camera's point in the slanted columns)
	int2 cell = int2(floor(at_eye / spacing)) - int(n / 2) + int2(k % n, k / n);
	float4 r = rain_random(cell, layer, 0u);
	rain_drop d;
	d.falls = r.x < shade.x;
	d.u = (float2(cell) + r.yz) * spacing;
	d.speed = blade.x * (0.85 + 0.3 * r.w);
	d.r = rain_random(cell, layer, 1013904223u);
	d.phase = d.r.x * kRainPeriod;
	return d;
}

rain_drop rain_column(uint iid)
{
	return rain_column_at(iid, eye.y);
}

// Behind what the game drew there (the depth copy)
bool behind_drawn(float4 pos)
{
	int2 q = clamp(int2(pos.xy) * int2(zinfo.yz) / int2(ground.yz), int2(0, 0), int2(zinfo.yz) - 1);
	uint v = zbuf.Load((q.y * zinfo.x + q.x) * 4);
	float d = zinfo.w == 1 ? (v & 0xFFFFFF) / 16777215.0 : asfloat(v);
	return pos.z > d + (zinfo.w == 1 ? 1.0 / 16777215.0 : 0.0);
}

struct rain_v2p
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;                 // x: 0 at its head .. 1 at its tail, y: across, -1..1
	nointerpolation float alpha : ALPHA;
};

rain_v2p rain_vs(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	rain_v2p o;
	o.pos = float4(0.0, 0.0, -1.0, 1.0); // (in front of the near plane: clipped)
	o.uv = 0.0;
	o.alpha = 0.0;
	rain_drop d = rain_column(iid);
	if (!d.falls)
		return o;
	// The copy of its drop in the volume drawn
	float top = eye.y - kRainAbove;
	float fallen = color.w * d.speed + d.phase;
	float y = fallen - kRainPeriod * floor(fallen / kRainPeriod);
	y += kRainPeriod * ceil((top - y) / kRainPeriod);
	float3 slant = float3(blade.y, 1.0, blade.z);
	float len = (0.45 + 0.4 * d.r.y) * d.speed / blade.x; // (yalms of fall the streak spans)
	float3 head = float3(d.u.x + blade.y * y, y, d.u.y + blade.z * y);
	float3 tail = head - slant * len;
	// It ends at the surface under it: gone once it's all past it, only the part above it while it gets there
	float under = min(rain_top(rain_cell(head.xz)), person_top(head.xz));
	if (tail.y >= under)
		return o;
	if (head.y > under)
		head = tail + slant * (under - tail.y);
	// Faded in at the volume's top, out at its bottom and toward the radius drawn in
	float fade = saturate((tail.y - top) / 3.0) * saturate((top + kRainPeriod - head.y) / 3.0);
	fade *= saturate((eye.w - distance(head.xz, eye.xz)) / (0.35 * eye.w));
	float4 ch = mul(float4(head, 1.0), view_proj), ct = mul(float4(tail, 1.0), view_proj);
	float nearest = min(ch.w, ct.w);
	if (nearest < 0.5)
		return o;
	fade *= saturate((nearest - 0.5) / 2.0); // (one right by the lens would be a bright bar across the screen)
	// (seen through the game's fog, as anything that far off)
	[branch] if (hazy())
		fade *= clear_at(ch.w);
	// A thin streak on screen from its head to its tail: 0.012 yalms wide, at least 1.25 pixels (fainter when it had
	// to be drawn wider than it is), half a pixel past each end; fainter too when it's wider than 3 pixels (near the
	// camera a drop is out of focus, not a glass rod)
	float2 half_px = float2(ground.yz) * 0.5;
	float2 sh = ch.xy / ch.w * half_px, st = ct.xy / ct.w * half_px;
	float2 along = sh - st;
	float lp = length(along);
	along = lp > 1e-3 ? along / lp : float2(0.0, 1.0);
	float2 side = float2(-along.y, along.x);
	float width = 0.012 * lin.z * half_px.x / ch.w;
	float shown = max(width, 1.25);
	o.alpha = shade.w * fade * (0.26 + 0.14 * d.r.z) * (width / shown) * min(1.0, 3.0 / shown);
	if (o.alpha <= 0.002)
		return o;
	static const float2 corner[6] = { float2(0, -1), float2(0, 1), float2(1, -1), float2(1, -1), float2(0, 1), float2(1, 1) };
	float2 cn = corner[vid % 6];
	float4 c = cn.x < 0.5 ? ch : ct;
	float2 offset = side * (cn.y * shown * 0.5) + along * (cn.x < 0.5 ? 0.5 : -0.5);
	c.xy += offset / half_px * c.w;
	o.pos = c;
	o.uv = cn;
	return o;
}

float4 rain_ps(rain_v2p i) : SV_Target
{
	if (behind_drawn(i.pos))
		discard;
	// Softer at its sides, fainter toward its tail
	float a = i.alpha * (1.0 - i.uv.y * i.uv.y) * lerp(1.0, 0.3, i.uv.x);
	return float4(color.rgb, a);
}

struct splash_v2p
{
	float4 pos : SV_Position;
	float2 at : TEXCOORD0;                   // on the sprite (yalms): the crown's across and up; the ring's x, z
	nointerpolation float4 info : TEXCOORD1; // x: how old (seconds), y: how strong, z: 1 the ring, w: 1 on water
	nointerpolation float seed : SEED;       // its own random number (how its droplets fly)
};

splash_v2p splash_vs(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	splash_v2p o;
	o.pos = float4(0.0, 0.0, -1.0, 1.0);
	o.at = 0.0;
	o.info = 0.0;
	o.seed = 0.0;
	rain_drop d = rain_column(iid);
	if (!d.falls || d.r.w >= 0.5 * shade.y)
		return o;
	// Where its drops end: the surface under its slanted path, found in three steps from the camera's height
	float y = eye.y + 1.5;
	uint c = 0u;
	bool person = false;
	[unroll] for (int k = 0; k < 3; ++k)
	{
		float2 xz = d.u + blade.yz * y;
		c = rain_cell(xz);
		float someone = person_top(xz);
		person = someone < rain_top(c);
		y = min(rain_top(c), someone);
	}
	uint kind = person ? 2u : (c & 3u);
	float top = eye.y - kRainAbove;
	if (kind < 2u || y < top || y > top + kRainPeriod) // (nothing there, leaves or a wall; or where no drop drawn gets)
		return o;
	// When it last got there
	float since = color.w * d.speed + d.phase - y;
	float age = (since - kRainPeriod * floor(since / kRainPeriod)) / d.speed;
	bool water = kind == 3u;
	float life = water ? 0.6 : 0.3;
	if (age >= life)
		return o;
	float3 at = float3(d.u.x + blade.y * y, y, d.u.y + blade.z * y);
	float far = distance(at, eye.xyz);
	float fade = saturate((eye.w - far) / (0.3 * eye.w)) * saturate((far - 0.5) / 0.5);
	float4 cc = mul(float4(at, 1.0), view_proj);
	[branch] if (hazy())
		fade *= clear_at(cc.w);
	if (fade <= 0.002 || cc.w < 0.3)
		return o;
	float size = 0.8 + 0.4 * d.r.y;
	bool ring = vid >= 6;
	static const float2 corner[6] = { float2(-1, 0), float2(1, 0), float2(-1, 1), float2(-1, 1), float2(1, 0), float2(1, 1) };
	float2 q = corner[vid % 6];
	float3 p;
	if (ring)
	{
		// Lying on the surface (a hair over it)
		float reach = (water ? 0.45 : 0.22) * size;
		float2 xz = float2(q.x, q.y * 2.0 - 1.0) * reach;
		p = at + float3(xz.x, -0.004, xz.y);
		o.at = xz / size;
	}
	else
	{
		// Upright, turned to face the camera, its foot a little into the surface
		float2 toward = eye.xz - at.xz;
		float2 h = dot(toward, toward) > 1e-6 ? normalize(toward) : float2(0.0, 1.0);
		float3 right = float3(-h.y, 0.0, h.x);
		float2 local = float2(q.x * 0.2, q.y * 0.22 - 0.02) * size;
		p = at + right * local.x - float3(0.0, local.y, 0.0); // (y is down)
		o.at = local / size;
	}
	o.pos = mul(float4(p, 1.0), view_proj);
	o.info = float4(age, fade * shade.w, ring ? 1.0 : 0.0, water ? 1.0 : 0.0);
	o.seed = frac(d.r.z * 13.7 + d.r.w * 3.1);
	return o;
}

float4 splash_ps(splash_v2p i) : SV_Target
{
	// Hidden by what's in front of it (with a little leeway: the crown stands on what's drawn, the ring lies on it)
	float drawn = drawn_depth(int2(i.pos.xy));
	if (i.pos.w > drawn + 0.04 + 0.004 * drawn)
		discard;
	float t = i.info.x;
	bool water = i.info.w > 0.5;
	float life = water ? 0.6 : 0.3;
	float a = 0.0;
	if (i.info.z > 0.5)
	{
		// A thin ring running out and fading: on water wide and clear, on ground a quick small one
		float r = length(i.at);
		float front = 0.02 + (water ? 0.7 : 0.65) * t;
		float thick = 0.012 + 0.03 * t;
		float x = (r - front) / thick;
		a = exp(-x * x) * (1.0 - t / life) * (1.0 - t / life) * (water ? 0.9 : 0.25);
	}
	else
	{
		// A crown: a few droplets thrown up and out, each its own way (from the splash's number), falling back (11
		// yalms a second squared), smaller and fainter as they go; and a moment's bright spot where the drop hit
		float tt = min(t, 0.3);
		[unroll] for (int k = 0; k < 5; ++k)
		{
			float h1 = frac(i.seed * 7.13 + k * 0.618034), h2 = frac(i.seed * 3.71 + k * 0.414214);
			float sideways = (h1 * 2.0 - 1.0) * 0.6;
			float upward = 0.8 + 0.8 * h2;
			float2 p = float2(sideways * tt, upward * tt - 5.5 * tt * tt);
			float radius = lerp(0.02, 0.008, tt / 0.3) * (0.7 + 0.6 * h2);
			a = max(a, smoothstep(radius, radius * 0.35, length(i.at - p)));
		}
		a = max(a, smoothstep(0.05, 0.0, length(i.at * float2(1.0, 3.0))) * saturate(1.0 - tt / 0.07));
		a *= saturate(1.0 - tt / 0.3) * (water ? 0.6 : 1.0);
	}
	return float4(color.rgb * 1.15, a * i.info.y * 0.7);
}

// Snow (snow_vs, snow_ps): flakes in the rain's columns (rain_column: the same slanted, world-anchored columns, its
// constants as the rain's), drifting down slowly (blade.x yalms a second, each flake 0.7 to 1.4 times that) and
// swaying on their way (shade.y yalms at most, each flake its own slow sway, a whole number of them an hour as the
// clock wraps hourly), soft round sprites facing the camera. A flake ends at the first surface under it as a drop
// does (the rain map, the tops of people near), and also where its slanted path came down through something (the map
// looked at 1.5 and 3 yalms back up its path: the lee of a roof in a blizzard stays clear); it shrinks into what it
// lands on, no splash. Fogged, hidden by what's in front as the rain. eye.w the radius drawn in, color.rgb the light
// the flakes catch, shade.x how hard it snows (the share of the columns with a flake), shade.w how bright.
struct snow_v2p
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;                 // on the flake, -1..1 across and up
	nointerpolation float alpha : ALPHA;
};

snow_v2p snow_vs(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	snow_v2p o;
	o.pos = float4(0.0, 0.0, -1.0, 1.0); // (in front of the near plane: clipped)
	o.uv = 0.0;
	o.alpha = 0.0;
	rain_drop d = rain_column(iid);
	if (!d.falls)
		return o;
	// The copy of its flake in the volume drawn (as a drop's, slower)
	float speed = d.speed * (0.8 + 0.4 * d.r.w);
	float top = eye.y - kRainAbove;
	float fallen = color.w * speed + d.phase;
	float y = fallen - kRainPeriod * floor(fallen / kRainPeriod);
	y += kRainPeriod * ceil((top - y) / kRainPeriod);
	// Its sway: across x and z, each its own number of turns an hour (periods of 2 to 4 seconds)
	float2 turns = floor(float2(900.0, 950.0) + float2(900.0, 850.0) * d.r.yz);
	float2 sway = shade.y * (0.6 + 0.4 * d.r.w) * sin(6.2831853 * (frac(color.w * turns / 3600.0) + d.r.zy));
	float3 at = float3(d.u.x + blade.y * y + sway.x, y, d.u.y + blade.z * y + sway.y);
	// It ends at the surface under it, or where its path came down through something
	float under = min(rain_top(rain_cell(at.xz)), person_top(at.xz));
	if (y >= under)
		return o;
	bool blocked = false;
	[unroll] for (int k = 1; k <= 2; ++k)
	{
		float back = 1.5 * k;
		blocked = blocked || rain_top(rain_cell(at.xz - blade.yz * back)) < y - back;
	}
	if (blocked)
		return o;
	// Faded in at the volume's top, out at its bottom and toward the radius drawn in; shrinking into what it lands on
	float fade = saturate((y - top) / 3.0) * saturate((top + kRainPeriod - y) / 3.0);
	fade *= saturate((eye.w - distance(at.xz, eye.xz)) / (0.35 * eye.w));
	float land = saturate((under - y) / 0.12);
	float4 c = mul(float4(at, 1.0), view_proj);
	if (c.w < 0.3)
		return o;
	fade *= saturate((c.w - 0.3) / 1.2); // (one right by the lens would be a blot across the screen)
	[branch] if (hazy())
		fade *= clear_at(c.w);
	// On screen: its radius (0.014 to 0.03 yalms) at least a pixel (fainter by the area it's drawn bigger than it is)
	float2 half_px = float2(ground.yz) * 0.5;
	float radius = (0.014 + 0.016 * d.r.y) * (0.4 + 0.6 * land) * lin.z * half_px.x / c.w;
	float shown = max(radius, 1.0);
	o.alpha = shade.w * fade * (0.55 + 0.3 * d.r.z) * (radius / shown) * (radius / shown);
	if (o.alpha <= 0.002)
		return o;
	static const float2 corner[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };
	float2 cn = corner[vid % 6];
	c.xy += cn * (shown + 0.5) / half_px * c.w; // (half a pixel more: its edge fades out inside the quad)
	o.pos = c;
	o.uv = cn * (shown + 0.5) / shown;
	return o;
}

float4 snow_ps(snow_v2p i) : SV_Target
{
	if (behind_drawn(i.pos))
		discard;
	// A soft round flake: brightest in its middle, nothing at its edge
	float r2 = dot(i.uv, i.uv);
	if (r2 >= 1.0)
		discard;
	float edge = 1.0 - r2;
	return float4(color.rgb, i.alpha * edge * sqrt(edge));
}

// Leaves in wind and gales (leaf_vs, leaf_ps): in the rain's columns (its constants as the snow's), falling slowly
// (blade.x yalms a second, each 0.7 to 1.3 times that) and blown far across as they go (blade.yz yalms across for each
// yalm fallen: the wind's way), tumbling about their path (shade.y yalms) and turning over. Their columns are taken round
// the ground under the camera (the weather's [kWx + 1].w), not the camera: blown this far across, they'd otherwise all be
// downwind of it by the time they come down. Seen only within kLeafHigh yalms of what's under them, in a volume from
// kLeafAbove yalms over the camera to kLeafPeriod below that: a gust carrying them along the ground. A small leaf facing
// the camera, its own colour (dry brown, yellow, dull green) in the light color.rgb, turning about its middle, its face
// catching the light as it turns over. It ends at what's under it; fogged and hidden as the snow.
static const float kLeafPeriod = 14.0, kLeafAbove = 5.0, kLeafHigh = 5.0;

struct leaf_v2p
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;               // across the sprite, -1..1
	nointerpolation float4 tint : TINT;  // its colour, and how opaque
	nointerpolation float2 turn : TURN;  // how far it has turned (its cosine and sine)
};

leaf_v2p leaf_vs(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	leaf_v2p o;
	o.pos = float4(0.0, 0.0, -1.0, 1.0); // (in front of the near plane: clipped)
	o.uv = 0.0;
	o.tint = 0.0;
	o.turn = float2(1.0, 0.0);
	float level = weather_fx() ? actors[kWx + 1].w : eye.y + 2.0;
	rain_drop d = rain_column_at(iid, level);
	if (!d.falls)
		return o;
	float speed = d.speed * (0.7 + 0.6 * d.r.w);
	float top = eye.y - kLeafAbove;
	float fallen = color.w * speed + d.phase;
	float y = fallen - kLeafPeriod * floor(fallen / kLeafPeriod);
	y += kLeafPeriod * ceil((top - y) / kLeafPeriod);
	// (tumbling: across x and z, each its own number of turns an hour as the clock wraps hourly)
	float2 turns = floor(float2(700.0, 760.0) + float2(800.0, 700.0) * d.r.yz);
	float2 tumble = shade.y * (0.5 + 0.5 * d.r.w) * sin(6.2831853 * (frac(color.w * turns / 3600.0) + d.r.zy));
	float3 at = float3(d.u.x + blade.y * y + tumble.x, y, d.u.y + blade.z * y + tumble.y);
	float under = min(rain_top(rain_cell(at.xz)), person_top(at.xz));
	if (y >= under)
		return o;
	float high = under - y; // (over what's under it: y is down)
	float fade = saturate(high / 0.1) * (1.0 - smoothstep(0.6 * kLeafHigh, kLeafHigh, high));
	fade *= saturate((y - top) / 2.0) * saturate((top + kLeafPeriod - y) / 2.0);
	fade *= saturate((eye.w - distance(at.xz, eye.xz)) / (0.35 * eye.w));
	float4 c = mul(float4(at, 1.0), view_proj);
	if (c.w < 0.4 || fade <= 0.0)
		return o;
	fade *= saturate((c.w - 0.4) / 1.5); // (one right by the lens would be a blot across the screen)
	[branch] if (hazy())
		fade *= clear_at(c.w);
	// On screen: its half-length (0.04 to 0.08 yalms) at least a pixel (fainter by the area it's drawn bigger than it is)
	float2 half_px = float2(ground.yz) * 0.5;
	float radius = (0.04 + 0.04 * d.r.y) * lin.z * half_px.x / c.w;
	float shown = max(radius, 1.0);
	float alpha = shade.w * fade * 0.95 * (radius / shown) * (radius / shown);
	if (alpha <= 0.002)
		return o;
	float3 hue = lerp(lerp(float3(0.42, 0.26, 0.1), float3(0.62, 0.5, 0.16), saturate(d.r.z * 2.0)), float3(0.3, 0.42, 0.14), saturate(d.r.z * 2.0 - 1.0));
	float spin = 6.2831853 * frac(color.w * floor(1800.0 + 5400.0 * d.r.x) / 3600.0 + d.r.y);
	o.turn = float2(cos(spin), sin(spin));
	o.tint = float4(hue * color.rgb * (0.7 + 0.3 * abs(cos(spin * 1.7))), alpha);
	static const float2 corner[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };
	float2 cn = corner[vid % 6];
	c.xy += cn * (shown + 0.5) / half_px * c.w;
	o.pos = c;
	o.uv = cn * (shown + 0.5) / shown;
	return o;
}

float4 leaf_ps(leaf_v2p i) : SV_Target
{
	if (behind_drawn(i.pos))
		discard;
	// A narrow oval turned about its middle, a little pointed toward its tip
	float2 uv = float2(i.uv.x * i.turn.x - i.uv.y * i.turn.y, i.uv.x * i.turn.y + i.uv.y * i.turn.x);
	float wide = 0.42 * (1.0 - 0.35 * saturate(uv.x));
	float e = uv.x * uv.x + uv.y * uv.y / (wide * wide);
	if (e >= 1.0)
		discard;
	return float4(i.tint.rgb, i.tint.a * saturate((1.0 - e) * 3.0));
}

// The air pass (weather_fx; grass_render.cpp air_pipeline_for): over the world as everything has been drawn into it (the
// game's, the grass, the water, the rain), the world times keep plus add (dual-source blending). In it:
// - dust (or sand, [kWx + 10]) and gloom's fog banks ([kWx + 12]): thick near the ground and thinning above it (an
//   exponential profile over the ground under the camera, all of it below that: valleys fill), along the view to what's
//   drawn (the depth copy: the grass and the water a yalm or two in front of what's behind them, near enough); the
//   banks' thickness wanders across the ground with slow noise drifting with the wind ([kWx + 14].xy);
// - streaks of blown dust near the camera ([kWx + 11]): on upright sheets along the wind fixed in the world, 2 to 26 yalms out, where that's
//   in front of what's drawn and low over the ground, noise stretched along the wind and blowing along it.
// (The lightning's flash is a pass of its own after it: flash_ps.) Names facelift drew into the world are left clear, as
// the shading pass leaves them; the sky gets only the streaks (its own dust and gloom are weather_sky's).
struct air_out
{
	float4 add : SV_Target0;
	float4 keep : SV_Target1;
};

// How much of a fog thinning upward is along a stretch, a unit of it at the ground (optical depth for a density of 1
// there): from height a to b over the ground (below it, the ground's density) over len yalms; scale: yalms it takes to
// thin to 1/e
float fog_along(float a, float b, float len, float scale)
{
	// (the profile exp(-max(h, 0) / scale) integrated over the height: h - scale below the ground, -scale e^(-h / scale) above)
	float fa = a <= 0.0 ? a - scale : -scale * exp(-a / scale);
	float fb = b <= 0.0 ? b - scale : -scale * exp(-b / scale);
	return abs(b - a) > 1e-3 ? len * (fb - fa) / (b - a) : len * exp(-max(a, 0.0) / scale);
}

air_out air_ps(float4 pos : SV_Position)
{
	air_out o;
	o.add = 0.0;
	o.keep = 1.0;
	int2 p = int2(pos.xy);
	if (ground.w != 0 && name_at(p))
		return o;
	float z = drawn_depth(p);
	bool sky = z > kSkyDepth;
	float4 wind = actors[kWx], r0 = actors[kWx + 1], r2 = actors[kWx + 2];
	float4 dust = actors[kWx + 10], streak = actors[kWx + 11], gloom = actors[kWx + 12], banks = actors[kWx + 14];
	float t = r2.w;
	// The view ray in the world (y down), and how far to what's drawn
	float2 ndc = float2((p.x + 0.5) / ground.y * 2.0 - 1.0, 1.0 - (p.y + 0.5) / ground.z * 2.0);
	float3 v = sky ? float3(ndc.x / lin.z, ndc.y / lin.w, up_row.w) : view_at(p);
	float len = sky ? 1e9 : length(v);
	float3 wd = normalize(float3(dot(r0.xyz, v), dot(up_row.xyz, v), dot(r2.xyz, v)));
	float3 add = 0.0, keep = 1.0;
	[branch] if (!sky && (dust.w > 0.0 || gloom.w > 0.0))
	{
		float he = r0.w - eye.y, hp = he - wd.y * len; // (over the ground under the camera, at the camera and at what's drawn)
		float2 far_xz = (eye.xz + wd.xz * min(len, 160.0)) / 48.0, near_xz = (eye.xz + wd.xz * min(len, 25.0)) / 48.0;
		float bank = 0.35 + 1.3 * smoothstep(0.25, 0.75, 0.5 * (noise_wrap(far_xz + banks.xy, 256.0) + noise_wrap(near_xz + banks.xy, 256.0)));
		float tau_g = gloom.w * 0.05 * bank * fog_along(he, hp, len, 2.2);
		float tau_d = dust.w * 0.016 * (0.75 + 0.25 * bank) * fog_along(he, hp, len, 9.0);
		float clear = exp(-(tau_g + tau_d));
		add = (gloom.rgb * tau_g + dust.rgb * tau_d) / max(tau_g + tau_d, 1e-6) * (1.0 - clear);
		keep = clear;
	}
	[branch] if (streak.x > 0.0)
	{
		// On sheets fixed in the world: upright, along the wind, kStreakGap yalms apart, read where the view ray crosses
		// them between 2 and 26 yalms out (faded in and out at the ends). They were read on spheres round the camera
		// (3.5 to 24 yalms), which slid through the noise as the camera moved: running forward, it streamed out from
		// the middle of the screen like rings (in game, 2026-10-23). A sheet's streaks blow along it; looking along
		// the wind, the ray crosses few of them.
		const float kStreakGap = 2.5, kStreakNear = 2.0, kStreakFar = 26.0;
		float2 way = wind.z > 0.0 ? wind.xy / wind.z : float2(1.0, 0.0);
		float2 side = float2(-way.y, way.x);
		float c = dot(wd.xz, side), e0 = dot(eye.xz, side);
		float clear = 1.0;
		[branch] if (abs(c) > 1e-3)
		{
			float dir = c > 0.0 ? 1.0 : -1.0;
			float u = (e0 + c * kStreakNear) / kStreakGap;
			float m = dir > 0.0 ? ceil(u) : floor(u); // (the first sheet past kStreakNear)
			float step_d = kStreakGap / abs(c);
			float dist = (m * kStreakGap - e0) / c;
			[loop] for (int k = 0; k < 10 && dist < min(kStreakFar, len); ++k) // (at most (26 - 2) / 2.5 + 1 crossings)
			{
				float3 s = eye.xyz + wd * dist;
				float over = r0.w - s.y;
				float low = 1.0 - smoothstep(0.3, 3.5, over);
				float along = dot(s.xz, way);
				float n = noise2(float2(along * 0.12 - t * streak.y * 0.12, over * 1.1 + frac(m * 0.6180339) * 97.0));
				float ends = smoothstep(kStreakNear, kStreakNear + 2.0, dist) * (1.0 - smoothstep(kStreakFar - 8.0, kStreakFar, dist));
				clear *= 1.0 - streak.x * 0.2 * smoothstep(0.5, 0.85, n) * low * ends * saturate((len - dist) / 1.5);
				m += dir;
				dist += step_d;
			}
		}
		add = add * clear + dust.rgb * (1.0 - clear);
		keep *= clear;
	}
	o.add = float4(add, 0.0);
	o.keep = float4(keep, 1.0);
	return o;
}

// The lightning's flash (weather_fx: [kWx + 8].x; grass_render.cpp air_pipeline_for, after the air pass): what's drawn lit
// cool white for a moment, the world plus the world times this, up to twice as bright (light on it, in proportion to its
// own colour). The sky is lit with its clouds (weather_sky); names facelift drew are left as they are.
float4 flash_ps(float4 pos : SV_Position) : SV_Target
{
	int2 p = int2(pos.xy);
	float f = actors[kWx + 8].x;
	if ((ground.w != 0 && name_at(p)) || drawn_depth(p) > kSkyDepth)
		f = 0.0;
	return float4(saturate(f * 0.9 * float3(0.78, 0.86, 1.0)), 0.0);
}

// The game's visibility tests (vistest.hpp; client-notes "The visibility test, and a replacement that doesn't
// wait"; research/ffxi-re/data/vistest-re.md), answered from the depth copy at the world's end instead of the
// game's GPU readback, counted as the game counts them. Its box's corners truncated and sorted; the box kept within
// the screen and 256 pixels round it, and within the screen; its 16 x 16 reduction filled only xs x ys of the way,
// xs = 16 x (on the screen) / (kept), so the parts off the screen aren't counted; the points read are every other
// texel of that, each sampling the screen at px0 + 1 + (px1 - px0 - 1) x 2a / xs (clamped to the screen), counted
// when the depth test at the box's depth passes there (LESSEQUAL). A pixel here is a point of a box: x the point (0..63,
// row by row of the 8 x 8), y the box.
StructuredBuffer<float4> vis_boxes : register(t2); // two a box: x0 y0 x1 y1 (viewport pixels), then the depth tested
                                                   // at (NDC z), the viewport's width and height, and the kind
RWByteAddressBuffer vis_points : register(u1);     // a uint a point: 1 counted, 0 not

void vis_ps(float4 pos : SV_Position)
{
	uint k = uint(pos.x), b = uint(pos.y);
	float4 r = vis_boxes[b * 2], m = vis_boxes[b * 2 + 1];
	int2 a = int2(trunc(r.xy)), c = int2(trunc(r.zw));
	int2 lo = min(a, c), hi = max(a, c);
	int2 size = int2(m.yz);
	int2 plo = max(lo, int2(-256, -256)), phi = min(hi, size + 256); // (kept within the screen and 256 round it)
	int2 slo = max(lo, int2(0, 0)), shi = min(hi, size);              // (and within the screen)
	int2 cell = int2(k % 8, k / 8) * 2;
	uint passed = 0;
	if (zinfo.w != 0 && all(shi > slo) && all(phi > plo))
	{
		int2 share = (16 * (shi - slo)) / (phi - plo);
		if (all(cell < share))
		{
			float2 at = floor(float2(plo + 1) + float2(phi - plo - 1) * float2(cell) / float2(share));
			int2 p = clamp(int2(at), int2(0, 0), size - 1);
			int2 q = any(int2(zinfo.yz) != size) ? clamp(p * int2(zinfo.yz) / size, int2(0, 0), int2(zinfo.yz) - 1) : p;
			uint v = zbuf.Load((q.y * zinfo.x + q.x) * 4);
			if (zinfo.w == 1)
				passed = uint(saturate(m.x) * 16777215.0 + 0.5) <= (v & 0xFFFFFF) ? 1u : 0u;
			else
				passed = m.x <= asfloat(v) ? 1u : 0u;
		}
	}
	vis_points.Store((b * 64 + k) * 4, passed);
}

// The look (grass_render.cpp, "The look"): tone mapping, bloom and a colour grade over the world as it ends, before the
// interface. The world's copy (t1) is its last picture; the look's buffer (u1) holds the bright parts at a quarter of its
// width and height and smaller (the bloom's levels, each half the one before: rgb the bright light, a its log2
// luminance, both as halves), and the exposure the world is shown at (log2, adapting over seconds) after them. Names
// facelift drew (alpha 1) aren't part of it: left out of the light measured and the bloom, and left as drawn.
//   view_proj[0]: the grade's gain (rgb, linear: white balance), its saturation
//   view_proj[1]: its lift (rgb: the shadows tinted toward it), its contrast (around 0.3 as the screen shows it)
//   view_proj[2]: the bloom's strength (over its levels' count), unused, seconds since the last frame (< 0: the exposure
//                 set at once), the light the scene is exposed for (its mean luminance's key)
//   view_proj[3]: the exposure's least and most (times), the bloom's threshold and knee (linear)
//   eye (uint bits): where the pass reads (bytes into the buffer), where it writes, the size it reads (w, h)
//   color (uint bits): the size it writes (w, h), where the exposure is, which parts are on (1 tone mapping, 2 bloom,
//                 4 the grade)
//   blade (uint bits): where the bloom's first level is, its size (w, h)
RWByteAddressBuffer look_buf : register(u1);

float3 srgb_to_linear(float3 c)
{
	c = saturate(c);
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float3 linear_to_srgb(float3 c)
{
	c = saturate(c);
	return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float look_luma(float3 l) { return dot(l, float3(0.2126, 0.7152, 0.0722)); }

float4 look_read(uint at, int2 q, uint2 size)
{
	q = clamp(q, int2(0, 0), int2(size) - 1);
	uint2 v = look_buf.Load2(at + (uint(q.y) * size.x + uint(q.x)) * 8);
	return float4(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y), f16tof32(v.y >> 16));
}
void look_write(uint at, int2 q, uint width, float4 c)
{
	look_buf.Store2(at + (uint(q.y) * width + uint(q.x)) * 8, uint2(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | (f32tof16(c.a) << 16)));
}
// A level read between its texels (q: texel coordinates, their centres whole)
float4 look_between(uint at, float2 q, uint2 size)
{
	float2 f = q - floor(q);
	int2 i = int2(floor(q));
	return lerp(lerp(look_read(at, i, size), look_read(at, i + int2(1, 0), size), f.x),
		lerp(look_read(at, i + int2(0, 1), size), look_read(at, i + int2(1, 1), size), f.x), f.y);
}

// The filmic curve (linear in, linear out, each channel): mid-grey (0.18) kept, a little more contrast through the
// shadows (a power of 1.06 under it) and the middle (a slope of 1.1 over it), then from 0.6 a shoulder rolling toward
// 1.3, so what's brighter than white (the exposure or the bloom pushing it) runs smoothly into white rather than
// clipping; white itself comes out at about 0.96 (0.98 as the screen shows it)
float3 look_tone(float3 v)
{
	const float p = 0.18, c = 1.1, k = 0.6, w = 1.3;
	const float kk = p + (k - p) * c;
	v = max(v, 0.0);
	float3 lo = p * pow(v / p + 1e-7, 1.06);
	float3 mid = p + (v - p) * c;
	float3 hi = kk + (w - kk) * (1.0 - exp(-c * (v - k) / (w - kk)));
	return v < p ? lo : (v < k ? mid : hi);
}

// The first level: each texel a 4 x 4 block of the world's pixels: the light over the bloom's threshold (a soft knee),
// averaged with less weight for the brightest (a lone bright pixel doesn't flicker), and the block's mean log2 luminance
void look_first_ps(float4 pos : SV_Position)
{
	int2 q = int2(pos.xy);
	int2 base = q * 4;
	float thr = view_proj[3].z, knee = max(view_proj[3].w, 1e-3);
	float3 sum = 0.0;
	float weights = 0.0, logs = 0.0, n = 0.0;
	[unroll] for (int j = 0; j < 4; ++j)
		[unroll] for (int i = 0; i < 4; ++i)
		{
			int2 p = min(base + int2(i, j), int2(ground.yz) - 1);
			uint v = world.Load((uint(p.y) * ground.x + uint(p.x)) * 4);
			if ((v >> 24) != 1u) // (not a name)
			{
				float3 c = float3(v & 255, (v >> 8) & 255, (v >> 16) & 255) / 255.0;
				float3 l = srgb_to_linear(ground.w == 2 ? c.bgr : c);
				float y = look_luma(l);
				logs += log2(y + 1e-3);
				n += 1.0;
				float b = max(l.r, max(l.g, l.b));
				float rq = clamp(b - thr + knee, 0.0, 2.0 * knee);
				rq = rq * rq / (4.0 * knee);
				float share = max(rq, b - thr) / max(b, 1e-4);
				float k = 1.0 / (1.0 + y);
				sum += l * share * k;
				weights += k;
			}
		}
	look_write(asuint(eye.y), q, asuint(color.x), float4(weights > 0.0 ? sum / weights : 0.0, n > 0.0 ? logs / n : log2(0.07)));
}

// A level from the one before (twice its size): a 4 x 4 tent (1 3 3 1 each way) around the texels it covers
void look_down_ps(float4 pos : SV_Position)
{
	int2 q = int2(pos.xy);
	uint2 size = uint2(asuint(eye.z), asuint(eye.w));
	const float w4[4] = { 1.0, 3.0, 3.0, 1.0 };
	float4 s = 0.0;
	[unroll] for (int j = 0; j < 4; ++j)
		[unroll] for (int i = 0; i < 4; ++i)
			s += look_read(asuint(eye.x), q * 2 - 1 + int2(i, j), size) * (w4[i] * w4[j]);
	look_write(asuint(eye.y), q, asuint(color.x), s / 64.0);
}

// The exposure (one pixel): the scene's mean log2 luminance from the smallest level, the middle of the screen counting
// most; the exposure that wants (a share of the way to the key, kept between the least and the most); and the exposure
// moving toward it over a couple of seconds (log2, kept after the levels with the mean; set at once when asked, or
// when there's none yet)
void look_exposure_ps(float4 pos : SV_Position)
{
	uint2 size = uint2(asuint(eye.z), asuint(eye.w));
	float sum = 0.0, weights = 0.0;
	[loop] for (uint y = 0; y < size.y; ++y)
		[loop] for (uint x = 0; x < size.x; ++x)
		{
			float2 d = ((float2(x, y) + 0.5) / float2(size) - 0.5) * 2.0;
			float w = 1.0 - 0.75 * saturate(dot(d, d) * 0.5);
			sum += look_read(asuint(eye.x), int2(x, y), size).a * w;
			weights += w;
		}
	float key = log2(max(view_proj[2].w, 1e-3));
	float mean = weights > 0.0 ? sum / weights : key;
	const float kShare = 0.3, kSeconds = 2.5;
	float want = clamp(kShare * (key - mean), log2(max(view_proj[3].x, 1e-3)), log2(max(view_proj[3].y, 1e-3)));
	uint at = asuint(color.z);
	float was = asfloat(look_buf.Load(at));
	float dt = view_proj[2].z;
	bool known = abs(was) < 4.0; // (not a number, or what a new buffer held: not known)
	float now = dt < 0.0 || !known ? want : was + (want - was) * (1.0 - exp(-dt / kSeconds));
	look_buf.Store2(at, uint2(asuint(now), asuint(mean)));
}

// A level with the next smaller one added (between its texels): from the smallest up, each level ends up holding the
// bright light blurred over all the sizes under it
void look_up_ps(float4 pos : SV_Position)
{
	int2 q = int2(pos.xy);
	uint2 here = uint2(asuint(color.x), asuint(color.y));
	float4 me = look_read(asuint(eye.y), q, here);
	float3 up = look_between(asuint(eye.x), (float2(q) + 0.5) * 0.5 - 0.5, uint2(asuint(eye.z), asuint(eye.w))).rgb;
	look_write(asuint(eye.y), q, here.x, float4(me.rgb + up, me.a));
}

// The world as it's shown: its pixel in linear light, the bloom added (from the first level, between its texels), the
// grade's white balance, the exposure and the filmic curve; back to the screen's values, the grade's saturation,
// contrast (around 0.3: the game's pictures are dark, a flatter grade mustn't wash out their shadows) and lift there, and a dither of half a step (no bands in the sky). Names are left as drawn, and the world's
// alpha (the pipeline writes rgb alone).
float4 look_ps(float4 pos : SV_Position) : SV_Target
{
	int2 p = int2(pos.xy);
	uint v = world.Load((uint(p.y) * ground.x + uint(p.x)) * 4);
	if ((v >> 24) == 1u)
		discard;
	float3 c = float3(v & 255, (v >> 8) & 255, (v >> 16) & 255) / 255.0;
	float3 l = srgb_to_linear(ground.w == 2 ? c.bgr : c);
	uint parts = asuint(color.w);
	[branch] if ((parts & 2u) != 0u)
	{
		float2 q = (float2(p) + 0.5) * 0.25 - 0.5;
		l += look_between(asuint(blade.x), q, uint2(asuint(blade.y), asuint(blade.z))).rgb * view_proj[2].x;
	}
	// (the grade's white balance and saturation weigh less on what's bright: a lamp's pool keeps its warmth at night)
	float bright = smoothstep(0.08, 0.5, max(l.r, max(l.g, l.b)));
	if ((parts & 4u) != 0u)
		l *= lerp(view_proj[0].rgb, 1.0, 0.7 * bright);
	[branch] if ((parts & 1u) != 0u)
		l = look_tone(l * exp2(asfloat(look_buf.Load(asuint(color.z)))));
	float3 s = linear_to_srgb(l);
	if ((parts & 4u) != 0u)
	{
		float y = dot(s, float3(0.299, 0.587, 0.114));
		s = lerp(y.xxx, s, lerp(view_proj[0].w, 1.0, 0.3 * bright));
		s = (s - 0.3) * view_proj[1].w + 0.3;
		s += view_proj[1].rgb * (1.0 - s);
	}
	s += (hash2(p) - 0.5) / 255.0;
	return float4(saturate(s), 1.0);
}
