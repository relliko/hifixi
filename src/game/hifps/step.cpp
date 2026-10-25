#include <windows.h>
#include "step.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// The hifps Ashita addon's hifps.lua (0.9.9), its frame step ported (its comments carried over where they still hold):
// the site tables, the smoothing loops, the name pulse, the divisor, enable() and disable(), the limiter and the frame
// rate's measurement. Its experimental toggles are exp.cpp; its graphics settings (the game's own lights) aren't HiFi
// XI's; its config window is HiFi XI's menu; bisect, the leak hunt and /hifps mem (debugging) are left out.
//
// The addon's history, as far as it explains the tables:
//   v0.4: the music-start and push-through-entity sites moved to whole ticks (found by bisecting).
//   v0.5: camera follow smoothing. The camera eases toward its target with a fixed-step loop (pos += (target - pos) *
//   k, once per whole tick). With whole ticks above 60 fps it only moved every other frame, which looked like blur
//   while turning. Those loops now run once per frame with k rescaled to the real step: k' = 1 - (1 - k)^step, which
//   matches stock exactly at whole ticks.
//   v0.6: the camera distance/collision loop (also per whole tick) runs once per frame too. Its gains and its delay
//   timer read float constants from memory; those instructions are pointed at our own slots, rewritten every frame:
//   gains as 1 - (1 - g)^step, per-tick amounts as v * step.
//   v0.7: works alongside camera addons that redirect the same operands (xicamera points the two 0.125 "jitter" gains
//   at its own 1.0). An operand that points somewhere else is left alone; if its value behaves the same at any frame
//   rate (a gain of 0 or 1, a per-tick amount of 0) the loop still runs once per frame, otherwise that loop is left
//   alone with a warning. Operands are re-checked every frame, so either load order works, and our memory is kept
//   alive on unload if another addon might still write a pointer to it back.
//   v0.8: other characters' movement and turning (CXiSkeletonActor::Update), the same loop counted down from the
//   fractional step: ceil(step) passes, 1 or 2 by whether a frame took longer than 1/60 s; everyone microstuttered.
//   v0.8.1: walking animations: constants meant per 1/60 s scaled to the frame's step.
//   v0.9.4: the target's name pulse counts whole ticks from real time.
//   v0.9.5: how other players see you walk: the 0.02 'moved' constant scaled, the standing count on the 1.0 stub.

namespace hifi::game::hifps
{
	namespace
	{
		// Call sites (offsets in the analysed client, FFXiMain_04490000.bin) that use the fractional step.
		const uint32_t kFracSites[] = {
			0x00766F, 0x01A9AB, 0x01B5D4, 0x01C5AF, 0x01F01F, 0x01F0F2, 0x01F82F, 0x01F87F,
			0x01F8D5, 0x01F91D, 0x0205B1, 0x02198E, 0x02AD55, 0x033ED1, 0x033FAB, 0x035944,
			0x0368B6, 0x0368CE, 0x0381D4, 0x0478BF, 0x0478D7, 0x06A000, 0x08D383,
			0x08D38F, 0x08D39D, 0x08D61D, 0x08D62B, 0x08D639, 0x08D647, 0x08D655, 0x09729C,
			0x0A57F9, 0x0A7B8A, 0x0A9FC7, 0x0AA1F7, 0x0AAB78, 0x0AADF5, 0x0B0829,
			0x0B08BF, 0x0B2F4E, 0x0B2F9E, 0x0B2FE1, 0x0B32A8, 0x0B32F0, 0x0B3333, 0x0B3376,
			0x0B56F2, 0x0B57FD, 0x0B5840, 0x0B587D, 0x0C4878, 0x0C648E,
			0x0C76CA, 0x0C7F13, 0x0C8AC8, 0x0C8AE7, 0x0CAEEA, 0x0CBC68, 0x0CDF96,
			0x12C179, 0x12C734, 0x12CBCF, 0x178BD0, 0x17A4BB, 0x188A54, 0x1EDF4C, 0x1EF1AC,
			0x1EF1D2, 0x1EF282, 0x1EF334, 0x1EF3DE, 0x1EF541, 0x1EF55D, 0x1EF570, 0x1EF590,
			0x1EF5AC, 0x1EF5BF, 0x1EF6FE, 0x1EF711, 0x1EFA27, 0x227E58, 0x24DE98, 0x24DF4E,
			0x24EBCA, 0x24EC1F, 0x24EE6D, 0x24EECB, 0x24F019, 0x24F6E6,
			// Moved from the whole-tick list: scales the step into the lock-on run animation blend (bisect, v0.7.3).
			0x0A5194,
		};
		// Call sites that truncate the step to an integer.
		const uint32_t kIntSites[] = {
			0x005A50, 0x007BB6, 0x007D74, 0x018A10, 0x018D0E, 0x01BC9C, 0x01EEFE,
			0x020148, 0x021185, 0x0211C1, 0x021207,
			0x021243, 0x0219A0, 0x0375A0, 0x0375FB, 0x039366, 0x087495, 0x087E99, 0x0884CC,
			0x0884F5, 0x0885B1, 0x0886B3, 0x088CA7, 0x088CD4, 0x088CF9, 0x088E63, 0x088ECF,
			0x089061, 0x0891FF, 0x089228, 0x0892E7, 0x0893CB, 0x0898F5, 0x089933, 0x089958,
			0x089AFB, 0x089B67, 0x08ADD1, 0x08AE39, 0x08D34B, 0x08D3BE, 0x08D663, 0x08D671,
			0x08DAAB, 0x08EA21, 0x08ED66, 0x091AD3, 0x09670D, 0x097271, 0x0984D5,
			0x09F661, 0x0A2843, 0x0AC55D, 0x0AECC0, 0x0B035D, 0x0B578B, 0x0B76A4,
			0x0B7725, 0x0C3568, 0x0C35E9, 0x0CB4ED, 0x1175D5, 0x117766, 0x11777B, 0x1177A2,
			0x11966C, 0x11970C, 0x119F31, 0x1248DC, 0x124D47, 0x1250B0, 0x1251FE, 0x125430,
			0x125C00, 0x1268A9, 0x12B7FA, 0x12E8EC, 0x133ABE, 0x1375ED, 0x13A033, 0x13A616,
			0x14CB7B, 0x1500EB, 0x159A6C, 0x15DE2B, 0x15FF0A, 0x1628E5, 0x162913, 0x162956,
			0x18C273, 0x18C293, 0x19EAF2, 0x1CE91B, 0x1D4008, 0x1D54BA, 0x1D5530, 0x1D7AD0,
			0x1E187B, 0x1EEFDA, 0x1EF008, 0x1EF067, 0x1FB9DC, 0x1FBA03, 0x2014F6, 0x2160A3,
			0x2164C3, 0x21F263, 0x21F2C0, 0x220364, 0x220998, 0x220B14, 0x220EA2, 0x221428,
			0x221492, 0x24C316, 0x24E8BE, 0x24EB4D,
			// Found by bisecting: these truncate after a jump, so the scan missed them.
			0x03746F, // music start countdown
			0x0A8778, // push-through-entity delay
		};
		// Call sites that get the 1.0 stub, the stock 60 fps step, at any frame rate.
		const uint32_t kStockSites[] = {
			// The position packet's movement counter while standing: 'counter + (int)step', which is 1 at stock. Whole
			// ticks made it 0 or 1 frame to frame (v0.9.5).
			0x0984F6,
		};

