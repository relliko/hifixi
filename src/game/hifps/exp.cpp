/*
 * hifps's experimental frame-time toggles (exp.hpp): hifps.lua's /hifps exp (exp_set, exp_toggle, restore_exp,
 * exp_tick, exp_stat and what they use: ashita_*, occlusion_*, walls_*, sse_*, proxy_*), in C++. Its comments
 * carried over where they still hold.
 *
 * Experimental frame-time toggles. All off by default; each one puts back what it changed when turned off, when
 * hifps's frame step goes off (hifps::on()) and on stop(). None of them changes the game step.
 *   ashita    Ashita's device wrapper hands every draw call to every plugin, and the addons plugin looks the event up
 *             in every loaded addon, about 3,000 times a frame although no addon here handles d3d_dp/dip/dpup/dipup.
 *             Skips that (those four events stop firing). Before turning on it reads the loaded addons' Lua files
 *             for those event names and each plugin's four draw callbacks (any that do more than return false), and
 *             refuses if something uses them (ashita_force overrides). While on, it checks again whenever the loaded
 *             addons or plugins change, and turns itself off for a newcomer.
 *   occlusion Characters test whether they are hidden behind walls with a GPU readback that stalls the frame; the
 *             client tests each one every 4th frame. Above 60fps this tests every 4m-th frame instead, m = fps/60
 *             rounded down to a power of two, so a result is never older than at stock 60fps. Targeting reads the
 *             result, so this keeps it that fresh.
 *   walls     Answers that test from the zone's collision instead, with no readback: the actor draw's call of it goes
 *             to walls.asm's stub (stubs.inc), which runs the game's own segment test (the one the camera keeps out
 *             of walls with) from the camera's eye to up to 9 points of the character's screen box at the client's
 *             test depth; hidden only when every one is blocked. Scenery without collision no longer hides
 *             characters, nor do characters each other; you and your target are always shown. At most kWallsMs of
 *             segments a frame (the rest of the frame's tests answer shown); in cutscenes and with no collision
 *             loaded the client's own test runs. Works with occlusion (which still decides how often each character
 *             is tested).
 *   sse       The client skins characters on the CPU and only uses its SSE code when the CPU says "Intel"; other CPUs
 *             get the x87 code. Turns the SSE code on for them.
 *   batch     The text and UI draw one quad per draw call; consecutive quads with nothing changed in between become
 *             one draw. Same triangles, same order, same state.
 *   dedupe    About 40% of the client's device calls set a value already in effect; drops those.
 *   batch and dedupe replace the client device's vtable with proxy.asm's stub (stubs.inc).
 *
 * The toggles follow HiFi XI's options ([hifps] exp_<name>, ashita_force) instead of hifps's settings file: a toggle
 * whose option goes on is turned on (once: one that can't be says why, and isn't tried again until its option goes
 * off and on, or hifps's frame step does); the saved ones are turned on once the client's device exists. Unlike the
 * addon, our memory is freed on stop() when nothing points into it any more, and nothing is put back over bytes that
 * aren't ours (the addon wrote the occlusion test's and the SSE flag's stock bytes back regardless).
 */

#include "../../../deps/ashita/plugins/sdk/Ashita.h" // (IAshitaCore: the actors, the plugins, Ashita's folder)
#include "exp.hpp"
#include "step.hpp" // (hifps::on(), hifps::fps())
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>

namespace hifi::game::hifps::exp
{
	namespace
	{
#include "stubs.inc"

		namespace px = stubs::proxy;
		namespace wl = stubs::walls;

		constexpr uint32_t kProxyBuf = 0x20000; // bytes of merged vertices: 780 text quads a draw
		constexpr double kWallsMs = 1.0;        // milliseconds of segment tests a frame, then the frame's other tests answer shown

		enum toggle_id
		{
			kAshita,
			kOcclusion,
			kWalls,
			kSse,
			kBatch,
			kDedupe,
			kCount
		};
		constexpr const char *kNames[kCount] = { "ashita", "occlusion", "walls", "sse", "batch", "dedupe" };
		constexpr const char *kOptions[kCount] = { "exp_ashita", "exp_occlusion", "exp_walls", "exp_sse", "exp_batch", "exp_dedupe" };
		constexpr const char *kDesc[kCount] = {
			"skip Ashita's per-draw plugin dispatch (d3d_dp/dip/dpup/dipup stop firing)",
			"test hidden characters less often above 60fps, as fresh as stock at 60fps (within 3%)",
			"tell hidden characters from the zone's walls on the CPU instead of waiting on the GPU",
			"SSE character skinning on non-Intel CPUs",
			"merge consecutive text and UI quads into one draw",
			"drop device calls that set a value already in effect",
		};
		constexpr const char *kSection = "hifps";

		// The client's IDirect3DDevice8 (Ashita's wrapper object): [renderer + 8]. The renderer pointer's address is
		// found once.
		constexpr const char *kDevicePat = "8BC8E8????????85C0A3????????0F84????????8B5C241C";

		// ashita: in each of the wrapper's four draw methods, 'cmp dword [eax+10Ch], 0; je forward' decides whether
		// plugins see the draw. je -> jmp.
		constexpr int kAshitaPat[] = { 0x83, 0xB8, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x74, -1, 0x8B, 0x88, 0x34, 0x01, 0x00, 0x00, 0x85, 0xC9, 0x74 };
		constexpr const char *kDrawEvents[] = { "d3d_dp", "d3d_dip", "d3d_dpup", "d3d_dipup" };

		// occlusion: at the rotation test 'and eax, 80000003h; jns; dec; or eax, -4; inc' (frame mod 4) the actor's
		// phase (0-3, edi) is compared with the frame; becomes 'and eax, 4m-1; shl edi, log2 m'.
		constexpr const char *kOcclPat = "8BBEF0090000E8????????250300008079054883C8FC403BC7";
		constexpr uint8_t kOcclStock[12] = { 0x25, 0x03, 0x00, 0x00, 0x80, 0x79, 0x05, 0x48, 0x83, 0xC8, 0xFC, 0x40 };

		// The occlusion test's start: mov eax, [renderer]; push ebx; push esi; mov esi, ecx; mov bl, [eax+828h] (walls
		// checks its call still goes there).
		constexpr const char *kVisTest = "A1????????53568BF18A9828080000";
		// The actor update's call to the frame count getter, whose code is kVisGetter (the 'occlusion' toggle changes
		// the bytes after this call, not these).
		constexpr const char *kVisFrame = "8BBEF0090000E8";
		constexpr const char *kVisGetter = "A1????????8B4034C3";

