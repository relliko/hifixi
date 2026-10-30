/*
 * HiFi XI's d3d8.dll, its pieces without a device (docs/own-frame/d3d8.md):
 *   - the formats: each Direct3D 8 format's pixel to what the GPU gets, a target's RGBA8 back to the game's bytes, DXT;
 *   - the fixed function's HLSL for a spread of states (every colour and alpha operation and argument modifier, lights
 *     of each type, material sources, fog modes, texture coordinate generation and matrices, vertex blending, clip planes,
 *     pretransformed vertices, the alpha test) compiled with D3DCompile: every one must;
 *   - the game's own zone shaders (vs.1.1 with 0..3 lamps and ps.1.1, assembled with D3DX 8 from their text in the FFXiMain
 *     dump) and a set of vs.1.1 / ps.1.1-1.4 programs through the shader model 1 translator, compiled the same way.
 *
 *   build\d3d8\d3d8_unit.exe
 */

#include <windows.h>
#include <d3d8.h>
#include <d3dx8.h> // (before Direct3D 12's headers: their GUID macros don't suit D3DX 8's)
#include "../src/d3d8/format.hpp"
#include "../src/d3d8/shaders.hpp"
#include <cstdarg>
#include <cstdio>
#include <string>

namespace hifi::d3d8
{
	// (the library's log, here the console when asked: D3D8_UNIT_VERBOSE)
	void log(int, const char *format, ...)
	{
		if (std::getenv("D3D8_UNIT_VERBOSE") == nullptr)
			return;
		va_list a;
		va_start(a, format);
		std::vprintf(format, a);
		va_end(a);
		std::printf("\n");
	}
	void log_once(int level, const char *format, ...)
	{
		char text[1024];
		va_list a;
		va_start(a, format);
		vsnprintf(text, sizeof(text), format, a);
		va_end(a);
		log(level, "%s", text);
	}
}

namespace
{
	using namespace hifi::d3d8;
	int failures = 0, checks = 0;

	void check(bool ok, const char *what)
	{
		checks++;
		if (!ok)
		{
			failures++;
			std::printf("FAIL  %s\n", what);
		}
	}

	bool compiles(const std::string &hlsl, const char *target, const char *what)
	{
		std::string why;
		const std::vector<uint8_t> &b = compile(hlsl, target, why);
		checks++;
		if (b.empty())
		{
			failures++;
			std::printf("FAIL  %s didn't compile: %s\n%s\n", what, why.c_str(), hlsl.c_str());
			return false;
		}
		return true;
	}

