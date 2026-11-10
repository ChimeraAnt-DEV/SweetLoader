/**
 * @file RulesFileCacheTest.cpp
 * @brief Host test for the rules-file parse cache.
 *
 * ReadConfiguredOptifineSlot, EvaluateConfiguredFeatureGate and LoadConfiguredGameHookSignatures each
 * reopened and reparsed the rules JSON on every call, so a single install re-read the file several
 * times. The cache keys on path + mtime + size: a repeat call with the same key must not re-read,
 * and a changed file must. The loader is injected, so this is tested without touching the disk.
 */

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "pl/runtime/RulesFileCache.hpp"

using pl::runtime::RulesFileCache;

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
  RulesFileCache cache;
  int loads = 0;
  auto loader = [&loads] {
    ++loads;
    return std::make_shared<const nlohmann::json>(nlohmann::json::parse(R"({"revision":7})"));
  };

  // First call loads and returns the parsed document.
  auto first = cache.get("/rules.json", 100, 50, loader);
  expect(first != nullptr && loads == 1, "the first call loads");
  expect((*first)["revision"].get<int>() == 7, "the parsed document is returned");

  // Same key: served from the cache, no second read.
  auto second = cache.get("/rules.json", 100, 50, loader);
  expect(loads == 1, "an unchanged file is not re-read");
  expect(second == first, "the same cached document is returned");

  // mtime changed: reload.
  cache.get("/rules.json", 101, 50, loader);
  expect(loads == 2, "a changed mtime reloads");

  // size changed: reload.
  cache.get("/rules.json", 101, 51, loader);
  expect(loads == 3, "a changed size reloads");

  // path changed: reload.
  cache.get("/other.json", 101, 51, loader);
  expect(loads == 4, "a changed path reloads");

  // invalidate() forces the next call to reload.
  cache.invalidate();
  cache.get("/other.json", 101, 51, loader);
  expect(loads == 5, "invalidate() forces a reload");

  // An empty path never loads.
  expect(cache.get("", 0, 0, loader) == nullptr, "an empty path returns null");
  expect(loads == 5, "an empty path does not load");

  // A loader that returns null is not cached as a hit: the next call retries.
  int nullLoads = 0;
  auto nullLoader = [&nullLoads] {
    ++nullLoads;
    return std::shared_ptr<const nlohmann::json>{};
  };
  expect(cache.get("/missing.json", 5, 5, nullLoader) == nullptr, "a null load returns null");
  cache.get("/missing.json", 5, 5, nullLoader);
  expect(nullLoads == 2, "a null load is retried rather than cached");

  std::printf("RulesFileCacheTest: %d checks passed\n", checks);
  return 0;
}
