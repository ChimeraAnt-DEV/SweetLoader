/**
 * @file SnapshotFreshnessTest.cpp
 * @brief Host test for the pure snapshot-freshness rule.
 *
 * The local-player feed published a snapshot and kept reporting it for up to 2000ms after the game
 * stopped calling the accessor, so a session that ended mid-frame reported a player position for two
 * seconds after teardown. The rule now also consults the present (eglSwapBuffers) signal: when a
 * present hook is live and no frame has been presented for a while, the game has stopped and the
 * snapshot is stale immediately, not after the window.
 */

#include <cstdio>
#include <cstdlib>

#include "pl/runtime/SnapshotFreshness.hpp"

using pl::runtime::FreshnessInputs;
using pl::runtime::IsSnapshotFresh;

namespace {
int checks = 0;
void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}
} // namespace

int main() {
  FreshnessInputs in;
  in.freshWindowMs = 2000;
  in.presentGapMs = 1500;

  // No snapshot at all.
  in.nowMs = 10000;
  in.stampMs = 0;
  in.presentTracked = false;
  expect(!IsSnapshotFresh(in), "no snapshot is never fresh");

  // A fresh snapshot within the window.
  in.stampMs = 9500;
  expect(IsSnapshotFresh(in), "a snapshot inside the window is fresh");

  // Inside the window but the game stopped presenting: teardown, stale now.
  in.presentTracked = true;
  in.lastPresentMs = 8000;
  expect(!IsSnapshotFresh(in), "no present for longer than the gap means teardown");

  // Presenting normally: fresh again.
  in.lastPresentMs = 9950;
  expect(IsSnapshotFresh(in), "a recent present keeps the snapshot fresh");

  // Past the freshness window even while presenting: stale.
  in.stampMs = 7000;
  in.lastPresentMs = 9950;
  expect(!IsSnapshotFresh(in), "past the window is stale even while presenting");

  // No present hook live (Vulkan-only): the window alone decides.
  in.presentTracked = false;
  in.stampMs = 9500;
  in.lastPresentMs = 0;
  expect(IsSnapshotFresh(in), "with no present hook the window alone decides");

  // A present hook is live but has never seen a present yet: the window alone decides.
  in.presentTracked = true;
  in.lastPresentMs = 0;
  expect(IsSnapshotFresh(in), "a present hook that has not fired yet does not force stale");

  // A stamp in the future (clock stepped backwards) keeps the last good read.
  in.stampMs = 11000;
  in.lastPresentMs = 9950;
  expect(IsSnapshotFresh(in), "a future stamp is treated as fresh");

  std::printf("SnapshotFreshnessTest: %d checks passed\n", checks);
  return 0;
}
