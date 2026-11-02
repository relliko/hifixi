#pragma once

// The texture pack (docs/texture-pack-plan.md): how a texture of the game's is known in it. A pack entry's key is its
// ORIGINAL texture chunk (type 0x20) as the game's own DAT holds it:
//   - FNV-1a 64 over the chunk's whole body: from body[0] (the format byte: 0xA1 DXT, 0xB1 8-bit palette, ...; the
//     16-byte name at body+1) to the next chunk's header, ((info >> 7) & 0x7FFFF) * 16 - 16 bytes. On disk that's
//     bake::chunks' body and size; in the game's memory it's charmodel.cpp's chunk_bytes() (the resource's +0x30,
//     data_size of the header copy at +0x24): the same bytes. The few bytes after the texels are part of it.
//   - that length;
//   - the width and height (int32 at body+21 and body+25).
// All three must match: a name isn't a key (hf_shie1 in ROM\55\43 and ROM\31\43 are different textures).
//
// Checked 2026-11-02 (M0, research\ffxi-re\tools\packkey_check.py): 74 of 74 textures copied from the running game
// (data\chartex\live-*, Oct 25 and 30) are byte-identical to their DATs' bodies over this span. While XIPivot's
// hifi-textures overlay is listed the game holds the overlay's copies instead (4x the size, other bytes): a key made
// from memory then misses, and only one made from the original file on disk (same path under the game's folder, same
// chunk) finds the entry.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hifi::pack
{
	struct key
	{
		uint64_t hash = 0;
		uint32_t bytes = 0, w = 0, h = 0;
		bool operator==(const key &) const = default;
	};

	inline uint64_t fnv1a64(const uint8_t *p, size_t n)
	{
		uint64_t h = 0xCBF29CE484222325ull;
		for (size_t i = 0; i < n; ++i)
			h = (h ^ p[i]) * 0x100000001B3ull;
		return h;
	}

	// A texture chunk's key from its body (body[0] = the format byte) and its size; empty (bytes 0) when it's too small
	// to be a texture
	inline key key_of(const uint8_t *body, size_t size)
	{
		key k;
		if (body == nullptr || size < 29 || size > UINT32_MAX)
			return k;
		int32_t w = 0, h = 0;
		std::memcpy(&w, body + 21, 4);
		std::memcpy(&h, body + 25, 4);
		k.hash = fnv1a64(body, size);
		k.bytes = uint32_t(size);
		k.w = uint32_t(w > 0 ? w : 0);
		k.h = uint32_t(h > 0 ? h : 0);
		return k;
	}
}
