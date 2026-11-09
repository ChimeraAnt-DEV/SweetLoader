/**
 * @file CameraBlockScanTest.cpp
 * @brief Host test for the pure view/projection finder used by the camera scan.
 *
 * The scan walks the game's writable data looking for a perspective projection matrix whose
 * preceding 16 floats are a rigid view transform. That walk is the expensive part and cannot run in
 * a host test, but the *rule* that decides what counts as a camera block can, and it is the part
 * that matters: a loose rule accepts random data and a mod then draws with a garbage matrix. This
 * exercises the rule against a synthetic buffer with a real pair buried in noise.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "pl/render/CameraBlockScan.hpp"

using pl::render::FindViewProjectionPair;

namespace {
int checks = 0;
void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

// A plausible view matrix (identity rotation, unit basis) and a plausible perspective projection.
void writeView(float *m) {
  std::memset(m, 0, sizeof(float) * 16);
  m[0] = 1.0F; m[5] = 1.0F; m[10] = 1.0F; m[15] = 1.0F;
}
void writeProjection(float *m) {
  std::memset(m, 0, sizeof(float) * 16);
  m[0] = 1.5F; m[5] = 2.0F; m[10] = -1.02F; m[11] = -1.0F;
}
} // namespace

int main() {
  const std::size_t kNoPair = std::numeric_limits<std::size_t>::max();

  // Too small to hold a pair.
  float tiny[16] = {0};
  expect(FindViewProjectionPair(tiny, 16) == kNoPair, "a buffer smaller than a pair has no pair");

  // A valid pair buried at offset 40 floats inside 200 floats of noise.
  float buffer[200];
  for (std::size_t i = 0; i < 200; ++i) buffer[i] = static_cast<float>((i * 7) % 13) * 0.001F;
  const std::size_t pairAt = 40;
  writeView(buffer + pairAt);
  writeProjection(buffer + pairAt + 16);
  const std::size_t found = FindViewProjectionPair(buffer, 200);
  expect(found == pairAt, "the finder locates the buried pair");

  // A projection with a wrong m[11] is not a camera.
  writeProjection(buffer + pairAt + 16);
  buffer[pairAt + 16 + 11] = 0.0F;
  expect(FindViewProjectionPair(buffer, 200) == kNoPair, "a projection without -1 in m[11] is rejected");
  writeProjection(buffer + pairAt + 16);

  // A view whose basis is not unit length is rejected.
  buffer[pairAt + 0] = 9.0F;
  expect(FindViewProjectionPair(buffer, 200) == kNoPair, "a non-rigid view is rejected");
  buffer[pairAt + 0] = 1.0F;

  // A non-finite entry is rejected.
  buffer[pairAt + 16 + 0] = std::numeric_limits<float>::quiet_NaN();
  expect(FindViewProjectionPair(buffer, 200) == kNoPair, "a non-finite projection is rejected");
  writeProjection(buffer + pairAt + 16);

  // The pair is still found after the noise was disturbed.
  expect(FindViewProjectionPair(buffer, 200) == pairAt, "the pair is found again after rejection paths");

  std::printf("CameraBlockScanTest: %d checks passed\n", checks);
  return 0;
}