		// Fixed-step smoothing loops: "n = (int)step; repeat n times: v += (target - v) * k", with k pushed as an
		// immediate float. Their call sites get the 1.0 stub (one pass per frame) and the immediate is rewritten every
		// frame to 1 - (1 - k)^step. imm = offset of the 4 immediate bytes, pre = the bytes in front of them (default
		// 68, a push).
		// ops: instructions in the loop that read a float constant from memory ("fmul/fsub dword [const]"). at = offset
		// of the instruction's disp32, constant = offset of the constant it reads. The disp32 is pointed at a slot of
		// ours; ease gets 1 - (1 - v)^step, lin v * step, pow v^step.
		// fallback: how the sites are fed if another addon changed the loop (whole ticks unless said).
		enum class mode : char
		{
			ease = 'e',
			lin = 'l',
			pow = 'p'
		};
		struct op_info
		{
			uint32_t at, constant;
			mode how;
		};
		struct loop_info
		{
			const char *name;
			const char *effect;
			char fallback;
			std::vector<uint32_t> sites;
			uint32_t imm;              // (0: none)
			std::vector<uint8_t> pre;
			double k;
			std::vector<op_info> ops;
		};
		constexpr const char *kCameraEffect = "That part of the camera may blur or move at the wrong speed above 60 fps. Unload the other camera addon, or set the frame cap to 60.";
		constexpr const char *kActorEffect = "Other characters may move or turn unevenly above 60 fps. Unload the other addon, or set the frame cap to 60.";
		constexpr const char *kMoveEffect = "Other players may see you walk in bursts above 60 fps. Unload the other addon, or set the frame cap to 60.";
		const std::vector<loop_info> kLoops = {
			{ "camera follow", kCameraEffect, 'i', { 0x01F5A2 }, 0x01F5D4, { 0x68 }, 0.25, {} },
			{ "camera ease", kCameraEffect, 'i', { 0x01F66B, 0x01F711 }, 0x01F6C7, { 0x68 }, 0.05, {} },
			{ "camera distance/collision", kCameraEffect, 'i', { 0x01FA38, 0x01FE7C }, 0, {}, 0.0, {
				{ 0x01FA57, 0x32961C, mode::lin },   // delay timer -= 1.0
				{ 0x01FCCA, 0x32A3E0, mode::lin },   // per-tick move amount
				{ 0x01FD28, 0x329A08, mode::ease },  // pull in when too far
				{ 0x01FD4E, 0x32A3C4, mode::ease },  // push out when too close
				{ 0x01FDA7, 0x32A3C0, mode::ease },
				{ 0x01FE38, 0x32A3BC, mode::ease },
				{ 0x01FE48, 0x32A3BC, mode::ease },
			} },
			// CXiSkeletonActor::Update, every character: "n = step; do { v += (target - v) * k; n -= 1 } while (n > 0)".
			// Counted down from the fractional step it ran ceil(step) passes, so a frame just over 1/60 s moved everyone
			// almost twice as far (bisect, v0.8). Fallback: the fractional step as before.
			{ "character movement", kActorEffect, 'f', { 0x0C65BA }, 0, {}, 0.0, {
				{ 0x0C65F0, 0x32A3BC, mode::ease },  // position += (target - position) * 0.125: x
				{ 0x0C6601, 0x32A3BC, mode::ease },  // y
				{ 0x0C660F, 0x32A3BC, mode::ease },  // z
			} },
			// Turning, two copies of the same loop. k is 0.125 (a "mov [esp+14], imm"), or the entity's speed (percent,
			// 100 by default) * 0.00125, which can only be scaled linearly here: within ~3% at 100.
			{ "character turning", kActorEffect, 'f', { 0x0C6882 }, 0x0C684B, { 0xC7, 0x44, 0x24, 0x14 }, 0.125, {
				{ 0x0C687A, 0x331364, mode::lin },
			} },
			{ "character turning", kActorEffect, 'f', { 0x0C6C64 }, 0x0C6C2D, { 0xC7, 0x44, 0x24, 0x14 }, 0.125, {
				{ 0x0C6C5C, 0x331364, mode::lin },
			} },
			// The walk/run animation, after the movement loop (no call sites of its own): this frame's movement under
			// 0.01 counts as none (0C7100; it's still applied, but the animation code gets a zero vector), under 0.0001
			// as speed 0 (0C8A74, in 0C8930); the animation speed (movement against the character's speed for the
			// frame) is smoothed into [actor+824] (0C8B30).
			{ "character animation", kActorEffect, 'i', {}, 0, {}, 0.0, {
				{ 0x0C7102, 0x329A18, mode::lin },   // moved at all: 0.01 a tick
				{ 0x0C8A76, 0x32A1A8, mode::lin },   // any speed: 0.0001 a tick
				{ 0x0C8B38, 0x329D34, mode::pow },   // smoothing: old * 0.75 ...
				{ 0x0C8B40, 0x329CE4, mode::ease },  // ... + new * 0.25
			} },
			// The 'moving' flag (CXiControlActor::Update, 0A5BA9): this frame's movement over 0.02 sets [actor+F8].
			// While it is set, the position packet (0983F0) counts the ticks spent moving and sends the count as
			// MoveFlame; a frame without it starts the count over. Other players' clients play your walk over the
			// count's change (08CD20), so a short frame that moved under 0.02 made you walk in bursts to them (v0.9.5).
			{ "movement flag", kMoveEffect, 'i', {}, 0, {}, 0.0, {
				{ 0x0A5BAB, 0x32B7B4, mode::lin },   // moved: 0.02 a tick
			} },
		};
		constexpr uint32_t kMaxOps = 24;

