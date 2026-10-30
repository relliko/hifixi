#pragma once

// An Os2 skinned mesh (chunk 0x2A) made ready for the GPU (research/ffxi-re/notes-characters.md "Os2", "Materials";
// tools/chars.py is the reference this is tested against).
//
// The game skins on the CPU every frame (02F1D0): a one-bone vertex is (x y z 1) M[b], a two-bone one
// (x1 y1 z1 w1) M[a] + (x2 y2 z2 w2) M[b] with the positions already times their weights; normals by the 3x3 parts.
// M is the palette: each bone's world matrix, row vectors. A mirrored mesh's other half (02D340) is the same vertices
// scaled by the mirror axis each bone reference names, skinned with the mirrored bones, winding reversed.
//
// Here each vertex keeps both bones and both halves' bones, so a vertex shader can skin either half from the same
// vertex: the triangles' corners (a vertex number and that corner's UV: UVs are per corner in Os2) are listed once for
// the mesh and once more, mirrored, when it has a mirrored half, in groups that share a texture and a material.
//
// The same decoder reads a DAT's chunk (section offsets in 16-bit words) and a live resource the game has loaded (its
// section offsets made absolute pointers by 02D8F0; its drawn texture commands rewritten 0x8000 -> 0x8001 with the
// name's first dword holding the texture object): the caller turns either into byte offsets from the data's start.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hifi::chars::os2
{
	// A vertex, both halves' bones: a one-bone vertex has w0 = 1 and w1 = 0 (p1, n1 zero)
	struct vertex
	{
		float p0[3];
		float w0;
		float p1[3];
		float w1;
		float n0[3];
		float n1[3];
		uint16_t bone[2];        // skeleton bones (through the bone table when the mesh uses it)
		uint16_t mirror_bone[2]; // the mirrored half's bones
		uint8_t axis[2];         // the mirrored half's flip of each: 0 none, 1 x, 2 y, 3 z (02D340's table)
		uint8_t pad[2];
	};
	static_assert(sizeof(vertex) == 68, "the vertex shader's stride");

	// A triangle corner: its vertex (bit 31: the mirrored half) and its UV
	struct corner
	{
		uint32_t vertex;
		float u, v;
	};
	constexpr uint32_t kMirrored = 0x80000000u;

	// A texture command (0x8000, or 0x8001 once the game has drawn it): where it is, and what names its texture
	struct texture_ref
	{
		uint32_t at = 0;        // the command's byte offset from the data's start
		bool cached = false;    // 0x8001: object is the game's texture object (0: none was found), name[0..3] gone
		uint32_t object = 0;
		uint8_t name[16] = {};
	};

	// Corners first .. first + count - 1 drawn with one texture and one material
	struct group
	{
		uint32_t first = 0, count = 0;
		int32_t texture = -1;   // index into textures (-1: none bound yet: the game keeps what was bound before)
		uint32_t material[11] = { 0x80808080u }; // the 0x8010 command's dwords (notes "Materials"); none yet: neutral
		bool has_material = false;
		bool mirrored = false;
		uint8_t kind() const { return uint8_t((material[3] >> 8) & 0xF); } // (the hide group)
	};

	struct mesh
	{
		uint16_t version = 0;
		uint8_t flags = 0;       // data + 2 (bit 0x80: bone numbers through the bone table; low 7 bits 1: cloth)
		uint8_t hides = 0;       // data + 3: the hide groups this mesh switches on (notes "Hide groups")
		bool mirrored = false;   // data + 4 bit 0
		// A type-1 mesh: cloth (research/ffxi-re/notes-actor-extras.md 1). The game skins and simulates its vertices on
		// the CPU every pose frame (023290: Verlet, constraints, bone spheres, the wind) into a work buffer of its own,
		// 60 bytes a vertex in command numbering (one-bone ones first), and draws them from there: positions in world
		// space, normals rebuilt from the triangles. Here vertex i is "bone i" alone at its bone's origin with the normal
		// (1, 0, 0): charmodel hands each frame's work vertex i over as palette row i (its normal as row 0, its
		// position as the translation), so the skinning above draws it as the game has it, and its last frame's row
		// gives its motion. one_bone / two_bone are the cloth's counts, pinned its pinned ones (+0x2A, +0x2C).
		bool cloth = false;
		uint16_t pinned[2] = {};
		uint32_t one_bone = 0, two_bone = 0;
		uint16_t max_bone = 0;   // the highest bone any vertex names (either half)
		std::vector<vertex> vertices;
		std::vector<corner> corners;
		std::vector<group> groups;
		std::vector<texture_ref> textures;
	};

	// The six sections' byte offsets from the data's start and their lengths in bytes
	struct sections
	{
		uint32_t at[6] = {}, size[6] = {};
	};
	// From a DAT chunk's data (offsets in words), or a live resource's data (offsets as pointers; base = the address
	// of the data's first byte, the resource + 0x30). False when the header can't be read.
	bool sections_of_chunk(const uint8_t *data, size_t size, sections &out);
	bool sections_of_live(const uint8_t *data, size_t size, uint32_t base, sections &out);

	// The mesh (an Os2 mesh, or a cloth one: mesh::cloth); false and why when it isn't one this can read (nothing kept
	// then)
	bool decode(const uint8_t *data, size_t size, const sections &s, mesh &out, std::string &why);

	// Only the texture commands, in order (cheap: the game's thread reads them to find a new mesh's textures); false
	// when the commands run past the data
	bool texture_commands(const uint8_t *data, size_t size, const sections &s, std::vector<texture_ref> &out);

	// The hide groups a mesh's header byte 3 switches on (02DFAA-02E07A), as bits 1 << kind
	uint32_t hidden_by(uint8_t byte3);

	// Where the en-spell effects go on a weapon (blade_of): a rigid mesh (every vertex one-bone, all on one bone, no
	// mirrored half) in a melee slot, held by its grip at the bone's origin. In the bone's space: the line from where
	// the effect starts (base) to its end (tip), a half width for its sprites, and what kind of piece it is:
	//   kLine   a blade (swords, daggers, katanas, thin clubs and staves): from just past the guard to the tip
	//   kHead   a head much wider than its shaft (axes, scythes, picks, polearms, headed clubs and staves): the head,
	//           its end found by its width (a polearm's spearhead, not its butt); the sprites start on its vertices
	//   kWhole  a compact piece (hand-to-hand, both hands' pieces): all of it
	// Not valid: anything else (ranged weapons and ammo by their slot, two-bone or mirrored meshes).
	struct blade
	{
		static constexpr uint8_t kLine = 0, kHead = 1, kWhole = 2;
		bool valid = false;
		uint8_t kind = kLine;
		uint16_t bone = 0;
		float base[3] = {}, tip[3] = {};
		float half_width = 0.0f;
		uint8_t edge_axis = 0; // the bone's axis across the blade's flat (its wider side): 0 x, 1 y, 2 z
	};
	// A weapon mesh's slot from its chunk name: "wep0" the main hand 0, "wep1" the sub 1, anything else -1 (ranged
	// "wep2", ammo "wep3", not a weapon)
	int weapon_slot(const char name[4]);
	blade blade_of(const mesh &m, int slot);
}
