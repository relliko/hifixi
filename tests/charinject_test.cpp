/*
 * The native character renderer on a list recorded as dgVoodoo records its own (src/chars/charinject.cpp,
 * chardraw.cpp), on WARP with the debug layer, as tests/zoneinject_test.cpp does for the zone: dgVoodoo's world target
 * and depth (typeless resources, UNORM and D24S8 views), its own root signature, heap, pipeline and vertex buffer bound
 * once; the depth plane in DEPTH_READ and the stencil plane in DEPTH_WRITE at the markers, as dgVoodoo leaves them.
 *
 * The Mithra NPC (ROM2/0/52.DAT) as the cache has it from the DAT, posed by research/ffxi-re/tools/chars.py (idl0, its
 * palette from tests/chars_pose.py), three times side by side in one frame: in the middle drawn by HiFi XI at the
 * characters' end marker (opaque), on the right see-through (colour alpha 0x40) at its own marker, on the left one the
 * game drew (not kNative: HiFi XI leaves it alone). Then dgVoodoo's next draw with nothing bound again.
 *   - the middle one in the target and its depth, inside its skinned vertices' projected box and filling it;
 *   - the see-through one blended about halfway between the sky and what the opaque one shows, its depth written;
 *   - the left one not drawn;
 *   - dgVoodoo's own draw as it meant it; the debug layer says nothing; the counts;
 *   - the zone's lamps (set_lamps): a lamp 1.5 yalms in front of the middle one lights it (against the sun alone), and
 *     its light then stands in for the game's own lamp slot (the lamp turned directional, left out): about as bright as
 *     the game's own, not both added; without the zone's lamps the game's slot as it is.
 *
 *   build\charinject_test.exe <pose.bin> [out.bmp]
 */

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include "../src/chars/cache.hpp"
#include "../src/chars/charframe.hpp"
#include "../src/chars/charinject.hpp"
#include "../src/chars/os2.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{
	namespace ch = hifi::chars;

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
	const float kSky[4] = { 0.55f, 0.65f, 0.78f, 1.0f };
	const float kShift = 1.1f; // (world x between the three)

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
		ch::inject::on_barrier(1, &b); // (as the hooks hand them over)
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
	// The game's camera: y down, looking from eye to at
	mat4 camera(const float eye[3], const float at[3])
	{
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
		const float ys = 1.0f / std::tan(0.4f), xs = ys / (float(W) / H), zn = 0.1f, zf = 500.0f;
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

	// ------------------------------------------------------------------------------------------ the Mithra from its DAT
	struct dat_chunk
	{
		uint32_t kind;
		size_t folder; // (the index of the folder chunk it's in)
		std::vector<uint8_t> data;
	};
	std::vector<dat_chunk> read_dat(const char *rel)
	{
		std::string path = std::string("C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\") + rel;
		std::ifstream f(path, std::ios::binary);
		std::vector<uint8_t> dat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::vector<dat_chunk> out;
		std::vector<size_t> folders;
		size_t off = 0;
		while (off + 16 <= dat.size())
		{
			uint32_t w;
			std::memcpy(&w, dat.data() + off + 4, 4);
			const size_t size = size_t((w >> 7) & 0x7FFFF) * 16;
			if (size < 16 || off + size > dat.size())
				break;
			dat_chunk c;
			c.kind = w & 0x7F;
			c.folder = folders.empty() ? 0 : folders.back();
			c.data.assign(dat.begin() + off + 16, dat.begin() + off + size);
			if (c.kind == 1)
				folders.push_back(out.size());
			else if (c.kind == 0 && !folders.empty())
				folders.pop_back();
			out.push_back(std::move(c));
			off += size;
		}
		return out;
	}

	struct loaded
	{
		std::vector<uint32_t> meshes;          // cache ids
		std::vector<ch::os2::mesh> decoded;    // (for the box)
		uint32_t textures = 0;
	};
	// Each mesh into the cache with the textures its commands name (the first of that name in its folder, from the
	// folder's start, as 03AF10 finds it)
	loaded load_into_cache(const std::vector<dat_chunk> &dat)
	{
		loaded L;
		std::map<size_t, uint32_t> texture_of_chunk;
		for (size_t i = 0; i < dat.size(); ++i)
		{
			if (dat[i].kind != 0x2A)
				continue;
			ch::os2::sections s;
			ch::os2::sections_of_chunk(dat[i].data.data(), dat[i].data.size(), s);
			std::vector<ch::os2::texture_ref> refs;
			ch::os2::texture_commands(dat[i].data.data(), dat[i].data.size(), s, refs);
			std::vector<uint32_t> ids;
			for (const auto &r : refs)
			{
				uint32_t id = ch::cache::kNone;
				int depth = 0;
				for (size_t j = dat[i].folder + 1; j < dat.size(); ++j)
				{
					if (dat[j].kind == 1)
						depth++;
					if (dat[j].kind == 0 && --depth < 0)
						break;
					if (dat[j].kind == 0x20 && dat[j].data.size() > 17 && std::memcmp(dat[j].data.data() + 1, r.name, 16) == 0)
					{
						auto known = texture_of_chunk.find(j);
						if (known == texture_of_chunk.end())
						{
							ch::cache::key k;
							k.resource = 0x70000000u + uint32_t(j) * 16;
							std::vector<uint8_t> bytes = dat[j].data;
							known = texture_of_chunk.emplace(j, ch::cache::add_texture(k, std::move(bytes))).first;
							L.textures++;
						}
						id = known->second;
						break;
					}
				}
				ids.push_back(id);
			}
			ch::cache::key k;
			k.resource = 0x60000000u + uint32_t(i) * 16;
			std::vector<uint8_t> bytes = dat[i].data;
			L.meshes.push_back(ch::cache::add_mesh(k, std::move(bytes), 0, std::move(ids)));
			ch::os2::mesh m;
			std::string why;
			ch::os2::decode(dat[i].data.data(), dat[i].data.size(), s, m, why);
			L.decoded.push_back(std::move(m));
		}
		return L;
	}

	// The skinned vertices' world box (both halves), shifted by dx
	void box_of(const loaded &L, const std::vector<ch::bone> &pal, float lo[3], float hi[3])
	{
		for (int k = 0; k < 3; ++k)
		{
			lo[k] = 1e30f;
			hi[k] = -1e30f;
		}
		auto apply = [&](uint32_t b, const float p[3], float w, float out[3]) {
			const float *m = pal[std::min<size_t>(b, pal.size() - 1)].m;
			for (int c = 0; c < 3; ++c)
				out[c] += p[0] * m[c] + p[1] * m[3 + c] + p[2] * m[6 + c] + w * m[9 + c];
		};
		for (const auto &m : L.decoded)
			for (int half = 0; half < (m.mirrored ? 2 : 1); ++half)
				for (const auto &v : m.vertices)
				{
					float p[3] = { 0, 0, 0 };
					float p0[3] = { v.p0[0], v.p0[1], v.p0[2] }, p1[3] = { v.p1[0], v.p1[1], v.p1[2] };
					if (half == 1)
					{
						if (v.axis[0] != 0)
							p0[v.axis[0] - 1] = -p0[v.axis[0] - 1];
						if (v.axis[1] != 0)
							p1[v.axis[1] - 1] = -p1[v.axis[1] - 1];
					}
					apply(half ? v.mirror_bone[0] : v.bone[0], p0, v.w0, p);
					apply(half ? v.mirror_bone[1] : v.bone[1], p1, v.w1, p);
					for (int k = 0; k < 3; ++k)
					{
						lo[k] = std::min(lo[k], p[k]);
						hi[k] = std::max(hi[k], p[k]);
					}
				}
	}

	// A world box's screen rectangle (all 8 corners projected)
	struct rect
	{
		float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
		bool inside(float x, float y, float margin) const { return x >= x0 - margin && x <= x1 + margin && y >= y0 - margin && y <= y1 + margin; }
	};
	rect project(const mat4 &vp, const float lo[3], const float hi[3], float dx)
	{
		rect r;
		for (int c = 0; c < 8; ++c)
		{
			const float p[4] = { ((c & 1) ? hi[0] : lo[0]) + dx, (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2], 1.0f };
			float o[4] = {};
			for (int j = 0; j < 4; ++j)
				for (int k = 0; k < 4; ++k)
					o[j] += p[k] * vp.m[k * 4 + j];
			const float sx = (o[0] / o[3] * 0.5f + 0.5f) * W, sy = (0.5f - o[1] / o[3] * 0.5f) * H;
			r.x0 = std::min(r.x0, sx);
			r.x1 = std::max(r.x1, sx);
			r.y0 = std::min(r.y0, sy);
			r.y1 = std::max(r.y1, sy);
		}
		return r;
	}
}

// A crash says where (code, address, module and offset) before the process goes, and what the debug layer said
ID3D12InfoQueue *g_info = nullptr;
LONG WINAPI report_crash(EXCEPTION_POINTERS *e)
{
	if (g_info != nullptr)
		for (UINT64 i = 0, n = g_info->GetNumStoredMessages(); i < n; ++i)
		{
			SIZE_T size = 0;
			g_info->GetMessage(i, nullptr, &size);
			std::vector<uint8_t> buf(size);
			D3D12_MESSAGE *m = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
			g_info->GetMessage(i, m, &size);
			if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
				std::printf("DEBUG LAYER #%d: %s\n", int(m->ID), m->pDescription);
		}
	const void *at = e->ExceptionRecord->ExceptionAddress;
	HMODULE m = nullptr;
	char name[MAX_PATH] = "?";
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(at), &m))
		GetModuleFileNameA(m, name, sizeof(name));
	std::printf("CRASH: exception %08lX at %p (%s +%lX)\n", e->ExceptionRecord->ExceptionCode, at, name,
		static_cast<unsigned long>(reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(m)));
	return EXCEPTION_EXECUTE_HANDLER;
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }
namespace hifi::backend
{
	void log(int, const char *) {} // (src/chars/chartest.cpp's lines; the backend isn't linked here)
}

