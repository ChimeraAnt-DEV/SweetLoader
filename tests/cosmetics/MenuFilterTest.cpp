// Host test for the in-game Mod Menu's filter rule (src/pl/menu/MenuFilter.cpp).
//
// The filter is the one part of the Mods tab that can be wrong in a way a screenshot would not
// reveal: a module silently missing from the list looks the same as a module that was never
// registered. It is ImGui-free on purpose, so it is verified here rather than on a device.

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "pl/menu/MenuTypes.hpp"

using pl::menu::MenuFilter;
using pl::menu::MenuModuleRow;
using pl::menu::MatchesSearch;
using pl::menu::VisibleModuleIndices;

static MenuModuleRow Row(const char *name, const char *description, const char *group, bool fav) {
  MenuModuleRow row;
  row.stableKey = name;
  row.name = name;
  row.description = description;
  row.groupName = group;
  row.favorite = fav;
  return row;
}

int main() {
  const std::vector<MenuModuleRow> modules = {
      Row("FPS Boost", "Quiets the launcher's host work", "Performance", false),
      Row("Motion Blur", "Softens the image with a trailing blend", "Visual", true),
      Row("Frame Generation", "Interpolates the presented frame", "Visual", true),
      Row("Hitboxes", "Draws entity hitboxes", "Combat", false),
  };

  // An empty search matches everything.
  assert(MatchesSearch(modules[0], ""));
  assert(MatchesSearch(modules[0], nullptr));

  // Case-insensitive, and matches any of name/description/group.
  assert(MatchesSearch(modules[0], "fps"));
  assert(MatchesSearch(modules[0], "FPS"));
  assert(MatchesSearch(modules[0], "launcher"));
  assert(MatchesSearch(modules[0], "performance"));
  assert(!MatchesSearch(modules[0], "motion"));

  // The visible set: no filter shows all four, in catalogue order.
  {
    const std::vector<int> all = VisibleModuleIndices(modules, MenuFilter{});
    assert(all.size() == 4);
    assert(all[0] == 0 && all[1] == 1 && all[2] == 2 && all[3] == 3);
  }

  // Search narrows to the matches without reordering.
  {
    const std::vector<int> visual =
        VisibleModuleIndices(modules, MenuFilter{std::string("visual"), false});
    assert(visual.size() == 2);
    assert(visual[0] == 1 && visual[1] == 2);
  }

  // Favourites-only keeps just the starred rows.
  {
    const std::vector<int> favs = VisibleModuleIndices(modules, MenuFilter{std::string(), true});
    assert(favs.size() == 2);
    assert(favs[0] == 1 && favs[1] == 2);
  }

  // Both filters compose: a favourite that does not match the search is still hidden.
  {
    const std::vector<int> both =
        VisibleModuleIndices(modules, MenuFilter{std::string("frame"), true});
    assert(both.size() == 1 && both[0] == 2);
  }

  // A search that matches nothing yields an empty list, not the whole catalogue.
  {
    const std::vector<int> none =
        VisibleModuleIndices(modules, MenuFilter{std::string("zzzz"), false});
    assert(none.empty());
  }

  // A needle longer than the haystack must not read past it.
  assert(!MatchesSearch(modules[0], "FPS Boost and then some"));

  std::printf("MenuFilterTest: all assertions passed\n");
  return 0;
}
