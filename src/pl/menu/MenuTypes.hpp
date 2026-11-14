#pragma once

/**
 * @file MenuTypes.hpp
 * @brief The in-game Mod Menu's data types, free of any ImGui dependency.
 *
 * <p>Kept apart from the layout so the filter rule — the one part of the Mods tab that can be wrong
 * in a way a screenshot would not reveal, a module silently missing from the list — can be compiled
 * and tested on the build host without a GL context.
 */

#include <string>
#include <vector>

namespace pl::menu {

/** How the settings panel edits a config entry. */
enum class MenuConfigKind {
  Toggle,   ///< A boolean switch.
  Slider,   ///< A numeric range.
  ReadOnly, ///< Not editable in-game (a colour, a keybind, a text field): shown, with its value.
};

/** One configurable entry of the selected module, flattened for the settings panel. */
struct MenuConfigRow {
  std::string key;
  std::string label;
  std::string value;
  MenuConfigKind kind = MenuConfigKind::Toggle;
  float floatValue = 0.0f;
  float floatMin = 0.0f;
  float floatMax = 1.0f;
};

/** One module row in the Mods tab. */
struct MenuModuleRow {
  std::string stableKey;
  std::string name;
  std::string description;
  std::string groupName;
  bool enabled = false;
  bool favorite = false;
  bool hasConfig = false;
  /** Non-empty when the module cannot run in this build; the row is badged and its toggle refused. */
  std::string unavailableReason;
  std::vector<MenuConfigRow> configs;
};

/** The Mods tab's filter: the search text and the favourites-only toggle. */
struct MenuFilter {
  std::string search;
  bool favoritesOnly = false;
};

/**
 * The rows the Mods tab shows for a filter. Pure, so it is host-testable: it is the one piece of
 * the tab that can be wrong in a way a screenshot would not reveal (a module silently missing from
 * the list looks the same as a module that was never registered).
 */
std::vector<int> VisibleModuleIndices(const std::vector<MenuModuleRow> &modules,
                                      const MenuFilter &filter);

/** True when a module row matches the search text, case-insensitively. */
bool MatchesSearch(const MenuModuleRow &row, const char *search);

} // namespace pl::menu
