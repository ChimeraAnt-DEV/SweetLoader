#pragma once

/**
 * @file AntEggZip.hpp
 * @brief The ZIP rules the .antegg reader enforces, as pure functions.
 *
 * <p>Two gaps in the reader are fixed here (audit items 12.3 and 12.5):
 *
 *  - the entry's stored CRC32 was never checked, so a corrupted entry inflated to garbage and was
 *    written as if it were the mod; and
 *  - a ZIP64 archive (which uses a different end-of-central-directory record and 64-bit fields) was
 *    parsed as if it were a plain ZIP, silently reading the wrong offsets.
 *
 * <p>Both rules are pure, so they are host-tested without a real archive.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pl::runtime {

/**
 * @brief Whether an entry's stored CRC32 matches the CRC32 computed over its extracted bytes.
 *
 * <p>A mismatch means the archive is corrupt or was tampered with, and the extracted bytes must not
 * be used. The one benign case is a stored CRC of zero with zero bytes extracted: an empty file has
 * CRC32 zero, so that is a match rather than a corruption.
 */
inline bool ZipCrcMatches(std::uint32_t storedCrc, std::uint32_t computedCrc) noexcept {
  return storedCrc == computedCrc;
}

/** @brief The signature of the ZIP64 end-of-central-directory locator. */
inline constexpr std::uint32_t kZip64EocdLocatorSignature = 0x07064b50u;

/** @brief The signature of the ZIP64 end-of-central-directory record. */
inline constexpr std::uint32_t kZip64EocdSignature = 0x06064b50u;

/**
 * @brief Whether the buffer carries a ZIP64 end-of-central-directory locator.
 *
 * <p>A ZIP64 archive stores 64-bit entry counts and offsets in a record the plain parser does not
 * read, so it would be parsed as a plain ZIP and the offsets read as the low 32 bits of a larger
 * value. The locator sits 20 bytes before the plain end-of-central-directory record, and this scan
 * finds it anywhere in the tail; the caller refuses a ZIP64 archive rather than mis-parsing it.
 */
inline bool HasZip64EndOfCentralDirectory(const std::uint8_t *data, std::size_t size) noexcept {
  if (data == nullptr || size < 4) return false;
  // The locator is 20 bytes and precedes the EOCD, which is itself at most 22 bytes plus a 64 KB
  // comment. A scan over the last 64 KB + 20 bytes is enough and bounded.
  const std::size_t window = size > (0xFFFFu + 20u) ? (0xFFFFu + 20u) : size;
  const std::size_t start = size - window;
  for (std::size_t i = start; i + 4 <= size; ++i) {
    std::uint32_t value = 0;
    std::memcpy(&value, data + i, 4);
    if (value == kZip64EocdLocatorSignature || value == kZip64EocdSignature) return true;
  }
  return false;
}

} // namespace pl::runtime
