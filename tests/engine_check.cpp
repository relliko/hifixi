/*
 * The render engine (src/render, build_engine.cmd's worldfx_engine.lib) in a program built with the plugin's flags
 * (C++20, /EHa): its headers compile there, the library links, and its built-in shaders are src/render/grass.hlsl
 * byte for byte with that file's shader key (the key the shader cache files are named by: unchanged by the move from
 * reshade-worldfx, so an installed cache stays good).
 *
 *   build\engine_check.exe     (from this folder)
 *
 * The engine's own tests (grass_test, e2e, shade_bench...) are in ..\reshade-worldfx\tests, built by its build.cmd.
 */

#include "../src/render/grass_render.hpp"
#include "../src/render/gpu_crash.hpp"
#include "../src/render/vistest.hpp"
#include <cstdio>
#include <string>

namespace
{
	int failures = 0;
	void check(bool ok, const char *what)
	{
		std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
		if (!ok)
			failures++;
	}
}

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::string source;
	if (FILE *f = nullptr; fopen_s(&f, "src\\render\\grass.hlsl", "rb") == 0 && f != nullptr)
	{
		char buffer[4096];
		size_t n;
		while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0)
			source.append(buffer, n);
		fclose(f);
	}
	check(!source.empty(), "src\\render\\grass.hlsl read");
	const std::string &built_in = grass::built_in_shader_source();
	check(built_in == source, "the built-in shaders are src\\render\\grass.hlsl byte for byte");
	const uint64_t key = grass::shader_key(built_in);
	std::printf("  shader key %016llx (%zu bytes)\n", static_cast<unsigned long long>(key), built_in.size());
	check(key != 0 && key == grass::shader_key(source), "their shader key (the cache file's name) is the file's");
	grass::renderer renderer; // (made and destroyed without a device: nothing to release)
	check(!renderer.ready(), "a renderer without a device isn't ready");
	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
