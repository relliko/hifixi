/*
 * The DAT access the bakers and the texture pack share (dat.hpp): the game's folder, file ids to paths, XIPivot's
 * overlays, chunks, XPRESS, whole-file writes. No game:: code, so the 64-bit texpack links it too; key_tables (found
 * in the running game's image) is in dat.cpp, and texpack has its own (tools\texpack\keys.cpp).
 */

#include <windows.h>
#include "dat.hpp"
#include <cstdio>
#include <cwchar>
#include <algorithm>
#include <cstring>

namespace hifi::bake
{
	namespace
	{
		std::wstring g_ffxi;

		// The game's folder from the running game: FFXiMain.dll's own (FINAL FANTASY XI\FFXiMain.dll)
		std::wstring game_folder()
		{
			wchar_t path[MAX_PATH] = L"";
			if (HMODULE m = GetModuleHandleW(L"FFXiMain.dll"); m != nullptr && GetModuleFileNameW(m, path, MAX_PATH) != 0)
			{
				std::wstring p = path;
				const size_t slash = p.find_last_of(L"\\/");
				if (slash != std::wstring::npos)
					return p.substr(0, slash);
			}
			return L"C:\\Games\\PhoenixXI\\SquareEnix\\FINAL FANTASY XI";
		}

		uint16_t u16(const std::vector<uint8_t> &b, size_t at)
		{
			return at + 2 <= b.size() ? uint16_t(b[at] | (b[at + 1] << 8)) : 0;
		}

