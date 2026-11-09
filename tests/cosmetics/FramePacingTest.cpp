/**
 * @file FramePacingTest.cpp
 * @brief Host test for the render-thread frame-pacing analyzer.
 *
 * The analyzer is pure, so its classification rules (a lock, a hitch, no reading before two frames,
 * and no lock claim when the refresh rate is unknown) are pinned without a device.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "pl/render/FramePacing.hpp"

using pl::render::FramePacingAnalyzer;
using pl::render::FramePacingReading;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

}  // namespace

int main() {
  // No reading before two frames have been seen.
  {
    FramePacingAnalyzer a;
    FramePacingReading r = a.onFrame(1000, 60);
    expect(r.sampleCount == 0, "the first frame produces no interval");
    expect(r.averageFrameUs == 0, "no average before an interval exists");
  }

  // A steady 60Hz stream reads as vsync-locked.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 40; ++i) {
      t += 16667; // ~60Hz
      a.onFrame(t, 60);
    }
    FramePacingReading r = a.reading(60);
    expect(r.sampleCount == 39, "every interval after the first is counted");
    expect(r.vsyncLocked, "a steady 60Hz stream is vsync-locked");
    expect(!r.jittering, "a steady stream does not jitter");
    expect(r.averageFrameUs > 16000 && r.averageFrameUs < 17500, "the average is ~16.7ms");
  }

  // One long frame in the window is reported as jitter.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 20; ++i) {
      t += 16667;
      a.onFrame(t, 60);
    }
    t += 60000; // a 60ms hitch
    FramePacingReading r = a.onFrame(t, 60);
    expect(r.jittering, "a 60ms frame against a 16.7ms interval is jitter");
    expect(r.worstFrameUs == 60000, "the worst interval is retained");
  }

  // A hitch ages out of the window.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    a.onFrame(t, 60);
    t += 60000; // the hitch
    a.onFrame(t, 60);
    for (int i = 0; i < FramePacingAnalyzer::kWindow + 2; ++i) {
      t += 16667;
      a.onFrame(t, 60);
    }
    FramePacingReading r = a.reading(60);
    expect(!r.jittering, "a hitch ages out of the window");
    expect(r.worstFrameUs < 30000, "the worst interval is now a normal one");
  }

  // An unknown refresh rate never claims a lock.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 20; ++i) {
      t += 16667;
      a.onFrame(t, 0);
    }
    FramePacingReading r = a.reading(0);
    expect(!r.vsyncLocked, "an unknown refresh rate cannot be locked");
    expect(!r.jittering, "a steady stream with no refresh rate does not jitter");
    expect(r.averageFrameUs > 0, "the average is still measured");
  }

  // An unknown refresh rate still flags a hitch, relative to the mean.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 20; ++i) {
      t += 16667;
      a.onFrame(t, 0);
    }
    t += 80000; // a long stall against a ~16.7ms mean
    FramePacingReading r = a.onFrame(t, 0);
    expect(r.jittering, "a hitch is flagged with no refresh rate, relative to the mean");
    expect(!r.vsyncLocked, "a lock is still not claimed with no refresh rate");
  }

  // A 30fps stream against a 60Hz display is not locked (the average is ~2x the interval).
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 20; ++i) {
      t += 33333;
      a.onFrame(t, 60);
    }
    FramePacingReading r = a.reading(60);
    expect(!r.vsyncLocked, "30fps against 60Hz is not locked");
    expect(r.jittering, "30fps against 60Hz is late on every frame");
  }

  // Reset clears the window.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 10; ++i) {
      t += 16667;
      a.onFrame(t, 60);
    }
    a.reset();
    FramePacingReading r = a.reading(60);
    expect(r.sampleCount == 0, "reset clears the sample count");
    expect(r.averageFrameUs == 0, "reset clears the average");
  }

  // A sub-millisecond floor collapses several model draws in one frame to one sample.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    a.onFrame(t, 60, 3000);
    t += 500; // a second model draw inside the same frame: ignored
    FramePacingReading r = a.onFrame(t, 60, 3000);
    expect(r.sampleCount == 0, "an interval below the floor is not recorded");
    t += 16667;
    r = a.onFrame(t, 60, 3000);
    expect(r.sampleCount == 1, "the next real frame is recorded");
  }

  // Percentiles and hitches per minute over the long window.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    // 100 steady 16.667ms frames, then 5 hitches of 50ms. The median stays ~16667us, so a 50ms
    // frame is above twice the median and counts as a hitch.
    for (int i = 0; i < 100; ++i) {
      t += 16667;
      a.onFrame(t, 60);
    }
    for (int i = 0; i < 5; ++i) {
      t += 50000;
      a.onFrame(t, 60);
    }
    FramePacingReading r = a.percentiles();
    expect(r.p50Us == 16667, "p50 is the steady interval");
    expect(r.p95Us >= r.p50Us, "p95 is at least the median");
    expect(r.p99Us >= r.p95Us, "p99 is at least p95");
    expect(r.p99Us == 50000, "the tail is the hitch interval");
    expect(r.hitchesPerMinute > 0, "the hitch frames produce a non-zero rate");
    // Span is 105 frames over ~1.75s; 5 hitches in 1.75s is about 171/min. Bound it loosely so the
    // test pins the rule, not the exact arithmetic.
    expect(r.hitchesPerMinute > 100 && r.hitchesPerMinute < 250,
           "the hitch rate is normalised by the window's wall span");
  }

  // No samples: the percentile reading is empty, not a fabricated zero.
  {
    FramePacingAnalyzer a;
    FramePacingReading r = a.percentiles();
    expect(r.sampleCount == 0, "no samples means no percentile reading");
    expect(r.p50Us == 0, "an empty window has no median");
  }

  // A uniform window has no hitches.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 50; ++i) {
      t += 16667;
      a.onFrame(t, 60);
    }
    FramePacingReading r = a.percentiles();
    expect(r.hitchesPerMinute == 0, "a perfectly steady window has no hitches");
    expect(r.p50Us == 16667 && r.p99Us == 16667, "every percentile is the steady interval");
  }

  // The span is the window's own span, not the whole session. A long steady run followed by a burst
  // must report the burst's real rate. The window holds 2000 samples: with 1900 steady 1ms frames and
  // 100 burst 10ms frames the span is 1900*1ms + 100*10ms = 2899ms, so 100 hitches is ~2069/min.
  // Before the fix the span grew from the first sample ever taken (3000ms + 1000ms = 4000ms), which
  // diluted the same burst to 1500/min — the exact defect the doc comment promised not to have. The
  // bound sits between the two, so it fails against the old code and passes against the new.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 3000; ++i) {
      t += 1000; // 1ms "frames": a long, irrelevant steady run
      a.onFrame(t, 0);
    }
    for (int i = 0; i < 100; ++i) {
      t += 10000; // 10ms "frames": the burst, above twice the 1ms median
      a.onFrame(t, 0);
    }
    FramePacingReading r = a.percentiles();
    expect(r.hitchesPerMinute > 1800,
           "the window span is the ring's span, so a burst is not diluted by the session");
  }

  // Two concurrent percentile readers must not corrupt each other. The old code sorted into one
  // shared scratch buffer, so a reader could see a torn median; the bound here fails against that.
  {
    FramePacingAnalyzer a;
    long long t = 0;
    for (int i = 0; i < 3000; ++i) {
      t += 1000;
      a.onFrame(t, 0);
    }
    std::atomic<int> bad{0};
    auto work = [&]() {
      for (int k = 0; k < 5000; ++k) {
        if (a.percentiles().p50Us != 1000) bad.fetch_add(1, std::memory_order_relaxed);
      }
    };
    std::thread t1(work);
    std::thread t2(work);
    t1.join();
    t2.join();
    expect(bad.load() == 0, "concurrent percentile readers agree on the median");
  }

  std::printf("FramePacingTest: %d checks passed\n", checks);
  return 0;
}
