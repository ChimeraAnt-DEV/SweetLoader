#pragma once

/**
 * @file MenuHost.hpp
 * @brief Owns the in-game Mod Menu's ImGui context, GLES backend and frame loop.
 *
 * <p>The menu draws from the present hook (`pl::render::OnPresentBoundary`), the one point in the
 * process that is on the render thread with the game's GL context current. This class is what that
 * hook calls: it lazily creates the ImGui context on the first present where the seam is live, pumps
 * one ImGui frame per present, feeds the touch/keyboard state the launcher forwards, and renders
 * through ImGui's GLES3 backend.
 *
 * <p><b>Fail-closed.</b> {@link SetLive} is only true when the present hook is installed and a GLES
 * context is current. Until then {@link Frame} does nothing and {@link IsMenuLive} reports false, so
 * the launcher keeps the in-game menu badged rather than opening a menu that cannot draw.
 *
 * <p>All entry points are called on the render thread (from the present hook) except the input and
 * catalogue setters, which are called from the launcher thread and guarded by a mutex.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "pl/menu/MenuRender.hpp"

namespace pl::menu {

/** A touch event forwarded from the launcher's view. */
struct TouchInput {
  int action = 0; // MotionEvent.ACTION_*
  float x = 0.0f;
  float y = 0.0f;
  float viewportWidth = 0.0f;
  float viewportHeight = 0.0f;
};

/** One frame's worth of input, drained by {@link Frame}. */
struct InputFrame {
  std::vector<TouchInput> touches;
  bool backPressed = false;
};

/** What a menu interaction asks the launcher to do. */
enum class MenuEventKind { ToggleModule, SetConfig, ToggleFavorite, Close };

/** One queued interaction. For {@link MenuEventKind::SetConfig}, {@code value} is "key=value". */
struct MenuEvent {
  MenuEventKind kind = MenuEventKind::Close;
  std::string stableKey;
  std::string value;
};

/**
 * @brief Installs the menu's frame hook. Safe to call repeatedly.
 *
 * <p>Does not create the ImGui context (that needs a current GL context); it only marks the menu as
 * wanted. The context is created on the first {@link Frame} after the present seam is live.
 */
void InstallMenu();

/** @brief True once the menu's ImGui context and GLES backend are up. */
bool IsMenuLive();

/**
 * @brief Runs one menu frame on the render thread with the GL context current.
 *
 * @param viewportWidth  the game surface width in pixels.
 * @param viewportHeight the game surface height in pixels.
 */
void Frame(float viewportWidth, float viewportHeight);

/** @brief Replaces the module catalogue the Mods tab shows. Launcher thread. */
void SetCatalogue(std::vector<MenuModuleRow> modules, std::string emptyReason);

/** @brief Queues a touch event for the next frame. Launcher thread. */
void PushTouch(const TouchInput &touch);

/** @brief Queues a back/close request for the next frame. Launcher thread. */
void PushBack();

/** @brief Opens or closes the menu. Launcher thread. */
void SetOpen(bool open);

/** @brief Whether the menu is currently open. */
bool IsOpen();

/** @brief Registers a font face for one role, from memory. Launcher thread, before the first frame. */
bool RegisterFont(theme::FontRole role, const void *data, int size);

/** @brief Drains the interactions the menu raised this frame. Launcher thread. */
std::vector<MenuEvent> DrainEvents();

} // namespace pl::menu