	void test_formats()
	{
		// One pixel of each format to BGRA8 (what the GPU gets for the converted ones), and a target's RGBA8 back
		struct px { D3DFORMAT f; uint8_t in[4]; uint8_t out[4]; };
		const px cases[] = {
			{ D3DFMT_R8G8B8, { 0x10, 0x20, 0x30 }, { 0x10, 0x20, 0x30, 0xFF } },
			{ D3DFMT_A4L4, { 0xF8 }, { 0x88, 0x88, 0x88, 0xFF } },
			{ D3DFMT_R3G3B2, { 0xE3 }, { 0xFF, 0x00, 0xFF, 0xFF } },
			{ D3DFMT_A8R3G3B2, { 0x1C, 0x40 }, { 0x00, 0xFF, 0x00, 0x40 } },
		};
		for (const px &c : cases)
		{
			uint8_t o[4] = {};
			to_gpu(c.f, c.in, 4, o, 4, 1, 1, nullptr);
			char what[64];
			std::snprintf(what, sizeof(what), "format %u to BGRA8", unsigned(c.f));
			check(std::memcmp(o, c.out, 4) == 0, what);
		}
		// A palette
		PALETTEENTRY pal[256] = {};
		pal[7] = { 0x11, 0x22, 0x33, 0x44 };
		const uint8_t p8 = 7;
		uint8_t o[4];
		to_gpu(D3DFMT_P8, &p8, 1, o, 4, 1, 1, pal);
		check(o[0] == 0x33 && o[1] == 0x22 && o[2] == 0x11 && o[3] == 0x44, "P8 through its palette");
		// A target's RGBA8 to the game's formats
		const uint8_t rgba[4] = { 0xFF, 0x80, 0x00, 0x40 };
		uint8_t b[4] = {};
		from_target(D3DFMT_A8R8G8B8, rgba, 4, b, 4, 1, 1);
		check(b[0] == 0x00 && b[1] == 0x80 && b[2] == 0xFF && b[3] == 0x40, "RGBA8 target to A8R8G8B8");
		from_target(D3DFMT_X8R8G8B8, rgba, 4, b, 4, 1, 1);
		check(b[3] == 0xFF, "RGBA8 target to X8R8G8B8 (alpha opaque)");
		uint16_t w = 0;
		from_target(D3DFMT_R5G6B5, rgba, 4, reinterpret_cast<uint8_t *>(&w), 2, 1, 1);
		check(w == ((31 << 11) | (32 << 5) | 0), "RGBA8 target to R5G6B5");
		// The game's A8R8G8B8 into a target and back
		const uint8_t argb[4] = { 0x01, 0x02, 0x03, 0x04 };
		uint8_t t[4] = {}, back[4] = {};
		to_target(D3DFMT_A8R8G8B8, argb, 4, t, 4, 1, 1);
		from_target(D3DFMT_A8R8G8B8, t, 4, back, 4, 1, 1);
		check(std::memcmp(argb, back, 4) == 0, "A8R8G8B8 through a target and back");
		// DXT1: a block of two colours, the third and fourth between them (or transparent)
		const uint8_t dxt1[8] = { 0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4 }; // red, blue; indices 0 1 2 3 per row
		uint8_t px16[16 * 4] = {};
		decode_dxt(D3DFMT_DXT1, dxt1, 8, px16, 16, 4, 4);
		check(px16[2] == 0xFF && px16[0] == 0x00, "DXT1 colour 0 (red)");
		check(px16[4 + 0] == 0xFF && px16[4 + 2] == 0x00, "DXT1 colour 1 (blue)");
		check(px16[8 + 2] == 0xAA && px16[8 + 0] == 0x55, "DXT1 colour 2 (two thirds red)");
		// Pitches
		check(row_pitch(D3DFMT_DXT1, 16) == 32 && row_count(D3DFMT_DXT1, 16) == 4, "DXT1 pitch and rows");
		check(row_pitch(D3DFMT_DXT3, 6) == 32, "DXT3 pitch of 6 pixels (2 blocks)");
		check(row_pitch(D3DFMT_A8R8G8B8, 5) == 20 && row_pitch(D3DFMT_R5G6B5, 5) == 10, "plain pitches");
		check(info(D3DFMT_A8R8G8B8).resource == DXGI_FORMAT_B8G8R8A8_UNORM && info(D3DFMT_DXT3).resource == DXGI_FORMAT_BC2_UNORM, "texture formats");
		check(info(D3DFMT_D24S8).resource == DXGI_FORMAT_R24G8_TYPELESS && info(D3DFMT_D24S8).view == DXGI_FORMAT_D24_UNORM_S8_UINT, "D24S8 as dgVoodoo's (44 and 45)");
		check(kTargetResource == DXGI_FORMAT_R8G8B8A8_TYPELESS && kTargetView == DXGI_FORMAT_R8G8B8A8_UNORM, "targets as dgVoodoo's (27 and 28)");
	}

