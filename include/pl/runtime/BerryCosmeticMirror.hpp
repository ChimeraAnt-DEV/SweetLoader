#pragma once

/**
 * @file BerryCosmeticMirror.hpp
 * @brief The preloader's mirror of BerryAPI's cosmetic C ABI, as a host-testable struct.
 *
 * <p>BerryAPI exports the cosmetics system as a plain C struct of function pointers
 * (`berry/cosmetic_c.h`, `BERRY_GetCosmeticApi`). The preloader `dlopen`s the library and cannot
 * include its headers, so it mirrors the struct and checks the numbers before calling anything —
 * the same contract `BerryReplayMirror.hpp` uses for the replay and `BerrySeamProvider.hpp` for the
 * world seam.
 *
 * <p>Every field is mirrored, in the library's order, because the host reads the whole struct from
 * the library and a field added, removed or reordered here would shift every pointer after it. The
 * `size`/`abi_version` pair the library reports is checked against what this layout produces, so a
 * drift is refused by a number rather than read through as a mis-typed function pointer.
 *
 * <p>Pure C++ (no Android, no dlopen), so the mirror check, the catalog marshalling and the bone
 * reader marshalling are host-tested without a device.
 */

#include <cstddef>
#include <cstdint>

namespace pl::berry {

/** The status codes, mirroring `BerryCosmeticStatus`. */
enum class CosmeticStatus : std::int32_t {
    Ok = 0,
    NotAttached = 1,
    BadArgument = 2,
    UnknownAsset = 3,
    NotOwned = 4,
    BadSlot = 5,
    BadFormat = 6,
    UnknownPreset = 7,
    BufferTooSmall = 8,
    NoHookBackend = 9,
    HookRefused = 10,
    AlreadyHooked = 11,
    BadReader = 12,
};

/** The cosmetic slots, mirroring `BerryCosmeticSlotC`. The values are the ABI. */
enum class CosmeticSlot : std::int32_t {
    None = 0,
    Head = 1,
    Back = 2,
    Shoulders = 3,
    Body = 4,
    Trail = 5,
    Effect = 6,
    Emote = 7,
    Count = 8,
};

/** The longest asset id a record carries, including the NUL. Mirrors `BERRY_COSMETIC_MAX_ID`. */
constexpr std::size_t kCosmeticMaxId = 128;
/** The longest custom-animation clip name, including the NUL. */
constexpr std::size_t kCosmeticMaxAnimation = 64;

/** One registry asset, matching `BerryCosmeticAssetC` field for field. */
struct CosmeticAssetMirror {
    char id[kCosmeticMaxId];
    std::int32_t slot;
    std::int32_t bone;
    std::uint64_t geometry;
    std::uint64_t texture;
    float offset_x;
    float offset_y;
    float offset_z;
    float rotation_pitch;
    float rotation_yaw;
    float rotation_roll;
    float scale;
    float tint_r;
    float tint_g;
    float tint_b;
    float trail;
    char animation[kCosmeticMaxAnimation];
    float animation_speed;
    char pack[kCosmeticMaxId];
    char author[kCosmeticMaxId];
};

/** The service counters, matching `BerryCosmeticStatsC` field for field. */
struct CosmeticStatsMirror {
    std::uint64_t frames;
    std::uint64_t submitted;
    std::uint64_t skipped_no_bone;
    std::uint64_t skipped_not_owned;
    std::uint64_t cache_hits;
    std::uint64_t cache_misses;
    std::uint32_t registry_count;
    std::uint32_t preset_count;
};

/** A bone's world transform, matching `BerryCosmeticBoneTransformC` field for field. */
struct CosmeticBoneTransformMirror {
    float px;
    float py;
    float pz;
    float pitch;
    float yaw;
    float roll;
    float vx;
    float vy;
    float vz;
    float scale;
    std::int32_t valid;
};

/**
 * @brief The per-build bone accessors the host fills, matching `BerryPlayerModelBoneReaderC`.
 *
 * <p>Both callbacks are optional: a null `bone_transform` makes the source resolve nothing (fail
 * closed), and a null `pose_revision` disables the resolve cache (correctness over speed).
 */
struct PlayerModelBoneReaderMirror {
    std::uint32_t size;
    std::uint32_t abi_version;
    void *user;
    std::int32_t (*bone_transform)(std::uint64_t raw_id, std::uint16_t runtime_id,
                                   std::int32_t bone, CosmeticBoneTransformMirror *out,
                                   void *user);
    std::int32_t (*pose_revision)(std::uint64_t raw_id, std::uint16_t runtime_id,
                                  std::uint64_t *out, void *user);
};

/**
 * @brief The cosmetic surface, matching `BerryCosmeticApi` field for field and in order.
 *
 * <p>The order is the ABI. Every pointer is optional; a null one means this build does not provide
 * that call, and the caller must not invoke it.
 */
struct CosmeticApiMirror {
    std::uint32_t size;
    std::uint32_t abi_version;

    std::uint32_t (*reload_packs)(const char *pack_dir, const char *asset_dir);
    std::int32_t (*list)(CosmeticAssetMirror *out, std::size_t cap, std::size_t *count);
    std::uint32_t (*registry_count)(void);

