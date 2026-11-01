/*
 * Offline test of the character passes' slot markers (src/game/charslot.cpp, src/chars/marks.cpp, slotmark.hpp;
 * research/ffxi-re/notes-characters.md "For a native character renderer", part 1): each FFXiMain dump mapped at its
 * own base (game_dump.hpp).
 *   - kind 4's name;
 *   - an image without the game's code: refused and said why, nothing written;
 *   - 0817E0's call of the walk 082D70 and the queue's immediate (0C7E82: 0CBD90) found; only the call's rel32 and the
 *     immediate changed;
 *   - the walk's stub run (its walk a stand-in): begin, the walk, end; eax returned, ebx/esi/edi kept;
 *   - the wrapper run as the flush calls it (cdecl, the actor; its callback a stand-in): the marker before the
 *     callback, the callback given the actor at [esp+4], from_flush while it runs only, the callback's eax returned;
 *   - through HiFi XI's d3d8.dll (a stand-in for HifiFrameMark): the three kinds with the seq; when it doesn't take them,
 *     counted as not sent;
 *   - the Direct3D 12 side's order check: a frame's begin, end, see-throughs, the next begin; and what's out of order;
 *   - stop(): the dump again, byte for byte; refused while something else changed either site.
 *
 *   build\charslot_test.exe     (built by build.cmd)
 */

