/*
 * The texture pack read (pack.hpp; docs/texture-pack/format.md): pack.idx loaded whole and checked before any of it is
 * used, records read with overlapped ReadFile in pieces, every byte checked against its CRC before it's handed over.
 */

#include <windows.h>
#include "pack.hpp"
#include "hash.hpp"
#include "validate.hpp"
#include <algorithm>
#include <cstdio>

namespace hifi::pack
{
	namespace
	{
		// (pack.idx is a few MB: 160 bytes an entry; one past this is no index of ours, and not worth the address space)
		constexpr uint64_t kMaxIndexBytes = 256ull << 20;

		std::string narrow(const std::wstring &w)
		{
			std::string s;
			for (wchar_t c : w)
				s += c < 128 ? char(c) : '?';
			return s;
		}

		uint64_t filetime(const FILETIME &t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

		// A file's size, or false when it isn't there (no handle kept: the folder stays free to rename)
		bool size_of(const std::wstring &path, uint64_t &size, uint64_t *mtime = nullptr)
		{
			WIN32_FILE_ATTRIBUTE_DATA a;
			if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				return false;
			size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
			if (mtime != nullptr)
				*mtime = filetime(a.ftLastWriteTime);
			return true;
		}
	}

	fingerprint client_fingerprint(const std::wstring &ffxi)
	{
		std::wstring base = ffxi;
		if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
			base += L'\\';
		fingerprint f;
		size_of(base + L"VTABLE.DAT", f.vtable_size, &f.vtable_mtime);
		size_of(base + L"FTABLE.DAT", f.ftable_size, &f.ftable_mtime);
		return f;
	}

	std::wstring folder_in(const std::wstring &base)
	{
		wchar_t env[MAX_PATH];
		const DWORD n = GetEnvironmentVariableW(L"HIFIXI_PACK", env, MAX_PATH);
		std::wstring f = n > 0 && n < MAX_PATH ? std::wstring(env, n) : base + (base.empty() || base.back() == L'\\' || base.back() == L'/' ? L"" : L"\\") + L"hifixi-pack";
		if (f.back() != L'\\' && f.back() != L'/')
			f += L'\\';
		return f;
	}

	std::wstring data_path(const std::wstring &folder, uint32_t file)
	{
		wchar_t name[32];
		std::swprintf(name, 32, L"pack-%03u.bin", file);
		return folder + name;
	}

	const entry *index::find(const key &k) const
	{
		auto it = std::lower_bound(entries.begin(), entries.end(), k, [](const entry &e, const key &v) { return e.the_key() < v; });
		return it != entries.end() && it->the_key() == k ? &*it : nullptr;
	}

	bool parse(const uint8_t *bytes, size_t size, index &out, std::string &why)
	{
		out = index {};
		header h;
		if (size < sizeof(header))
		{
			why = "pack.idx is cut short (" + std::to_string(size) + " bytes, not even a header)";
			return false;
		}
		std::memcpy(&h, bytes, sizeof(h));
		if (std::memcmp(h.magic, kMagic, 4) != 0)
		{
			why = "pack.idx isn't a HiFi XI texture pack index";
			return false;
		}
		if (h.version != kVersion)
		{
			why = "pack.idx is version " + std::to_string(h.version) + "; this HiFi XI reads version " + std::to_string(kVersion);
			return false;
		}
		if (crc32(0, bytes, offsetof(header, header_crc)) != h.header_crc)
		{
			why = "pack.idx's header is damaged (CRC)";
			return false;
		}
		if (h.header_bytes != sizeof(header) || h.entry_bytes != sizeof(entry) || h.record_bytes != sizeof(record) || h.records_per_entry != 3)
		{
			why = "pack.idx's table sizes aren't version 1's";
			return false;
		}
		if (h.flags != kSpanWholeBody)
		{
			why = "pack.idx's keys aren't made over the whole texture chunk (flags " + std::to_string(h.flags) + ")";
			return false;
		}
		if (h.file_count > kMaxFiles)
		{
			why = "pack.idx names " + std::to_string(h.file_count) + " data files";
			return false;
		}
		const uint64_t want = sizeof(header) + uint64_t(h.file_count) * 8 + uint64_t(h.entry_count) * sizeof(entry) + uint64_t(h.crc_count) * 4;
		if (size != want)
		{
			why = size < want ? "pack.idx is cut short (" + std::to_string(size) + " of " + std::to_string(want) + " bytes)"
							  : "pack.idx has bytes past its tables";
			return false;
		}
		if (crc32(0, bytes + sizeof(header), size - sizeof(header)) != h.table_crc)
		{
			why = "pack.idx's tables are damaged (CRC)";
			return false;
		}
		out.head = h;
		const uint8_t *p = bytes + sizeof(header);
		out.file_bytes.resize(h.file_count);
		if (h.file_count != 0)
			std::memcpy(out.file_bytes.data(), p, size_t(h.file_count) * 8);
		p += size_t(h.file_count) * 8;
		out.entries.resize(h.entry_count);
		if (h.entry_count != 0)
			std::memcpy(out.entries.data(), p, size_t(h.entry_count) * sizeof(entry));
		p += size_t(h.entry_count) * sizeof(entry);
		out.level_crcs.resize(h.crc_count);
		if (h.crc_count != 0)
			std::memcpy(out.level_crcs.data(), p, size_t(h.crc_count) * 4);
		for (size_t i = 0; i < out.entries.size(); ++i)
		{
			const entry &e = out.entries[i];
			if (i > 0 && !(out.entries[i - 1].the_key() < e.the_key()))
			{
				why = "pack.idx's entries aren't sorted by key (entry " + std::to_string(i) + ")";
				return false;
			}
			for (const record &r : e.maps)
			{
				if (r.kind != kColour && r.kind != kHeight && r.kind != kMaterial)
					continue; // (unused, or a kind of a later version: skipped)
				if (r.file >= h.file_count || uint64_t(r.offset) + r.bytes > out.file_bytes[r.file])
				{
					why = "an entry's map lies past the end of " + narrow(data_path(L"", r.file)) + " (entry " + std::to_string(i) + ")";
					return false;
				}
				if (uint64_t(r.offset) + r.bytes > kMaxFileBytes)
				{
					// (a data file is at most 1 GiB, every record inside it: what keeps u32 offsets from overflowing)
					why = "an entry's map ends past a data file's 1 GiB (entry " + std::to_string(i) + ")";
					return false;
				}
				if (uint64_t(r.level_crc) + r.levels > h.crc_count)
				{
					why = "an entry's level CRCs lie past the table (entry " + std::to_string(i) + ")";
					return false;
				}
			}
		}
		return true;
	}

