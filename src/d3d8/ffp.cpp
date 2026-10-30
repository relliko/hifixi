// Direct3D 8's fixed function as HLSL (shaders.hpp): the vertex side (transforms, vertex blending, 8 lights, the material
// and its colour sources, vertex fog, texture coordinate generation and matrices, user clip planes, pretransformed
// vertices) and the pixel side (8 texture stages with every colour and alpha operation, bump mapping, specular, the
// alpha test, vertex and table fog). The formulas are Direct3D 8's own (the SDK's "Mathematics of lighting" and the
// texture stage operations), each stage's result clamped to 0..1 as the fixed function does.

#include "shaders.hpp"
#include <cstdarg>
#include <cstdio>

namespace hifi::d3d8
{
	namespace
	{
		std::string fmt(const char *f, ...)
		{
			char buffer[1024];
			va_list a;
			va_start(a, f);
			vsnprintf(buffer, sizeof(buffer), f, a);
			va_end(a);
			return buffer;
		}

		const char *kFfVsConstants = R"(
struct Light { float4 diffuse, specular, ambient, position, direction, atten, spot; };
cbuffer ff : register(b0)
{
	row_major float4x4 wvp;
	row_major float4x4 wv[4];
	row_major float4x4 nrm[4];
	row_major float4x4 proj;
	row_major float4x4 texm[8];
	row_major float4x4 world;
	float4 mdiffuse, mambient, mspecular, memissive, mpower;
	float4 gambient;
	float4 fogp;
	float4 vp;
	float4 zr;
	float4 clipp[6];
	Light lights[8];
};
)";
	}

	std::string hlsl_vout(bool pixel, bool flat, uint8_t clip_planes)
	{
		std::string s = "struct VOut\n{\n\tfloat4 pos : SV_Position;\n";
		const char *interp = pixel && flat ? "nointerpolation " : "";
		s += fmt("\t%sfloat4 d0 : COLOR0;\n\t%sfloat4 d1 : COLOR1;\n", interp, interp);
		for (int i = 0; i < 8; ++i)
			s += fmt("\tfloat4 t%d : TEXCOORD%d;\n", i, i);
		s += "\tfloat4 fog : FOG;\n";
		if (!pixel && clip_planes != 0)
			s += "\tfloat4 cd0 : SV_ClipDistance0;\n\tfloat2 cd1 : SV_ClipDistance1;\n";
		s += "};\n";
		return s;
	}

	std::string hlsl_vin(const uint8_t inputs[16])
	{
		std::string s = "struct VIn\n{\n";
		bool any = false;
		for (int r = 0; r < 16; ++r)
		{
			const char *type = nullptr;
			switch (inputs[r])
			{
			case in_float1: case in_float2: case in_float3: case in_float4: case in_color: type = "float4"; break;
			case in_ubyte4: type = "uint4"; break;
			case in_short2: case in_short4: type = "int4"; break;
			default: break;
			}
			if (type != nullptr)
			{
				s += fmt("\t%s v%d : ATTR%d;\n", type, r, r);
				any = true;
			}
		}
		if (!any)
			s += "\tuint id : SV_VertexID;\n";
		s += "};\n";
		return s;
	}

	std::string hlsl_vin_read(const uint8_t inputs[16])
	{
		std::string s;
		for (int r = 0; r < 16; ++r)
			s += inputs[r] != in_none ? fmt("\tfloat4 v%d = float4(a.v%d);\n", r, r) : fmt("\tfloat4 v%d = float4(0, 0, 0, 1);\n", r);
		return s;
	}

	std::string hlsl_ps_constants()
	{
		return R"(
cbuffer ps : register(b1)
{
	float4 tfactor, fogcolor, fogp, aref;
	float4 bumpm[8];
	float4 bumpl[8];
	float4 pc[8];
};
)";
	}

	std::string hlsl_ps_tail(const ps_tail &t)
	{
		std::string s;
		if (t.alpha_func != 0 && t.alpha_func != D3DCMP_ALWAYS)
		{
			// (on the 8-bit alpha, as the hardware compares it with ALPHAREF)
			static const char *const ops[9] = { "", "false", "<", "==", "<=", ">", "!=", ">=", "true" };
			if (t.alpha_func == D3DCMP_NEVER)
				s += "\tdiscard;\n";
			else if (t.alpha_func <= 8)
				s += fmt("\t{\n\t\tconst float a8 = round(saturate(col.a) * 255.0);\n\t\tif (!(a8 %s aref.x))\n\t\t\tdiscard;\n\t}\n", ops[t.alpha_func]);
		}
		if (t.fog != 0)
		{
			s += "\t{\n";
			switch (t.fog)
			{
			case 1: s += "\t\tconst float f = saturate(i.fog.x);\n"; break;
			case 2: s += "\t\tconst float f = saturate(exp(-fogp.z * i.fog.y));\n"; break;
			case 3: s += "\t\tconst float f = saturate(exp(-(fogp.z * i.fog.y) * (fogp.z * i.fog.y)));\n"; break;
			default: s += "\t\tconst float f = saturate((fogp.y - i.fog.y) * fogp.w);\n"; break;
			}
			s += "\t\tcol.rgb = lerp(fogcolor.rgb, col.rgb, f);\n\t}\n";
		}
		s += "\treturn col;\n";
		return s;
	}

	std::string ff_vs_hlsl(const ff_vs_key &k)
	{
		std::string s = kFfVsConstants;
		s += hlsl_vin(k.inputs);
		s += hlsl_vout(false, false, k.clip_planes);
		s += "VOut main(VIn a)\n{\n\tVOut o = (VOut)0;\n";
		s += hlsl_vin_read(k.inputs);
		const bool has_normal = k.inputs[3] != in_none, has_diffuse = k.inputs[5] != in_none, has_specular = k.inputs[6] != in_none;

		if (k.rhw)
		{
			// Pretransformed: screen x, y (pixels), z (0..1), 1 / w. Into clip space through the viewport, with w kept for
			// perspective-correct interpolation (rhw 0 read as 1).
			s += "\tconst float w = v0.w != 0.0 ? 1.0 / v0.w : 1.0;\n";
			s += "\to.pos = float4(((v0.x - vp.x) * vp.z - 1.0) * w, (1.0 - (v0.y - vp.y) * vp.w) * w, v0.z * w, w);\n";
			s += has_diffuse ? "\to.d0 = v5;\n" : "\to.d0 = float4(1, 1, 1, 1);\n";
			s += has_specular ? "\to.d1 = v6;\n" : "\to.d1 = float4(0, 0, 0, 0);\n";
			// (fog: per vertex from the specular's alpha; the table's by w)
			s += has_specular ? "\to.fog = float4(v6.a, w, 0, 0);\n" : "\to.fog = float4(1, w, 0, 0);\n";
			for (int st = 0; st < 8; ++st)
			{
				// (through the stage's matrix as well, as dgVoodoo does with pretransformed vertices: the d3dlog replays'
				// interface drawn after the effects left COUNT2 set)
				const int set = k.stage_index[st] & 7, v = 7 + set;
				const int dims = k.inputs[v] >= in_float1 && k.inputs[v] <= in_float4 ? k.inputs[v] : 2;
				if ((k.stage_xform[st] & 7) != 0)
				{
					const std::string tc = dims == 1 ? fmt("float4(v%d.x, 1, 0, 0)", v) : dims == 2 ? fmt("float4(v%d.xy, 1, 0)", v) : dims == 3 ? fmt("float4(v%d.xyz, 1)", v) : fmt("v%d", v);
					s += fmt("\to.t%d = mul(%s, texm[%d]);\n", st, tc.c_str(), st);
				}
				else
					s += fmt("\to.t%d = v%d;\n", st, v);
			}
			s += "\treturn o;\n}\n";
			return s;
		}

		// Position (and normal) in view space, blended over 1..4 matrices
		const int matrices = k.vertex_blend + 1;
		s += "\tconst float4 p = float4(v0.xyz, 1.0);\n";
		if (matrices == 1)
		{
			s += "\tfloat3 P = mul(p, wv[0]).xyz;\n";
			s += "\to.pos = mul(p, wvp);\n";
			if (has_normal)
				s += "\tfloat3 N = mul(float4(v3.xyz, 0), nrm[0]).xyz;\n";
		}
		else
		{
			s += "\tfloat wt[4];\n\tfloat last = 1.0;\n";
			const char *comp = "xyzw";
			for (int m = 0; m < matrices - 1; ++m)
				s += fmt("\twt[%d] = v1.%c;\n\tlast -= wt[%d];\n", m, comp[m], m);
			s += fmt("\twt[%d] = last;\n", matrices - 1);
			s += "\tfloat3 P = 0;\n";
			if (has_normal)
				s += "\tfloat3 N = 0;\n";
			for (int m = 0; m < matrices; ++m)
			{
				s += fmt("\tP += wt[%d] * mul(p, wv[%d]).xyz;\n", m, m);
				if (has_normal)
					s += fmt("\tN += wt[%d] * mul(float4(v3.xyz, 0), nrm[%d]).xyz;\n", m, m);
			}
			s += "\to.pos = mul(float4(P, 1.0), proj);\n";
		}
		if (!has_normal)
			s += "\tfloat3 N = float3(0, 0, 0);\n";
		else if (k.normalize)
			s += "\tN = any(N) ? normalize(N) : N;\n";
		// (the depth range of the viewport: Direct3D 12's is always 0..1, as dgVoodoo sets it)
		s += "\to.pos.z = o.pos.z * zr.y + zr.x * o.pos.w;\n";

		// Lighting
		if (k.lighting)
		{
			auto source = [&](uint8_t src, const char *material) -> std::string {
				if (k.color_vertex && src == D3DMCS_COLOR1 && has_diffuse)
					return "v5";
				if (k.color_vertex && src == D3DMCS_COLOR2 && has_specular)
					return "v6";
				return material;
			};
			s += fmt("\tconst float4 md = %s;\n", source(k.src_diffuse, "mdiffuse").c_str());
			s += fmt("\tconst float4 ma = %s;\n", source(k.src_ambient, "mambient").c_str());
			s += fmt("\tconst float4 ms = %s;\n", source(k.src_specular, "mspecular").c_str());
			s += fmt("\tconst float4 me = %s;\n", source(k.src_emissive, "memissive").c_str());
			s += "\tfloat3 dif = 0, amb = 0, spe = 0;\n";
			if (k.specular)
				s += k.local_viewer ? "\tconst float3 V = -normalize(P);\n" : "\tconst float3 V = float3(0, 0, -1);\n";
			for (int l = 0; l < 8; ++l)
			{
				const uint8_t type = k.light_type[l];
				if (type == 0)
					continue;
				s += "\t{\n\t\tfloat3 L;\n\t\tfloat att = 1.0;\n";
				if (type == D3DLIGHT_DIRECTIONAL)
					s += fmt("\t\tL = -lights[%d].direction.xyz;\n", l);
				else
				{
					s += fmt("\t\tconst float3 d = lights[%d].position.xyz - P;\n\t\tconst float dist = length(d);\n\t\tL = dist > 0.0 ? d / dist : float3(0, 0, 1);\n", l);
					s += fmt("\t\tatt = dist <= lights[%d].position.w ? 1.0 / (lights[%d].atten.x + lights[%d].atten.y * dist + lights[%d].atten.z * dist * dist) : 0.0;\n", l, l, l, l);
					if (type == D3DLIGHT_SPOT)
					{
						s += fmt("\t\tconst float rho = dot(-L, lights[%d].direction.xyz);\n", l);
						s += fmt("\t\tif (rho <= lights[%d].spot.y)\n\t\t\tatt = 0.0;\n", l);
						s += fmt("\t\telse if (rho <= lights[%d].spot.x)\n\t\t\tatt *= pow(saturate((rho - lights[%d].spot.y) / max(lights[%d].spot.x - lights[%d].spot.y, 1e-6)), lights[%d].direction.w);\n", l, l, l, l, l);
					}
				}
				s += fmt("\t\tamb += lights[%d].ambient.rgb * att;\n", l);
				s += "\t\tconst float ndl = dot(N, L);\n";
				s += fmt("\t\tif (ndl > 0.0)\n\t\t{\n\t\t\tdif += lights[%d].diffuse.rgb * (ndl * att);\n", l);
				if (k.specular)
					s += fmt("\t\t\tconst float3 H = normalize(L + V);\n\t\t\tconst float ndh = dot(N, H);\n\t\t\tif (ndh > 0.0)\n\t\t\t\tspe += lights[%d].specular.rgb * (pow(ndh, mpower.x) * att);\n", l);
				s += "\t\t}\n\t}\n";
			}
			s += "\to.d0 = saturate(float4(me.rgb + ma.rgb * (gambient.rgb + amb) + md.rgb * dif, md.a));\n";
			s += k.specular ? "\to.d1 = saturate(float4(ms.rgb * spe, ms.a));\n" : "\to.d1 = float4(0, 0, 0, ms.a);\n";
		}
		else
		{
			s += has_diffuse ? "\to.d0 = v5;\n" : "\to.d0 = float4(1, 1, 1, 1);\n";
			s += has_specular ? "\to.d1 = v6;\n" : "\to.d1 = float4(0, 0, 0, 0);\n";
		}

		// Fog: per vertex by the view depth (or the distance, range fog); the table's depth by w (or z)
		s += "\tfloat f = 1.0;\n";
		if (k.fog_vertex != 0)
		{
			s += k.range_fog ? "\tconst float fd = length(P);\n" : "\tconst float fd = P.z;\n";
			switch (k.fog_vertex)
			{
			case D3DFOG_EXP: s += "\tf = exp(-fogp.z * fd);\n"; break;
			case D3DFOG_EXP2: s += "\tf = exp(-(fogp.z * fd) * (fogp.z * fd));\n"; break;
			default: s += "\tf = (fogp.y - fd) * fogp.w;\n"; break;
			}
		}
		s += "\to.fog = float4(saturate(f), zr.z != 0.0 ? P.z : o.pos.z / o.pos.w, 0, 0);\n";

		// Texture coordinates, per stage: a set or generated, through the stage's matrix (the reflection by the normal made
		// unit: dgVoodoo's, pixel for pixel in d3d8_frame's texgen scene)
		s += "\tconst float3 Nn = any(N) ? normalize(N) : N;\n";
		for (int st = 0; st < 8; ++st)
		{
			const int set = k.stage_index[st] & 7;
			const int dims = k.inputs[7 + set] >= in_float1 && k.inputs[7 + set] <= in_float4 ? k.inputs[7 + set] : 2;
			std::string tc;
			switch (k.stage_gen[st])
			{
			case 1: tc = "float4(N, 1.0)"; break;
			case 2: tc = "float4(P, 1.0)"; break;
			case 3: tc = k.local_viewer ? "float4(reflect(normalize(P), Nn), 1.0)" : "float4(reflect(float3(0, 0, 1), Nn), 1.0)"; break;
			default:
				if ((k.stage_xform[st] & 7) != 0)
				{
					// (Direct3D pads the set before its matrix: the element after the last is 1, the rest 0)
					const int v = 7 + set;
					tc = dims == 1 ? fmt("float4(v%d.x, 1, 0, 0)", v) : dims == 2 ? fmt("float4(v%d.xy, 1, 0)", v) : dims == 3 ? fmt("float4(v%d.xyz, 1)", v) : fmt("v%d", v);
				}
				else
					tc = fmt("v%d", 7 + set);
				break;
			}
			if ((k.stage_xform[st] & 7) != 0)
				s += fmt("\to.t%d = mul(%s, texm[%d]);\n", st, tc.c_str(), st);
			else
				s += fmt("\to.t%d = %s;\n", st, tc.c_str());
		}

		// User clip planes (world space for the fixed function)
		if (k.clip_planes != 0)
		{
			s += "\tconst float4 pw = mul(p, world);\n";
			const char *const dst[6] = { "o.cd0.x", "o.cd0.y", "o.cd0.z", "o.cd0.w", "o.cd1.x", "o.cd1.y" };
			for (int c = 0; c < 6; ++c)
				s += (k.clip_planes >> c) & 1 ? fmt("\t%s = dot(pw, clipp[%d]);\n", dst[c], c) : fmt("\t%s = 1.0;\n", dst[c]);
		}
		s += "\treturn o;\n}\n";
		return s;
	}

	namespace
	{
		// An argument of a stage: its source and modifiers
		std::string arg(uint8_t a, int stage, bool alpha)
		{
			std::string v;
			switch (a & D3DTA_SELECTMASK)
			{
			case D3DTA_DIFFUSE: v = "i.d0"; break;
			case D3DTA_CURRENT: v = "cur"; break;
			case D3DTA_TEXTURE: v = fmt("tx%d", stage); break;
			case D3DTA_TFACTOR: v = "tfactor"; break;
			case D3DTA_SPECULAR: v = "i.d1"; break;
			case 5: v = "tmp"; break; // D3DTA_TEMP
			default: v = "float4(1, 1, 1, 1)"; break;
			}
			if (a & D3DTA_ALPHAREPLICATE)
				v = "(" + v + ").aaaa";
			if (a & D3DTA_COMPLEMENT)
				v = "(1.0 - " + v + ")";
			(void)alpha;
			return v;
		}

		bool reads_texture(uint8_t a) { return (a & D3DTA_SELECTMASK) == D3DTA_TEXTURE; }

		// Whether an operation reads the stage's texture through the arguments it uses (or its texture's alpha)
		bool needs_texture(uint8_t o, uint8_t a1, uint8_t a2, uint8_t a0)
		{
			switch (o)
			{
			case D3DTOP_SELECTARG1: case D3DTOP_PREMODULATE: return reads_texture(a1);
			case D3DTOP_SELECTARG2: return reads_texture(a2);
			case D3DTOP_MULTIPLYADD: case D3DTOP_LERP: return reads_texture(a0) || reads_texture(a1) || reads_texture(a2);
			case D3DTOP_BLENDTEXTUREALPHA: case D3DTOP_BLENDTEXTUREALPHAPM: return true;
			default: return reads_texture(a1) || reads_texture(a2);
			}
		}

		// One operation; ch: ".rgb" or ".a"
		std::string op(uint8_t o, const std::string &a1, const std::string &a2, const std::string &a0, const char *ch, int stage)
		{
			const std::string x1 = "(" + a1 + ")" + ch, x2 = "(" + a2 + ")" + ch, x0 = "(" + a0 + ")" + ch;
			switch (o)
			{
			case D3DTOP_SELECTARG1: return x1;
			case D3DTOP_SELECTARG2: return x2;
			case D3DTOP_MODULATE: return x1 + " * " + x2;
			case D3DTOP_MODULATE2X: return "2.0 * " + x1 + " * " + x2;
			case D3DTOP_MODULATE4X: return "4.0 * " + x1 + " * " + x2;
			case D3DTOP_ADD: return x1 + " + " + x2;
			case D3DTOP_ADDSIGNED: return x1 + " + " + x2 + " - 0.5";
			case D3DTOP_ADDSIGNED2X: return "2.0 * (" + x1 + " + " + x2 + " - 0.5)";
			case D3DTOP_SUBTRACT: return x1 + " - " + x2;
			case D3DTOP_ADDSMOOTH: return x1 + " + " + x2 + " - " + x1 + " * " + x2;
			case D3DTOP_BLENDDIFFUSEALPHA: return "lerp(" + x2 + ", " + x1 + ", i.d0.a)";
			case D3DTOP_BLENDTEXTUREALPHA: return fmt("lerp(%s, %s, tx%d.a)", x2.c_str(), x1.c_str(), stage);
			case D3DTOP_BLENDFACTORALPHA: return "lerp(" + x2 + ", " + x1 + ", tfactor.a)";
			case D3DTOP_BLENDTEXTUREALPHAPM: return fmt("%s + %s * (1.0 - tx%d.a)", x1.c_str(), x2.c_str(), stage);
			case D3DTOP_BLENDCURRENTALPHA: return "lerp(" + x2 + ", " + x1 + ", cur.a)";
			case D3DTOP_PREMODULATE: return x1;
			case D3DTOP_MODULATEALPHA_ADDCOLOR: return std::string(ch) == ".a" ? x1 + " * " + x2 : x1 + " + (" + a1 + ").a * " + x2;
			case D3DTOP_MODULATECOLOR_ADDALPHA: return std::string(ch) == ".a" ? x1 + " * " + x2 : x1 + " * " + x2 + " + (" + a1 + ").a";
			case D3DTOP_MODULATEINVALPHA_ADDCOLOR: return std::string(ch) == ".a" ? x1 + " * " + x2 : "(1.0 - (" + a1 + ").a) * " + x2 + " + " + x1;
			case D3DTOP_MODULATEINVCOLOR_ADDALPHA: return std::string(ch) == ".a" ? x1 + " * " + x2 : "(1.0 - " + x1 + ") * " + x2 + " + (" + a1 + ").a";
			case D3DTOP_DOTPRODUCT3: return std::string(ch) == ".a" ? "dot((" + a1 + ").rgb - 0.5, (" + a2 + ").rgb - 0.5) * 4.0" : "dot((" + a1 + ").rgb - 0.5, (" + a2 + ").rgb - 0.5).xxx * 4.0";
			case D3DTOP_MULTIPLYADD: return x0 + " + " + x1 + " * " + x2;
			case D3DTOP_LERP: return "lerp(" + x2 + ", " + x1 + ", " + x0 + ")";
			default: return x1;
			}
		}
	}

	std::string ff_ps_hlsl(const ff_ps_key &k)
	{
		std::string s = hlsl_ps_constants();
		static const char *const kinds[4] = { "Texture2D", "Texture2D", "TextureCube", "Texture3D" };
		for (int st = 0; st < 8; ++st)
			s += fmt("%s tex%d : register(t%d);\nSamplerState smp%d : register(s%d);\n", kinds[k.stages[st].tex & 3], st, st, st, st);
		s += hlsl_vout(true, k.tail.flat != 0, 0);
		s += "float4 main(VOut i) : SV_Target\n{\n\tfloat4 cur = i.d0, tmp = float4(0, 0, 0, 0);\n";
		// (the bump offset and luminance the stage before hands this one)
		s += "\tfloat2 bump = float2(0, 0);\n\tfloat lum = 1.0;\n";
		const char *const coord[8] = { "i.t0", "i.t1", "i.t2", "i.t3", "i.t4", "i.t5", "i.t6", "i.t7" };
		for (int st = 0; st < 8; ++st)
		{
			const ff_ps_stage &g = k.stages[st];
			if (g.cop == D3DTOP_DISABLE || g.cop == 0)
				break;
			const bool bump_op = g.cop == D3DTOP_BUMPENVMAP || g.cop == D3DTOP_BUMPENVMAPLUMINANCE;
			const bool sample = g.tex != 0 && (reads_texture(g.ca1) || reads_texture(g.ca2) || reads_texture(g.ca0) || reads_texture(g.aa1) ||
				reads_texture(g.aa2) || reads_texture(g.aa0) || g.cop == D3DTOP_BLENDTEXTUREALPHA || g.cop == D3DTOP_BLENDTEXTUREALPHAPM ||
				g.aop == D3DTOP_BLENDTEXTUREALPHA || g.aop == D3DTOP_BLENDTEXTUREALPHAPM || bump_op);
			// (a stage with no texture reads white, as dgVoodoo's)
			s += fmt("\t// stage %d\n\tfloat4 tx%d = float4(1, 1, 1, 1);\n", st, st);
			if (sample)
			{
				s += fmt("\t{\n\t\tfloat4 c = %s;\n", coord[st]);
				// (projected: by the fourth element whatever the count, as dgVoodoo divides, pixel for pixel in the texgen
				// scene; Direct3D 8's documentation says by the count's last)
				if (g.projected)
					s += "\t\tc.xyz /= c.w;\n";
				s += "\t\tc.xy += bump;\n";
				if ((g.tex & 3) == 1)
					s += fmt("\t\ttx%d = tex%d.Sample(smp%d, c.xy) * lum;\n", st, st, st);
				else
					s += fmt("\t\ttx%d = tex%d.Sample(smp%d, c.xyz) * lum;\n", st, st, st);
				s += "\t}\n";
			}
			s += "\tbump = float2(0, 0);\n\tlum = 1.0;\n";
			const std::string dst = (g.result & D3DTA_SELECTMASK) == 5 ? "tmp" : "cur";
			if (bump_op)
			{
				// (this stage's texture is the bump map: it moves the next stage's coordinates, and scales its colour)
				s += fmt("\tbump = float2(bumpm[%d].x * tx%d.r + bumpm[%d].z * tx%d.g, bumpm[%d].y * tx%d.r + bumpm[%d].w * tx%d.g);\n", st, st, st, st, st, st, st, st);
				if (g.cop == D3DTOP_BUMPENVMAPLUMINANCE)
					s += fmt("\tlum = saturate(tx%d.b * bumpl[%d].x + bumpl[%d].y);\n", st, st, st);
				continue;
			}
			// (a stage that reads a texture it hasn't got hands the current colour, or alpha, on: dgVoodoo's way, seen in
			// d3d8_frame's formats and blend scenes)
			const bool no_texture = g.tex == 0;
			const std::string c1 = arg(g.ca1, st, false), c2 = arg(g.ca2, st, false), c0 = arg(g.ca0, st, false);
			if (no_texture && needs_texture(g.cop, g.ca1, g.ca2, g.ca0))
				s += fmt("\tconst float3 rgb%d = cur.rgb;\n", st);
			else
				s += fmt("\tconst float3 rgb%d = saturate(%s);\n", st, op(g.cop, c1, c2, c0, ".rgb", st).c_str());
			if (g.cop == D3DTOP_DOTPRODUCT3)
				s += fmt("\tconst float a%d = rgb%d.r;\n", st, st);
			else if (g.aop == D3DTOP_DISABLE || g.aop == 0 || (no_texture && needs_texture(g.aop, g.aa1, g.aa2, g.aa0)))
				s += fmt("\tconst float a%d = cur.a;\n", st);
			else
			{
				const std::string a1 = arg(g.aa1, st, true), a2 = arg(g.aa2, st, true), a0 = arg(g.aa0, st, true);
				s += fmt("\tconst float a%d = saturate(%s);\n", st, op(g.aop, a1, a2, a0, ".a", st).c_str());
			}
			s += fmt("\t%s = float4(rgb%d, a%d);\n", dst.c_str(), st, st);
		}
		s += "\tfloat4 col = cur;\n";
		if (k.specular)
			s += "\tcol.rgb = saturate(col.rgb + i.d1.rgb);\n";
		s += hlsl_ps_tail(k.tail);
		s += "}\n";
		return s;
	}
}
