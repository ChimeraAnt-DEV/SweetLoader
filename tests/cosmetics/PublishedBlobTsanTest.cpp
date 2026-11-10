/**
 * @file PublishedBlobTsanTest.cpp
 * @brief ThreadSanitizer test for the atomic-published blob used by the cosmetic frame and geometry.
 *
 * The cosmetic frame buffer was a `std::vector` swapped in place under a mutex while the render
 * thread read it lock-free - a data race (UB; on ARM64 a torn pointer pair can abort). This test
 * hammers one publisher against several readers under TSAN: a race in the store would be reported
 * here, and the reader must always observe a whole buffer, never a half-updated one.
 */

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "pl/runtime/PublishedBlob.hpp"

using pl::runtime::PublishedBlob;

namespace {
std::atomic<bool> g_stop{false};
std::atomic<int> g_badReads{0};
} // namespace

int main() {
  PublishedBlob blob;

  // Seed with a known pattern.
  std::vector<std::uint8_t> seed(64, 0xAB);
  blob.publish(seed.data(), seed.size());

  std::thread writer([&] {
    std::vector<std::uint8_t> next(64, 0);
    std::uint8_t value = 1;
    while (!g_stop.load(std::memory_order_relaxed)) {
      // Every published buffer is uniform, so a reader that sees a torn buffer observes a mix of
      // two values - which the reader below treats as a failure.
      for (std::size_t i = 0; i < next.size(); ++i) next[i] = value;
      blob.publish(next.data(), next.size());
      ++value;
    }
  });

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      std::vector<std::uint8_t> out;
      while (!g_stop.load(std::memory_order_relaxed)) {
        if (!blob.snapshot(out)) continue;
        if (out.empty()) continue;
        const std::uint8_t first = out[0];
        for (std::uint8_t b : out) {
          if (b != first) {
            g_badReads.fetch_add(1, std::memory_order_relaxed);
            break;
          }
        }
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  g_stop.store(true, std::memory_order_relaxed);
  writer.join();
  for (auto &t : readers) t.join();

  const int bad = g_badReads.load(std::memory_order_relaxed);
  if (bad != 0) {
    std::fprintf(stderr, "FAIL: %d torn reads observed\n", bad);
    return 1;
  }
  std::printf("PublishedBlobTsanTest: no torn reads\n");
  return 0;
}
