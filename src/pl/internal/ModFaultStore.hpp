#pragma once

/**
 * @file ModFaultStore.hpp
 * @brief The on-disk shape of the per-mod fault budget.
 *
 * <p>The in-memory {@code ModFaultTracker} forgets every fault when the process exits, so a mod that
 * faults on every launch is retried forever (audit item 6.5). The counts are persisted to a small
 * JSON file beside the mods and read back on the next launch, so "three faults and it is disabled"
 * holds across sessions rather than within one.
 *
 * <p>The file is keyed on the mod id <em>and its version</em>: a new version of a mod is a different
 * artifact, and a fault count earned by the old build must not disable the new one. The key rule,
 * the serialize/parse round trip and the lookup rule are pure, so they are host-tested.
 */

#include <map>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace pl::internal::mod {

/** @brief One mod's persisted fault budget. */
struct ModFaultRecord {
  int faults = 0;
  bool disabled = false;
};

/** @brief The fault store: store key to its persisted budget. */
using ModFaultStore = std::map<std::string, ModFaultRecord>;

/** @brief The schema version written into the file, so a future format can be told apart. */
inline constexpr int kModFaultStoreSchema = 1;

/**
 * @brief The store key for a mod: its id and version.
 *
 * <p>The version is part of the key so an updated mod starts with a fresh budget. An empty version
 * is a valid key (a mod with no declared version), distinct from a versioned one.
 */
inline std::string MakeModFaultKey(std::string_view modId, std::string_view version) {
  std::string key(modId);
  key.push_back('@');
  key.append(version);
  return key;
}

/**
 * @brief Whether a persisted fault record should be applied to a mod loaded now.
 *
 * <p>Applied only when the key matches exactly, which it does when the mod id and version are
 * unchanged. A version change produces a different key, so the old record is simply not found and
 * the mod starts with a full budget.
 */
inline bool ShouldApplyPersistedFaults(const ModFaultStore &store, std::string_view key,
                                       ModFaultRecord &out) {
  const auto it = store.find(std::string(key));
  if (it == store.end()) return false;
  out = it->second;
  return true;
}

/** @brief Serializes the store to the JSON text written to disk. */
inline std::string SerializeModFaultStore(const ModFaultStore &store) {
  nlohmann::json document;
  document["version"] = kModFaultStoreSchema;
  nlohmann::json mods = nlohmann::json::object();
  for (const auto &entry : store) {
    mods[entry.first] = {{"faults", entry.second.faults}, {"disabled", entry.second.disabled}};
  }
  document["mods"] = std::move(mods);
  return document.dump(2);
}

/**
 * @brief Parses the JSON text read from disk.
 *
 * <p>Fail-closed: malformed text, a missing `mods` object, or a record with the wrong value types
 * yields an empty store rather than a partially-understood one. An unknown future schema version is
 * also refused, so a newer file is not silently misread by an older build.
 */
inline ModFaultStore ParseModFaultStore(std::string_view text) {
  ModFaultStore store;
  if (text.empty()) return store;
  nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
  if (document.is_discarded() || !document.is_object()) return store;
  const auto version = document.find("version");
  if (version == document.end() || !version->is_number_integer() ||
      version->get<int>() != kModFaultStoreSchema) {
    return store;
  }
  const auto mods = document.find("mods");
  if (mods == document.end() || !mods->is_object()) return store;
  for (auto it = mods->begin(); it != mods->end(); ++it) {
    if (!it.value().is_object()) continue;
    const auto faults = it.value().find("faults");
    const auto disabled = it.value().find("disabled");
    if (faults == it.value().end() || !faults->is_number_integer()) continue;
    ModFaultRecord record;
    record.faults = faults->get<int>();
    if (record.faults < 0) record.faults = 0;
    record.disabled = disabled != it.value().end() && disabled->is_boolean() &&
                      disabled->get<bool>();
    store[it.key()] = record;
  }
  return store;
}

} // namespace pl::internal::mod
