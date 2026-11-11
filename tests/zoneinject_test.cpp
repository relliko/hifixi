/*
 * The native zone renderer on a list recorded as dgVoodoo records its own (src/zone/inject.cpp), on WARP with the debug
 * layer: dgVoodoo's world target and depth (typeless resources, UNORM and D24S8 views), its own root signature,
 * descriptor heap, pipeline and vertex buffer bound once; the depth plane in DEPTH_READ and the stencil plane moved to
 * DEPTH_WRITE for the zone's end marker, as dgVoodoo leaves them; the pass (Southern San d'Oria's street, as the game
 * side publishes it) drawn at the marker; then dgVoodoo's next draw (a "character") with nothing bound again, as it
 * binds lazily, and the planes moved on as dgVoodoo moves them.
 *   - the zone is in the target and its depth (most of the picture, nearer than the far plane);
 *   - the character is drawn as dgVoodoo meant: its texture through its heap and table, its vertices through its
 *     buffer, its pipeline (each would be wrong had our state been left bound);
 *   - the debug layer says nothing (the planes' states, the heaps, the views);
 *   - a pass not HiFi XI's, an old seq, a depth not yet followed: nothing drawn (and said why when it should have been);
 *   - the model actors at a characters' end marker (on_actors_end): drawn once for their walk's seq, a walk with none
 *     not at all, the debug layer saying nothing of it;
 *   - the light split (src/zone/light_split.hpp) drawn with it once World FX has asked: a word on most of the zone's
 *     pixels, its depth the depth buffer's, the ground facing up, lit mostly by the sun, the walls turned from it not;
 *     under the character dgVoodoo drew afterwards, its depth not the depth buffer's (World FX leaves those words);
 *   - a lamp in the frame that no object names (the game's per-object lamps): it lights nothing, per pixel or per vertex
 *     (the game's own way: every lamp in reach, draw.cpp near_lamp_light, lit a Mog House far too bright, so it's off);
 *     named by every object, per vertex it lights them;
 *   - World FX's lamp glow added by the pass itself from a published block (src/render/sun_shadows.hpp's lamps, [zone]
 *     nativelamps): per pixel with the option on the street under the lamp is lit and its words marked; the option off,
 *     per vertex or a stale block: the picture without any block, byte for byte, nothing marked; a character's capsule
 *     and the lamp's cube take a zone's lamp's light;
 *   - the texture pack's FSR bias set as main.cpp sets it (inject::set_texture_bias): the same picture over the game's
 *     one-level textures; over a copy whose textures have a red second level, less red at -1 than at 0;
 *   - the sun's shadows from a made-up zone: the moving layer (a swaying card's shadow moves with its sway, an open door's
 *     is where the pass drew it), the near cascade leaving those to it while the far one keeps them at rest, and the
 *     second map's (an interior's) placements cast with the zone's.
 *
 *   build\zoneinject_test.exe [out.bmp]       (ZONE_PERPIXEL=1: drawn with the per-pixel option)
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include "../src/bake/zonemesh.hpp"
#include "../src/zone/draw.hpp"
#include "../src/zone/frame.hpp"
#include "../src/zone/inject.hpp"
#include "../src/zone/light_split.hpp"
#include "../src/zone/zone_gpu.hpp"
#include "../src/zone/zoneshadow.hpp"
#include "../src/render/sun_shadows.hpp"
#include "game_dump.hpp"
#include "../src/host/shadercache.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	namespace zm = hifi::bake::zonemesh;

	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	// (ZONE_SIZE=3840x2160: a player's background resolution, for the timed passes below; the checks are 800x450's)
	UINT W = 800, H = 450;

	// dgVoodoo's stand-in: a textured quad from a vertex buffer, through a root table
	const char *kGame = R"(
Texture2D t : register(t0);
SamplerState s : register(s0);
struct v { float2 p : POSITION; float2 uv : TEXCOORD; };
struct o { float4 p : SV_Position; float2 uv : TEXCOORD; };
o vs(v i) { o r; r.p = float4(i.p, 0.0, 1.0); r.uv = i.uv; return r; }
float4 ps(o i) : SV_Target { return t.Sample(s, i.uv); }
)";

	ID3DBlob *compile(const char *src, const char *entry, const char *target)
	{
		using compile_fn = decltype(&D3DCompile);
		static const auto c = reinterpret_cast<compile_fn>(GetProcAddress(LoadLibraryW(L"d3dcompiler_47.dll"), "D3DCompile"));
		ID3DBlob *code = nullptr, *errors = nullptr;
		if (c == nullptr || FAILED(c(src, std::strlen(src), "game", nullptr, nullptr, entry, target, 0, 0, &code, &errors)))
			return nullptr;
		return code;
	}

	ID3D12Resource *g_marked = nullptr; // (the depth the marker named: the only one followed, as the renderer's)
	// The depth's planes as the test moves them (a new depth: both as the first barrier found them), handed to the
	// renderer as our d3d8.dll hands them at each mark
	void follow_planes(ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		static ID3D12Resource *of = nullptr;
		static D3D12_RESOURCE_STATES planes[2] = {};
		if (r != of)
		{
			of = r;
			planes[0] = planes[1] = from;
		}
		if (sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
			planes[0] = planes[1] = to;
		else if (sub < 2)
			planes[sub] = to;
		hifi::zone::inject::set_depth_planes(r, planes);
	}

	void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Transition.pResource = r;
		b.Transition.Subresource = sub;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
		// (the marked depth's planes handed over as our d3d8.dll hands them at each mark)
		if (r == g_marked)
			follow_planes(r, sub, from, to);
	}

	struct mat4
	{
		float m[16];
	};
	mat4 mul(const mat4 &a, const mat4 &b)
	{
		mat4 o = {};
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				for (int k = 0; k < 4; ++k)
					o.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
		return o;
	}
	mat4 camera(const float eye[3], float yaw, float tilt)
	{
		const float at[3] = { eye[0] + std::sin(yaw) * std::cos(tilt), eye[1] + std::sin(tilt), eye[2] + std::cos(yaw) * std::cos(tilt) };
		float z[3] = { eye[0] - at[0], eye[1] - at[1], eye[2] - at[2] }, up[3] = { 0, -1, 0 }, x[3], y[3];
		auto norm = [](float *v) {
			const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			for (int k = 0; k < 3; ++k)
				v[k] /= l;
		};
		auto cross = [](const float *a, const float *b, float *o) {
			o[0] = a[1] * b[2] - a[2] * b[1];
			o[1] = a[2] * b[0] - a[0] * b[2];
			o[2] = a[0] * b[1] - a[1] * b[0];
		};
		auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
		norm(z);
		cross(up, z, x);
		norm(x);
		cross(z, x, y);
		const mat4 v = { { x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1 } };
		const float ys = 1.0f / std::tan(0.5f), xs = ys / (float(W) / H), zn = 0.5f, zf = 2000.0f;
		const mat4 p = { { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zn - zf), -1, 0, 0, zn * zf / (zn - zf), 0 } };
		return mul(v, p);
	}

	uint32_t debug_messages(ID3D12InfoQueue *q, std::string &first)
	{
		if (q == nullptr)
			return 0;
		const UINT64 n = q->GetNumStoredMessages();
		uint32_t errors = 0;
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T size = 0;
			q->GetMessage(i, nullptr, &size);
			std::vector<uint8_t> buf(size);
			D3D12_MESSAGE *m = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
			q->GetMessage(i, m, &size);
			if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && m->ID != D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE &&
				m->ID != D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE)
			{
				if (errors++ == 0)
					first = m->pDescription;
			}
		}
		q->ClearStoredMessages();
		return errors;
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0); // (each line out as it's printed: a crash shows where it was)
	namespace inj = hifi::zone::inject;
	// (the zone's shaders through the shader cache, as in game: the first run compiles them and writes them down, a run
	// after reads them back; the pictures below are checked either way)
	hifi::shadercache::set_folder(L"build\\shadercache_test\\");
	char size_text[32] = {};
	const bool big = GetEnvironmentVariableA("ZONE_SIZE", size_text, sizeof(size_text)) != 0 && std::sscanf(size_text, "%ux%u", &W, &H) == 2 && W >= 64 && H >= 64;
	if (big)
		std::printf("ZONE_SIZE: the zone pass timed at %ux%u (the checks after the first picture are left out)\n", W, H);
	uintptr_t base = 0;
	if (dump::map(dump::kDumps[0], base) == 0)
	{
		check(false, "the FFXiMain dump mapped");
		return 1;
	}
	zm::zone z;
	hifi::bake::result r;
	check(zm::load(230, z, r), "Southern San d'Oria read");

	ID3D12Debug *debug = nullptr;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
	{
		debug->EnableDebugLayer();
		// (HIFIXI_GBV=1: GPU-based validation as well, as the add-on's debug layer takes it: what the shaders read, the
		// states resources are really in)
		ID3D12Debug1 *debug1 = nullptr;
		if (GetEnvironmentVariableA("HIFIXI_GBV", nullptr, 0) != 0 && SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))))
		{
			debug1->SetEnableGPUBasedValidation(TRUE);
			debug1->Release();
			std::printf("      (GPU-based validation on)\n");
		}
	}
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	ID3D12Device *device = nullptr;
	// (ZONE_GPU=1: the first GPU in place of WARP, for what WARP doesn't do: the breadcrumbs' writes)
	const bool on_gpu = GetEnvironmentVariableA("ZONE_GPU", nullptr, 0) != 0;
	if (FAILED(D3D12CreateDevice(on_gpu ? nullptr : warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
	{
		check(false, "a WARP device");
		return 1;
	}
	ID3D12InfoQueue *info = nullptr;
	device->QueryInterface(IID_PPV_ARGS(&info));
	ID3D12CommandQueue *direct = nullptr, *copy = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&direct));
	qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&copy));
	hifi::zone::gpu_zone gz, gz_red;
	std::string why;
	// (a copy whose colour maps have a second level, solid red, as the texture pack's have more than one: the FSR bias
	// below draws less of it at -1, through inject::set_texture_bias as main.cpp sets it)
	zm::zone z_red = z;
	uint32_t red_levels = 0;
	for (zm::texture &t : z_red.textures)
		if (t.w % 8 == 0 && t.h % 8 == 0 && t.w >= 8 && t.h >= 8)
		{
			const uint32_t w = t.w / 2, h = t.h / 2;
			std::vector<uint8_t> level;
			if (t.format == zm::kBC2)
			{
				// (alpha 15 throughout, both colours 565 red, every index 0)
				const uint8_t block[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0 };
				for (uint32_t b = 0; b < (w / 4) * (h / 4); ++b)
					level.insert(level.end(), block, block + 16);
			}
			else
			{
				const uint8_t px[4] = { 0, 0, 255, 255 }; // (B G R A)
				for (uint32_t p = 0; p < w * h; ++p)
					level.insert(level.end(), px, px + 4);
			}
			t.mips = { level };
			t.levels = 2;
			red_levels++;
		}
	check(gz.upload(device, copy, std::move(z), why) && gz.wait(10000), "on the GPU (its copy queue)" + why);
	check(gz_red.upload(device, copy, std::move(z_red), why) && gz_red.wait(10000) && red_levels > 0, "  and a copy with a red second level in " +
		std::to_string(red_levels) + " of its textures" + why);
	check(inj::prepare(device, why), "the renderer made" + (why.empty() ? "" : ": " + why));
	inj::set_options(GetEnvironmentVariableA("ZONE_PERPIXEL", nullptr, 0) != 0); // (the per-pixel option: ZONE_PERPIXEL=1)

	// dgVoodoo's world: typeless resources, UNORM / D24S8 views
	D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, up = { D3D12_HEAP_TYPE_UPLOAD }, rb = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = W;
	td.Height = H;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.SampleDesc.Count = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	ID3D12Resource *A = nullptr, *Az = nullptr;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&A));
	td.Format = DXGI_FORMAT_R24G8_TYPELESS;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&Az));
	ID3D12DescriptorHeap *rtvh = nullptr, *dsvh = nullptr, *srvh = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh));
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh));
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvh));
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvh->GetCPUDescriptorHandleForHeapStart(), dsv = dsvh->GetCPUDescriptorHandleForHeapStart();
	D3D12_RENDER_TARGET_VIEW_DESC rvd = {};
	rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(A, &rvd, rtv);
	D3D12_DEPTH_STENCIL_VIEW_DESC dvd = {};
	dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dvd.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL; // (dgVoodoo's draws keep the stencil plane in DEPTH_READ)
	device->CreateDepthStencilView(Az, &dvd, dsv);
	ID3D12DescriptorHeap *dsvh2 = nullptr;
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh2));
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv_clear = dsvh2->GetCPUDescriptorHandleForHeapStart();
	dvd.Flags = D3D12_DSV_FLAG_NONE; // (its clears' view)
	device->CreateDepthStencilView(Az, &dvd, dsv_clear);

	// dgVoodoo's texture (a 2x2 of known colours, in its heap's third slot), vertex buffer, root signature, pipeline
	ID3D12Resource *tex = nullptr, *tex_up = nullptr, *vb = nullptr;
	td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = 2;
	td.Height = 2;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.SampleDesc.Count = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = 1024;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tex_up));
	device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vb));
	{
		uint8_t *p = nullptr;
		tex_up->Map(0, nullptr, reinterpret_cast<void **>(&p));
		const uint32_t px = 0xFF2080E0u; // (A B G R: an orange-ish 224 128 32)
		for (int row = 0; row < 2; ++row)
			for (int x = 0; x < 2; ++x)
				std::memcpy(p + row * 256 + x * 4, &px, 4);
		tex_up->Unmap(0, nullptr);
		float *v = nullptr;
		vb->Map(0, nullptr, reinterpret_cast<void **>(&v));
		// (a quad in the picture's lower right: x 0.5..0.9, y -0.9..-0.5)
		const float quad[] = { 0.5f, -0.5f, 0, 0, 0.9f, -0.5f, 1, 0, 0.5f, -0.9f, 0, 1, 0.9f, -0.9f, 1, 1 };
		std::memcpy(v, quad, sizeof(quad));
		vb->Unmap(0, nullptr);
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
	sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sv.Texture2D.MipLevels = 1;
	const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	D3D12_CPU_DESCRIPTOR_HANDLE srv = srvh->GetCPUDescriptorHandleForHeapStart();
	srv.ptr += 2 * step;
	device->CreateShaderResourceView(tex, &sv, srv);
	D3D12_GPU_DESCRIPTOR_HANDLE table = srvh->GetGPUDescriptorHandleForHeapStart();
	table.ptr += 2 * step;
	D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0 };
	D3D12_ROOT_PARAMETER rp[2] = {};
	rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; // (a constant first, so a table set at 1 is ours to put back)
	rp[0].Constants.Num32BitValues = 1;
	rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rp[1].DescriptorTable.NumDescriptorRanges = 1;
	rp[1].DescriptorTable.pDescriptorRanges = &range;
	D3D12_STATIC_SAMPLER_DESC ss = {};
	ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
	ID3DBlob *blob = nullptr, *err = nullptr;
	ID3D12RootSignature *game_root = nullptr;
	D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
	device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&game_root));
	ID3DBlob *gvs = compile(kGame, "vs", "vs_5_0"), *gps = compile(kGame, "ps", "ps_5_0");
	const D3D12_INPUT_ELEMENT_DESC il[] = { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
	pd.pRootSignature = game_root;
	pd.VS = { gvs->GetBufferPointer(), gvs->GetBufferSize() };
	pd.PS = { gps->GetBufferPointer(), gps->GetBufferSize() };
	pd.InputLayout = { il, 2 };
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 1;
	pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	pd.SampleDesc.Count = 1;
	pd.SampleMask = UINT_MAX;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.DepthStencilState.DepthEnable = TRUE;
	pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	ID3D12PipelineState *game_pso = nullptr;
	check(gvs != nullptr && gps != nullptr && SUCCEEDED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&game_pso))), "dgVoodoo's stand-in pipeline");

	// The pass, as the game side publishes it: the street, every placement, a day's light
	const zm::zone &d = gz.data();
	const float eye[3] = { -27.1f, -4.5f, 35.1f };
	const mat4 vp = camera(eye, 1.5708f, 0.12f);
	hifi::zone::gpu_zone *fill_zone = &gz; // (the red-levelled copy for the FSR bias's passes: the same placements)
	auto fill = [&](uint32_t seq, bool native, const hifi::zone::frame_lamp *lamp = nullptr, bool named = false) {
		hifi::zone::frame &f = hifi::zone::frame_slot(seq);
		f.native = native;
		f.zone = fill_zone;
		f.map = 230;
		std::memcpy(f.view_proj, vp.m, 64);
		std::memcpy(f.camera, eye, 12);
		const float sd[3] = { 0.36f, -0.83f, 0.42f }, sun[3] = { 1.26f, 1.25f, 1.1f }, amb[3] = { 0.49f, 0.47f, 0.49f }, fog[3] = { 0.55f, 0.65f, 0.78f };
		std::memcpy(f.sun_dir, sd, 12);
		std::memcpy(f.sun, sun, 12);
		std::memcpy(f.sun_bump, sun, 12);
		std::memcpy(f.ambient, amb, 12);
		std::memcpy(f.ambient_bump, amb, 12);
		std::memcpy(f.fog, fog, 12);
		f.fog_start = 150.0f;
		f.fog_end = 600.0f;
		f.fog_scale = 1.0f;
		f.count = 0;
		for (const zm::placement &p : d.placements)
		{
			hifi::zone::frame_object &o = f.objects[f.count++];
			for (int k = 0; k < 3; ++k)
			{
				o.world[k * 4 + 0] = p.m[k];
				o.world[k * 4 + 1] = p.m[3 + k];
				o.world[k * 4 + 2] = p.m[6 + k];
				o.world[k * 4 + 3] = p.t[k];
			}
			o.model = p.model;
			o.flags = p.flags;
			for (uint32_t &l : o.lamps)
				l = 0xFFFFFFFFu;
			if (named)
				o.lamps[0] = 0;
		}
		f.lamp_count = lamp != nullptr ? 1 : 0;
		if (lamp != nullptr)
			f.lamps[0] = *lamp;
		hifi::zone::publish(f, seq);
	};

	// The list, as dgVoodoo records a frame round the zone's end marker
	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *list = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
	{
		// (dgVoodoo's texture filled first)
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = tex;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.pResource = tex_up;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 256 };
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		barrier(list, tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	inj::on_marker(Az); // (the zone's begin marker named the depth)
	g_marked = Az;
	const float sky[4] = { 0.55f, 0.65f, 0.78f, 1.0f };
	list->ClearRenderTargetView(rtv, sky, 0, nullptr);
	list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	// (both planes start in DEPTH_WRITE: dgVoodoo's frame start moves the stencil plane to DEPTH_READ)
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
	// dgVoodoo's state bound once: targets, viewport, heaps, root signature and table, pipeline, vertices
	list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	const D3D12_VIEWPORT vpt = { 0.5f, 0.5f, float(W), float(H), 0, 1 };
	const D3D12_RECT sc = { 0, 0, LONG(W), LONG(H) };
	list->RSSetViewports(1, &vpt);
	list->RSSetScissorRects(1, &sc);
	list->SetDescriptorHeaps(1, &srvh);
	list->SetGraphicsRootSignature(game_root);
	list->SetGraphicsRoot32BitConstant(0, 7, 0);
	list->SetGraphicsRootDescriptorTable(1, table);
	list->SetPipelineState(game_pso);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGPUVirtualAddress(), 64, 16 };
	list->IASetVertexBuffers(0, 1, &vbv);
	// What the game has bound now (our d3d8.dll binds it all again after a mark: the test does that with it)
	grass::list_state bound;
	bound.rtvs[0] = rtv;
	bound.rtv_count = 1;
	bound.dsv = dsv;
	bound.pipeline = game_pso;
	bound.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
	bound.viewports[0] = vpt;
	bound.viewport_count = 1;
	bound.scissors[0] = sc;
	bound.scissor_count = 1;
	bound.set_root(game_root);
	bound.arg(1).kind = grass::root_arg::table;
	bound.arg(1).value = table.ptr;
	const uint32_t seven = 7;
	bound.set_constants(0, 0, 1, &seven);

	// The planes as dgVoodoo leaves them for a marker after a blended draw: depth read, stencil written
	barrier(list, Az, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	inj::world w;
	w.rt = A;
	w.ds = Az;
	w.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	w.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	fill(5, true);
	float zlo[3] = {}, zhi[3] = {};
	uint64_t zgen = 0;
	const bool bounds_before = inj::zone_bounds(zlo, zhi, zgen);
	hifi::light::take(); // (World FX asking for the light split: from now on the native passes draw it)
	inj::on_zone_end(list, w, 5);
	// Passes it mustn't draw: the same seq again, a pass not HiFi XI's
	inj::on_zone_end(list, w, 5);
	fill(6, false);
	inj::on_zone_end(list, w, 6);
	// A characters' end marker with the walk's model actors (one of the zone's models, as a model DAT's would be; high
	// above the street, out of the picture the checks below read), the same seq again, and a walk with none
	{
		hifi::zone::actor_frame &af = hifi::zone::actor_slot(3);
		const hifi::zone::frame *zf = hifi::zone::published(5);
		std::memcpy(af.view_proj, vp.m, 64);
		std::memcpy(af.camera, eye, 12);
		af.env = hifi::zone::frame_env();
		if (zf != nullptr)
		{
			std::memcpy(af.env.sun_dir, zf->sun_dir, 12);
			std::memcpy(af.env.sun, zf->sun, 12);
			std::memcpy(af.env.ambient, zf->ambient, 12);
		}
		af.count = 1;
		af.objects[0] = zf != nullptr && zf->count != 0 ? zf->objects[0] : hifi::zone::frame_object();
		af.objects[0].world[7] += 5000.0f;
		af.objects[0].flags = hifi::zone::kDoor;
		af.objects[0].zone = 0;
		af.data_count = 1;
		af.data[0] = &gz;
		af.lamp_count = 0;
		hifi::zone::actor_publish(af, 3);
		hifi::zone::actor_slot(4).count = 0;
		hifi::zone::actor_publish(hifi::zone::actor_slot(4), 4);
		// (the planes as the characters' end marker finds them: as dgVoodoo left them for this one)
		inj::on_actors_end(list, w, 3);
		inj::on_actors_end(list, w, 3);
		inj::on_actors_end(list, w, 4);
		const std::vector<std::string> st = inj::status();
		const std::string &line = st.back();
		check(line.find("model actors") != std::string::npos && line.find("1 passes drawn, 0 couldn't be") != std::string::npos && line.find("the last: 1 objects") != std::string::npos &&
			inj::actors_ready(), "the model actors drawn at the characters' end once, from the zone renderer: " + line);
	}
	// World FX's sun shadow cascade from the zone just drawn (record_zone_casters): the sun overhead, a D32 map over the
	// whole of what the zone places (x across, z down, depth by height), then the game's state back as World FX does
	const bool bounds_after = inj::zone_bounds(zlo, zhi, zgen);
	constexpr UINT kMap = 2048;
	const float m_reach = 0.5f * std::max(zhi[0] - zlo[0], zhi[2] - zlo[2]) + 1.0f, m_x = 0.5f * (zlo[0] + zhi[0]), m_z = 0.5f * (zlo[2] + zhi[2]);
	const float m_top = zlo[1] - 1.0f, m_span = (zhi[1] - zlo[1]) + 2.0f;
	const float sun_m[16] = { 1.0f / m_reach, 0, 0, 0, 0, 0, 1.0f / m_span, 0, 0, -1.0f / m_reach, 0, 0, -m_x / m_reach, m_z / m_reach, -m_top / m_span, 1 };
	ID3D12Resource *sun_map = nullptr, *sun_back = nullptr;
	ID3D12DescriptorHeap *sun_heap = nullptr;
	{
		D3D12_RESOURCE_DESC cd = {};
		cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		cd.Width = kMap;
		cd.Height = kMap;
		cd.DepthOrArraySize = 1;
		cd.MipLevels = 1;
		cd.Format = DXGI_FORMAT_D32_FLOAT;
		cd.SampleDesc.Count = 1;
		cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		D3D12_CLEAR_VALUE cv = {};
		cv.Format = DXGI_FORMAT_D32_FLOAT;
		cv.DepthStencil.Depth = 1.0f;
		D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT }, rh = { D3D12_HEAP_TYPE_READBACK };
		device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&sun_map));
		D3D12_DESCRIPTOR_HEAP_DESC sd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
		device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&sun_heap));
		device->CreateDepthStencilView(sun_map, nullptr, sun_heap->GetCPUDescriptorHandleForHeapStart());
		D3D12_RESOURCE_DESC b = {};
		b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		b.Width = UINT64(kMap) * kMap * 4;
		b.Height = 1;
		b.DepthOrArraySize = 1;
		b.MipLevels = 1;
		b.SampleDesc.Count = 1;
		b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&sun_back));
	}
	const D3D12_CPU_DESCRIPTOR_HANDLE sun_dsv = sun_heap->GetCPUDescriptorHandleForHeapStart();
	list->ClearDepthStencilView(sun_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	const uint32_t sun_triangles = bounds_after ? inj::record_zone_casters(list, sun_m, sun_dsv, kMap) : 0;
	list->SetDescriptorHeaps(1, &srvh); // (as our d3d8.dll does after a mark: its heaps, then its state)
	bound.restore(list);
	barrier(list, sun_map, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	{
		D3D12_TEXTURE_COPY_LOCATION sdst = {}, ssrc = {};
		sdst.pResource = sun_back;
		sdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		sdst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_TYPELESS, kMap, kMap, 1, kMap * 4 };
		ssrc.pResource = sun_map;
		ssrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		list->CopyTextureRegion(&sdst, 0, 0, 0, &ssrc, nullptr);
	}
	// The ground's colour map for the grass (floor_hook: docs/own-frame/grass-colour.md) over the whole zone from above,
	// as World FX draws it (x across, z down from its far edge, the lowest surface by height), out to be read
	constexpr UINT kGround = 512;
	ID3D12Resource *ground_map = nullptr, *ground_depth = nullptr, *ground_back = nullptr;
	ID3D12DescriptorHeap *ground_rtv_heap = nullptr, *ground_dsv_heap = nullptr;
	uint32_t ground_triangles = 0;
	{
		D3D12_RESOURCE_DESC cd = {};
		cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		cd.Width = cd.Height = kGround;
		cd.DepthOrArraySize = 1;
		cd.MipLevels = 1;
		cd.SampleDesc.Count = 1;
		cd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		D3D12_CLEAR_VALUE cv = {};
		cv.Format = cd.Format;
		cv.Color[3] = -65504.0f;
		D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT }, rh = { D3D12_HEAP_TYPE_READBACK };
		device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&ground_map));
		cd.Format = DXGI_FORMAT_D32_FLOAT;
		cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		D3D12_CLEAR_VALUE dv = {};
		dv.Format = cd.Format;
		device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv, IID_PPV_ARGS(&ground_depth));
		D3D12_DESCRIPTOR_HEAP_DESC rd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 }, dd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
		device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&ground_rtv_heap));
		device->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&ground_dsv_heap));
		device->CreateRenderTargetView(ground_map, nullptr, ground_rtv_heap->GetCPUDescriptorHandleForHeapStart());
		device->CreateDepthStencilView(ground_depth, nullptr, ground_dsv_heap->GetCPUDescriptorHandleForHeapStart());
		D3D12_RESOURCE_DESC b = {};
		b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		b.Width = UINT64(kGround) * kGround * 8;
		b.Height = 1;
		b.DepthOrArraySize = 1;
		b.MipLevels = 1;
		b.SampleDesc.Count = 1;
		b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ground_back));
		const float none[4] = { 0.0f, 0.0f, 0.0f, -65504.0f };
		const D3D12_CPU_DESCRIPTOR_HANDLE grtv = ground_rtv_heap->GetCPUDescriptorHandleForHeapStart(), gdsv = ground_dsv_heap->GetCPUDescriptorHandleForHeapStart();
		list->ClearRenderTargetView(grtv, none, 0, nullptr);
		list->ClearDepthStencilView(gdsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
		float gm[16] = {};
		gm[0] = 1.0f / m_reach;
		gm[9] = 1.0f / m_reach;
		gm[6] = 1.0f / m_span;
		gm[12] = -m_x / m_reach;
		gm[13] = -m_z / m_reach;
		gm[14] = -m_top / m_span;
		gm[15] = 1.0f;
		ground_triangles = bounds_after ? inj::floor_hook()(nullptr, list, gm, grtv, gdsv, kGround) : 0;
		list->SetDescriptorHeaps(1, &srvh); // (as our d3d8.dll does after a mark: its heaps, then its state)
		bound.restore(list);
		barrier(list, ground_map, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_TEXTURE_COPY_LOCATION gdst = {}, gsrc = {};
		gdst.pResource = ground_back;
		gdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		gdst.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, kGround, kGround, 1, kGround * 8 };
		gsrc.pResource = ground_map;
		gsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		list->CopyTextureRegion(&gdst, 0, 0, 0, &gsrc, nullptr);
	}
	// The light split drawn with the pass, as World FX takes it, out to be read
	ID3D12Resource *const light = hifi::light::take(), *light_back = nullptr;
	if (light != nullptr)
	{
		bd.Width = UINT64(W) * 4 * H;
		device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&light_back));
		barrier(list, light, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, hifi::light::kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_TEXTURE_COPY_LOCATION ldst = {}, lsrc = {};
		ldst.pResource = light_back;
		ldst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		ldst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, W, H, 1, W * 4 };
		lsrc.pResource = light;
		lsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		list->CopyTextureRegion(&ldst, 0, 0, 0, &lsrc, nullptr);
		barrier(list, light, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, hifi::light::kRead);
	}
	// dgVoodoo's next draw, nothing bound again (the planes moved on as it moves them)
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
	barrier(list, Az, 0, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	list->DrawInstanced(4, 1, 0, 0);
	// The picture and depth out
	barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ID3D12Resource *readback = nullptr, *depth_back = nullptr;
	bd.Width = UINT64(W) * 4 * H;
	device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
	device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&depth_back));
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	dst.pResource = readback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, W, H, 1, W * 4 };
	src.pResource = A;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	dst.pResource = depth_back;
	dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_TYPELESS, W, H, 1, W * 4 };
	src.pResource = Az;
	src.SubresourceIndex = 0;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	list->Close();
	ID3D12CommandList *lists[] = { list };
	direct->ExecuteCommandLists(1, lists);
	ID3D12Fence *fence = nullptr;
	device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	direct->Signal(fence, 1);
	HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	fence->SetEventOnCompletion(1, e);
	WaitForSingleObject(e, 60000);
	check(device->GetDeviceRemovedReason() == S_OK, "the device still there");
	std::string first;
	const uint32_t messages = debug_messages(info, first);
	check(messages == 0, "the debug layer: nothing (" + std::to_string(messages) + (messages != 0 ? ": " + first : std::string()) + ")");

	// ZONE_SIZE (best with ZONE_GPU=1): the zone pass at that size, per pixel with the light split, under a load of lamps
	// as a town at night has (64, four of them named by every object, a ring of them round the street), 20 times, each
	// timed on its own; the device must stay and no pass come near the 2 s Windows gives the GPU. Then out: the checks
	// after this read 800x450's pixels. Not part of a normal run.
	if (big)
	{
		double worst = 0.0, sum = 0.0;
		int drawn = 0;
		bool alive = true;
		// (ZONE_NATIVE_LAMPS=1: with World FX's lamp glow added by the pass as well, [zone] nativelamps: a block published
		// before each pass as World FX publishes it, 16 zone lamps of reach 12 within 10 yalms of the camera, two yalms
		// under it, lamp shadows at 0.5, four of them with a cube (nothing in the cubes: every tap read, none hides), four
		// characters' capsules on the street: the most the glow costs. Against the same run without it.)
		const bool native_lamps = GetEnvironmentVariableA("ZONE_NATIVE_LAMPS", nullptr, 0) != 0;
		ID3D12Resource *bench_cubes = nullptr;
		if (native_lamps)
		{
			D3D12_HEAP_PROPERTIES up_heap = {};
			up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			const UINT64 cube_bytes = UINT64(256) * 256 * 4 * 6 * hifi::sun::kMaxLampCubes;
			bd.Width = cube_bytes;
			device->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&bench_cubes));
			float *texels = nullptr;
			bench_cubes->Map(0, nullptr, reinterpret_cast<void **>(&texels));
			for (size_t i = 0; i < size_t(cube_bytes / 4); ++i)
				texels[i] = 1.0f;
			bench_cubes->Unmap(0, nullptr);
		}
		for (uint32_t pass = 0; pass < 20 && alive; ++pass)
		{
			const uint32_t seq = 40 + pass;
			fill(seq, true);
			if (native_lamps)
			{
				hifi::sun::block b;
				b.device = device;
				b.buffer = bench_cubes;
				hifi::sun::gpu_block &g = b.gpu;
				g.mode[2] = hifi::sun::kHasLamps;
				g.lamp_head[0] = float(hifi::sun::kMaxLamps);
				g.lamp_head[1] = 0.5f;
				g.lamp_head[2] = float(hifi::sun::kMaxBodies);
				for (uint32_t i = 0; i < hifi::sun::kMaxLamps; ++i)
				{
					const float a = 0.7f * float(i), r = 3.0f + float(i % 8);
					const float at[4] = { eye[0] + r * std::cos(a), eye[1] + 2.0f, eye[2] + r * std::sin(a), 12.0f };
					const float colour[4] = { 0.73f, 0.5f, 0.11f, i < hifi::sun::kMaxLampCubes ? 2.0f + float(i) : 1.0f };
					std::memcpy(g.lamp_pos[i], at, 16);
					std::memcpy(g.lamp_col[i], colour, 16);
				}
				g.lamp_where[1] = 256;
				g.lamp_map[0] = 1.0f;
				g.lamp_map[1] = 0.95f;
				g.lamp_map[2] = 1.0f;
				for (uint32_t j = 0; j < hifi::sun::kMaxBodies; ++j)
				{
					const float feet[4] = { eye[0] + 4.0f * std::cos(1.5f * float(j)), eye[1] + 4.5f, eye[2] + 4.0f * std::sin(1.5f * float(j)), 1.0f };
					std::memcpy(g.body[j], feet, 16);
				}
				hifi::sun::publish(b);
			}
			hifi::zone::frame &f = hifi::zone::frame_slot(seq);
			f.lamp_count = hifi::zone::kMaxLamps;
			for (uint32_t i = 0; i < f.lamp_count; ++i)
			{
				hifi::zone::frame_lamp &l = f.lamps[i];
				const float a = 0.7f * float(i), r = 3.0f + 4.0f * float(i % 8);
				l.pos[0] = eye[0] + r * std::cos(a);
				l.pos[1] = eye[1] + 1.5f - float(i % 3);
				l.pos[2] = eye[2] + r * std::sin(a);
				l.range = 6.0f + 2.0f * float(i % 4);
				l.colour[0] = 1.0f;
				l.colour[1] = 0.8f;
				l.colour[2] = 0.5f;
				l.attenuation2 = 0.5f;
			}
			for (uint32_t o = 0; o < f.count; ++o)
				for (uint32_t k = 0; k < 4; ++k)
					f.objects[o].lamps[k] = (o * 4 + k) % hifi::zone::kMaxLamps;
			hifi::zone::publish(f, seq); // (frame_slot took it back to fill: published again)
			hifi::light::take(); // (the light split wanted, as World FX asks each frame)
			alloc->Reset();
			list->Reset(alloc, nullptr);
			list->SetDescriptorHeaps(1, &srvh);
			barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			inj::set_options(true);
			list->ClearRenderTargetView(rtv, sky, 0, nullptr);
			list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			inj::on_zone_end(list, w, seq);
			barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			list->Close();
			LARGE_INTEGER t0, t1, fq;
			QueryPerformanceFrequency(&fq);
			QueryPerformanceCounter(&t0);
			direct->ExecuteCommandLists(1, lists);
			const UINT64 v = fence->GetCompletedValue() + 1;
			direct->Signal(fence, v);
			fence->SetEventOnCompletion(v, e);
			const bool done = WaitForSingleObject(e, 60000) == WAIT_OBJECT_0;
			QueryPerformanceCounter(&t1);
			const double ms = 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(fq.QuadPart);
			alive = done && device->GetDeviceRemovedReason() == S_OK;
			if (pass != 0) // (the first makes the pipelines and the light split's texture)
			{
				worst = std::max(worst, ms);
				sum += ms;
				drawn++;
			}
		}
		const uint32_t big_messages = debug_messages(info, first);
		char line[400];
		std::snprintf(line, sizeof(line), "%ux%u, per pixel, 64 lamps%s: %d passes timed, %.2f ms each on average, %.2f ms the longest; the device %s; %u debug messages", W, H,
			native_lamps ? (inj::status()[0].find("the lamps' glow added") != std::string::npos ? ", World FX's glow of 16 lamps (4 cubes, 4 capsules) added by the pass" :
				", World FX's glow NOT added (no block taken)") : "",
			drawn, drawn != 0 ? sum / drawn : 0.0, worst, alive ? "still there" : "LOST", big_messages);
		hifi::sun::release();
		if (bench_cubes != nullptr)
			bench_cubes->Release();
		check(alive && worst < 500.0 && big_messages == 0, line + (big_messages != 0 ? ": " + first : std::string()));
		std::printf("%s", hifi::zone::breadcrumb_report().c_str());
		for (const std::string &s : inj::status())
			std::printf("  %s\n", s.c_str());
		if (failures == 0)
			std::printf("\nall passed (%d)\n", passes);
		else
			std::printf("\n%d failed\n", failures);
		return failures == 0 ? 0 : 1;
	}

	// What came out
	uint8_t *px = nullptr;
	uint32_t *dz = nullptr;
	readback->Map(0, nullptr, reinterpret_cast<void **>(&px));
	depth_back->Map(0, nullptr, reinterpret_cast<void **>(&dz));
	uint32_t not_sky = 0, nearer = 0;
	for (UINT y = 0; y < H; ++y)
		for (UINT x = 0; x < W; ++x)
		{
			const uint8_t *p = px + (size_t(y) * W + x) * 4;
			not_sky += !(std::abs(p[0] - 140) <= 2 && std::abs(p[1] - 166) <= 2 && std::abs(p[2] - 199) <= 2);
			nearer += (dz[size_t(y) * W + x] & 0xFFFFFF) < 0xFFFFFF;
		}
	const double share = double(not_sky) / (W * H), depth_share = double(nearer) / (W * H);
	char line[400];
	std::snprintf(line, sizeof(line), "the zone in the target (%.0f%% of it not the sky) and its depth (%.0f%% nearer than the far plane)", share * 100, depth_share * 100);
	check(share > 0.5 && depth_share > 0.5, line);
	// (the character: the quad in the lower right, x 0.5..0.9 -> 600..760, y -0.5..-0.9 -> 337..427)
	const uint8_t *c = px + (size_t(380) * W + 680) * 4;
	std::snprintf(line, sizeof(line), "the character drawn as dgVoodoo meant: its texture's colour (%u %u %u)", c[0], c[1], c[2]);
	check(c[0] == 0xE0 && c[1] == 0x80 && c[2] == 0x20, line);
	const std::vector<std::string> st = inj::status();
	check(!st.empty() && st[0].find("1 passes drawn, 0 couldn't be") != std::string::npos, "one pass drawn, the repeat and the game's left alone: " + st[0]);
	// The light split: a word where the zone was drawn (the sky and the blended-only pixels none), its depth code the
	// depth buffer's (the camera's projection: view depth = _43 / (d + _33)) but under the character drawn after it (its
	// quad, above), the ground (facing up: y is down) mostly lit by the sun overhead (0.36, -0.83, 0.42: about 1.04 of
	// sun to 0.48 of ambient, a share of 0.68), some walls turned from it barely
	check(light != nullptr && light_back != nullptr, "the light split drawn and taken");
	if (light_back != nullptr)
	{
		uint32_t *lw = nullptr;
		light_back->Map(0, nullptr, reinterpret_cast<void **>(&lw));
		const float zn = 0.5f, zf = 2000.0f, p33 = zf / (zn - zf), p43 = zn * zf / (zn - zf);
		uint32_t coded = 0, drawn = 0, agree = 0, up = 0, dark = 0, under = 0, under_agree = 0;
		double up_share = 0.0;
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const uint32_t v = lw[size_t(y) * W + x], dd = dz[size_t(y) * W + x] & 0xFFFFFF;
				drawn += dd < 0xFFFFFF;
				const uint32_t code = v & 0x7FF;
				if (code == 0)
					continue;
				coded++;
				const double depth = p43 / (dd / 16777215.0 + p33);
				const bool same = std::abs((std::log2(std::max(depth, 1e-3)) + 3.0) * 128.0 - code) <= 2.5;
				const bool character = x >= 602 && x < 758 && y >= 339 && y < 425;
				under += character;
				under_agree += character && same;
				agree += !character && same;
				float o[2] = { float((v >> 11) & 127) / 127.0f * 2 - 1, float((v >> 18) & 127) / 127.0f * 2 - 1 };
				float n[3] = { o[0], o[1], 1.0f - std::abs(o[0]) - std::abs(o[1]) };
				const float t = std::clamp(-n[2], 0.0f, 1.0f);
				n[0] += n[0] >= 0 ? -t : t;
				n[1] += n[1] >= 0 ? -t : t;
				const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				const float share = float((v >> 25) & 63) / 63.0f; // (6 bits, bit 31 the mark: sun_shadows.hpp)
				if (n[1] / l < -0.9f)
				{
					up++;
					up_share += share;
				}
				dark += share < 0.1f;
			}
		up_share = up != 0 ? up_share / up : 0.0;
		std::snprintf(line, sizeof(line), "the light split: a word on %.0f%% of the zone's pixels, %.2f%% of them at the depth's own, %u of %u under the "
			"character; %u facing up, the sun's share there %.2f; %u with under a tenth", drawn != 0 ? 100.0 * coded / drawn : 0.0,
			coded > under ? 100.0 * agree / (coded - under) : 0.0, under_agree, under, up, up_share, dark);
		check(coded > drawn / 2 && agree > (coded - under) * 0.995 && under > 10000 && under_agree < under / 50 && up > 1000 && up_share > 0.55 &&
			up_share < 0.8 && dark > 100, line);
	}
	// The sun's cascade from the zone's data: nothing before a zone was drawn; then the town seen from above: its streets,
	// plazas and gate (18% of the box: walls seen from straight above are edge on, and the box holds the whole layout),
	// its depths over the box's height
	{
		float *sz = nullptr;
		sun_back->Map(0, nullptr, reinterpret_cast<void **>(&sz));
		uint32_t covered = 0, low = 0, high = 0;
		for (size_t i = 0; i < size_t(kMap) * kMap; ++i)
		{
			const float d = sz[i];
			if (!(d < 1.0f))
				continue;
			covered++;
			low += d > 0.5f;
			high += d < 0.5f;
		}
		const double share = double(covered) / (double(kMap) * kMap);
		std::snprintf(line, sizeof(line), "the zone's sun cascade from its data: no zone before it was drawn (%s), its box %.0f..%.0f x %.0f..%.0f x %.0f..%.0f; %u triangles, %.0f%% of the map covered (%u deeper half, %u nearer)",
			bounds_before ? "had one" : "none", zlo[0], zhi[0], zlo[1], zhi[1], zlo[2], zhi[2], sun_triangles, share * 100, low, high);
		check(!bounds_before && bounds_after && sun_triangles > 10000 && share > 0.12 && low > 0 && high > 0, line);
		// (a picture of it for a look, 512 square, nearer brighter, nothing black: build\zone_sun_map.pgm)
		if (FILE *pf = nullptr; fopen_s(&pf, "build\\zone_sun_map.pgm", "wb") == 0 && pf != nullptr)
		{
			std::fprintf(pf, "P5 512 512 255\n");
			for (UINT y = 0; y < 512; ++y)
				for (UINT x = 0; x < 512; ++x)
				{
					const float d = sz[size_t(y * 4) * kMap + x * 4];
					const uint8_t v = d < 1.0f ? uint8_t(40 + 215 * (1.0f - std::min(d, 1.0f))) : 0;
					std::fwrite(&v, 1, 1, pf);
				}
			std::fclose(pf);
		}
	// The ground's colour map: what faces up covers a good part of it, every height within the zone's box, colours lit
	// and not black (build\zone_ground_map.ppm for a look)
	if (ground_back != nullptr)
	{
		const auto half = [](uint16_t h) {
			const uint32_t sign = uint32_t(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023;
			uint32_t bits;
			if (e == 0)
				bits = sign; // (denormals as 0: none matter here)
			else if (e == 31)
				bits = sign | 0x7F800000u | (m << 13);
			else
				bits = sign | ((e - 15 + 127) << 23) | (m << 13);
			float f;
			std::memcpy(&f, &bits, 4);
			return f;
		};
		uint16_t *gp = nullptr;
		ground_back->Map(0, nullptr, reinterpret_cast<void **>(&gp));
		uint32_t covered = 0, inside = 0, lit = 0;
		double luma = 0.0;
		for (size_t i = 0; i < size_t(kGround) * kGround; ++i)
		{
			const float y = half(gp[i * 4 + 3]);
			if (y < -60000.0f)
				continue;
			covered++;
			inside += y >= zlo[1] - 1.0f && y <= zhi[1] + 1.0f;
			const float r = half(gp[i * 4]), g = half(gp[i * 4 + 1]), b = half(gp[i * 4 + 2]);
			const float l = 0.3f * r + 0.59f * g + 0.11f * b;
			lit += l > 0.02f && l <= 1.0f;
			luma += l;
		}
		const double share = double(covered) / (double(kGround) * kGround);
		std::snprintf(line, sizeof(line), "the ground's colour map from the zone's data: %u triangles, %.0f%% of it covered, %u of %u heights in the zone's box, "
			"%u lit (a mean brightness of %.2f)", ground_triangles, share * 100, inside, covered, lit, covered != 0 ? luma / covered : 0.0);
		check(ground_triangles > 1000 && share > 0.1 && inside == covered && lit > covered / 2, line);
		if (FILE *pf = nullptr; fopen_s(&pf, "build\\zone_ground_map.ppm", "wb") == 0 && pf != nullptr)
		{
			std::fprintf(pf, "P6 %u %u 255\n", kGround, kGround);
			for (size_t i = 0; i < size_t(kGround) * kGround; ++i)
			{
				const bool here = half(gp[i * 4 + 3]) > -60000.0f;
				uint8_t c[3];
				for (int k = 0; k < 3; ++k)
					c[k] = here ? uint8_t(std::min(255.0f, std::max(0.0f, half(gp[i * 4 + k])) * 255.0f)) : uint8_t(k == 2 ? 80 : 0);
				std::fwrite(c, 1, 3, pf);
			}
			std::fclose(pf);
		}
		ground_back->Unmap(0, nullptr);
	}
		sun_back->Unmap(0, nullptr);
		check(st.size() > 1 && st[1].find("1 drawn") != std::string::npos, "counted: " + (st.size() > 1 ? st[1] : std::string()));
	}
	// A lamp no object names (the game names up to 4 per object; this one none): a yalm in front of the street where the
	// picture's middle column meets its lower third, and a yalm up. Per pixel (seq 7 without it, 8 with it) the street
	// round it gets brighter; per vertex (seq 9) it's as the first pass drew it, unlit by it; and named by every object
	// (seq 10), per vertex it lights the street (the game's 4 a byte each now).
	{
		const UINT lx = W / 2, ly = H * 7 / 10;
		const double d = double(dz[size_t(ly) * W + lx] & 0xFFFFFF) / 16777215.0;
		// (the point there, back through the view and projection: row vectors, so world = (ndc x w) x inverse)
		double m[4][8] = {};
		for (int i = 0; i < 4; ++i)
		{
			for (int j = 0; j < 4; ++j)
				m[i][j] = vp.m[i * 4 + j];
			m[i][4 + i] = 1.0;
		}
		for (int c = 0; c < 4; ++c)
		{
			int best = c;
			for (int r = c + 1; r < 4; ++r)
				best = std::fabs(m[r][c]) > std::fabs(m[best][c]) ? r : best;
			for (int k = 0; k < 8; ++k)
				std::swap(m[c][k], m[best][k]);
			const double pivot = m[c][c];
			for (int k = 0; k < 8; ++k)
				m[c][k] /= pivot;
			for (int r = 0; r < 4; ++r)
				if (r != c)
				{
					const double f = m[r][c];
					for (int k = 0; k < 8; ++k)
						m[r][k] -= f * m[c][k];
				}
		}
		const double ndc[4] = { (lx + 0.5) / W * 2.0 - 1.0, 1.0 - (ly + 0.5) / H * 2.0, d, 1.0 };
		double q[4] = {};
		for (int j = 0; j < 4; ++j)
			for (int i = 0; i < 4; ++i)
				q[j] += ndc[i] * m[i][4 + j];
		const float P[3] = { float(q[0] / q[3]), float(q[1] / q[3]), float(q[2] / q[3]) };
		float back[3] = { eye[0] - P[0], 0.0f, eye[2] - P[2] };
		const float bl = std::sqrt(back[0] * back[0] + back[2] * back[2]);
		hifi::zone::frame_lamp lamp = {};
		lamp.pos[0] = P[0] + back[0] / bl;
		lamp.pos[1] = P[1] - 1.0f;
		lamp.pos[2] = P[2] + back[2] / bl;
		lamp.range = 6.0f;
		lamp.colour[0] = 1.0f;
		lamp.colour[1] = 0.75f;
		lamp.colour[2] = 0.4f;
		lamp.attenuation2 = 0.5f;
		// (the first pass's pixels round it, per vertex without the lamp, before the readback is used again)
		auto luma_at = [&](const uint8_t *img) {
			double sum = 0.0;
			for (UINT y = ly - 6; y <= ly + 6; ++y)
				for (UINT x = lx - 6; x <= lx + 6; ++x)
				{
					const uint8_t *p = img + (size_t(y) * W + x) * 4;
					sum += 0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2];
				}
			return sum / 169.0;
		};
		const double first_luma = luma_at(px);
		alloc->Reset();
		list->Reset(alloc, nullptr);
		list->SetDescriptorHeaps(1, &srvh); // (dgVoodoo's heaps on its new list, as the stand-in's root table needs them)
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		// (and a fifth, drawn first: the first again with the texture pack's FSR bias set, inject::set_texture_bias: the
		// zone's game textures have one level, so the same picture, through the shader's SampleBias; before it a sixth and
		// seventh, the first over the red-levelled copy at 0 and at -1: less red at -1, so the bias set here reaches the
		// pass; the lamp's pass last, for the breadcrumbs below)
		ID3D12Resource *outs[7] = {};
		bd.Width = UINT64(W) * 4 * H;
		for (ID3D12Resource *&o : outs)
			device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&o));
		const uint32_t seqs[7] = { 7, 8, 9, 10, 11, 12, 13 };
		for (const int k : { 5, 6, 4, 0, 1, 2, 3 })
		{
			fill_zone = k >= 5 ? &gz_red : &gz;
			fill(seqs[k], true, k == 0 || k >= 4 ? nullptr : &lamp, k == 3);
			fill_zone = &gz;
			inj::set_options(k < 2 || k >= 4);
			inj::set_texture_bias(k == 4 || k == 6 ? -1.0f : 0.0f);
			list->ClearRenderTargetView(rtv, sky, 0, nullptr);
			list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			inj::on_zone_end(list, w, seqs[k]);
			barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
			to.pResource = outs[k];
			to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			to.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, W, H, 1, W * 4 };
			from.pResource = A;
			from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
			barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}
		inj::set_options(false);
		inj::set_texture_bias(0.0f);
		// (the target and its depth back as the first pass left them, for the parts after this one)
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
		list->Close();
		direct->ExecuteCommandLists(1, lists);
		direct->Signal(fence, 2);
		fence->SetEventOnCompletion(2, e);
		WaitForSingleObject(e, 60000);
		check(device->GetDeviceRemovedReason() == S_OK, "the device still there (the lamp's passes)");
		// The breadcrumbs after the passes finished: the last frame's pieces all finished, its lamp named
		{
			const std::string crumbs = hifi::zone::breadcrumb_report();
			if (crumbs.empty())
				std::printf("      (no breadcrumbs: this device can't write them%s)\n", on_gpu ? "" : "; ZONE_GPU=1 runs this on the GPU");
			else
				check(crumbs.find("every zone draw the GPU began it finished") != std::string::npos && crumbs.find("1 lamps:") != std::string::npos &&
					crumbs.find("lamp 0: at") != std::string::npos && crumbs.find("the last finished: draw") != std::string::npos &&
					crumbs.find("got as far as: the first pipeline bound") != std::string::npos,
					"the breadcrumbs: every draw begun finished, the last named, the setup's last step, the frame's lamp listed\n" + crumbs);
		}
		const uint32_t lamp_messages = debug_messages(info, first);
		check(lamp_messages == 0, "the debug layer: nothing (" + std::to_string(lamp_messages) + (lamp_messages != 0 ? ": " + first : std::string()) + ")");
		double lumas[5] = {};
		std::vector<uint8_t> pictures[2], reds[2];
		// (how red a picture is: each pixel's red past the mean of its green and blue, summed)
		auto redness = [](const std::vector<uint8_t> &img) {
			double sum = 0.0;
			for (size_t i = 0; i + 3 < img.size(); i += 4)
				sum += std::max(0.0, double(img[i]) - 0.5 * (double(img[i + 1]) + double(img[i + 2])));
			return sum;
		};
		for (int k = 0; k < 7; ++k)
		{
			uint8_t *img = nullptr;
			outs[k]->Map(0, nullptr, reinterpret_cast<void **>(&img));
			if (k < 5)
				lumas[k] = luma_at(img);
			if (k == 0 || k == 4)
				pictures[k == 4].assign(img, img + size_t(W) * H * 4);
			if (k >= 5)
				reds[k == 6].assign(img, img + size_t(W) * H * 4);
			outs[k]->Unmap(0, nullptr);
			outs[k]->Release();
		}
		check(!pictures[0].empty() && pictures[0] == pictures[1], "the texture pack's FSR bias (-1, inject::set_texture_bias) on the zone's one-level game textures: the same picture");
		std::snprintf(line, sizeof(line), "  over the red-levelled copy: less of the red level 1 at -1 (%.0f -> %.0f), so the bias set reaches the pass",
			redness(reds[0]), redness(reds[1]));
		check(!reds[0].empty() && reds[0] != reds[1] && redness(reds[1]) < redness(reds[0]), line);
		std::snprintf(line, sizeof(line), "a lamp no object names, at (%.1f %.1f %.1f): the street under it per pixel %.1f -> %.1f; per vertex %.1f (first pass %.1f)",
			lamp.pos[0], lamp.pos[1], lamp.pos[2], lumas[0], lumas[1], lumas[2], first_luma);
		check(d < 1.0 && std::fabs(lumas[1] - lumas[0]) < 0.5, line);
		// (the first pass per vertex as the game draws it, unless ZONE_PERPIXEL drew it per pixel: then there's no per-vertex
		// picture without the lamp to hold this one to)
		const bool first_per_vertex = GetEnvironmentVariableA("ZONE_PERPIXEL", nullptr, 0) == 0;
		check(!first_per_vertex || std::fabs(lumas[2] - first_luma) < 0.5, "  per vertex, as the game lights: only the lamps an object names (none here)");
		check(lumas[3] > first_luma + 5.0, "  ... and named by every object, per vertex it lights them: " + std::to_string(lumas[3]));

		// World FX's lamp glow added by the pass itself (src/render/sun_shadows.hpp's lamps). A block published as World FX
		// publishes it, with one lamp 3 yalms over that point of the street (reach 9), no lamp in the frame itself. The
		// street under it gets brighter and every word of the light split is marked (bit 31), lit per pixel or per vertex
		// (the block is looked up per pixel either way); with the block gone stale the picture is the one without any
		// block, byte for byte, and no word is marked. A zone's lamp (it casts) loses light behind a character's capsule
		// standing under it, and behind what its cube holds (a cube that hides everything: no glow left; an empty one: all
		// of it). The faithful look (the block's small-room word, lamp_map.w 1: what a Mog House gets) for the measures;
		// then the tuned one. (The cases "[zone] nativelamps off: the picture as with no block" and "per vertex: the block
		// not applied" went with the switch and with World FX's own glow, which lit what the pass didn't.)
		{
			namespace sun = hifi::sun;
			D3D12_HEAP_PROPERTIES up_heap = {};
			up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
			ID3D12Resource *cubes[2] = {};
			const UINT64 cube_bytes = UINT64(256) * 256 * 4 * 6;
			bd.Width = cube_bytes;
			for (int k = 0; k < 2; ++k)
			{
				device->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cubes[k]));
				float *texels = nullptr;
				cubes[k]->Map(0, nullptr, reinterpret_cast<void **>(&texels));
				for (size_t i = 0; i < size_t(cube_bytes / 4); ++i)
					texels[i] = k == 0 ? 0.0f : 1.0f; // (0: something at the near plane every way; 1: nothing drawn)
				cubes[k]->Unmap(0, nullptr);
			}
			// (kind: 0 a glow, 1 a zone's lamp, 2 one with its cube; body: a character standing on the point)
			auto publish_lamp = [&](float kind, bool body, ID3D12Resource *cube, bool stale, bool tuned) {
				sun::block b;
				b.device = device;
				b.buffer = cube;
				sun::gpu_block &g = b.gpu;
				g.mode[2] = sun::kHasLamps;
				g.lamp_head[0] = 1.0f;
				g.lamp_head[1] = 1.0f;
				g.lamp_head[2] = body ? 1.0f : 0.0f;
				const float at[4] = { P[0], P[1] - 3.0f, P[2], 9.0f }, colour[4] = { 0.73f, 0.5f, 0.11f, kind };
				std::memcpy(g.lamp_pos[0], at, 16);
				std::memcpy(g.lamp_col[0], colour, 16);
				g.lamp_where[1] = 256;
				g.lamp_map[0] = 1.0f;
				g.lamp_map[1] = 0.95f;
				g.lamp_map[2] = 1.0f;
				g.lamp_map[3] = tuned ? 0.0f : 1.0f;
				const float feet[4] = { P[0], P[1], P[2], 1.0f };
				std::memcpy(g.body[0], feet, 16);
				sun::publish(b);
				if (stale)
				{
					std::lock_guard<std::mutex> lock(sun::detail::g.mutex);
					sun::detail::g.at -= 10 * sun::kFreshMs;
				}
			};
			struct pass_t
			{
				const char *name;
				bool per_pixel, option;
				float kind;
				bool body;
				int cube; // (-1: no buffer)
				bool stale;
				bool tuned = false; // (the tuned look, the default: sun_shadows.hpp; else the faithful port)
				int sun = 0;        // (the sun's shadows in the block instead: 1 a cascade with nothing in it, 2 all of it in shadow)
			};
			// The sun's shadows from the block (a cascade made by hand, 128 yalms across round that point of the street, a
			// texel 2 yalms: every texel nearer the sun than anything, or none): under the empty one the picture is the one
			// without a block, byte for byte; under the full one the sunlit street loses the sun's part of its light, lit
			// per pixel or per vertex.
			const UINT kCascade = 64;
			ID3D12Resource *cascade_map = nullptr;
			bd.Width = UINT64(kCascade) * kCascade * 4 * 2;
			device->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cascade_map));
			{
				float *texels = nullptr;
				cascade_map->Map(0, nullptr, reinterpret_cast<void **>(&texels));
				for (size_t i = 0; i < size_t(kCascade) * kCascade; ++i)
				{
					texels[i] = 0.0f;
					texels[size_t(kCascade) * kCascade + i] = 1.0f;
				}
				cascade_map->Unmap(0, nullptr);
			}
			auto publish_sun = [&](bool full) {
				sun::block b;
				b.device = device;
				b.buffer = cascade_map;
				sun::gpu_block &g = b.gpu;
				g.mode[2] = sun::kHasSun;
				const float texel = 2.0f;
				for (int d = 0; d < 2; ++d)
				{
					g.rows[3 * d][0] = 1.0f / texel;
					g.rows[3 * d][3] = -(P[0] - 64.0f) / texel;
					g.rows[3 * d + 1][2] = 1.0f / texel;
					g.rows[3 * d + 1][3] = -(P[2] - 64.0f) / texel;
					g.rows[3 * d + 2][3] = 0.5f;
				}
				g.cascade[0][0] = g.cascade[0][1] = 0.001f;
				g.cascade[0][2] = texel;
				g.cascade[0][3] = 1.0f;
				g.where[0][0] = g.where[0][1] = full ? 0u : kCascade * kCascade;
				g.where[0][2] = g.where[0][3] = kCascade;
				g.sun[1] = -1.0f;
				g.sun[3] = 0.8f;
				sun::publish(b);
			};
			const pass_t passes[] = {
				{ "no block", true, false, 0.0f, false, -1, false },
				{ "a glow, again", true, true, 0.0f, false, -1, false },
				{ "a glow", true, true, 0.0f, false, -1, false },
				{ "per vertex, no block", false, false, 0.0f, false, -1, false },
				{ "per vertex", false, true, 0.0f, false, -1, false },
				{ "a stale block", true, true, 0.0f, false, -1, true },
				{ "a zone's lamp", true, true, 1.0f, false, -1, false },
				{ "a zone's lamp, a character under it", true, true, 1.0f, true, -1, false },
				{ "a zone's lamp, its cube hiding everything", true, true, 2.0f, false, 0, false },
				{ "a zone's lamp, its cube empty", true, true, 2.0f, false, 1, false },
				{ "the tuned look: a zone's lamp", true, true, 1.0f, false, -1, false, true },
				{ "the tuned look: a character under it", true, true, 1.0f, true, -1, false, true },
				{ "the tuned look: its cube hiding everything", true, true, 2.0f, false, 0, false, true },
				{ "the sun: a cascade with nothing in it", true, true, 0.0f, false, -1, false, false, 1 },        // 13
				{ "the sun: a cascade all in shadow", true, true, 0.0f, false, -1, false, false, 2 },             // 14
				{ "the sun, per vertex: a cascade all in shadow", false, true, 0.0f, false, -1, false, false, 2 }, // 15
			};
			constexpr int kPasses = int(sizeof(passes) / sizeof(passes[0]));
			ID3D12Resource *shots[kPasses] = {}, *words[kPasses] = {};
			bd.Width = UINT64(W) * 4 * H;
			for (int k = 0; k < kPasses; ++k)
			{
				device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&shots[k]));
				device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&words[k]));
			}
			alloc->Reset();
			list->Reset(alloc, nullptr);
			list->SetDescriptorHeaps(1, &srvh);
			barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			bool lamps_added[kPasses] = {}, split_drawn[kPasses] = {};
			for (int k = 0; k < kPasses; ++k)
			{
				const pass_t &ps = passes[k];
				const uint32_t seq = 40 + uint32_t(k);
				fill(seq, true);
				inj::set_options(ps.per_pixel);
				if (k == 0 || k == 3)
					sun::publish_none();
				else if (ps.sun != 0)
					publish_sun(ps.sun == 2);
				else
					publish_lamp(ps.kind, ps.body, ps.cube >= 0 ? cubes[ps.cube] : nullptr, ps.stale, ps.tuned);
				hifi::light::take(); // (the light split wanted, as World FX asks each frame)
				list->ClearRenderTargetView(rtv, sky, 0, nullptr);
				list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
				inj::on_zone_end(list, w, seq);
				lamps_added[k] = inj::status()[0].find("the lamps' glow added") != std::string::npos;
				barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
				D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
				to.pResource = shots[k];
				to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				to.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, W, H, 1, W * 4 };
				from.pResource = A;
				from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
				barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				if (ID3D12Resource *const split = hifi::light::take(); split != nullptr)
				{
					split_drawn[k] = true;
					barrier(list, split, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, hifi::light::kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
					to.pResource = words[k];
					to.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, W, H, 1, W * 4 };
					from.pResource = split;
					list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
					barrier(list, split, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, hifi::light::kRead);
				}
			}
			inj::set_options(false);
			sun::release();
			barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			list->Close();
			direct->ExecuteCommandLists(1, lists);
			const UINT64 fv = fence->GetCompletedValue() + 1;
			direct->Signal(fence, fv);
			fence->SetEventOnCompletion(fv, e);
			WaitForSingleObject(e, 120000);
			check(device->GetDeviceRemovedReason() == S_OK, "the device still there (the native lamps' passes)");
			const uint32_t glow_messages = debug_messages(info, first);
			check(glow_messages == 0, "the debug layer: nothing (" + std::to_string(glow_messages) + (glow_messages != 0 ? ": " + first : std::string()) + ")");
			{
				const std::string crumbs = hifi::zone::breadcrumb_report();
				if (!crumbs.empty())
					check(crumbs.find("every zone draw the GPU began it finished") != std::string::npos, "the breadcrumbs after the native lamps' passes: every draw begun finished");
			}
			std::vector<uint8_t> img[kPasses];
			uint32_t coded[kPasses] = {}, marked[kPasses] = {};
			double spot[kPasses] = {};
			for (int k = 0; k < kPasses; ++k)
			{
				uint8_t *p = nullptr;
				shots[k]->Map(0, nullptr, reinterpret_cast<void **>(&p));
				img[k].assign(p, p + size_t(W) * H * 4);
				spot[k] = luma_at(p);
				shots[k]->Unmap(0, nullptr);
				shots[k]->Release();
				uint32_t *v = nullptr;
				words[k]->Map(0, nullptr, reinterpret_cast<void **>(&v));
				for (size_t i = 0; split_drawn[k] && i < size_t(W) * H; ++i)
					if ((v[i] & 0x7FF) != 0)
					{
						coded[k]++;
						marked[k] += v[i] >> 31;
					}
				words[k]->Unmap(0, nullptr);
				words[k]->Release();
			}
			for (ID3D12Resource *c : cubes)
				c->Release();
			cascade_map->Release();
			{
				const double vertex_lit = luma_at(img[3].data()), vertex_shade = luma_at(img[15].data());
				std::snprintf(line, sizeof(line), "the sun's shadows by the zone pass (a cascade made by hand): the sunlit street %.1f; under an empty cascade %s; all in "
					"shadow %.1f (%u of %u words marked); per vertex %.1f -> %.1f", spot[0], img[13] == img[0] ? "the same picture, byte for byte" : "ANOTHER PICTURE",
					spot[14], marked[14], coded[14], vertex_lit, vertex_shade);
				check(img[13] == img[0] && marked[13] == coded[13] && coded[13] > size_t(W) * H / 2, "  " + std::string(line));
				check(spot[14] < 0.9 * spot[0] && spot[14] > 0.3 * spot[0] && marked[14] == coded[14], "  a cascade all in shadow: the street loses the sun's part of its light, the rest stays");
				check(vertex_shade < 0.9 * vertex_lit && vertex_shade > 0.3 * vertex_lit && marked[15] == coded[15], "  and lit per vertex the same: the shadow looked up per pixel");
			}
			size_t changed = 0;
			for (size_t i = 0; i < size_t(W) * H; ++i)
				changed += std::memcmp(&img[2][i * 4], &img[0][i * 4], 3) != 0 ? 1 : 0;
			std::snprintf(line, sizeof(line), "World FX's lamp glow by the zone pass (a block with one lamp 3 yalms over the street): the street under it %.1f -> %.1f "
				"(%zu pixels changed), %u of %u words marked; the pass says so: %s", spot[0], spot[2], changed, marked[2], coded[2], lamps_added[2] ? "yes" : "no");
			check(spot[2] > spot[0] + 5.0 && changed > 1000 && coded[2] > size_t(W) * H / 2 && marked[2] == coded[2] && lamps_added[2], line);
			check(img[0] == pictures[0], "  no block: the per-pixel picture as the lamp part's first (the same pass again)");
			check(img[1] == img[2] && lamps_added[1], "  the same block again: the same picture, byte for byte");
			double vertex_spot[2] = {};
			for (int k = 0; k < 2; ++k)
				vertex_spot[k] = luma_at(img[3 + k].data());
			std::snprintf(line, sizeof(line), "  per vertex with the block: the street under the lamp %.1f -> %.1f, %u of %u words marked (the pass says %s)", vertex_spot[0],
				vertex_spot[1], marked[4], coded[4], lamps_added[4] ? "added" : "not added");
			check(img[3] != img[0] && vertex_spot[1] > vertex_spot[0] + 5.0 && coded[4] > size_t(W) * H / 2 && marked[4] == coded[4] && lamps_added[4], line);
			std::snprintf(line, sizeof(line), "  a stale block: the picture as with no block, byte for byte, %u of %u words marked", marked[5], coded[5]);
			check(img[5] == img[0] && coded[5] > size_t(W) * H / 2 && marked[5] == 0 && !lamps_added[5], line);
			std::snprintf(line, sizeof(line), "  a zone's lamp %.1f; a character standing under it %.1f; its cube hiding everything %.1f (no lamp %.1f), empty %.1f",
				spot[6], spot[7], spot[8], spot[0], spot[9]);
			check(std::fabs(spot[6] - spot[2]) < 0.5 && spot[7] < spot[6] - 2.0 && spot[7] >= spot[0] - 0.5 && std::fabs(spot[8] - spot[0]) < 0.5 &&
				std::fabs(spot[9] - spot[6]) < 0.5 && marked[7] == coded[7] && marked[8] == coded[8], line);
			// (the tuned look, the default: by day the sunlit street takes next to none of it, the light being there already;
			// what's in the shade round it does)
			size_t tuned_changed = 0, tuned_hidden = 0;
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				tuned_changed += std::memcmp(&img[10][i * 4], &img[0][i * 4], 3) != 0 ? 1 : 0;
				tuned_hidden += std::memcmp(&img[12][i * 4], &img[0][i * 4], 3) != 0 ? 1 : 0;
			}
			std::snprintf(line, sizeof(line), "  the tuned look by day: the sunlit street under the lamp %.1f -> %.1f (the faithful port %.1f), %zu pixels changed (in the shade), "
				"%u of %u words marked; a character under it %.1f; its cube hiding everything: %zu pixels changed", spot[0], spot[10], spot[6], tuned_changed, marked[10],
				coded[10], spot[11], tuned_hidden);
			check(spot[10] >= spot[0] && spot[10] < spot[6] && tuned_changed > 1000 && marked[10] == coded[10] && coded[10] > size_t(W) * H / 2 &&
				spot[11] <= spot[10] && marked[12] == coded[12] && tuned_hidden < tuned_changed / 4, line);
		}
	}
	if (argc > 1)
	{
		FILE *f = nullptr;
		if (fopen_s(&f, argv[1], "wb") == 0 && f != nullptr)
		{
			uint8_t head[54] = { 'B', 'M' };
			auto put = [&](int at, uint32_t v) { std::memcpy(head + at, &v, 4); };
			put(2, 54 + W * H * 4);
			put(10, 54);
			put(14, 40);
			put(18, W);
			put(22, uint32_t(-int32_t(H)));
			head[26] = 1;
			head[28] = 32;
			fwrite(head, 1, 54, f);
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const uint8_t bgra[4] = { px[i * 4 + 2], px[i * 4 + 1], px[i * 4 + 0], 255 };
				fwrite(bgra, 1, 4, f);
			}
			fclose(f);
		}
	}
	// The moving layer and the second map (zoneshadow.hpp; notes-mesh-shadows.md M3), on a made-up zone: a swaying card
	// (kind 2, its sway vector 4 yalms along x) placed twice, a door (its part drawn 4 yalms along z from its placement),
	// a wall, each a 2 x 2 yalm card lying flat at its own height; and a second map with a roof. Drawn looking straight
	// down (depth by height: the card 0.5, the door 0.45, the wall 0.55, the roof 0.25), 512 texels square.
	{
		auto card = [](zm::zone &z, const char *name, uint32_t kind, float y, float sway_x) {
			zm::model m;
			std::memcpy(m.name, name, std::strlen(name));
			m.kind = kind;
			m.first_piece = uint32_t(z.pieces.size());
			m.pieces = 1;
			m.first_vertex = uint32_t(z.vertices.size());
			m.vertices = 4;
			const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
			for (const auto &c : corners)
			{
				zm::vertex v = {};
				v.pos[0] = c[0];
				v.pos[1] = y;
				v.pos[2] = c[1];
				v.normal[1] = -1.0f;
				v.colour = 0x80FFFFFFu;
				v.sway[0] = sway_x;
				z.vertices.push_back(v);
			}
			zm::piece pc;
			pc.first_index = uint32_t(z.indices.size());
			pc.triangles = 2;
			for (const uint32_t i : { 0u, 1u, 2u, 2u, 1u, 3u })
				z.indices.push_back(m.first_vertex + i);
			z.pieces.push_back(pc);
			m.lo[0] = -1.0f;
			m.hi[0] = 1.0f + sway_x;
			m.lo[1] = m.hi[1] = y;
			m.lo[2] = -1.0f;
			m.hi[2] = 1.0f;
			z.models.push_back(m);
			return uint32_t(z.models.size() - 1);
		};
		auto place = [](zm::zone &z, uint32_t model, float x, float y, float zz) {
			zm::placement p;
			p.model = model;
			p.m[0] = p.m[4] = p.m[8] = 1.0f;
			p.t[0] = x;
			p.t[1] = y;
			p.t[2] = zz;
			z.placements.push_back(p);
		};
		zm::zone za, zb;
		za.map = 9001;
		zb.map = 9002;
		const uint32_t palm = card(za, "palm", 2, 0.0f, 4.0f), door = card(za, "door", 1, 0.0f, 0.0f), wall = card(za, "wall", 1, 0.0f, 0.0f);
		place(za, palm, 0.0f, -10.0f, 0.0f);   // (in the near cascade's hole)
		place(za, palm, -24.0f, -10.0f, 0.0f); // (outside it; never in a pass: its model's sway)
		place(za, door, 10.0f, -12.0f, 0.0f);
		place(za, wall, -10.0f, -8.0f, 0.0f);
		const uint32_t roof = card(zb, "roof", 1, 0.0f, 0.0f);
		place(zb, roof, 0.0f, -20.0f, 10.0f);
		hifi::zone::gpu_zone ga, gb;
		check(ga.upload(device, copy, std::move(za), why) && gb.upload(device, copy, std::move(zb), why) && ga.wait(10000) && gb.wait(10000), "the made-up zone and second map on the GPU" + why);

		auto pass = [&](uint32_t seq, float sway) {
			hifi::zone::frame &f = hifi::zone::frame_slot(seq);
			f.native = true;
			f.zone = &ga;
			f.second = &gb;
			f.map = 9001;
			std::memcpy(f.view_proj, vp.m, 64);
			std::memcpy(f.camera, eye, 12);
			f.env_count = 1;
			f.count = 0;
			f.lamp_count = 0;
			auto object = [&](uint32_t model, float x, float y, float z, uint32_t flags, float s, uint32_t zone) {
				hifi::zone::frame_object &o = f.objects[f.count++];
				o = hifi::zone::frame_object();
				o.world[0] = o.world[5] = o.world[10] = 1.0f;
				o.world[3] = x;
				o.world[7] = y;
				o.world[11] = z;
				o.model = model;
				o.flags = flags;
				o.sway = s;
				o.zone = zone;
			};
			object(palm, 0.0f, -10.0f, 0.0f, 0, sway, 0);
			object(door, 10.0f, -12.0f, 4.0f, hifi::zone::kDoor, 0.0f, 0); // (open: 4 yalms from where it's placed)
			object(wall, -10.0f, -8.0f, 0.0f, 0, 0.0f, 0);
			object(roof, 0.0f, -20.0f, 10.0f, 0, 0.0f, 1);
			hifi::zone::publish(f, seq);
		};

		constexpr UINT kSmall = 512;
		ID3D12Resource *cmap = nullptr, *backs[5] = {};
		ID3D12DescriptorHeap *small_heap = nullptr;
		{
			D3D12_RESOURCE_DESC cd = {};
			cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			cd.Width = kSmall;
			cd.Height = kSmall;
			cd.DepthOrArraySize = 1;
			cd.MipLevels = 1;
			cd.Format = DXGI_FORMAT_D32_FLOAT;
			cd.SampleDesc.Count = 1;
			cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			D3D12_CLEAR_VALUE cv = {};
			cv.Format = DXGI_FORMAT_D32_FLOAT;
			cv.DepthStencil.Depth = 1.0f;
			D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT }, rh = { D3D12_HEAP_TYPE_READBACK };
			device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&cmap));
			D3D12_DESCRIPTOR_HEAP_DESC sd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
			device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&small_heap));
			device->CreateDepthStencilView(cmap, nullptr, small_heap->GetCPUDescriptorHandleForHeapStart());
			D3D12_RESOURCE_DESC b = {};
			b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			b.Width = UINT64(kSmall) * kSmall * 4;
			b.Height = 1;
			b.DepthOrArraySize = 1;
			b.MipLevels = 1;
			b.SampleDesc.Count = 1;
			b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			for (ID3D12Resource *&back : backs)
				device->CreateCommittedResource(&rh, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&back));
		}
		const D3D12_CPU_DESCRIPTOR_HANDLE small_dsv = small_heap->GetCPUDescriptorHandleForHeapStart();
		// (looking straight down, centred on the origin, reach yalms each way: x across, z up the map, depth by height
		// from 30 yalms up over 40)
		auto down = [](float reach, float out[16]) {
			const float m[16] = { 1.0f / reach, 0, 0, 0, 0, 0, 1.0f / 40.0f, 0, 0, -1.0f / reach, 0, 0, 0, 0, 30.0f / 40.0f, 1 };
			std::memcpy(out, m, sizeof(m));
		};

		alloc->Reset();
		list->Reset(alloc, nullptr);
		list->SetDescriptorHeaps(1, &srvh);
		// (the world back from its copies: dgVoodoo's planes as for the marker before)
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		uint64_t gen_before = 0, gen_after = 0;
		float blo[3], bhi[3];
		auto map_out = [&](int k, auto &&draw) {
			list->ClearDepthStencilView(small_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			draw();
			list->SetDescriptorHeaps(1, &srvh); // (as our d3d8.dll does after a mark: its heaps, then its state)
			bound.restore(list);
			barrier(list, cmap, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			D3D12_TEXTURE_COPY_LOCATION sdst = {}, ssrc = {};
			sdst.pResource = backs[k];
			sdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			sdst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_TYPELESS, kSmall, kSmall, 1, kSmall * 4 };
			ssrc.pResource = cmap;
			ssrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			list->CopyTextureRegion(&sdst, 0, 0, 0, &ssrc, nullptr);
			barrier(list, cmap, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		};
		float m_movers[16], m_near[16], m_far[16];
		down(32.0f, m_movers);
		down(40.0f, m_near);
		down(160.0f, m_far);
		bool movers_drew[2] = {};
		uint32_t near_triangles = 0;
		// The pass with the card at rest, then swayed all the way: the moving layer after each
		pass(9, 0.0f);
		inj::on_zone_end(list, w, 9);
		const bool bounds_now = inj::zone_bounds(blo, bhi, gen_before);
		map_out(0, [&] { movers_drew[0] = inj::record_zone_movers(list, m_movers, small_dsv, kSmall); });
		pass(10, 1.0f);
		inj::on_zone_end(list, w, 10);
		map_out(1, [&] { movers_drew[1] = inj::record_zone_movers(list, m_movers, small_dsv, kSmall); });
		inj::zone_bounds(blo, bhi, gen_after);
		// The near and far cascades, the layer drawn just now
		map_out(2, [&] { near_triangles = inj::record_zone_casters(list, m_near, small_dsv, kSmall); });
		map_out(3, [&] { inj::record_zone_casters(list, m_far, small_dsv, kSmall); });
		// A lamp's cube face (World FX's draw_lamp_maps: a perspective from the light, here 4 yalms over the card looking
		// down at it, from 1 yalm to 8; its middle 0.95 of it a quarter turn)
		const float lamp_at[3] = { 0.0f, -14.0f, 0.0f }, l_near = 1.0f, l_far = 8.0f, l_scale = 0.95f;
		const float l_a = l_far / (l_far - l_near), l_b = -l_near * l_far / (l_far - l_near);
		const float m_lamp[16] = { l_scale, 0, 0, 0, 0, 0, l_a, 1, 0, l_scale, 0, 0, -l_scale * lamp_at[0], -l_scale * lamp_at[2], -l_a * lamp_at[1] + l_b, -lamp_at[1] };
		uint32_t lamp_triangles = 0;
		map_out(4, [&] { lamp_triangles = inj::record_zone_casters(list, m_lamp, small_dsv, kSmall); });
		list->Close();
		direct->ExecuteCommandLists(1, lists);
		const UINT64 moving_done = fence->GetCompletedValue() + 1; // (after the lamps' passes')
		direct->Signal(fence, moving_done);
		fence->SetEventOnCompletion(moving_done, e);
		WaitForSingleObject(e, 60000);
		check(device->GetDeviceRemovedReason() == S_OK, "the device still there after the moving layer");
		const uint32_t later = debug_messages(info, first);
		check(later == 0, "the debug layer: nothing (" + std::to_string(later) + (later != 0 ? ": " + first : std::string()) + ")");

		// Each thing by its depth: texels and their middle (column, row)
		struct band
		{
			uint32_t n = 0;
			double col = 0.0, row = 0.0;
		};
		auto measure = [&](int k, float depth, float x_from = -1.0f) {
			float *zz = nullptr;
			backs[k]->Map(0, nullptr, reinterpret_cast<void **>(&zz));
			band b;
			for (UINT y = 0; y < kSmall; ++y)
				for (UINT x = 0; x < kSmall; ++x)
					if (std::fabs(zz[size_t(y) * kSmall + x] - depth) < 0.01f && float(x) >= x_from)
					{
						b.n++;
						b.col += x;
						b.row += y;
					}
			backs[k]->Unmap(0, nullptr);
			if (b.n != 0)
			{
				b.col /= b.n;
				b.row /= b.n;
			}
			return b;
		};
		const float d_card = 0.5f, d_door = 0.45f, d_wall = 0.55f, d_roof = 0.25f;
		// (in the movers' map, 32 yalms each way: 8 texels a yalm, the origin at 256)
		const band rest = measure(0, d_card, 128.0f), swayed = measure(1, d_card, 128.0f), far_rest = measure(0, d_card), far_swayed = measure(1, d_card);
		const band open = measure(1, d_door), wall_m = measure(1, d_wall), roof_m = measure(1, d_roof);
		std::snprintf(line, sizeof(line), "the moving layer: drawn (%d %d); the card at rest at column %.1f (%u texels), swayed 4 yalms at %.1f (%u): its shadow moves with it",
			movers_drew[0], movers_drew[1], rest.col, rest.n, swayed.col, swayed.n);
		check(movers_drew[0] && movers_drew[1] && rest.n > 200 && swayed.n > 200 && std::fabs(rest.col - 256.0) < 2.0 && std::fabs(swayed.col - 288.0) < 2.0, line);
		std::snprintf(line, sizeof(line), "the card the pass didn't draw sways with its model's (its texels' middle %.1f -> %.1f over both cards)", far_rest.col, far_swayed.col);
		check(far_rest.n > rest.n && std::fabs((far_swayed.col - far_rest.col) - 32.0) < 2.0, line);
		std::snprintf(line, sizeof(line), "the open door where the pass drew it (row %.1f, column %.1f, %u texels); nothing static in the layer (wall %u, roof %u)",
			open.row, open.col, open.n, wall_m.n, roof_m.n);
		check(open.n > 200 && std::fabs(open.row - 288.0) < 2.0 && std::fabs(open.col - 336.0) < 2.0 && wall_m.n == 0 && roof_m.n == 0, line);
		// (the near cascade, 40 yalms each way: 6.4 texels a yalm; the far one 160: 1.6)
		const band n_card = measure(2, d_card), n_door = measure(2, d_door), n_wall = measure(2, d_wall), n_roof = measure(2, d_roof);
		std::snprintf(line, sizeof(line), "the near cascade leaves the movers in its middle %.0f yalms to the layer: cards %u texels (column %.1f: the far one's), door %u; keeps the wall (%u) and the interior's roof (%u); %u triangles",
			hifi::zone::zone_shadow::kHole, n_card.n, n_card.col, n_door.n, n_wall.n, n_roof.n, near_triangles);
		check(n_card.n > 100 && std::fabs(n_card.col - (256.0 - 24.0 * 6.4)) < 2.0 && n_door.n == 0 && n_wall.n > 100 && n_roof.n > 100, line);
		const band f_card = measure(3, d_card), f_middle = measure(3, d_card, 240.0f), f_door = measure(3, d_door);
		std::snprintf(line, sizeof(line), "the far cascade keeps them all at rest: cards %u texels (%u the middle one's), the door %u at its placement (row %.1f)", f_card.n, f_middle.n, f_door.n, f_door.row);
		check(f_middle.n >= 4 && f_card.n >= f_middle.n + 4 && f_door.n >= 4 && std::fabs(f_door.row - 256.0) < 2.0, line);
		std::snprintf(line, sizeof(line), "the bounds take in the second map (y %.0f..%.0f, z %.0f..%.0f); the cascades' number changes as the layer comes in", blo[1], bhi[1], blo[2], bhi[2]);
		check(bounds_now && blo[1] <= -20.0f && bhi[2] >= 11.0f && gen_before != gen_after, line);
		{
			// (the card 4 yalms down the face's middle: z = f / (f - n) x (1 - n / 4); a yalm each way of it 0.95 / 4 of the
			// face's half)
			float *zz = nullptr;
			backs[4]->Map(0, nullptr, reinterpret_cast<void **>(&zz));
			const float centre = zz[size_t(256) * kSmall + 256], expected = l_a * (1.0f - l_near / 4.0f);
			uint32_t covered = 0;
			for (size_t i = 0; i < size_t(kSmall) * kSmall; ++i)
				covered += zz[i] < 1.0f;
			backs[4]->Unmap(0, nullptr);
			const double side = 0.95 / 4.0 * kSmall;
			std::snprintf(line, sizeof(line), "a lamp's cube face: the card under it at depth %.4f (%.4f), %u texels (%.0f), %u triangles (only the card in its frustum: the rest beside, behind or past its reach)",
				centre, expected, covered, side * side, lamp_triangles);
			check(std::fabs(centre - expected) < 1e-3f && std::fabs(double(covered) - side * side) < 0.05 * side * side && lamp_triangles == 2, line);
		}
		const std::vector<std::string> st2 = inj::status();
		check(st2.size() > 2 && st2[2].find("2 frames") != std::string::npos, "counted: " + (st2.size() > 2 ? st2[2] : std::string()));
		for (ID3D12Resource *back : backs)
			back->Release();
		cmap->Release();
		small_heap->Release();
	}
	dump::unmap(base);
	std::printf("%s\n", hifi::shadercache::status().c_str());
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
