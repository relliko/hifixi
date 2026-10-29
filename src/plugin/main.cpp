/*
 * HiFi XI: Relli's graphics work in one Ashita v4 plugin (hifps, drawcull, World FX and its feeder), on dgVoodoo's
 * Direct3D 12 output, without ReShade.
 *
 * - World FX (src/render/worldfx_core: grass, shading, shadows, water, weather, sky, lamps, glows) draws into the
 *   game's own frame on HiFi XI's Direct3D 12 hooks (src/host, src/backend). HiFi XI's starter (bootloader\dxgi.dll)
 *   starts it before dgVoodoo makes its device; without it, it starts here, late, and can't find the world.
 * - The game side (src/game): what World FX needs from the game each frame (feed: the zone, characters near, weapon
 *   skills, lamps, glows, the weather), and the game's own effects it replaces left out while World FX draws;
 *   hifps's frame step, limiter and speed-ups (hifps/); drawcull's view distance (drawcull).
 * - Each zone's files made on the player's PC from their own game files the first time it's entered (src/bake).
 * - Settings in config\hifixi\settings.ini, the menu (src/ui/menu) drawn through Ashita's ImGui.
 *
 *   /hifi          the menu (presets, settings by tab, status), or closes it
 *   /hifi status   what each part is doing (the hooks' counts per frame since they went in, or /hifi reset)
 *   /hifi glows    the glows lighting the scene now
 *   /hifi gpu      World FX's GPU time by section, the costliest first
 *   /hifi gamedraw [zone|chars] [on|off]   the game's own zone or character drawing switched off (a debug toggle for
 *                  the native renderer to come: off at every start), or what's off
 *   /hifi streamlog [frames] [label]   the next world frame (up to 8) as dgVoodoo records it in Direct3D 12, call by
 *                  call, and the game's own Direct3D 8 draws of it, into bootloader\hifixi-stream-<time>-<label>.txt
 *                  (src/backend/streamlog; the label is what /hifi gamedraw has off when none is given)
 *   /hifi zoneslot [on|off]   the zone pass's slot markers (src/game/zoneslot: on by default, [zone] slot in the
 *                  settings): where the game draws its zone, marked in its own Direct3D 8 stream for the Direct3D 12
 *                  side (World FX finds the world by them, the game's zone drawing on or off), or what they're doing
 *   HiFi XI always draws the zone and the characters itself (since 0.42: no switching back to the game's drawing;
 *   the game still draws whatever HiFi XI can't yet). What each part is doing:
 *   /hifi zonedata   the native zone renderer's data (src/zone/native): each zone's meshes, textures and normal maps
 *                  read from the game's DATs and put on the GPU as it's entered
 *   /hifi zoneslot   the zone pass's slot markers (src/game/zoneslot)
 *   /hifi nativezone [perpixel on|off]   HiFi XI drawing the zone in the game's place (src/game/zonedraw, src/zone;
 *                  [zone] perpixel its per-pixel lighting)
 *   /hifi charslot   the character passes' slot markers (src/game/charslot): the opaque actors' walk and each
 *                  see-through actor
 *   /hifi chardata   the native character renderer's data (src/game/charmodel, src/chars): each model the game draws
 *                  recorded at its draw (its palettes, lights, meshes), the meshes and textures put on the GPU as
 *                  they're first seen; /hifi gamedraw chars goes through its stub
 *   /hifi enspell [auto|off|fire|ice|wind|earth|thunder|water|light|dark]   the en-spell effect on your blades,
 *                  HiFi XI's own (src/chars chardraw; [chars] enspell): auto = the element of the en-spell on you
 *                  (Enfire .. Endark, the IIs too); off; or one element always (to look at it)
 *   /hifi nativechars [perpixel [on|off]]   HiFi XI drawing the characters in the game's place (src/chars; [chars]
 *                  perpixel its per-pixel lighting); the game draws any model whose data isn't up yet
 *   /hifi upscale [on|off] [sharpen <0..1>] [framegen on|off]   the world upscaled to the screen with AMD FSR 1 when the background
 *                  resolution is below the screen's (src/upscale; [upscale] fsr, sharpen)
 *   /hifi weather [<name>|off|flash]   World FX's own weather forced to one of the game's weathers (fine, suny, clod,
 *                  mist, dryw, heat, rain, squl, dust, sand, wind, stom, snow, bliz, thdr, bolt, aura, ligt, fogd, dark),
 *                  cross-faded; off: back to the real one; flash: lightning now. Debug, not saved (off at every start);
 *                  only World FX's own drawing: the zone's light and fog still follow the real weather
 *   /hifi reset    starts the counts again
 *   /hifi off|on   takes the hooks out, or puts them back
 */

