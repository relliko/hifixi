/*
 * The native effect renderer (render.hpp). One shader for every class: the vertex shader reads the frame's vertices
 * (FVF 0x152) from a structured buffer and places them with WORLD (the element's draw matrix, view space) and its
 * PROJECTION; the pixel shader is the two texture stages of the class's state block, picked by the draw's stage bits
 * (frame.hpp stage_bits), then D3D8's fog. Pipelines by blend and z write (and the target's formats).
 *
 * Root signature: 0 48 constants b0 (below), 1 the vertices t0 (a root view of the upload ring), 2 a table: the
 * texture t1, 3 and 4 an MMB effect mesh's zone vertices t2 and indices t3 (root views of the zone's buffers: its
 * draws read their vertices through the zone's indices), 5 the frame's lights t4 (the upload ring, after the
 * vertices: a lit element's D3D8 fixed-function lighting in the vertex shader). Static sampler s0: MIN/MAG LINEAR, no mips (the game sets no
 * MIPFILTER), WRAP (the D3M runs' and 183760's ADDRESS WRAP; D3D8's default for the rest); s1 the same, MIRROR (the
 * distortion's: 353028 sets it on stage 0 and the fan after keeps it).
 *
 * A distortion grab (vs_grab / ps_grab, the same root signature: its quad's 4 corners in b0's first 16 constants, the
 * world's copy as the table's texture) draws into the 256 x 256 target with pre-transformed vertices as D3D9 places
 * them (pixel centres on whole numbers: + 0.5 here), no blend, no depth: colour the world's, alpha 0x80 (TFACTOR).
 *
 * An MMB effect mesh's textures are the zone's (its heap bound for that element, ours again after).
 */