    std::int32_t (*equip)(const char *asset_id);
    std::int32_t (*unequip)(std::int32_t slot);
    std::int32_t (*equipped)(std::int32_t slot, char *out, std::size_t cap);
    std::int32_t (*clear_loadout)(void);
    std::int32_t (*loadout_canonical)(char *out, std::size_t cap, std::size_t *out_len);
    std::int32_t (*set_loadout_canonical)(const char *text);

    std::int32_t (*save_preset)(const char *name);
    std::int32_t (*apply_preset)(const char *name);
    std::int32_t (*has_preset)(const char *name);
    std::int32_t (*preset_names)(char *names, std::size_t name_cap, std::size_t cap,
                                 std::size_t *count);

    std::int32_t (*set_local_only)(std::int32_t local_only);
    std::int32_t (*local_only)(void);

    void (*stats)(CosmeticStatsMirror *out);

    std::int32_t (*set_player_model_hook)(void *target,
                                          const PlayerModelBoneReaderMirror *reader);
    void (*clear_player_model_hook)(void);
    std::int32_t (*has_player_model_hook)(void);
};

/** The `size`/`abi_version` a host reads from BerryAPI before it uses the surface. */
struct CosmeticAbi {
    std::uint32_t struct_size;
    std::uint32_t abi_version;
};

/**
 * @brief The cosmetic ABI version this mirror targets.
 *
 * <p>Must equal `BERRY_COSMETIC_ABI_VERSION` in the library. `CosmeticMirrorMatches` compares the
 * two, so a bump on either side is caught before a pointer is called.
 */
constexpr std::uint32_t kHostCosmeticAbiVersion = 1;

/**
 * @brief The `sizeof` values this layout produces, pinned at compile time.
 *
 * <p>BerryAPI refuses a surface whose `size` is not exactly its own `sizeof`, and refuses a reader
 * whose `size` is not exactly its own. Pinning both here turns a field added, removed or reordered
 * in the mirror into a compile error instead of a surface that is silently refused.
 */
static_assert(sizeof(CosmeticAssetMirror) == 520,
              "CosmeticAssetMirror must match BerryAPI's BerryCosmeticAssetC layout");
static_assert(sizeof(CosmeticStatsMirror) == 56,
              "CosmeticStatsMirror must match BerryAPI's BerryCosmeticStatsC layout");
static_assert(sizeof(CosmeticBoneTransformMirror) == 44,
              "CosmeticBoneTransformMirror must match BerryAPI's BerryCosmeticBoneTransformC layout");
static_assert(sizeof(PlayerModelBoneReaderMirror) == 32,
              "PlayerModelBoneReaderMirror must match BerryAPI's BerryPlayerModelBoneReaderC layout");
static_assert(sizeof(CosmeticApiMirror) == 160,
              "CosmeticApiMirror must match BerryAPI's BerryCosmeticApi (ABI 1) layout");

/**
 * @brief Whether the host's mirrored struct matches the library's.
 *
 * <p>A zero on either side means the query did not resolve, which is a mismatch: using a surface of
 * unknown ABI is exactly what this check exists to prevent.
 */
inline bool CosmeticMirrorMatches(const CosmeticAbi &reported, std::uint32_t hostSize,
                                  std::uint32_t hostAbi) {
    if (reported.struct_size == 0 || reported.abi_version == 0) return false;
    return reported.struct_size == hostSize && reported.abi_version == hostAbi;
}

/**
 * @brief Whether a reader the host fills is well-formed enough to hand the library.
 *
 * <p>The library re-checks this, but a host-side check keeps a bad reader from crossing the ABI at
 * all: a size or ABI mismatch, or a null `bone_transform`, means the source would resolve nothing,
 * so the hook is refused by name rather than installed as a no-op.
 */
inline bool CosmeticReaderUsable(const PlayerModelBoneReaderMirror &reader) {
    return reader.size == sizeof(PlayerModelBoneReaderMirror) &&
           reader.abi_version == kHostCosmeticAbiVersion && reader.bone_transform != nullptr;
}

/**
 * @brief Whether a surface has every pointer the launcher's cosmetics screen uses.
 *
 * <p>The launcher lists the catalog, equips and unequips, reads what is equipped, clears the
 * loadout, saves/applies/lists presets, gates remote rendering, reads stats and installs the
 * player-model hook. A library that omits any of these is reported as unusable rather than driven
 * through a null. The canonical-loadout pair is optional — the screen drives slots directly — so it
 * is deliberately not required.
 */
inline bool CosmeticApiUsable(const CosmeticApiMirror &api) {
    return api.list != nullptr && api.registry_count != nullptr && api.equip != nullptr &&
           api.unequip != nullptr && api.equipped != nullptr && api.clear_loadout != nullptr &&
           api.save_preset != nullptr && api.apply_preset != nullptr && api.has_preset != nullptr &&
           api.preset_names != nullptr && api.set_local_only != nullptr &&
           api.local_only != nullptr && api.stats != nullptr &&
           api.set_player_model_hook != nullptr && api.clear_player_model_hook != nullptr &&
           api.has_player_model_hook != nullptr;
}

/** The slot enum value as a `CosmeticSlot`, clamped to a known value. */
inline CosmeticSlot CosmeticSlotFromInt(std::int32_t slot) {
    if (slot < 0 || slot >= static_cast<std::int32_t>(CosmeticSlot::Count)) return CosmeticSlot::None;
    return static_cast<CosmeticSlot>(slot);
}

} // namespace pl::berry
