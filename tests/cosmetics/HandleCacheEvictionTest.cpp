/**
 * @file HandleCacheEvictionTest.cpp
 * @brief Host test for the module-handle eviction rule.
 *
 * DobbyHookManager caches one GlossOpen handle per module and never closes it. That is fine for a
 * session that resolves a handful of modules once, but a hot-reloading or heavily-modded session
 * grows the cache without bound. The eviction rule decides which cached handles a maintenance pass
 * may close: a handle not touched within the last N generations, and never the pinned game module.
 */
#include <cstdio>
#include <cstdlib>

#include "pl/hooks/HandleCacheEviction.hpp"

using pl::hooks::ShouldEvictHandle;

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
  // A handle touched this generation is never evicted.
  expect(!ShouldEvictHandle(/*lastTouch=*/10, /*currentGen=*/10, /*keepRecent=*/2, /*pinned=*/false),
         "a just-touched handle is kept");
  expect(!ShouldEvictHandle(9, 10, 2, false), "a handle touched last generation is kept");
  expect(!ShouldEvictHandle(8, 10, 2, false), "a handle within the keep window is kept");

  // A handle untouched for longer than the window is evicted.
  expect(ShouldEvictHandle(7, 10, 2, false), "a handle older than the window is evicted");
  expect(ShouldEvictHandle(0, 100, 2, false), "a long-untouched handle is evicted");

  // A pinned handle (the game module) is never evicted, however stale.
  expect(!ShouldEvictHandle(0, 100, 2, /*pinned=*/true), "a pinned handle is never evicted");

  // A zero window still keeps the current generation: evicting what was just used would defeat the
  // cache and could close a handle another thread is resolving through.
  expect(!ShouldEvictHandle(10, 10, 0, false), "a zero window keeps the current generation");
  expect(ShouldEvictHandle(9, 10, 0, false), "a zero window evicts anything older");

  // A generation that wrapped below the last touch (should not happen, but must not evict blindly).
  expect(!ShouldEvictHandle(10, 5, 2, false), "a lower current generation does not evict");

  std::printf("HandleCacheEvictionTest: %d checks passed\n", checks);
  return 0;
}
