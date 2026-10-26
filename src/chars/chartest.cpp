/*
 * For the offline probe (research/ffxi-re/tools/dgvprobe, native mode; hifixi/build/chars/probe), which has no game:
 * a frame of characters as the game side would publish it, for HiFi XI to draw at the characters' markers on dgVoodoo's
 * own list. The Mithra NPC (ROM2/0/52.DAT) read from the player's DATs into the cache once, posed by a palette file
 * (tests/chars_pose.py: u32 bones, 16 floats a bone), lit by a day's sun: one opaque in the middle, and with
 * see_through 1 a see-through one beside it (colour alpha 0x40). 1 when the frame is published for seq (once the
 * Mithra is on the GPU), else 0.
 */

#include <windows.h>
#include "cache.hpp"
#include "charframe.hpp"
#include "charinject.hpp"
#include "os2.hpp"
#include "../render/worldfx_core.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	namespace ch = hifi::chars;

	struct mithra
	{
		bool loaded = false;
		std::vector<uint32_t> meshes;
		std::vector<ch::cache::key> keys, texture_keys; // (named each frame, so they're kept)
		std::vector<ch::bone> pose;
	};
	mithra g_mithra;

	void load(const char *pose_file)
	{
		g_mithra.loaded = true;
		{
			std::ifstream f(pose_file, std::ios::binary);
			uint32_t n = 0;
			f.read(reinterpret_cast<char *>(&n), 4);
			for (uint32_t i = 0; i < n && i < 255 && f; ++i)
			{
				float m[16];
				f.read(reinterpret_cast<char *>(m), 64);
				ch::bone b;
				for (int r = 0; r < 4; ++r)
					for (int c = 0; c < 3; ++c)
						b.m[r * 3 + c] = m[r * 4 + c];
				g_mithra.pose.push_back(b);
			}
		}
		std::ifstream f("C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\ROM2\\0\\52.DAT", std::ios::binary);
		std::vector<uint8_t> dat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		struct chunk
		{
			uint32_t kind;
			size_t folder, at, size;
		};
		std::vector<chunk> chunks;
		std::vector<size_t> folders;
		for (size_t off = 0; off + 16 <= dat.size();)
		{
			uint32_t w;
			std::memcpy(&w, dat.data() + off + 4, 4);
			const size_t size = size_t((w >> 7) & 0x7FFFF) * 16;
			if (size < 16 || off + size > dat.size())
				break;
			chunks.push_back({ w & 0x7F, folders.empty() ? 0 : folders.back(), off + 16, size - 16 });
			if ((w & 0x7F) == 1)
				folders.push_back(chunks.size() - 1);
			else if ((w & 0x7F) == 0 && !folders.empty())
				folders.pop_back();
			off += size;
		}
		std::vector<uint32_t> texture_of(chunks.size(), ch::cache::kNone);
		for (size_t i = 0; i < chunks.size(); ++i)
		{
			if (chunks[i].kind != 0x2A)
				continue;
			const uint8_t *d = dat.data() + chunks[i].at;
			ch::os2::sections s;
			std::vector<ch::os2::texture_ref> refs;
			if (!ch::os2::sections_of_chunk(d, chunks[i].size, s) || !ch::os2::texture_commands(d, chunks[i].size, s, refs))
				continue;
			std::vector<uint32_t> ids;
			for (const auto &r : refs)
			{
				uint32_t id = ch::cache::kNone;
				int depth = 0;
				for (size_t j = chunks[i].folder + 1; j < chunks.size(); ++j)
				{
					if (chunks[j].kind == 1)
						depth++;
					if (chunks[j].kind == 0 && --depth < 0)
						break;
					if (chunks[j].kind == 0x20 && chunks[j].size > 17 && std::memcmp(dat.data() + chunks[j].at + 1, r.name, 16) == 0)
					{
						if (texture_of[j] == ch::cache::kNone)
						{
							ch::cache::key k;
							k.resource = 0x7F000000u + uint32_t(j) * 16;
							texture_of[j] = ch::cache::add_texture(k, std::vector<uint8_t>(dat.begin() + chunks[j].at, dat.begin() + chunks[j].at + chunks[j].size));
							g_mithra.texture_keys.push_back(k);
						}
						id = texture_of[j];
						break;
					}
				}
				ids.push_back(id);
			}
			ch::cache::key k;
			k.resource = 0x7E000000u + uint32_t(i) * 16;
			g_mithra.meshes.push_back(ch::cache::add_mesh(k, std::vector<uint8_t>(d, d + chunks[i].size), 0, std::move(ids)));
			g_mithra.keys.push_back(k);
		}
	}
}

