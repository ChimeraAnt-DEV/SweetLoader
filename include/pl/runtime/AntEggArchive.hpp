#pragma once

/**
 * @file AntEggArchive.hpp
 * @brief The pure ZIP reading the .antegg loader is built on.
 *
 * <p>These functions take a byte pointer and a length rather than a {@code std::vector}, so the
 * loader can walk a memory-mapped archive (audit item 2.16) with no copy, and a host test can hand
 * them a small buffer with no file at all. They read untrusted bytes, so every offset is bounds
 * checked before it is dereferenced and every allocation is capped before it is made.
 */

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include <zlib.h>

#include "pl/runtime/AntEggZip.hpp"

namespace pl::runtime {

/** The largest a single entry may inflate to; a larger declared size is a zip bomb. */
inline constexpr std::uint64_t kMaxUncompressedBytes = 256ull * 1024ull * 1024ull;

/** One entry from the ZIP central directory. */
struct ZipEntryRecord {
  std::string name;
  std::uint16_t method = 0;
  std::uint32_t crc = 0;
  std::uint32_t compressed_size = 0;
  std::uint32_t uncompressed_size = 0;
  std::uint32_t local_offset = 0;
  bool is_directory = false;
};

/** Reads a little-endian 16-bit value. The caller has bounds-checked @p offset + 2. */
inline std::uint16_t ZipReadU16(const std::uint8_t *data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}

/** Reads a little-endian 32-bit value. The caller has bounds-checked @p offset + 4. */
inline std::uint32_t ZipReadU32(const std::uint8_t *data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

/**
 * @brief Locates the End Of Central Directory and parses the central directory entry table.
 *
 * <p>Refuses a ZIP64 archive (audit item 12.3), whose 64-bit fields this parser does not read.
 */
inline bool ParseCentralDirectory(const std::uint8_t *data, std::size_t size,
                                  std::vector<ZipEntryRecord> &out) {
  if (data == nullptr || size < 22) return false;
  const std::size_t search_start = size > (0xFFFF + 22) ? size - (0xFFFF + 22) : 0;
  std::size_t eocd = std::string::npos;
  for (std::size_t i = size - 22 + 1; i-- > search_start;) {
    if (data[i] == 0x50 && data[i + 1] == 0x4b && data[i + 2] == 0x05 && data[i + 3] == 0x06) {
      eocd = i;
      break;
    }
  }
  if (eocd == std::string::npos) return false;

  if (HasZip64EndOfCentralDirectory(data, size)) return false;

  const std::uint16_t entry_count = ZipReadU16(data, eocd + 10);
  const std::uint32_t cd_offset = ZipReadU32(data, eocd + 16);
  if (cd_offset >= size) return false;

  std::size_t cursor = cd_offset;
  for (std::uint16_t i = 0; i < entry_count; ++i) {
    if (cursor + 46 > size) return false;
    if (ZipReadU32(data, cursor) != 0x02014b50) return false;
    ZipEntryRecord record;
    record.method = ZipReadU16(data, cursor + 10);
    record.crc = ZipReadU32(data, cursor + 16);
    record.compressed_size = ZipReadU32(data, cursor + 20);
    record.uncompressed_size = ZipReadU32(data, cursor + 24);
    const std::uint16_t name_len = ZipReadU16(data, cursor + 28);
    const std::uint16_t extra_len = ZipReadU16(data, cursor + 30);
    const std::uint16_t comment_len = ZipReadU16(data, cursor + 32);
    record.local_offset = ZipReadU32(data, cursor + 42);
    if (cursor + 46 + name_len > size) return false;
    record.name.assign(reinterpret_cast<const char *>(data + cursor + 46), name_len);
    for (char &c : record.name) {
      if (c == '\\') c = '/';
    }
    record.is_directory = !record.name.empty() && record.name.back() == '/';
    out.push_back(record);
    cursor += 46 + name_len + extra_len + comment_len;
  }
  return true;
}

/**
 * @brief Inflates one entry. Method 0 is stored, 8 is deflate; anything else is unsupported.
 *
 * <p>Verifies the entry's stored CRC32 against the CRC of the extracted bytes before returning, so a
 * corrupted entry is refused rather than written as the mod (audit item 12.5).
 */
inline bool ExtractEntryData(const std::uint8_t *data, std::size_t size,
                             const ZipEntryRecord &entry, std::vector<std::uint8_t> &out) {
  if (data == nullptr) return false;
  if (static_cast<std::size_t>(entry.local_offset) + 30 > size) return false;
  if (ZipReadU32(data, entry.local_offset) != 0x04034b50) return false;
  const std::uint16_t name_len = ZipReadU16(data, entry.local_offset + 26);
  const std::uint16_t extra_len = ZipReadU16(data, entry.local_offset + 28);
  const std::size_t start =
      static_cast<std::size_t>(entry.local_offset) + 30 + name_len + extra_len;
  if (start + entry.compressed_size > size) return false;

  if (entry.method == 0) {
    if (entry.compressed_size > kMaxUncompressedBytes) return false;
    out.assign(data + start, data + start + entry.compressed_size);
    const uLong computed = ::crc32(0L, out.data(), static_cast<uInt>(out.size()));
    return ZipCrcMatches(entry.crc, static_cast<std::uint32_t>(computed));
  }
  if (entry.method != 8) return false;

  // Bound the allocation by the entry's *declared* size before inflating into it: the declared size
  // is attacker-controlled and drives the resize, so an unchecked one is a zip bomb.
  if (entry.uncompressed_size > kMaxUncompressedBytes) return false;
  out.resize(entry.uncompressed_size);
  z_stream stream{};
  stream.next_in = const_cast<Bytef *>(data + start);
  stream.avail_in = entry.compressed_size;
  stream.next_out = out.data();
  stream.avail_out = entry.uncompressed_size;
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
  const int status = inflate(&stream, Z_FINISH);
  inflateEnd(&stream);
  if (status != Z_STREAM_END) return false;
  out.resize(stream.total_out);
  const uLong computed = ::crc32(0L, out.data(), static_cast<uInt>(out.size()));
  return ZipCrcMatches(entry.crc, static_cast<std::uint32_t>(computed));
}

} // namespace pl::runtime