		typedef LONG(NTAPI *get_size_fn)(USHORT, PULONG, PULONG);
		typedef LONG(NTAPI *compress_fn)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, ULONG, PULONG, PVOID);
		typedef LONG(NTAPI *decompress_fn)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG, PVOID);
		constexpr USHORT kXpressHuff = 4, kMaximum = 0x100;
	}

	const std::wstring &ffxi_folder()
	{
		if (g_ffxi.empty())
			g_ffxi = game_folder();
		return g_ffxi;
	}

	void set_ffxi_folder(const std::wstring &folder)
	{
		g_ffxi = folder;
	}

	bool read_file(const std::wstring &path, std::vector<uint8_t> &out)
	{
		out.clear();
		HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (f == INVALID_HANDLE_VALUE)
			return false;
		LARGE_INTEGER size = {};
		bool ok = GetFileSizeEx(f, &size) && size.QuadPart < 0x40000000;
		if (ok)
		{
			out.resize(size_t(size.QuadPart));
			DWORD got = 0;
			ok = out.empty() || (ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
		}
		CloseHandle(f);
		if (!ok)
			out.clear();
		return ok;
	}

	namespace
	{
		// pivot.ini's root_path and its overlays' names, the lowest N first; false when there's no pivot.ini (HIFIXI_PIVOT_INI's,
		// else config\pivot\pivot.ini in Ashita's folder: this module is in its plugins), no root or no overlay
		bool pivot(std::wstring &root_path, std::vector<std::wstring> &names)
		{
			names.clear();
			wchar_t ini[MAX_PATH] = L"";
			if (GetEnvironmentVariableW(L"HIFIXI_PIVOT_INI", ini, MAX_PATH) == 0)
			{
				HMODULE self = nullptr;
				if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&pivot), &self) ||
					GetModuleFileNameW(self, ini, MAX_PATH) == 0)
					return false;
				std::wstring p = ini;
				for (int up = 0; up < 2; ++up)
					p = p.substr(0, p.find_last_of(L"\\/") == std::wstring::npos ? 0 : p.find_last_of(L"\\/"));
				std::swprintf(ini, MAX_PATH, L"%s\\config\\pivot\\pivot.ini", p.c_str());
			}
			if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES)
				return false;
			wchar_t root[MAX_PATH] = L"";
			GetPrivateProfileStringW(L"settings", L"root_path", L"", root, MAX_PATH, ini);
			std::vector<wchar_t> section(16384);
			const DWORD got = GetPrivateProfileSectionW(L"overlays", section.data(), DWORD(section.size()), ini);
			if (root[0] == 0 || got == 0)
				return false;
			std::vector<std::pair<int, std::wstring>> listed; // (N=name, by N)
			for (const wchar_t *e = section.data(); *e != 0; e += std::wcslen(e) + 1)
			{
				const std::wstring line = e;
				const size_t eq = line.find(L'=');
				if (eq != std::wstring::npos && eq > 0)
					listed.emplace_back(_wtoi(line.substr(0, eq).c_str()), line.substr(eq + 1));
			}
			std::sort(listed.begin(), listed.end());
			root_path = root;
			for (auto &o : listed)
				names.push_back(std::move(o.second));
			return true;
		}
	}

	std::wstring overlaid(const std::wstring &path, const wchar_t *exclude)
	{
		const std::wstring &ffxi = ffxi_folder();
		if (path.size() <= ffxi.size() + 1 || CompareStringOrdinal(path.c_str(), int(ffxi.size()), ffxi.c_str(), int(ffxi.size()), TRUE) != CSTR_EQUAL)
			return path;
		const std::wstring rel = path.substr(ffxi.size() + 1);
		std::wstring root;
		std::vector<std::wstring> names;
		if (!pivot(root, names))
			return path;
		for (const std::wstring &name : names)
		{
			// (the one left out: the texture pack's zones read the original under hifi-textures' copy, zones.md)
			if (exclude != nullptr && CompareStringOrdinal(name.c_str(), -1, exclude, -1, TRUE) == CSTR_EQUAL)
				continue;
			const std::wstring candidate = root + L"\\" + name + L"\\" + rel;
			if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
				return candidate;
		}
		return path;
	}

	std::vector<std::wstring> overlays()
	{
		std::wstring root;
		std::vector<std::wstring> names;
		pivot(root, names);
		return names;
	}

	std::wstring dat_path(uint32_t id, std::string &why)
	{
		const std::wstring &ffxi = ffxi_folder();
		std::vector<uint8_t> vt, ft;
		wchar_t path[MAX_PATH];
		if (read_file(ffxi + L"\\VTABLE.DAT", vt) && id < vt.size() && vt[id] == 1 && read_file(ffxi + L"\\FTABLE.DAT", ft) && (id + 1) * 2 <= ft.size())
		{
			const uint16_t v = u16(ft, id * 2);
			std::swprintf(path, MAX_PATH, L"%s\\ROM\\%u\\%u.DAT", ffxi.c_str(), unsigned(v >> 7), unsigned(v & 0x7F));
			return path;
		}
		for (unsigned n = 2; n < 10; ++n)
		{
			wchar_t v_name[MAX_PATH], f_name[MAX_PATH];
			std::swprintf(v_name, MAX_PATH, L"%s\\ROM%u\\VTABLE%u.DAT", ffxi.c_str(), n, n);
			std::swprintf(f_name, MAX_PATH, L"%s\\ROM%u\\FTABLE%u.DAT", ffxi.c_str(), n, n);
			if (!read_file(v_name, vt) || !read_file(f_name, ft))
				continue;
			if (id < vt.size() && vt[id] == n && (id + 1) * 2 <= ft.size())
			{
				const uint16_t v = u16(ft, id * 2);
				std::swprintf(path, MAX_PATH, L"%s\\ROM%u\\%u\\%u.DAT", ffxi.c_str(), n, unsigned(v >> 7), unsigned(v & 0x7F));
				return path;
			}
		}
		char text[96];
		std::snprintf(text, sizeof(text), "file id %u is in no ROM", id);
		why = text;
		return std::wstring();
	}

	std::vector<chunk> chunks(const std::vector<uint8_t> &dat)
	{
		std::vector<chunk> out;
		size_t off = 0;
		while (off + 16 <= dat.size())
		{
			uint32_t w = 0;
			std::memcpy(&w, &dat[off + 4], 4);
			const size_t size = size_t((w >> 7) & 0x7FFFF) * 16;
			chunk c = {};
			c.type = w & 0x7F;
			std::memcpy(c.name, &dat[off], 4);
			c.body = dat.data() + off + 16;
			c.size = size >= 16 ? std::min(size - 16, dat.size() - off - 16) : 0;
			out.push_back(c);
			if (size < 16)
				break;
			off += size;
		}
		return out;
	}

	bool find_chunk(const std::vector<uint8_t> &dat, uint32_t type, std::vector<uint8_t> &out)
	{
		for (const chunk &c : chunks(dat))
			if (c.type == type)
			{
				out.assign(c.body, c.body + c.size);
				return true;
			}
		return false;
	}

	std::vector<uint8_t> xpress(const uint8_t *data, size_t size)
	{
		HMODULE nt = GetModuleHandleW(L"ntdll.dll");
		const auto get_size = nt != nullptr ? reinterpret_cast<get_size_fn>(GetProcAddress(nt, "RtlGetCompressionWorkSpaceSize")) : nullptr;
		const auto compress = nt != nullptr ? reinterpret_cast<compress_fn>(GetProcAddress(nt, "RtlCompressBuffer")) : nullptr;
		ULONG ws = 0, fs = 0;
		if (get_size == nullptr || compress == nullptr || get_size(kXpressHuff | kMaximum, &ws, &fs) != 0)
			return {};
		std::vector<uint8_t> work(ws), out(size + size / 8 + 4096);
		ULONG final_size = 0;
		if (compress(kXpressHuff | kMaximum, const_cast<PUCHAR>(data), ULONG(size), out.data(), ULONG(out.size()), 4096, &final_size, work.data()) != 0)
			return {};
		out.resize(final_size);
		return out;
	}

	bool unxpress(const uint8_t *data, size_t size, size_t unpacked, std::vector<uint8_t> &out)
	{
		HMODULE nt = GetModuleHandleW(L"ntdll.dll");
		const auto get_size = nt != nullptr ? reinterpret_cast<get_size_fn>(GetProcAddress(nt, "RtlGetCompressionWorkSpaceSize")) : nullptr;
		const auto decompress = nt != nullptr ? reinterpret_cast<decompress_fn>(GetProcAddress(nt, "RtlDecompressBufferEx")) : nullptr;
		ULONG ws = 0, fs = 0;
		if (get_size == nullptr || decompress == nullptr || get_size(kXpressHuff, &ws, &fs) != 0)
			return false;
		std::vector<uint8_t> work(ws);
		out.assign(unpacked, 0);
		ULONG final_size = 0;
		return decompress(kXpressHuff, out.data(), ULONG(unpacked), const_cast<PUCHAR>(data), ULONG(size), &final_size, work.data()) == 0 && final_size == unpacked;
	}

	bool write_file(const std::wstring &path, const void *data, size_t size)
	{
		const size_t slash = path.find_last_of(L"\\/");
		if (slash != std::wstring::npos)
		{
			// (each folder on the way, made when missing)
			const std::wstring folder = path.substr(0, slash);
			for (size_t at = folder.find_first_of(L"\\/", 3); ; at = folder.find_first_of(L"\\/", at + 1))
			{
				CreateDirectoryW(folder.substr(0, at).c_str(), nullptr);
				if (at == std::wstring::npos)
					break;
			}
		}
		const std::wstring temp = path + L".part";
		HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		if (f == INVALID_HANDLE_VALUE)
			return false;
		DWORD put = 0;
		const bool ok = size == 0 || (WriteFile(f, data, DWORD(size), &put, nullptr) && put == size);
		CloseHandle(f);
		if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
		{
			DeleteFileW(temp.c_str());
			return false;
		}
		return true;
	}
}
