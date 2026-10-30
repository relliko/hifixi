/*
 * HiFi XI's d3d8.dll offline (docs/own-frame/d3d8.md): Direct3D 8 programs run through a d3d8.dll named on the command
 * line: ours (build\d3d8\d3d8.dll), or dgVoodoo 2.87's copied with its dgVoodoo.conf from the game's bootloader into a
 * scratch folder (never the game's folder). Each scene is drawn into a render target, read back the way the game reads
 * its visibility tests (CopyRects into an image surface, LockRect) and written as a BMP; `compare` puts two runs side by
 * side, pixel by pixel.
 *
 *   d3d8_frame.exe <d3d8.dll> caps                         the caps and formats it reports
 *   d3d8_frame.exe <d3d8.dll> scenes <out folder> [name]   every scene (or one) -> <out>\<scene>.bmp
 *   d3d8_frame.exe <d3d8.dll> replay <capture> <out.bmp>   a d3dlog capture's frame replayed (d3d8_replay.hpp)
 *   d3d8_frame.exe compare <a.bmp> <b.bmp> [diff.bmp]      mean / 99th percentile / largest channel difference,
 *                                                          share of pixels within 2 and within 8 (of 255)
 *
 * The scenes, each a grid of small cases (Direct3D 8 the way FFXI, its Dancer engine and Ashita use it):
 *   formats   every texture format FFXI and Ashita use, pretransformed quads, point sampling
 *   ops       the texture stage operations and argument modifiers, two-stage and TEMP chains, TFACTOR
 *   blend     blend factors and operations (REVSUBTRACT), alpha test, colour write masks, filtering
 *   light     the fixed function's lights (directional, point, spot), material sources, ambient, specular
 *   fog       vertex fog (linear, range, exp, exp2), table fog, pretransformed fog
 *   texgen    texture matrices (UV scroll as the effects do), generated coordinates, a cube map, addressing, mips
 *   stencil   stencil as the Dancer uses it (INCR, EQUAL, LESS), ZBIAS, depth functions
 *   streams   declarations over three streams (as the characters), vertex blending, fans, lines, points, UP draws
 *   shaders   vs.1.1 instructions, the game's own zone shaders (assembled from its text in the FFXiMain dump), ps.1.1-1.4
 *   imgui     an Ashita-like ImGui DX8 frame: state block, dynamic buffers, base vertex, clipped by viewports
 *   readback  16x16 visibility tests read back several times a frame, GetFrontBuffer
 */

