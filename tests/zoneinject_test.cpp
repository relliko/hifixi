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
 *   - a lamp in the frame that no object names (the game's per-object lamps): per pixel it lights the street near it all
 *     the same (draw.cpp near_lamps), per vertex it lights nothing (the game's own way); named by every object, per
 *     vertex it lights them.
 *
 *   build\zoneinject_test.exe [out.bmp]
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include "../src/bake/zonemesh.hpp"
#include "../src/zone/frame.hpp"
#include "../src/zone/inject.hpp"
#include "../src/zone/zone_gpu.hpp"
#include "game_dump.hpp"
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

	constexpr UINT W = 800, H = 450;

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

	void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Transition.pResource = r;
		b.Transition.Subresource = sub;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		list->ResourceBarrier(1, &b);
		hifi::zone::inject::on_barrier(1, &b); // (as the hooks hand them over)
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
	namespace inj = hifi::zone::inject;
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
		debug->EnableDebugLayer();
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
	ID3D12Device *device = nullptr;
	if (FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
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
	hifi::zone::gpu_zone gz;
	std::string why;
	check(gz.upload(device, copy, std::move(z), why) && gz.wait(10000), "on the GPU (its copy queue)" + why);
	check(inj::prepare(device, why), "the renderer made" + (why.empty() ? "" : ": " + why));

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
	auto fill = [&](uint32_t seq, bool native, const hifi::zone::frame_lamp *lamp = nullptr, bool named = false) {
		hifi::zone::frame &f = hifi::zone::frame_slot(seq);
		f.native = native;
		f.zone = &gz;
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
	inj::on_reset(list);
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
	inj::on_heaps(list, 1, &srvh);
	list->SetGraphicsRootSignature(game_root);
	list->SetGraphicsRoot32BitConstant(0, 7, 0);
	list->SetGraphicsRootDescriptorTable(1, table);
	list->SetPipelineState(game_pso);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGPUVirtualAddress(), 64, 16 };
	list->IASetVertexBuffers(0, 1, &vbv);
	// What dgVoodoo has bound now, as the backend's list_state follows it
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
	inj::on_zone_end(list, bound, w, 5);
	const D3D12_RECT pixel = { 16, 0, 17, 1 };
	list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 1, &pixel); // (the marker itself)
	// Passes it mustn't draw: the same seq again, a pass not HiFi XI's
	inj::on_zone_end(list, bound, w, 5);
	fill(6, false);
	inj::on_zone_end(list, bound, w, 6);
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
		inj::on_reset(list);
		list->SetDescriptorHeaps(1, &srvh); // (dgVoodoo's heaps on its new list, as the stand-in's root table needs them)
		inj::on_heaps(list, 1, &srvh);
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		ID3D12Resource *outs[4] = {};
		bd.Width = UINT64(W) * 4 * H;
		for (ID3D12Resource *&o : outs)
			device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&o));
		const uint32_t seqs[4] = { 7, 8, 9, 10 };
		for (int k = 0; k < 4; ++k)
		{
			fill(seqs[k], true, k == 0 ? nullptr : &lamp, k == 3);
			inj::set_options(k < 2);
			list->ClearRenderTargetView(rtv, sky, 0, nullptr);
			list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			inj::on_zone_end(list, bound, w, seqs[k]);
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
		list->Close();
		direct->ExecuteCommandLists(1, lists);
		direct->Signal(fence, 2);
		fence->SetEventOnCompletion(2, e);
		WaitForSingleObject(e, 60000);
		check(device->GetDeviceRemovedReason() == S_OK, "the device still there (the lamp's passes)");
		const uint32_t lamp_messages = debug_messages(info, first);
		check(lamp_messages == 0, "the debug layer: nothing (" + std::to_string(lamp_messages) + (lamp_messages != 0 ? ": " + first : std::string()) + ")");
		double lumas[4] = {};
		for (int k = 0; k < 4; ++k)
		{
			uint8_t *img = nullptr;
			outs[k]->Map(0, nullptr, reinterpret_cast<void **>(&img));
			lumas[k] = luma_at(img);
			outs[k]->Unmap(0, nullptr);
			outs[k]->Release();
		}
		std::snprintf(line, sizeof(line), "a lamp no object names, at (%.1f %.1f %.1f): the street under it per pixel %.1f -> %.1f; per vertex %.1f (first pass %.1f)",
			lamp.pos[0], lamp.pos[1], lamp.pos[2], lumas[0], lumas[1], lumas[2], first_luma);
		check(d < 1.0 && lumas[1] > lumas[0] + 5.0, line);
		check(std::fabs(lumas[2] - first_luma) < 0.5, "  per vertex, as the game lights: only the lamps an object names (none here)");
		check(lumas[3] > first_luma + 5.0, "  ... and named by every object, per vertex it lights them: " + std::to_string(lumas[3]));
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
	dump::unmap(base);
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