#include "../../deps/ashita/plugins/sdk/Ashita.h"
#include "../backend/backend.hpp"
#include "../backend/streamlog.hpp"
#include "../bake/runner.hpp"
#include "../game/drawcull.hpp"
#include "../game/feed.hpp"
#include "../game/gamedraw.hpp"
#include "../game/glows.hpp"
#include "../game/hifps/exp.hpp"
#include "../game/hifps/step.hpp"
#include "../game/zonedraw.hpp"
#include "../game/zoneslot.hpp"
#include "../game/jitter.hpp"
#include "../game/charslot.hpp" // (native characters)
#include "../chars/marks.hpp"    // (native characters)
#include "../game/charmodel.hpp"  // (native characters)
#include "../chars/cache.hpp"     // (native characters)
#include "../chars/charframe.hpp"     // (native characters)
#include "../chars/charinject.hpp"    // (native characters)
#include "../chars/chardraw.hpp"      // (native characters: their lamps)
#include "../chars/charoptions.hpp"   // (native characters)
#include "../zone/zoneoptions.hpp"
#include "../zone/inject.hpp"
#include "../zone/light_split.hpp"
#include "../zone/native.hpp"
#include "../upscale/upscale.hpp"
#include "../upscale/masks.hpp"
#include "../framegen/framegen.hpp"
#include "../host/hooks.hpp"
#include "../render/worldfx_core.hpp"
#include "../ui/menu.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
	constexpr double kVersion = 0.53;

	IGuiManager *g_gui = nullptr;
	ILogManager *g_ashita_log = nullptr;
	IAshitaCore *g_core = nullptr;

	double now_seconds()
	{
		LARGE_INTEGER t, f;
		QueryPerformanceCounter(&t);
		QueryPerformanceFrequency(&f);
		return double(t.QuadPart) / double(f.QuadPart);
	}

	const char *format_name(uint32_t f)
	{
		switch (f)
		{
		case 28: return "R8G8B8A8_UNORM";
		case 27: return "R8G8B8A8_TYPELESS";
		case 87: return "B8G8R8A8_UNORM";
		case 88: return "B8G8R8X8_UNORM";
		case 45: return "D24_UNORM_S8_UINT";
		case 44: return "R24G8_TYPELESS";
		case 40: return "D32_FLOAT";
		case 39: return "R32_TYPELESS";
		case 0: return "none";
		default: return "other";
		}
	}

	// The game's entities for the game-side parts (src/game), through Ashita's
	class ashita_entities final : public hifi::game::entities
	{
		IEntity *e() const { return g_core != nullptr ? g_core->GetMemoryManager()->GetEntity() : nullptr; }

	public:
		const char *name(uint32_t i) const override { return e() != nullptr ? e()->GetName(i) : nullptr; }
		bool position(uint32_t i, float &x, float &y, float &z) const override
		{
			if (e() == nullptr)
				return false;
			x = e()->GetLocalPositionX(i);
			y = e()->GetLocalPositionY(i);
			z = e()->GetLocalPositionZ(i);
			return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
		}
		bool shown(uint32_t i) const override { return e() != nullptr && (e()->GetRenderFlags0(i) & 0x200) != 0; }
		uint32_t server_id(uint32_t i) const override { return e() != nullptr ? e()->GetServerId(i) : 0; }
		uint32_t status(uint32_t i) const override { return e() != nullptr ? e()->GetStatus(i) : 0; }
		float model_size(uint32_t i) const override { return e() != nullptr ? e()->GetModelSize(i) : 0.0f; }
	};
	const ashita_entities g_entities;

	// You, as the party data has you
	hifi::game::feed::party party_now()
	{
		hifi::game::feed::party p;
		if (g_core == nullptr)
			return p;
		IParty *party = g_core->GetMemoryManager()->GetParty();
		IPlayer *player = g_core->GetMemoryManager()->GetPlayer();
		p.target_index = party->GetMemberTargetIndex(0);
		p.server_id = party->GetMemberServerId(0);
		p.zone = party->GetMemberZone(0);
		p.in_game = player->GetLoginStatus() == 2 && player->GetIsZoning() == 0;
		return p;
	}

	// The backend's log lines into Ashita's log too
	void ashita_log(int level, const char *text)
	{
		if (ILogManager *const l = g_ashita_log)
			l->Log(uint32_t(level == 2 ? Ashita::LogLevel::Error : level == 1 ? Ashita::LogLevel::Warn : Ashita::LogLevel::Info), "hifixi", text);
	}

	// HiFi XI's menu through Ashita's ImGui
	const hifi::ui::gui &ashita_gui()
	{
		static const hifi::ui::gui gui = [] {
			hifi::ui::gui x;
			x.begin = [](const char *title, bool *open, float width, float height) {
				g_gui->SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
				return g_gui->Begin(title, open, 0);
			};
			x.end = [] { g_gui->End(); };
			x.begin_child = [](const char *id, float width, float height, bool border) {
				return g_gui->BeginChild(id, ImVec2(width, height), border ? ImGuiChildFlags_Borders : 0, 0);
			};
			x.end_child = [] { g_gui->EndChild(); };
			x.same_line = [] { g_gui->SameLine(); };
			x.separator = [] { g_gui->Separator(); };
			x.spacing = [] { g_gui->Spacing(); };
			x.set_next_item_width = [](float width) { g_gui->SetNextItemWidth(width); };
			x.available_width = [] { return g_gui->GetContentRegionAvail().x; };
			x.push_id = [](const char *id) { g_gui->PushID(id); };
			x.pop_id = [] { g_gui->PopID(); };
			x.begin_disabled = [](bool disabled) { g_gui->BeginDisabled(disabled); };
			x.end_disabled = [] { g_gui->EndDisabled(); };
			x.text = [](const char *s) { g_gui->TextUnformatted(s); };
			x.text_colored = [](const float rgba[4], const char *s) { g_gui->TextColored(ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]), "%s", s); };
			x.text_disabled = [](const char *s) {
				g_gui->PushTextWrapPos(0.0f);
				g_gui->TextDisabled("%s", s);
				g_gui->PopTextWrapPos();
			};
			x.text_wrapped = [](const char *s) { g_gui->TextWrapped("%s", s); };
			x.selectable = [](const char *label, bool selected) { return g_gui->Selectable(label, selected); };
			x.button = [](const char *label) { return g_gui->Button(label); };
			x.toggle_button = [](const char *label, bool selected) {
				// (the picked one in the accent colour, dark text on it)
				if (selected)
				{
					g_gui->PushStyleColor(ImGuiCol_Button, ImVec4(0.89f, 0.71f, 0.38f, 1.0f));
					g_gui->PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.94f, 0.78f, 0.47f, 1.0f));
					g_gui->PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.62f, 0.30f, 1.0f));
					g_gui->PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.09f, 0.07f, 1.0f));
				}
				const bool clicked = g_gui->Button(label);
				if (selected)
					g_gui->PopStyleColor(4);
				return clicked;
			};
			x.checkbox = [](const char *label, bool *v) { return g_gui->Checkbox(label, v); };
			x.slider_float = [](const char *label, float *v, float low, float high, const char *format) { return g_gui->SliderFloat(label, v, low, high, format); };
			x.slider_int = [](const char *label, int *v, int low, int high) { return g_gui->SliderInt(label, v, low, high); };
			x.color_edit3 = [](const char *label, float rgb[3]) { return g_gui->ColorEdit3(label, rgb, ImGuiColorEditFlags_NoInputs); };
			x.input_text = [](const char *id, const char *hint, char *buffer, size_t size) { return g_gui->InputTextWithHint(id, hint, buffer, size); };
			x.begin_combo = [](const char *id, const char *preview) { return g_gui->BeginCombo(id, preview, 0); };
			x.end_combo = [] { g_gui->EndCombo(); };
			x.begin_context = [](const char *id) { return g_gui->BeginPopupContextItem(id, ImGuiPopupFlags_MouseButtonRight); };
			x.end_popup = [] { g_gui->EndPopup(); };
			x.tooltip = [](const char *text) {
				if (!g_gui->IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					return;
				if (g_gui->BeginTooltip())
				{
					g_gui->PushTextWrapPos(420.0f);
					g_gui->TextUnformatted(text);
					g_gui->PopTextWrapPos();
					g_gui->EndTooltip();
				}
			};
			x.command = [](const char *text) {
				if (g_core != nullptr)
					g_core->GetChatManager()->QueueCommand(int32_t(Ashita::CommandMode::AshitaParse), text);
			};
			return x;
		}();
		return gui;
	}
}