	void (*load_test_window)() = nullptr;

	std::shared_ptr<const index> load(const std::wstring &folder_given, const fingerprint *client, std::string &why, const std::atomic<bool> *cancel)
	{
		auto cancelled = [&] {
			if (cancel == nullptr || !cancel->load())
				return false;
			why = "cancelled";
			return true;
		};
		std::wstring folder = folder_given;
		if (folder.empty() || (folder.back() != L'\\' && folder.back() != L'/'))
			folder += L'\\';
		std::vector<uint8_t> bytes;
		{
			HANDLE f = CreateFileW((folder + L"pack.idx").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE)
			{
				const DWORD e = GetLastError();
				why = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? "no pack.idx" : "pack.idx can't be opened (error " + std::to_string(e) + ")";
				return nullptr;
			}
			LARGE_INTEGER size = {};
			bool ok = GetFileSizeEx(f, &size) != 0 && uint64_t(size.QuadPart) <= kMaxIndexBytes;
			if (ok)
			{
				bytes.resize(size_t(size.QuadPart));
				for (size_t at = 0; ok && at < bytes.size();)
				{
					if (load_test_window != nullptr)
						load_test_window();
					if (cancelled())
					{
						CloseHandle(f);
						return nullptr;
					}
					DWORD got = 0;
					ok = ReadFile(f, bytes.data() + at, DWORD(std::min<size_t>(bytes.size() - at, kIndexPiece)), &got, nullptr) != 0 && got != 0;
					at += got;
				}
			}
			CloseHandle(f);
			if (!ok)
			{
				why = "pack.idx can't be read";
				return nullptr;
			}
		}
		if (cancelled())
			return nullptr;
		auto out = std::make_shared<index>();
		if (!parse(bytes.data(), bytes.size(), *out, why))
			return nullptr;
		out->folder = folder;
		for (uint32_t i = 0; i < out->head.file_count; ++i)
		{
			if (cancelled())
				return nullptr;
			uint64_t real = 0;
			const std::wstring path = data_path(folder, i);
			if (!size_of(path, real))
			{
				why = narrow(data_path(L"", i)) + " is missing";
				return nullptr;
			}
			if (real < out->file_bytes[i])
			{
				why = narrow(data_path(L"", i)) + " is shorter than pack.idx says (" + std::to_string(real) + " of " + std::to_string(out->file_bytes[i]) + " bytes)";
				return nullptr;
			}
		}
		const header &h = out->head;
		const fingerprint made { h.vtable_size, h.vtable_mtime, h.ftable_size, h.ftable_mtime };
		if (client != nullptr && made != fingerprint {} && !(made == *client))
			out->status = "The texture pack was made from other game files (VTABLE.DAT or FTABLE.DAT changed since): textures the game changed are drawn as the game's";
		return out;
	}

	reader::reader(std::shared_ptr<const index> pack) : idx(std::move(pack))
	{
		handles.assign(idx ? idx->head.file_count : 0, INVALID_HANDLE_VALUE);
		event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	}

	reader::~reader()
	{
		close();
		if (event != nullptr)
			CloseHandle(event);
	}

