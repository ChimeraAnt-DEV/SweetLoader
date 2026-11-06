/**
 * @file PlayerSnapshotTsanTest.cpp
 * @brief ThreadSanitizer test for the local-player seqlock's memory model.
 *
 * The old shape stored the transform in plain `float` arrays read and written by two threads with no
 * synchronisation. That is a data race on every field; the sequence check hid the symptom but not the
 * undefined behaviour. This test drives a writer and a reader concurrently and asserts the sanitizer
 * sees no unsynchronised access, and that the payload is never torn.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "pl/ipc/PlayerSnapshot.hpp"

using pl::ipc::PlayerSnapshot;
using pl::ipc::PlayerTransform;

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
  PlayerSnapshot snapshot;
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0};
  std::atomic<int> reads{0};

  std::thread writer([&]() {
    long long i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const float v = static_cast<float>(i);
      PlayerTransform t;
      t.position[0] = t.position[1] = t.position[2] = v;
      t.rotation[0] = t.rotation[1] = v;
      t.stampMs = i;
      snapshot.publish(t);
      ++i;
    }
  });

  std::thread reader([&]() {
    PlayerTransform out;
    while (!stop.load(std::memory_order_relaxed)) {
      if (!snapshot.read(out)) continue;
      reads.fetch_add(1, std::memory_order_relaxed);
      const float v = out.position[0];
      if (out.position[1] != v || out.position[2] != v || out.rotation[0] != v ||
          out.rotation[1] != v) {
        torn.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  for (int i = 0; i < 200 && torn.load(std::memory_order_relaxed) == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  reader.join();

  expect(reads.load() > 0, "the reader observed at least one transform");
  expect(torn.load() == 0, "no torn transform under concurrent publish and read");

  std::printf("PlayerSnapshotTsanTest: %d checks passed (%d reads)\n", checks, reads.load());
  return 0;
}