class hifixi final : public IPlugin
{
	IAshitaCore *m_core = nullptr;
	ILogManager *m_log = nullptr;
	uint32_t m_id = 0;
	uint64_t m_presents = 0;
	double m_since = 0.0;
	bool m_menu = false;
	double m_next_update = 0.0;  // (the game-side parts follow their options every half second)
	uint32_t m_zone = 0;         // (the zone the zone files were last asked for)
	bool m_zoneslot_refused = false; // (the zone slot couldn't be marked: not tried again until its option goes off and on)
	bool m_native_refused = false;   // (likewise the native zone draw)
	bool m_upscale_refused = false;  // (likewise the upscale: the game's stretch stays)
	bool m_jitter_refused = false;   // (likewise the projection calls for FSR 2's jitter)
	// Whether the end markers sent are coming through to Direct3D 12 (the native draws are left to HiFi XI only then): over
	// the last kWindow presents, at least one ever seen and no more than 8 fewer seen than sent. (The totals since the start
	// were compared before: 97 markers lost once, at a zone change or a stall, left native drawing off for the rest of
	// the session, in game 2026-10-26.)
	struct marker_flow
	{
		static constexpr uint32_t kWindow = 32;
		uint64_t sent[kWindow] = {}, seen[kWindow] = {};
		uint32_t at = 0, filled = 0;
		bool update(uint64_t s, uint64_t e)
		{
			const uint32_t oldest = filled < kWindow ? 0 : at;
			const uint64_t s0 = filled != 0 ? sent[oldest] : s, e0 = filled != 0 ? seen[oldest] : e;
			sent[at] = s;
			seen[at] = e;
			at = (at + 1) % kWindow;
			filled = std::min(filled + 1, kWindow);
			const uint64_t ds = s - std::min(s, s0), de = e - std::min(e, e0);
			return s != 0 && e != 0 && de + 8 >= ds;
		}
	};
	marker_flow m_zone_flow, m_char_flow;
	bool m_casters_set = false;      // (the characters' sun shadows handed to World FX: worldfx::set_casters)
	bool m_zone_casters_set = false; // (the zone's sun shadow cascades handed to World FX: worldfx::set_zone_casters)
	bool m_charslot_refused = false; // (native characters: likewise the character slots)
	bool m_chardata_refused = false; // (native characters: likewise the model draw's stub)
	bool m_nativechars_refused = false; // (native characters: likewise their renderer)

	void say(const std::string &text, bool error = false)
	{
		if (m_core == nullptr)
			return;
		const std::string body = error ? Ashita::Chat::Error(text) : Ashita::Chat::Message(text);
		m_core->GetChatManager()->Writef(0, false, "%s%s", Ashita::Chat::Header("hifi").c_str(), body.c_str());
	}

	void start()
	{
		std::string why;
		if (hifi::backend::started())
			say(hifi::backend::early() ? "Started before the game's Direct3D 12 device (the starter): World FX runs without ReShade. /hifi for the menu."
				: "Running (started late).");
		else if (hifi::backend::late_start(why))
			say("Direct3D 12 hooks in, late: no starter (bootloader\\dxgi.dll), so the views dgVoodoo made before aren't known and World FX can't find the world.", true);
		else
			say("Direct3D 12 hooks not in: " + why, true);
		hifi::host::reset_stats();
		m_presents = 0;
		m_since = now_seconds();
	}

	void report()
	{
		for (const std::string &line : hifi::backend::status())
			say(line);
		if (!worldfx::forced_weather().empty()) // (weather debug: /hifi weather)
			say(worldfx::forced_weather_text());
		for (const std::string &line : hifi::game::feed::status())
			say(line);
		for (const auto &lines : { hifi::game::hifps::status(), hifi::game::hifps::exp::status(),
				 hifi::game::drawcull::status(), hifi::bake::runner::status().lines })
			for (const std::string &line : lines)
				say(line);
		say(hifi::game::gamedraw::status());
		say(hifi::game::zoneslot::status());
		say(hifi::game::charslot::status()); // (native characters)
		say(hifi::game::charmodel::status());
		for (const std::string &line : hifi::chars::cache::status())
			say(line);
		for (const std::string &line : hifi::chars::inject::status())
			say(line);
		for (const std::string &line : hifi::zone::native::status())
			say(line);
		say(hifi::game::zonedraw::status());
		for (const std::string &line : hifi::zone::inject::status())
			say(line);
		for (const std::string &line : hifi::upscale::status())
			say(line);
		say(hifi::game::jitter::status());
		for (const std::string &line : hifi::framegen::status())
			say(line);
		if (!hifi::host::installed())
		{
			say("The hooks aren't in (/hifi on).", true);
			return;
		}
		const hifi::host::stats s = hifi::host::get_stats();
		const double secs = now_seconds() - m_since;
		const double frames = m_presents != 0 ? double(m_presents) : 1.0;
		char line[320];
		std::snprintf(line, sizeof(line), "%.1f s, %llu frames (%.1f fps); lists submitted %llu (%.1f a frame), not hooked %llu; list vtables hooked %llu, refused %llu; lists known %llu; calls on lists in an unknown state %llu; queues %u",
			secs, static_cast<unsigned long long>(m_presents), secs > 0 ? m_presents / secs : 0.0, static_cast<unsigned long long>(s.lists_executed),
			s.lists_executed / frames, static_cast<unsigned long long>(s.lists_foreign), static_cast<unsigned long long>(s.vtables_hooked),
			static_cast<unsigned long long>(s.vtables_refused), static_cast<unsigned long long>(s.lists_seen),
			static_cast<unsigned long long>(s.lists_unknown_state), s.queues_seen);
		say(line);
		std::snprintf(line, sizeof(line), "world: %llux%u %s, its end seen %llu times (%.2f a frame); depth: %llux%u %s",
			static_cast<unsigned long long>(s.world.width), s.world.height, format_name(s.world.format), static_cast<unsigned long long>(s.world.ends),
			s.world.ends / frames, static_cast<unsigned long long>(s.depth.width), s.depth.height, format_name(s.depth.format));
		say(line);
		std::string calls = "calls a frame:";
		for (uint32_t i = 0; i < hifi::host::slot_count; ++i)
		{
			char item[80];
			std::snprintf(item, sizeof(item), " %s %.1f;", hifi::host::slot_name(i), s.calls[i] / frames);
			calls += item;
			if (calls.size() > 180)
			{
				say(calls);
				calls = " ";
			}
		}
		if (calls.size() > 1)
			say(calls);
	}

