/**
 * @file OptifineDeviceRulesTest.cpp
 * @brief Host test for the device rules, with real SoC frequency tables.
 *
 * The core-selection rule decides which cores the client pins the game thread to. A wrong rule is
 * only visible as a device that boots but stutters, so it is pinned here with the frequency tables of
 * real tri-cluster and bi-cluster SoCs.
 *
 * The old rule counted a core as "performance" when its max frequency was at least 0.9 of the fastest.
 * On a tri-cluster SoC whose gold cores run below 0.9 of the prime core (Snapdragon 8 Gen 1: 2.5GHz
 * gold vs 3.0GHz prime = 0.83), that counts only the single prime core, so the "need 4+" check refuses
 * to pin and the feature silently does nothing. The rule here clusters by the largest frequency gap,
 * which recovers the whole big cluster (prime + gold).
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "pl/runtime/optifine/OptifineDeviceRules.h"

using pl::runtime::BigClusterCoreCount;
using pl::runtime::BuildCoreMask;
using pl::runtime::SelectBigClusterCoreIndices;
using pl::runtime::ShouldPinToPerformanceCores;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

struct Table {
  const char *name;
  std::uint64_t freq[8];
  int count;
  int expectedBig;
};

// The frequency tables are per-core max frequencies in kHz, in core order, as sysfs reports them.
const Table kTables[] = {
    // 1x3.0 prime + 3x2.5 gold + 4x1.8 silver. 2.5/3.0 = 0.83, so the old 0.9 rule saw 1 core and
    // skipped pinning. The big cluster is 4.
    {"Snapdragon 8 Gen 1 (1+3+4)", {3000, 2500, 2500, 2500, 1800, 1800, 1800, 1800}, 8, 4},
    // 1x3.2 + 4x2.8 + 3x2.0. 2.8/3.2 = 0.875 < 0.9, so the old rule also saw 1 here. Big = 5.
    {"Snapdragon 8 Gen 2 (1+4+3)", {3200, 2800, 2800, 2800, 2800, 2000, 2000, 2000}, 8, 5},
    // 1x2.84 + 3x2.42 + 4x1.8. 2.42/2.84 = 0.85 < 0.9. Big = 4.
    {"Snapdragon 888 (1+3+4)", {2840, 2420, 2420, 2420, 1800, 1800, 1800, 1800}, 8, 4},
    // 1x2.8 + 3x2.52 + 4x1.82. 2.52/2.8 = 0.9 exactly, so even the old rule passed here. Big = 4.
    {"Exynos 2200 (1+3+4)", {2800, 2520, 2520, 2520, 1820, 1820, 1820, 1820}, 8, 4},
    // 1x3.05 + 3x2.85 + 4x1.8. 2.85/3.05 = 0.934 >= 0.9, so the old rule passed here. Big = 4.
    {"Dimensity 9000 (1+3+4)", {3050, 2850, 2850, 2850, 1800, 1800, 1800, 1800}, 8, 4},
    // 4x2.4 + 4x1.8: a bi-cluster mid-range SoC. Big = 4.
    {"Snapdragon 765G (4+4)", {2400, 2400, 2400, 2400, 1800, 1800, 1800, 1800}, 8, 4},
    // 2x2.2 + 6x1.8: a 2+6 SoC. Big = 2, which is below the 4-core floor, so pinning is refused.
    {"Dimensity 700 (2+6)", {2200, 2200, 1800, 1800, 1800, 1800, 1800, 1800}, 8, 2},
    // Every core the same: there is no gap to cluster on, so the rule must decline rather than guess.
    {"All cores equal (unknown)", {2000, 2000, 2000, 2000, 2000, 2000, 2000, 2000}, 8, 0},
    // A core with no cpufreq directory reports 0 and must not be counted as a performance core.
    {"Offline core reported as 0", {3000, 2500, 2500, 2500, 0, 1800, 1800, 1800}, 8, 4},
};

} // namespace

int main() {
  for (const Table &t : kTables) {
    const int big = BigClusterCoreCount(t.freq, t.count);
    if (big != t.expectedBig) {
      std::fprintf(stderr, "FAIL: %s: expected big cluster %d, got %d\n", t.name, t.expectedBig,
                   big);
      std::abort();
    }
    ++checks;
  }

  // The indices selected are the ones at the top of the table, and they build the expected mask.
  {
    const Table &t = kTables[0]; // Snapdragon 8 Gen 1
    int indices[8] = {};
    const int n = SelectBigClusterCoreIndices(t.freq, t.count, indices, 8);
    expect(n == 4, "four big-cluster indices are selected");
    for (int i = 0; i < n; ++i) {
      expect(indices[i] < 4, "every selected index is one of the four fast cores");
    }
    expect(BuildCoreMask(indices, n) == 0xFULL, "the mask is the low four bits");
  }

  // The 4-core floor: a big cluster of 4 pins, one of 2 does not.
  expect(ShouldPinToPerformanceCores(4), "four performance cores are enough to pin");
  expect(!ShouldPinToPerformanceCores(2), "two performance cores are not enough to pin");

  // Discriminator: the old 0.9-ratio rule counts only the prime core on a tri-cluster SoC, so it
  // reports fewer than 4 and the feature silently does nothing. BigClusterCoreCount recovers the
  // whole big cluster, which is what makes pinning happen on those devices.
  {
    const std::uint64_t sd8gen1[8] = {3000, 2500, 2500, 2500, 1800, 1800, 1800, 1800};
    expect(pl::runtime::CountPerformanceCores(sd8gen1, 8) < 4,
           "the old 0.9-ratio rule under-counts a tri-cluster's big cluster");
    expect(BigClusterCoreCount(sd8gen1, 8) == 4,
           "the gap rule counts the whole big cluster on the same table");
  }

  // An empty or null table declines.
  expect(BigClusterCoreCount(nullptr, 8) == 0, "a null table has no big cluster");
  expect(BigClusterCoreCount(kTables[0].freq, 0) == 0, "an empty table has no big cluster");

  std::printf("OptifineDeviceRulesTest: %d checks passed\n", checks);
  return 0;
}
