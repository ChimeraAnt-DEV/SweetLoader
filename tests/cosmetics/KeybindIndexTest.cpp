/**
 * @file KeybindIndexTest.cpp
 * @brief Host test for the sorted key-code index behind the Mod Menu key callback.
 *
 * The key callback used to walk every module and every config entry per key event, parsing each
 * keybind value with std::stoi. The index is built once per registry change and answers a lookup by
 * binary search, so the rule (sort, find all matches for a key code, no false positives) is pinned
 * here without a device.
 */

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "pl/runtime/KeybindIndex.hpp"

using pl::runtime::KeybindEntry;
using pl::runtime::KeybindIndex;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

} // namespace

int main() {
  // An empty index finds nothing.
  {
    KeybindIndex index;
    std::vector<int> out;
    index.find(65, out);
    expect(out.empty(), "an empty index has no matches");
    expect(index.empty(), "an empty index reports empty");
  }

  // A single entry is found by its key code and nothing else.
  {
    KeybindIndex index;
    index.rebuild({{65, 0}});
    std::vector<int> out;
    index.find(65, out);
    expect(out.size() == 1 && out[0] == 0, "the key code finds its entry");
    out.clear();
    index.find(66, out);
    expect(out.empty(), "a different key code finds nothing");
    expect(index.size() == 1, "the index holds one entry");
  }

  // Entries are sorted, so a lookup in a large set is a binary search and still exact.
  {
    KeybindIndex index;
    std::vector<KeybindEntry> entries;
    for (int i = 0; i < 1000; ++i) entries.push_back({i * 3, i});
    index.rebuild(std::move(entries));
    std::vector<int> out;
    index.find(3 * 500, out);
    expect(out.size() == 1 && out[0] == 500, "a mid-range key code finds its payload");
    out.clear();
    index.find(4, out); // no entry at 4 (only multiples of 3)
    expect(out.empty(), "a key code between entries finds nothing");
  }

  // A key code bound to more than one module yields every match.
  {
    KeybindIndex index;
    index.rebuild({{70, 0}, {70, 1}, {80, 2}});
    std::vector<int> out;
    index.find(70, out);
    expect(out.size() == 2, "a shared key code yields both payloads");
    expect((out[0] == 0 && out[1] == 1) || (out[0] == 1 && out[1] == 0),
           "both payloads are returned");
  }

  // Rebuilding replaces the previous index rather than merging into it.
  {
    KeybindIndex index;
    index.rebuild({{65, 0}});
    index.rebuild({{66, 1}});
    std::vector<int> out;
    index.find(65, out);
    expect(out.empty(), "a rebuild drops the old entries");
    index.find(66, out);
    expect(out.size() == 1 && out[0] == 1, "the rebuilt entry is found");
  }

  std::printf("KeybindIndexTest: %d checks passed\n", checks);
  return 0;
}
