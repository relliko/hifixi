#pragma once
// The zone's sun shadow casters from the native zone data (research/ffxi-re/notes-mesh-shadows.md, milestone 2): every
// object the zone's MZB places (gpu_zone's placements: the same list the SHD2 bake reads) drawn depth only into a cascade
// of World FX's sun shadow map, as the bake's triangles were. Blended pieces (bit 31: water sheets, the ground's decals,
// glows) don't cast; pieces of an object placed with alpha testing (its record's name starts with '_') are tested as the
// zone pass tests them (4 x texture alpha x vertex alpha > 0x60), the rest solid. Sway left out (their rest positions).
//
// One instanced draw per (model, alpha tested, piece): the placements in the cascade (their world boxes against its
// clip box) listed by model into an upload ring, read by SV_InstanceID. Every read from a root SRV clamped below its
// count (a wild read can hang the GPU).
//
// record() sets its root signature, pipeline, heaps (the zone's), target, viewport and scissor and leaves them so.
#include <d3d12.h>
#include <cstdint>
#include <string>
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
		// The placements seen through world_to_clip into dsv (D32, size texels square, cleared); triangles drawn
		uint32_t record(ID3D12GraphicsCommandList *list, gpu_zone &z, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, uint32_t size);

	private:
		bool prepare_zone(gpu_zone &z);
		ID3D12Device *_device = nullptr;
		ID3D12RootSignature *_root = nullptr;
		ID3D12PipelineState *_pso = nullptr;
		// The visible lists: kRingSlots slots of kMaxVisible uints (upload heap, mapped), one slot a record
		ID3D12Resource *_ring = nullptr;
		uint32_t *_mapped = nullptr;
		uint32_t _slot = 0;
		// The zone they're for (its placements as 12 floats each, upload heap; each one's world box)
		const gpu_zone *_zone = nullptr;
		ID3D12Resource *_places = nullptr;
		std::vector<std::pair<ID3D12Resource *, uint32_t>> _old; // (placements' buffers left, and the record they were left at)
		std::vector<float> _boxes; // (6 a placement: lo xyz, hi xyz)
		std::vector<uint32_t> _by_model; // (the placements sorted by model, then alpha tested)
		float _lo[3] = {}, _hi[3] = {};
		bool _any = false;
	};
}
