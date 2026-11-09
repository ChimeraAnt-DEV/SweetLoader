#pragma once

/**
 * @file KeybindIndex.hpp
 * @brief A sorted key-code index over mod keybind entries.
 *
 * <p>The Mod Menu key callback used to walk every registered module and every config entry on every
 * key event, parsing each keybind's value with {@code std::stoi} to compare it against the event's
 * key code. A key event arrives at typing speed, so that is an O(modules x configs) scan plus a
 * string parse per event (audit item 2.18).
 *
 * <p>This index is rebuilt when the registry changes (register, unregister, a config set, a toggle)
 * and answers a lookup in O(log n + matches). The entries carry an opaque payload index, so the
 * caller keeps its own parallel array of callbacks and never copies them into the index.
 *
 * <p>Pure and allocation-bounded, so the build + lookup rule is host-tested without a device.
 */

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pl::runtime {

/** @brief One indexed keybind: the key code it fires on and the caller's payload slot. */
struct KeybindEntry {
  int keyCode = 0;
  int payloadIndex = -1;
};

/** @brief A sorted-by-keyCode index over keybind entries. */
class KeybindIndex {
public:
  /** @brief Replaces the index with the given entries, sorting by key code. */
  void rebuild(std::vector<KeybindEntry> entries) {
    entries_ = std::move(entries);
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const KeybindEntry &a, const KeybindEntry &b) {
                       return a.keyCode < b.keyCode;
                     });
  }

  /**
   * @brief Appends the payload indices whose key code equals @p keyCode to @p out.
   *
   * <p>The entries are sorted, so this is a binary search over the key code; a key code bound to more
   * than one module yields every match.
   */
  void find(int keyCode, std::vector<int> &out) const {
    const auto lessThanKey = [keyCode](const KeybindEntry &entry) {
      return entry.keyCode < keyCode;
    };
    const auto first = std::find_if_not(entries_.begin(), entries_.end(), lessThanKey);
    for (auto it = first; it != entries_.end() && it->keyCode == keyCode; ++it) {
      out.push_back(it->payloadIndex);
    }
  }

  /** @brief Whether the index holds any entries. */
  [[nodiscard]] bool empty() const { return entries_.empty(); }

  /** @brief The number of indexed entries. */
  [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
  std::vector<KeybindEntry> entries_;
};

} // namespace pl::runtime
