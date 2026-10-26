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
#include <algorithm>
#include <cmath>
#include <cstring>


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
};
StructuredBuffer<vertex_t> vertices : register(t0);
StructuredBuffer<corner_t> corners : register(t1);
Texture2D colour_map : register(t2);
StructuredBuffer<bone_t> palette : register(t3);
StructuredBuffer<model_t> models : register(t4);
SamplerState clamp_linear : register(s0);

static const float kMaterialDiffuse = 0.5;

float3 bone_point(uint b, float4 p)
{
	const bone_t m = palette[min(b, bone_count - 1)];
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

// Direct3D 8's fixed-function colour for a normal (the game's states: directional lights, no specular, no emissive)
float4 lit(model_t m, float3 n)
{
	float3 diffuse = 0, light_ambient = 0;
	[unroll] for (int k = 0; k < 4; ++k)
	{
		diffuse += m.light_dir[k].w * m.light_diffuse[k].rgb * light_scale * max(0, dot(n, m.light_dir[k].xyz));
		light_ambient += m.light_dir[k].w * m.light_ambient[k].rgb;
	}
	const float3 c = m.material_ambient.rgb * (m.ambient.rgb * light_scale + light_ambient) + kMaterialDiffuse * diffuse;
	return float4(saturate(c), kMaterialDiffuse);
}

struct v2p
{
	float4 pos : SV_Position;
	float4 colour : COLOR0;
	float2 uv : TEXCOORD0;
	float fog : TEXCOORD1;
	float3 normal : TEXCOORD2;
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
	const model_t mo = models[min(model_index, (uint)options.y - 1)];
	const float4 f = mo.fog.w >= 0 ? mo.fog : fog;
	const float e = mo.fog.w >= 0 ? mo.fog_end.x : fog_end.x;
	r.fog = f.w > 0 ? saturate((e - r.pos.w) * f.w) : 1;
	r.uv = c.uv;
	r.normal = nn;
	r.colour = lit(models[min(model_index, (uint)options.y - 1)], nn);
	return r;
}

float4 ps(v2p i) : SV_Target
{
	const model_t m = models[min(model_index, (uint)options.y - 1)];
	const float4 t = colour_map.Sample(clamp_linear, i.uv);
	const float4 colour = options.x > 0 ? lit(m, normalize(i.normal)) : i.colour;
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
	return float4(lerp(m.fog.w >= 0 ? m.fog.rgb : fog.rgb, rgb, i.fog), a);
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
		};
		static_assert(sizeof(gpu_model) == 272, "the shader's model_t");
		struct gpu_constants
		{
			float view_proj[16];
			float fog[4];
			float fog_end[4];
			float options[4];
		};
		static_assert(sizeof(gpu_constants) <= kConstantsBytes);
		constexpr UINT64 kSlotBytes = (kConstantsBytes + UINT64(kMaxModels) * sizeof(gpu_model) + 255) & ~UINT64(255);
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

		void model_constants(const model_entry &m, gpu_model &g)
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
		}

		enum kind
		{
			kModel = 0,
			kComposite = 1,
		};
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
		if (_ring != nullptr && _mapped != nullptr)
			_ring->Unmap(0, nullptr);
		_mapped = nullptr;
		drop(_ring);
		drop(_root);
		for (void **b : { &_vs, &_ps, &_cvs, &_cps, &_cps_ms })
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
		const entry entries[] = { { "vs", "vs_5_0", &_vs }, { "ps", "ps_5_0", &_ps }, { "cvs", "vs_5_0", &_cvs }, { "cps", "ps_5_0", &_cps }, { "cps_ms", "ps_5_0", &_cps_ms } };
		for (const entry &e : entries)
		{
			ID3DBlob *code = nullptr, *errors = nullptr;
			if (FAILED(compile(kShader, std::strlen(kShader), "hifixi_chars", nullptr, nullptr, e.name, e.target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
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
		D3D12_ROOT_PARAMETER p[6] = {};
		p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		p[0].Descriptor.ShaderRegister = 0;
		p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		p[1].Constants.ShaderRegister = 1;
		p[1].Constants.Num32BitValues = 8;
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
		D3D12_STATIC_SAMPLER_DESC s = {};
		s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		s.MaxLOD = D3D12_FLOAT32_MAX;
		s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		const D3D12_ROOT_SIGNATURE_DESC rd = { 6, p, 1, &s, D3D12_ROOT_SIGNATURE_FLAG_NONE };
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
		const uint64_t key = uint64_t(t.rtv_format) | uint64_t(t.dsv_format) << 8 | uint64_t(t.samples) << 16 | uint64_t(k) << 24;
		const auto known = _pipelines.find(key);
		if (known != _pipelines.end())
			return known->second;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		d.pRootSignature = _root;
		ID3DBlob *const vs = static_cast<ID3DBlob *>(k == kModel ? _vs : _cvs);
		ID3DBlob *const ps = static_cast<ID3DBlob *>(k == kModel ? _ps : t.samples > 1 ? _cps_ms : _cps);
		d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		d.NumRenderTargets = 1;
		d.RTVFormats[0] = t.rtv_format;
		d.SampleDesc.Count = t.samples;
		d.SampleMask = UINT_MAX;
		d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // (the character pass: CULL NONE)
		d.RasterizerState.DepthClipEnable = TRUE;
		d.RasterizerState.MultisampleEnable = t.samples > 1;
		D3D12_RENDER_TARGET_BLEND_DESC &b = d.BlendState.RenderTarget[0];
		b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		if (k == kModel)
		{
			d.DSVFormat = t.dsv_format;
			d.DepthStencilState.DepthEnable = TRUE;
			d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
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

	bool char_renderer::begin(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, const char_options &o)
	{
		ID3D12DescriptorHeap *const heap = cache::heap();
		ID3D12PipelineState *const pso = pipeline(t, kModel);
		if (_root == nullptr || heap == nullptr || pso == nullptr || !f.camera || f.models == 0)
			return false;
		// The frame's constants (rewritten for each draw of the frame: the same values) and its models'
		const uint32_t seq = f.seq.load(std::memory_order_acquire);
		const UINT64 slot = UINT64(seq % kFrames) * kSlotBytes;
		gpu_constants c = {};
		std::memcpy(c.view_proj, f.view_proj, sizeof(c.view_proj));
		const float span = f.fog_end - f.fog_start;
		c.fog[0] = f.fog[0];
		c.fog[1] = f.fog[1];
		c.fog[2] = f.fog[2];
		c.fog[3] = span > 1e-3f ? 1.0f / span : 0.0f;
		c.fog_end[0] = f.fog_end;
		c.options[0] = o.per_pixel ? 1.0f : 0.0f;
		c.options[1] = float(std::min(f.models, kMaxModels));
		std::memcpy(_mapped + slot, &c, sizeof(c));
		gpu_model *gm = reinterpret_cast<gpu_model *>(_mapped + slot + kModelsAt);
		for (uint32_t i = 0; i < std::min(f.models, kMaxModels); ++i)
			model_constants(f.model[i], gm[i]);
		const D3D12_GPU_VIRTUAL_ADDRESS base = _ring->GetGPUVirtualAddress() + slot;

		list->OMSetRenderTargets(1, &t.rtv, FALSE, &t.dsv);
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

	void char_renderer::draw_model(ID3D12GraphicsCommandList *list, const char_target &, const frame &f, uint32_t mi, char_stats &st)
	{
		const model_entry &m = f.model[mi];
		const D3D12_GPU_VIRTUAL_ADDRESS palettes = cache::palettes(f.seq.load(std::memory_order_relaxed));
		if (palettes == 0)
			return;
		st.models++;
		for (uint32_t ii = 0; ii < m.instances && m.first_instance + ii < kMaxInstances; ++ii)
		{
			const instance_entry &ie = f.instance[m.first_instance + ii];
			if (ie.bones == 0)
				continue;
			list->SetGraphicsRootShaderResourceView(2, palettes + UINT64(ie.first_bone) * sizeof(bone));
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
					uint32_t dc[8] = { mi, 0, shine, mv.vertices, ie.bones, 0, gr.first, mv.corners };
					std::memcpy(&dc[1], &scale, 4);
					list->SetGraphicsRoot32BitConstants(1, 8, dc, 0);
					list->DrawInstanced(gr.count, 1, 0, 0);
					st.draws++;
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
		for (uint32_t i = 0; i < opaque; ++i)
			if ((f.model[i].flags & kNative) != 0 && (f.model[i].colour >> 24) != 0)
				draw_model(list, t, f, i, st);
		return st;
	}

	char_stats char_renderer::record_model(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, uint32_t model, const char_options &o)
	{
		char_stats st;
		if (model >= std::min(f.models, kMaxModels) || !begin(list, t, f, o))
			return st;
		draw_model(list, t, f, model, st);
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
		const uint32_t dc[8] = { 0, 0, 0, 0, 0, colour, 0, 0 };
		list->SetGraphicsRoot32BitConstants(1, 8, dc, 0);
		list->SetGraphicsRootDescriptorTable(5, srv);
		list->DrawInstanced(3, 1, 0, 0);
	}
}