	// /hifi weather [<name>|off|flash]: World FX's own weather forced to a weather (debug, not saved: worldfx::force_weather),
	// back to the real one, or a flash of lightning now; without one, what's forced and the names
	void weather(std::string rest)
	{
		while (!rest.empty() && rest.front() == ' ')
			rest.erase(rest.begin());
		while (!rest.empty() && rest.back() == ' ')
			rest.pop_back();
		if (rest == "flash")
		{
			worldfx::flash_lightning();
			say("Lightning now (World FX's own, while it draws the zone's weather).");
			return;
		}
		const bool known = rest.empty() || worldfx::force_weather(rest.c_str());
		if (!known)
			say("/hifi weather: no weather \"" + rest + "\".", true);
		say(worldfx::forced_weather_text());
		if (rest.empty() || !known)
		{
			size_t n = 0;
			const worldfx::weather_choice *c = worldfx::weather_choices(n);
			std::string names = "/hifi weather <name>|off|flash:";
			for (size_t i = 0; i < n; ++i)
				names += std::string(" ") + c[i].name + " (" + c[i].label + ")" + (i + 1 < n ? "," : ".");
			say(names);
		}
	}

	// /hifi gamedraw [zone|chars] [on|off]: one part of the game's own drawing switched (toggled without on|off), or
	// what's off now
	void gamedraw(std::string rest)
	{
		while (!rest.empty() && rest.front() == ' ')
			rest.erase(rest.begin());
		const size_t space = rest.find(' ');
		const std::string which = rest.substr(0, space), how = space == std::string::npos ? std::string() : rest.substr(space + 1);
		using hifi::game::gamedraw::part;
		if (which.empty())
		{
			say(hifi::game::gamedraw::status());
			return;
		}
		if ((which != "zone" && which != "chars") || (!how.empty() && how != "on" && how != "off"))
		{
			say("/hifi gamedraw zone|chars [on|off]: the game's own zone or character drawing on or off (toggled without on|off)");
			return;
		}
		const part p = which == "zone" ? part::zone : part::chars;
		// (native characters: while the model draw's stub is in, it's the one that leaves the draw)
		if (p == part::chars && hifi::game::charmodel::in())
		{
			const bool chars_off = how.empty() ? hifi::game::charmodel::game_draws() : how == "off";
			hifi::game::charmodel::set_game_draws(!chars_off);
			say(std::string("The game's own character drawing ") + (chars_off ? "OFF" : "on") + " (through the model draw's stub).");
			return;
		}
		const bool off = how.empty() ? !hifi::game::gamedraw::is_off(p) : how == "off";
		std::string why;
		if (!hifi::game::gamedraw::set(p, off, why))
			say("The game's " + std::string(p == part::zone ? "zone" : "character") + " drawing not switched: " + why, true);
		else
			say(hifi::game::gamedraw::status());
	}

	// The zone pass's slot markers in (once FFXiMain is there: the native zone draw needs them), and sent only while the
	// Direct3D 12 side is attached to see them
	void follow_zoneslot()
	{
		std::string why;
		if (!hifi::game::zoneslot::in() && !m_zoneslot_refused && hifi::game::image_known())
		{
			if (hifi::game::zoneslot::start(why))
				hifi::backend::log(0, "HiFi XI: the zone pass's slot marked (178CCE through our stub)");
			else
			{
				m_zoneslot_refused = true; // (not tried again: the game draws its zone itself)
				hifi::backend::log(1, ("HiFi XI: the zone pass's slot not marked: " + why).c_str());
			}
		}
		hifi::game::zoneslot::set_wanted(hifi::game::zoneslot::in() && hifi::backend::started() && hifi::host::installed() && worldfx::attached() != nullptr);
	}

	// (native characters) The character passes' slot markers in (the character data needs their frames), sent only while
	// the Direct3D 12 side is attached to see them
	void follow_charslot()
	{
		std::string why;
		if (!hifi::game::charslot::in() && !m_charslot_refused && hifi::game::image_known())
		{
			if (hifi::game::charslot::start(why))
				hifi::backend::log(0, "HiFi XI: the character passes' slots marked (0817E0 and 0C7E7C through our stubs)");
			else
			{
				m_charslot_refused = true; // (not tried again: the game draws its characters itself)
				hifi::backend::log(1, ("HiFi XI: the character passes' slots not marked: " + why).c_str());
			}
		}
		hifi::game::charslot::set_wanted(hifi::game::charslot::in() && hifi::backend::started() && hifi::host::installed() && worldfx::attached() != nullptr);
	}

	// (native characters) The model draw's stub in (after the character slots), taking over /hifi gamedraw chars's
	// state; the cache's device (while the Direct3D 12 side is attached) and its forgetting
	void follow_chardata(double now)
	{
		namespace cm = hifi::game::charmodel;
		using hifi::game::gamedraw::part;
		std::string why;
		if (!cm::in() && !m_chardata_refused && hifi::game::charslot::in())
		{
			const bool game_off = hifi::game::gamedraw::is_off(part::chars);
			if (game_off && !hifi::game::gamedraw::set(part::chars, false, why))
				m_chardata_refused = true;
			else if (cm::start(why))
			{
				cm::set_game_draws(!game_off);
				hifi::backend::log(0, "HiFi XI: the character models recorded (02B7E3 through our stub)");
			}
			else
			{
				m_chardata_refused = true;
				if (game_off)
				{
					std::string ignored;
					hifi::game::gamedraw::set(part::chars, true, ignored);
				}
			}
			if (m_chardata_refused)
				hifi::backend::log(1, ("HiFi XI: the character models not recorded: " + why).c_str());
		}
		if (cm::in())
			hifi::chars::cache::frame_tick(hifi::backend::started() && hifi::host::installed() ? worldfx::attached() : nullptr, now);
	}

	// (native characters) The en-spell elements' names for /hifi enspell, in charframe.hpp's kEn* order
	static constexpr const char *kEnspellNames[] = { "off", "fire", "ice", "wind", "earth", "thunder", "water", "light", "dark" };

	// The en-spell element of a status effect (Phoenix sql/spell_list.sql: Enfire .. Enwater 94-99, their IIs
	// 277-282, Enlight 274, Endark 288), kEnNone for any other
	static uint8_t enspell_of(int16_t effect)
	{
		if (effect >= 94 && effect <= 99)
			return uint8_t(hifi::chars::kEnFire + (effect - 94));
		if (effect >= 277 && effect <= 282)
			return uint8_t(hifi::chars::kEnFire + (effect - 277));
		return effect == 274 ? hifi::chars::kEnLight : effect == 288 ? hifi::chars::kEnDark : hifi::chars::kEnNone;
	}

