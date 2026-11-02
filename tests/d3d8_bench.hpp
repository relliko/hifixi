#pragma once
// d3d8_frame.cpp's bench: what hifps's batch and dedupe save the game's thread on our d3d8.dll (docs/frame-rate.md).
//
//   d3d8_frame.exe <d3d8.dll> bench <capture> [all|today] [rounds]
//
// A d3dlog capture's frame (the replay's reading of it, d3d8_replay.hpp) turned once into a list of ready device calls,
// then played again and again, each frame timed on the calling thread by QueryThreadCycleTime: the CPU the calls take,
// waits on the GPU left out. Played several ways, in rounds (each way a few frames a round, so drift hits them alike):
//   direct        our d3d8.dll as it is
//   proxy         hifps's proxy.asm in front of it as HiFi XI laid it on the client device (the addon's own
//                 proxy\proxy.blob, read from C:\Games\PhoenixXI\addons\hifps), batch and dedupe off: what the proxy costs
//   proxy dedupe, proxy batch, proxy both
//   own dedupe, own batch, own both   the same done inside our d3d8.dll (HifiFrameSetSpeedups), when it has them
// Ashita's own wrapper isn't in between here (it is in game): its per-draw dispatch to the plugins ('ashita' skips it)
// isn't measured.
//
// "today" leaves out the calls the game no longer makes since HiFi XI draws the zone, the characters and their shadows
// and World FX's VisTest answers the visibility tests (the callers by FFXiMain offset, as tools/hifps_proxy_counts.py).
//
// The calls: render and stage states, textures (by size and format, as the capture names them; render targets sampled
// as the replay picks them), the projection and texture matrices, world and view (the capture's matrices where it has
// them, else each call a new value so dedupe can't drop it), lights, light enables, materials, SetVertexShader where
// the capture has it (FVFs; the programmable shaders as the replay's fixed function), streams and indices (one buffer per
// one the capture names), state blocks applied (empty ones: through the proxy they forget its shadow, as in game),
// targets, viewports, clears, scenes, draws (made-up geometry, the replay's), Present. Visibility tests are read back
// ("all"); their waits aren't counted (no CPU).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bench
{
	struct op
	{
		enum kind : uint8_t
		{
			rs, tss, tex, xform, light, light_enable, material, vs, stream, indices, apply, rt, viewport, clear, begin, end, dp, dip, dpup, readback
		} k;
		DWORD a = 0, b = 0, c = 0;
		void *p = nullptr, *q = nullptr;
		uint32_t data = 0; // index into the frame's matrices, lights, materials, viewports, UP vertices
	};

	struct frame
	{
		std::vector<op> ops;
		std::vector<D3DMATRIX> matrices;
		std::vector<D3DLIGHT8> lights;
		std::vector<D3DMATERIAL8> materials;
		std::vector<D3DVIEWPORT8> viewports;
		std::vector<std::vector<uint8_t>> up;
		std::vector<IDirect3DVertexBuffer8 *> vbs;
		IDirect3DIndexBuffer8 *ib = nullptr;
		IDirect3DSurface8 *sys16 = nullptr;
		DWORD block = 0;
		uint32_t draws = 0, ups = 0;
	};

	// The callers the game no longer reaches (FFXiMain offsets), as tools/hifps_proxy_counts.py has them
	inline bool today(const replay::call &c)
	{
		static const uint32_t out[][2] = {
			{ 0x17ADE0, 0x17B510 }, { 0x17C9C0, 0x17E000 }, { 0x183DA0, 0x184500 }, { 0x029F00, 0x02F300 }, { 0x033800, 0x033D00 },
			{ 0x184FE0, 0x185C00 }, { 0x185CA0, 0x186B00 }, { 0x06C280, 0x06CA00 },
		};
		for (uint32_t a : c.chain)
			for (const auto &r : out)
				if (a >= r[0] && a < r[1])
					return false;
		return true;
	}

	inline uint32_t hash32_of(const std::string &s)
	{
		uint32_t h = 2166136261u;
		for (const char ch : s)
			h = (h ^ uint8_t(ch)) * 16777619u;
		return h;
	}

	inline bool build(const char *capture, bool only_today, frame &f)
	{
		replay::state st;
		if (!replay::parse(capture, st))
			return false;
		const UINT kVb = 1u << 20;
		dev->CreateIndexBuffer(1u << 17, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &f.ib);
		{
			BYTE *p = nullptr;
			f.ib->Lock(0, 0, &p, 0);
			for (UINT i = 0; i < (1u << 16); ++i)
				reinterpret_cast<WORD *>(p)[i] = WORD(i % 4096);
			f.ib->Unlock();
		}
		dev->CreateImageSurface(16, 16, D3DFMT_A8R8G8B8, &f.sys16);
		dev->BeginStateBlock();
		dev->EndStateBlock(&f.block);
		std::map<std::string, uint32_t> vb_of;
		auto vb_named = [&](const std::string &id) {
			const auto it = vb_of.find(id);
			if (it != vb_of.end())
				return f.vbs[it->second];
			// (up to 32 distinct buffers, then shared: enough for dedupe to tell them apart as the game's are)
			if (f.vbs.size() >= 32)
				return f.vbs[vb_of[id] = uint32_t(hash32_of(id) % f.vbs.size())];
			IDirect3DVertexBuffer8 *vb = nullptr;
			dev->CreateVertexBuffer(kVb, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb);
			BYTE *p = nullptr;
			vb->Lock(0, 0, &p, 0);
			uint32_t r = uint32_t(f.vbs.size()) * 977u + 5u;
			// (small triangles in front of the camera, whatever the format: positions first in every FVF here)
			for (UINT i = 0; i < kVb / 4; ++i)
				reinterpret_cast<float *>(p)[i] = (i % 3 == 2) ? 3.0f + replay::rnd(r) * 30.0f : (replay::rnd(r) - 0.5f) * 0.8f;
			vb->Unlock();
			vb_of[id] = uint32_t(f.vbs.size());
			f.vbs.push_back(vb);
			return vb;
		};
		const D3DMATRIX id = identity();
		uint32_t fresh = 0;
		for (const replay::call &c : st.calls)
		{
			if (only_today && !today(c))
				continue;
			const std::vector<std::string> w = replay::split(c.args);
			op o{};
			if (c.op == "RS" || c.op == "TSS")
			{
				const size_t eq = c.args.find(" = ");
				const std::vector<std::string> left = replay::split(c.args.substr(0, eq));
				const std::string value = eq != std::string::npos ? replay::split(c.args.substr(eq + 3)).front() : std::string();
				const std::string name = left.empty() ? std::string() : left.back();
				DWORD v = 0;
				if (!replay::value_of(name, value, v))
					continue;
				if (c.op == "RS")
				{
					const auto it = replay::names().find(name);
					if (it == replay::names().end())
						continue;
					o.k = op::rs;
					o.a = it->second;
					o.b = v;
				}
				else
				{
					const auto it = replay::stage_names().find(name);
					if (it == replay::stage_names().end() || left.size() < 2)
						continue;
					o.k = op::tss;
					o.a = DWORD(std::atoi(left[0].c_str()));
					o.b = it->second;
					o.c = v;
				}
			}
			else if (c.op == "Tex" && w.size() >= 2)
			{
				o.k = op::tex;
				o.a = DWORD(std::atoi(w[0].c_str()));
				if (w[1] == "null")
					o.p = nullptr;
				else if (w.back() == "RT")
				{
					// (a render target sampled: the one last drawn into that isn't bound, as the replay picks it at the draw)
					std::string pick;
					for (auto it = st.drawn_order.rbegin(); it != st.drawn_order.rend(); ++it)
						if (*it != st.current_rt && st.target_tex.count(*it) != 0)
						{
							pick = *it;
							break;
						}
					o.p = pick.empty() ? nullptr : st.target_tex[pick];
				}
				else
				{
					std::string desc;
					for (size_t i = 2; i < w.size(); ++i)
						desc += (i > 2 ? " " : "") + w[i];
					o.p = replay::texture_named(st, desc);
				}
			}
			else if (c.op == "Xform" && !w.empty())
			{
				D3DTRANSFORMSTATETYPE t = D3DTS_WORLD;
				if (w[0] == "PROJECTION")
					t = D3DTS_PROJECTION;
				else if (w[0] == "VIEW")
					t = D3DTS_VIEW;
				else if (w[0].rfind("TEXTURE", 0) == 0)
					t = D3DTRANSFORMSTATETYPE(D3DTS_TEXTURE0 + std::atoi(w[0].c_str() + 7));
				else if (w[0].rfind("WORLD", 0) != 0)
					continue;
				const auto m = st.matrices.find(c.n);
				D3DMATRIX v = id;
				if (m != st.matrices.end() && (t == D3DTS_PROJECTION || (t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7)))
					v = m->second;
				else
					v.m[3][0] = 1e-6f * float(++fresh); // (not in the capture, or the made-up geometry's identity: a new value each call)
				o.k = op::xform;
				o.a = t;
				o.data = uint32_t(f.matrices.size());
				f.matrices.push_back(v);
			}
			else if (c.op == "SetLight")
			{
				// (the replay's reading, into a light of our own)
				D3DLIGHT8 l = {};
				bool ok = true;
				replay::light(c, ok);
				if (!ok)
					continue;
				o.k = op::light;
				o.a = DWORD(std::atoi(w[0].c_str()));
				dev->GetLight(o.a, &l);
				o.data = uint32_t(f.lights.size());
				f.lights.push_back(l);
			}
			else if (c.op == "LightEnable" && w.size() >= 2)
			{
				o.k = op::light_enable;
				o.a = DWORD(std::atoi(w[0].c_str()));
				o.b = w[1] == "on";
			}
			else if (c.op == "SetMaterial")
			{
				D3DMATERIAL8 m = {};
				auto group = [&](const char *name, float *out) {
					const size_t at = c.args.find(std::string(name) + "(");
					if (at != std::string::npos)
					{
						std::stringstream ss(c.args.substr(at + std::strlen(name) + 1));
						ss >> out[0] >> out[1] >> out[2] >> out[3];
					}
				};
				group("diff", &m.Diffuse.r);
				group("amb", &m.Ambient.r);
				group("spec", &m.Specular.r);
				group("emis", &m.Emissive.r);
				const size_t pw = c.args.find("power ");
				m.Power = pw != std::string::npos ? std::strtof(c.args.c_str() + pw + 6, nullptr) : 0.0f;
				o.k = op::material;
				o.data = uint32_t(f.materials.size());
				f.materials.push_back(m);
			}
			else if (c.op == "VS" && !w.empty())
			{
				st.fvf = replay::fvf_named(w[0]);
				o.k = op::vs;
				o.a = st.fvf;
			}
			else if (c.op == "Stream" && w.size() >= 5)
			{
				// "0 vb ID stride S"
				o.k = op::stream;
				o.a = DWORD(std::atoi(w[0].c_str()));
				o.p = vb_named(w[2]);
				o.b = DWORD(std::atoi(w[4].c_str()));
				if (o.a != 0)
					continue; // (the replay's draws read stream 0 alone)
			}
			else if (c.op == "Indices" && w.size() >= 4)
			{
				o.k = op::indices;
				o.p = f.ib;
				o.b = 0;
			}
			else if (c.op == "ApplyStateBlock")
				o.k = op::apply;
			else if (c.op == "SetRenderTarget" && w.size() >= 3)
			{
				o.k = op::rt;
				o.p = replay::surface_of(st, w[0], false);
				o.q = w[2] == "00000000" ? nullptr : replay::surface_of(st, w[2], true);
				st.current_rt = w[0];
			}
			else if (c.op == "SetViewport" && w.size() >= 4)
			{
				D3DVIEWPORT8 vp = { DWORD(std::atoi(w[0].c_str()) / 2), DWORD(std::atoi(w[1].c_str()) / 2), DWORD(std::max(1, std::atoi(w[2].c_str()) / 2)),
					DWORD(std::max(1, std::atoi(w[3].c_str()) / 2)), 0.0f, 1.0f };
				o.k = op::viewport;
				o.data = uint32_t(f.viewports.size());
				f.viewports.push_back(vp);
			}
			else if (c.op == "Clear" && w.size() >= 6)
			{
				o.k = op::clear;
				o.a = DWORD(std::strtoul(w[1].c_str(), nullptr, 16));
				o.b = DWORD(std::strtoul(w[3].c_str(), nullptr, 16));
			}
			else if (c.op == "BeginScene")
				o.k = op::begin;
			else if (c.op == "EndScene")
				o.k = op::end;
			else if (c.op == "DP" || c.op == "DIP" || c.op == "DPUP")
			{
				// "#n TYPE prims P verts V [stride S]"
				if (w.size() < 6)
					continue;
				const D3DPRIMITIVETYPE t = replay::prim_named(w[1]);
				const UINT prims = UINT(std::atoi(w[3].c_str())), verts = UINT(std::atoi(w[5].c_str()));
				if (prims == 0)
					continue;
				o.a = t;
				o.b = prims;
				f.draws++;
				if (c.op == "DPUP")
				{
					o.k = op::dpup;
					const UINT n = replay::verts_for(t, prims);
					o.data = uint32_t(f.up.size());
					f.up.push_back(replay::vertices(st, n, uint32_t(std::atoi(w[0].c_str() + 1))));
					o.c = replay::fvf_stride(st.fvf);
					f.ups++;
				}
				else
				{
					o.k = c.op == "DP" ? op::dp : op::dip;
					o.c = std::min<UINT>(std::max(verts, 3u), 4000);
				}
				if (std::find(st.drawn_order.begin(), st.drawn_order.end(), st.current_rt) != st.drawn_order.end())
					st.drawn_order.erase(std::find(st.drawn_order.begin(), st.drawn_order.end(), st.current_rt));
				st.drawn_order.push_back(st.current_rt);
				f.ops.push_back(o);
				// The visibility test's downsample, read back as the game reads it
				bool vis = false;
				for (uint32_t a : c.chain)
					vis = vis || a == 0x06C867;
				if (vis)
				{
					op r{};
					r.k = op::readback;
					r.p = st.surfaces[st.current_rt];
					f.ops.push_back(r);
				}
				continue;
			}
			else
				continue;
			f.ops.push_back(o);
		}
		return true;
	}

	// One frame of the list, as the game would call it
	inline void play(frame &f)
	{
		IDirect3DDevice8 *d = dev;
		for (const op &o : f.ops)
		{
			switch (o.k)
			{
			case op::rs: d->SetRenderState(D3DRENDERSTATETYPE(o.a), o.b); break;
			case op::tss: d->SetTextureStageState(o.a, D3DTEXTURESTAGESTATETYPE(o.b), o.c); break;
			case op::tex: d->SetTexture(o.a, static_cast<IDirect3DBaseTexture8 *>(o.p)); break;
			case op::xform: d->SetTransform(D3DTRANSFORMSTATETYPE(o.a), &f.matrices[o.data]); break;
			case op::light: d->SetLight(o.a, &f.lights[o.data]); break;
			case op::light_enable: d->LightEnable(o.a, BOOL(o.b)); break;
			case op::material: d->SetMaterial(&f.materials[o.data]); break;
			case op::vs: d->SetVertexShader(o.a); break;
			case op::stream: d->SetStreamSource(o.a, static_cast<IDirect3DVertexBuffer8 *>(o.p), o.b); break;
			case op::indices: d->SetIndices(static_cast<IDirect3DIndexBuffer8 *>(o.p), o.b); break;
			case op::apply: d->ApplyStateBlock(f.block); break;
			case op::rt: d->SetRenderTarget(static_cast<IDirect3DSurface8 *>(o.p), static_cast<IDirect3DSurface8 *>(o.q)); break;
			case op::viewport: d->SetViewport(&f.viewports[o.data]); break;
			case op::clear: d->Clear(0, nullptr, o.a, D3DCOLOR(o.b), 1.0f, 0); break;
			case op::begin: d->BeginScene(); break;
			case op::end: d->EndScene(); break;
			case op::dp: d->DrawPrimitive(D3DPRIMITIVETYPE(o.a), 0, o.b); break;
			case op::dip: d->DrawIndexedPrimitive(D3DPRIMITIVETYPE(o.a), 0, o.c, 0, o.b); break;
			case op::dpup: d->DrawPrimitiveUP(D3DPRIMITIVETYPE(o.a), o.b, f.up[o.data].data(), o.c); break;
			case op::readback:
			{
				const RECT r16 = { 0, 0, 8, 8 };
				const POINT p0 = { 0, 0 };
				if (o.p != nullptr && SUCCEEDED(d->CopyRects(static_cast<IDirect3DSurface8 *>(o.p), &r16, 1, f.sys16, &p0)))
				{
					D3DLOCKED_RECT lr;
					if (SUCCEEDED(f.sys16->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
						f.sys16->UnlockRect();
				}
				break;
			}
			}
		}
		d->Present(nullptr, nullptr, nullptr, nullptr);
		MSG msg;
		while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
			DispatchMessageA(&msg);
	}

	// ---- hifps's proxy (proxy.asm), from the addon's proxy\proxy.blob, laid on our device as HiFi XI laid it on Ashita's
	struct proxy
	{
		uint8_t *base = nullptr;
		uint32_t size = 0;
		// (the symbols in proxy.asm's order, as the blob lists them)
		enum sym { D_PENDING, D_PBYTES, D_STRIDE, D_PDEV, D_BUF, D_BUFSIZE, S_SKIPPED, S_MERGED, S_BATCHES, F_BATCH, F_DEDUPE, F_REC, ORIG, VT_RTTI, VTABLE, THUNK_TABLE, VALID_START, VALID_END, FLUSH, kSyms };
		uint32_t at[kSyms] = {};
		uint32_t own_vt = 0;
		uint8_t *s(sym n) const { return base + at[n]; }
		uint32_t &u32(sym n) const { return *reinterpret_cast<uint32_t *>(s(n)); }
	};

	inline bool proxy_load(proxy &px)
	{
		FILE *fp = std::fopen("C:\\Games\\PhoenixXI\\addons\\hifps\\proxy\\proxy.blob", "rb");
		if (fp == nullptr)
			return false;
		std::vector<uint8_t> d;
		uint8_t buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0)
			d.insert(d.end(), buf, buf + n);
		std::fclose(fp);
		if (d.size() < 12)
			return false;
		uint32_t h[3];
		std::memcpy(h, d.data(), 12);
		const uint32_t size = h[0], nrel = h[1], nsym = h[2];
		if (nsym != proxy::kSyms || d.size() != 12 + 4 * size_t(nrel + nsym) + size)
			return false;
		const uint8_t *rel = d.data() + 12, *sym = rel + 4 * nrel, *code = sym + 4 * nsym;
		const uint32_t kBuf = 0x20000;
		px.base = static_cast<uint8_t *>(VirtualAlloc(nullptr, size + 16 + kBuf, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (px.base == nullptr)
			return false;
		px.size = size;
		std::memcpy(px.base, code, size);
		for (uint32_t i = 0; i < nrel; ++i)
		{
			uint32_t r, v;
			std::memcpy(&r, rel + 4 * i, 4);
			std::memcpy(&v, px.base + r, 4);
			v += uint32_t(reinterpret_cast<uintptr_t>(px.base));
			std::memcpy(px.base + r, &v, 4);
		}
		std::memcpy(px.at, sym, 4 * nsym);
		const uintptr_t b = reinterpret_cast<uintptr_t>(px.base) + size + 15;
		px.u32(proxy::D_BUF) = uint32_t(b - b % 16);
		px.u32(proxy::D_BUFSIZE) = kBuf;
		return true;
	}

	// The proxy on (its flags) or off (the device's own vtable back)
	inline void proxy_set(proxy &px, bool on, bool batch, bool dedupe)
	{
		uint32_t *object = reinterpret_cast<uint32_t *>(dev);
		if (px.own_vt == 0)
			px.own_vt = object[0];
		if (!on)
		{
			object[0] = px.own_vt;
			return;
		}
		const uint32_t vt = px.own_vt;
		std::memset(px.s(proxy::VALID_START), 0, px.at[proxy::VALID_END] - px.at[proxy::VALID_START]);
		for (proxy::sym n : { proxy::D_PENDING, proxy::S_SKIPPED, proxy::S_MERGED, proxy::S_BATCHES })
			px.u32(n) = 0;
		*px.s(proxy::F_REC) = 0;
		for (uint32_t i = 0; i < 128; ++i)
		{
			const uint32_t fn = reinterpret_cast<const uint32_t *>(uintptr_t(vt))[i];
			const uint32_t t = reinterpret_cast<const uint32_t *>(px.s(proxy::THUNK_TABLE))[i];
			reinterpret_cast<uint32_t *>(px.s(proxy::ORIG))[i] = fn;
			reinterpret_cast<uint32_t *>(px.s(proxy::VTABLE))[i] = t != 0 ? t : fn;
		}
		px.u32(proxy::VT_RTTI) = reinterpret_cast<const uint32_t *>(uintptr_t(vt))[-1];
		*px.s(proxy::F_BATCH) = batch ? 1 : 0;
		*px.s(proxy::F_DEDUPE) = dedupe ? 1 : 0;
		object[0] = uint32_t(reinterpret_cast<uintptr_t>(px.s(proxy::VTABLE)));
	}

	// Our d3d8.dll's own (HifiFrameSetSpeedups / HifiFrameSpeedupStats; nullptr: a DLL without them)
	using set_speedups_fn = uint32_t(__cdecl *)(uint32_t flags);
	using speedup_stats_fn = void(__cdecl *)(uint32_t out[4]);

	struct way
	{
		const char *name;
		bool proxy, batch, dedupe;
		uint32_t own; // (HifiFrameSetSpeedups' flags: 1 dedupe, 2 batch)
		std::vector<double> us;   // the calling thread's CPU a frame, microseconds
		std::vector<double> wall; // milliseconds a frame
		uint64_t dropped = 0, merged = 0, batches = 0, frames = 0;
	};

	inline double cycles_per_us()
	{
		LARGE_INTEGER f, t0, t1;
		QueryPerformanceFrequency(&f);
		ULONG64 c0 = 0, c1 = 0;
		QueryPerformanceCounter(&t0);
		QueryThreadCycleTime(GetCurrentThread(), &c0);
		do
			QueryPerformanceCounter(&t1);
		while (double(t1.QuadPart - t0.QuadPart) < 0.2 * double(f.QuadPart));
		QueryThreadCycleTime(GetCurrentThread(), &c1);
		return double(c1 - c0) / (double(t1.QuadPart - t0.QuadPart) * 1e6 / double(f.QuadPart));
	}

	inline double median(std::vector<double> v)
	{
		if (v.empty())
			return 0;
		std::sort(v.begin(), v.end());
		return v[v.size() / 2];
	}

	inline int run(const char *dll, const char *capture, const char *view, int rounds)
	{
		const bool only_today = view != nullptr && std::strcmp(view, "today") == 0;
		frame f;
		if (!build(capture, only_today, f))
		{
			std::printf("bench: %s not read\n", capture);
			return 1;
		}
		HMODULE h = GetModuleHandleA(dll);
		if (h == nullptr)
			h = GetModuleHandleA("d3d8.dll");
		const auto set_own = h != nullptr ? reinterpret_cast<set_speedups_fn>(GetProcAddress(h, "HifiFrameSetSpeedups")) : nullptr;
		const auto own_stats = h != nullptr ? reinterpret_cast<speedup_stats_fn>(GetProcAddress(h, "HifiFrameSpeedupStats")) : nullptr;
		proxy px;
		const bool have_proxy = proxy_load(px);
		std::vector<way> ways = { { "direct", false, false, false, 0 } };
		if (have_proxy)
			for (way w : { way{ "proxy (flags off)", true, false, false, 0 }, way{ "proxy dedupe", true, false, true, 0 }, way{ "proxy batch", true, true, false, 0 },
					 way{ "proxy both", true, true, true, 0 } })
				ways.push_back(w);
		if (set_own != nullptr)
			for (way w : { way{ "own dedupe", false, false, false, 1 }, way{ "own batch", false, false, false, 2 }, way{ "own both", false, false, false, 3 } })
				ways.push_back(w);
		if (set_own != nullptr)
			set_own(0);
		const double cpu = cycles_per_us();
		std::printf("bench %s (%s): %zu calls a frame, %u draws (%u UP), %d rounds of %zu ways; %.0f cycles a microsecond%s\n", capture, only_today ? "today" : "all",
			f.ops.size(), f.draws, f.ups, rounds, ways.size(), cpu, have_proxy ? "" : "; no proxy.blob: the proxy's ways left out");
		// (warm: pipelines and shaders made, uploads settled)
		for (int i = 0; i < 20; ++i)
			play(f);
		constexpr int kFrames = 12;
		LARGE_INTEGER qf;
		QueryPerformanceFrequency(&qf);
		for (int r = 0; r < rounds; ++r)
			for (way &w : ways)
			{
				if (set_own != nullptr)
					set_own(w.own);
				if (w.proxy)
					proxy_set(px, true, w.batch, w.dedupe);
				uint32_t s0[4] = {};
				if (own_stats != nullptr)
					own_stats(s0);
				for (int i = 0; i < kFrames + 2; ++i)
				{
					LARGE_INTEGER t0, t1;
					ULONG64 c0 = 0, c1 = 0;
					QueryPerformanceCounter(&t0);
					QueryThreadCycleTime(GetCurrentThread(), &c0);
					play(f);
					QueryThreadCycleTime(GetCurrentThread(), &c1);
					QueryPerformanceCounter(&t1);
					// (the first two after a switch: its effect on caches and the frames in flight settle)
					if (i < 2)
						continue;
					w.us.push_back(double(c1 - c0) / cpu);
					w.wall.push_back(double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(qf.QuadPart));
					w.frames++;
				}
				if (w.proxy)
				{
					w.dropped += px.u32(proxy::S_SKIPPED);
					w.merged += px.u32(proxy::S_MERGED);
					w.batches += px.u32(proxy::S_BATCHES);
					proxy_set(px, false, false, false);
				}
				if (own_stats != nullptr && w.own != 0)
				{
					uint32_t s1[4] = {};
					own_stats(s1);
					w.dropped += s1[0] - s0[0];
					w.merged += s1[1] - s0[1];
					w.batches += s1[2] - s0[2];
				}
			}
		if (set_own != nullptr)
			set_own(0);
		const double base = median(ways[0].us);
		for (const way &w : ways)
		{
			const double fr = double(std::max<uint64_t>(w.frames, 1));
			const double us = median(w.us);
			std::printf("  %-18s %8.0f us CPU a frame (%+6.0f us, %+5.1f%%)  %6.2f ms wall", w.name, us, us - base, base > 0 ? 100.0 * (us - base) / base : 0.0, median(w.wall));
			// (the proxy's counters run over the frames before the first timed one too: a frame's worth is about right)
			if (w.dropped != 0 || w.merged != 0)
				std::printf("  dropped %.0f, %.0f quads in %.0f draws a frame", double(w.dropped) / (fr + 2.0 * rounds), double(w.merged) / (fr + 2.0 * rounds),
					double(w.batches) / (fr + 2.0 * rounds));
			std::printf("\n");
		}
		return 0;
	}
}
