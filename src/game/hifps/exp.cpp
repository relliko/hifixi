/*
 * hifps's experimental frame-time toggles (exp.hpp): hifps.lua's /hifps exp (exp_set, exp_toggle, restore_exp,
 * exp_tick, exp_stat and what they use: ashita_*, occlusion_*), in C++, with batch and dedupe now done in HiFi XI's
 * own d3d8.dll. Its comments carried over where they still hold. (docs/frame-rate.md: what each saves, measured, and
 * why walls, sse and the device proxy went.)
 *
 * Frame-time toggles, off by default (experimental: no preset turns one on); each one puts back what it changed when
 * turned off, when hifps's frame step goes off (hifps::on()) and on stop(). None of them changes the game step.
 *   ashita    Ashita's device wrapper hands every draw call to every plugin, and the addons plugin looks the event up
 *             in every loaded addon, about 3,000 times a frame although no addon here handles d3d_dp/dip/dpup/dipup.
 *             Skips that (those four events stop firing). Before turning on it reads the loaded addons' Lua files
 *             for those event names and each plugin's four draw callbacks (any that do more than return false), and
 *             refuses if something uses them (ashita_force overrides). While on, it checks again whenever the loaded
 *             addons or plugins change, and turns itself off for a newcomer.
 *   occlusion Characters test whether they are hidden behind walls with a GPU readback that stalls the frame; the
 *             client tests each one every 4th frame. Above 60fps this tests every 4m-th frame instead, m = fps/60
 *             rounded down to a power of two, so a result is never older than at stock 60fps. Targeting reads the
 *             result, so this keeps it that fresh. While World FX's VisTest answers the characters' test (from its
 *             depth copy, without waiting: src/game/feed.cpp, set_actors_elsewhere) every character is tested every
 *             frame instead: an answer costs nothing there. That is the one path for "is this character hidden":
 *             VisTest while World FX draws, else the game's own test at this rate.
 *   batch     The text and UI draw one quad per draw call; our d3d8.dll draws consecutive quads with nothing changed
 *             in between as one draw. Same triangles, same order, same state (HifiFrameSetSpeedups).
 *   dedupe    Many of the client's device calls set a value already in effect; our d3d8.dll returns from those at
 *             once.
 *
 * The toggles follow HiFi XI's options ([hifps] exp_<name>, ashita_force) instead of hifps's settings file: a toggle
 * whose option goes on is turned on (once: one that can't be says why, and isn't tried again until its option goes
 * off and on, or hifps's frame step does); the saved ones are turned on once the client's device exists. Unlike the
 * addon, our memory is freed on stop() when nothing points into it any more, and nothing is put back over bytes that
 * aren't ours (the addon wrote the occlusion test's stock bytes back regardless).
 */

