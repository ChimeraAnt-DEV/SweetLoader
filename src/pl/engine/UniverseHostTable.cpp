#include "pl/engine/UniverseHostTable.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/Patch.hpp"
#include "pl/memory/Signature.hpp"
#include "pl/runtime/BerryApiBridge.h"
#include "pl/runtime/GameLocalPlayer.h"

namespace pl::engine {
namespace {

// The table's field order is the ABI. Pin the offsets so a field added or reordered here without a
// matching change to `universe_api.h` (and a bump of UNIVERSE_HOST_ABI_VERSION) fails the build
// rather than misreading a pointer on device. The C++ side builds the table the Rust side mirrors.
static_assert(offsetof(universe_host_t, size) == 0, "host table ABI: size must be first");
static_assert(offsetof(universe_host_t, abi_version) == sizeof(uint32_t),
              "host table ABI: abi_version must follow size");
static_assert(offsetof(universe_host_t, register_hook) == 2 * sizeof(uint32_t),
              "host table ABI: the function pointers must follow the two u32s");

constexpr const char *kDefaultModule = "libminecraftpe.so";

// The most signatures one session may hook at once. A bound rather than a growable pool because the
// detour veneers are a fixed, compiled-in set (one per slot); a mod that needs more than this is
// refused with a 0 handle rather than silently sharing a slot.
constexpr std::size_t kMaxHostHooks = 16;

struct HostHookSlot {
  std::atomic<bool> inUse{false};
  /// Set by the veneer when the hooked function runs; cleared by the pump after it fires the callback.
  std::atomic<bool> pending{false};
  /// The resolved function the veneer is installed on, so it can be unhooked by target.
  void *target = nullptr;
  /// The original trampoline the veneer tail-branches to. Written once, before the hook installs.
  void *original = nullptr;
  universe_hook_fn callback = nullptr;
  void *context = nullptr;
};

HostHookSlot g_slots[kMaxHostHooks];
std::mutex g_hookMutex;

const char *ModuleOrDefault(const char *module) {
  return (module != nullptr && module[0] != '\0') ? module : kDefaultModule;
}

// -- The host capabilities ----------------------------------------------------------------------

/// Reads `length` bytes from `address` into `out`, refusing anything that is not a readable mapping.
bool HostReadMemory(uintptr_t address, void *out, std::size_t length) {
  if (out == nullptr || length == 0) return false;
  // readBytes checks /proc/self/maps before dereferencing and returns an empty vector on a bad
  // range, so a mod cannot fault the game thread by asking to read an unmapped address.
  const std::vector<uint8_t> bytes = pl::memory::readBytes(address, length);
  if (bytes.size() != length) return false;
  std::memcpy(out, bytes.data(), length);
  return true;
}

/// Writes `length` bytes to `address`. The permission gate is applied by the loader's reduction, so
/// a mod that reaches this function was granted `write_memory` (and is trusted-signed).
bool HostWriteMemory(uintptr_t address, const void *data, std::size_t length) {
  if (data == nullptr || length == 0) return false;
  const auto *bytes = static_cast<const uint8_t *>(data);
  return pl::memory::writeBytes(address, std::span<const uint8_t>(bytes, length),
                                "universe_mod_write");
}

/// The local player's position and rotation, when the world feed is bound this frame.
bool HostGetLocalPlayer(float out_position[3], float out_rotation[2]) {
  if (out_position == nullptr || out_rotation == nullptr) return false;
  const bool havePosition = pl::runtime::ReadLocalPlayerPosition(out_position);
  const bool haveRotation = pl::runtime::ReadLocalPlayerRotation(out_rotation);
  return havePosition && haveRotation;
}

const char *HostBerryApiVersion() {
  // A process-lifetime static so the pointer stays valid; an empty version reports null.
  static const std::string version = pl::berry::Version();
  return version.empty() ? nullptr : version.c_str();
}

void HostLog(int level, const char *message) {
  if (message == nullptr) return;
  switch (level) {
    case UNIVERSE_LOG_ERROR:
      sweetLoaderLogger.error("[universe] {}", message);
      break;
    case UNIVERSE_LOG_WARN:
      sweetLoaderLogger.warn("[universe] {}", message);
      break;
    case UNIVERSE_LOG_INFO:
      sweetLoaderLogger.info("[universe] {}", message);
      break;
    default:
      sweetLoaderLogger.debug("[universe] {}", message);
      break;
  }
}

// -- register_hook ------------------------------------------------------------------------------

// The veneers below are the detours a signature hook installs. They are register-preserving
// passthroughs (the same technique as RegisterAwareHook): each saves every volatile register, calls
// its record stub (which marks the slot pending and returns the original trampoline), restores the
// registers, and tail-branches to the original. The hooked function's return value is preserved, and
// the mod's callback is *not* called here — it is fired later by the pump, so mod code never runs on
// the hooked thread's unknown ABI.

/// Records that slot `index` fired and returns its original trampoline for the veneer to branch to.
extern "C" void *pl_universe_host_record(long index) {
  if (index >= 0 && static_cast<std::size_t>(index) < kMaxHostHooks) {
    HostHookSlot &slot = g_slots[index];
    if (slot.inUse.load(std::memory_order_acquire)) {
      slot.pending.store(true, std::memory_order_release);
      return slot.original;
    }
  }
  return nullptr;
}

// A register-preserving detour veneer per slot. Each saves every volatile register, calls its record
// stub (which marks the slot pending and returns the original trampoline), restores the registers,
// and tail-branches to the original. The hooked function's return value is preserved, and the mod's
// callback is *not* called here — it is fired later by the pump, so mod code never runs on the
// hooked thread's unknown ABI.
//
// Off-device (the host test build) there is no AArch64 and no game to hook, so the veneer is a plain
// function that makes the same record call: the table stays compilable and host-testable, while the
// asm path is exercised only by the device build.
#if defined(__aarch64__)
#define UNIVERSE_HOST_VENEER(index)                                                     \
  extern "C" __attribute__((naked, used, noinline)) void pl_universe_hook_##index(       \
      void) {                                                                           \
    __asm__ __volatile__(                                                               \
        "stp q0, q1, [sp, #-32]!\n"                                                      \
        "stp q2, q3, [sp, #-32]!\n"                                                      \
        "stp q4, q5, [sp, #-32]!\n"                                                      \
        "stp q6, q7, [sp, #-32]!\n"                                                      \
        "stp x0, x1, [sp, #-16]!\n"                                                      \
        "stp x2, x3, [sp, #-16]!\n"                                                      \
        "stp x4, x5, [sp, #-16]!\n"                                                      \
        "stp x6, x7, [sp, #-16]!\n"                                                      \
        "stp x8, x9, [sp, #-16]!\n"                                                      \
        "stp x10, x11, [sp, #-16]!\n"                                                    \
        "stp x12, x13, [sp, #-16]!\n"                                                    \
        "stp x14, x15, [sp, #-16]!\n"                                                    \
        "stp x16, x17, [sp, #-16]!\n"                                                    \
        "stp x18, x30, [sp, #-16]!\n"                                                    \
        "mov x0, #" #index "\n"                                                          \
        "bl pl_universe_host_record\n"                                                   \
        "str x0, [sp, #16]\n"                                                            \
        "ldp x18, x30, [sp], #16\n"                                                      \
        "ldr x16, [sp]\n"                                                                \
        "add sp, sp, #16\n"                                                              \
        "ldp x14, x15, [sp], #16\n"                                                      \
        "ldp x12, x13, [sp], #16\n"                                                      \
        "ldp x10, x11, [sp], #16\n"                                                      \
        "ldp x8, x9, [sp], #16\n"                                                        \
        "ldp x6, x7, [sp], #16\n"                                                        \
        "ldp x4, x5, [sp], #16\n"                                                        \
        "ldp x2, x3, [sp], #16\n"                                                        \
        "ldp x0, x1, [sp], #16\n"                                                        \
        "ldp q6, q7, [sp], #32\n"                                                        \
        "ldp q4, q5, [sp], #32\n"                                                        \
        "ldp q2, q3, [sp], #32\n"                                                        \
        "ldp q0, q1, [sp], #32\n"                                                        \
        "br x16\n"                                                                       \
        :                                                                                \
        :                                                                                \
        : "memory");                                                                     \
  }
#else
#define UNIVERSE_HOST_VENEER(index)                                       \
  extern "C" void pl_universe_hook_##index(void) {                        \
    (void)pl_universe_host_record(index);                                 \
  }
