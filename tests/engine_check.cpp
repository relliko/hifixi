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
#include "../src/render/lamp_glow.hpp"
#include <cmath>
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

	// A lamp's glow by the game's light for it (lamp_glow.hpp): fully lit as before the hour was known (setting x
	// (1 - e^(-b/4))); in a ramp the share of full brightness the game gives it (San d'Oria's 'pttm' lamps, 2 fully
	// lit, at 1 or 0.5; West Ronfaure's torches, 30, at 15); off at 0; without a baked brightness as before
	const auto close_to = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
	const float full2 = 1.0f - std::exp(-0.5f), full30 = 1.0f - std::exp(-7.5f);
	check(close_to(grass::lamp_glow_strength(1.0f, 2.0f, 2.0f), full2) && close_to(grass::lamp_glow_strength(0.5f, 30.0f, 30.0f), 0.5f * full30),
		"a lamp's glow fully lit: as before (1 - e^(-b/4)) x the setting");
	check(close_to(grass::lamp_glow_strength(1.0f, 1.0f, 2.0f), 0.5f * full2) && close_to(grass::lamp_glow_strength(1.0f, 0.5f, 2.0f), 0.25f * full2) &&
		close_to(grass::lamp_glow_strength(1.0f, 15.0f, 30.0f), 0.5f * full30), "in a ramp at dusk or dawn: its share of full brightness");
	check(grass::lamp_glow_strength(1.0f, 0.0f, 2.0f) == 0.0f && grass::lamp_glow_strength(1.0f, -1.0f, 2.0f) == 0.0f &&
		grass::lamp_glow_strength(1.0f, NAN, 2.0f) == 0.0f, "off (0, below, not a number): no glow");
	check(close_to(grass::lamp_glow_strength(1.0f, 3.0f, 0.0f), 1.0f - std::exp(-0.75f)) && close_to(grass::lamp_glow_strength(1.0f, 3.0f, 2.0f), 1.0f - std::exp(-0.75f)),
		"not baked, or brighter than baked: as bright as the game's light now (as before)");
	std::printf(failures == 0 ? "\nall passed\n" : "\n%d failed\n", failures);
	return failures == 0 ? 0 : 1;
}
