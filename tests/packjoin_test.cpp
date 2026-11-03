/*
 * The texture pack's module joined with its real consumers as the plugin wires them (src/plugin/pack_wiring: main.cpp
 * makes the same two calls; docs/texture-pack/characters.md decision 12, M4's open item "main.cpp's wiring has no
 * offline test"), driven through passes::modules() ("textures": update, present, device, command, stop) on WARP with
 * the debug layer (HIFIXI_GBV=1: GPU-based validation too), its settings in a folder of its own, a made-up pack under
 * build\test\packjoin whose records are 4x of made-up character textures (DXT1 chunks of their own), the characters'
 * cache on the same device:
 *   - wire (twice, as a plugin loaded twice in a session would): the zones' consumer, then the characters', once each;
 *   - the hifi-textures overlay listed in a scratch pivot.ini (HIFIXI_PIVOT_INI): the menu says it's installed, and the
 *     characters' line says why none of theirs is from the pack (a key not in it); unlisted and reloaded: no such lines;
 *   - on: the characters' textures from the pack (their second pairs), the one not in it the game's; the menu's
 *     Characters line with what the game drew itself (the game_drawn source); the status's character line;
 *   - follow_texture_bias with no FSR: 0; with FSR drawing a world half the screen's width (upscale::test::drawn): -1
 *     into both injects, the zones' and the characters'; [textures] fsr_bias=0: 0 again;
 *   - off: every character's pack part let go in that same call (the game's views handed out at once, the pairs behind
 *     the GPU), the owner dropped; on again: from the pack again;
 *   - reload: a new epoch, from the pack again;
 *   - [textures] chars off with the pack on (set_mode alone): every character's pack part let go, the owner dropped, the
 *     zones still in use, the line says so; on again: from the pack again;
 *   - a device change to another adapter's device (the GPU while the rest is on WARP; D3D12 makes one device per
 *     adapter, so a second WARP device is the same one; skipped without a second adapter): the characters ask nothing (the cache never follows a device), their line says
 *     so; back to the cache's device: from the pack again;
 *   - the video memory over 95% (the streamer's limits, between two of the module's updates): every character texture
 *     back to the game's in the module's present, nothing asked, the line and the log say so; held kEvictHoldS once
 *     it's gone, then from the pack again;
 *   - the streamer's GPU parts refused at the next device (stream::test::fail_parts): the menu and the status say
 *     "Texture pack can't be used: ... Restart the game to try again." and no "Found";
 *   - the plugin's unload: the module's stop (both consumers cleared: no character pack pair, no streamer texture), the
 *     cache's stop, retire::release_all; the debug layer silent throughout.
 * Zone 230 through native (the FFXiMain dump at its base) isn't drawn here: zonenative_test and packmodule_test cover the
 * zones' glue; here it's registered and called in its turn.
 *
 *   build\packjoin_test.exe     (ZONE_GPU=1: the GPU instead of WARP)
 */

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../src/backend/backend.hpp"
#include "../src/chars/cache.hpp"
#include "../src/chars/charinject.hpp"
#include "../src/chars/charpack.hpp"
#include "../src/game/game.hpp"
#include "../src/host/passes.hpp"
#include "../src/host/retire.hpp"
#include "../src/pack/bc2.hpp"
#include "../src/pack/pack.hpp"
#include "../src/pack/pack_module.hpp"
#include "../src/pack/pack_write.hpp"
#include "../src/pack/stream.hpp"
#include "../src/plugin/pack_wiring.hpp"
#include "../src/upscale/upscale.hpp"
#include "../src/zone/inject.hpp"
#include "../src/zone/zonepack.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace st = hifi::pack::stream;
namespace pk = hifi::pack;
namespace cache = hifi::chars::cache;
namespace cp = hifi::chars::charpack;

