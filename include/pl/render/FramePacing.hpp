#pragma once

/**
 * @file FramePacing.hpp
 * @brief Frame-time analysis for the render thread.
 *
 * <p>The render thread's priority is boosted (see `OptifineRenderPriority.cpp`), but priority alone
 * does not tell the client whether frames are actually arriving evenly. The post-render callback
 * fires once per rendered frame with a real monotonic timestamp, so the client can measure the
 * frame interval and classify it.
 *
 * <p><b>Honest scope.</b> This measures and reports; it does not reschedule the engine's frame
 * submission. Actively pacing against the display's vsync needs the engine's own swap/present call,
 * which the stripped binary does not expose (the same limit every native seam here documents). The
 * value delivered is a real reading the diagnostics can show and the launcher can act on, rather
 * than a claim that pacing changed.
 *
 * <p>Pure and allocation-free so it is host-tested and can run on the render thread without cost.
 *
 * <p><b>Threading.</b> {@link #onFrame} and {@link #reading} are render-thread-only, so their fields
 * are plain. {@link #percentiles} is called from a diagnostics reader on another thread, so every
 * field it shares with the render thread is a relaxed atomic (the value is approximate for the frame
 * in flight, which is all a summary needs) and its scratch buffer is guarded by a mutex so two
 * concurrent readers cannot corrupt each other. The percentile result is memoized on a sample
 * generation, so a diagnostics page that polls repeatedly does not re-sort the 2000-sample window on
 * every call (audit item 2.3).
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace pl::render {

/** @brief One frame-time reading, classified against the display interval. */
struct FramePacingReading {
  /** The smoothed frame interval in microseconds, or 0 before two frames have been seen. */
  long long averageFrameUs = 0;
  /** The worst frame interval in the recent window, in microseconds. */
  long long worstFrameUs = 0;
  /** Frames measured so far. */
  std::size_t sampleCount = 0;
  /** A frame arrived more than one interval late (a hitch). */
  bool jittering = false;
  /** The interval matches the display period within tolerance (vsync-locked). */
  bool vsyncLocked = false;
  /**
   * The 50th/95th/99th percentile frame interval in microseconds over the long window.
   *
   * <p>Only filled by {@link FramePacingAnalyzer#percentiles}; 0 in the per-frame reading, because
   * sorting the window on every render call would cost more than the measurement is worth.
   */
  long long p50Us = 0;
  long long p95Us = 0;
  long long p99Us = 0;
  /**
   * Hitches per minute, where a hitch is a frame interval above twice the median.
   *
   * <p>Only filled by {@link FramePacingAnalyzer#percentiles}; 0 in the per-frame reading.
   */
  int hitchesPerMinute = 0;
};

/**
 * @brief Tracks frame intervals from the render tick and classifies them.
 *
 * <p>A fixed-size ring of the last {@link kWindow} intervals is kept, so a hitch ages out and the
 * reading reflects the recent steady state rather than the whole session. No allocation: the ring
 * is a member array.
 */
class FramePacingAnalyzer {
public:
  /** Intervals retained for the worst-case and jitter decision. */
  static constexpr std::size_t kWindow = 60;
  /**
   * A frame is "late" when it exceeds the expected interval by this factor. 1.5 tolerates the
   * normal variance around vsync without calling a single dropped frame a pacing problem.
   */
  static constexpr int kLateFactor = 3; // numerator
  static constexpr int kLateDivisor = 2; // denominator: 3/2 = 1.5x
  /** The lock tolerance: within this fraction of the display interval counts as locked. */
  static constexpr int kLockTolerancePercent = 12;