	// (native characters) The en-spell effect on your own model's blades ([chars] enspell: auto = the element of the
	// en-spell on you, off, or an element's name = always that one), handed to the model recording as your actor
	void follow_enspell()
	{
		uint8_t element = hifi::chars::kEnNone;
		uintptr_t actor = 0;
		const std::string mode = hifi::game::option_text("chars", "enspell", "auto");
		if (g_core != nullptr && mode != "off")
		{
			IParty *const party = g_core->GetMemoryManager()->GetParty();
			IPlayer *const player = g_core->GetMemoryManager()->GetPlayer();
			IEntity *const entity = g_core->GetMemoryManager()->GetEntity();
			const uint32_t me = party->GetMemberTargetIndex(0);
			actor = me != 0 ? entity->GetActorPointer(me) : 0;
			for (uint8_t k = hifi::chars::kEnFire; k <= hifi::chars::kEnDark; ++k)
				if (mode == kEnspellNames[k] || (k == hifi::chars::kEnFire && mode == "on"))
					element = k;
			if (const int16_t *buffs = player->GetBuffs(); buffs != nullptr && element == hifi::chars::kEnNone && mode == "auto")
				for (int i = 0; i < 32 && element == hifi::chars::kEnNone; ++i)
					element = enspell_of(buffs[i]);
			if (actor == 0)
				element = hifi::chars::kEnNone;
		}
		hifi::game::charmodel::set_enspell(actor, element);
	}

	// (native characters) HiFi XI's own character draw (its renderer made for the game's device first), its per-pixel
	// option; and whether models are left to it now: their data recorded, the renderer ready, and the characters' end
	// markers coming through (no more than 8 behind those sent)
	void follow_nativechars()
	{
		std::string why;
		const bool want = hifi::game::charmodel::in();
		ID3D12Device *const device = worldfx::attached();
		if (want && !m_nativechars_refused && device != nullptr && !hifi::chars::inject::ready())
		{
			if (!hifi::chars::inject::prepare(device, why))
			{
				m_nativechars_refused = true; // (not tried again: the game draws its characters itself)
				hifi::backend::log(1, ("HiFi XI: the native character renderer not made: " + why).c_str());
			}
		}
		hifi::chars::inject::set_options(hifi::game::option_on("chars", "perpixel", hifi::chars::options::kPerPixelByDefault));
		// (the zone's sun, for the light split's share of their light: the zone pass's of the last quarter second)
		{
			hifi::zone::inject::frame_camera c;
			const bool known = hifi::zone::inject::last_camera(c) && GetTickCount64() - c.at <= 250;
			hifi::chars::inject::set_sun(known ? c.sun_dir : nullptr);
			// (and its lamps, when it drew the zone itself: the characters lit by the lamps the zone is lit by)
			static_assert(sizeof(hifi::zone::frame_lamp) == sizeof(hifi::chars::char_lamp) && hifi::zone::kMaxLamps == hifi::chars::kFrameLamps);
			hifi::zone::frame_lamp lamps[hifi::zone::kMaxLamps];
			const uint32_t lamp_count = known ? hifi::zone::inject::last_lamps(lamps) : 0;
			hifi::chars::inject::set_lamps(lamp_count != 0 ? &lamps[0].pos[0] : nullptr, lamp_count);
		}
		// (their sun shadows from their meshes: World FX calls for them in its shading pass, once its renderer is up)
		if (!m_casters_set && hifi::chars::inject::ready())
		{
			worldfx::set_casters(all_casters, nullptr);
			m_casters_set = true;
		}
		const bool flowing = m_char_flow.update(hifi::game::charslot::get_counts().sent[3], hifi::chars::marks::get_counts().end);
		hifi::game::charmodel::set_native(want && flowing && hifi::chars::inject::ready());
	}

	// The native zone draw in (its renderer made for the game's device first), its per-pixel option; and each pass's
	// gate inputs: the zone's data, the Direct3D 12 side's readiness (its renderer, and the zone's end markers coming
	// through: no more than 8 behind the ones sent)
	void follow_native()
	{
		std::string why;
		// (the sun's shadow cascades from the zone this draws, once it draws one: World FX asks each frame it draws them)
		if (!m_zone_casters_set)
		{
			worldfx::set_zone_casters(hifi::zone::inject::zone_bounds_hook(), hifi::zone::inject::zone_draw_hook(), nullptr);
			worldfx::set_camera_source(frame_camera, nullptr);
			// (and the light split the zone and characters draw: World FX takes it in its shading pass)
			worldfx::set_light_source([](void *) { return hifi::light::take(); }, nullptr);
			m_zone_casters_set = true;
			// (and the zone's moving layer, with the characters, into World FX's casters' map each frame)
			if (!m_casters_set)
			{
				worldfx::set_casters(all_casters, nullptr);
				m_casters_set = true;
			}
		}
		ID3D12Device *const device = worldfx::attached();
		if (!hifi::game::zonedraw::in() && !m_native_refused && device != nullptr && hifi::game::zoneslot::in())
		{
			if (!hifi::zone::inject::prepare(device, why) || !hifi::game::zonedraw::start(why))
			{
				m_native_refused = true; // (not tried again: the game draws its zone itself)
				hifi::backend::log(1, ("HiFi XI: the native zone draw not started: " + why).c_str());
			}
			else
				hifi::backend::log(0, "HiFi XI: the native zone draw in (17CBA8 through our stub; the renderer made)");
		}
		hifi::zone::inject::set_options(hifi::game::option_on("zone", "perpixel", hifi::zone::options::kPerPixelByDefault));
	}
	// World FX's casters each frame (worldfx::set_casters): the characters and monsters from their meshes, then the zone's
	// moving layer (its swaying plants, doors and elevators) into the same map. Whether either drew.
	static bool all_casters(void *, ID3D12GraphicsCommandList *list, const float world_to_clip[16], D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT size)
	{
		const bool chars = hifi::chars::inject::record_casters(list, world_to_clip, dsv, size);
		const bool zone = hifi::zone::inject::record_zone_movers(list, world_to_clip, dsv, size);
		return chars || zone;
	}
	// The camera of the frame dgVoodoo is finishing, for World FX: the zone pass's (its frame record, with the jitter it
	// was drawn with) when one was recorded in the last quarter second
	static bool frame_camera(void *, float view[16], float proj[16])
	{
		hifi::zone::inject::frame_camera c;
		if (!hifi::zone::inject::last_camera(c) || GetTickCount64() - c.at > 250)
			return false;
		std::memcpy(view, c.view, 64);
		std::memcpy(proj, c.proj, 64);
		return true;
	}

