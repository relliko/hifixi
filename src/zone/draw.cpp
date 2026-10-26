/*
 * The native zone renderer's drawing (draw.hpp). One root signature:
 *   0  CBV b0      the frame's constants (the upload ring's slot)
 *   1  4 constants b1: the object, the piece's first index, its flags
 *   2  SRV t0      the zone's vertices (48 bytes)        3  SRV t1  its indices (32-bit) (the second map's for its objects)
 *   4  SRV t2      the frame's objects (80 bytes)        5  SRV t3  its lamps (32 bytes)
 *   6  table t4    the piece's texture                   7  table t5  its normal map
 *   s0             linear, wrapping (the zone pass draws without mips: MIPFILTER NONE)
 * Pipelines are made as targets of new formats or sample counts come, one for each (blended, cull, door) set.
 */

#include <windows.h>
#include <d3dcompiler.h>
#include "draw.hpp"
#include "zone_gpu.hpp"
#include <algorithm>
#include <cstring>

namespace hifi::zone
{
	namespace
	{
		namespace zm = bake::zonemesh;

		const char *kShader = R"(
// A lighting environment (frame.hpp's frame_env): the zone's own is 0
struct env_t
{
	float4 sun_dir;      // toward the sun, world
	float4 sun;          // fixed-function pieces' sun colour
	float4 sun_bump;     // bump pieces' (c6)
	float4 ambient;      // fixed-function pieces' ambient
	float4 ambient_bump; // bump pieces' (c7)
	float4 fog;          // rgb; w: scale / (end - start)
	float4 fog_end;      // x: end
};
cbuffer frame_constants : register(b0)
{
	row_major float4x4 view_proj;
	float4 options;      // x: per-pixel lighting; yzw: the camera (the fixed-function pieces' range fog)
	env_t envs[8];
};
cbuffer draw_constants : register(b1)
{
	uint object_index;
	uint first_index;
	uint piece;          // kBump, kTested, kDoorEnv, kMapped (a normal map: used per pixel)
	uint unused;
};
struct vertex { float3 pos; float3 normal; uint colour; float2 uv; float3 sway; };
struct object { float4 c0; float4 c1; float4 c2; uint flags; uint model; float sway; uint env; uint4 lamp; }; // (env: its own | a door's << 8)
struct lamp_t { float3 pos; float range; float3 colour; float att2; };
StructuredBuffer<vertex> vertices : register(t0);
StructuredBuffer<uint> indices : register(t1);
StructuredBuffer<object> objects : register(t2);
StructuredBuffer<lamp_t> lamps : register(t3);
Texture2D colour_map : register(t4);
Texture2D normal_map : register(t5);
SamplerState wrap : register(s0);

static const uint kBump = 1, kTested = 2, kDoorEnv = 4, kMapped = 8, kMaxLamps = 64; // (frame.hpp's kMaxLamps)

// The piece's environment: its object's, or for a door's bit-28 piece the door's (never past the 8 there are)
env_t env_of(object o)
{
	return envs[((piece & kDoorEnv) != 0 ? o.env >> 8 : o.env) & 7];
}

float3 to_world(object o, float3 p) { return float3(dot(float4(p, 1), o.c0), dot(float4(p, 1), o.c1), dot(float4(p, 1), o.c2)); }
// A direction through the inverse transpose of the object's matrix, not normalized: how Direct3D 8 carries a normal
// into its lighting (NORMALIZENORMALS off in the zone pass, 17C21A), and what the bump shader's object-space light
// (c4, the sun through the object's inverse matrix) comes to; an object scaled by s is lit 1 / s as brightly
float3 dir_light(object o, float3 d)
{
	const float3 r0 = float3(o.c0.x, o.c1.x, o.c2.x), r1 = float3(o.c0.y, o.c1.y, o.c2.y), r2 = float3(o.c0.z, o.c1.z, o.c2.z);
	const float3 a = cross(r1, r2), b = cross(r2, r0), c = cross(r0, r1);
	const float det = dot(r0, a);
	return (d.x * a + d.y * b + d.z * c) / (abs(det) > 1e-12 ? det : 1e-12);
}
float4 unpack(uint c) { return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0; }

// The object's lamps (point lights: attenuation 1 / (a2 d^2) within their range). The game's light stops dead at its
// range, still at an eighth of its colour (a2 0.5, range 3-6): a hard circle round each lamp that the eye takes for a
// dark ring past it (in game, 2026-10-26; the game's own drawing has it too, softened by its vertices). Here it fades
// to none over the outer 40% of the range instead.
float3 lamp_light(object o, float3 world, float3 n)
{
	float3 sum = 0;
	[unroll] for (int k = 0; k < 4; ++k)
	{
		// (an index past the frame's lamps, 0xFFFFFFFF for none, is never read: the compiler may load before the test,
		// and a root SRV has no bounds: on the GPU such a read went to a wild address and hung it. Lamp 0 is read instead.)
		const uint i = o.lamp[k];
		const bool used = i < kMaxLamps;
		const lamp_t l = lamps[used ? i : 0];
		if (!used)
			continue;
		const float3 d = l.pos - world;
		const float d2 = dot(d, d);
		const float r2 = l.range * l.range;
		if (d2 > r2)
			continue;
		const float edge = 1.0 - smoothstep(0.36, 1.0, d2 / max(r2, 1e-6));
		sum += edge * l.colour * max(0, dot(n, d * rsqrt(max(d2, 1e-6)))) / max(l.att2 * d2, 1e-3);
	}
	return sum;
}

struct v2p
{
	float4 pos : SV_Position;
	float4 lit : COLOR0;       // fixed-function: the lit colour (saturated), vertex alpha
	float4 vcol : COLOR1;      // the vertex colour
	float3 light_ts : TEXCOORD0; // bump: the sun in the game's tangent frame (T.L, B.L, N.L)
	float3 lamps : TEXCOORD1;    // bump: vertex colour x the lamps
	float2 uv : TEXCOORD2;
	float fog : TEXCOORD3;
	float3 world : TEXCOORD4;
	float3 normal : TEXCOORD5;
};

v2p vs(uint vid : SV_VertexID)
{
	const object o = objects[object_index];
	const uint tri = vid / 3, corner = vid - tri * 3;
	const uint i0 = indices[first_index + tri * 3], i1 = indices[first_index + tri * 3 + 1], i2 = indices[first_index + tri * 3 + 2];
	const vertex v = vertices[corner == 0 ? i0 : corner == 1 ? i1 : i2];
	const float3 world = to_world(o, v.pos + v.sway * o.sway);
	const float3 n = dir_light(o, v.normal);
	const env_t e = env_of(o);
	v2p r;
	r.pos = mul(float4(world, 1), view_proj);
	// (the fixed-function pieces' fog by the distance from the camera, RANGEFOGENABLE as dgVoodoo reports FOGRANGE; the
	// bump shader's by the view depth, its oFog from w)
	const float fog_at = (piece & kBump) != 0 ? r.pos.w : distance(world, options.yzw);
	r.fog = saturate((e.fog_end.x - fog_at) * e.fog.w);
	r.uv = v.uv;
	r.world = world;
	r.normal = n;
	r.vcol = unpack(v.colour);
	const float3 lamp_rgb = lamp_light(o, world, n);
	r.lamps = r.vcol.rgb * lamp_rgb;
	r.light_ts = 0;
	if ((piece & kBump) != 0)
	{
		// 170B30: per triangle, D = (p2 - p1)(u0 - u1) - (p0 - p1)(u2 - u1), the direction u is constant along, the
		// same for each corner; T = D x N; the shader's B = N x T
		const vertex a = vertices[i0], b = vertices[i1], c = vertices[i2];
		const float3 D = (c.pos - b.pos) * (a.uv.x - b.uv.x) - (a.pos - b.pos) * (c.uv.x - b.uv.x);
		const float l2 = dot(D, D);
		const float3 Dn = l2 > 0 ? D * rsqrt(l2) : 0;
		const float3 T = cross(Dn, v.normal), B = cross(v.normal, T);
		// (oD1 is a colour: each of them clamped to -1..1 as it is)
		r.light_ts = clamp(float3(dot(dir_light(o, T), e.sun_dir.xyz), dot(dir_light(o, B), e.sun_dir.xyz), dot(n, e.sun_dir.xyz)), -1.0, 1.0);
		r.lit = r.vcol;
	}
	else
		r.lit = float4(saturate(r.vcol.rgb * (e.sun.rgb * max(0, dot(n, e.sun_dir.xyz)) + e.ambient.rgb) + r.lamps), r.vcol.a);
	return r;
}

float4 ps(v2p i) : SV_Target
{
	const float4 t = colour_map.Sample(wrap, i.uv);
	const float a = saturate(4 * t.a * i.vcol.a);
	if ((piece & kTested) != 0 && a <= 96.0 / 255.0)
		discard;
	float3 lit;
	const bool bump = (piece & kBump) != 0;
	const env_t e = env_of(objects[object_index]);
	if (options.x > 0)
	{
		// Per pixel: the sun and the lamps at each pixel; a normal map in the frame its UVs give (its green is the
		// slope along u, its red along v)
		// (the normal's length kept: the object's scale in its light, as per vertex)
		const float scale = length(i.normal);
		float3 n = i.normal / max(scale, 1e-6);
		// (every piece with a normal map uses it here, not only the bump shader's models: this is HiFi XI's own look)
		if ((piece & kMapped) != 0)
		{
			const float3 m = normal_map.Sample(wrap, i.uv).rgb * 2 - 1;
			const float3 dp1 = ddx(i.world), dp2 = ddy(i.world);
			const float2 du1 = ddx(i.uv), du2 = ddy(i.uv);
			const float3 p1 = cross(dp2, n), p2 = cross(n, dp1);
			float3 U = p1 * du1.x + p2 * du2.x, V = p1 * du1.y + p2 * du2.y;
			const float s = rsqrt(max(max(dot(U, U), dot(V, V)), 1e-20));
			n = normalize(m.g * U * s + m.r * V * s + m.b * n);
		}
		n *= scale;
		const float3 s_rgb = bump ? e.sun_bump.rgb : e.sun.rgb, a_rgb = bump ? e.ambient_bump.rgb : e.ambient.rgb;
		lit = saturate(i.vcol.rgb * (s_rgb * saturate(dot(n, e.sun_dir.xyz)) + a_rgb) + i.vcol.rgb * lamp_light(objects[object_index], i.world, n));
	}
	else if (bump)
	{
		const float3 m = normal_map.Sample(wrap, i.uv).rgb * 2 - 1;
		// (ps.1.1: texcoord t2 and t3, the sun and the ambient plus lamps each times the vertex colour, clamp to 0..1)
		const float3 t2 = saturate(i.vcol.rgb * e.sun_bump.rgb), t3 = saturate(i.vcol.rgb * e.ambient_bump.rgb + i.lamps);
		lit = saturate(t2 * saturate(dot(m, i.light_ts)) + t3);
	}
	else
		lit = i.lit.rgb;
	return float4(lerp(e.fog.rgb, 2 * lit * t.rgb, i.fog), a);
}
)";

