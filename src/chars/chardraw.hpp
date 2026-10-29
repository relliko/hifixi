#pragma once

// The native character renderer's drawing: a frame's character models (charframe.hpp) recorded onto a command list,
// into the world's target and depth, as the game draws them (research/ffxi-re/notes-characters.md "Materials",
// "Lighting", "For a native character renderer" part 2):
//
//   skinning      in the vertex shader from the frame's palette rows (02F1D0: one-bone (x y z 1) M[b], two-bone
//                 (x1 y1 z1 w1) M[a] + (x2 y2 z2 w2) M[b]; the mirrored half with the mirrored bones, flipped by axis)
//   lighting      Direct3D 8's fixed function per vertex, as the game sets it: up to 4 directional lights (the
//                 actor's, 0CB4E0) and AMBIENT, both times the material's light scale (dword 4); the material diffuse
//                 0.5 and ambient the actor colour / 128 (its bytes in the game's order: byte 0 to red); clamped
//   texturing     stage 0 MODULATE2X(texture, lit), alpha MODULATE2X(texture, TFACTOR); stage 1 MODULATE2X(current,
//                 TFACTOR), alpha MODULATE4X(current, TFACTOR) (TFACTOR = the global colour factor, alpha 0x80);
//                 each stage clamped; alpha test GREATER 0x45 (not for a shine material: its alpha always passes);
//                 linear fog by view depth
//   states        no blending, Z LESSEQUAL with writes, no culling; hide groups the model's meshes switch on skipped
//   see-through   the model drawn alone into a private target T (cleared to 0) with the world's depth (test and
//                 write), then T laid over the world: rgb MODULATE2X(T, actor colour), alpha MODULATE2X(T, actor
//                 alpha), SRCALPHA / INVSRCALPHA, no depth
//
// Option: per-pixel lighting (the same lights at each pixel, the normal interpolated).
//
// Motion (for FSR, src/upscale): with a motion target (char_target::motion_rtv, R16G16B16A16_FLOAT) the opaque models
// also write, at each pixel, how far that point moved in the world since the last frame (this frame's skinning less
// the last frame's: the same model and instance in the last frame's record, its palette still in the cache's ring)
// and its view depth: xyz the move (yalms), w the depth. A model not in the last frame (or its skeleton changed) moved
// nowhere of its own.
//
// En-spells (record_enspells, after the opaque models): HiFi XI's own effect on a model's blades (cache::mesh_view's
// os2::blade) while it has one (model_entry::enspell, one of the eight elements); not the game's. The blade's mesh
// drawn again a little swollen with the element's surface on it (fire climbing, frost, wind streaks, glowing cracks,
// crackling lines, flowing caustics, a warm glow, a smouldering dark), and the element's sprites round it (flames and
// embers; sparkles, motes and ice shards; wisps on helices; circling rocks and dust; arcs and sparks; falling drops, a
// stream and bubbles; stars, motes and rays; smoke and wisps). Stateless: every sprite's place and age follow from the
// time and its number, so nothing is kept between frames. Premultiplied onto the world's target (ONE, INVSRCALPHA,
// colour only: glows add, smoke and stone cover), depth tested against it, nothing written to depth.
//
// Vertices, corners and palette rows are read in the vertex shader (descriptor table, root SRV), so nothing of the
// input assembler's is touched. record_*() set the targets, viewport, heaps, root signature and pipelines and leave
// them so (putting the list's own back is the caller's).

#include <d3d12.h>
#include "charframe.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

namespace hifi::chars
{
	struct char_target
	{
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = {}, dsv = {};
		DXGI_FORMAT rtv_format = DXGI_FORMAT_UNKNOWN, dsv_format = DXGI_FORMAT_UNKNOWN;
		UINT samples = 1;
		float width = 0.0f, height = 0.0f;
		float offset = 0.5f; // (dgVoodoo's half-pixel for Direct3D 8's pixel centres)
		D3D12_CPU_DESCRIPTOR_HANDLE motion_rtv = {}; // (the opaque models' motion target, or none)
		DXGI_FORMAT motion_format = DXGI_FORMAT_UNKNOWN;
		D3D12_CPU_DESCRIPTOR_HANDLE light_rtv = {};  // (the opaque models' light split: light_split.hpp; or none)
	};