	void test_fixed_function()
	{
		// Vertex shaders
		uint32_t made = 0;
		const uint8_t inputs_lit[16] = { in_float3, 0, 0, in_float3, 0, in_color, 0, in_float2 };
		const uint8_t inputs_rhw[16] = { in_float4, 0, 0, 0, 0, in_color, in_color, in_float2, in_float2 };
		const uint8_t inputs_blend[16] = { in_float3, in_float2, 0, in_float3, 0, in_color, in_color, in_float2 };
		for (int variant = 0; variant < 48; ++variant)
		{
			ff_vs_key k;
			const uint8_t *in = variant % 6 == 0 ? inputs_rhw : variant % 6 == 1 ? inputs_blend : inputs_lit;
			std::memcpy(k.inputs, in, 16);
			k.rhw = in == inputs_rhw;
			k.lighting = uint8_t(variant & 1 ? 0 : 1);
			k.normalize = uint8_t((variant >> 1) & 1);
			k.local_viewer = uint8_t((variant >> 2) & 1);
			k.specular = uint8_t((variant >> 3) & 1);
			k.color_vertex = 1;
			k.src_diffuse = uint8_t(variant % 3);
			k.src_ambient = uint8_t((variant + 1) % 3);
			k.src_specular = uint8_t((variant + 2) % 3);
			k.src_emissive = uint8_t(variant % 2);
			for (int l = 0; l < 8; ++l)
				k.light_type[l] = uint8_t(l < (variant % 9) ? 1 + (l + variant) % 3 : 0);
			k.vertex_blend = uint8_t(in == inputs_blend ? 2 : 0);
			k.fog_vertex = uint8_t(variant % 4);
			k.range_fog = uint8_t((variant >> 4) & 1);
			for (int st = 0; st < 8; ++st)
			{
				k.stage_index[st] = uint8_t(st % 2);
				k.stage_gen[st] = uint8_t((st + variant) % 4);
				k.stage_xform[st] = uint8_t(((st + variant) % 5) | (st == 3 ? 0x80 : 0));
			}
			k.clip_planes = uint8_t(variant % 5 == 0 ? 0x2B : 0);
			char what[64];
			std::snprintf(what, sizeof(what), "fixed-function vertex shader %d", variant);
			made += compiles(ff_vs_hlsl(k), "vs_5_0", what) ? 1 : 0;
		}
		// Pixel shaders: every operation in stage 0 with the arguments and modifiers turned round, then pairs
		const uint8_t args[] = { D3DTA_DIFFUSE, D3DTA_CURRENT, D3DTA_TEXTURE, D3DTA_TFACTOR, D3DTA_SPECULAR, 5, D3DTA_TEXTURE | D3DTA_COMPLEMENT,
			D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE, D3DTA_DIFFUSE | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE };
		for (uint8_t op = D3DTOP_SELECTARG1; op <= D3DTOP_LERP; ++op)
		{
			ff_ps_key k;
			k.stages[0].cop = op;
			k.stages[0].ca1 = args[op % 9];
			k.stages[0].ca2 = args[(op + 3) % 9];
			k.stages[0].ca0 = args[(op + 5) % 9];
			k.stages[0].aop = uint8_t(op == D3DTOP_DOTPRODUCT3 ? D3DTOP_SELECTARG1 : op);
			k.stages[0].aa1 = args[(op + 1) % 9];
			k.stages[0].aa2 = args[(op + 2) % 9];
			k.stages[0].tex = uint8_t(1 + op % 3);
			k.stages[0].result = uint8_t(op % 4 == 0 ? 5 : D3DTA_CURRENT);
			k.stages[1].cop = uint8_t(op % 2 ? D3DTOP_MODULATE2X : D3DTOP_DISABLE);
			k.stages[1].ca1 = D3DTA_TEXTURE;
			k.stages[1].ca2 = D3DTA_CURRENT;
			k.stages[1].aop = D3DTOP_MODULATE4X;
			k.stages[1].aa1 = D3DTA_TEXTURE;
			k.stages[1].aa2 = D3DTA_TFACTOR;
			k.stages[1].tex = uint8_t(op % 5 == 0 ? 0 : 1);
			k.stages[1].projected = uint8_t(op % 3 == 0);
			k.tail.alpha_func = uint8_t(op % 9);
			k.tail.fog = uint8_t(op % 5);
			k.tail.flat = uint8_t(op % 2);
			k.specular = uint8_t(op % 3 == 1);
			char what[64];
			std::snprintf(what, sizeof(what), "fixed-function pixel shader, operation %u", op);
			made += compiles(ff_ps_hlsl(k), "ps_5_0", what) ? 1 : 0;
		}
		std::printf("fixed function: %u shaders compiled\n", made);
	}

