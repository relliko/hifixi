/*
 * The hang report (hangwatch.hpp). Each thread is suspended in turn and only read while it is: its registers, the
 * return addresses up its frame chain (ebp) and the words near the top of its stack that point into code, into fixed
 * arrays (a suspended thread may hold the heap's lock: nothing is allocated until it runs again). Names and the file
 * come after.
 */

#include <windows.h>
#include <tlhelp32.h>
#include "hangwatch.hpp"
#include "../render/gpu_crash.hpp"
#include <share.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <cstdint>
#include <vector>

namespace hifi::hangwatch
{
	namespace
	{
		constexpr ULONGLONG kHangMs = 4000;    // (no present this long: a hang; a zone's load stalls a second or two, a Present stuck after an alt-tab 5 s)
		constexpr uint64_t kMinPresents = 300; // (the game running first: not the start's long shader compile)
		constexpr int kMaxReports = 3;         // (a session's)
		constexpr int kFrames = 32;            // (up the frame chain)
		constexpr int kScan = 24;              // (code addresses near the top of the stack)
		constexpr DWORD kScanBytes = 8192;
		constexpr int kLogLines = 40;          // (the log's last lines in the report)

		struct state
		{
			std::atomic<ULONGLONG> last { 0 };
			std::atomic<uint64_t> presents { 0 };
			std::atomic<DWORD> game_thread { 0 };
			std::atomic<bool> quit { false };
			HANDLE thread = nullptr;
			HANDLE wake = nullptr;
			std::wstring folder;
			void (*log)(int, const char *) = nullptr;
			std::string (*context)() = nullptr;
		};
		state g;

		struct walk
		{
			DWORD id = 0;
			bool read = false;
			DWORD eip = 0, esp = 0, ebp = 0;
			DWORD frames[kFrames] = {};
			int count = 0;
			DWORD scan[kScan] = {};
			int scanned = 0;
		};