	struct char_options
	{
		bool per_pixel = false;
		bool sun_known = false; // sun: toward the zone's sun (world), for the light split's share
		float sun[3] = {};
	};

	struct char_stats
	{
		uint32_t models = 0, draws = 0, triangles = 0;
		uint32_t not_ready = 0, hidden = 0, untextured = 0; // groups (or models, not ready) left out
	};

	class char_renderer
	{
	public:
		char_renderer() = default;
		char_renderer(const char_renderer &) = delete;
		char_renderer &operator=(const char_renderer &) = delete;
		~char_renderer();

		bool init(ID3D12Device *device, std::string &why);
		bool ready() const { return _root != nullptr; }
		void release();

		// The frame's opaque models HiFi XI draws (kNative), each with its meshes ready, into the target. The
		// constants go into the upload ring's slot for the frame's seq.
		char_stats record_opaque(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, const char_options &o);
		// One model into `t` as it is (the see-through path's first half: the caller has bound T's view as t.rtv and
		// cleared it)
		char_stats record_model(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, uint32_t model, const char_options &o);
		// The frame's models HiFi XI draws (kNative, opaque and see-through), depth only, into a sun shadow map (`dsv`, D32,
		// size texels square, already cleared) seen through world_to_clip: the same skinning and alpha test, no colour.
		// Its constants and models go into the slot's second half (the frame's own draw may still read the first).
		char_stats record_casters(ID3D12GraphicsCommandList *list, const frame &f, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, uint32_t size);
		// T (an SRV of it in `srv`, a descriptor of the cache's heap) laid over the world's target as the game does
		void record_composite(ID3D12GraphicsCommandList *list, const char_target &world, D3D12_GPU_DESCRIPTOR_HANDLE srv, uint32_t colour);
		// The frame's opaque models' en-spell effects (models with enspell set and a blade among their meshes) into the
		// target, at `seconds` (any clock: only its steps are seen)
		char_stats record_enspells(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, double seconds);

	private:
		bool begin(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, const char_options &o);
		D3D12_GPU_VIRTUAL_ADDRESS write_frame(const frame &f, const char_options &o, const float view_proj[16], uint32_t part);
		void draw_model(ID3D12GraphicsCommandList *list, const char_target &t, const frame &f, uint32_t model, char_stats &st, const frame *last = nullptr);
		ID3D12PipelineState *pipeline(const char_target &t, int kind);

		ID3D12Device *_device = nullptr;
		ID3D12RootSignature *_root = nullptr;
		void *_vs = nullptr, *_ps = nullptr, *_cvs = nullptr, *_cps = nullptr, *_cps_ms = nullptr, *_sps = nullptr; // (ID3DBlob)
		void *_ps_motion = nullptr; // (ID3DBlob: the model's pixel shader with its motion as well)
		void *_ps_light = nullptr, *_ps_light_motion = nullptr; // (... with the light split; with both)
		ID3D12RootSignature *_en_root = nullptr; // (the en-spells': 32 constants, the palette, the mesh)
		void *_en_svs = nullptr, *_en_sps = nullptr, *_en_pvs = nullptr, *_en_pps = nullptr; // (ID3DBlob: shell, sprites)
		ID3D12PipelineState *_shadow_pso = nullptr; // (depth only, D32: made in init, not under the injection's mutex)
		ID3D12Resource *_ring = nullptr; // upload: kFrames slots, each two halves of the frame's constants and its models' (its draw; its shadow)
		uint8_t *_mapped = nullptr;
		std::unordered_map<uint64_t, ID3D12PipelineState *> _pipelines;
	};
}