namespace
{
	int failures = 0, passes = 0;
	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
		if (ok)
			passes++;
		else
			failures++;
	}
	void note(const std::string &what) { std::printf("--    %s\n", what.c_str()); }

	double now_ms()
	{
		LARGE_INTEGER t, f;
		QueryPerformanceCounter(&t);
		QueryPerformanceFrequency(&f);
		return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
	}

	void remove_tree(const std::wstring &folder)
	{
		WIN32_FIND_DATAW f;
		HANDLE h = FindFirstFileW((folder + L"*").c_str(), &f);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
			{
				const std::wstring name = f.cFileName;
				if (name == L"." || name == L"..")
					continue;
				if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					remove_tree(folder + name + L"\\");
				else
					DeleteFileW((folder + name).c_str());
			} while (FindNextFileW(h, &f));
			FindClose(h);
		}
		RemoveDirectoryW(folder.substr(0, folder.size() - 1).c_str());
	}

	std::string narrow(const std::wstring &w)
	{
		std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
		return s;
	}

	void write_text(const std::wstring &path, const std::string &text)
	{
		if (FILE *f = nullptr; _wfopen_s(&f, path.c_str(), L"wb") == 0 && f != nullptr)
		{
			fwrite(text.data(), 1, text.size(), f);
			fclose(f);
		}
	}

	// ---- The devices (the cache's and the module's; a second one for the device change)

	struct gpu_t
	{
		ID3D12Device *device = nullptr;
		ID3D12InfoQueue *info = nullptr;
		ID3D12CommandQueue *direct = nullptr;
	};
	gpu_t G, G2;

	bool make_gpu(gpu_t &g, bool hardware)
	{
		IDXGIFactory4 *factory = nullptr;
		IDXGIAdapter1 *warp = nullptr;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
			return false;
		if (!hardware)
			factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
		const bool ok = SUCCEEDED(D3D12CreateDevice(hardware ? nullptr : warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device)));
		if (warp != nullptr)
			warp->Release();
		factory->Release();
		if (!ok)
			return false;
		g.device->QueryInterface(IID_PPV_ARGS(&g.info));
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.direct));
		return g.direct != nullptr;
	}

	uint32_t debug_messages(gpu_t &g, std::string &first)
	{
		if (g.info == nullptr)
			return 0;
		const UINT64 n = g.info->GetNumStoredMessages();
		uint32_t errors = 0;
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T size = 0;
			g.info->GetMessage(i, nullptr, &size);
			std::vector<uint8_t> buf(size);
			D3D12_MESSAGE *m = reinterpret_cast<D3D12_MESSAGE *>(buf.data());
			g.info->GetMessage(i, m, &size);
			if (m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && errors++ == 0)
				first = m->pDescription;
		}
		g.info->ClearStoredMessages();
		return errors;
	}
	void messages_check(const std::string &part)
	{
		std::string first;
		const uint32_t n = debug_messages(G, first) + debug_messages(G2, first);
		check(n == 0 && G.device->GetDeviceRemovedReason() == S_OK, part + ": the debug layer said nothing (" + std::to_string(n) + (n != 0 ? ": " + first : std::string()) + ")");
	}

	// ---- The made-up character textures and their pack (4x records: 256 x 256 BC2, the chain to 4 x 4)

	// A DXT1 64 x 64 chunk with its blocks and a name of its own (i): a key of its own
	std::vector<uint8_t> named(uint32_t i)
	{
		std::vector<uint8_t> c(69 + 16 * 16 * 8, 0);
		c[0] = 0x20;
		char name[17];
		std::snprintf(name, sizeof(name), "pj pack %08u", i);
		std::memcpy(c.data() + 1, name, 16);
		const uint16_t side = 64;
		std::memcpy(c.data() + 21, &side, 2);
		std::memcpy(c.data() + 25, &side, 2);
		std::memcpy(c.data() + 57, "1TXD", 4);
		return c;
	}

	bool make_pack(const std::wstring &folder, uint32_t keys, std::string &why)
	{
		remove_tree(folder);
		pk::writer w;
		pk::writer_options o;
		o.folder = folder;
		o.source = "packjoin_test";
		if (!w.open(o, why))
			return false;
		for (uint32_t i = 0; i < keys; ++i)
		{
			const std::vector<uint8_t> c = named(i);
			const pk::key k = pk::key_of(c.data(), c.size());
			pk::entry e;
			e.hash = k.hash;
			e.bytes = k.bytes;
			e.w = k.w;
			e.h = k.h;
			e.orig_format = c[0];
			e.source_kind = pk::kSourceModel;
			std::memcpy(e.name, c.data() + 1, 16);
			pk::map_in m;
			m.format = pk::kBC2;
			m.w = m.h = 256;
			std::vector<uint8_t> px(size_t(256) * 256 * 4, 0x80);
			const std::vector<uint8_t> top = pk::bc2::encode_bc2(px.data(), 256, 256);
			m.levels = pk::bc2::full_chain(top.data(), 256, 256, pk::kAlphaNone, pk::bc2::kCharsCutThreshold);
			if (w.add(e, { m }, why) != pk::add_result::ok)
				return false;
		}
		const bool ok = w.commit(why);
		w.close();
		return ok;
	}

	// ---- The plugin's calls

	const hifi::passes::module *M = nullptr;
	double g_now = 100.0, g_cache_now = 1.0;
	std::vector<cache::key> g_named; // (the character textures the game names each frame: kept by the cache)
	ID3D12CommandQueue *g_queue = nullptr; // (the game's queue: the retire list's, as backend::present has it)

	// (pack_wiring::wire's sources: no zone pass here, the game drew 3 models itself)
	bool no_camera(float[3], uint32_t &) { return false; }
	uint32_t three_drawn() { return 3; }
	std::mutex g_log_mutex;
	std::vector<std::string> g_log;
	void log_sink(int, const char *text)
	{
		std::lock_guard<std::mutex> lock(g_log_mutex);
		g_log.push_back(text);
	}

	void update()
	{
		g_now += 0.5;
		M->update(g_now);
	}
	// A present as the plugin makes one: the textures named at the marks, the cache's tick, the modules' presents (the
	// texture pack's: the streamer, the zones' then the characters' frames), the FSR bias, then backend::present's
	// retire::on_present, the GPU idle after
	void present()
	{
		g_now += 0.004;
		g_cache_now += 0.004;
		for (const cache::key &k : g_named)
			cache::texture(k);
		cache::frame_tick(G.device, g_cache_now);
		M->present(g_now, true);
		hifi::pack_wiring::follow_texture_bias();
		hifi::retire::on_present(g_queue);
		for (int k = 0; k < 100; ++k)
		{
			const hifi::retire::counts r = hifi::retire::status();
			if (r.completed >= r.signalled)
				break;
			Sleep(1);
		}
	}
	std::vector<std::string> command(const char *what)
	{
		std::vector<std::string> args { "textures" }, out;
		if (what != nullptr)
			args.push_back(what);
		M->command(args, out);
		return out;
	}
	bool has(const std::vector<std::string> &lines, const std::string &text)
	{
		for (const std::string &l : lines)
			if (l.find(text) != std::string::npos)
				return true;
		return false;
	}
	std::string first_with(const std::vector<std::string> &lines, const std::string &text)
	{
		for (const std::string &l : lines)
			if (l.find(text) != std::string::npos)
				return l;
		return "(none)";
	}
	bool wait_in_use(double timeout_ms = 30000)
	{
		const double t0 = now_ms();
		while (now_ms() - t0 < timeout_ms)
		{
			update();
			present();
			if (pk::in_use())
				return true;
			Sleep(2);
		}
		return false;
	}
	// Presents (and an update each half second's worth) until the cache's pack parts are settled: n ready, none asked
	bool wait_ready(uint32_t n, double timeout_ms = 30000)
	{
		const double t0 = now_ms();
		for (int i = 0; now_ms() - t0 < timeout_ms; ++i)
		{
			present();
			if (i % 20 == 0)
				update();
			const cache::counts c = cache::get_counts();
			if (c.pack_ready == n && c.pack_asked == 0)
			{
				update(); // (the lines made again with it)
				return true;
			}
			Sleep(1);
		}
		return false;
	}

	// The characters' textures' views now: how many the pack's (and every one handed out)
	uint32_t pack_views(const std::vector<uint32_t> &ids, bool &all_out)
	{
		uint32_t n = 0;
		all_out = true;
		for (uint32_t id : ids)
		{
			D3D12_GPU_DESCRIPTOR_HANDLE h = {};
			bool is_pack = false;
			all_out = all_out && cache::texture_view_of(id, h, true, &is_pack);
			n += is_pack;
		}
		return n;
	}
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const std::wstring build = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\') + 1);
	const std::wstring here = build + L"test\\packjoin\\", pack_folder = here + L"hifixi-pack\\", settings_file = here + L"config\\hifixi\\settings.ini";
	const std::wstring pivot = here + L"pivot.ini";
	CreateDirectoryW((build + L"test").c_str(), nullptr);
	CreateDirectoryW(here.c_str(), nullptr);
	CreateDirectoryW((here + L"config").c_str(), nullptr);
	CreateDirectoryW((here + L"config\\hifixi").c_str(), nullptr);
	DeleteFileW(settings_file.c_str());
	SetEnvironmentVariableW(L"HIFIXI_PACK", pack_folder.c_str());
	// (a scratch pivot.ini listing the hifi-textures overlay, as a PC that hasn't run install.py --unlist: never the game's)
	write_text(pivot, "[settings]\r\nroot_path=" + narrow(here) + "DATs\r\n\r\n[overlays]\r\n1=hifi-textures\r\n");
	SetEnvironmentVariableW(L"HIFIXI_PIVOT_INI", pivot.c_str());
	const bool on_gpu = GetEnvironmentVariableA("ZONE_GPU", nullptr, 0) != 0;

	check(hifi::backend::start_settings_only(settings_file.c_str(), here.c_str()), "the settings alone started (no hooks), in a folder of its own");
	hifi::backend::set_log_sink(log_sink);
	ID3D12Debug *debug = nullptr;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
	{
		debug->EnableDebugLayer();
		ID3D12Debug1 *debug1 = nullptr;
		if (GetEnvironmentVariableA("HIFIXI_GBV", nullptr, 0) != 0 && SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&debug1))))
		{
			debug1->SetEnableGPUBasedValidation(TRUE);
			debug1->Release();
			std::printf("      (GPU-based validation on)\n");
		}
	}
	if (!make_gpu(G, on_gpu))
	{
		check(false, on_gpu ? "a device on the GPU" : "a WARP device");
		return 1;
	}
	// (the device change's other device: another adapter's, as D3D12 hands out one device per adapter; none on a PC with
	// WARP only: that part skipped)
	const bool second = make_gpu(G2, !on_gpu) && G2.device != G.device;
	g_queue = G.direct;
	note(std::string(on_gpu ? "on the GPU" : "on WARP") + (G.info != nullptr ? ", the debug layer on" : ", NO debug layer (Graphics Tools missing)"));
	for (const hifi::passes::module &m : hifi::passes::modules())
		if (m.name != nullptr && std::strcmp(m.name, "textures") == 0)
			M = &m;
	check(M != nullptr && M->update != nullptr && M->present != nullptr && M->stop != nullptr && M->command != nullptr && M->device != nullptr,
		"the \"textures\" module registered");
	if (M == nullptr)
		return 1;

	// ---- The wiring, as main.cpp's Initialize makes it (twice: kept once)
	hifi::pack_wiring::wire(no_camera, log_sink, three_drawn);
	hifi::pack_wiring::wire(no_camera, log_sink, three_drawn);
	{
		const std::vector<pk::consumer> c = pk::test::consumers();
		const pk::consumer z = hifi::zone::zonepack::consumer(), ch = cp::consumer();
		check(c.size() == 2 && c[0].clear_all == z.clear_all && c[0].frame == z.frame && c[1].clear_all == ch.clear_all && c[1].frame == ch.frame &&
			c[1].lines == ch.lines && c[1].set_mode == ch.set_mode, "pack_wiring::wire (twice): the zones' consumer, then the characters', once each (" + std::to_string(c.size()) + ")");
	}

	// ---- The made-up pack and the characters' textures (8 in it, one not)
	constexpr uint32_t kKeys = 8;
	{
		std::string why;
		check(make_pack(pack_folder, kKeys, why), "a made-up pack of " + std::to_string(kKeys) + " character textures' 4x records" + (why.empty() ? std::string() : ": " + why));
	}
	pk::test::force_quiet(0);
	M->device(G.device);
	cache::frame_tick(G.device, g_cache_now);
	std::vector<uint32_t> ids;
	uint32_t miss = cache::kNone;
	{
		// (one not in the pack first: what a key of the overlay's copies would be)
		cache::key k;
		k.resource = 0x20000000u;
		miss = cache::add_texture(k, named(9000));
		g_named.push_back(k);
	}
	cache::settle(60000);

	// ---- The overlay listed: said in the menu, and why the characters' none
	hifi::game::set_option_on("textures", "pack", true);
	{
		const bool in = wait_in_use();
		bool settled = false;
		for (int i = 0; i < 4000 && !settled; ++i)
		{
			present();
			settled = std::strcmp(cache::test::pack_state_of(miss), "spent") == 0;
			Sleep(1);
		}
		update();
		const std::vector<std::string> menu = pk::menu_lines();
		check(in && settled && pk::overlay_listed() && has(menu, "The hifi-textures XIPivot overlay is still installed") &&
			has(menu, "Characters: none of 1 textures from the pack: the hifi-textures overlay is still installed"),
			"the overlay listed (a scratch pivot.ini): the menu says it's installed, the characters' line why none of theirs: " + first_with(menu, "Characters:"));
		present();
		bool logged = false;
		{
			std::lock_guard<std::mutex> lock(g_log_mutex);
			logged = has(g_log, "the characters' textures aren't in the pack while the hifi-textures XIPivot overlay is installed");
		}
		check(logged, "  logged once");
	}
	write_text(pivot, "[settings]\r\nroot_path=" + narrow(here) + "DATs\r\n\r\n[overlays]\r\n1=SomeoneElse\r\n");
	command("reload");
	for (uint32_t i = 0; i < kKeys; ++i)
	{
		cache::key k;
		k.resource = 0x10000000u + i * 16;
		ids.push_back(cache::add_texture(k, named(i)));
		g_named.push_back(k);
	}
	cache::settle(60000);

	// ---- On: the characters from the pack, the line, the status
	{
		const bool in = wait_in_use();
		const bool ready = wait_ready(kKeys);
		bool all = false;
		const uint32_t from_pack = pack_views(ids, all);
		D3D12_GPU_DESCRIPTOR_HANDLE h = {};
		bool miss_pack = true;
		const bool miss_out = cache::texture_view_of(miss, h, true, &miss_pack);
		const std::vector<std::string> menu = pk::menu_lines();
		const std::string line = "Characters: " + std::to_string(kKeys) + " of " + std::to_string(kKeys + 1) + " textures from the pack (1 not in it, 0 coming); 3 models drawn by the game itself.";
		check(in && ready && !pk::overlay_listed() && from_pack == kKeys && all && miss_out && !miss_pack,
			"unlisted and reloaded: the " + std::to_string(kKeys) + " characters' textures from the pack, the one not in it the game's (" + std::to_string(from_pack) + ")");
		check(has(menu, line) && !has(menu, "XIPivot overlay is still installed"), "  the menu: " + first_with(menu, "Characters:"));
		const std::vector<std::string> status = command("status");
		check(has(cache::status(), "texture pack: " + std::to_string(kKeys) + " ready") && has(status, "characters on"),
			"  the status: " + first_with(cache::status(), "texture pack:"));
		check(hifi::pack_wiring::texture_bias() == 0.0f && hifi::zone::inject::texture_bias() == 0.0f && hifi::chars::inject::texture_bias() == 0.0f,
			"  follow_texture_bias with no FSR: 0 into both injects");
		// (FSR drawing a world half the screen's width, then two thirds: the clamp at -1, and log2 between)
		hifi::upscale::test::drawn(1280, 2560);
		hifi::pack_wiring::follow_texture_bias();
		const float half_zone = hifi::zone::inject::texture_bias(), half_chars = hifi::chars::inject::texture_bias();
		hifi::upscale::test::drawn(1707, 2560);
		hifi::pack_wiring::follow_texture_bias();
		const float third_zone = hifi::zone::inject::texture_bias(), third_chars = hifi::chars::inject::texture_bias();
		char text[200];
		std::snprintf(text, sizeof(text), "  follow_texture_bias under FSR: half the width %.3f / %.3f, two thirds %.3f / %.3f (zones / characters)", half_zone, half_chars,
			third_zone, third_chars);
		check(half_zone == -1.0f && half_chars == -1.0f && third_zone == third_chars && third_zone < -0.5f && third_zone > -0.65f, text);
		// (and none once FSR's gone a quarter second)
		Sleep(300);
		hifi::pack_wiring::follow_texture_bias();
		check(hifi::zone::inject::texture_bias() == 0.0f && hifi::chars::inject::texture_bias() == 0.0f, "  FSR gone: 0 into both again");
		messages_check("on");
	}

	// ---- Off: let go in that call; on again
	{
		const uint32_t pairs = cache::get_counts().pack_pairs;
		command("off");
		bool all = false;
		const uint32_t from_pack = pack_views(ids, all);
		const cache::counts c = cache::get_counts();
		check(pairs == kKeys && from_pack == 0 && all && c.pack_pairs == 0 && c.pack_ready == 0 && !cp::get_counts().owner && !pk::in_use(),
			"off: every character's pack part let go in that same call (the game's views at once, " + std::to_string(pairs) + " pairs behind the GPU), the owner dropped");
		update();
		present();
		command("on");
		const bool in = wait_in_use();
		const bool ready = wait_ready(kKeys);
		check(in && ready && pack_views(ids, all) == kKeys, "on again: from the pack again (characters at once)");
		messages_check("off and on");
	}

	// ---- Reload: a new epoch, from the pack again
	{
		const uint32_t e0 = pk::epoch_in_use();
		command("reload");
		bool all = false;
		const uint32_t right_after = pack_views(ids, all);
		const bool in = wait_in_use();
		const bool ready = wait_ready(kKeys);
		check(right_after == 0 && in && ready && pk::epoch_in_use() > e0 && pack_views(ids, all) == kKeys,
			"reload: let go at once, then from the pack again in epoch " + std::to_string(pk::epoch_in_use()));
	}

	// ---- [textures] chars off with the pack on (the hidden key: set_mode alone, no clear_all): every character's pack part
	// let go in the next update, the owner dropped, the zones still in use, the line says so; on again: from the pack again
	{
		const uint32_t pairs = cache::get_counts().pack_pairs;
		hifi::game::set_option_on("textures", "chars", false);
		update();
		present();
		bool all = false;
		const uint32_t from_pack = pack_views(ids, all);
		const cache::counts c = cache::get_counts();
		const std::vector<std::string> menu = pk::menu_lines();
		check(pairs == kKeys && from_pack == 0 && all && c.pack_pairs == 0 && c.pack_ready == 0 && !cp::get_counts().owner && pk::in_use() &&
				has(menu, "Characters: the game's own textures ([textures] chars is off)."),
			"[textures] chars off, the pack on: every character's pack part let go (" + std::to_string(pairs) + " pairs), the owner dropped, the zones' still in use; " +
				first_with(menu, "Characters:"));
		hifi::game::set_option_on("textures", "chars", true);
		update();
		const bool ready = wait_ready(kKeys);
		check(ready && pack_views(ids, all) == kKeys && cp::get_counts().owner, "  chars on again: from the pack again");
		messages_check("chars off and on");
	}

	// ---- A device change: the characters wait for the cache's device
	if (!second)
		note("the device change: skipped (no second adapter on this PC)");
	else
	{
		M->device(nullptr);
		M->device(G2.device);
		const bool in = wait_in_use();
		for (int i = 0; i < 40; ++i)
			present();
		update();
		bool all = false;
		const uint32_t from_pack = pack_views(ids, all);
		const std::vector<std::string> menu = pk::menu_lines();
		check(in && pk::device_in_use() == G2.device && from_pack == 0 && cache::get_counts().pack_asked == 0 && cp::get_counts().other_device &&
			has(menu, "Characters: not from the pack until the game is restarted (the graphics device changed)."),
			"the module on another adapter's device: the characters ask nothing (the cache's device differs), the line says so: " + first_with(menu, "Characters"));
		M->device(nullptr);
		M->device(G.device);
		const bool back = wait_in_use() && wait_ready(kKeys);
		check(back && pack_views(ids, all) == kKeys && !cp::get_counts().other_device, "  back on the cache's device: from the pack again");
		messages_check("the device change");
	}

	// ---- The video memory nearly full (the streamer's limits over 95% between two updates, as the module's own update
	// would set them on a full card): the characters' textures back to the game's through the module's present, nothing
	// asked meanwhile, the line and the log say so; held kEvictHoldS once the pressure's gone, then from the pack again
	{
		cp::test::set_evict_bytes(1ull << 40); // (every texture in one round: the made-up ones are small)
		update();
		st::limits full;
		full.vram_usage = 99ull << 20;
		full.vram_budget = 100ull << 20;
		st::set_limits(full);
		uint32_t asked = 0;
		for (int i = 0; i < 20; ++i)
		{
			present();
			asked += cache::get_counts().pack_asked;
		}
		bool all = false;
		const uint32_t from_pack = pack_views(ids, all);
		const cache::counts c = cache::get_counts();
		std::vector<std::string> lines;
		cp::lines(lines);
		bool logged = false;
		{
			std::lock_guard<std::mutex> lock(g_log_mutex);
			logged = has(g_log, "character textures back to the game's (video memory over 95%)");
		}
		check(st::pressure() && from_pack == 0 && all && c.pack_evicted == kKeys && c.pack_pairs == 0 && asked == 0 && logged &&
				has(lines, std::to_string(kKeys) + " back to the game's while the graphics card is nearly full"),
			"the video memory over 95%: the " + std::to_string(kKeys) + " characters' textures back to the game's in the module's present, nothing asked, logged; " + first_with(lines, "Characters:"));
		// (the module's next updates set the real numbers: the pressure gone, the textures held till kEvictHoldS)
		const double gone_at = g_now;
		while (g_now - gone_at < cp::kEvictHoldS - 1.5)
		{
			update();
			present();
		}
		const bool held = !st::pressure() && pack_views(ids, all) == 0 && cache::get_counts().pack_asked == 0;
		while (g_now - gone_at < cp::kEvictHoldS + 1.0)
		{
			update();
			present();
		}
		const bool back = wait_ready(kKeys);
		check(held && back && pack_views(ids, all) == kKeys, "  the pressure gone: held " + std::to_string(int(cp::kEvictHoldS)) + " s, then from the pack again");
		cp::test::set_evict_bytes(0);
		messages_check("the eviction");
	}

	// ---- The streamer's GPU parts refused (as a card short of the ring): said, no "Found"
	{
		st::test::fail_parts("the test refused the ring");
		M->device(nullptr);
		M->device(G.device);
		bool said = false;
		for (int i = 0; i < 200 && !said; ++i)
		{
			update();
			present();
			said = has(pk::menu_lines(), "Texture pack can't be used: the test refused the ring. Restart the game to try again.");
			Sleep(5);
		}
		const std::vector<std::string> menu = pk::menu_lines(), status = command("status");
		check(said && !has(menu, "Found:") && has(status, "can't be used: the test refused the ring. Restart the game to try again."),
			"the ring refused: the menu and the status say \"Texture pack can't be used: ... Restart the game to try again.\", no Found");
		st::test::fail_parts(nullptr);
	}

	// ---- The plugin's unload: the module's stop clears both consumers, then the cache, then the retire list
	{
		std::string why;
		const bool stopped = M->stop(why);
		const cache::counts c = cache::get_counts();
		check(stopped && c.pack_pairs == 0 && c.pack_ready == 0 && !cp::get_counts().owner && st::get_counts().resident == 0,
			"unload: the module's stop let go of every character pack part and the owner; no streamer texture left");
		M->device(nullptr);
		cache::stop();
		hifi::retire::release_all();
		messages_check("the unload");
	}

	if (G2.info != nullptr)
		G2.info->Release();
	if (G2.direct != nullptr)
		G2.direct->Release();
	if (G2.device != nullptr)
		G2.device->Release();
	if (G.info != nullptr)
		G.info->Release();
	G.direct->Release();
	G.device->Release();
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
