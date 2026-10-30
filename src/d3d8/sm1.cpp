// Shader model 1 bytecode as HLSL (shaders.hpp): vs.1.1 (vs.1.0) and ps.1.0-1.4, the token format Direct3D 8 and
// D3DX 8's assembler use (the game's zone shaders are assembled from text at its start: notes-zone.md).
//
// Tokens: the version (0xFFFE0101 vs.1.1, 0xFFFF01xx ps.1.x), instructions (opcode in the low 16 bits, ps's co-issue bit
// 30) each followed by its parameter tokens (bit 31 set, which is how an instruction's length is told in shader model
// 1: no length field before shader model 2), comments (0xFFFE, their length in bits 16-30), the end (0x0000FFFF). def
// is the one instruction whose parameters aren't all marked: a destination and four floats.
//
// Each instruction becomes one HLSL statement on float4 locals named as the registers, every source read before the
// destination is written (Direct3D's order: `mul r0, r0.yzxw, r1` reads the old r0). Pixel shader registers aren't
// clamped between instructions: dgVoodoo reports MaxPixelShaderValue as the largest float (its registers are floats).

#include "shaders.hpp"
#include <cstdarg>
#include <cstdio>
#include <set>

namespace hifi::d3d8
{
	namespace
	{
		enum op : uint32_t
		{
			NOP = 0, MOV, ADD, SUB, MAD, MUL, RCP, RSQ, DP3, DP4, MIN, MAX, SLT, SGE, EXP, LOG, LIT, DST, LRP, FRC,
			M4x4, M4x3, M3x4, M3x3, M3x2,
			TEXCOORD = 64, TEXKILL, TEX, TEXBEM, TEXBEML, TEXREG2AR, TEXREG2GB, TEXM3x2PAD, TEXM3x2TEX, TEXM3x3PAD, TEXM3x3TEX,
			TEXM3x3DIFF, TEXM3x3SPEC, TEXM3x3VSPEC, EXPP, LOGP, CND, DEF, TEXREG2RGB, TEXDP3TEX, TEXM3x2DEPTH, TEXDP3, TEXM3x3,
			TEXDEPTH, CMP, BEM,
			PHASE = 0xFFFD, COMMENT = 0xFFFE, END = 0xFFFF,
		};

		enum regtype : uint32_t { R_TEMP = 0, R_INPUT, R_CONST, R_ADDR_TEX, R_RASTOUT, R_ATTROUT, R_TEXCRDOUT };

		std::string fmt(const char *f, ...)
		{
			char buffer[512];
			va_list a;
			va_start(a, f);
			vsnprintf(buffer, sizeof(buffer), f, a);
			va_end(a);
			return buffer;
		}

		struct param
		{
			DWORD token = 0;
			uint32_t reg() const { return token & 0x7FF; }
			uint32_t type() const { return (token >> 28) & 7; }
			uint32_t mask() const { return (token >> 16) & 15; }
			bool sat() const { return (token & 0x00100000) != 0; }
			int shift() const { const int s = int((token >> 24) & 15); return s >= 8 ? s - 16 : s; }
			uint32_t swizzle() const { return (token >> 16) & 0xFF; }
			uint32_t modifier() const { return (token >> 24) & 15; }
			bool relative() const { return (token & 0x2000) != 0; }
		};

		struct instr
		{
			uint32_t opcode = 0;
			bool coissue = false;
			param dst;
			param src[4];
			uint32_t nsrc = 0;
			bool has_dst = false;
			float def[4] = {};
		};

		// Parameters an opcode has (dst + sources), -1 when it's read off the tokens
		bool has_dest(uint32_t o)
		{
			switch (o)
			{
			case NOP: case PHASE: case END: case COMMENT: return false;
			default: return true;
			}
		}