#include <windows.h>
#include <d3d8.h>
#include <d3dx8.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace
{
	IDirect3DDevice8 *dev = nullptr;
	IDirect3D8 *d3d = nullptr;
	HWND window = nullptr;
	constexpr UINT W = 256, H = 256;
	IDirect3DTexture8 *target_tex = nullptr;
	IDirect3DSurface8 *target = nullptr, *depth = nullptr, *readback_surface = nullptr;
	int failures = 0;

	void check(bool ok, const char *what)
	{
		if (!ok)
		{
			std::printf("FAIL  %s\n", what);
			failures++;
		}
	}

	// ---- BMP ----
	bool write_bmp(const std::string &path, UINT w, UINT h, const std::vector<uint32_t> &px)
	{
		FILE *f = std::fopen(path.c_str(), "wb");
		if (f == nullptr)
			return false;
		const uint32_t row = w * 4, size = 54 + row * h;
		uint8_t header[54] = { 'B', 'M' };
		std::memcpy(header + 2, &size, 4);
		header[10] = 54;
		header[14] = 40;
		std::memcpy(header + 18, &w, 4);
		const int32_t hh = -int32_t(h);
		std::memcpy(header + 22, &hh, 4);
		header[26] = 1;
		header[28] = 32;
		std::fwrite(header, 1, 54, f);
		std::fwrite(px.data(), 4, px.size(), f);
		std::fclose(f);
		return true;
	}

	bool read_bmp(const std::string &path, UINT &w, UINT &h, std::vector<uint32_t> &px)
	{
		FILE *f = std::fopen(path.c_str(), "rb");
		if (f == nullptr)
			return false;
		uint8_t header[54];
		if (std::fread(header, 1, 54, f) != 54)
		{
			std::fclose(f);
			return false;
		}
		int32_t iw, ih;
		std::memcpy(&iw, header + 18, 4);
		std::memcpy(&ih, header + 22, 4);
		w = UINT(iw);
		h = UINT(ih < 0 ? -ih : ih);
		px.resize(size_t(w) * h);
		const size_t got = std::fread(px.data(), 4, px.size(), f);
		std::fclose(f);
		return got == px.size();
	}

	// ---- The device and the target the scenes draw into ----
	LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

	bool make_device(const char *dll, UINT bw, UINT bh)
	{
		WNDCLASSA wc = {};
		wc.lpfnWndProc = wndproc;
		wc.hInstance = GetModuleHandleA(nullptr);
		wc.lpszClassName = "d3d8_frame";
		RegisterClassA(&wc);
		RECT r = { 0, 0, LONG(bw), LONG(bh) };
		AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
		window = CreateWindowA("d3d8_frame", "d3d8_frame", WS_OVERLAPPEDWINDOW, 40, 40, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
		ShowWindow(window, SW_SHOWNOACTIVATE);
		HMODULE m = LoadLibraryA(dll);
		if (m == nullptr)
		{
			std::printf("no %s (%lu)\n", dll, GetLastError());
			return false;
		}
		using create_fn = IDirect3D8 *(WINAPI *)(UINT);
		const auto create = reinterpret_cast<create_fn>(GetProcAddress(m, "Direct3DCreate8"));
		d3d = create != nullptr ? create(D3D_SDK_VERSION) : nullptr;
		if (d3d == nullptr)
		{
			std::printf("Direct3DCreate8 failed\n");
			return false;
		}
		D3DPRESENT_PARAMETERS pp = {};
		pp.BackBufferWidth = bw;
		pp.BackBufferHeight = bh;
		pp.BackBufferFormat = D3DFMT_X8R8G8B8;
		pp.BackBufferCount = 1;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.hDeviceWindow = window;
		pp.Windowed = TRUE;
		pp.EnableAutoDepthStencil = TRUE;
		pp.AutoDepthStencilFormat = D3DFMT_D24S8;
		const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
		if (FAILED(hr))
		{
			std::printf("CreateDevice 0x%08lX\n", hr);
			return false;
		}
		dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &target_tex);
		target_tex->GetSurfaceLevel(0, &target);
		dev->CreateDepthStencilSurface(W, H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, &depth);
		dev->CreateImageSurface(W, H, D3DFMT_A8R8G8B8, &readback_surface);
		return target != nullptr && depth != nullptr && readback_surface != nullptr;
	}

	// The target's pixels (as the game reads its tests back)
	std::vector<uint32_t> read_target(IDirect3DSurface8 *src, UINT w, UINT h, IDirect3DSurface8 *sys)
	{
		std::vector<uint32_t> px(size_t(w) * h, 0);
		const RECT r = { 0, 0, LONG(w), LONG(h) };
		const POINT p = { 0, 0 };
		check(SUCCEEDED(dev->CopyRects(src, &r, 1, sys, &p)), "CopyRects into the image surface");
		D3DLOCKED_RECT lr;
		if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
		{
			for (UINT y = 0; y < h; ++y)
				std::memcpy(px.data() + size_t(y) * w, static_cast<const uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch, size_t(w) * 4);
			sys->UnlockRect();
		}
		return px;
	}

	void begin_scene(D3DCOLOR clear = 0xFF404850)
	{
		dev->SetRenderTarget(target, depth);
		dev->BeginScene();
		dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, clear, 1.0f, 0);
	}

	// A cell of the 8x8 grid as a viewport (cases draw into their own cell)
	void cell(int i, int cols = 8)
	{
		const UINT s = W / UINT(cols);
		D3DVIEWPORT8 vp = { UINT(i % cols) * s, UINT(i / cols) * s, s, s, 0.0f, 1.0f };
		dev->SetViewport(&vp);
	}

	// ---- Vertex formats ----
	struct VRhw { float x, y, z, rhw; DWORD c; float u, v; };
	constexpr DWORD kRhw = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
	struct VLit { float x, y, z, nx, ny, nz; DWORD c; float u, v; };
	constexpr DWORD kLit = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1;
	struct VSpec { float x, y, z, rhw; DWORD c, s; float u, v; };
	constexpr DWORD kRhwSpec = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1;

	// A pretransformed quad over the current cell (inset), corners coloured c0..c3, UVs 0..us
	void quad(D3DCOLOR c0, D3DCOLOR c1, D3DCOLOR c2, D3DCOLOR c3, float us = 1.0f, float inset = 2.0f, float z = 0.5f)
	{
		D3DVIEWPORT8 vp;
		dev->GetViewport(&vp);
		const float x0 = float(vp.X) + inset, y0 = float(vp.Y) + inset, x1 = float(vp.X + vp.Width) - inset, y1 = float(vp.Y + vp.Height) - inset;
		const VRhw v[4] = { { x0, y0, z, 1, c0, 0, 0 }, { x1, y0, z, 1, c1, us, 0 }, { x0, y1, z, 1, c2, 0, us }, { x1, y1, z, 1, c3, us, us } };
		dev->SetVertexShader(kRhw);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(VRhw));
	}

	void defaults()
	{
		dev->SetPixelShader(0);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
		dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		dev->SetRenderState(D3DRS_ZBIAS, 0);
		dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
		for (DWORD s = 0; s < 8; ++s)
		{
			dev->SetTexture(s, nullptr);
			dev->SetTextureStageState(s, D3DTSS_COLOROP, s == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE);
			dev->SetTextureStageState(s, D3DTSS_ALPHAOP, s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
			dev->SetTextureStageState(s, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(s, D3DTSS_COLORARG2, D3DTA_CURRENT);
			dev->SetTextureStageState(s, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(s, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
			dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, s);
			dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			dev->SetTextureStageState(s, D3DTSS_MINFILTER, D3DTEXF_POINT);
			dev->SetTextureStageState(s, D3DTSS_MAGFILTER, D3DTEXF_POINT);
			dev->SetTextureStageState(s, D3DTSS_MIPFILTER, D3DTEXF_NONE);
			dev->SetTextureStageState(s, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
			dev->SetTextureStageState(s, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
			dev->SetTextureStageState(s, D3DTSS_RESULTARG, D3DTA_CURRENT);
		}
	}

	// ---- Textures: a 16x16 pattern in each format ----
	uint32_t pattern(UINT x, UINT y, UINT seed)
	{
		// (opaque/translucent squares in four colours: every channel and alpha level shows)
		const uint32_t cols[4] = { 0xFFE03020, 0x8020C040, 0x402060E0, 0xFFF0F0F0 };
		uint32_t c = cols[((x / 4) + (y / 4) + seed) & 3];
		if (((x ^ y) & 1) != 0)
			c = (c & 0xFF000000) | ((c >> 1) & 0x7F7F7F);
		return c;
	}

	void encode(D3DFORMAT f, uint32_t argb, uint8_t *o)
	{
		const uint32_t a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
		switch (f)
		{
		case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: std::memcpy(o, &argb, 4); break;
		case D3DFMT_R5G6B5: { const uint16_t v = uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); std::memcpy(o, &v, 2); break; }
		case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: { const uint16_t v = uint16_t((a >= 128 ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)); std::memcpy(o, &v, 2); break; }
		case D3DFMT_A4R4G4B4: { const uint16_t v = uint16_t((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4)); std::memcpy(o, &v, 2); break; }
		case D3DFMT_L8: o[0] = uint8_t((r * 77 + g * 150 + b * 29) >> 8); break;
		case D3DFMT_A8: o[0] = uint8_t(a); break;
		case D3DFMT_A8L8: o[0] = uint8_t((r * 77 + g * 150 + b * 29) >> 8); o[1] = uint8_t(a); break;
		case D3DFMT_P8: o[0] = uint8_t(((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6)); break;
		default: break;
		}
	}

	// A DXT block of two colours and alpha levels (the pattern above, coarsely)
	void dxt_block(D3DFORMAT f, UINT bx, UINT by, uint8_t *o)
	{
		const uint32_t c0 = pattern(bx * 4, by * 4, 1), c1 = pattern(bx * 4 + 1, by * 4 + 2, 2);
		auto to565 = [](uint32_t c) { return uint16_t(((c >> 19) & 31) << 11 | ((c >> 10) & 63) << 5 | ((c >> 3) & 31)); };
		uint16_t a = to565(c0), b = to565(c1);
		if (a < b)
			std::swap(a, b);
		if (a == b)
			a = uint16_t(a | 0x8000);
		uint8_t *colour = f == D3DFMT_DXT1 ? o : o + 8;
		std::memcpy(colour, &a, 2);
		std::memcpy(colour + 2, &b, 2);
		const uint32_t idx = 0x1B6C93E4u ^ (bx * 0x9E3779B1u) ^ (by * 0x85EBCA6Bu);
		std::memcpy(colour + 4, &idx, 4);
		if (f == D3DFMT_DXT3)
			for (int i = 0; i < 8; ++i)
				o[i] = uint8_t((i * 37 + bx * 11 + by * 5) & 0xFF);
		if (f == D3DFMT_DXT5)
		{
			o[0] = 230;
			o[1] = 40;
			const uint64_t bits = 0x924924924924ull ^ (uint64_t(bx) << 7) ^ (uint64_t(by) << 19);
			std::memcpy(o + 2, &bits, 6);
		}
	}

	IDirect3DTexture8 *make_texture(D3DFORMAT f, UINT size = 16, UINT levels = 1, UINT seed = 0)
	{
		IDirect3DTexture8 *t = nullptr;
		if (FAILED(dev->CreateTexture(size, size, levels, 0, f, D3DPOOL_MANAGED, &t)))
		{
			std::printf("  (format %u not made)\n", unsigned(f));
			return nullptr;
		}
		for (UINT l = 0; l < t->GetLevelCount(); ++l)
		{
			D3DSURFACE_DESC d;
			t->GetLevelDesc(l, &d);
			D3DLOCKED_RECT lr;
			if (FAILED(t->LockRect(l, &lr, nullptr, 0)))
				continue;
			uint8_t *bits = static_cast<uint8_t *>(lr.pBits);
			if (f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5)
			{
				const UINT bytes = f == D3DFMT_DXT1 ? 8 : 16;
				for (UINT by = 0; by < (d.Height + 3) / 4; ++by)
					for (UINT bx = 0; bx < (d.Width + 3) / 4; ++bx)
						dxt_block(f, bx + l * 3, by + seed, bits + size_t(by) * lr.Pitch + size_t(bx) * bytes);
			}
			else
			{
				const UINT bpp = f == D3DFMT_A8R8G8B8 || f == D3DFMT_X8R8G8B8 ? 4 : f == D3DFMT_L8 || f == D3DFMT_A8 || f == D3DFMT_P8 ? 1 : 2;
				for (UINT y = 0; y < d.Height; ++y)
					for (UINT x = 0; x < d.Width; ++x)
					{
						// (each mip level its own colours, to see which is sampled)
						uint32_t c = pattern(x * (1u << l), y * (1u << l), seed + l);
						if (l > 0)
							c = (c & 0xFF000000) | (l == 1 ? 0x00F000F0 : l == 2 ? 0x0000F0F0 : 0x00F0F000);
						encode(f, c, bits + size_t(y) * lr.Pitch + size_t(x) * bpp);
					}
			}
			t->UnlockRect(l);
		}
		return t;
	}

	// ---- Scenes ----
	void scene_formats()
	{
		defaults();
		PALETTEENTRY pal[256];
		for (int i = 0; i < 256; ++i)
			pal[i] = { BYTE(((i >> 4) & 3) * 85), BYTE(((i >> 2) & 3) * 85), BYTE((i & 3) * 85), BYTE(i < 32 ? 128 : 255) };
		dev->SetPaletteEntries(0, pal);
		dev->SetCurrentTexturePalette(0);
		const D3DFORMAT fs[] = { D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_A1R5G5B5, D3DFMT_X1R5G5B5, D3DFMT_A4R4G4B4, D3DFMT_DXT1, D3DFMT_DXT3,
			D3DFMT_DXT5, D3DFMT_L8, D3DFMT_A8, D3DFMT_A8L8, D3DFMT_P8 };
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		int i = 0;
		for (D3DFORMAT f : fs)
		{
			IDirect3DTexture8 *t = make_texture(f);
			dev->SetTexture(0, t);
			cell(i++);
			quad(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
			// (the same modulated by a gradient)
			cell(i++);
			quad(0xFFFF8080, 0x80FFFFFF, 0xFF8080FF, 0x40FFFF00, 2.0f);
			dev->SetTexture(0, nullptr);
			if (t != nullptr)
				t->Release();
		}
		// A DXT1 texture whose top level isn't whole blocks (6x6)
		IDirect3DTexture8 *odd = nullptr;
		if (SUCCEEDED(dev->CreateTexture(6, 6, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &odd)))
		{
			D3DLOCKED_RECT lr;
			odd->LockRect(0, &lr, nullptr, 0);
			for (UINT by = 0; by < 2; ++by)
				for (UINT bx = 0; bx < 2; ++bx)
					dxt_block(D3DFMT_DXT1, bx, by, static_cast<uint8_t *>(lr.pBits) + by * lr.Pitch + bx * 8);
			odd->UnlockRect(0);
			dev->SetTexture(0, odd);
			cell(i++);
			quad(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
			dev->SetTexture(0, nullptr);
			odd->Release();
		}
		else
		{
			// (dgVoodoo makes no DXT texture of a size that isn't whole blocks: the cell left empty, the grid kept)
			std::printf("  (a 6x6 DXT1 texture wasn't made)\n");
			i++;
		}
		// Linear magnification of the A8R8G8B8 pattern
		IDirect3DTexture8 *t = make_texture(D3DFMT_A8R8G8B8);
		dev->SetTexture(0, t);
		dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		cell(i++);
		quad(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0.5f);
		dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
		dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
		cell(i++);
		quad(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0.25f);
		dev->SetTexture(0, nullptr);
		t->Release();
		// No texture set, a stage that reads one
		cell(i++);
		quad(0xFF80C0FF, 0xFF80C0FF, 0xFF80C0FF, 0xFF80C0FF);
	}

	void scene_ops()
	{
		defaults();
		IDirect3DTexture8 *t0 = make_texture(D3DFMT_A8R8G8B8, 16, 1, 0), *t1 = make_texture(D3DFMT_A8R8G8B8, 16, 1, 2);
		dev->SetTexture(0, t0);
		dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0x80C08040);
		struct op_case { DWORD cop, ca1, ca2, aop, aa1, aa2; };
		const op_case cases[] = {
			{ D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_MODULATE2X, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE4X, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_MODULATE4X, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG2, D3DTA_TEXTURE, D3DTA_TFACTOR },
			{ D3DTOP_ADD, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_ADD, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_ADDSIGNED, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_ADDSIGNED2X, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_SUBTRACT, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SUBTRACT, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_ADDSMOOTH, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_BLENDDIFFUSEALPHA, D3DTA_TEXTURE, D3DTA_TFACTOR, D3DTOP_SELECTARG1, D3DTA_DIFFUSE, D3DTA_DIFFUSE },
			{ D3DTOP_BLENDTEXTUREALPHA, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_BLENDFACTORALPHA, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TFACTOR, D3DTA_DIFFUSE },
			{ D3DTOP_BLENDTEXTUREALPHAPM, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_BLENDCURRENTALPHA, D3DTA_TEXTURE, D3DTA_TFACTOR, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_MODULATEALPHA_ADDCOLOR, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_MODULATECOLOR_ADDALPHA, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_MODULATEINVALPHA_ADDCOLOR, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_MODULATEINVCOLOR_ADDALPHA, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_DOTPRODUCT3, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_SELECTARG1, D3DTA_TEXTURE | D3DTA_COMPLEMENT, D3DTA_DIFFUSE, D3DTOP_SELECTARG1, D3DTA_TEXTURE | D3DTA_COMPLEMENT, D3DTA_DIFFUSE },
			{ D3DTOP_SELECTARG1, D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE, D3DTA_DIFFUSE, D3DTOP_SELECTARG2, D3DTA_TEXTURE, D3DTA_DIFFUSE },
			{ D3DTOP_SELECTARG2, D3DTA_TEXTURE, D3DTA_TFACTOR, D3DTOP_SELECTARG2, D3DTA_TEXTURE, D3DTA_TFACTOR },
			{ D3DTOP_MODULATE, D3DTA_DIFFUSE, D3DTA_TFACTOR, D3DTOP_DISABLE, D3DTA_TEXTURE, D3DTA_DIFFUSE },
		};
		int i = 0;
		for (const op_case &c : cases)
		{
			dev->SetTextureStageState(0, D3DTSS_COLOROP, c.cop);
			dev->SetTextureStageState(0, D3DTSS_COLORARG1, c.ca1);
			dev->SetTextureStageState(0, D3DTSS_COLORARG2, c.ca2);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP, c.aop);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, c.aa1);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, c.aa2);
			cell(i++);
			quad(0xFF204080, 0x80FF8000, 0xC000FF80, 0x40808080);
		}
		// Triadic ops (MULTIPLYADD, LERP)
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MULTIPLYADD);
		dev->SetTextureStageState(0, D3DTSS_COLORARG0, D3DTA_TFACTOR);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		cell(i++);
		quad(0xFF204080, 0x80FF8000, 0xC000FF80, 0x40808080);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_LERP);
		cell(i++);
		quad(0xFF204080, 0x80FF8000, 0xC000FF80, 0x40808080);
		dev->SetTextureStageState(0, D3DTSS_COLORARG0, D3DTA_CURRENT);
		// Two stages: the effects' MODULATE2X then MODULATE4X with TFACTOR (water layers), and the TEMP register
		dev->SetTexture(1, t1);
		const DWORD two[][6] = {
			{ D3DTOP_MODULATE2X, D3DTOP_MODULATE, D3DTA_TFACTOR, D3DTOP_MODULATE4X, D3DTOP_MODULATE, D3DTA_TFACTOR },
			{ D3DTOP_MODULATE, D3DTOP_ADD, D3DTA_TEXTURE, D3DTOP_MODULATE, D3DTOP_ADD, D3DTA_TEXTURE },
			{ D3DTOP_SELECTARG1, D3DTOP_BLENDCURRENTALPHA, D3DTA_TEXTURE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTA_TEXTURE },
		};
		for (const auto &c : two)
		{
			dev->SetTextureStageState(0, D3DTSS_COLOROP, c[0]);
			dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP, c[3]);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
			dev->SetTextureStageState(1, D3DTSS_COLOROP, c[1]);
			dev->SetTextureStageState(1, D3DTSS_COLORARG1, c[2]);
			dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
			dev->SetTextureStageState(1, D3DTSS_ALPHAOP, c[4]);
			dev->SetTextureStageState(1, D3DTSS_ALPHAARG1, c[5]);
			dev->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
			dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 0);
			cell(i++);
			quad(0xFF204080, 0x80FF8000, 0xC000FF80, 0x40808080);
		}
		// TEMP: stage 0 into TEMP, stage 1 the diffuse, stage 2 adds TEMP
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_RESULTARG, 5);
		dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_ADD);
		dev->SetTextureStageState(2, D3DTSS_COLORARG1, 5);
		dev->SetTextureStageState(2, D3DTSS_COLORARG2, D3DTA_CURRENT);
		dev->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(2, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
		cell(i++);
		quad(0xFF204080, 0x80FF8000, 0xC000FF80, 0x40808080);
		// Specular added (pretransformed with a specular colour)
		defaults();
		dev->SetTexture(0, t0);
		dev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
		cell(i++);
		{
			D3DVIEWPORT8 vp;
			dev->GetViewport(&vp);
			const float x0 = float(vp.X) + 2, y0 = float(vp.Y) + 2, x1 = float(vp.X + vp.Width) - 2, y1 = float(vp.Y + vp.Height) - 2;
			const VSpec v[4] = { { x0, y0, 0.5f, 1, 0xFF808080, 0xFF400000, 0, 0 }, { x1, y0, 0.5f, 1, 0xFF808080, 0xFF004000, 1, 0 },
				{ x0, y1, 0.5f, 1, 0xFF808080, 0xFF000040, 0, 1 }, { x1, y1, 0.5f, 1, 0xFF808080, 0xFF404040, 1, 1 } };
			dev->SetVertexShader(kRhwSpec);
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(VSpec));
		}
		dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
		// Flat shading
		dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_FLAT);
		cell(i++);
		quad(0xFF204080, 0x80FF8000, 0xC000FF80, 0x40808080);
		defaults();
		t0->Release();
		t1->Release();
	}

	void scene_blend()
	{
		defaults();
		IDirect3DTexture8 *t = make_texture(D3DFMT_A8R8G8B8);
		struct bcase { DWORD src, dst, op; };
		const bcase cases[] = {
			{ D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD }, { D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_ADD },
			{ D3DBLEND_SRCALPHA, D3DBLEND_ONE, D3DBLENDOP_ADD }, { D3DBLEND_ZERO, D3DBLEND_INVSRCCOLOR, D3DBLENDOP_ADD },
			{ D3DBLEND_SRCALPHA, D3DBLEND_ONE, D3DBLENDOP_REVSUBTRACT }, { D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_SUBTRACT },
			{ D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_MIN }, { D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_MAX },
			{ D3DBLEND_DESTCOLOR, D3DBLEND_ZERO, D3DBLENDOP_ADD }, { D3DBLEND_INVDESTCOLOR, D3DBLEND_SRCCOLOR, D3DBLENDOP_ADD },
			{ D3DBLEND_DESTALPHA, D3DBLEND_INVDESTALPHA, D3DBLENDOP_ADD }, { D3DBLEND_SRCALPHASAT, D3DBLEND_ONE, D3DBLENDOP_ADD },
			{ D3DBLEND_BOTHSRCALPHA, D3DBLEND_ZERO, D3DBLENDOP_ADD }, { D3DBLEND_BOTHINVSRCALPHA, D3DBLEND_ZERO, D3DBLENDOP_ADD },
		};
		int i = 0;
		dev->SetTexture(0, t);
		for (const bcase &c : cases)
		{
			cell(i);
			// (a background to blend over)
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			dev->SetTexture(0, nullptr);
			quad(0x80A04020, 0x40206080, 0xFF808080, 0x00FFFFFF, 1.0f, 0.0f);
			dev->SetTexture(0, t);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
			dev->SetRenderState(D3DRS_SRCBLEND, c.src);
			dev->SetRenderState(D3DRS_DESTBLEND, c.dst);
			dev->SetRenderState(D3DRS_BLENDOP, c.op);
			quad(0xFFFFFFFF, 0xC0FFFFFF, 0x80FFFFFF, 0x40FFFFFF);
			i++;
		}
		dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		// Alpha test: the zone's GREATER 0x60 with alpha x4, the characters' GREATER 0x45, LESS, EQUAL
		const DWORD funcs[][2] = { { D3DCMP_GREATER, 0x60 }, { D3DCMP_GREATER, 0x45 }, { D3DCMP_LESS, 0x80 }, { D3DCMP_GREATEREQUAL, 0x80 }, { D3DCMP_EQUAL, 0xFF }, { D3DCMP_NOTEQUAL, 0xFF } };
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
		for (const auto &f : funcs)
		{
			dev->SetRenderState(D3DRS_ALPHAFUNC, f[0]);
			dev->SetRenderState(D3DRS_ALPHAREF, f[1]);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP, f[1] == 0x60 ? D3DTOP_MODULATE4X : D3DTOP_MODULATE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
			cell(i++);
			quad(0xFFFFFFFF, 0xC0FFFFFF, 0x80FFFFFF, 0x20FFFFFF);
		}
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		// Colour write masks
		for (DWORD m : { 0x1u, 0x6u, 0x8u, 0x7u })
		{
			dev->SetRenderState(D3DRS_COLORWRITEENABLE, m);
			cell(i++);
			quad(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
		}
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		// Filters: point, linear, magnified 8x; min linear on a mipmapped texture with mips off/point/linear
		IDirect3DTexture8 *mip = make_texture(D3DFMT_A8R8G8B8, 64, 0, 1);
		dev->SetTexture(0, mip);
		const DWORD filt[][3] = { { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE }, { D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE },
			{ D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_POINT }, { D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR }, { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_POINT } };
		for (const auto &f : filt)
		{
			dev->SetTextureStageState(0, D3DTSS_MINFILTER, f[0]);
			dev->SetTextureStageState(0, D3DTSS_MAGFILTER, f[1]);
			dev->SetTextureStageState(0, D3DTSS_MIPFILTER, f[2]);
			cell(i++);
			quad(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 6.0f);
		}
		dev->SetTextureStageState(0, D3DTSS_MAXMIPLEVEL, 2);
		cell(i++);
		quad(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 6.0f);
		dev->SetTextureStageState(0, D3DTSS_MAXMIPLEVEL, 0);
		dev->SetTextureStageState(0, D3DTSS_MIPMAPLODBIAS, 0x3F800000); // 1.0
		cell(i++);
		quad(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 3.0f);
		dev->SetTextureStageState(0, D3DTSS_MIPMAPLODBIAS, 0);
		dev->SetTexture(0, nullptr);
		mip->Release();
		t->Release();
	}

	// ---- 3D helpers ----
	D3DMATRIX identity() { D3DMATRIX m = {}; m._11 = m._22 = m._33 = m._44 = 1.0f; return m; }
	D3DMATRIX perspective(float fovy, float aspect, float zn, float zf)
	{
		D3DMATRIX m = {};
		const float ys = 1.0f / std::tan(fovy * 0.5f);
		m._11 = ys / aspect;
		m._22 = ys;
		m._33 = zf / (zf - zn);
		m._34 = 1.0f;
		m._43 = -zn * zf / (zf - zn);
		return m;
	}
	D3DMATRIX translate(float x, float y, float z) { D3DMATRIX m = identity(); m._41 = x; m._42 = y; m._43 = z; return m; }
	D3DMATRIX rot_y(float a) { D3DMATRIX m = identity(); m._11 = std::cos(a); m._13 = -std::sin(a); m._31 = std::sin(a); m._33 = std::cos(a); return m; }
	D3DMATRIX scale(float s) { D3DMATRIX m = identity(); m._11 = m._22 = m._33 = s; return m; }
	D3DMATRIX mul(const D3DMATRIX &a, const D3DMATRIX &b)
	{
		D3DMATRIX r;
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
		return r;
	}

	// A sphere (lat-long), its vertices lit with colours varying
	std::vector<VLit> sphere_vertices;
	std::vector<WORD> sphere_indices;
	void make_sphere()
	{
		if (!sphere_vertices.empty())
			return;
		const int rings = 12, segs = 16;
		for (int r = 0; r <= rings; ++r)
			for (int s = 0; s <= segs; ++s)
			{
				const float th = 3.14159265f * r / rings, ph = 6.2831853f * s / segs;
				const float x = std::sin(th) * std::cos(ph), y = std::cos(th), z = std::sin(th) * std::sin(ph);
				const DWORD c = 0xFF000000 | (DWORD(128 + 100 * x) << 16) | (DWORD(128 + 100 * y) << 8) | DWORD(128 + 100 * z);
				sphere_vertices.push_back({ x, y, z, x, y, z, c, float(s) / segs * 2, float(r) / rings });
			}
		for (int r = 0; r < rings; ++r)
			for (int s = 0; s < segs; ++s)
			{
				const WORD a = WORD(r * (segs + 1) + s), b = WORD(a + segs + 1);
				for (WORD i : { a, b, WORD(a + 1), WORD(a + 1), b, WORD(b + 1) })
					sphere_indices.push_back(i);
			}
	}

	void draw_sphere(const D3DMATRIX &world)
	{
		make_sphere();
		dev->SetTransform(D3DTS_WORLD, &world);
		dev->SetVertexShader(kLit);
		dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, UINT(sphere_vertices.size()), UINT(sphere_indices.size() / 3), sphere_indices.data(), D3DFMT_INDEX16,
			sphere_vertices.data(), sizeof(VLit));
	}

	void camera()
	{
		const D3DMATRIX view = translate(0, 0, 3.0f), proj = perspective(0.9f, 1.0f, 0.1f, 100.0f);
		dev->SetTransform(D3DTS_VIEW, &view);
		dev->SetTransform(D3DTS_PROJECTION, &proj);
	}

	void scene_light()
	{
		defaults();
		camera();
		dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
		dev->SetRenderState(D3DRS_LIGHTING, TRUE);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
		D3DMATERIAL8 m = {};
		m.Diffuse = { 1, 1, 1, 1 };
		m.Ambient = { 0.7f, 0.7f, 0.7f, 1 };
		m.Specular = { 1, 1, 1, 1 };
		m.Emissive = { 0.05f, 0, 0.1f, 0 };
		m.Power = 20;
		dev->SetMaterial(&m);
		D3DLIGHT8 sun = {};
		sun.Type = D3DLIGHT_DIRECTIONAL;
		sun.Diffuse = { 1.259f, 1.247f, 1.1f, 1 };
		sun.Specular = { 1, 1, 1, 1 };
		sun.Direction = { -0.4421f, -0.897f, 0.3f };
		D3DLIGHT8 lamp = {};
		lamp.Type = D3DLIGHT_POINT;
		lamp.Diffuse = { 1.469f, 1.0f, 0.219f, 1 };
		lamp.Ambient = { 0.1f, 0.05f, 0, 0 };
		lamp.Position = { 1.2f, 0.5f, -1.0f };
		lamp.Range = 3.0f;
		lamp.Attenuation2 = 0.5f;
		D3DLIGHT8 spot = {};
		spot.Type = D3DLIGHT_SPOT;
		spot.Diffuse = { 0.2f, 0.6f, 1.0f, 1 };
		spot.Position = { -1.0f, 1.5f, -1.5f };
		spot.Direction = { 0.5f, -0.7f, 0.8f };
		spot.Range = 10.0f;
		spot.Attenuation0 = 1.0f;
		spot.Theta = 0.4f;
		spot.Phi = 0.9f;
		spot.Falloff = 1.0f;
		dev->SetLight(0, &sun);
		dev->SetLight(1, &lamp);
		dev->SetLight(2, &spot);
		struct lcase { int lights; DWORD amb, diffsrc, ambsrc, colorvertex, specular, normalize; float s; };
		const lcase cases[] = {
			{ 1, 0x7D787D, D3DMCS_COLOR1, D3DMCS_COLOR1, TRUE, FALSE, FALSE, 1.0f },     // the zone's fixed path
			{ 1, 0x7B797E, D3DMCS_MATERIAL, D3DMCS_MATERIAL, TRUE, FALSE, FALSE, 1.0f }, // the characters' material
			{ 3, 0x404040, D3DMCS_COLOR1, D3DMCS_MATERIAL, TRUE, FALSE, FALSE, 1.0f },
			{ 3, 0x202020, D3DMCS_MATERIAL, D3DMCS_MATERIAL, FALSE, TRUE, FALSE, 1.0f },
			{ 1, 0x7D787D, D3DMCS_COLOR1, D3DMCS_COLOR1, TRUE, FALSE, FALSE, 0.6f },     // scaled: lit 1/s (normals not renormalized)
			{ 1, 0x7D787D, D3DMCS_COLOR1, D3DMCS_COLOR1, TRUE, FALSE, TRUE, 0.6f },
			{ 2, 0x101010, D3DMCS_MATERIAL, D3DMCS_MATERIAL, FALSE, FALSE, FALSE, 1.0f },
			{ 4, 0x000000, D3DMCS_COLOR1, D3DMCS_COLOR1, TRUE, TRUE, TRUE, 1.0f },
		};
		int i = 0;
		for (const lcase &c : cases)
		{
			dev->LightEnable(0, c.lights == 1 || c.lights == 3 || c.lights == 4);
			dev->LightEnable(1, c.lights == 2 || c.lights == 3 || c.lights == 4);
			dev->LightEnable(2, c.lights >= 3);
			dev->SetRenderState(D3DRS_AMBIENT, c.amb);
			dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, c.diffsrc);
			dev->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, c.ambsrc);
			dev->SetRenderState(D3DRS_COLORVERTEX, c.colorvertex);
			dev->SetRenderState(D3DRS_SPECULARENABLE, c.specular);
			dev->SetRenderState(D3DRS_NORMALIZENORMALS, c.normalize);
			cell(i++, 4);
			draw_sphere(mul(scale(c.s), rot_y(0.3f * i)));
		}
		// Cull modes and the depth test between two spheres; then lighting off with vertex colours
		dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
		cell(i++, 4);
		draw_sphere(identity());
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
		cell(i++, 4);
		draw_sphere(translate(-0.4f, 0, 0));
		draw_sphere(translate(0.4f, 0, 0.3f));
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		cell(i++, 4);
		draw_sphere(identity());
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		for (int l = 0; l < 3; ++l)
			dev->LightEnable(l, FALSE);
	}

	void scene_fog()
	{
		defaults();
		camera();
		dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
		dev->SetRenderState(D3DRS_FOGCOLOR, 0x62636C);
		const float start = 2.5f, end = 6.0f, density = 0.25f;
		DWORD s, e, d;
		std::memcpy(&s, &start, 4);
		std::memcpy(&e, &end, 4);
		std::memcpy(&d, &density, 4);
		dev->SetRenderState(D3DRS_FOGSTART, s);
		dev->SetRenderState(D3DRS_FOGEND, e);
		dev->SetRenderState(D3DRS_FOGDENSITY, d);
		IDirect3DTexture8 *t = make_texture(D3DFMT_DXT3, 16);
		dev->SetTexture(0, t);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		struct fcase { DWORD vmode, tmode, range; };
		const fcase cases[] = { { D3DFOG_LINEAR, D3DFOG_NONE, FALSE }, { D3DFOG_LINEAR, D3DFOG_NONE, TRUE }, { D3DFOG_EXP, D3DFOG_NONE, FALSE }, { D3DFOG_EXP2, D3DFOG_NONE, FALSE },
			{ D3DFOG_NONE, D3DFOG_LINEAR, FALSE }, { D3DFOG_NONE, D3DFOG_EXP, FALSE }, { D3DFOG_NONE, D3DFOG_EXP2, FALSE } };
		// (a long floor going away, so the fog grows across the cell)
		const VLit floor[4] = { { -2, -1, -2, 0, 1, 0, 0xFF808080, 0, 0 }, { 2, -1, -2, 0, 1, 0, 0xFF808080, 4, 0 }, { -2, -1, 12, 0, 1, 0, 0xFF808080, 0, 14 },
			{ 2, -1, 12, 0, 1, 0, 0xFF808080, 4, 14 } };
		int i = 0;
		for (const fcase &c : cases)
		{
			dev->SetRenderState(D3DRS_FOGVERTEXMODE, c.vmode);
			dev->SetRenderState(D3DRS_FOGTABLEMODE, c.tmode);
			dev->SetRenderState(D3DRS_RANGEFOGENABLE, c.range);
			cell(i++, 4);
			const D3DMATRIX w = identity();
			dev->SetTransform(D3DTS_WORLD, &w);
			dev->SetVertexShader(kLit);
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, floor, sizeof(VLit));
			draw_sphere(mul(scale(0.6f), translate(0.8f, 0.0f, float(i) * 0.5f)));
		}
		// Pretransformed: the fog from the specular's alpha
		dev->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
		dev->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		cell(i++, 4);
		{
			D3DVIEWPORT8 vp;
			dev->GetViewport(&vp);
			const float x0 = float(vp.X) + 2, y0 = float(vp.Y) + 2, x1 = float(vp.X + vp.Width) - 2, y1 = float(vp.Y + vp.Height) - 2;
			const VSpec v[4] = { { x0, y0, 0.5f, 1, 0xFFC08040, 0xFF000000, 0, 0 }, { x1, y0, 0.5f, 1, 0xFFC08040, 0x80000000, 1, 0 },
				{ x0, y1, 0.5f, 1, 0xFFC08040, 0x40000000, 0, 1 }, { x1, y1, 0.5f, 1, 0xFFC08040, 0x00000000, 1, 1 } };
			dev->SetVertexShader(kRhwSpec);
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(VSpec));
		}
		// Table fog with pretransformed vertices (by 1/rhw)
		dev->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
		cell(i++, 4);
		{
			D3DVIEWPORT8 vp;
			dev->GetViewport(&vp);
			const float x0 = float(vp.X) + 2, y0 = float(vp.Y) + 2, x1 = float(vp.X + vp.Width) - 2, y1 = float(vp.Y + vp.Height) - 2;
			const VRhw v[4] = { { x0, y0, 0.2f, 1.0f / 1.0f, 0xFFC08040, 0, 0 }, { x1, y0, 0.2f, 1.0f / 3.0f, 0xFFC08040, 1, 0 },
				{ x0, y1, 0.9f, 1.0f / 5.0f, 0xFFC08040, 0, 1 }, { x1, y1, 0.9f, 1.0f / 8.0f, 0xFFC08040, 1, 1 } };
			dev->SetVertexShader(kRhw);
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(VRhw));
		}
		dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		dev->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
		dev->SetTexture(0, nullptr);
		t->Release();
	}

	void scene_texgen()
	{
		defaults();
		camera();
		dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		IDirect3DTexture8 *t = make_texture(D3DFMT_A8R8G8B8, 16);
		dev->SetTexture(0, t);
		int i = 0;
		// UV scroll (COUNT2, the offset in the third row: the effects' 040C97)
		D3DMATRIX tm = identity();
		tm._31 = 0.3f;
		tm._32 = 0.15f;
		tm._11 = 1.5f;
		dev->SetTransform(D3DTS_TEXTURE0, &tm);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
		cell(i++, 4);
		draw_sphere(identity());
		// Projected (COUNT3 | PROJECTED) from camera-space position
		D3DMATRIX pm = identity();
		pm._33 = 0.5f;
		pm._43 = 1.5f;
		dev->SetTransform(D3DTS_TEXTURE0, &pm);
		dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3 | D3DTTFF_PROJECTED);
		cell(i++, 4);
		draw_sphere(identity());
		// Camera-space normal through a matrix, 2 coordinates
		const D3DMATRIX hm = mul(scale(0.5f), translate(0.5f, 0.5f, 0));
		dev->SetTransform(D3DTS_TEXTURE0, &hm);
		dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACENORMAL);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
		cell(i++, 4);
		draw_sphere(identity());
		// Reflection into a cube map (the effects' 1|CAMERASPACEREFLECTIONVECTOR with COUNT3)
		IDirect3DCubeTexture8 *cube = nullptr;
		if (SUCCEEDED(dev->CreateCubeTexture(8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube)))
		{
			const DWORD face_colour[6] = { 0xFFFF4040, 0xFF40FFFF, 0xFF40FF40, 0xFFFF40FF, 0xFF4040FF, 0xFFFFFF40 };
			for (int f = 0; f < 6; ++f)
			{
				D3DLOCKED_RECT lr;
				cube->LockRect(D3DCUBEMAP_FACES(f), 0, &lr, nullptr, 0);
				for (int y = 0; y < 8; ++y)
					for (int x = 0; x < 8; ++x)
						static_cast<DWORD *>(lr.pBits)[y * lr.Pitch / 4 + x] = ((x ^ y) & 2) ? face_colour[f] : (face_colour[f] >> 1) | 0xFF000000;
				cube->UnlockRect(D3DCUBEMAP_FACES(f), 0);
			}
			dev->SetTexture(0, cube);
			const D3DMATRIX id = identity();
			dev->SetTransform(D3DTS_TEXTURE0, &id);
			dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 1 | D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
			dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
			cell(i++, 4);
			draw_sphere(identity());
			dev->SetTexture(0, t);
			cube->Release();
		}
		dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		// Addressing: wrap, mirror, clamp, border; pretransformed with UVs -1..2
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		const DWORD modes[] = { D3DTADDRESS_WRAP, D3DTADDRESS_MIRROR, D3DTADDRESS_CLAMP, D3DTADDRESS_BORDER };
		dev->SetTextureStageState(0, D3DTSS_BORDERCOLOR, 0xFF20C020);
		for (DWORD m : modes)
		{
			dev->SetTextureStageState(0, D3DTSS_ADDRESSU, m);
			dev->SetTextureStageState(0, D3DTSS_ADDRESSV, m);
			cell(i++, 4);
			D3DVIEWPORT8 vp;
			dev->GetViewport(&vp);
			const float x0 = float(vp.X) + 2, y0 = float(vp.Y) + 2, x1 = float(vp.X + vp.Width) - 2, y1 = float(vp.Y + vp.Height) - 2;
			const VRhw v[4] = { { x0, y0, 0.5f, 1, 0xFFFFFFFF, -1, -1 }, { x1, y0, 0.5f, 1, 0xFFFFFFFF, 2, -1 }, { x0, y1, 0.5f, 1, 0xFFFFFFFF, -1, 2 }, { x1, y1, 0.5f, 1, 0xFFFFFFFF, 2, 2 } };
			dev->SetVertexShader(kRhw);
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(VRhw));
		}
		dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
		dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
		// A floor of the mipmapped texture far away: mips off (the zone's MIPFILTER NONE) and linear
		IDirect3DTexture8 *mip = make_texture(D3DFMT_DXT3, 64, 0, 3);
		dev->SetTexture(0, mip);
		dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		const VLit floor[4] = { { -2, -1, -2, 0, 1, 0, 0xFFFFFFFF, 0, 0 }, { 2, -1, -2, 0, 1, 0, 0xFFFFFFFF, 8, 0 }, { -2, -1, 30, 0, 1, 0, 0xFFFFFFFF, 0, 64 },
			{ 2, -1, 30, 0, 1, 0, 0xFFFFFFFF, 8, 64 } };
		for (DWORD mf : { D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_LINEAR })
		{
			dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
			dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
			dev->SetTextureStageState(0, D3DTSS_MIPFILTER, mf);
			cell(i++, 4);
			const D3DMATRIX w = identity();
			dev->SetTransform(D3DTS_WORLD, &w);
			dev->SetVertexShader(kLit);
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, floor, sizeof(VLit));
		}
		dev->SetTexture(0, nullptr);
		mip->Release();
		t->Release();
	}

	void scene_stencil()
	{
		defaults();
		camera();
		int i = 0;
		// The Dancer's fade without overlap: the model into stencil (INCR, ALWAYS), then a quad only where it drew (LESS 0)
		dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
		dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
		dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_INCR);
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
		cell(i, 2);
		draw_sphere(translate(-0.3f, 0, 0));
		draw_sphere(translate(0.3f, 0.2f, 0.5f));
		dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_LESS);
		dev->SetRenderState(D3DRS_STENCILREF, 0);
		dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
		quad(0xC0FF8040, 0xC0FF8040, 0xC0FF8040, 0xC0FF8040, 1.0f, 0.0f);
		// The shadow polygons: EQUAL 0 then INCR, each pixel darkened once
		dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
		dev->SetRenderState(D3DRS_STENCILREF, 0);
		dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_INCR);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
		i++;
		cell(i, 2);
		quad(0xFF808080, 0xFF808080, 0xFF808080, 0xFF808080, 1.0f, 10.0f);
		quad(0xFF808080, 0xFF808080, 0xFF808080, 0xFF808080, 1.0f, 20.0f);
		dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		// ZBIAS 8 on a coplanar blended piece (the zone's), and the depth functions
		i++;
		cell(i, 2);
		dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		const VLit wall[4] = { { -1, -1, 1, 0, 0, -1, 0xFF2060C0, 0, 0 }, { 1, -1, 2, 0, 0, -1, 0xFF2060C0, 1, 0 }, { -1, 1, 1, 0, 0, -1, 0xFF2060C0, 0, 1 }, { 1, 1, 2, 0, 0, -1, 0xFF2060C0, 1, 1 } };
		VLit over[4];
		std::memcpy(over, wall, sizeof(wall));
		for (VLit &v : over)
			v.c = 0xFFC06020;
		const D3DMATRIX w = identity();
		dev->SetTransform(D3DTS_WORLD, &w);
		dev->SetVertexShader(kLit);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, wall, sizeof(VLit));
		dev->SetRenderState(D3DRS_ZBIAS, 8);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, over, sizeof(VLit));
		dev->SetRenderState(D3DRS_ZBIAS, 0);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		i++;
		cell(i, 2);
		draw_sphere(identity());
		const DWORD funcs[] = { D3DCMP_GREATER, D3DCMP_EQUAL, D3DCMP_LESS };
		const D3DCOLOR cols[] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF };
		for (int f = 0; f < 3; ++f)
		{
			dev->SetRenderState(D3DRS_ZFUNC, funcs[f]);
			dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
			dev->SetRenderState(D3DRS_TEXTUREFACTOR, cols[f]);
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
			draw_sphere(mul(scale(0.7f + 0.15f * f), translate(0.1f * f, 0, -0.2f * f)));
		}
		dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	}

	void scene_streams()
	{
		defaults();
		camera();
		dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		dev->SetRenderState(D3DRS_LIGHTING, TRUE);
		D3DLIGHT8 sun = {};
		sun.Type = D3DLIGHT_DIRECTIONAL;
		sun.Diffuse = { 1.241f, 1.235f, 1.118f, 1 };
		sun.Direction = { -0.3f, -0.8f, 0.5f };
		dev->SetLight(0, &sun);
		dev->LightEnable(0, TRUE);
		dev->SetRenderState(D3DRS_AMBIENT, 0x7B797E);
		D3DMATERIAL8 m = {};
		m.Diffuse = { 0.5f, 0.5f, 0.5f, 1 };
		m.Ambient = { 1, 1, 1, 1 };
		m.Power = 10;
		dev->SetMaterial(&m);
		dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
		dev->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
		IDirect3DTexture8 *t = make_texture(D3DFMT_DXT3, 16);
		dev->SetTexture(0, t);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		// The characters' declaration: streams 0/1/2 = position, normal, UV, no shader code
		make_sphere();
		const DWORD decl[] = { D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3), D3DVSD_STREAM(1), D3DVSD_REG(D3DVSDE_NORMAL, D3DVSDT_FLOAT3),
			D3DVSD_STREAM(2), D3DVSD_REG(D3DVSDE_TEXCOORD0, D3DVSDT_FLOAT2), D3DVSD_END() };
		DWORD handle = 0;
		check(SUCCEEDED(dev->CreateVertexShader(decl, nullptr, &handle, 0)), "CreateVertexShader (declaration, no code)");
		const UINT n = UINT(sphere_vertices.size());
		IDirect3DVertexBuffer8 *pos = nullptr, *nrm = nullptr, *uv = nullptr;
		IDirect3DIndexBuffer8 *ib = nullptr;
		dev->CreateVertexBuffer(n * 12, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &pos);
		dev->CreateVertexBuffer(n * 12, D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &nrm);
		dev->CreateVertexBuffer(n * 8, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &uv);
		dev->CreateIndexBuffer(UINT(sphere_indices.size()) * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib);
		float *p = nullptr, *q = nullptr, *r = nullptr;
		pos->Lock(0, 0, reinterpret_cast<BYTE **>(&p), 0);
		nrm->Lock(0, 0, reinterpret_cast<BYTE **>(&q), D3DLOCK_DISCARD);
		uv->Lock(0, 0, reinterpret_cast<BYTE **>(&r), 0);
		for (UINT k = 0; k < n; ++k)
		{
			const VLit &v = sphere_vertices[k];
			p[k * 3] = v.x; p[k * 3 + 1] = v.y; p[k * 3 + 2] = v.z;
			q[k * 3] = v.nx; q[k * 3 + 1] = v.ny; q[k * 3 + 2] = v.nz;
			r[k * 2] = v.u; r[k * 2 + 1] = v.v;
		}
		pos->Unlock();
		nrm->Unlock();
		uv->Unlock();
		WORD *ix = nullptr;
		ib->Lock(0, 0, reinterpret_cast<BYTE **>(&ix), 0);
		std::memcpy(ix, sphere_indices.data(), sphere_indices.size() * 2);
		ib->Unlock();
		dev->SetVertexShader(handle);
		dev->SetStreamSource(0, pos, 12);
		dev->SetStreamSource(1, nrm, 12);
		dev->SetStreamSource(2, uv, 8);
		dev->SetIndices(ib, 0);
		int i = 0;
		cell(i++, 4);
		D3DMATRIX w = identity();
		dev->SetTransform(D3DTS_WORLD, &w);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, n, 0, UINT(sphere_indices.size() / 3));
		// Half the sphere again from index 96 on, with the stream offsets moved by a base vertex of 0 (indices as given)
		cell(i++, 4);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, n, 96, UINT(sphere_indices.size() / 3) / 2);
		// The dynamic normal buffer rewritten (as the game's CPU skinning): DISCARD, then drawn again
		nrm->Lock(0, 0, reinterpret_cast<BYTE **>(&q), D3DLOCK_DISCARD);
		for (UINT k = 0; k < n; ++k)
		{
			q[k * 3] = -sphere_vertices[k].nx;
			q[k * 3 + 1] = -sphere_vertices[k].ny;
			q[k * 3 + 2] = -sphere_vertices[k].nz;
		}
		nrm->Unlock();
		cell(i++, 4);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, n, 0, UINT(sphere_indices.size() / 3));
		// The managed position buffer locked again (part of it) and drawn
		pos->Lock(0, 12 * 40, reinterpret_cast<BYTE **>(&p), 0);
		for (UINT k = 0; k < 40; ++k)
			p[k * 3 + 1] *= 1.4f;
		pos->Unlock();
		cell(i++, 4);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, n, 0, UINT(sphere_indices.size() / 3));
		dev->SetStreamSource(1, nullptr, 0);
		dev->SetStreamSource(2, nullptr, 0);
		// Vertex blending: two matrices, a weight per vertex
		struct VB1 { float x, y, z, w0, nx, ny, nz; DWORD c; float u, v; };
		std::vector<VB1> bv;
		for (const VLit &v : sphere_vertices)
			bv.push_back({ v.x, v.y, v.z, (v.y + 1) * 0.5f, v.nx, v.ny, v.nz, v.c, v.u, v.v });
		const D3DMATRIX w0 = translate(-0.3f, 0, 0), w1 = mul(rot_y(0.8f), translate(0.4f, 0.3f, 0));
		dev->SetTransform(D3DTS_WORLDMATRIX(0), &w0);
		dev->SetTransform(D3DTS_WORLDMATRIX(1), &w1);
		dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS);
		dev->SetVertexShader(D3DFVF_XYZB1 | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		cell(i++, 4);
		dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, n, UINT(sphere_indices.size() / 3), sphere_indices.data(), D3DFMT_INDEX16, bv.data(), sizeof(VB1));
		dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
		dev->SetTransform(D3DTS_WORLDMATRIX(1), &w);
		dev->SetTransform(D3DTS_WORLD, &w);
		// Fans, lines and points (pretransformed, unlit)
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		dev->SetTexture(0, nullptr);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		cell(i++, 4);
		{
			D3DVIEWPORT8 vp;
			dev->GetViewport(&vp);
			const float cx = vp.X + vp.Width * 0.5f, cy = vp.Y + vp.Height * 0.5f;
			VRhw fan[10];
			fan[0] = { cx, cy, 0.5f, 1, 0xFFFFFFFF, 0, 0 };
			for (int k = 1; k < 10; ++k)
			{
				const float a = 0.7f * k;
				fan[k] = { cx + 25 * std::cos(a), cy + 25 * std::sin(a), 0.5f, 1, DWORD(0xFF000000 | (k * 0x1C0F07)), 0, 0 };
			}
			dev->SetVertexShader(kRhw);
			dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 8, fan, sizeof(VRhw));
			const WORD fi[6] = { 0, 9, 8, 7, 6, 5 };
			for (VRhw &v : fan)
				v.x -= 12;
			dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLEFAN, 0, 10, 4, fi, D3DFMT_INDEX16, fan, sizeof(VRhw));
		}
		cell(i++, 4);
		{
			D3DVIEWPORT8 vp;
			dev->GetViewport(&vp);
			VRhw lines[16];
			for (int k = 0; k < 16; ++k)
				lines[k] = { float(vp.X) + 4 + 3.5f * k, float(vp.Y) + 4 + float((k * 7) % 50), 0.5f, 1, DWORD(0xFF000000 | (k * 0x0F1C27)), 0, 0 };
			dev->SetVertexShader(kRhw);
			dev->DrawPrimitiveUP(D3DPT_LINELIST, 8, lines, sizeof(VRhw));
			for (VRhw &v : lines)
				v.y += 10;
			dev->DrawPrimitiveUP(D3DPT_LINESTRIP, 15, lines, sizeof(VRhw));
			for (VRhw &v : lines)
				v.y -= 20;
			dev->DrawPrimitiveUP(D3DPT_POINTLIST, 16, lines, sizeof(VRhw));
		}
		dev->SetVertexShader(kRhw);
		dev->SetIndices(nullptr, 0);
		dev->SetStreamSource(0, nullptr, 0);
		dev->DeleteVertexShader(handle);
		pos->Release();
		nrm->Release();
		uv->Release();
		ib->Release();
		t->Release();
		dev->LightEnable(0, FALSE);
	}

	// ---- Shaders ----
	std::vector<uint8_t> dump;
	std::string shader_text(uint32_t table)
	{
		if (dump.empty())
		{
			FILE *f = std::fopen("C:\\Games\\PhoenixXI\\FFXiMain_04490000.bin", "rb");
			if (f == nullptr)
				return std::string();
			std::fseek(f, 0, SEEK_END);
			dump.resize(size_t(std::ftell(f)));
			std::fseek(f, 0, SEEK_SET);
			if (std::fread(dump.data(), 1, dump.size(), f) != dump.size())
				dump.clear();
			std::fclose(f);
		}
		std::string text;
		for (uint32_t i = 0; table + 4 * i + 4 <= dump.size(); ++i)
		{
			uint32_t va = 0;
			std::memcpy(&va, dump.data() + table + 4 * i, 4);
			const uint32_t o = va - 0x04490000u;
			if (va == 0 || o >= dump.size())
				break;
			text += reinterpret_cast<const char *>(dump.data() + o);
			text += "\n";
		}
		return text;
	}

	DWORD assemble_vs(const std::string &text, const DWORD *decl, const char *label)
	{
		LPD3DXBUFFER code = nullptr, errors = nullptr;
		if (FAILED(D3DXAssembleShader(text.c_str(), UINT(text.size()), 0, nullptr, &code, &errors)))
		{
			std::printf("  vs %s didn't assemble: %s\n", label, errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
			failures++;
			return 0;
		}
		DWORD h = 0;
		check(SUCCEEDED(dev->CreateVertexShader(decl, static_cast<const DWORD *>(code->GetBufferPointer()), &h, 0)), "CreateVertexShader (vs.1.1)");
		code->Release();
		return h;
	}

	DWORD assemble_ps(const std::string &text, const char *label)
	{
		LPD3DXBUFFER code = nullptr, errors = nullptr;
		if (FAILED(D3DXAssembleShader(text.c_str(), UINT(text.size()), 0, nullptr, &code, &errors)))
		{
			std::printf("  ps %s didn't assemble: %s\n", label, errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "?");
			failures++;
			return 0;
		}
		DWORD h = 0;
		check(SUCCEEDED(dev->CreatePixelShader(static_cast<const DWORD *>(code->GetBufferPointer()), &h)), "CreatePixelShader");
		code->Release();
		return h;
	}

	void transpose_into(float *dst, const D3DMATRIX &m)
	{
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
				dst[r * 4 + c] = m.m[c][r];
	}

	void scene_shaders()
	{
		defaults();
		camera();
		dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		make_sphere();
		const D3DMATRIX view = translate(0, 0, 3.0f), proj = perspective(0.9f, 1.0f, 0.1f, 100.0f);
		int i = 0;
		IDirect3DTexture8 *colour = make_texture(D3DFMT_DXT3, 16, 1, 0), *bump = make_texture(D3DFMT_A8R8G8B8, 16, 1, 1);
		// The game's zone shaders: vs.1.1 with 0..3 lamps (tables 0x376FF8 ..: handles 0x5..0xB in the captures) and ps.1.1 (0x3772C4), its declaration
		// v0 position, v3 normal, v5 colour, v7 UV, v15 tangent, its constants (notes-zone.md)
		const DWORD zone_decl[] = { D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3), D3DVSD_REG(3, D3DVSDT_FLOAT3), D3DVSD_REG(5, D3DVSDT_D3DCOLOR),
			D3DVSD_REG(7, D3DVSDT_FLOAT2), D3DVSD_REG(15, D3DVSDT_FLOAT3), D3DVSD_END() };
		struct VZ { float x, y, z, nx, ny, nz; DWORD c; float u, v, tx, ty, tz; };
		std::vector<VZ> zv;
		for (const VLit &v : sphere_vertices)
		{
			// (a tangent across the normal)
			float tx = -v.nz, ty = 0, tz = v.nx;
			const float l = std::sqrt(tx * tx + tz * tz);
			if (l > 0)
			{
				tx /= l;
				tz /= l;
			}
			else
				tx = 1;
			zv.push_back({ v.x, v.y, v.z, v.nx, v.ny, v.nz, v.c, v.u, v.v, tx, ty, tz });
		}
		const uint32_t tables[5] = { 0x376FF8, 0x377048, 0x3770D0, 0x377168, 0x377210 };
		const std::string ps_text = shader_text(0x3772C4);
		const DWORD zone_ps = ps_text.empty() ? 0 : assemble_ps(ps_text, "zone");
		if (ps_text.empty())
			std::printf("  (no FFXiMain dump: the game's zone shaders left out)\n");
		for (int lamps = 0; lamps < 4 && zone_ps != 0; ++lamps)
		{
			const DWORD vs = assemble_vs(shader_text(tables[lamps]), zone_decl, std::to_string(lamps).c_str());
			if (vs == 0)
				continue;
			const D3DMATRIX world = rot_y(0.4f * lamps);
			float c[19][4] = {};
			transpose_into(&c[0][0], mul(mul(world, view), proj));
			// c4 toward the sun in object space, c5 0, c6 sun, c7 ambient, c8 0.5, c9 fog, c10 1, c11.. lamps, c15-c17 colours, c18 range^2
			const float sun[3] = { 0.4421f, 0.897f, -0.3f };
			c[4][0] = sun[0] * std::cos(0.4f * lamps) + sun[2] * std::sin(0.4f * lamps);
			c[4][1] = sun[1];
			c[4][2] = -sun[0] * std::sin(0.4f * lamps) + sun[2] * std::cos(0.4f * lamps);
			c[6][0] = 1.259f; c[6][1] = 1.247f; c[6][2] = 1.1f; c[6][3] = 1;
			c[7][0] = 0.488f; c[7][1] = 0.472f; c[7][2] = 0.49f; c[7][3] = 1;
			c[8][0] = c[8][1] = c[8][2] = c[8][3] = 0.5f;
			c[9][0] = 1.0f / (5.0f - 2.5f); c[9][2] = 5.0f;
			c[10][0] = c[10][1] = c[10][2] = c[10][3] = 1.0f;
			for (int l = 0; l < 4; ++l)
			{
				c[11 + l][0] = 1.2f * std::cos(1.7f * l);
				c[11 + l][1] = 0.6f;
				c[11 + l][2] = 1.2f * std::sin(1.7f * l);
			}
			for (int l = 0; l < 4; ++l)
			{
				c[15][l] = 2.938f * (l == 0 ? 1.0f : 0.5f);
				c[16][l] = 2.0f * (l == 1 ? 1.0f : 0.4f);
				c[17][l] = 0.4375f * (l == 2 ? 2.0f : 1.0f);
				c[18][l] = 4.0f;
			}
			dev->SetVertexShaderConstant(0, c, 19);
			dev->SetVertexShader(vs);
			dev->SetPixelShader(zone_ps);
			dev->SetTexture(0, colour);
			dev->SetTexture(1, bump);
			dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
			dev->SetRenderState(D3DRS_FOGCOLOR, 0x62636C);
			cell(i++, 4);
			dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, UINT(zv.size()), UINT(sphere_indices.size() / 3), sphere_indices.data(), D3DFMT_INDEX16, zv.data(), sizeof(VZ));
			dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		}
		dev->SetPixelShader(0);
		dev->SetTexture(1, nullptr);
		// A vs.1.1 shader with the rest of the instructions, relative addressing and the declaration's constants
		const char *vs_text =
			"vs.1.1\n"
			"m4x4 oPos, v0, c0\n"
			"mov r0, v3\n"
			"dp3 r1.x, r0, c4\n"
			"max r1.x, r1.x, c5.x\n"
			"rsq r2.x, c6.x\n"
			"rcp r2.y, c6.y\n"
			"mul r3, c7, r1.x\n"
			"mad r3, r3, r2.xxxx, c8\n"
			"min r3, r3, c10\n"
			"slt r4, v0, c5\n"
			"sge r5, v0, c5\n"
			"mad r3.xyz, r4, c11, r3\n"
			"expp r6, c6.z\n"
			"logp r7, c6.w\n"
			"lit r8, c12\n"
			"dst r9, c13, c14\n"
			"frc r10, v7\n"
			"mul r11.x, v0.y, c15.x\n"
			"mov a0.x, r11.x\n"
			"add r3.xyz, r3, c[a0.x + 16]\n"
			"mov oD0, r3\n"
			"mul oD1, r6.xyzw, c15.yyyy\n"
			"mad oT0.xy, v7, c9.xy, r10.xy\n"
			"m3x3 r1.xyz, v3, c20\n"
			"mov oT1.xyz, r1\n"
			"mov oT1.w, r8.z\n"
			"dp4 oFog.x, v0, c23\n"
			"add oT2, r7, r9\n";
		const DWORD tdecl[] = { D3DVSD_STREAM(0), D3DVSD_REG(0, D3DVSDT_FLOAT3), D3DVSD_REG(3, D3DVSDT_FLOAT3), D3DVSD_REG(5, D3DVSDT_D3DCOLOR),
			D3DVSD_REG(7, D3DVSDT_FLOAT2), DWORD(D3DVSD_CONST(16, 2)), 0x3E000000, 0, 0, 0, 0, 0x3E000000, 0, 0, D3DVSD_END() };
		const DWORD tvs = assemble_vs(vs_text, tdecl, "test");
		float c[24][4] = {};
		transpose_into(&c[0][0], mul(view, proj));
		c[4][0] = 0.3f; c[4][1] = 0.8f; c[4][2] = -0.5f;
		c[6][0] = 4.0f; c[6][1] = 2.0f; c[6][2] = 1.5f; c[6][3] = 8.0f;
		c[7][0] = 0.9f; c[7][1] = 0.7f; c[7][2] = 0.5f; c[7][3] = 1.0f;
		c[8][0] = 0.1f; c[8][1] = 0.1f; c[8][2] = 0.2f; c[8][3] = 0.0f;
		c[9][0] = 2.0f; c[9][1] = 3.0f;
		c[10][0] = c[10][1] = c[10][2] = c[10][3] = 1.0f;
		c[11][0] = 0.2f; c[11][1] = -0.1f; c[11][2] = 0.3f;
		c[12][0] = 0.7f; c[12][1] = 0.5f; c[12][3] = 3.0f;
		c[13][1] = 2.0f; c[13][2] = 3.0f;
		c[14][1] = 0.5f; c[14][3] = 4.0f;
		c[15][0] = 1.0f; c[15][1] = 0.25f;
		transpose_into(&c[20][0], rot_y(0.5f));
		c[23][2] = -0.1f; c[23][3] = 1.0f;
		if (tvs != 0)
		{
			dev->SetVertexShaderConstant(0, c, 16);
			dev->SetVertexShaderConstant(20, c[20], 4);
			dev->SetVertexShader(tvs);
			dev->SetTexture(0, colour);
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
			dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_SPECULAR);
			dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
			cell(i++, 4);
			dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, UINT(sphere_vertices.size()), UINT(sphere_indices.size() / 3), sphere_indices.data(), D3DFMT_INDEX16,
				sphere_vertices.data(), sizeof(VLit));
			dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		}
		// ps.1.1-1.4 over a fixed-function vertex path
		dev->SetVertexShader(kLit);
		const D3DMATRIX w = identity();
		dev->SetTransform(D3DTS_WORLD, &w);
		dev->SetTexture(0, colour);
		dev->SetTexture(1, bump);
		const float pc[8][4] = { { 0.5f, 0.25f, 0.75f, 0.5f }, { 1, 0, 0, 1 }, { 0, 1, 0.5f, 0.3f }, { 0.2f, 0.2f, 0.2f, 1 }, {}, {}, {}, {} };
		dev->SetPixelShaderConstant(0, pc, 8);
		const char *ps_texts[] = {
			"ps.1.1\ntex t0\ntex t1\nmul_x2 r0, t0, v0\nlrp r1.rgb, c0, t1, r0\n+mul r1.a, t0.a, v0.a\nmad_sat r0.rgb, r1, c2, t1_bias\n+mov r0.a, r1.a\n",
			"ps.1.1\ntex t0\ntexcoord t1\ndp3_sat r1, t1_bx2, v1_bx2\nmul r0, t0, r1\ncnd r0, r0.a, r0, c1\n",
			"ps.1.3\ntex t0\ntex t1\nsub r0, t0, t1\ncmp r0, r0, t0, 1-t1\ndp4 r1, t0, c0\nmul_d2 r0.rgb, r0, r1\n+mov r0.a, c0.a\n",
			"ps.1.4\ntexld r0, t0\ntexcrd r1.rgb, t1\nmad r0.rgb, r1, c2, r0\nphase\ntexld r1, r0\nmul r0, r0, v0\nlrp r0, c0.a, r0, r1\n",
		};
		for (const char *text : ps_texts)
		{
			const DWORD ps = assemble_ps(text, text);
			if (ps == 0)
				continue;
			dev->SetPixelShader(ps);
			cell(i++, 4);
			draw_sphere(rot_y(0.3f * i));
			dev->SetPixelShader(0);
		}
		dev->SetTexture(0, nullptr);
		dev->SetTexture(1, nullptr);
		colour->Release();
		bump->Release();
	}

	// ---- An Ashita-like ImGui DX8 frame ----
	void scene_imgui()
	{
		defaults();
		// The game's state, as Ashita finds it, captured whole and put back after
		DWORD block = 0;
		check(SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &block)), "CreateStateBlock(ALL)");
		// The font atlas: a managed A8R8G8B8 texture, alpha the glyphs; one row rewritten later (a partial lock)
		IDirect3DTexture8 *font = nullptr;
		dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &font);
		D3DLOCKED_RECT lr;
		font->LockRect(0, &lr, nullptr, 0);
		for (int y = 0; y < 64; ++y)
			for (int x = 0; x < 64; ++x)
				static_cast<DWORD *>(lr.pBits)[y * lr.Pitch / 4 + x] = (((x / 2) ^ (y / 3)) & 1) ? 0xFFFFFFFF : 0x00FFFFFF;
		font->UnlockRect(0);
		const RECT row = { 0, 10, 64, 14 };
		font->LockRect(0, &lr, &row, 0);
		for (int y = 0; y < 4; ++y)
			for (int x = 0; x < 64; ++x)
				static_cast<DWORD *>(lr.pBits)[y * lr.Pitch / 4 + x] = 0x80FFFFFF;
		font->UnlockRect(0);
		// Dynamic buffers in the default pool, DISCARD each frame
		IDirect3DVertexBuffer8 *vb = nullptr;
		IDirect3DIndexBuffer8 *ib = nullptr;
		struct VI { float x, y, z; DWORD c; float u, v; };
		const DWORD fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;
		dev->CreateVertexBuffer(4096 * sizeof(VI), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, fvf, D3DPOOL_DEFAULT, &vb);
		dev->CreateIndexBuffer(8192 * 2, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib);
		VI *v = nullptr;
		WORD *ix = nullptr;
		vb->Lock(0, 0, reinterpret_cast<BYTE **>(&v), D3DLOCK_DISCARD);
		ib->Lock(0, 0, reinterpret_cast<BYTE **>(&ix), D3DLOCK_DISCARD);
		// Three windows: a frame quad and 12 glyph quads each
		UINT nv = 0, ni = 0;
		struct cmd { UINT first_index, count, base; RECT clip; };
		std::vector<cmd> cmds;
		for (int wdw = 0; wdw < 3; ++wdw)
		{
			const UINT base = nv;
			const float ox = 10.0f + 80.0f * wdw, oy = 20.0f + 60.0f * wdw;
			auto add_quad = [&](float x0, float y0, float x1, float y1, DWORD c, float u0, float v0, float u1, float v1) {
				const WORD b = WORD(nv - base);
				v[nv++] = { x0, y0, 0, c, u0, v0 };
				v[nv++] = { x1, y0, 0, c, u1, v0 };
				v[nv++] = { x1, y1, 0, c, u1, v1 };
				v[nv++] = { x0, y1, 0, c, u0, v1 };
				for (WORD k : { WORD(0), WORD(1), WORD(2), WORD(0), WORD(2), WORD(3) })
					ix[ni++] = WORD(b + k);
			};
			const UINT first = ni;
			add_quad(ox, oy, ox + 120, oy + 90, 0xE0202838, 0, 0, 0, 0);
			for (int g = 0; g < 12; ++g)
				add_quad(ox + 4 + 9.0f * g, oy + 6 + (g % 3) * 20, ox + 12 + 9.0f * g, oy + 18 + (g % 3) * 20, 0xFFFFE0A0, g / 12.0f, 0, (g + 1) / 12.0f, 0.25f);
			cmds.push_back({ first, (ni - first) / 3, base, { LONG(ox), LONG(oy), LONG(ox + 100), LONG(oy + 70) } });
		}
		vb->Unlock();
		ib->Unlock();
		// The backend's state: no lighting, no depth, alpha blending, a projection over the screen
		dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		dev->SetStreamSource(0, vb, sizeof(VI));
		dev->SetVertexShader(fvf);
		dev->SetPixelShader(0);
		const D3DMATRIX id = identity();
		dev->SetTransform(D3DTS_WORLD, &id);
		dev->SetTransform(D3DTS_VIEW, &id);
		// Each command clipped by a viewport of its clip rect, the projection moved to match (no scissor in Direct3D 8)
		for (const cmd &c : cmds)
		{
			const RECT &r = c.clip;
			D3DVIEWPORT8 vp = { DWORD(r.left), DWORD(r.top), DWORD(r.right - r.left), DWORD(r.bottom - r.top), 0.0f, 1.0f };
			dev->SetViewport(&vp);
			D3DMATRIX p = identity();
			const float L = float(r.left) + 0.5f, R = float(r.right) + 0.5f, T = float(r.top) + 0.5f, B = float(r.bottom) + 0.5f;
			p._11 = 2.0f / (R - L);
			p._22 = 2.0f / (T - B);
			p._41 = (L + R) / (L - R);
			p._42 = (T + B) / (B - T);
			dev->SetTransform(D3DTS_PROJECTION, &p);
			dev->SetTexture(0, font);
			dev->SetIndices(ib, c.base);
			dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 52, c.first_index, c.count);
		}
		// NOOVERWRITE: more quads appended after the drawn ones, drawn too
		vb->Lock(UINT(nv * sizeof(VI)), 4 * sizeof(VI), reinterpret_cast<BYTE **>(&v), D3DLOCK_NOOVERWRITE);
		v[0] = { 180, 180, 0, 0xC080FF80, 0, 0 };
		v[1] = { 250, 180, 0, 0xC080FF80, 1, 0 };
		v[2] = { 250, 250, 0, 0xC080FF80, 1, 1 };
		v[3] = { 180, 250, 0, 0xC080FF80, 0, 1 };
		vb->Unlock();
		D3DVIEWPORT8 full = { 0, 0, W, H, 0.0f, 1.0f };
		dev->SetViewport(&full);
		D3DMATRIX p = identity();
		p._11 = 2.0f / W;
		p._22 = -2.0f / H;
		p._41 = -1.0f - 1.0f / W;
		p._42 = 1.0f + 1.0f / H;
		dev->SetTransform(D3DTS_PROJECTION, &p);
		dev->SetIndices(ib, nv);
		dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 4, 0, 2);
		// The game's state back
		dev->ApplyStateBlock(block);
		dev->DeleteStateBlock(block);
		DWORD z = 0;
		dev->GetRenderState(D3DRS_ZENABLE, &z);
		check(z == FALSE, "the state block put ZENABLE back");
		dev->SetStreamSource(0, nullptr, 0);
		dev->SetIndices(nullptr, 0);
		dev->SetTexture(0, nullptr);
		vb->Release();
		ib->Release();
		font->Release();
	}

	// ---- The visibility tests' readbacks ----
	void scene_readback()
	{
		defaults();
		// A world with depth, then 6 tests: a quad into a test target with the world's depth, a 16x16 downsample, read back
		IDirect3DTexture8 *test_tex = nullptr, *tiny_tex = nullptr;
		IDirect3DSurface8 *test = nullptr, *tiny = nullptr, *sys = nullptr;
		dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &test_tex);
		test_tex->GetSurfaceLevel(0, &test);
		dev->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tiny_tex);
		tiny_tex->GetSurfaceLevel(0, &tiny);
		dev->CreateImageSurface(16, 16, D3DFMT_A8R8G8B8, &sys);
		camera();
		dev->SetRenderState(D3DRS_ZENABLE, TRUE);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
		draw_sphere(scale(0.8f));
		dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		uint32_t counts[6] = {};
		for (int k = 0; k < 6; ++k)
		{
			dev->SetRenderTarget(test, depth);
			dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
			dev->SetRenderState(D3DRS_ZENABLE, TRUE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
			D3DVIEWPORT8 vp = { 0, 0, W, H, 0.0f, 1.0f };
			dev->SetViewport(&vp);
			cell(k * 9, 8);
			// (at the depth of the sphere's front: half hidden)
			quad(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 1.0f, 0.0f, 0.95f + 0.008f * k);
			dev->SetRenderState(D3DRS_ZENABLE, FALSE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
			dev->SetRenderTarget(tiny, nullptr);
			dev->SetTexture(0, test_tex);
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
			const VRhw q[4] = { { -0.5f, -0.5f, 0, 1, 0xFFFFFFFF, 0, 0 }, { 15.5f, -0.5f, 0, 1, 0xFFFFFFFF, 1, 0 }, { -0.5f, 15.5f, 0, 1, 0xFFFFFFFF, 0, 1 }, { 15.5f, 15.5f, 0, 1, 0xFFFFFFFF, 1, 1 } };
			dev->SetVertexShader(kRhw);
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(VRhw));
			dev->SetTexture(0, nullptr);
			const std::vector<uint32_t> px = read_target(tiny, 16, 16, sys);
			for (uint32_t c : px)
				counts[k] += (c >> 24) >= 0x80 ? 1 : 0;
			dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
			dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
		}
		std::printf("  visibility tests' counts: %u %u %u %u %u %u\n", counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
		// The counts drawn into the picture as bars, so the comparison sees them
		dev->SetRenderTarget(target, depth);
		for (int k = 0; k < 6; ++k)
		{
			D3DVIEWPORT8 vp = { 8, DWORD(8 + 20 * k), std::max<DWORD>(1, counts[k]), 14, 0.0f, 1.0f };
			dev->SetViewport(&vp);
			quad(0xFFFFFF00, 0xFFFFFF00, 0xFFFFFF00, 0xFFFFFF00, 1.0f, 0.0f);
		}
		D3DVIEWPORT8 vp = { 0, 0, W, H, 0.0f, 1.0f };
		dev->SetViewport(&vp);
		sys->Release();
		tiny->Release();
		tiny_tex->Release();
		test->Release();
		test_tex->Release();
	}

	struct scene
	{
		const char *name;
		void (*fn)();
	};
	const scene kScenes[] = { { "formats", scene_formats }, { "ops", scene_ops }, { "blend", scene_blend }, { "light", scene_light }, { "fog", scene_fog },
		{ "texgen", scene_texgen }, { "stencil", scene_stencil }, { "streams", scene_streams }, { "shaders", scene_shaders }, { "imgui", scene_imgui },
		{ "readback", scene_readback } };

	int run_scenes(const std::string &out, const char *only)
	{
		CreateDirectoryA(out.c_str(), nullptr);
		for (const scene &s : kScenes)
		{
			if (only != nullptr && std::strcmp(only, s.name) != 0)
				continue;
			// (three frames, the last read: anything a first frame does differently is past)
			std::vector<uint32_t> px;
			for (int frame = 0; frame < 3; ++frame)
			{
				begin_scene();
				s.fn();
				dev->EndScene();
				if (frame == 2)
					px = read_target(target, W, H, readback_surface);
				// (the back buffer shows the target, for a look while it runs)
				IDirect3DSurface8 *bb = nullptr;
				dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
				dev->SetRenderTarget(bb, nullptr);
				dev->BeginScene();
				dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
				defaults();
				dev->SetTexture(0, target_tex);
				D3DVIEWPORT8 vp = { 0, 0, W, H, 0.0f, 1.0f };
				dev->SetViewport(&vp);
				quad(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 1.0f, 0.0f);
				dev->SetTexture(0, nullptr);
				dev->EndScene();
				bb->Release();
				dev->Present(nullptr, nullptr, nullptr, nullptr);
				MSG msg;
				while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
					DispatchMessageA(&msg);
			}
			const std::string path = out + "\\" + s.name + ".bmp";
			write_bmp(path, W, H, px);
			std::printf("%s -> %s\n", s.name, path.c_str());
		}
		return failures;
	}

	int run_caps()
	{
		D3DCAPS8 c = {};
		dev->GetDeviceCaps(&c);
		std::printf("Caps 0x%08lX Caps2 0x%08lX Caps3 0x%08lX PresentationIntervals 0x%08lX CursorCaps 0x%08lX\n", c.Caps, c.Caps2, c.Caps3, c.PresentationIntervals, c.CursorCaps);
		std::printf("DevCaps 0x%08lX PrimitiveMiscCaps 0x%08lX RasterCaps 0x%08lX ZCmpCaps 0x%08lX SrcBlendCaps 0x%08lX DestBlendCaps 0x%08lX AlphaCmpCaps 0x%08lX\n",
			c.DevCaps, c.PrimitiveMiscCaps, c.RasterCaps, c.ZCmpCaps, c.SrcBlendCaps, c.DestBlendCaps, c.AlphaCmpCaps);
		std::printf("ShadeCaps 0x%08lX TextureCaps 0x%08lX TextureFilterCaps 0x%08lX CubeTextureFilterCaps 0x%08lX VolumeTextureFilterCaps 0x%08lX\n",
			c.ShadeCaps, c.TextureCaps, c.TextureFilterCaps, c.CubeTextureFilterCaps, c.VolumeTextureFilterCaps);
		std::printf("TextureAddressCaps 0x%08lX VolumeTextureAddressCaps 0x%08lX LineCaps 0x%08lX MaxTexture %lux%lu MaxVolumeExtent %lu MaxTextureRepeat %lu MaxTextureAspectRatio %lu MaxAnisotropy %lu\n",
			c.TextureAddressCaps, c.VolumeTextureAddressCaps, c.LineCaps, c.MaxTextureWidth, c.MaxTextureHeight, c.MaxVolumeExtent, c.MaxTextureRepeat, c.MaxTextureAspectRatio, c.MaxAnisotropy);
		std::printf("MaxVertexW %g GuardBand %g %g %g %g ExtentsAdjust %g StencilCaps 0x%08lX FVFCaps 0x%08lX TextureOpCaps 0x%08lX\n", c.MaxVertexW, c.GuardBandLeft,
			c.GuardBandTop, c.GuardBandRight, c.GuardBandBottom, c.ExtentsAdjust, c.StencilCaps, c.FVFCaps, c.TextureOpCaps);
		std::printf("MaxTextureBlendStages %lu MaxSimultaneousTextures %lu VertexProcessingCaps 0x%08lX MaxActiveLights %lu MaxUserClipPlanes %lu MaxVertexBlendMatrices %lu MaxVertexBlendMatrixIndex %lu\n",
			c.MaxTextureBlendStages, c.MaxSimultaneousTextures, c.VertexProcessingCaps, c.MaxActiveLights, c.MaxUserClipPlanes, c.MaxVertexBlendMatrices, c.MaxVertexBlendMatrixIndex);
		std::printf("MaxPointSize %g MaxPrimitiveCount 0x%08lX MaxVertexIndex 0x%08lX MaxStreams %lu MaxStreamStride %lu VertexShaderVersion 0x%08lX MaxVertexShaderConst %lu PixelShaderVersion 0x%08lX MaxPixelShaderValue %g\n",
			c.MaxPointSize, c.MaxPrimitiveCount, c.MaxVertexIndex, c.MaxStreams, c.MaxStreamStride, c.VertexShaderVersion, c.MaxVertexShaderConst, c.PixelShaderVersion, c.MaxPixelShaderValue);
		const D3DFORMAT fs[] = { D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_A1R5G5B5, D3DFMT_X1R5G5B5, D3DFMT_A4R4G4B4, D3DFMT_R8G8B8, D3DFMT_A8, D3DFMT_L8, D3DFMT_A8L8, D3DFMT_A4L4,
			D3DFMT_P8, D3DFMT_A8P8, D3DFMT_V8U8, D3DFMT_Q8W8V8U8, D3DFMT_DXT1, D3DFMT_DXT2, D3DFMT_DXT3, D3DFMT_DXT4, D3DFMT_DXT5, D3DFMT_UYVY, D3DFMT_YUY2, D3DFMT_D16, D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D32 };
		std::printf("formats (texture / render target / depth):");
		for (D3DFORMAT f : fs)
			std::printf(" %u:%d%d%d", unsigned(f), SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, f)),
				SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, f)),
				SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, f)));
		std::printf("\nadapter modes: %u\n", d3d->GetAdapterModeCount(0));
		return 0;
	}

	int compare(const char *a, const char *b, const char *diff_path)
	{
		UINT wa, ha, wb, hb;
		std::vector<uint32_t> pa, pb;
		if (!read_bmp(a, wa, ha, pa) || !read_bmp(b, wb, hb, pb) || wa != wb || ha != hb)
		{
			std::printf("compare: %s and %s couldn't be read, or differ in size\n", a, b);
			return 1;
		}
		std::vector<int> d(pa.size());
		std::vector<uint32_t> diff(pa.size());
		double sum = 0;
		int largest = 0;
		size_t within2 = 0, within8 = 0;
		for (size_t i = 0; i < pa.size(); ++i)
		{
			int m = 0;
			// (colour only: the alpha channel is the target's own, which the tests don't look at)
			for (int c = 0; c < 3; ++c)
				m = std::max(m, std::abs(int((pa[i] >> (8 * c)) & 255) - int((pb[i] >> (8 * c)) & 255)));
			d[i] = m;
			sum += m;
			largest = std::max(largest, m);
			within2 += m <= 2 ? 1 : 0;
			within8 += m <= 8 ? 1 : 0;
			const uint32_t v = uint32_t(std::min(255, m * 8));
			diff[i] = 0xFF000000 | (v << 16) | (v << 8) | v;
		}
		std::vector<int> sorted = d;
		std::sort(sorted.begin(), sorted.end());
		const int p99 = sorted[std::min(sorted.size() - 1, size_t(sorted.size() * 0.99))];
		std::printf("mean %.3f  p99 %d  largest %d  within 2: %.2f%%  within 8: %.2f%%\n", sum / double(pa.size()), p99, largest,
			100.0 * double(within2) / double(pa.size()), 100.0 * double(within8) / double(pa.size()));
		if (diff_path != nullptr)
			write_bmp(diff_path, wa, ha, diff);
		return 0;
	}
}