		// Frame step getter: mov ecx, [obj]; fld [ecx+28]; fcomp [1.0]; fnstsw ax; test ah, 5; jp +7; fld [1.0]; ret;
		// fld [ecx+28]; ret
		constexpr const char *kGetter = "8B0D????????D94128D81D????????DFE0F6C4057A07D905????????C3D94128C3";
		// The fps divisor: the game object's +0x30 (the global at +0x0C)
		constexpr const char *kDivisor = "81EC000100003BC174218B0D";
		// The target's name pulse: 'mov cl, [esi+3]; mov ebp, ecx; call <frame count>; shl eax, 4; cdq; mov ecx, 360' in
		// GetNamePlace's colour (0x083192); the call at +5. Our count and its getter live after the operand slots.
		constexpr const char *kPulse = "8A4E038BE9E8????????C1E00499B968010000";
		// The frame count getter it calls: mov eax, [game]; mov eax, [eax+34h]; ret
		constexpr const char *kFrameCount = "A1????????8B4034C3";
		// What an earlier hifps left a site calling: fld dword [x]; ret
		constexpr const char *kOldStub = "D905????????C3";
		constexpr uint32_t kPulseOff = 32 + kMaxOps * 4;
		// Our memory: two step floats, and an executable stub "fld dword [1.0]; ret" (+16, its 1.0 at +24). +32: one
		// float slot per redirected operand. Then the name pulse's tick count (u32) and its getter
		// "mov eax, [count]; ret".
		constexpr uint32_t kMemSize = kPulseOff + 16;

		constexpr double kMinStep = 0.05; // ticks; ~1200 fps
		constexpr double kMaxStep = 4.0;  // ticks; hitches longer than this slow the game down like before
		constexpr uint32_t kForeignMax = 8;

		// The limiter: a high-resolution waitable timer for most of the wait, then a short spin to the frame's time.
		// Without one (before Windows 10 1803, some Wine), Sleep as the addon did: most of the time left when over
		// 2 ms, 1.5 ms of it spun.
		constexpr double kSpin = 0.0005;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

		struct site_patch
		{
			uintptr_t at;
			char kind;
			uint8_t backup[4];
			uintptr_t target;
			bool patched;
		};
		struct smooth_patch
		{
			uintptr_t at;
			double k;
			uint8_t backup[4];
			uint8_t ours[4]; // (what is there by our doing: the backup until the first frame writes)
			DWORD prot;
			const char *name, *effect;
			std::string warning; // (something else wrote it since: left to that)
		};
		struct op_patch
		{
			uintptr_t at;
			uint32_t constant; // (where the game's constant is)
			double v;          // (its value)
			mode how;
			uintptr_t slot;
			const char *name, *effect;
			bool warned;
			std::string warning;
		};
		struct getter_patch
		{
			uintptr_t at;
			uint8_t backup[7], ours[7];
		};
		struct pulse_patch
		{
			uintptr_t at = 0;
			uint8_t backup[4] = {};
			uintptr_t target = 0;
		};

		struct state
		{
			// Resolved once: FFXiMain's image, the two getters (fractional first), the tables' offsets in this client
			bool resolved = false;
			uintptr_t base = 0;
			size_t size = 0;
			uintptr_t getters[2] = {};
			sites::found found;
			uint32_t moved = 0;
			uintptr_t div_global = 0; // (the game object's global: its +0x30 the divisor)

			// The options
			bool want = true;
			double limit = 240.0;
			bool tried = false, retry = false;
			std::string why;                 // (why it isn't on)
			std::vector<std::string> notes;  // (what enable() found, said on or not)

			// While on
			uintptr_t mem = 0;
			std::vector<site_patch> sites;
			std::vector<smooth_patch> smooth;
			std::vector<op_patch> ops;
			std::vector<getter_patch> getter_patches;
			bool pulse_on = false;
			pulse_patch pulse;
			uint32_t ticks = 0;
			uintptr_t div_ptr = 0;
			uint32_t div_orig = 0;
			bool div_set = false;
			uintptr_t g_frac = 0, g_int = 0, stub = 0;
			uint32_t missing = 0;
			double accum = 0.0, step = 1.0;
			double last = 0.0;
			bool last_known = false;

