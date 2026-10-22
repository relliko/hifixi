#pragma once

// What HiFi XI's menu draws with: ImGui's calls, each one a pointer, so the menu doesn't need Ashita's headers (the
// plugin fills it from Ashita's GUI manager) and a test can draw it with a fake that clicks. A widget returns true
// when it was changed or clicked.

#include <cstddef>

namespace hifi::ui
{
	struct gui
	{
		// The window (open: its close box), and a part of it with its own scrolling (w or h 0: the rest of the window)
		bool (*begin)(const char *title, bool *open, float width, float height) = nullptr;
		void (*end)() = nullptr;
		bool (*begin_child)(const char *id, float width, float height, bool border) = nullptr;
		void (*end_child)() = nullptr;
		// Layout
		void (*same_line)() = nullptr;
		void (*separator)() = nullptr;
		void (*spacing)() = nullptr;
		void (*set_next_item_width)(float width) = nullptr;
		float (*available_width)() = nullptr;
		void (*push_id)(const char *id) = nullptr;
		void (*pop_id)() = nullptr;
		void (*begin_disabled)(bool disabled) = nullptr;
		void (*end_disabled)() = nullptr;
		// Text
		void (*text)(const char *text) = nullptr;
		void (*text_colored)(const float rgba[4], const char *text) = nullptr;
		void (*text_disabled)(const char *text) = nullptr;
		void (*text_wrapped)(const char *text) = nullptr;
		// Widgets
		bool (*selectable)(const char *label, bool selected) = nullptr;
		bool (*button)(const char *label) = nullptr;
		// (a button in the accent colour while selected: a preset picked, a tab)
		bool (*toggle_button)(const char *label, bool selected) = nullptr;
		bool (*checkbox)(const char *label, bool *value) = nullptr;
		bool (*slider_float)(const char *label, float *value, float low, float high, const char *format) = nullptr;
		bool (*slider_int)(const char *label, int *value, int low, int high) = nullptr;
		bool (*color_edit3)(const char *label, float rgb[3]) = nullptr;
		bool (*input_text)(const char *id, const char *hint, char *buffer, size_t size) = nullptr;
		// The tooltip for the item just drawn, while the mouse is over it
		void (*tooltip)(const char *text) = nullptr;
		// An Ashita command, as typed (/hifps)
		void (*command)(const char *text) = nullptr;
	};
}
