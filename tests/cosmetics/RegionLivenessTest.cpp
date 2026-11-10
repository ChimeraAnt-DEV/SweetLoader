/**
 * @file RegionLivenessTest.cpp
 * @brief Host test for the cached-region liveness rule.
 *
 * The scanner trusts a cached region list for a short TTL, then re-validates: every region must still
 * answer a one-byte read, and the base must still begin with the ELF magic (audit item 4.3). The
 * second part is what catches a module reloaded at a different base. A fake reader pins the rule
 * without a live process.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "pl/memory/RegionLiveness.hpp"

using pl::memory::RegionsStillMapped;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

// A fake address space: a set of mapped addresses, each holding a byte; the base holds ELF magic.
struct FakeSpace {
  std::vector<std::uintptr_t> mapped;
  std::uintptr_t base = 0x1000;
  bool elfAtBase = true;
};

FakeSpace g_space;

bool Read(const std::uintptr_t address, const std::size_t size, std::uint8_t *out) {
  if (address == g_space.base && size == 4) {
    if (!g_space.elfAtBase) return false;
    const std::uint8_t magic[4] = {0x7F, 'E', 'L', 'F'};
    std::memcpy(out, magic, 4);
    return true;
  }
  for (std::uintptr_t m : g_space.mapped) {
    if (address == m && size == 1) {
      out[0] = 0;
      return true;
    }
  }
  return false;
}

} // namespace

int main() {
  // No regions -> never valid (an empty list is not a mapped module).
  expect(!RegionsStillMapped(nullptr, 0, g_space.base, Read), "an empty region list is not valid");

  // All regions mapped and the base is ELF: valid.
  g_space.mapped = {0x2000, 0x3000};
  const std::uintptr_t starts[] = {0x2000, 0x3000};
  expect(RegionsStillMapped(starts, 2, g_space.base, Read), "all mapped and ELF base is valid");

  // One region unmapped: invalid.
  g_space.mapped = {0x2000};
  expect(!RegionsStillMapped(starts, 2, g_space.base, Read), "a missing region is invalid");

  // Regions mapped but the base is no longer ELF (a reload at a different base): invalid.
  g_space.mapped = {0x2000, 0x3000};
  g_space.elfAtBase = false;
  expect(!RegionsStillMapped(starts, 2, g_space.base, Read), "a non-ELF base is invalid");
  g_space.elfAtBase = true;

  // A null reader fails closed.
  expect(!RegionsStillMapped(starts, 2, g_space.base, nullptr), "a null reader fails closed");

  // The magic rule itself.
  const std::uint8_t elf[4] = {0x7F, 'E', 'L', 'F'};
  const std::uint8_t notElf[4] = {0x7F, 'E', 'L', 'X'};
  expect(pl::memory::IsElfMagic(elf), "ELF magic is recognised");
  expect(!pl::memory::IsElfMagic(notElf), "a non-ELF first word is rejected");

  std::printf("RegionLivenessTest: %d checks passed\n", checks);
  return 0;
}
