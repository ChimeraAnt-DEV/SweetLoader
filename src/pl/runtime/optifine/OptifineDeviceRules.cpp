#include "pl/runtime/optifine/OptifineDeviceRules.h"

#include <cstdint>

namespace pl::runtime {
namespace {

// A scratch big enough for kOptifineMaxCores entries, so the helpers can sort a copy without
// allocating (they are called from the apply pass, which must not allocate).
constexpr int kScratch = kOptifineMaxCores;

// Sorts @p values ascending into @p out (which must hold @p count entries) and returns the number of
// non-zero entries.
int SortedAscending(const std::uint64_t *values, int count, std::uint64_t *out) {
  int n = 0;
  for (int i = 0; i < count; ++i) {
    if (values[i] != 0) out[n++] = values[i];
  }
  for (int i = 1; i < n; ++i) {
    const std::uint64_t key = out[i];
    int j = i - 1;
    while (j >= 0 && out[j] > key) {
      out[j + 1] = out[j];
      --j;
    }
    out[j + 1] = key;
  }
  return n;
}

/**
 * @brief The frequency below which a core is a little core, or 0 when there is no gap to cluster on.
 *
 * The big cluster is everything at or above the largest relative gap in the ascending table. A
 * relative gap is used so the rule is the same whether the table is in kHz or Hz, and the *largest*
 * relative gap is what separates the performance cluster from the little cores on a tri-cluster SoC
 * (for a Snapdragon 8 Gen 1 the little-to-gold gap is ~39% while the gold-to-prime gap is only ~20%,
 * so the 39% gap is the cluster boundary and the whole prime+gold cluster is recovered).
 */
constexpr std::uint64_t kMinGapPercent = 8; // a >=8% rise separates two clusters

std::uint64_t BigClusterThreshold(const std::uint64_t *sortedAscending, int n) {
  if (n <= 1) return 0;
  // The best candidate so far, as a relative rise: rise / low.
  std::uint64_t bestRise = 0;
  std::uint64_t bestLow = 0;
  int bestIndex = -1; // the boundary sits between bestIndex and bestIndex+1
  for (int i = 0; i + 1 < n; ++i) {
    const std::uint64_t low = sortedAscending[i];
    const std::uint64_t high = sortedAscending[i + 1];
    if (low == 0) continue;
    const std::uint64_t rise = high - low;
    if (rise * 100ULL < low * kMinGapPercent) continue; // not a cluster boundary
    // Compare rise/low > bestRise/bestLow without floating point.
    if (bestIndex < 0 || rise * bestLow > bestRise * low) {
      bestRise = rise;
      bestLow = low;
      bestIndex = i;
    }
  }
  if (bestIndex < 0) return 0; // every core within kMinGapPercent of its neighbour: one cluster
  return sortedAscending[bestIndex + 1]; // the big cluster is everything at or above this
}

} // namespace

int CountPerformanceCores(const std::uint64_t *maxFreqKhz, int count) {
  if (maxFreqKhz == nullptr || count <= 0) return 0;

  std::uint64_t fastest = 0;
  for (int i = 0; i < count; ++i) {
    if (maxFreqKhz[i] > fastest) fastest = maxFreqKhz[i];
  }
  // No core reported a frequency: the caller cannot tell fast from slow, so it must not pin.
  if (fastest == 0) return 0;

  int performance = 0;
  for (int i = 0; i < count; ++i) {
    if (maxFreqKhz[i] == 0) continue;
    // Compare as integers to avoid floating-point edge cases: freq*10 >= fastest*9 is the
    // same as freq/fastest >= 0.9, with no rounding surprises at exactly 0.9.
    if (maxFreqKhz[i] * 10ULL >= fastest * 9ULL) ++performance;
  }
  return performance;
}

int BigClusterCoreCount(const std::uint64_t *maxFreqKhz, int count) {
  if (maxFreqKhz == nullptr || count <= 0) return 0;
  std::uint64_t sorted[kScratch] = {};
  const int n = SortedAscending(maxFreqKhz, count > kScratch ? kScratch : count, sorted);
  if (n == 0) return 0;
  const std::uint64_t threshold = BigClusterThreshold(sorted, n);
  if (threshold == 0) return 0; // no gap: cannot tell which cluster is the big one
  int big = 0;
  for (int i = 0; i < n; ++i) {
    if (sorted[i] >= threshold) ++big;
  }
  return big;
}

int SelectBigClusterCoreIndices(const std::uint64_t *maxFreqKhz, int count, int *out, int maxOut) {
  if (maxFreqKhz == nullptr || count <= 0 || out == nullptr || maxOut <= 0) return 0;
  std::uint64_t sorted[kScratch] = {};
  const int n = SortedAscending(maxFreqKhz, count > kScratch ? kScratch : count, sorted);
  if (n == 0) return 0;
  const std::uint64_t threshold = BigClusterThreshold(sorted, n);
  if (threshold == 0) return 0;
  int written = 0;
  for (int i = 0; i < count && written < maxOut; ++i) {
    if (maxFreqKhz[i] >= threshold) out[written++] = i;
  }
  return written;
}

bool ShouldPinToPerformanceCores(int performanceCoreCount) {
  return performanceCoreCount >= kOptifineMinPerformanceCores;
}

std::uint64_t BuildCoreMask(const int *coreIndices, int count) {
  if (coreIndices == nullptr || count <= 0) return 0;
  std::uint64_t mask = 0;
  for (int i = 0; i < count; ++i) {
    const int index = coreIndices[i];
    if (index >= 0 && index < kOptifineMaxCores) {
      mask |= (1ULL << index);
    }
  }
  return mask;
}

int SelectTargetRefreshRate(const int *supportedHz, int count, int currentHz) {
  if (supportedHz == nullptr || count <= 0) return 0;

  int highest = 0;
  for (int i = 0; i < count; ++i) {
    if (supportedHz[i] > highest) highest = supportedHz[i];
  }
  // Nothing to unlock: the panel's best is not above what the game already uses.
  if (highest <= currentHz) return 0;
  return highest;
}

} // namespace pl::runtime