	// The world upscaled to the screen (src/upscale): its options, and its passes made for the game's device (with what
	// dgVoodoo's thread has asked for since: the screen-size texture, a pipeline for the back buffer's format)
	void follow_upscale()
	{
		const bool fsr = hifi::game::option_on("upscale", "fsr", true);
		hifi::upscale::set_options(fsr && !m_upscale_refused, hifi::game::option_on("upscale", "temporal", true), hifi::game::option_number("upscale", "sharpen", 0.8f),
			hifi::game::option_on("upscale", "fsr3", true));
		hifi::framegen::set_on(fsr && hifi::game::option_on("upscale", "framegen", false));
		hifi::framegen::set_automatic(hifi::game::option_on("upscale", "framegen_auto", true));
		// (FSR's masks: the world copied at World FX's passes as well as at the characters' end)
		hifi::upscale::masks::set_on(hifi::game::option_on("upscale", "masks", true));
		// (the characters' own motion for FSR: drawn while it runs)
		hifi::upscale::set_chars_motion_source(&hifi::chars::inject::motion);
		ID3D12Device *const device = worldfx::attached();
		std::string why;
		if (fsr && !m_upscale_refused && device != nullptr && !hifi::upscale::prepare(device, why))
		{
			m_upscale_refused = true; // (not tried again: the game's stretch stays)
			hifi::backend::log(1, ("HiFi XI: the upscale passes not made: " + why).c_str());
		}
	}

	// The four projection calls through ours for FSR 2's jitter (src/game/jitter), once FFXiMain is there; offsets only
	// while FSR 2 draws (Direct3DPresent)
	void follow_jitter()
	{
		std::string why;
		if (!hifi::game::jitter::in() && !m_jitter_refused && hifi::game::image_known())
		{
			if (hifi::game::jitter::start(why))
				hifi::backend::log(0, "HiFi XI: the projection calls through ours (FSR 2's jitter)");
			else
			{
				m_jitter_refused = true;
				hifi::backend::log(1, ("HiFi XI: the projection calls not taken (no jitter for FSR 2): " + why).c_str());
			}
		}
	}

	void native_gate()
	{
		const bool want = hifi::game::zonedraw::in();
		const bool flowing = m_zone_flow.update(hifi::game::zoneslot::get_counts().sent[1], hifi::backend::zone_ends_seen());
		hifi::game::zonedraw::set_state(want, hifi::zone::native::ready_zone(0), hifi::zone::native::ready_zone(1), flowing && hifi::zone::inject::ready());
	}

	// /hifi streamlog [frames] [label]: the next world frame(s) as dgVoodoo records them, and the game's draws of them,
	// into a file beside hifixi.log (written a frame or two later, when it says so)
	void streamlog(std::string rest)
	{
		using hifi::game::gamedraw::is_off;
		using hifi::game::gamedraw::part;
		if (!hifi::backend::started() || !hifi::host::installed())
		{
			say("The Direct3D 12 hooks aren't in: there's nothing to log.", true);
			return;
		}
		uint32_t frames = 1;
		std::string label;
		while (!rest.empty())
		{
			while (!rest.empty() && rest.front() == ' ')
				rest.erase(rest.begin());
			const size_t space = rest.find(' ');
			const std::string word = rest.substr(0, space);
			rest = space == std::string::npos ? std::string() : rest.substr(space + 1);
			if (word.empty())
				continue;
			if (word.find_first_not_of("0123456789") == std::string::npos && word.size() <= 2)
				frames = uint32_t(std::stoul(word));
			else
				label += (label.empty() ? "" : "-") + word;
		}
		if (frames < 1 || frames > 8)
		{
			say("/hifi streamlog [frames 1-8] [label]: the next world frame(s) as dgVoodoo records them, into bootloader\\hifixi-stream-<time>-<label>.txt");
			return;
		}
		if (label.empty())
			label = std::string("zone-") + (is_off(part::zone) ? "off" : "on") + "-chars-" + (is_off(part::chars) ? "off" : "on");
		char version[32];
		std::snprintf(version, sizeof(version), "HiFi XI %.2f", kVersion);
		const std::string context = std::string(version) + "; " + hifi::game::gamedraw::status() + "; World FX " +
			(worldfx_drawing() != 0 ? "draws into the game's world" : "doesn't draw (no world found yet)") + "\n";
		if (!hifi::backend::streamlog::request(frames, label, context))
		{
			say("A stream log is being made already.", true);
			return;
		}
		say("Stream log: the next " + std::to_string(frames) + " world frame(s) go into bootloader\\hifixi-stream-<time>-" + label + ".txt");
	}

public:
	const char *GetName() const override { return "hifixi"; }
	const char *GetAuthor() const override { return "Relli"; }
	const char *GetDescription() const override { return "HiFi XI: higher frame rates, longer view distance and a richer world, in one plugin."; }
	const char *GetLink() const override { return "https://github.com/relliko/hifixi"; }
	double GetVersion() const override { return kVersion; }
	double GetInterfaceVersion() const override { return ASHITA_INTERFACE_VERSION; }
	int32_t GetPriority() const override { return 0; }
	uint32_t GetFlags() const override
	{
		return uint32_t(Ashita::PluginFlags::UseCommands) | uint32_t(Ashita::PluginFlags::UseDirect3D) | uint32_t(Ashita::PluginFlags::UsePackets);
	}