namespace hifi::backend
{
	void log(int level, const char *text);
}

extern "C" __declspec(dllexport) int __cdecl hifixi_chars_test(const char *pose_file, uint32_t seq, const float *view_proj, uint32_t see_through)
{
	char v[4] = {};
	const bool trace = GetEnvironmentVariableA("HIFIXI_CHARS_TRACE", v, sizeof(v)) != 0 && v[0] == '1';
	if (trace)
		hifi::backend::log(0, "chars: test frame asked");
	ID3D12Device *const device = worldfx::attached();
	if (device == nullptr || pose_file == nullptr)
		return 0;
	if (!g_mithra.loaded)
		load(pose_file);
	static double now = 1.0;
	now += 0.02;
	ch::cache::frame_tick(device, now);
	std::string why;
	if (trace)
		hifi::backend::log(0, "chars: test frame: cache ticked, preparing");
	if (g_mithra.pose.empty() || g_mithra.meshes.empty() || !ch::inject::prepare(device, why))
		return 0;
	if (trace)
		hifi::backend::log(0, "chars: test frame: prepared");
	// (their sun shadows from their meshes, as the plugin hands them to World FX)
	static bool casters_set = false;
	if (!casters_set)
	{
		worldfx::set_casters(ch::inject::caster_hook(), nullptr);
		casters_set = true;
	}
	for (const ch::cache::key &k : g_mithra.keys)
		ch::cache::mesh(k);
	for (const ch::cache::key &k : g_mithra.texture_keys)
		ch::cache::texture(k);
	for (uint32_t id : g_mithra.meshes)
		if (!ch::cache::mesh_ready(id))
			return 0;
	ch::frame &f = ch::begin_frame(seq);
	std::memcpy(f.view_proj, view_proj, 64);
	f.camera = true;
	const float fog[3] = { 0.55f, 0.65f, 0.78f };
	std::memcpy(f.fog, fog, 12);
	f.fog_start = 150.0f;
	f.fog_end = 600.0f;
	auto add = [&](float dx, uint32_t colour, uint32_t flags) {
		ch::model_entry &m = f.model[f.models];
		m = ch::model_entry();
		m.actor = 0xAC700000u + f.models;
		m.colour = colour;
		m.flags = flags;
		ch::light8 sun = {};
		sun.type = 3;
		sun.diffuse[0] = 1.48f;
		sun.diffuse[1] = 1.47f;
		sun.diffuse[2] = 1.42f;
		sun.direction[0] = 0.31f;
		sun.direction[1] = 0.951f;
		m.lights[0] = sun;
		m.ambient = 0xFF606060u;
		m.tfactor = 0x80808080u;
		m.first_instance = f.instances;
		m.instances = 1;
		ch::instance_entry &ie = f.instance[f.instances++];
		ie.first_bone = f.bones;
		ie.bones = uint32_t(g_mithra.pose.size());
		for (size_t b = 0; b < g_mithra.pose.size(); ++b)
		{
			f.palette[f.bones + b] = g_mithra.pose[b];
			f.palette[f.bones + b].m[9] += dx;
		}
		f.bones += ie.bones;
		ie.first_mesh = f.meshes;
		ie.meshes = uint32_t(g_mithra.meshes.size());
		for (size_t k = 0; k < g_mithra.meshes.size(); ++k)
		{
			f.mesh[f.meshes + k].mesh = g_mithra.meshes[k];
			m.hidden |= ch::os2::hidden_by(ch::cache::mesh_hides(g_mithra.meshes[k]));
		}
		f.meshes += ie.meshes;
		f.models++;
	};
	add(0.0f, 0x80808080u, ch::kNative);
	ch::publish_opaque(f);
	if (see_through != 0)
	{
		add(1.1f, 0x40808080u, ch::kNative | ch::kFromFlush);
		ch::publish_see_through(f);
	}
	ch::cache::publish_palettes(f, 0, f.bones);
	if (trace)
		hifi::backend::log(0, "chars: test frame published");
	return 1;
}

// For the offline probe: the native character renderer's status lines
extern "C" __declspec(dllexport) void __cdecl hifixi_chars_draw_status(char *out, uint32_t size)
{
	std::string all;
	for (const auto &lines : { ch::inject::status(), ch::cache::status() })
		for (const std::string &line : lines)
			all += (all.empty() ? "" : "; ") + line;
	if (out != nullptr && size != 0)
		std::snprintf(out, size, "%s", all.c_str());
}