#include "../src/chars/marks.hpp"
#include "../src/game/charslot.hpp"
#include "../src/game/slotmark.hpp"
#include "../src/game/directmark.hpp"
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

	// What happened, in order: 'B'/'E'/'S' + seq for a marker, 'W' for the walk, 'C' for the callback
	std::string events;
	uint32_t callback_actor = 0;
	bool flush_in_callback = false;
	void sink(uint32_t kind, uint32_t seq)
	{
		events += kind == hifi::slotmark::chars_begin ? 'B' : kind == hifi::slotmark::chars_end ? 'E' : kind == hifi::slotmark::see_through ? 'S' : '?';
		events += char('0' + seq % 10);
	}
	uint32_t listened_actor = 0;
	void listener(uint32_t kind, uint32_t, uintptr_t actor)
	{
		if (kind == hifi::slotmark::see_through)
			listened_actor = uint32_t(actor);
	}
	uint32_t __cdecl fake_walk()
	{
		events += 'W';
		return 0xC0FFEE00;
	}
	uint32_t __cdecl fake_callback(uint32_t actor)
	{
		events += 'C';
		callback_actor = actor;
		flush_in_callback = hifi::game::charslot::from_flush();
		return 0xBEEF0001;
	}

	struct regs
	{
		uint32_t eax, ebx, esi, edi, esp_diff;
	};
	regs run_walk(uintptr_t stub)
	{
		uint32_t a = 0, b = 0, s = 0, d = 0, before = 0, after = 0;
		__asm {
			push ebx
			push esi
			push edi
			mov ebx, 0x11111111
			mov esi, 0x22222222
			mov edi, 0x33333333
			mov before, esp
			call dword ptr [stub]
			mov after, esp
			mov a, eax
			mov b, ebx
			mov s, esi
			mov d, edi
			pop edi
			pop esi
			pop ebx
		}
		return { a, b, s, d, before - after };
	}
	// As the flush calls a node's callback: push the actor; call; add esp, 4
	regs run_wrapper(uintptr_t wrapper, uint32_t actor)
	{
		uint32_t a = 0, b = 0, s = 0, d = 0, before = 0, after = 0;
		__asm {
			push ebx
			push esi
			push edi
			mov ebx, 0x11111111
			mov esi, 0x22222222
			mov edi, 0x33333333
			mov before, esp
			push actor
			call dword ptr [wrapper]
			add esp, 4
			mov after, esp
			mov a, eax
			mov b, ebx
			mov s, esi
			mov d, edi
			pop edi
			pop esi
			pop ebx
		}
		return { a, b, s, d, before - after };
	}

	// HiFi XI's d3d8.dll as the marks reach it (src/game/directmark.hpp): each recorded; answer 0 as a d3d8.dll of ours
	// with no handler yet does
	std::vector<std::pair<uint32_t, uint32_t>> direct;
	int direct_answer = 1;
	int __cdecl direct_fn(uint32_t kind, uint32_t seq)
	{
		direct.push_back({ kind, seq });
		return direct_answer;
	}

	void test_read()
	{
		check(std::string(hifi::slotmark::kind_name(hifi::slotmark::see_through)) == "see-through character", "kind 4's name");
	}

	void test_order()
	{
		namespace mk = hifi::chars::marks;
		mk::reset();
		const uint32_t B = hifi::slotmark::chars_begin, E = hifi::slotmark::chars_end, S = hifi::slotmark::see_through;
		bool ok = mk::seen(B, 15) && mk::seen(E, 15) && mk::seen(B, 0) && mk::seen(E, 0) && mk::seen(S, 0) && mk::seen(S, 0) && mk::seen(S, 0) &&
			mk::seen(B, 1) && mk::seen(E, 1) && mk::seen(S, 1);
		mk::counts c = mk::get_counts();
		check(ok && c.out_of_order == 0 && c.begin == 3 && c.end == 3 && c.see_through == 4 && c.most_see_through == 3,
			"D3D12 side: frames B E, B E S S S, B E S in order (seq 15 wraps to 0); most see-through in a frame 3");
		check(!mk::seen(S, 2), "  a see-through with the next seq: out of order");
		check(!mk::seen(E, 2), "  an end after a see-through: out of order");
		check(!mk::seen(B, 2), "  a begin after an end of the same seq: out of order");
		check(!mk::seen(B, 4) && mk::seen(E, 4), "  a begin skipping a seq (a frame not marked): out of order, its end then fine");
		check(!mk::seen(0, 4) && mk::get_counts().out_of_order == 4, "  a zone kind: not taken; 4 out of order counted");
		check(mk::status().find("see-through 5") != std::string::npos, "  status: " + mk::status());
		mk::reset();
	}

	void test_dump(const std::string &path)
	{
		namespace cs = hifi::game::charslot;
		std::printf("\n%s\n", path.c_str());
		uintptr_t base = 0;
		const size_t size = dump::map(path, base);
		check(size != 0, "mapped at its base");
		if (size == 0)
			return;
		std::string why;
		check(cs::start(why) && cs::in(), "in" + (why.empty() ? "" : ": " + why));
		const cs::sites s = cs::where();
		check(s.call == base + 0x0817E0 && s.walk == base + 0x082D70, "  the call at 0817E0, its walk 082D70");
		check(s.immediate == base + 0x0C7E82 && s.callback == base + 0x0CBD90, "  the immediate at 0C7E82, its callback 0CBD90");
		const uint8_t *c = reinterpret_cast<const uint8_t *>(s.call);
		check(c[0] == 0xE8 && s.call + 5 + *reinterpret_cast<const int32_t *>(c + 1) == s.stub, "  0817E0: call the stub");
		check(*reinterpret_cast<const uint32_t *>(s.immediate) == s.wrapper, "  0C7E7C: mov [esi+8D0h], the wrapper");
		check(differences(path, base, size) <= 8, "  nothing else changed (" + std::to_string(differences(path, base, size)) + " bytes: the rel32, the immediate)");
		const uint8_t *st = reinterpret_cast<const uint8_t *>(s.stub);
		check(st[0x0C] == 0xE8 && s.stub + 0x11 + *reinterpret_cast<const int32_t *>(st + 0x0D) == s.walk, "  the stub calls the walk at +0x0C");
		const uint8_t *wr = reinterpret_cast<const uint8_t *>(s.wrapper);
		check(wr[0x10] == 0xE8 && s.wrapper + 0x15 + *reinterpret_cast<const int32_t *>(wr + 0x11) == s.callback, "  the wrapper calls the callback at +0x10");

		// The stubs run, the walk and the callback stand-ins (their calls rewritten in our copy)
		*reinterpret_cast<int32_t *>(s.stub + 0x0D) = int32_t(reinterpret_cast<uintptr_t>(&fake_walk) - (s.stub + 0x11));
		*reinterpret_cast<int32_t *>(s.wrapper + 0x11) = int32_t(reinterpret_cast<uintptr_t>(&fake_callback) - (s.wrapper + 0x15));
		cs::set_sink(sink);
		cs::set_listener(listener);
		cs::set_wanted(true);
		events.clear();
		regs r = run_walk(s.stub);
		check(events == "B1WE1", "  walk: begin 1, the walk, end 1 (" + events + ")");
		check(r.eax == 0xC0FFEE00 && r.ebx == 0x11111111 && r.esi == 0x22222222 && r.edi == 0x33333333 && r.esp_diff == 0,
			"  the walk's eax returned; ebx, esi, edi, esp kept");
		events.clear();
		r = run_wrapper(s.wrapper, 0x0A0C7001);
		check(events == "S1C", "  see-through: its marker (seq 1), then the callback (" + events + ")");
		check(callback_actor == 0x0A0C7001 && listened_actor == 0x0A0C7001, "  the callback and the listener had the actor (" + std::to_string(callback_actor) + ", " + std::to_string(listened_actor) + ")");
		check(flush_in_callback && !cs::from_flush(), "  from_flush while the callback runs, not after");
		check(r.eax == 0xBEEF0001 && r.ebx == 0x11111111 && r.esi == 0x22222222 && r.edi == 0x33333333 && r.esp_diff == 0,
			"  the callback's eax returned; ebx, esi, edi, esp kept");
		run_wrapper(s.wrapper, 2);
		events.clear();
		run_walk(s.stub);
		check(events == "B2WE2", "  the next walk: seq 2 (" + events + ")");
		cs::set_wanted(false);
		events.clear();
		run_walk(s.stub);
		run_wrapper(s.wrapper, 3);
		check(events == "WC", "  not wanted: the walk and the callback alone (" + events + ")");
		const cs::counts n = cs::get_counts();
		check(n.seq == 3 && n.sent[2] == 2 && n.sent[3] == 2 && n.sent[4] == 2 && n.see_through == 3 && n.most_see_through == 2,
			"  counts: 3 walks, 2 + 2 + 2 markers, 3 see-through callbacks, most 2 in a frame");

		// Through HiFi XI's d3d8.dll
		cs::set_sink(nullptr);
		hifi::game::set_direct_mark(&direct_fn);
		cs::set_wanted(true);
		direct.clear();
		events.clear();
		run_walk(s.stub);           // (seq 4)
		run_wrapper(s.wrapper, 7);
		check(events == "WC" && direct.size() == 3 && direct[0] == std::make_pair(uint32_t(hifi::slotmark::chars_begin), 4u) &&
			direct[1] == std::make_pair(uint32_t(hifi::slotmark::chars_end), 4u) && direct[2] == std::make_pair(uint32_t(hifi::slotmark::see_through), 4u),
			"  marked before and after the walk and before the callback, seq 4");
		direct_answer = 0;
		const uint64_t not_sent = cs::get_counts().not_sent;
		run_walk(s.stub);
		run_wrapper(s.wrapper, 7);
		check(cs::get_counts().not_sent == not_sent + 3, "  not taken (no handler in the d3d8.dll yet): all 3 counted as not sent");
		check(cs::status().find("not sent 3") != std::string::npos, "  status: " + cs::status());
		direct_answer = 1;
		hifi::game::set_direct_mark(nullptr);
		cs::set_wanted(false);
		cs::set_listener(nullptr);

		// Back, and put back over something else's change
		*reinterpret_cast<int32_t *>(s.stub + 0x0D) = int32_t(s.walk - (s.stub + 0x11));
		*reinterpret_cast<int32_t *>(s.wrapper + 0x11) = int32_t(s.callback - (s.wrapper + 0x15));
		uint8_t *imm = reinterpret_cast<uint8_t *>(s.immediate);
		const uint8_t kept = imm[0];
		imm[0] ^= 0x40;
		why.clear();
		const bool refused = !cs::stop(why) && cs::in();
		check(refused, "  stop() refused while something else changed the immediate: " + why);
		imm[0] = kept;
		check(cs::stop(why) && !cs::in() && dump::same_as_file(path, base, size), "stop(): the dump again, byte for byte");
		check(cs::status().find("not marked") != std::string::npos, "  status: " + cs::status());
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
	namespace cs = hifi::game::charslot;
	test_read();
	test_order();
	std::vector<uint8_t> empty(0x10000, 0);
	hifi::game::set_image(reinterpret_cast<uintptr_t>(empty.data()), empty.size());
	std::string why;
	const bool refused = !cs::start(why) && !cs::in() && !why.empty();
	check(refused, "an image without the game's code: refused, " + why);
	check(cs::stop(why), "  stop(): nothing to put back");

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