		// walls: the actor draw's call of the occlusion test, pointed at walls.asm's entry. kWallsAt is the call's
		// offset in the pattern.
		// lea ecx, [esi+8F0h]; call test; cmp eax, ebp; mov [esi+9F4h], eax (the character's test object, its result)
		constexpr const char *kWallsCall = "8D8EF0080000E8????????3BC58986F4";
		constexpr intptr_t kWallsAt = 6;
		// add ecx, 178h; jmp ...: the segment test, thiscall on the world renderer (from, to, hit), the collision map
		// at +178h
		constexpr const char *kWallsSeg = "81C178010000E9????????90909090908A442404A2????????C3";
		// mov ecx, [renderer]; ...; call segment test; test al, al; jne (the camera's own test: the renderer global at
		// +2, the call at +17h)
		constexpr const char *kWallsRenderer = "8B0D????????8D94248C0000008D6F44528D4424605550E8????????84C07517";
		// sub esp, 0D0h; ...; mov eax, [edi+60h]; ...; test eax, eax; jz (the collision core: the map's grid field, 0
		// while no collision is loaded, is the byte at +14)
		constexpr const char *kWallsGrid = "81ECD0000000535556578BF98B4760C7877C8001000000000085C00F84";
		// mov eax, [gGame]; add eax, 94h / 154h; ret (the camera's view and projection; walls.asm reads them there,
		// and the frame count at +34h)
		constexpr const char *kWallsView = "A1????????0594000000C3";
		constexpr const char *kWallsProj = "A1????????0554010000C3";
		constexpr uint32_t kWallsCounters[7] = { wl::S_TESTS, wl::S_SEGS, wl::S_HIDDEN, wl::S_SHOWN, wl::S_OVER, wl::S_FALLBACK, wl::S_TICKS };

		// sse: the CPU feature object (getter 'mov eax, [obj]; mov al, [eax+9]; ret'); +9 = SSE skinning.
		constexpr const char *kSsePat = "A1????????8A4009C3";

		struct walls_state
		{
			uintptr_t base = 0; // walls.asm's copy, kept once made (freed by stop() when nothing calls into it)
			uintptr_t site = 0; // while on: the call
			uint8_t backup[4] = {};
			bool held = false;  // something else repointed the call since: the copy stays
			bool have_prev = false, have_rate = false;
			uint32_t prev[7] = {}, rate[7] = {}; // tests, segments, hidden, shown, over, fallback, ticks (a second)
		};

		struct proxy_state
		{
			uintptr_t base = 0;         // proxy.asm's copy, kept once made
			uintptr_t dev = 0, vt = 0;  // the client device while the proxy is on, its own vtable
			bool held = false;          // something else replaced our vtable since: the copy stays
			bool have_prev = false, have_rate = false;
			uint32_t prev[3] = {}, rate[3] = {}; // skipped, merged, batches (a second)
		};

		// The draw tap (draw_tap): the client device's four draw slots pointed at the stream log's for a few frames
		struct tap_state
		{
			bool on = false;
			bool in_proxy = false;      // in the proxy's own vtable (it's on); else our copy of the device's in front
			uintptr_t dev = 0;          // (our copy: the device whose vtable it is, and the one it had)
			uint32_t vt = 0;
			uint32_t taps[4] = {};      // what the slots hold while on
			uint32_t saved[4] = {};     // (in the proxy: what they held)
			uint32_t *copy = nullptr;   // (our copy: vtable[-1] then 128 slots; never freed while the device may use it)
			bool held = false;          // something else replaced our copy on the device since: it stays
		};

		struct state
		{
			bool on[kCount] = {};
			std::string err[kCount];          // why it could not be turned on
			std::vector<uintptr_t> ashita;    // patched jumps
			bool occl = false;                // the occlusion test's bytes (and the m written, 0: none yet)
			uintptr_t occl_addr = 0;
			uint32_t occl_m = 0;
			walls_state walls;
			bool sse = false;                 // the flag byte we set (0: it was on already)
			uintptr_t sse_addr = 0;
			proxy_state proxy;
			tap_state tap;
			bool force = false;             // 'ashita' turned on although something uses the draw events
			uintptr_t dp = 0;                 // the device wrapper's DrawPrimitive (for the plugin check)
			std::map<std::string, std::vector<std::string>> scan; // addon name -> its uses of the draw events (cache)
			std::vector<std::string> users;   // what would lose the draw events: 'addon x (...)', 'plugin y (...)'
			std::string checked;              // 'n addons, m plugins'
			std::vector<std::string> notes;   // what could not be checked
			std::string sig;                  // loaded addons and plugins at the last check
			uintptr_t renderer_ptr = 0;
			// Once a second (exp_tick), from present()
			bool timer = false;
			double tick_at = 0.0, freq = 0.0;
			// hifps on (its frame step: hifps::on()), the options as last followed
			bool enabled = false, restore_pending = false;
			bool wanted[kCount] = {};
			bool wanted_force = false;
			std::vector<std::string> said;    // what the addon would have printed, the last few
		};
		state g;
		IAshitaCore *g_core = nullptr;
		const host *g_host = nullptr;

		std::string fmt(const char *f, ...)
		{
			char buf[2048];
			va_list a;
			va_start(a, f);
			std::vsnprintf(buf, sizeof(buf), f, a);
			va_end(a);
			return buf;
		}

		std::string join(const std::vector<std::string> &list, const char *sep)
		{
			std::string s;
			for (size_t i = 0; i < list.size(); ++i)
				s += (i > 0 ? sep : "") + list[i];
			return s;
		}

		std::string lower(std::string s)
		{
			for (char &c : s)
				c = char(std::tolower(uint8_t(c)));
			return s;
		}

		void say(const std::string &s)
		{
			g.said.push_back(s);
			if (g.said.size() > 6)
				g.said.erase(g.said.begin());
		}

		uint8_t byte_at(uintptr_t at)
		{
			uint8_t v = 0;
			read(at, v);
			return v;
		}

		bool readable(uintptr_t p, size_t n)
		{
			if (p == 0)
				return false;
			std::vector<uint8_t> b(n);
			return read(p, b.data(), n);
		}

		// Our own memory (the stubs' copies)
		void set32(uintptr_t at, uint32_t v)
		{
			std::memcpy(reinterpret_cast<void *>(at), &v, 4);
		}

		uint32_t get32(uintptr_t at)
		{
			uint32_t v = 0;
			std::memcpy(&v, reinterpret_cast<const void *>(at), 4);
			return v;
		}

		// The game's data (the device's vtable pointer, the CPU flag): written as its code is
		bool write32(uintptr_t at, uint32_t v)
		{
			return write_code(at, &v, 4);
		}

		uint32_t at_u32(const uint8_t *t, size_t i)
		{
			uint32_t v = 0;
			std::memcpy(&v, t + i, 4);
			return v;
		}

		// The first place in t where pat (-1: any byte) is, -1 when nowhere
		int find_bytes(const uint8_t *t, size_t n, const int *pat, size_t m)
		{
			for (size_t i = 0; i + m <= n; ++i)
			{
				size_t k = 0;
				while (k < m && (pat[k] < 0 || t[i + k] == uint8_t(pat[k])))
					++k;
				if (k == m)
					return int(i);
			}
			return -1;
		}

		template <size_t N>
		int find_bytes(const uint8_t *t, size_t n, const int (&pat)[N])
		{
			return find_bytes(t, n, pat, N);
		}

		// True if the bytes at addr match a hex pattern with ?? wildcards.
		bool bytes_match(uintptr_t addr, const char *pat)
		{
			const size_t n = std::strlen(pat) / 2;
			std::vector<uint8_t> t(n);
			if (!read(addr, t.data(), n))
				return false;
			for (size_t i = 0; i < n; ++i)
			{
				if (pat[2 * i] == '?')
					continue;
				const char two[3] = { pat[2 * i], pat[2 * i + 1], 0 };
				if (t[i] != uint8_t(std::strtoul(two, nullptr, 16)))
					return false;
			}
			return true;
		}

