/*
 * HiFi XI's own 2D renderer (render2d.hpp; docs/own-frame/2d.md).
 *
 * One root signature: b0 eight constants (stage 0's colour and alpha operations, the alpha test and shading, the
 * texture factor, 2 / the Direct3D 8 target's size), t1 a root SRV of the vertex ring (the vertices as Direct3D 8 had
 * them, 28 bytes each), t0 a table of one texture (the store's slot), s0 a table of one sampler (the heap holds every
 * MIN x MAG x MIP x U x V combination). One pipeline per blend (frame2d.hpp blend_key) and target format.
 */

#include <windows.h>
#include "render2d.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace hifi::ui2d::render
{
	namespace
	{
		constexpr uint32_t kNullSlot = kMaxTextures;             // (the SRV of a texture of nothing)
		// Samplers: 14 filters (MIN x MAG x MIP (none, point, linear) as stage 0 asks; then anisotropic, without and with mips)
		// x U x V (wrap, mirror, clamp)
		constexpr uint32_t kFilters = 2 * 2 * 3 + 2, kAnisotropic = 12;
		constexpr uint32_t kSamplers = kFilters * 3 * 3;
		constexpr uint32_t kPipelines = 256;
		constexpr uint32_t kConstants = 10;
		constexpr uint32_t kTargetViews = 8;
		constexpr uint64_t kRingBytes = 16ull << 20;             // (the vertices of about 25 frames of 2,000 quads)
		constexpr double kDropDelay = 5.0;                       // seconds a dropped texture stays (dgVoodoo's lists in flight)

		const char *const kShader = R"(
cbuffer c : register(b0)
{
	uint colour_ops, alpha_ops, test, tfactor;
	uint base;      // (the draw's first vertex in the ring: SV_VertexID starts at 0 on some drivers, WARP among them)
	uint unused;
	float2 origin;  // the Direct3D 8 viewport's corner, and 2 / its size
	float2 scale;
};
struct V { float4 p; uint c; float2 t; };
StructuredBuffer<V> verts : register(t1);
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct O
{
	float4 pos : SV_Position;
	float4 col : COLOR0;
	nointerpolation float4 flat_col : COLOR1;
	float2 uv : TEXCOORD0;
};
float4 unpack(uint c)
{
	return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0;
}
O vs(uint id : SV_VertexID)
{
	V v = verts[id + base];
	O o;
	const float w = v.p.w > 0.0 ? 1.0 / v.p.w : 1.0;
	o.pos = float4(((v.p.x - origin.x) * scale.x - 1.0) * w, (1.0 - (v.p.y - origin.y) * scale.y) * w, v.p.z * w, w);
	o.col = unpack(v.c);
	o.flat_col = o.col;
	o.uv = v.t;
	return o;
}
float4 arg(uint a, float4 d, float4 t, float4 f)
{
	const uint s = a & 15;
	float4 v = s == 2 ? t : s == 3 ? f : s == 4 ? float4(0, 0, 0, 0) : d;
	if (a & 0x10)
		v = 1.0 - v;
	if (a & 0x20)
		v = v.aaaa;
	return v;
}
float4 op(uint ops, float4 d, float4 t, float4 f)
{
	const uint o = ops & 255;
	const float4 a1 = arg((ops >> 8) & 255, d, t, f), a2 = arg((ops >> 16) & 255, d, t, f);
	switch (o)
	{
	case 2: return a1;
	case 3: return a2;
	case 4: return a1 * a2;
	case 5: return saturate(a1 * a2 * 2.0);
	case 6: return saturate(a1 * a2 * 4.0);
	case 7: return saturate(a1 + a2);
	case 8: return saturate(a1 + a2 - 0.5);
	case 9: return saturate((a1 + a2 - 0.5) * 2.0);
	case 10: return saturate(a1 - a2);
	case 11: return saturate(a1 + a2 - a1 * a2);
	case 12: return lerp(a2, a1, d.a);
	case 13: return lerp(a2, a1, t.a);
	case 14: return lerp(a2, a1, f.a);
	case 15: return saturate(a1 + a2 * (1.0 - t.a));
	case 16: return lerp(a2, a1, d.a);
	default: return d;
	}
}
float4 ps(O i) : SV_Target
{
	const float4 d = ((test >> 16) & 1) != 0 ? i.flat_col : i.col;
	const float4 t = tex.Sample(smp, i.uv);
	const float4 f = unpack(tfactor);
	float4 r = d;
	if ((colour_ops & 255) != 1)
	{
		r.rgb = op(colour_ops, d, t, f).rgb;
		r.a = (alpha_ops & 255) != 1 ? op(alpha_ops, d, t, f).a : d.a;
	}
	if (((test >> 24) & 1) != 0)
	{
		const uint func = test & 255;
		const float a = r.a, ref = float((test >> 8) & 255) / 255.0;
		const bool kept = func == 1 ? false : func == 2 ? a < ref : func == 3 ? a == ref : func == 4 ? a <= ref : func == 5 ? a > ref :
			func == 6 ? a != ref : func == 7 ? a >= ref : true;
		if (!kept)
			discard;
	}
	return r;
}
)";

		struct tex_slot
		{
			std::atomic<ID3D12Resource *> res { nullptr };
			std::atomic<uint8_t> state { 0 }; // kFree, kPending, kReady, kFailed, kDropped
			uint64_t bytes = 0;
			double drop_at = 0.0;
		};
		enum : uint8_t { kFree, kPending, kReady, kFailed, kDropped };

		struct pending_tex
		{
			uint32_t slot = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::vector<texfmt::level> levels;
		};

		struct pso_entry
		{
			uint64_t key = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			uint32_t samples = 0;
			ID3D12PipelineState *pso = nullptr;
		};

		struct state
		{
			std::atomic<bool> ready { false };
			std::mutex use; // (record() runs under it: release() waits for one under way)
			ID3D12Device *device = nullptr;
			ID3D12RootSignature *root = nullptr;
			ID3DBlob *vs = nullptr, *ps = nullptr;
			ID3D12DescriptorHeap *srv_heap = nullptr, *sampler_heap = nullptr, *rtv_heap = nullptr;
			UINT srv_inc = 0, sampler_inc = 0, rtv_inc = 0;
			ID3D12Resource *ring = nullptr;
			uint8_t *ring_cpu = nullptr;
			D3D12_GPU_VIRTUAL_ADDRESS ring_gpu = 0;
			uint64_t ring_at = 0;
			ID3D12CommandQueue *queue = nullptr;
			ID3D12CommandAllocator *allocator = nullptr;
			ID3D12GraphicsCommandList *list = nullptr;
			ID3D12Fence *fence = nullptr;
			uint64_t fence_value = 0;
			HANDLE event = nullptr;
			tex_slot tex[kMaxTextures + 1];
			uint32_t next_free = 0;
			std::vector<pending_tex> pending;
			uint8_t null_texel[4] = { 0, 0, 0, 255 };
			std::atomic<uint32_t> forced { filtering::app }, anisotropy { 1 };
			std::atomic<bool> keep_point { true };
			std::mutex pso_mutex; // (writers only: made outside it)
			pso_entry psos[kPipelines];
			std::atomic<uint32_t> pso_count { 0 };
			std::atomic<uint32_t> target_format { 0 }, target_samples { 0 };
			std::mutex rtv_mutex;
			ID3D12Resource *rtv_res[kTargetViews] = {};
			DXGI_FORMAT rtv_format[kTargetViews] = {};
			uint32_t rtv_next = 0;
			std::atomic<uint64_t> segments { 0 }, draws { 0 }, batches { 0 }, vertices { 0 };
			uint64_t texture_bytes = 0, uploads = 0;
			uint32_t textures = 0;
		};
		state g;

		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		void release_objects()
		{
			for (uint32_t i = 0; i < g.pso_count.load(); ++i)
				drop(g.psos[i].pso);
			g.pso_count.store(0);
			for (tex_slot &t : g.tex)
			{
				ID3D12Resource *r = t.res.exchange(nullptr);
				if (r != nullptr)
					r->Release();
				t.state.store(kFree);
				t.bytes = 0;
			}
			g.pending.clear();
			g.next_free = 0;
			g.textures = 0;
			g.texture_bytes = 0;
			if (g.ring != nullptr && g.ring_cpu != nullptr)
				g.ring->Unmap(0, nullptr);
			g.ring_cpu = nullptr;
			drop(g.ring);
			drop(g.list);
			drop(g.allocator);
			drop(g.queue);
			drop(g.fence);
			if (g.event != nullptr)
				CloseHandle(g.event);
			g.event = nullptr;
			drop(g.srv_heap);
			drop(g.sampler_heap);
			drop(g.rtv_heap);
			drop(g.root);
			drop(g.vs);
			drop(g.ps);
			for (ID3D12Resource *&r : g.rtv_res)
				r = nullptr;
			g.device = nullptr;
		}

		D3D12_BLEND factor(uint8_t b, bool alpha)
		{
			switch (b)
			{
			case 1: return D3D12_BLEND_ZERO;
			case 2: return D3D12_BLEND_ONE;
			case 3: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
			case 4: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
			case 5: return D3D12_BLEND_SRC_ALPHA;
			case 6: return D3D12_BLEND_INV_SRC_ALPHA;
			case 7: return D3D12_BLEND_DEST_ALPHA;
			case 8: return D3D12_BLEND_INV_DEST_ALPHA;
			case 9: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
			case 10: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
			case 11: return D3D12_BLEND_SRC_ALPHA_SAT;
			default: return D3D12_BLEND_ONE;
			}
		}

		D3D12_BLEND_OP blend_op(uint8_t o)
		{
			return o >= 1 && o <= 5 ? D3D12_BLEND_OP(o) : D3D12_BLEND_OP_ADD;
		}

		ID3D12PipelineState *make_pso(const blend_key &k, DXGI_FORMAT format, uint32_t samples)
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature = g.root;
			d.VS = { g.vs->GetBufferPointer(), g.vs->GetBufferSize() };
			d.PS = { g.ps->GetBufferPointer(), g.ps->GetBufferSize() };
			D3D12_RENDER_TARGET_BLEND_DESC &b = d.BlendState.RenderTarget[0];
			uint8_t src = k.src, dst = k.dst;
			if (src == 12) // (D3DBLEND_BOTHSRCALPHA, BOTHINVSRCALPHA: the source names both)
				src = 5, dst = 6;
			else if (src == 13)
				src = 6, dst = 5;
			b.BlendEnable = k.enable != 0;
			b.SrcBlend = factor(src, false);
			b.DestBlend = factor(dst, false);
			b.BlendOp = blend_op(k.op);
			b.SrcBlendAlpha = factor(src, true);
			b.DestBlendAlpha = factor(dst, true);
			b.BlendOpAlpha = blend_op(k.op);
			b.LogicOp = D3D12_LOGIC_OP_NOOP;
			b.RenderTargetWriteMask = UINT8(k.write_mask & 15);
			d.SampleMask = UINT_MAX;
			d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			// (Direct3D 8 culls counter-clockwise faces by default, on the screen as seen: Direct3D 12's clockwise front)
			d.RasterizerState.CullMode = k.cull == 2 ? D3D12_CULL_MODE_FRONT : k.cull == 3 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
			d.RasterizerState.FrontCounterClockwise = FALSE;
			d.RasterizerState.DepthClipEnable = TRUE;
			d.DepthStencilState.DepthEnable = FALSE;
			d.DepthStencilState.StencilEnable = FALSE;
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets = 1;
			d.RTVFormats[0] = format;
			d.SampleDesc.Count = samples;
			ID3D12PipelineState *pso = nullptr;
			if (FAILED(g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))))
				return nullptr;
			return pso;
		}

		ID3D12PipelineState *find_pso(uint64_t key, DXGI_FORMAT format, uint32_t samples)
		{
			const uint32_t n = g.pso_count.load(std::memory_order_acquire);
			for (uint32_t i = 0; i < n; ++i)
				if (g.psos[i].key == key && g.psos[i].format == format && g.psos[i].samples == samples)
					return g.psos[i].pso;
			return nullptr;
		}

		// Found, or made (outside the lock) and put in
		ID3D12PipelineState *get_pso(const blend_key &k, DXGI_FORMAT format, uint32_t samples)
		{
			const uint64_t key = k.value();
			if (ID3D12PipelineState *p = find_pso(key, format, samples))
				return p;
			ID3D12PipelineState *made = make_pso(k, format, samples);
			if (made == nullptr)
				return nullptr;
			std::lock_guard<std::mutex> lock(g.pso_mutex);
			if (ID3D12PipelineState *p = find_pso(key, format, samples))
			{
				made->Release();
				return p;
			}
			const uint32_t n = g.pso_count.load(std::memory_order_relaxed);
			if (n >= kPipelines)
			{
				made->Release();
				return nullptr;
			}
			g.psos[n] = { key, format, samples, made };
			g.pso_count.store(n + 1, std::memory_order_release);
			return made;
		}

		// The filter dgVoodoo samples with: stage 0's, unless its own setting forces one (Filtering: point, bilinear,
		// linear mip, trilinear or an anisotropic level), which a texture sampled with MINFILTER POINT keeps out of when
		// KeepFilterIfPointSampled is on (as tests/ui2d_dgv.cpp finds dgVoodoo 2.87 doing)
		uint32_t filter_code(const draw &d)
		{
			uint32_t mn = d.min_filter == 2 ? 1 : 0, mg = d.mag_filter == 2 ? 1 : 0, mp = d.mip_filter == 2 ? 2 : d.mip_filter == 1 ? 1 : 0;
			const uint32_t forced = g.forced.load(std::memory_order_relaxed);
			if (forced != filtering::app && !(g.keep_point.load(std::memory_order_relaxed) && d.min_filter == 1))
				switch (forced)
				{
				case filtering::point:
					mn = mg = 0, mp = mp != 0 ? 1 : 0;
					break;
				case filtering::bilinear:
					mn = mg = 1, mp = mp != 0 ? 1 : 0;
					break;
				case filtering::linear_mip:
				case filtering::trilinear:
					mn = mg = 1, mp = mp != 0 ? 2 : 0;
					break;
				default:
					return kAnisotropic + (mp != 0 ? 1 : 0);
				}
			return (mn * 2 + mg) * 3 + mp;
		}

		uint32_t sampler_index(const draw &d)
		{
			const uint32_t u = d.address_u >= 1 && d.address_u <= 3 ? d.address_u - 1 : 0, v = d.address_v >= 1 && d.address_v <= 3 ? d.address_v - 1 : 0;
			return (filter_code(d) * 3 + u) * 3 + v;
		}

		// Every sampler stage 0 can ask for (the anisotropic ones at dgVoodoo's level)
		void write_samplers()
		{
			for (uint32_t f = 0; f < kFilters; ++f)
				for (uint32_t u = 0; u < 3; ++u)
					for (uint32_t v = 0; v < 3; ++v)
					{
						D3D12_SAMPLER_DESC s = {};
						const uint32_t mn = f < kAnisotropic ? f / 6 : 1, mg = f < kAnisotropic ? f / 3 % 2 : 1, mp = f < kAnisotropic ? f % 3 : (f - kAnisotropic) * 2;
						s.Filter = f >= kAnisotropic ? D3D12_FILTER_ANISOTROPIC : D3D12_FILTER((mn << 4) | (mg << 2) | (mp == 2 ? 1 : 0));
						s.AddressU = D3D12_TEXTURE_ADDRESS_MODE(u + 1);
						s.AddressV = D3D12_TEXTURE_ADDRESS_MODE(v + 1);
						s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
						s.MaxAnisotropy = f >= kAnisotropic ? std::max(1u, std::min(16u, g.anisotropy.load())) : 1;
						s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
						s.MaxLOD = mp == 0 ? 0.0f : D3D12_FLOAT32_MAX;
						D3D12_CPU_DESCRIPTOR_HANDLE at = g.sampler_heap->GetCPUDescriptorHandleForHeapStart();
						at.ptr += SIZE_T((f * 3 + u) * 3 + v) * g.sampler_inc;
						g.device->CreateSampler(&s, at);
					}
		}

		bool make_texture(ID3D12Resource *&out, DXGI_FORMAT format, uint32_t w, uint32_t h, uint32_t levels)
		{
			D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			rd.Width = w;
			rd.Height = h;
			rd.DepthOrArraySize = 1;
			rd.MipLevels = UINT16(levels);
			rd.Format = format;
			rd.SampleDesc.Count = 1;
			return SUCCEEDED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out)));
		}

		void write_srv(uint32_t slot, ID3D12Resource *r, DXGI_FORMAT format, uint32_t levels)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
			sv.Format = format;
			sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			sv.Texture2D.MipLevels = levels;
			D3D12_CPU_DESCRIPTOR_HANDLE at = g.srv_heap->GetCPUDescriptorHandleForHeapStart();
			at.ptr += SIZE_T(slot) * g.srv_inc;
			g.device->CreateShaderResourceView(r, &sv, at);
		}

		// The pending textures copied up on our queue and waited for: true when they all went
		bool send(std::vector<pending_tex> &work, std::string &why)
		{
			std::vector<ID3D12Resource *> staging;
			bool ok = SUCCEEDED(g.allocator->Reset()) && SUCCEEDED(g.list->Reset(g.allocator, nullptr));
			if (!ok)
				why = "the upload list couldn't be reset";
			std::vector<ID3D12Resource *> made(work.size(), nullptr);
			for (size_t i = 0; ok && i < work.size(); ++i)
			{
				pending_tex &p = work[i];
				const uint32_t levels = uint32_t(p.levels.size());
				if (!make_texture(made[i], p.format, p.levels[0].w, p.levels[0].h, levels))
				{
					ok = false;
					why = "a texture couldn't be made";
					break;
				}
				const D3D12_RESOURCE_DESC rd = made[i]->GetDesc();
				std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> spots(levels);
				std::vector<UINT> rows(levels);
				std::vector<UINT64> row_bytes(levels);
				UINT64 total = 0;
				g.device->GetCopyableFootprints(&rd, 0, levels, 0, spots.data(), rows.data(), row_bytes.data(), &total);
				D3D12_HEAP_PROPERTIES up = { D3D12_HEAP_TYPE_UPLOAD };
				D3D12_RESOURCE_DESC bd = {};
				bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				bd.Width = total;
				bd.Height = 1;
				bd.DepthOrArraySize = 1;
				bd.MipLevels = 1;
				bd.SampleDesc.Count = 1;
				bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				ID3D12Resource *buf = nullptr;
				uint8_t *cpu = nullptr;
				if (FAILED(g.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf))) ||
					FAILED(buf->Map(0, nullptr, reinterpret_cast<void **>(&cpu))))
				{
					if (buf != nullptr)
						buf->Release();
					ok = false;
					why = "no upload memory";
					break;
				}
				staging.push_back(buf);
				for (uint32_t l = 0; l < levels; ++l)
				{
					const texfmt::level &lv = p.levels[l];
					const UINT n = std::min<UINT>(rows[l], UINT(lv.bytes.size() / std::max<uint32_t>(lv.pitch, 1)));
					const size_t take = size_t(std::min<UINT64>(row_bytes[l], lv.pitch));
					for (UINT r = 0; r < n; ++r)
						std::memcpy(cpu + spots[l].Offset + UINT64(r) * spots[l].Footprint.RowPitch, lv.bytes.data() + size_t(r) * lv.pitch, take);
					D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
					dst.pResource = made[i];
					dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					dst.SubresourceIndex = l;
					src.pResource = buf;
					src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					src.PlacedFootprint = spots[l];
					g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				}
				buf->Unmap(0, nullptr);
				D3D12_RESOURCE_BARRIER b = {};
				b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				b.Transition.pResource = made[i];
				b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				g.list->ResourceBarrier(1, &b);
			}
			if (ok && FAILED(g.list->Close()))
			{
				ok = false;
				why = "the upload list couldn't be closed";
			}
			else if (!ok)
				g.list->Close();
			if (ok)
			{
				ID3D12CommandList *const lists[] = { g.list };
				g.queue->ExecuteCommandLists(1, lists);
				const uint64_t value = ++g.fence_value;
				g.queue->Signal(g.fence, value);
				if (g.fence->GetCompletedValue() < value)
				{
					g.fence->SetEventOnCompletion(value, g.event);
					WaitForSingleObject(g.event, 10000);
				}
				if (g.fence->GetCompletedValue() < value)
				{
					ok = false;
					why = "the upload didn't finish";
				}
			}
			for (ID3D12Resource *b : staging)
				b->Release();
			for (size_t i = 0; i < work.size(); ++i)
			{
				tex_slot &t = g.tex[work[i].slot];
				if (ok && made[i] != nullptr)
				{
					const uint32_t levels = uint32_t(work[i].levels.size());
					write_srv(work[i].slot, made[i], work[i].format, levels);
					uint64_t bytes = 0;
					for (const texfmt::level &l : work[i].levels)
						bytes += l.bytes.size();
					t.bytes = bytes;
					g.texture_bytes += bytes;
					t.res.store(made[i], std::memory_order_release);
					t.state.store(kReady, std::memory_order_release);
				}
				else
				{
					if (made[i] != nullptr)
						made[i]->Release();
					t.state.store(kFailed, std::memory_order_release);
				}
			}
			return ok;
		}

		// Our view of the target (made again when it changes)
		bool target_view(ID3D12Resource *target, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE &out)
		{
			std::lock_guard<std::mutex> lock(g.rtv_mutex);
			uint32_t at = kTargetViews;
			for (uint32_t i = 0; i < kTargetViews; ++i)
				if (g.rtv_res[i] == target && g.rtv_format[i] == format)
					at = i;
			const bool fresh = at == kTargetViews;
			if (fresh)
				at = g.rtv_next++ % kTargetViews;
			out = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
			out.ptr += SIZE_T(at) * g.rtv_inc;
			if (fresh)
			{
				const D3D12_RESOURCE_DESC rd = target->GetDesc();
				D3D12_RENDER_TARGET_VIEW_DESC rv = {};
				rv.Format = format;
				rv.ViewDimension = rd.SampleDesc.Count > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
				g.device->CreateRenderTargetView(target, &rv, out);
				g.rtv_res[at] = target;
				g.rtv_format[at] = format;
			}
			return true;
		}
	}

	bool make(ID3D12Device *device, std::string &why)
	{
		if (g.ready.load() && g.device == device)
			return true;
		release();
		if (device == nullptr)
		{
			why = "no device";
			return false;
		}
		g.device = device;
		using compile_fn = decltype(&D3DCompile);
		HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
		const auto compile = compiler != nullptr ? reinterpret_cast<compile_fn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
		if (compile == nullptr)
		{
			why = "d3dcompiler_47.dll isn't there";
			release_objects();
			return false;
		}
		const size_t length = std::strlen(kShader);
		ID3DBlob *errors = nullptr;
		if (FAILED(compile(kShader, length, "hifixi_2d", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g.vs, &errors)) ||
			FAILED(compile(kShader, length, "hifixi_2d", nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g.ps, &errors)))
		{
			why = errors != nullptr ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()) : "the shaders didn't compile";
			drop(errors);
			release_objects();
			return false;
		}
		drop(errors);

		D3D12_DESCRIPTOR_RANGE srv = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
		D3D12_DESCRIPTOR_RANGE smp = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0 };
		D3D12_ROOT_PARAMETER p[4] = {};
		p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		p[0].Constants = { 0, 0, kConstants };
		p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		p[1].Descriptor = { 1, 0 };
		p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		p[2].DescriptorTable = { 1, &srv };
		p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		p[3].DescriptorTable = { 1, &smp };
		p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		const D3D12_ROOT_SIGNATURE_DESC rd = { 4, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		using serialize_fn = decltype(&D3D12SerializeRootSignature);
		const auto serialize = reinterpret_cast<serialize_fn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
		ID3DBlob *blob = nullptr;
		const bool rooted = serialize != nullptr && SUCCEEDED(serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) &&
			SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root)));
		drop(blob);
		drop(errors);
		if (!rooted)
		{
			why = "the root signature couldn't be made";
			release_objects();
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kMaxTextures + 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		D3D12_DESCRIPTOR_HEAP_DESC sd = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplers, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
		D3D12_DESCRIPTOR_HEAP_DESC td = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kTargetViews };
		if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.srv_heap))) || FAILED(device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g.sampler_heap))) ||
			FAILED(device->CreateDescriptorHeap(&td, IID_PPV_ARGS(&g.rtv_heap))))
		{
			why = "the descriptor heaps couldn't be made";
			release_objects();
			return false;
		}
		g.srv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		g.sampler_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
		g.rtv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		write_samplers();

		D3D12_HEAP_PROPERTIES up = { D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = kRingBytes;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g.ring))) ||
			FAILED(g.ring->Map(0, nullptr, reinterpret_cast<void **>(&g.ring_cpu))))
		{
			why = "the vertex ring couldn't be made";
			release_objects();
			return false;
		}
		g.ring_gpu = g.ring->GetGPUVirtualAddress();
		g.ring_at = 0;

		D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
		if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue))) ||
			FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator))) ||
			FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr, IID_PPV_ARGS(&g.list))) ||
			FAILED(g.list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))) ||
			(g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) == nullptr)
		{
			why = "the upload queue couldn't be made";
			release_objects();
			return false;
		}
		g.fence_value = 0;

		// The texture of nothing
		std::vector<pending_tex> work(1);
		work[0].slot = kNullSlot;
		work[0].format = DXGI_FORMAT_B8G8R8A8_UNORM;
		work[0].levels.resize(1);
		work[0].levels[0].w = work[0].levels[0].h = 1;
		work[0].levels[0].pitch = 4;
		work[0].levels[0].bytes.assign(g.null_texel, g.null_texel + 4);
		g.tex[kNullSlot].state.store(kPending);
		if (!send(work, why))
		{
			release_objects();
			return false;
		}
		g.texture_bytes = 0;
		g.ready.store(true, std::memory_order_release);
		return true;
	}

	void release()
	{
		g.ready.store(false, std::memory_order_release);
		{
			// (a record() under way finishes first; nothing is released under the lock)
			std::lock_guard<std::mutex> lock(g.use);
		}
		if (g.queue != nullptr && g.fence != nullptr && g.event != nullptr)
		{
			const uint64_t value = ++g.fence_value;
			g.queue->Signal(g.fence, value);
			if (g.fence->GetCompletedValue() < value)
			{
				g.fence->SetEventOnCompletion(value, g.event);
				WaitForSingleObject(g.event, 2000);
			}
		}
		release_objects();
		g.target_format.store(0);
		g.target_samples.store(0);
	}

	bool ready()
	{
		return g.ready.load(std::memory_order_acquire);
	}

	filtering::setting filtering::read(const std::string &conf)
	{
		setting out;
		bool directx = false;
		size_t at = 0;
		while (at < conf.size())
		{
			size_t end = conf.find(char(10), at);
			if (end == std::string::npos)
				end = conf.size();
			std::string line = conf.substr(at, end - at);
			at = end + 1;
			while (!line.empty() && (line.back() == char(13) || line.back() == ' ' || line.back() == char(9)))
				line.pop_back();
			if (line.empty() || line[0] == ';')
				continue;
			if (line[0] == '[')
			{
				directx = line == "[DirectX]";
				continue;
			}
			const size_t eq = line.find('=');
			if (!directx || eq == std::string::npos)
				continue;
			std::string key = line.substr(0, eq), value = line.substr(eq + 1);
			while (!key.empty() && (key.back() == ' ' || key.back() == char(9)))
				key.pop_back();
			while (!value.empty() && (value.front() == ' ' || value.front() == char(9)))
				value.erase(value.begin());
			if (key == "Filtering")
			{
				const int level = std::atoi(value.c_str());
				out.forced = level >= 1 && level <= 16 ? anisotropic : value == "pointsampled" ? point : value == "bilinear" || value == "pointmip" ? bilinear :
					value == "linearmip" ? linear_mip : value == "trilinear" ? trilinear : app;
				out.anisotropy = level >= 1 && level <= 16 ? uint32_t(level) : 1;
			}
			else if (key == "KeepFilterIfPointSampled")
				out.keep_point = value == "true";
		}
		return out;
	}

	void set_filtering(uint32_t forced, uint32_t anisotropy, bool keep_point)
	{
		g.forced.store(forced);
		g.anisotropy.store(anisotropy);
		g.keep_point.store(keep_point);
		if (ready())
			write_samplers();
	}

	void set_null_texel(const uint8_t bgra[4])
	{
		std::memcpy(g.null_texel, bgra, 4);
	}

	uint32_t add_texture(const texfmt::texture &t, std::string &why)
	{
		if (!ready())
		{
			why = "the renderer isn't made";
			return kNoTexture;
		}
		pending_tex p;
		if (!texfmt::convert(t, p.format, p.levels, why))
			return kNoTexture;
		uint32_t slot = kNoTexture;
		for (uint32_t i = 0; i < kMaxTextures; ++i)
		{
			const uint32_t at = (g.next_free + i) % kMaxTextures;
			if (g.tex[at].state.load(std::memory_order_relaxed) == kFree)
			{
				slot = at;
				break;
			}
		}
		if (slot == kNoTexture)
		{
			why = "the texture store is full";
			return kNoTexture;
		}
		g.next_free = (slot + 1) % kMaxTextures;
		g.tex[slot].state.store(kPending, std::memory_order_release);
		g.textures++;
		p.slot = slot;
		g.pending.push_back(std::move(p));
		return slot;
	}

	bool upload(std::string &why)
	{
		if (g.pending.empty())
			return true;
		if (!ready())
		{
			why = "the renderer isn't made";
			for (pending_tex &p : g.pending)
				g.tex[p.slot].state.store(kFailed);
			g.pending.clear();
			return false;
		}
		std::vector<pending_tex> work;
		work.swap(g.pending);
		g.uploads++;
		return send(work, why);
	}

	bool pending()
	{
		return !g.pending.empty();
	}

	void drop_texture(uint32_t slot)
	{
		if (slot >= kMaxTextures)
			return;
		tex_slot &t = g.tex[slot];
		const uint8_t s = t.state.load();
		if (s == kFree || s == kDropped)
			return;
		if (s == kPending)
		{
			// (not sent yet: it never goes)
			for (size_t i = 0; i < g.pending.size(); ++i)
				if (g.pending[i].slot == slot)
				{
					g.pending.erase(g.pending.begin() + ptrdiff_t(i));
					break;
				}
		}
		t.drop_at = -1.0; // (collect() stamps it)
		t.state.store(kDropped, std::memory_order_release);
	}

	void collect(double now)
	{
		for (uint32_t i = 0; i < kMaxTextures; ++i)
		{
			tex_slot &t = g.tex[i];
			if (t.state.load(std::memory_order_relaxed) != kDropped)
				continue;
			if (t.drop_at < 0.0)
			{
				t.drop_at = now;
				continue;
			}
			if (now - t.drop_at < kDropDelay)
				continue;
			if (ID3D12Resource *r = t.res.exchange(nullptr))
				r->Release();
			g.texture_bytes -= std::min(g.texture_bytes, t.bytes);
			t.bytes = 0;
			g.textures -= g.textures != 0 ? 1 : 0;
			t.state.store(kFree, std::memory_order_release);
		}
	}

	bool target_known(DXGI_FORMAT &format, uint32_t &samples)
	{
		format = DXGI_FORMAT(g.target_format.load(std::memory_order_acquire));
		samples = g.target_samples.load(std::memory_order_acquire);
		return format != DXGI_FORMAT_UNKNOWN && samples != 0;
	}

	void note_target(DXGI_FORMAT format, uint32_t samples)
	{
		g.target_format.store(uint32_t(format), std::memory_order_release);
		g.target_samples.store(samples, std::memory_order_release);
	}

	bool pipeline(const blend_key &k)
	{
		DXGI_FORMAT format;
		uint32_t samples;
		if (!ready() || !target_known(format, samples))
			return false;
		return get_pso(k, format, samples) != nullptr;
	}

	const char *record(ID3D12GraphicsCommandList *list, const frame &f, const segment &s, ID3D12Resource *target, DXGI_FORMAT format,
		const grass::list_state &bound, UINT heap_count, ID3D12DescriptorHeap *const *heaps, bool draw, D3D12_RESOURCE_STATES target_state)
	{
		if (!ready())
			return "the renderer isn't made";
		std::lock_guard<std::mutex> use(g.use);
		if (!ready())
			return "the renderer isn't made";
		if (target == nullptr || format == DXGI_FORMAT_UNKNOWN)
			return "the target isn't known";
		const D3D12_RESOURCE_DESC td = target->GetDesc();
		const uint32_t samples = std::max<uint32_t>(td.SampleDesc.Count, 1);
		note_target(format, samples);
		if (heap_count == 0 || heaps == nullptr)
			return "the list's descriptor heaps aren't known";
		if (s.first + s.count > f.draw_count || s.first + s.count < s.first)
			return "the segment isn't in its frame";
		if (s.count == 0)
			return nullptr;
		// Everything it needs there first: nothing is drawn of a segment that can't be drawn whole
		uint32_t v_first = f.draws[s.first].first, v_end = v_first;
		for (uint32_t i = s.first; i < s.first + s.count; ++i)
		{
			const ui2d::draw &d = f.draws[i];
			if (d.texture != kNoTexture && (d.texture >= kMaxTextures || g.tex[d.texture].state.load(std::memory_order_acquire) != kReady))
				return "a texture isn't on the GPU";
			if (get_pso(d.blend, format, samples) == nullptr)
				return "a pipeline couldn't be made";
			if (d.target_w == 0 || d.target_h == 0)
				return "a draw has no target size";
			if (d.first < v_end || d.first + d.count > f.vertex_count)
				return "a draw's vertices aren't in order";
			v_end = d.first + d.count;
		}
		if (!draw)
			return nullptr;
		const uint64_t bytes = uint64_t(v_end - v_first) * sizeof(vertex);
		if (bytes > kRingBytes)
			return "too many vertices";
		D3D12_CPU_DESCRIPTOR_HANDLE rtv;
		if (!target_view(target, format, rtv))
			return "no view of the target";
		if (g.ring_at + bytes > kRingBytes)
			g.ring_at = 0;
		const uint64_t at = g.ring_at;
		std::memcpy(g.ring_cpu + at, f.vertices + v_first, size_t(bytes));
		g.ring_at = (at + bytes + 255) & ~uint64_t(255);

		ID3D12DescriptorHeap *const ours[2] = { g.srv_heap, g.sampler_heap };
		list->SetDescriptorHeaps(2, ours);
		list->SetGraphicsRootSignature(g.root);
		list->SetGraphicsRootShaderResourceView(1, g.ring_gpu + at);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		const float tw = float(td.Width), th = float(td.Height);
		const D3D12_RECT whole = { 0, 0, LONG(td.Width), LONG(td.Height) };
		list->RSSetScissorRects(1, &whole);
		// (A between dgVoodoo's draws may still be in the state its last read left it: a render target for ours)
		D3D12_RESOURCE_BARRIER to_target = {};
		to_target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		to_target.Transition.pResource = target;
		to_target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		to_target.Transition.StateBefore = target_state;
		to_target.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		if (target_state != D3D12_RESOURCE_STATE_RENDER_TARGET)
			list->ResourceBarrier(1, &to_target);
		list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		const D3D12_GPU_DESCRIPTOR_HANDLE srv0 = g.srv_heap->GetGPUDescriptorHandleForHeapStart(), smp0 = g.sampler_heap->GetGPUDescriptorHandleForHeapStart();

		ID3D12PipelineState *last_pso = nullptr;
		D3D12_VIEWPORT last_vp = { -1.0f, -1.0f, -1.0f, -1.0f, 0.0f, 1.0f };
		uint64_t batches = 0;
		for (uint32_t i = s.first; i < s.first + s.count;)
		{
			const ui2d::draw &d = f.draws[i];
			uint32_t count = d.count, j = i + 1;
			while (j < s.first + s.count && f.draws[j].first == d.first + count && f.draws[j].same_state(d))
				count += f.draws[j++].count;
			i = j;
			// The Direct3D 8 viewport as dgVoodoo sets it (moved by half a pixel: Direct3D 8's pixel centres), the
			// vertices placed in it: they're clipped to it as Direct3D 8 clips pre-transformed vertices
			const float sx = tw / float(d.target_w), sy = th / float(d.target_h);
			const float vw = float(d.clip[2] - d.clip[0]), vh = float(d.clip[3] - d.clip[1]);
			if (!(vw > 0.0f && vh > 0.0f) || count == 0)
				continue;
			const D3D12_VIEWPORT vp = { (float(d.clip[0]) + 0.5f) * sx, (float(d.clip[1]) + 0.5f) * sy, vw * sx, vh * sy, 0.0f, 1.0f };
			if (std::memcmp(&vp, &last_vp, sizeof(vp)) != 0)
			{
				list->RSSetViewports(1, &vp);
				last_vp = vp;
			}
			ID3D12PipelineState *const pso = find_pso(d.blend.value(), format, samples);
			if (pso != last_pso)
			{
				list->SetPipelineState(pso);
				last_pso = pso;
			}
			const float place[4] = { float(d.clip[0]), float(d.clip[1]), 2.0f / vw, 2.0f / vh };
			uint32_t c[kConstants] = { d.colour.packed(), d.alpha.packed(),
				uint32_t(d.alpha_func) | uint32_t(d.alpha_ref) << 8 | uint32_t(d.flat ? 1 : 0) << 16 | uint32_t(d.alpha_test ? 1 : 0) << 24, d.tfactor, d.first - v_first, 0 };
			std::memcpy(&c[6], place, 16);
			list->SetGraphicsRoot32BitConstants(0, kConstants, c, 0);
			D3D12_GPU_DESCRIPTOR_HANDLE srv = srv0, smp = smp0;
			srv.ptr += UINT64(d.texture == kNoTexture ? kNullSlot : d.texture) * g.srv_inc;
			smp.ptr += UINT64(sampler_index(d)) * g.sampler_inc;
			list->SetGraphicsRootDescriptorTable(2, srv);
			list->SetGraphicsRootDescriptorTable(3, smp);
			list->DrawInstanced(count, 1, 0, 0);
			batches++;
		}

		if (target_state != D3D12_RESOURCE_STATE_RENDER_TARGET)
		{
			std::swap(to_target.Transition.StateBefore, to_target.Transition.StateAfter);
			list->ResourceBarrier(1, &to_target);
		}
		// dgVoodoo's heaps, then its state (a table must be in the heaps bound when it's set)
		list->SetDescriptorHeaps(heap_count, heaps);
		bound.restore(list);
		g.segments.fetch_add(1, std::memory_order_relaxed);
		g.draws.fetch_add(s.count, std::memory_order_relaxed);
		g.batches.fetch_add(batches, std::memory_order_relaxed);
		g.vertices.fetch_add(v_end - v_first, std::memory_order_relaxed);
		return nullptr;
	}

	stats get_stats()
	{
		stats s;
		s.segments = g.segments.load();
		s.draws = g.draws.load();
		s.batches = g.batches.load();
		s.vertices = g.vertices.load();
		s.textures = g.textures;
		s.texture_bytes = g.texture_bytes;
		s.uploads = g.uploads;
		s.pipelines = g.pso_count.load();
		return s;
	}
}