  /**
   * @brief Feeds one rendered frame.
   *
   * @param nowUs    a monotonic microsecond timestamp.
   * @param displayRefreshHz the display's refresh rate, or 0 when unknown (locking is then not
   *        claimed, because there is no interval to compare against).
   * @param minIntervalUs intervals shorter than this are ignored rather than recorded. The render
   *        entry is called once per rendered player model, so several calls can fall inside one
   *        frame; a floor (a few milliseconds) collapses them to one sample so the window measures
   *        real frames instead of model draws. 0 records every interval.
   * @return the updated reading.
   */
  FramePacingReading onFrame(long long nowUs, int displayRefreshHz, long long minIntervalUs = 0) {
    if (lastUs_ != 0 && nowUs > lastUs_) {
      const long long interval = nowUs - lastUs_;
      if (interval >= minIntervalUs) {
        ring_[next_] = interval;
        next_ = (next_ + 1) % kWindow;
        if (count_ < kWindow) ++count_;
        ++sampleCount_;

        // The long window is fed in lockstep so the percentiles cover the same samples. The
        // timestamp travels with each sample, so the span used for hitches-per-minute is the span of
        // the samples actually still in the ring — not the whole session.
        const std::size_t slot = percentileNext_;
        percentileRing_[slot] = interval;
        percentileStamp_[slot] = nowUs;
        percentileNext_ = (percentileNext_ + 1) % kPercentileWindow;
        if (percentileCount_ < kPercentileWindow) ++percentileCount_;
        // The oldest sample in the window: the one just overwritten when the ring is full, else the
        // first sample ever taken.
        const std::size_t oldest = (percentileCount_ == kPercentileWindow) ? percentileNext_ : 0;
        const long long oldestStamp = percentileStamp_[oldest];
        percentileSpanUs_.store(oldestStamp == 0 ? 0 : nowUs - oldestStamp,
                                std::memory_order_relaxed);
        // A new sample invalidates the memoized percentile reading.
        percentileGeneration_.fetch_add(1, std::memory_order_release);
      }
    }
    lastUs_ = nowUs;
    return reading(displayRefreshHz);
  }

  /** @return the current reading without feeding a frame. */
  FramePacingReading reading(int displayRefreshHz) const {
    FramePacingReading out;
    out.sampleCount = sampleCount_;
    if (count_ == 0) return out;

    long long sum = 0;
    long long worst = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      sum += ring_[i];
      if (ring_[i] > worst) worst = ring_[i];
    }
    out.averageFrameUs = sum / static_cast<long long>(count_);
    out.worstFrameUs = worst;

