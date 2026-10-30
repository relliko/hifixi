#pragma once

// Direct3D 8's formats in Direct3D 12: what each is stored as on the GPU, read as by shaders, and the conversions
// between its bytes as the game sees them (locks, the CPU's copy) and the GPU's.
//
// Colour render targets are all R8G8B8A8 (typeless, R8G8B8A8_UNORM views: dgVoodoo's own, which HiFi XI's hooks see as
// formats 27 and 28); depth buffers R24G8 typeless with D24_UNORM_S8_UINT views (44 and 45) when they have stencil or 24
// bits. Textures keep their own layout when Direct3D 12 can sample it (BGRA for ARGB, BC for DXT); the rest are
// converted to BGRA8 as they're uploaded.

#include "common.hpp"

namespace hifi::d3d8
{
	struct format_info
	{
		D3DFORMAT d3d = D3DFMT_UNKNOWN;
		DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN; // the texture's own format (typeless for targets and depth)
		DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;     // its shader view's (or the depth view's for depth)
		UINT mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		uint8_t bpp = 0;          // bytes a pixel as the game sees it (0: a block format)
		uint8_t block_bytes = 0;  // DXT: bytes a 4x4 block
		bool convert = false;     // uploaded as BGRA8 through to_bgra8
		bool depth = false, stencil = false;
		bool known = false;
	};

	// What a format is (known false: not one this supports)
	const format_info &info(D3DFORMAT f);
	// Checks the device for the 16-bit formats (B5G6R5, B5G5R5A1, B4G4R4A4): converted when it can't sample them
	void check_formats(ID3D12Device *device);

	// The render target's format on the GPU (all colour targets), and its view's
	constexpr DXGI_FORMAT kTargetResource = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	constexpr DXGI_FORMAT kTargetView = DXGI_FORMAT_R8G8B8A8_UNORM;
	// A target's shader view: alpha read as 1 for the X formats
	UINT target_mapping(D3DFORMAT f);
	bool is_target_format(D3DFORMAT f);
	bool is_depth_format(D3DFORMAT f);

	// Bytes in one row of pixels (of 4x4 blocks for DXT), and the rows (block rows), of a level w x h
	UINT row_pitch(D3DFORMAT f, UINT w);
	UINT row_count(D3DFORMAT f, UINT h);
	inline UINT level_dim(UINT d, UINT level) { const UINT v = d >> level; return v != 0 ? v : 1; }
	UINT level_bytes(D3DFORMAT f, UINT w, UINT h);

	// One level's game bytes (rows of src_pitch) to the GPU's (rows of dst_pitch, as many as the GPU format has), w x h
	// pixels. palette: P8's (256 entries, PALETTEENTRY rgba), may be null.
	void to_gpu(D3DFORMAT f, const uint8_t *src, UINT src_pitch, uint8_t *dst, UINT dst_pitch, UINT w, UINT h, const PALETTEENTRY *palette);
	// The GPU's rows to the game's: a render target's RGBA8 (R8G8B8A8) to f
	void from_target(D3DFORMAT f, const uint8_t *src, UINT src_pitch, uint8_t *dst, UINT dst_pitch, UINT w, UINT h);
	// The game's bytes of f to RGBA8 for a render target (CopyRects from a texture or an image into a target)
	void to_target(D3DFORMAT f, const uint8_t *src, UINT src_pitch, uint8_t *dst, UINT dst_pitch, UINT w, UINT h);

	// A DXT level whose size isn't whole blocks: decoded to BGRA8
	void decode_dxt(D3DFORMAT f, const uint8_t *src, UINT src_pitch, uint8_t *dst, UINT dst_pitch, UINT w, UINT h);
	bool is_dxt(D3DFORMAT f);
}