#endif

UNIVERSE_HOST_VENEER(0)
UNIVERSE_HOST_VENEER(1)
UNIVERSE_HOST_VENEER(2)
UNIVERSE_HOST_VENEER(3)
UNIVERSE_HOST_VENEER(4)
UNIVERSE_HOST_VENEER(5)
UNIVERSE_HOST_VENEER(6)
UNIVERSE_HOST_VENEER(7)
UNIVERSE_HOST_VENEER(8)
UNIVERSE_HOST_VENEER(9)
UNIVERSE_HOST_VENEER(10)
UNIVERSE_HOST_VENEER(11)
UNIVERSE_HOST_VENEER(12)
UNIVERSE_HOST_VENEER(13)
UNIVERSE_HOST_VENEER(14)
UNIVERSE_HOST_VENEER(15)

#undef UNIVERSE_HOST_VENEER

using VeneerFn = void (*)(void);
VeneerFn HostVeneerFor(std::size_t index) {
  static const VeneerFn veneers[kMaxHostHooks] = {
      pl_universe_hook_0,  pl_universe_hook_1,  pl_universe_hook_2,  pl_universe_hook_3,
      pl_universe_hook_4,  pl_universe_hook_5,  pl_universe_hook_6,  pl_universe_hook_7,
      pl_universe_hook_8,  pl_universe_hook_9,  pl_universe_hook_10, pl_universe_hook_11,
      pl_universe_hook_12, pl_universe_hook_13, pl_universe_hook_14, pl_universe_hook_15};
  return index < kMaxHostHooks ? veneers[index] : nullptr;
}

