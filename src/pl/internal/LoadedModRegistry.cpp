#include "LoadedModRegistry.h"

#include <mutex>
#include <unordered_map>
#include <utility>

#include "pl/Logger.hpp"
#include "pl/internal/ModLoadOrder.hpp"

namespace pl::internal::mod {
namespace {
std::mutex gLoadedModsMutex;
std::unordered_map<std::string, LoadedModEntry> gLoadedModLibraries;
std::vector<std::string> gLoadedModOrder;
} // namespace

bool isModAlreadyRegistered(const std::string &normalizedLibraryPath) {
  std::lock_guard<std::mutex> lock(gLoadedModsMutex);
  const auto it = gLoadedModLibraries.find(normalizedLibraryPath);
  return it != gLoadedModLibraries.end() &&
         it->second.state != LoadedModState::Unloaded;
}

void registerLoadedMod(const std::string &normalizedLibraryPath,
                       LoadedModEntry entry) {
  std::lock_guard<std::mutex> lock(gLoadedModsMutex);
  const auto [it, inserted] =
      gLoadedModLibraries.emplace(normalizedLibraryPath, LoadedModEntry{});
  it->second = std::move(entry);
  if (inserted) {
    gLoadedModOrder.push_back(normalizedLibraryPath);
  }
}

std::vector<std::string> getLoadedModKeysSnapshot() {
  std::lock_guard<std::mutex> lock(gLoadedModsMutex);
  std::vector<std::string> keys;
  keys.reserve(gLoadedModOrder.size());
  for (const auto &key : gLoadedModOrder) {
    const auto it = gLoadedModLibraries.find(key);
    if (it == gLoadedModLibraries.end()) {
      continue;
    }

    const auto &entry = it->second;
    if ((entry.kind == LoadedModKind::Lifecycle ||
         entry.kind == LoadedModKind::CppLifecycle) &&
        entry.state != LoadedModState::Unloaded) {
      keys.push_back(key);
    }
  }

  // Item 447: order the set by declared dependencies, keeping registration order as the tie-break.
  // A cycle is reported with its members and the remaining mods are still ordered, because refusing
  // the whole set over one bad pair would make every other mod unloadable. Enabling and disabling
  // both go through here, and the disable path reverses the result, so a mod is torn down before
  // anything it depended on.
  std::vector<ModLoadNode> nodes;
  nodes.reserve(keys.size());
  for (const auto &key : keys) {
    const auto it = gLoadedModLibraries.find(key);
    if (it == gLoadedModLibraries.end()) continue;
    nodes.push_back(ModLoadNode{it->second.modId, it->second.dependencies});
  }

  const ModLoadOrder resolved = ResolveModLoadOrder(nodes);
  if (resolved.hasCycle) {
    std::string members;
    for (const auto &id : resolved.cycle) {
      if (!members.empty()) members.append(" -> ");
      members.append(id);
    }
    preloaderLogger.warn("Mod dependency cycle: {}; loading in registration order", members);
    return keys;
  }
  if (resolved.order.size() != keys.size()) return keys;

  // Map the ids back to the registry keys (a mod id is not always its library path).
  std::vector<std::string> ordered;
  ordered.reserve(keys.size());
  for (const auto &id : resolved.order) {
    for (const auto &key : keys) {
      const auto it = gLoadedModLibraries.find(key);
      if (it != gLoadedModLibraries.end() && it->second.modId == id) {
        ordered.push_back(key);
        break;
      }
    }
  }
  return ordered.size() == keys.size() ? ordered : keys;
}

std::optional<LoadedModEntry> getLoadedModEntry(const std::string &key) {
  std::lock_guard<std::mutex> lock(gLoadedModsMutex);
  const auto it = gLoadedModLibraries.find(key);
  if (it == gLoadedModLibraries.end()) {
    return std::nullopt;
  }

  return it->second;
}

void setLoadedModState(const std::string &key, LoadedModState state) {
  std::lock_guard<std::mutex> lock(gLoadedModsMutex);
  const auto it = gLoadedModLibraries.find(key);
  if (it != gLoadedModLibraries.end()) {
    it->second.state = state;
  }
}

} // namespace pl::internal::mod
