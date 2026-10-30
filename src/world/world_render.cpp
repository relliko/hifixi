/*
 * The world part's passes on Direct3D 12 (render.hpp).
 *
 * Root signature (the world's):          and drawing over the screen's (the names after the upscale):
 *   0  constants b0 (24)                   the same, and
 *   1  SRV t0  the pass's records          3  table t2  A's depth (plane 0)
 *   2  SRV t1  the name pages (texels)
 *
 * Vertices come from the records by SV_VertexID (no input layout): the dome's strips one draw each (a triangle strip),
 * a glyph 6 vertices (its quad as two triangles: TL TR BL, BL TR BR), a grid outline 8 (4 lines).
 */

#include <windows.h>
#include "render.hpp"
#include "dome.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hifi::world
{
	namespace
	{
		constexpr UINT kConstants = 24;
		constexpr size_t kSlotBytes[3] = { kSkySlotBytes, kNamesSlotBytes, kGridSlotBytes };

		const char *const kShader = R"(
cbuffer k : register(b0) { float4 k0, k1, k2, k3, k4, k5; };

float4 d3dcolor(uint c) { return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0; }
float4 rgba8(uint c) { return float4(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24) / 255.0; }

struct col_out { float4 pos : SV_Position; float4 col : COLOR; };
float4 colour_ps(col_out i) : SV_Target { return i.col; }

// The dome: k0..k3 WORLD x VIEW x PROJECTION's rows (row vectors), k4.x the draw's first vertex (SV_VertexID counts
// from 0 whatever the draw's start)
struct sky_vertex { float3 p; uint c; };
StructuredBuffer<sky_vertex> sky_vertices : register(t0);
col_out sky_vs(uint id : SV_VertexID)
{
	sky_vertex v = sky_vertices[id + asuint(k4.x)];
	col_out o;
	o.pos = mul(float4(v.p, 1.0), float4x4(k0, k1, k2, k3));
	o.col = d3dcolor(v.c);
	return o;
}

// The grid: k0..k3 VIEW x PROJECTION's rows, k4.x the draw's first outline; each outline p0 p1 p2 p3 p0
struct grid_record { float3 p0, p1, p2, p3; uint c, keep; };
StructuredBuffer<grid_record> grid_lines : register(t0);
col_out grid_vs(uint id : SV_VertexID)
{
	grid_record l = grid_lines[id / 8 + asuint(k4.x)];
	uint corner = ((id % 8) / 2 + id % 2) % 4;
	float3 p = corner == 0 ? l.p0 : corner == 1 ? l.p1 : corner == 2 ? l.p2 : l.p3;
	col_out o;
	o.pos = mul(float4(p, 1.0), float4x4(k0, k1, k2, k3));
	o.col = d3dcolor(l.c);
	return o;
}

// The names: k0 the target's size, its pixels per the game's; k1.xy added to the game's pixels first (0.5: Direct3D 8's
// pixel centres, less the jitter over the screen), k1.z where page 1 starts; k2 the pages' sizes; k3 A's pixel for a
// target pixel (xy x, + zw), k4.xy A's size
struct glyph_vertex { float4 p; uint c; float2 uv; };
struct glyph_record { glyph_vertex v[4]; uint page, ref, name, pad; };
StructuredBuffer<glyph_record> glyphs : register(t0);
StructuredBuffer<uint> pages : register(t1);
struct glyph_out { float4 pos : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD0; nointerpolation uint2 pr : PAGE; float z : NAMEZ; };
glyph_out names_vs(uint id : SV_VertexID)
{
	glyph_record g = glyphs[id / 6];
	uint k = id % 6;
	uint corner = k == 0 ? 0 : k == 1 || k == 4 ? 1 : k == 5 ? 3 : 2;
	glyph_vertex v = g.v[corner];
	float x = (v.p.x + k1.x) * k0.z, y = (v.p.y + k1.y) * k0.w;
	glyph_out o;
	o.pos = float4(x / k0.x * 2.0 - 1.0, 1.0 - y / k0.y * 2.0, v.p.z, 1.0);
	o.col = d3dcolor(v.c);
	o.uv = v.uv;
	o.pr = uint2(g.page, g.ref);
	o.z = v.p.z;
	return o;
}
// The page's texel as LINEAR filtering with WRAP gives it
float4 texel(uint page, float2 uv)
{
	float2 size = page == 0 ? k2.xy : k2.zw;
	uint base = page == 0 ? 0 : asuint(k1.z);
	int w = int(size.x), h = int(size.y);
	float2 t = uv * size - 0.5;
	float2 f = frac(t);
	int2 i = int2(floor(t));
	int x0 = ((i.x % w) + w) % w, y0 = ((i.y % h) + h) % h;
	int x1 = (x0 + 1) % w, y1 = (y0 + 1) % h;
	float4 a = rgba8(pages[base + y0 * w + x0]), b = rgba8(pages[base + y0 * w + x1]);
	float4 c = rgba8(pages[base + y1 * w + x0]), d = rgba8(pages[base + y1 * w + x1]);
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
// MODULATE2X colour, MODULATE4X alpha, the alpha test GREATER the glyph's ALPHAREF (on the 8-bit alpha)
float4 glyph_colour(glyph_out i)
{
	float4 t = texel(i.pr.x, i.uv);
	float4 c = float4(saturate(t.rgb * i.col.rgb * 2.0), saturate(t.a * i.col.a * 4.0));
	if (round(c.a * 255.0) <= float(i.pr.y))
		discard;
	return c;
}
float4 names_ps(glyph_out i) : SV_Target { return glyph_colour(i); }
// Over the screen: LESSEQUAL against A's depth at the matching pixel (the name's z as a 24-bit depth)
Texture2D<float> world_depth : register(t2);
float4 names_over_ps(glyph_out i) : SV_Target
{
	float4 c = glyph_colour(i);
	float2 a = i.pos.xy * k3.xy + k3.zw;
	int2 p = clamp(int2(floor(a)), int2(0, 0), int2(k4.xy) - 1);
	float d = world_depth.Load(int3(p, 0));
	float z = round(saturate(i.z) * 16777215.0) / 16777215.0;
	if (z > d)
		discard;
	return c;
}
)";

		struct shader_entry
		{
			const char *name, *profile;
		};
		constexpr shader_entry kEntries[6] = { { "sky_vs", "vs_5_0" }, { "colour_ps", "ps_5_0" }, { "names_vs", "vs_5_0" }, { "names_ps", "ps_5_0" },
			{ "names_over_ps", "ps_5_0" }, { "grid_vs", "vs_5_0" } };

		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		D3D12_SHADER_BYTECODE code_of(void *blob)
		{
			ID3DBlob *const b = static_cast<ID3DBlob *>(blob);
			return { b->GetBufferPointer(), b->GetBufferSize() };
		}

		void bind_common(ID3D12GraphicsCommandList *list, const target &t, float offset)
		{
			const D3D12_VIEWPORT vp = { offset, offset, float(t.width), float(t.height), 0.0f, 1.0f };
			const D3D12_RECT sc = { 0, 0, LONG(t.width), LONG(t.height) };
			list->RSSetViewports(1, &vp);
			list->RSSetScissorRects(1, &sc);
		}

		// The records of the glyphs as the shader reads them (128 bytes each)
		struct glyph_record
		{
			name_vertex v[4];
			uint32_t page, ref, name, pad;
		};
		static_assert(sizeof(glyph_record) == 128, "a glyph record is 128 bytes");
		struct grid_record
		{
			float p[4][3];
			uint32_t colour, keep;
		};
		static_assert(sizeof(grid_record) == 56, "a grid record is 56 bytes");
	}

	renderer::~renderer()
	{
		release();
	}

	void renderer::release()
	{
		for (auto &p : _pipelines)
			if (p.second != nullptr)
				p.second->Release();
		_pipelines.clear();
		for (int i = 0; i < 3; ++i)
		{
			if (_rings[i] != nullptr && _mapped[i] != nullptr)
				_rings[i]->Unmap(0, nullptr);
			_mapped[i] = nullptr;
			drop(_rings[i]);
		}
		drop(_pages);
		drop(_heap);
		drop(_root);
		drop(_root_over);
		for (void *&c : _code)
		{
			if (c != nullptr)
				static_cast<ID3DBlob *>(c)->Release();
			c = nullptr;
		}
		_device = nullptr;
	}

	bool renderer::init(ID3D12Device *device, std::string &why)
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
		for (int k = 0; k < 6; ++k)
		{
			ID3DBlob *code = nullptr, *errors = nullptr;
			if (FAILED(compile(kShader, std::strlen(kShader), "hifixi_world", nullptr, nullptr, kEntries[k].name, kEntries[k].profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
					&errors)))
			{
				why = std::string("the world's shaders didn't compile: ") + (errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
				if (errors != nullptr)
					errors->Release();
				release();
				return false;
			}
			if (errors != nullptr)
				errors->Release();
			_code[k] = code;
		}

		using serialize_fn = decltype(&D3D12SerializeRootSignature);
		HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
		const auto serialize = d3d12 != nullptr ? reinterpret_cast<serialize_fn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
		for (int r = 0; r < 2; ++r)
		{
			const D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2 };
			D3D12_ROOT_PARAMETER p[4] = {};
			p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			p[0].Constants.Num32BitValues = kConstants;
			p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			p[1].Descriptor.ShaderRegister = 0;
			p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			p[2].Descriptor.ShaderRegister = 1;
			p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			p[3].DescriptorTable.NumDescriptorRanges = 1;
			p[3].DescriptorTable.pDescriptorRanges = &range;
			p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			const D3D12_ROOT_SIGNATURE_DESC rd = { r == 0 ? 3u : 4u, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
			ID3DBlob *blob = nullptr, *errors = nullptr;
			ID3D12RootSignature *&out = r == 0 ? _root : _root_over;
			if (serialize == nullptr || FAILED(serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
				FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out))))
			{
				why = "the world's root signatures couldn't be made";
				if (blob != nullptr)
					blob->Release();
				if (errors != nullptr)
					errors->Release();
				release();
				return false;
			}
			blob->Release();
		}
		for (uint32_t i = 0; i < 3; ++i)
			if (!ring(i, kSlotBytes[i] * kSlots, why))
			{
				release();
				return false;
			}
		const D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_heap))))
		{
			why = "the world's descriptor heap couldn't be made";
			release();
			return false;
		}
		_heap_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		return true;
	}

	bool renderer::ring(uint32_t which, size_t bytes, std::string &why)
	{
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = bytes;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		const D3D12_RANGE none = { 0, 0 };
		if (FAILED(_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_rings[which]))) ||
			FAILED(_rings[which]->Map(0, &none, reinterpret_cast<void **>(&_mapped[which]))))
		{
			why = "the world's upload rings couldn't be made";
			return false;
		}
		_rings[which]->SetName(which == 0 ? L"hifixi: sky dome (upload)" : which == 1 ? L"hifixi: names (upload)" : L"hifixi: layout grid (upload)");
		return true;
	}

	bool renderer::set_pages(const font_pages &pages, std::string &why)
	{
		if (_device == nullptr)
		{
			why = "no device";
			return false;
		}
		for (int i = 0; i < 2; ++i)
			if (pages.w[i] == 0 || pages.h[i] == 0 || pages.texels[i].size() != size_t(pages.w[i]) * pages.h[i])
			{
				why = "the name pages aren't whole";
				return false;
			}
		const size_t count = pages.texels[0].size() + pages.texels[1].size();
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = count * 4;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *buffer = nullptr;
		uint8_t *mapped = nullptr;
		const D3D12_RANGE none = { 0, 0 };
		if (FAILED(_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer))) ||
			FAILED(buffer->Map(0, &none, reinterpret_cast<void **>(&mapped))))
		{
			drop(buffer);
			why = "the name pages' buffer couldn't be made";
			return false;
		}
		std::memcpy(mapped, pages.texels[0].data(), pages.texels[0].size() * 4);
		std::memcpy(mapped + pages.texels[0].size() * 4, pages.texels[1].data(), pages.texels[1].size() * 4);
		buffer->Unmap(0, nullptr);
		buffer->SetName(L"hifixi: name pages");
		drop(_pages); // (set before any list could use the old one: the game's thread, before native names are let through)
		_pages = buffer;
		for (int i = 0; i < 2; ++i)
		{
			_page_w[i] = pages.w[i];
			_page_h[i] = pages.h[i];
		}
		_page1 = uint32_t(pages.texels[0].size());
		return true;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE renderer::heap_cpu(uint64_t frame) const
	{
		return { _heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(frame % kSlots) * _heap_step };
	}

	D3D12_GPU_DESCRIPTOR_HANDLE renderer::heap_gpu(uint64_t frame) const
	{
		return { _heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(frame % kSlots) * _heap_step };
	}

	ID3D12PipelineState *renderer::pipeline(kind k, const target &t)
	{
		const bool depth = k == names_kind || k == grid_depth;
		const uint64_t key = uint64_t(t.rtv_format) | uint64_t(depth ? t.dsv_format : DXGI_FORMAT_UNKNOWN) << 8 | uint64_t(t.samples) << 16 | uint64_t(k) << 24;
		const auto known = _pipelines.find(key);
		if (known != _pipelines.end())
			return known->second;
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		d.pRootSignature = k == over_kind ? _root_over : _root;
		const bool grid = k == grid_flat || k == grid_depth, names = k == names_kind || k == over_kind;
		d.VS = code_of(_code[grid ? 5 : names ? 2 : 0]);
		d.PS = code_of(_code[k == names_kind ? 3 : k == over_kind ? 4 : 1]);
		D3D12_RENDER_TARGET_BLEND_DESC &b = d.BlendState.RenderTarget[0];
		b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		b.BlendEnable = names ? TRUE : FALSE;
		b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
		b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		b.LogicOp = D3D12_LOGIC_OP_NOOP;
		d.SampleMask = UINT_MAX;
		d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		d.RasterizerState.DepthClipEnable = TRUE;
		d.DepthStencilState.DepthEnable = depth ? TRUE : FALSE;
		d.DepthStencilState.DepthWriteMask = depth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
		d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		d.PrimitiveTopologyType = grid ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		d.NumRenderTargets = 1;
		d.RTVFormats[0] = t.rtv_format;
		d.DSVFormat = depth ? t.dsv_format : DXGI_FORMAT_UNKNOWN;
		d.SampleDesc.Count = t.samples;
		ID3D12PipelineState *p = nullptr;
		if (FAILED(_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p))))
			p = nullptr;
		_pipelines[key] = p; // (a failure is kept: not tried again every frame)
		return p;
	}

	bool renderer::sky(ID3D12GraphicsCommandList *list, const target &t, const sky_frame &f, uint64_t frame, std::string &why)
	{
		if (!ready())
		{
			why = "the renderer isn't made";
			return false;
		}
		static thread_local std::vector<dome::strip> strips;
		static thread_local std::vector<dome::vertex> vertices;
		dome::build(f.radius, f.segments, f.colours, f.heights, strips, vertices);
		if (vertices.size() * sizeof(dome::vertex) > kSkySlotBytes)
		{
			why = "the dome has more strips than its ring's slot holds";
			return false;
		}
		if (strips.size() <= f.skip)
			return true; // (all of it left out, as the game leaves it out: drawn, as nothing)
		ID3D12PipelineState *const pso = pipeline(sky_kind, t);
		if (pso == nullptr)
		{
			why = "the dome's pipeline couldn't be made";
			return false;
		}
		const size_t at = size_t(frame % kSlots) * kSkySlotBytes;
		std::memcpy(_mapped[0] + at, vertices.data(), vertices.size() * sizeof(dome::vertex));
		list->SetGraphicsRootSignature(_root);
		list->SetPipelineState(pso);
		list->SetGraphicsRoot32BitConstants(0, 16, f.world_view_proj, 0);
		list->SetGraphicsRootShaderResourceView(1, _rings[0]->GetGPUVirtualAddress() + at);
		list->SetGraphicsRootShaderResourceView(2, _rings[0]->GetGPUVirtualAddress() + at);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		bind_common(list, t, 0.5f);
		list->OMSetRenderTargets(1, &t.rtv, FALSE, nullptr);
		const UINT per_strip = 2 * (f.segments + 1);
		for (size_t s = f.skip; s < strips.size(); ++s)
		{
			list->SetGraphicsRoot32BitConstants(0, 1, &strips[s].first, 16);
			list->DrawInstanced(per_strip, 1, 0, 0);
		}
		return true;
	}

	namespace
	{
		// The glyphs into the ring's slot; how many
		uint32_t put_glyphs(uint8_t *to, const names_frame &f)
		{
			const uint32_t n = std::min(f.glyph_count, kMaxGlyphs);
			glyph_record *const r = reinterpret_cast<glyph_record *>(to);
			for (uint32_t i = 0; i < n; ++i)
			{
				const name_glyph &g = f.glyphs[i];
				std::memcpy(r[i].v, g.v, sizeof(g.v));
				r[i].page = g.page;
				r[i].ref = g.alpha_ref;
				r[i].name = g.name;
				r[i].pad = 0;
			}
			return n;
		}
	}

	bool renderer::names(ID3D12GraphicsCommandList *list, const target &t, const names_frame &f, uint64_t frame, std::string &why)
	{
		if (!ready() || _pages == nullptr)
		{
			why = !ready() ? "the renderer isn't made" : "the name pages aren't up";
			return false;
		}
		if (t.dsv_format == DXGI_FORMAT_UNKNOWN)
		{
			why = "the names need the world's depth";
			return false;
		}
		// (A's size as the game laid the names out in it; the target's own when gGame couldn't be read: the same but for
		// dgVoodoo's scaling)
		const float game_w = float(f.width != 0 ? f.width : t.width), game_h = float(f.height != 0 ? f.height : t.height);
		ID3D12PipelineState *const pso = pipeline(names_kind, t);
		if (pso == nullptr)
		{
			why = "the names' pipeline couldn't be made";
			return false;
		}
		const size_t at = size_t(frame % kSlots) * kNamesSlotBytes;
		const uint32_t n = put_glyphs(_mapped[1] + at, f);
		if (n == 0)
			return true;
		float k[kConstants] = {};
		k[0] = float(t.width);
		k[1] = float(t.height);
		k[2] = float(t.width) / game_w;
		k[3] = float(t.height) / game_h;
		k[4] = 0.5f;
		k[5] = 0.5f;
		std::memcpy(&k[6], &_page1, 4);
		k[8] = float(_page_w[0]);
		k[9] = float(_page_h[0]);
		k[10] = float(_page_w[1]);
		k[11] = float(_page_h[1]);
		list->SetGraphicsRootSignature(_root);
		list->SetPipelineState(pso);
		list->SetGraphicsRoot32BitConstants(0, kConstants, k, 0);
		list->SetGraphicsRootShaderResourceView(1, _rings[1]->GetGPUVirtualAddress() + at);
		list->SetGraphicsRootShaderResourceView(2, _pages->GetGPUVirtualAddress());
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		bind_common(list, t, 0.0f);
		list->OMSetRenderTargets(1, &t.rtv, FALSE, &t.dsv);
		list->DrawInstanced(6 * n, 1, 0, 0);
		return true;
	}

	bool renderer::names_over(ID3D12GraphicsCommandList *list, const target &t, const over_screen &o, const names_frame &f, uint64_t frame, std::string &why)
	{
		if (!ready() || _pages == nullptr)
		{
			why = !ready() ? "the renderer isn't made" : "the name pages aren't up";
			return false;
		}
		if (o.a_width == 0 || o.a_height == 0 || o.depth.ptr == 0)
		{
			why = "the world's size or depth isn't known";
			return false;
		}
		const float game_w = float(f.width != 0 ? f.width : o.a_width), game_h = float(f.height != 0 ? f.height : o.a_height); // (as names())
		ID3D12PipelineState *const pso = pipeline(over_kind, t);
		if (pso == nullptr)
		{
			why = "the names' pipeline over the screen couldn't be made";
			return false;
		}
		// (glyphs drawn far to near: there's no depth to keep a farther name's glyphs off a nearer one's, as the game's
		// depth writes do in A, so the nearer name is drawn last, over it)
		static thread_local std::vector<uint32_t> order;
		order.clear();
		const uint32_t n = std::min(f.glyph_count, kMaxGlyphs);
		for (uint32_t i = 0; i < n; ++i)
			order.push_back(i);
		std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return f.glyphs[a].v[0].z > f.glyphs[b].v[0].z; });
		const size_t at = size_t(frame % kSlots) * kNamesSlotBytes;
		glyph_record *const r = reinterpret_cast<glyph_record *>(_mapped[1] + at);
		for (uint32_t i = 0; i < n; ++i)
		{
			const name_glyph &g = f.glyphs[order[i]];
			std::memcpy(r[i].v, g.v, sizeof(g.v));
			r[i].page = g.page;
			r[i].ref = g.alpha_ref;
			r[i].name = g.name;
			r[i].pad = 0;
		}
		if (n == 0)
			return true;
		const float sx = float(t.width) / game_w, sy = float(t.height) / game_h; // (screen pixels a game pixel)
		const float ax = float(o.a_width) / float(t.width), ay = float(o.a_height) / float(t.height);
		float k[kConstants] = {};
		k[0] = float(t.width);
		k[1] = float(t.height);
		k[2] = sx;
		k[3] = sy;
		k[4] = 0.5f - f.jitter[0];
		k[5] = 0.5f - f.jitter[1];
		std::memcpy(&k[6], &_page1, 4);
		k[8] = float(_page_w[0]);
		k[9] = float(_page_h[0]);
		k[10] = float(_page_w[1]);
		k[11] = float(_page_h[1]);
		k[12] = ax;
		k[13] = ay;
		k[14] = f.jitter[0] * float(o.a_width) / game_w;
		k[15] = f.jitter[1] * float(o.a_height) / game_h;
		k[16] = float(o.a_width);
		k[17] = float(o.a_height);
		list->SetDescriptorHeaps(1, &_heap); // (the caller binds dgVoodoo's again)
		list->SetGraphicsRootSignature(_root_over);
		list->SetPipelineState(pso);
		list->SetGraphicsRoot32BitConstants(0, kConstants, k, 0);
		list->SetGraphicsRootShaderResourceView(1, _rings[1]->GetGPUVirtualAddress() + at);
		list->SetGraphicsRootShaderResourceView(2, _pages->GetGPUVirtualAddress());
		list->SetGraphicsRootDescriptorTable(3, o.depth);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		bind_common(list, t, 0.0f);
		list->OMSetRenderTargets(1, &t.rtv, FALSE, nullptr);
		list->DrawInstanced(6 * n, 1, 0, 0);
		return true;
	}

	bool renderer::grid(ID3D12GraphicsCommandList *list, const target &t, const grid_frame &f, uint64_t frame, std::string &why)
	{
		if (!ready())
		{
			why = "the renderer isn't made";
			return false;
		}
		const uint32_t n = std::min(f.count, kMaxGridLines);
		if (n == 0)
			return true;
		bool any_depth = false;
		const size_t at = size_t(frame % kSlots) * kGridSlotBytes;
		grid_record *const r = reinterpret_cast<grid_record *>(_mapped[2] + at);
		for (uint32_t i = 0; i < n; ++i)
		{
			std::memcpy(r[i].p, f.lines[i].p, sizeof(r[i].p));
			r[i].colour = f.lines[i].colour;
			r[i].keep = f.lines[i].keep_z;
			any_depth = any_depth || f.lines[i].keep_z != 0;
		}
		if (any_depth && t.dsv_format == DXGI_FORMAT_UNKNOWN)
		{
			why = "a grid line keeps the depth test, and the world's depth isn't known";
			return false;
		}
		ID3D12PipelineState *const flat = pipeline(grid_flat, t), *const tested = any_depth ? pipeline(grid_depth, t) : nullptr;
		if (flat == nullptr || (any_depth && tested == nullptr))
		{
			why = "the grid's pipelines couldn't be made";
			return false;
		}
		list->SetGraphicsRootSignature(_root);
		list->SetGraphicsRoot32BitConstants(0, 16, f.view_proj, 0);
		list->SetGraphicsRootShaderResourceView(1, _rings[2]->GetGPUVirtualAddress() + at);
		list->SetGraphicsRootShaderResourceView(2, _rings[2]->GetGPUVirtualAddress() + at);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
		bind_common(list, t, 0.5f);
		// (in order, a run of outlines at a time with the same depth state)
		for (uint32_t i = 0; i < n;)
		{
			const bool keep = f.lines[i].keep_z != 0;
			uint32_t end = i + 1;
			while (end < n && (f.lines[end].keep_z != 0) == keep)
				end++;
			list->SetPipelineState(keep ? tested : flat);
			list->OMSetRenderTargets(1, &t.rtv, FALSE, keep ? &t.dsv : nullptr);
			list->SetGraphicsRoot32BitConstants(0, 1, &i, 16);
			list->DrawInstanced(8 * (end - i), 1, 0, 0);
			i = end;
		}
		return true;
	}
}
