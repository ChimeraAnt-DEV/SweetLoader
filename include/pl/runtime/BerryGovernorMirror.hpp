#pragma once

/**
 * @file BerryGovernorMirror.hpp
 * @brief The preloader's mirror of BerryAPI's performance-governor C ABI, as a host-testable struct.
 *
 * <p>BerryAPI exports its adaptive quality governor as a plain C struct of function pointers
 * (`berry/governor_c.h`, `BERRY_GetGovernorApi`). The preloader `dlopen`s the library and cannot
 * include its headers, so it mirrors the struct and checks the numbers before calling anything — the
 * same contract `BerryReplayMirror.hpp` and `BerrySeamProvider.hpp` use.
 *
 * <p>Pure C++ (no Android, no dlopen), so the mirror check and the reading are host-tested without a
 * device.
 */

#include <cstddef>
#include <cstdint>

namespace pl::berry {

/** The quality knobs, in the library's enum order. The order is the ABI. */
enum class GovernorKnob : std::int32_t {
    Particles = 0,
    EntityDistance = 1,
    AnimationDistance = 2,
    EntityCulling = 3,
    LightingUpdates = 4,
    RenderDistance = 5,
    Count = 6,
};

/** Platform thermal pressure, mirroring `ThermalState`. */
enum class ThermalState : std::int32_t {
    Nominal = 0,
    Fair = 1,
    Serious = 2,
    Critical = 3,
    Shutdown = 4,
};

/** The device class the governor tunes for, mirroring `PlatformClass`. */
enum class PlatformClass : std::int32_t {
    Generic = 0,
    MobileLow = 1,
    MobileHigh = 2,
    Desktop = 3,
};

/** A reading of the governor, matching `BerryGovernorStatus` field for field. */
struct GovernorStatusMirror {
    float quality[static_cast<int>(GovernorKnob::Count)];
    std::int32_t thermal;
    std::int32_t thermally_limited;
    std::int32_t platform;
    float ceiling;
    std::int32_t render_distance_chunks;
    std::int32_t max_particles;
    float entity_distance;
    float animation_distance;
    std::int32_t cull_far_entities;
    std::int32_t lighting_updates_per_tick;
    std::int32_t target_fps;
};

/** The governor surface, matching `BerryGovernorApi` field for field and in order. */
struct GovernorApiMirror {
    std::uint32_t size;
    std::uint32_t abi_version;

    void (*status)(GovernorStatusMirror *out);
    std::int32_t (*set_thermal)(std::int32_t state);
    void (*set_platform)(std::int32_t platform);
    void (*set_ceiling)(float ceiling);
    std::int32_t (*available)(void);
};

/** The `size`/`abi_version` a host reads from BerryAPI before it uses the surface. */
struct GovernorAbi {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
};

/** The governor ABI version this mirror targets. */
constexpr std::uint32_t kHostGovernorAbiVersion = 1;

/**
 * @brief The `sizeof(GovernorApiMirror)` this layout produces, pinned at compile time.
 *
 * <p>BerryAPI refuses a surface whose `size` is not exactly its own `sizeof(BerryGovernorApi)`, so
 * the mirror must match field for field. Pinning the number here turns a field added, removed or
 * reordered in the mirror into a compile error instead of a surface that is silently refused.
 */
static_assert(sizeof(GovernorApiMirror) == 48,
              "GovernorApiMirror must match BerryAPI's BerryGovernorApi (ABI 1) layout");

/**
 * @brief Whether the host's mirrored struct matches the library's.
 *
 * <p>A zero on either side means the query did not resolve, which is a mismatch.
 */
inline bool GovernorMirrorMatches(const GovernorAbi &reported, std::uint32_t hostSize,
                                  std::uint32_t hostAbi) {
    if (reported.struct_size == 0 || reported.abi_version == 0) return false;
    return reported.struct_size == hostSize && reported.abi_version == hostAbi;
}

/** Whether a surface has the calls the launcher reads. `set_thermal`/`set_platform` are optional. */
inline bool GovernorApiUsable(const GovernorApiMirror &api) {
    return api.status != nullptr && api.available != nullptr;
}

/** Whether the platform is actually throttling (Serious or worse). */
inline bool GovernorThrottling(const GovernorStatusMirror &s) {
    return s.thermal == static_cast<std::int32_t>(ThermalState::Serious) ||
           s.thermal == static_cast<std::int32_t>(ThermalState::Critical) ||
           s.thermal == static_cast<std::int32_t>(ThermalState::Shutdown);
}

/** The mean quality across the knobs, 0..1. */
inline float GovernorMeanQuality(const GovernorStatusMirror &s) {
    float sum = 0.0f;
    for (int i = 0; i < static_cast<int>(GovernorKnob::Count); ++i) sum += s.quality[i];
    return sum / static_cast<float>(static_cast<int>(GovernorKnob::Count));
}

} // namespace pl::berry