#include "../../../deps/ashita/plugins/sdk/Ashita.h" // (IAshitaCore: the plugins, Ashita's folder)
#include "../../host/frameapi.hpp" // (our d3d8.dll's HifiFrameSetSpeedups)
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
		enum toggle_id
		{
			kAshita,
			kOcclusion,
			kBatch,
			kDedupe,
			kCount
		};
		constexpr const char *kNames[kCount] = { "ashita", "occlusion", "batch", "dedupe" };
		constexpr const char *kOptions[kCount] = { "exp_ashita", "exp_occlusion", "exp_batch", "exp_dedupe" };
		constexpr const char *kDesc[kCount] = {
			"skip Ashita's per-draw plugin dispatch (d3d_dp/dip/dpup/dipup stop firing)",
			"test hidden characters less often above 60fps, as fresh as stock at 60fps (within 3%)",
			"merge consecutive text and UI quads into one draw (in HiFi XI's d3d8.dll)",
			"drop device calls that set a value already in effect (in HiFi XI's d3d8.dll)",
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

		// batch and dedupe: in our d3d8.dll (HifiFrameSetSpeedups); its counts once a second
		struct speedup_state
		{
			uint32_t flags = 0;      // as last set (kHifiDedupe, kHifiBatch)
			bool have_prev = false, have_rate = false;
			uint32_t prev[4] = {}, rate[4] = {}; // dropped, quads, their draws, every draw (a second)
		};

		// The draw tap (draw_tap): the client device's four draw slots pointed at the stream log's for a few frames
		struct tap_state
		{
			bool on = false;
			uintptr_t dev = 0;          // the device whose vtable our copy stands in for, and the one it had
			uint32_t vt = 0;
			uint32_t taps[4] = {};      // what the slots hold while on
			uint32_t *copy = nullptr;   // (vtable[-1] then 128 slots; never freed while the device may use it)
			bool held = false;          // something else replaced our copy on the device since: it stays
		};

		struct state
		{
			bool on[kCount] = {};
			std::string err[kCount];          // why it could not be turned on
			std::vector<uintptr_t> ashita;    // patched jumps
			bool occl = false;                // the occlusion test's bytes (and the m written, 0: none yet; kEveryFrame)
			uintptr_t occl_addr = 0;
			uint32_t occl_m = 0;
			speedup_state speed;
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
			double tick_at = 0.0;
			// hifps on (its frame step: hifps::on()), the options as last followed
			bool enabled = false, restore_pending = false;
			bool wanted[kCount] = {};
			bool wanted_force = false;
			bool actors_elsewhere = false;    // the characters' test is World FX's VisTest's (set_actors_elsewhere)
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

		// The game's data (the device's vtable pointer): written as its code is
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
			// (the device's own vtable: the stream log's tap may have a copy of it in front)
			const uint32_t vt = g.tap.on && g.tap.dev == dev ? g.tap.vt : word(dev);
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

		// (m kEveryFrame: 'xor eax, eax; xor edi, edi': the frame and the phase compared equal, a test every frame)
		constexpr uint32_t kEveryFrame = 0xFFFFFFFFu;

		void occl_bytes(uint32_t m, uint8_t out[12])
		{
			if (m == kEveryFrame)
			{
				const uint8_t b[12] = { 0x31, 0xC0, 0x31, 0xFF, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
				std::memcpy(out, b, 12);
				return;
			}
			const uint8_t sh = uint8_t(std::floor(std::log(double(m)) / std::log(2.0) + 0.5));
			const uint8_t b[12] = { 0x83, 0xE0, uint8_t(4 * m - 1), 0xC1, 0xE7, sh, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
			std::memcpy(out, b, 12);
		}

		// 3% slack: a frame limit of 120 measures as 119.9fps and should still count as 120.
		uint32_t occl_m()
		{
			if (g.actors_elsewhere)
				return kEveryFrame;
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

		// ---------------------------------------------------------------- batch / dedupe: in our d3d8.dll

		// Our d3d8.dll's speed-up calls (src/host/frameapi.hpp; the host's when the tests give their own); false when the
		// d3d8.dll loaded isn't HiFi XI's
		bool speedup_calls(hifi_frame_set_speedups_fn &set, hifi_frame_speedup_stats_fn &stats)
		{
			if (g_host != nullptr)
			{
				set = g_host->set_speedups;
				stats = g_host->speedup_stats;
			}
			else
			{
				const HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
				set = d3d8 != nullptr ? reinterpret_cast<hifi_frame_set_speedups_fn>(GetProcAddress(d3d8, kHifiFrameSetSpeedups)) : nullptr;
				stats = d3d8 != nullptr ? reinterpret_cast<hifi_frame_speedup_stats_fn>(GetProcAddress(d3d8, kHifiFrameSpeedupStats)) : nullptr;
			}
			return set != nullptr && stats != nullptr;
		}

		// The d3d8.dll told what's on now (g.on: batch, dedupe)
		std::string speedups_follow()
		{
			hifi_frame_set_speedups_fn set = nullptr;
			hifi_frame_speedup_stats_fn stats = nullptr;
			const uint32_t flags = (g.on[kDedupe] ? kHifiDedupe : 0) | (g.on[kBatch] ? kHifiBatch : 0);
			if (!speedup_calls(set, stats))
				return flags != 0 ? "it's done in HiFi XI's own d3d8.dll, and the game's d3d8.dll isn't it" : std::string();
			set(flags);
			if (flags != g.speed.flags)
				g.speed.have_prev = g.speed.have_rate = false;
			g.speed.flags = flags;
			return std::string();
		}

		// ---------------------------------------------------------------- the draw tap (draw_tap)

		constexpr uint32_t kTapFirst = 70; // DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP, DrawIndexedPrimitiveUP

		void tap_off()
		{
			if (!g.tap.on)
				return;
			if (g.tap.copy != nullptr)
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
			// A copy of the device's vtable, the four slots ours, put in front
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
			g.tap.dev = dev;
			g.tap.vt = vt;
			g.tap.on = true;
			return true;
		}

		// ---------------------------------------------------------------- the toggles

		std::string turn_on(int n)
		{
			switch (n)
			{
			case kAshita: return ashita_on();
			case kOcclusion: return occlusion_on();
			default: return speedups_follow();
			}
		}

		void turn_off(int n)
		{
			switch (n)
			{
			case kAshita: ashita_off(); break;
			case kOcclusion: occlusion_off(); break;
			default: speedups_follow(); break;
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
			ashita_watch();
			hifi_frame_set_speedups_fn set = nullptr;
			hifi_frame_speedup_stats_fn stats = nullptr;
			if (g.speed.flags != 0 && speedup_calls(set, stats))
			{
				uint32_t now_c[4] = {};
				stats(now_c);
				if (g.speed.have_prev)
				{
					for (size_t i = 0; i < 4; ++i)
						g.speed.rate[i] = now_c[i] - g.speed.prev[i];
					g.speed.have_rate = true;
				}
				std::memcpy(g.speed.prev, now_c, sizeof(now_c));
				g.speed.have_prev = true;
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
				return g.occl_m == kEveryFrame ? std::string("every frame (World FX's VisTest answers the characters' test)")
												: fmt("every %u frames at %.0f fps", 4 * (g.occl_m != 0 ? g.occl_m : 1), hifps::fps());
			else if (g.speed.have_rate && hifps::fps() > 0)
			{
				// (rates a second: dropped, quads, their draws, every draw)
				const double f = hifps::fps();
				const uint32_t *r = g.speed.rate;
				if (n == kBatch)
					return fmt("%.0f quads a frame in %.0f draws; %.0f draws a frame in all", r[1] / f, r[2] / f, r[3] / f);
				if (n == kDedupe)
					return fmt("%.0f calls a frame dropped", r[0] / f);
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

	void set_actors_elsewhere(bool on)
	{
		if (g.actors_elsewhere == on)
			return;
		g.actors_elsewhere = on;
		if (g.occl)
			occl_set(occl_m());
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
		}
		bool wanted[kCount];
		for (int n = 0; n < kCount; ++n)
			wanted[n] = option_on(kSection, kOptions[n], false); // (each off unless turned on: experimental, off by default)
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
		const uintptr_t dp = word((g.tap.on && g.tap.dev == dev ? g.tap.vt : word(dev)) + 4 * 70);
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

	std::string state_line()
	{
		if (!g.enabled)
			return "none (the frame step is off)";
		if (g.restore_pending)
			return "waiting for the client's device";
		std::string on, off;
		for (int n = 0; n < kCount; ++n)
		{
			std::string one = kNames[n];
			if (n == kOcclusion && g.on[n] && g.occl_m == kEveryFrame)
				one += " (every frame: VisTest answers the characters' test)";
			else if (n == kAshita && g.on[n] && g.force)
				one += " (forced)";
			else if (!g.on[n] && !g.err[n].empty())
				one += " (" + g.err[n] + ")";
			std::string &list = g.on[n] ? on : off;
			list += (list.empty() ? "" : ", ") + one;
		}
		return "on: " + (on.empty() ? std::string("none") : on) + "; off: " + (off.empty() ? std::string("none") : off);
	}

	sites where()
	{
		sites s;
		s.occlusion = g.occl ? g.occl_addr : 0;
		s.ashita = g.ashita;
		s.speedups = g.speed.flags;
		return s;
	}
}
