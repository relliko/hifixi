/*
 * The zone's sun shadow casters from the native zone data (zoneshadow.hpp). One root signature:
 *   0  22 constants b0  world_to_clip (16), the piece's first index, alpha tested, the list's start, and the counts
 *                       every read is clamped below (indices, vertices, placements)
 *   1  SRV t0  the zone's vertices (48 bytes)   2  SRV t1  its indices (32-bit, absolute)
 *   3  SRV t2  the placements (64 bytes: three rows, the translation, how far along the sway vectors, 3 spare)
 *   4  SRV t3  the visible list (uints)
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
struct place { float3 r0; float3 r1; float3 r2; float3 t; float sway; float3 spare; };
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
	const float3 o = v.pos + v.sway * p.sway; // (kind 2's frame: draw.cpp's vs)
	const float3 w = o.x * p.r0 + o.y * p.r1 + o.z * p.r2 + p.t;
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
		constexpr uint32_t kOldRecords = 256; // (records after which a zone's placements left behind are let go)
		constexpr uint32_t kMaxVisible = 65536;
		constexpr uint32_t kMaxMovers = 4096;   // (a frame's: the plants and door parts within the casters' map)
		constexpr uint32_t kPlaceFloats = 16;
		constexpr UINT64 kMoverSlot = UINT64(kMaxMovers) * (kPlaceFloats * 4 + 4);
		constexpr UINT kConstants = 22;

		// A placement's key for finding it again from a pass's object: its model and its translation to a sixteenth
		uint64_t place_key(uint32_t model, const float t[3])
		{
			uint64_t k = uint64_t(model) * 0x9E3779B97F4A7C15ull;
			for (int a = 0; a < 3; ++a)
				k = (k ^ uint64_t(uint32_t(int32_t(std::lround(double(t[a]) * 16.0))))) * 0xBF58476D1CE4E5B9ull;
			return k;
		}

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
		if (_mover_ring != nullptr && _mover_mapped != nullptr)
			_mover_ring->Unmap(0, nullptr);
		_mover_mapped = nullptr;
		drop(_mover_ring);
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
			bd.Width = kRingSlots * kMoverSlot;
			ok = ok && SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&_mover_ring))) &&
				SUCCEEDED(_mover_ring->Map(0, &none, reinterpret_cast<void **>(&_mover_mapped)));
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
		if (_zone == &z && _zone_map == d.map && _places != nullptr)
			return _any;
		_zone = &z;
		_zone_map = d.map;
		_boxes.clear();
		_by_model.clear();
		_any = false;
		_swaying.clear();
		_sway_of.clear();
		_sway.assign(d.placements.size(), 0.0f);
		_sway_pass.assign(d.placements.size(), 0);
		_model_sway.assign(d.models.size(), 0.0f);
		_model_sway_pass.assign(d.models.size(), 0);
		_door_model.assign(d.models.size(), 0);
		_doors.clear();
		_mover_generation++;
		// (the old placements' buffer may still be read by the GPU: let go kOldRecords records on)
		if (_places != nullptr)
			_old.push_back({ _places, _slot });
		_places = nullptr;
		const size_t n = d.placements.size();
		if (n == 0)
			return false;
		D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = UINT64(n) * kPlaceFloats * 4;
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
			float *const to = mapped + i * kPlaceFloats;
			std::memcpy(to, p.m, 36);
			std::memcpy(to + 9, p.t, 12);
			to[12] = to[13] = to[14] = to[15] = 0.0f; // (at rest)
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
		// (the swaying ones, in the same order, and how they're found from a pass's objects)
		for (const uint32_t i : _by_model)
			if (d.models[d.placements[i].model].kind == 2)
			{
				_swaying.push_back(i);
				_sway_of.emplace(place_key(d.placements[i].model, d.placements[i].t), i);
			}
		return _any;
	}

	bool zone_shadow::is_mover(uint32_t model) const
	{
		const zm::zone &d = _zone->data();
		return model < d.models.size() && (d.models[model].kind == 2 || (model < _door_model.size() && _door_model[model] != 0));
	}

	bool zone_shadow::bounds(gpu_zone &z, float lo[3], float hi[3])
	{
		if (_pso == nullptr || !z.ready() || !prepare_zone(z))
			return false;
		std::memcpy(lo, _lo, 12);
		std::memcpy(hi, _hi, 12);
		return true;
	}

	uint32_t zone_shadow::record(ID3D12GraphicsCommandList *list, gpu_zone &z, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, uint32_t size, bool hole)
	{
		if (_pso == nullptr || !z.ready() || !prepare_zone(z) || z.heap() == nullptr || z.vertices() == nullptr || z.indices() == nullptr)
			return 0;
		const zm::zone &d = z.data();
		// The placements in the cascade: their boxes' corners through world_to_clip within x, y of -1..1 (a box all to
		// one side of it is out); depth isn't cut (anything along the rays shades)
		for (size_t i = 0; i < _old.size();)
			if (_slot - _old[i].second > kOldRecords)
			{
				drop(_old[i].first);
				_old.erase(_old.begin() + ptrdiff_t(i));
			}
			else
				++i;
		_slot++;
		// (the list from the ring's next free place, back at its start when a whole kMaxVisible wouldn't fit: a record
		// takes only what it lists, so a frame's cascades and lamp faces together come nowhere near going round)
		if (_ring_at + kMaxVisible > kRingSlots * kMaxVisible)
			_ring_at = 0;
		const uint32_t first = _ring_at;
		uint32_t *out = _mapped + first;
		std::vector<group> groups;
		uint32_t listed = 0;
		const float *M = world_to_clip;
		// (a cascade looks along parallel rays, w 1 everywhere; a lamp's face from a point, w the depth along its middle:
		// then what's behind it or past its far plane is out as well)
		const bool ortho = M[3] == 0.0f && M[7] == 0.0f && M[11] == 0.0f;
		// (the hole the moving layer fills: only in a near cascade, its first column's length being 1 / its reach)
		const float reach = 1.0f / std::max(std::sqrt(M[0] * M[0] + M[4] * M[4] + M[8] * M[8]), 1e-6f);
		const float cut = hole && ortho && reach <= kHoleReach ? kHole / reach : 0.0f;
		// (each test on a box exactly as on its 8 corners, from its middle and half sizes: a linear function's least and
		// greatest over a box are its value at the middle less and plus its weights' sizes times the half sizes. A box is
		// out when it's wholly past one plane: x > w, x < -w, y > w, y < -w; and for a lamp's face z > w, z < 0)
		auto spread = [](const float *col, const float h[3]) { return std::fabs(col[0]) * h[0] + std::fabs(col[4]) * h[1] + std::fabs(col[8]) * h[2]; };
		const float planes[6][4] = { // (each a column (x, y, z rows' weights, then the constant) of the function past it: > 0)
			{ M[0] - M[3], M[4] - M[7], M[8] - M[11], M[12] - M[15] }, { -M[0] - M[3], -M[4] - M[7], -M[8] - M[11], -M[12] - M[15] },
			{ M[1] - M[3], M[5] - M[7], M[9] - M[11], M[13] - M[15] }, { -M[1] - M[3], -M[5] - M[7], -M[9] - M[11], -M[13] - M[15] },
			{ M[2] - M[3], M[6] - M[7], M[10] - M[11], M[14] - M[15] }, { -M[2], -M[6], -M[10], -M[14] } };
		for (uint32_t i : _by_model)
		{
			const float *b = &_boxes[size_t(i) * 6];
			const float c[3] = { 0.5f * (b[0] + b[3]), 0.5f * (b[1] + b[4]), 0.5f * (b[2] + b[5]) };
			const float h[3] = { 0.5f * (b[3] - b[0]), 0.5f * (b[4] - b[1]), 0.5f * (b[5] - b[2]) };
			bool culled = listed >= kMaxVisible;
			for (int k = 0; k < (ortho ? 4 : 6) && !culled; ++k)
			{
				const float *f = planes[k];
				culled = c[0] * f[0] + c[1] * f[1] + c[2] * f[2] + f[3] - (std::fabs(f[0]) * h[0] + std::fabs(f[1]) * h[1] + std::fabs(f[2]) * h[2]) > 0.0f;
			}
			if (culled)
				continue;
			const zm::placement &p = d.placements[i];
			if (cut > 0.0f && is_mover(p.model))
			{
				const float x = c[0] * M[0] + c[1] * M[4] + c[2] * M[8] + M[12], y = c[0] * M[1] + c[1] * M[5] + c[2] * M[9] + M[13];
				const float sx = spread(M, h), sy = spread(M + 1, h);
				if (x - sx >= -cut && x + sx <= cut && y - sy >= -cut && y + sy <= cut)
					continue;
			}
			const uint32_t tested = (p.flags & zm::kAlphaTested) != 0 ? 1u : 0u;
			if (groups.empty() || groups.back().model != p.model || groups.back().tested != tested)
				groups.push_back({ p.model, tested, listed, 0 });
			out[listed++] = i;
			groups.back().count++;
		}
		_ring_at = first + ((listed + 63u) & ~63u);
		if (groups.empty())
			return 0;
		return draw_groups(list, z, world_to_clip, dsv, size, _places->GetGPUVirtualAddress(), _ring->GetGPUVirtualAddress() + UINT64(first) * 4,
			uint32_t(d.placements.size()), groups);
	}

	uint32_t zone_shadow::draw_groups(ID3D12GraphicsCommandList *list, gpu_zone &z, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, uint32_t size,
		D3D12_GPU_VIRTUAL_ADDRESS places, D3D12_GPU_VIRTUAL_ADDRESS visible, uint32_t place_count, const std::vector<group> &groups)
	{
		const zm::zone &d = z.data();
		const D3D12_RESOURCE_DESC vd = z.vertices()->GetDesc(), id = z.indices()->GetDesc();
		const uint32_t vertex_count = uint32_t(vd.Width / 48), index_count = uint32_t(id.Width / 4);
		if (vertex_count == 0 || index_count == 0 || place_count == 0)
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
		list->SetGraphicsRootShaderResourceView(3, places);
		list->SetGraphicsRootShaderResourceView(4, visible);
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

	void zone_shadow::note_pass(gpu_zone &z, const frame_object *objects, uint32_t count, uint32_t zone_index)
	{
		if (_pso == nullptr || !z.ready() || !prepare_zone(z))
			return;
		const zm::zone &d = z.data();
		++_pass;
		_doors.clear();
		for (uint32_t i = 0; i < count; ++i)
		{
			const frame_object &o = objects[i];
			if (o.zone != zone_index || o.model >= d.models.size())
				continue;
			if ((o.flags & kDoor) != 0)
			{
				if (_doors.size() < kMaxMovers)
					_doors.push_back(o);
				if (_door_model[o.model] == 0)
				{
					// (a model first seen moving: the cascades leave it out from now on, drawn again for that)
					_door_model[o.model] = 1;
					_mover_generation++;
				}
				continue;
			}
			if (d.models[o.model].kind != 2)
				continue;
			const float sway = std::isfinite(o.sway) ? std::clamp(o.sway, 0.0f, 1.0f) : 0.0f;
			_model_sway[o.model] = sway;
			_model_sway_pass[o.model] = _pass;
			const float t[3] = { o.world[3], o.world[7], o.world[11] };
			if (const auto found = _sway_of.find(place_key(o.model, t)); found != _sway_of.end())
			{
				_sway[found->second] = sway;
				_sway_pass[found->second] = _pass;
			}
		}
	}

	// The moving layer: every swaying placement and the last pass's door parts whose boxes fall in the map, each its own
	// place in the movers' ring (its matrix, its sway), the list in model order
	uint32_t zone_shadow::record_movers(ID3D12GraphicsCommandList *list, gpu_zone &z, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, uint32_t size)
	{
		_last_movers = 0;
		if (_pso == nullptr || _mover_mapped == nullptr || !z.ready() || !prepare_zone(z) || z.heap() == nullptr || z.vertices() == nullptr || z.indices() == nullptr)
			return 0;
		if (_swaying.empty() && _doors.empty())
			return 0;
		const zm::zone &d = z.data();
		const float *M = world_to_clip;
		auto in_map = [&](const float b[6]) {
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
			return !(x1 < -1.0f || x0 > 1.0f || y1 < -1.0f || y0 > 1.0f);
		};
		struct mover
		{
			uint32_t model, tested;
			float place[kPlaceFloats];
		};
		std::vector<mover> movers;
		for (const uint32_t i : _swaying)
		{
			if (movers.size() >= kMaxMovers)
				break;
			if (!in_map(&_boxes[size_t(i) * 6]))
				continue;
			const zm::placement &p = d.placements[i];
			mover mv = { p.model, (p.flags & zm::kAlphaTested) != 0 ? 1u : 0u, {} };
			std::memcpy(mv.place, p.m, 36);
			std::memcpy(mv.place + 9, p.t, 12);
			// (its own sway when the last pass drew it; else its model's in that pass, else its own last)
			mv.place[12] = _sway_pass[i] == _pass ? _sway[i] : _model_sway_pass[p.model] == _pass ? _model_sway[p.model] : _sway[i];
			movers.push_back(mv);
		}
		for (const frame_object &o : _doors)
		{
			if (movers.size() >= kMaxMovers)
				break;
			const zm::model &m = d.models[o.model];
			float b[6] = { 1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f };
			for (int c = 0; c < 8; ++c)
			{
				const float q[3] = { (c & 1) ? m.hi[0] : m.lo[0], (c & 2) ? m.hi[1] : m.lo[1], (c & 4) ? m.hi[2] : m.lo[2] };
				for (int a = 0; a < 3; ++a)
				{
					const float w = q[0] * o.world[a * 4 + 0] + q[1] * o.world[a * 4 + 1] + q[2] * o.world[a * 4 + 2] + o.world[a * 4 + 3];
					b[a] = std::min(b[a], w);
					b[3 + a] = std::max(b[3 + a], w);
				}
			}
			if (!in_map(b))
				continue;
			mover mv = { o.model, (o.flags & zm::kAlphaTested) != 0 ? 1u : 0u, {} };
			// (frame_object's rows are the world's x, y, z; a place's are the model's x, y, z axes in the world)
			for (int k = 0; k < 3; ++k)
			{
				mv.place[k] = o.world[k * 4 + 0];
				mv.place[3 + k] = o.world[k * 4 + 1];
				mv.place[6 + k] = o.world[k * 4 + 2];
				mv.place[9 + k] = o.world[k * 4 + 3];
			}
			movers.push_back(mv);
		}
		if (movers.empty())
			return 0;
		std::stable_sort(movers.begin(), movers.end(), [](const mover &a, const mover &b) { return a.model != b.model ? a.model < b.model : a.tested < b.tested; });
		const uint32_t slot = _mover_slot++ % kRingSlots;
		uint8_t *const base = _mover_mapped + slot * kMoverSlot;
		float *const places = reinterpret_cast<float *>(base);
		uint32_t *const visible = reinterpret_cast<uint32_t *>(base + UINT64(kMaxMovers) * kPlaceFloats * 4);
		std::vector<group> groups;
		for (uint32_t i = 0; i < uint32_t(movers.size()); ++i)
		{
			std::memcpy(places + size_t(i) * kPlaceFloats, movers[i].place, sizeof(movers[i].place));
			visible[i] = i;
			if (groups.empty() || groups.back().model != movers[i].model || groups.back().tested != movers[i].tested)
				groups.push_back({ movers[i].model, movers[i].tested, i, 0 });
			groups.back().count++;
		}
		_last_movers = uint32_t(movers.size());
		const D3D12_GPU_VIRTUAL_ADDRESS at = _mover_ring->GetGPUVirtualAddress() + slot * kMoverSlot;
		return draw_groups(list, z, world_to_clip, dsv, size, at, at + UINT64(kMaxMovers) * kPlaceFloats * 4, _last_movers, groups);
	}
}
