#pragma once

// One frame's characters as the game would have drawn them, handed from the game's thread (src/game/charmodel: the
// model draw's stub, after posing) to the thread that records dgVoodoo's Direct3D 12 list (the native character draw
// to come: at the characters' end marker, and at each see-through marker). research/ffxi-re/notes-characters.md
// "For a native character renderer" has where each value comes from.
//
// A ring of kFrames slots keyed by the opaque walk's sequence number (the markers carry its low 4 bits; charslot
// counts it). The game thread fills slot seq % kFrames from the walk's begin: the opaque models during the walk,
// published at its end (before the end marker goes); then the see-through models from the flush, each published
// before its own see-through marker (see_through counts them). dgVoodoo's thread, a frame or so behind, reads what a
// marker names once it's published.

#include <atomic>
#include <cstdint>

namespace hifi::chars
{
	constexpr uint32_t kFrames = 16;        // (the markers' seq has 4 bits)
	constexpr uint32_t kMaxModels = 256;    // a frame's models (the game's draw budget is config 0x3C: 50 actors, each one model; held items and mounts are actors too)
	constexpr uint32_t kMaxInstances = 512; // skeleton instances (a model's +0x44 list)
	constexpr uint32_t kMaxMeshes = 4096;   // meshes (an instance's +0x20 list: a dressed PC has 8-11)
	constexpr uint32_t kMaxBones = 16384;   // palette rows (a PC's skeleton has 107-108 bones)

	// A bone's world matrix as the game's palette has it (row vectors, translation in the last row), its 4th column
	// (0, 0, 0, 1) left out: rows 0-3, x y z each
	struct bone
	{
		float m[12];
	};

	// A D3DLIGHT8 as the game sets it for a character (the table 0CB4E0 fills, 4 slots: type 0 = off)
	struct light8
	{
		uint32_t type;
		float diffuse[4], specular[4], ambient[4];
		float position[3], direction[3];
		float range, falloff, attenuation[3], theta, phi;
	};
	static_assert(sizeof(light8) == 0x68, "D3DLIGHT8");

	struct mesh_entry
	{
		uint32_t mesh = 0xFFFFFFFFu; // the cache's mesh (cache.hpp): 0xFFFFFFFF none
	};

	struct instance_entry
	{
		uint32_t first_bone = 0, bones = 0;
		uint32_t first_mesh = 0, meshes = 0;
	};

	constexpr uint32_t kFromFlush = 1u << 0; // drawn from the priority-12 flush (see-through), at its own marker
	constexpr uint32_t kNative = 1u << 2;    // HiFi XI draws it (the game left it out); else the game drew it

	// An en-spell on the actor (model_entry::enspell): HiFi XI's own effect on its blades (chardraw.hpp), by element in
	// the game's order
	constexpr uint8_t kEnNone = 0, kEnFire = 1, kEnIce = 2, kEnWind = 3, kEnEarth = 4, kEnThunder = 5, kEnWater = 6, kEnLight = 7, kEnDark = 8;

	struct model_entry
	{
		uintptr_t actor = 0, model = 0;
		uint32_t colour = 0;      // 02B6E0's argument 7: 0xAARRGGBB, alpha 0x80 = opaque (carries every fade)
		uint32_t flags = 0;
		uint32_t hidden = 0;      // hide groups (1 << kind) switched on by the model's meshes' header byte 3
		uint8_t enspell = kEnNone; // the en-spell effect on its blades (the game side sets it: src/game/charmodel)
		uint32_t first_instance = 0, instances = 0;
		light8 lights[4];
		uint32_t ambient = 0;     // AMBIENT (D3DCOLOR) as 0CB4E0 set it for this actor
		uint32_t tfactor = 0x80808080u; // the global colour factor the meshes' TEXTUREFACTOR is made with (02DE5C)
		// The actor's own fog, as 0CB4E0 set it on the device for this model (18BA20 on its area environment, the target
		// flash scaling the colour): read back from the device at the draw. Not read: the frame's (the zone's) is used.
		bool fog_read = false;
		uint32_t fog_colour = 0;
		float fog_start = 0.0f, fog_end = 0.0f;
	};

	struct frame
	{
		std::atomic<uint32_t> seq { 0 };          // the walk this slot holds (0: none)
		std::atomic<uint32_t> opaque { 0 };       // published at the walk's end: models [0, opaque)
		std::atomic<uint32_t> see_through { 0 };  // published one by one: the n-th see-through model is models[opaque + n]
		uint32_t models = 0, instances = 0, meshes = 0, bones = 0; // (filled so far)
		uint32_t dropped = 0;                     // models that didn't fit
		// The camera and the zone's fog as the zone pass left them in the renderer (characters are drawn in world space:
		// WORLD is identity for them)
		float view_proj[16] = {};                 // renderer +0x38F5C: VIEW x PROJECTION, row vectors
		float fog[3] = {};                        // FOGCOLOR +0x39254 (rgb 0..1)
		float fog_start = 0.0f, fog_end = 0.0f;   // +0x3925C, +0x39258
		bool camera = false;                      // (those were read)
		model_entry model[kMaxModels];
		instance_entry instance[kMaxInstances];
		mesh_entry mesh[kMaxMeshes];
		bone palette[kMaxBones];
	};

	// The ring (allocated once): the slot for a seq, to fill (the game's thread) or to read (dgVoodoo's: nullptr
	// unless that seq's walk is published there)
	frame &frame_slot(uint32_t seq);
	const frame *published(uint32_t seq_low4);
	// The game's thread: a walk's begin (the slot emptied, its seq set), its end (the opaque models published), one
	// see-through model's (the last model added published as the next see-through one)
	frame &begin_frame(uint32_t seq);
	void publish_opaque(frame &f);
	void publish_see_through(frame &f);
	void clear_frames();
}
