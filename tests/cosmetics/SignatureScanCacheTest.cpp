/**
 * @file SignatureScanCacheTest.cpp
 * @brief Host test for the on-disk signature scan cache.
 *
 * The cache lets a repeat launch skip the byte-pattern scan. The two ways it could be wrong are
 * storing an absolute address (wrong the next launch, because ASLR moves the base) and answering
 * for a different build (wrong after a game update). Both are pinned here, along with the refusal
 * of a truncated or foreign file.
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include "pl/memory/SignatureScanCache.hpp"

using pl::memory::ParseSignatureScanCache;
using pl::memory::SerializeSignatureScanCache;

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
  const std::string buildId = "a1b2c3d4e5f6";

  // A round trip preserves the entries and stores offsets, not addresses.
  {
    std::unordered_map<std::string, std::uintptr_t> entries{
        {"pauseMenuDtorSig", 0x1234},
        {"hudScreenOpenSig", 0xdeadbeef},
    };
    const std::string text = SerializeSignatureScanCache(buildId, entries);
    std::unordered_map<std::string, std::uintptr_t> parsed;
    expect(ParseSignatureScanCache(text, buildId, parsed), "a written cache parses back");
    expect(parsed.size() == 2, "both entries survive the round trip");
    expect(parsed["pauseMenuDtorSig"] == 0x1234, "an offset round-trips");
    expect(parsed["hudScreenOpenSig"] == 0xdeadbeef, "a large offset round-trips");
  }

  // The header carries the magic and the build id, so a foreign file is refused.
  {
    const std::string text = SerializeSignatureScanCache(buildId, {{"sig", 0x10}});
    std::unordered_map<std::string, std::uintptr_t> parsed;
    expect(!ParseSignatureScanCache(text, "different-build", parsed),
           "a cache for another build is refused");
    expect(parsed.empty(), "a refused cache leaves the map empty");
  }

  // A file with no magic is refused.
  {
    std::unordered_map<std::string, std::uintptr_t> parsed;
    expect(!ParseSignatureScanCache("not a cache file\nabc\n10 sig\n", buildId, parsed),
           "a file with the wrong magic is refused");
  }

  // A zero offset is never stored: zero is the resolver's "not found", and re-scanning is cheaper
  // than trusting a cached miss.
  {
    const std::string text = SerializeSignatureScanCache(buildId, {{"found", 0x20}, {"missing", 0}});
    std::unordered_map<std::string, std::uintptr_t> parsed;
    expect(ParseSignatureScanCache(text, buildId, parsed), "a cache with a zero entry parses");
    expect(parsed.count("missing") == 0, "a zero offset is not stored");
    expect(parsed["found"] == 0x20, "the non-zero entry is kept");
  }

  // An empty cache (header only) parses and yields no entries.
  {
    const std::string text = SerializeSignatureScanCache(buildId, {});
    std::unordered_map<std::string, std::uintptr_t> parsed;
    expect(ParseSignatureScanCache(text, buildId, parsed), "a header-only cache parses");
    expect(parsed.empty(), "a header-only cache has no entries");
  }

  // A malformed entry line is skipped, not fatal.
  {
    std::string text = std::string(pl::memory::kSignatureScanCacheMagic) + "\n" + buildId + "\n";
    text += "nothex sig\n";      // bad offset
    text += "10\n";              // no signature
    text += "zz sig\n";          // non-hex offset
    text += "20 good\n";         // valid
    std::unordered_map<std::string, std::uintptr_t> parsed;
    expect(ParseSignatureScanCache(text, buildId, parsed), "a cache with junk lines parses");
    expect(parsed.size() == 1 && parsed["good"] == 0x20,
           "only the well-formed entry survives");
  }

  // An empty expected build id (no build id could be read) refuses every file, because the key
  // would otherwise be empty and every build would match it.
  {
    const std::string text = SerializeSignatureScanCache(buildId, {{"sig", 0x10}});
    std::unordered_map<std::string, std::uintptr_t> parsed;
    expect(!ParseSignatureScanCache(text, "", parsed),
           "an unknown build id refuses the cache rather than matching everything");
  }

  std::printf("SignatureScanCacheTest: %d checks passed\n", checks);
  return 0;
}