		bool in_code(DWORD a)
		{
			MEMORY_BASIC_INFORMATION mb;
			if (a < 0x10000 || VirtualQuery(reinterpret_cast<const void *>(uintptr_t(a)), &mb, sizeof(mb)) == 0 || mb.State != MEM_COMMIT)
				return false;
			return (mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
		}

		// (the thread suspended: nothing allocated, every read guarded)
		void read_stack(walk &w)
		{
			__try
			{
				DWORD ebp = w.ebp;
				for (int i = 0; i < kFrames; ++i)
				{
					if (ebp == 0 || (ebp & 3) != 0 || ebp < w.esp || ebp - w.esp > 0x100000)
						break;
					const DWORD next = *reinterpret_cast<const DWORD *>(uintptr_t(ebp));
					const DWORD ret = *reinterpret_cast<const DWORD *>(uintptr_t(ebp) + 4);
					if (ret == 0)
						break;
					w.frames[w.count++] = ret;
					if (next <= ebp)
						break;
					ebp = next;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
			__try
			{
				for (DWORD at = w.esp; at < w.esp + kScanBytes && w.scanned < kScan; at += 4)
				{
					const DWORD v = *reinterpret_cast<const DWORD *>(uintptr_t(at));
					if (in_code(v))
						w.scan[w.scanned++] = v;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}

		std::string where(DWORD a)
		{
			char out[160];
			HMODULE m = nullptr;
			wchar_t path[MAX_PATH] = {};
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(uintptr_t(a)), &m) &&
				GetModuleFileNameW(m, path, MAX_PATH) != 0)
			{
				const wchar_t *name = wcsrchr(path, L'\\');
				name = name != nullptr ? name + 1 : path;
				std::snprintf(out, sizeof(out), "%08lX %ls+0x%lX", static_cast<unsigned long>(a), name, static_cast<unsigned long>(a - DWORD(uintptr_t(m))));
			}
			else
				std::snprintf(out, sizeof(out), "%08lX (no module)", static_cast<unsigned long>(a));
			return out;
		}

		std::string description(HANDLE t)
		{
			using get_fn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
			static const auto get = reinterpret_cast<get_fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
			PWSTR text = nullptr;
			std::string out;
			if (get != nullptr && SUCCEEDED(get(t, &text)) && text != nullptr)
			{
				for (const wchar_t *c = text; *c != 0 && out.size() < 60; ++c)
					out += *c < 128 ? char(*c) : '?';
				LocalFree(text);
			}
			return out;
		}

		std::string log_tail()
		{
			std::string out;
			FILE *f = _wfsopen((g.folder + L"hifixi.log").c_str(), L"rb", _SH_DENYNO);
			if (f == nullptr)
				return out;
			std::fseek(f, 0, SEEK_END);
			const long size = std::ftell(f);
			const long from = size > 16384 ? size - 16384 : 0;
			std::fseek(f, from, SEEK_SET);
			std::vector<char> text(size_t(size - from));
			text.resize(std::fread(text.data(), 1, text.size(), f));
			std::fclose(f);
			std::vector<std::string> lines(1);
			for (char c : text)
				if (c == '\n')
					lines.emplace_back();
				else if (c != '\r')
					lines.back() += c;
			if (from != 0 && !lines.empty())
				lines.erase(lines.begin()); // (cut in the middle)
			const size_t first = lines.size() > size_t(kLogLines) ? lines.size() - kLogLines : 0;
			for (size_t i = first; i < lines.size(); ++i)
				if (!lines[i].empty())
					out += "  " + lines[i] + "\n";
			return out;
		}

		void report(ULONGLONG stalled_ms)
		{
			// The threads (before any is suspended: the snapshot allocates)
			const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId(), game = g.game_thread.load();
			std::vector<walk> walks;
			walks.reserve(256);
			if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE)
			{
				THREADENTRY32 te = {};
				te.dwSize = sizeof(te);
				for (BOOL more = Thread32First(snap, &te); more && walks.size() < walks.capacity(); more = Thread32Next(snap, &te))
					if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
					{
						walk w;
						w.id = te.th32ThreadID;
						walks.push_back(w);
					}
				CloseHandle(snap);
			}
			std::vector<std::string> names(walks.size());
			for (size_t i = 0; i < walks.size(); ++i)
			{
				walk &w = walks[i];
				HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, w.id);
				if (t == nullptr)
					continue;
				if (SuspendThread(t) != DWORD(-1))
				{
					CONTEXT c = {};
					c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
					if (GetThreadContext(t, &c))
					{
						w.read = true;
						w.eip = c.Eip;
						w.esp = c.Esp;
						w.ebp = c.Ebp;
						read_stack(w);
					}
					ResumeThread(t);
				}
				names[i] = description(t);
				CloseHandle(t);
			}

			// The report
			SYSTEMTIME st;
			GetLocalTime(&st);
			std::string out;
			char line[256];
			std::snprintf(line, sizeof(line), "HiFi XI: the game showed no frame for %.1f s (at %04u-%02u-%02u %02u:%02u:%02u.%03u), after %llu presents.\n",
				double(stalled_ms) / 1000.0, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
				static_cast<unsigned long long>(g.presents.load()));
			out += line;
			out += "(Dragging or resizing the game's window stops its frames too: then this isn't a hang.)\n\n";
			out += std::string("Direct3D's device removed (the GPU hung or faulted): ") + (gpu_crash::lost() ? "yes (see worldfx-gpu-crash.txt)" : "no") + "\n";
			if (g.context != nullptr)
				out += g.context();
			out += "\nThe threads (the game's first; each: where it is, then the return addresses up its frame chain, then code "
				"addresses near the top of its stack; HiFi XI's own offsets are named in build\\hifixi.map and build\\d3d8\\d3d8.map):\n";
			std::vector<size_t> order;
			for (size_t i = 0; i < walks.size(); ++i)
				if (walks[i].id == game)
					order.insert(order.begin(), i);
				else
					order.push_back(i);
			for (size_t i : order)
			{
				const walk &w = walks[i];
				std::snprintf(line, sizeof(line), "\n  thread %lu%s%s%s%s\n", static_cast<unsigned long>(w.id), w.id == game ? " (the game's: its presents)" : "",
					names[i].empty() ? "" : " \"", names[i].c_str(), names[i].empty() ? "" : "\"");
				out += line;
				if (!w.read)
				{
					out += "    (couldn't be read)\n";
					continue;
				}
				out += "    at     " + where(w.eip) + "\n";
				for (int k = 0; k < w.count; ++k)
					out += "    called " + where(w.frames[k]) + "\n";
				if (w.scanned != 0)
				{
					out += "    on its stack:";
					for (int k = 0; k < w.scanned; ++k)
						out += (k % 3 == 0 ? "\n      " : "  ") + where(w.scan[k]);
					out += "\n";
				}
			}
			out += "\nThe log's last lines (hifixi.log):\n" + log_tail();

			const std::wstring path = g.folder + L"hifixi-hang.txt";
			FILE *f = nullptr;
			const bool written = _wfopen_s(&f, path.c_str(), L"wb") == 0 && f != nullptr;
			if (written)
			{
				std::fwrite(out.data(), 1, out.size(), f);
				std::fclose(f);
			}
			if (g.log != nullptr)
			{
				std::snprintf(line, sizeof(line), "HiFi XI: the game has shown no frame for %.0f s: %s", double(stalled_ms) / 1000.0,
					written ? "a hang report in bootloader\\hifixi-hang.txt" : "the hang report couldn't be written");
				g.log(2, line);
			}
		}

		DWORD WINAPI watch(void *)
		{
			int reports = 0;
			bool stalled = false;
			uint64_t stalled_at = 0;
			ULONGLONG since = 0;
			while (!g.quit.load())
			{
				WaitForSingleObject(g.wake, 500);
				if (g.quit.load())
					break;
				const uint64_t presents = g.presents.load();
				const ULONGLONG last = g.last.load(), now = GetTickCount64();
				if (stalled)
				{
					if (presents != stalled_at)
					{
						stalled = false;
						if (g.log != nullptr)
						{
							char line[128];
							std::snprintf(line, sizeof(line), "HiFi XI: the game shows frames again, after %.0f s", double(now - since) / 1000.0);
							g.log(1, line);
						}
					}
					continue;
				}
				if (presents < kMinPresents || last == 0 || now - last < kHangMs || reports >= kMaxReports)
					continue;
				stalled = true;
				stalled_at = presents;
				since = last;
				reports++;
				report(now - last);
			}
			return 0;
		}
	}

	void start(const std::wstring &folder, void (*log)(int level, const char *text), std::string (*context)())
	{
		if (g.thread != nullptr)
			return;
		g.folder = folder;
		g.log = log;
		g.context = context;
		g.quit = false;
		g.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		g.thread = CreateThread(nullptr, 0, watch, nullptr, 0, nullptr);
	}

	void stop()
	{
		if (g.thread == nullptr)
			return;
		g.quit = true;
		SetEvent(g.wake);
		WaitForSingleObject(g.thread, 5000);
		CloseHandle(g.thread);
		CloseHandle(g.wake);
		g.thread = nullptr;
		g.wake = nullptr;
	}

	void present()
	{
		g.last.store(GetTickCount64(), std::memory_order_relaxed);
		g.presents.fetch_add(1, std::memory_order_relaxed);
		if (g.game_thread.load(std::memory_order_relaxed) == 0)
			g.game_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
	}
}
