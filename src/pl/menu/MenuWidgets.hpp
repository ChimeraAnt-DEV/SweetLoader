#pragma once

/**
 * @file MenuWidgets.hpp
 * @brief The in-game Mod Menu's glass widget kit.
 *
 * <p>Ported from the UI pack's `imgui/src/CaramelWidgets.{h,cpp}` (Lynxifyy/CaramelClient-UI-pack).
 * Every control draws with the foreground draw list and reports its interaction through the return
 * value, so a screen is a list of calls with no retained widget tree — the same shape the pack uses,
 * which is what lets the menu be re-laid-out without touching state.
 *
 * <p>The pack's optional backdrop-blur hook is dropped: it copies the desktop framebuffer, and on
 * Android the game's own framebuffer is not ours to read back at that point. The frosted navy base
 * plus tint gives the same read without the copy.
 */

#include "imgui.h"
#include "pl/menu/MenuTheme.hpp"

namespace pl::menu::ui {

/** The semantic colour a control picks its gradient from. */
enum class Tone { Green, Caramel, Blue, Red };

/** The line-art glyphs the pack draws by hand (no icon font is shipped). */
enum class Icon {
  Close, Check, Search, Down, Up, Left, Plus, Sliders, Hud, Swap, Dots,
  Puzzle, Replay, Cube, Mic, Gear, Star, StarFill, Play, Map
};

// --- Surfaces ---------------------------------------------------------------------------------

/** Draws a frosted, rounded glass panel between two corners, with an optional drop shadow. */
void Glass(ImVec2 a, ImVec2 b, float radius = 18.0f, bool shadow = true,
           ImU32 tint = IM_COL32(150, 175, 255, 14));

/** A 1px horizontal rule at {@code y}. */
void Hairline(float x0, float x1, float y);

// --- Drawing helpers --------------------------------------------------------------------------

/** Draws one line-art {@link Icon} centred on {@code center}. */
void DrawIcon(Icon icon, ImVec2 center, float size, ImU32 color, float thickness = 1.8f);

/** Exponential ease of {@code current} toward {@code target}, keyed by {@code id}. */
float Anim(ImGuiID id, float target, float speed = 14.0f);

/** Draws {@code text} at {@code pos} with an optional centre anchor on either axis. */
void TextAt(ImVec2 pos, ImFont *font, ImU32 color, const char *text, bool centerX = false,
            bool centerY = true);

/** A dashed rectangle outline, for the HUD-layout editor's selection handles. */
void DashedRect(ImVec2 a, ImVec2 b, ImU32 color, float dash = 6.0f, float gap = 4.0f,
                float thickness = 1.5f);

// --- Controls (return true on click / change) -------------------------------------------------

bool PrimaryButton(const char *id, const char *label, ImVec2 size, Tone tone = Tone::Green);
bool GlassButton(const char *id, const char *label, ImVec2 size, bool danger = false,
                 bool active = false, const Icon *icon = nullptr);
bool Toggle(const char *id, bool *value);
bool CheckBox(const char *id, const char *label, bool *value);
bool Slider(const char *id, float *v, float lo, float hi);
bool Chip(const char *label, bool active);
bool SearchField(const char *id, char *buf, size_t cap, float width, const char *hint);
bool TextField(const char *id, char *buf, size_t cap, float width);
bool Segmented(const char *id, const char *const *items, int count, int *selected, float itemWidth);
bool TabBar(const char *id, const char *const *labels, const Icon *icons, int count, int *selected,
            float itemWidth);
bool ColorSwatches(const char *id, const ImU32 *colors, int count, int *selected);
bool Dropdown(const char *id, const char *const *items, int count, int *selected, float width);
void StatusLabel(const char *text, ImU32 color);

} // namespace pl::menu::ui
