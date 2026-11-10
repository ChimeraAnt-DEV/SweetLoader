/**
 * @file SignatureCachePolicyTest.cpp
 * @brief Host test for the bounded-pattern-cache and negative-cache rules.
 *
 * Two resolver caches had a defect (audit items 4.5, 4.6): the parsed-pattern cache grew without
 * bound, and a genuinely absent signature was re-derived by a full scan on every resolve. The rules
 * that fix both are pure, so they are pinned here: the cap trims only past the bound, and a miss is
 * only remembered when the scan actually ran over a readable module and still found nothing.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "pl/memory/SignatureCachePolicy.hpp"

using pl::memory::kPatternCacheCap;
using pl::memory::MissIsCacheable;
using pl::memory::PatternCacheNeedsTrim;

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
  // The parsed-pattern cache is trimmed only once it has outgrown its cap.
  expect(!PatternCacheNeedsTrim(0), "an empty cache needs no trim");
  expect(!PatternCacheNeedsTrim(kPatternCacheCap), "a cache exactly at the cap needs no trim");
  expect(PatternCacheNeedsTrim(kPatternCacheCap + 1), "a cache past the cap is trimmed");
  expect(!PatternCacheNeedsTrim(10, 10), "a custom cap is honoured at the boundary");
  expect(PatternCacheNeedsTrim(11, 10), "a custom cap is honoured past the boundary");

  // A miss is remembered only when the scan ran over a readable module and found nothing.
  expect(MissIsCacheable(true, true, 0), "a scan miss over a readable module is cacheable");
  expect(!MissIsCacheable(false, true, 0), "a miss from a module with no regions is not cacheable");
  expect(!MissIsCacheable(true, false, 0), "a miss the scan never reached is not cacheable");
  expect(!MissIsCacheable(true, true, 0x1234), "a resolved address is a hit, never a cached miss");
  expect(!MissIsCacheable(false, false, 0), "no readable module and no scan is not cacheable");

  std::printf("SignatureCachePolicyTest: %d checks passed\n", checks);
  return 0;
}
