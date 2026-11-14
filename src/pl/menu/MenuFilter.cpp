/**
 * @file MenuFilter.cpp
 * @brief The Mods tab's filter rule (see MenuTypes.hpp). No ImGui, so it is host-testable.
 */

#include <cctype>
#include <cstring>

#include "pl/menu/MenuTypes.hpp"

namespace pl::menu {
namespace {

bool ContainsIgnoreCase(const std::string &hay, const char *needle) {
  if (needle == nullptr || *needle == '\0') return true;
  const std::size_t n = std::strlen(needle);
  if (n > hay.size()) return false;
  for (std::size_t i = 0; i + n <= hay.size(); ++i) {
    std::size_t j = 0;
    while (j < n && std::tolower(static_cast<unsigned char>(hay[i + j])) ==
                        std::tolower(static_cast<unsigned char>(needle[j])))
      ++j;
    if (j == n) return true;
  }
  return false;
}

} // namespace

bool MatchesSearch(const MenuModuleRow &row, const char *search) {
  if (search == nullptr || *search == '\0') return true;
  return ContainsIgnoreCase(row.name, search) || ContainsIgnoreCase(row.description, search) ||
         ContainsIgnoreCase(row.groupName, search);
}

std::vector<int> VisibleModuleIndices(const std::vector<MenuModuleRow> &modules,
                                      const MenuFilter &filter) {
  std::vector<int> out;
  for (std::size_t i = 0; i < modules.size(); ++i) {
    const MenuModuleRow &row = modules[i];
    if (filter.favoritesOnly && !row.favorite) continue;
    if (!MatchesSearch(row, filter.search.c_str())) continue;
    out.push_back(static_cast<int>(i));
  }
  return out;
}

} // namespace pl::menu
