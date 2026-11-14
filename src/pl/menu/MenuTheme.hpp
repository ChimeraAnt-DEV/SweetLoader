#pragma once

/**
 * @file MenuTheme.hpp
 * @brief The in-game Mod Menu's design tokens and ImGui style.
 *
 * <p>Ported from the UI pack's `imgui/src/CaramelTheme.{h,cpp}` (Lynxifyy/CaramelClient-UI-pack).
 * The pack's tokens are the same ones the launcher's Compose theme uses, so the in-game menu and the
 * launcher look like one product rather than two. Only the renderer-agnostic half of the pack is
 * used: its GLFW desktop backend has no Android path, so the backend here is ImGui's GLES3 one.
 *
 * <p>The colours are plain `ImU32`s rather than a class so a widget can blend one without an
 * allocation. Nothing here draws; it is a token table plus the one `ApplyStyle` call.
 */

#include <cstdint>

#include "imgui.h"

namespace pl::menu::theme {

/** The pack's colour tokens, each named after the role it plays, not the hue it happens to be. */
namespace colour {
inline ImU32 Abyss() { return IM_COL32(6, 11, 26, 255); }
inline ImU32 Navy() { return IM_COL32(10, 19, 48, 255); }
inline ImU32 Blue() { return IM_COL32(74, 120, 255, 255); }
inline ImU32 BlueLight() { return IM_COL32(98, 140, 255, 255); }
inline ImU32 BlueDark() { return IM_COL32(52, 88, 230, 255); }
inline ImU32 Caramel() { return IM_COL32(245, 167, 66, 255); }
inline ImU32 Green() { return IM_COL32(47, 211, 107, 255); }
inline ImU32 GreenLight() { return IM_COL32(62, 226, 122, 255); }
inline ImU32 GreenDark() { return IM_COL32(24, 160, 80, 255); }
inline ImU32 Red() { return IM_COL32(255, 84, 104, 255); }
inline ImU32 Orange() { return IM_COL32(255, 159, 46, 255); }
inline ImU32 Text() { return IM_COL32(234, 240, 255, 255); }
inline ImU32 Text2() { return IM_COL32(154, 168, 208, 255); }
inline ImU32 Text3() { return IM_COL32(102, 116, 158, 255); }
inline ImU32 OnGreen() { return IM_COL32(6, 36, 18, 255); }
inline ImU32 White(int alpha) { return IM_COL32(255, 255, 255, alpha); }
} // namespace colour

/** The five font roles the pack's layout uses. A null entry means "fall back to the default font". */
struct Fonts {
  ImFont *body = nullptr;
  ImFont *bodyMedium = nullptr;
  ImFont *bodyBold = nullptr;
  ImFont *title = nullptr;
  ImFont *display = nullptr;
};

/** Which slot a face fills. */
enum class FontRole { Body, BodyMedium, BodyBold, Title, Display };

/** The loaded fonts, valid after {@link LoadFonts}. */
Fonts &GetFonts();

/**
 * Loads a face from memory into one role at {@code pixelSize}.
 *
 * <p>Memory rather than a path: the game ships no loose font files, and on Android the launcher's
 * faces live inside the APK's `res/font`, so the Java side reads the bytes and pushes them here.
 * Returns false (and leaves the role on the default font) when the data is empty or unreadable, so a
 * build with no font assets still shows a legible menu. Must run before the atlas is first built.
 */
bool AddFont(FontRole role, const void *data, int size, float pixelSize);

/** Loads every role from files under {@code assetDir}; used by the host test, not on device. */
Fonts LoadFonts(const char *assetDir);

/** Applies the pack's rounding, spacing and colour scheme to the current ImGui style. */
void ApplyStyle();

} // namespace pl::menu::theme