		bool parse(const DWORD *t, std::vector<instr> &out, uint32_t &version, std::string &why, size_t *length)
		{
			const DWORD *const start = t;
			version = t[0];
			if ((version & 0xFFFE0000) != 0xFFFE0000)
			{
				why = "no version token";
				return false;
			}
			++t;
			for (int guard = 0; guard < 65536; ++guard)
			{
				const DWORD tok = *t;
				const uint32_t o = tok & 0xFFFF;
				if (tok == 0x0000FFFF)
				{
					if (length != nullptr)
						*length = size_t(t - start) + 1;
					return true;
				}
				if (o == COMMENT)
				{
					t += 1 + ((tok >> 16) & 0x7FFF);
					continue;
				}
				instr in;
				in.opcode = o;
				in.coissue = (tok & 0x40000000) != 0;
				++t;
				if (o == DEF)
				{
					in.dst.token = t[0];
					in.has_dst = true;
					std::memcpy(in.def, t + 1, 16);
					t += 5;
					out.push_back(in);
					continue;
				}
				std::vector<DWORD> params;
				while ((*t & 0x80000000) != 0)
					params.push_back(*t++);
				if (o == PHASE || o == NOP)
				{
					out.push_back(in);
					continue;
				}
				if (!params.empty() && has_dest(o))
				{
					in.dst.token = params[0];
					in.has_dst = true;
					for (size_t i = 1; i < params.size() && in.nsrc < 4; ++i)
						in.src[in.nsrc++].token = params[i];
				}
				out.push_back(in);
			}
			why = "no end token";
			return false;
		}

		const char *kComp = "xyzw";

		struct writer
		{
			bool pixel = false;
			uint32_t minor = 1;
			std::string body;
			std::set<uint32_t> defs; // def'd constant registers (literals)
			const sm1_shader *shader = nullptr;
			bool ok = true;
			std::string why;
			bool ps14() const { return pixel && minor >= 4; }

			std::string reg_name(const param &p, bool is_dst)
			{
				const uint32_t n = p.reg();
				switch (p.type())
				{
				case R_TEMP: return fmt("r%u", n);
				case R_INPUT: return pixel ? fmt("v%u", n) : fmt("v%u", n);
				case R_CONST:
					if (is_dst)
						return fmt("c%u", n);
					if (pixel)
						return defs.count(n) != 0 ? fmt("k%u", n) : fmt("pc[%u]", n & 7);
					if (p.relative())
						return fmt("cread(a0 + %u)", n);
					return defs.count(n) != 0 ? fmt("k%u", n) : fmt("c[%u]", n);
				case R_ADDR_TEX: return pixel ? fmt("t%u", n) : std::string("a0");
				case R_RASTOUT: return n == 0 ? "oPos" : n == 1 ? "oFog" : "oPts";
				case R_ATTROUT: return fmt("oD%u", n & 1);
				case R_TEXCRDOUT: return fmt("oT%u", n & 7);
				default:
					ok = false;
					why = fmt("register type %u", p.type());
					return "r0";
				}
			}

			// A source as a float4 expression, swizzled and modified
			std::string src(const param &p)
			{
				std::string base = reg_name(p, false);
				if (p.type() == R_ADDR_TEX && !pixel)
					base = "float4(a0, a0, a0, a0)";
				const uint32_t sw = p.swizzle();
				if (sw != 0xE4)
				{
					std::string s = ".";
					for (int i = 0; i < 4; ++i)
						s += kComp[(sw >> (2 * i)) & 3];
					base = "(" + base + ")" + s;
				}
				switch (p.modifier())
				{
				case 0: return base;
				case 1: return "(-" + base + ")";
				case 2: return "(" + base + " - 0.5)";
				case 3: return "(-(" + base + " - 0.5))";
				case 4: return "(2.0 * " + base + " - 1.0)";
				case 5: return "(-(2.0 * " + base + " - 1.0))";
				case 6: return "(1.0 - " + base + ")";
				case 7: return "(2.0 * " + base + ")";
				case 8: return "(-2.0 * " + base + ")";
				case 9: return "(" + base + " / " + base + ".z)"; // _dz (ps.1.4 texld/texcrd)
				case 10: return "(" + base + " / " + base + ".w)"; // _dw
				default: return base;
				}
			}

			std::string mask_of(const param &d)
			{
				uint32_t m = d.mask();
				if (m == 0)
					m = 15;
				std::string s = ".";
				for (int i = 0; i < 4; ++i)
					if (m & (1u << i))
						s += kComp[i];
				return s;
			}

