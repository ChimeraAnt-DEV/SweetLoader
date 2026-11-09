/**
 * @file PublishedBlobTest.cpp
 * @brief Host test for the atomic-published blob's semantics (no threads).
 *
 * The publish/snapshot contract the cosmetic frame and geometry rely on: empty before the first
 * publish, a full copy after one, an empty publish clears the previous buffer, and the reader can
 * pin a buffer that stays valid while the writer publishes the next one.
 */

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "pl/runtime/PublishedBlob.hpp"

using pl::runtime::PublishedBlob;

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
  PublishedBlob blob;
  std::vector<std::uint8_t> out;

  // Nothing published yet: no snapshot, size 0, and `out` is cleared rather than left stale.
  out.assign(8, 0xEE);
  expect(!blob.snapshot(out), "an un-published blob has no snapshot");
  expect(out.empty(), "a failed snapshot clears the output");
  expect(blob.size() == 0, "an un-published blob has size 0");
  expect(blob.empty(), "an un-published blob is empty");

  // Publish a buffer: the reader gets a full copy.
  const std::vector<std::uint8_t> frame = {1, 2, 3, 4, 5};
  blob.publish(frame.data(), frame.size());
  expect(blob.snapshot(out), "a published blob snapshots");
  expect(out == frame, "the snapshot equals the published bytes");
  expect(blob.size() == frame.size(), "the size matches");
  expect(!blob.empty(), "a published blob is not empty");

  // Publishing an empty frame clears the previous one (how the writer stops the reader drawing).
  blob.publish(nullptr, 0);
  expect(!blob.snapshot(out), "an empty publish clears the snapshot");
  expect(out.empty(), "the output is cleared after an empty publish");
  expect(blob.empty(), "the blob is empty after an empty publish");

  // A pinned buffer stays valid and unchanged while the writer publishes the next one.
  std::vector<std::uint8_t> first = {9, 9, 9};
  blob.publish(first.data(), first.size());
  auto pinned = blob.acquire();
  const std::vector<std::uint8_t> second = {7, 7};
  blob.publish(second.data(), second.size());
  expect(pinned != nullptr && pinned->size() == 3, "the pinned buffer keeps its size");
  expect((*pinned)[0] == 9 && (*pinned)[2] == 9, "the pinned buffer keeps its bytes");
  expect(blob.snapshot(out) && out == second, "a new snapshot sees the latest publish");

  std::printf("PublishedBlobTest: %d checks passed\n", checks);
  return 0;
}
