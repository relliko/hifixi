/*
 * HiFi XI's stretch of the world over the screen (upscale.hpp). One root signature:
 *   0  8 constants b0  the world's size, the screen's, RCAS's stops
 *   1  table t0        the pass's source: the world for EASU, our screen-size texture for RCAS
 *   s0  linear, clamping (EASU's gathers)
 * Its descriptors: a shader-visible heap of kSlots pairs (the world's view, our texture's), a pair a use, so one the GPU
 * may still read isn't written again for kSlots uses; and an RTV heap of one, our texture's. The texture is
 * R10G10B10A2 (EASU's output kept finer than the 8 bits it's read from), PIXEL_SHADER_RESOURCE between uses.
 */

#include <windows.h>
#include <d3dcompiler.h>
#include "upscale.hpp"
#include "../render/grass_render.hpp"
#include "../zone/inject.hpp"
#include "../render/worldfx_core.hpp"
#include "temporal.hpp"
#include "masks.hpp"
#include "fsr2_shaders.hpp"
#include "../framegen/framegen.hpp"
#include "fsr2/ffx_fsr2.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace hifi::upscale
{
	namespace
	{
		const char kSource[] =
#include "upscale_hlsl.inc"
			;
		const char kFfxA[] =
#include "ffx_a_h.inc"
			;
		const char kFfxFsr1[] =
#include "ffx_fsr1_h.inc"
			;

		constexpr UINT kSlots = 16;
		constexpr UINT kConstants = 8;
		constexpr DXGI_FORMAT kMidFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
		// (the resolve is one quad: two triangles, or a strip of four)
		constexpr uint64_t kResolveVertices = 6;

		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		// AMD's two headers for the shader's #include lines
		struct includes final : ID3DInclude
		{
			HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID *data, UINT *bytes) override
			{
				const char *text = std::strcmp(name, "ffx_a.h") == 0 ? kFfxA : std::strcmp(name, "ffx_fsr1.h") == 0 ? kFfxFsr1 : nullptr;
				if (text == nullptr)
					return E_FAIL;
				*data = text;
				*bytes = UINT(std::strlen(text));
				return S_OK;
			}
			HRESULT __stdcall Close(LPCVOID) override { return S_OK; }
		};

		struct per_list
		{
			ID3D12DescriptorHeap *heaps[2] = {};
			UINT heap_count = 0;
			ID3D12Resource *world = nullptr;
			D3D12_RESOURCE_STATES world_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			bool ended = false;   // the world's end seen here, the first draw out of it not yet
			bool pending = false; // the game's resolve drawn: ours goes in at the next call
			ID3D12Resource *target = nullptr;
			uint64_t target_rtv = 0;
			DXGI_FORMAT target_format = DXGI_FORMAT_UNKNOWN;
			D3D12_RESOURCE_STATES target_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		};

		struct made_t
		{
			ID3D12Device *device = nullptr;
			ID3D12RootSignature *root = nullptr;
			ID3D12PipelineState *easu = nullptr;
			std::vector<std::pair<DXGI_FORMAT, ID3D12PipelineState *>> rcas; // (one for each back buffer format)
			std::vector<std::pair<DXGI_FORMAT, ID3D12PipelineState *>> copy; // (FSR 2's picture onto the back buffer: likewise)
			ID3D12DescriptorHeap *srv_heap = nullptr, *rtv_heap = nullptr;
			UINT srv_step = 0;
			ID3D12Resource *mid = nullptr;
			UINT mid_w = 0, mid_h = 0;
		};

		struct state_t
		{
			std::mutex mutex;
			// (lists waiting on a resolve or ours: none, and a draw costs nothing here)
			std::atomic<uint32_t> armed{ 0 };
			std::atomic<bool> fsr{ true }, temporal{ true };
			std::atomic<float> sharpen{ 0.8f };
			made_t made;
			// (the compiled shaders, kept for pipelines of formats asked for later)
			std::vector<uint8_t> vs, easu_ps, rcas_ps, copy_ps;
			// FSR 2 (temporal.hpp): its context for A's size and the screen's, made off dgVoodoo's thread; ones let go and when
			// (released a few seconds on, past the GPU)
			std::unique_ptr<temporal::context> fsr2;
			std::vector<std::pair<std::unique_ptr<temporal::context>, ULONGLONG>> fsr2_old;
			UINT want_rw = 0, want_rh = 0; // (A's size, asked for by dgVoodoo's thread for FSR 2)
			uint32_t last_seq = 0;
			uint64_t blade_frames = 0; // (World FX's count of frames its blades wrote their motion: a new one, this frame's)
			LARGE_INTEGER last_qpc = {};
			uint64_t fsr2_drawn = 0, fsr2_skipped = 0;
			std::string fsr2_why, fsr2_failed;
			std::string fsr3_refused; // (why FSR 3.1's upscaler couldn't be made: FSR 2's since)
			std::atomic<upscale::chars_motion_fn> chars_motion{ nullptr };
			uint64_t char_frames = 0;
			std::atomic<bool> fsr3{ true };
			std::atomic<ULONGLONG> fsr2_at{ 0 };   // (when FSR 2 last drew: the jitter follows it)
			std::atomic<uint32_t> phases{ 8 };
			std::vector<std::pair<ID3D12Resource *, uint64_t>> old; // (textures let go, and the use they were let go at)
			uint64_t uses = 0;
			// What dgVoodoo's thread asked for: the screen's size, its back buffer's formats
			UINT want_w = 0, want_h = 0;
			std::vector<DXGI_FORMAT> want_formats;
			std::unordered_map<ID3D12GraphicsCommandList *, per_list> lists;
			// For the status
			uint64_t drawn = 0, skipped = 0, game_stretch = 0;
			const char *why = nullptr;
			std::string failed;
			UINT in_w = 0, in_h = 0, out_w = 0, out_h = 0;
		};
		state_t g;

		// A list's two flags, with the count of lists waiting kept (mutex held)
		void arm(per_list &l, bool ended, bool pending)
		{
			const bool was = l.ended || l.pending, now = ended || pending;
			l.ended = ended;
			l.pending = pending;
			if (was != now)
				g.armed.fetch_add(now ? 1u : ~0u, std::memory_order_relaxed);
		}

		// Every list forgotten (mutex held)
		void forget_lists()
		{
			g.lists.clear();
			g.armed.store(0, std::memory_order_relaxed);
		}

		bool bigger(ID3D12Resource *a, ID3D12Resource *b)
		{
			const D3D12_RESOURCE_DESC x = a->GetDesc(), y = b->GetDesc();
			return (x.Width > y.Width && x.Height >= y.Height) || (x.Height > y.Height && x.Width >= y.Width);
		}
		bool covers(ID3D12Resource *a, ID3D12Resource *b)
		{
			const D3D12_RESOURCE_DESC x = a->GetDesc(), y = b->GetDesc();
			return x.Width >= y.Width && x.Height >= y.Height;
		}

		// A view's format for a resource made typeless (dgVoodoo's world may be)
		DXGI_FORMAT readable(DXGI_FORMAT f)
		{
			switch (f)
			{
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_B8G8R8X8_TYPELESS:
				return DXGI_FORMAT_B8G8R8X8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			default:
				return f;
			}
		}

		void release_made(made_t &m)
		{
			for (auto &p : m.rcas)
				drop(p.second);
			m.rcas.clear();
			for (auto &p : m.copy)
				drop(p.second);
			m.copy.clear();
			drop(m.easu);
			drop(m.root);
			drop(m.srv_heap);
			drop(m.rtv_heap);
			drop(m.mid);
			m.mid_w = m.mid_h = 0;
			m.device = nullptr;
		}

		ID3D12PipelineState *pipeline(ID3D12Device *device, ID3D12RootSignature *root, const std::vector<uint8_t> &ps, DXGI_FORMAT format)
		{
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
			d.pRootSignature = root;
			d.VS = { g.vs.data(), g.vs.size() };
			d.PS = { ps.data(), ps.size() };
			d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			d.NumRenderTargets = 1;
			d.RTVFormats[0] = format;
			d.SampleDesc.Count = 1;
			d.SampleMask = UINT_MAX;
			d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			d.RasterizerState.DepthClipEnable = TRUE;
			d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			ID3D12PipelineState *pso = nullptr;
			if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso))))
				return nullptr;
			return pso;
		}

		// The shaders, root signature, EASU's pipeline and the heaps (not under the mutex)
		bool make(ID3D12Device *device, made_t &m, std::string &why)
		{
			m.device = device;
			if (g.vs.empty())
			{
				using compile_fn = decltype(&D3DCompile);
				HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
				const auto compile = compiler != nullptr ? reinterpret_cast<compile_fn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
				if (compile == nullptr)
				{
					why = "d3dcompiler_47.dll isn't there";
					return false;
				}
				includes inc;
				std::vector<uint8_t> *const outs[4] = { &g.vs, &g.easu_ps, &g.rcas_ps, &g.copy_ps };
				const char *const entries[4][2] = { { "full_vs", "vs_5_0" }, { "easu_ps", "ps_5_0" }, { "rcas_ps", "ps_5_0" }, { "copy_ps", "ps_5_0" } };
				for (int i = 0; i < 4; ++i)
				{
					ID3DBlob *code = nullptr, *errors = nullptr;
					if (FAILED(compile(kSource, std::strlen(kSource), "hifixi_upscale", nullptr, &inc, entries[i][0], entries[i][1], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
					{
						why = std::string("the upscale shader didn't compile: ") + (errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
						drop(errors);
						g.vs.clear();
						return false;
					}
					drop(errors);
					const uint8_t *p = static_cast<const uint8_t *>(code->GetBufferPointer());
					outs[i]->assign(p, p + code->GetBufferSize());
					drop(code);
				}
			}
			D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0 };
			D3D12_ROOT_PARAMETER p[2] = {};
			p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			p[0].Constants.Num32BitValues = kConstants;
			p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			p[1].DescriptorTable.NumDescriptorRanges = 1;
			p[1].DescriptorTable.pDescriptorRanges = &range;
			p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_STATIC_SAMPLER_DESC s = {};
			s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			s.MaxLOD = D3D12_FLOAT32_MAX;
			s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			const D3D12_ROOT_SIGNATURE_DESC rd = { 2, p, 1, &s, D3D12_ROOT_SIGNATURE_FLAG_NONE };
			using serialize_fn = decltype(&D3D12SerializeRootSignature);
			const auto serialize = reinterpret_cast<serialize_fn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
			ID3DBlob *blob = nullptr, *errors = nullptr;
			bool ok = serialize != nullptr && SUCCEEDED(serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) &&
				SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m.root)));
			drop(blob);
			drop(errors);
			if (ok)
				ok = (m.easu = pipeline(device, m.root, g.easu_ps, kMidFormat)) != nullptr;
			if (ok)
			{
				D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
				const D3D12_DESCRIPTOR_HEAP_DESC rh = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
				ok = SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m.srv_heap))) && SUCCEEDED(device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m.rtv_heap)));
				m.srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}
			if (!ok)
				why = "the upscale passes couldn't be made";
			return ok;
		}

		// Our screen-size texture and its target view
		ID3D12Resource *make_mid(ID3D12Device *device, UINT w, UINT h)
		{
			D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC td = {};
			td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			td.Width = w;
			td.Height = h;
			td.DepthOrArraySize = 1;
			td.MipLevels = 1;
			td.Format = kMidFormat;
			td.SampleDesc.Count = 1;
			td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			ID3D12Resource *t = nullptr;
			if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&t))))
				return nullptr;
			t->SetName(L"hifixi: upscale (EASU's picture)");
			return t;
		}

		void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
		{
			if (from == to)
				return;
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = r;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = from;
			b.Transition.StateAfter = to;
			list->ResourceBarrier(1, &b);
		}

		// FSR 2 over l.target (temporal.hpp), its picture drawn onto it, dgVoodoo's heaps and state back; false (nothing
		// drawn) when it can't be (mutex held)
		bool record_temporal(ID3D12GraphicsCommandList *list, per_list &l, const grass::list_state &state, const D3D12_RESOURCE_DESC &in, UINT out_w, UINT out_h)
		{
			made_t &m = g.made;
			auto copy = std::find_if(m.copy.begin(), m.copy.end(), [&](const auto &p) { return p.first == l.target_format; });
			zone::inject::frame_camera c;
			ID3D12Resource *depth = nullptr;
			D3D12_RESOURCE_STATES planes[2] = {};
			const char *why = m.root == nullptr ? "the passes aren't made" : l.heap_count == 0 ? "the list's descriptor heaps aren't known" :
				g.fsr2 == nullptr || !g.fsr2->fits(UINT(in.Width), in.Height, out_w, out_h, g.fsr2->v3()) ? "waiting for FSR 2's context" :
				copy == m.copy.end() ? "waiting for the screen's pipeline" :
				!zone::inject::last_camera(c) || GetTickCount64() - c.at > 250 ? "no camera from the native zone's frame" :
				!zone::inject::world_depth(depth, planes) ? "the world's depth isn't known" : nullptr;
			if (why != nullptr)
			{
				g.fsr2_why = why;
				g.fsr2_skipped++;
				return false;
			}
			temporal::camera cam;
			std::memcpy(cam.view, c.view, sizeof(cam.view));
			std::memcpy(cam.proj, c.proj, sizeof(cam.proj));
			cam.jitter[0] = c.jitter[0];
			cam.jitter[1] = c.jitter[1];
			cam.seq = c.seq;
			LARGE_INTEGER now, freq;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&freq);
			const float dt = g.last_qpc.QuadPart != 0 ? float(1000.0 * double(now.QuadPart - g.last_qpc.QuadPart) / double(freq.QuadPart)) : 16.7f;
			g.last_qpc = now;
			// (a zone pass missed: a zone change, a load, a camera cut: start over)
			const bool reset = c.seq != g.last_seq + 1;
			g.last_seq = c.seq;
			// (the blades' own motion, when World FX's blades wrote it this frame)
			ID3D12Resource *blades = nullptr;
			uint64_t blade_frames = 0;
			if (worldfx::blade_motion(blades, blade_frames) && blade_frames == g.blade_frames)
				blades = nullptr;
			g.blade_frames = blade_frames;
			std::string fail;
			// (this frame's copies of the world for FSR's masks: masks.hpp)
			const masks::taken copies = masks::take();
			// (the characters' own motion, when they were drawn with it this frame)
			ID3D12Resource *chars = nullptr;
			uint64_t char_frames = 0;
			if (const upscale::chars_motion_fn source = g.chars_motion.load(std::memory_order_relaxed))
				if (source(chars, char_frames) && char_frames == g.char_frames)
					chars = nullptr;
			g.char_frames = char_frames;
			const bool drawn = g.fsr2->record(list, l.world, l.world_state, depth, planes[0], blades, cam, std::clamp(g.sharpen.load(), 0.0f, 1.0f), dt, reset, fail,
				masks::on() ? &copies : nullptr, chars);
			if (drawn)
			{
				// Its picture onto the back buffer
				const UINT slot = UINT(g.uses++ % kSlots);
				D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
				sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				sd.Texture2D.MipLevels = 1;
				sd.Format = g.fsr2->output_format();
				m.device->CreateShaderResourceView(g.fsr2->output(), &sd, { m.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(slot * 2) * m.srv_step });
				transition(list, l.target, l.target_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
				const float k[kConstants] = { float(in.Width), float(in.Height), float(out_w), float(out_h) };
				const D3D12_VIEWPORT vp = { 0.0f, 0.0f, float(out_w), float(out_h), 0.0f, 1.0f };
				const D3D12_RECT sc = { 0, 0, LONG(out_w), LONG(out_h) };
				const D3D12_CPU_DESCRIPTOR_HANDLE target_rtv = { SIZE_T(l.target_rtv) };
				list->SetDescriptorHeaps(1, &m.srv_heap);
				list->SetGraphicsRootSignature(m.root);
				list->SetGraphicsRoot32BitConstants(0, kConstants, k, 0);
				list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				list->RSSetViewports(1, &vp);
				list->RSSetScissorRects(1, &sc);
				list->SetPipelineState(copy->second);
				list->OMSetRenderTargets(1, &target_rtv, FALSE, nullptr);
				list->SetGraphicsRootDescriptorTable(1, { m.srv_heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(slot * 2) * m.srv_step });
				list->DrawInstanced(3, 1, 0, 0);
				transition(list, l.target, D3D12_RESOURCE_STATE_RENDER_TARGET, l.target_state);
				// The frame to frame generation, when on (framegen: AMD's swap chain in dgVoodoo's), FSR 2's picture the
				// world without the UI
				framegen::frame f;
				f.depth = g.fsr2->depth();
				f.motion = g.fsr2->motion();
				f.picture = g.fsr2->output();
				f.jitter[0] = c.jitter[0];
				f.jitter[1] = c.jitter[1];
				f.fov = 2.0f * std::atan(1.0f / c.proj[5]);
				f.dt_ms = dt;
				f.reset = reset;
				std::string unused;
				framegen::prepare(list, f, unused);
			}
			// (FSR 2 set its own heaps, root signatures and pipelines, drawn or not)
			list->SetDescriptorHeaps(l.heap_count, l.heaps);
			state.restore(list);
			if (!drawn)
			{
				g.fsr2_why = fail;
				g.fsr2_skipped++;
				return false;
			}
			g.fsr2_drawn++;
			g.fsr2_why.clear();
			g.fsr2_at.store(GetTickCount64(), std::memory_order_relaxed);
			g.in_w = UINT(in.Width);
			g.in_h = in.Height;
			g.out_w = out_w;
			g.out_h = out_h;
			return true;
		}

		// The two passes over l.target, then dgVoodoo's heaps and state back (mutex held)
		void record(ID3D12GraphicsCommandList *list, per_list &l, const grass::list_state &state)
		{
			made_t &m = g.made;
			const D3D12_RESOURCE_DESC in = l.world->GetDesc(), out = l.target->GetDesc();
			const UINT out_w = UINT(out.Width), out_h = out.Height;
			if (g.want_w != out_w || g.want_h != out_h)
			{
				g.want_w = out_w;
				g.want_h = out_h;
			}
			auto rcas = std::find_if(m.rcas.begin(), m.rcas.end(), [&](const auto &p) { return p.first == l.target_format; });
			if (rcas == m.rcas.end() && l.target_format != DXGI_FORMAT_UNKNOWN &&
				std::find(g.want_formats.begin(), g.want_formats.end(), l.target_format) == g.want_formats.end())
				g.want_formats.push_back(l.target_format);
			// FSR 2 first, with the world at most the screen's size
			const bool smaller = in.Width < out_w || in.Height < out_h;
			if (g.temporal.load(std::memory_order_relaxed) && in.Width <= out_w && in.Height <= out_h)
			{
				g.want_rw = UINT(in.Width);
				g.want_rh = in.Height;
				if (record_temporal(list, l, state, in, out_w, out_h))
					return;
			}
			if (!smaller)
				return;
			const char *why = m.root == nullptr ? "the passes aren't made" : l.heap_count == 0 ? "the list's descriptor heaps aren't known" :
				l.target_format == DXGI_FORMAT_UNKNOWN ? "the screen's format isn't known" :
				m.mid == nullptr || m.mid_w != out_w || m.mid_h != out_h || rcas == m.rcas.end() ? "waiting for the screen-size texture" : nullptr;
			if (why != nullptr)
			{
				g.why = why;
				g.skipped++;
				return;
			}
			const UINT slot = UINT(g.uses++ % kSlots);
			const D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = m.srv_heap->GetCPUDescriptorHandleForHeapStart();
			const D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = m.srv_heap->GetGPUDescriptorHandleForHeapStart();
			D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			sd.Texture2D.MipLevels = 1;
			sd.Format = readable(in.Format);
			m.device->CreateShaderResourceView(l.world, &sd, { cpu0.ptr + SIZE_T(slot * 2) * m.srv_step });
			sd.Format = kMidFormat;
			m.device->CreateShaderResourceView(m.mid, &sd, { cpu0.ptr + SIZE_T(slot * 2 + 1) * m.srv_step });
			const D3D12_CPU_DESCRIPTOR_HANDLE mid_rtv = m.rtv_heap->GetCPUDescriptorHandleForHeapStart();
			m.device->CreateRenderTargetView(m.mid, nullptr, mid_rtv);

			const D3D12_RESOURCE_STATES readable_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			const D3D12_RESOURCE_STATES world_was = l.world_state;
			const bool world_moved = (world_was & readable_state) == 0;
			if (world_moved)
				transition(list, l.world, world_was, readable_state);
			transition(list, l.target, l.target_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
			transition(list, m.mid, readable_state, D3D12_RESOURCE_STATE_RENDER_TARGET);

			float c[kConstants] = { float(in.Width), float(in.Height), float(out_w), float(out_h), 2.0f * (1.0f - std::clamp(g.sharpen.load(), 0.0f, 1.0f)) };
			const D3D12_VIEWPORT vp = { 0.0f, 0.0f, float(out_w), float(out_h), 0.0f, 1.0f };
			const D3D12_RECT sc = { 0, 0, LONG(out_w), LONG(out_h) };
			list->SetDescriptorHeaps(1, &m.srv_heap);
			list->SetGraphicsRootSignature(m.root);
			list->SetGraphicsRoot32BitConstants(0, kConstants, c, 0);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->RSSetViewports(1, &vp);
			list->RSSetScissorRects(1, &sc);
			// EASU: the world up to the screen's size
			list->SetPipelineState(m.easu);
			list->OMSetRenderTargets(1, &mid_rtv, FALSE, nullptr);
			list->SetGraphicsRootDescriptorTable(1, { gpu0.ptr + UINT64(slot * 2) * m.srv_step });
			list->DrawInstanced(3, 1, 0, 0);
			transition(list, m.mid, D3D12_RESOURCE_STATE_RENDER_TARGET, readable_state);
			// RCAS: sharpened into the back buffer
			const D3D12_CPU_DESCRIPTOR_HANDLE target_rtv = { SIZE_T(l.target_rtv) };
			list->SetPipelineState(rcas->second);
			list->OMSetRenderTargets(1, &target_rtv, FALSE, nullptr);
			list->SetGraphicsRootDescriptorTable(1, { gpu0.ptr + UINT64(slot * 2 + 1) * m.srv_step });
			list->DrawInstanced(3, 1, 0, 0);

			transition(list, l.target, D3D12_RESOURCE_STATE_RENDER_TARGET, l.target_state);
			if (world_moved)
				transition(list, l.world, readable_state, world_was);
			// (its heaps before its root tables: a table must be in the heaps bound when it's set)
			list->SetDescriptorHeaps(l.heap_count, l.heaps);
			state.restore(list);
			g.drawn++;
			g.why = nullptr;
			g.in_w = UINT(in.Width);
			g.in_h = in.Height;
			g.out_w = out_w;
			g.out_h = out_h;
		}
	}

	void set_chars_motion_source(chars_motion_fn fn)
	{
		g.chars_motion.store(fn, std::memory_order_relaxed);
	}

	void set_options(bool fsr, bool temporal, float sharpen, bool fsr3)
	{
		g.fsr3.store(fsr3, std::memory_order_relaxed);
		g.fsr.store(fsr, std::memory_order_relaxed);
		g.temporal.store(temporal, std::memory_order_relaxed);
		g.sharpen.store(sharpen, std::memory_order_relaxed);
	}

	bool temporal_running()
	{
		const ULONGLONG at = g.fsr2_at.load(std::memory_order_relaxed);
		return at != 0 && GetTickCount64() - at < 250;
	}

	uint32_t jitter_phases()
	{
		return g.phases.load(std::memory_order_relaxed);
	}

	bool prepare(ID3D12Device *device, std::string &why)
	{
		if (device == nullptr)
			return false;
		UINT want_w = 0, want_h = 0, want_rw = 0, want_rh = 0;
		bool want_v3 = false;
		std::vector<DXGI_FORMAT> want_formats;
		bool fresh = false;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (!g.failed.empty() && g.made.device == device)
			{
				why = g.failed;
				return false;
			}
			fresh = g.made.device != device || g.made.root == nullptr;
			want_w = g.want_w;
			want_h = g.want_h;
			want_formats = g.want_formats;
			// (FSR 2's context: made for A's size and the screen's once dgVoodoo's thread has seen them)
			if (g.temporal.load() && !fresh && g.fsr2_failed.empty() && g.want_rw != 0 && want_w != 0 &&
				(g.fsr2 == nullptr || !g.fsr2->fits(g.want_rw, g.want_rh, want_w, want_h, g.fsr3.load() && g.fsr3_refused.empty())))
			{
				want_v3 = g.fsr3.load() && g.fsr3_refused.empty();
				want_rw = g.want_rw;
				want_rh = g.want_rh;
			}
			if (!fresh && (want_w == 0 || (g.made.mid_w == want_w && g.made.mid_h == want_h)) && want_formats.empty() && want_rw == 0 && g.fsr2_old.empty())
				return true;
		}
		std::unique_ptr<temporal::context> fsr2;
		std::string fsr2_why, v3_refused;
		if (want_rw != 0)
		{
			fsr2 = std::make_unique<temporal::context>();
			// (FSR 3.1's upscaler, unless off or refused before: then FSR 2's, as when it can't be made)
			bool v3 = want_v3;
			if (v3 && !fsr2->make(device, want_rw, want_rh, want_w, want_h, true, fsr2_why))
			{
				v3_refused = fsr2_why;
				fsr2_why.clear();
				v3 = false;
			}
			if (!v3 && !fsr2->make(device, want_rw, want_rh, want_w, want_h, false, fsr2_why))
			{
				fsr2.reset();
				const std::string shader = fsr2_shaders::last_error();
				if (!shader.empty())
					fsr2_why += " (" + shader + ")";
			}
		}
		// (made outside the mutex, swapped in under it: dgVoodoo's thread holds its device lock when it takes it)
		made_t fresh_made;
		if (fresh && !make(device, fresh_made, why))
		{
			release_made(fresh_made);
			std::lock_guard<std::mutex> lock(g.mutex);
			g.failed = why;
			g.made.device = device;
			return false;
		}
		ID3D12Resource *mid = nullptr;
		if (want_w != 0 && (fresh || g.made.mid_w != want_w || g.made.mid_h != want_h))
			mid = make_mid(device, want_w, want_h);
		std::vector<std::pair<DXGI_FORMAT, ID3D12PipelineState *>> pipelines, copies;
		ID3D12RootSignature *const root = fresh ? fresh_made.root : g.made.root;
		for (const DXGI_FORMAT f : want_formats)
		{
			if (ID3D12PipelineState *pso = pipeline(device, root, g.rcas_ps, f))
				pipelines.push_back({ f, pso });
			if (ID3D12PipelineState *pso = pipeline(device, root, g.copy_ps, f))
				copies.push_back({ f, pso });
		}
		made_t gone;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			if (fresh)
			{
				gone = g.made;
				g.made = fresh_made;
				forget_lists();
				if (gone.mid != nullptr)
				{
					g.old.push_back({ gone.mid, g.uses });
					gone.mid = nullptr;
				}
			}
			if (mid != nullptr)
			{
				if (g.made.mid != nullptr)
					g.old.push_back({ g.made.mid, g.uses });
				g.made.mid = mid;
				g.made.mid_w = want_w;
				g.made.mid_h = want_h;
			}
			for (auto &p : pipelines)
				g.made.rcas.push_back(p);
			for (auto &p : copies)
				g.made.copy.push_back(p);
			g.want_formats.clear();
			if (!v3_refused.empty())
				g.fsr3_refused = v3_refused;
			if (fsr2 != nullptr)
			{
				if (g.fsr2 != nullptr)
					g.fsr2_old.push_back({ std::move(g.fsr2), GetTickCount64() });
				g.phases.store(uint32_t(std::max(2, ffxFsr2GetJitterPhaseCount(int32_t(want_rw), int32_t(want_w)))));
				g.fsr2 = std::move(fsr2);
				g.last_seq = 0;
			}
			else if (!fsr2_why.empty())
				g.fsr2_failed = fsr2_why;
		}
		// (FSR 2's contexts let go: released once past the GPU, a few seconds on)
		std::vector<std::unique_ptr<temporal::context>> gone_fsr2;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			const ULONGLONG now = GetTickCount64();
			for (size_t i = 0; i < g.fsr2_old.size();)
				if (now - g.fsr2_old[i].second > 3000)
				{
					gone_fsr2.push_back(std::move(g.fsr2_old[i].first));
					g.fsr2_old.erase(g.fsr2_old.begin() + ptrdiff_t(i));
				}
				else
					++i;
			// (textures let go kSlots uses ago are past the GPU)
			for (size_t i = 0; i < g.old.size();)
				if (g.uses - g.old[i].second > kSlots)
				{
					drop(g.old[i].first);
					g.old.erase(g.old.begin() + ptrdiff_t(i));
				}
				else
					++i;
		}
		release_made(gone);
		return true;
	}

	void release()
	{
		made_t gone;
		std::vector<std::pair<ID3D12Resource *, uint64_t>> old;
		std::unique_ptr<temporal::context> fsr2;
		std::vector<std::pair<std::unique_ptr<temporal::context>, ULONGLONG>> fsr2_old;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			gone = g.made;
			g.made = made_t();
			old.swap(g.old);
			forget_lists();
			g.failed.clear();
			fsr2 = std::move(g.fsr2);
			fsr2_old.swap(g.fsr2_old);
			g.fsr2_failed.clear();
			g.want_rw = g.want_rh = 0;
		}
		release_made(gone);
		for (auto &o : old)
			drop(o.first);
	}

	std::vector<std::string> status()
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		std::vector<std::string> lines;
		char line[320];
		if (!g.failed.empty())
			std::snprintf(line, sizeof(line), "upscale: not made (%s)", g.failed.c_str());
		else if (!g.fsr.load())
			std::snprintf(line, sizeof(line), "upscale: off (the game's stretch); %llu frames drawn while on", static_cast<unsigned long long>(g.drawn));
		else
			std::snprintf(line, sizeof(line), "upscale: FSR 1, %llu frames drawn (the last %ux%u to %ux%u), %llu skipped%s%s%s; %llu with the background at least the screen's size (the game's stretch)",
				static_cast<unsigned long long>(g.drawn), g.in_w, g.in_h, g.out_w, g.out_h, static_cast<unsigned long long>(g.skipped),
				g.why != nullptr ? " (the last: " : "", g.why != nullptr ? g.why : "", g.why != nullptr ? ")" : "", static_cast<unsigned long long>(g.game_stretch));
		lines.push_back(line);
		if (g.temporal.load())
		{
			uint32_t shaders = 0;
			double ms = 0.0;
			fsr2_shaders::stats(shaders, ms);
			std::snprintf(line, sizeof(line), "upscale: %s %s; %llu frames drawn (%u with the grass's own motion, %u the characters'), %llu skipped%s%s%s; %u jitter phases; FSR 2's %u shaders compiled in %.0f ms",
				g.fsr2 != nullptr ? g.fsr2->name() : g.fsr3.load() && g.fsr3_refused.empty() ? "FSR 3.1" : "FSR 2",
				!g.fsr2_failed.empty() ? ("not made (" + g.fsr2_failed + ")").c_str() : g.fsr2 != nullptr ? "made" : "not made yet",
				static_cast<unsigned long long>(g.fsr2_drawn), g.fsr2 != nullptr ? g.fsr2->blade_frames() : 0u, g.fsr2 != nullptr ? g.fsr2->char_frames() : 0u,
				static_cast<unsigned long long>(g.fsr2_skipped),
				g.fsr2_why.empty() ? "" : " (the last: ", g.fsr2_why.c_str(), g.fsr2_why.empty() ? "" : ")", g.phases.load(), shaders, ms);
			lines.push_back(line);
			if (!g.fsr3_refused.empty())
				lines.push_back("upscale: FSR 3.1's upscaler couldn't be made, FSR 2's instead: " + g.fsr3_refused);
			std::snprintf(line, sizeof(line), "upscale: masks %s; %u frames with them; the world copied for them in %llu frames at the characters' end, %llu before World FX, %llu shaded, %llu before its air",
				masks::on() ? "on" : "off", g.fsr2 != nullptr ? g.fsr2->mask_frames() : 0u, static_cast<unsigned long long>(masks::frames_with(masks::opaque)),
				static_cast<unsigned long long>(masks::frames_with(masks::pre_fx)), static_cast<unsigned long long>(masks::frames_with(masks::shaded)),
				static_cast<unsigned long long>(masks::frames_with(masks::pre_air)));
			lines.push_back(line);
		}
		return lines;
	}

	void on_reset(ID3D12GraphicsCommandList *list)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		per_list &l = g.lists[list];
		arm(l, false, false);
		l = per_list();
	}

	void on_heaps(ID3D12GraphicsCommandList *list, UINT count, ID3D12DescriptorHeap *const *heaps)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		per_list &l = g.lists[list];
		l.heap_count = std::min<UINT>(count, 2);
		for (UINT i = 0; i < l.heap_count; ++i)
			l.heaps[i] = heaps[i];
	}

	void on_world_end(ID3D12GraphicsCommandList *list, ID3D12Resource *world, D3D12_RESOURCE_STATES state)
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		per_list &l = g.lists[list];
		l.world = world;
		l.world_state = state;
		arm(l, true, false);
	}

	void before_draw(ID3D12GraphicsCommandList *list, uint64_t vertices, ID3D12Resource *rt, uint64_t rtv, DXGI_FORMAT (*format_of)(uint64_t),
		const grass::list_state &state)
	{
		if (g.armed.load(std::memory_order_relaxed) == 0)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		const auto it = g.lists.find(list);
		if (it == g.lists.end())
			return;
		per_list &l = it->second;
		if (l.pending)
		{
			arm(l, false, false);
			record(list, l, state);
			return;
		}
		if (!l.ended || rt == nullptr || rt == l.world || l.world == nullptr)
			return;
		// (the first draw out of the world: the resolve, when it's a quad into something bigger)
		arm(l, false, false);
		if (!g.fsr.load(std::memory_order_relaxed) || vertices > kResolveVertices || rtv == 0)
			return;
		if (!(g.temporal.load(std::memory_order_relaxed) ? covers(rt, l.world) : bigger(rt, l.world)))
		{
			g.game_stretch++;
			return;
		}
		arm(l, false, true);
		l.target = rt;
		l.target_rtv = rtv;
		l.target_format = format_of != nullptr ? format_of(rtv) : DXGI_FORMAT_UNKNOWN;
		if (l.target_format == DXGI_FORMAT_UNKNOWN)
			l.target_format = rt->GetDesc().Format;
		l.target_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}

	void before_other(ID3D12GraphicsCommandList *list, const grass::list_state &state)
	{
		if (g.armed.load(std::memory_order_relaxed) == 0)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		const auto it = g.lists.find(list);
		if (it == g.lists.end() || !it->second.pending)
			return;
		arm(it->second, false, false);
		record(list, it->second, state);
	}

	void after_barrier(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RESOURCE_BARRIER *barriers, const grass::list_state &state)
	{
		if (g.armed.load(std::memory_order_relaxed) == 0)
			return;
		std::lock_guard<std::mutex> lock(g.mutex);
		const auto it = g.lists.find(list);
		if (it == g.lists.end())
			return;
		per_list &l = it->second;
		bool touched = false;
		for (UINT i = 0; i < count; ++i)
		{
			const D3D12_RESOURCE_BARRIER &b = barriers[i];
			if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
				continue;
			if (b.Transition.pResource == l.world && l.world != nullptr)
			{
				l.world_state = b.Transition.StateAfter;
				touched = true;
			}
			else if (b.Transition.pResource == l.target && l.target != nullptr)
			{
				l.target_state = b.Transition.StateAfter;
				touched = true;
			}
		}
		// (moved by dgVoodoo after the resolve: drawn now, from where they are)
		if (l.pending && touched)
		{
			arm(l, false, false);
			record(list, l, state);
		}
	}
}
