/**
 * @file UniverseBridge.cpp
 * @brief Resolves the `.universe` Rust loader's C ABI. See UniverseApi.h.
 */

#include "pl/engine/UniverseApi.h"

#include <dlfcn.h>

#include <mutex>

namespace pl::engine {
namespace {

// The loader's library name. The JVM loads it with System.loadLibrary("caramel_universe"); this
// bridge only ever *looks it up* with RTLD_NOLOAD, so it can never load a second copy or leak a
// handle. A null lookup means the launcher has not loaded it yet, which is a reported "unavailable"
// rather than an error.
constexpr const char *kLibraryName = "libcaramel_universe.so";

// The C ABI the loader exports. `init` takes the BerryAPI pointer; the rest are queries.
using InitFn = char *(*)(void *);
using ValidateFn = char *(*)(const char *);
using ExtractFn = char *(*)(const char *, const char *, const char *);
using FreeFn = void (*)(char *);
using IsCancelledFn = bool (*)();
using SetCancelledFn = void (*)(bool);
using ClientVersionFn = const char *(*)();
using VerifySignatureFn = char *(*)(const char *, const char *, const char *);
using CheckPermissionsFn = char *(*)(const char *, const char *, bool);
using ResolveLoadOrderFn = char *(*)(const char *);
using LoadModsFn = char *(*)(const char *, const void *);
using TickModsFn = void (*)(float);
using ShutdownModsFn = void (*)();
using LiveModsFn = char *(*)();

std::once_flag g_once;
bool g_loaded = false;
InitFn g_init = nullptr;
ValidateFn g_validate = nullptr;
ExtractFn g_extract = nullptr;
FreeFn g_free = nullptr;
IsCancelledFn g_isCancelled = nullptr;
SetCancelledFn g_setCancelled = nullptr;
ClientVersionFn g_clientVersion = nullptr;
VerifySignatureFn g_verifySignature = nullptr;
CheckPermissionsFn g_checkPermissions = nullptr;
ResolveLoadOrderFn g_resolveLoadOrder = nullptr;
LoadModsFn g_loadMods = nullptr;
TickModsFn g_tickMods = nullptr;
ShutdownModsFn g_shutdownMods = nullptr;
LiveModsFn g_liveMods = nullptr;

template <typename T>
T Resolve(void *handle, const char *name) {
  return reinterpret_cast<T>(dlsym(handle, name));
}

void LoadOnce() {
  // RTLD_NOLOAD: only return a handle if the JVM already loaded the library. RTLD_NOW resolves its
  // relocations up front; combined with NOLOAD it never triggers a fresh load.
  void *handle = dlopen(kLibraryName, RTLD_NOW | RTLD_NOLOAD);
  if (handle == nullptr) return;

  g_init = Resolve<InitFn>(handle, "caramel_universe_init");
  g_validate = Resolve<ValidateFn>(handle, "caramel_universe_validate");
  g_extract = Resolve<ExtractFn>(handle, "caramel_universe_extract");
  g_free = Resolve<FreeFn>(handle, "caramel_universe_free");
  g_isCancelled = Resolve<IsCancelledFn>(handle, "caramel_universe_is_cancelled");
  g_setCancelled = Resolve<SetCancelledFn>(handle, "caramel_universe_set_cancelled");
  g_clientVersion = Resolve<ClientVersionFn>(handle, "caramel_universe_client_version");
  g_verifySignature = Resolve<VerifySignatureFn>(handle, "caramel_universe_verify_signature");
  g_checkPermissions = Resolve<CheckPermissionsFn>(handle, "caramel_universe_check_permissions");
  g_resolveLoadOrder = Resolve<ResolveLoadOrderFn>(handle, "caramel_universe_resolve_load_order");
  g_loadMods = Resolve<LoadModsFn>(handle, "caramel_universe_load_mods");
  g_tickMods = Resolve<TickModsFn>(handle, "caramel_universe_tick_mods");
  g_shutdownMods = Resolve<ShutdownModsFn>(handle, "caramel_universe_shutdown_mods");
  g_liveMods = Resolve<LiveModsFn>(handle, "caramel_universe_live_mods");

  // The cancel/free pair is required for the loader to be usable at all; without it a message this
  // bridge produced could not be freed and the cancellation flag could not be read. Requiring both
  // makes "available" mean the whole surface, not a subset.
  g_loaded = g_free != nullptr && g_isCancelled != nullptr && g_setCancelled != nullptr;
  // The handle is intentionally not closed: the library is owned by the JVM's class loader.
}

bool TakeMessage(char *message, std::string *outError) {
  if (message == nullptr) {
    if (outError != nullptr) outError->clear();
    return true;
  }
  if (outError != nullptr) *outError = message;
  if (g_free != nullptr) g_free(message);
  return false;
}

} // namespace

bool EnsureUniverseLoaded() {
  std::call_once(g_once, LoadOnce);
  return g_loaded;
}

bool UniverseAvailable() { return EnsureUniverseLoaded(); }

std::string UniverseClientVersion() {
  if (!EnsureUniverseLoaded() || g_clientVersion == nullptr) return std::string();
  const char *version = g_clientVersion();
  return version != nullptr ? std::string(version) : std::string();
}

bool UniverseInit(void *berryApiPointer) {
  if (!EnsureUniverseLoaded() || g_init == nullptr) return false;
  // `init` returns a message pointer, which the loader only produces on a real failure.
  return TakeMessage(g_init(berryApiPointer), nullptr);
}

void FreeMessage(char *message) {
  if (message == nullptr) return;
  if (EnsureUniverseLoaded() && g_free != nullptr) {
    g_free(message);
    return;
  }
  // The loader is not resolvable, so a message cannot have come from it; there is nothing this
  // bridge may safely free.
}

bool UniverseValidate(const std::string &archivePath, std::string *outError) {
  if (!EnsureUniverseLoaded() || g_validate == nullptr) {
    if (outError != nullptr) *outError = "the .universe loader is not loaded";
    return false;
  }
  return TakeMessage(g_validate(archivePath.c_str()), outError);
}

bool UniverseExtract(const std::string &archivePath,
                     const std::string &sandboxRoot,
                     const std::string &allowedRoot,
                     std::string *outError) {
  if (!EnsureUniverseLoaded() || g_extract == nullptr) {
    if (outError != nullptr) *outError = "the .universe loader is not loaded";
    return false;
  }
  return TakeMessage(
          g_extract(archivePath.c_str(), sandboxRoot.c_str(), allowedRoot.c_str()), outError);
}

bool UniverseIsCancelled() {
  if (!EnsureUniverseLoaded() || g_isCancelled == nullptr) return false;
  return g_isCancelled();
}

void UniverseSetCancelled(bool cancelled) {
  if (!EnsureUniverseLoaded() || g_setCancelled == nullptr) return;
  g_setCancelled(cancelled);
}

bool UniverseVerifySignature(const std::string &archivePath,
                             const std::string &signatureHex,
                             const std::string &trustedKeysHex,
                             std::string *outError) {
  if (!EnsureUniverseLoaded() || g_verifySignature == nullptr) {
    if (outError != nullptr) *outError = "the .universe loader is not loaded";
    return false;
  }
  return TakeMessage(
          g_verifySignature(archivePath.c_str(), signatureHex.c_str(), trustedKeysHex.c_str()),
          outError);
}

bool UniverseCheckPermissions(const std::string &declaredCsv,
                              const std::string &requestedCsv,
                              bool trusted,
                              std::string *outError) {
  if (!EnsureUniverseLoaded() || g_checkPermissions == nullptr) {
    if (outError != nullptr) *outError = "the .universe loader is not loaded";
    return false;
  }
  return TakeMessage(
          g_checkPermissions(declaredCsv.c_str(), requestedCsv.c_str(), trusted), outError);
}

std::string UniverseResolveLoadOrder(const std::string &nodesJson) {
  if (!EnsureUniverseLoaded() || g_resolveLoadOrder == nullptr) return std::string();
  // The load-order report is always a non-null JSON object, so it is returned directly rather than
  // through TakeMessage.
  char *report = g_resolveLoadOrder(nodesJson.c_str());
  if (report == nullptr) return std::string();
  std::string result(report);
  if (g_free != nullptr) g_free(report);
  return result;
}

std::string UniverseLoadMods(const std::string &specsJson, const void *hostTable) {
  if (!EnsureUniverseLoaded() || g_loadMods == nullptr) return std::string();
  char *report = g_loadMods(specsJson.c_str(), hostTable);
  if (report == nullptr) return std::string();
  std::string result(report);
  if (g_free != nullptr) g_free(report);
  return result;
}

void UniverseTickMods(float deltaSeconds) {
  if (!EnsureUniverseLoaded() || g_tickMods == nullptr) return;
  g_tickMods(deltaSeconds);
}

void UniverseShutdownMods() {
  if (!EnsureUniverseLoaded() || g_shutdownMods == nullptr) return;
  g_shutdownMods();
}

std::string UniverseLiveMods() {
  if (!EnsureUniverseLoaded() || g_liveMods == nullptr) return std::string();
  char *report = g_liveMods();
  if (report == nullptr) return std::string();
  std::string result(report);
  if (g_free != nullptr) g_free(report);
  return result;
}

} // namespace pl::engine
