/*
 * The texture pack's level layout and record rules (validate.hpp), computed as D3D12's GetCopyableFootprints does for
 * a 2D texture copied from offset 0: each level at the running total aligned to D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
 * (512), its rows D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256) apart, the total ending at the last level's last block (no
 * pitch padding after it). pack_test holds this against WARP's.
 */

#include <windows.h>
#include <d3d12.h>
#include "validate.hpp"

namespace hifi::pack
{
	static_assert(kBC2 == DXGI_FORMAT_BC2_UNORM && kBC4 == DXGI_FORMAT_BC4_UNORM && kBC7 == DXGI_FORMAT_BC7_UNORM);
	static_assert(kRowPitch == D3D12_TEXTURE_DATA_PITCH_ALIGNMENT && kLevelAlign == D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	static_assert(kMaxSide == D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);

	uint32_t block_bytes(uint32_t format)
	{
		switch (format)
		{
		case kBC2:
		case kBC7:
			return 16;
		case kBC4:
			return 8;
		default:
			return 0;
		}
	}

	uint32_t full_levels(uint32_t w, uint32_t h)
	{
		uint32_t n = 1;
		for (uint32_t s = w > h ? w : h; s > 1; s /= 2)
			++n;
		return n;
	}

	bool footprint(uint32_t format, uint32_t w, uint32_t h, uint32_t levels, layout &out)
	{
		out = layout {};
		const uint32_t block = block_bytes(format);
		if (block == 0 || w == 0 || h == 0 || w > kMaxSide || h > kMaxSide || levels == 0 || levels > full_levels(w, h) || levels > kMaxLevels)
			return false;
		uint64_t total = 0;
		for (uint32_t i = 0; i < levels; ++i)
		{
			level_layout &l = out.level[i];
			l.w = w >> i ? w >> i : 1;
			l.h = h >> i ? h >> i : 1;
			l.row_bytes = (l.w + 3) / 4 * block;
			l.row_pitch = (l.row_bytes + kRowPitch - 1) / kRowPitch * kRowPitch;
			l.rows = (l.h + 3) / 4;
			l.offset = uint32_t((total + kLevelAlign - 1) / kLevelAlign * kLevelAlign);
			total = l.offset + uint64_t(l.row_pitch) * (l.rows - 1) + l.row_bytes;
		}
		out.levels = levels;
		out.total = total;
		return true;
	}

	const char *check(const record &r, ID3D12Device *device)
	{
		if (r.kind != kColour && r.kind != kHeight && r.kind != kMaterial)
			return "unknown record kind";
		if (block_bytes(r.format) == 0)
			return "format isn't BC2, BC7 or BC4 UNORM";
		if (r.w == 0 || r.h == 0 || r.w % 4 != 0 || r.h % 4 != 0)
			return "width or height not a multiple of 4";
		if (r.w > kMaxSide || r.h > kMaxSide)
			return "width or height over 16384";
		if (r.levels == 0 || r.levels > full_levels(r.w, r.h))
			return "level count past the whole chain";
		if (r.offset % kLevelAlign != 0)
			return "offset not a multiple of 512";
		layout l;
		if (!footprint(r.format, r.w, r.h, r.levels, l) || l.total != r.bytes)
			return "stored bytes aren't the copy layout's (row pitch or level alignment)";
		if (device != nullptr)
		{
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = r.w;
			d.Height = r.h;
			d.DepthOrArraySize = 1;
			d.MipLevels = uint16_t(r.levels);
			d.Format = DXGI_FORMAT(r.format);
			d.SampleDesc.Count = 1;
			d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kMaxLevels] = {};
			UINT rows[kMaxLevels] = {};
			UINT64 row_bytes[kMaxLevels] = {}, total = 0;
			device->GetCopyableFootprints(&d, 0, r.levels, 0, fp, rows, row_bytes, &total);
			if (total != r.bytes)
				return "stored bytes aren't GetCopyableFootprints' total";
			for (uint32_t i = 0; i < r.levels; ++i)
				if (fp[i].Offset != l.level[i].offset || fp[i].Footprint.RowPitch != l.level[i].row_pitch || rows[i] != l.level[i].rows ||
					row_bytes[i] != l.level[i].row_bytes)
					return "a level isn't where GetCopyableFootprints puts it";
		}
		return nullptr;
	}
}