		// A stub's copy in memory of our own: its bytes, the copy's base added to every relocated word
		uintptr_t lay_down(const uint8_t *bytes, uint32_t size, const uint32_t *relocs, size_t count, size_t alloc)
		{
			const uintptr_t base = alloc_code(alloc);
			if (base == 0)
				return 0;
			std::memcpy(reinterpret_cast<void *>(base), bytes, size);
			for (size_t i = 0; i < count; ++i)
				set32(base + relocs[i], get32(base + relocs[i]) + uint32_t(base));
			return base;
		}

		double qpc_freq()
		{
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			return double(f.QuadPart);
		}

		// The client's IDirect3DDevice8 (Ashita's wrapper object): [renderer + 8]; 0 while it doesn't exist
		uintptr_t client_device()
		{
			if (g.renderer_ptr == 0)
			{
				const uintptr_t p = find(kDevicePat);
				if (p == 0)
					return 0;
				g.renderer_ptr = word(p + 10);
			}
			const uint32_t r = word(g.renderer_ptr);
			if (r == 0)
				return 0;
			return word(r + 8);
		}

		// ---------------------------------------------------------------- ashita: who would lose the draw events

		std::string module_name(uintptr_t a, bool &known)
		{
			known = false;
			HMODULE h = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(a), &h))
				return std::string();
			char s[260] = "";
			const DWORD n = GetModuleFileNameA(h, s, 260);
			const std::string path(s, n);
			const size_t at = path.find_last_of("\\/");
			const std::string name = at == std::string::npos ? path : path.substr(at + 1);
			known = !name.empty();
			return name;
		}

		std::optional<std::string> cstring(uintptr_t a)
		{
			uint8_t b[48];
			if (a == 0 || !read(a, b, sizeof(b)))
				return std::nullopt;
			std::string s;
			for (const uint8_t c : b)
			{
				if (c == 0)
					break;
				s += char(c);
			}
			if (s.empty())
				return std::nullopt;
			return s;
		}

		// The constant a one-line function returns, read from its code (nothing is run): 'mov eax, imm32; ret',
		// 'push imm8; pop eax; ret' or 'xor eax, eax; ret'.
		std::optional<uint32_t> const_return(uintptr_t fn)
		{
			uint8_t b[8];
			if (fn == 0 || !read(fn, b, sizeof(b)))
				return std::nullopt;
			if (b[0] == 0xB8 && b[5] == 0xC3)
				return at_u32(b, 1);
			if (b[0] == 0x6A && b[2] == 0x58 && b[3] == 0xC3)
				return b[1];
			if (b[0] == 0x33 && b[1] == 0xC0 && b[2] == 0xC3)
				return 0u;
			return std::nullopt;
		}

		// A plugin callback that only returns false ('xor al, al' or 'mov al, 0', then ret).
		bool returns_false(uintptr_t fn)
		{
			uint8_t b[6];
			if (fn == 0 || !read(fn, b, sizeof(b)))
				return false;
			const bool zero = ((b[0] == 0x30 || b[0] == 0x31 || b[0] == 0x32 || b[0] == 0x33) && b[1] == 0xC0) || (b[0] == 0xB0 && b[1] == 0x00);
			return zero && (b[2] == 0xC2 || b[2] == 0xC3);
		}

		// Loaded addon names (other than hifps), false if Ashita doesn't say. (A plugin can't list them: only when
		// whoever set the host can.)
		bool loaded_addons(std::vector<std::string> &list)
		{
			list.clear();
			std::vector<std::string> names;
			if (g_host == nullptr || g_host->addons == nullptr || !g_host->addons(names))
				return false;
			std::vector<std::string> seen;
			for (const std::string &name : names)
			{
				const std::string l = lower(name);
				if (!name.empty() && std::find(seen.begin(), seen.end(), l) == seen.end() && l != "hifps")
				{
					seen.push_back(l);
					list.push_back(name);
				}
			}
			std::sort(list.begin(), list.end());
			return true;
		}

		std::string install_path()
		{
			if (g_host != nullptr)
				return g_host->install;
			const char *p = g_core != nullptr ? g_core->GetInstallPath() : nullptr;
			return p != nullptr ? std::string(p) : std::string();
		}

		// ashita: who would lose the draw events. Addons: the Lua files of the loaded ones that name one.
		std::vector<std::string> file_draw_events(const std::string &path, bool &opened)
		{
			std::vector<std::string> hits;
			std::ifstream f(path);
			opened = f.is_open();
			if (!opened)
				return hits;
			const size_t slash = path.find_last_of("\\/");
			const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
			std::string line;
			int n = 0;
			while (std::getline(f, line))
			{
				n++;
				size_t i = 0;
				while (i < line.size() && std::isspace(uint8_t(line[i])))
					++i;
				if (line.compare(i, 2, "--") == 0)
					continue;
				for (const char *e : kDrawEvents)
				{
					if (line.find(std::string("'") + e + "'") != std::string::npos || line.find(std::string("\"") + e + "\"") != std::string::npos)
						hits.push_back(fmt("%s line %d uses %s", base.c_str(), n, e));
				}
			}
			return hits;
		}

		// The Lua files under a folder (and its folders), relative to it, in order
		void lua_files(const std::string &dir, const std::string &rel, std::vector<std::string> &out)
		{
			WIN32_FIND_DATAA d;
			const HANDLE h = FindFirstFileA((dir + "\\" + (rel.empty() ? std::string() : rel + "\\") + "*").c_str(), &d);
			if (h == INVALID_HANDLE_VALUE)
				return;
			do
			{
				const std::string name = d.cFileName;
				if (name == "." || name == "..")
					continue;
				const std::string path = rel.empty() ? name : rel + "\\" + name;
				if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
					lua_files(dir, path, out);
				else if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".lua") == 0)
					out.push_back(path);
			} while (FindNextFileA(h, &d));
			FindClose(h);
		}

		// An addon's uses of the draw events: 'file line n uses d3d_x', ..., cached per addon.
		const std::vector<std::string> &addon_draw_events(const std::string &name)
		{
			const auto cached = g.scan.find(name);
			if (cached != g.scan.end())
				return cached->second;
			const std::string dir = install_path() + "addons\\" + name;
			std::vector<std::string> rel;
			lua_files(dir, std::string(), rel);
			std::sort(rel.begin(), rel.end());
			std::vector<std::string> hits;
			for (const std::string &r : rel)
			{
				bool opened = false;
				for (const std::string &h : file_draw_events(dir + "\\" + r, opened))
					hits.push_back(h);
			}
			return g.scan[name] = hits;
		}

		// Plugins: Ashita's plugin list, read the way its per-draw loop reads it (dp: the wrapper's DrawPrimitive; the
		// offsets come from the code it calls). The names of plugins whose draw callbacks do more than return false and
		// how many plugins were checked, or false and why. Only reads memory; no plugin code is run.
		bool plugin_draw_users(uintptr_t dp, std::vector<std::string> &users, int &checked, std::string &why)
		{
			users.clear();
			checked = 0;
			why = "unknown Ashita build";
			uint8_t w[0x60];
			if (dp == 0 || !read(dp, w, sizeof(w)))
				return false;
			const int i = find_bytes(w, sizeof(w), { 0xE8, -1, -1, -1, -1, 0x83, 0xB8, -1, -1, -1, -1, 0x00, -1 }); // call core; cmp [eax+..], 0; je (jmp while 'ashita' is on)
			const int j = find_bytes(w, sizeof(w), { 0x8B, 0x88, -1, -1, -1, -1, 0x85, 0xC9, 0x74 });                // mov ecx, [eax+pm]; test; je
			const int k = find_bytes(w, sizeof(w), { 0xE8, -1, -1, -1, -1, 0x84, 0xC0, 0x74 });                      // call the plugins' draw; test al, al
			if (i < 0 || j < 0 || k < 0)
				return false;
			const uintptr_t getter = uint32_t(dp + i + 5 + at_u32(w, size_t(i) + 1));
			const uintptr_t pmfn = uint32_t(dp + k + 5 + at_u32(w, size_t(k) + 1));
			uint8_t gb[0x60], f[0x160];
			if (getter == 0 || pmfn == 0 || !read(getter, gb, sizeof(gb)) || !read(pmfn, f, sizeof(f)))
				return false;
			const int c = find_bytes(gb, sizeof(gb), { 0xB8, -1, -1, -1, -1, 0x8B, 0x4D, 0xF4 });                      // mov eax, offset core
			const int h = find_bytes(f, sizeof(f), { 0x8B, 0x46, -1, 0xA8, 0x01 });                                    // mov eax, [esi+head]; test al, 1
			const int x = find_bytes(f, sizeof(f), { 0x38, 0x48, -1 });                                                // cmp [eax+faulted], cl
			const int p = find_bytes(f, sizeof(f), { 0x8B, 0x48, -1, 0x8B, 0x01, 0x8B, 0x40, -1, 0xFF, 0xD0, 0xA8, 0x08 }); // plugin; GetFlags; test al, 8
			const int d = find_bytes(f, sizeof(f), { 0x8B, 0x48, -1, 0x8B, 0x01, 0x8B, 0x40, -1, 0xFF, 0x75 });         // plugin; its DrawPrimitive
			const int n = find_bytes(f, sizeof(f), { 0x8B, 0x4E, -1, 0x8B, 0x04, 0x08 });                              // next = [entry + [esi+link]]
			if (c < 0 || h < 0 || x < 0 || p < 0 || d < 0 || n < 0)
				return false;
			const uint32_t core = at_u32(gb, size_t(c) + 1), pmoff = at_u32(w, size_t(j) + 2);
			const uint32_t head = f[h + 2], fault = f[x + 2], plug_at = f[p + 2], flags_at = f[p + 7], dpoff = f[d + 7], link_at = f[n + 2];
			if (!readable(uint32_t(core + pmoff), 4))
			{
				why = "no plugin manager";
				return false;
			}
			const uint32_t pm = word(uint32_t(core + pmoff));
			if (!readable(pm, size_t(std::max(head, link_at)) + 4))
			{
				why = "no plugin manager";
				return false;
			}
			const uint32_t link = word(pm + link_at);
			uint32_t e = word(pm + head);
			for (int step = 0; step < 64; ++step)
			{
				if (e == 0 || e % 2 == 1 || link > 0x1000 || !readable(e, size_t(std::max({ fault, plug_at, link })) + 4))
					break;
				if (byte_at(e + fault) == 0)
				{
					const uint32_t plug = word(e + plug_at);
					const uint32_t vt = readable(plug, 4) ? word(plug) : 0;
					if (readable(vt, size_t(dpoff) + 16))
					{
						checked++;
						bool known = false;
						const std::optional<uint32_t> name_at = const_return(word(vt));
						std::optional<std::string> name = cstring(name_at ? *name_at : 0);
						if (!name)
						{
							const std::string m = module_name(vt, known);
							name = known ? m : fmt("plugin at %08X", plug);
						}
						const std::optional<uint32_t> flags = const_return(word(vt + flags_at));
						const std::string mod = lower(module_name(word(vt + dpoff), known));
						if (lower(*name) != "addons" && mod != "addons.dll" && (!flags || (*flags & 8) != 0))
						{
							for (uint32_t q = 0; q < 4; ++q)
							{
								if (!returns_false(word(vt + dpoff + 4 * q)))
								{
									users.push_back(*name);
									break;
								}
							}
						}
					}
				}
				e = word(e + link);
			}
			return true;
		}

		// Everything that would stop working with 'ashita' on (dp: the wrapper's DrawPrimitive): users 'addon x (...)',
		// 'plugin y (...)', a summary of what was checked, and what could not be.
		void draw_event_users(uintptr_t dp, std::vector<std::string> &users, std::string &checked, std::vector<std::string> &notes)
		{
			users.clear();
			notes.clear();
			std::vector<std::string> list;
			const bool listed = loaded_addons(list);
			if (!listed)
				notes.push_back("could not list the loaded addons");
			else
			{
				for (const std::string &name : list)
				{
					const std::vector<std::string> &hits = addon_draw_events(name);
					if (!hits.empty())
						users.push_back("addon " + name + " (" + join(hits, ", ") + ")");
				}
			}
			std::vector<std::string> plugs;
			int n = 0;
			std::string why;
			const bool plugged = plugin_draw_users(dp, plugs, n, why);
			if (!plugged)
				notes.push_back("could not check the plugins: " + why);
			else
			{
				for (const std::string &name : plugs)
					users.push_back("plugin " + name + " (handles draw calls)");
			}
			checked = (listed ? std::to_string(list.size()) : std::string("?")) + " addons, " + (plugged ? std::to_string(n) : std::string("?")) +
				" plugins";
		}

		// The loaded addons and plugins right now, to notice a change cheaply.
		std::string loaded_signature()
		{
			std::vector<std::string> list;
			const bool listed = loaded_addons(list);
			int np = -1;
			if (g_host != nullptr)
				np = g_host->plugins;
			else if (g_core != nullptr && g_core->GetPluginManager() != nullptr)
				np = int(g_core->GetPluginManager()->Count());
			return (listed ? join(list, ",") : std::string("?")) + "|" + (np >= 0 ? std::to_string(np) : std::string("?"));
		}

		bool ashita_module(uintptr_t &base, size_t &size)
		{
			base = 0;
			size = 0;
			if (g_host != nullptr)
			{
				base = g_host->ashita_base;
				size = g_host->ashita_size;
				return base != 0;
			}
			const auto m = reinterpret_cast<const uint8_t *>(GetModuleHandleA("Ashita.dll"));
			if (m == nullptr)
				return false;
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(m + reinterpret_cast<const IMAGE_DOS_HEADER *>(m)->e_lfanew);
			base = reinterpret_cast<uintptr_t>(m);
			size = nt->OptionalHeader.SizeOfImage;
			return true;
		}

		// ashita: each of the four draw methods' je to jmp. Refuses if a loaded addon or plugin uses the draw events,
		// unless forced.
		std::string ashita_on()
		{
			const uintptr_t dev = client_device();
			if (dev == 0)
				return "the client device was not found";
			const uint32_t vt = g.proxy.vt != 0 ? uint32_t(g.proxy.vt) : word(dev);
			uintptr_t base = 0;
			size_t size = 0;
			if (!ashita_module(base, size))
				return "Ashita.dll was not found";
			std::vector<uintptr_t> sites;
			for (uint32_t slot = 70; slot <= 73; ++slot)
			{
				const uint32_t fn = word(vt + 4 * slot);
				if (fn < base || fn >= base + size)
					return fmt("draw method %u is at %08X, outside Ashita.dll (another addon hooks it)", slot, fn);
				uint8_t t[0x60];
				const int at = read(fn, t, sizeof(t)) ? find_bytes(t, sizeof(t), kAshitaPat) : -1;
				if (at < 0)
					return fmt("draw method %u doesn't look like the analysed Ashita (no plugin check found)", slot);
				sites.push_back(fn + uint32_t(at) + 7);
			}
			g.dp = word(vt + 4 * 70);
			g.scan.clear();
			draw_event_users(g.dp, g.users, g.checked, g.notes);
			g.sig = loaded_signature();
			if (!g.users.empty() && !g.force)
				return "it would break " + join(g.users, "; ");
			for (const uintptr_t a : sites)
			{
				const uint8_t jmp = 0xEB;
				if (!write_code(a, &jmp, 1))
					return "could not write Ashita.dll";
				g.ashita.push_back(a);
			}
			return std::string();
		}

		std::string ashita_off()
		{
			for (const uintptr_t a : g.ashita)
			{
				if (byte_at(a) == 0xEB)
				{
					const uint8_t je = 0x74;
					write_code(a, &je, 1);
				}
			}
			g.ashita.clear();
			g.force = false;
			return std::string();
		}

		// While 'ashita' is on: when the loaded addons or plugins change, check them again. A newcomer that uses the
		// draw events turns it off (unless it was forced on).
		void ashita_watch()
		{
			if (!g.on[kAshita] || g.dp == 0)
				return;
			const std::string sig = loaded_signature();
			if (sig == g.sig)
				return;
			g.sig = sig;
			// Forget unloaded addons, so one loaded again is read again; keep the rest (reading is slow).
			std::vector<std::string> now_loaded;
			loaded_addons(now_loaded);
			for (auto it = g.scan.begin(); it != g.scan.end();)
				it = std::find(now_loaded.begin(), now_loaded.end(), it->first) == now_loaded.end() ? g.scan.erase(it) : std::next(it);
			std::vector<std::string> users, notes;
			std::string checked;
			draw_event_users(g.dp, users, checked, notes);
			std::vector<std::string> fresh;
			for (const std::string &u : users)
				if (std::find(g.users.begin(), g.users.end(), u) == g.users.end())
					fresh.push_back(u);
			g.users = users;
			g.checked = checked;
			g.notes = notes;
			if (fresh.empty())
				return;
			if (g.force)
			{
				say("ashita (forced on): " + join(fresh, "; ") + " will not get its draw events.");
				return;
			}
			g.on[kAshita] = false;
			ashita_off();
			g.err[kAshita] = "Turned off: " + join(fresh, "; ") + " needs the draw events.";
			say("ashita turned off: " + join(fresh, "; ") + " needs the draw events. ashita_force keeps it on anyway.");
		}

		// ---------------------------------------------------------------- occlusion

		void occl_bytes(uint32_t m, uint8_t out[12])
		{
			const uint8_t sh = uint8_t(std::floor(std::log(double(m)) / std::log(2.0) + 0.5));
			const uint8_t b[12] = { 0x83, 0xE0, uint8_t(4 * m - 1), 0xC1, 0xE7, sh, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
			std::memcpy(out, b, 12);
		}

		// 3% slack: a frame limit of 120 measures as 119.9fps and should still count as 120.
		uint32_t occl_m()
		{
			uint32_t m = 1;
			while (m < 16 && m * 2 <= hifps::fps() * 1.03 / 60)
				m *= 2;
			return m;
		}

		void occl_set(uint32_t m)
		{
			if (!g.occl || g.occl_m == m)
				return;
			uint8_t b[12];
			occl_bytes(m, b);
			if (write_code(g.occl_addr, b, sizeof(b)))
				g.occl_m = m;
		}

		std::string occlusion_on()
		{
			const uintptr_t p = find(kOcclPat);
			if (p == 0)
				return "the occlusion test was not found (changed client, or another addon patched it)";
			const uintptr_t a = p + 11;
			if (!code_is(a, kOcclStock, sizeof(kOcclStock)))
				return "the occlusion test is not the stock code";
			g.occl = true;
			g.occl_addr = a;
			g.occl_m = 0;
			occl_set(occl_m());
			return std::string();
		}

		std::string occlusion_off()
		{
			if (g.occl && g.occl_m != 0)
			{
				uint8_t ours[12];
				occl_bytes(g.occl_m, ours);
				if (code_is(g.occl_addr, ours, sizeof(ours)))
					write_code(g.occl_addr, kOcclStock, sizeof(kOcclStock));
				else if (!code_is(g.occl_addr, kOcclStock, sizeof(kOcclStock)))
					say("occlusion: something else has changed the occlusion test since: left as it is");
			}
			g.occl = false;
			g.occl_addr = 0;
			g.occl_m = 0;
			return std::string();
		}

		// ---------------------------------------------------------------- walls

		uintptr_t wsym(uint32_t offset)
		{
			return g.walls.base + offset;
		}

		// The player's and the target's actors (walls always shows them), and the player's status.
		bool walls_actors(uint32_t &a, uint32_t &b, uint32_t &status)
		{
			a = b = status = 0;
			if (g_host != nullptr)
				return g_host->actors != nullptr && g_host->actors(a, b, status);
			if (g_core == nullptr)
				return false;
			IMemoryManager *mm = g_core->GetMemoryManager();
			IEntity *ent = mm != nullptr ? mm->GetEntity() : nullptr;
			IParty *party = mm != nullptr ? mm->GetParty() : nullptr;
			ITarget *target = mm != nullptr ? mm->GetTarget() : nullptr;
			if (ent == nullptr || party == nullptr || target == nullptr)
				return false;
			const uint32_t me = party->GetMemberTargetIndex(0);
			const uint32_t t = target->GetTargetIndex(0);
			a = uint32_t(ent->GetActorPointer(me));
			b = t != 0 ? uint32_t(ent->GetActorPointer(t)) : 0;
			status = ent->GetStatus(me);
			return true;
		}

		// Every frame while walls is on. In a cutscene (status 4) the client's own test runs: an event camera can sit
		// inside a wall, where every line from it would be blocked.
		void walls_keep()
		{
			uint32_t a = 0, b = 0, status = 0;
			const bool known = walls_actors(a, b, status);
			if (!known)
				a = b = status = 0;
			set32(wsym(wl::KEEP_A), a);
			set32(wsym(wl::KEEP_B), b);
			set32(wsym(wl::USE_STOCK), known && status == 4 ? 1 : 0);
		}

		std::string walls_on()
		{
			const uintptr_t p = find(kWallsCall);
			if (p == 0)
				return "the characters' test call was not found (changed client, or another addon patched it)";
			const uintptr_t site = p + kWallsAt;
			const uint32_t test = rel_target(site);
			if (!bytes_match(test, kVisTest))
				return "the characters' test call goes somewhere else (another addon hooks it)";
			const uintptr_t seg = find(kWallsSeg), rp = find(kWallsRenderer), core = find(kWallsGrid);
			if (seg == 0 || rp == 0 || core == 0)
				return "the game's wall test was not found";
			if (rel_target(rp + 0x17) != uint32_t(seg) || byte_at(core + 14) != 0x60)
				return "the game's wall test is not as expected";
			const uintptr_t vp = find(kWallsView), pp = find(kWallsProj), fp = find(kVisFrame);
			if (vp == 0 || pp == 0 || fp == 0)
				return "the camera was not found";
			const uint32_t game = word(vp + 1);
			const uint32_t getter = rel_target(fp + 6);
			if (word(pp + 1) != game || !bytes_match(getter, kVisGetter) || word(getter + 1) != game)
				return "the camera is not as expected";
			const HMODULE k32 = GetModuleHandleA("kernel32.dll");
			const FARPROC qpc = k32 != nullptr ? GetProcAddress(k32, "QueryPerformanceCounter") : nullptr;
			if (qpc == nullptr)
				return "QueryPerformanceCounter was not found";
			if (g.walls.base == 0)
			{
				g.walls.base = lay_down(wl::bytes, wl::size, wl::relocs, std::size(wl::relocs), wl::size);
				if (g.walls.base == 0)
					return "could not allocate memory";
			}
			set32(wsym(wl::ORIG_TEST), test);
			set32(wsym(wl::SEG_TEST), uint32_t(seg));
			set32(wsym(wl::RENDERER), word(rp + 2));
			set32(wsym(wl::GRID_OFF), 0x178 + 0x60);
			set32(wsym(wl::GAME), game);
			set32(wsym(wl::QPC), uint32_t(reinterpret_cast<uintptr_t>(qpc)));
			set32(wsym(wl::BUDGET), uint32_t(std::floor(g.freq * kWallsMs / 1000)));
			set32(wsym(wl::KEEP_A), 0);
			set32(wsym(wl::KEEP_B), 0);
			set32(wsym(wl::USE_STOCK), 0);
			for (const uint32_t c : kWallsCounters)
				set32(wsym(c), 0);
			g.walls.have_prev = g.walls.have_rate = false;
			uint8_t backup[4];
			if (!read(site + 1, backup, 4))
				return "could not write the client's code";
			uint8_t rel[4];
			put32(rel, uint32_t(wsym(wl::ENTRY) - (site + 5)));
			if (!write_code(site + 1, rel, 4))
				return "could not write the client's code";
			std::memcpy(g.walls.backup, backup, 4);
			g.walls.site = site;
			walls_keep();
			return std::string();
		}

		std::string walls_off()
		{
			if (g.walls.base == 0 || g.walls.site == 0)
				return std::string();
			// Put the call back unless something else has repointed it since.
			if (rel_target(g.walls.site) == wsym(wl::ENTRY))
				write_code(g.walls.site + 1, g.walls.backup, 4);
			else
			{
				g.walls.held = true;
				say("walls: something else has repointed the characters' test call since: left as it is");
			}
			g.walls.site = 0;
			return std::string();
		}

		// ---------------------------------------------------------------- sse

		std::string sse_on()
		{
			const uintptr_t p = find(kSsePat);
			if (p == 0)
				return "the CPU feature flags were not found";
			const uint32_t obj = word(word(p + 1));
			if (obj == 0)
				return "the CPU feature object does not exist yet";
			if (byte_at(obj + 9) != 0)
			{
				g.sse = true;
				g.sse_addr = 0;
				say("sse: the client already uses its SSE skinning on this CPU (Intel); nothing to change.");
				return std::string();
			}
			const uint8_t one = 1;
			write_code(obj + 9, &one, 1);
			g.sse = true;
			g.sse_addr = obj + 9;
			return std::string();
		}

		std::string sse_off()
		{
			if (g.sse && g.sse_addr != 0)
			{
				const uint8_t zero = 0;
				if (byte_at(g.sse_addr) == 1)
					write_code(g.sse_addr, &zero, 1);
				else
					say("sse: something else has changed the CPU's SSE flag since: left as it is");
			}
			g.sse = false;
			g.sse_addr = 0;
			return std::string();
		}

		// ---------------------------------------------------------------- batch / dedupe: the device proxy

		uintptr_t psym(uint32_t offset)
		{
			return g.proxy.base + offset;
		}

		void proxy_flags()
		{
			*reinterpret_cast<volatile uint8_t *>(psym(px::F_BATCH)) = g.on[kBatch] ? 1 : 0;
			*reinterpret_cast<volatile uint8_t *>(psym(px::F_DEDUPE)) = g.on[kDedupe] ? 1 : 0;
		}

		uintptr_t proxy_make()
		{
			const size_t size = px::size + 16 + kProxyBuf;
			const uintptr_t base = lay_down(px::bytes, px::size, px::relocs, std::size(px::relocs), size);
			if (base == 0)
				return 0;
			const uintptr_t buf = base + px::size + 15 - (base + px::size + 15) % 16;
			set32(base + px::D_BUF, uint32_t(buf));
			set32(base + px::D_BUFSIZE, kProxyBuf);
			return base;
		}

		void tap_off();

		std::string proxy_on()
		{
			if (g.proxy.dev != 0)
			{
				proxy_flags();
				return std::string();
			}
			tap_off(); // (the proxy copies the device's vtable as it is now: not with the tap in it)
			const uintptr_t dev = client_device();
			if (dev == 0)
				return "the client device was not found";
			if (g.proxy.base == 0)
			{
				g.proxy.base = proxy_make();
				if (g.proxy.base == 0)
					return "could not allocate memory";
			}
			const uint32_t vt = word(dev);
			const uintptr_t orig = psym(px::ORIG), vtab = psym(px::VTABLE), thunks = psym(px::THUNK_TABLE);
			// Fresh state: nothing known, nothing pending, counters at 0.
			std::memset(reinterpret_cast<void *>(psym(px::VALID_START)), 0, px::VALID_END - px::VALID_START);
			for (const uint32_t n : { px::D_PENDING, px::S_SKIPPED, px::S_MERGED, px::S_BATCHES })
				set32(psym(n), 0);
			*reinterpret_cast<volatile uint8_t *>(psym(px::F_REC)) = 0;
			for (uint32_t i = 0; i < 128; ++i)
			{
				const uint32_t f = word(vt + 4 * i);
				const uint32_t t = get32(thunks + 4 * i);
				set32(orig + 4 * i, f);
				set32(vtab + 4 * i, t != 0 ? t : f);
			}
			set32(psym(px::VT_RTTI), word(vt - 4));
			proxy_flags();
			g.proxy.dev = dev;
			g.proxy.vt = vt;
			g.proxy.have_prev = g.proxy.have_rate = false;
			if (!write32(dev, uint32_t(vtab)))
			{
				g.proxy.dev = g.proxy.vt = 0;
				return "could not write the client device";
			}
			return std::string();
		}

		// ---------------------------------------------------------------- the draw tap (draw_tap)

		constexpr uint32_t kTapFirst = 70; // DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP, DrawIndexedPrimitiveUP

		void tap_off()
		{
			if (!g.tap.on)
				return;
			if (g.tap.in_proxy)
			{
				// (the proxy's vtable is ours: its slots back, unless something else wrote them since)
				for (uint32_t i = 0; i < 4; ++i)
				{
					const uintptr_t slot = psym(px::VTABLE) + 4 * (kTapFirst + i);
					if (g.proxy.base != 0 && get32(slot) == g.tap.taps[i])
						set32(slot, g.tap.saved[i]);
				}
			}
			else if (g.tap.copy != nullptr)
			{
				// (the device's own vtable back, unless something else has replaced ours since: then our copy stays)
				if (word(g.tap.dev) == uint32_t(reinterpret_cast<uintptr_t>(g.tap.copy + 1)))
					write32(g.tap.dev, g.tap.vt);
				else
				{
					g.tap.held = true;
					say("draw tap: something else has replaced the client device's vtable since: left as it is");
				}
			}
			g.tap.on = false;
			g.tap.dev = 0;
			g.tap.vt = 0;
		}

		bool tap_on(const void *const taps[4], void *originals[4], std::string &why)
		{
			tap_off();
			for (uint32_t i = 0; i < 4; ++i)
				g.tap.taps[i] = uint32_t(reinterpret_cast<uintptr_t>(taps[i]));
			if (g.proxy.dev != 0 && word(g.proxy.dev) == uint32_t(psym(px::VTABLE)))
			{
				// The proxy is the device's vtable: its own slots swapped (our memory, the game's thread: no write to the game)
				for (uint32_t i = 0; i < 4; ++i)
				{
					const uintptr_t slot = psym(px::VTABLE) + 4 * (kTapFirst + i);
					g.tap.saved[i] = get32(slot);
					originals[i] = reinterpret_cast<void *>(uintptr_t(g.tap.saved[i]));
					set32(slot, g.tap.taps[i]);
				}
				g.tap.in_proxy = true;
				g.tap.on = true;
				return true;
			}
			const uintptr_t dev = client_device();
			if (dev == 0)
			{
				why = "the client device was not found";
				return false;
			}
			const uint32_t vt = word(dev);
			if (vt == 0)
			{
				why = "the client device has no vtable";
				return false;
			}
			if (g.tap.held)
			{
				why = "an earlier copy of the device's vtable is still in use";
				return false;
			}
			// Else a copy of the device's vtable (as the proxy makes its own), the four slots ours, put in front
			if (g.tap.copy == nullptr)
				g.tap.copy = static_cast<uint32_t *>(std::calloc(129, sizeof(uint32_t)));
			if (g.tap.copy == nullptr)
			{
				why = "no memory";
				return false;
			}
			g.tap.copy[0] = word(vt - 4);
			for (uint32_t i = 0; i < 128; ++i)
				g.tap.copy[1 + i] = word(vt + 4 * i);
			for (uint32_t i = 0; i < 4; ++i)
			{
				originals[i] = reinterpret_cast<void *>(uintptr_t(g.tap.copy[1 + kTapFirst + i]));
				g.tap.copy[1 + kTapFirst + i] = g.tap.taps[i];
			}
			if (!write32(dev, uint32_t(reinterpret_cast<uintptr_t>(g.tap.copy + 1))))
			{
				why = "could not write the client device";
				return false;
			}
			g.tap.in_proxy = false;
			g.tap.dev = dev;
			g.tap.vt = vt;
			g.tap.on = true;
			return true;
		}

		std::string proxy_off()
		{
			tap_off(); // (the tap goes first: it may be in the vtable going)
			if (g.proxy.base == 0)
				return std::string();
			proxy_flags();
			if (g.on[kBatch] || g.on[kDedupe] || g.proxy.dev == 0)
				return std::string();
			// Put the device's own vtable back unless something else has replaced ours since.
			if (word(g.proxy.dev) == uint32_t(psym(px::VTABLE)))
				write32(g.proxy.dev, uint32_t(g.proxy.vt));
			else
			{
				g.proxy.held = true;
				say("batch/dedupe: something else has replaced the client device's vtable since: left as it is");
			}
			set32(psym(px::D_PENDING), 0);
			g.proxy.dev = g.proxy.vt = 0;
			return std::string();
		}

		// ---------------------------------------------------------------- the toggles

		std::string turn_on(int n)
		{
			switch (n)
			{
			case kAshita: return ashita_on();
			case kOcclusion: return occlusion_on();
			case kWalls: return walls_on();
			case kSse: return sse_on();
			default: return proxy_on();
			}
		}

		void turn_off(int n)
		{
			switch (n)
			{
			case kAshita: ashita_off(); break;
			case kOcclusion: occlusion_off(); break;
			case kWalls: walls_off(); break;
			case kSse: sse_off(); break;
			default: proxy_off(); break;
			}
		}

		// Empty, or why it could not be turned on.
		std::string exp_set(int n, bool on)
		{
			if (on == g.on[n])
				return std::string();
			g.on[n] = on;
			std::string e;
			if (on)
				e = turn_on(n);
			else
				turn_off(n);
			if (!e.empty() && on)
			{
				// Undo whatever part of it was done.
				g.on[n] = false;
				turn_off(n);
			}
			return e;
		}

		void exp_all_off()
		{
			for (int n = 0; n < kCount; ++n)
				exp_set(n, false);
		}

		// Turns a toggle on or off (force: 'ashita' on even if something uses the draw events).
		void exp_toggle(int n, bool on, bool force)
		{
			if (n == kAshita && on)
			{
				if (force)
					g.force = true;
				else if (!g.on[kAshita])
					g.force = false;
			}
			const std::string e = exp_set(n, on);
			g.err[n] = !e.empty() ? "Not turned on: " + e + "." : std::string();
			if (!e.empty())
			{
				const char *hint = n == kAshita && !g.users.empty() ? " ashita_force turns it on anyway." : "";
				say(std::string(kNames[n]) + ": not turned on: " + e + "." + hint);
			}
			else if (n == kAshita && on && g.force && !g.users.empty())
				say("ashita forced on: " + join(g.users, "; ") + " will not get its draw events.");
		}

		// Once a second from present().
		void exp_tick()
		{
			if (g.occl)
				occl_set(occl_m());
			if (g.walls.base != 0 && g.walls.site != 0)
			{
				uint32_t now_w[7];
				for (size_t i = 0; i < 7; ++i)
					now_w[i] = get32(wsym(kWallsCounters[i]));
				if (g.walls.have_prev)
				{
					for (size_t i = 0; i < 7; ++i)
						g.walls.rate[i] = now_w[i] - g.walls.prev[i];
					g.walls.have_rate = true;
				}
				std::memcpy(g.walls.prev, now_w, sizeof(now_w));
				g.walls.have_prev = true;
			}
			ashita_watch();
			if (g.proxy.dev != 0)
			{
				const uint32_t now_c[3] = { get32(psym(px::S_SKIPPED)), get32(psym(px::S_MERGED)), get32(psym(px::S_BATCHES)) };
				if (g.proxy.have_prev)
				{
					for (size_t i = 0; i < 3; ++i)
						g.proxy.rate[i] = now_c[i] - g.proxy.prev[i];
					g.proxy.have_rate = true;
				}
				std::memcpy(g.proxy.prev, now_c, sizeof(now_c));
				g.proxy.have_prev = true;
			}
		}

		// What a toggle that is on is doing right now, or empty.
		std::string exp_stat(int n)
		{
			if (!g.on[n])
				return std::string();
			if (n == kAshita && !g.checked.empty())
			{
				if (g.force && !g.users.empty())
					return fmt("forced on: %d without their draw events", int(g.users.size()));
				return "nothing uses the draw events (" + g.checked + ")";
			}
			else if (n == kOcclusion && g.occl)
				return fmt("every %u frames at %.0f fps", 4 * (g.occl_m != 0 ? g.occl_m : 1), hifps::fps());
			else if (n == kWalls && g.walls.base != 0 && g.walls.site != 0)
			{
				// r: tests, segments, hidden, shown without a clear segment, over budget, the client's test, ticks (a second)
				const uint32_t *r = g.walls.rate;
				const double f = hifps::fps();
				if (!g.walls.have_rate || f <= 0)
					return std::string();
				const double ms = double(r[6]) * 1000 / g.freq;
				std::string s = fmt("%.1f tests a frame, %.1f segments, %.2f ms a frame", r[0] / f, r[1] / f, ms / f);
				if (r[1] > 0)
					s += fmt(" (%.0f us a segment)", ms * 1000 / r[1]);
				if (r[0] > 0)
					s += fmt("; %.0f%% hidden", 100.0 * r[2] / r[0]);
				if (r[4] > 0)
					s += fmt("; %u a second over the %.1f ms budget", r[4], kWallsMs);
				if (r[5] > 0)
					s += fmt("; %u a second on the GPU (a cutscene, or no collision)", r[5]);
				return s;
			}
			else if (n == kSse && g.sse && g.sse_addr == 0)
				return "already on (Intel CPU)";
			else if (g.proxy.have_rate && hifps::fps() > 0)
			{
				const double f = hifps::fps();
				if (n == kBatch)
					return fmt("%.0f quads a frame in %.0f draws", g.proxy.rate[1] / f, g.proxy.rate[2] / f);
				if (n == kDedupe)
					return fmt("%.0f calls a frame dropped", g.proxy.rate[0] / f);
			}
			return std::string();
		}

		int toggle_index(const char *name)
		{
			for (int n = 0; n < kCount; ++n)
				if (name != nullptr && std::strcmp(name, kNames[n]) == 0)
					return n;
			return -1;
		}
	}

	void set_ashita(void *ashita_core)
	{
		g_core = static_cast<IAshitaCore *>(ashita_core);
	}

	void set_host(const host *h)
	{
		g_host = h;
	}

	void update()
	{
		if (!hifps::on())
		{
			// ("Enable hifps first": nothing of this while the frame step is off)
			if (g.enabled)
				exp_all_off();
			g.enabled = false;
			g.restore_pending = false;
			return;
		}
		if (!g.enabled)
		{
			// (hifps turned on: the saved toggles back on once the client's device exists)
			g.enabled = true;
			g.restore_pending = true;
			g.timer = false;
			g.freq = qpc_freq();
		}
		bool wanted[kCount];
		for (int n = 0; n < kCount; ++n)
			wanted[n] = option_on(kSection, kOptions[n], true); // (each on unless turned off: the frame rate's options are on by default)
		const bool force = option_on(kSection, "ashita_force", false);
		if (g.restore_pending)
		{
			// Turns the saved toggles back on, once the client's device exists.
			if (client_device() == 0)
				return;
			g.restore_pending = false;
			std::vector<std::string> done;
			for (int n = 0; n < kCount; ++n)
			{
				if (wanted[n] && !g.on[n])
				{
					exp_toggle(n, true, n == kAshita && force);
					if (g.on[n])
						done.push_back(kNames[n]);
				}
			}
			if (!done.empty())
				say("Saved experimental toggles back on: " + join(done, ", ") + ".");
			std::memcpy(g.wanted, wanted, sizeof(wanted));
			g.wanted_force = force;
			return;
		}
		for (int n = 0; n < kCount; ++n)
		{
			if (wanted[n] != g.wanted[n])
			{
				g.wanted[n] = wanted[n];
				exp_toggle(n, wanted[n], n == kAshita && force);
			}
		}
		if (force != g.wanted_force)
		{
			g.wanted_force = force;
			if (wanted[kAshita] && force)
				exp_toggle(kAshita, true, true); // (on anyway, or forced while on)
			else if (wanted[kAshita] && g.on[kAshita] && g.force)
			{
				// No longer forced: off again if something needs the draw events
				g.force = false;
				if (!g.users.empty())
				{
					g.on[kAshita] = false;
					ashita_off();
					g.err[kAshita] = "Turned off: " + join(g.users, "; ") + " needs the draw events.";
				}
			}
		}
	}

	void present(double now)
	{
		if (!g.enabled)
			return;
		if (g.walls.base != 0 && g.walls.site != 0)
			walls_keep();
		if (!g.timer)
		{
			g.timer = true;
			g.tick_at = now;
		}
		else if (now - g.tick_at >= 1.0)
		{
			g.tick_at = now;
			exp_tick();
		}
	}

	void stop()
	{
		exp_all_off();
		tap_off();
		if (g.tap.copy != nullptr && !g.tap.held)
			std::free(g.tap.copy);
		if (g.walls.base != 0 && !g.walls.held)
			free_code(g.walls.base);
		if (g.proxy.base != 0 && !g.proxy.held)
			free_code(g.proxy.base);
		g = state();
	}

	bool draw_tap(const void *const taps[4], void *originals[4], std::string &why)
	{
		return tap_on(taps, originals, why);
	}

	void draw_untap()
	{
		tap_off();
	}

	std::vector<std::string> status()
	{
		std::vector<std::string> lines;
		if (!g.enabled)
		{
			lines.push_back("off: hifps's frame step is off, so nothing is turned on");
			return lines;
		}
		if (g.restore_pending)
			lines.push_back("the saved toggles go on once the client's device exists");
		for (int n = 0; n < kCount; ++n)
		{
			const std::string s = exp_stat(n);
			lines.push_back(fmt("%-9s %s  %s%s", kNames[n], g.on[n] ? "ON " : "off", kDesc[n], s.empty() ? "" : (" (" + s + ")").c_str()));
			if (!g.err[n].empty())
				lines.push_back("          " + g.err[n]);
			if (n == kAshita)
			{
				if (g.force && g.on[kAshita] && !g.users.empty())
					lines.push_back("          Without their draw events: " + join(g.users, "; "));
				for (const std::string &note : g.notes)
					lines.push_back("          Note: " + note + ".");
			}
		}
		for (const std::string &s : g.said)
			lines.push_back(s);
		return lines;
	}

	toggle_state toggle(const char *name)
	{
		toggle_state t;
		const int n = toggle_index(name);
		if (n < 0)
			return t;
		t.on = g.on[n];
		t.stat = exp_stat(n);
		t.err = g.err[n];
		return t;
	}

	ashita_view ashita_state()
	{
		ashita_view v;
		v.users = g.users;
		v.notes = g.notes;
		v.checked = g.checked;
		v.force = g.force;
		return v;
	}

	// /hifps exp ashita check: what uses the draw events, without changing anything.
	std::vector<std::string> ashita_check()
	{
		std::vector<std::string> lines;
		const uintptr_t dev = client_device();
		if (dev == 0)
		{
			lines.push_back("ashita check: the client device was not found.");
			return lines;
		}
		const uintptr_t dp = word((g.proxy.vt != 0 ? uint32_t(g.proxy.vt) : word(dev)) + 4 * 70);
		g.scan.clear();
		std::vector<std::string> users, notes;
		std::string checked;
		draw_event_users(dp, users, checked, notes);
		if (users.empty())
			lines.push_back("ashita check: " + checked + " checked; nothing uses the draw events.");
		else
			lines.push_back("ashita check: " + checked + " checked; these use the draw events: " + join(users, "; ") + ".");
		for (const std::string &note : notes)
			lines.push_back("ashita check: " + note + ".");
		return lines;
	}

	sites where()
	{
		sites s;
		s.walls = g.walls.base;
		s.proxy = g.proxy.base;
		s.walls_site = g.walls.site;
		s.occlusion = g.occl ? g.occl_addr : 0;
		s.sse = g.sse_addr;
		s.device = g.proxy.dev;
		s.vtable = g.proxy.base != 0 && g.proxy.dev != 0 ? g.proxy.base + px::VTABLE : 0;
		s.ashita = g.ashita;
		return s;
	}
}
