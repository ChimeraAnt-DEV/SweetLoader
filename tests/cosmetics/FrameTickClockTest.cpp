/**
 * @file FrameTickClockTest.cpp
 * @brief Host test for the frame-boundary clock.
 *
 * The render entry is called once per rendered player model, so a frame boundary has to be inferred
 * from timestamps. The old rule ("new frame if more than one second since the last call") advanced the
 * tick only after a one-second stall, so the tick was effectively frozen during play and every
 * consumer that keys on it — cape/pet physics, the cosmetics frame version — was dead. The new rule is
 * a small floor (a few ms), which collapses several model draws inside one frame into one tick while
 * still advancing every real frame.
 */

#include <cstdio>
#include <cstdlib>

#include "pl/render/FrameTickClock.hpp"

using pl::render::FrameTickClock;

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
  // The first call always begins a frame.
  {
    FrameTickClock clock;
    expect(clock.onRenderCall(1'000'000), "the first call is a new frame");
    expect(clock.tick() == 1, "the tick advanced to 1");
  }

  // Several model draws inside one 16.7ms frame collapse to a single tick.
  {
    FrameTickClock clock;
    expect(clock.onRenderCall(0), "frame 1 starts");
    expect(!clock.onRenderCall(500), "a call 0.5ms later is the same frame");
    expect(!clock.onRenderCall(1500), "a call 1.5ms later is the same frame");
    expect(clock.onRenderCall(20000), "a call 20ms later is the next frame");
    expect(clock.tick() == 2, "two frames were counted, not four calls");
  }

  // The bug this replaces: a steady 60Hz stream (16.7ms apart) must advance the tick every frame.
  // The old >1s rule left the tick at 1 for the whole run.
  {
    FrameTickClock clock;
    long long t = 0;
    for (int i = 0; i < 120; ++i) {
      clock.onRenderCall(t);
      t += 16667;
    }
    expect(clock.tick() >= 119,
           "a steady 60Hz stream advances the tick once per frame (the old rule did not)");
  }

  // A burst faster than the floor is one frame; a gap at least the floor is a new frame.
  {
    FrameTickClock under;
    expect(under.onRenderCall(0), "start");
    expect(!under.onRenderCall(FrameTickClock::kDefaultMinIntervalUs - 1),
           "just under the floor is the same frame");
  }
  {
    FrameTickClock at;
    expect(at.onRenderCall(0), "start");
    expect(at.onRenderCall(FrameTickClock::kDefaultMinIntervalUs),
           "exactly the floor begins a new frame");
  }

  // A clock that jumps backwards (a reset) starts a new frame rather than merging forever.
  {
    FrameTickClock clock;
    clock.onRenderCall(1'000'000);
    expect(clock.onRenderCall(500), "a backwards jump begins a new frame");
    expect(clock.tick() == 2, "the tick advanced across the reset");
  }

  // reset() returns to the fresh state.
  {
    FrameTickClock clock;
    clock.onRenderCall(0);
    clock.onRenderCall(100000);
    clock.reset();
    expect(clock.tick() == 0, "reset clears the tick");
    expect(clock.onRenderCall(100000), "the first call after reset is a new frame");
    expect(clock.tick() == 1, "the tick restarts at 1");
  }

  std::printf("FrameTickClockTest: %d checks passed\n", checks);
  return 0;
}
