#pragma once

// The characters' meshes and textures on the GPU, for the native character renderer: each Os2 mesh and texture chunk
// the game has loaded, taken once from the game's own memory the first time a drawn model names it (src/game/charmodel:
// the bytes copied on the game's thread), decoded on a thread of ours (os2, tex) and uploaded on a copy queue of HiFi
// XI's own, never on dgVoodoo's lists (as the zone's data: resources made in COMMON, decaying back to COMMON once the
// copy is done, promoted on first use by whichever queue reads them).
//
//   a mesh       its vertices (os2::vertex, 68 bytes) and its corners (os2::corner, 12 bytes) as two buffers with a
//                structured-buffer view each; its groups kept on the CPU with the cache's textures they use
//   a texture    R8G8B8A8_UNORM with the game's mips (tex::make_mips), its alpha as the game keeps it (0x80 = opaque)
//   descriptors  one shader-visible heap of ours (kDescriptors): a mesh's two views side by side, a texture's one
//   palettes     an upload buffer of kFrames slots, each frame's bones (charframe.hpp: 48 bytes a bone) copied into the
//                frame's slot as models are published
//
// Entries are keyed by the game's resource and checked against its handle, size and data pointer each frame (a freed
// resource's memory can be reused for another); an entry not named for kForgetSeconds is let go.
//
// The texture pack's part (docs/texture-pack/characters.md): each texture's key is made from the bytes the game holds
// (pack::key_of, on the worker, before they're freed), and a texture on the GPU may get a second pair of descriptors
// with the pack's view of it, which the renderer then binds. The cache makes no streamer call (it holds the tickets,
// the pairs and the states); src/chars/charpack.cpp asks, reads the tickets and writes the views on the game's thread,
// and the tickets the cache lets go are handed to its drop hook outside the mutex.

#include <d3d12.h>
#include "os2.hpp"
#include "../pack/pack.hpp"
#include "../pack/stream.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::chars
{
	struct frame;
}

namespace hifi::chars::cache
{
	constexpr uint32_t kNone = 0xFFFFFFFFu;
	constexpr uint32_t kDescriptors = 16384;
	constexpr double kForgetSeconds = 20.0;
	// The worker's batch: the upload bytes it copies at once, which is also what it holds decoded at most (it decodes
	// only the next batch; one entry bigger than that is decoded and uploaded alone, before any other once it's been held
	// back). A texture whose colour with its mips would be more than kEntryCap isn't decoded at all: it fails, "too big",
	// and its models are left to the game (the game's 32-bit memory is shared with the game's own and the driver's). One
	// whose detail map would take it over kEntryCap (the map with its mips and make_detail's working images,
	// tex::detail_work_bytes) is decoded without one.
	constexpr uint64_t kBatchBytes = 48ull << 20;
	constexpr uint64_t kEntryCap = 96ull << 20;

	// What tells a game resource from another one at the same address
	struct key
	{
		uintptr_t resource = 0;
		uint32_t handle = 0, size = 0, data = 0;
		bool operator==(const key &o) const { return resource == o.resource && handle == o.handle && size == o.size && data == o.data; }
	};

	// The game's thread. A mesh or texture already known (seen now, so it's kept), else kNone.
	uint32_t mesh(const key &k);
	uint32_t texture(const key &k);
	// A new one, with its bytes (the chunk's data) to decode: its id. A mesh's texture ids, one per texture command in
	// command order (kNone where none was found); live_base: the data's address in the game (its section pointers);
	// name: its chunk's 4-character name (a weapon's slot: os2::weapon_slot, for its blade), nullptr when not known.
	uint32_t add_mesh(const key &k, std::vector<uint8_t> &&data, uint32_t live_base, std::vector<uint32_t> &&textures, const char *name = nullptr);
	uint32_t add_texture(const key &k, std::vector<uint8_t> &&data);
	// A mesh's header byte 3 (the hide groups it switches on), known when it's added (0 for an unknown id)
	uint8_t mesh_hides(uint32_t id);

	// The game's thread, each present: the device to upload with (nullptr: decode only, nothing on the GPU), the time;
	// entries not named for a while let go
	void frame_tick(ID3D12Device *device, double now);
	// Whether textures get a detail map ([chars] detail; tex::make_detail): made only while it's on, for the textures
	// decoded from then on (those already on the GPU keep what they have until they're let go and taken again), and only
	// for those it keeps within kEntryCap
	void set_detail(bool on);
	// A frame's palette rows [from, to) into its upload slot (the game's thread, as models are published)
	void publish_palettes(const frame &f, uint32_t from, uint32_t to);
	// Where a frame slot's palette rows are on the GPU (0 before the upload buffer is made)
	D3D12_GPU_VIRTUAL_ADDRESS palettes(uint32_t seq);

