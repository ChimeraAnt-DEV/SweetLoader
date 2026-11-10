#pragma once

/**
 * @file SignatureCachePolicy.hpp
 * @brief The bounded-cache and negative-cache rules for the signature resolver.
 *
 * <p>Two caches back the resolver, and both had a defect (audit items 4.5 and 4.6):
 *
 *  - the parsed-pattern cache grew without bound for the life of the process, and
 *  - a pattern known to be absent was re-derived by a full scan on every resolve, because the
 *    resolver deliberately refuses to memoize a miss (a miss is often transient: resolution can run
 *    before the module is fully mapped).
 *
 * <p>The second point is why the negative cache is not simply "cache every zero". A miss is only
 * safe to remember when the scan actually ran over a fully-readable module and still found nothing:
 * that is a property of the build, and the build does not change within a session. A miss from a
 * module that had no readable regions, or from a resolve that never reached the scan, is transient
 * and must not be cached. The rule is pure, so it is host-tested.
 */

#include <cstddef>
#include <cstdint>

namespace pl::memory {

/** The maximum number of parsed patterns held in memory. Beyond this, the oldest are evicted. */
inline constexpr std::size_t kPatternCacheCap = 1024;

/**
 * @brief Whether the parsed-pattern cache has outgrown its cap and should be trimmed.
 *
 * <p>Trimming happens only past the cap, so the cost is amortized over many inserts rather than paid
 * on every insert.
 */
inline bool PatternCacheNeedsTrim(std::size_t size, std::size_t cap = kPatternCacheCap) noexcept {
  return size > cap;
}

/**
 * @brief Whether a resolve miss can be remembered as "absent in this build" for the session.
 *
 * @param moduleHadReadableRegions the module had at least one readable region
 * @param scanRan                  the resolve reached the byte-pattern scan (not skipped for a
 *                                 missing region, an unreadable section, or a resolution that
 *                                 failed earlier)
 * @param resolvedAddress          the address the resolve produced; a non-zero value is a hit, not
 *                                 a miss
 * @return true only when the scan ran over a readable module and still found nothing
 */
inline bool MissIsCacheable(bool moduleHadReadableRegions, bool scanRan,
                            std::uintptr_t resolvedAddress) noexcept {
  return resolvedAddress == 0 && moduleHadReadableRegions && scanRan;
}

} // namespace pl::memory
