#include "ModManager.h"

#include <algorithm>
#include <dlfcn.h>
#include <string>
#include <utility>

#include "pl/Logger.hpp"
#include "pl/internal/LoadedModRegistry.h"
#include "pl/internal/ModLoadError.hpp"
#include "pl/internal/ModManifest.h"
#include "pl/internal/NativeModLifecycle.h"

namespace {

pl::internal::mod::ModLoadErrorStore &modLoadErrorStore() {
  static pl::internal::mod::ModLoadErrorStore store;
  return store;
}

void SetLoadError(std::string message) { modLoadErrorStore().set(std::move(message)); }

} // namespace

std::string ModManager::LastLoadError() { return modLoadErrorStore().get(); }

bool ModManager::LoadModLibrary(
    const std::filesystem::path &libraryPath,
    const std::optional<std::filesystem::path> &sourceModDirectory,
    JavaVM *vm) {
  using namespace pl::internal::mod;

  // A fresh attempt clears the previous reason, so a stale message is never read back for this load
  // (audit item 6.17).
  modLoadErrorStore().clear();

  RuntimeModInfo modInfoStorage;
  if (!createRuntimeModInfo(libraryPath, sourceModDirectory, modInfoStorage)) {
    // A manifest that cannot be parsed is not attributable to a mod id yet, so it is reported
    // without a fault count: the failure is the manifest's, not the mod's runtime behaviour.
    SetLoadError("the mod manifest could not be read or is invalid: " +
                 libraryPath.string());
    return false;
  }

  // Restore this mod's persisted fault budget before the check below, so a mod that faulted on
  // earlier launches is disabled on this one rather than retried forever (audit item 6.5). Keyed on
  // the mod id and version, so a version update starts with a fresh budget.
  SeedModFaultsFromStore(modInfoStorage.modId, modInfoStorage.version);

  // A mod that exhausted its fault budget is refused before anything is loaded, so a mod
  // that faults on every launch costs one launch attempt, not one per launch forever. The user can
  // re-enable it, which clears the budget.
  if (modFaultTracker().isDisabled(modInfoStorage.modId)) {
    preloaderLogger.warn(
        "Refusing to load mod {}: it faulted {} times in a row and is disabled until re-enabled",
        modInfoStorage.modId, modFaultTracker().faultsFor(modInfoStorage.modId));
    SetLoadError("the mod is disabled after repeated faults; re-enable it to retry");
    return false;
  }

  const std::string libraryPathString = libraryPath.lexically_normal().string();
  const std::string normalizedLibraryPath =
      normalizeLibraryPath(libraryPathString);

  if (isModAlreadyRegistered(normalizedLibraryPath)) {
    return true;
  }

  void *handle = acquireModHandle(libraryPathString);
  if (!handle) {
    const char *loadError = dlerror();
    preloaderLogger.error("Failed to load mod library {}: {}",
                          libraryPathString,
                          loadError ? loadError : "unknown error");
    SetLoadError(std::string("the mod library could not be loaded: ") +
                 (loadError ? loadError : "unknown dlopen error"));
    return false;
  }

  auto entry = createLoadedModEntry(handle, vm, modInfoStorage,
                                    normalizedLibraryPath);
  if (!entry.has_value()) {
    // The mod loaded as a library but its lifecycle entry failed. That is this mod's
    // fault and no other mod's, so it is counted here and the rest of the set keeps loading.
    const ModFaultDecision decision = modFaultTracker().recordFault(modInfoStorage.modId);
    PersistModFaultState(modInfoStorage.modId, modInfoStorage.version, decision.faultCount,
                         decision.action == ModFaultAction::Disable);
    if (decision.action == ModFaultAction::Disable) {
      preloaderLogger.error(
          "Disabling mod {} after {} consecutive faults; re-enable it to retry",
          modInfoStorage.modId, decision.faultCount);
    }
    SetLoadError("the mod's load entry point failed; the mod is now disabled after " +
                 std::to_string(decision.faultCount) + " fault(s)");
    return false;
  }

  // A mod that reached this point loaded successfully, so its fault budget is reset, and the reset
  // is persisted so a later launch does not restore a spent budget (audit item 6.5).
  modFaultTracker().recordSuccess(modInfoStorage.modId);
  PersistModFaultState(modInfoStorage.modId, modInfoStorage.version, 0, false);
  registerLoadedMod(normalizedLibraryPath, std::move(*entry));
  return true;
}

void ModManager::EnableLoadedMods() {
  using namespace pl::internal::mod;

  const auto keys = getLoadedModKeysSnapshot();
  for (const auto &key : keys) {
    const auto entry = getLoadedModEntry(key);
    if (!entry.has_value() ||
        (entry->kind != LoadedModKind::Lifecycle &&
         entry->kind != LoadedModKind::CppLifecycle) ||
        entry->state != LoadedModState::Loaded) {
      continue;
    }

    if (enableLoadedModEntry(*entry)) {
      setLoadedModState(key, LoadedModState::Enabled);
      modFaultTracker().recordSuccess(entry->modId);
      PersistModFaultState(entry->modId, entry->version, 0, false);
      continue;
    }

    // An enable that fails is isolated the same way a load that fails is. The mod is
    // disabled after its budget is spent, and the loop keeps going so one broken mod cannot stop
    // every other mod from enabling.
    const ModFaultDecision decision = modFaultTracker().recordFault(entry->modId);
    PersistModFaultState(entry->modId, entry->version, decision.faultCount,
                         decision.action == ModFaultAction::Disable);
    if (decision.action == ModFaultAction::Disable) {
      preloaderLogger.error(
          "Disabling mod {} after {} consecutive enable faults; re-enable it to retry",
          entry->modId, decision.faultCount);
    }
    SetLoadError("the mod's enable entry point failed for " + entry->modId);
  }
}

void ModManager::DisableAndUnloadLoadedMods() {
  using namespace pl::internal::mod;

  auto keys = getLoadedModKeysSnapshot();
  std::reverse(keys.begin(), keys.end());

  for (const auto &key : keys) {
    auto entry = getLoadedModEntry(key);
    if (!entry.has_value() ||
        (entry->kind != LoadedModKind::Lifecycle &&
         entry->kind != LoadedModKind::CppLifecycle) ||
        entry->state == LoadedModState::Unloaded) {
      continue;
    }

    if (entry->state == LoadedModState::Enabled &&
        disableLoadedModEntry(*entry)) {
      setLoadedModState(key, LoadedModState::Loaded);
    }

    entry = getLoadedModEntry(key);
    if (!entry.has_value() || entry->state == LoadedModState::Unloaded) {
      continue;
    }

    if (unloadLoadedModEntry(*entry)) {
      setLoadedModState(key, LoadedModState::Unloaded);
    }
  }
}
