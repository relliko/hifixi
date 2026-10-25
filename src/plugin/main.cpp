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
 *   /hifi reset    starts the counts again
 *   /hifi off|on   takes the hooks out, or puts them back
 */

#include "../../deps/ashita/plugins/sdk/Ashita.h"
#include "../backend/backend.hpp"
#include "../bake/runner.hpp"
#include "../game/drawcull.hpp"
#include "../game/feed.hpp"
#include "../game/gamedraw.hpp"
#include "../game/glows.hpp"
#include "../game/hifps/exp.hpp"
#include "../game/hifps/step.hpp"
#include "../host/hooks.hpp"
#include "../render/worldfx_core.hpp"
#include "../ui/menu.hpp"
#include <cmath>
#include <cstdio>
#include <string>

namespace
{
	constexpr double kVersion = 0.20;

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
			x.text_disabled = [](const char *s) { g_gui->TextDisabled("%s", s); };
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
		for (const std::string &line : hifi::game::feed::status())
			say(line);
		for (const auto &lines : { hifi::game::hifps::status(), hifi::game::hifps::exp::status(),
				 hifi::game::drawcull::status(), hifi::bake::runner::status().lines })
			for (const std::string &line : lines)
				say(line);
		say(hifi::game::gamedraw::status());
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
		const bool off = how.empty() ? !hifi::game::gamedraw::is_off(p) : how == "off";
		std::string why;
		if (!hifi::game::gamedraw::set(p, off, why))
			say("The game's " + std::string(p == part::zone ? "zone" : "character") + " drawing not switched: " + why, true);
		else
			say(hifi::game::gamedraw::status());
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
		start();
		hifi::bake::runner::start(hifi::backend::folder());
		return true;
	}

	void Release() override
	{
		// (the game's own drawing back first, while the log still takes the reason it can't be)
		std::string why;
		if (!hifi::game::gamedraw::stop(why))
			ashita_log(1, ("the game's own drawing not put back: " + why).c_str());
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
			say("/hifi: the menu; /hifi status: what HiFi XI has seen; /hifi glows: the glows lighting the scene now; /hifi gpu: World FX's GPU time by section; /hifi gamedraw zone|chars [on|off]: the game's own drawing off (debug); /hifi reset: its counts again; /hifi off|on: its hooks out or in.");
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
		if (now >= m_next_update)
		{
			m_next_update = now + 0.5;
			hifi::game::hifps::update();
			hifi::game::hifps::exp::update();
			hifi::game::drawcull::update();
		}
		// (the game's parts while World FX runs here: the worldfx Ashita addon's work; the zone's files made the first
		// time it's entered)
		if (hifi::backend::started() && hifi::host::installed())
		{
			hifi::game::feed::frame(g_entities, party_now(), now);
			// (only while World FX draws into the game's world: on another output its files would go unused)
			if (const uint32_t zone = hifi::game::feed::zone(); zone != m_zone && worldfx_drawing() != 0)
			{
				m_zone = zone;
				hifi::bake::runner::zone_entered(zone);
			}
		}
		hifi::backend::present();
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
