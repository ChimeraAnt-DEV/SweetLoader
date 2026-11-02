#pragma once

/**
 * @file ModLoadOrder.hpp
 * @brief Deterministic load ordering for native mods from their declared dependencies (item 447).
 *
 * <p>Two properties matter and both are pure, so they are host-tested:
 *
 * <ul>
 *   <li><b>Determinism.</b> The same set of mods loads in the same order every launch, so a
 *       failure is reproducible. Mods with no ordering constraint keep their registration order
 *       rather than being re-sorted by name, because a mod's own init order is what the user
 *       observed last time.</li>
 *   <li><b>Cycle detection.</b> A dependency cycle has no valid order, so it is reported rather
 *       than silently broken at an arbitrary point. The cycle members are named, so the author can
 *       see which edge is wrong.</li>
 * </ul>
 *
 * <p>A dependency on a mod that is not present is <em>not</em> an error here: it is a missing
 * optional mod, and the dependent still loads. Refusing to load would turn one absent mod into a
 * broken install.
 */

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pl::internal::mod {

/** @brief One mod's identity and the mods it declares it needs loaded first. */
struct ModLoadNode {
  std::string id;
  std::vector<std::string> dependencies;
};

/** @brief The result of ordering a set of mods. */
struct ModLoadOrder {
  /** The mod ids in the order they should be loaded. */
  std::vector<std::string> order;
  /** True when a dependency cycle was found; [cycle] then names the members. */
  bool hasCycle{false};
  /** The ids that form a cycle, in the order they were visited. */
  std::vector<std::string> cycle;
};

namespace detail {

inline void VisitModNode(const std::string &id,
                         const std::unordered_map<std::string, ModLoadNode> &byId,
                         std::unordered_map<std::string, int> &state,
                         std::vector<std::string> &stack, ModLoadOrder &result) {
  const int current = state.count(id) ? state[id] : 0;
  if (current == 2) return; // already emitted
  if (current == 1) {
    // A back edge to a node still on the stack is the cycle. Record the members from that node on.
    result.hasCycle = true;
    if (result.cycle.empty()) {
      auto start = std::find(stack.begin(), stack.end(), id);
      if (start != stack.end()) {
        result.cycle.assign(start, stack.end());
      } else {
        result.cycle.push_back(id);
      }
    }
    return;
  }

  state[id] = 1;
  stack.push_back(id);

  const auto it = byId.find(id);
  if (it != byId.end()) {
    for (const std::string &dependency : it->second.dependencies) {
      // A dependency on an absent mod is skipped, not an error.
      if (byId.find(dependency) == byId.end()) continue;
      VisitModNode(dependency, byId, state, stack, result);
    }
  }

  stack.pop_back();
  state[id] = 2;
  result.order.push_back(id);
}

} // namespace detail

/**
 * @brief Orders [mods] so a mod is emitted after every dependency it declares that is present.
 *
 * <p>Input order is the tie-breaker, so a set with no dependencies comes back unchanged. A cycle
 * is reported (with its members) and the remaining mods are still ordered, because a caller that
 * refuses the whole set over one bad pair would make the other mods unloadable.
 */
inline ModLoadOrder ResolveModLoadOrder(const std::vector<ModLoadNode> &mods) {
  ModLoadOrder result;

  std::unordered_map<std::string, ModLoadNode> byId;
  byId.reserve(mods.size());
  std::vector<std::string> inputOrder;
  inputOrder.reserve(mods.size());
  for (const ModLoadNode &mod : mods) {
    if (mod.id.empty()) continue;
    // A duplicate id keeps the first declaration; a mod cannot be loaded twice.
    if (byId.find(mod.id) != byId.end()) continue;
    byId.emplace(mod.id, mod);
    inputOrder.push_back(mod.id);
  }

  std::unordered_map<std::string, int> state;
  state.reserve(inputOrder.size());
  std::vector<std::string> stack;
  for (const std::string &id : inputOrder) {
    detail::VisitModNode(id, byId, state, stack, result);
  }
  return result;
}

} // namespace pl::internal::mod
