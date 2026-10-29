/*
 * AMD FSR 2 over the world (temporal.hpp). The motion pass's root signature:
 *   0  40 constants b0  A's width, height, the depth copy's row pitch (words), whether the blades' motion is there; the
 *                       jitter (NDC); PROJECTION's _11 and _22 (without the jitter); the reprojection: this frame's view
 *                       space to the last frame's clip (inverse VIEW x last VIEW x last PROJECTION, row vectors); VIEW's
 *                       first three rows (a world direction into view space); whether the characters' motion is there
 *   1  table            t0 the depth copy (raw), t1 the blades' motion (World FX's; a null view when there's none), t2
 *                       the characters' motion (chars::inject::motion; or a null view), u0 the depth (R32_FLOAT), u1 the
 *                       motion (R16G16_FLOAT)
 * Its descriptors: kSlots fives in a shader-visible heap of its own, five a frame.
 *
 * The masks' pass (masks.hpp) before FSR's: 4 constants b0 (A's width, height, which masks: 1 reactive, 2 transparency),
 * then a table: t0-t3 the world's copies (opaque, pre_fx, shaded, pre_air), u0 the reactive mask, u1 the transparency
 * and composition mask (R8_UNORM). Where two copies differ (each tone-mapped, its largest channel), the mask rises from
 * nothing at a difference of 0.03 to its most (0.9 reactive, 0.8 transparency) at 0.33: FSR leans on this frame there,
 * not on what it remembers. Its descriptors: kSlots sixes in a heap of its own.
 */

#include <windows.h>
#include <d3dcompiler.h>
#include "temporal.hpp"
#include "fsr2/ffx_fsr2.h"
#include "fsr2/dx12/ffx_fsr2_dx12.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace hifi::upscale::temporal
{
	namespace
	{
		const char *kMotion = R"(
cbuffer motion_constants : register(b0)
{
	uint width, height, pitch, use_blades;
	float2 jitter_ndc;
	float p11, p22;
	row_major float4x4 reproject;
	float4 view_x, view_y, view_z; // (VIEW's rows: a world direction into view space)
	uint use_chars, pad0, pad1, pad2;
};
ByteAddressBuffer depth_in : register(t0);
Texture2D<float4> blades_in : register(t1);
Texture2D<float4> chars_in : register(t2); // xyz a character's move since the last frame (world), w its view depth
RWTexture2D<float> depth_out : register(u0);
RWTexture2D<float2> motion_out : register(u1);

[numthreads(8, 8, 1)]
void motion_cs(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= width || id.y >= height)
		return;
	// (D24 in a word's low 24 bits, as the copy lays plane 0 out)
	const float z = float(depth_in.Load((id.y * pitch + id.x) * 4) & 0xFFFFFF) / 16777215.0;
	depth_out[id.xy] = z;
	// The point under the pixel's centre, without the jitter it was drawn with: z = 1 - 0.1 / d (the sky: far off)
	const float2 ndc = float2((id.x + 0.5) / width * 2.0 - 1.0, 1.0 - (id.y + 0.5) / height * 2.0) - jitter_ndc;
	const float d = z < 0.9999995 ? 0.1 / max(1.0 - z, 1e-7) : 200000.0;
	const float4 last = mul(float4(ndc.x * d / p11, ndc.y * d / p22, -d, 1.0), reproject);
	// (UV units, from this frame's place to the last frame's; none for a point the last camera had behind it)
	float2 motion = last.w > 1e-4 ? (last.xy / last.w - ndc) * float2(0.5, -0.5) : float2(0.0, 0.0);
	// A blade still the one seen here (nothing drawn over it since: its view depth the world's): its own motion, the wind's
	// sway in it
	if (use_blades != 0)
	{
		const float4 b = blades_in[id.xy];
		if (b.w > 0.5 && abs(b.z - d) <= 0.004 * b.z + 0.02)
			motion = b.xy;
	}
	// (a character still what's seen there: the point it was at, its own move back from where it is now, through the
	// last frame's camera)
	if (use_chars != 0)
	{
		const float4 c = chars_in[id.xy];
		if (c.w > 0.0 && abs(c.w - d) <= 0.01 * c.w + 0.05)
		{
			const float3 move = c.x * view_x.xyz + c.y * view_y.xyz + c.z * view_z.xyz;
			const float4 was = mul(float4(float3(ndc.x * d / p11, ndc.y * d / p22, -d) - move, 1.0), reproject);
			if (was.w > 1e-4)
				motion = (was.xy / was.w - ndc) * float2(0.5, -0.5);
		}
	}
	motion_out[id.xy] = motion;
}
)";

		const char *kMask = R"(