			// Every present: the frame rate, and the last present's time (where the step counts from once on)
			double last_present = 0.0;
			bool have_present = false;
			double fps_timer = 0.0, fps = 0.0;
			uint32_t frames = 0;
			bool fps_started = false;

			// The limiter
			HANDLE timer = nullptr;
			bool timer_tried = false, high_res = false;
			double freq = 0.0;
			double waited = 0.0, spun = 0.0;
		};
		state g;

		void le32(uint8_t out[4], uintptr_t v)
		{
			game::put32(out, uint32_t(v));
		}

		void put_float(uintptr_t at, float v)
		{
			std::memcpy(reinterpret_cast<void *>(at), &v, 4);
		}

		void put_u32(uintptr_t at, uint32_t v)
		{
			std::memcpy(reinterpret_cast<void *>(at), &v, 4);
		}

		// Our bytes into the game's code where we made it writable (the smoothing immediates, every frame), without the
		// two protection calls a write_code costs; through write_code if something has made it read-only since
		bool poke_raw(uintptr_t at, const void *bytes, size_t size)
		{
			__try
			{
				std::memcpy(reinterpret_cast<void *>(at), bytes, size);
				return true;
			}
			__except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
			{
				return false;
			}
		}

		void poke(uintptr_t at, const void *bytes, size_t size)
		{
			if (!poke_raw(at, bytes, size))
				game::write_code(at, bytes, size);
		}

		// A word of the game's data (the divisor, on its heap), written without crashing if it's gone
		bool write_word(uintptr_t at, uint32_t v)
		{
			SIZE_T done = 0;
			return at >= 0x10000 && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(at), &v, 4, &done) != 0 && done == 4;
		}

		// A float the game keeps (its bits decoded, never read as a float); false when unreadable, a NaN or infinite
		bool float_at(uintptr_t at, double &out)
		{
			uint32_t bits = 0;
			float f = 0.0f;
			if (!game::read(at, bits) || !game::float_of(bits, f))
				return false;
			out = f;
			return true;
		}

		// True if the bytes at `at` match a hex pattern with ?? wildcards (false when they can't be read)
		bool bytes_match(uintptr_t at, const char *pattern)
		{
			const size_t n = std::strlen(pattern) / 2;
			uint8_t buf[64] = {};
			if (n > sizeof(buf) || !game::read(at, buf, n))
				return false;
			for (size_t i = 0; i < n; ++i)
			{
				if (pattern[2 * i] == '?')
					continue;
				const char two[3] = { pattern[2 * i], pattern[2 * i + 1], 0 };
				if (buf[i] != uint8_t(std::strtoul(two, nullptr, 16)))
					return false;
			}
			return true;
		}

		std::string hex_bytes(uintptr_t at, size_t n)
		{
			std::string s;
			char two[4];
			for (size_t i = 0; i < n; ++i)
			{
				uint8_t b = 0;
				game::read(at + i, b);
				std::snprintf(two, sizeof(two), i == 0 ? "%02X" : " %02X", unsigned(b));
				s += two;
			}
			return s;
		}

		// True if an operand with this value behaves the same at any step, so it needs no rescaling.
		bool step_invariant(mode how, double v)
		{
			if (how == mode::ease)
				return v == 0.0 || std::fabs(v) == 1.0;
			if (how == mode::pow)
				return v == 0.0 || v == 1.0;
			return v == 0.0;
		}

		std::string loop_warning(const char *name, const char *effect, uintptr_t at, const std::string &detail)
		{
			char line[512];
			if (detail == "not found in this client")
				std::snprintf(line, sizeof(line), "The %s code (%08X in the analysed client) wasn't found in this client, so it can't be adjusted. %s", name,
					unsigned(at), effect);
			else
				std::snprintf(line, sizeof(line), "The %s code at %08X was changed by another addon (%s) in a way that can't be adjusted for. %s", name,
					unsigned(at), detail.c_str(), effect);
			return line;
		}

		bool point_site(uintptr_t at, uintptr_t &current, uintptr_t target)
		{
			if (current == target)
				return true;
			uint8_t rel[4];
			le32(rel, target - (at + 5));
			if (!game::write_code(at + 1, rel, 4))
				return false;
			current = target;
			return true;
		}