	// For the renderer (any thread): a mesh or texture once it's on the GPU
	struct mesh_view
	{
		bool ready = false;
		uint8_t hides = 0;
		uint32_t vertices = 0, corners = 0;
		std::vector<os2::group> groups;
		std::vector<uint32_t> textures; // the cache's texture per the mesh's texture index
		os2::blade blade;               // its blade, when it's a weapon with one (the en-spell effects)
		D3D12_GPU_DESCRIPTOR_HANDLE views {}; // vertices' view, then the corners'
	};
	bool mesh_view_of(uint32_t id, mesh_view &out);
	// Whether a mesh and the textures it found are all on the GPU (the game's thread asks before leaving a model out). A
	// texture that couldn't be read (or was too big) never will be: its models stay the game's for good.
	bool mesh_ready(uint32_t id);
	// A texture's pair once it's on the GPU: the texture pack's when it has one ready and allow_pack (the shadow casters
	// refuse it: they keep the game's cut-outs) and /hifi textures compare is off, else the game's; is_pack says which
	// (the game's thread: the time a pack pair is handed out is kept, its "last drawn"). is_pack: the pair's colour is the
	// pack's (a pair written for a height alone holds a view of the game's texture: false). relief (M9, docs/texture-pack/
	// characters.md "Pack relief"): above 0 when the pair handed out holds the pack's height in the detail map's place,
	// its relief scale; 0 otherwise (the game's pair, or a pack pair with the detail view)
	bool texture_view_of(uint32_t id, D3D12_GPU_DESCRIPTOR_HANDLE &out, bool allow_pack = true, bool *is_pack = nullptr, float *relief = nullptr);
	ID3D12DescriptorHeap *heap();
	// The renderer's own descriptors in that heap (k < kReserved: 0 the see-through target's view, 1 the scene's copy, 2
	// the aura ring's 256 x 256 target); false before the heap. (The heap's last kReserved / 2 pairs.)
	constexpr uint32_t kReserved = 4;
	bool reserved(uint32_t k, D3D12_CPU_DESCRIPTOR_HANDLE &cpu, D3D12_GPU_DESCRIPTOR_HANDLE &gpu);

	struct counts
	{
		uint32_t meshes = 0, meshes_ready = 0, meshes_failed = 0;
		uint32_t textures = 0, textures_ready = 0, textures_failed = 0;
		uint64_t bytes_in = 0, gpu_bytes = 0;
		double decode_ms = 0.0, upload_ms = 0.0; // (totals)
		uint32_t forgotten = 0;
		std::string last_failure;
		// The worker's memory: textures' bytes decoded and not yet on the GPU (now, and the most at once), the batches
		// uploaded and the biggest's upload bytes, textures refused as too big (kEntryCap)
		uint64_t decoded_bytes = 0, decoded_peak = 0, batch_bytes_max = 0;
		uint32_t batches = 0, too_big = 0;
		// Textures on the GPU with a detail map, textures decoded without one while it's on (it would have taken them
		// over kEntryCap); meshes left to the game for good (a texture of theirs couldn't be read)
		uint32_t detail_textures = 0, detail_skipped = 0, meshes_left_failed = 0;
		// (in all: textures let go to be made again with a detail map, as they were turned on: set_detail)
		uint32_t detail_redone = 0;
		// Descriptor pairs free, and let go but not given back yet (lists in flight may still read them: retire::then)
		uint32_t slots_free = 0, slots_retiring = 0;
		// The texture pack's part. Now: textures on the GPU with a key, with the pack's view ready (and its view bytes),
		// asked (a ticket on its way), spent as not in the pack (or no_pack) or with no key at all, failed, gone (the
		// ticket's epoch ended before it was read), and pairs held for pack views. In all: tickets handed to the drop hook,
		// ready tickets dropped for want of descriptors (kPackSlotFloor), and textures not asked for that reason the last
		// time asks were taken
		uint32_t pack_keyed = 0, pack_ready = 0, pack_asked = 0, pack_not_in = 0, pack_failed = 0, pack_gone = 0, pack_pairs = 0;
		uint64_t pack_bytes = 0;
		uint64_t pack_dropped = 0, pack_skipped_slots = 0;
		uint32_t pack_waiting_slots = 0;
		// Textures evicted now (the streamer's pressure: the game's until it's gone a while), and evictions in all
		uint32_t pack_evicted = 0;
		uint64_t pack_evictions = 0;
		// (M9) Ready pack pairs holding the pack's height (relief), of them the ones whose colour is the game's (a height
		// alone: a materials-only entry, or a colour the pack hasn't); heights asked now (on their way)
		uint32_t pack_relief = 0, pack_relief_only = 0, pack_heights_asked = 0;
	};
	counts get_counts();
	std::vector<std::string> status();
	// For the tests: a mesh's two buffers and a texture's resource once on the GPU (nullptr otherwise)
	void resources_of(uint32_t mesh, uint32_t texture, ID3D12Resource *&vertices, ID3D12Resource *&corners, ID3D12Resource *&tex);

