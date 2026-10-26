/*
 * The zone's sun shadow casters from the native zone data (zoneshadow.hpp). One root signature:
 *   0  22 constants b0  world_to_clip (16), the piece's first index, alpha tested, the list's start, and the counts
 *                       every read is clamped below (indices, vertices, placements)
 *   1  SRV t0  the zone's vertices (48 bytes)   2  SRV t1  its indices (32-bit, absolute)
 *   3  SRV t2  the placements (48 bytes: three rows, then the translation)   4  SRV t3  the visible list (uints)
 *   5  table t4  the piece's texture (alpha tested pieces only)
 *   s0  linear, wrapping (as the zone pass)
 */

#include <windows.h>
#include <d3dcompiler.h>
#include "zoneshadow.hpp"
#include "zone_gpu.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

namespace hifi::zone
{
	namespace
	{
		namespace zm = bake::zonemesh;

		const char *kShader = R"(
cbuffer draw_constants : register(b0)
{
	row_major float4x4 world_to_clip;
	uint first_index;
	uint tested;
	uint list_start;
	uint index_count;
	uint vertex_count;
	uint place_count;
};
struct vertex { float3 pos; float3 normal; uint colour; float2 uv; float3 sway; };
struct place { float3 r0; float3 r1; float3 r2; float3 t; };
StructuredBuffer<vertex> vertices : register(t0);
StructuredBuffer<uint> indices : register(t1);
StructuredBuffer<place> places : register(t2);
StructuredBuffer<uint> visible : register(t3);
Texture2D colour_map : register(t4);
SamplerState wrap : register(s0);

struct v2p
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
	float alpha : TEXCOORD1;
};

v2p vs(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	const vertex v = vertices[min(indices[min(first_index + vid, index_count - 1)], vertex_count - 1)];
	const place p = places[min(visible[list_start + iid], place_count - 1)];
	const float3 w = v.pos.x * p.r0 + v.pos.y * p.r1 + v.pos.z * p.r2 + p.t;
	v2p r;
	r.pos = mul(float4(w, 1), world_to_clip);
	r.uv = v.uv;
	r.alpha = float(v.colour >> 24) / 255.0;
	return r;
}

// (the zone pass's test: ALPHAREF 0x60 GREATER against 4 x texture alpha x vertex alpha)
void ps(v2p i)
{
	if (tested != 0 && saturate(4 * colour_map.Sample(wrap, i.uv).a * i.alpha) <= 96.0 / 255.0)
		discard;
}
)";

		constexpr uint32_t kRingSlots = 16;
		constexpr uint32_t kMaxVisible = 65536;
		constexpr UINT kConstants = 22;

		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}
	}

	zone_shadow::~zone_shadow()
	{
		release();
	}

	void zone_shadow::release()
	{
		if (_ring != nullptr && _mapped != nullptr)
			_ring->Unmap(0, nullptr);
		_mapped = nullptr;
		drop(_ring);
		drop(_places);
		for (auto &o : _old)
			drop(o.first);
		_old.clear();
		drop(_pso);
		drop(_root);
		_zone = nullptr;
		_boxes.clear();
		_by_model.clear();
		_any = false;
		_device = nullptr;
	}

	bool zone_shadow::init(ID3D12Device *device, std::string &why)
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
		ID3DBlob *vs = nullptr, *ps = nullptr, *errors = nullptr;
		for (auto [entry, target, out] : { std::tuple<const char *, const char *, ID3DBlob **> { "vs", "vs_5_0", &vs }, { "ps", "ps_5_0", &ps } })
			if (FAILED(compile(kShader, std::strlen(kShader), "hifixi_zone_shadow", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors)))
			{
				why = std::string("the zone's shadow shader didn't compile: ") + (errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
				if (errors != nullptr)
					errors->Release();
				drop(vs);
				release();
				return false;
			}
		D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4 };
		D3D12_ROOT_PARAMETER p[6] = {};
		p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		p[0].Constants.Num32BitValues = kConstants;
		for (UINT i = 0; i < 4; ++i)
		{
			p[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			p[1 + i].Descriptor.ShaderRegister = i;
		}
		p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		p[5].DescriptorTable.NumDescriptorRanges = 1;
		p[5].DescriptorTable.pDescriptorRanges = &range;
		p[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_STATIC_SAMPLER_DESC s = {};
		s.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		const D3D12_ROOT_SIGNATURE_DESC rd = { 6, p, 1, &s, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		using serialize_fn = decltype(&D3D12SerializeRootSignature);
		const auto serialize = reinterpret_cast<serialize_fn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
		ID3DBlob *blob = nullptr;
		errors = nullptr;
		bool ok = serialize != nullptr && SUCCEEDED(serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) &&
			SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_root)));
		drop(blob);
		drop(errors);
		if (ok)
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature = _root;
			d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
			d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets = 0;
			d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
			d.SampleDesc.Count = 1;
			d.SampleMask = UINT_MAX;
			d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // (as the cascades' own: cards and the ground are single sheets)
			d.RasterizerState.DepthClipEnable = TRUE;
			d.DepthStencilState.DepthEnable = TRUE;
			d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
			ok = SUCCEEDED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&_pso)));
		}
		drop(vs);
		drop(ps);
		if (ok)
		{
			D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = UINT64(kRingSlots) * kMaxVisible * 4;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			const D3D12_RANGE none = { 0, 0 };
			ok = SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_ring))) &&
				SUCCEEDED(_ring->Map(0, &none, reinterpret_cast<void **>(&_mapped)));
		}
		if (!ok)
		{
			why = "the zone's shadow pass couldn't be made";
			release();
			return false;
		}
		return true;
	}

	// Each placement's world box (its model's box through its matrix) and the placements in model order; the placements
	// as the shader reads them. Once a zone (and again for another).
	bool zone_shadow::prepare_zone(gpu_zone &z)
	{
		const zm::zone &d = z.data();
		if (_zone == &z && _places != nullptr)
			return _any;
		_zone = &z;
		_boxes.clear();
		_by_model.clear();
		_any = false;
		// (the old placements' buffer may still be read by the GPU: let go kRingSlots records on, as the ring's slots)
		if (_places != nullptr)
			_old.push_back({ _places, _slot });
		_places = nullptr;
		const size_t n = d.placements.size();
		if (n == 0)
			return false;
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = UINT64(n) * 48;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		float *mapped = nullptr;
		const D3D12_RANGE none = { 0, 0 };
		if (FAILED(_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_places))) ||
			FAILED(_places->Map(0, &none, reinterpret_cast<void **>(&mapped))))
		{
			drop(_places);
			return false;
		}
		_boxes.resize(n * 6);
		for (int a = 0; a < 3; ++a)
		{
			_lo[a] = 1e30f;
			_hi[a] = -1e30f;
		}
		for (size_t i = 0; i < n; ++i)
		{
			const zm::placement &p = d.placements[i];
			std::memcpy(mapped + i * 12, p.m, 36);
			std::memcpy(mapped + i * 12 + 9, p.t, 12);
			float *b = &_boxes[i * 6];
			for (int a = 0; a < 3; ++a)
			{
				b[a] = 1e30f;
				b[3 + a] = -1e30f;
			}
			if (p.model >= d.models.size())
				continue;
			const zm::model &m = d.models[p.model];
			for (int c = 0; c < 8; ++c)
			{
				const float q[3] = { (c & 1) ? m.hi[0] : m.lo[0], (c & 2) ? m.hi[1] : m.lo[1], (c & 4) ? m.hi[2] : m.lo[2] };
				for (int a = 0; a < 3; ++a)
				{
					const float w = q[0] * p.m[a] + q[1] * p.m[3 + a] + q[2] * p.m[6 + a] + p.t[a];
					b[a] = std::min(b[a], w);
					b[3 + a] = std::max(b[3 + a], w);
				}
			}
			for (int a = 0; a < 3; ++a)
			{
				_lo[a] = std::min(_lo[a], b[a]);
				_hi[a] = std::max(_hi[a], b[3 + a]);
			}
			_by_model.push_back(uint32_t(i));
			_any = true;
		}
		_places->Unmap(0, nullptr);
		std::stable_sort(_by_model.begin(), _by_model.end(), [&](uint32_t a, uint32_t b) {
			const zm::placement &pa = d.placements[a], &pb = d.placements[b];
			return pa.model != pb.model ? pa.model < pb.model : (pa.flags & zm::kAlphaTested) < (pb.flags & zm::kAlphaTested);
		});
		return _any;
	}

	bool zone_shadow::bounds(gpu_zone &z, float lo[3], float hi[3])
	{
		if (_pso == nullptr || !z.ready() || !prepare_zone(z))
			return false;
		std::memcpy(lo, _lo, 12);
		std::memcpy(hi, _hi, 12);
		return true;
	}

	uint32_t zone_shadow::record(ID3D12GraphicsCommandList *list, gpu_zone &z, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, uint32_t size)
	{
		if (_pso == nullptr || !z.ready() || !prepare_zone(z) || z.heap() == nullptr || z.vertices() == nullptr || z.indices() == nullptr)
			return 0;
		const zm::zone &d = z.data();
		const D3D12_RESOURCE_DESC vd = z.vertices()->GetDesc(), id = z.indices()->GetDesc();
		const uint32_t vertex_count = uint32_t(vd.Width / 48), index_count = uint32_t(id.Width / 4), place_count = uint32_t(d.placements.size());
		if (vertex_count == 0 || index_count == 0)
			return 0;
		// The placements in the cascade: their boxes' corners through world_to_clip within x, y of -1..1 (a box all to
		// one side of it is out); depth isn't cut (anything along the rays shades)
		for (size_t i = 0; i < _old.size();)
			if (_slot - _old[i].second > kRingSlots)
			{
				drop(_old[i].first);
				_old.erase(_old.begin() + ptrdiff_t(i));
			}
			else
				++i;
		const uint32_t slot = _slot++ % kRingSlots;
		uint32_t *out = _mapped + size_t(slot) * kMaxVisible;
		struct group
		{
			uint32_t model, tested, first, count;
		};
		std::vector<group> groups;
		uint32_t listed = 0;
		const float *M = world_to_clip;
		for (uint32_t i : _by_model)
		{
			const float *b = &_boxes[size_t(i) * 6];
			float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
			for (int c = 0; c < 8; ++c)
			{
				const float q[3] = { (c & 1) ? b[3] : b[0], (c & 2) ? b[4] : b[1], (c & 4) ? b[5] : b[2] };
				const float x = q[0] * M[0] + q[1] * M[4] + q[2] * M[8] + M[12], y = q[0] * M[1] + q[1] * M[5] + q[2] * M[9] + M[13];
				x0 = std::min(x0, x);
				x1 = std::max(x1, x);
				y0 = std::min(y0, y);
				y1 = std::max(y1, y);
			}
			if (x1 < -1.0f || x0 > 1.0f || y1 < -1.0f || y0 > 1.0f || listed >= kMaxVisible)
				continue;
			const zm::placement &p = d.placements[i];
			const uint32_t tested = (p.flags & zm::kAlphaTested) != 0 ? 1u : 0u;
			if (groups.empty() || groups.back().model != p.model || groups.back().tested != tested)
				groups.push_back({ p.model, tested, listed, 0 });
			out[listed++] = i;
			groups.back().count++;
		}
		if (groups.empty())
			return 0;

		ID3D12DescriptorHeap *const heap = z.heap();
		list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
		const D3D12_VIEWPORT vp = { 0.0f, 0.0f, float(size), float(size), 0.0f, 1.0f };
		const D3D12_RECT sc = { 0, 0, LONG(size), LONG(size) };
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &sc);
		list->SetDescriptorHeaps(1, &heap);
		list->SetGraphicsRootSignature(_root);
		list->SetPipelineState(_pso);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->SetGraphicsRootShaderResourceView(1, z.vertices()->GetGPUVirtualAddress());
		list->SetGraphicsRootShaderResourceView(2, z.indices()->GetGPUVirtualAddress());
		list->SetGraphicsRootShaderResourceView(3, _places->GetGPUVirtualAddress());
		list->SetGraphicsRootShaderResourceView(4, _ring->GetGPUVirtualAddress() + UINT64(slot) * kMaxVisible * 4);
		list->SetGraphicsRoot32BitConstants(0, 16, world_to_clip, 0);
		const uint32_t counts[3] = { index_count, vertex_count, place_count };
		list->SetGraphicsRoot32BitConstants(0, 3, counts, 19);
		// (a table must always be set: the white stand-in where nothing's tested)
		D3D12_GPU_DESCRIPTOR_HANDLE bound = z.texture_srv(zm::kNone);
		list->SetGraphicsRootDescriptorTable(5, bound);
		uint32_t triangles = 0;
		for (const group &gr : groups)
		{
			if (gr.model >= d.models.size())
				continue;
			const zm::model &m = d.models[gr.model];
			for (uint32_t k = 0; k < m.pieces && m.first_piece + k < d.pieces.size(); ++k)
			{
				const zm::piece &pc = d.pieces[m.first_piece + k];
				if (pc.triangles == 0 || (pc.bits & zm::kBlended) != 0)
					continue;
				if (gr.tested != 0)
				{
					const D3D12_GPU_DESCRIPTOR_HANDLE h = z.texture_srv(pc.texture);
					if (h.ptr != bound.ptr)
					{
						list->SetGraphicsRootDescriptorTable(5, h);
						bound = h;
					}
				}
				const uint32_t c[3] = { pc.first_index, gr.tested, gr.first };
				list->SetGraphicsRoot32BitConstants(0, 3, c, 16);
				list->DrawInstanced(pc.triangles * 3, gr.count, 0, 0);
				triangles += pc.triangles * gr.count;
			}
		}
		return triangles;
	}
}
