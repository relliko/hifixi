#pragma once
// The zone's sun shadow casters from the native zone data (research/ffxi-re/notes-mesh-shadows.md, milestone 2): every
// object the zone's MZB places (gpu_zone's placements: the same list the SHD2 bake reads) drawn depth only into a cascade
// of World FX's sun shadow map, as the bake's triangles were. Blended pieces (bit 31: water sheets, the ground's decals,
// glows) don't cast; pieces of an object placed with alpha testing (its record's name starts with '_') are tested as the
// zone pass tests them (4 x texture alpha x vertex alpha > 0x60), the rest solid. Swaying plants (kind 2) at their rest
// positions.
//
// The moving layer (milestone 3): what moves can't be held in cascades drawn again about once a second, so the plants
// that sway (every placement of a kind-2 model) and the doors and elevators (the parts the last pass drew, at their live
// matrices) are drawn each frame into World FX's casters' map around the camera (grass::renderer::set_casters, beside
// the characters), the plants at the sway the last pass drew them with (a plant it didn't draw, off screen: its model's
// sway in that pass, else its own last one). While that layer is drawn, the near cascade leaves out every mover whose
// box lies wholly within kHole of its middle (the camera is never more than a quarter of the cascade from there, so
// those all fall well inside the casters' map); further out they stay in the cascades at rest.
//
// One instanced draw per (model, alpha tested, piece): the placements in the cascade (their world boxes against its
// clip box) listed by model into an upload ring, read by SV_InstanceID. Every read from a root SRV clamped below its
// count (a wild read can hang the GPU).
//
// record() and record_movers() set their root signature, pipeline, heaps (the zone's), target, viewport and scissor
// and leave them so.
#include <d3d12.h>
#include "frame.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hifi::zone
{
	class gpu_zone;

	class zone_shadow
	{
	public:
		zone_shadow() = default;
		zone_shadow(const zone_shadow &) = delete;
		zone_shadow &operator=(const zone_shadow &) = delete;
		~zone_shadow();
		bool init(ID3D12Device *device, std::string &why);
		bool ready() const { return _pso != nullptr; }
		void release();
		// The box around everything the zone places (world), for the map's depth; false when nothing's placed
		bool bounds(gpu_zone &z, float lo[3], float hi[3]);
		// The placements seen through world_to_clip into dsv (D32, size texels square, cleared); triangles drawn. With
		// hole, a near cascade (kHoleReach yalms each way or less) leaves out the movers wholly within kHole of its middle.
		uint32_t record(ID3D12GraphicsCommandList *list, gpu_zone &z, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, uint32_t size, bool hole = false);

		// The moving layer. note_pass: the objects of the last pass drawn that are this map's (zone_index: 0 the zone, 1
		// the second map), for the plants' sways and the doors' live matrices; record_movers: the movers seen through
		// world_to_clip into dsv (cleared by the caller), triangles drawn
		void note_pass(gpu_zone &z, const frame_object *objects, uint32_t count, uint32_t zone_index);
		uint32_t record_movers(ID3D12GraphicsCommandList *list, gpu_zone &z, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, uint32_t size);
		// A number that changes when what the cascades leave out does (a model first seen as a door's)
		uint32_t mover_generation() const { return _mover_generation; }
		// (the last record_movers': instances listed)
		uint32_t last_movers() const { return _last_movers; }

		static constexpr float kHole = 16.0f;      // (yalms each way from the near cascade's middle: plus the quarter of
		                                           // its 40 the camera may be away, 26, inside the casters' map's 32 less
		                                           // its faded edge)
		static constexpr float kHoleReach = 64.0f; // (a cascade reaching no further than this is the near one)

	private:
		bool prepare_zone(gpu_zone &z);
		struct group
		{
			uint32_t model, tested, first, count;
		};
		// The groups' pieces drawn: places and list the root SRVs t2 and t3 read, place_count their places
		uint32_t draw_groups(ID3D12GraphicsCommandList *list, gpu_zone &z, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, uint32_t size,
			D3D12_GPU_VIRTUAL_ADDRESS places, D3D12_GPU_VIRTUAL_ADDRESS visible, uint32_t place_count, const std::vector<group> &groups);
		bool is_mover(uint32_t model) const;
		ID3D12Device *_device = nullptr;
		ID3D12RootSignature *_root = nullptr;
		ID3D12PipelineState *_pso = nullptr;
		// The visible lists: kRingSlots slots of kMaxVisible uints (upload heap, mapped), one slot a record
		ID3D12Resource *_ring = nullptr;
		uint32_t *_mapped = nullptr;
		uint32_t _slot = 0;
		// The movers' ring: kRingSlots slots of kMaxMovers places (16 floats) then as many uints, one slot a record_movers
		ID3D12Resource *_mover_ring = nullptr;
		uint8_t *_mover_mapped = nullptr;
		uint32_t _mover_slot = 0;
		// The zone they're for (its placements as 16 floats each, upload heap; each one's world box)
		const gpu_zone *_zone = nullptr;
		uint32_t _zone_map = 0;
		ID3D12Resource *_places = nullptr;
		std::vector<std::pair<ID3D12Resource *, uint32_t>> _old; // (placements' buffers left, and the record they were left at)
		std::vector<float> _boxes; // (6 a placement: lo xyz, hi xyz)
		std::vector<uint32_t> _by_model; // (the placements sorted by model, then alpha tested)
		float _lo[3] = {}, _hi[3] = {};
		bool _any = false;
		// The moving layer's: the swaying placements (in model order) and each one's sway and the pass it was seen in, found
		// by model and translation; each model's sway in the last pass and that pass; the models seen as doors' parts; the
		// door parts of the last pass
		std::vector<uint32_t> _swaying;
		std::unordered_map<uint64_t, uint32_t> _sway_of;
		std::vector<float> _sway;
		std::vector<uint32_t> _sway_pass;
		std::vector<float> _model_sway;
		std::vector<uint32_t> _model_sway_pass;
		std::vector<uint8_t> _door_model;
		std::vector<frame_object> _doors;
		uint32_t _pass = 0, _mover_generation = 0, _last_movers = 0;
	};
}
