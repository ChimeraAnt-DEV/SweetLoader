#pragma once

/**
 * @file OptifineGovernor.h
 * @brief Pure hysteresis rule for dynamic render distance.
 *
 * The governor decides when the effective render distance should step down under frame pressure
 * and when it may step back up. It is pure so the rule — which is where the regressions live —
 * is unit-testable without a game.
 *
 * The asymmetry is deliberate: a drop happens after a short sustained dip, a restore only after
 * a long sustained recovery. Symmetric thresholds make the distance oscillate at the boundary,
 * which is more distracting than a slightly-too-low distance.
 */
namespace pl::runtime {

struct RenderDistanceConfig {
  int minDistance{4};
  int maxDistance{12};
  int fpsThreshold{45};
  /** Consecutive ms below threshold before stepping down one step. */
  long long dropSustainMs{1500};
  /** Consecutive ms above threshold before stepping back up one step. */
  long long restoreSustainMs{10000};
  /**
   * Chunks moved per step. Exposed to settings (item 383) so a device can trade a coarse distance
   * for fewer transitions, or a finer one for smoother adaptation.
   */
  int stepSize{1};
  /**
   * Minimum ms between any two changes. A dip that ends just as the down-timer expires, then a
   * recovery that ends just as the up-timer expires, would otherwise let the distance change on
   * two consecutive sustain windows and read as flicker; the dwell makes each change settle before
   * the next is allowed. Zero disables the dwell (the pre-existing behaviour).
   */
  long long minDwellMs{4000};
};

struct RenderDistanceState {
  int currentDistance{0};
  long long belowSinceMs{0};
  long long aboveSinceMs{0};
  /** Monotonic ms of the last accepted change; guards {@link RenderDistanceConfig::minDwellMs}. */
  long long lastChangeMs{0};
  /** Number of accepted changes, so the client can report each one (item 383). */
  int changeCount{0};
  bool initialized{false};
};

/**
 * @brief Feeds one FPS sample and returns the distance to use now.
 *
 * @param config the bounds and thresholds.
 * @param state  carried between calls (initialise to {} on first use).
 * @param fps    the measured frames per second.
 * @param nowMs  monotonic milliseconds.
 * @return the effective render distance after this sample.
 */
int StepRenderDistance(const RenderDistanceConfig &config, RenderDistanceState &state,
                       int fps, long long nowMs);

} // namespace pl::runtime
