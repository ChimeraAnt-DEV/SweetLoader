/**
 * @file BerryApiBridge.cpp
 * @brief dlopen-based bridge to the bundled BerryAPI runtime. See BerryApiBridge.h.
 */

#include "pl/runtime/BerryApiBridge.h"

#include <dlfcn.h>

#include <atomic>
#include <mutex>

namespace pl::berry {
namespace {

/** The BerryAPI library, resolved from the app's native library directory. */
constexpr const char *kLibraryName = "libBerryAPI.so";

// The C ABI BerryAPI exports. Read-only after the first successful load.
using InjectFn = bool (*)();
using IsAttachedFn = bool (*)();
using VersionFn = const char *(*)();

std::mutex g_loadMutex;
std::atomic<bool> g_attempted{false};
std::atomic<bool> g_loaded{false};
std::atomic<bool> g_hasAttachQuery{false};
std::atomic<bool> g_attachAttempted{false};
InjectFn g_inject = nullptr;
IsAttachedFn g_isAttached = nullptr;
VersionFn g_version = nullptr;
std::string g_loadError;

/**
 * @brief Resolves one symbol, or null.
 *
 * <p>`dlsym` returns null for both "absent" and "null value"; for function pointers that ambiguity
 * is harmless here, because a null function pointer is exactly the "not usable" answer we want.
 */
template <typename T>
T Resolve(void *handle, const char *name) {
  return reinterpret_cast<T>(dlsym(handle, name));
}

void LoadOnce() {
  if (g_attempted.exchange(true, std::memory_order_acq_rel)) return;

  // RTLD_NOW: resolve every relocation up front so a broken dependency (libdobby.so) fails the
  // load here, where it is reported, rather than faulting later inside a game callback.
  void *handle = dlopen(kLibraryName, RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char *err = dlerror();
    g_loadError = err != nullptr ? err : "dlopen failed";
    return;
  }

  g_version = Resolve<VersionFn>(handle, "BERRY_Version");
  g_isAttached = Resolve<IsAttachedFn>(handle, "BERRY_IsAttached");
  g_inject = Resolve<InjectFn>(handle, "BERRY_Inject");

  // The version string is the cheapest proof the ABI is the one we expect; without it the library
  // is not BerryAPI as this bridge understands it, so it is treated as absent.
  if (g_version == nullptr) {
    g_loadError = "libBerryAPI.so loaded but BERRY_Version is missing";
    dlclose(handle);
    g_version = nullptr;
    return;
  }

  // BERRY_IsAttached is the capability query; when it is missing we can still report the library,
  // but the attach bit stays clear (fail closed).
  g_hasAttachQuery.store(g_isAttached != nullptr, std::memory_order_relaxed);
  g_loaded.store(true, std::memory_order_release);
  // The handle is deliberately never closed: BerryAPI owns threads and hooks once attached.
}

} // namespace

Capability operator|(Capability a, Capability b) {
  return static_cast<Capability>(static_cast<std::uint32_t>(a) |
                                 static_cast<std::uint32_t>(b));
}

Capability operator&(Capability a, Capability b) {
  return static_cast<Capability>(static_cast<std::uint32_t>(a) &
                                 static_cast<std::uint32_t>(b));
}

bool HasCapability(Capability mask, Capability flag) {
  const std::uint32_t m = static_cast<std::uint32_t>(mask);
  const std::uint32_t f = static_cast<std::uint32_t>(flag);
  return (m & f) == f;
}

bool EnsureLoaded() {
  LoadOnce();
  return g_loaded.load(std::memory_order_acquire);
}

bool Available() { return g_loaded.load(std::memory_order_acquire); }

std::string Version() {
  if (!g_loaded.load(std::memory_order_acquire) || g_version == nullptr) return std::string();
  const char *v = g_version();
  return v != nullptr ? std::string(v) : std::string();
}

bool Attached() {
  if (!g_loaded.load(std::memory_order_acquire)) return false;
  if (!g_hasAttachQuery.load(std::memory_order_relaxed) || g_isAttached == nullptr) return false;
  return g_isAttached();
}

bool EnsureAttached() {
  // Idempotent: attempt the attach at most once per process. A second attempt after a failure
  // would install a half-built hook set, which is worse than reporting the failure.
  if (g_attachAttempted.exchange(true, std::memory_order_acq_rel)) return Attached();
  if (!EnsureLoaded()) return false;
  if (Attached()) return true;
  if (g_inject == nullptr) return false;
  // BerryAPI checks its own hook backend and returns false when it has none; a false here is a
  // reported failure, not a crash, so the caller can grey the dependent features.
  g_inject();
  return Attached();
}

Capability Capabilities() {
  Capability mask = Capability::None;
  if (!g_loaded.load(std::memory_order_acquire)) return mask;
  mask = mask | Capability::LibraryLoaded;
  if (Attached()) mask = mask | Capability::Attached;
  // Capability::WorldFeed stays clear: no LevelSeam is bound in this build.
  return mask;
}

std::string Summary() {
  if (!EnsureLoaded()) {
    return "BerryAPI: not available (" + (g_loadError.empty() ? "not bundled" : g_loadError) + ")";
  }
  const std::string version = Version();
  std::string line = "BerryAPI: loaded";
  if (!version.empty()) line += " v" + version;
  line += Attached() ? ", attached" : ", not attached";
  if (!HasCapability(Capabilities(), Capability::WorldFeed)) {
    line += ", world feed unavailable";
  }
  return line;
}

} // namespace pl::berry
