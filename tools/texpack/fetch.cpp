// hifixi-texpack get-upscaler: Real-ESRGAN's official Windows build fetched on the player's say-so (docs\texture-pack\
// upscaler.md: the builder doesn't ship it). It says what it fetches, from where, how big and under which licences,
// asks y/N (unless --yes), downloads the release zip into a temporary file, refuses it unless its size and SHA-256 are
// the official file's (the file deleted), unpacks it with Windows' own tar.exe and keeps only what make runs:
// realesrgan-ncnn-vulkan.exe, vcomp140.dll (never the debug vcomp140d.dll), models\realesrgan-x4plus.{bin,param} and
// the readme, plus PROVENANCE.txt saying where they came from. Default folder: <the tool's folder>\upscaler, where
// make looks. Then the HDCube model, which ships with the tool (CC0-1.0, <tool folder>\models-hdcube), is copied into
// its models\ (place_hdcube, hashes checked): make's default model, with no other download.

#include "shared.hpp"
#include "upscale.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <urlmon.h>
#include <cstdio>
#include <cstring>

namespace hifi::texpack
{
	namespace
	{
		const wchar_t *kUrl = L"https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip";
		constexpr uint64_t kBytes = 45474481;
		const char *kSha256 = "abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d";

		// A file's SHA-256 in lower-case hex ("" when it can't be read)
		std::string sha256_of(const std::wstring &path)
		{
			HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (f == INVALID_HANDLE_VALUE)
				return {};
			BCRYPT_ALG_HANDLE alg = nullptr;
			BCRYPT_HASH_HANDLE h = nullptr;
			std::string hex;
			if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0)
			{
				std::vector<uint8_t> buf(1 << 20);
				DWORD got = 0;
				bool ok = true;
				while (ok && ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr) && got > 0)
					ok = BCryptHashData(h, buf.data(), got, 0) == 0;
				uint8_t digest[32];
				if (ok && BCryptFinishHash(h, digest, sizeof(digest), 0) == 0)
					for (uint8_t b : digest)
					{
						char two[3];
						std::snprintf(two, sizeof(two), "%02x", b);
						hex += two;
					}
			}
			if (h != nullptr)
				BCryptDestroyHash(h);
			if (alg != nullptr)
				BCryptCloseAlgorithmProvider(alg, 0);
			CloseHandle(f);
			return hex;
		}

		// The mark in <dir>.part: only a .part folder with it is ever deleted
		const wchar_t *kPartMark = L"\\made-by-hifixi-texpack-get-upscaler.txt";

		// What make runs, kept from the zip (all of them, or the folder isn't used)
		const wchar_t *kKept[] = { L"realesrgan-ncnn-vulkan.exe", L"vcomp140.dll", L"models\\realesrgan-x4plus.bin", L"models\\realesrgan-x4plus.param",
			L"README_windows.md" };

		// A program run to its end with no window; its exit code (-1: it couldn't be started)
		long run_wait(const std::wstring &exe, std::wstring cmd)
		{
			STARTUPINFOW si = { sizeof(si) };
			PROCESS_INFORMATION pi = {};
			if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
				return -1;
			CloseHandle(pi.hThread);
			WaitForSingleObject(pi.hProcess, INFINITE);
			DWORD code = 1;
			GetExitCodeProcess(pi.hProcess, &code);
			CloseHandle(pi.hProcess);
			return long(code);
		}

		// file:///C:/x/y.zip (tests) as C:\x\y.zip; "" when the URL isn't a local file's
		std::wstring local_path(const std::wstring &url)
		{
			if (_wcsnicmp(url.c_str(), L"file:///", 8) != 0)
				return {};
			std::wstring p = url.substr(8);
			for (wchar_t &c : p)
				if (c == L'/')
					c = L'\\';
			return p;
		}
	}

	namespace
	{
		// The HDCube model as it ships (research\ffxi-re\tools\upscale\hdcube\PROVENANCE.txt: 4x_HDCube_200k by Venomalia,
		// CC0-1.0, converted for realesrgan-ncnn-vulkan 2026-11-03)
		struct shipped
		{
			const wchar_t *name;
			const char *sha256;
		};
		constexpr shipped kHdcube[] = {
			{ L"realesrgan-hdcube-x4.param", "44b9db926798e1db80604bc66d92facfb4f7397a8eae7e6c93f4f71e30df793d" },
			{ L"realesrgan-hdcube-x4.bin", "bceee5fdefe51110416c96bffe5d9e09b6b5751c635bcbe512252894afbbcd7b" },
		};
	}

	bool place_hdcube(const std::wstring &upscaler, std::string &why)
	{
		const std::wstring from = exe_folder() + L"\\models-hdcube", to = no_slash(upscaler) + L"\\models";
		for (const shipped &f : kHdcube)
		{
			const std::string sha = sha256_of(from + L"\\" + f.name);
			if (sha.empty())
			{
				why = narrow(from) + "\\" + narrow(f.name) + " isn't there (it comes in the tool's zip)";
				return false;
			}
			if (sha != f.sha256)
			{
				why = narrow(from) + "\\" + narrow(f.name) + " isn't the file the tool shipped with (SHA-256 " + sha + ")";
				return false;
			}
		}
		CreateDirectoryW(to.c_str(), nullptr);
		for (const shipped &f : kHdcube)
			if (sha256_of(to + L"\\" + f.name) != f.sha256 && !CopyFileW((from + L"\\" + f.name).c_str(), (to + L"\\" + f.name).c_str(), FALSE))
			{
				why = "it can't be copied into " + narrow(to) + " (error " + std::to_string(GetLastError()) + ")";
				return false;
			}
		CopyFileW((from + L"\\PROVENANCE.txt").c_str(), (to + L"\\PROVENANCE-hdcube.txt").c_str(), FALSE);
		return true;
	}

	int get_upscaler(options &o)
	{
		const std::wstring dir = no_slash(o.dir.empty() ? exe_folder() + L"\\upscaler" : o.dir);
		const std::wstring url = o.url.empty() ? kUrl : o.url;
		std::printf(
			"hifixi-texpack get-upscaler: Real-ESRGAN, the program that makes the textures bigger (make runs it on your\n"
			"graphics card). HiFi XI doesn't ship it: it's fetched from its authors' official release, here, with your OK.\n"
			"  file:      realesrgan-ncnn-vulkan-20220424-windows.zip (Real-ESRGAN v0.2.5.0, 45,474,481 bytes, 43.4 MB)\n"
			"  from:      %s\n"
			"  checked:   size and SHA-256 %s (anything else is refused and deleted)\n"
			"  licences:  Real-ESRGAN (xinntao), BSD-3-Clause; Real-ESRGAN-ncnn-vulkan, MIT; ncnn (Tencent, built into the\n"
			"             program), BSD-3-Clause. See https://github.com/xinntao/Real-ESRGAN and\n"
			"             https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan\n"
			"  kept:      realesrgan-ncnn-vulkan.exe, vcomp140.dll, models\\realesrgan-x4plus.bin/.param, the readme\n"
			"  into:      %s\n",
			narrow(url).c_str(), kSha256, narrow(dir).c_str());
		if (!o.yes)
		{
			std::printf("Download it now? [y/N] ");
			std::fflush(stdout);
			char answer[64] = "";
			if (std::fgets(answer, sizeof(answer), stdin) == nullptr || (answer[0] != 'y' && answer[0] != 'Y'))
			{
				std::printf("Nothing downloaded.\n");
				return kFailed;
			}
		}
		const std::wstring parent = up(dir);
		if (!parent.empty() && !is_folder(parent))
		{
			std::printf("%s isn't there.\n", narrow(parent).c_str());
			return kFailed;
		}
		const std::wstring part = dir + L".part", zip = part + L"\\download.zip", staging = part + L"\\unpacked", kept = part + L"\\kept";
		if (GetFileAttributesW(part.c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			if (!is_folder(part) || !is_file(part + kPartMark))
			{
				std::printf("%s is there and isn't get-upscaler's: move it, or give --dir another folder. Nothing downloaded.\n", narrow(part).c_str());
				return kFailed;
			}
			remove_tree(part);
		}
		const char mark[] = "hifixi-texpack get-upscaler's download folder: deleted when it's done.\r\n";
		if (!CreateDirectoryW(part.c_str(), nullptr) || !bk::write_file(part + kPartMark, mark, std::strlen(mark)))
		{
			std::printf("%s can't be made. Nothing downloaded.\n", narrow(part).c_str());
			remove_tree(part);
			return kFailed;
		}
		auto refuse = [&](const char *what) {
			std::printf("%s\n", what);
			remove_tree(part);
			return kFailed;
		};
		std::printf("Downloading...\n");
		std::fflush(stdout);
		const std::wstring local = local_path(url);
		if (!local.empty() ? !CopyFileW(local.c_str(), zip.c_str(), FALSE) : FAILED(URLDownloadToFileW(nullptr, url.c_str(), zip.c_str(), 0, nullptr)))
			return refuse("The download failed (no connection, or the address is gone): nothing changed.");
		uint64_t size = 0, mtime = 0;
		file_info(zip, size, mtime);
		if (size != kBytes)
		{
			std::printf("The file is %llu bytes, not the official 45,474,481: refused, deleted.\n", static_cast<unsigned long long>(size));
			remove_tree(part);
			return kFailed;
		}
		const std::string sha = sha256_of(zip);
		if (sha != kSha256)
		{
			std::printf("Its SHA-256 is %s, not the official file's: refused, deleted.\n", sha.empty() ? "unreadable" : sha.c_str());
			remove_tree(part);
			return kFailed;
		}
		std::printf("Size and SHA-256 are the official file's.\n");
		wchar_t system[MAX_PATH] = L"";
		GetSystemDirectoryW(system, MAX_PATH);
		const std::wstring tar = std::wstring(system) + L"\\tar.exe";
		CreateDirectoryW(staging.c_str(), nullptr);
		if (!is_file(tar))
			return refuse("Windows' tar.exe (System32) isn't there (Windows 10 1803 or later has it): nothing changed.");
		if (run_wait(tar, L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + staging + L"\"") != 0)
			return refuse("The zip can't be unpacked: nothing changed.");
		// (what's kept gathered in <dir>.part\kept first: the folder make uses gets it only when all of it is there)
		CreateDirectoryW(kept.c_str(), nullptr);
		CreateDirectoryW((kept + L"\\models").c_str(), nullptr);
		std::string missing;
		for (const wchar_t *f : kKept)
			if (!CopyFileW((staging + L"\\" + f).c_str(), (kept + L"\\" + f).c_str(), FALSE))
				missing += (missing.empty() ? "" : ", ") + narrow(f);
		if (!missing.empty())
		{
			std::printf("The zip didn't hold what make needs, or it couldn't be copied (%s): nothing changed.\n", missing.c_str());
			remove_tree(part);
			return kFailed;
		}
		// (any licence text the zip has, kept with it; the release zip has none today)
		std::vector<std::wstring> licences;
		WIN32_FIND_DATAW fd;
		HANDLE fh = FindFirstFileW((staging + L"\\*").c_str(), &fd);
		if (fh != INVALID_HANDLE_VALUE)
		{
			do
				if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && (_wcsnicmp(fd.cFileName, L"LICENSE", 7) == 0 || _wcsnicmp(fd.cFileName, L"NOTICE", 6) == 0))
					if (CopyFileW((staging + L"\\" + fd.cFileName).c_str(), (kept + L"\\" + fd.cFileName).c_str(), FALSE))
						licences.push_back(fd.cFileName);
			while (FindNextFileW(fh, &fd));
			FindClose(fh);
		}
		SYSTEMTIME st;
		GetLocalTime(&st);
		char text[2048];
		std::snprintf(text, sizeof(text),
			"Real-ESRGAN ncnn-vulkan, Windows build realesrgan-ncnn-vulkan-20220424-windows.zip (Real-ESRGAN release v0.2.5.0)\r\n"
			"From: %s\r\n"
			"Fetched %04u-%02u-%02u by hifixi-texpack get-upscaler, with the player's OK (45,474,481 bytes, sha256 %s).\r\n"
			"Kept: realesrgan-ncnn-vulkan.exe, vcomp140.dll, models\\realesrgan-x4plus.bin, models\\realesrgan-x4plus.param, README_windows.md.\r\n"
			"Licences: Real-ESRGAN (https://github.com/xinntao/Real-ESRGAN) BSD-3-Clause; Real-ESRGAN-ncnn-vulkan\r\n"
			"(https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan) MIT; ncnn (https://github.com/Tencent/ncnn, built into the program)\r\n"
			"BSD-3-Clause. Used by hifixi-texpack make (model realesrgan-x4plus).\r\n",
			narrow(url).c_str(), st.wYear, st.wMonth, st.wDay, kSha256);
		if (!bk::write_file(kept + L"\\PROVENANCE.txt", text, std::strlen(text)))
			return refuse("PROVENANCE.txt can't be written: nothing changed.");
		// Into place: the whole folder moved when there's none yet; else each file copied over the old one's (PROVENANCE.txt
		// last, after every other file is there)
		bool placed = false;
		if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
			placed = MoveFileW(kept.c_str(), dir.c_str()) != 0;
		else if (is_folder(dir))
		{
			DeleteFileW((dir + L"\\PROVENANCE.txt").c_str());
			CreateDirectoryW((dir + L"\\models").c_str(), nullptr);
			placed = true;
			for (const wchar_t *f : kKept)
				placed = placed && CopyFileW((kept + L"\\" + f).c_str(), (dir + L"\\" + f).c_str(), FALSE) != 0;
			for (const std::wstring &f : licences)
				placed = placed && CopyFileW((kept + L"\\" + f).c_str(), (dir + L"\\" + f).c_str(), FALSE) != 0;
			placed = placed && CopyFileW((kept + L"\\PROVENANCE.txt").c_str(), (dir + L"\\PROVENANCE.txt").c_str(), FALSE) != 0;
		}
		remove_tree(part);
		const std::string still = upscale::missing_files(dir);
		if (!placed || !still.empty())
		{
			std::printf("The upscaler couldn't be put into %s%s%s: run get-upscaler again.\n", narrow(dir).c_str(), still.empty() ? "" : " (missing: ",
				still.empty() ? "" : (still + ")").c_str());
			return kFailed;
		}
		std::printf("The upscaler is in %s.\n", narrow(dir).c_str());
		// (the HDCube model, make's default, ships with the tool: copied in beside x4plus, no download)
		std::string hd_why;
		if (place_hdcube(dir, hd_why))
			std::printf("The HDCube model (CC0-1.0, shipped with the tool) is in %s\\models too.\n", narrow(dir).c_str());
		else
			std::printf("The HDCube model couldn't be put beside it: %s. make --upscaler-model x4plus works without it; unpack the tool's zip again "
						"for HDCube (make's default).\n",
				hd_why.c_str());
		return kDone;
	}
}