#include <windows.h>
#include "fxrender.hpp"
#include "fxtextures.hpp"
#include "../zone/zone_gpu.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace hifi::fx
{
	namespace
	{
		const char *kShader = R"(
cbuffer element : register(b0)
{
	row_major float4x4 world; // the draw matrix: view space
	row_major float4x4 proj;
	float4 tfactor;           // TEXTUREFACTOR
	float4 fog_colour;        // rgb; a: 0 no fog, 1 by the view depth, 2 by the distance
	float4 misc;              // fog start, fog end, TEXTURE0's offset u, v
	uint4 flags;              // x the stage bits, y the first vertex, z the element's light (0xFFFFFFFF: unlit)
};
struct vtx
{
	float3 pos;
	float3 normal;
	uint colour;
	float2 uv;
};
struct zvtx
{
	float3 pos;
	float3 normal;
	uint colour;
	float2 uv;
	float3 sway;
};
StructuredBuffer<vtx> verts : register(t0);
Texture2D tex : register(t1);
StructuredBuffer<zvtx> zverts : register(t2);
StructuredBuffer<uint> zindices : register(t3);
struct lit
{
	float4 n0, n1, n2;
	float4 toward0, toward1;
	float4 diffuse0, diffuse1;
	float4 ambient;
	float4 emissive;
};
StructuredBuffer<lit> lits : register(t4);
SamplerState smp : register(s0);
SamplerState smp_mirror : register(s1);

struct v2p
{
	float4 pos : SV_Position;
	float4 col : COLOR0;
	float2 uv : TEXCOORD0;
	float fog : TEXCOORD1;
};

v2p vs(uint id : SV_VertexID)
{
	vtx v;
	if (flags.x & 256)
	{
		zvtx z = zverts[zindices[flags.y + id]];
		v.pos = z.pos;
		v.normal = z.normal;
		v.colour = z.colour;
		v.uv = z.uv;
	}
	else
		v = verts[flags.y + id];
	v2p o;
	float3 vp = mul(float4(v.pos, 1.0), world).xyz;
	o.pos = mul(float4(vp, 1.0), proj);
	uint c = v.colour;
	o.col = float4(float((c >> 16) & 255), float((c >> 8) & 255), float(c & 255), float(c >> 24)) / 255.0;
	if (flags.z != 0xFFFFFFFF)
	{
		// (D3D8's lighting: the vertex colour both materials, directional lights, no specular)
		lit l = lits[flags.z];
		float3 n = v.normal.x * l.n0.xyz + v.normal.y * l.n1.xyz + v.normal.z * l.n2.xyz;
		float3 light = l.ambient.rgb + l.diffuse0.rgb * max(0.0, dot(n, l.toward0.xyz)) + l.diffuse1.rgb * max(0.0, dot(n, l.toward1.xyz));
		o.col.rgb = saturate(o.col.rgb * light + l.emissive.rgb);
	}
	o.uv = v.uv + misc.zw;
	float d = fog_colour.a > 1.5 ? length(vp) : -vp.z;
	o.fog = fog_colour.a > 0.5 ? saturate((misc.y - d) / max(misc.y - misc.x, 1e-6)) : 1.0;
	return o;
}

float4 ps(v2p i) : SV_Target
{
	uint s = flags.x;
	float4 d = i.col;
	float3 c;
	float a;
	if (s & 128)
	{
		c = d.rgb;
		a = d.a;
	}
	else if (s & 512)
	{
		float4 t = tex.SampleLevel(smp, i.uv, 0);
		c = saturate(saturate(t.rgb * tfactor.rgb) * d.rgb * 4.0);
		a = saturate(saturate(t.a * tfactor.a * 2.0) * d.a * 4.0);
	}
	else if (s & 1024)
	{
		float4 t = tex.SampleLevel(smp, i.uv, 0);
		c = saturate(saturate(t.rgb * d.rgb) * tfactor.rgb * 4.0);
		a = saturate(d.a * tfactor.a * 4.0);
	}
	else if (s & 2048)
	{
		float4 t = tex.SampleLevel(smp_mirror, i.uv, 0);
		c = saturate(t.rgb * d.rgb * 2.0);
		a = saturate(d.a * tfactor.a * 4.0);
	}
	else if (s & 1)
	{
		float4 t = tex.SampleLevel(smp, i.uv, 0);
		float3 c0 = (s & 2) ? d.rgb : saturate(d.rgb * t.rgb * 2.0);
		float a0 = (s & 4) ? d.a : saturate(d.a * t.a * 2.0);
		c = (s & 8) ? tfactor.rgb : saturate(c0 * tfactor.rgb * 2.0);
		a = (s & 16) ? tfactor.a : saturate(a0 * tfactor.a * 4.0);
	}
	else
	{
		c = saturate(d.rgb * tfactor.rgb * 2.0);
		a = saturate(d.a * tfactor.a * ((s & 32) ? 2.0 : 4.0));
	}
	if ((s & 64) && a <= 96.0 / 255.0)
		discard;
	c = lerp(fog_colour.rgb, c, i.fog);
	return float4(c, a);
}

struct g2p
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

g2p vs_grab(uint id : SV_VertexID)
{
	// (the strip 0 1 2 3 as the list 0 1 2, 1 2 3; each corner a row of b0: x, y in the target's D3D9 pixels, u, v)
	static const uint corner[6] = { 0, 1, 2, 1, 2, 3 };
	float4 r = world[corner[id]];
	g2p o;
	o.pos = float4((r.x + 0.5) / 128.0 - 1.0, 1.0 - (r.y + 0.5) / 128.0, 0.0, 1.0);
	o.uv = r.zw;
	return o;
}

float4 ps_grab(g2p i) : SV_Target
{
	return float4(tex.SampleLevel(smp_mirror, i.uv, 0).rgb, 128.0 / 255.0);
}
)";

		constexpr UINT kConstants = 48;
		constexpr UINT64 kVertexBytes = UINT64(kMaxVertices) * sizeof(vertex);
		constexpr UINT64 kSlotBytes = kVertexBytes + UINT64(kMaxLit) * sizeof(lighting);

		float channel(uint32_t c, int shift)
		{
			return float((c >> shift) & 0xFF) / 255.0f;
		}

		D3D12_BLEND_DESC blend_desc(uint32_t blend)
		{
			D3D12_BLEND_DESC d = {};
			D3D12_RENDER_TARGET_BLEND_DESC &b = d.RenderTarget[0];
			b.BlendEnable = TRUE;
			b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			switch (blend)
			{
			case blend_one_zero:
				b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ONE;
				b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_ZERO;
				break;
			case blend_alpha:
				b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
				b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				break;
			case blend_darken:
				b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ZERO;
				b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				break;
			case blend_subtract:
				b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
				b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_ONE;
				b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_REV_SUBTRACT;
				break;
			case blend_multiply:
				// (D3D8 blends alpha with the same factors: DESTCOLOR's alpha is the target's)
				b.SrcBlend = D3D12_BLEND_DEST_COLOR;
				b.SrcBlendAlpha = D3D12_BLEND_DEST_ALPHA;
				b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_SRC_ALPHA;
				break;
			default: // blend_add
				b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
				b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_ONE;
				break;
			}
			return d;
		}
	}

	void fx_renderer::release()
	{
		for (auto &p : _pipelines)
			if (p.second != nullptr)
				p.second->Release();
		_pipelines.clear();
		for (IUnknown **u : { reinterpret_cast<IUnknown **>(&_grab_pso), reinterpret_cast<IUnknown **>(&_dist), reinterpret_cast<IUnknown **>(&_scene),
				 reinterpret_cast<IUnknown **>(&_dist_rtv) })
		{
			if (*u != nullptr)
				(*u)->Release();
			*u = nullptr;
		}
		_dist_format = DXGI_FORMAT_UNKNOWN;
		_scene_width = 0;
		_scene_height = 0;
		if (_ring != nullptr)
		{
			_ring->Release();
			_ring = nullptr;
		}
		_mapped = nullptr;
		if (_root != nullptr)
		{
			_root->Release();
			_root = nullptr;
		}
		for (void **b : { &_vs, &_ps, &_vs_grab, &_ps_grab })
		{
			if (*b != nullptr)
				static_cast<ID3DBlob *>(*b)->Release();
			*b = nullptr;
		}
		_device = nullptr;
	}

	bool fx_renderer::init(ID3D12Device *device, std::string &why)
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
		const entry entries[] = { { "vs", "vs_5_0", &_vs }, { "ps", "ps_5_0", &_ps }, { "vs_grab", "vs_5_0", &_vs_grab }, { "ps_grab", "ps_5_0", &_ps_grab } };
		for (const entry &e : entries)
		{
			ID3DBlob *code = nullptr, *errors = nullptr;
			if (FAILED(compile(kShader, std::strlen(kShader), "hifixi_fx", nullptr, nullptr, e.name, e.target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
			{
				why = std::string("the effects' shader didn't compile: ") + (errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
				if (errors != nullptr)
					errors->Release();
				release();
				return false;
			}
			if (errors != nullptr)
				errors->Release();
			*e.out = code;
		}

		D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1 };
		D3D12_ROOT_PARAMETER p[6] = {};
		p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		p[0].Constants.ShaderRegister = 0;
		p[0].Constants.Num32BitValues = kConstants;
		p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		p[1].Descriptor.ShaderRegister = 0;
		p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		p[2].DescriptorTable.NumDescriptorRanges = 1;
		p[2].DescriptorTable.pDescriptorRanges = &range;
		p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		for (UINT k = 3; k < 6; ++k)
		{
			p[k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			p[k].Descriptor.ShaderRegister = k - 1;
			p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		}
		D3D12_STATIC_SAMPLER_DESC s[2] = {};
		s[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		s[0].AddressU = s[0].AddressV = s[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		s[0].MaxLOD = 0.0f;
		s[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		s[1] = s[0];
		s[1].AddressU = s[1].AddressV = s[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
		s[1].ShaderRegister = 1;
		const D3D12_ROOT_SIGNATURE_DESC rd = { 6, p, 2, s, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		using serialize_fn = decltype(&D3D12SerializeRootSignature);
		const auto serialize = reinterpret_cast<serialize_fn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
		ID3DBlob *blob = nullptr, *errors = nullptr;
		if (serialize == nullptr || FAILED(serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
			FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_root))))
		{
			why = "the effects' root signature couldn't be made";
			if (blob != nullptr)
				blob->Release();
			if (errors != nullptr)
				errors->Release();
			_root = nullptr;
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
			why = "the effects' upload ring couldn't be made";
			release();
			return false;
		}
		_ring->SetName(L"hifixi: the effects' vertices");
		return true;
	}

	ID3D12PipelineState *fx_renderer::pipeline(const target &t, uint32_t blend, bool z_write)
	{
		const uint64_t key = uint64_t(t.rtv_format) | uint64_t(t.dsv_format) << 8 | uint64_t(t.samples) << 16 | uint64_t(blend) << 24 | uint64_t(z_write ? 1 : 0) << 32;
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
		d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // (0467E0: CULLMODE NONE)
		d.RasterizerState.DepthClipEnable = TRUE;
		d.RasterizerState.MultisampleEnable = t.samples > 1;
		d.BlendState = blend_desc(blend);
		d.DepthStencilState.DepthEnable = TRUE;
		d.DepthStencilState.DepthWriteMask = z_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		ID3D12PipelineState *pso = nullptr;
		if (FAILED(_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))))
			pso = nullptr;
		_pipelines[key] = pso;
		return pso;
	}

	// The distortion target (256 x 256, the world's format: as 044270 makes the game's with 06A150), the world's copy (its
	// size, not multisampled) and the grab's pipeline, made or made again for this world; their views in textures'
	// reserved descriptors 1 and 2
	bool fx_renderer::dist_resources(const target &t)
	{
		if (t.rt == nullptr || t.rtv_format == DXGI_FORMAT_UNKNOWN)
			return false;
		const D3D12_RESOURCE_DESC rd = t.rt->GetDesc();
		D3D12_CPU_DESCRIPTOR_HANDLE dist_cpu, scene_cpu;
		D3D12_GPU_DESCRIPTOR_HANDLE gpu;
		if (!textures::reserved(1, dist_cpu, gpu) || !textures::reserved(2, scene_cpu, gpu))
			return false;
		const D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
		D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format = t.rtv_format;
		sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sv.Texture2D.MipLevels = 1;
		if (_dist == nullptr || _dist_format != t.rtv_format)
		{
			for (IUnknown **u : { reinterpret_cast<IUnknown **>(&_dist), reinterpret_cast<IUnknown **>(&_grab_pso) })
			{
				if (*u != nullptr)
					(*u)->Release();
				*u = nullptr;
			}
			if (_dist_rtv == nullptr)
			{
				const D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
				if (FAILED(_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_dist_rtv))))
				{
					_dist_rtv = nullptr;
					return false;
				}
			}
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = d.Height = 256;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = t.rtv_format;
			d.SampleDesc.Count = 1;
			d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			D3D12_CLEAR_VALUE cv = {};
			cv.Format = t.rtv_format;
			if (FAILED(_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&_dist))))
			{
				_dist = nullptr;
				return false;
			}
			_dist->SetName(L"hifixi: the distortion target");
			D3D12_RENDER_TARGET_VIEW_DESC rv = {};
			rv.Format = t.rtv_format;
			rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			_device->CreateRenderTargetView(_dist, &rv, _dist_rtv->GetCPUDescriptorHandleForHeapStart());
			_device->CreateShaderResourceView(_dist, &sv, dist_cpu);
			_dist_format = t.rtv_format;
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
			pd.pRootSignature = _root;
			pd.VS = { static_cast<ID3DBlob *>(_vs_grab)->GetBufferPointer(), static_cast<ID3DBlob *>(_vs_grab)->GetBufferSize() };
			pd.PS = { static_cast<ID3DBlob *>(_ps_grab)->GetBufferPointer(), static_cast<ID3DBlob *>(_ps_grab)->GetBufferSize() };
			pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pd.NumRenderTargets = 1;
			pd.RTVFormats[0] = t.rtv_format;
			pd.SampleDesc.Count = 1;
			pd.SampleMask = UINT_MAX;
			pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			pd.RasterizerState.DepthClipEnable = TRUE;
			pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&_grab_pso))))
			{
				_grab_pso = nullptr;
				return false;
			}
		}
		if (_scene == nullptr || _scene_width != rd.Width || _scene_height != rd.Height || _scene->GetDesc().Format != rd.Format)
		{
			if (_scene != nullptr)
				_scene->Release();
			D3D12_RESOURCE_DESC d = rd;
			d.MipLevels = 1;
			d.DepthOrArraySize = 1;
			d.SampleDesc.Count = 1;
			d.SampleDesc.Quality = 0;
			d.Flags = D3D12_RESOURCE_FLAG_NONE;
			d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			d.Alignment = 0;
			if (FAILED(_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&_scene))))
			{
				_scene = nullptr;
				return false;
			}
			_scene->SetName(L"hifixi: the world's copy (distortion)");
			_scene_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			_scene_width = rd.Width;
			_scene_height = rd.Height;
			_device->CreateShaderResourceView(_scene, &sv, scene_cpu);
		}
		return true;
	}

	bool fx_renderer::grab(ID3D12GraphicsCommandList *list, const target &t, const fx::grab &gr)
	{
		if (!dist_resources(t))
			return false;
		D3D12_CPU_DESCRIPTOR_HANDLE cpu;
		D3D12_GPU_DESCRIPTOR_HANDLE scene_srv;
		textures::reserved(2, cpu, scene_srv);
		const auto barrier = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = r;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = from;
			b.Transition.StateAfter = to;
			list->ResourceBarrier(1, &b);
		};
		// The world as drawn so far (resolved when multisampled)
		const bool resolve = t.samples > 1;
		const D3D12_RESOURCE_STATES from = resolve ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE;
		const D3D12_RESOURCE_STATES to = resolve ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST;
		barrier(t.rt, D3D12_RESOURCE_STATE_RENDER_TARGET, from);
		barrier(_scene, _scene_state, to);
		if (resolve)
			list->ResolveSubresource(_scene, 0, t.rt, 0, t.rtv_format);
		else
			list->CopyResource(_scene, t.rt);
		barrier(_scene, to, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		_scene_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier(t.rt, from, D3D12_RESOURCE_STATE_RENDER_TARGET);
		// Its quad into the distortion target
		barrier(_dist, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = _dist_rtv->GetCPUDescriptorHandleForHeapStart();
		list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		const D3D12_VIEWPORT vp = { 0.0f, 0.0f, 256.0f, 256.0f, 0.0f, 1.0f };
		const D3D12_RECT sc = { 0, 0, 256, 256 };
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sc);
		list->SetPipelineState(_grab_pso);
		float c[16];
		for (int k = 0; k < 4; ++k)
		{
			c[4 * k + 0] = gr.pos[k][0];
			c[4 * k + 1] = gr.pos[k][1];
			c[4 * k + 2] = gr.uv[k][0];
			c[4 * k + 3] = gr.uv[k][1];
		}
		list->SetGraphicsRoot32BitConstants(0, 16, c, 0);
		list->SetGraphicsRootDescriptorTable(2, scene_srv);
		list->DrawInstanced(6, 1, 0, 0);
		barrier(_dist, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		return true;
	}

	run_stats fx_renderer::record_run(ID3D12GraphicsCommandList *list, const target &t, const frame &f, uint32_t seq, uint32_t run)
	{
		run_stats st;
		if (!ready() || run >= std::min(f.runs.load(std::memory_order_acquire), kMaxRuns) || textures::heap() == nullptr)
			return st;
		const fx::run &r = f.run_list[run];
		if (r.count == 0)
			return st;
		// The run's vertices into the flush's slot of the ring (at the frame's own offsets; an MMB mesh's are the zone's)
		uint32_t lo = kMaxVertices, hi = 0;
		bool any = false;
		for (uint32_t i = r.first; i < r.first + r.count && i < kMaxElements; ++i)
		{
			const element &e = f.elements[i];
			for (uint32_t k = e.first_draw; k < e.first_draw + e.draws && k < kMaxDraws; ++k)
			{
				any = any || f.draws[k].count != 0;
				if ((f.draws[k].stages & stage_zone_mesh) != 0)
					continue;
				lo = std::min(lo, f.draws[k].first);
				hi = std::max(hi, std::min(f.draws[k].first + f.draws[k].count, kMaxVertices));
			}
		}
		if (!any)
			return st;
		const UINT64 slot = UINT64(seq % kFrames) * kSlotBytes;
		if (hi > lo)
			std::memcpy(_mapped + slot + UINT64(lo) * sizeof(vertex), f.vertices + lo, size_t(hi - lo) * sizeof(vertex));
		// (the lit elements' lights after them: the run's, at the frame's own indices)
		uint32_t llo = kMaxLit, lhi = 0;
		for (uint32_t i = r.first; i < r.first + r.count && i < kMaxElements; ++i)
			if (f.elements[i].light < kMaxLit)
			{
				llo = std::min(llo, f.elements[i].light);
				lhi = std::max(lhi, f.elements[i].light + 1);
			}
		if (lhi > llo)
			std::memcpy(_mapped + slot + kVertexBytes + UINT64(llo) * sizeof(lighting), f.lights + llo, size_t(lhi - llo) * sizeof(lighting));

		D3D12_VIEWPORT vp = { 0.0f, 0.0f, t.width, t.height, 0.0f, 1.0f };
		D3D12_RECT sc = { 0, 0, LONG(t.width), LONG(t.height) };
		list->OMSetRenderTargets(1, &t.rtv, FALSE, &t.dsv);
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sc);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->SetGraphicsRootSignature(_root);
		list->SetGraphicsRootShaderResourceView(1, _ring->GetGPUVirtualAddress() + slot);
		// (the zone's views stand on the ring until an MMB mesh needs them: every root view set before a draw)
		list->SetGraphicsRootShaderResourceView(3, _ring->GetGPUVirtualAddress());
		list->SetGraphicsRootShaderResourceView(4, _ring->GetGPUVirtualAddress());
		list->SetGraphicsRootShaderResourceView(5, _ring->GetGPUVirtualAddress() + slot + kVertexBytes);
		ID3D12DescriptorHeap *const ours = textures::heap();
		ID3D12DescriptorHeap *heap_bound = ours;
		const zone::gpu_zone *zone_bound = nullptr;
		ID3D12PipelineState *bound = nullptr;
		uint32_t c[kConstants] = {};
		for (uint32_t i = r.first; i < r.first + r.count && i < kMaxElements; ++i)
		{
			const element &e = f.elements[i];
			ID3D12PipelineState *const pso = pipeline(t, e.blend, e.z_write != 0);
			if (pso == nullptr)
				continue;
			if (pso != bound)
			{
				list->SetPipelineState(pso);
				bound = pso;
			}
			float proj[16];
			element_projection(f, e, proj);
			std::memcpy(c, e.world, 64);
			std::memcpy(c + 16, proj, 64);
			const float tf[4] = { channel(e.tfactor, 16), channel(e.tfactor, 8), channel(e.tfactor, 0), channel(e.tfactor, 24) };
			std::memcpy(c + 32, tf, 16);
			const float fog[8] = { channel(e.fog_colour, 16), channel(e.fog_colour, 8), channel(e.fog_colour, 0), float(e.fog), e.fog_start, e.fog_end, e.uv[0], e.uv[1] };
			std::memcpy(c + 36, fog, 32);
			list->SetGraphicsRoot32BitConstants(0, 44, c, 0);
			// (an MMB mesh: the zone's buffers and heap; anything else ours)
			const zone::gpu_zone *const z = e.zone;
			ID3D12DescriptorHeap *const want = z != nullptr ? z->heap() : ours;
			if (want != heap_bound)
			{
				list->SetDescriptorHeaps(1, &want);
				heap_bound = want;
			}
			if (z != nullptr && z != zone_bound)
			{
				list->SetGraphicsRootShaderResourceView(3, z->vertices()->GetGPUVirtualAddress());
				list->SetGraphicsRootShaderResourceView(4, z->indices()->GetGPUVirtualAddress());
				zone_bound = z;
			}
			for (uint32_t k = e.first_draw; k < e.first_draw + e.draws && k < kMaxDraws; ++k)
			{
				const draw &dr = f.draws[k];
				const bool zoned = (dr.stages & stage_zone_mesh) != 0;
				if (dr.count == 0 || (zoned ? z == nullptr : dr.first + dr.count > kMaxVertices))
					continue;
				if (dr.grab != kNone)
				{
					// (a distortion: the world so far into the distortion target first, then the world's target again)
					if (z != nullptr || dr.grab >= kMaxGrabs || !grab(list, t, f.grabs[dr.grab]))
						continue;
					st.grabs++;
					list->OMSetRenderTargets(1, &t.rtv, FALSE, &t.dsv);
					list->RSSetViewports(1, &vp);
					list->RSSetScissorRects(1, &sc);
					list->SetPipelineState(pso);
					bound = pso;
					list->SetGraphicsRoot32BitConstants(0, 44, c, 0);
				}
				const uint32_t fl[3] = { dr.stages, dr.first, e.light < kMaxLit ? e.light : kNone };
				list->SetGraphicsRoot32BitConstants(0, 3, fl, 44);
				D3D12_CPU_DESCRIPTOR_HANDLE dist_cpu;
				D3D12_GPU_DESCRIPTOR_HANDLE dist_gpu;
				if (dr.texture == kDistTexture)
				{
					if (!textures::reserved(1, dist_cpu, dist_gpu) || _dist == nullptr)
						continue;
					list->SetGraphicsRootDescriptorTable(2, dist_gpu);
				}
				else if (zoned)
					list->SetGraphicsRootDescriptorTable(2, z->texture_srv((dr.stages & (stage_mmb_textured | stage_mmb_textured_d)) != 0 ? dr.texture : textures::kNone));
				else
					list->SetGraphicsRootDescriptorTable(2, textures::view((dr.stages & stage_texture) != 0 ? dr.texture : textures::kNone));
				list->DrawInstanced(dr.count, 1, 0, 0);
				st.draws++;
				st.triangles += dr.count / 3;
			}
			st.elements++;
		}
		if (heap_bound != ours)
			list->SetDescriptorHeaps(1, &ours);
		return st;
	}
}
