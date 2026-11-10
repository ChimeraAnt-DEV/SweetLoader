/**
 * @file VtableMemoTest.cpp
 * @brief Host test for the vtable memo cache keys and the section-cache rule.
 *
 * Two vtable-resolver lookups were repeated per session (audit items 4.12, 4.14). The keys must
 * separate lookups that can differ (a different needle, module, type, or slot bound), and a section
 * miss must be cached only once the module is known, so a not-yet-mapped module can still resolve
 * later.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "pl/memory/VtableMemo.hpp"

using pl::memory::MakeSectionKey;
using pl::memory::MakeVtableNeedleKey;
using pl::memory::ShouldCacheSection;

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
  const std::string base =
      MakeVtableNeedleKey("libminecraftpe.so", "18LivePlayerRenderer", "variable.player_x_rotation",
                          100);
  expect(base == MakeVtableNeedleKey("libminecraftpe.so", "18LivePlayerRenderer",
                                     "variable.player_x_rotation", 100),
         "identical needle lookups share a key");
  expect(base != MakeVtableNeedleKey("libminecraftpe.so", "18LivePlayerRenderer",
                                     "variable.is_first_person", 100),
         "a different needle changes the key");
  expect(base != MakeVtableNeedleKey("libminecraftpe.so", "18LivePlayerRenderer",
                                     "variable.player_x_rotation", 200),
         "a different slot bound changes the key");
  expect(base != MakeVtableNeedleKey("other.so", "18LivePlayerRenderer",
                                     "variable.player_x_rotation", 100),
         "a different module changes the key");
  expect(base != MakeVtableNeedleKey("libminecraftpe.so", "18OtherType",
                                     "variable.player_x_rotation", 100),
         "a different type changes the key");

  const std::string rodata = MakeSectionKey("libminecraftpe.so", ".rodata");
  expect(rodata == MakeSectionKey("libminecraftpe.so", ".rodata"), "identical sections share a key");
  expect(rodata != MakeSectionKey("libminecraftpe.so", ".text"),
         "a different section changes the key");
  expect(rodata != MakeSectionKey("other.so", ".rodata"), "a different module changes the key");

  expect(ShouldCacheSection(true, 0x1000), "a section hit for a found module is cached");
  expect(ShouldCacheSection(true, 0), "a section miss for a found module is cached");
  expect(!ShouldCacheSection(false, 0), "a miss from an unmapped module is not cached");

  std::printf("VtableMemoTest: %d checks passed\n", checks);
  return 0;
}