    if (displayRefreshHz > 0 && out.averageFrameUs > 0) {
      const long long expectedUs = 1000000LL / displayRefreshHz;
      // Late: a frame exceeded 1.5x the expected interval.
      out.jittering = worst * kLateDivisor > expectedUs * kLateFactor;
      // Locked: the average is within the tolerance band of the expected interval.
      const long long low = expectedUs * (100 - kLockTolerancePercent) / 100;
      const long long high = expectedUs * (100 + kLockTolerancePercent) / 100;
      out.vsyncLocked = out.averageFrameUs >= low && out.averageFrameUs <= high;
    } else {
      // No refresh rate: classify jitter relative to the average (a frame more than 1.5x the mean
      // is a hitch), which needs no absolute interval. A lock cannot be claimed without one.
      out.jittering = out.averageFrameUs > 0 &&
                      out.worstFrameUs * kLateDivisor > out.averageFrameUs * kLateFactor;
      out.vsyncLocked = false;
    }
    return out;
  }

  /**
   * @brief The percentile + hitch reading.
   *
   * <p>Separate from {@link #reading} because it sorts the long window, which is too much work to do
   * on every render call. A diagnostics reader calls this off the render thread; the ring is
   * written by the render thread and read here, so the copy is taken under no lock but the values
   * are only approximate for the one frame in flight, which is fine for a summary.
   *
   * <p>Percentiles are taken over the last {@link kPercentileWindow} intervals. A hitch is a frame
   * above twice the median, and hitches per minute is normalised by the wall time the window spans,
   * so a long session does not dilute a burst.
   */
  FramePacingReading percentiles() const {
    // Memoized on the sample generation: the sort below is O(n log n) over 2000 samples, and a
    // diagnostics page can poll far faster than frames arrive, so recomputing an unchanged window is
    // wasted work (audit item 2.3). The generation is bumped only when a new sample lands, so a
    // repeat poll returns the cached reading without touching the ring.
    const std::uint64_t generation = percentileGeneration_.load(std::memory_order_acquire);

    FramePacingReading out;
    out.sampleCount = sampleCount_;
    const std::size_t n = percentileCount_.load(std::memory_order_relaxed);
    if (n == 0) return out;

    // Copy and sort the live window under a lock, so two concurrent readers cannot corrupt each
    // other's scratch and a reader cannot race the render thread's writes mid-copy. The render
    // thread never takes this lock, so a diagnostics read cannot stall a frame. The cache fields are
    // only touched under this same lock.
    std::lock_guard<std::mutex> guard(scratchMutex_);
    if (cachedGeneration_ == generation) return cachedPercentiles_;
    for (std::size_t i = 0; i < n; ++i) {
      scratch_[i] = percentileRing_[i].load(std::memory_order_relaxed);
    }
    std::sort(scratch_, scratch_ + n);

    out.p50Us = scratch_[n * 50 / 100];
    out.p95Us = scratch_[(n * 95 / 100) < n ? (n * 95 / 100) : n - 1];
    out.p99Us = scratch_[(n * 99 / 100) < n ? (n * 99 / 100) : n - 1];

    // A hitch is above twice the median. Count them, then normalise by the window's wall span.
    const long long hitchThreshold = out.p50Us * 2;
    std::size_t hitches = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (scratch_[i] > hitchThreshold) ++hitches;
    }
    const long long span = percentileSpanUs_.load(std::memory_order_relaxed);
    if (span > 0) {
      // hitches per minute = hitches * 60_000_000us / spanUs, done in 64-bit.
      out.hitchesPerMinute = static_cast<int>((static_cast<long long>(hitches) * 60'000'000LL) /
                                              span);
    }
    cachedPercentiles_ = out;
    cachedGeneration_ = generation;
    return out;
  }

  /** Clears the window, e.g. when a session starts. */
  void reset() {
    next_ = 0;
    count_ = 0;
    lastUs_ = 0;
    sampleCount_ = 0;
    percentileNext_ = 0;
    percentileCount_.store(0, std::memory_order_relaxed);
    percentileSpanUs_.store(0, std::memory_order_relaxed);
    percentileGeneration_.fetch_add(1, std::memory_order_release);
    cachedGeneration_ = ~0ULL;
    for (auto &v : percentileRing_) v.store(0, std::memory_order_relaxed);
    for (auto &v : percentileStamp_) v = 0;
  }

private:
  long long ring_[kWindow] = {};
  std::size_t next_ = 0;
  std::size_t count_ = 0;
  long long lastUs_ = 0;
  std::size_t sampleCount_ = 0;

  /** The long window the percentiles are taken over. */
  static constexpr std::size_t kPercentileWindow = 2000;
  std::array<std::atomic<long long>, kPercentileWindow> percentileRing_{};
  /** The timestamp of each sample in the long window, so its true span is known. */
  long long percentileStamp_[kPercentileWindow] = {};
  /** Shared by percentiles() callers only; never touched by the render thread. */
  mutable long long scratch_[kPercentileWindow] = {};
  mutable std::mutex scratchMutex_;
  /** Bumped whenever a sample lands or the window resets; keys the memoized percentile reading. */
  std::atomic<std::uint64_t> percentileGeneration_{0};
  /** The memoized percentile reading and the generation it was computed for (scratchMutex_-guarded). */
  mutable FramePacingReading cachedPercentiles_{};
  mutable std::uint64_t cachedGeneration_ = ~0ULL;
  std::size_t percentileNext_ = 0;
  std::atomic<std::size_t> percentileCount_{0};
  std::atomic<long long> percentileSpanUs_{0};
};

} // namespace pl::render