	// ---- The texture pack's part (src/chars/charpack.cpp, the game's thread; docs/texture-pack/characters.md "The
	// upgrade state machine"). A texture's pack part: none (a key, nothing asked), asked (a ticket on its way), ready (the
	// ticket ready and the pack's pair written: texture_view_of hands it out), spent (missed or gone: the game's until
	// the pack's part is forgotten), evicted (let go under the streamer's pressure: the game's until pack_unevict). Every
	// change is made under the cache's mutex; device calls and the streamer's outside

	// While fewer pairs than this are free none is taken for a pack view (the heap is shared with the meshes and the
	// game's textures, which must always find one): asks are held and a ready ticket is dropped (the entry asks again)
	constexpr uint32_t kPackSlotFloor = 1024;
	// Where the tickets the cache lets go are dropped (charpack: stream::drop), from the thread that lets them go (the
	// game's, or the cache's worker) with the mutex left. nullptr: none (the cache never asks then)
	using pack_drop_fn = void (*)(hifi::pack::stream::ticket t);
	void set_pack_drop(pack_drop_fn fn);
	// Whether pack parts are wanted (charpack, while the pack is in use with [textures] chars on the cache's device): on,
	// every keyed texture on the GPU with no pack part is put on the asks' list, and each one ready from then on; off,
	// the list emptied. Cheap when unchanged
	void set_pack_on(bool on);
	// Every texture's pack part let go (its ticket to the drop hook, its pair behind the GPU: retire::then), all of them
	// none again (and asked again while set_pack_on): the game's textures and pairs stay. Before every close, open,
	// reload, stop, device change and loss (the module's clear_all through charpack)
	void forget_pack_textures();
	// /hifi textures compare: the game's pairs handed out while it's on (nothing let go)
	void set_pack_compare(bool on);
	// The present's time, kept as a pack pair's "last drawn" when texture_view_of hands it out
	void pack_tick(double now);
	// The device the cache's heap and textures were made on (nullptr: none yet): the pack's views are written only for
	// textures of this device
	ID3D12Device *device();