int main(int argc, char **argv)
{
	namespace inj = ch::inject;
	std::setvbuf(stdout, nullptr, _IONBF, 0); // (each line out as it's written: a crash keeps what came before it)
	SetUnhandledExceptionFilter(report_crash);
	if (argc < 2)
	{
		std::printf("usage: charinject_test <pose.bin> [out.bmp]\n");
		return 2;
	}
	// The pose (chars.py's palette: 4x4 row vectors, translation in the last row)
	std::vector<ch::bone> pose;
	{
		std::ifstream f(argv[1], std::ios::binary);
		uint32_t n = 0;
		f.read(reinterpret_cast<char *>(&n), 4);
		for (uint32_t i = 0; i < n && f; ++i)
		{
			float m[16];
			f.read(reinterpret_cast<char *>(m), 64);
			ch::bone b;
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 3; ++c)
					b.m[r * 3 + c] = m[r * 4 + c];
			pose.push_back(b);
		}
	}
	if (getenv("CHARS_GROUPS") != nullptr)
	{
		const std::vector<dat_chunk> d0 = read_dat("ROM2\\0\\52.DAT");
		for (const auto &c : d0)
			if (c.kind == 0x2A)
			{
				ch::os2::sections s0;
				ch::os2::sections_of_chunk(c.data.data(), c.data.size(), s0);
				ch::os2::mesh m0;
				std::string w0;
				ch::os2::decode(c.data.data(), c.data.size(), s0, m0, w0);
				uint32_t mg = 0;
				for (const auto &g : m0.groups)
					mg += g.mirrored;
				std::printf("      mesh: mirrored %d, %zu groups (%u mirrored), %zu corners\n", int(m0.mirrored), m0.groups.size(), mg, m0.corners.size());
			}
	}
	check(pose.size() == 108, "the pose: " + std::to_string(pose.size()) + " bones (chars.py, idl0)");

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
	g_info = info;
	ID3D12CommandQueue *direct = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	device->CreateCommandQueue(&qd, IID_PPV_ARGS(&direct));

	// The Mithra into the cache, on the GPU
	const std::vector<dat_chunk> dat = read_dat("ROM2\\0\\52.DAT");
	const loaded L = load_into_cache(dat);
	ch::cache::frame_tick(device, 1.0);
	bool settled = ch::cache::settle(60000);
	for (int i = 0; i < 400 && ch::cache::get_counts().meshes_ready < L.meshes.size(); ++i)
		Sleep(50);
	settled = settled && ch::cache::settle(60000);
	bool all_ready = true;
	for (uint32_t id : L.meshes)
		all_ready = all_ready && ch::cache::mesh_ready(id);
	check(settled && all_ready, "the Mithra's " + std::to_string(L.meshes.size()) + " meshes and " + std::to_string(L.textures) + " textures on the GPU: " +
		ch::cache::status()[0]);
	std::string why;
	check(inj::prepare(device, why), "the renderer made" + (why.empty() ? "" : ": " + why));
	inj::set_options(getenv("CHARS_PERPIXEL") != nullptr, getenv("CHARS_DETAIL") != nullptr); // (the per-pixel option: CHARS_PERPIXEL=1; the detail maps: CHARS_DETAIL=1)
	// (with it the zone's sun, as the plugin passes it: the models' light 0 below is that sun, for its highlight)
	const float sun_l = std::sqrt(0.31f * 0.31f + 0.951f * 0.951f), toward_sun[3] = { -0.31f / sun_l, -0.951f / sun_l, 0.0f };
	inj::set_sun(getenv("CHARS_PERPIXEL") != nullptr ? toward_sun : nullptr);

	// The camera on the three of them
	float lo[3], hi[3];
	box_of(L, pose, lo, hi);
	const float centre[3] = { (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2 };
	// (CHARS_YAW: another side, for a look)
	const float yaw = getenv("CHARS_YAW") != nullptr ? float(atof(getenv("CHARS_YAW"))) * 3.14159265f / 180.0f : 0.0f;
	const float eye[3] = { centre[0] - 4.2f * std::sin(yaw), centre[1] - 0.3f, centre[2] - 4.2f * std::cos(yaw) };
	mat4 vp = camera(eye, centre);
	if (getenv("CHARS_ORTHO") != nullptr)
	{
		// (chars.py's view for a side-by-side look: orthographic along +z, fitted to 90% of the height, y down)
		const float scale = 0.9f / std::max(hi[0] - lo[0], hi[1] - lo[1]) * 2.0f;
		const mat4 o = { { scale * float(H) / W, 0, 0, 0, 0, -scale, 0, 0, 0, 0, 0.001f, 0, -centre[0] * scale * float(H) / W, centre[1] * scale, 0.5f - centre[2] * 0.001f, 1 } };
		vp = o;
	}
	const rect middle = project(vp, lo, hi, 0.0f), right = project(vp, lo, hi, kShift), left = project(vp, lo, hi, -kShift),
			   fogged = project(vp, lo, hi, 2 * kShift);
	if (getenv("CHARS_GROUPS") != nullptr)
	{
		// (where each half's skinned vertices land on screen)
		for (int half = 0; half < 2; ++half)
		{
			float x0 = 1e9f, x1 = -1e9f;
			for (const auto &m : L.decoded)
				for (const auto &v : m.vertices)
				{
					float p[3] = { 0, 0, 0 };
					float p0[3] = { v.p0[0], v.p0[1], v.p0[2] }, p1[3] = { v.p1[0], v.p1[1], v.p1[2] };
					if (half == 1)
					{
						if (v.axis[0] != 0)
							p0[v.axis[0] - 1] = -p0[v.axis[0] - 1];
						if (v.axis[1] != 0)
							p1[v.axis[1] - 1] = -p1[v.axis[1] - 1];
					}
					const uint32_t b0 = half ? v.mirror_bone[0] : v.bone[0], b1 = half ? v.mirror_bone[1] : v.bone[1];
					for (int c = 0; c < 3; ++c)
						p[c] = p0[0] * pose[b0].m[c] + p0[1] * pose[b0].m[3 + c] + p0[2] * pose[b0].m[6 + c] + v.w0 * pose[b0].m[9 + c] + p1[0] * pose[b1].m[c] +
							p1[1] * pose[b1].m[3 + c] + p1[2] * pose[b1].m[6 + c] + v.w1 * pose[b1].m[9 + c];
					float o[4] = {};
					const float q[4] = { p[0], p[1], p[2], 1 };
					for (int j = 0; j < 4; ++j)
						for (int k = 0; k < 4; ++k)
							o[j] += q[k] * vp.m[k * 4 + j];
					const float sx = (o[0] / o[3] * 0.5f + 0.5f) * W;
					x0 = std::min(x0, sx);
					x1 = std::max(x1, sx);
				}
			std::printf("      half %d on screen x %.0f..%.0f\n", half, x0, x1);
		}
		uint32_t axes[4] = {};
		for (const auto &m : L.decoded)
			for (const auto &v : m.vertices)
				axes[v.axis[0] & 3]++;
		std::printf("      mirror axes: none %u, x %u, y %u, z %u\n", axes[0], axes[1], axes[2], axes[3]);
	}
	std::printf("      the Mithra's box %.2f..%.2f, %.2f..%.2f, %.2f..%.2f; on screen x %.0f..%.0f y %.0f..%.0f\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], middle.x0, middle.x1,
		middle.y0, middle.y1);

	// The frame, as the game side publishes it
	const uint32_t seq = 21;
	ch::frame &f = ch::begin_frame(seq);
	std::memcpy(f.view_proj, vp.m, 64);
	f.camera = true;
	auto add_model = [&](float dx, uint32_t colour, uint32_t flags) {
		ch::model_entry &m = f.model[f.models];
		m = ch::model_entry();
		m.actor = 0xAC700000u + f.models;
		m.colour = colour;
		m.flags = flags;
		ch::light8 sun = {};
		sun.type = 3;
		sun.diffuse[0] = 1.48f;
		sun.diffuse[1] = 1.47f;
		sun.diffuse[2] = 1.42f;
		sun.direction[0] = 0.31f;
		sun.direction[1] = 0.951f;
		m.lights[0] = sun;
		m.ambient = 0xFF606060u;
		m.tfactor = 0x80808080u;
		m.first_instance = f.instances;
		m.instances = 1;
		ch::instance_entry &ie = f.instance[f.instances++];
		ie.first_bone = f.bones;
		ie.bones = uint32_t(pose.size());
		for (size_t b = 0; b < pose.size(); ++b)
		{
			f.palette[f.bones + b] = pose[b];
			f.palette[f.bones + b].m[9] += dx;
		}
		f.bones += ie.bones;
		ie.first_mesh = f.meshes;
		ie.meshes = uint32_t(L.meshes.size());
		uint32_t hidden = 0;
		for (size_t k = 0; k < L.meshes.size(); ++k)
		{
			f.mesh[f.meshes + k].mesh = L.meshes[k];
			hidden |= ch::os2::hidden_by(ch::cache::mesh_hides(L.meshes[k]));
		}
		m.hidden = getenv("CHARS_NOHIDE") != nullptr ? 0 : hidden;
		f.meshes += ie.meshes;
		f.models++;
	};
	add_model(0.0f, 0x80808080u, ch::kNative);   // the middle: HiFi XI's
	add_model(-kShift, 0x80808080u, 0);          // the left: the game's
	add_model(2 * kShift, 0x80808080u, ch::kNative); // the far right: HiFi XI's, in its own fog (all of it: red)
	{
		ch::model_entry &m = f.model[f.models - 1];
		m.fog_read = true;
		m.fog_colour = 0xFFFF0000u;
		m.fog_start = 0.0f;
		m.fog_end = 0.5f;
	}
	ch::publish_opaque(f);
	add_model(kShift, 0x40808080u, ch::kNative | ch::kFromFlush); // the right: see-through, from the flush
	ch::publish_see_through(f);
	ch::cache::publish_palettes(f, 0, f.bones);

	// dgVoodoo's world: typeless resources, UNORM / D24S8 views; CHARS_MSAA=n multisampled (its depth then isn't read back:
	// what's drawn stands for it)
	const UINT samples = getenv("CHARS_MSAA") != nullptr ? UINT(atoi(getenv("CHARS_MSAA"))) : 1u;
	if (samples > 1)
		std::printf("      the world multisampled: %u samples\n", samples);
	D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, up = { D3D12_HEAP_TYPE_UPLOAD }, rb = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = W;
	td.Height = H;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.SampleDesc.Count = samples;
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
	rvd.ViewDimension = samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(A, &rvd, rtv);
	D3D12_DEPTH_STENCIL_VIEW_DESC dvd = {};
	dvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dvd.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
	dvd.Flags = D3D12_DSV_FLAG_READ_ONLY_STENCIL;
	device->CreateDepthStencilView(Az, &dvd, dsv);
	ID3D12DescriptorHeap *dsvh2 = nullptr;
	hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvh2));
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv_clear = dsvh2->GetCPUDescriptorHandleForHeapStart();
	dvd.Flags = D3D12_DSV_FLAG_NONE;
	device->CreateDepthStencilView(Az, &dvd, dsv_clear);

	// dgVoodoo's own draw: a textured quad (a 2x2 of one colour) in the lower right corner
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
		const uint32_t px = 0xFF2080E0u;
		for (int row = 0; row < 2; ++row)
			for (int x = 0; x < 2; ++x)
				std::memcpy(p + row * 256 + x * 4, &px, 4);
		tex_up->Unmap(0, nullptr);
		float *v = nullptr;
		vb->Map(0, nullptr, reinterpret_cast<void **>(&v));
		const float quad[] = { 0.8f, -0.8f, 0, 0, 0.98f, -0.8f, 1, 0, 0.8f, -0.98f, 0, 1, 0.98f, -0.98f, 1, 1 };
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
	rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
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
	pd.SampleDesc.Count = samples;
	pd.RasterizerState.MultisampleEnable = samples > 1;
	pd.SampleMask = UINT_MAX;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.DepthStencilState.DepthEnable = TRUE;
	pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	ID3D12PipelineState *game_pso = nullptr;
	check(gvs != nullptr && gps != nullptr && SUCCEEDED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&game_pso))), "dgVoodoo's stand-in pipeline");

	// The list, as dgVoodoo records a frame round the characters' markers
	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *list = nullptr;
	device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
	device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
	inj::on_reset(list);
	{
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = tex;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.pResource = tex_up;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 256 };
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		barrier(list, tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	inj::on_marker(Az);
	list->ClearRenderTargetView(rtv, kSky, 0, nullptr);
	list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
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
	inj::world w;
	w.rt = A;
	w.ds = Az;
	w.rt_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	w.ds_format = DXGI_FORMAT_D24_UNORM_S8_UINT;

	// The characters' end marker: the planes as dgVoodoo leaves them (depth read, stencil written for the clear)
	barrier(list, Az, 0, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	inj::set_motion_wanted(samples == 1); // (the characters' motion for FSR, as while it draws: chardraw.hpp "Motion")
	inj::on_chars_end(list, bound, w, seq & 15);
	const D3D12_RECT end_pixel = { 48 + 5, 0, 48 + 6, 1 };
	list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 1, &end_pixel);
	inj::on_chars_end(list, bound, w, seq & 15); // (the same seq again: nothing)
	// A draw of dgVoodoo's in between, writing depth (the stencil plane back to read; then to write for the marker)
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
	barrier(list, Az, 0, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	inj::on_see_through(list, bound, w, seq & 15);
	const D3D12_RECT st_pixel = { 64 + 5, 0, 64 + 6, 1 };
	list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 1, &st_pixel);
	inj::on_see_through(list, bound, w, seq & 15); // (a second see-through marker: there's no second model)
	// World FX's casters' pass (its shading pass calls record_casters): the sun straight overhead, a D32 map
	// kCasterSize square looking down on all four (x across, z down the map, depth by height: the top of the box
	// nearest), then the game's state put back as World FX does after its pass
	constexpr UINT kCasterSize = 2048;
	const float c_reach = 0.5f * ((hi[0] + 2 * kShift) - (lo[0] - kShift)) + 0.5f, c_x = 0.5f * ((hi[0] + 2 * kShift) + (lo[0] - kShift)), c_z = 0.5f * (lo[2] + hi[2]);
	const float c_top = lo[1] - 1.0f, c_span = (hi[1] - lo[1]) + 2.0f;
	const float caster_m[16] = { 1.0f / c_reach, 0, 0, 0, 0, 0, 1.0f / c_span, 0, 0, -1.0f / c_reach, 0, 0, -c_x / c_reach, c_z / c_reach, -c_top / c_span, 1 };
	ID3D12Resource *caster_map = nullptr, *caster_back = nullptr;
	ID3D12DescriptorHeap *caster_heap = nullptr;
	{
		D3D12_RESOURCE_DESC cd = {};
		cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		cd.Width = kCasterSize;
		cd.Height = kCasterSize;
		cd.DepthOrArraySize = 1;
		cd.MipLevels = 1;
		cd.Format = DXGI_FORMAT_D32_FLOAT;
		cd.SampleDesc.Count = 1;
		cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		D3D12_CLEAR_VALUE cv = {};
		cv.Format = DXGI_FORMAT_D32_FLOAT;
		cv.DepthStencil.Depth = 1.0f;
		device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&caster_map));
		D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
		device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&caster_heap));
		device->CreateDepthStencilView(caster_map, nullptr, caster_heap->GetCPUDescriptorHandleForHeapStart());
		D3D12_RESOURCE_DESC cb = bd;
		cb.Width = UINT64(kCasterSize) * kCasterSize * 4;
		device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &cb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&caster_back));
	}
	const D3D12_CPU_DESCRIPTOR_HANDLE caster_dsv = caster_heap->GetCPUDescriptorHandleForHeapStart();
	list->ClearDepthStencilView(caster_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	const bool casters_drawn = inj::record_casters(list, caster_m, caster_dsv, kCasterSize);
	bound.restore(list);
	barrier(list, caster_map, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	{
		D3D12_TEXTURE_COPY_LOCATION cdst = {}, csrc = {};
		cdst.pResource = caster_back;
		cdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		cdst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_TYPELESS, kCasterSize, kCasterSize, 1, kCasterSize * 4 };
		csrc.pResource = caster_map;
		csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		list->CopyTextureRegion(&cdst, 0, 0, 0, &csrc, nullptr);
	}
	// dgVoodoo's next draw, nothing bound again
	barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
	list->DrawInstanced(4, 1, 0, 0);
	// The picture and depth out
	ID3D12Resource *picture = A;
	if (samples > 1)
	{
		D3D12_RESOURCE_DESC rd = A->GetDesc();
		rd.SampleDesc.Count = 1;
		rd.Alignment = 0; // (a multisampled resource's 4 MB alignment isn't a plain one's)
		rd.Flags = D3D12_RESOURCE_FLAG_NONE;
		device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&picture));
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		list->ResolveSubresource(picture, 0, A, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
		barrier(list, picture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
	}
	else
	{
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier(list, Az, 1, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
	}
	ID3D12Resource *readback = nullptr, *depth_back = nullptr;
	bd.Width = UINT64(W) * 4 * H;
	device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
	device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&depth_back));
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	dst.pResource = readback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_TYPELESS, W, H, 1, W * 4 };
	src.pResource = picture;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	if (samples == 1)
	{
		dst.pResource = depth_back;
		dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_TYPELESS, W, H, 1, W * 4 };
		src.pResource = Az;
		src.SubresourceIndex = 0;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}
	// (the characters' motion, read back: R16G16B16A16_FLOAT)
	ID3D12Resource *motion_tex = nullptr, *motion_back = nullptr;
	uint64_t motion_frames = 0;
	const bool have_motion = inj::motion(motion_tex, motion_frames);
	if (have_motion)
	{
		bd.Width = UINT64(W) * 8 * H;
		device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&motion_back));
		barrier(list, motion_tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_COPY_SOURCE);
		dst.pResource = motion_back;
		dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 1, W * 8 };
		src.pResource = motion_tex;
		src.SubresourceIndex = 0;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		barrier(list, motion_tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
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
	const int sky8[3] = { int(kSky[0] * 255 + 0.5f), int(kSky[1] * 255 + 0.5f), int(kSky[2] * 255 + 0.5f) };
	auto is_sky = [&](const uint8_t *p) { return std::abs(p[0] - sky8[0]) <= 2 && std::abs(p[1] - sky8[1]) <= 2 && std::abs(p[2] - sky8[2]) <= 2; };
	uint32_t in_middle = 0, in_right = 0, in_left = 0, outside = 0, depth_middle = 0, depth_right = 0, in_fogged = 0, fog_red = 0;
	double off_sky_middle = 0, off_sky_right = 0;
	for (UINT y = 0; y < H; ++y)
		for (UINT x = 0; x < W; ++x)
		{
			const uint8_t *p = px + (size_t(y) * W + x) * 4;
			const bool nearer = samples == 1 ? (dz[size_t(y) * W + x] & 0xFFFFFF) < 0xFFFFFF : !is_sky(px + (size_t(y) * W + x) * 4);
			const float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
			const bool quad = x >= 720 && y >= 405; // (dgVoodoo's own quad)
			if (quad)
				continue;
			const double off = std::abs(p[0] - sky8[0]) + std::abs(p[1] - sky8[1]) + std::abs(p[2] - sky8[2]);
			if (middle.inside(cx, cy, 2))
			{
				if (!is_sky(p))
				{
					in_middle++;
					off_sky_middle += off;
				}
				depth_middle += nearer;
			}
			else if (right.inside(cx, cy, 2))
			{
				if (!is_sky(p))
				{
					in_right++;
					off_sky_right += off;
				}
				depth_right += nearer;
			}
			else if (left.inside(cx, cy, 2))
				in_left += !is_sky(p) || nearer;
			else if (fogged.inside(cx, cy, 2))
			{
				if (!is_sky(p))
				{
					in_fogged++;
					fog_red += p[0] >= 250 && p[1] <= 5 && p[2] <= 5;
				}
			}
			else
				outside += !is_sky(p) || nearer;
		}
	const double box = (middle.x1 - middle.x0) * (middle.y1 - middle.y0);
	char line[300];
	std::snprintf(line, sizeof(line), "the middle Mithra drawn at the end marker: %u pixels (%.0f%% of its box), its depth %u, nothing outside the boxes (%u)", in_middle,
		100.0 * in_middle / box, depth_middle, outside);
	check(in_middle > box * 0.15 && depth_middle >= in_middle * 9 / 10 && outside == 0, line);
	// Its motion (for FSR): on each of its pixels its view depth and no move of its own (no last frame: this is the first);
	// nothing elsewhere but the other opaque one HiFi XI draws
	if (samples == 1)
	{
		uint32_t on_middle = 0, still_middle = 0, sane_depth = 0, stray = 0;
		const uint16_t *mo = nullptr;
		if (motion_back != nullptr)
			motion_back->Map(0, nullptr, reinterpret_cast<void **>(const_cast<uint16_t **>(&mo)));
		for (UINT y = 0; y < H && mo != nullptr; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const uint16_t *q = mo + (size_t(y) * W + x) * 4;
				const float mx = DirectX::PackedVector::XMConvertHalfToFloat(q[0]), my = DirectX::PackedVector::XMConvertHalfToFloat(q[1]),
					mz = DirectX::PackedVector::XMConvertHalfToFloat(q[2]), mw = DirectX::PackedVector::XMConvertHalfToFloat(q[3]);
				const float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
				if (middle.inside(cx, cy, 2) && !is_sky(px + (size_t(y) * W + x) * 4) && mw > 0.0f)
				{
					on_middle++;
					still_middle += mx == 0.0f && my == 0.0f && mz == 0.0f ? 1 : 0;
					sane_depth += mw > 0.05f && mw < 100.0f ? 1 : 0;
				}
				stray += !middle.inside(cx, cy, 2) && !fogged.inside(cx, cy, 2) && mw != 0.0f ? 1 : 0;
			}
		check(have_motion && motion_frames == 1 && on_middle >= in_middle * 9 / 10 && still_middle == on_middle && sane_depth == on_middle && stray == 0,
			"its motion for FSR: " + std::to_string(on_middle) + " of its " + std::to_string(in_middle) + " pixels with its depth (" + std::to_string(sane_depth) +
			" sane), none moved on a first frame (" + std::to_string(still_middle) + "), nothing outside HiFi XI's opaque ones (" + std::to_string(stray) + ")");
	}
	const double mean_middle = in_middle ? off_sky_middle / in_middle : 0, mean_right = in_right ? off_sky_right / in_right : 0;
	std::snprintf(line, sizeof(line), "the see-through one at its marker: %u pixels, %.0f%% as far from the sky as the opaque one's (about half: alpha 0x40), its depth written %u",
		in_right, mean_middle > 0 ? 100.0 * mean_right / mean_middle : 0.0, depth_right);
	check(in_right > box * 0.1 && mean_right > 0.3 * mean_middle && mean_right < 0.7 * mean_middle && depth_right >= in_right * 9 / 10, line);
	check(in_left == 0, "the game's own (not kNative) left alone: " + std::to_string(in_left) + " pixels");
	check(in_fogged > box * 0.1 && (samples == 1 ? fog_red == in_fogged : fog_red >= in_fogged * 8 / 10), "one in its own fog (the actor's, red, all of it): " + std::to_string(fog_red) + " of " + std::to_string(in_fogged) +
		" pixels the fog's colour");
	// Every skinned vertex of the middle one (both halves) on a drawn pixel (or one next to it): what a missing or
	// misplaced half, bone or mesh would show
	{
		uint32_t total = 0, on = 0, half_on[2] = {}, half_total[2] = {};
		for (const auto &m : L.decoded)
			for (int half = 0; half < (m.mirrored ? 2 : 1); ++half)
				for (const auto &v : m.vertices)
				{
					float p0[3] = { v.p0[0], v.p0[1], v.p0[2] }, p1[3] = { v.p1[0], v.p1[1], v.p1[2] };
					if (half == 1)
					{
						if (v.axis[0] != 0)
							p0[v.axis[0] - 1] = -p0[v.axis[0] - 1];
						if (v.axis[1] != 0)
							p1[v.axis[1] - 1] = -p1[v.axis[1] - 1];
					}
					const uint32_t b0 = std::min<uint32_t>(half ? v.mirror_bone[0] : v.bone[0], uint32_t(pose.size() - 1));
					const uint32_t b1 = std::min<uint32_t>(half ? v.mirror_bone[1] : v.bone[1], uint32_t(pose.size() - 1));
					float q[4] = { 0, 0, 0, 1 };
					for (int c = 0; c < 3; ++c)
						q[c] = p0[0] * pose[b0].m[c] + p0[1] * pose[b0].m[3 + c] + p0[2] * pose[b0].m[6 + c] + v.w0 * pose[b0].m[9 + c] + p1[0] * pose[b1].m[c] +
							p1[1] * pose[b1].m[3 + c] + p1[2] * pose[b1].m[6 + c] + v.w1 * pose[b1].m[9 + c];
					float o[4] = {};
					for (int j = 0; j < 4; ++j)
						for (int k = 0; k < 4; ++k)
							o[j] += q[k] * vp.m[k * 4 + j];
					const int sx = int((o[0] / o[3] * 0.5f + 0.5f) * W), sy = int((0.5f - o[1] / o[3] * 0.5f) * H);
					bool drawn = false;
					for (int dy = -1; dy <= 1 && !drawn; ++dy)
						for (int dx = -1; dx <= 1 && !drawn; ++dx)
						{
							const int x = sx + dx, y = sy + dy;
							if (x >= 0 && y >= 0 && x < int(W) && y < int(H))
								drawn = !is_sky(px + (size_t(y) * W + x) * 4);
						}
					total++;
					on += drawn;
					half_total[half]++;
					half_on[half] += drawn;
				}
		std::snprintf(line, sizeof(line), "the middle one's skinned vertices on drawn pixels: %u of %u (%.0f%%; this half %.0f%%, the mirrored half %.0f%%)", on, total,
			100.0 * on / std::max(total, 1u), 100.0 * half_on[0] / std::max(half_total[0], 1u), 100.0 * half_on[1] / std::max(half_total[1], 1u));
		check(on >= total * 9 / 10 && half_on[0] >= half_total[0] * 9 / 10 && half_on[1] >= half_total[1] * 9 / 10, line);
	}
	const uint8_t *c = px + (size_t(430) * W + 760) * 4;
	std::snprintf(line, sizeof(line), "dgVoodoo's next draw as it meant it: its texture's colour (%u %u %u)", c[0], c[1], c[2]);
	check(c[0] == 0xE0 && c[1] == 0x80 && c[2] == 0x20, line);
	const inj::counts n = inj::get_counts();
	check(n.frames == 1 && n.models == 2 && n.see_through == 1 && n.failed == 0, "drawn: 1 frame, 2 opaque models, 1 see-through: " + inj::status()[0]);
	// The casters' map: each of HiFi XI's three (the middle, the see-through one, the fogged one) seen from above in its
	// own x band, the game's own (left) not; the nearest texel the top of the box (the head), within a texel or two
	{
		float *cz = nullptr;
		caster_back->Map(0, nullptr, reinterpret_cast<void **>(&cz));
		uint32_t band[4] = {};
		float nearest = 1.0f;
		const float dxs[4] = { 0.0f, -kShift, kShift, 2 * kShift }; // (middle, left, see-through, fogged)
		for (UINT y = 0; y < kCasterSize; ++y)
			for (UINT x = 0; x < kCasterSize; ++x)
			{
				const float d = cz[size_t(y) * kCasterSize + x];
				if (!(d < 1.0f))
					continue;
				nearest = std::min(nearest, d);
				const float wx = c_x + ((float(x) + 0.5f) / kCasterSize * 2.0f - 1.0f) * c_reach;
				for (int k = 0; k < 4; ++k)
					if (wx >= lo[0] + dxs[k] - 0.05f && wx <= hi[0] + dxs[k] + 0.05f)
						band[k]++;
			}
		const float head = (lo[1] - c_top) / c_span, texel_depth = 2.0f * c_reach / kCasterSize / c_span;
		std::snprintf(line, sizeof(line), "the casters' map from above: %u, %u, %u texels under HiFi XI's three, %u under the game's own; nearest %.4f (the box's top %.4f)",
			band[0], band[2], band[3], band[1], nearest, head);
		check(casters_drawn && band[0] > 2000 && band[2] > 2000 && band[3] > 2000 && band[1] == 0 && std::fabs(nearest - head) < 3.0f * texel_depth + 0.002f, line);
		const inj::counts nc = inj::get_counts();
		check(nc.caster_frames == 1 && nc.caster_models == 3, "counted: 1 casters' frame, 3 models (" + std::to_string(nc.caster_models) + ")");
		caster_back->Unmap(0, nullptr);
	}
	// The zone's lamps (chardraw.hpp "Lamps"): the middle one alone again (seqs 22-24), a lamp 1.5 yalms in front of it
	// toward the camera at its middle's height. 22: the game's own lamp slot (the lamp turned directional from the actor's
	// root, its colour times its weight there), no zone lamps; 23: the same slot and the zone's lamp handed over; 24: the
	// sun alone. Its pixels' mean brightness: 23 above 24, and within half again of 22 (the slot left out, not added to).
	if (samples == 1)
	{
		const ch::model_entry &mm = f.model[0];
		const ch::instance_entry &mi = f.instance[mm.first_instance];
		const float *root = f.palette[mi.first_bone].m + 9;
		float way[3] = { eye[0] - centre[0], 0.0f, eye[2] - centre[2] };
		const float wl = std::sqrt(way[0] * way[0] + way[2] * way[2]);
		const float lamp_at[3] = { centre[0] + 1.5f * way[0] / wl, centre[1], centre[2] + 1.5f * way[2] / wl };
		const float lamp_colour[3] = { 0.6f, 0.5f, 0.3f };
		const float to_actor[3] = { root[0] - lamp_at[0], root[1] - lamp_at[1], root[2] - lamp_at[2] };
		const float d2 = to_actor[0] * to_actor[0] + to_actor[1] * to_actor[1] + to_actor[2] * to_actor[2];
		const float weight = 1.0f / std::max(d2 * 0.5f, 1e-4f);
		auto lamp_frame = [&](uint32_t s, bool game_lamp) {
			ch::frame &lf = ch::begin_frame(s);
			std::memcpy(lf.view_proj, vp.m, 64);
			lf.camera = true;
			lf.model[0] = mm;
			lf.model[0].first_instance = 0;
			lf.model[0].actor = 0xAC710000u;
			if (game_lamp)
			{
				ch::light8 l = {};
				l.type = 3;
				for (int k = 0; k < 3; ++k)
				{
					l.diffuse[k] = lamp_colour[k] * weight;
					l.direction[k] = to_actor[k] / std::sqrt(d2);
				}
				lf.model[0].lights[1] = l;
			}
			lf.instance[0] = mi;
			lf.instance[0].first_bone = 0;
			lf.instance[0].first_mesh = 0;
			for (uint32_t b = 0; b < mi.bones; ++b)
				lf.palette[b] = f.palette[mi.first_bone + b];
			for (uint32_t k = 0; k < mi.meshes; ++k)
				lf.mesh[k] = f.mesh[mi.first_mesh + k];
			lf.models = 1;
			lf.instances = 1;
			lf.bones = mi.bones;
			lf.meshes = mi.meshes;
			ch::publish_opaque(lf);
			ch::publish_see_through(lf);
			ch::cache::publish_palettes(lf, 0, lf.bones);
		};
		const float sun_toward[3] = { -0.31f / 1.00025f, -0.951f / 1.00025f, 0.0f };
		inj::set_sun(sun_toward);
		const float zone_lamp[8] = { lamp_at[0], lamp_at[1], lamp_at[2], 6.0f, lamp_colour[0], lamp_colour[1], lamp_colour[2], 0.5f };
		alloc->Reset();
		list->Reset(alloc, nullptr);
		inj::on_reset(list);
		list->SetDescriptorHeaps(1, &srvh);
		inj::on_heaps(list, 1, &srvh);
		barrier(list, A, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		barrier(list, Az, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		ID3D12Resource *outs[3] = {};
		bd.Width = UINT64(W) * 4 * H;
		for (int k = 0; k < 3; ++k)
		{
			device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outs[k]));
			const uint32_t s = 22 + k;
			lamp_frame(s, k < 2);
			inj::set_lamps(k == 1 ? zone_lamp : nullptr, k == 1 ? 1 : 0);
			list->ClearRenderTargetView(rtv, kSky, 0, nullptr);
			list->ClearDepthStencilView(dsv_clear, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			inj::on_chars_end(list, bound, w, s & 15);
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
		inj::set_lamps(nullptr, 0);
		inj::set_sun(nullptr);
		list->Close();
		direct->ExecuteCommandLists(1, lists);
		direct->Signal(fence, 2);
		fence->SetEventOnCompletion(2, e);
		WaitForSingleObject(e, 60000);
		check(device->GetDeviceRemovedReason() == S_OK, "the device still there (the lamps' frames)");
		const uint32_t lamp_messages = debug_messages(info, first);
		check(lamp_messages == 0, "the debug layer: nothing (" + std::to_string(lamp_messages) + (lamp_messages != 0 ? ": " + first : std::string()) + ")");
		double mean[3] = {};
		uint32_t count[3] = {};
		for (int k = 0; k < 3; ++k)
		{
			uint8_t *img = nullptr;
			outs[k]->Map(0, nullptr, reinterpret_cast<void **>(&img));
			for (UINT y = 0; y < H; ++y)
				for (UINT x = 0; x < W; ++x)
				{
					const uint8_t *p = img + (size_t(y) * W + x) * 4;
					if (middle.inside(float(x) + 0.5f, float(y) + 0.5f, 2) && !is_sky(p))
					{
						mean[k] += 0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2];
						count[k]++;
					}
				}
			mean[k] /= std::max(count[k], 1u);
			outs[k]->Unmap(0, nullptr);
			outs[k]->Release();
		}
		std::snprintf(line, sizeof(line), "a lamp before the middle one (%u, %u, %u pixels): the game's lamp slot %.1f, the zone's lamp in its stead %.1f, the sun alone %.1f",
			count[0], count[1], count[2], mean[0], mean[1], mean[2]);
		check(count[1] > 1000 && mean[1] > mean[2] + 3.0, line);
		check(mean[1] - mean[2] < 1.5 * (mean[0] - mean[2]) && mean[1] - mean[2] > 0.4 * (mean[0] - mean[2]),
			"  the zone's lamp stands in for the game's lamp slot (about as bright: the slot left out, not added to)");
	}
	if (argc > 2)
	{
		FILE *fo = nullptr;
		if (fopen_s(&fo, argv[2], "wb") == 0 && fo != nullptr)
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
			fwrite(head, 1, 54, fo);
			for (size_t i = 0; i < size_t(W) * H; ++i)
			{
				const uint8_t bgra[4] = { px[i * 4 + 2], px[i * 4 + 1], px[i * 4 + 0], 255 };
				fwrite(bgra, 1, 4, fo);
			}
			fclose(fo);
		}
	}
	inj::release();
	ch::cache::stop();
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
