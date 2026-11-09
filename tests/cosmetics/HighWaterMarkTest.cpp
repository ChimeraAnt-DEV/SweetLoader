/**
 * @file HighWaterMarkTest.cpp
 * @brief Host test for the atomic high-water-mark raiser.
 *
 * The render-callback timing aggregate must keep the worst value ever seen, not the last frame's, so
 * a one-off stall is not overwritten before a diagnostics reader polls it. The rule is pure, so the
 * never-lower behaviour and its concurrency are pinned here.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "pl/runtime/HighWaterMark.hpp"

using pl::runtime::RaiseHighWaterMark;

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
  {
    std::atomic<std::uint32_t> mark{0};
    RaiseHighWaterMark(mark, 10);
    expect(mark.load() == 10, "a larger value raises the mark");
    RaiseHighWaterMark(mark, 4);
    expect(mark.load() == 10, "a smaller value does not lower the mark");
    RaiseHighWaterMark(mark, 10);
    expect(mark.load() == 10, "an equal value is a no-op");
    RaiseHighWaterMark(mark, 99);
    expect(mark.load() == 99, "a larger value raises the mark again");
  }

  // Concurrent raisers converge to the maximum and never lose it.
  {
    std::atomic<std::uint32_t> mark{0};
    std::vector<std::thread> threads;
    for (std::uint32_t t = 1; t <= 8; ++t) {
      threads.emplace_back([&mark, t]() {
        for (std::uint32_t i = 1; i <= 20000; ++i) {
          RaiseHighWaterMark(mark, i * t);
        }
      });
    }
    for (auto &th : threads) th.join();
    expect(mark.load() == 20000u * 8u, "the mark converges to the maximum raised value");
  }

  std::printf("HighWaterMarkTest: %d checks passed\n", checks);
  return 0;
}
