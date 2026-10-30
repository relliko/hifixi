#pragma once

// One zone pass as the game would have drawn it, handed from the game's thread (src/game/zonedraw: recorded in the
// pass itself) to the thread that records dgVoodoo's Direct3D 12 list (src/zone/draw: drawn at the zone's end marker).
// research/ffxi-re/notes-zone.md has where each value comes from.
//
// A ring of kFrames slots keyed by the zone pass's sequence number (the markers carry its low 4 bits): the game
// thread fills slot seq % kFrames between the zone's begin and end markers and publishes it (its seq stored last);
// dgVoodoo's thread, a frame or so behind, reads the slot the end marker names once it's published with that seq.

#include <atomic>
#include <cstdint>

namespace hifi::zone
{
	class gpu_zone;

	constexpr uint32_t kFrames = 16;          // (the markers' seq has 4 bits)
	constexpr uint32_t kMaxObjects = 16384;   // a pass's objects (Port Windurst drew 630; West Ronfaure places 8,118, and view distance x10 draws most)
	constexpr uint32_t kMaxLamps = 64;        // distinct lamps a pass uses
	constexpr uint32_t kMaxEnvs = 8;          // lighting environments a pass uses (0: the zone's own; the rest area ones)

	// frame_object::flags past zonemesh's placement flags
	constexpr uint32_t kDoor = 8;             // a door or elevator part (17ADE0 -> 183DA0): fixed-function, its blended
	                                          // pieces keep Z writes, bit-28 pieces lit by door_env

	// One object the game would have drawn: its model in the zone's data, where it is, how
	struct frame_object
	{
		float world[12];        // the world's x, y and z, each as (m0k, m1k, m2k, t_k) from object +0x40 (row vectors:
		                        // world = position x M + t, so world_k = dot((position, 1), column k))
		uint32_t model = 0;     // (index into the gpu_zone's models)
		uint32_t flags = 0;     // kAlphaTested, kMirrored, kClamped (zonemesh::placement_flag) as the object has them; kDoor
		uint32_t lamps[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu }; // (indices into the frame's lamps)
		float sway = 0.0f;      // kind 2: how far along its sway vector, 0..1 (the game's frame: trunc((N - 1) x weight) / (N - 1))
		uint32_t env = 0;       // its lighting environment (the frame's envs: 0 the zone's own)
		uint32_t door_env = 0;  // a door's bit-28 pieces' (the zone's ev10, renderer +0x50)
		uint32_t zone = 0;      // whose data its model is in: 0 the zone's, 1 the second map's (an interior)
	};

	// A lighting environment: the sun, ambient and fog an object is lit and fogged by (17C9C0 17D3F6..: the zone's own
	// from the renderer, an area's from its environment object)
	struct frame_env
	{
		float sun_dir[3] = {};          // toward the sun (the D3DLIGHT8's Direction, negated)
		float sun[3] = {};              // its diffuse, fixed-function objects
		float sun_bump[3] = {};         // c6: the bump shader's, after the brightness option
		float ambient[3] = {};          // AMBIENT (a D3DCOLOR), fixed-function objects
		float ambient_bump[3] = {};     // c7
		float fog[3] = {};              // FOGCOLOR
		float fog_start = 0.0f, fog_end = 0.0f, fog_scale = 1.0f;
	};

	// A lamp (the game's light table entry, a D3DLIGHT8 point light): world position, colour, range
	struct frame_lamp
	{
		float pos[3];
		float range;
		float colour[3];
		float attenuation2; // (Attenuation2: the game's 0.5)
	};

	struct frame
	{
		std::atomic<uint32_t> seq { 0 };  // published: the zone pass this holds (0: none)
		bool native = false;            // HiFi XI draws this pass (the game left the recorded objects out)
	bool no_bump = false;           // (tests: every piece fixed-function, as a D3D8 replay draws it)
		gpu_zone *zone = nullptr;       // the data the objects' models index (zone 0)
		gpu_zone *second = nullptr;     // the second map's (zone 1: a building's interior), when it has objects drawn
		uint32_t map = 0;
		// The pass's camera and light (renderer fields set by 17B510 for the zone's own environment)
		float view_proj[16];            // +0x38F5C: VIEW x PROJECTION, row vectors
		float view[16];                 // +0x38D9C: VIEW
		float proj[16];                 // +0x38DDC: PROJECTION (the camera's, jitter in it)
		float jitter[2];                // the jitter it was drawn with (game/jitter: A's pixels; 0 while off)
		float camera[3];                // +0x390F0
		float sun_dir[3];               // toward the sun (the D3DLIGHT8's Direction at +0x391C0, negated)
		float sun[3];                   // its diffuse (+0x39184), fixed-function objects
		float sun_bump[3];              // c6 (+0x394D0): the bump shader's, after the brightness option
		float ambient[3];               // AMBIENT (+0x39250, a D3DCOLOR), fixed-function objects
		float ambient_bump[3];          // c7 (+0x394E0)
		float fog[3];                   // FOGCOLOR (+0x39254)
		float fog_start = 0.0f, fog_end = 0.0f, fog_scale = 1.0f; // +0x3925C, +0x39258, +0x3954C
		// Environments 1.. (the fields above are environment 0)
		uint32_t env_count = 1;
		frame_env envs[kMaxEnvs];
		uint32_t count = 0;
		frame_object objects[kMaxObjects];
		uint32_t lamp_count = 0;
		frame_lamp lamps[kMaxLamps];
	};

	// The ring (static): the slot for a seq, to fill (the game's thread) or to read (dgVoodoo's: nullptr unless that
	// seq's pass is published there)
	frame &frame_slot(uint32_t seq);
	const frame *published(uint32_t seq_low4);
	void publish(frame &f, uint32_t seq);
	// Every slot emptied (a zone change, unload)
	void clear_frames();

	// The model actors of one character pass (FFXiMain 0C4C20 -> 1844C0: ships, Mog House furniture, gardening plants,
	// "model" NPCs: an actor whose look is one MMB), recorded in the actor walk by src/game/modelactor and drawn by the
	// zone renderer at the characters' end marker (inject::on_actors_end). Their own ring, keyed by the walk's seq (the
	// characters' markers carry its low 4 bits), filled and published as the zone's.
	constexpr uint32_t kMaxActors = 256;
	struct actor_frame
	{
		std::atomic<uint32_t> seq { 0 };
		// The camera and light 1844C0 draws with: the zone renderer's (VIEW x PROJECTION +0x38F5C, the camera +0x390F0),
		// lit by the default environment (the zone's own: its sun, ambient and fog)
		float view_proj[16];
		float camera[3];
		frame_env env;
		uint32_t count = 0;
		frame_object objects[kMaxActors]; // (zone: the index into data; flags kDoor: fixed-function, blended parts with
		                                  // Z writes, as 1844C0 leaves them)
		uint32_t data_count = 0;
		gpu_zone *data[kMaxActors];      // (each a DAT's models: zonemesh::load_file)
		uint32_t lamp_count = 0;
		frame_lamp lamps[1];             // (a Mog House's room lamp, l_01: lamp 0 of each object then)
	};
	actor_frame &actor_slot(uint32_t seq);
	const actor_frame *actor_published(uint32_t seq_low4);
	void actor_publish(actor_frame &f, uint32_t seq);
	void clear_actor_frames();
}
