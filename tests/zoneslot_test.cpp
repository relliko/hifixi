/*
 * Offline test of the zone pass's slot markers (src/game/zoneslot.cpp, src/game/slotmark.hpp;
 * research/ffxi-re/notes-replace-slots.md): each FFXiMain dump mapped at its own base (game_dump.hpp).
 *   - the marker's shape read back: the pixel and dgVoodoo-scaled squares, and what isn't one;
 *   - an image without the game's code: refused and said why, nothing written;
 *   - the call found at 178CCE, its pass 17B510; only its rel32 changed, pointing at the stub;
 *   - the stub run (its pass swapped for a stand-in): begin, the pass with the renderer in ecx, end, in that order;
 *     the pass's eax returned, ebx/esi/edi kept; the seq counts zone passes; not wanted: the pass alone;
 *   - through a stand-in device: Clear(1, {x, 0, x + 1, 1}, STENCIL, 0, 1.0, 0) at x = 16 kind + seq, the depth
 *     surface released; a depth without stencil: no Clear, counted as not sent;
 *   - stop(): the dump again, byte for byte; refused while something else changed the call.
 *
 *   build\zoneslot_test.exe     (built by build.cmd)
 */

#include "../src/game/slotmark.hpp"
#include "../src/game/zoneslot.hpp"
#include "game_dump.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

	size_t differences(const std::string &path, uintptr_t base, size_t size)
	{
		FILE *f = nullptr;
		if (fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr)
			return size_t(-1);
		std::vector<uint8_t> file(size);
		const size_t got = fread(file.data(), 1, size, f);
		fclose(f);
		if (got != size)
			return size_t(-1);
		size_t n = 0;
		const uint8_t *image = reinterpret_cast<const uint8_t *>(base);
		for (size_t i = 0; i < size; ++i)
			n += image[i] != file[i];
		return n;
	}

	// What happened, in order: 'B'/'E' + seq for a marker, 'P' for the pass
	std::string events;
	uint32_t pass_ecx = 0;
	void sink(uint32_t kind, uint32_t seq)
	{
		events += kind == hifi::slotmark::zone_begin ? 'B' : kind == hifi::slotmark::zone_end ? 'E' : '?';
		events += char('0' + seq % 10);
	}
	uint32_t __fastcall fake_pass(uint32_t ecx)
	{
		pass_ecx = ecx;
		events += 'P';
		return 0xC0FFEE00;
	}

	struct regs
	{
		uint32_t eax, ebx, esi, edi;
	};
	regs run_stub(uintptr_t stub, uint32_t renderer)
	{
		regs r = {};
		uint32_t a = 0, b = 0, s = 0, d = 0;
		__asm {
			push ebx
			push esi
			push edi
			mov ebx, 0x11111111
			mov esi, 0x22222222
			mov edi, 0x33333333
			mov ecx, renderer
			call dword ptr [stub]
			mov a, eax
			mov b, ebx
			mov s, esi
			mov d, edi
			pop edi
			pop esi
			pop ebx
		}
		r = { a, b, s, d };
		return r;
	}

	// A stand-in Direct3D 8 device: GetDepthStencilSurface (33) hands out a surface whose GetDesc (8) has the format
	// below; Clear (36) is recorded
	uint32_t depth_format = 75;
	int surfaces_out = 0, clears = 0;
	uint32_t clear_count = 0, clear_flags = 0, clear_stencil = 99;
	int32_t clear_rect[4] = {};
	float clear_z = 0.0f;
	uint32_t surface_vt[16] = {}, device_vt[40] = {};
	uint32_t surface_obj[1] = {}, device_obj[1] = {};
	unsigned long __stdcall s_release(uintptr_t) { surfaces_out--; return 0; }
	long __stdcall s_get_desc(uintptr_t, uint32_t desc[8])
	{
		std::memset(desc, 0, 32);
		desc[0] = depth_format;
		return 0;
	}
	long __stdcall d_get_ds(uintptr_t, uintptr_t *out)
	{
		*out = reinterpret_cast<uintptr_t>(surface_obj);
		surfaces_out++;
		return 0;
	}
	long __stdcall d_clear(uintptr_t, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t, float z, uint32_t stencil)
	{
		clears++;
		clear_count = count;
		std::memcpy(clear_rect, rects, sizeof(clear_rect));
		clear_flags = flags;
		clear_z = z;
		clear_stencil = stencil;
		return 0;
	}
	void make_device()
	{
		surface_vt[2] = uint32_t(reinterpret_cast<uintptr_t>(&s_release));
		surface_vt[8] = uint32_t(reinterpret_cast<uintptr_t>(&s_get_desc));
		surface_obj[0] = uint32_t(reinterpret_cast<uintptr_t>(surface_vt));
		device_vt[33] = uint32_t(reinterpret_cast<uintptr_t>(&d_get_ds));
		device_vt[36] = uint32_t(reinterpret_cast<uintptr_t>(&d_clear));
		device_obj[0] = uint32_t(reinterpret_cast<uintptr_t>(device_vt));
	}

	void test_read()
	{
		using hifi::slotmark::read;
		hifi::slotmark::mark m;
		check(read(false, true, 1, 16 + 5, 0, 16 + 6, 1, m) && m.kind == 1 && m.seq == 5, "a marker's pixel read back: x 21 = zone end, seq 5");
		check(read(false, true, 1, 2 * 37, 0, 2 * 37 + 2, 2, m) && m.kind == 2 && m.seq == 5, "  a 2x2 square at 2 x 37 (a world scaled 2x): kind 2, seq 5");
		check(!read(true, true, 1, 3, 0, 4, 1, m), "  depth cleared too: not a marker");
		check(!read(false, true, 0, 3, 0, 4, 1, m) && !read(false, true, 2, 3, 0, 4, 1, m), "  no rect, two rects: not markers");
		check(!read(false, true, 1, 3, 1, 4, 2, m), "  not in row 0: not a marker");
		check(!read(false, true, 1, 3, 0, 5, 1, m), "  not a square: not a marker");
		check(!read(false, true, 1, 3, 0, 5, 2, m), "  a 2x2 square at an odd x: not a marker");
		check(!read(false, true, 1, 80, 0, 81, 1, m), "  x 80 (past the kinds): not a marker");
		check(!read(false, false, 1, 3, 0, 4, 1, m), "  nothing cleared: not a marker");
		check(hifi::slotmark::x_of(hifi::slotmark::zone_end, 21) == 16 + 5, "  x_of: 16 kind + the seq's low 4 bits");
	}

	void test_dump(const std::string &path)
	{
		namespace zs = hifi::game::zoneslot;
		std::printf("\n%s\n", path.c_str());
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		check(size != 0, "mapped at its base");
		if (size == 0)
			return;
		std::string why;
		check(zs::start(why) && zs::in(), "in" + (why.empty() ? "" : ": " + why));
		const zs::sites s = zs::where();
		check(s.call == base + 0x178CCE && s.pass == base + 0x17B510, "  the call at 178CCE, its pass 17B510");
		const uint8_t *c = reinterpret_cast<const uint8_t *>(s.call);
		check(c[0] == 0xE8 && s.call + 5 + *reinterpret_cast<const int32_t *>(c + 1) == s.stub, "  178CCE: call the stub");
		check(differences(path, base, size) <= 4, "  nothing else changed (" + std::to_string(differences(path, base, size)) + " bytes, the rel32)");
		const uint8_t *st = reinterpret_cast<const uint8_t *>(s.stub);
		check(st[0x0D] == 0xE8 && s.stub + 0x12 + *reinterpret_cast<const int32_t *>(st + 0x0E) == s.pass, "  the stub calls the pass at +0x0D");

		// The stub run, its pass a stand-in (its call rewritten in our copy)
		const uintptr_t fake = reinterpret_cast<uintptr_t>(&fake_pass);
		*reinterpret_cast<int32_t *>(s.stub + 0x0E) = int32_t(fake - (s.stub + 0x12));
		zs::set_sink(sink);
		zs::set_wanted(true);
		events.clear();
		const regs r = run_stub(s.stub, 0x5A5A0001);
		check(events == "B1PE1", "  run: begin 1, the pass, end 1 (" + events + ")");
		check(pass_ecx == 0x5A5A0001, "  the pass had the renderer in ecx");
		check(r.eax == 0xC0FFEE00 && r.ebx == 0x11111111 && r.esi == 0x22222222 && r.edi == 0x33333333, "  the pass's eax returned; ebx, esi, edi kept");
		events.clear();
		run_stub(s.stub, 1);
		check(events == "B2PE2", "  again: seq 2 (" + events + ")");
		zs::set_wanted(false);
		events.clear();
		run_stub(s.stub, 1);
		check(events == "P", "  not wanted: the pass alone (" + events + ")");
		check(zs::get_counts().seq == 3 && zs::get_counts().sent[0] == 2 && zs::get_counts().sent[1] == 2, "  counts: 3 passes, 2 + 2 markers");

		// Through a stand-in device
		zs::set_sink(nullptr);
		make_device();
		zs::set_device(reinterpret_cast<uintptr_t>(device_obj));
		zs::set_wanted(true);
		depth_format = 75;
		clears = 0;
		events.clear();
		run_stub(s.stub, 1); // (seq 4)
		check(clears == 2 && events == "P", "  D24S8 depth: two Clears around the pass");
		check(clear_count == 1 && clear_rect[0] == 16 * 1 + 4 && clear_rect[1] == 0 && clear_rect[2] == 16 * 1 + 5 && clear_rect[3] == 1,
			"  the end's: one rect {" + std::to_string(clear_rect[0]) + ", " + std::to_string(clear_rect[1]) + ", " + std::to_string(clear_rect[2]) + ", " +
			std::to_string(clear_rect[3]) + "} = x 16 + seq 4");
		check(clear_flags == 4 && clear_stencil == 0 && clear_z == 1.0f, "  stencil only, to 0 (z 1.0)");
		check(surfaces_out == 0, "  the depth surface released");
		hifi::slotmark::mark m;
		check(hifi::slotmark::read(false, true, 1, clear_rect[0], clear_rect[1], clear_rect[2], clear_rect[3], m) && m.kind == 1 && m.seq == 4,
			"  read back as zone end, seq 4");
		depth_format = 77; // (D24X8)
		clears = 0;
		const uint64_t not_sent = zs::get_counts().not_sent;
		run_stub(s.stub, 1);
		check(clears == 0 && zs::get_counts().not_sent == not_sent + 2, "  a depth without stencil: no Clear, both counted as not sent");
		check(zs::status().find("not sent 2") != std::string::npos, "  status: " + zs::status());
		zs::set_device(0);
		zs::set_wanted(false);

		// Back, and put back over something else's change
		*reinterpret_cast<int32_t *>(s.stub + 0x0E) = int32_t(s.pass - (s.stub + 0x12));
		uint8_t *call = reinterpret_cast<uint8_t *>(s.call);
		const uint8_t kept = call[4];
		call[4] ^= 0x40;
		why.clear();
		const bool refused = !zs::stop(why) && zs::in();
		check(refused, "  stop() refused while something else changed the call: " + why);
		call[4] = kept;
		check(zs::stop(why) && !zs::in() && dump::same_as_file(path, base, size), "stop(): the dump again, byte for byte");
		check(zs::status().find("not marked") != std::string::npos, "  status: " + zs::status());
		dump::unmap(base);
	}
}

// (HiFi XI's options and World FX's settings: not looked at)
extern "C" int __cdecl hifixi_option(const char *, const char *, char *, uint32_t) { return 0; }
extern "C" void __cdecl hifixi_set_option(const char *, const char *, const char *) {}
extern "C" int __cdecl worldfx_setting(const char *, char *, uint32_t) { return 0; }
extern "C" const char *__cdecl worldfx_folder() { return ""; }

int main()
{
	namespace zs = hifi::game::zoneslot;
	test_read();
	std::vector<uint8_t> empty(0x10000, 0);
	hifi::game::set_image(reinterpret_cast<uintptr_t>(empty.data()), empty.size());
	std::string why;
	const bool refused = !zs::start(why) && !zs::in() && !why.empty();
	check(refused, "an image without the game's code: refused, " + why);
	check(zs::stop(why), "  stop(): nothing to put back");

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
	return failures;
}
