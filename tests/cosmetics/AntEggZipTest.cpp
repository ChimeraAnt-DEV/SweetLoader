/**
 * @file AntEggZipTest.cpp
 * @brief Host test for the .antegg ZIP rules.
 *
 * Two reader gaps are covered (audit items 12.3 and 12.5): an entry's stored CRC32 was never
 * verified, and a ZIP64 archive was parsed as a plain ZIP and its 32-bit fields misread. Both rules
 * are pure, so they need no archive.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "pl/runtime/AntEggZip.hpp"

using pl::runtime::HasZip64EndOfCentralDirectory;
using pl::runtime::kZip64EocdLocatorSignature;
using pl::runtime::kZip64EocdSignature;
using pl::runtime::ZipCrcMatches;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

void put32(std::vector<std::uint8_t> &buf, std::uint32_t value) {
  std::uint8_t bytes[4];
  std::memcpy(bytes, &value, 4);
  buf.insert(buf.end(), bytes, bytes + 4);
}

} // namespace

int main() {
  // The CRC rule.
  expect(ZipCrcMatches(0, 0), "an empty entry's zero CRC matches");
  expect(ZipCrcMatches(0x1234abcd, 0x1234abcd), "an equal CRC matches");
  expect(!ZipCrcMatches(0x1234abcd, 0x00000000), "a mismatched CRC is refused");
  expect(!ZipCrcMatches(0, 0xdeadbeef), "a zero stored CRC against a non-zero computed one is refused");

  // No ZIP64 markers: a small plain-ZIP tail reads as not-ZIP64.
  std::vector<std::uint8_t> plain;
  put32(plain, 0x04034b50u);   // local file header
  plain.insert(plain.end(), 40, 0);
  put32(plain, 0x06054b50u);   // the plain EOCD signature
  plain.insert(plain.end(), 18, 0);
  expect(!HasZip64EndOfCentralDirectory(plain.data(), plain.size()),
         "a plain ZIP has no ZIP64 record");

  // A ZIP64 locator in the tail is detected.
  std::vector<std::uint8_t> zip64 = plain;
  put32(zip64, kZip64EocdLocatorSignature);
  zip64.insert(zip64.end(), 16, 0);
  put32(zip64, 0x06054b50u);
  zip64.insert(zip64.end(), 18, 0);
  expect(HasZip64EndOfCentralDirectory(zip64.data(), zip64.size()),
         "a ZIP64 EOCD locator is detected");

  // A ZIP64 EOCD record alone is detected too.
  std::vector<std::uint8_t> zip64Record;
  put32(zip64Record, kZip64EocdSignature);
  zip64Record.insert(zip64Record.end(), 40, 0);
  put32(zip64Record, 0x06054b50u);
  zip64Record.insert(zip64Record.end(), 18, 0);
  expect(HasZip64EndOfCentralDirectory(zip64Record.data(), zip64Record.size()),
         "a ZIP64 EOCD record is detected");

  // Degenerate inputs do not crash.
  expect(!HasZip64EndOfCentralDirectory(nullptr, 0), "a null buffer is not ZIP64");
  expect(!HasZip64EndOfCentralDirectory(plain.data(), 3), "a buffer shorter than a signature is not ZIP64");

  std::printf("AntEggZipTest: %d checks passed\n", checks);
  return 0;
}