	bool Initialize(IAshitaCore *core, ILogManager *logger, const uint32_t id) override
	{
		m_core = core;
		m_log = logger;
		m_id = id;
		g_core = core;
		g_gui = core->GetGuiManager();
		g_ashita_log = logger;
		// Pinned: dgVoodoo's threads call into this DLL's hooks, so it stays loaded even after Ashita unloads the plugin
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&now_seconds), &self);
		hifi::backend::set_log_sink(ashita_log);
		hifi::game::hifps::exp::set_ashita(core);
		// (the characters' visibility test: World FX's VisTest takes it from walls, and gives it back)
		hifi::game::feed::set_actor_handoff(hifi::game::hifps::exp::set_actors_elsewhere, hifi::game::hifps::exp::walls_holding);
		// (the stream log's view of the game's draws: a tap on the client device, the hifps proxy's slots when it's in)
		hifi::backend::streamlog::set_d3d8_tap(hifi::game::hifps::exp::draw_tap, hifi::game::hifps::exp::draw_untap);
		// (the zone pass's begin and end to the native zone draw)
		hifi::game::zoneslot::set_listener(hifi::game::zonedraw::on_slot);
		hifi::game::charslot::set_listener(hifi::game::charmodel::on_slot); // (native characters)
		start();
		hifi::bake::runner::start(hifi::backend::folder());
		return true;
	}

	void Release() override
	{
		// (the game's own drawing back first, while the log still takes the reason it can't be)
		hifi::backend::streamlog::set_d3d8_tap(nullptr, nullptr);
		std::string why;
		if (!hifi::game::gamedraw::stop(why))
			ashita_log(1, ("the game's own drawing not put back: " + why).c_str());
		// (the game draws its zone again first; the renderer's objects are left: dgVoodoo's lists may still hold them)
		if (!hifi::game::zonedraw::stop(why))
			ashita_log(1, ("the native zone draw not taken out: " + why).c_str());
		hifi::game::zoneslot::set_listener(nullptr);
		// (the zone's data stays: the renderer may still be drawing with it on dgVoodoo's lists)
		hifi::game::zoneslot::set_wanted(false);
		if (!hifi::game::jitter::stop(why))
			ashita_log(1, ("the projection calls not put back: " + why).c_str());
		if (!hifi::game::zoneslot::stop(why))
			ashita_log(1, ("the zone pass's slot markers not taken out: " + why).c_str());
		// (native characters: the game draws every model again, then the model draw's stub out, the game's own drawing back
		// with it; the renderer's objects are left: dgVoodoo's lists may still hold them)
		hifi::game::charmodel::set_native(false);
		if (!hifi::game::charmodel::stop(why))
			ashita_log(1, ("the model draw's stub not taken out: " + why).c_str());
		hifi::chars::clear_frames();
		hifi::chars::cache::stop();
		hifi::game::charslot::set_wanted(false);
		if (!hifi::game::charslot::stop(why))
			ashita_log(1, ("the character slot markers not taken out: " + why).c_str());
		hifi::backend::set_log_sink(nullptr);
		g_ashita_log = nullptr;
		m_menu = false;
		hifi::bake::runner::stop();
		hifi::game::feed::stop();
		hifi::game::hifps::exp::stop();
		hifi::game::hifps::stop();
		hifi::game::drawcull::stop();
		hifi::game::hifps::exp::set_ashita(nullptr);
		g_core = nullptr;
		hifi::backend::stop();
	}

	bool HandleCommand(int32_t mode, const char *command, bool injected) override
	{
		UNREFERENCED_PARAMETER(mode);
		UNREFERENCED_PARAMETER(injected);
		if (command == nullptr || _strnicmp(command, "/hifi", 5) != 0 || (command[5] != '\0' && command[5] != ' '))
			return false;
		std::string arg = command + 5;
		while (!arg.empty() && arg.front() == ' ')
			arg.erase(arg.begin());
		if (arg == "reset")
		{
			hifi::host::reset_stats();
			m_presents = 0;
			m_since = now_seconds();
			say("Counts started again.");
		}
		else if (arg == "status")
			report();
		else if (arg == "gpu")
		{
			// (World FX's GPU time by section, the costliest first)
			for (const std::string &line : worldfx::gpu_times())
				say(line);
		}
		else if (arg == "glows")
		{
			// (what lights the scene now: the glowing fixtures, then the effects, nearest first)
			say(hifi::game::glows::status());
			for (const std::string &line : hifi::game::glows::list(g_entities))
				say("  " + line);
		}
		else if (arg == "gamedraw" || arg.rfind("gamedraw ", 0) == 0)
			gamedraw(arg.substr(8));
		else if (arg == "streamlog" || arg.rfind("streamlog ", 0) == 0)
			streamlog(arg.substr(9));
		else if (arg == "nativezone" || arg.rfind("nativezone ", 0) == 0)
		{
			const std::string how = arg.size() > 11 ? arg.substr(11) : std::string();
			if (how == "perpixel on" || how == "perpixel off")
				hifi::game::set_option_on("zone", "perpixel", how == "perpixel on");
			else if (!how.empty())
				say("/hifi nativezone [perpixel on|off]: what HiFi XI's own zone drawing is doing (the game draws what it can't), with its per-pixel lighting or the game's");
			follow_native();
			say(hifi::game::zonedraw::status());
			for (const std::string &line : hifi::zone::inject::status())
				say(line);
		}
		else if (arg == "zonedata")
		{
			say("Native zone data: each zone's meshes, textures and normal maps go on the GPU as it's entered.");
			for (const std::string &line : hifi::zone::native::status())
				say(line);
		}
		else if (arg == "zoneslot")
		{
			say(hifi::game::zoneslot::status());
		}
		else if (arg == "charslot") // (native characters)
		{
			say(hifi::game::charslot::status());
			say(hifi::chars::marks::status());
		}
		else if (arg == "chardata") // (native characters)
		{
			say("Native character data: each model the game draws is recorded, its meshes and textures put on the GPU as they're first seen.");
			say(hifi::game::charmodel::status());
			for (const std::string &line : hifi::chars::cache::status())
				say(line);
		}
		else if (arg == "nativechars" || arg == "nativechars perpixel" || arg == "nativechars perpixel on" || arg == "nativechars perpixel off") // (native characters)
		{
			if (arg != "nativechars")
			{
				const bool on = arg == "nativechars perpixel" ? !hifi::game::option_on("chars", "perpixel", hifi::chars::options::kPerPixelByDefault) : arg == "nativechars perpixel on";
				hifi::game::set_option_on("chars", "perpixel", on);
				follow_nativechars();
			}
			say(std::string("Native characters: HiFi XI draws each model whose data is on the GPU, the game the rest") +
				(hifi::game::option_on("chars", "perpixel", hifi::chars::options::kPerPixelByDefault) ? "; lit per pixel." : "; lit as the game lights them."));
			say(hifi::game::charmodel::status());
			for (const std::string &line : hifi::chars::inject::status())
				say(line);
		}
		else if (arg == "enspell" || arg.rfind("enspell ", 0) == 0) // (native characters)
		{
			const std::string want = arg.size() > 8 ? arg.substr(8) : std::string();
			bool known = want.empty() || want == "auto";
			for (const char *name : kEnspellNames)
				known = known || want == name;
			if (!known)
				say("/hifi enspell auto|off|fire|ice|wind|earth|thunder|water|light|dark");
			else
			{
				if (!want.empty())
					hifixi_set_option("chars", "enspell", want.c_str());
				follow_enspell();
				const std::string chosen = hifi::game::option_text("chars", "enspell", "auto");
				say(chosen == "auto" ? "En-spell effect: on your blades while an en-spell is on you, in its element." :
					chosen == "off" ? "En-spell effect: off." : "En-spell effect: always " + chosen + " on your blades (/hifi enspell auto for the real one).");
			}
		}
		else if (arg == "upscale" || arg.rfind("upscale ", 0) == 0)
		{
			const std::string how = arg.size() > 8 ? arg.substr(8) : std::string();
			if (how == "on" || how == "off")
				hifi::game::set_option_on("upscale", "fsr", how == "on");
			else if (how.rfind("sharpen ", 0) == 0)
				hifixi_set_option("upscale", "sharpen", how.substr(8).c_str());
			else if (how == "framegen on" || how == "framegen off")
				hifi::game::set_option_on("upscale", "framegen", how == "framegen on");
			else if (!how.empty())
				say("/hifi upscale [on|off] [sharpen <0..1>] [framegen on|off]: the world upscaled to the screen with AMD FSR; frame generation (on from the next start)");
			follow_upscale();
			for (const std::string &line : hifi::upscale::status())
				say(line);
			for (const std::string &line : hifi::framegen::status())
				say(line);
		}
		else if (arg == "weather" || arg.rfind("weather ", 0) == 0)
			weather(arg.substr(7));
		else if (arg.empty() || arg == "menu")
			m_menu = !m_menu;
		else if (arg == "off")
		{
			hifi::backend::stop();
			say("Direct3D 12 hooks out.");
		}
		else if (arg == "on")
		{
			std::string why;
			if (!hifi::backend::started())
				start();
			else if (hifi::backend::resume(why))
				say("Direct3D 12 hooks back in.");
			else
				say("Direct3D 12 hooks not in: " + why, true);
		}
		else
			say("/hifi: the menu; /hifi status: what HiFi XI has seen; /hifi glows: the glows lighting the scene now; /hifi gpu: World FX's GPU time by section; /hifi gamedraw zone|chars [on|off]: the game's own drawing off (debug); /hifi streamlog [frames] [label]: the next world frame call by call, into a file (debug); /hifi zoneslot: the zone pass's markers; /hifi charslot: the character passes' markers; /hifi chardata: the characters' data on the GPU; /hifi nativechars [perpixel [on|off]]: HiFi XI drawing the characters; /hifi enspell [auto|off|<element>]: the en-spell effect on your blades; /hifi zonedata: each zone's data on the GPU; /hifi nativezone [perpixel on|off]: HiFi XI drawing the zone; /hifi weather <name>|off|flash: World FX's own weather forced (debug); /hifi reset: its counts again; /hifi off|on: its hooks out or in.");
		return true;
	}

	// The action packet (weapon skills cutting the grass) and the zone-in packet (a Mog House), read only
	bool HandleIncomingPacket(uint16_t id, uint32_t size, const uint8_t *data, uint8_t *modified, uint32_t size_chunk, const uint8_t *data_chunk, bool injected, bool blocked) override
	{
		UNREFERENCED_PARAMETER(modified);
		UNREFERENCED_PARAMETER(size_chunk);
		UNREFERENCED_PARAMETER(data_chunk);
		UNREFERENCED_PARAMETER(injected);
		if (!blocked && hifi::backend::started() && hifi::host::installed())
			hifi::game::feed::packet_in(id, data, size, g_entities);
		return false;
	}

	bool Direct3DInitialize(IDirect3DDevice8 *device) override
	{
		UNREFERENCED_PARAMETER(device);
		return true;
	}

	void Direct3DPresent(const RECT *, const RECT *, HWND, const RGNDATA *) override
	{
		m_presents++;
		const double now = now_seconds();
		// The frame rate first (the cap waits here, before the frame is shown), then the game-side parts: hifps's,
		// and drawcull's work, each following its options every half second
		hifi::game::hifps::present(now);
		hifi::game::hifps::exp::present(now);
		hifi::game::drawcull::frame(now);
		// (the next frame's offset, before the camera tick builds its projection: only while FSR 2 draws to take it back out)
		hifi::game::jitter::set_on(hifi::upscale::temporal_running());
		hifi::chars::inject::set_motion_wanted(hifi::upscale::temporal_running()); // (the characters' own motion for FSR)
		worldfx::set_blade_motion(hifi::upscale::temporal_running()); // (the grass's own motion for FSR 2)
		hifi::game::jitter::set_phases(hifi::upscale::jitter_phases());
		hifi::game::jitter::next_frame();
		hifi::framegen::present(); // (frame generation on or off as FSR 2 runs and the setting says)
		if (now >= m_next_update)
		{
			m_next_update = now + 0.5;
			hifi::game::hifps::update();
			hifi::game::hifps::exp::update();
			hifi::game::drawcull::update();
			follow_zoneslot();
			follow_charslot(); // (native characters)
			follow_chardata(now);
			follow_nativechars();
			follow_enspell();
			follow_native();
			follow_upscale();
			follow_jitter();
		}
		// (the game's parts while World FX runs here: the worldfx Ashita addon's work; the zone's files made the first
		// time it's entered)
		if (hifi::backend::started() && hifi::host::installed())
		{
			const hifi::game::feed::party p = party_now();
			hifi::game::feed::frame(g_entities, p, now);
			// (the native zone renderer's data for the zone and its second map)
			hifi::zone::native::frame(worldfx::attached(), p.in_game ? hifi::game::feed::zone() : 0, p.in_game ? hifi::game::zonedraw::second_map() : 0, true);
			native_gate();
			// (only while World FX draws into the game's world: on another output its files would go unused)
			if (const uint32_t zone = hifi::game::feed::zone(); zone != m_zone && worldfx_drawing() != 0)
			{
				m_zone = zone;
				hifi::bake::runner::zone_entered(zone);
			}
		}
		hifi::backend::present();
		if (const std::string line = hifi::backend::streamlog::present(); !line.empty())
			say(line);
		if (g_gui != nullptr && hifi::game::option_on("hifps", "counter", true) && hifi::game::hifps::fps() > 0.0)
		{
			// (the frame rate, top left)
			g_gui->SetNextWindowPos(ImVec2(6.0f, 4.0f), ImGuiCond_Always);
			g_gui->SetNextWindowBgAlpha(0.0f);
			if (g_gui->Begin("##hifixi_fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
					ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
				g_gui->Text("%.0f", hifi::game::hifps::fps());
			g_gui->End();
		}
		if (m_menu && g_gui != nullptr)
			hifi::ui::draw_menu(ashita_gui(), &m_menu);
	}
};

extern "C" double __stdcall expGetInterfaceVersion(void)
{
	return ASHITA_INTERFACE_VERSION;
}

extern "C" IPlugin *__stdcall expCreatePlugin(const char *args)
{
	UNREFERENCED_PARAMETER(args);
	return new hifixi();
}

extern "C" void __stdcall expDestroyPlugin(IPlugin *instance)
{
	delete instance;
}
