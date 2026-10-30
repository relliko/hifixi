/*
 * The text font at 2048 x 4096 (fonthigh.hpp). The menu DAT's 'font    moji' image chunk (type 0x20): byte 0 flags,
 * the 16-byte name, a BITMAPINFOHEADER (width +4, height +8), then 'DXT3' backwards, a dword, the row pitch and the
 * blocks (research/ffxi-re/tools/ffxi_fonts.py find_texture). The two PNG fonts are whole files (ui_sprites.py
 * png_size). WIC through ole32 loaded at run time (nothing more linked).
 */

#include <windows.h>
#include <wincodec.h>
#include "fonthigh.hpp"
#include "../bake/dat.hpp"
#include <cstring>

namespace hifi::ui2d::fonthigh
{
	namespace
	{
		constexpr uint32_t kMenuDat = 0x9A76, kPng1024 = 0x9A77, kPng2048 = 0x9A78;
		constexpr uint32_t kDXT3 = 0x33545844, kA8R8G8B8 = 21;

		struct state
		{
			bool loaded = false;
			std::vector<uint8_t> moji, png1024; // (the smaller fonts' level 0 as the game's texture holds it)
			texfmt::texture high;
		};
		state g;

		template <typename T>
		void drop(T *&p)
		{
			if (p != nullptr)
				p->Release();
			p = nullptr;
		}

		bool same(const texfmt::texture &t, uint32_t format, const std::vector<uint8_t> &want)
		{
			if (t.format != format || t.levels.size() != 1 || t.levels[0].w != 1024 || t.levels[0].h != 2048 || want.empty())
				return false;
			const texfmt::level &l = t.levels[0];
			const uint32_t pitch = texfmt::row_bytes(format, 1024), rows = texfmt::rows(format, 2048);
			if (l.pitch != pitch || l.bytes.size() < size_t(pitch) * rows || want.size() != size_t(pitch) * rows)
				return false;
			return std::memcmp(l.bytes.data(), want.data(), want.size()) == 0;
		}
	}

