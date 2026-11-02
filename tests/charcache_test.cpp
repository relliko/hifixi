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
 *     empty, so the last heap's closures have run).
 *
 *   build\charcache_test.exe     (built by build.cmd)
 */

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "../src/chars/cache.hpp"
#include "../src/chars/tex.hpp"
#include "../src/host/retire.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
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
