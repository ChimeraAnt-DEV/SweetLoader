#pragma once

/**
 * @file SignatureScanCache.hpp
 * @brief Pure serialisation for the on-disk signature scan cache.
 *
 * A byte-pattern scan walks every executable mapping of a module, and `libminecraftpe`'s `.text` is
 * ~220 MB, so it is real work on every launch. The offsets a scan finds are stable for a given
 * binary, so the result is worth keeping across launches.
 *
 * What is stored is the offset **from the module's load base**, never the absolute address: ASLR
 * moves the base on every launch, so an absolute address would be wrong the second time. The file
 * is keyed by the module's ELF build id, which changes on every relink, so a game update cannot read
 * the previous build's offsets.
 *
 * The parse and the format are pure functions over a string, so they are host-testable without the
 * filesystem or a live process; the caller does the file IO.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pl::memory {

/** One resolved signature: its name and its offset from the module's load base. */
struct CachedSignature {
  std::string signature;
  std::uintptr_t offset = 0;
};

/** The header line of a cache file, so a truncated or foreign file is rejected rather than parsed. */
inline constexpr std::string_view kSignatureScanCacheMagic = "chimerasigcache1";

/**
 * @brief Serialises a scan result.
 *
 * Format: the magic line, then `buildId`, then one `offset hex signature` line per entry. A
 * signature that contained a newline cannot be represented and is skipped; the resolver's rule
 * names never do, and skipping is safer than writing a file the parser would misread.
 */
std::string SerializeSignatureScanCache(
    std::string_view buildId, const std::unordered_map<std::string, std::uintptr_t> &entries);

/**
 * @brief Parses a cache file.
 *
 * @param text           the file contents.
 * @param expectedBuildId the module's current build id; a file for a different build is refused, so
 *                        a game update cannot answer from the previous build's offsets.
 * @param out            receives the signature-to-offset map on success. Only non-zero offsets are
 *                       stored, because zero is the resolver's "not found" and re-scanning is
 *                       cheaper than trusting a cached miss.
 * @return true when the file is well-formed and matches the build id.
 */
bool ParseSignatureScanCache(std::string_view text, std::string_view expectedBuildId,
                             std::unordered_map<std::string, std::uintptr_t> &out);

} // namespace pl::memory