			// dst = expr (a float4), with the destination's mask, shift and saturate
			void assign(const param &d, const std::string &expr)
			{
				std::string e = expr;
				if (pixel)
				{
					const int sh = d.shift();
					if (sh > 0)
						e = fmt("(%s) * %d.0", e.c_str(), 1 << sh);
					else if (sh < 0)
						e = fmt("(%s) / %d.0", e.c_str(), 1 << -sh);
					if (d.sat())
						e = "saturate(" + e + ")";
				}
				else if (d.sat())
					e = "saturate(" + e + ")";
				if (!pixel && d.type() == R_ADDR_TEX)
				{
					// (the address register loaded rounded to the nearest: dgVoodoo's, seen in d3d8_frame's shaders scene)
					body += fmt("\ta0 = (int)floor((%s).x + 0.5);\n", e.c_str());
					return;
				}
				const std::string m = mask_of(d);
				if (!redirect.empty())
				{
					// (a co-issued half: into a copy of its register, written back once both halves have read)
					body += fmt("\tfloat4 %s = %s;\n\t%s%s = (%s)%s;\n", redirect.c_str(), reg_name(d, true).c_str(), redirect.c_str(), m.c_str(), e.c_str(), m.c_str());
					return;
				}
				body += fmt("\t%s%s = (%s)%s;\n", reg_name(d, true).c_str(), m.c_str(), e.c_str(), m.c_str());
			}
			std::string redirect;

			std::string cmat(const param &p, int row)
			{
				param q = p;
				q.token = (p.token & ~0x7FFu) | ((p.reg() + uint32_t(row)) & 0x7FF);
				return reg_name(q, false);
			}

			std::string coords(uint32_t stage) { return fmt("tc%u", stage & 7); }
			std::string sample(uint32_t stage, const std::string &c)
			{
				const uint32_t s = stage & 7;
				return fmt("tex%u.Sample(smp%u, (%s).%s)", s, s, c.c_str(), tex_dims(s));
			}
			const char *tex_dims(uint32_t s);
			const uint8_t *tex = nullptr;

			void emit(const instr &in);
		};

		const char *writer::tex_dims(uint32_t s) { return tex != nullptr && (tex[s] == 2 || tex[s] == 3) ? "xyz" : "xy"; }

