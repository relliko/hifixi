#pragma once
// HiFi XI's own look over the game's lighting, for the native zone's and characters' per-pixel path (their option
// "perpixel"; the fixed-function path stays the game's): the ambient as a sky's, more from above than from below, and
// a highlight of the sun. Header only: each shader puts it after light_split.hpp's and holds its own strengths.
namespace hifi::light
{
	inline const char *kHemiSpecHlsl = R"(
// (hemi_spec.hpp) A light's hue at a brightness of 1, for a tint that keeps the brightness (white for a black one: a
// dungeon's black fog, an unknown sun)
float3 light_hue(float3 c)
{
	const float l = dot(c, float3(0.2126, 0.7152, 0.0722));
	return lerp(1.0, clamp(c / max(l, 1e-4), 0.0, 2.0), saturate(l * 20.0));
}

// The ambient a as a sky's: by the surface's facing n (world, y down; any length) below x a straight down, above x a
// straight up, sideways halfway; tint: how far toward the sky's hue from above and the ground's from below
float3 hemi_ambient(float3 a, float3 n, float below, float above, float tint, float3 sky, float3 ground)
{
	const float l = length(n);
	const float k = (l > 1e-6 ? -n.y / l : 0.0) * 0.5 + 0.5;
	return a * lerp(below, above, k) * lerp(1.0, lerp(light_hue(ground), light_hue(sky), k), tint);
}

// The sun's highlight (Blinn-Phong, kept to its energy: a sharper one is brighter, not wider), added after the
// texture: n the surface's facing, to_sun and to_eye toward the sun and the camera (world, any length), sun its colour;
// power how sharp, strength how much at a full one. None where the surface faces away from either, nor where the sun or
// the camera has no direction (the login screen, a Mog House: normalize(0) is NaN, and a NaN drew whole characters black,
// 2026-10-29)
float3 sun_spec(float3 n, float3 to_sun, float3 to_eye, float3 sun, float power, float strength)
{
	if (dot(n, n) < 1e-12 || dot(to_sun, to_sun) < 1e-12 || dot(to_eye, to_eye) < 1e-12 || strength <= 0 || max(sun.r, max(sun.g, sun.b)) <= 0)
		return 0;
	n = normalize(n);
	const float3 l = normalize(to_sun), v = normalize(to_eye);
	const float nl = dot(n, l);
	if (nl <= 0 || dot(n, v) <= 0 || strength <= 0)
		return 0;
	const float nh = saturate(dot(n, normalize(l + v)));
	return sun * (strength * (power + 8.0) / 8.0 * pow(nh, power) * nl);
}

// The sun's share (light_share) once its highlight is added to the colour drawn: share of drawn, all of spec
float spec_share(float share, float3 drawn, float3 spec)
{
	const float3 k = float3(0.2126, 0.7152, 0.0722);
	const float a = dot(drawn, k), s = dot(spec, k);
	return s > 0 && a + s > 1e-4 ? saturate((share * a + s) / (a + s)) : share;
}
)";
}