	void reader::close()
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (void *&h : handles)
			if (h != INVALID_HANDLE_VALUE)
			{
				CloseHandle(h);
				h = INVALID_HANDLE_VALUE;
			}
	}

	void reader::cancel()
	{
		cancelled = true;
		std::lock_guard<std::mutex> lock(mutex);
		for (void *h : handles)
			if (h != INVALID_HANDLE_VALUE)
				CancelIoEx(h, nullptr);
	}

	void reader::resume() { cancelled = false; }

	bool reader::read(uint32_t file, uint64_t offset, uint8_t *out, uint64_t n, std::string &why)
	{
		if (!idx || file >= handles.size() || event == nullptr)
		{
			why = "no such data file";
			return false;
		}
		HANDLE h;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (handles[file] == INVALID_HANDLE_VALUE)
				handles[file] = CreateFileW(data_path(idx->folder, file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
			h = handles[file];
		}
		if (h == INVALID_HANDLE_VALUE)
		{
			why = narrow(data_path(L"", file)) + " can't be opened (error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		for (uint64_t at = 0; at < n;)
		{
			// (checked before each piece: a cancel that came between CancelIoEx and the next ReadFile still ends it here)
			if (cancelled)
			{
				why = "cancelled";
				return false;
			}
			const DWORD piece = DWORD(std::min<uint64_t>(n - at, kReadPiece));
			OVERLAPPED ov = {};
			ov.Offset = DWORD(offset + at);
			ov.OffsetHigh = DWORD((offset + at) >> 32);
			ov.hEvent = event;
			DWORD got = 0;
			if (test_window != nullptr)
				test_window(*this);
			BOOL ok = ReadFile(h, out + size_t(at), piece, nullptr, &ov);
			DWORD e = ok ? ERROR_SUCCESS : GetLastError();
			if (ok || e == ERROR_IO_PENDING)
			{
				// (a cancel() between the check above and this ReadFile found no read to cancel: cancelled is stored before
				// cancel()'s CancelIoEx, so either that CancelIoEx saw this read or this load sees the flag)
				if (e == ERROR_IO_PENDING && cancelled)
					CancelIoEx(h, &ov);
				ok = GetOverlappedResult(h, &ov, &got, TRUE);
				e = ok ? ERROR_SUCCESS : GetLastError();
			}
			if (e == ERROR_OPERATION_ABORTED)
			{
				why = "cancelled";
				return false;
			}
			if (!ok || got != piece)
			{
				why = e == ERROR_HANDLE_EOF || (ok && got != piece) ? narrow(data_path(L"", file)) + " is cut short"
																	 : "read failed (error " + std::to_string(e) + ")";
				return false;
			}
			at += piece;
		}
		return true;
	}

	bool reader::read_record(const record &r, uint8_t *out, std::string &why)
	{
		if (const char *bad = check(r))
		{
			why = bad;
			return false;
		}
		if (!read(r.file, r.offset, out, r.bytes, why))
			return false;
		if (crc32(0, out, r.bytes) != r.crc)
		{
			why = "CRC mismatch";
			return false;
		}
		return true;
	}

	bool reader::read_level(const record &r, uint32_t level, uint8_t *out, std::string &why)
	{
		layout l;
		if (const char *bad = check(r))
		{
			why = bad;
			return false;
		}
		if (level >= r.levels || !footprint(r.format, r.w, r.h, r.levels, l))
		{
			why = "no such level";
			return false;
		}
		const uint64_t span = l.level_span(level);
		if (!read(r.file, uint64_t(r.offset) + l.level[level].offset, out, span, why))
			return false;
		if (crc32(0, out, size_t(span)) != idx->level_crc(r, level))
		{
			why = "CRC mismatch";
			return false;
		}
		return true;
	}

	uint64_t reader::levels_span(const record &r, uint32_t first, uint32_t count)
	{
		layout l;
		if (check(r) != nullptr || count == 0 || first >= r.levels || count > r.levels - first || !footprint(r.format, r.w, r.h, r.levels, l))
			return 0;
		const uint32_t end = first + count;
		return (end < l.levels ? l.level[end].offset : l.total) - l.level[first].offset;
	}

	bool reader::read_levels(const record &r, uint32_t first, uint32_t count, uint8_t *out, std::string &why)
	{
		layout l;
		if (const char *bad = check(r))
		{
			why = bad;
			return false;
		}
		const uint64_t span = levels_span(r, first, count);
		if (span == 0 || !footprint(r.format, r.w, r.h, r.levels, l))
		{
			why = "no such levels";
			return false;
		}
		if (!read(r.file, uint64_t(r.offset) + l.level[first].offset, out, span, why))
			return false;
		for (uint32_t i = first; i < first + count; ++i)
			if (crc32(0, out + (l.level[i].offset - l.level[first].offset), size_t(l.level_span(i))) != idx->level_crc(r, i))
			{
				why = "CRC mismatch (level " + std::to_string(i) + ")";
				return false;
			}
		return true;
	}
}
