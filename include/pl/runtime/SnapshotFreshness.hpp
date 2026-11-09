#pragma once

/**
 * @file SnapshotFreshness.hpp
 * @brief The pure rule that decides whether a local-player snapshot is still live.
 *
 * A snapshot is stamped when the game calls its local-player accessor. During play the accessor runs
 * every frame, so the stamp is always fresh. The problem is teardown: when the session ends the
 * accessor stops being called and the last snapshot keeps reading as fresh for the whole window, so
 * the launcher reports a player position for up to two seconds after the game is gone.
 *
 * The fix uses a second signal that stops immediately at teardown: the present call
 * (`eglSwapBuffers`). While a world is up the game presents every frame; when it stops, no present
 * arrives. So when a present hook is live and the last present is older than the gap, the game has
 * stopped and the snapshot is stale at once, not after the window. On a Vulkan-only build there is no
 * present hook, and the window alone decides, exactly as before.
 *
 * Pure and header-only so the whole rule is host-tested (`SnapshotFreshnessTest`).
 */

#include <cstdint>

namespace pl::runtime {

/** The inputs the freshness rule needs, all in monotonic milliseconds. */
struct FreshnessInputs {
  /** The current monotonic time. */
  long long nowMs = 0;
  /** The snapshot's publish time; 0 means "no snapshot". */
  long long stampMs = 0;
  /** How long a snapshot stays fresh after its stamp. */
  long long freshWindowMs = 2000;
  /** Whether a present (eglSwapBuffers) hook is installed and counting frames. */
  bool presentTracked = false;
  /** The last present time; 0 means "no present seen yet". */
  long long lastPresentMs = 0;
  /** No present for longer than this means the game has stopped presenting. */
  long long presentGapMs = 1500;
};

/**
 * @brief Whether the snapshot is still live enough to report.
 *
 * @return false when there is no snapshot, when it is older than the freshness window, or when a
 *         present hook is live but no frame has been presented for longer than the gap.
 */
inline bool IsSnapshotFresh(const FreshnessInputs &in) {
  if (in.stampMs == 0) return false;
  const long long age = in.nowMs - in.stampMs;
  // A future stamp (a clock stepped backwards) keeps the last good read rather than dropping it.
  if (age > in.freshWindowMs) return false;
  if (in.presentTracked && in.lastPresentMs != 0 && in.nowMs - in.lastPresentMs > in.presentGapMs) {
    return false;
  }
  return true;
}

} // namespace pl::runtime
