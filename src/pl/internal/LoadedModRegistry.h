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

} // namespace pl::internal::mod
