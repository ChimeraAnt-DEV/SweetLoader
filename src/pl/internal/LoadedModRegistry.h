#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pl/Mod.hpp"
#include "pl/internal/ModFaultPolicy.hpp"

namespace pl::internal::mod {

using PLModLifecycleFunc = bool (*)();

enum class LoadedModKind {
  Legacy,
  Lifecycle,
  CppLifecycle,
  NoEntry,
};

enum class LoadedModState {
  Loaded,
  Enabled,
  Unloaded,
};

struct LoadedModEntry {
  std::string libraryPath;
  std::string modId;
  /** The mod's declared version, part of the persisted fault key (audit item 6.5). */
  std::string version;
  /** The mod ids this mod declares it needs loaded first, from its manifest. */
  std::vector<std::string> dependencies;
  LoadedModKind kind{LoadedModKind::NoEntry};
  LoadedModState state{LoadedModState::Loaded};
  PLModLifecycleFunc enable{};
  PLModLifecycleFunc disable{};
  PLModLifecycleFunc unload{};
  pl::mod::ModRegistration *cppRegistration{};
  std::shared_ptr<pl::mod::NativeMod> cppNativeMod;
  std::shared_ptr<pl::mod::ModContext> cppContext;
};

bool isModAlreadyRegistered(const std::string &normalizedLibraryPath);

void registerLoadedMod(const std::string &normalizedLibraryPath,
                       LoadedModEntry entry);

std::vector<std::string> getLoadedModKeysSnapshot();

std::optional<LoadedModEntry> getLoadedModEntry(const std::string &key);

void setLoadedModState(const std::string &key, LoadedModState state);

/**
 * @brief The process-wide per-mod fault tracker.
 *
 * <p>A mod that faults is isolated: the fault is recorded against the mod id, the remaining mods
 * keep loading, and a mod that faults on every launch is disabled after {@link kMaxModFaults}
 * consecutive faults rather than retried forever. The tracker lives beside the registry because it
 * is the same process-scoped state, and the loader consults it before loading a mod.
 */
ModFaultTracker &modFaultTracker();

/**
 * @brief Points the persisted fault store at a file, reloading it when the path changes.
 *
 * <p>The file is `mods/.faults.json` (the mods directory's parent-relative store). An empty path
 * disables persistence, which is the safe default: nothing is read and nothing is written.
 */
void SetModFaultStorePath(const std::string &path);

/**
 * @brief Restores a mod's persisted fault budget into the in-memory tracker.
 *
 * <p>Called once per mod at load, before the disabled check, so a mod that faulted in an earlier
 * session is disabled on the launch that exhausts its budget rather than retried forever (audit item
 * 6.5). Keyed on the mod id and version, so a version update starts with a fresh budget.
 */
void SeedModFaultsFromStore(const std::string &modId, const std::string &version);

/**
 * @brief Writes a mod's current fault state back to the persisted store.
 *
 * <p>A no-op when persistence is disabled. A zero fault count with `disabled` false clears the
 * record, which is what a successful load/enable does.
 */
void PersistModFaultState(const std::string &modId, const std::string &version, int faults,
                          bool disabled);

} // namespace pl::internal::mod
