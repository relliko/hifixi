#include "format.hpp"
#include <algorithm>

namespace hifi::d3d8
{
	namespace
	{
		constexpr UINT map(UINT r, UINT g, UINT b, UINT a) { return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(r, g, b, a); }
		constexpr UINT kOne = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
		constexpr UINT kZero = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
		constexpr UINT kRGB1 = map(0, 1, 2, kOne);

		// (filled once: check_formats may turn the 16-bit ones into conversions)
		bool have_565 = true, have_5551 = true, have_4444 = true;

		format_info make(D3DFORMAT d, DXGI_FORMAT r, DXGI_FORMAT v, UINT m, uint8_t bpp, uint8_t block = 0, bool convert = false)
		{
			format_info i;
			i.d3d = d;
			i.resource = r;
			i.view = v;
			i.mapping = m;
			i.bpp = bpp;
			i.block_bytes = block;
			i.convert = convert;
			i.known = true;
			return i;
		}
		format_info converted(D3DFORMAT d, uint8_t bpp)
		{
			return make(d, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, bpp, 0, true);
		}
		format_info depth(D3DFORMAT d, DXGI_FORMAT r, DXGI_FORMAT v, uint8_t bpp, bool stencil)
		{
			format_info i = make(d, r, v, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, bpp);
			i.depth = true;
			i.stencil = stencil;
			return i;
		}

		format_info lookup(D3DFORMAT f)
		{
			switch (f)
			{
			case D3DFMT_A8R8G8B8: return make(f, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 4);
			case D3DFMT_X8R8G8B8: return make(f, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, kRGB1, 4);
			case D3DFMT_R5G6B5: return have_565 ? make(f, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 2) : converted(f, 2);
			case D3DFMT_A1R5G5B5: return have_5551 ? make(f, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 2) : converted(f, 2);
			case D3DFMT_X1R5G5B5: return have_5551 ? make(f, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, kRGB1, 2) : converted(f, 2);
			case D3DFMT_A4R4G4B4: return have_4444 ? make(f, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 2) : converted(f, 2);
			case D3DFMT_X4R4G4B4: return have_4444 ? make(f, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kRGB1, 2) : converted(f, 2);
			case D3DFMT_R8G8B8: return converted(f, 3);
			case D3DFMT_R3G3B2: return converted(f, 1);
			case D3DFMT_A8R3G3B2: return converted(f, 2);
			// (A8 reads white with its alpha, as dgVoodoo samples it)
			case D3DFMT_A8: return make(f, DXGI_FORMAT_A8_UNORM, DXGI_FORMAT_A8_UNORM, map(kOne, kOne, kOne, 3), 1);
			case D3DFMT_L8: return make(f, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, map(0, 0, 0, kOne), 1);
			case D3DFMT_A8L8: return make(f, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, map(0, 0, 0, 1), 2);
			case D3DFMT_A4L4: return converted(f, 1);
			case D3DFMT_P8: return converted(f, 1);
			case D3DFMT_A8P8: return converted(f, 2);
			case D3DFMT_V8U8: return make(f, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_R8G8_SNORM, map(0, 1, kOne, kOne), 2);
			case D3DFMT_Q8W8V8U8: return make(f, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_SNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 4);
			case D3DFMT_V16U16: return make(f, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_SNORM, map(0, 1, kOne, kOne), 4);
			case D3DFMT_X8L8V8U8: return make(f, DXGI_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_SNORM, map(0, 1, 2, kOne), 4);
			case D3DFMT_L6V5U5: return converted(f, 2);
			case D3DFMT_W11V11U10: return converted(f, 4);
			case D3DFMT_A2W10V10U10: return converted(f, 4);
			case D3DFMT_A2B10G10R10: return make(f, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 4);
			case D3DFMT_G16R16: return make(f, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_UNORM, map(0, 1, kOne, kOne), 4);
			case D3DFMT_UYVY: return converted(f, 2);
			case D3DFMT_YUY2: return converted(f, 2);
			case D3DFMT_DXT1: return make(f, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0, 8);
			case D3DFMT_DXT2: return make(f, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0, 16);
			case D3DFMT_DXT3: return make(f, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0, 16);
			case D3DFMT_DXT4: return make(f, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0, 16);
			case D3DFMT_DXT5: return make(f, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0, 16);
			case D3DFMT_D16_LOCKABLE: return depth(f, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, 2, false);
			case D3DFMT_D16: return depth(f, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, 2, false);
			case D3DFMT_D32: return depth(f, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, 4, false);
			case D3DFMT_D15S1: return depth(f, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, 2, true);
			case D3DFMT_D24S8: return depth(f, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, true);
			case D3DFMT_D24X8: return depth(f, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, false);
			case D3DFMT_D24X4S4: return depth(f, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, 4, true);
			default: return format_info();
			}
		}

