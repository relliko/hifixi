#pragma once

// The game's weather left out while World FX draws its own (began as the worldfx addon's /worldfx gamerain, in C++):
// the effect element draw's "mov al, [ebp+17Ah]" (FFXiMain 04609D) becomes a call to a routine of ours that also skips
// weather elements. While World FX draws all of the weather (worldfx_weather_drawn's kWeatherFxAll) the routine's all
// word is set and it skips every weather element, sky layers too; otherwise, while World FX draws its rain or snow
// (kWeatherFxRain, kWeatherFxSnow), it skips those on its list (weather folder name, model name), keeping their sky
// layers. World FX is asked at every update.

#include "game.hpp"
#include <string>

namespace hifi::game::gamerain
{
	// The draw's read to the routine (hide) or back (the HideGameRain setting, with World FX drawing); false and why when
	// it can't be
	bool route(bool hide, std::string &why);
	// Every half second: the routine's all word and list as World FX says now, or both cleared
	void update(uint32_t zone);
	void count_frame();
	// Put back, its memory freed when nothing points into it, everything forgotten (a route after starts afresh); false
	// and why when something else changed the code since
	bool stop(std::string &why);
	std::string status();
	std::string seen();

	// For the tests: the routine's memory (0 before the first route) and the site
	struct sites
	{
		uintptr_t memory = 0, site = 0;
	};
	sites where();
}
