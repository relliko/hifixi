#pragma once

// HiFi XI's own 2D renderer (docs/own-frame/2d.md): the game's screen-space draws (frame2d.hpp) drawn on dgVoodoo's
// Direct3D 12 list at their slot markers, the way dgVoodoo draws them from Direct3D 8:
//   - pre-transformed vertices: a pixel centre at x + 0.5, w = 1 / rhw (perspective-correct, as Direct3D 8), clipped
//     to the Direct3D 8 viewport (a scissor of it: dgVoodoo clips them to a viewport moved by half a pixel);
//   - stage 0 of the fixed-function combiner, every D3DTOP through BLENDCURRENTALPHA with its arguments (DIFFUSE,
//     CURRENT, TEXTURE, TFACTOR, SPECULAR, COMPLEMENT, ALPHAREPLICATE): MODULATE2X is the PS2's "0x80 = 1";
//   - the alpha test (D3DCMP against ALPHAREF), flat or Gouraud colour, the cull mode, the colour write mask;
//   - Direct3D 8's blend (one factor pair and operation for colour and alpha: a colour factor taken as its alpha for
//     the alpha channel), REVSUBTRACT and SUBTRACT included: one pipeline per blend;
//   - the sampler as stage 0 has it (MIN/MAG/MIP POINT or LINEAR, MIPFILTER NONE = the top level only; WRAP, MIRROR,
//     CLAMP), from a heap of every combination.
// The vertices go in a ring of our own read by the vertex shader (no input assembler: dgVoodoo's vertex buffers stay
// bound), the textures in a store of our own uploaded from the game's thread, our own descriptor heaps set for the
// draws and dgVoodoo's put back, then its state (grass::list_state::restore).
//
// Threads: make/release, the texture store and pipeline() on the game's; record() on dgVoodoo's (inside the hooks).
// No device call is made under a lock the other thread takes.

#include <d3d12.h>
#include "frame2d.hpp"
#include "texfmt.hpp"
#include "../render/grass_render.hpp" // (grass::list_state)
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::ui2d::render
{
	constexpr uint32_t kMaxTextures = 4096;

	// The device's objects (root signature, shaders, heaps, the vertex ring, the upload queue). False and why when they
	// can't be made. Again with another device: everything made again there.
	bool make(ID3D12Device *device, std::string &why);
	void release();
	bool ready();

	// dgVoodoo's own Filtering setting (dgVoodoo.conf [DirectX]), which overrides stage 0's filter: app-driven, point,
	// bilinear, linear mip, trilinear, or anisotropic (at a level 1-16); KeepFilterIfPointSampled keeps a texture sampled
	// with MINFILTER POINT as it asks. The setting as dgVoodoo reads it: filtering::read.
	namespace filtering
	{
		enum : uint32_t { app = 0, point = 1, bilinear = 2, linear_mip = 3, trilinear = 4, anisotropic = 5 };
		struct setting
		{
			uint32_t forced = app, anisotropy = 1;
			bool keep_point = true;
		};
		// dgVoodoo.conf's text: [DirectX] Filtering ("appdriven", "pointsampled", "bilinear", "pointmip", "linearmip",
		// "trilinear" or an anisotropic level 1-16) and KeepFilterIfPointSampled
		setting read(const std::string &conf);
	}
	void set_filtering(uint32_t forced, uint32_t anisotropy, bool keep_point);

	// What a texture of nothing reads as (Direct3D 8's stage with no texture set): B, G, R, A
	void set_null_texel(const uint8_t bgra[4]);

	// The texture store (the game's thread). add: a slot for it (its pixels kept until upload()), kNoTexture and why
	// when it can't be had. upload: every texture added since on the GPU (waits for it); false and why when that
	// failed (those slots fail record()). drop: let go (its resource a few seconds later: dgVoodoo's lists may still
	// hold it).
	uint32_t add_texture(const texfmt::texture &t, std::string &why);
	bool upload(std::string &why);
	bool pending();
	void drop_texture(uint32_t slot);
	// Each present (the game's thread): what was dropped long enough ago let go
	void collect(double now);

	// The target the segments are drawn into, as record() last found it (its view's format, its samples); pipeline():
	// the pipeline for a blend made for it, now (the game's thread, before the marker that needs it). False when the
	// target isn't known yet or it couldn't be made.
	bool target_known(DXGI_FORMAT &format, uint32_t &samples);
	void note_target(DXGI_FORMAT format, uint32_t samples); // (record() does; tests)
	bool pipeline(const blend_key &k);

	// dgVoodoo's list (its thread, at a marker): segment s of frame f drawn into target (view format: dgVoodoo's), with
	// dgVoodoo's heaps and state put back after; or, when !draw, only checked. Null when it was (or could be) drawn,
	// else why not.
	const char *record(ID3D12GraphicsCommandList *list, const frame &f, const segment &s, ID3D12Resource *target, DXGI_FORMAT format,
		const grass::list_state &bound, UINT heap_count, ID3D12DescriptorHeap *const *heaps, bool draw);

	struct stats
	{
		uint64_t segments = 0, draws = 0, batches = 0, vertices = 0; // drawn
		uint32_t textures = 0;          // in the store now
		uint64_t texture_bytes = 0;     // ... their bytes on the GPU
		uint64_t uploads = 0;           // upload() rounds that sent something
		uint32_t pipelines = 0;
	};
	stats get_stats();
}
