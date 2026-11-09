#pragma once

/**
 * @file BerryReplayMirror.hpp
 * @brief The preloader's mirror of BerryAPI's replay C ABI, as a host-testable struct.
 *
 * <p>BerryAPI exports a replay surface as a plain C struct of function pointers
 * (`berry/replay_c.h`, `BERRY_GetReplayApi`). The preloader `dlopen`s the library and cannot include
 * its headers, so it mirrors the struct and checks the numbers before calling anything — the same
 * contract `BerrySeamProvider.hpp` uses for the world seam.
 *
 * <p>Only the fields the launcher's replay screen needs are mirrored; the struct is laid out in the
 * library's field order, and `size`/`abi_version` are checked against what the library reports
 * (`BERRY_ReplayStructSize`/`BERRY_ReplayAbiVersion`). A drift is refused by a number, never read
 * through as a mis-typed function pointer.
 *
 * <p>Pure C++ (no Android, no dlopen), so the mirror check and the status/playback marshalling are
 * host-tested without a device.
 */

#include <cstddef>
#include <cstdint>

namespace pl::berry {

/** The status codes, mirroring `BerryReplayStatus`. */
enum class ReplayStatus : std::int32_t {
    Ok = 0,
    NotAttached = 1,
    BadReplay = 2,
    BadArgument = 3,
    NotLoaded = 4,
    NotRecording = 5,
};

/** The capture state, mirroring the `capture_state()` return values. */
enum class ReplayCaptureState : std::int32_t {
    Idle = 0,
    Recording = 1,
    Throttled = 2,
    Stopped = 3,
};

/** The loaded replay's summary, matching `BerryReplayInfo` field for field. */
struct ReplayInfoMirror {
    std::int32_t loaded;
    std::uint64_t first_tick;
    std::uint64_t last_tick;
    std::uint32_t section_count;
    std::uint32_t record_count;
    std::uint64_t payload_bytes;
    std::uint32_t build_id;
    std::uint32_t protocol_version;
};

/** The playback state, matching `BerryReplayPlayback` field for field. */
struct ReplayPlaybackMirror {
    std::int32_t active;
    std::int32_t playing;
    std::int32_t active_world;
    std::uint64_t tick;
    float speed;
};

/**
 * @brief The replay surface, matching `BerryReplayApi` field for field and in order.
 *
 * <p>The order is the ABI. Every pointer is optional; a null one means this build does not provide
 * that call, and the caller must not invoke it.
 */
struct ReplayApiMirror {
    std::uint32_t size;
    std::uint32_t abi_version;

    std::int32_t (*load)(const void *bytes, std::size_t len);
    const void *(*save)(std::size_t *out_len);
    void (*unload)(void);
    std::int32_t (*info)(ReplayInfoMirror *out);

    std::int32_t (*play)(void);
    void (*pause)(void);
    void (*stop)(void);
    std::int32_t (*seek)(std::uint64_t tick);
    std::int32_t (*set_speed)(float speed);
    void (*playback)(ReplayPlaybackMirror *out);

    void (*set_local_player)(std::uint64_t entity_id);

    std::int32_t (*loaded)(void);
    const char *(*status)(void);

    // The capture (recorder) half. A host that can see the game's network stream
    // records it here: `capture_start` opens a session, `capture_packet` feeds one
    // datagram, and `save` exports it. Every pointer is optional.
    std::int32_t (*capture_start)(void);
    std::int32_t (*capture_stop)(void);
    std::int32_t (*capture_state)(void);
    std::int32_t (*capture_packet)(std::int32_t dir, std::uint32_t id, const void *payload,
                                   std::size_t len, std::uint64_t tick);
};

/** The `size`/`abi_version` a host reads from BerryAPI before it uses the surface. */
struct ReplayAbi {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
};

/**
 * @brief The replay ABI version this mirror targets.
 *
 * <p>Must equal `BERRY_REPLAY_ABI_VERSION` in the library. `ReplayMirrorMatches` compares the two, so
 * a bump on either side is caught before a pointer is called. v2 grew the capture (recorder) fields
 * (`capture_start`/`capture_stop`/`capture_state`/`capture_packet`), so the mirrored struct is the
 * full v2 layout and a v1 library (a shorter struct) is refused by the size check.
 */
constexpr std::uint32_t kHostReplayAbiVersion = 2;

/**
 * @brief The `sizeof(ReplayApiMirror)` this layout produces, pinned at compile time.
 *
 * <p>BerryAPI refuses a surface whose `size` is not exactly its own `sizeof(BerryReplayApi)`, so the
 * mirror must match field for field. Pinning the number here turns a field added, removed or
 * reordered in the mirror into a compile error instead of a surface that is silently refused.
 */
static_assert(sizeof(ReplayApiMirror) == 144,
              "ReplayApiMirror must match BerryAPI's BerryReplayApi (ABI 2) layout");

/**
 * @brief Whether the host's mirrored struct matches the library's.
 *
 * <p>A zero on either side means the query did not resolve, which is a mismatch: using a surface of
 * unknown ABI is exactly what this check exists to prevent.
 */
inline bool ReplayMirrorMatches(const ReplayAbi &reported, std::uint32_t hostSize,
                                std::uint32_t hostAbi) {
    if (reported.struct_size == 0 || reported.abi_version == 0) return false;
    return reported.struct_size == hostSize && reported.abi_version == hostAbi;
}

/**
 * @brief Whether a surface has every pointer the launcher's replay screen uses.
 *
 * <p>The launcher drives load/info/playback/seek/play/unload and reads the status line; a library
 * that omits any of them is reported as unusable rather than driven with a null call. `save`,
 * `pause`, `stop` and `set_local_player` are optional — the screen does not need them — so they are
 * deliberately not required.
 */
inline bool ReplayApiUsable(const ReplayApiMirror &api) {
    return api.load != nullptr && api.info != nullptr && api.playback != nullptr &&
           api.seek != nullptr && api.play != nullptr && api.unload != nullptr &&
           api.loaded != nullptr;
}

/**
 * @brief Whether the surface can record the live stream (the capture half).
 *
 * <p>Separate from {@link ReplayApiUsable}: a build can play replays without being able to record
 * one, so the launcher shows "record" only when every capture call is present rather than calling a
 * null `capture_packet`.
 */
inline bool ReplayCaptureUsable(const ReplayApiMirror &api) {
    return api.capture_start != nullptr && api.capture_stop != nullptr &&
           api.capture_state != nullptr && api.capture_packet != nullptr;
}

/** The tick length of a loaded replay, or 0 when empty. */
inline std::uint64_t ReplayTickLength(const ReplayInfoMirror &info) {
    if (info.loaded == 0) return 0;
    return info.last_tick >= info.first_tick ? info.last_tick - info.first_tick : 0;
}

} // namespace pl::berry
