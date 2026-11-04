/*
 * A stand-in for realesrgan-ncnn-vulkan.exe in texpack_test (built by tools\build_texpack.cmd): the same command line
 * (-i <folder> -o <folder> -n <model> -m <models> -f png), each PNG of the input folder enlarged 4x by nearest
 * neighbour (WIC, tools\texpack\image_io.cpp) into the output folder under its own name. FAKE_ESRGAN_MODE:
 *   (unset) as above; "slow": FAKE_ESRGAN_SLOW_MS (default 300) ms before each picture; "hang": nothing written, waits
 *   for ever; "fail": exit 3 at once.
 * FAKE_ESRGAN_LOG names a file each run appends "start <pid>" and "end <pid>" to (the test reads which ran and whether
 * one was ended before its end). With no -i it only waits for ever: copied under another name, it is the test's "game".
 */

#include <windows.h>
#include "../tools/texpack/image_io.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace img = hifi::texpack::image;

static void log_line(const char *what)
{
	char path[MAX_PATH] = "";
	if (GetEnvironmentVariableA("FAKE_ESRGAN_LOG", path, MAX_PATH) == 0)
		return;
	FILE *f = nullptr;
	if (fopen_s(&f, path, "ab") == 0 && f != nullptr)
	{
		std::fprintf(f, "%s %lu\r\n", what, GetCurrentProcessId());
		std::fclose(f);
	}
}

int wmain(int argc, wchar_t **argv)
{
	std::wstring in, out;
	for (int i = 1; i + 1 < argc; ++i)
	{
		if (std::wstring(argv[i]) == L"-i")
			in = argv[++i];
		else if (std::wstring(argv[i]) == L"-o")
			out = argv[++i];
	}
	if (in.empty())
	{
		Sleep(INFINITE);
		return 0;
	}
	char mode[32] = "", slow[32] = "";
	GetEnvironmentVariableA("FAKE_ESRGAN_MODE", mode, sizeof(mode));
	const DWORD slow_ms = GetEnvironmentVariableA("FAKE_ESRGAN_SLOW_MS", slow, sizeof(slow)) ? DWORD(std::atoi(slow)) : 300;
	log_line("start");
	if (std::string(mode) == "fail")
		return 3;
	if (std::string(mode) == "hang")
	{
		Sleep(INFINITE);
		return 0;
	}
	img::com_scope com;
	std::vector<std::wstring> names;
	WIN32_FIND_DATAW fd;
	HANDLE f = FindFirstFileW((in + L"\\*.png").c_str(), &fd);
	if (f != INVALID_HANDLE_VALUE)
	{
		do
			names.push_back(fd.cFileName);
		while (FindNextFileW(f, &fd));
		FindClose(f);
	}
	std::sort(names.begin(), names.end());
	for (const std::wstring &n : names)
	{
		if (std::string(mode) == "slow")
			Sleep(slow_ms);
		img::picture p;
		std::string why;
		if (!img::read(in + L"\\" + n, p, why))
		{
			std::printf("can't read %ls: %s\n", n.c_str(), why.c_str());
			return 2;
		}
		const uint32_t W = p.w * 4, H = p.h * 4;
		std::vector<uint8_t> rgb(size_t(W) * H * 3);
		for (uint32_t y = 0; y < H; ++y)
			for (uint32_t x = 0; x < W; ++x)
				for (int c = 0; c < 3; ++c)
					rgb[(size_t(y) * W + x) * 3 + c] = p.px[(size_t(y / 4) * p.w + x / 4) * 4 + c];
		if (!img::write_png(out + L"\\" + n, rgb.data(), W, H, 3, why))
		{
			std::printf("can't write %ls: %s\n", n.c_str(), why.c_str());
			return 2;
		}
	}
	log_line("end");
	return 0;
}