	// A texture to ask the pack for: its id and serial (ids are reused), its key, and the game's own smallest level
	// (the levels down_to: the key's size halved tex::mip_count - 1 times)
	struct pack_ask
	{
		uint32_t id = kNone, serial = 0;
		hifi::pack::key k;
		uint32_t down_to_w = 0, down_to_h = 0;
	};
	// At most max of the asks' list (still none, still the same texture), out appended. While kPackSlotFloor pairs or
	// fewer are free none is handed out (pack_waiting_slots: how many wait); over it, no more than the pairs over the
	// floor less the textures already asked or being written (each ready ticket takes a pair)
	void take_pack_asks(uint32_t max, std::vector<pack_ask> &out);
	// The ticket a request gave: kept (asked, in epoch), or false when the texture went meanwhile (drop both). height
	// (M9): the same texture's height ticket, asked beside the colour while heights are on and the pack has one for its
	// key (none: generation 0); the texture's pair is written only once both are settled (pack_settle)
	bool pack_asked(uint32_t id, uint32_t serial, hifi::pack::stream::ticket t, uint32_t epoch, hifi::pack::stream::ticket height = {});
	// A request that gave no ticket (the owner gone, the table full): spent, failed
	void pack_refused(uint32_t id, uint32_t serial);
	// A texture's pair being written (pack_settle or pack_begin, then write_pack_detail and pack_end): t the colour's
	// ticket (generation 0: the pack has no colour for it, and game_colour is AddRef'd: descriptor 2s a view of the game's
	// texture), height the height's (generation 0: none, and detail_map is AddRef'd, or nullptr: descriptor 2s + 1 the
	// detail map's view as before M9), relief its relief scale
	struct pack_writing
	{
		uint32_t id = kNone, serial = 0, slot = 0;
		hifi::pack::stream::ticket t, height;
		float relief = 0.0f;
		D3D12_CPU_DESCRIPTOR_HANDLE colour {}, detail {}; // (the pair's two descriptors)
		ID3D12Resource *detail_map = nullptr, *game_colour = nullptr;
	};
	// A ticket take_done handed over missed or gone: let go, the texture spent (until its pack part is forgotten)
	enum class pack_miss : uint8_t
	{
		not_in_pack, // (not in the pack, or its no_pack flag)
		failed,
		gone,
	};
	// (M9) A ticket take_done handed over, ready or not (why: a colour's miss), for a texture with or without a height:
	// what's to be done now. none: not the cache's any more (or no pair to spare: let go, asked again); wait: the other
	// of its two tickets isn't settled yet; write: a pair taken, out to be written (the caller writes the pack's views
	// with the mutex left, then write_pack_detail, then pack_end); spent: neither had anything (the texture spent by the
	// colour's why). relief: a height ticket's relief scale (ready only when above 0: the caller checked it). colour_bytes:
	// a ready colour ticket's view bytes, kept while its height is on its way (pack_evict's pace for such a held colour)
	enum class pack_step : uint8_t { none, wait, write, spent };
	pack_step pack_settle(hifi::pack::stream::ticket t, bool ready, pack_miss why, float relief, pack_writing &out, uint64_t colour_bytes = 0);
	// (M9) Whether t is a texture's height ticket, and that texture's key width (the game's texture: the relief's grid)
	bool pack_height_of(hifi::pack::stream::ticket t, uint32_t &key_w);
	// A colour ticket take_done handed over ready (no height asked): pack_settle's write. false: not the cache's ticket
	// any more, no pair (the ticket let go, the texture asks again), or a height still on its way
	bool pack_begin(hifi::pack::stream::ticket t, pack_writing &out);
	// (the mutex not held: device calls) The pair's descriptors that aren't the pack's: the second a new view of the game
	// texture's detail map (its levels from its desc), or the null view the game's pair has when it has none (nothing
	// when the height is there); the first a view of the game's texture when the pack has no colour for it. Never a copy
	// from the shader-visible heap. What was AddRef'd Released
	void write_pack_detail(pack_writing &w);
	// The pack's views written (stream::write_view: ok) or not: ready with their view bytes, or the pair and the tickets
	// let go and the texture spent (failed). Nothing when the texture's pack part was let go meanwhile
	void pack_end(const pack_writing &w, bool ok, uint64_t bytes);
	// A colour ticket take_done handed over missed or gone (no height asked): pack_settle's spent
	void pack_spent(hifi::pack::stream::ticket t, pack_miss why);
	// The streamer's pressure (charpack, each half second while it holds: plan section 5): the ready textures whose pack
	// pair was handed out longest ago let go (pair and ticket behind the GPU, as any), at least one, until their view bytes
	// reach max_bytes; each evicted, not asked again until pack_unevict. How many (their view bytes in bytes). M9: first
	// the asked ones holding a ready colour while their height is on its way (nothing draws them; their tickets let go)
	uint32_t pack_evict(uint64_t max_bytes, uint64_t &bytes);
	// The evicted textures a model drew last none again, and on the asks' list while pack parts are wanted (the pressure
	// gone a while), at least one, until their view bytes reach max_bytes (charpack paces them a round each half second,
	// so a pressure that comes back stops them part way): how many, and how many are still evicted in left
	uint32_t pack_unevict(uint64_t max_bytes, uint32_t &left);

	// Waits until nothing is queued or being uploaded (the tests); false on timeout. With no device to upload to, textures
	// the batch holds back (kBatchBytes already decoded) count as settled.
	bool settle(uint32_t ms);
	// Everything let go (the thread stopped, the GPU's copies waited for)
	void stop();

	namespace test
	{
		// (tests) A texture's key as the worker made it (empty: none yet, or none), its pack state ("none", "asked",
		// "writing", "ready", "spent", "evicted"; "" for no texture), its ticket and pack pair (kNone: none)
		hifi::pack::key pack_key_of(uint32_t id);
		const char *pack_state_of(uint32_t id);
		hifi::pack::stream::ticket pack_ticket_of(uint32_t id);
		hifi::pack::stream::ticket pack_height_ticket_of(uint32_t id); // (M9: its height's ticket, while held)
		uint32_t pack_slot_of(uint32_t id);
		uint32_t game_slot_of(uint32_t id);
		// (tests) Pairs taken out of free_slots till leave_free are left (the floor reached by hand), and given back
		void hold_slots(uint32_t leave_free);
		void give_back_slots();
		// (tests) Whether a pair is in free_slots now; whether free_slots and every entry's pairs are all different (no
		// pair handed out twice, none both free and in use)
		bool slot_free(uint32_t slot);
		bool slots_consistent(std::string &why);
	}
}
