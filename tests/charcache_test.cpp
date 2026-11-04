/*
 * The characters' cache's batches and descriptor pairs (src/chars/cache.cpp), with made-up DXT1 texture chunks (the
 * header as tex.hpp has it, the blocks left out: decoded as transparent black) and one real mesh (the Mithra's first,
 * ROM2/0/52.DAT), on WARP:
 *   - 2,000 1024 x 1024 textures queued with no device: no more than one batch held decoded (kBatchBytes and one entry at
 *     most), the rest held back, settle() still returns; let go when no longer named, queued ones too;
 *   - still with no device, a 2048 x 2048 held back by 8 1024 x 1024 decoded: 4 small ones queued after it aren't
 *     decoded while it waits (though they'd fit), and once the 8 are let go it's decoded, then they are;
 *   - on WARP, 24 more: three batches or more, none over kBatchBytes, the peak decoded still one batch; detail maps off:
 *     none made;
 *   - one 4096 x 4096 (85 MB with its mips, over kBatchBytes) queued with small ones: uploaded in a batch of its own,
 *     decoded alone (the peak is its bytes exactly);
 *   - detail maps on: a small one gets its detail map; a 2048 x 2048 whose map with make_detail's working images would
 *     be over kEntryCap is on the GPU without one; a mesh naming a texture that can't be read isn't ready (left to the
 *     game) while one naming a ready texture is;
 *   - the pairs of descriptors let go wait for the GPU (retire::then): not free after the next present, free once the
 *     fence has passed two; let go before stop() never come back into the next heap (counted once the retire list is
 *     empty, so the last heap's closures have run);
 *   - the texture pack's part (docs/texture-pack/characters.md; the streamer on WARP, a made-up pack of 160 4x records
 *     under build\test\charcache, src/chars/charpack driven as the module drives it): with no glue, keys made and nothing
 *     asked; 64 in the pack ready through pairs of their own (BC2, the record's levels down to the game's last, at most
 *     its chain), one not in it spent, the line; compare; the same bytes under two resources one texture, one let go and
 *     the other still holding it; clear_all lets go of the pack's part only; the free pairs at kPackSlotFloor: nothing
 *     asked, and ready tickets let go for want of a pair, asked again once there are; 1,000 and more let go while
 *     upgrading (asked, between pack_begin and pack_end, ready): no pair twice, none free before the fence, all back
 *     after; five pairs over the floor: five asked, none read for nothing; under the streamer's pressure (set_limits over
 *     95%) the least recently drawn (not the first ready) back to the game's a round each half second, nothing asked
 *     meanwhile (a new texture among it), asked again once it's been gone kEvictHoldS, a round at a time; back just after:
 *     held twice as long, then 10 s again; stop() drops every ticket, and
 *     no pack pair of the last heap comes back into the next.
 *
 *   build\charcache_test.exe     (built by build.cmd)
 */

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../src/chars/cache.hpp"
#include "../src/chars/charpack.hpp"
#include "../src/chars/tex.hpp"
#include "../src/host/retire.hpp"
#include "../src/pack/bc2.hpp"
#include "../src/pack/pack.hpp"
#include "../src/pack/pack_write.hpp"
#include "../src/pack/stream.hpp"
#include "../src/pack/validate.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

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

	namespace cache = hifi::chars::cache;
	constexpr uint32_t kAllPairs = cache::kDescriptors / 2 - cache::kReserved / 2;

	std::string mb(uint64_t bytes)
	{
		char t[32];
		std::snprintf(t, sizeof(t), "%.1f MB", double(bytes) / 1048576.0);
		return t;
	}

	// A DXT1 texture chunk's body (layout 2, DXT only): the header and, when asked, black blocks
	std::vector<uint8_t> chunk(uint32_t w, uint32_t h, bool blocks = false)
	{
		std::vector<uint8_t> c(69, 0);
		c[0] = 0x20;
		std::memcpy(c.data() + 1, "charcache test  ", 16);
		const uint16_t w16 = uint16_t(w), h16 = uint16_t(h);
		std::memcpy(c.data() + 21, &w16, 2);
		std::memcpy(c.data() + 25, &h16, 2);
		std::memcpy(c.data() + 57, "1TXD", 4); // ('DXT1' backwards; then a dword and the row pitch, the blocks at 69)
		if (blocks)
			c.resize(69 + size_t(w / 4) * (h / 4) * 8, 0);
		return c;
	}

	// What one decodes to (RGBA8 with the game's mips) and its upload bytes, as the cache counts them
	uint64_t decoded_bytes(uint32_t w, uint32_t h)
	{
		uint64_t n = 0;
		for (uint32_t l = 0; l < hifi::chars::tex::mip_count(w, h); ++l)
			n += uint64_t(std::max(w >> l, 1u)) * std::max(h >> l, 1u) * 4;
		return n;
	}
	uint64_t upload_bytes(uint32_t w, uint32_t h)
	{
		uint64_t n = 0;
		for (uint32_t l = 0; l < hifi::chars::tex::mip_count(w, h); ++l)
		{
			const uint64_t row = (uint64_t(std::max(w >> l, 1u)) * 4 + 255) & ~uint64_t(255);
			n += (row * std::max(h >> l, 1u) + 511) & ~uint64_t(511);
		}
		return n;
	}

	uint32_t next_key = 0x10000000u;
	cache::key key_of()
	{
		cache::key k;
		k.resource = next_key;
		next_key += 16;
		return k;
	}

	// The Mithra's first mesh chunk (kind 0x2A) from the game's DAT
	std::vector<uint8_t> first_mesh()
	{
		std::ifstream f("C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI\\ROM2\\0\\52.DAT", std::ios::binary);
		std::vector<uint8_t> dat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		for (size_t off = 0; off + 16 <= dat.size();)
		{
			uint32_t w;
			std::memcpy(&w, dat.data() + off + 4, 4);
			const size_t size = size_t((w >> 7) & 0x7FFFF) * 16;
			if (size < 16 || off + size > dat.size())
				break;
			if ((w & 0x7F) == 0x2A)
				return std::vector<uint8_t>(dat.begin() + off + 16, dat.begin() + off + size);
			off += size;
		}
		return {};
	}

	// Until the GPU is past every signal of the retire list's fence (1 s at most)
	void gpu_past_signals()
	{
		for (int k = 0; k < 100; ++k)
		{
			const hifi::retire::counts r = hifi::retire::status();
			if (r.completed >= r.signalled)
				break;
			Sleep(10);
		}
	}
	// Presents, the GPU idle between them, until the pairs let go are back (at most 40)
	int presents_until_back(ID3D12CommandQueue *queue)
	{
		for (int i = 1; i <= 40; ++i)
		{
			hifi::retire::on_present(queue);
			if (cache::get_counts().slots_retiring == 0)
				return i;
			gpu_past_signals();
		}
		return -1;
	}
	// Presents, the GPU idle between them, until the retire list holds nothing (its closures run: at most 40)
	int presents_until_none_held(ID3D12CommandQueue *queue)
	{
		for (int i = 1; i <= 40; ++i)
		{
			hifi::retire::on_present(queue);
			if (hifi::retire::status().held == 0)
				return i;
			gpu_past_signals();
		}
		return -1;
	}

	// ---- The texture pack's part (docs/texture-pack/characters.md)

	namespace pk = hifi::pack;
	namespace st = hifi::pack::stream;
	namespace cp = hifi::chars::charpack;

	// A DXT1 chunk with its blocks and a name of its own (i): a key of its own
	std::vector<uint8_t> named(uint32_t w, uint32_t h, uint32_t i)
	{
		std::vector<uint8_t> c = chunk(w, h, true);
		char name[17];
		std::snprintf(name, sizeof(name), "cc pack %08u", i);
		std::memcpy(c.data() + 1, name, 16);
		return c;
	}

	void remove_tree(const std::wstring &folder)
	{
		WIN32_FIND_DATAW f;
		HANDLE h = FindFirstFileW((folder + L"*").c_str(), &f);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
				if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
					DeleteFileW((folder + f.cFileName).c_str());
			while (FindNextFileW(h, &f));
			FindClose(h);
		}
		RemoveDirectoryW(folder.substr(0, folder.size() - 1).c_str());
	}

	// A made-up pack: for each of `keys` named(64, 64, i) a 4x record (256 x 256 BC2, its chain to 4 x 4, flat grey)
	bool make_pack(const std::wstring &folder, uint32_t keys, std::string &why)
	{
		remove_tree(folder);
		pk::writer w;
		pk::writer_options o;
		o.folder = folder;
		o.source = "charcache_test";
		if (!w.open(o, why))
			return false;
		for (uint32_t i = 0; i < keys; ++i)
		{
			const std::vector<uint8_t> c = named(64, 64, i);
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

	ID3D12Device *g_device = nullptr;
	ID3D12Device *device_of() { return g_device; }

	// The module's present as the plugin makes it, then backend::present's retire::on_present, the GPU idle after
	void present_pack(ID3D12CommandQueue *queue, double &now)
	{
		now += 1.0 / 60.0;
		cache::frame_tick(g_device, now);
		st::on_present();
		cp::frame(now, st::epoch());
		hifi::retire::on_present(queue);
		gpu_past_signals();
	}
	// Presents until every texture asked has come back one way or the other (at most n)
	bool presents_until_settled(ID3D12CommandQueue *queue, double &now, int n = 400)
	{
		for (int i = 0; i < n; ++i)
		{
			present_pack(queue, now);
			const cache::counts c = cache::get_counts();
			if (c.pack_asked == 0 && i > 2)
				return true;
			Sleep(2);
		}
		return false;
	}
}

int main()
{
	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter *warp = nullptr;
	ID3D12Device *device = nullptr;
	ID3D12CommandQueue *queue = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
		FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) || FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
	{
		check(false, "a WARP device");
		return 1;
	}
	const uint64_t entry = decoded_bytes(1024, 1024);

	// 2,000 queued with no device: one batch decoded at most
	cache::set_detail(false);
	cache::frame_tick(nullptr, 1.0);
	for (int i = 0; i < 2000; ++i)
		cache::add_texture(key_of(), chunk(1024, 1024));
	check(cache::settle(120000), "2,000 1024 x 1024 textures queued with no device: settled");
	cache::counts c = cache::get_counts();
	check(c.textures == 2000 && c.textures_failed == 0 && c.textures_ready == 0, "  none failed, none on the GPU (" + std::to_string(c.textures) + " known)");
	check(c.decoded_bytes > 0 && c.decoded_peak <= cache::kBatchBytes + entry && c.decoded_bytes <= c.decoded_peak,
		"  decoded now " + mb(c.decoded_bytes) + ", at the most " + mb(c.decoded_peak) + " (" + std::to_string(c.decoded_peak / entry) + " of them; a batch is " +
			mb(cache::kBatchBytes) + ")");
	cache::frame_tick(nullptr, 1.0 + cache::kForgetSeconds + 1.0);
	c = cache::get_counts();
	check(c.textures == 0 && c.decoded_bytes == 0 && c.forgotten == 2000, "  let go once not named, the queued ones too (" + std::to_string(c.forgotten) + ")");

	// One held back goes first: small ones coming after it (ids freed last, so ahead of it in the list) aren't decoded
	// beside what's held, it's decoded once that's gone
	cache::frame_tick(nullptr, 23.0);
	for (int i = 0; i < 8; ++i)
		cache::add_texture(key_of(), chunk(1024, 1024));
	cache::settle(60000);
	cache::frame_tick(nullptr, 30.0);
	cache::add_texture(key_of(), chunk(2048, 2048));
	cache::settle(60000);
	for (int i = 0; i < 4; ++i)
		cache::add_texture(key_of(), chunk(512, 512));
	check(cache::settle(60000), "8 1024 x 1024 decoded with no device, then a 2048 x 2048 held back, then 4 512 x 512: settled");
	c = cache::get_counts();
	check(c.decoded_bytes == 8 * entry, "  nothing decoded while it waits: " + mb(c.decoded_bytes) + " (the 8 are " + mb(8 * entry) + ")");
	cache::frame_tick(nullptr, 23.0 + cache::kForgetSeconds + 1.0);
	check(cache::settle(60000), "  the 8 let go: settled");
	c = cache::get_counts();
	const uint64_t then_all = decoded_bytes(2048, 2048) + 4 * decoded_bytes(512, 512);
	check(c.textures == 5 && c.decoded_bytes == then_all, "  then it and the small ones: " + mb(c.decoded_bytes) + " (" + mb(then_all) + ")");
	cache::frame_tick(nullptr, 30.0 + cache::kForgetSeconds + 1.0);
	check(cache::get_counts().textures == 0, "  let go");

	// On WARP: batches by their upload bytes, the detail maps off
	double now = 60.0;
	cache::frame_tick(device, now);
	const uint32_t batches0 = c.batches;
	for (int i = 0; i < 24; ++i)
		cache::add_texture(key_of(), chunk(1024, 1024));
	check(cache::settle(120000), "24 more on WARP: settled");
	c = cache::get_counts();
	check(c.textures_ready == 24 && c.textures_failed == 0, "  all on the GPU: " + cache::status()[0] + (c.last_failure.empty() ? "" : " / " + c.last_failure));
	check(c.batches - batches0 >= 3 && c.batch_bytes_max <= cache::kBatchBytes, "  in " + std::to_string(c.batches - batches0) + " batches, the largest " + mb(c.batch_bytes_max));
	check(c.decoded_peak <= cache::kBatchBytes + entry, "  decoded at the most " + mb(c.decoded_peak));
	check(c.detail_textures == 0, "  detail maps off: none made (" + std::to_string(c.detail_textures) + ")");
	check(c.slots_free == kAllPairs - 24 && c.slots_retiring == 0, "  " + std::to_string(c.slots_free) + " pairs of descriptors free");

	// One bigger than a batch: alone
	const uint32_t batches1 = c.batches;
	const uint32_t big = cache::add_texture(key_of(), chunk(4096, 4096));
	for (int i = 0; i < 4; ++i)
		cache::add_texture(key_of(), chunk(256, 256));
	check(cache::settle(120000), "a 4096 x 4096 (" + mb(upload_bytes(4096, 4096)) + " to upload) with 4 small ones: settled");
	c = cache::get_counts();
	D3D12_GPU_DESCRIPTOR_HANDLE view = {};
	check(cache::texture_view_of(big, view) && c.textures_ready == 29, "  all on the GPU");
	check(c.batches - batches1 >= 2 && c.batch_bytes_max == upload_bytes(4096, 4096),
		"  in a batch of its own: " + std::to_string(c.batches - batches1) + " batches, the largest " + mb(c.batch_bytes_max) + " (it alone)");
	check(c.decoded_peak == decoded_bytes(4096, 4096), "  decoded at the most " + mb(c.decoded_peak) + ": it alone (" + mb(decoded_bytes(4096, 4096)) + ")");

	// Detail maps on; one they'd take over the cap goes without; one that can't be read
	cache::set_detail(true);
	const uint32_t little = cache::add_texture(key_of(), chunk(256, 256, true));
	const bool little_up = cache::settle(60000);
	check(little_up && cache::get_counts().detail_textures == 1, "detail maps on: a small one's made (" + std::to_string(cache::get_counts().detail_textures) + ")");
	const uint64_t with_work = 2 * decoded_bytes(2048, 2048) + hifi::chars::tex::detail_work_bytes(2048, 2048);
	const uint32_t wide = cache::add_texture(key_of(), chunk(2048, 2048));
	check(cache::settle(60000), "a 2048 x 2048 with its detail map and make_detail's working images (" + mb(with_work) + ", over " + mb(cache::kEntryCap) + "): settled");
	c = cache::get_counts();
	check(cache::texture_view_of(wide, view) && c.detail_textures == 1 && c.detail_skipped == 1 && c.too_big == 0 && c.textures_failed == 0,
		"  on the GPU without one (" + std::to_string(c.detail_skipped) + " without, " + std::to_string(c.detail_textures) + " with)");
	const uint32_t bad = cache::add_texture(key_of(), chunk(0, 0));
	check(cache::settle(60000), "a texture that can't be read (0 x 0): settled");
	c = cache::get_counts();
	check(c.textures_failed == 1 && c.detail_skipped == 1, "  failed: " + c.last_failure);
	const std::vector<uint8_t> mesh_bytes = first_mesh();
	check(!mesh_bytes.empty(), "the Mithra's first mesh from ROM2/0/52.DAT (" + std::to_string(mesh_bytes.size()) + " bytes)");
	const uint32_t left = cache::add_mesh(key_of(), std::vector<uint8_t>(mesh_bytes), 0, { bad });
	const uint32_t drawn = cache::add_mesh(key_of(), std::vector<uint8_t>(mesh_bytes), 0, { little, cache::kNone });
	check(cache::settle(60000), "  two meshes, one naming it, one a ready texture and none: settled");
	c = cache::get_counts();
	check(c.meshes_ready == 2 && !cache::mesh_ready(left) && cache::mesh_ready(drawn) && c.meshes_left_failed == 1,
		"  the one naming it left to the game, the other ready (" + std::to_string(c.meshes_left_failed) + " left)");
	check(cache::status().size() >= 2, "status: " + cache::status().back());

	// The pairs let go behind the GPU
	const uint32_t used = kAllPairs - c.slots_free;
	now += cache::kForgetSeconds + 1.0;
	cache::frame_tick(device, now);
	c = cache::get_counts();
	check(c.textures == 0 && c.meshes == 0 && c.slots_retiring == used && c.slots_free == kAllPairs - used,
		"everything let go: " + std::to_string(c.slots_retiring) + " pairs waiting for the GPU, " + std::to_string(c.slots_free) + " free");
	hifi::retire::on_present(queue);
	check(cache::get_counts().slots_retiring == used, "  still waiting after the next present (its signal is the first behind them)");
	const int presents = presents_until_back(queue);
	c = cache::get_counts();
	check(presents > 0 && c.slots_free == kAllPairs && c.slots_retiring == 0, "  all free again " + std::to_string(presents + 1) + " presents on");

	// Let go before stop(): never into the next heap
	for (int i = 0; i < 2; ++i)
		cache::add_texture(key_of(), chunk(64, 64));
	cache::settle(60000);
	now += cache::kForgetSeconds + 1.0;
	cache::frame_tick(device, now);
	check(cache::get_counts().slots_retiring == 2, "two more let go, waiting");
	cache::stop();
	cache::frame_tick(device, now);
	cache::add_texture(key_of(), chunk(64, 64));
	cache::settle(60000);
	// (the last heap's closures run first: until then a pair given back wrongly wouldn't show yet)
	const int runs = presents_until_none_held(queue);
	c = cache::get_counts();
	check(runs > 0 && c.textures_ready == 1 && c.slots_free == kAllPairs - 1 && c.slots_retiring == 0,
		"  stopped, made again, one texture: " + std::to_string(c.slots_free) + " free once the retire list is empty (" + std::to_string(runs) +
			" presents; none of the last heap's given back to this one)");

	// ---- The texture pack's part (docs/texture-pack/characters.md; the streamer on WARP, a made-up pack)
	g_device = device;
	{
		// The key path with no glue (no drop hook, nothing wanted): keys made from the bytes given, nothing asked, every
		// view the game's
		std::vector<uint32_t> plain;
		std::vector<pk::key> plain_keys;
		for (uint32_t i = 0; i < 3; ++i)
		{
			std::vector<uint8_t> b = named(64, 64, 1000 + i);
			plain_keys.push_back(pk::key_of(b.data(), b.size()));
			plain.push_back(cache::add_texture(key_of(), std::move(b)));
		}
		cache::settle(60000);
		bool keyed = true, game_views = true;
		for (size_t i = 0; i < plain.size(); ++i)
		{
			keyed = keyed && plain_keys[i].bytes != 0 && cache::test::pack_key_of(plain[i]) == plain_keys[i] && std::strcmp(cache::test::pack_state_of(plain[i]), "none") == 0;
			bool is_pack = true;
			D3D12_GPU_DESCRIPTOR_HANDLE h = {};
			game_views = game_views && cache::texture_view_of(plain[i], h, true, &is_pack) && !is_pack;
		}
		std::vector<cache::pack_ask> asks;
		cache::take_pack_asks(64, asks);
		c = cache::get_counts();
		check(keyed && game_views && asks.empty() && c.pack_keyed == 4 && c.pack_ready == 0 && c.pack_pairs == 0,
			"the texture pack's keys with no glue: each texture keyed from its bytes (pack::key_of), none asked, every view the game's (" +
				std::to_string(c.pack_keyed) + " keyed)");
	}
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	const std::wstring build = std::wstring(exe).substr(0, std::wstring(exe).find_last_of(L'\\') + 1);
	CreateDirectoryW((build + L"test").c_str(), nullptr);
	CreateDirectoryW((build + L"test\\charcache").c_str(), nullptr);
	const std::wstring pack_folder = build + L"test\\charcache\\hifixi-pack\\";
	constexpr uint32_t kKeys = 160;
	{
		std::string why;
		const bool made = make_pack(pack_folder, kKeys, why);
		const bool started = st::start(device);
		const uint64_t posted = st::open(pack_folder);
		for (int i = 0; i < 2000 && st::posts_done() < posted; ++i)
			Sleep(5);
		check(made && started && st::current() != nullptr, "a made-up pack of " + std::to_string(kKeys) + " (64 x 64 textures' 4x records, 256 x 256 BC2), the streamer on WARP: opened" +
			(why.empty() ? std::string() : ": " + why));
	}
	cp::sources sources;
	sources.device = device_of;
	cp::set_sources(sources);
	{
		pk::modes m;
		m.chars = true;
		cp::set_mode(m);
	}
	const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	std::vector<uint32_t> hits;
	std::vector<cache::key> hit_keys; // (named again by the eviction's long waits: never let go as unused)
	uint32_t miss = cache::kNone;
	{
		// 64 in the pack and one not: the pack's pairs for the 64, ready, BC2 with the record's levels down to the game's
		// last (the chain stops at 4 x 4: 7 of the 8 that would reach 2 x 2); the other spent, its game pair kept
		for (uint32_t i = 0; i < 64; ++i)
		{
			hit_keys.push_back(key_of());
			hits.push_back(cache::add_texture(hit_keys.back(), named(64, 64, i)));
		}
		miss = cache::add_texture(key_of(), named(64, 64, 5000));
		cache::settle(60000);
		const bool settled = presents_until_settled(queue, now);
		c = cache::get_counts();
		bool states = std::strcmp(cache::test::pack_state_of(miss), "spent") == 0, views = true, levels = true;
		const D3D12_GPU_DESCRIPTOR_HANDLE start = cache::heap()->GetGPUDescriptorHandleForHeapStart();
		std::string level_line;
		for (uint32_t id : hits)
		{
			states = states && std::strcmp(cache::test::pack_state_of(id), "ready") == 0;
			D3D12_GPU_DESCRIPTOR_HANDLE h = {}, g = {};
			bool is_pack = false, game_is_pack = true;
			views = views && cache::texture_view_of(id, h, true, &is_pack) && is_pack && h.ptr == start.ptr + uint64_t(2 * cache::test::pack_slot_of(id)) * step &&
				cache::texture_view_of(id, g, false, &game_is_pack) && !game_is_pack && g.ptr == start.ptr + uint64_t(2 * cache::test::game_slot_of(id)) * step;
			st::view v;
			const bool got = st::get(cache::test::pack_ticket_of(id), v);
			levels = levels && got && v.format == DXGI_FORMAT_BC2_UNORM && v.w == 256 && v.h == 256 && v.levels == 7;
			if (level_line.empty() && got)
				level_line = std::to_string(v.w) + " x " + std::to_string(v.h) + " format " + std::to_string(int(v.format)) + ", " + std::to_string(v.levels) + " levels";
		}
		bool miss_game = false, miss_is_pack = true;
		D3D12_GPU_DESCRIPTOR_HANDLE mh = {};
		miss_game = cache::texture_view_of(miss, mh, true, &miss_is_pack) && !miss_is_pack;
		std::string why;
		const bool consistent = cache::test::slots_consistent(why);
		check(settled && states && c.pack_ready == 64 && c.pack_pairs == 64 && c.pack_asked == 0, "64 in the pack ready, one not spent: " + cache::status().back());
		check(views && miss_game, "  texture_view_of: the pack's pair for those (the game's with the pack refused: the casters), the game's for the other");
		check(levels, "  their views' textures: " + level_line + " (BC2, from the record down to the game's last level, at most its chain)");
		check(consistent, "  no pair twice" + (why.empty() ? std::string() : ": " + why));
		std::vector<std::string> lines;
		cp::lines(lines);
		check(lines.size() == 1 && lines[0].rfind("Characters: 64 of " + std::to_string(c.textures_ready) + " textures from the pack (" + std::to_string(c.pack_not_in) + " not in it, 0 coming)", 0) == 0,
			"  the line: " + (lines.empty() ? std::string() : lines[0]));
		// /hifi textures compare: the game's pair handed out, nothing let go
		cp::set_compare(true);
		bool is_pack = true;
		D3D12_GPU_DESCRIPTOR_HANDLE h = {};
		const bool compared = cache::texture_view_of(hits[0], h, true, &is_pack) && !is_pack;
		cp::set_compare(false);
		check(compared && cache::texture_view_of(hits[0], h, true, &is_pack) && is_pack && cache::get_counts().pack_pairs == 64,
			"  compare: the game's pair while it's on, the pack's again after, nothing let go");
	}
	{
		// Two textures with the same bytes (two game resources): two tickets, one texture of the streamer's; one let go, the
		// other's still there
		const st::counts s0 = st::get_counts();
		const cache::key twin_key = key_of();
		const uint32_t twin = cache::add_texture(twin_key, named(64, 64, 7));
		cache::settle(60000);
		presents_until_settled(queue, now);
		const st::counts s1 = st::get_counts();
		st::view a, b;
		const bool both = st::get(cache::test::pack_ticket_of(hits[7]), a) && st::get(cache::test::pack_ticket_of(twin), b) && a.resource == b.resource &&
			!(cache::test::pack_ticket_of(hits[7]) == cache::test::pack_ticket_of(twin));
		check(both && s1.resident == s0.resident && s1.shared == s0.shared + 1 && std::strcmp(cache::test::pack_state_of(twin), "ready") == 0,
			"the same bytes under two game resources: two tickets, one texture (resident " + std::to_string(s1.resident) + ", shared +" + std::to_string(s1.shared - s0.shared) + ")");
		cache::key moved = twin_key;
		moved.handle++;
		cache::texture(moved); // (another resource at its address: the twin let go)
		for (int i = 0; i < 4; ++i)
			present_pack(queue, now);
		st::view a2;
		check(st::get(cache::test::pack_ticket_of(hits[7]), a2) && a2.resource == a.resource && st::get_counts().resident == s0.resident,
			"  one let go: the other still holds the texture");
	}
	{
		// Every pack part let go (the module's clear_all): the game's pairs stay, the pack's behind the GPU
		const uint32_t pairs = cache::get_counts().pack_pairs;
		const uint32_t retiring0 = cache::get_counts().slots_retiring;
		cp::clear_all();
		c = cache::get_counts();
		bool none = true;
		for (uint32_t id : hits)
			none = none && std::strcmp(cache::test::pack_state_of(id), "none") == 0;
		check(none && c.pack_pairs == 0 && c.slots_retiring == retiring0 + pairs && c.textures_ready >= 65 && !cp::get_counts().owner,
			"clear_all: every pack part let go (" + std::to_string(pairs) + " pairs behind the GPU), the game's kept, the owner dropped");
		presents_until_back(queue);
		// Exhaustion: with kPackSlotFloor pairs free (1,024: the rest held by the test) nothing is asked
		cache::test::hold_slots(cache::kPackSlotFloor);
		const uint64_t asked0 = cp::get_counts().asked;
		for (int i = 0; i < 10; ++i)
			present_pack(queue, now);
		c = cache::get_counts();
		check(cp::get_counts().asked == asked0 && c.pack_ready == 0 && c.pack_waiting_slots >= 64 && c.slots_free == cache::kPackSlotFloor,
			"the free pairs down to the floor (" + std::to_string(c.slots_free) + "): nothing asked, " + std::to_string(c.pack_waiting_slots) + " waiting");
		std::vector<std::string> lines;
		cp::lines(lines);
		check(!lines.empty() && lines[0].find("skipped (the characters' descriptors are nearly all in use)") != std::string::npos, "  the line says so: " + lines[0]);
		// Five pairs over the floor: five asked (not the 64 a present may), each given a pair once ready, none read and let
		// go for want of one; nothing more asked while the five are on their way or the floor is reached
		cache::test::give_back_slots();
		cache::test::hold_slots(cache::kPackSlotFloor + 5);
		const uint64_t skipped5 = cache::get_counts().pack_skipped_slots;
		present_pack(queue, now);
		const uint64_t asked5 = cp::get_counts().asked - asked0;
		// (a few of the textures first on the list aren't in the pack: their misses free their share for the next)
		for (int i = 0; i < 300 && (cache::get_counts().pack_asked != 0 || cache::get_counts().slots_free > cache::kPackSlotFloor); ++i)
		{
			present_pack(queue, now);
			Sleep(2);
		}
		const uint64_t asked_floor = cp::get_counts().asked - asked0;
		for (int i = 0; i < 5; ++i)
			present_pack(queue, now);
		c = cache::get_counts();
		check(asked5 == 5 && c.pack_skipped_slots == skipped5 && c.pack_ready == 5 && c.slots_free == cache::kPackSlotFloor && cp::get_counts().asked - asked0 == asked_floor,
			"  five pairs over the floor: " + std::to_string(asked5) + " asked in the first present (" + std::to_string(asked_floor) + " in all, the misses' share asked "
				"again), " + std::to_string(c.pack_ready) + " ready, " + std::to_string(c.pack_skipped_slots - skipped5) + " let go for want of a pair, none asked at the floor");
		cp::clear_all();
		presents_until_back(queue);
		cache::test::give_back_slots();
		cache::test::hold_slots(cache::kPackSlotFloor);
		// Asked above it, then the floor reached before they're ready: each ready ticket let go for want of a pair, the
		// texture asked again once there are pairs
		cache::test::give_back_slots();
		present_pack(queue, now);
		const uint64_t asked1 = cp::get_counts().asked;
		cache::test::hold_slots(cache::kPackSlotFloor);
		for (int i = 0; i < 300 && cache::get_counts().pack_asked != 0; ++i)
		{
			present_pack(queue, now);
			Sleep(2);
		}
		c = cache::get_counts();
		check(asked1 > asked0 && c.pack_skipped_slots > 0 && c.pack_ready + c.pack_skipped_slots + c.pack_not_in >= 64 && c.slots_free == cache::kPackSlotFloor,
			"  asked above the floor, then the floor reached: " + std::to_string(c.pack_skipped_slots) + " ready tickets let go for want of a pair (" +
				std::to_string(c.pack_ready) + " got one first)");
		cache::test::give_back_slots();
		const bool again = presents_until_settled(queue, now);
		c = cache::get_counts();
		std::string why;
		check(again && c.pack_ready == 64 && cache::test::slots_consistent(why), "  pairs again: asked again, all ready (" + std::to_string(c.pack_ready) + ")" +
			(why.empty() ? std::string() : ": " + why));
	}
	{
		// 1,000 textures let go while upgrading: a third with the ticket asked, a third between pack_begin and pack_end
		// (the views written after), a third once ready. No pair is ever both free and in use, or in use twice; none let
		// go is free before the fence has passed it; all of them back once the retire list has passed them
		pk::modes off;
		cp::set_mode(off); // (the glue out of the way: the test drives the cache's calls itself)
		cache::set_pack_drop(&st::drop);
		cache::set_pack_on(true);
		const st::owner o = st::add_owner("charcache_test");
		uint32_t forgotten = 0, rounds = 0, begun = 0, early = 0;
		bool consistent = true, back = true;
		std::string why;
		while (forgotten < 1000 && rounds < 40)
		{
			rounds++;
			std::vector<std::pair<uint32_t, cache::key>> made;
			for (uint32_t i = 0; i < 99; ++i)
			{
				const cache::key k = key_of();
				made.emplace_back(cache::add_texture(k, named(64, 64, (rounds * 99 + i) % kKeys)), k);
			}
			cache::settle(60000);
			now += 0.01;
			cache::frame_tick(device, now);
			std::vector<cache::pack_ask> asks;
			cache::take_pack_asks(200, asks);
			for (const cache::pack_ask &a : asks)
			{
				// (only this round's: the textures from before stay as they are)
				if (std::none_of(made.begin(), made.end(), [&](const auto &m) { return m.first == a.id; }))
					continue;
				st::want w;
				w.k = a.k;
				w.down_to_w = a.down_to_w;
				w.down_to_h = a.down_to_h;
				w.epoch = st::epoch();
				const st::ticket t = st::request(o, w);
				if (!cache::pack_asked(a.id, a.serial, t, w.epoch))
					st::drop(t);
			}
			auto forget = [&](const cache::key &k) {
				cache::key moved = k;
				moved.handle++;
				cache::texture(moved);
				forgotten++;
			};
			std::vector<uint32_t> let_go;
			for (size_t j = 0; j < made.size(); j += 3)
				forget(made[j].second); // (asked: its ticket dropped wherever it is)
			// The rest's tickets ready
			for (int i = 0; i < 2000; ++i)
			{
				bool all = true;
				for (size_t j = 0; j < made.size() && all; ++j)
					if (j % 3 != 0)
						all = st::poll(cache::test::pack_ticket_of(made[j].first)) == st::state::ready;
				if (all)
					break;
				st::on_present();
				Sleep(1);
			}
			for (size_t j = 0; j < made.size(); ++j)
			{
				if (j % 3 == 0)
					continue;
				const st::ticket t = cache::test::pack_ticket_of(made[j].first);
				cache::pack_writing w;
				if (!cache::pack_begin(t, w))
					continue;
				begun++;
				let_go.push_back(w.slot);
				if (j % 3 == 1)
					forget(made[j].second); // (between pack_begin and pack_end: the pair let go before it's even written)
				st::view v;
				const bool ok = st::get(t, v) && st::write_view(t, w.colour);
				cache::write_pack_detail(w);
				cache::pack_end(w, ok, v.bytes);
				if (j % 3 == 2)
					forget(made[j].second); // (ready)
			}
			for (uint32_t s : let_go)
				early += cache::test::slot_free(s);
			std::string w;
			if (!cache::test::slots_consistent(w))
			{
				consistent = false;
				if (why.empty())
					why = w;
			}
			// (what take_done hands over now is no one's: let go already)
			std::vector<std::pair<st::ticket, st::state>> done;
			st::take_done(o, done);
			for (const auto &[t, s] : done)
			{
				cache::pack_writing w2;
				if (s == st::state::ready && cache::pack_begin(t, w2))
				{
					consistent = false;
					why = "a ticket let go was still the cache's";
				}
			}
			hifi::retire::on_present(queue);
			for (uint32_t s : let_go)
				early += cache::test::slot_free(s); // (one present on: still not past the fence's +2)
			presents_until_back(queue);
			for (uint32_t s : let_go)
				back = back && cache::test::slot_free(s);
		}
		cache::forget_pack_textures(); // (the test's tickets let go, as the module's clear_all would)
		st::drop_owner(o);
		presents_until_back(queue);
		c = cache::get_counts();
		std::string w;
		check(forgotten >= 1000 && consistent && cache::test::slots_consistent(w), std::to_string(forgotten) + " textures let go while upgrading (" + std::to_string(begun) +
			" pairs taken, half of them let go before they were written): no pair twice" + (why.empty() ? std::string() : ": " + why));
		check(early == 0 && back && c.slots_retiring == 0 && c.slots_free == kAllPairs - c.textures_ready - c.meshes_ready - c.pack_pairs,
			"  none of their pairs free before the fence (" + std::to_string(early) + "), all back after (" + std::to_string(c.slots_free) + " free, " + std::to_string(c.pack_pairs) +
				" pack pairs in use, " + std::to_string(c.textures_ready) + " textures)");
	}
	{
		// M9: a texture asked with a height whose colour settled ready while the height is still on its way (the streamer
		// holds a height back while its pressure lasts) holds the colour in video memory with nothing drawing it:
		// pack_evict lets it go first, its view bytes counted, both tickets with it, evicted until pack_unevict
		pk::modes off;
		cp::set_mode(off);
		cache::set_pack_drop(&st::drop);
		cache::set_pack_on(true);
		const st::owner o = st::add_owner("charcache_test held");
		const cache::key k = key_of();
		const uint32_t id = cache::add_texture(k, named(64, 64, 150));
		cache::settle(60000);
		now += 0.01;
		cache::frame_tick(device, now);
		std::vector<cache::pack_ask> asks;
		cache::take_pack_asks(200, asks);
		st::ticket t, h;
		for (const cache::pack_ask &a : asks)
		{
			if (a.id != id)
				continue;
			st::want w;
			w.k = a.k;
			w.down_to_w = a.down_to_w;
			w.down_to_h = a.down_to_h;
			w.epoch = st::epoch();
			t = st::request(o, w);
			// (the height's stand-in: another key's ticket, never settled, so the height stays on its way)
			st::want hw = w;
			hw.k = cache::test::pack_key_of(hits[8]);
			h = st::request(o, hw);
			if (!cache::pack_asked(a.id, a.serial, t, w.epoch, h))
			{
				st::drop(t);
				st::drop(h);
				t = h = st::ticket {};
			}
		}
		for (int i = 0; i < 2000 && t.generation != 0 && st::poll(t) != st::state::ready; ++i)
		{
			st::on_present();
			Sleep(1);
		}
		st::view v;
		const bool got = t.generation != 0 && st::get(t, v) && v.bytes != 0;
		cache::pack_writing w;
		const cache::pack_step settled = got ? cache::pack_settle(t, true, cache::pack_miss::failed, 0.0f, w, v.bytes) : cache::pack_step::none;
		const bool held = settled == cache::pack_step::wait && std::strcmp(cache::test::pack_state_of(id), "asked") == 0 && cache::get_counts().pack_heights_asked == 1;
		uint64_t bytes = 0;
		const uint32_t n = cache::pack_evict(1, bytes);
		const bool evicted = n == 1 && bytes == v.bytes && std::strcmp(cache::test::pack_state_of(id), "evicted") == 0 &&
			cache::test::pack_ticket_of(id).generation == 0 && cache::test::pack_height_ticket_of(id).generation == 0 &&
			cache::get_counts().pack_heights_asked == 0 && cache::get_counts().pack_asked == 0;
		uint32_t still = 0;
		const uint32_t back = cache::pack_unevict(1, still);
		const bool none_again = back == 1 && still == 0 && std::strcmp(cache::test::pack_state_of(id), "none") == 0;
		check(got && held && evicted && none_again, "M9: a colour held ready while its height is on its way: let go first under pressure (" + std::to_string(n) +
			" evicted, " + mb(bytes) + " against its view's " + mb(v.bytes) + "), both tickets with it, none again after pack_unevict");
		cache::key moved = k;
		moved.handle++;
		cache::texture(moved); // (forgotten: the eviction test below has the 64 alone)
		cache::forget_pack_textures();
		st::drop_owner(o);
		presents_until_back(queue);
	}
	{
		// Eviction under the streamer's pressure (plan section 5, characters.md "Eviction under pressure"): the 64 drawn one
		// after another and then the first three again, the video memory over 95% (set_limits): each half second the ones
		// drawn longest ago back to the game's (not the first ready: those were drawn last), up to the round's bytes (three
		// textures' worth here); a new texture meanwhile not asked; held while the pressure's been gone under the hold, then
		// asked again a round at a time, the ones drawn last first; the pressure back just after: the next hold twice as
		// long, and 10 s again once it's stayed away kBackoffWindowS
		pk::modes on;
		on.chars = true;
		cp::set_mode(on);
		presents_until_settled(queue, now);
		cache::key late_key = key_of();
		const uint32_t ready0 = cache::get_counts().pack_ready;
		st::view v0;
		st::get(cache::test::pack_ticket_of(hits[0]), v0);
		cp::test::set_evict_bytes(3 * v0.bytes);
		// (hits[i] drawn at now + 1 + i / 100, then hits[0..2] again at now + 1.9: hits[3] the longest ago, though hits[0]
		// was ready first and asked first)
		auto draw = [&](uint32_t i, double at) {
			cache::pack_tick(at);
			D3D12_GPU_DESCRIPTOR_HANDLE h = {};
			bool is_pack = false;
			cache::texture_view_of(hits[i], h, true, &is_pack);
		};
		for (uint32_t i = 0; i < hits.size(); ++i)
			draw(i, now + 1.0 + i * 0.01);
		for (uint32_t i = 0; i < 3; ++i)
			draw(i, now + 1.9);
		now += 2.0;
		// (seconds on, a present at most each 5 s with every texture named again: the cache lets go of one unnamed for
		// kForgetSeconds)
		auto wait = [&](double seconds) {
			const double until = now + seconds;
			while (now < until)
			{
				now = std::min(until, now + 5.0);
				present_pack(queue, now);
				for (const cache::key &k : hit_keys)
					cache::texture(k);
				cache::texture(late_key);
			}
		};
		auto in_state = [&](uint32_t from, uint32_t to, const char *s) {
			bool all = true;
			for (uint32_t i = from; i < to; ++i)
				all = all && std::strcmp(cache::test::pack_state_of(hits[i]), s) == 0;
			return all;
		};
		st::limits full;
		full.vram_usage = 99ull << 20;
		full.vram_budget = 100ull << 20;
		st::set_limits(full);
		const uint64_t asked0 = cp::get_counts().asked;
		present_pack(queue, now);
		c = cache::get_counts();
		check(st::pressure() && ready0 == 64 && c.pack_evicted == 3 && in_state(3, 6, "evicted") && in_state(0, 3, "ready") && in_state(6, 7, "ready") &&
			cp::get_counts().evict_rounds == 1, "the video memory over 95%: the 3 drawn longest ago back to the game's in the first round, the 3 ready first but drawn "
				"since kept (" + std::to_string(c.pack_evicted) + " evicted, the round's bytes " + mb(3 * v0.bytes) + ")");
		bool game_view = true;
		for (uint32_t i = 3; i < 6; ++i)
		{
			D3D12_GPU_DESCRIPTOR_HANDLE h = {};
			bool is_pack = true;
			game_view = game_view && cache::texture_view_of(hits[i], h, true, &is_pack) && !is_pack;
		}
		// (a new character's texture while it holds, in the pack: not asked)
		late_key = key_of();
		const uint32_t late = cache::add_texture(late_key, named(64, 64, 100));
		cache::settle(60000);
		for (int i = 0; i < 12; ++i)
			present_pack(queue, now); // (0.2 s: no second round yet)
		const uint32_t after_quarter = cache::get_counts().pack_evicted;
		for (int i = 0; i < 20; ++i)
			present_pack(queue, now); // (past the half second)
		c = cache::get_counts();
		check(game_view && after_quarter == 3 && c.pack_evicted == 6 && in_state(6, 9, "evicted") && in_state(0, 3, "ready") && cp::get_counts().evict_rounds == 2,
			"  their views the game's; the next 3 half a second on, not before (" + std::to_string(c.pack_evicted) + " evicted)");
		check(cp::get_counts().asked == asked0 && std::strcmp(cache::test::pack_state_of(late), "none") == 0,
			"  nothing asked meanwhile, a texture new since in the pack among it (" + std::string(cache::test::pack_state_of(late)) + ")");
		std::vector<std::string> lines;
		cp::lines(lines);
		check(!lines.empty() && lines[0].find("6 back to the game's while the graphics card is nearly full") != std::string::npos, "  the line: " + (lines.empty() ? std::string() : lines[0]));
		std::string why;
		check(c.pack_pairs == ready0 - 6 && cache::test::slots_consistent(why), "  their pairs let go (" + std::to_string(c.pack_pairs) + " in use)" + (why.empty() ? std::string() : ": " + why));
		// The pressure gone: the new one asked at once, the evicted held for kEvictHoldS, then asked again a round each half
		// second, the ones drawn last first
		st::set_limits(st::limits {});
		present_pack(queue, now);
		const bool late_asked = cp::get_counts().asked == asked0 + 1 && std::strcmp(cache::test::pack_state_of(late), "none") != 0;
		wait(cp::kEvictHoldS - 1.0);
		c = cache::get_counts();
		const bool held = !st::pressure() && c.pack_evicted == 6 && cp::get_counts().asked == asked0 + 1;
		wait(1.5);
		const bool first_round = cache::get_counts().pack_evicted == 3 && in_state(3, 6, "evicted") && cp::get_counts().asked == asked0 + 4;
		// (until none is evicted and none coming: the rounds are half a second apart)
		auto until_back = [&]() {
			for (int i = 0; i < 600; ++i)
			{
				present_pack(queue, now);
				const cache::counts k = cache::get_counts();
				if (k.pack_asked == 0 && k.pack_evicted == 0)
					return true;
				Sleep(1);
			}
			return false;
		};
		const bool back = until_back();
		c = cache::get_counts();
		check(late_asked && held && first_round && back && c.pack_evicted == 0 && c.pack_ready == ready0 + 1 && cp::get_counts().asked == asked0 + 7,
			"  the pressure gone: the new one asked at once; the evicted still the game's " + std::to_string(int(cp::kEvictHoldS - 1.0)) + " s on, after " +
				std::to_string(int(cp::kEvictHoldS)) + " s the 3 drawn last asked first, the rest half a second on, all ready (" + std::to_string(c.pack_ready) + ")");
		// The pressure back just after: they don't fit, the next hold 20 s; 10 s again once it's stayed away kBackoffWindowS
		now += 1.0;
		st::set_limits(full);
		present_pack(queue, now);
		const uint32_t again = cache::get_counts().pack_evicted;
		const double hold = cp::get_counts().hold_s;
		st::set_limits(st::limits {});
		present_pack(queue, now);
		wait(2.0 * cp::kEvictHoldS - 1.0);
		const bool held_longer = cache::get_counts().pack_evicted == again;
		wait(1.5);
		const bool back_again = until_back();
		check(again == 3 && hold == 2.0 * cp::kEvictHoldS && held_longer && back_again,
			"  the pressure back 1 s after: " + std::to_string(again) + " evicted, held " + std::to_string(int(hold)) + " s (still the game's " +
				std::to_string(int(2.0 * cp::kEvictHoldS - 1.0)) + " s on: " + (held_longer ? "yes" : "no") + "), then back (" + (back_again ? "yes" : "no") + ", pressure " + (st::pressure() ? "on" : "off") + ", " +
				std::to_string(cache::get_counts().pack_evicted) + " evicted, " + std::to_string(cache::get_counts().pack_asked) + " asked)");
		wait(cp::kBackoffWindowS + 1.0);
		check(cp::get_counts().hold_s == cp::kEvictHoldS, "  the pressure away " + std::to_string(int(cp::kBackoffWindowS)) + " s: the hold " +
			std::to_string(int(cp::get_counts().hold_s)) + " s again");
		cp::test::set_evict_bytes(0);
	}
	{
		// stop(): every ticket to the drop hook, the pack pairs of the last heap never into the next one
		pk::modes on;
		on.chars = true;
		cp::set_mode(on);
		presents_until_settled(queue, now);
		const uint32_t held = cache::get_counts().pack_pairs;
		const uint64_t resident = st::get_counts().resident;
		cache::stop();
		check(held != 0 && resident != 0 && st::get_counts().resident == 0, "cache::stop() with " + std::to_string(held) + " pack pairs: every ticket to the drop hook, the streamer's " +
			std::to_string(resident) + " textures let go");
		cache::frame_tick(device, now);
		cache::add_texture(key_of(), chunk(64, 64));
		cache::settle(60000);
		const int remade = presents_until_none_held(queue);
		c = cache::get_counts();
		check(remade > 0 && c.slots_free == kAllPairs - 1 && st::get_counts().resident == 0, "  made again: " + std::to_string(c.slots_free) +
			" free (none of the last heap's pack pairs given back to this one), the streamer's textures all let go");
	}
	cp::clear_all();
	st::stop();

	cache::stop();
	hifi::retire::release_all();
	queue->Release();
	device->Release();
	warp->Release();
	factory->Release();
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures;
}