		double clock_now()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return double(t.QuadPart) / g.freq;
		}

		// The name pulse's call pointed at our tick count (mem + kPulseOff), which starts from the game's frame count
		// (its getter is 'mov eax, [obj]; mov eax, [eax+0x34]; ret'). Not found, or its call not the game's own: the pulse
		// stays the game's.
		void patch_pulse()
		{
			const uintptr_t p = game::find(kPulse);
			if (p == 0)
			{
				g.notes.push_back("The target's name pulse wasn't found in this client: it pulses faster above 60 fps.");
				return;
			}
			const uintptr_t site = p + 5;
			const uintptr_t tgt = game::rel_target(site);
			if (!bytes_match(tgt, kFrameCount))
			{
				char line[256];
				std::snprintf(line, sizeof(line), "The target's name pulse's call at %08X goes somewhere else (another addon?): left as it is.", unsigned(site));
				g.notes.push_back(line);
				return;
			}
			uint32_t start = 0;
			const uint32_t obj = game::word(game::word(tgt + 1));
			if (obj != 0)
				start = game::word(obj + 0x34);
			const uintptr_t cnt = g.mem + kPulseOff;
			g.ticks = start;
			put_u32(cnt, start);
			uint8_t getter[6] = { 0xA1, 0, 0, 0, 0, 0xC3 };
			le32(getter + 1, cnt);
			std::memcpy(reinterpret_cast<void *>(cnt + 4), getter, sizeof(getter));
			pulse_patch s;
			s.at = site;
			game::read(site + 1, s.backup, 4);
			s.target = tgt;
			if (point_site(s.at, s.target, cnt + 4))
			{
				g.pulse = s;
				g.pulse_on = true;
			}
		}

		void disable()
		{
			// The smoothing loops' immediates, unless something else has written them since; then their protection, the
			// last taken first (two share a page: the second one taken found it already writable)
			for (const smooth_patch &l : g.smooth)
				if (game::code_is(l.at, l.ours, 4))
					game::write_code(l.at, l.backup, 4);
			for (auto l = g.smooth.rbegin(); l != g.smooth.rend(); ++l)
			{
				DWORD old = 0;
				VirtualProtect(reinterpret_cast<void *>(l->at), 4, l->prot, &old);
			}
			g.smooth.clear();
			// Operands: restore only the ones that still point at our slot. If another addon holds one, it may write our
			// slot address back into the code when it unloads, so keep our memory alive with the stock values in the
			// slots.
			bool keep_mem = false;
			for (const op_patch &o : g.ops)
			{
				const uint32_t d = game::word(o.at);
				if (d == uint32_t(o.slot))
				{
					uint8_t c[4];
					le32(c, o.constant);
					game::write_code(o.at, c, 4);
				}
				else if (d != o.constant)
					keep_mem = true;
			}
			// The name pulse's call, the call sites and the getters: put back where they're still ours. One that
			// something else has changed since is left as it is, and our memory kept (it may hold our address to go
			// back to).
			if (g.pulse_on)
			{
				uint8_t ours[5] = { 0xE8 };
				le32(ours + 1, g.mem + kPulseOff + 4 - (g.pulse.at + 5));
				if (game::code_is(g.pulse.at, ours, 5))
					game::write_code(g.pulse.at + 1, g.pulse.backup, 4);
				else
					keep_mem = true;
				g.pulse_on = false;
				g.pulse = pulse_patch();
			}
			for (const site_patch &s : g.sites)
			{
				if (!s.patched)
					continue;
				uint8_t ours[5] = { 0xE8 };
				le32(ours + 1, s.target - (s.at + 5));
				if (game::code_is(s.at, ours, 5))
					game::write_code(s.at + 1, s.backup, 4);
				else
					keep_mem = true;
			}
			g.sites.clear();
			for (const getter_patch &p : g.getter_patches)
			{
				if (game::code_is(p.at, p.ours, 7))
					game::write_code(p.at, p.backup, 7);
				else
					keep_mem = true;
			}
			g.getter_patches.clear();
			if (g.div_set)
			{
				uint32_t now = 1;
				if (game::read(g.div_ptr, now) && now == 0)
					write_word(g.div_ptr, g.div_orig);
				g.div_set = false;
			}
			if (g.mem != 0)
			{
				if (keep_mem)
				{
					// (anything still reading it gets the stock step and the stock values in the slots)
					put_float(g.mem, 1.0f);
					put_float(g.mem + 4, 1.0f);
					for (const op_patch &o : g.ops)
						put_float(o.slot, float(o.v));
				}
				else
					game::free_code(g.mem);
				g.mem = 0;
			}
			g.ops.clear();
			g.g_frac = g.g_int = g.stub = 0;
			g.div_ptr = 0;
			g.notes.clear();
		}

		// Points every site at its normal target.
		bool apply_targets()
		{
			for (site_patch &s : g.sites)
			{
				const uintptr_t t = s.kind == 'f' ? g.g_frac : s.kind == 's' ? g.stub : g.g_int;
				if (!point_site(s.at, s.target, t))
					return false;
				s.patched = true;
			}
			return true;
		}

		// The getters and the tables' offsets in this client, once. False and why when they can't be (retry: worth
		// trying again later)
		bool resolve(std::string &why)
		{
			if (g.resolved)
				return true;
			const uintptr_t first = game::find(kGetter);
			uintptr_t base = 0;
			size_t size = 0;
			std::vector<uintptr_t> addrs;
			if (first != 0 && sites::image_of(first, base, size))
				addrs = sites::find_all(base, size, kGetter, 4);
			if (addrs.size() != 2)
			{
				char line[128];
				std::snprintf(line, sizeof(line), "expected 2 frame step getters, found %u; nothing patched", unsigned(addrs.size()));
				why = line;
				return false;
			}
			std::sort(addrs.begin(), addrs.end());
			g.base = base;
			g.size = size;
			g.getters[0] = addrs[0];
			g.getters[1] = addrs[1];
			// Find the tables' offsets in this client (they move when the client is updated).
			g.found = sites::resolve(reinterpret_cast<const uint8_t *>(base), size, uint32_t(addrs[0] - base), uint32_t(addrs[1] - base));
			g.moved = 0;
			for (const auto &m : g.found.map)
				if (m.first != m.second)
					g.moved++;
			g.resolved = true;
			return true;
		}

		bool enable(std::string &why, bool &retry)
		{
			retry = false;
			g.notes.clear();
			if (!game::image_known())
			{
				why = "FFXiMain.dll isn't loaded yet";
				retry = true;
				return false;
			}
			if (g.freq == 0.0)
			{
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				g.freq = double(f.QuadPart);
			}
			if (!resolve(why))
				return false;
			const uintptr_t base = g.base;
			const uintptr_t g1 = g.getters[0], g2 = g.getters[1];

			// (the global found once: while the game object isn't there yet, this is tried every update)
			if (g.div_global == 0)
			{
				const uintptr_t dp = game::find(kDivisor);
				if (dp == 0)
				{
					why = "the fps divisor wasn't found; nothing patched";
					return false;
				}
				g.div_global = game::word(dp + 0x0C);
			}
			const uint32_t obj = game::word(g.div_global);
			if (!game::pointer(obj))
			{
				why = "the game's fps divisor isn't there yet";
				retry = true;
				return false;
			}
			const uintptr_t div_ptr = obj + 0x30;

			// Check every call site before touching anything.
			const auto loc = [&](uint32_t rva) -> uintptr_t
			{
				const auto m = g.found.map.find(rva);
				return m != g.found.map.end() ? base + m->second : 0;
			};
			std::vector<std::string> notes;
			std::vector<site_patch> list;
			// A site that calls neither getter is either left over from an earlier hifps that never put it back (it
			// calls a 'fld [x]; ret' stub, or memory that is gone): it is taken back, and stop points it at the getter
			// (both getters are the same code). Or something else hooks it: that site is left alone. Many of those
			// means a different client, and nothing is patched.
			uint32_t reclaimed = 0, missing = 0;
			std::vector<uintptr_t> foreign;
			const auto add_at = [&](uintptr_t a, char kind)
			{
				uint8_t opcode = 0;
				game::read(a, opcode);
				const bool call = opcode == 0xE8;
				const uintptr_t t = call ? game::rel_target(a) : 0;
				site_patch s = { a, kind, {}, t, false };
				game::read(a + 1, s.backup, 4);
				if (!call || (t != g1 && t != g2))
				{
					uint8_t probe[7];
					if (call && (!game::read(t, probe, 7) || bytes_match(t, kOldStub)))
					{
						le32(s.backup, g1 - (a + 5));
						reclaimed++;
					}
					else
					{
						foreign.push_back(a);
						return;
					}
				}
				list.push_back(s);
			};
			// A site not found in this client isn't patched. If its call is still there, it is one of found.extra and
			// gets whole ticks below.
			const auto add = [&](const uint32_t *rvas, size_t n, char kind)
			{
				for (size_t i = 0; i < n; ++i)
				{
					const uintptr_t a = loc(rvas[i]);
					if (a == 0)
						missing++;
					else
						add_at(a, kind);
				}
			};
			add(kFracSites, std::size(kFracSites), 'f');
			add(kIntSites, std::size(kIntSites), 'i');
			add(kStockSites, std::size(kStockSites), 's');
			std::vector<smooth_patch> smooth;
			std::vector<op_patch> ops;
			for (const loop_info &l : kLoops)
			{
				// Check the loop's constants first. If something else (e.g. another addon) already changed one, leave
				// this loop on whole ticks instead of refusing to load.
				bool bad = false;
				uintptr_t bad_at = 0;
				std::string detail;
				std::vector<smooth_patch> l_smooth;
				std::vector<op_patch> l_ops;
				// Not found in this client: the loop runs as if another addon had changed it.
				const auto lost = [&](uint32_t rva)
				{
					bad = true;
					bad_at = base + rva;
					detail = "not found in this client";
				};
				if (l.imm != 0 && loc(l.imm) == 0)
					lost(l.imm);
				for (const op_info &o : l.ops)
					if (!bad && loc(o.at) == 0)
						lost(o.at);
				if (l.imm != 0 && !bad)
				{
					const uintptr_t a = loc(l.imm);
					const float kf = float(l.k);
					bool ok = game::code_is(a, &kf, 4);
					for (size_t i = 0; i < l.pre.size(); ++i)
					{
						uint8_t b = 0;
						game::read(a - l.pre.size() + i, b);
						if (b != l.pre[i])
							ok = false;
					}
					if (!ok)
					{
						bad = true;
						bad_at = a - l.pre.size();
					}
					else
					{
						smooth_patch s = { a, l.k, {}, {}, 0, l.name, l.effect, {} };
						game::read(a, s.backup, 4);
						std::memcpy(s.ours, s.backup, 4);
						l_smooth.push_back(s);
					}
				}
				if (bad && detail.empty())
					detail = "found " + hex_bytes(bad_at, l.pre.size() + 4);
				for (const op_info &o : l.ops)
				{
					if (bad)
						break;
					const uintptr_t a = loc(o.at);
					const uint32_t d = game::word(a);
					// The constant: where the instruction points, if that's inside the client (another addon's redirect
					// points at its own memory); else where it was in the analysed client.
					uint32_t c = uint32_t(base + o.constant);
					if (d >= base && d < base + g.size)
						c = d;
					else if (a != base + o.at)
						lost(o.at);
					uint8_t prefix = 0;
					game::read(a - 2, prefix);
					double dv = 0.0, cv = 0.0;
					if (bad)
					{
						// Moved, and redirected by another addon: its own constant is unknown.
					}
					else if (prefix != 0xD8)
					{
						bad = true;
						bad_at = a - 2;
						detail = "found " + hex_bytes(a - 2, 6);
					}
					else if (d != c && (!float_at(d, dv) || !step_invariant(o.how, dv)))
					{
						// Redirected by another addon to a value that depends on the frame rate.
						char text[64];
						if (float_at(d, dv))
							std::snprintf(text, sizeof(text), "value %g", dv);
						else
							std::snprintf(text, sizeof(text), "its value unreadable");
						bad = true;
						bad_at = a - 2;
						detail = text;
					}
					else if (!float_at(c, cv))
					{
						bad = true;
						bad_at = a - 2;
						detail = "its constant not a number";
					}
					else
						l_ops.push_back({ a, c, cv, o.how, 0, l.name, l.effect, false, {} });
				}
				if (bad)
				{
					notes.push_back(loop_warning(l.name, l.effect, bad_at, detail));
					add(l.sites.data(), l.sites.size(), l.fallback);
				}
				else
				{
					add(l.sites.data(), l.sites.size(), 's');
					smooth.insert(smooth.end(), l_smooth.begin(), l_smooth.end());
					ops.insert(ops.end(), l_ops.begin(), l_ops.end());
				}
			}
			if (ops.size() > kMaxOps)
			{
				why = "too many operand patches; nothing patched";
				return false;
			}
			// Calls to the getter no table names (new or changed code in an updated client): whole ticks, which keep
			// game time right at any frame rate.
			for (const uint32_t o : g.found.extra)
				add_at(base + o, 'i');
			char line[512];
			if (g.moved > 0 || missing > 0 || !g.found.extra.empty())
			{
				std::snprintf(line, sizeof(line), "Updated client: %u of its offsets moved and were found again, %u not found, %u new call sites on whole ticks.",
					g.moved, missing, unsigned(g.found.extra.size()));
				notes.insert(notes.begin(), line);
			}
			g.notes = notes; // (said whether it goes on or not, as the addon said them)
			if (foreign.size() > kForeignMax)
			{
				std::snprintf(line, sizeof(line), "%u call sites don't call the step getter: the client differs from the analysed one; nothing patched",
					unsigned(foreign.size()));
				why = line;
				return false;
			}
			for (const uintptr_t a : foreign)
			{
				std::snprintf(line, sizeof(line), "Call site %08X is hooked by something else: left at stock speed.", unsigned(a));
				notes.push_back(line);
			}
			if (reclaimed > 0)
			{
				std::snprintf(line, sizeof(line), "Took back %u call site(s) an earlier hifps left patched.", reclaimed);
				notes.push_back(line);
			}

			g.mem = game::alloc_code(kMemSize);
			if (g.mem == 0)
			{
				why = "couldn't allocate memory; nothing patched";
				return false;
			}
			g.notes = notes;
			g.missing = missing;
			put_float(g.mem, 1.0f);
			put_float(g.mem + 4, 1.0f);
			put_float(g.mem + 24, 1.0f);
			uint8_t stub[7] = { 0xD9, 0x05, 0, 0, 0, 0, 0xC3 };
			le32(stub + 2, g.mem + 24);
			std::memcpy(reinterpret_cast<void *>(g.mem + 16), stub, sizeof(stub));
			g.stub = g.mem + 16;
			g.accum = 0.0;
			g.step = 1.0;

			// New getter bodies: fld dword ptr [mem + 0 / 4]; ret
			for (int i = 0; i < 2; ++i)
			{
				getter_patch p = { g.getters[i], {}, { 0xD9, 0x05, 0, 0, 0, 0, 0xC3 } };
				le32(p.ours + 2, g.mem + uint32_t(i) * 4);
				game::read(p.at, p.backup, 7);
				if (!game::write_code(p.at, p.ours, 7))
				{
					disable();
					why = "couldn't write the game's code; put back";
					return false;
				}
				g.getter_patches.push_back(p);
			}
			g.g_frac = g.getters[0];
			g.g_int = g.getters[1];

			g.sites = list;
			for (smooth_patch &l : smooth)
			{
				if (!VirtualProtect(reinterpret_cast<void *>(l.at), 4, PAGE_EXECUTE_READWRITE, &l.prot))
				{
					disable();
					why = "couldn't unprotect a smoothing constant; put back";
					return false;
				}
				g.smooth.push_back(l);
			}
			for (size_t i = 0; i < ops.size(); ++i)
			{
				op_patch o = ops[i];
				o.slot = g.mem + 32 + uint32_t(i) * 4;
				put_float(o.slot, float(o.v));
				g.ops.push_back(o);
				// Take over only operands that still point at the game's constant; leave other addons' alone.
				if (game::word(o.at) == o.constant)
				{
					uint8_t s[4];
					le32(s, o.slot);
					if (!game::write_code(o.at, s, 4))
					{
						disable();
						why = "couldn't redirect an operand; put back";
						return false;
					}
				}
			}
			if (!apply_targets())
			{
				disable();
				why = "couldn't redirect a call; put back";
				return false;
			}
			patch_pulse();

			g.div_ptr = div_ptr;
			g.div_orig = game::word(div_ptr);
			g.div_set = write_word(div_ptr, 0);
			g.last = g.last_present;
			g.last_known = g.have_present;
			return true;
		}

		// The limiter's wait to `target` (on the caller's clock, `now` at the call): the time it ended
		double wait_until(double now, double target)
		{
			g.waited = g.spun = 0.0;
			const double remain = target - now;
			if (remain <= 0.0)
				return now;
			if (g.freq == 0.0)
			{
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				g.freq = double(f.QuadPart);
			}
			if (!g.timer_tried)
			{
				g.timer_tried = true;
				g.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
				g.high_res = g.timer != nullptr;
			}
			const double start = clock_now();
			if (g.high_res)
			{
				if (remain > kSpin)
				{
					LARGE_INTEGER due;
					due.QuadPart = -static_cast<LONGLONG>((remain - kSpin) * 1e7);
					if (SetWaitableTimer(g.timer, &due, 0, nullptr, nullptr, FALSE))
						WaitForSingleObject(g.timer, DWORD(remain * 1000.0) + 20);
				}
			}
			else if (remain > 0.002)
				Sleep(DWORD(std::floor((remain - 0.0015) * 1000.0)));
			const double slept = clock_now();
			double t = now + (slept - start);
			while (t < target)
			{
				YieldProcessor();
				t = now + (clock_now() - start);
			}
			g.waited = slept - start;
			g.spun = t - now - g.waited;
			return t;
		}
	}

	void update()
	{
		g.want = game::option_on("hifps", "enabled", true);
		const double limit = std::floor(double(game::option_number("hifps", "limit", 240.0f)));
		g.limit = std::max(0.0, std::min(1000.0, limit));
		if (!g.want)
		{
			if (g.mem != 0)
				disable();
			g.tried = g.retry = false;
			g.why.clear();
			g.notes.clear();
			return;
		}
		if (g.mem != 0 || (g.tried && !g.retry))
			return;
		g.tried = true;
		std::string why;
		bool retry = false;
		if (enable(why, retry))
			g.why.clear();
		else
			g.why = why;
		g.retry = retry;
	}

	void present(double now)
	{
		double t = now;
		// Frame limiter: most of the time left on the timer, then a short spin.
		if (g.mem != 0 && g.limit > 0.0 && g.last_known)
			t = wait_until(now, g.last + 1.0 / g.limit);

		if (!g.fps_started)
		{
			g.fps_started = true;
			g.fps_timer = t;
			g.frames = 0;
		}
		else
		{
			g.frames++;
			if (t - g.fps_timer >= 1.0)
			{
				g.fps = g.frames / (t - g.fps_timer);
				g.frames = 0;
				g.fps_timer = t;
			}
		}
		g.last_present = t;
		g.have_present = true;
		if (g.mem == 0)
			return;

		double step = 1.0;
		if (g.last_known)
			step = std::max(kMinStep, std::min(kMaxStep, (t - g.last) * 60.0));
		g.last = t;
		g.last_known = true;
		g.step = step;
		put_float(g.mem, float(step));

		// Whole ticks for the callers that truncate; the remainder carries to the next frame.
		g.accum += step;
		const double whole = std::floor(g.accum);
		g.accum -= whole;
		put_float(g.mem + 4, float(whole));
		// The name pulse counts the same whole ticks: 60 a second at any frame rate.
		if (g.pulse_on)
		{
			g.ticks = uint32_t(g.ticks + uint32_t(whole));
			put_u32(g.mem + kPulseOff, g.ticks);
		}

		// Smoothing loops run once per frame; scale their factor to this frame's step.
		// (Only over what we wrote last: one something else has written since is left to it.)
		for (smooth_patch &l : g.smooth)
		{
			if (std::memcmp(reinterpret_cast<const void *>(l.at), l.ours, 4) != 0)
			{
				if (l.warning.empty())
					l.warning = loop_warning(l.name, l.effect, l.at, "its factor written over: found " + hex_bytes(l.at, 4));
				continue;
			}
			const float v = float(1.0 - std::pow(1.0 - l.k, step));
			std::memcpy(l.ours, &v, 4);
			poke(l.at, &v, 4);
		}
		for (op_patch &o : g.ops)
		{
			double v = o.v * step;
			const double m = std::fabs(o.v);
			if (o.how == mode::ease && m > 0.0 && m < 1.0)
				v = (1.0 - std::pow(1.0 - m, step)) * (o.v < 0.0 ? -1.0 : 1.0);
			else if (o.how == mode::pow)
				v = o.v > 0.0 ? std::pow(o.v, step) : o.v;
			put_float(o.slot, float(v));
			// Another addon may have redirected this operand since, or handed it back.
			const uint32_t d = game::word(o.at);
			if (d == o.constant)
			{
				uint8_t s[4];
				le32(s, o.slot);
				game::write_code(o.at, s, 4);
				o.warned = false;
				o.warning.clear();
			}
			else if (d != uint32_t(o.slot) && !o.warned)
			{
				double dv = 0.0;
				const bool readable = float_at(d, dv);
				if (!readable || !step_invariant(o.how, dv))
				{
					char text[64];
					if (readable)
						std::snprintf(text, sizeof(text), "value %g", dv);
					else
						std::snprintf(text, sizeof(text), "its value unreadable");
					o.warning = loop_warning(o.name, o.effect, o.at - 2, text);
					o.warned = true;
				}
			}
		}
	}

	void stop()
	{
		disable();
		if (g.timer != nullptr)
			CloseHandle(g.timer);
		g = state();
	}

	std::vector<std::string> status()
	{
		std::vector<std::string> out;
		char line[512];
		char cap[32];
		if (g.limit > 0.0)
			std::snprintf(cap, sizeof(cap), "%.0f", g.limit);
		else
			std::snprintf(cap, sizeof(cap), "none");
		if (g.mem != 0)
		{
			std::snprintf(line, sizeof(line), "On: real frame time into the game's step at %u call sites (%u smoothing immediates, %u operands rescaled%s); %.0f fps, step %.3f ticks, cap %s.",
				unsigned(g.sites.size()), unsigned(g.smooth.size()), unsigned(g.ops.size()), g.pulse_on ? ", the name pulse on whole ticks" : "",
				g.fps, g.step, cap);
			out.push_back(line);
			out.insert(out.end(), g.notes.begin(), g.notes.end());
			for (const smooth_patch &l : g.smooth)
				if (!l.warning.empty())
					out.push_back(l.warning);
			for (const op_patch &o : g.ops)
				if (!o.warning.empty())
					out.push_back(o.warning);
		}
		else if (!g.want)
			out.push_back("Off: the game's own step (60 fps at most).");
		else if (!g.why.empty())
		{
			out.push_back("Not on: " + g.why + ".");
			out.insert(out.end(), g.notes.begin(), g.notes.end());
		}
		else
			out.push_back("Not on yet.");
		return out;
	}

	double fps()
	{
		return g.fps;
	}

	bool on()
	{
		return g.mem != 0;
	}

	patched where()
	{
		patched p;
		p.moved = g.moved;
		p.missing = g.missing;
		p.timer = g.waited;
		p.spin = g.spun;
		p.high_resolution = g.high_res;
		p.step = g.step;
		if (g.mem == 0)
			return p;
		p.memory = g.mem;
		p.getters[0] = g.g_frac;
		p.getters[1] = g.g_int;
		p.divisor = g.div_ptr;
		p.divisor_was = g.div_orig;
		p.pulse = g.pulse_on ? g.pulse.at : 0;
		for (const site_patch &s : g.sites)
			p.sites.push_back({ s.at, s.kind, s.target });
		for (const smooth_patch &l : g.smooth)
			p.immediates.push_back(l.at);
		for (const op_patch &o : g.ops)
			p.operands.push_back({ o.at, o.constant, o.slot, char(o.how) });
		return p;
	}

	const sites::found *resolved()
	{
		return g.resolved ? &g.found : nullptr;
	}
}
