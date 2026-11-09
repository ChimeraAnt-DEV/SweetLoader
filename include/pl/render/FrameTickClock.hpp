#pragma once

/**
 * @file FrameTickClock.hpp
 * @brief Infers frame boundaries from a per-call timestamp, for a render entry that runs more than
 *        once per frame.
 *
 * <p>The player-render entry is invoked once per rendered player model, so a frame boundary has to be
 * inferred. The previous rule treated a gap of more than one second as a new frame, which is wrong:
 * during play the entry runs every ~16ms, so the "new frame" test was true only for the very first
 * frame after a second-long stall. The tick was therefore frozen during play, and every consumer that
 * keys on it (cape/pet physics, the cosmetics frame version) was effectively dead.
 *
 * <p>The rule here is a small floor: the first call, and any call at least @p minIntervalUs after the
 * previous one, begins a new frame. Several model draws inside one frame collapse to a single tick;
 * every real frame advances it. A backwards timestamp (a reset) also begins a new frame rather than
 * merging frames forever.
 *
 * <p>Pure and allocation-free, so the rule is host-tested — see
 * `tests/cosmetics/FrameTickClockTest.cpp`. When the engine's swap/present call is hooked, this class
 * is the seam that will be driven by the true frame boundary instead of an inferred one.
 */

#include <cstdint>

namespace pl::render {

/** @brief Counts frames from a stream of render-entry timestamps. */
class FrameTickClock {
public:
  /**
   * The default floor. The player-render entry runs once per rendered player model, and models inside
   * one frame are drawn within a few hundred microseconds of each other, so 3ms collapses them to one
   * sample while still separating frames at any refresh rate up to ~300Hz.
   */
  static constexpr long long kDefaultMinIntervalUs = 3000;

  FrameTickClock() = default;

  /**
   * @brief Records one render-entry call.
   *
   * @param nowUs a monotonic microsecond timestamp.
   * @return true when this call begins a new frame.
   */
  bool onRenderCall(long long nowUs, long long minIntervalUs = kDefaultMinIntervalUs) {
    const bool newFrame =
        !started_ || nowUs < lastUs_ || (nowUs - lastUs_) >= minIntervalUs;
    lastUs_ = nowUs;
    started_ = true;
    if (newFrame) ++tick_;
    return newFrame;
  }

  /** @brief The number of frames seen so far. */
  [[nodiscard]] std::uint32_t tick() const { return tick_; }

  /** @brief The timestamp of the most recent call. */
  [[nodiscard]] long long lastUs() const { return lastUs_; }

  /** @brief True once at least one call has been recorded. */
  [[nodiscard]] bool started() const { return started_; }

  /** @brief Returns to the fresh state, e.g. when a session starts. */
  void reset() {
    tick_ = 0;
    lastUs_ = 0;
    started_ = false;
  }

private:
  std::uint32_t tick_ = 0;
  long long lastUs_ = 0;
  bool started_ = false;
};

} // namespace pl::render
