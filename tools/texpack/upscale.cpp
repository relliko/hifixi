// upscale.py in C++ (upscale.hpp)

#include "upscale.hpp"
#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hifi::texpack::upscale
{
	std::vector<uint8_t> wrap_pad_rgb(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t pad)
	{
		const uint32_t W = w + 2 * pad, H = h + 2 * pad;
		std::vector<uint8_t> out(size_t(W) * H * 3);
		for (uint32_t Y = 0; Y < H; ++Y)
		{
			const uint32_t y = uint32_t((int64_t(Y) - pad + int64_t(h) * (pad / h + 1)) % h);
			for (uint32_t X = 0; X < W; ++X)
			{
				const uint32_t x = uint32_t((int64_t(X) - pad + int64_t(w) * (pad / w + 1)) % w);
				const uint8_t *s = rgba + (size_t(y) * w + x) * 4;
				uint8_t *d = out.data() + (size_t(Y) * W + X) * 3;
				d[0] = s[0], d[1] = s[1], d[2] = s[2];
			}
		}
		return out;
	}

	std::vector<uint8_t> crop(const uint8_t *px, uint32_t W, uint32_t c, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
	{
		std::vector<uint8_t> out(size_t(w) * h * c);
		for (uint32_t y = 0; y < h; ++y)
			std::memcpy(out.data() + size_t(y) * w * c, px + (size_t(y0 + y) * W + x0) * c, size_t(w) * c);
		return out;
	}

	namespace
	{
		constexpr int kPrecision = 32 - 8 - 2; // (Pillow's PRECISION_BITS)

		double sinc(double x)
		{
			if (x == 0.0)
				return 1.0;
			x = x * 3.14159265358979323846;
			return std::sin(x) / x;
		}

		double lanczos3(double x) { return (-3.0 <= x && x < 3.0) ? sinc(x) * sinc(x / 3) : 0.0; }

		// Pillow's precompute_coeffs and normalize_coeffs_8bpc: for each output texel, the first input texel and the
		// fixed-point weights
		struct coeffs
		{
			std::vector<int> first, count;
			std::vector<std::vector<int64_t>> k;
		};

		coeffs precompute(uint32_t in_size, uint32_t out_size)
		{
			coeffs c;
			const double scale = double(in_size) / double(out_size);
			const double filterscale = scale < 1.0 ? 1.0 : scale;
			const double support = 3.0 * filterscale, ss = 1.0 / filterscale;
			c.first.resize(out_size), c.count.resize(out_size), c.k.resize(out_size);
			for (uint32_t xx = 0; xx < out_size; ++xx)
			{
				const double center = (xx + 0.5) * scale;
				int xmin = int(center - support + 0.5);
				if (xmin < 0)
					xmin = 0;
				int xmax = int(center + support + 0.5);
				if (xmax > int(in_size))
					xmax = int(in_size);
				xmax -= xmin;
				std::vector<double> w(size_t(xmax > 0 ? xmax : 0));
				double ww = 0.0;
				for (int x = 0; x < xmax; ++x)
				{
					w[size_t(x)] = lanczos3((x + xmin - center + 0.5) * ss);
					ww += w[size_t(x)];
				}
				c.first[xx] = xmin, c.count[xx] = xmax;
				c.k[xx].resize(w.size());
				for (size_t x = 0; x < w.size(); ++x)
				{
					const double v = (ww != 0.0 ? w[x] / ww : w[x]) * double(1 << kPrecision);
					c.k[xx][x] = v < 0 ? int64_t(int(-0.5 + v)) : int64_t(int(0.5 + v));
				}
			}
			return c;
		}

		uint8_t clip8(int64_t v)
		{
			const int64_t s = v >> kPrecision;
			return uint8_t(s < 0 ? 0 : s > 255 ? 255 : s);
		}
	}

	std::vector<uint8_t> lanczos(const uint8_t *px, uint32_t w, uint32_t h, uint32_t c, uint32_t W, uint32_t H)
	{
		// (horizontal, then vertical; Pillow skips a pass whose size doesn't change)
		std::vector<uint8_t> mid;
		const uint8_t *src = px;
		if (W != w)
		{
			const coeffs cx = precompute(w, W);
			mid.resize(size_t(W) * h * c);
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t X = 0; X < W; ++X)
					for (uint32_t k = 0; k < c; ++k)
					{
						int64_t ss0 = int64_t(1) << (kPrecision - 1);
						for (int x = 0; x < cx.count[X]; ++x)
							ss0 += int64_t(px[(size_t(y) * w + cx.first[X] + x) * c + k]) * cx.k[X][size_t(x)];
						mid[(size_t(y) * W + X) * c + k] = clip8(ss0);
					}
			src = mid.data();
		}
		else
			W = w;
		if (H == h)
			return mid.empty() ? std::vector<uint8_t>(px, px + size_t(w) * h * c) : mid;
		const coeffs cy = precompute(h, H);
		std::vector<uint8_t> out(size_t(W) * H * c);
		for (uint32_t Y = 0; Y < H; ++Y)
			for (uint32_t x = 0; x < W; ++x)
				for (uint32_t k = 0; k < c; ++k)
				{
					int64_t ss0 = int64_t(1) << (kPrecision - 1);
					for (int y = 0; y < cy.count[Y]; ++y)
						ss0 += int64_t(src[(size_t(cy.first[Y] + y) * W + x) * c + k]) * cy.k[Y][size_t(y)];
					out[(size_t(Y) * W + x) * c + k] = clip8(ss0);
				}
		return out;
	}

	bool finish(const uint8_t *orig, uint32_t w, uint32_t h, const uint8_t *esrgan, uint32_t ew, uint32_t eh, uint32_t factor, std::vector<uint8_t> &out,
		std::string &why)
	{
		if (ew != (w + 2 * kPad) * kScale || eh != (h + 2 * kPad) * kScale)
		{
			char text[160];
			std::snprintf(text, sizeof(text), "the upscaler gave %ux%u for a %ux%u picture (wanted %ux%u)", ew, eh, w + 2 * kPad, h + 2 * kPad,
				(w + 2 * kPad) * kScale, (h + 2 * kPad) * kScale);
			why = text;
			return false;
		}
		if (factor != 2 && factor != 4)
		{
			why = "the factor must be 2 or 4";
			return false;
		}
		std::vector<uint8_t> rgba = crop(esrgan, ew, 4, kPad * kScale, kPad * kScale, w * kScale, h * kScale), rgb(size_t(w) * kScale * h * kScale * 3);
		for (size_t i = 0; i < size_t(w) * kScale * h * kScale; ++i)
			for (int k = 0; k < 3; ++k)
				rgb[i * 3 + k] = rgba[i * 4 + k];
		if (factor != kScale)
			rgb = lanczos(rgb.data(), w * kScale, h * kScale, 3, w * factor, h * factor);
		std::vector<uint8_t> a(size_t(w) * h);
		for (size_t i = 0; i < a.size(); ++i)
			a[i] = orig[i * 4 + 3];
		const std::vector<uint8_t> A = lanczos(a.data(), w, h, 1, w * factor, h * factor);
		const size_t n = size_t(w) * factor * h * factor;
		out.resize(n * 4);
		for (size_t i = 0; i < n; ++i)
		{
			out[i * 4] = rgb[i * 3], out[i * 4 + 1] = rgb[i * 3 + 1], out[i * 4 + 2] = rgb[i * 3 + 2];
			out[i * 4 + 3] = A[i];
		}
		return true;
	}

	std::string missing_files(const std::wstring &folder)
	{
		std::string missing;
		for (const wchar_t *f : { L"realesrgan-ncnn-vulkan.exe", L"vcomp140.dll", L"models\\realesrgan-x4plus.param", L"models\\realesrgan-x4plus.bin" })
		{
			const DWORD a = GetFileAttributesW((folder + L"\\" + f).c_str());
			if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				char name[128];
				std::snprintf(name, sizeof(name), "%ls", f);
				missing += (missing.empty() ? "" : ", ") + std::string(name);
			}
		}
		return missing;
	}

	namespace
	{
		uint32_t files_in(const std::wstring &folder)
		{
			uint32_t n = 0;
			WIN32_FIND_DATAW fd;
			HANDLE f = FindFirstFileW((folder + L"\\*.png").c_str(), &fd);
			if (f == INVALID_HANDLE_VALUE)
				return 0;
			do
				n++;
			while (FindNextFileW(f, &fd));
			FindClose(f);
			return n;
		}
	}

	end run(const run_options &o, std::string &why)
	{
		const std::wstring exe = o.folder + L"\\realesrgan-ncnn-vulkan.exe";
		std::wstring cmd = L"\"" + exe + L"\" -i \"" + o.in + L"\" -o \"" + o.out + L"\" -n realesrgan-x4plus -m \"" + o.folder + L"\\models\" -f png";

		// (its console output to the log: the only handle it inherits)
		SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
		HANDLE log = o.log.empty() ? INVALID_HANDLE_VALUE
								   : CreateFileW(o.log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (log == INVALID_HANDLE_VALUE)
			log = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
		SIZE_T bytes = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
		std::vector<uint8_t> attr_mem(bytes);
		auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_mem.data());
		HANDLE inherit[1] = { log };
		const bool have_attrs = InitializeProcThreadAttributeList(attrs, 1, 0, &bytes) &&
			UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);
		STARTUPINFOEXW si = {};
		si.StartupInfo.cb = sizeof(si);
		si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		si.StartupInfo.hStdInput = nullptr;
		si.StartupInfo.hStdOutput = log;
		si.StartupInfo.hStdError = log;
		si.lpAttributeList = have_attrs ? attrs : nullptr;

		HANDLE job = CreateJobObjectW(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (job != nullptr)
			SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

		PROCESS_INFORMATION pi = {};
		const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
			CREATE_SUSPENDED | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | (have_attrs ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, o.folder.c_str(),
			&si.StartupInfo, &pi);
		const DWORD start_error = started ? 0 : GetLastError();
		if (have_attrs)
			DeleteProcThreadAttributeList(attrs);
		CloseHandle(log);
		if (!started)
		{
			if (job != nullptr)
				CloseHandle(job);
			why = "it can't be started (error " + std::to_string(start_error) + ")";
			return end::failed;
		}
		// (in the job before it runs a single instruction: it can't start anything outside it)
		if (job == nullptr || !AssignProcessToJobObject(job, pi.hProcess))
		{
			TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, 5000);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			if (job != nullptr)
				CloseHandle(job);
			why = "it can't be put in a job object (error " + std::to_string(GetLastError()) + ")";
			return end::failed;
		}
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);

		end result = end::done;
		uint32_t seen = 0;
		auto last_new = std::chrono::steady_clock::now();
		for (;;)
		{
			if (WaitForSingleObject(pi.hProcess, DWORD(o.poll_s * 1000)) == WAIT_OBJECT_0)
				break;
			if (o.should_end)
				if (const std::string w = o.should_end(); !w.empty())
				{
					why = w;
					result = end::ended;
					break;
				}
			const uint32_t n = files_in(o.out);
			if (n != seen)
				seen = n, last_new = std::chrono::steady_clock::now();
			else if (std::chrono::duration<double>(std::chrono::steady_clock::now() - last_new).count() > o.stall_s)
			{
				char text[128];
				std::snprintf(text, sizeof(text), "no new picture for %.0f s (%u of the batch made)", o.stall_s, n);
				why = text;
				result = end::stalled;
				break;
			}
		}
		if (result != end::done)
		{
			TerminateJobObject(job, 1);
			WaitForSingleObject(pi.hProcess, 10000);
		}
		else
		{
			DWORD code = 1;
			GetExitCodeProcess(pi.hProcess, &code);
			if (code != 0)
			{
				why = "it exited with code " + std::to_string(code);
				result = end::failed;
			}
		}
		CloseHandle(pi.hProcess);
		CloseHandle(job);
		return result;
	}
}