cbuffer mask_constants : register(b0)
{
	uint width, height, which, unused;
};
Texture2D<float4> opaque_in : register(t0);
Texture2D<float4> pre_fx_in : register(t1);
Texture2D<float4> shaded_in : register(t2);
Texture2D<float4> pre_air_in : register(t3);
RWTexture2D<float> reactive_out : register(u0);
RWTexture2D<float> composition_out : register(u1);

float3 tone(float3 c) { return c / (1.0 + max(c.r, max(c.g, c.b))); }
float apart(float3 a, float3 b)
{
	const float3 d = abs(tone(a) - tone(b));
	return max(d.r, max(d.g, d.b));
}

[numthreads(8, 8, 1)]
void mask_cs(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= width || id.y >= height)
		return;
	float r = 0.0, t = 0.0;
	if ((which & 1) != 0)
		r = apart(opaque_in[id.xy].rgb, pre_fx_in[id.xy].rgb);
	if ((which & 2) != 0)
		t = apart(shaded_in[id.xy].rgb, pre_air_in[id.xy].rgb);
	reactive_out[id.xy] = 0.9 * saturate((r - 0.03) / 0.3);
	composition_out[id.xy] = 0.8 * saturate((t - 0.03) / 0.3);
}
)";

		constexpr UINT kSlots = 16;
		constexpr UINT kConstants = 40;
		constexpr UINT kMotionViews = 5; // (a frame's: the depth copy, the blades', the characters', the depth, the motion)
		constexpr UINT kMaskConstants = 4;

		// (a copy's typeless format as its views read it)
		DXGI_FORMAT typed(DXGI_FORMAT f)
		{
			switch (f)
			{
			case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
			default: return f;
			}
		}

		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		ID3D12Resource *texture(ID3D12Device *device, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_STATES state, const wchar_t *name)
		{
			D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC td = {};
			td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			td.Width = w;
			td.Height = h;
			td.DepthOrArraySize = 1;
			td.MipLevels = 1;
			td.Format = format;
			td.SampleDesc.Count = 1;
			td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			ID3D12Resource *t = nullptr;
			if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, state, nullptr, IID_PPV_ARGS(&t))))
				return nullptr;
			t->SetName(name);
			return t;
		}

		void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
		{
			if (from == to)
				return;
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = r;
			b.Transition.Subresource = sub;
			b.Transition.StateBefore = from;
			b.Transition.StateAfter = to;
			list->ResourceBarrier(1, &b);
		}

		// 4x4 row-major products and inverse, in double
		void mul(const double *a, const double *b, double *out)
		{
			double o[16] = {};
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 4; ++j)
					for (int k = 0; k < 4; ++k)
						o[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
			std::memcpy(out, o, sizeof(o));
		}
		bool invert(const double *m, double *out)
		{
			double a[4][8];
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 8; ++j)
					a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
			for (int c = 0; c < 4; ++c)
			{
				int p = c;
				for (int r = c + 1; r < 4; ++r)
					if (std::fabs(a[r][c]) > std::fabs(a[p][c]))
						p = r;
				if (std::fabs(a[p][c]) < 1e-12)
					return false;
				for (int j = 0; j < 8; ++j)
					std::swap(a[c][j], a[p][j]);
				const double d = a[c][c];
				for (int j = 0; j < 8; ++j)
					a[c][j] /= d;
				for (int r = 0; r < 4; ++r)
					if (r != c)
					{
						const double f = a[r][c];
						for (int j = 0; j < 8; ++j)
							a[r][j] -= f * a[c][j];
					}
			}
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 4; ++j)
					out[i * 4 + j] = a[i][j + 4];
			return true;
		}
	}

	context::~context()
	{
		release();
	}

	void context::release()
	{
		if (_fsr != nullptr)
		{
			if (_made)
				ffxFsr2ContextDestroy(static_cast<FfxFsr2Context *>(_fsr));
			delete static_cast<FfxFsr2Context *>(_fsr);
			_fsr = nullptr;
		}
		fsr3::destroy(_fsr3);
		_fsr3 = nullptr;
		std::free(_scratch);
		_scratch = nullptr;
		drop(_motion);
		drop(_root);
		drop(_heap);
		drop(_depth_copy);
		drop(_depth);
		drop(_mv);
		drop(_output);
		drop(_mask);
		drop(_mask_root);
		drop(_mask_heap);
		drop(_reactive);
		drop(_tc);
		_mask_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		_made = false;
		_have_last = false;
		_device = nullptr;
	}

	bool context::make(ID3D12Device *device, uint32_t render_w, uint32_t render_h, uint32_t display_w, uint32_t display_h, bool v3, std::string &why)
	{
		release();
		_device = device;
		_rw = render_w;
		_rh = render_h;
		_dw = display_w;
		_dh = display_h;
		// The motion pass
		using compile_fn = decltype(&D3DCompile);
		HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
		const auto compile = compiler != nullptr ? reinterpret_cast<compile_fn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
		ID3DBlob *cs = nullptr, *errors = nullptr;
		if (compile == nullptr || FAILED(compile(kMotion, std::strlen(kMotion), "hifixi_motion", nullptr, nullptr, "motion_cs", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &errors)))
		{
			why = std::string("the motion pass didn't compile: ") + (errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "no d3dcompiler_47.dll");
			drop(errors);
			release();
			return false;
		}
		drop(errors);
		D3D12_DESCRIPTOR_RANGE ranges[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 3 } };
		D3D12_ROOT_PARAMETER p[2] = {};
		p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		p[0].Constants.Num32BitValues = kConstants;
		p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		p[1].DescriptorTable.NumDescriptorRanges = 2;
		p[1].DescriptorTable.pDescriptorRanges = ranges;
		const D3D12_ROOT_SIGNATURE_DESC rd = { 2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		using serialize_fn = decltype(&D3D12SerializeRootSignature);
		const auto serialize = reinterpret_cast<serialize_fn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
		ID3DBlob *blob = nullptr;
		bool ok = serialize != nullptr && SUCCEEDED(serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) &&
			SUCCEEDED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_root)));
		drop(blob);
		drop(errors);
		if (ok)
		{
			D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
			d.pRootSignature = _root;
			d.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
			ok = SUCCEEDED(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&_motion)));
		}
		drop(cs);
		if (ok)
		{
			const D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * kMotionViews, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
			ok = SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_heap)));
			_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}
		// The depth copy: plane 0 of a D24S8 of A's size, as CopyTextureRegion lays it out
		if (ok)
		{
			D3D12_RESOURCE_DESC dd = {};
			dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			dd.Width = render_w;
			dd.Height = render_h;
			dd.DepthOrArraySize = 1;
			dd.MipLevels = 1;
			dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
			dd.SampleDesc.Count = 1;
			dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
			UINT64 size = 0;
			device->GetCopyableFootprints(&dd, 0, 1, 0, &fp, nullptr, nullptr, &size);
			_pitch_words = fp.Footprint.RowPitch / 4;
			D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = size;
			bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ok = size != 0 && size != UINT64(-1) &&
				SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&_depth_copy)));
			_copy_state = D3D12_RESOURCE_STATE_COMMON; // (buffers start in COMMON, whatever is asked)
		}
		if (ok)
		{
			_depth = texture(device, render_w, render_h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"hifixi: FSR 2's depth");
			_mv = texture(device, render_w, render_h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"hifixi: FSR 2's motion");
			_output = texture(device, display_w, display_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"hifixi: FSR 2's picture");
			ok = _depth != nullptr && _mv != nullptr && _output != nullptr;
			_depth_state = _mv_state = _output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		}
		// The masks' pass and its masks
		if (ok)
		{
			ID3DBlob *mcs = nullptr;
			ok = SUCCEEDED(compile(kMask, std::strlen(kMask), "hifixi_masks", nullptr, nullptr, "mask_cs", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &mcs, &errors));
			drop(errors);
			D3D12_DESCRIPTOR_RANGE mr[2] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0 }, { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 4 } };
			D3D12_ROOT_PARAMETER mp[2] = {};
			mp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			mp[0].Constants.Num32BitValues = kMaskConstants;
			mp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			mp[1].DescriptorTable.NumDescriptorRanges = 2;
			mp[1].DescriptorTable.pDescriptorRanges = mr;
			const D3D12_ROOT_SIGNATURE_DESC mrd = { 2, mp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
			ID3DBlob *mblob = nullptr;
			ok = ok && SUCCEEDED(serialize(&mrd, D3D_ROOT_SIGNATURE_VERSION_1, &mblob, &errors)) &&
				SUCCEEDED(device->CreateRootSignature(0, mblob->GetBufferPointer(), mblob->GetBufferSize(), IID_PPV_ARGS(&_mask_root)));
			drop(mblob);
			drop(errors);
			if (ok)
			{
				D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
				d.pRootSignature = _mask_root;
				d.CS = { mcs->GetBufferPointer(), mcs->GetBufferSize() };
				ok = SUCCEEDED(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&_mask)));
			}
			drop(mcs);
			const D3D12_DESCRIPTOR_HEAP_DESC mh = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * 6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
			ok = ok && SUCCEEDED(device->CreateDescriptorHeap(&mh, IID_PPV_ARGS(&_mask_heap)));
			_reactive = ok ? texture(device, render_w, render_h, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"hifixi: FSR's reactive mask") : nullptr;
			_tc = ok ? texture(device, render_w, render_h, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"hifixi: FSR's transparency mask") : nullptr;
			ok = _reactive != nullptr && _tc != nullptr;
			_mask_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		}
		if (!ok)
		{
			why = "FSR's motion or masks' pass or textures couldn't be made";
			release();
			return false;
		}
		// FSR 3.1's upscaler (its shaders built in: tools/ffx_sc), or FSR 2's context (its shaders compiled here: fsr2_shaders.cpp)
		if (v3)
		{
			_fsr3 = fsr3::make(device, render_w, render_h, display_w, display_h, why);
			if (_fsr3 == nullptr)
			{
				release();
				return false;
			}
			_made = true;
			_frames = 0;
			return true;
		}
		const size_t scratch = ffxFsr2GetScratchMemorySizeDX12();
		_scratch = std::calloc(1, scratch);
		FfxFsr2ContextDescription desc = {};
		if (_scratch == nullptr || ffxFsr2GetInterfaceDX12(&desc.callbacks, device, _scratch, scratch) != FFX_OK)
		{
			why = "FSR 2's DX12 backend couldn't be set up";
			release();
			return false;
		}
		desc.device = ffxGetDeviceDX12(device);
		desc.flags = FFX_FSR2_ENABLE_AUTO_EXPOSURE | FFX_FSR2_ENABLE_DEPTH_INFINITE;
		desc.maxRenderSize = { render_w, render_h };
		desc.displaySize = { display_w, display_h };
		_fsr = new FfxFsr2Context();
		const FfxErrorCode e = ffxFsr2ContextCreate(static_cast<FfxFsr2Context *>(_fsr), &desc);
		if (e != FFX_OK)
		{
			why = "FSR 2's context couldn't be made (error " + std::to_string(int(e)) + ")";
			delete static_cast<FfxFsr2Context *>(_fsr);
			_fsr = nullptr;
			release();
			return false;
		}
		_made = true;
		_frames = 0;
		return true;
	}

	bool context::record(ID3D12GraphicsCommandList *list, ID3D12Resource *color, D3D12_RESOURCE_STATES color_state, ID3D12Resource *depth,
		D3D12_RESOURCE_STATES depth_state, ID3D12Resource *blades, const camera &cam, float sharpen, float dt_ms, bool reset, std::string &why,
		const masks::taken *m, ID3D12Resource *chars)
	{
		if (!_made)
		{
			why = "FSR 2 isn't made";
			return false;
		}
		const D3D12_RESOURCE_DESC cd = color->GetDesc(), dd = depth->GetDesc();
		if (cd.Width != _rw || cd.Height != _rh || dd.Width != _rw || dd.Height != _rh ||
			(dd.Format != DXGI_FORMAT_R24G8_TYPELESS && dd.Format != DXGI_FORMAT_D24_UNORM_S8_UINT))
		{
			why = "the world or its depth isn't as FSR 2 was made for";
			return false;
		}
		// This frame's reprojection: inverse VIEW x the last frame's VIEW x PROJECTION (both without the jitter)
		double view[16], proj[16], view_proj[16], inv_view[16], reproject[16];
		for (int i = 0; i < 16; ++i)
		{
			view[i] = cam.view[i];
			proj[i] = cam.proj[i];
		}
		proj[8] = proj[9] = 0.0; // (the jitter out)
		mul(view, proj, view_proj);
		if (!invert(view, inv_view) || std::fabs(proj[0]) < 1e-6 || std::fabs(proj[5]) < 1e-6)
		{
			why = "the camera's matrices aren't usable";
			return false;
		}
		double last[16];
		for (int i = 0; i < 16; ++i)
			last[i] = _have_last && !reset ? double(_last_view_proj[i]) : view_proj[i];
		mul(inv_view, last, reproject);
		for (int i = 0; i < 16; ++i)
			_last_view_proj[i] = float(view_proj[i]);
		_have_last = true;

		// A's depth (plane 0) into the copy
		transition(list, depth, 0, depth_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		transition(list, _depth_copy, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, _copy_state, D3D12_RESOURCE_STATE_COPY_DEST);
		{
			D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
			dst.pResource = _depth_copy;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			UINT64 size = 0;
			_device->GetCopyableFootprints(&dd, 0, 1, 0, &dst.PlacedFootprint, nullptr, nullptr, &size);
			src.pResource = depth;
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = 0;
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		}
		transition(list, depth, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, depth_state);
		transition(list, _depth_copy, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		_copy_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

		// The motion pass: the depth as floats and the camera's motion
		const UINT slot = _slot++ % kSlots;
		const D3D12_CPU_DESCRIPTOR_HANDLE cpu = { _heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(slot * kMotionViews) * _step };
		if (blades != nullptr)
		{
			const D3D12_RESOURCE_DESC bd = blades->GetDesc();
			if (bd.Width != _rw || bd.Height != _rh)
				blades = nullptr; // (another size: drawn for another world)
		}
		if (chars != nullptr)
		{
			const D3D12_RESOURCE_DESC ch = chars->GetDesc();
			if (ch.Width != _rw || ch.Height != _rh)
				chars = nullptr;
		}
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC s = {};
			s.Format = DXGI_FORMAT_R32_TYPELESS;
			s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			s.Buffer.NumElements = UINT(_depth_copy->GetDesc().Width / 4);
			s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			_device->CreateShaderResourceView(_depth_copy, &s, cpu);
			// (the blades' motion, or a null view of its kind)
			D3D12_SHADER_RESOURCE_VIEW_DESC b = {};
			b.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			b.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			b.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			b.Texture2D.MipLevels = 1;
			_device->CreateShaderResourceView(blades, &b, { cpu.ptr + _step });
			_device->CreateShaderResourceView(chars, &b, { cpu.ptr + 2 * _step }); // (the same format: the characters' motion, or a null view)
			D3D12_UNORDERED_ACCESS_VIEW_DESC u = {};
			u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			u.Format = DXGI_FORMAT_R32_FLOAT;
			_device->CreateUnorderedAccessView(_depth, nullptr, &u, { cpu.ptr + 3 * _step });
			u.Format = DXGI_FORMAT_R16G16_FLOAT;
			_device->CreateUnorderedAccessView(_mv, nullptr, &u, { cpu.ptr + 4 * _step });
		}
		transition(list, _depth, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, _depth_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		transition(list, _mv, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, _mv_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		{
			uint32_t c[kConstants] = {};
			c[0] = _rw;
			c[1] = _rh;
			c[2] = _pitch_words;
			c[3] = blades != nullptr ? 1u : 0u;
			const float f[4] = { 2.0f * cam.jitter[0] / float(_rw), -2.0f * cam.jitter[1] / float(_rh), float(proj[0]), float(proj[5]) };
			std::memcpy(&c[4], f, sizeof(f));
			for (int i = 0; i < 16; ++i)
			{
				const float v = float(reproject[i]);
				std::memcpy(&c[8 + i], &v, 4);
			}
			// (VIEW's first three rows, x y z of each: a world direction into view space)
			for (int r = 0; r < 3; ++r)
				for (int k = 0; k < 4; ++k)
				{
					const float v = k < 3 ? float(view[r * 4 + k]) : 0.0f;
					std::memcpy(&c[24 + r * 4 + k], &v, 4);
				}
			c[36] = chars != nullptr ? 1u : 0u;
			list->SetDescriptorHeaps(1, &_heap);
			list->SetComputeRootSignature(_root);
			list->SetPipelineState(_motion);
			list->SetComputeRoot32BitConstants(0, kConstants, c, 0);
			list->SetComputeRootDescriptorTable(1, { _heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(slot * kMotionViews) * _step });
			list->Dispatch((_rw + 7) / 8, (_rh + 7) / 8, 1);
		}

		// The masks: the game's effects (opaque against pre_fx) and World FX's own see-through things (shaded, or pre_fx
		// when World FX didn't shade, against pre_air), from this frame's copies of A's size
		uint32_t which = 0;
		ID3D12Resource *copies[masks::kinds] = {};
		if (m != nullptr && _mask != nullptr && m->copy[0] != nullptr && m->copy[0]->GetDesc().Width == _rw && m->copy[0]->GetDesc().Height == _rh)
		{
			for (int k = 0; k < masks::kinds; ++k)
				copies[k] = m->copy[k];
			if (m->fresh[masks::opaque] && m->fresh[masks::pre_fx])
				which |= 1u;
			if (m->fresh[masks::pre_air] && (m->fresh[masks::shaded] || m->fresh[masks::pre_fx]))
			{
				if (!m->fresh[masks::shaded])
					copies[masks::shaded] = m->copy[masks::pre_fx];
				which |= 2u;
			}
		}
		if (which != 0)
		{
			const UINT step = _step;
			const D3D12_CPU_DESCRIPTOR_HANDLE mc = { _mask_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(slot * 6) * step };
			D3D12_SHADER_RESOURCE_VIEW_DESC s = {};
			s.Format = typed(m->format);
			s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			s.Texture2D.MipLevels = 1;
			for (int k = 0; k < masks::kinds; ++k)
				_device->CreateShaderResourceView(copies[k], &s, { mc.ptr + SIZE_T(k) * step });
			D3D12_UNORDERED_ACCESS_VIEW_DESC u = {};
			u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			u.Format = DXGI_FORMAT_R8_UNORM;
			_device->CreateUnorderedAccessView(_reactive, nullptr, &u, { mc.ptr + 4 * step });
			_device->CreateUnorderedAccessView(_tc, nullptr, &u, { mc.ptr + 5 * step });
			transition(list, _reactive, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, _mask_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			transition(list, _tc, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, _mask_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			const uint32_t c[kMaskConstants] = { _rw, _rh, which, 0 };
			list->SetDescriptorHeaps(1, &_mask_heap);
			list->SetComputeRootSignature(_mask_root);
			list->SetPipelineState(_mask);
			list->SetComputeRoot32BitConstants(0, kMaskConstants, c, 0);
			list->SetComputeRootDescriptorTable(1, { _mask_heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(slot * 6) * step });
			list->Dispatch((_rw + 7) / 8, (_rh + 7) / 8, 1);
			const D3D12_RESOURCE_STATES mask_read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			transition(list, _reactive, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mask_read);
			transition(list, _tc, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, mask_read);
			_mask_state = mask_read;
			_mask_frames++;
		}
		const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		transition(list, _depth, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
		transition(list, _mv, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
		_depth_state = _mv_state = read;

		// FSR 2: its inputs where it expects them (compute-read), its output an unordered-access target
		transition(list, color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, color_state, read);
		transition(list, _output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, _output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		FfxErrorCode e = FFX_OK;
		std::string fsr3_why;
		if (_fsr3 != nullptr)
		{
			fsr3::frame f;
			f.list = list;
			f.color = color;
			f.depth = _depth;
			f.motion = _mv;
			f.output = _output;
			f.jitter[0] = cam.jitter[0];
			f.jitter[1] = cam.jitter[1];
			f.sharpen = sharpen;
			f.dt_ms = dt_ms;
			f.fov = 2.0f * std::atan(1.0f / float(proj[5]));
			f.reset = reset || _frames == 0;
			f.reactive = which != 0 ? _reactive : nullptr;
			f.composition = which != 0 ? _tc : nullptr;
			if (!fsr3::dispatch(_fsr3, f, fsr3_why))
				e = FFX_ERROR_BACKEND_API_ERROR;
		}
		else
		{
			FfxFsr2Context *const fsr = static_cast<FfxFsr2Context *>(_fsr);
			FfxFsr2DispatchDescription d = {};
			d.commandList = ffxGetCommandListDX12(list);
			d.color = ffxGetResourceDX12(fsr, color, L"hifixi: the world", FFX_RESOURCE_STATE_COMPUTE_READ);
			d.depth = ffxGetResourceDX12(fsr, _depth, L"hifixi: FSR 2's depth", FFX_RESOURCE_STATE_COMPUTE_READ);
			d.motionVectors = ffxGetResourceDX12(fsr, _mv, L"hifixi: FSR 2's motion", FFX_RESOURCE_STATE_COMPUTE_READ);
			d.exposure = ffxGetResourceDX12(fsr, nullptr, L"hifixi: no exposure");
			d.reactive = ffxGetResourceDX12(fsr, which != 0 ? _reactive : nullptr, L"hifixi: FSR's reactive mask", FFX_RESOURCE_STATE_COMPUTE_READ);
			d.transparencyAndComposition = ffxGetResourceDX12(fsr, which != 0 ? _tc : nullptr, L"hifixi: FSR's transparency mask", FFX_RESOURCE_STATE_COMPUTE_READ);
			d.output = ffxGetResourceDX12(fsr, _output, L"hifixi: FSR 2's picture", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
			d.jitterOffset = { cam.jitter[0], cam.jitter[1] };
			d.motionVectorScale = { float(_rw), float(_rh) };
			d.renderSize = { _rw, _rh };
			d.enableSharpening = sharpen > 0.0f;
			d.sharpness = std::clamp(sharpen, 0.0f, 1.0f);
			d.frameTimeDelta = std::clamp(dt_ms, 1.0f, 100.0f);
			d.preExposure = 1.0f;
			d.reset = reset || _frames == 0;
			d.cameraNear = 0.1f;
			d.cameraFar = 65535.0f;
			d.cameraFovAngleVertical = 2.0f * std::atan(1.0f / float(proj[5]));
			d.viewSpaceToMetersFactor = 1.0f;
			e = ffxFsr2ContextDispatch(fsr, &d);
		}
		// (where FSR 2 left them: its inputs compute-read, its output unordered access)
		transition(list, color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, read, color_state);
		transition(list, _output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		_output_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		if (e != FFX_OK)
		{
			why = !fsr3_why.empty() ? fsr3_why : "FSR 2's dispatch failed (error " + std::to_string(int(e)) + ")";
			return false;
		}
		_frames++;
		if (blades != nullptr)
			_blade_frames++;
		if (chars != nullptr)
			_char_frames++;
		return true;
	}
}
