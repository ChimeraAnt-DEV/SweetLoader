#include "pl/runtime/GameLocalPlayer.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>

#include "pl/Logger.hpp"
#include "pl/ipc/PlayerSnapshot.hpp"
#include "pl/runtime/SnapshotFreshness.hpp"
#include "pl/runtime/SwapFrameBoundary.h"
#include "pl/memory/Hook.hpp"
#include "pl/memory/Vtable.hpp"

namespace pl::runtime {
namespace {

// The local player's entity origin (a Vec3) and view rotation (yaw, pitch, in
// degrees) on the Actor/LocalPlayer object. Both offsets are shared by every build
// the feed targets (1.26.33 / 1.26.50 / 1.26.51) and were confirmed against the
// 1.26.50.4 and 1.26.60.28 binaries.
constexpr std::size_t kActorPositionOffset = 0x230;
constexpr std::size_t kActorRotationOffset = 0x238;

// ClientInstance's vtable slot whose call returns the local player. Reached by name
// through the existing RTTI resolver, so no per-build code address is needed.
constexpr std::size_t kGetLocalPlayerSlot = 31;

constexpr const char *kGameModule = "libminecraftpe.so";

// A snapshot is considered live for this long after the last game call. The hook runs
// every frame while a world is loaded, so during play the stamp is always fresh; after
// teardown the readers stop reporting a position. The window is the backstop for a build
// with no present hook; when one is live the present gap catches teardown in a frame or
// two instead of leaving a stale position for the whole window (see SnapshotFreshness).
constexpr long long kSnapshotFreshMs = 2000;

// No present for longer than this, with a present hook live, means the game has stopped
// presenting and the session is over. Generous enough to survive a loading stall or a
// paused frame, short enough that a teardown is not reported as a live player.
constexpr long long kPresentGapMs = 1500;

void *(*g_origGetLocalPlayer)(void *) = nullptr;
std::atomic_bool g_hookInstalled{false};
// Set only once the detour actually installed. `g_hookInstalled` is an install *attempt* guard (the
// exchange prevents a second try); this flag is the outcome, so the availability report can tell a
// resolved seam from a failed one.
std::atomic_bool g_hookLive{false};

// The resolved {@code ClientInstance::getLocalPlayer} slot address, kept for the hook self-test
//. 0 until resolved.
std::atomic<std::uintptr_t> g_slotTarget{0};

// The game thread writes the snapshot, the Java UI thread reads it. The seqlock lives in one shared
// class (pl::ipc::PlayerSnapshot) so the ordering rules are fixed once and host-tested; a torn
// cross-field read (a new x with an old y) must not be observable.
pl::ipc::PlayerSnapshot g_snapshot;

// The last transform a read returned consistently. Served when a read is merely *contended* (the
// writer outran the retry budget) so a fast writer cannot make the UI flicker a position out of
// existence; the freshness rule still applies, so a genuinely stale last-good is dropped. Guarded
// by a mutex because the read runs on the UI thread and a recreate can race it.
std::mutex g_lastGoodMutex;
pl::ipc::PlayerTransform g_lastGood;
bool g_hasLastGood = false;

long long MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

bool FinitePosition(const float p[3]) {
  // A garbage read shows up as NaN/inf; a real Bedrock position is well within the
  // world border, so an absurd magnitude is treated as "no data" too.
  constexpr float kWorldLimit = 3.0e7f;
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(p[i]) || std::fabs(p[i]) > kWorldLimit) {
      return false;
    }
  }
  return true;
}

void PublishSnapshot(const void *localPlayer) {
  const auto *position = reinterpret_cast<const float *>(
      reinterpret_cast<std::uintptr_t>(localPlayer) + kActorPositionOffset);
  const auto *rotation = reinterpret_cast<const float *>(
      reinterpret_cast<std::uintptr_t>(localPlayer) + kActorRotationOffset);

  pl::ipc::PlayerTransform next;
  next.position[0] = position[0];
  next.position[1] = position[1];
  next.position[2] = position[2];
  next.rotation[0] = rotation[0];
  next.rotation[1] = rotation[1];
  if (!FinitePosition(next.position)) {
    return;
  }
  next.stampMs = MonotonicMs();
  g_snapshot.publish(next);
}