/// Resolves `signature` in `module` and installs the slot's passthrough veneer on it.
uint64_t HostRegisterHook(const char *signature, const char *module, universe_hook_fn callback,
                          void *context) {
  if (signature == nullptr || callback == nullptr) return 0;
  const uintptr_t target =
      pl::memory::resolveSignature(signature, ModuleOrDefault(module));
  if (target == 0) return 0;

  std::lock_guard<std::mutex> lock(g_hookMutex);
  for (std::size_t index = 0; index < kMaxHostHooks; ++index) {
    HostHookSlot &slot = g_slots[index];
    bool expected = false;
    if (!slot.inUse.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      continue;
    }
    slot.callback = callback;
    slot.context = context;
    slot.pending.store(false, std::memory_order_relaxed);
    void *original = nullptr;
    const int result = pl::memory::hook(reinterpret_cast<void *>(target),
                                        reinterpret_cast<void *>(HostVeneerFor(index)),
                                        &original, pl::memory::HookPriority::Normal,
                                        "universe_signature_hook");
    if (result != 0 || original == nullptr) {
      slot.inUse.store(false, std::memory_order_release);
      return 0;
    }
    slot.original = original;
    slot.target = reinterpret_cast<void *>(target);
    // 1-based so a 0 handle unambiguously means "not installed".
    return static_cast<uint64_t>(index) + 1;
  }
  return 0;
}

void HostUnregisterHook(uint64_t handle) {
  if (handle == 0 || handle > kMaxHostHooks) return;
  const std::size_t index = static_cast<std::size_t>(handle - 1);
  std::lock_guard<std::mutex> lock(g_hookMutex);
  HostHookSlot &slot = g_slots[index];
  if (!slot.inUse.load(std::memory_order_acquire)) return;
  if (slot.target != nullptr) {
    pl::memory::unhook(slot.target, reinterpret_cast<void *>(HostVeneerFor(index)));
  }
  slot.original = nullptr;
  slot.target = nullptr;
  slot.callback = nullptr;
  slot.context = nullptr;
  slot.pending.store(false, std::memory_order_relaxed);
  slot.inUse.store(false, std::memory_order_release);
}

uintptr_t HostResolveSignature(const char *signature, const char *module) {
  if (signature == nullptr) return 0;
  return pl::memory::resolveSignature(signature, ModuleOrDefault(module));
}

const universe_host_t kHostTable = {
    sizeof(universe_host_t),
    UNIVERSE_HOST_ABI_VERSION,
    HostRegisterHook,
    HostUnregisterHook,
    HostResolveSignature,
    HostReadMemory,
    HostWriteMemory,
    HostGetLocalPlayer,
    HostBerryApiVersion,
    HostLog,
};

} // namespace

const universe_host_t *UniverseFullHostTable() { return &kHostTable; }

void UniversePumpHostHooks(unsigned long long frame) {
  for (std::size_t index = 0; index < kMaxHostHooks; ++index) {
    HostHookSlot &slot = g_slots[index];
    if (!slot.inUse.load(std::memory_order_acquire)) continue;
    if (!slot.pending.exchange(false, std::memory_order_acq_rel)) continue;
    universe_hook_fn callback = slot.callback;
    if (callback != nullptr) {
      callback(static_cast<uint64_t>(frame), slot.context);
    }
  }
}

void UniverseResetHostHooks() {
  for (std::size_t index = 0; index < kMaxHostHooks; ++index) {
    HostUnregisterHook(static_cast<uint64_t>(index) + 1);
  }
}

} // namespace pl::engine
