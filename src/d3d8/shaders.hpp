#pragma once

// The translator's shaders, all HLSL compiled with D3DCompile (vs_5_0, ps_5_0) and kept by their text's hash, in memory
// and in a folder of compiled blobs (d3d8-shader-cache beside this DLL, or D3D8_SHADER_CACHE).
//
// - Fixed function: one vertex and one pixel shader per combination of the states that change the code (ff_vs_key,
//   ff_pls_key). Values (matrices, lights, the material, fog, the texture factor) are constants.
// - vs.1.1 bytecode (the zone's shaders, assembled by the game from text with D3DX, notes-zone.md) and ps.1.0-1.4
//   bytecode, read instruction by instruction into HLSL.
//
// What every vertex shader hands the pixel shader (the same struct both sides, so any pair links):
//   pos SV_Position; d0 COLOR0 (diffuse); d1 COLOR1 (specular); t0..t7 TEXCOORD0..7; fog FOG (x the vertex fog factor,
//   y the depth table fog goes by); then clip distances when user clip planes are on (the pixel shader leaves them out).
// Vertex inputs are ATTR<register>: the Direct3D 8 declaration's registers (an FVF is read as the declaration Direct3D 8
// would make of it: position v0, blend weights v1, indices v2, normal v3, point size v4, diffuse v5, specular v6,
// texture coordinates v7..v14).

#include "common.hpp"

namespace hifi::d3d8
{
	// An input register's kind (the declaration's type, as the input layout reads it)
	enum input_kind : uint8_t { in_none = 0, in_float1, in_float2, in_float3, in_float4, in_color, in_ubyte4, in_short2, in_short4 };

	// The vertex shader constants' extra rows (after the 256 vs.1.1 registers c0..c255, at kExtraRow): the viewport for
	// pretransformed vertices and the depth range, and the clip planes
	constexpr uint32_t kVsConstRegs = 256;

	// The constants as the shaders read them (cbuffer b0 for vertex shaders, b1 for pixel shaders; matrices row_major,
	// as Direct3D keeps them)
	struct ff_light_c
	{
		float diffuse[4], specular[4], ambient[4];
		float position[4];  // view space, w the range
		float direction[4]; // view space (normalized), w the falloff
		float atten[4];     // a0 a1 a2
		float spot[4];      // cos(theta / 2), cos(phi / 2)
	};
	struct ff_vs_constants
	{
		float wvp[16];
		float wv[4][16];
		float nrm[4][16];
		float proj[16];
		float texm[8][16];
		float world[16];
		float mdiffuse[4], mambient[4], mspecular[4], memissive[4], mpower[4];
		float gambient[4];
		float fogp[4];      // start, end, density, 1 / (end - start)
		float vp[4];        // pretransformed: viewport x, y, 2 / width, 2 / height
		float zr[4];        // min z, max z - min z, table fog by w (1) or z (0)
		float clipp[6][4];  // world space
		ff_light_c lights[8];
	};
	struct sm1_vs_constants
	{
		float c[kVsConstRegs][4];
		float zr[4];
		float clipp[6][4];  // clip space
	};
	struct ps_constants
	{
		float tfactor[4], fogcolor[4];
		float fogp[4];      // start, end, density, 1 / (end - start)
		float aref[4];      // x: ALPHAREF (0..255)
		float bumpm[8][4];  // per stage BUMPENVMAT00, 01, 10, 11
		float bumpl[8][4];  // per stage BUMPENVLSCALE, BUMPENVLOFFSET
		float pc[8][4];     // ps.1.x c0..c7
	};

	// Fixed-function vertex shader: what shapes its code
	struct ff_vs_key
	{
		uint8_t inputs[16] = {};      // input_kind per register (v0..v15)
		uint8_t rhw = 0;              // pretransformed (XYZRHW)
		uint8_t lighting = 0, normalize = 0, local_viewer = 0, specular = 0, color_vertex = 0;
		uint8_t src_diffuse = 0, src_ambient = 0, src_specular = 0, src_emissive = 0; // D3DMCS_*
		uint8_t light_type[8] = {};   // 0 off, else D3DLIGHTTYPE
		uint8_t vertex_blend = 0;     // D3DVBF_ 0..3 (weights)
		uint8_t fog_vertex = 0;       // D3DFOG_* when fog is per vertex (fog on, table none), else 0
		uint8_t range_fog = 0;
		uint8_t stage_index[8] = {};  // texture coordinates set read for each stage's output
		uint8_t stage_gen[8] = {};    // 0 none, 1 camera-space normal, 2 position, 3 reflection vector
		uint8_t stage_xform[8] = {};  // D3DTTFF count (1..4, 0 none), with 0x80 projected
		uint8_t clip_planes = 0;      // D3DRS_CLIPPLANEENABLE
		uint8_t pad[3] = {};
	};

