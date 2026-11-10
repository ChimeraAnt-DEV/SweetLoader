/**
 * @file BerryApiBridge.cpp
 * @brief dlopen-based bridge to the bundled BerryAPI runtime. See BerryApiBridge.h.
 */

#include "pl/runtime/BerryApiBridge.h"

#include "pl/runtime/BerryReplayMirror.hpp"
#include "pl/runtime/BerryGovernorMirror.hpp"

#include <dlfcn.h>

#include <atomic>
#include <mutex>

#include "pl/runtime/BerrySeamProvider.hpp"

// The local-player reading the seam provider marshals. Declared in GameLocalPlayer.h; forward
// declared here so this bridge does not pull the whole runtime in.
namespace pl::runtime {
bool IsLocalPlayerAvailable();
bool ReadLocalPlayerPosition(float out[3]);
bool ReadLocalPlayerRotation(float out[2]);
} // namespace pl::runtime

namespace pl::berry {
namespace {

/** The BerryAPI library, resolved from the app's native library directory. */
constexpr const char *kLibraryName = "libBerryAPI.so";

// The C ABI BerryAPI exports. Read-only after the first successful load.
//
// BERRY_Inject / BERRY_IsAttached return `int` (0/1), not C++ `bool`: BerryAPI's own header
// documents that a 1-byte bool return leaves the upper result-register bytes undefined, so a
// caller that declared `bool (*)()` could read a true as false. The typedefs match the real
// signature.
using InjectFn = int (*)();
using IsAttachedFn = int (*)();
using VersionFn = const char *(*)();
using SetWorldSeamFn = std::int32_t (*)(const void *);
using ClearWorldSeamFn = void (*)();
using HasWorldSeamFn = std::int32_t (*)();
using WorldSeamStructSizeFn = std::uint32_t (*)();
using WorldSeamAbiVersionFn = std::uint32_t (*)();
// The replay surface (BerryAPI replay ABI v1). Optional: a library without it still loads.
using GetReplayApiFn = const pl::berry::ReplayApiMirror *(*)();
using ReplayStructSizeFn = std::uint32_t (*)();
using ReplayAbiVersionFn = std::uint32_t (*)();
// The governor surface (BerryAPI governor ABI v1). Optional.
using GetGovernorApiFn = const pl::berry::GovernorApiMirror *(*)();
using GovernorStructSizeFn = std::uint32_t (*)();
using GovernorAbiVersionFn = std::uint32_t (*)();
// The cosmetics surface (BerryAPI cosmetic ABI v1). Optional.
using GetCosmeticApiFn = const pl::berry::CosmeticApiMirror *(*)();
using CosmeticStructSizeFn = std::uint32_t (*)();
using CosmeticAbiVersionFn = std::uint32_t (*)();

std::mutex g_loadMutex;
std::atomic<bool> g_attempted{false};
std::atomic<bool> g_loaded{false};
std::atomic<bool> g_hasAttachQuery{false};
std::atomic<bool> g_attachAttempted{false};
InjectFn g_inject = nullptr;
IsAttachedFn g_isAttached = nullptr;
VersionFn g_version = nullptr;
SetWorldSeamFn g_setWorldSeam = nullptr;
ClearWorldSeamFn g_clearWorldSeam = nullptr;
HasWorldSeamFn g_hasWorldSeam = nullptr;
WorldSeamStructSizeFn g_worldSeamStructSize = nullptr;
WorldSeamAbiVersionFn g_worldSeamAbiVersion = nullptr;
GetReplayApiFn g_getReplayApi = nullptr;
ReplayStructSizeFn g_replayStructSize = nullptr;
ReplayAbiVersionFn g_replayAbiVersion = nullptr;
GetGovernorApiFn g_getGovernorApi = nullptr;
GovernorStructSizeFn g_governorStructSize = nullptr;
GovernorAbiVersionFn g_governorAbiVersion = nullptr;
GetCosmeticApiFn g_getCosmeticApi = nullptr;
CosmeticStructSizeFn g_cosmeticStructSize = nullptr;
CosmeticAbiVersionFn g_cosmeticAbiVersion = nullptr;
std::atomic<bool> g_seamBound{false};
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
  // The world-seam surface (BerryAPI ABI v8). Optional: a library without it still loads and
  // attaches, and the world feed simply stays off.
  g_setWorldSeam = Resolve<SetWorldSeamFn>(handle, "BERRY_SetWorldSeam");
  g_clearWorldSeam = Resolve<ClearWorldSeamFn>(handle, "BERRY_ClearWorldSeam");
  g_hasWorldSeam = Resolve<HasWorldSeamFn>(handle, "BERRY_HasWorldSeam");
  g_worldSeamStructSize = Resolve<WorldSeamStructSizeFn>(handle, "BERRY_WorldSeamStructSize");
  g_worldSeamAbiVersion = Resolve<WorldSeamAbiVersionFn>(handle, "BERRY_WorldSeamAbiVersion");
  // The replay surface. Optional: a library built before it still loads and attaches.
  g_getReplayApi = Resolve<GetReplayApiFn>(handle, "BERRY_GetReplayApi");
  g_replayStructSize = Resolve<ReplayStructSizeFn>(handle, "BERRY_ReplayStructSize");
  g_replayAbiVersion = Resolve<ReplayAbiVersionFn>(handle, "BERRY_ReplayAbiVersion");
  // The governor surface. Optional.
  g_getGovernorApi = Resolve<GetGovernorApiFn>(handle, "BERRY_GetGovernorApi");
  g_governorStructSize = Resolve<GovernorStructSizeFn>(handle, "BERRY_GovernorStructSize");
  g_governorAbiVersion = Resolve<GovernorAbiVersionFn>(handle, "BERRY_GovernorAbiVersion");
  // The cosmetics surface. Optional.
  g_getCosmeticApi = Resolve<GetCosmeticApiFn>(handle, "BERRY_GetCosmeticApi");
  g_cosmeticStructSize = Resolve<CosmeticStructSizeFn>(handle, "BERRY_CosmeticStructSize");
  g_cosmeticAbiVersion = Resolve<CosmeticAbiVersionFn>(handle, "BERRY_CosmeticAbiVersion");

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
  return g_isAttached() != 0;
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
  // WorldFeed is set only once the launcher's local-player seam is actually bound, so a module
  // that needs a feed is never advertised as working on a promise.
  if (g_seamBound.load(std::memory_order_acquire)) mask = mask | Capability::WorldFeed;
  return mask;
}

std::string Summary() {
  if (!EnsureLoaded()) {
    return "BerryAPI: not available (" + (g_loadError.empty() ? "not bundled" : g_loadError) + ")";
  }
  const std::string version = Version();
  std::string line = "BerryAPI: loaded";
  if (!version.empty()) line += " v" + version;
  // In the launcher process the library loads but is not attached: attaching needs the game, so
  // the client arms it at session start. Say so, rather than reporting a bare "not attached" that
  // reads like a failure.
  line += Attached() ? ", attached" : ", armed at game launch";
  line += HasCapability(Capabilities(), Capability::WorldFeed) ? ", world feed bound"
                                                               : ", world feed unavailable";
  return line;
}

bool WorldSeamBound() { return g_seamBound.load(std::memory_order_acquire); }

bool BindWorldSeam() {
  if (!EnsureLoaded()) return false;
  if (g_setWorldSeam == nullptr || g_worldSeamStructSize == nullptr ||
      g_worldSeamAbiVersion == nullptr) {
    // The library predates the seam ABI (or was built without it): the world feed stays off.
    return false;
  }

  // Verify the host's mirrored struct against the library's own numbers before binding, so a
  // layout drift is caught by a number rather than by a misread function pointer.
  const SeamAbi reported{g_worldSeamStructSize(), g_worldSeamAbiVersion()};
  if (!MirrorMatches(reported, static_cast<std::uint32_t>(sizeof(SeamMirror)),
                     static_cast<std::uint32_t>(kHostSeamAbiVersion))) {
    return false;
  }

  // Fill only the slot this build can answer. Every other slot stays null, which BerryAPI reads as
  // its fail-closed default, so an unreachable read is "no data" rather than an invention.
  static SeamMirror seam{};
  seam.size = reported.struct_size;
  seam.abi_version = reported.abi_version;
  seam.user = nullptr;
  seam.get_local_player = [](LocalPlayerMirror *out, void *) -> std::int32_t {
    if (out == nullptr) return 0;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[2] = {0.0f, 0.0f};
    // A reading is "have data" only when both the position and the view rotation were read; a
    // partial read (one of the two) is reported as no data rather than a half-filled pose.
    const bool have = pl::runtime::ReadLocalPlayerPosition(position) &&
                      pl::runtime::ReadLocalPlayerRotation(rotation);
    *out = BuildLocalPlayerState(have, position, rotation);
    return 1;
  };

  if (g_setWorldSeam(&seam) == 0) {
    g_seamBound.store(false, std::memory_order_release);
    return false;
  }
  g_seamBound.store(true, std::memory_order_release);
  return true;
}

const pl::berry::ReplayApiMirror *ReplayApi() {
  if (!EnsureLoaded()) return nullptr;
  if (g_getReplayApi == nullptr || g_replayStructSize == nullptr || g_replayAbiVersion == nullptr) {
    // The library predates the replay ABI: the surface stays absent.
    return nullptr;
  }
  // Check the numbers before reading a single pointer, so a layout drift is refused by a number.
  const pl::berry::ReplayAbi reported{g_replayStructSize(), g_replayAbiVersion()};
  if (!pl::berry::ReplayMirrorMatches(reported, static_cast<std::uint32_t>(
                                                 sizeof(pl::berry::ReplayApiMirror)),
                                      pl::berry::kHostReplayAbiVersion)) {
    return nullptr;
  }
  const pl::berry::ReplayApiMirror *api = g_getReplayApi();
  if (api == nullptr) return nullptr;
  // A surface missing a call the replay screen drives is refused rather than called through null.
  if (!pl::berry::ReplayApiUsable(*api)) return nullptr;
  return api;
}

bool ReplayAvailable() { return ReplayApi() != nullptr; }

const pl::berry::GovernorApiMirror *GovernorApi() {
  if (!EnsureLoaded()) return nullptr;
  if (g_getGovernorApi == nullptr || g_governorStructSize == nullptr ||
      g_governorAbiVersion == nullptr) {
    return nullptr;
  }
  const pl::berry::GovernorAbi reported{g_governorStructSize(), g_governorAbiVersion()};
  if (!pl::berry::GovernorMirrorMatches(reported, static_cast<std::uint32_t>(
                                                     sizeof(pl::berry::GovernorApiMirror)),
                                        pl::berry::kHostGovernorAbiVersion)) {
    return nullptr;
  }
  const pl::berry::GovernorApiMirror *api = g_getGovernorApi();
  if (api == nullptr || !pl::berry::GovernorApiUsable(*api)) return nullptr;
  return api;
}

bool GovernorAvailable() { return GovernorApi() != nullptr; }

const pl::berry::CosmeticApiMirror *CosmeticApi() {
  if (!EnsureLoaded()) return nullptr;
  if (g_getCosmeticApi == nullptr || g_cosmeticStructSize == nullptr ||
      g_cosmeticAbiVersion == nullptr) {
    // The library predates the cosmetic ABI: the surface stays absent.
    return nullptr;
  }
  const pl::berry::CosmeticAbi reported{g_cosmeticStructSize(), g_cosmeticAbiVersion()};
  if (!pl::berry::CosmeticMirrorMatches(reported, static_cast<std::uint32_t>(
                                                     sizeof(pl::berry::CosmeticApiMirror)),
                                        pl::berry::kHostCosmeticAbiVersion)) {
    return nullptr;
  }
  const pl::berry::CosmeticApiMirror *api = g_getCosmeticApi();
  if (api == nullptr || !pl::berry::CosmeticApiUsable(*api)) return nullptr;
  return api;
}

bool CosmeticAvailable() { return CosmeticApi() != nullptr; }

} // namespace pl::berry
