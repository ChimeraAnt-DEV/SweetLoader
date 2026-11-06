/**
 * @file SnapshotChannelTsanTest.cpp
 * @brief ThreadSanitizer test for the snapshot channel's memory model.
 *
 * The seqlock contract is not just "the reader never returns a torn payload" — it is also that the
 * reader and the writer never touch the same bytes without an ordering edge between them. A plain
 * `memcpy` into the buffer (the writer) racing a plain `memcpy` out of it (the reader) is a data race
 * even when the sequence check discards the result, and on ARM64 it can let a reader observe a
 * half-written payload with an even sequence.
 *
 * The payload here is deliberately byte-uniform per publish (all `i`, then all `i+1`), so a torn read
 * is also detectable without the sanitizer. Run under `-fsanitize=thread` this reports the race on
 * the buffer; run normally it reports a torn payload. Both must be clean.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <thread>
#include <vector>

#include "pl/ipc/SnapshotChannel.hpp"

using pl::ipc::SnapshotChannel;

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
  constexpr std::size_t kCapacity = 256;

  SnapshotChannel channel(kCapacity);
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0};
  std::atomic<int> reads{0};

  // The writer publishes a uniform-byte payload and never stops until the reader is done, so the
  // two threads overlap for the whole run and every buffer byte is written while the reader may be
  // copying it. That is the exact shape a device hits every frame.
  std::thread writer([&]() {
    std::vector<std::uint8_t> payload(kCapacity);
    std::uint8_t value = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      payload.assign(kCapacity, value);
      channel.publish(std::span<const std::uint8_t>(payload));
      ++value;
    }
  });

  std::thread reader([&]() {
    std::vector<std::uint8_t> out;
    while (!stop.load(std::memory_order_relaxed)) {
      if (!channel.read(out)) continue;
      reads.fetch_add(1, std::memory_order_relaxed);
      // A consistent snapshot of a uniform payload has every byte equal. A mix means the copy was
      // taken across a publish.
      for (std::size_t i = 1; i < out.size(); ++i) {
        if (out[i] != out[0]) {
          torn.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
    }
  });

  // Run long enough for the sanitizer to observe an overlap; the wall time is bounded.
  for (int i = 0; i < 200 && torn.load(std::memory_order_relaxed) == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  reader.join();

  expect(reads.load() > 0, "the reader observed at least one snapshot");
  expect(torn.load() == 0, "no torn payload under concurrent publish and read");

  std::printf("SnapshotChannelTsanTest: %d checks passed (%d reads)\n", checks,
              reads.load());
  return 0;
}
