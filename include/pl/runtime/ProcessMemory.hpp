#pragma once

/**
 * @file ProcessMemory.hpp
 * @brief Reads this process's own memory footprint from `/proc`.
 *
 * A session's growth is the cheapest leak signal the launcher can show. A hook or cache
 * that retains per-world state shows up as a footprint that climbs and never returns, and without
 * a reading that is invisible until the device is out of memory.
 *
 * Two sources, in preference order:
 *  - `/proc/self/smaps_rollup` `Pss:` (proportional set size, the honest per-process share of
 *    shared pages) when the kernel exposes it;
 *  - `/proc/self/statm` resident pages as the fallback, which counts shared pages in full.
 *
 * Pure C++ (no Android types) so the parse rules are host-testable.
 */

#include <cstddef>
#include <cstdint>

namespace pl::runtime {

/** A memory reading, in kibibytes. Zero means "not available", never "no memory". */
struct ProcessMemoryReading {
  std::uint64_t residentKb = 0;
  std::uint64_t proportionalKb = 0;
  /** True when the value came from `smaps_rollup`'s Pss rather than `statm`'s RSS. */
  bool hasProportional = false;
};

/**
 * @brief Parses the `Pss:` line of an `smaps_rollup` document.
 *
 * @param text the document's contents
 * @param outKb the parsed Pss in KiB; untouched when no line matches
 * @return true when a Pss line was found and parsed
 */
bool ParseSmapsRollupPss(const char *text, std::size_t length, std::uint64_t &outKb);

/**
 * @brief Converts a `statm` resident-page count to kibibytes.
 *
 * `statm`'s second field is resident pages. A zero page count, a non-numeric field, or a
 * `pageSizeKb` of zero reads as "not available" (0) rather than a fabricated size.
 */
std::uint64_t ResidentPagesToKb(std::uint64_t residentPages, std::uint64_t pageSizeKb);

/**
 * @brief Whether a cached reading taken at @p lastMs is still fresh at @p nowMs.
 *
 * <p>A reading older than @p ttlMs is stale. A clock that has gone backwards (a negative delta) is
 * treated as fresh, so a wrap does not force a re-read on every call.
 */
inline bool ProcessMemoryCacheFresh(long long lastMs, long long nowMs, long long ttlMs) noexcept {
  if (lastMs <= 0) return false;
  const long long age = nowMs - lastMs;
  // A negative age (a clock that moved backwards) reads as fresh rather than forcing a re-read on
  // every call until the clock catches up.
  return age < 0 || age < ttlMs;
}

/**
 * @brief Reads the current footprint, memoized for a short TTL.
 *
 * <p>Reading `/proc/self/smaps_rollup` and `/proc/self/statm` costs 100us+ per call and the launcher
 * polls this from the UI thread, so a reading is reused for {@code kProcessMemoryTtlMs} (1 s) rather
 * than re-read every poll. A missing or unreadable source reads as zero, never an estimate.
 */
ProcessMemoryReading ReadProcessMemory();

/** The TTL for the memoized footprint reading, in milliseconds. */
inline constexpr long long kProcessMemoryTtlMs = 1000;

} // namespace pl::runtime
