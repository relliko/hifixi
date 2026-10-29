/*
 * Offline test of the world's sub-pixel jitter (src/game/jitter.cpp; research/ffxi-re/notes-fsr.md, milestone 2): each
 * FFXiMain dump mapped at its own base (game_dump.hpp).
 *   - an image without the game's code: refused and said why, nothing written;
 *   - the four calls found (015490, 0467E0, 177420, 046070 in the 04490000 client), all to one builder; the character
 *     silhouettes' call (033800) left alone; only the four rel32s changed, each to ours;
 *   - the builder run through ours (the dump's own code): off, the very matrix the game builds (F 242 at 16:9); on, the
 *     same but _31 and _32 moved by minus the frame's NDC offset (2 px / width, -2 py / height for A's 5120 x 2880);
 *   - the sequence: FSR 2's own (ffxFsr2GetJitterOffset: Halton 2 and 3 of index % phases + 1, less 0.5), a new offset
 *     each next_frame, all inside half a pixel, every phase different;
 *   - stop(): the dump again, byte for byte; off again, no offset.
 *
 *   build\jitter_test.exe     (built by build.cmd)
 */

#include "../src/game/jitter.hpp"
#include "game_dump.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (!ok)
			failures++;
		else
			passes++;
	}

	// FSR 2's own (ffx_fsr2.cpp halton + ffxFsr2GetJitterOffset), as a reference
	float fsr2_halton(int32_t index, int32_t base)
	{
		float f = 1.0f, result = 0.0f;
		for (int32_t i = index; i > 0;)
		{
			f /= float(base);
			result = result + f * float(i % base);
			i = int32_t(std::floor(float(i) / float(base)));
		}
		return result;
	}

	using builder_fn = float *(__stdcall *)(float *, float, float, float, float);

	void test_dump(const std::string &path)
	{
		namespace jt = hifi::game::jitter;
		std::printf("\n%s\n", path.c_str());
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		check(size != 0, "mapped at its base");
		if (size == 0)
			return;
		std::string why;
		check(jt::start(why) && jt::in(), "in" + (why.empty() ? "" : ": " + why));
		const jt::sites s = jt::where();
		if (base == 0x04490000)
			check(s.calls[0] == base + 0x0154D5 && s.calls[1] == base + 0x046F48 && s.calls[2] == base + 0x1774B7 &&
				s.calls[3] == base + 0x04630C && s.builder == base + 0x2D83C6, "  the calls at 0154D5, 046F48, 1774B7, 04630C; the builder 2D83C6");
		bool all_ours = true;
		for (uintptr_t c : s.calls)
		{
			const uint8_t *p = reinterpret_cast<const uint8_t *>(c);
			const uintptr_t to = c + 5 + *reinterpret_cast<const int32_t *>(p + 1);
			all_ours = all_ours && c != 0 && p[0] == 0xE8 && to != s.builder && to != 0;
		}
		check(all_ours, "  each of the four calls through ours");
		// (the silhouettes' call, the only other caller in the 04490000 client, still the builder's)
		if (base == 0x04490000)
		{
			const uint8_t *p = reinterpret_cast<const uint8_t *>(base + 0x0339EE);
			check(p[0] == 0xE8 && base + 0x0339EE + 5 + *reinterpret_cast<const int32_t *>(p + 1) == s.builder, "  the character silhouettes' call (0339EE) left alone");
		}

		// The game's camera projection: F 242 (fovY 2 atan(192/242)), aspect 0.9482 x 16/9, near 0.1, far 65535
		const float fovy = 2.0f * std::atan(192.0f / 242.0f), aspect = 0.75f * (16.0f / 9.0f) / 0.791f;
		float game[16] = {}, off[16] = {}, on[16] = {};
		reinterpret_cast<builder_fn>(s.builder)(game, fovy, aspect, 0.1f, 65535.0f);
		jt::set_on(false);
		jt::next_frame();
		jt::build(off, fovy, aspect, 0.1f, 65535.0f);
		check(std::memcmp(game, off, 64) == 0 && std::fabs(game[5] - 242.0f / 192.0f) < 1e-4f && game[11] == -1.0f,
			"  off: the very matrix the game builds (_22 " + std::to_string(game[5]) + ", _34 -1)");

		// On: (the dump has no game object at its pointer: A's size from the zone renderer's place won't read, so a
		// stand-in game object with A's size is put where gGame's pointer points)
		std::vector<uint8_t> fake_game(0x400, 0);
		const uint16_t a_size[2] = { 5120, 2880 };
		std::memcpy(fake_game.data() + 0x18, a_size, 4);
		uint32_t game_ptr_at = 0;
		{
			// gGame's pointer: the operand of "mov eax, [gGame]; add eax, 94h; ret" (the camera reader's pattern)
			const uint8_t want[] = { 0x05, 0x94, 0x00, 0x00, 0x00, 0xC3 };
			for (size_t i = 0; i + 11 < size && game_ptr_at == 0; ++i)
			{
				const uint8_t *p = reinterpret_cast<const uint8_t *>(base + i);
				if (p[0] == 0xA1 && std::memcmp(p + 5, want, sizeof(want)) == 0)
					game_ptr_at = *reinterpret_cast<const uint32_t *>(p + 1);
			}
		}
		check(game_ptr_at >= base && game_ptr_at < base + size, "  gGame's pointer found in the dump");
		uint32_t saved_ptr = 0;
		if (game_ptr_at != 0)
		{
			std::memcpy(&saved_ptr, reinterpret_cast<void *>(uintptr_t(game_ptr_at)), 4);
			const uint32_t fake = uint32_t(reinterpret_cast<uintptr_t>(fake_game.data()));
			std::memcpy(reinterpret_cast<void *>(uintptr_t(game_ptr_at)), &fake, 4);
		}
		jt::set_on(true);
		jt::set_phases(32);
		std::set<std::pair<float, float>> seen;
		bool inside = true, matches = true, moved = true, same_rest = true;
		for (uint32_t k = 0; k < 32; ++k)
		{
			jt::next_frame();
			const jt::offset o = jt::current();
			const float rx = fsr2_halton(int32_t(o.index % 32) + 1, 2) - 0.5f, ry = fsr2_halton(int32_t(o.index % 32) + 1, 3) - 0.5f;
			matches = matches && std::fabs(o.px[0] - rx) < 1e-6f && std::fabs(o.px[1] - ry) < 1e-6f;
			inside = inside && std::fabs(o.px[0]) <= 0.5f && std::fabs(o.px[1]) <= 0.5f;
			seen.insert({ o.px[0], o.px[1] });
			jt::build(on, fovy, aspect, 0.1f, 65535.0f);
			moved = moved && std::fabs(on[8] - (game[8] - 2.0f * o.px[0] / 5120.0f)) < 1e-7f && std::fabs(on[9] - (game[9] + 2.0f * o.px[1] / 2880.0f)) < 1e-7f;
			for (int i = 0; i < 16; ++i)
				if (i != 8 && i != 9)
					same_rest = same_rest && on[i] == game[i];
		}
		check(matches && inside && seen.size() == 32, "  on: FSR 2's sequence (Halton 2, 3), 32 phases all different, each inside half a pixel");
		check(moved && same_rest, "  on: _31 -= 2 px / 5120 and _32 += 2 py / 2880, the rest as the game's");
		{
			// A point straight ahead moves by exactly the offset on screen (pixels of A)
			const jt::offset o = jt::current();
			const float v[4] = { 3.0f, -2.0f, -40.0f, 1.0f };
			float c0[4] = {}, c1[4] = {};
			for (int j = 0; j < 4; ++j)
				for (int i = 0; i < 4; ++i)
				{
					c0[j] += v[i] * game[i * 4 + j];
					c1[j] += v[i] * on[i * 4 + j];
				}
			const float dx = (c1[0] / c1[3] - c0[0] / c0[3]) * 0.5f * 5120.0f, dy = -(c1[1] / c1[3] - c0[1] / c0[3]) * 0.5f * 2880.0f;
			check(std::fabs(dx - o.px[0]) < 2e-3f && std::fabs(dy - o.px[1]) < 2e-3f,
				"  a point on screen moved by the offset (" + std::to_string(dx) + ", " + std::to_string(dy) + " px)");
		}
		if (game_ptr_at != 0)
			std::memcpy(reinterpret_cast<void *>(uintptr_t(game_ptr_at)), &saved_ptr, 4);
		jt::set_on(false);
		check(jt::stop(why) && !jt::in() && dump::same_as_file(path, base, size), "stop(): the dump again, byte for byte");
		jt::next_frame();
		check(jt::current().px[0] == 0.0f && jt::current().px[1] == 0.0f, "  out: no offset");
		dump::unmap(base);
	}
}

extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main()
{
	namespace jt = hifi::game::jitter;
	std::vector<uint8_t> empty(0x10000, 0);
	hifi::game::set_image(reinterpret_cast<uintptr_t>(empty.data()), empty.size());
	std::string why;
	const bool refused = !jt::start(why) && !jt::in() && !why.empty();
	check(refused, "an image without the game's code: refused, " + why);
	check(jt::stop(why), "  stop(): nothing to put back");
	for (const char *path : dump::kDumps)
	{
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			check(false, std::string(path) + " is there");
			continue;
		}
		test_dump(path);
	}
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