	bool decode_png(const std::vector<uint8_t> &file, uint32_t &w, uint32_t &h, std::vector<uint8_t> &bgra, std::string &why)
	{
		static const GUID kFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };    // CLSID_WICImagingFactory
		static const GUID kBGRA = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f } };       // GUID_WICPixelFormat32bppBGRA
		HMODULE ole = LoadLibraryW(L"ole32.dll");
		using init_fn = HRESULT(WINAPI *)(void *, DWORD);
		using uninit_fn = void(WINAPI *)();
		using create_fn = HRESULT(WINAPI *)(const GUID &, IUnknown *, DWORD, const IID &, void **);
		const auto init = ole != nullptr ? reinterpret_cast<init_fn>(GetProcAddress(ole, "CoInitializeEx")) : nullptr;
		const auto uninit = ole != nullptr ? reinterpret_cast<uninit_fn>(GetProcAddress(ole, "CoUninitialize")) : nullptr;
		const auto create = ole != nullptr ? reinterpret_cast<create_fn>(GetProcAddress(ole, "CoCreateInstance")) : nullptr;
		if (init == nullptr || uninit == nullptr || create == nullptr)
		{
			why = "ole32 isn't there";
			return false;
		}
		// (whatever apartment the thread is in already: only our own init is undone)
		const bool ours = SUCCEEDED(init(nullptr, COINIT_MULTITHREADED));
		IWICImagingFactory *factory = nullptr;
		IWICStream *stream = nullptr;
		IWICBitmapDecoder *decoder = nullptr;
		IWICBitmapFrameDecode *frame = nullptr;
		IWICFormatConverter *convert = nullptr;
		UINT fw = 0, fh = 0;
		bool ok = SUCCEEDED(create(kFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void **>(&factory))) &&
			SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE *>(file.data()), DWORD(file.size()))) &&
			SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) && SUCCEEDED(decoder->GetFrame(0, &frame)) &&
			SUCCEEDED(factory->CreateFormatConverter(&convert)) &&
			SUCCEEDED(convert->Initialize(frame, kBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
			SUCCEEDED(convert->GetSize(&fw, &fh)) && fw != 0 && fh != 0 && fw <= 8192 && fh <= 8192;
		if (ok)
		{
			bgra.resize(size_t(fw) * fh * 4);
			ok = SUCCEEDED(convert->CopyPixels(nullptr, fw * 4, UINT(bgra.size()), bgra.data()));
		}
		drop(convert);
		drop(frame);
		drop(decoder);
		drop(stream);
		drop(factory);
		if (ours)
			uninit();
		if (!ok)
		{
			why = "the PNG couldn't be decoded";
			return false;
		}
		w = fw;
		h = fh;
		return true;
	}

	bool moji_blocks(std::vector<uint8_t> &blocks, uint32_t &w, uint32_t &h, std::string &why)
	{
		const std::wstring path = bake::dat_path(kMenuDat, why);
		std::vector<uint8_t> dat;
		if (path.empty() || !bake::read_file(path, dat))
		{
			why = why.empty() ? "the menu DAT can't be read" : why;
			return false;
		}
		for (const bake::chunk &c : bake::chunks(dat))
		{
			if (c.type != 0x20 || c.size < 57 + 12 || std::memcmp(c.body + 1, "font    moji    ", 16) != 0)
				continue;
			int32_t iw = 0, ih = 0;
			uint32_t fourcc = 0;
			std::memcpy(&iw, c.body + 17 + 4, 4);
			std::memcpy(&ih, c.body + 17 + 8, 4);
			std::memcpy(&fourcc, c.body + 57, 4);
			const size_t bytes = size_t(iw) * size_t(ih); // (DXT3: 16 bytes a 4 x 4 block)
			if (iw != 1024 || ih != 2048 || fourcc != 0x44585433 || c.size < 57 + 12 + bytes) // ('3TXD': 'DXT3' backwards)
			{
				why = "'font    moji' isn't the 1024 x 2048 DXT3 it should be";
				return false;
			}
			blocks.assign(c.body + 57 + 12, c.body + 57 + 12 + bytes);
			w = uint32_t(iw);
			h = uint32_t(ih);
			return true;
		}
		why = "'font    moji' isn't in the menu DAT";
		return false;
	}

	bool load(std::string &why)
	{
		if (g.loaded)
			return true;
		std::vector<uint8_t> file, bgra;
		uint32_t w = 0, h = 0;
		const std::wstring big = bake::dat_path(kPng2048, why);
		if (big.empty() || !bake::read_file(big, file) || !decode_png(file, w, h, bgra, why) || w != 2048 || h != 4096)
		{
			why = why.empty() ? "the 2048 x 4096 font isn't there" : why;
			return false;
		}
		g.high = texfmt::texture();
		g.high.format = kA8R8G8B8;
		texfmt::level l;
		l.w = w, l.h = h, l.pitch = w * 4;
		l.bytes = std::move(bgra);
		g.high.levels.push_back(std::move(l));
		// (the smaller ones, to know them by: either may be missing)
		std::string unused;
		const std::wstring smaller = bake::dat_path(kPng1024, unused);
		std::vector<uint8_t> px;
		if (!smaller.empty() && bake::read_file(smaller, file) && decode_png(file, w, h, px, unused) && w == 1024 && h == 2048)
			g.png1024 = std::move(px);
		uint32_t mw = 0, mh = 0;
		moji_blocks(g.moji, mw, mh, unused);
		g.loaded = true;
		return true;
	}

	bool loaded()
	{
		return g.loaded;
	}

	bool is_smaller_font(const texfmt::texture &t)
	{
		return g.loaded && (same(t, kDXT3, g.moji) || same(t, kA8R8G8B8, g.png1024));
	}

	const texfmt::texture &high()
	{
		return g.high;
	}

	void unload()
	{
		g = state();
	}
}