		void writer::emit(const instr &in)
		{
			const param &d = in.dst;
			auto s = [&](uint32_t i) { return i < in.nsrc ? src(in.src[i]) : std::string("float4(0, 0, 0, 0)"); };
			switch (in.opcode)
			{
			case NOP: case PHASE: case DEF: return;
			case MOV: assign(d, s(0)); return;
			case ADD: assign(d, s(0) + " + " + s(1)); return;
			case SUB: assign(d, s(0) + " - " + s(1)); return;
			case MAD: assign(d, s(0) + " * " + s(1) + " + " + s(2)); return;
			case MUL: assign(d, s(0) + " * " + s(1)); return;
			case RCP: assign(d, "rcp1((" + s(0) + ").w).xxxx"); return;
			case RSQ: assign(d, "rsq1((" + s(0) + ").w).xxxx"); return;
			case DP3: assign(d, "dot((" + s(0) + ").xyz, (" + s(1) + ").xyz).xxxx"); return;
			case DP4: assign(d, "dot(" + s(0) + ", " + s(1) + ").xxxx"); return;
			case MIN: assign(d, "min(" + s(0) + ", " + s(1) + ")"); return;
			case MAX: assign(d, "max(" + s(0) + ", " + s(1) + ")"); return;
			case SLT: assign(d, "float4((" + s(0) + ") < (" + s(1) + "))"); return;
			case SGE: assign(d, "float4((" + s(0) + ") >= (" + s(1) + "))"); return;
			case EXP: assign(d, "exp2((" + s(0) + ").w).xxxx"); return;
			case LOG: assign(d, "log1((" + s(0) + ").w).xxxx"); return;
			case EXPP: assign(d, "expp1((" + s(0) + ").w)"); return;
			case LOGP: assign(d, "logp1((" + s(0) + ").w)"); return;
			case LIT: assign(d, "lit1(" + s(0) + ")"); return;
			case DST: assign(d, "float4(1.0, (" + s(0) + ").y * (" + s(1) + ").y, (" + s(0) + ").z, (" + s(1) + ").w)"); return;
			case LRP: assign(d, "lerp(" + s(2) + ", " + s(1) + ", " + s(0) + ")"); return;
			case FRC: assign(d, "frac(" + s(0) + ")"); return;
			case M4x4: case M4x3: case M3x4: case M3x3: case M3x2:
			{
				const int rows = in.opcode == M4x4 || in.opcode == M3x4 ? 4 : in.opcode == M4x3 || in.opcode == M3x3 ? 3 : 2;
				const bool four = in.opcode == M4x4 || in.opcode == M4x3;
				std::string e = "float4(";
				for (int r = 0; r < 4; ++r)
				{
					if (r < rows)
						e += four ? "dot(" + s(0) + ", " + cmat(in.src[1], r) + ")" : "dot((" + s(0) + ").xyz, (" + cmat(in.src[1], r) + ").xyz)";
					else
						e += "0.0";
					e += r < 3 ? ", " : ")";
				}
				assign(d, e);
				return;
			}
			case CND:
				if (ps14())
					assign(d, "(" + s(0) + " > 0.5) ? " + s(1) + " : " + s(2));
				else
					assign(d, "((" + s(0) + ").a > 0.5) ? " + s(1) + " : " + s(2));
				return;
			case CMP: assign(d, "(" + s(0) + " >= 0.0) ? " + s(1) + " : " + s(2)); return;
			case BEM:
			{
				// (ps.1.4: the bump matrix of the destination's stage over src1's rg, added to src0's)
				const uint32_t st = d.reg() & 7;
				assign(d, fmt("float4((%s).xy + float2(bumpm[%u].x * (%s).x + bumpm[%u].z * (%s).y, bumpm[%u].y * (%s).x + bumpm[%u].w * (%s).y), 0, 0)",
					s(0).c_str(), st, s(1).c_str(), st, s(1).c_str(), st, s(1).c_str(), st, s(1).c_str()));
				return;
			}
			// Texture instructions (ps.1.0-1.3: the destination tN is stage N; ps.1.4: texld/texcrd into rN)
			case TEX:
				if (ps14())
				{
					// texld rN, tM or rM: stage N sampled at the source
					const std::string c = in.nsrc != 0 ? s(0) : coords(d.reg());
					body += fmt("\tr%u = %s;\n", d.reg(), sample(d.reg(), c).c_str());
				}
				else
					body += fmt("\tt%u = %s;\n", d.reg(), sample(d.reg(), coords(d.reg())).c_str());
				return;
			case TEXCOORD:
				if (ps14())
					assign(d, in.nsrc != 0 ? s(0) : coords(d.reg()));
				else
					body += fmt("\tt%u = float4(saturate(%s.xyz), 1.0);\n", d.reg(), coords(d.reg()).c_str());
				return;
			case TEXKILL:
				if (ps14() && d.type() == R_TEMP)
					body += fmt("\tif (any(r%u.xyz < 0.0))\n\t\tdiscard;\n", d.reg());
				else
					body += fmt("\tif (any(%s.xyz < 0.0))\n\t\tdiscard;\n", coords(d.reg()).c_str());
				return;
			case TEXBEM: case TEXBEML:
			{
				const uint32_t n = d.reg() & 7;
				const std::string m = src(in.src[0]);
				const std::string c = fmt("float4(%s.xy + float2(bumpm[%u].x * %s.x + bumpm[%u].z * %s.y, bumpm[%u].y * %s.x + bumpm[%u].w * %s.y), 0, 0)",
					coords(n).c_str(), n, m.c_str(), n, m.c_str(), n, m.c_str(), n, m.c_str());
				body += fmt("\tt%u = %s;\n", n, sample(n, c).c_str());
				if (in.opcode == TEXBEML)
					body += fmt("\tt%u.rgb *= saturate(%s.z * bumpl[%u].x + bumpl[%u].y);\n", n, m.c_str(), n, n);
				return;
			}
			case TEXREG2AR: body += fmt("\tt%u = %s;\n", d.reg(), sample(d.reg(), "float4(" + s(0) + ".w, " + s(0) + ".x, 0, 0)").c_str()); return;
			case TEXREG2GB: body += fmt("\tt%u = %s;\n", d.reg(), sample(d.reg(), "float4(" + s(0) + ".y, " + s(0) + ".z, 0, 0)").c_str()); return;
			case TEXREG2RGB: body += fmt("\tt%u = %s;\n", d.reg(), sample(d.reg(), "float4(" + s(0) + ".xyz, 0)").c_str()); return;
			case TEXM3x2PAD: case TEXM3x3PAD:
				body += fmt("\tm3[%u] = dot(%s.xyz, (%s).xyz);\n", (d.reg() & 3) % 3, coords(d.reg()).c_str(), s(0).c_str());
				body += fmt("\tm3n = %u;\n", d.reg());
				return;
			case TEXM3x2TEX:
				body += fmt("\tt%u = %s;\n", d.reg(), sample(d.reg(), fmt("float4(m3[%u], dot(%s.xyz, (%s).xyz), 0, 0)", ((d.reg() - 1) & 3) % 3, coords(d.reg()).c_str(), s(0).c_str())).c_str());
				return;
			case TEXM3x2DEPTH:
				log_once(1, "ps.1.3 texm3x2depth: depth output not written");
				body += fmt("\tt%u = float4(0, 0, 0, 0);\n", d.reg());
				return;
			case TEXM3x3TEX: case TEXM3x3: case TEXM3x3SPEC: case TEXM3x3VSPEC:
			{
				const uint32_t n = d.reg();
				body += fmt("\t{\n\t\tfloat3 m = float3(dot(%s.xyz, (%s).xyz), dot(%s.xyz, (%s).xyz), dot(%s.xyz, (%s).xyz));\n",
					coords(n - 2).c_str(), s(0).c_str(), coords(n - 1).c_str(), s(0).c_str(), coords(n).c_str(), s(0).c_str());
				if (in.opcode == TEXM3x3)
					body += fmt("\t\tt%u = float4(m, 1.0);\n", n);
				else if (in.opcode == TEXM3x3TEX)
					body += fmt("\t\tt%u = %s;\n", n, sample(n, "float4(m, 0)").c_str());
				else
				{
					// (the eye vector: a constant, or the three coordinates' w)
					const std::string e = in.opcode == TEXM3x3SPEC ? "(" + s(1) + ").xyz" : fmt("float3(%s.w, %s.w, %s.w)", coords(n - 2).c_str(), coords(n - 1).c_str(), coords(n).c_str());
					body += fmt("\t\tconst float3 e = %s;\n\t\tconst float3 rv = 2.0 * dot(m, e) / dot(m, m) * m - e;\n", e.c_str());
					body += fmt("\t\tt%u = %s;\n", n, sample(n, "float4(rv, 0)").c_str());
				}
				body += "\t}\n";
				return;
			}
			case TEXDP3TEX: body += fmt("\tt%u = %s;\n", d.reg(), sample(d.reg(), fmt("float4(dot(%s.xyz, (%s).xyz), 0, 0, 0)", coords(d.reg()).c_str(), s(0).c_str())).c_str()); return;
			case TEXDP3: body += fmt("\tt%u = dot(%s.xyz, (%s).xyz).xxxx;\n", d.reg(), coords(d.reg()).c_str(), s(0).c_str()); return;
			case TEXDEPTH:
				log_once(1, "ps.1.4 texdepth: depth output not written");
				return;
			default:
				ok = false;
				why = fmt("opcode %u", in.opcode);
				return;
			}
		}

