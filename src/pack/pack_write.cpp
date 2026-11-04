/*
 * The texture pack written (pack_write.hpp; docs/texture-pack/format.md "Writing").
 */

#include <windows.h>
#include "pack_write.hpp"
#include "hash.hpp"
#include "validate.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace hifi::pack
{
	namespace
	{
		std::wstring with_slash(std::wstring f)
		{
			if (f.empty() || (f.back() != L'\\' && f.back() != L'/'))
				f += L'\\';
			return f;
		}

		// Each folder on the way made when missing; false when the last isn't a folder afterwards
		bool make_folder(const std::wstring &folder)
		{
			const std::wstring f = folder.substr(0, folder.find_last_not_of(L"\\/") + 1);
			for (size_t at = f.find_first_of(L"\\/", 3);; at = f.find_first_of(L"\\/", at + 1))
			{
				CreateDirectoryW(f.substr(0, at).c_str(), nullptr);
				if (at == std::wstring::npos)
					break;
			}
			const DWORD a = GetFileAttributesW(f.c_str());
			return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
		}

		bool write_all(HANDLE f, const uint8_t *p, uint64_t n)
		{
			while (n > 0)
			{
				const DWORD piece = DWORD(std::min<uint64_t>(n, 64u << 20));
				DWORD put = 0;
				if (!WriteFile(f, p, piece, &put, nullptr) || put != piece)
					return false;
				p += piece;
				n -= piece;
			}
			return true;
		}

		// A whole small file written beside path and moved over it (retried a moment while a reader has the old one
		// open: readers read pack.idx whole and close it at once)
		bool replace_file(const std::wstring &path, const uint8_t *p, size_t n, std::string &why)
		{
			const std::wstring temp = path + L".new";
			HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE)
			{
				why = "can't write a .new file (error " + std::to_string(GetLastError()) + ")";
				return false;
			}
			const bool ok = write_all(f, p, n) && FlushFileBuffers(f);
			CloseHandle(f);
			if (!ok)
			{
				DeleteFileW(temp.c_str());
				why = "writing a .new file failed";
				return false;
			}
			for (int tries = 0;; ++tries)
			{
				if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
					return true;
				const DWORD e = GetLastError();
				if ((e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION) || tries >= 50)
				{
					DeleteFileW(temp.c_str());
					why = "can't move the .new file over the old one (error " + std::to_string(e) + ")";
					return false;
				}
				Sleep(20);
			}
		}

		uint64_t align(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

		// A pack.src line ("rel\tsize\tmtime\tmaker\toverlay_size\toverlay_mtime"; a line of four fields, from before
		// the overlay's were kept, has 0 and 0 for them); false for a line that isn't one
		bool parse_source(const std::string &line, std::string &rel, source_info &s)
		{
			size_t tab[5];
			size_t n = 0;
			for (size_t at = line.find('\t'); at != std::string::npos && n < 5; at = line.find('\t', at + 1))
				tab[n++] = at;
			if (n < 3 || tab[0] == 0)
				return false;
			rel = line.substr(0, tab[0]);
			s = source_info {};
			s.size = std::strtoull(line.c_str() + tab[0] + 1, nullptr, 10);
			s.mtime = std::strtoull(line.c_str() + tab[1] + 1, nullptr, 10);
			s.maker = uint32_t(std::strtoul(line.c_str() + tab[2] + 1, nullptr, 10));
			if (n == 5)
			{
				s.overlay_size = std::strtoull(line.c_str() + tab[3] + 1, nullptr, 10);
				s.overlay_mtime = std::strtoull(line.c_str() + tab[4] + 1, nullptr, 10);
			}
			return true;
		}

		std::string source_text(const std::string &rel, const source_info &s)
		{
			return rel + "\t" + std::to_string(s.size) + "\t" + std::to_string(s.mtime) + "\t" + std::to_string(s.maker) + "\t" + std::to_string(s.overlay_size) +
				"\t" + std::to_string(s.overlay_mtime) + "\r\n";
		}
	}

	bool folder_lock::take(const std::wstring &folder, std::string &why)
	{
		release();
		// (no sharing: a second open fails while this handle lives; deleted on close, also when the process dies)
		HANDLE f = CreateFileW((with_slash(folder) + L"pack.lock").c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
		if (f == INVALID_HANDLE_VALUE)
		{
			const DWORD e = GetLastError();
			why = e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED ? "another texpack is writing this pack (pack.lock is held)"
																			  : "pack.lock can't be made (error " + std::to_string(e) + ")";
			return false;
		}
		handle = f;
		return true;
	}

	void folder_lock::release()
	{
		if (handle != nullptr)
		{
			CloseHandle(handle);
			handle = nullptr;
		}
	}

	writer::~writer() { close(); }

	void writer::close_file()
	{
		if (handle != nullptr)
		{
			CloseHandle(handle);
			handle = nullptr;
		}
	}

	void writer::close()
	{
		close_file();
		lock.release();
	}

	bool writer::open(const writer_options &options, std::string &why)
	{
		close();
		list.clear();
		where.clear();
		crcs.clear();
		ends.clear();
		current = 0;
		sources.clear();
		pending.clear();
		rewrite_sources = false;
		counts = writer_stats {};
		opened = false;
		opt = options;
		opt.folder = with_slash(opt.folder);
		opt.max_file_bytes = std::min(opt.max_file_bytes, kMaxFileBytes);
		if (opt.source.size() > sizeof(header::source) - 1)
			opt.source.resize(sizeof(header::source) - 1);
		if (!make_folder(opt.folder))
		{
			why = "the pack's folder can't be made";
			return false;
		}
		// (before the pack is read: what's loaded is then what no other writer changes until close)
		if (!lock.take(opt.folder, why))
			return false;
		std::string load_why;
		std::shared_ptr<const index> old = load(opt.folder, nullptr, load_why);
		if (old)
		{
			list = old->entries;
			for (size_t i = 0; i < list.size(); ++i)
				where.emplace(list[i].the_key(), i);
			crcs = old->level_crcs;
			ends = old->file_bytes;
			current = ends.empty() ? 0 : uint32_t(ends.size() - 1);
			counts.kept = uint32_t(list.size());
			// (pack.src: a line a source DAT, parse_source; the last line for a rel counts)
			HANDLE f = CreateFileW((opt.folder + L"pack.src").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f != INVALID_HANDLE_VALUE)
			{
				LARGE_INTEGER size = {};
				std::string text;
				if (GetFileSizeEx(f, &size) && size.QuadPart < (256 << 20))
				{
					text.resize(size_t(size.QuadPart));
					DWORD got = 0;
					if (!text.empty() && (!ReadFile(f, text.data(), DWORD(text.size()), &got, nullptr) || got != text.size()))
						text.clear();
				}
				CloseHandle(f);
				for (size_t at = 0; at < text.size();)
				{
					size_t end = text.find('\n', at);
					if (end == std::string::npos)
						end = text.size();
					std::string line = text.substr(at, end - at);
					at = end + 1;
					if (!line.empty() && line.back() == '\r')
						line.pop_back();
					std::string rel;
					source_info s;
					if (parse_source(line, rel, s))
						sources[rel] = s;
				}
			}
		}
		else if (load_why == "no pack.idx")
		{
			// (a pack.src with no index describes a pack that's gone: kept aside, never trusted)
			MoveFileExW((opt.folder + L"pack.src").c_str(), (opt.folder + L"pack.src.old").c_str(), MOVEFILE_REPLACE_EXISTING);
		}
		else
		{
			lock.release();
			why = "the pack there can't be used (" + load_why + "): delete it or pick another folder";
			return false;
		}
		opened = true;
		return true;
	}

	bool writer::done(const std::string &rel, uint64_t size, uint64_t mtime, uint64_t overlay_size, uint64_t overlay_mtime) const
	{
		auto it = sources.find(rel);
		return it != sources.end() && it->second.size == size && it->second.mtime == mtime && it->second.overlay_size == overlay_size &&
			it->second.overlay_mtime == overlay_mtime && it->second.maker == opt.maker_version;
	}

	const entry *writer::find(const key &k) const
	{
		auto it = where.find(k);
		return it == where.end() ? nullptr : &list[it->second];
	}

	bool writer::open_file(uint32_t file, std::string &why)
	{
		// (the file left is flushed: the next commit flushes only the current one, and its pack.idx names both)
		if (handle != nullptr && !FlushFileBuffers(handle))
		{
			why = "flushing a data file failed (error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		close_file();
		if (file >= kMaxFiles)
		{
			why = "the pack has its most data files";
			return false;
		}
		HANDLE f = CreateFileW(data_path(opt.folder, file).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		LARGE_INTEGER size = {};
		if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &size))
		{
			why = "can't open " + std::string(file < 10 ? "pack-00" : file < 100 ? "pack-0" : "pack-") + std::to_string(file) + ".bin (error " +
				std::to_string(GetLastError()) + ")";
			if (f != INVALID_HANDLE_VALUE)
				CloseHandle(f);
			return false;
		}
		if (ends.size() <= file)
			ends.resize(file + 1, 0);
		// (appending at the real end: past what an interrupted run left, which nothing references)
		ends[file] = std::max<uint64_t>(ends[file], uint64_t(size.QuadPart));
		handle = f;
		current = file;
		return true;
	}

	bool writer::append(const uint8_t *data, uint64_t size, uint32_t &offset, uint16_t &file, std::string &why)
	{
		if (size > opt.max_file_bytes)
		{
			why = "a map bigger than a data file";
			return false;
		}
		if (handle == nullptr && !open_file(current, why))
			return false;
		uint64_t at = align(ends[current], kLevelAlign);
		// (a file past the index's may hold an interrupted run's bytes, and so not have room either: the next then)
		while (at + size > opt.max_file_bytes)
		{
			if (!open_file(current + 1, why))
				return false;
			at = align(ends[current], kLevelAlign);
		}
		LARGE_INTEGER to;
		to.QuadPart = LONGLONG(ends[current]);
		static const uint8_t zeros[kLevelAlign] = {};
		bool ok = SetFilePointerEx(handle, to, nullptr, FILE_BEGIN) && write_all(handle, zeros, at - ends[current]) && write_all(handle, data, size);
		if (!ok)
		{
			// (whatever reached the file is past the end the index will name)
			LARGE_INTEGER real = {};
			if (GetFileSizeEx(handle, &real))
				ends[current] = std::max<uint64_t>(ends[current], uint64_t(real.QuadPart));
			why = "writing a data file failed (error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		ends[current] = at + size;
		offset = uint32_t(at);
		file = uint16_t(current);
		return true;
	}

	const char *writer::lay_out(const map_in &in, laid &o, std::string &why)
	{
		o = laid {};
		layout l;
		if (in.w % 4 != 0 || in.h % 4 != 0 || !footprint(in.format, in.w, in.h, uint32_t(in.levels.size()), l))
			return (why = "a map's format, size or level count breaks the rules").c_str();
		o.bytes.assign(size_t(l.total), 0);
		for (uint32_t i = 0; i < l.levels; ++i)
		{
			const level_layout &lv = l.level[i];
			if (in.levels[i].size() != size_t(lv.row_bytes) * lv.rows)
				return (why = "level " + std::to_string(i) + "'s bytes aren't its blocks'").c_str();
			for (uint32_t row = 0; row < lv.rows; ++row)
				std::memcpy(o.bytes.data() + lv.offset + size_t(row) * lv.row_pitch, in.levels[i].data() + size_t(row) * lv.row_bytes, lv.row_bytes);
			o.level_crcs.push_back(crc32(0, o.bytes.data() + lv.offset, size_t(l.level_span(i))));
		}
		o.r.kind = in.kind;
		o.r.levels = uint8_t(l.levels);
		o.r.w = uint16_t(in.w);
		o.r.h = uint16_t(in.h);
		o.r.format = in.format;
		o.r.bytes = uint32_t(l.total);
		o.r.crc = crc32(0, o.bytes.data(), o.bytes.size());
		o.r.param = in.param;
		if (const char *bad = check(o.r))
			return (why = bad).c_str();
		return nullptr;
	}

	add_result writer::set_map(const key &k, const map_in &map, uint16_t flags_set, uint16_t flags_clear, std::string &why)
	{
		auto refuse = [&](const std::string &w) {
			why = w;
			counts.refused++;
			return add_result::refused;
		};
		if (!opened)
			return refuse("the writer isn't open");
		auto it = where.find(k);
		if (it == where.end())
			return refuse("no entry with that key");
		laid o;
		std::string bad;
		if (lay_out(map, o, bad) != nullptr)
			return refuse(bad);
		entry &e = list[it->second];
		record *slot = nullptr;
		for (record &r : e.maps)
			if (r.kind == map.kind)
				slot = &r;
		if (slot != nullptr && slot->format == o.r.format && slot->w == o.r.w && slot->h == o.r.h && slot->levels == o.r.levels && slot->bytes == o.r.bytes &&
			slot->crc == o.r.crc && slot->param == o.r.param)
		{
			e.flags = uint16_t((e.flags | flags_set) & ~flags_clear);
			counts.same++;
			return add_result::same;
		}
		const bool replacing = slot != nullptr;
		if (slot == nullptr)
			for (record &r : e.maps)
				if (r.kind == kUnused && slot == nullptr)
					slot = &r;
		if (slot == nullptr)
			return refuse("the entry has no free map slot");
		if (!append(o.bytes.data(), o.bytes.size(), o.r.offset, o.r.file, why))
		{
			counts.refused++;
			return add_result::refused;
		}
		o.r.level_crc = uint32_t(crcs.size());
		crcs.insert(crcs.end(), o.level_crcs.begin(), o.level_crcs.end());
		*slot = o.r;
		e.flags = uint16_t((e.flags | flags_set) & ~flags_clear);
		counts.bytes += o.bytes.size();
		(replacing ? counts.maps_replaced : counts.maps_added)++;
		return replacing ? add_result::replaced : add_result::ok;
	}

	add_result writer::add(const entry &e, const std::vector<map_in> &maps, std::string &why, bool replace)
	{
		const key k = e.the_key();
		auto refuse = [&](const std::string &w) {
			why = w;
			counts.refused++;
			return add_result::refused;
		};
		if (!opened)
			return refuse("the writer isn't open");
		if (k.bytes == 0)
			return refuse("an empty key");
		if (maps.empty() || maps.size() > 3)
			return refuse("an entry has 1 to 3 maps");
		// (each map laid out in the copy layout, its CRCs made, before anything is written or compared)
		std::vector<laid> out(maps.size());
		for (size_t m = 0; m < maps.size(); ++m)
		{
			for (size_t n = 0; n < m; ++n)
				if (maps[n].kind == maps[m].kind)
					return refuse("two maps of one kind");
			std::string bad;
			if (lay_out(maps[m], out[m], bad) != nullptr)
				return refuse(bad);
		}
		size_t old = list.size();
		if (auto it = where.find(k); it != where.end())
		{
			const entry &have = list[it->second];
			bool same = true;
			for (const laid &o : out)
			{
				const record *r = have.map(o.r.kind);
				same = same && r != nullptr && r->format == o.r.format && r->w == o.r.w && r->h == o.r.h && r->levels == o.r.levels &&
					r->bytes == o.r.bytes && r->crc == o.r.crc && r->param == o.r.param;
			}
			if (same || !replace)
			{
				(same ? counts.same : counts.differs)++;
				return same ? add_result::same : add_result::differs;
			}
			// (the old entry's place taken once the new bytes are written; its bytes are left for compact)
			old = it->second;
		}
		entry n = e;
		for (record &r : n.maps)
			r = record {};
		for (size_t m = 0; m < out.size(); ++m)
		{
			laid &o = out[m];
			if (!append(o.bytes.data(), o.bytes.size(), o.r.offset, o.r.file, why))
			{
				counts.refused++;
				return add_result::refused;
			}
			o.r.level_crc = uint32_t(crcs.size());
			crcs.insert(crcs.end(), o.level_crcs.begin(), o.level_crcs.end());
			n.maps[m] = o.r;
			counts.bytes += o.bytes.size();
		}
		if (old != list.size())
		{
			list[old] = n;
			counts.replaced++;
			return add_result::replaced;
		}
		where.emplace(k, list.size());
		list.push_back(n);
		counts.added++;
		return add_result::ok;
	}

	void writer::finished(const std::string &rel, uint64_t size, uint64_t mtime, uint64_t overlay_size, uint64_t overlay_mtime)
	{
		pending += source_text(rel, source_info { size, mtime, overlay_size, overlay_mtime, opt.maker_version });
	}

	bool writer::forget(const std::string &rel)
	{
		if (sources.erase(rel) == 0)
			return false;
		rewrite_sources = true;
		return true;
	}

	bool writer::drop(const key &k)
	{
		auto it = where.find(k);
		if (it == where.end())
			return false;
		const size_t i = it->second;
		where.erase(it);
		if (i + 1 != list.size())
		{
			list[i] = list.back();
			where[list[i].the_key()] = i;
		}
		list.pop_back();
		counts.dropped++;
		return true;
	}

	bool writer::commit(std::string &why)
	{
		if (!opened)
		{
			why = "the writer isn't open";
			return false;
		}
		if (handle != nullptr && !FlushFileBuffers(handle))
		{
			why = "flushing a data file failed (error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		if (rewrite_sources)
		{
			// (lines taken out before the index changes: a line missing only means its DAT is converted again, a line
			// whose entries are gone would mean they never come back)
			std::vector<const std::pair<const std::string, source_info> *> by_rel;
			for (const auto &s : sources)
				by_rel.push_back(&s);
			std::sort(by_rel.begin(), by_rel.end(), [](const auto *a, const auto *b) { return a->first < b->first; });
			std::string text;
			for (const auto *s : by_rel)
				text += source_text(s->first, s->second);
			if (!replace_file(opt.folder + L"pack.src", reinterpret_cast<const uint8_t *>(text.data()), text.size(), why))
			{
				why = "pack.src can't be rewritten: " + why;
				return false;
			}
			rewrite_sources = false;
		}
		std::vector<entry> sorted = list;
		std::sort(sorted.begin(), sorted.end(), [](const entry &a, const entry &b) { return a.the_key() < b.the_key(); });
		header h;
		h.entry_count = uint32_t(sorted.size());
		h.file_count = uint32_t(ends.size());
		h.crc_count = uint32_t(crcs.size());
		h.maker_version = opt.maker_version;
		FILETIME now;
		GetSystemTimeAsFileTime(&now);
		h.build_time = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
		h.vtable_size = opt.client.vtable_size;
		h.vtable_mtime = opt.client.vtable_mtime;
		h.ftable_size = opt.client.ftable_size;
		h.ftable_mtime = opt.client.ftable_mtime;
		std::memcpy(h.source, opt.source.data(), opt.source.size());
		std::vector<uint8_t> bytes(sizeof(header) + ends.size() * 8 + sorted.size() * sizeof(entry) + crcs.size() * 4);
		uint8_t *p = bytes.data() + sizeof(header);
		if (!ends.empty())
			std::memcpy(p, ends.data(), ends.size() * 8);
		p += ends.size() * 8;
		if (!sorted.empty())
			std::memcpy(p, sorted.data(), sorted.size() * sizeof(entry));
		p += sorted.size() * sizeof(entry);
		if (!crcs.empty())
			std::memcpy(p, crcs.data(), crcs.size() * 4);
		h.table_crc = crc32(0, bytes.data() + sizeof(header), bytes.size() - sizeof(header));
		h.header_crc = crc32(0, &h, offsetof(header, header_crc));
		std::memcpy(bytes.data(), &h, sizeof(h));
		if (!replace_file(opt.folder + L"pack.idx", bytes.data(), bytes.size(), why))
			return false;
		counts.commits++;
		if (!pending.empty())
		{
			HANDLE f = CreateFileW((opt.folder + L"pack.src").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			const bool ok = f != INVALID_HANDLE_VALUE && write_all(f, reinterpret_cast<const uint8_t *>(pending.data()), pending.size());
			if (f != INVALID_HANDLE_VALUE)
				CloseHandle(f);
			if (!ok)
			{
				// (pack.idx has the entries: a rerun redoes these sources and finds every key there already)
				why = "pack.src can't be written";
				return false;
			}
			for (size_t at = 0; at < pending.size();)
			{
				const size_t end = pending.find("\r\n", at);
				std::string rel;
				source_info s;
				if (parse_source(pending.substr(at, end - at), rel, s))
					sources[rel] = s;
				at = end + 2;
			}
			pending.clear();
		}
		return true;
	}

	bool stop_requested(const std::wstring &folder)
	{
		return GetFileAttributesW((with_slash(folder) + L"stop").c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	bool write_progress(const std::wstring &folder, const std::string &text)
	{
		std::string why;
		return replace_file(with_slash(folder) + L"progress.txt", reinterpret_cast<const uint8_t *>(text.data()), text.size(), why);
	}
}
