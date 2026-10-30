// HiFi XI's stretch of the world over the screen (upscale.cpp): AMD FidelityFX Super Resolution 1 (fsr1/, MIT), its
// 32-bit pixel shader paths. Two passes, each one full-screen triangle:
//   easu_ps  the world (t0, the background resolution) up to the screen's size, along its edges, into a texture of ours
//   rcas_ps  that texture (t0) sharpened into the screen's back buffer
// Constants (b0): the world's size, the screen's, RCAS's sharpness in stops (0 the sharpest; each stop halves it), and
// the game's tint on the world (its resolve's, 1 the picture as it is: cutscene fades), put on by the pass into the back
// buffer.
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

cbuffer pass_constants : register(b0)
{
	float2 in_size;
	float2 out_size;
	float stops;
	float3 tint;
};
Texture2D<float4> source : register(t0);
SamplerState bilinear : register(s0);

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return source.GatherRed(bilinear, p); }
AF4 FsrEasuGF(AF2 p) { return source.GatherGreen(bilinear, p); }
AF4 FsrEasuBF(AF2 p) { return source.GatherBlue(bilinear, p); }

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return source.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "ffx_fsr1.h"

float4 full_vs(uint vid : SV_VertexID) : SV_Position
{
	// (one triangle over the whole target)
	const float2 uv = float2((vid << 1) & 2, vid & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 easu_ps(float4 pos : SV_Position) : SV_Target
{
	AU4 con0, con1, con2, con3;
	FsrEasuCon(con0, con1, con2, con3, in_size.x, in_size.y, in_size.x, in_size.y, out_size.x, out_size.y);
	AF3 c;
	FsrEasuF(c, AU2(pos.xy), con0, con1, con2, con3);
	return float4(c, 1.0);
}

// FSR 2's picture (t0, the screen's size) onto the back buffer as it is, but for the tint
float4 copy_ps(float4 pos : SV_Position) : SV_Target
{
	return float4(source.Load(int3(pos.xy, 0)).rgb * tint, 1.0);
}

float4 rcas_ps(float4 pos : SV_Position) : SV_Target
{
	AU4 con;
	FsrRcasCon(con, stops);
	AF1 r, g, b;
	FsrRcasF(r, g, b, AU2(pos.xy), con);
	return float4(float3(r, g, b) * tint, 1.0);
}
