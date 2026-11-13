#pragma once

/**
 * @file BerrySeamProvider.hpp
 * @brief The launcher's answer to BerryAPI's world seam, as a host-testable mirror.
 *
 * <p>BerryAPI's World/Combat/Render modules read game state through a `LevelSeam` the embedder
 * supplies. BerryAPI now exposes that seam as a C struct of function pointers plus
 * `BERRY_SetWorldSeam` (see BerryAPI's `berry/seam.h`), so a host that `dlopen`s `libBerryAPI.so`
 * and cannot include its C++ headers — this sweetloader — can still bind one.
 *
 * <p><b>What this build can honestly answer.</b> The sweetloader already reads the local player's
 * position and view rotation from the `ClientInstance` vtable slot the game itself returns (see
 * `GameLocalPlayer`). It cannot enumerate other entities, read a block, or read the level time:
 * the game strips its symbols, so there is no anchor for those, and inventing one would be the
 * over-claim the rest of this module is built to avoid. So the provider fills the *local player*
 * callbacks and leaves the rest null, which BerryAPI documents as the fail-closed default.
 *
 * <p>The struct layout is mirrored, not included: the two libraries are built separately, so the
 * only shared contract is the numbers. {@link MirrorMatches} compares the host's own
 * `sizeof`/ABI against the values BerryAPI reports before anything is bound, so a layout drift is
 * caught by a number rather than by a misread function pointer.
 *
 * <p>Pure C++ (no Android, no dlopen), so the mirror check and the local-player marshalling are
 * host-tested without a device.
 */

#include <cstdint>

namespace pl::berry {

/** One entity, matching BerryAPI's `BerryEntitySnapshotC` field for field. */
struct EntitySnapshotMirror {
    std::uint64_t raw_id;
    std::uint16_t runtime_id;
    std::uint32_t type;
    float px, py, pz;
    float vx, vy, vz;
    float pitch, yaw, roll;
    float health, max_health;
    std::int32_t alive;
    std::int32_t on_ground;
    const char *name;
    std::uint64_t last_seen_tick;
};

/** The local player's own state, matching BerryAPI's `BerryLocalPlayerC` field for field. */
struct LocalPlayerMirror {
    std::int32_t valid;
    std::int32_t hunger;
    float saturation;
    std::int32_t armor_points;
    std::int32_t gamemode;
    std::int32_t dimension;
    std::int32_t xp_level;
    float xp_progress;
    float air;
    std::int32_t on_fire;
    std::int32_t sneaking;
    std::int32_t sprinting;
    float px, py, pz;
    float pitch, yaw, roll;
};

/**
 * @brief The seam the sweetloader binds, matching BerryAPI's `BerrySeamC` field for field.
 *
 * <p>Only `get_local_player` is ever set; every other slot stays null so the matching read fails
 * closed. `size` and `abi_version` are stamped from the values BerryAPI reports, so the bind can
 * never be attempted with a stale layout.
 */
struct SeamMirror {
    std::uint32_t size;
    std::uint32_t abi_version;
    void *user;