		constexpr uint32_t kBump = 1, kTested = 2, kDoorEnv = 4, kMapped = 8;
		constexpr UINT64 kConstantsBytes = 1024;
		// Blended pieces' depth bias: the game sends ZBIAS 8 for them (notes-zone, "Blended pieces"), which dgVoodoo 2.87
		// makes DepthBias trunc(-2.5 ZBIAS) and SlopeScaledDepthBias -0.5 ZBIAS, no clamp (measured: dgvprobe ZBIAS_TEST)
		constexpr INT kBlendedDepthBias = -20;
		constexpr float kBlendedSlopeBias = -4.0f;
		struct gpu_object
		{
			float c[12];
			uint32_t flags, model;
			float sway;
			uint32_t env; // (its own | a door's << 8)
			uint32_t lamps[4];
		};
		static_assert(sizeof(gpu_object) == 80);
		static_assert(kMaxLamps == 64); // (the shader's kMaxLamps)
		struct gpu_env
		{
			float sun_dir[4], sun[4], sun_bump[4], ambient[4], ambient_bump[4], fog[4], fog_end[4];
		};
		struct gpu_constants
		{
			float view_proj[16];
			float options[4];
			gpu_env envs[kMaxEnvs];
		};
		static_assert(kMaxEnvs == 8); // (the shader's envs[8])
		static_assert(sizeof(gpu_constants) <= kConstantsBytes);
		constexpr UINT64 kSlotBytes = (kConstantsBytes + UINT64(kMaxObjects) * sizeof(gpu_object) + UINT64(kMaxLamps) * sizeof(frame_lamp) + 255) & ~UINT64(255);
		constexpr UINT64 kObjectsAt = kConstantsBytes, kLampsAt = kConstantsBytes + UINT64(kMaxObjects) * sizeof(gpu_object);

		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		void put3(float *to, const float *from, float w = 0.0f)
		{
			to[0] = from[0];
			to[1] = from[1];
			to[2] = from[2];
			to[3] = w;
		}
	}

	zone_renderer::~zone_renderer()
	{
		release();
	}

	void zone_renderer::release()
	{
		for (auto &p : _pipelines)
			if (p.second != nullptr)
				p.second->Release();
		_pipelines.clear();
		if (_ring != nullptr && _mapped != nullptr)
			_ring->Unmap(0, nullptr);
		_mapped = nullptr;
		drop(_ring);
		drop(_root);
		if (_vs != nullptr)
			static_cast<ID3DBlob *>(_vs)->Release();
		if (_ps != nullptr)
			static_cast<ID3DBlob *>(_ps)->Release();
		_vs = _ps = nullptr;
		_device = nullptr;
	}

	bool zone_renderer::init(ID3D12Device *device, std::string &why)
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
		for (int k = 0; k < 2; ++k)
		{
			ID3DBlob *code = nullptr, *errors = nullptr;
			if (FAILED(compile(kShader, std::strlen(kShader), "hifixi_zone", nullptr, nullptr, k == 0 ? "vs" : "ps", k == 0 ? "vs_5_0" : "ps_5_0",
					D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
			{
				why = std::string("the zone shader didn't compile: ") + (errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
				if (errors != nullptr)
					errors->Release();
				release();
				return false;
			}
			if (errors != nullptr)
				errors->Release();
			(k == 0 ? _vs : _ps) = code;
		}

		D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4 }, { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5 } };
		D3D12_ROOT_PARAMETER p[8] = {};
		p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		p[0].Descriptor.ShaderRegister = 0;
		p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		p[1].Constants.ShaderRegister = 1;
		p[1].Constants.Num32BitValues = 4;
		for (UINT i = 0; i < 4; ++i)
		{
			p[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			p[2 + i].Descriptor.ShaderRegister = i;
		}
		for (UINT i = 0; i < 2; ++i)
		{
			p[6 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			p[6 + i].DescriptorTable.NumDescriptorRanges = 1;
			p[6 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
			p[6 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		}
		D3D12_STATIC_SAMPLER_DESC s = {};
		s.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		s.MaxLOD = D3D12_FLOAT32_MAX;
		s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		const D3D12_ROOT_SIGNATURE_DESC rd = { 8, p, 1, &s, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		using serialize_fn = decltype(&D3D12SerializeRootSignature);
		const auto serialize = reinterpret_cast<serialize_fn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
		ID3DBlob *blob = nullptr, *errors = nullptr;
		if (serialize == nullptr || FAILED(serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
			FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_root))))
		{
			why = "the zone's root signature couldn't be made";
			if (blob != nullptr)
				blob->Release();
			if (errors != nullptr)
				errors->Release();
			release();
			return false;
		}
		blob->Release();

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
			why = "the zone's upload ring couldn't be made";
			release();
			return false;
		}
		return true;
	}

	ID3D12PipelineState *zone_renderer::pipeline(const draw_target &t, bool blended, int cull, bool door)
	{
		const uint64_t key = uint64_t(t.rtv_format) | uint64_t(t.dsv_format) << 8 | uint64_t(t.samples) << 16 | uint64_t(blended) << 24 | uint64_t(cull) << 25 |
			uint64_t(door) << 27;
		const auto known = _pipelines.find(key);
		if (known != _pipelines.end())
			return known->second;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		d.pRootSignature = _root;
		d.VS = { static_cast<ID3DBlob *>(_vs)->GetBufferPointer(), static_cast<ID3DBlob *>(_vs)->GetBufferSize() };
		d.PS = { static_cast<ID3DBlob *>(_ps)->GetBufferPointer(), static_cast<ID3DBlob *>(_ps)->GetBufferSize() };
		d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		d.NumRenderTargets = 1;
		d.RTVFormats[0] = t.rtv_format;
		d.DSVFormat = t.dsv_format;
		d.SampleDesc.Count = t.samples;
		d.SampleMask = UINT_MAX;
		d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		// (Direct3D 8's CULL CCW drops counter-clockwise triangles: Direct3D 12's back faces; CW for mirrored objects)
		d.RasterizerState.CullMode = cull == 0 ? D3D12_CULL_MODE_NONE : cull == 1 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_FRONT;
		d.RasterizerState.DepthClipEnable = TRUE;
		d.RasterizerState.MultisampleEnable = t.samples > 1;
		// (the zone pass's blended pieces: no Z writes, ZBIAS 8; the doors pass sets only the blending, Z writes on, no bias)
		const bool zone_blended = blended && !door;
		d.RasterizerState.DepthBias = zone_blended ? kBlendedDepthBias : 0;
		d.RasterizerState.SlopeScaledDepthBias = zone_blended ? kBlendedSlopeBias : 0.0f;
		d.DepthStencilState.DepthEnable = TRUE;
		d.DepthStencilState.DepthWriteMask = zone_blended ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		D3D12_RENDER_TARGET_BLEND_DESC &b = d.BlendState.RenderTarget[0];
		b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		if (blended)
		{
			// (Direct3D 8 blends alpha as it blends colour)
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

	draw_stats zone_renderer::record(ID3D12GraphicsCommandList *list, const draw_target &t, const frame &f, const draw_options &o, uint64_t frame_number)
	{
		draw_stats st;
		if (_root == nullptr || f.zone == nullptr || !f.zone->ready())
			return st;
		gpu_zone *const zones[2] = { f.zone, f.second != nullptr && f.second->ready() ? f.second : nullptr };

		// The frame into its slot of the upload ring
		const UINT64 slot = (frame_number % kFrames) * kSlotBytes;
		uint8_t *at = _mapped + slot;
		gpu_constants c = {};
		std::memcpy(c.view_proj, f.view_proj, sizeof(c.view_proj));
		c.options[0] = o.per_pixel ? 1.0f : 0.0f;
		std::memcpy(c.options + 1, f.camera, 12);
		// (environment 0 is the frame's own fields; the rest its envs; any not filled are 0's)
		frame_env own;
		std::memcpy(own.sun_dir, f.sun_dir, 12);
		std::memcpy(own.sun, f.sun, 12);
		std::memcpy(own.sun_bump, f.sun_bump, 12);
		std::memcpy(own.ambient, f.ambient, 12);
		std::memcpy(own.ambient_bump, f.ambient_bump, 12);
		std::memcpy(own.fog, f.fog, 12);
		own.fog_start = f.fog_start;
		own.fog_end = f.fog_end;
		own.fog_scale = f.fog_scale;
		for (uint32_t i = 0; i < kMaxEnvs; ++i)
		{
			const frame_env &e = i == 0 ? own : i < f.env_count ? f.envs[i] : own;
			gpu_env &g = c.envs[i];
			put3(g.sun_dir, e.sun_dir);
			put3(g.sun, e.sun);
			put3(g.sun_bump, e.sun_bump);
			put3(g.ambient, e.ambient);
			put3(g.ambient_bump, e.ambient_bump);
			const float span = e.fog_end - e.fog_start;
			put3(g.fog, e.fog, span > 1e-3f ? e.fog_scale / span : 1e-30f); // (no span: no fog)
			g.fog_end[0] = span > 1e-3f ? e.fog_end : 1e30f;
		}
		std::memcpy(at, &c, sizeof(c));
		const uint32_t objects = std::min(f.count, kMaxObjects), lamp_count = std::min(f.lamp_count, kMaxLamps);
		gpu_object *go = reinterpret_cast<gpu_object *>(at + kObjectsAt);
		for (uint32_t i = 0; i < objects; ++i)
		{
			const frame_object &fo = f.objects[i];
			std::memcpy(go[i].c, fo.world, sizeof(go[i].c));
			go[i].flags = fo.flags;
			go[i].model = fo.model;
			go[i].sway = fo.sway;
			go[i].env = (fo.env < kMaxEnvs ? fo.env : 0) | (fo.door_env < kMaxEnvs ? fo.door_env : 0) << 8;
			std::memcpy(go[i].lamps, fo.lamps, sizeof(go[i].lamps));
		}
		std::memcpy(at + kLampsAt, f.lamps, size_t(lamp_count) * sizeof(frame_lamp));
		const D3D12_GPU_VIRTUAL_ADDRESS base = _ring->GetGPUVirtualAddress() + slot;

		// Targets, heaps, root
		list->OMSetRenderTargets(1, &t.rtv, FALSE, &t.dsv);
		const D3D12_VIEWPORT vp = { t.offset, t.offset, t.width, t.height, 0.0f, 1.0f };
		const D3D12_RECT sc = { 0, 0, LONG(t.width), LONG(t.height) };
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sc);
		list->SetGraphicsRootSignature(_root);
		list->SetGraphicsRootConstantBufferView(0, base);
		list->SetGraphicsRootShaderResourceView(4, base + kObjectsAt);
		list->SetGraphicsRootShaderResourceView(5, base + kLampsAt);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// The objects in the game's order, each one's pieces in file order (blended ones where they come)
		ID3D12PipelineState *bound = nullptr;
		gpu_zone *bound_zone = nullptr;
		for (uint32_t i = 0; i < objects; ++i)
		{
			const frame_object &fo = f.objects[i];
			gpu_zone *const z = fo.zone < 2 ? zones[fo.zone] : nullptr;
			if (z == nullptr || fo.model >= z->data().models.size())
			{
				st.skipped++;
				continue;
			}
			// (the zone's data or the second map's: its heap and buffers, when the object's are another's than the last's)
			if (z != bound_zone)
			{
				ID3D12DescriptorHeap *heaps[] = { z->heap() };
				list->SetDescriptorHeaps(1, heaps);
				list->SetGraphicsRootShaderResourceView(2, z->vertices()->GetGPUVirtualAddress());
				list->SetGraphicsRootShaderResourceView(3, z->indices()->GetGPUVirtualAddress());
				bound_zone = z;
			}
			const zm::zone &d = z->data();
			const bool door = (fo.flags & kDoor) != 0;
			st.objects++;
			const zm::model &mo = d.models[fo.model];
			for (uint32_t k = mo.first_piece; k < mo.first_piece + mo.pieces; ++k)
			{
				const zm::piece &pc = d.pieces[k];
				const bool blended = (pc.bits & zm::kBlended) != 0;
				const int cull = (pc.bits & zm::kTwoSided) != 0 ? 0 : (fo.flags & zm::kMirrored) != 0 ? 2 : 1;
				ID3D12PipelineState *const pso = pipeline(t, blended, cull, door);
				if (pso == nullptr)
					continue;
				if (pso != bound)
					list->SetPipelineState(bound = pso);
				// (doors take the fixed-function path, never the bump shader; their bit-28 pieces the door's environment)
				const uint32_t flags = (pc.normal_map != zm::kNone && mo.bump && !door && !f.no_bump ? kBump : 0) | ((fo.flags & zm::kAlphaTested) != 0 ? kTested : 0) |
					(door && (pc.bits & zm::kDoorsEnv) != 0 ? kDoorEnv : 0) | (pc.normal_map != zm::kNone && !door && !f.no_bump ? kMapped : 0);
				const uint32_t dc[4] = { i, pc.first_index, flags, 0 };
				list->SetGraphicsRoot32BitConstants(1, 4, dc, 0);
				list->SetGraphicsRootDescriptorTable(6, z->texture_srv(pc.texture));
				list->SetGraphicsRootDescriptorTable(7, z->normal_srv(pc.normal_map));
				list->DrawInstanced(pc.triangles * 3, 1, 0, 0);
				st.draws++;
				st.triangles += pc.triangles;
			}
		}
		return st;
	}
}
