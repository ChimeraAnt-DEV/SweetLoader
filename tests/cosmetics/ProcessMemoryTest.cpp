// Host tests for the `/proc` footprint reader.
//
// The parse rules are the part that can be wrong silently: a Pss line read from the wrong field
// would report a plausible number that means something else, and a fabricated 0 would read as an
// empty process. Both are pinned here without a device.

#include "pl/runtime/ProcessMemory.hpp"

#include <cstdio>
#include <cstring>

using pl::runtime::ParseSmapsRollupPss;
using pl::runtime::ResidentPagesToKb;

static int checks = 0;
static int failures = 0;

static void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("FAIL: %s\n", what);
  }
}

int main() {
  // A real smaps_rollup header, with Rss and Pss both present: only Pss is the proportional share.
  {
    const char *doc =
        "55f2d3e000-7fff0000000 ---p 00000000 00:00 0                          [rollup]\n"
        "Rss:              180000 kB\n"
        "Pss:               90000 kB\n"
        "Pss_Anon:          40000 kB\n"
        "Pss_File:          50000 kB\n";
    std::uint64_t pss = 0;
    expect(ParseSmapsRollupPss(doc, std::strlen(doc), pss), "a Pss line is found");
    expect(pss == 90000, "the Pss value is read, not Rss and not Pss_Anon");
  }

  // Pss_Anon must not satisfy a search for "Pss:" - the colon is what makes it a distinct field.
  {
    const char *doc = "Rss:  10 kB\nPss_Anon: 500 kB\nPss_File: 600 kB\n";
    std::uint64_t pss = 0;
    expect(!ParseSmapsRollupPss(doc, std::strlen(doc), pss),
           "Pss_Anon/Pss_File alone do not satisfy the Pss lookup");
  }

  // A Pss line that appears mid-document (not first) is still found.
  {
    const char *doc = "Rss: 100 kB\nShared_Clean: 5 kB\nPss: 42 kB\n";
    std::uint64_t pss = 0;
    expect(ParseSmapsRollupPss(doc, std::strlen(doc), pss), "a later Pss line is found");
    expect(pss == 42, "the later Pss value is parsed");
  }

  // A value named in another field cannot be picked up because the match is anchored at a line
  // start: "SomePss: 7" contains "Pss:" but not at a boundary.
  {
    const char *doc = "SomePss: 7 kB\nRss: 1 kB\n";
    std::uint64_t pss = 0;
    expect(!ParseSmapsRollupPss(doc, std::strlen(doc), pss),
           "a field whose name merely ends in Pss is not matched");
  }

  // Empty and null input are refusals, not zeros written to the out parameter.
  {
    std::uint64_t pss = 123;
    expect(!ParseSmapsRollupPss("", 0, pss), "empty input is refused");
    expect(pss == 123, "a refused parse leaves the out parameter untouched");
    expect(!ParseSmapsRollupPss(nullptr, 10, pss), "null input is refused");
  }

  // A "Pss:" with no digits is a refusal rather than a 0 reading.
  {
    const char *doc = "Pss:\n";
    std::uint64_t pss = 55;
    expect(!ParseSmapsRollupPss(doc, std::strlen(doc), pss), "a digitless Pss line is refused");
  }

  // Resident pages: 0 pages or a 0 page size is "not available", never 0 KiB of memory.
  expect(ResidentPagesToKb(0, 4) == 0, "zero resident pages read as unavailable");
  expect(ResidentPagesToKb(100, 0) == 0, "an unknown page size reads as unavailable");
  expect(ResidentPagesToKb(1024, 4) == 4096, "resident pages convert to KiB");

  std::printf("ProcessMemoryTest: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