	struct ff_ps_stage
	{
		uint8_t cop = 0, ca1 = 0, ca2 = 0, ca0 = 0;
		uint8_t aop = 0, aa1 = 0, aa2 = 0, aa0 = 0;
		uint8_t result = 0;           // D3DTA_CURRENT or D3DTA_TEMP
		uint8_t tex = 0;              // 0 none, 1 2D, 2 cube, 3 volume
		uint8_t coords = 0;           // components the coordinates have (2, 3 or 4)
		uint8_t projected = 0;
	};

	// The pixel end shared by every pixel shader: fog and the alpha test (after the stages or the ps.1.x shader)
	struct ps_tail
	{
		uint8_t alpha_func = 0;       // 0 no test, else D3DCMPFUNC
		uint8_t fog = 0;              // 0 none, 1 per vertex, 2 table exp, 3 table exp2, 4 table linear
		uint8_t flat = 0;             // SHADEMODE FLAT: colours not interpolated
		uint8_t pad = 0;
	};

	struct ff_ps_key
	{
		ff_ps_stage stages[8];
		ps_tail tail;
		uint8_t specular = 0;
		uint8_t pad[3] = {};
	};

	// A vs.1.1 or ps.1.x shader read: HLSL text (or why not)
	struct sm1_shader
	{
		std::vector<DWORD> tokens;    // the function as the game gave it
		bool pixel = false;
		uint32_t version = 0;         // 0x0101 ...
		bool ok = false;
		std::string why;
		uint32_t max_const = 0;       // highest constant register read (relative addressing: all)
		// def'd constants
		struct def { uint32_t reg; float v[4]; };
		std::vector<def> defs;
		uint16_t inputs_read = 0;     // v registers a vertex shader reads
		uint8_t textures_read = 0;    // t registers a pixel shader samples
	};
	bool read_sm1(const DWORD *function, sm1_shader &out);
	size_t sm1_length(const DWORD *function); // in DWORDs, the end token included

	// HLSL text
	std::string ff_vs_hlsl(const ff_vs_key &k);
	std::string ff_ps_hlsl(const ff_ps_key &k);
	// inputs: the declaration's input kinds; clip_planes: CLIPPLANEENABLE
	std::string sm1_vs_hlsl(const sm1_shader &s, const uint8_t inputs[16], uint8_t clip_planes);
	// tex: per t register 1 2D, 2 cube, 3 volume; projected: per stage
	std::string sm1_ps_hlsl(const sm1_shader &s, const uint8_t tex[8], uint8_t projected, const ps_tail &tail);

	// (shared pieces of the HLSL: the vertex output struct, the vertex input struct, the pixel constants and the pixel
	// end; the pixel end reads `float4 col` and `VOut i` and returns)
	std::string hlsl_vout(bool pixel, bool flat, uint8_t clip_planes);
	std::string hlsl_vin(const uint8_t inputs[16]);
	std::string hlsl_vin_read(const uint8_t inputs[16]); // float4 v0..v15 from `VIn a`
	std::string hlsl_ps_constants();
	std::string hlsl_ps_tail(const ps_tail &t);

	// Compiled bytecode by the text (target "vs_5_0" / "ps_5_0"); empty with the compiler's words in why
	// (thread-safe; ahead: a worker of compile_ahead's, counted apart)
	const std::vector<uint8_t> &compile(const std::string &hlsl, const char *target, std::string &why, bool ahead = false);
	void set_shader_cache_folder(const std::wstring &folder);
	struct compile_stats { uint32_t compiled = 0, from_disk = 0, failed = 0, ahead = 0; double ms = 0.0, ahead_ms = 0.0; };
	// Shader keys, kept in keys.bin in the cache folder as draws meet them (note_shader_key: a key's bytes, then more bytes
	// for a program's tokens), compiled at the device's making by worker threads when their blob is missing (compile_ahead)
	enum key_kind : uint8_t { key_ff_vs = 1, key_ff_ps = 2, key_sm1_vs = 3, key_sm1_ps = 4 };
	void note_shader_key(uint8_t kind, const void *bytes, size_t n, const void *more = nullptr, size_t more_n = 0);
	void compile_ahead();
	// Every key in keys.bin compiled now, on this thread (the build's shipped cache: d3d8_unit --warm); how many blobs there are
	uint32_t compile_keys_now();
	compile_stats shader_stats();
}
