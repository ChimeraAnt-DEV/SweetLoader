/**
 * @file AntEggArchiveTest.cpp
 * @brief Host test for the pointer-based .antegg ZIP reading and the mapped-file view.
 *
 * The ZIP reader used to take a std::vector, which forced the loader to read the whole archive into
 * heap (audit item 2.16). It now takes a byte pointer and a length, so the loader can walk a
 * read-only mapping, and a test can hand it a buffer built in memory. This test builds a minimal
 * ZIP by hand, reads it back, and checks that the CRC check rejects a corrupted entry.
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

#include "pl/runtime/AntEggArchive.hpp"
#include "pl/runtime/MappedFile.hpp"

using pl::runtime::ExtractEntryData;
using pl::runtime::MappedFile;
using pl::runtime::ParseCentralDirectory;
using pl::runtime::ZipEntryRecord;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

void put16(std::vector<std::uint8_t> &b, std::uint16_t v) {
  b.push_back(static_cast<std::uint8_t>(v & 0xff));
  b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
}

void put32(std::vector<std::uint8_t> &b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xff));
}

/// Builds a one-entry, stored (uncompressed) ZIP with the given name and content.
std::vector<std::uint8_t> BuildStoredZip(const std::string &name, const std::string &content,
                                         std::uint32_t crcOverride = 0xffffffffu) {
  const std::uint32_t crc = ::crc32(0L, reinterpret_cast<const Bytef *>(content.data()),
                                    static_cast<uInt>(content.size()));
  const std::uint32_t storedCrc = crcOverride == 0xffffffffu ? crc : crcOverride;
  std::vector<std::uint8_t> out;
  const std::uint32_t localOffset = 0;
  // Local file header.
  put32(out, 0x04034b50);
  put16(out, 20);  // version needed
  put16(out, 0);   // flags
  put16(out, 0);   // method 0 = stored
  put16(out, 0);   // mod time
  put16(out, 0);   // mod date
  put32(out, storedCrc);
  put32(out, static_cast<std::uint32_t>(content.size()));
  put32(out, static_cast<std::uint32_t>(content.size()));
  put16(out, static_cast<std::uint16_t>(name.size()));
  put16(out, 0);  // extra len
  out.insert(out.end(), name.begin(), name.end());
  out.insert(out.end(), content.begin(), content.end());

  const std::uint32_t cdOffset = static_cast<std::uint32_t>(out.size());
  // Central directory header.
  put32(out, 0x02014b50);
  put16(out, 20);  // version made by
  put16(out, 20);  // version needed
  put16(out, 0);   // flags
  put16(out, 0);   // method
  put16(out, 0);   // time
  put16(out, 0);   // date
  put32(out, storedCrc);
  put32(out, static_cast<std::uint32_t>(content.size()));
  put32(out, static_cast<std::uint32_t>(content.size()));
  put16(out, static_cast<std::uint16_t>(name.size()));
  put16(out, 0);  // extra
  put16(out, 0);  // comment
  put16(out, 0);  // disk start
  put16(out, 0);  // internal attrs
  put32(out, 0);  // external attrs
  put32(out, localOffset);
  out.insert(out.end(), name.begin(), name.end());

  const std::uint32_t cdSize = static_cast<std::uint32_t>(out.size()) - cdOffset;
  // End of central directory.
  put32(out, 0x06054b50);
  put16(out, 0);  // disk
  put16(out, 0);  // cd start disk
  put16(out, 1);  // entries on this disk
  put16(out, 1);  // total entries
  put32(out, cdSize);
  put32(out, cdOffset);
  put16(out, 0);  // comment len
  return out;
}

} // namespace

int main() {
  const std::string payload = "{\"name\":\"T\"}";
  const std::vector<std::uint8_t> zip = BuildStoredZip("egg.json", payload);

  // Parse the central directory.
  std::vector<ZipEntryRecord> entries;
  expect(ParseCentralDirectory(zip.data(), zip.size(), entries), "the central directory parses");
  expect(entries.size() == 1, "one entry is read");
  expect(entries.size() == 1 && entries[0].name == "egg.json", "the entry name is read");
  expect(entries.size() == 1 && entries[0].method == 0, "the method is stored");
  expect(entries.size() == 1 && entries[0].crc ==
             ::crc32(0L, reinterpret_cast<const Bytef *>(payload.data()),
                     static_cast<uInt>(payload.size())),
         "the stored CRC is read");

  // Extract it and check the bytes and CRC.
  std::vector<std::uint8_t> content;
  expect(ExtractEntryData(zip.data(), zip.size(), entries[0], content), "the entry extracts");
  expect(std::string(content.begin(), content.end()) == payload, "the bytes round trip");

  // A corrupted CRC is refused.
  const std::vector<std::uint8_t> badZip = BuildStoredZip("egg.json", payload, 0x12345678u);
  std::vector<ZipEntryRecord> badEntries;
  expect(ParseCentralDirectory(badZip.data(), badZip.size(), badEntries),
         "the corrupted archive's directory still parses");
  std::vector<std::uint8_t> badContent;
  expect(!ExtractEntryData(badZip.data(), badZip.size(), badEntries[0], badContent),
         "a mismatched CRC is refused");

  // Degenerate inputs are refused, not crashed on.
  std::vector<ZipEntryRecord> none;
  expect(!ParseCentralDirectory(nullptr, 0, none), "a null buffer is refused");
  expect(!ParseCentralDirectory(zip.data(), 10, none), "a too-short buffer is refused");

  // The mapped-file view reads the same bytes and validates a size cap.
  MappedFile mapped = MappedFile::Open("/proc/self/exe", 1024ull * 1024ull * 1024ull);
  expect(mapped.valid(), "a real file maps");
  expect(mapped.size() > 0, "the mapped file has a size");
  expect(mapped.data() != nullptr, "the mapped file has bytes");
  // Over the cap: refused.
  MappedFile refused = MappedFile::Open("/proc/self/exe", 4);
  expect(!refused.valid(), "a file over the cap is refused");
  // A missing file: refused.
  MappedFile missing = MappedFile::Open("/nonexistent/does/not/exist", 1024);
  expect(!missing.valid(), "a missing file is refused");

  std::printf("AntEggArchiveTest: %d checks passed\n", checks);
  return 0;
}
