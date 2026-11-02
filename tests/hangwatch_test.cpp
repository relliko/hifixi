/*
 * The hang report (src/plugin/hangwatch): a game thread that presents, then stops for 10 s inside a function of this
 * test's; the report names that thread as the game's, finds it in this program and keeps the log's lines; the log is
 * told when it's written and when the presents come back.
 */

#include <windows.h>
#include "../src/plugin/hangwatch.hpp"
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	int failures = 0, passes = 0;
	std::mutex log_mutex;
	std::vector<std::string> logged;

	void check(bool ok, const std::string &what)
	{
		std::printf("%s  %s\n", ok ? "ok   " : "FAIL ", what.c_str());
		(ok ? passes : failures)++;
	}

	void on_log(int, const char *text)
	{
		std::lock_guard<std::mutex> lock(log_mutex);
		logged.push_back(text);
	}

	std::string context() { return "the test's own line\n"; }

	__declspec(noinline) void stuck_here(DWORD ms)
	{
		Sleep(ms);
	}

	bool logged_has(const char *text)
	{
		std::lock_guard<std::mutex> lock(log_mutex);
		for (const std::string &l : logged)
			if (l.find(text) != std::string::npos)
				return true;
		return false;
	}
}

int main()
{
	wchar_t temp[MAX_PATH];
	GetTempPathW(MAX_PATH, temp);
	const std::wstring folder = std::wstring(temp) + L"hangwatch_test\\";
	CreateDirectoryW(folder.c_str(), nullptr);
	DeleteFileW((folder + L"hifixi-hang.txt").c_str());
	if (FILE *f = nullptr; _wfopen_s(&f, (folder + L"hifixi.log").c_str(), L"wb") == 0 && f != nullptr)
	{
		std::fputs("12:00:00:000 [1] | INFO  | a line of the log before the hang\r\n", f);
		std::fclose(f);
	}

	hifi::hangwatch::start(folder, on_log, context);
	for (int i = 0; i < 400; ++i)
	{
		hifi::hangwatch::present();
		Sleep(1);
	}
	stuck_here(10000);
	check(logged_has("a hang report in bootloader\\hifixi-hang.txt"), "the log told of the report");
	std::string report;
	if (FILE *f = nullptr; _wfopen_s(&f, (folder + L"hifixi-hang.txt").c_str(), L"rb") == 0 && f != nullptr)
	{
		char buf[4096];
		for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) != 0;)
			report.append(buf, n);
		std::fclose(f);
	}
	check(report.find("showed no frame for") != std::string::npos && report.find("after 400 presents") != std::string::npos, "the report: how long, after 400 presents");
	const size_t game = report.find("(the game's: its presents)");
	const size_t next = report.find("\n  thread ", game + 1);
	const std::string game_part = game == std::string::npos ? std::string() : report.substr(game, next == std::string::npos ? std::string::npos : next - game);
	check(game != std::string::npos && game_part.find("hangwatch_test.exe+") != std::string::npos, "the game's thread found, in this program's code");
	check(report.find("Direct3D's device removed (the GPU hung or faulted): no") != std::string::npos && report.find("the test's own line") != std::string::npos,
		"the GPU's state and the caller's lines");
	check(report.find("a line of the log before the hang") != std::string::npos, "the log's last lines");
	for (int i = 0; i < 5; ++i)
	{
		hifi::hangwatch::present();
		Sleep(200);
	}
	check(logged_has("the game shows frames again"), "the log told when the frames came back");
	hifi::hangwatch::stop();
	if (failures == 0)
		std::printf("\nall passed (%d)\n", passes);
	else
		std::printf("\n%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
