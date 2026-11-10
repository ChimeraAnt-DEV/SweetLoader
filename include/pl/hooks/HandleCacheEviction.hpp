#pragma once

/**
 * @file HandleCacheEviction.hpp
 * @brief The pure rule for which cached module handles a maintenance pass may close.
 *
 * <p>{@link pl::hooks::DobbyHookManager} caches one `GlossOpen` handle per module and never closes it.
 * That is right for a session that resolves a handful of modules once — reopening the game module on
 * every resolve would be far worse — but a session that hot-reloads mods, or that resolves many
 * distinct mod `.so`s, grows the cache without bound. A maintenance pass (driven from the launcher's
 * trim-memory callback) advances a generation counter and closes the handles this rule marks stale.
 *
 * <p>Pure (no Gloss, no Android), so the rule is host-tested without a device.
 */

#include <cstdint>

namespace pl::hooks {

/**
 * @brief Whether a cached handle may be closed by a maintenance pass.
 *
 * @param lastTouch    the generation in which the handle was last used
 * @param currentGen   the current generation counter
 * @param keepRecent   how many recent generations to keep (0 keeps only the current generation)
 * @param pinned       whether the handle is pinned and must never be closed (the game module)
 * @return true when the handle is stale and safe to close
 */
inline bool ShouldEvictHandle(std::uint64_t lastTouch, std::uint64_t currentGen,
                              std::uint64_t keepRecent, bool pinned) {
    if (pinned) return false;
    // A current generation below the last touch means the counter wrapped or the clock went
    // backwards; never evict on that, because the arithmetic would underflow.
    if (currentGen < lastTouch) return false;
    // Keep everything touched within the last `keepRecent` generations, inclusive of the current
    // one. With keepRecent == 0 this keeps only the current generation, so a handle touched last
    // generation is evicted.
    return (currentGen - lastTouch) > keepRecent;
}

} // namespace pl::hooks
