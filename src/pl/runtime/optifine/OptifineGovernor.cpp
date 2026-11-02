#include "pl/runtime/optifine/OptifineGovernor.h"

#include <algorithm>

namespace pl::runtime {

int StepRenderDistance(const RenderDistanceConfig &config, RenderDistanceState &state,
                       int fps, long long nowMs) {
  const int minDistance = std::max(1, config.minDistance);
  const int maxDistance = std::max(minDistance, config.maxDistance);
  const int stepSize = std::max(1, config.stepSize);

  if (!state.initialized) {
    state.currentDistance = maxDistance;
    state.initialized = true;
    state.belowSinceMs = 0;
    state.aboveSinceMs = 0;
    state.lastChangeMs = nowMs;
  }
  state.currentDistance = std::clamp(state.currentDistance, minDistance, maxDistance);

  // The dwell gates every change, including the first, so a governor configured with a long dwell
  // cannot change twice in quick succession when it starts mid-dip.
  const bool dwellElapsed = config.minDwellMs <= 0 || (nowMs - state.lastChangeMs) >= config.minDwellMs;

  const bool below = fps < config.fpsThreshold;
  if (below) {
    // A dip starts (or continues) the down-timer and clears the up-timer, so a single bad
    // sample between two good ones cannot both arm a drop and a restore.
    state.aboveSinceMs = 0;
    if (state.belowSinceMs == 0) state.belowSinceMs = nowMs;
    if (dwellElapsed && nowMs - state.belowSinceMs >= config.dropSustainMs &&
        state.currentDistance > minDistance) {
      const int next = std::max(minDistance, state.currentDistance - stepSize);
      if (next != state.currentDistance) {
        state.currentDistance = next;
        state.lastChangeMs = nowMs;
        ++state.changeCount;
      }
      state.belowSinceMs = nowMs; // re-arm: one step per sustain window, not one per sample.
    }
  } else {
    state.belowSinceMs = 0;
    if (state.aboveSinceMs == 0) state.aboveSinceMs = nowMs;
    if (dwellElapsed && nowMs - state.aboveSinceMs >= config.restoreSustainMs &&
        state.currentDistance < maxDistance) {
      const int next = std::min(maxDistance, state.currentDistance + stepSize);
      if (next != state.currentDistance) {
        state.currentDistance = next;
        state.lastChangeMs = nowMs;
        ++state.changeCount;
      }
      state.aboveSinceMs = nowMs;
    }
  }

  return state.currentDistance;
}

} // namespace pl::runtime