#include "d3d8_replay.hpp"

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc >= 4 && std::strcmp(argv[1], "compare") == 0)
		return compare(argv[2], argv[3], argc > 4 ? argv[4] : nullptr);
	if (argc < 3)
	{
		std::printf("d3d8_frame.exe <d3d8.dll> caps | scenes <out> [scene] | replay <capture> <out prefix>\n"
			"d3d8_frame.exe compare <a.bmp> <b.bmp> [diff.bmp]\n");
		return 1;
	}
	const std::string cmd = argv[2];
	const bool replay = cmd == "replay";
	if (!make_device(argv[1], replay ? 1280 : W, replay ? 720 : H))
		return 1;
	int r = 0;
	if (cmd == "caps")
		r = run_caps();
	else if (cmd == "scenes" && argc > 3)
		r = run_scenes(argv[3], argc > 4 ? argv[4] : nullptr);
	else if (replay && argc > 4)
		r = replay::run(argv[3], argv[4], argc > 5 ? std::atoi(argv[5]) : 3);
	else
		r = 1;
	std::printf("%s (%d failures)\n", r == 0 && failures == 0 ? "done" : "FAILED", failures);
	std::fflush(stdout);
	// (the device's objects are left to the process's end, as the game does)
	ExitProcess(r == 0 && failures == 0 ? 0 : 1);
}
