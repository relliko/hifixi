#pragma once

// HiFi XI's zone files, made on the player's PC (bake.hpp) on a thread of their own: a zone's missing ones the first
// time it's entered (before anything else waiting), or every zone's on request ("Make every zone's files now"). Each
// kind goes to World FX's folder as worldfx-<kind>\<zone>.bin; a zone that has none of a kind gets an empty
// worldfx-<kind>\<zone>.none, so it isn't tried again. When the zone you're in has new files, World FX reads them again.

#include <cstdint>
#include <string>
#include <vector>

namespace hifi::bake
{
	struct runner_status
	{
		bool busy = false;
		uint32_t zone = 0;          // (the one being made now)
		uint32_t left = 0, total = 0;
		std::vector<std::string> lines;
	};

	namespace runner
	{
		// The thread started (once), its files going into `folder` (World FX's, with a slash at the end)
		void start(const std::wstring &folder);
		// A zone entered: its missing files made first
		void zone_entered(uint32_t zone);
		// Every zone's missing files, one after another
		void build_all();
		// What's waiting dropped (the zone being made finishes)
		void cancel();
		runner_status status();
		// The thread ended (the plugin unloaded): waits for the zone being made
		void stop();
	}
}
