// Host test for the `.universe` Rust-loader bridge (pl/engine/UniverseBridge.cpp).
//
// The bridge resolves the loader's C ABI from the *already-loaded* library with RTLD_NOLOAD, so on
// the build host (where the Rust cdylib is never loaded) it must report "unavailable" and refuse
// every operation rather than calling through a null pointer. That fail-closed behaviour is the
// whole point of the bridge, so it is what this test pins. The positive path (the ABI resolving and
// being called) is covered by the loader's own Rust tests and by the device build, because it needs
// a loaded libcaramel_universe.so.

#include <cassert>
#include <iostream>
#include <string>

#include "pl/engine/UniverseApi.h"

namespace {

void theBridgeReportsUnavailableWhenTheLibraryIsNotLoaded() {
  // The Rust cdylib is not loaded on the host, so the bridge must say so.
  assert(!pl::engine::UniverseAvailable());
  assert(pl::engine::UniverseClientVersion().empty());
}

void everyOperationRefusesRatherThanCrashing() {
  std::string error;
  // Validate / extract must fail with a message, not fault.
  assert(!pl::engine::UniverseValidate("/nonexistent/x.universe", &error));
  assert(!error.empty());

  error.clear();
  assert(!pl::engine::UniverseExtract("/nonexistent/x.universe", "/tmp/sandbox", "/tmp", &error));
  assert(!error.empty());

  // Init must report failure rather than pretend it succeeded.
  assert(!pl::engine::UniverseInit(nullptr));

  // The cancellation flag is false (never set) and setting it is a safe no-op.
  assert(!pl::engine::UniverseIsCancelled());
  pl::engine::UniverseSetCancelled(true);
  assert(!pl::engine::UniverseIsCancelled());

  // The signature, permission and load-order seams must also refuse rather than fault.
  error.clear();
  assert(!pl::engine::UniverseVerifySignature("/nonexistent/x.universe", "00", "ab", &error));
  assert(!error.empty());

  error.clear();
  assert(!pl::engine::UniverseCheckPermissions("read_memory", "read_memory", false, &error));
  assert(!error.empty());

  assert(pl::engine::UniverseResolveLoadOrder("[]").empty());

  // The load lifecycle must also be a safe no-op when the loader is absent.
  assert(pl::engine::UniverseLoadMods("[]", nullptr).empty());
  assert(pl::engine::UniverseLiveMods().empty());
  pl::engine::UniverseTickMods(0.016f);
  pl::engine::UniverseShutdownMods();
}

void aNullMessageFreeIsSafe() {
  // Freeing null is explicitly a no-op, so a caller can free unconditionally.
  pl::engine::FreeMessage(nullptr);
}

} // namespace

int main() {
  theBridgeReportsUnavailableWhenTheLibraryIsNotLoaded();
  everyOperationRefusesRatherThanCrashing();
  aNullMessageFreeIsSafe();
  std::cout << "UniverseBridgeTest: ok\n";
  return 0;
}