		format_info table[128];
		format_info dxt[5];
		format_info yuv[2];
		bool table_made = false;

		void make_table()
		{
			for (int i = 0; i < 128; ++i)
				table[i] = lookup(D3DFORMAT(i));
			const D3DFORMAT d[5] = { D3DFMT_DXT1, D3DFMT_DXT2, D3DFMT_DXT3, D3DFMT_DXT4, D3DFMT_DXT5 };
			for (int i = 0; i < 5; ++i)
				dxt[i] = lookup(d[i]);
			yuv[0] = lookup(D3DFMT_UYVY);
			yuv[1] = lookup(D3DFMT_YUY2);
			table_made = true;
		}

		uint8_t expand(uint32_t v, int bits) { return uint8_t(bits == 0 ? 0 : (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }
		uint8_t clamp8(int v) { return uint8_t(std::clamp(v, 0, 255)); }

		// One pixel of f to B G R A bytes
		void pixel_to_bgra(D3DFORMAT f, const uint8_t *s, uint8_t *o, const PALETTEENTRY *pal)
		{
			uint32_t v = 0;
			switch (f)
			{
			case D3DFMT_A8R8G8B8: std::memcpy(o, s, 4); return;
			case D3DFMT_X8R8G8B8: std::memcpy(o, s, 3); o[3] = 255; return;
			case D3DFMT_R8G8B8: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; return;
			case D3DFMT_R5G6B5: v = s[0] | (s[1] << 8); o[0] = expand(v & 31, 5); o[1] = expand((v >> 5) & 63, 6); o[2] = expand(v >> 11, 5); o[3] = 255; return;
			case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5:
				v = s[0] | (s[1] << 8); o[0] = expand(v & 31, 5); o[1] = expand((v >> 5) & 31, 5); o[2] = expand((v >> 10) & 31, 5);
				o[3] = f == D3DFMT_X1R5G5B5 ? 255 : (v & 0x8000) ? 255 : 0; return;
			case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
				v = s[0] | (s[1] << 8); o[0] = expand(v & 15, 4); o[1] = expand((v >> 4) & 15, 4); o[2] = expand((v >> 8) & 15, 4);
				o[3] = f == D3DFMT_X4R4G4B4 ? 255 : expand(v >> 12, 4); return;
			case D3DFMT_R3G3B2: v = s[0]; o[0] = expand(v & 3, 2); o[1] = expand((v >> 2) & 7, 3); o[2] = expand(v >> 5, 3); o[3] = 255; return;
			case D3DFMT_A8R3G3B2: v = s[0]; o[0] = expand(v & 3, 2); o[1] = expand((v >> 2) & 7, 3); o[2] = expand(v >> 5, 3); o[3] = s[1]; return;
			case D3DFMT_A4L4: o[0] = o[1] = o[2] = expand(s[0] & 15, 4); o[3] = expand(s[0] >> 4, 4); return;
			case D3DFMT_A8: o[0] = o[1] = o[2] = 255; o[3] = s[0]; return;
			case D3DFMT_L8: o[0] = o[1] = o[2] = s[0]; o[3] = 255; return;
			case D3DFMT_A8L8: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; return;
			case D3DFMT_P8: case D3DFMT_A8P8:
			{
				const PALETTEENTRY e = pal != nullptr ? pal[s[0]] : PALETTEENTRY { s[0], s[0], s[0], 255 };
				o[0] = e.peBlue; o[1] = e.peGreen; o[2] = e.peRed; o[3] = f == D3DFMT_A8P8 ? s[1] : e.peFlags;
				return;
			}
			case D3DFMT_L6V5U5:
			{
				v = s[0] | (s[1] << 8);
				const int u = int(v << 27) >> 27, vv = int(v << 22) >> 27;
				o[2] = uint8_t(u * 4 + 128); o[1] = uint8_t(vv * 4 + 128); o[0] = expand(v >> 10, 6); o[3] = 255;
				return;
			}
			case D3DFMT_W11V11U10: case D3DFMT_A2W10V10U10:
				std::memset(o, 128, 3); o[3] = 255; return;
			default:
				std::memset(o, 0, 4); return;
			}
		}

		void yuv_to_bgra(int y, int u, int v, uint8_t *o)
		{
			const int c = y - 16, d = u - 128, e = v - 128;
			o[2] = clamp8((298 * c + 409 * e + 128) >> 8);
			o[1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
			o[0] = clamp8((298 * c + 516 * d + 128) >> 8);
			o[3] = 255;
		}

		// A 565 colour to B G R
		void c565(uint16_t c, uint8_t *o) { o[0] = expand(c & 31, 5); o[1] = expand((c >> 5) & 63, 6); o[2] = expand(c >> 11, 5); }

		// One 4x4 block to 16 BGRA pixels
		void decode_block(D3DFORMAT f, const uint8_t *b, uint8_t out[16][4])
		{
			const uint8_t *colour = f == D3DFMT_DXT1 ? b : b + 8;
			const uint16_t c0 = uint16_t(colour[0] | (colour[1] << 8)), c1 = uint16_t(colour[2] | (colour[3] << 8));
			uint8_t pal[4][4];
			c565(c0, pal[0]);
			c565(c1, pal[1]);
			pal[0][3] = pal[1][3] = 255;
			if (c0 > c1 || f != D3DFMT_DXT1)
			{
				for (int k = 0; k < 3; ++k)
				{
					pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k] + 1) / 3);
					pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k] + 1) / 3);
				}
				pal[2][3] = pal[3][3] = 255;
			}
			else
			{
				for (int k = 0; k < 3; ++k)
				{
					pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
					pal[3][k] = 0;
				}
				pal[2][3] = 255;
				pal[3][3] = 0;
			}
			const uint32_t bits = colour[4] | (colour[5] << 8) | (colour[6] << 16) | (uint32_t(colour[7]) << 24);
			for (int i = 0; i < 16; ++i)
				std::memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
			if (f == D3DFMT_DXT2 || f == D3DFMT_DXT3)
			{
				for (int i = 0; i < 16; ++i)
					out[i][3] = expand((b[i / 2] >> ((i & 1) * 4)) & 15, 4);
			}
			else if (f == D3DFMT_DXT4 || f == D3DFMT_DXT5)
			{
				uint8_t a[8];
				a[0] = b[0];
				a[1] = b[1];
				if (a[0] > a[1])
					for (int k = 1; k < 7; ++k)
						a[k + 1] = uint8_t(((7 - k) * a[0] + k * a[1] + 3) / 7);
				else
				{
					for (int k = 1; k < 5; ++k)
						a[k + 1] = uint8_t(((5 - k) * a[0] + k * a[1] + 2) / 5);
					a[6] = 0;
					a[7] = 255;
				}
				uint64_t abits = 0;
				for (int k = 0; k < 6; ++k)
					abits |= uint64_t(b[2 + k]) << (8 * k);
				for (int i = 0; i < 16; ++i)
					out[i][3] = a[(abits >> (3 * i)) & 7];
			}
		}
	}

	void check_formats(ID3D12Device *device)
	{
		auto can_sample = [&](DXGI_FORMAT f) {
			D3D12_FEATURE_DATA_FORMAT_SUPPORT s = { f };
			return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof(s))) && (s.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
		};
		have_565 = can_sample(DXGI_FORMAT_B5G6R5_UNORM);
		have_5551 = can_sample(DXGI_FORMAT_B5G5R5A1_UNORM);
		have_4444 = can_sample(DXGI_FORMAT_B4G4R4A4_UNORM);
		table_made = false;
		if (!have_565 || !have_5551 || !have_4444)
			log(1, "16-bit formats sampled as converted BGRA8: B5G6R5 %d, B5G5R5A1 %d, B4G4R4A4 %d", have_565, have_5551, have_4444);
	}

	const format_info &info(D3DFORMAT f)
	{
		if (!table_made)
			make_table();
		static const format_info none;
		if (uint32_t(f) < 128)
			return table[f];
		switch (f)
		{
		case D3DFMT_DXT1: return dxt[0];
		case D3DFMT_DXT2: return dxt[1];
		case D3DFMT_DXT3: return dxt[2];
		case D3DFMT_DXT4: return dxt[3];
		case D3DFMT_DXT5: return dxt[4];
		case D3DFMT_UYVY: return yuv[0];
		case D3DFMT_YUY2: return yuv[1];
		default: return none;
		}
	}

	UINT target_mapping(D3DFORMAT f)
	{
		return f == D3DFMT_X8R8G8B8 || f == D3DFMT_X1R5G5B5 || f == D3DFMT_X4R4G4B4 || f == D3DFMT_R5G6B5 || f == D3DFMT_R8G8B8 ? kRGB1 : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	}

	bool is_target_format(D3DFORMAT f)
	{
		switch (f)
		{
		case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5:
		case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4: case D3DFMT_R8G8B8:
			return true;
		default:
			return false;
		}
	}

	bool is_depth_format(D3DFORMAT f) { return info(f).depth; }
	bool is_dxt(D3DFORMAT f) { return info(f).block_bytes != 0; }

	UINT row_pitch(D3DFORMAT f, UINT w)
	{
		const format_info &i = info(f);
		if (i.block_bytes != 0)
			return ((w + 3) / 4) * i.block_bytes;
		return w * (i.bpp != 0 ? i.bpp : 4);
	}

	UINT row_count(D3DFORMAT f, UINT h)
	{
		return info(f).block_bytes != 0 ? (h + 3) / 4 : h;
	}

	UINT level_bytes(D3DFORMAT f, UINT w, UINT h) { return row_pitch(f, w) * row_count(f, h); }

	void to_gpu(D3DFORMAT f, const uint8_t *src, UINT src_pitch, uint8_t *dst, UINT dst_pitch, UINT w, UINT h, const PALETTEENTRY *palette)
	{
		const format_info &fi = info(f);
		if (!fi.convert)
		{
			const UINT bytes = row_pitch(f, w), rows = row_count(f, h);
			for (UINT y = 0; y < rows; ++y)
				std::memcpy(dst + size_t(y) * dst_pitch, src + size_t(y) * src_pitch, bytes);
			return;
		}
		if (f == D3DFMT_UYVY || f == D3DFMT_YUY2)
		{
			for (UINT y = 0; y < h; ++y)
			{
				const uint8_t *s = src + size_t(y) * src_pitch;
				uint8_t *o = dst + size_t(y) * dst_pitch;
				for (UINT x = 0; x + 1 < w + 1; x += 2)
				{
					const int y0 = f == D3DFMT_YUY2 ? s[0] : s[1], u = f == D3DFMT_YUY2 ? s[1] : s[0];
					const int y1 = f == D3DFMT_YUY2 ? s[2] : s[3], v = f == D3DFMT_YUY2 ? s[3] : s[2];
					yuv_to_bgra(y0, u, v, o);
					if (x + 1 < w)
						yuv_to_bgra(y1, u, v, o + 4);
					s += 4;
					o += 8;
				}
			}
			return;
		}
		for (UINT y = 0; y < h; ++y)
		{
			const uint8_t *s = src + size_t(y) * src_pitch;
			uint8_t *o = dst + size_t(y) * dst_pitch;
			for (UINT x = 0; x < w; ++x, s += fi.bpp, o += 4)
				pixel_to_bgra(f, s, o, palette);
		}
	}

	void from_target(D3DFORMAT f, const uint8_t *src, UINT src_pitch, uint8_t *dst, UINT dst_pitch, UINT w, UINT h)
	{
		for (UINT y = 0; y < h; ++y)
		{
			const uint8_t *s = src + size_t(y) * src_pitch;
			uint8_t *o = dst + size_t(y) * dst_pitch;
			for (UINT x = 0; x < w; ++x, s += 4)
			{
				const uint32_t r = s[0], g = s[1], b = s[2], a = s[3];
				switch (f)
				{
				case D3DFMT_R5G6B5:
				{
					const uint16_t v = uint16_t(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
					std::memcpy(o, &v, 2);
					o += 2;
					break;
				}
				case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5:
				{
					const uint16_t v = uint16_t((f == D3DFMT_X1R5G5B5 || a >= 128 ? 0x8000 : 0) | ((r * 31 + 127) / 255) << 10 | ((g * 31 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
					std::memcpy(o, &v, 2);
					o += 2;
					break;
				}
				case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
				{
					const uint16_t v = uint16_t((f == D3DFMT_X4R4G4B4 ? 15 : (a * 15 + 127) / 255) << 12 | ((r * 15 + 127) / 255) << 8 | ((g * 15 + 127) / 255) << 4 | ((b * 15 + 127) / 255));
					std::memcpy(o, &v, 2);
					o += 2;
					break;
				}
				case D3DFMT_R8G8B8:
					o[0] = uint8_t(b); o[1] = uint8_t(g); o[2] = uint8_t(r);
					o += 3;
					break;
				case D3DFMT_X8R8G8B8:
					o[0] = uint8_t(b); o[1] = uint8_t(g); o[2] = uint8_t(r); o[3] = 255;
					o += 4;
					break;
				default:
					o[0] = uint8_t(b); o[1] = uint8_t(g); o[2] = uint8_t(r); o[3] = uint8_t(a);
					o += 4;
					break;
				}
			}
		}
	}

	void to_target(D3DFORMAT f, const uint8_t *src, UINT src_pitch, uint8_t *dst, UINT dst_pitch, UINT w, UINT h)
	{
		// (through BGRA: DXT decoded whole, everything else pixel by pixel)
		std::vector<uint8_t> bgra(size_t(w) * h * 4);
		if (is_dxt(f))
			decode_dxt(f, src, src_pitch, bgra.data(), w * 4, w, h);
		else
			for (UINT y = 0; y < h; ++y)
				for (UINT x = 0; x < w; ++x)
					pixel_to_bgra(f, src + size_t(y) * src_pitch + size_t(x) * info(f).bpp, bgra.data() + (size_t(y) * w + x) * 4, nullptr);
		for (UINT y = 0; y < h; ++y)
		{
			uint8_t *o = dst + size_t(y) * dst_pitch;
			const uint8_t *b = bgra.data() + size_t(y) * w * 4;
			for (UINT x = 0; x < w; ++x)
			{
				o[x * 4 + 0] = b[x * 4 + 2];
				o[x * 4 + 1] = b[x * 4 + 1];
				o[x * 4 + 2] = b[x * 4 + 0];
				o[x * 4 + 3] = b[x * 4 + 3];
			}
		}
	}

	void decode_dxt(D3DFORMAT f, const uint8_t *src, UINT src_pitch, uint8_t *dst, UINT dst_pitch, UINT w, UINT h)
	{
		const UINT bw = (w + 3) / 4, bh = (h + 3) / 4, bytes = info(f).block_bytes;
		uint8_t px[16][4];
		for (UINT by = 0; by < bh; ++by)
			for (UINT bx = 0; bx < bw; ++bx)
			{
				decode_block(f, src + size_t(by) * src_pitch + size_t(bx) * bytes, px);
				for (UINT i = 0; i < 16; ++i)
				{
					const UINT x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
					if (x < w && y < h)
						std::memcpy(dst + size_t(y) * dst_pitch + size_t(x) * 4, px[i], 4);
				}
			}
	}
}