		const char *kVsHelpers = R"(
float rcp1(float x) { return x == 1.0 ? 1.0 : 1.0 / x; }
float rsq1(float x) { const float a = abs(x); return a == 1.0 ? 1.0 : rsqrt(a); }
float log1(float x) { const float a = abs(x); return a != 0.0 ? log2(a) : -3.402823466e+38; }
float4 expp1(float x) { const float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1.0); }
float4 logp1(float x)
{
	const float a = abs(x);
	if (a == 0.0)
		return float4(-3.402823466e+38, 1.0, -3.402823466e+38, 1.0);
	const float e = floor(log2(a));
	return float4(e, a / exp2(e), log2(a), 1.0);
}
float4 lit1(float4 s)
{
	float4 d = float4(1.0, 0.0, 0.0, 1.0);
	if (s.x > 0.0)
	{
		d.y = s.x;
		const float y = max(s.y, 0.0), p = clamp(s.w, -128.0, 128.0);
		d.z = y > 0.0 ? exp2(p * log2(y)) : (p == 0.0 ? 1.0 : 0.0);
	}
	return d;
}
)";
	}

	size_t sm1_length(const DWORD *function)
	{
		std::vector<instr> ins;
		uint32_t version = 0;
		std::string why;
		size_t n = 0;
		return parse(function, ins, version, why, &n) ? n : 0;
	}

	bool read_sm1(const DWORD *function, sm1_shader &out)
	{
		std::vector<instr> ins;
		size_t n = 0;
		out = sm1_shader();
		if (!parse(function, ins, out.version, out.why, &n))
			return false;
		out.tokens.assign(function, function + n);
		out.pixel = (out.version & 0xFFFF0000) == 0xFFFF0000;
		const uint32_t major = (out.version >> 8) & 0xFF, minor = out.version & 0xFF;
		if (major != 1 || (out.pixel ? minor > 4 : minor > 1))
		{
			out.why = fmt("%s.%u.%u isn't shader model 1", out.pixel ? "ps" : "vs", major, minor);
			return false;
		}
		for (const instr &in : ins)
		{
			if (in.opcode == DEF)
			{
				sm1_shader::def d;
				d.reg = in.dst.reg();
				std::memcpy(d.v, in.def, 16);
				out.defs.push_back(d);
				continue;
			}
			for (uint32_t i = 0; i < in.nsrc; ++i)
			{
				const param &p = in.src[i];
				if (p.type() == R_CONST)
					out.max_const = std::max<uint32_t>(out.max_const, p.relative() ? kVsConstRegs - 1 : p.reg() + (i == 1 && in.opcode >= M4x4 && in.opcode <= M3x2 ? 3 : 0));
				if (!out.pixel && p.type() == R_INPUT)
					out.inputs_read |= uint16_t(1u << (p.reg() & 15));
			}
			if (out.pixel && in.has_dst && (in.opcode >= TEXCOORD && in.opcode <= BEM) && in.opcode != DEF && in.opcode != CND && in.opcode != CMP)
				out.textures_read |= uint8_t(1u << (in.dst.reg() & 7));
		}
		out.ok = true;
		return true;
	}

	std::string sm1_vs_hlsl(const sm1_shader &sh, const uint8_t inputs[16], uint8_t clip_planes)
	{
		std::vector<instr> ins;
		uint32_t version = 0;
		std::string why;
		parse(sh.tokens.data(), ins, version, why, nullptr);
		writer w;
		w.pixel = false;
		w.shader = &sh;
		for (const sm1_shader::def &d : sh.defs)
			w.defs.insert(d.reg);

		std::string s = "cbuffer vsc : register(b0)\n{\n\tfloat4 c[256];\n\tfloat4 zr;\n\tfloat4 clipp[6];\n};\n";
		s += kVsHelpers;
		s += "float4 cread(int i) { return i >= 0 && i < 256 ? c[i] : float4(0, 0, 0, 0); }\n";
		for (const sm1_shader::def &d : sh.defs)
			s += fmt("static const float4 k%u = float4(%.9g, %.9g, %.9g, %.9g);\n", d.reg, d.v[0], d.v[1], d.v[2], d.v[3]);
		s += hlsl_vin(inputs);
		s += hlsl_vout(false, false, clip_planes);
		s += "VOut main(VIn a)\n{\n\tVOut o = (VOut)0;\n";
		s += hlsl_vin_read(inputs);
		s += "\tfloat4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0;\n";
		s += "\tint a0 = 0;\n\tfloat4 oPos = 0, oFog = 1, oPts = 0, oD0 = 0, oD1 = 0;\n";
		s += "\tfloat4 oT0 = float4(0, 0, 0, 1), oT1 = oT0, oT2 = oT0, oT3 = oT0, oT4 = oT0, oT5 = oT0, oT6 = oT0, oT7 = oT0;\n";
		for (const instr &in : ins)
			w.emit(in);
		if (!w.ok)
			return std::string();
		s += w.body;
		s += "\to.pos = oPos;\n\to.pos.z = o.pos.z * zr.y + zr.x * o.pos.w;\n";
		s += "\to.d0 = saturate(oD0);\n\to.d1 = saturate(oD1);\n";
		for (int i = 0; i < 8; ++i)
			s += fmt("\to.t%d = oT%d;\n", i, i);
		s += "\to.fog = float4(saturate(oFog.x), zr.z != 0.0 ? oPos.w : oPos.z / oPos.w, 0, 0);\n";
		if (clip_planes != 0)
		{
			// (clip space, for shaders)
			const char *const dst[6] = { "o.cd0.x", "o.cd0.y", "o.cd0.z", "o.cd0.w", "o.cd1.x", "o.cd1.y" };
			for (int c = 0; c < 6; ++c)
				s += (clip_planes >> c) & 1 ? fmt("\t%s = dot(oPos, clipp[%d]);\n", dst[c], c) : fmt("\t%s = 1.0;\n", dst[c]);
		}
		s += "\treturn o;\n}\n";
		return s;
	}

	std::string sm1_ps_hlsl(const sm1_shader &sh, const uint8_t tex[8], uint8_t projected, const ps_tail &tail)
	{
		std::vector<instr> ins;
		uint32_t version = 0;
		std::string why;
		parse(sh.tokens.data(), ins, version, why, nullptr);
		writer w;
		w.pixel = true;
		w.minor = sh.version & 0xFF;
		w.shader = &sh;
		w.tex = tex;
		for (const sm1_shader::def &d : sh.defs)
			w.defs.insert(d.reg);

		std::string s = hlsl_ps_constants();
		static const char *const kinds[4] = { "Texture2D", "Texture2D", "TextureCube", "Texture3D" };
		for (int st = 0; st < 8; ++st)
			s += fmt("%s tex%d : register(t%d);\nSamplerState smp%d : register(s%d);\n", kinds[tex[st] & 3], st, st, st, st);
		for (const sm1_shader::def &d : sh.defs)
			s += fmt("static const float4 k%u = float4(%.9g, %.9g, %.9g, %.9g);\n", d.reg, d.v[0], d.v[1], d.v[2], d.v[3]);
		s += hlsl_vout(true, tail.flat != 0, 0);
		s += "float4 main(VOut i) : SV_Target\n{\n";
		s += "\tfloat4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;\n\tfloat4 v0 = i.d0, v1 = i.d1;\n";
		s += "\tfloat m3[3] = { 0, 0, 0 };\n\tuint m3n = 0;\n";
		const char *const coord[8] = { "i.t0", "i.t1", "i.t2", "i.t3", "i.t4", "i.t5", "i.t6", "i.t7" };
		for (int st = 0; st < 8; ++st)
		{
			// (the stages' coordinates, divided by their last element when projected)
			s += fmt("\tfloat4 tc%d = %s;\n", st, coord[st]);
			if (!w.ps14() && ((projected >> st) & 1))
				s += fmt("\ttc%d.xyz /= tc%d.w;\n", st, st);
		}
		if (w.ps14())
			s += "\tfloat4 t0 = tc0, t1 = tc1, t2 = tc2, t3 = tc3, t4 = tc4, t5 = tc5;\n";
		else
			s += "\tfloat4 t0 = 0, t1 = 0, t2 = 0, t3 = 0;\n";
		// Co-issued pairs: the second reads what the first read (both its sources before either is written)
		for (size_t n = 0; n < ins.size(); ++n)
		{
			const instr &in = ins[n];
			if (n + 1 < ins.size() && ins[n + 1].coissue && in.has_dst && ins[n + 1].has_dst)
			{
				// (both halves computed into copies of their registers, then written: neither sees the other's result)
				const instr &b = ins[n + 1];
				w.body.clear();
				s += "\t{\n";
				w.redirect = "coa";
				w.emit(in);
				w.redirect = "cob";
				w.emit(b);
				w.redirect.clear();
				s += w.body;
				const std::string ma = w.mask_of(in.dst), mb = w.mask_of(b.dst);
				s += fmt("\t%s%s = coa%s;\n\t%s%s = cob%s;\n\t}\n", w.reg_name(in.dst, true).c_str(), ma.c_str(), ma.c_str(), w.reg_name(b.dst, true).c_str(), mb.c_str(), mb.c_str());
				++n;
				continue;
			}
			w.body.clear();
			w.emit(in);
			s += w.body;
		}
		if (!w.ok)
			return std::string();
		s += "\tfloat4 col = saturate(r0);\n";
		s += hlsl_ps_tail(tail);
		s += "}\n";
		return s;
	}
}
