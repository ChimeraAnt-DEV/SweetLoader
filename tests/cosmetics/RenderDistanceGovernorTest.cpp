// Host test for the dynamic-render-distance hysteresis rule. Pure C++, no device.
//
// The governor decides when the render distance steps down under frame pressure and when it may
// step back up. The rule is where the regressions live: a wrong sustain window makes the distance
// oscillate, a missing dwell lets a drop and a restore land on consecutive windows, and a step size
// that is not applied makes a coarse setting behave like the fine one. Exercised here so a defect
// is caught without a device, where it would only show as a distance that flickers.

#include <cstdio>

#include "pl/runtime/optifine/OptifineGovernor.h"

using pl::runtime::RenderDistanceConfig;
using pl::runtime::RenderDistanceState;
using pl::runtime::StepRenderDistance;

static int checks = 0;
static int failures = 0;

static void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

int main() {
  // The first sample initialises at the maximum and never changes the distance.
  {
    RenderDistanceConfig config;
    RenderDistanceState state;
    expect(StepRenderDistance(config, state, 60, 0) == config.maxDistance,
           "the governor starts at the maximum distance");
    expect(state.changeCount == 0, "initialisation is not counted as a change");
  }

  // A dip shorter than the sustain window does not step.
  {
    RenderDistanceConfig config;
    RenderDistanceState state;
    StepRenderDistance(config, state, 60, 0);
    StepRenderDistance(config, state, 30, 100);
    const int distance = StepRenderDistance(config, state, 30, 100 + config.dropSustainMs - 1);
    expect(distance == config.maxDistance, "a dip shorter than the sustain window does not step");
    expect(state.changeCount == 0, "a sub-sustain dip is not a change");
  }

  // A sustained dip steps down exactly once per sustain window.
  {
    RenderDistanceConfig config;
    config.minDwellMs = 0;  // isolate the sustain rule from the dwell
    RenderDistanceState state;
    StepRenderDistance(config, state, 60, 0);
    StepRenderDistance(config, state, 30, 100);
    const int first = StepRenderDistance(config, state, 30, 100 + config.dropSustainMs);
    expect(first == config.maxDistance - 1, "a sustained dip steps down one chunk");
    expect(state.changeCount == 1, "the first step is counted");
    const int second = StepRenderDistance(config, state, 30, 100 + config.dropSustainMs * 2);
    expect(second == config.maxDistance - 2, "a continuing dip steps down again");
    expect(state.changeCount == 2, "the second step is counted");
  }

  // The step size is applied, and the distance never goes below the minimum.
  {
    RenderDistanceConfig config;
    config.minDwellMs = 0;
    config.stepSize = 3;
    config.minDistance = 4;
    config.maxDistance = 12;
    RenderDistanceState state;
    StepRenderDistance(config, state, 60, 0);
    StepRenderDistance(config, state, 30, 100);  // arm the dip
    int distance = StepRenderDistance(config, state, 30, 100 + config.dropSustainMs);
    expect(distance == 9, "a step size of 3 moves three chunks at once");
    distance = StepRenderDistance(config, state, 30, 100 + config.dropSustainMs * 2);
    expect(distance == 6, "a second step moves three more chunks");
    distance = StepRenderDistance(config, state, 30, 100 + config.dropSustainMs * 3);
    expect(distance == config.minDistance,
           "a step larger than the remaining range clamps to the minimum");
    distance = StepRenderDistance(config, state, 30, 100 + config.dropSustainMs * 4);
    expect(distance == config.minDistance, "the distance never steps below the minimum");
  }

  // The dwell blocks a change inside the dwell window even when the sustain timer is due.
  {
    RenderDistanceConfig config;
    config.minDistance = 4;
    config.maxDistance = 12;
    config.dropSustainMs = 1000;
    config.restoreSustainMs = 1000;
    config.minDwellMs = 3000;
    RenderDistanceState state;
    StepRenderDistance(config, state, 60, 0);
    // A dip from t=0 is due at t=1000, but the dwell (measured from initialisation) holds it until
    // t=3000.
    expect(StepRenderDistance(config, state, 30, 1000) == config.maxDistance,
           "the first change is held until the dwell has elapsed");
    expect(StepRenderDistance(config, state, 30, 3000) == config.maxDistance - 1,
           "the first change is allowed once the dwell has elapsed");
    // A recovery from t=3100 is due at t=4100, inside the dwell window (last change at t=3000).
    StepRenderDistance(config, state, 60, 3100);
    expect(StepRenderDistance(config, state, 60, 4100) == config.maxDistance - 1,
           "a change inside the dwell window is held back");
    expect(StepRenderDistance(config, state, 60, 6000) == config.maxDistance,
           "the change is allowed once the dwell has elapsed");
  }

  // A zero dwell preserves the original per-sustain-window behaviour.
  {
    RenderDistanceConfig config;
    config.minDwellMs = 0;
    RenderDistanceState state;
    StepRenderDistance(config, state, 60, 0);
    StepRenderDistance(config, state, 30, 100);
    const int first = StepRenderDistance(config, state, 30, 100 + config.dropSustainMs);
    const int second = StepRenderDistance(config, state, 30, 100 + config.dropSustainMs * 2);
    expect(first == config.maxDistance - 1 && second == config.maxDistance - 2,
           "a zero dwell changes on every due sustain window");
  }

  // A single good sample between two bad ones does not both arm a drop and a restore.
  {
    RenderDistanceConfig config;
    config.minDwellMs = 0;
    RenderDistanceState state;
    StepRenderDistance(config, state, 60, 0);
    StepRenderDistance(config, state, 30, 100);
    StepRenderDistance(config, state, 60, 100 + config.dropSustainMs - 1);  // one good sample
    const int distance = StepRenderDistance(config, state, 30, 100 + config.dropSustainMs);
    expect(distance == config.maxDistance,
           "a single good sample resets the dip timer, so the drop does not fire");
  }

  // The recovery needs its own, longer sustain window.
  {
    RenderDistanceConfig config;
    config.minDwellMs = 0;
    RenderDistanceState state;
    StepRenderDistance(config, state, 60, 0);
    StepRenderDistance(config, state, 30, 100);
    StepRenderDistance(config, state, 30, 100 + config.dropSustainMs);
    const int stillLow = StepRenderDistance(config, state, 60, 100 + config.dropSustainMs + 1);
    expect(stillLow == config.maxDistance - 1,
           "a recovery shorter than the restore window does not step up");
    const int restored = StepRenderDistance(
        config, state, 60, 100 + config.dropSustainMs + 1 + config.restoreSustainMs);
    expect(restored == config.maxDistance, "a sustained recovery steps back up");
  }

  std::printf("RenderDistanceGovernorTest: %d checks passed\n", checks);
  return failures == 0 ? 0 : 1;
}