	std::vector<uint8_t> dump;
	std::string shader_text(uint32_t table)
	{
		if (dump.empty())
		{
			FILE *f = std::fopen("C:\\Games\\PhoenixXI\\FFXiMain_04490000.bin", "rb");
			if (f == nullptr)
				return std::string();
			std::fseek(f, 0, SEEK_END);
			dump.resize(size_t(std::ftell(f)));
			std::fseek(f, 0, SEEK_SET);
			if (std::fread(dump.data(), 1, dump.size(), f) != dump.size())
				dump.clear();
			std::fclose(f);
		}
		std::string text;
		for (uint32_t i = 0; table + 4 * i + 4 <= dump.size(); ++i)
		{
			uint32_t va = 0;
			std::memcpy(&va, dump.data() + table + 4 * i, 4);
			const uint32_t o = va - 0x04490000u;
			if (va == 0 || o >= dump.size())
				break;
			text += reinterpret_cast<const char *>(dump.data() + o);
			text += "\n";
		}
		return text;
	}

	bool translate(const std::string &text, const char *what, sm1_shader &s)
	{
		LPD3DXBUFFER code = nullptr, errors = nullptr;
		if (FAILED(D3DXAssembleShader(text.c_str(), UINT(text.size()), 0, nullptr, &code, &errors)))
		{
			std::printf("FAIL  %s didn't assemble: %s\n", what, errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
			failures++;
			return false;
		}
		const bool ok = read_sm1(static_cast<const DWORD *>(code->GetBufferPointer()), s);
		check(ok, what);
		check(sm1_length(static_cast<const DWORD *>(code->GetBufferPointer())) * 4 == code->GetBufferSize(), "the shader's length in tokens");
		code->Release();
		return ok;
	}

	void test_sm1()
	{
		// The game's zone shaders (notes-zone.md)
		const uint8_t zone_inputs[16] = { in_float3, 0, 0, in_float3, 0, in_color, 0, in_float2, 0, 0, 0, 0, 0, 0, 0, in_float3 };
		const uint32_t tables[4] = { 0x376FF8, 0x377048, 0x3770D0, 0x377168 };
		if (shader_text(tables[0]).empty())
			std::printf("(no FFXiMain dump at C:\\Games\\PhoenixXI: the game's shaders left out)\n");
		else
		{
			for (int l = 0; l < 4; ++l)
			{
				sm1_shader s;
				char what[64];
				std::snprintf(what, sizeof(what), "the zone's vertex shader with %d lamps", l);
				if (translate(shader_text(tables[l]), what, s))
				{
					check(!s.pixel && s.version == 0xFFFE0101, "vs.1.1 read as such");
					check((s.inputs_read & 0x80A9) == 0x80A9, "v0 v3 v5 v7 v15 read");
					compiles(sm1_vs_hlsl(s, zone_inputs, 0), "vs_5_0", what);
					compiles(sm1_vs_hlsl(s, zone_inputs, 3), "vs_5_0", "the zone's vertex shader with clip planes");
				}
			}
			sm1_shader p;
			if (translate(shader_text(0x3772C4), "the zone's pixel shader", p))
			{
				check(p.pixel && (p.version & 0xFFFF) == 0x0101, "ps.1.1 read as such");
				const uint8_t tex[8] = { 1, 1, 0, 0 };
				ps_tail tail;
				tail.fog = 1;
				tail.alpha_func = D3DCMP_GREATER;
				compiles(sm1_ps_hlsl(p, tex, 0, tail), "ps_5_0", "the zone's pixel shader");
			}
		}
		// Every vs.1.1 instruction
		const char *vs =
			"vs.1.1\nm4x4 oPos, v0, c0\nm4x3 r0.xyz, v0, c4\nm3x4 r1, v3, c8\nm3x3 r2.xyz, v3, c12\nm3x2 r3.xy, v3, c16\nmov r4, -v5.wzyx\n"
			"add r5, r4, c20\nmad r6, r5, c21.x, -c22\nmul r7, r6, r6\nrcp r8.x, r7.y\nrsq r8.y, r7.z\ndp3 r9.x, r0, r1\ndp4 r9.y, r1, r2\nmin r10, r5, r6\n"
			"max r11, r5, r6\nslt r0, r10, r11\nsge r1, r10, r11\nexp r2.x, r8.x\nlog r2.y, r8.y\nexpp r3, r9.x\nlogp r4, r9.y\nlit r5, r7\n"
			"dst r6, r5, r4\nfrc r7.xy, r6\nmov a0.x, r8.x\nmov oD0, c[a0.x + 30]\nmov oD1, r1\nmov oT0, r2\nmov oT1, r3\nmov oT7, r4\nmov oFog.x, r5.x\nmov oPts.x, r6.x\n";
		sm1_shader s;
		const uint8_t inputs[16] = { in_float4, 0, 0, in_float3, 0, in_color };
		if (translate(vs, "every vs.1.1 instruction", s))
		{
			check(s.max_const == kVsConstRegs - 1, "relative addressing reads all the constants");
			compiles(sm1_vs_hlsl(s, inputs, 0), "vs_5_0", "every vs.1.1 instruction");
		}
		// Pixel shaders: 1.0 to 1.4, modifiers, co-issue, the texture addressing instructions
		const char *ps[] = {
			"ps.1.0\ntex t0\ntexcoord t1\nmov r0, t0\nmul r0, r0, t1\n",
			"ps.1.1\ndef c4, 0.5, 0.25, 1.0, 0.0\ntex t0\ntex t1\nmul_x2 r0, t0, v0\nlrp r1.rgb, c0, t1, r0\n+mul r1.a, t0.a, v0.a\nmad_sat r0.rgb, r1, c4, t1_bias\n+mov r0.a, 1-r1.a\n",
			"ps.1.1\ntex t0\ntexbem t1, t0\ntexbeml t2, t0\nadd_d2 r0, t1, t2\nsub_x4 r1, -t1_bx2, v1\ncnd r0, r0.a, r0, r1\n",
			"ps.1.1\ntex t0\ntexm3x2pad t1, t0_bx2\ntexm3x2tex t2, t0_bx2\nmov r0, t2\n",
			"ps.1.1\ntex t0\ntexm3x3pad t1, t0_bx2\ntexm3x3pad t2, t0_bx2\ntexm3x3vspec t3, t0_bx2\nmul r0, t3, v0\n",
			"ps.1.1\ntex t0\ntexreg2ar t1, t0\ntexreg2gb t2, t0\ndp3_sat r0, t1, t2\ntexkill t3\n",
			"ps.1.2\ntex t0\ntexreg2rgb t1, t0\ndp4 r0, t0, t1\ncmp r0, r0, t0, t1\n",
			"ps.1.3\ntex t0\ntexm3x2pad t1, t0\ntexm3x2depth t2, t0\nmov r0, t0\n",
			"ps.1.4\ntexld r0, t0\ntexcrd r1.rgb, t1\nmad r0.rgb, r1, c2, r0\nphase\ntexld r1, r0\nmul r0, r0, v0\nlrp r0, c0.a, r0, r1\n",
			"ps.1.4\ntexld r0, t0_dz\ntexcrd r2.xyz, t2_dw.xyw\nbem r1.rg, r0, r2\nphase\ntexld r3, r1\ncmp r0, r3, r3, -r3\ncnd r0, r0, r0, r3\n",
		};
		int n = 0;
		for (const char *text : ps)
		{
			sm1_shader p;
			char what[96];
			std::snprintf(what, sizeof(what), "pixel shader %d (%.6s)", n++, text);
			if (!translate(text, what, p))
				continue;
			const uint8_t tex[8] = { 1, 1, 2, 3, 1, 1 };
			ps_tail tail;
			tail.fog = uint8_t(n % 5);
			compiles(sm1_ps_hlsl(p, tex, uint8_t(n % 2 ? 0x2 : 0), tail), "ps_5_0", what);
		}
	}
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	test_formats();
	test_fixed_function();
	test_sm1();
	const compile_stats cs = shader_stats();
	std::printf("%d checks, %d failed; %u shaders compiled in %.0f ms\n", checks, failures, cs.compiled, cs.ms);
	return failures == 0 ? 0 : 1;
}
