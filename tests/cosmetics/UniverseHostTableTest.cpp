// Host test for the `.universe` host table (pl/engine/UniverseHostTable.cpp).
//
// The table's ABI layout and its per-call fail-closed behaviour are what a `.universe` mod depends
// on, and both are checkable without a device: the layout is pinned against the header, and the
// capabilities that need a live engine (signature resolution, memory access, the local player)
// must answer "no" rather than fault when the engine is not there. The facility stubs live in
// UniverseHostTestStubs.hpp so this test and the bridge test compile the same stand-ins. The
// positive paths that need the real game (a signature resolving in libminecraftpe.so, a hook firing
// from real game code) are covered by the device build.

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

#include "UniverseHostTestStubs.hpp"
#include "pl/engine/UniverseHostTable.h"

namespace ut = pl::universe_test;

namespace {

void theTableAdvertisesItsOwnSizeAndAbi() {
  const universe_host_t *table = pl::engine::UniverseFullHostTable();
  assert(table != nullptr);
  assert(table->size == sizeof(universe_host_t));
  assert(table->abi_version == UNIVERSE_HOST_ABI_VERSION);
}

void everyCapabilityPointerIsNonNull() {
  // The full table is fully populated; a capability is withdrawn by the loader's per-mod reduction,
  // not by a null here. A null in the full table would be an unbuildable capability.
  const universe_host_t *table = pl::engine::UniverseFullHostTable();
  assert(table->register_hook != nullptr);
  assert(table->unregister_hook != nullptr);
  assert(table->resolve_signature != nullptr);
  assert(table->read_memory != nullptr);
  assert(table->write_memory != nullptr);
  assert(table->get_local_player != nullptr);
  assert(table->berry_api_version != nullptr);
  assert(table->log != nullptr);
}

void resolveSignatureUsesTheCallersModuleAndFailsClosed() {
  const universe_host_t *table = pl::engine::UniverseFullHostTable();
  ut::signatureResolves = false;
  assert(table->resolve_signature("aa bb cc", "libminecraftpe.so") == 0);
  assert(ut::signatureSeen == "aa bb cc");
  assert(ut::moduleSeen == "libminecraftpe.so");
  ut::signatureResolves = true;
  assert(table->resolve_signature("aa bb cc", "libminecraftpe.so") == 0x1000u);
  // A null module defaults to the game module rather than faulting.
  ut::moduleSeen.clear();
  table->resolve_signature("aa", nullptr);
  assert(ut::moduleSeen == "libminecraftpe.so");
}

void readMemoryFailsClosedWhenTheRangeIsNotReadable() {
  const universe_host_t *table = pl::engine::UniverseFullHostTable();
  std::uint8_t out[4] = {};
  ut::readSucceeds = false;
  assert(!table->read_memory(0x1000u, out, sizeof(out)));
  ut::readSucceeds = true;
  assert(table->read_memory(0x1000u, out, sizeof(out)));
  assert(out[0] == 0xAB && out[3] == 0xAB);
  // A zero-length or null-out read is refused without calling the facility.
  assert(!table->read_memory(0x1000u, nullptr, 4));
  assert(!table->read_memory(0x1000u, out, 0));
}

void writeMemoryReportsTheFacilitysAnswer() {
  const universe_host_t *table = pl::engine::UniverseFullHostTable();
  const std::uint8_t data[2] = {0x01, 0x02};
  ut::writeSucceeds = false;
  assert(!table->write_memory(0x1000u, data, sizeof(data)));
  ut::writeSucceeds = true;
  assert(table->write_memory(0x1000u, data, sizeof(data)));
  assert(!table->write_memory(0x1000u, nullptr, sizeof(data)));
}

void getLocalPlayerNeedsBothPositionAndRotation() {
  const universe_host_t *table = pl::engine::UniverseFullHostTable();
  float position[3] = {};
  float rotation[2] = {};
  ut::playerAvailable = false;
  assert(!table->get_local_player(position, rotation));
  ut::playerAvailable = true;
  assert(table->get_local_player(position, rotation));
  assert(position[0] == 1.0f && rotation[1] == 5.0f);
  // A null output is refused rather than written through.
  assert(!table->get_local_player(nullptr, rotation));
}

void berryApiVersionIsAlwaysANonNullString() {
  const universe_host_t *table = pl::engine::UniverseFullHostTable();
  const char *version = table->berry_api_version();
  assert(version != nullptr);
  assert(std::strcmp(version, "9.9.9-test") == 0);
}

void registerHookResolvesThenInstalls() {
  pl::engine::UniverseResetHostHooks();
  const universe_host_t *table = pl::engine::UniverseFullHostTable();
  ut::signatureResolves = false;
  assert(table->register_hook("aa", nullptr, nullptr, nullptr) == 0);  // no callback is refused
  // A callback with an unresolvable signature is refused, not half-installed.
  auto callback = +[](std::uint64_t, void *) {};
  assert(table->register_hook("aa", nullptr, callback, nullptr) == 0);
  ut::signatureResolves = true;
  ut::hookInstalls = 0;
  const std::uint64_t handle = table->register_hook("aa", nullptr, callback, nullptr);
  assert(handle != 0);            // a resolvable signature installs
  assert(ut::hookInstalls == 1);  // the detour was installed exactly once
  // Unregistering is safe and idempotent.
  table->unregister_hook(handle);
  table->unregister_hook(handle);
  pl::engine::UniverseResetHostHooks();
}

void thePumpFiresARegisteredCallbackOncePerFrameItRan() {
  pl::engine::UniverseResetHostHooks();
  const universe_host_t *table = pl::engine::UniverseFullHostTable();
  ut::signatureResolves = true;
  int calls = 0;
  std::uint64_t lastFrame = 0;
  struct State {
    int *calls;
    std::uint64_t *lastFrame;
  } state{&calls, &lastFrame};
  auto callback = +[](std::uint64_t frame, void *context) {
    auto *s = static_cast<State *>(context);
    ++*s->calls;
    *s->lastFrame = frame;
  };
  const std::uint64_t handle = table->register_hook("aa", nullptr, callback, &state);
  assert(handle != 0);
  // Nothing has "run" yet, so the pump fires nothing.
  pl::engine::UniversePumpHostHooks(100);
  assert(calls == 0);
  // Simulate the hooked function running: the veneer calls the record stub with its slot index,
  // which marks the slot pending. This is the exact call the real detour makes.
  pl_universe_host_record(static_cast<long>(handle - 1));
  pl::engine::UniversePumpHostHooks(101);
  assert(calls == 1);         // fired exactly once
  assert(lastFrame == 101);   // with the frame the pump was given
  // The pending flag was cleared, so a second pump without another run fires nothing.
  pl::engine::UniversePumpHostHooks(102);
  assert(calls == 1);
  pl::engine::UniverseResetHostHooks();
}

void theLogCapabilityAcceptsEveryLevel() {
  const universe_host_t *table = pl::engine::UniverseFullHostTable();
  // A null message is dropped rather than dereferenced.
  table->log(UNIVERSE_LOG_ERROR, nullptr);
  table->log(UNIVERSE_LOG_DEBUG, "debug");
  table->log(UNIVERSE_LOG_INFO, "info");
  table->log(UNIVERSE_LOG_WARN, "warn");
  table->log(UNIVERSE_LOG_ERROR, "error");
  table->log(999, "unknown level");
}

} // namespace

int main() {
  theTableAdvertisesItsOwnSizeAndAbi();
  everyCapabilityPointerIsNonNull();
  resolveSignatureUsesTheCallersModuleAndFailsClosed();
  readMemoryFailsClosedWhenTheRangeIsNotReadable();
  writeMemoryReportsTheFacilitysAnswer();
  getLocalPlayerNeedsBothPositionAndRotation();
  berryApiVersionIsAlwaysANonNullString();
  registerHookResolvesThenInstalls();
  thePumpFiresARegisteredCallbackOncePerFrameItRan();
  theLogCapabilityAcceptsEveryLevel();
  std::cout << "UniverseHostTableTest: ok\n";
  return 0;
}