// Reads the latest snapshot, or false when none is live. The dereference of the local
// player happens in the hook (on the game thread, while the game itself just returned
// the pointer); this side only ever touches the copy, so a session ending can never
// leave the UI thread reading freed memory.
bool ReadSnapshot(float outPosition[3], float outRotation[2]) {
  pl::ipc::PlayerTransform transform;
  const pl::ipc::ReadResult result = g_snapshot.readWithResult(transform);
  if (result == pl::ipc::ReadResult::NotPublished) {
    return false;
  }
  if (result == pl::ipc::ReadResult::Contended) {
    std::lock_guard<std::mutex> lock(g_lastGoodMutex);
    if (!g_hasLastGood) return false;
    transform = g_lastGood;
  } else {
    std::lock_guard<std::mutex> lock(g_lastGoodMutex);
    g_lastGood = transform;
    g_hasLastGood = true;
  }
  FreshnessInputs fresh;
  fresh.nowMs = MonotonicMs();
  fresh.stampMs = transform.stampMs;
  fresh.freshWindowMs = kSnapshotFreshMs;
  fresh.presentGapMs = kPresentGapMs;
  // The present hook's "last present" is in microseconds; convert once here. `presentTracked` is
  // false on a Vulkan-only build (no libEGL present call), where the window alone decides.
  fresh.presentTracked = pl::runtime::IsSwapFrameBoundaryLive();
  if (fresh.presentTracked) {
    const long long lastPresentUs =
        pl_runtime_swap_buffers_last_us.load(std::memory_order_relaxed);
    fresh.lastPresentMs = lastPresentUs > 0 ? lastPresentUs / 1000LL : 0;
  }
  if (!IsSnapshotFresh(fresh)) {
    return false;
  }
  std::memcpy(outPosition, transform.position, sizeof(transform.position));
  std::memcpy(outRotation, transform.rotation, sizeof(transform.rotation));
  return true;
}

// The one hook: the client's own local-player accessor becomes the place we latch the
// live local player and snapshot its position/rotation, once per call.
void *HookGetLocalPlayer(void *self) {
  void *localPlayer = g_origGetLocalPlayer ? g_origGetLocalPlayer(self) : nullptr;
  if (localPlayer != nullptr) {
    PublishSnapshot(localPlayer);
  }
  return localPlayer;
}

} // namespace

void InitLocalPlayerSource() {
  if (g_hookInstalled.exchange(true, std::memory_order_relaxed)) {
    return;
  }

  const std::uintptr_t slot =
      pl::memory::resolveVtableFunction("14ClientInstance", kGetLocalPlayerSlot,
                                        kGameModule);
  if (!slot) {
    sweetLoaderLogger.warn(
        "Local-player feed: ClientInstance local-player slot unresolved; "
        "in-world voice nametag icons stay off");
    return;
  }

  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(slot),
                       reinterpret_cast<pl::memory::FuncPtr>(HookGetLocalPlayer),
                       reinterpret_cast<pl::memory::FuncPtr *>(
                           &g_origGetLocalPlayer),
                       pl::memory::HookPriority::Normal,
                       "ClientInstance::getLocalPlayer") != 0) {
    sweetLoaderLogger.warn("Local-player feed: hook install failed; icons stay off");
    return;
  }
  g_slotTarget.store(slot, std::memory_order_relaxed);
  g_hookLive.store(true, std::memory_order_relaxed);
}

bool IsLocalPlayerAvailable() {
  float position[3];
  float rotation[2];
  return ReadSnapshot(position, rotation);
}

std::uintptr_t LocalPlayerHookTarget() {
  return g_slotTarget.load(std::memory_order_relaxed);
}

bool IsLocalPlayerHookInstalled() {
  return g_hookInstalled.load(std::memory_order_relaxed) && g_hookLive.load(std::memory_order_relaxed);
}

bool ReadLocalPlayerPosition(float out[3]) {
  float rotation[2];
  return ReadSnapshot(out, rotation);
}

bool ReadLocalPlayerRotation(float out[2]) {
  float position[3];
  return ReadSnapshot(position, out);
}

} // namespace pl::runtime
