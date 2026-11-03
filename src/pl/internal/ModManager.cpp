#include "ModManager.h"

#include <algorithm>
#include <dlfcn.h>
#include <string>
#include <utility>

#include "pl/Logger.hpp"
#include "pl/internal/LoadedModRegistry.h"
#include "pl/internal/ModManifest.h"
#include "pl/internal/NativeModLifecycle.h"

bool ModManager::LoadModLibrary(
    const std::filesystem::path &libraryPath,
    const std::optional<std::filesystem::path> &sourceModDirectory,
    JavaVM *vm) {
  using namespace pl::internal::mod;

  RuntimeModInfo modInfoStorage;
  if (!createRuntimeModInfo(libraryPath, sourceModDirectory, modInfoStorage)) {
    // A manifest that cannot be parsed is not attributable to a mod id yet, so it is reported
    // without a fault count: the failure is the manifest's, not the mod's runtime behaviour.
    return false;
  }

  // A mod that exhausted its fault budget is refused before anything is loaded, so a mod
  // that faults on every launch costs one launch attempt, not one per launch forever. The user can
  // re-enable it, which clears the budget.
  if (modFaultTracker().isDisabled(modInfoStorage.modId)) {
    preloaderLogger.warn(
        "Refusing to load mod {}: it faulted {} times in a row and is disabled until re-enabled",
        modInfoStorage.modId, modFaultTracker().faultsFor(modInfoStorage.modId));
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
    return false;
  }

  auto entry = createLoadedModEntry(handle, vm, modInfoStorage,
                                    normalizedLibraryPath);
  if (!entry.has_value()) {
    // The mod loaded as a library but its lifecycle entry failed. That is this mod's
    // fault and no other mod's, so it is counted here and the rest of the set keeps loading.
    const ModFaultDecision decision = modFaultTracker().recordFault(modInfoStorage.modId);
    if (decision.action == ModFaultAction::Disable) {
      preloaderLogger.error(
          "Disabling mod {} after {} consecutive faults; re-enable it to retry",
          modInfoStorage.modId, decision.faultCount);
    }
    return false;
  }

  // A mod that reached this point loaded successfully, so its fault budget is reset.
  modFaultTracker().recordSuccess(modInfoStorage.modId);
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
      continue;
    }

    // An enable that fails is isolated the same way a load that fails is. The mod is
    // disabled after its budget is spent, and the loop keeps going so one broken mod cannot stop
    // every other mod from enabling.
    const ModFaultDecision decision = modFaultTracker().recordFault(entry->modId);
    if (decision.action == ModFaultAction::Disable) {
      preloaderLogger.error(
          "Disabling mod {} after {} consecutive enable faults; re-enable it to retry",
          entry->modId, decision.faultCount);
    }
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