    std::int32_t (*get_entity)(std::uint64_t, EntitySnapshotMirror *, void *);
    std::int32_t (*all_entities)(std::uint64_t *, std::size_t, std::size_t *, void *);
    std::int32_t (*entities_in_radius)(float, float, float, float, std::uint64_t *, std::size_t,
                                       std::size_t *, void *);
    std::int32_t (*entities_in_aabb)(float, float, float, float, float, float, std::uint32_t,
                                     std::uint64_t *, std::size_t, std::size_t *, void *);
    std::int32_t (*raycast_entity)(float, float, float, float, float, float, float, std::uint64_t *,
                                   float *, void *);
    std::int32_t (*raycast_block)(float, float, float, float, float, float, float, float *, float *,
                                  float *, std::uint32_t *, void *);
    std::uint32_t (*get_block)(std::int32_t, std::int32_t, std::int32_t, void *);
    std::int32_t (*get_time)(void *, void *);
    std::int32_t (*get_biome)(std::int32_t, std::int32_t, std::int32_t, char *, std::size_t, void *);
    std::int32_t (*get_light)(std::int32_t, std::int32_t, std::int32_t, void *);
    std::int32_t (*get_chunk_state)(std::int32_t, std::int32_t, std::int32_t, void *);
    std::int32_t (*get_local_player)(LocalPlayerMirror *, void *);
};

/** The `size`/`abi_version` a host reads from BerryAPI before it binds. */
struct SeamAbi {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
};

/**
 * @brief The BerryAPI seam ABI version this mirror targets.
 *
 * <p>Must equal the `BERRY_ABI_VERSION` BerryAPI was built with. `MirrorMatches` compares the two,
 * so a bump on either side is caught at the bind rather than by a misread function pointer.
 */
constexpr std::uint32_t kHostSeamAbiVersion = 8;

/**
 * @brief The `sizeof(SeamMirror)` this layout produces, pinned at compile time.
 *
 * <p>BerryAPI refuses a seam whose `size` is not exactly its own `sizeof(BerrySeamC)`, so the mirror
 * must match field for field. Pinning the number here turns a field added, removed or reordered in
 * the mirror into a compile error instead of a bind that silently fails on the device.
 */
static_assert(sizeof(SeamMirror) == 112,
              "SeamMirror must match BerryAPI's BerrySeamC (ABI 8) layout");

/**
 * @brief Whether the host's mirrored struct matches the library's.
 *
 * <p>A zero on either side means the query did not resolve, which is treated as a mismatch: binding
 * with an unknown ABI is exactly what this check exists to prevent.
 */
inline bool MirrorMatches(const SeamAbi &reported, std::uint32_t hostSize, std::uint32_t hostAbi) {
    if (reported.struct_size == 0 || reported.abi_version == 0) return false;
    return reported.struct_size == hostSize && reported.abi_version == hostAbi;
}

/**
 * @brief Fills the one slot this build can answer: the local player.
 *
 * <p>Marshals a position/rotation reading into the mirror and sets `valid` only when a reading was
 * actually taken. With no reading (no world loaded, the seam stale) it reports `valid = 0`, which
 * BerryAPI documents as "no data" — never a fabricated stat block. `yaw`/`pitch` are the game's
 * view angles, which is what the caller needs for an aim/heading read.
 *
 * @param haveReading whether {@code GameLocalPlayer} produced a position this call
 * @param position    the position reading (ignored when {@code haveReading} is false)
 * @param rotation    the view rotation reading, {yaw, pitch} (ignored when false)
 * @return the marshalled state; `valid` is set only for a real reading
 */
inline LocalPlayerMirror BuildLocalPlayerState(bool haveReading, const float position[3],
                                               const float rotation[2]) {
    LocalPlayerMirror out{};
    out.valid = 0;
    if (!haveReading) return out;
    out.valid = 1;
    out.px = position[0];
    out.py = position[1];
    out.pz = position[2];
    // The game reports {yaw, pitch}; the struct's rotation block is {pitch, yaw, roll}.
    out.yaw = rotation[0];
    out.pitch = rotation[1];
    out.roll = 0.0f;
    // Hunger, armour, gamemode, dimension, xp and the flags are not reachable in this build — the
    // game strips its symbols and the local-player hook reads only position and view rotation — so
    // they keep their fail-closed defaults rather than being invented.
    return out;
}

/** True when any slot other than `get_local_player` is set — a wiring bug the test pins. */
inline bool HasUnexpectedSlots(const SeamMirror &seam) {
    return seam.get_entity != nullptr || seam.all_entities != nullptr ||
           seam.entities_in_radius != nullptr || seam.entities_in_aabb != nullptr ||
           seam.raycast_entity != nullptr || seam.raycast_block != nullptr ||
           seam.get_block != nullptr || seam.get_time != nullptr || seam.get_biome != nullptr ||
           seam.get_light != nullptr || seam.get_chunk_state != nullptr;
}

} // namespace pl::berry
