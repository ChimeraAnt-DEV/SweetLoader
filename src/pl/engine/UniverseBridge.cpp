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

std::once_flag g_once;
bool g_loaded = false;
InitFn g_init = nullptr;
ValidateFn g_validate = nullptr;
ExtractFn g_extract = nullptr;
FreeFn g_free = nullptr;
IsCancelledFn g_isCancelled = nullptr;
SetCancelledFn g_setCancelled = nullptr;
ClientVersionFn g_clientVersion = nullptr;

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

} // namespace pl::engine
