#pragma once

/**
 * @file MenuRender.hpp
 * @brief The in-game Mod Menu: the pack's layout, drawn on the game's own GL context.
 *
 * <p>This is the screen-level port of the UI pack's `imgui/src/CaramelMenu.{h,cpp}`
 * (Lynxifyy/CaramelClient-UI-pack). The pack's tab set, glass panels, HUD-layout editor and widget
 * arrangement are kept; what changes is the data behind them. The pack ships a single "Example"
 * module and stand-in HUD overlays; here the Mods tab is the real module catalogue and the settings
 * panel is driven by a module's config entries, so a toggle in the menu is a toggle of a real
 * module rather than a demo bool.
 *
 * <p>Pure presentation plus a small state machine: the module data arrives as an immutable
 * {@link MenuModuleRow} list, and every interaction is reported back through {@link MenuActions},
 * so the layout can be reviewed and unit-tested without a device or a GL context.
 */

#include <functional>
#include <string>
#include <vector>

#include "pl/menu/MenuTypes.hpp"
#include "pl/menu/MenuWidgets.hpp"

namespace pl::menu {

/** The five tabs, in the order the header shows them (the pack's order). */
enum class MenuTab { Mods, Replay, Cosmetics, Voice, Settings };

/** The menu's whole mutable state. One instance lives on the render thread. */
struct MenuState {
  bool open = false;
  MenuTab tab = MenuTab::Mods;
  int profile = 0;
  int category = 0;
  char search[64] = {};
  bool favoritesOnly = false;
  int selectedModule = -1;
  bool moduleSettingsOpen = false;
  /** The state the settings panel edits; index into {@link modules}. */
  int settingsModule = -1;

  // Replay
  bool autoRecord = false;
  float buffer = 0.3f;
  int quality = 1;
  char replayKey[8] = "F8";

  // Voice chat
  int inDevice = 0;
  int outDevice = 0;
  float micVol = 0.8f;
  float outVol = 0.65f;
  float range = 0.45f;
  bool ptt = true;
  bool noiseSuppression = true;
  bool proximity = true;
  char pttKey[8] = "V";

  // Settings
  bool autoJoin = false;
  bool chatToggles = true;
  bool notifications = true;
  bool smoothCamera = false;
  float menuScale = 0.4f;
  float blur = 0.6f;
  int language = 0;
  int accent = 0;
  bool rounded = true;
  bool animations = true;

  // HUD-layout editor
  bool hudEditing = false;
  bool snap = true;
  bool showGrid = true;
  int hudSel = 2;

  /** The module catalogue, refreshed by the caller when its revision changes. */
  std::vector<MenuModuleRow> modules;
  /** A human-readable reason the catalogue is empty, shown in the placeholder. */
  std::string emptyReason;
};

/** Every interaction the menu can raise, so the caller owns the effects. */
struct MenuActions {
  std::function<void(const std::string &stableKey, bool enabled)> onToggleModule;
  std::function<void(const std::string &stableKey, const std::string &key,
                     const std::string &value)>
      onSetConfig;
  std::function<void(const std::string &stableKey)> onToggleFavorite;
  std::function<void()> onClose;
  std::function<void()> onRequestCatalogue;
};

/**
 * Draws the whole menu centred in {@code display}, or the HUD-layout editor when
 * {@code state.hudEditing}. A no-op when {@code state.open} is false.
 *
 * <p>The caller must have an ImGui frame in progress on the game's GL context.
 */
void RenderMenu(MenuState &state, const MenuActions &actions, ImVec2 display);

} // namespace pl::menu
