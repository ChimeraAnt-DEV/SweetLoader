#include <atomic>
#include <string>

#include "pl/runtime/OptifineConfig.h"
#include "pl/runtime/OptifineMode.h"
#include "pl/runtime/optifine/OptifineDeviceRules.h"

/**
 * @file OptifineRefreshRate.cpp
 * @brief Tier-1 refresh-rate unlock (launcher-applied, sweetloader-reported).
 *
 * Only the launcher holds the {@code Window}/{@code Surface} needed to call
 * {@code Surface.setFrameRate}, so the request itself is made on the launcher side. The
 * sweetloader owns the *rule* (which rate to ask for) and the reported outcome, so the Settings
 * screen reads one state, not two.
 *
 * The launcher computes the target with {@code OptifineRefreshRateRule} (a Java port of
 * {@link SelectTargetRefreshRate}) and passes it in the config blob as
 * {@code refresh_rate_target}. This item then reports what was requested, or why nothing was.
 *
 * Fail-safe: a 60Hz-only panel, an unknown mode list, or the game already at the top rate all
 * mean there is nothing to unlock; the item reports "skipped" and the game is left alone.
 */

namespace pl::runtime {
namespace {

// Written from the launcher's JNI thread and read by the apply pass on another thread, so it
// must be atomic (audit item 14).
std::atomic<int> gRequestedHz{0};

void ApplyRefreshRate(bool enabled, OptifineItemState &state) {
  if (!enabled) return;
  const int hz = gRequestedHz.load(std::memory_order_relaxed);
  if (hz <= 0) {
    MarkOptifineSkipped(state, "no higher refresh rate available");
    return;
  }
  MarkOptifineActive(state, "requested " + std::to_string(hz) + " Hz");
}

struct Registrar {
  Registrar() { RegisterOptifineItem(OptifineItem::RefreshRate, false, &ApplyRefreshRate); }
};
Registrar gRegistrar;

} // namespace

/**
 * @brief Records the refresh rate the launcher requested.
 *
 * Called from the launcher before it launches the game, once it has resolved the target from
 * the display's supported modes. Kept in the sweetloader so the item's reported state comes from
 * the same place as every other item's.
 */
void SetOptifineRefreshRateTarget(int hz) { gRequestedHz.store(hz, std::memory_order_relaxed); }

} // namespace pl::runtime
