#pragma once

/**
 * @file singularity_api.h
 * @brief The C ABI a `.singularity` native pack links against.
 *
 * <p>A `.singularity` pack is a ZIP holding `manifest.json`, `biomes/`, `structures/` and (optionally)
 * `lib/`. The world-generation engine itself lives in the loader's Rust core; a native library is
 * only needed when a pack wants to run *its own* code inside the generation loop (a custom terrain
 * shaper, a procedural structure builder). Such a library exports the same three entry points a
 * `.universe` mod does, but receives this table:
 *
 * ```c
 * int32_t universe_init(const singularity_host_t *host);
 * void    universe_tick(uint64_t frame, float dt);
 * void    universe_shutdown(void);
 * ```
 *
 * <p><b>ABI.</b> `size`/`abi_version` are checked first, exactly as in `universe_api.h`. Append
 * fields and bump `SINGULARITY_HOST_ABI_VERSION`; never reorder or remove.
 *
 * <p><b>Determinism.</b> Everything the engine calls during generation is a pure function of the
 * seed and the coordinates. A pack's own shaper must be too, or two clients on the same seed will
 * disagree and the world will desync. The engine cannot enforce this, so it is a rule of the
 * format: use `seed_hash` and `noise2`/`noise3` below rather than `rand()` or the clock.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The ABI version of the host table. Bump on any change to the layout below. */
#define SINGULARITY_HOST_ABI_VERSION 1u

/**
 * @brief The world-gen engine's host table.
 */
typedef struct singularity_host {
  /** `sizeof(singularity_host_t)` as the loader built it. */
  uint32_t size;
  /** `SINGULARITY_HOST_ABI_VERSION` as the loader built it. */
  uint32_t abi_version;

  /** @brief The world's 64-bit seed, already hashed from the seed string. */
  uint64_t (*world_seed)(void);

  /**
   * @brief The 3D fractal noise the engine uses for terrain.
   *
   * @return A value in `[0, 1]`. Deterministic for a given `(seed, x, y, z, octaves)`.
   */
  float (*noise3)(uint64_t seed, float x, float y, float z, uint32_t octaves);

  /** @brief The 2D fractal noise (a slice of `noise3`). */
  float (*noise2)(uint64_t seed, float x, float z, uint32_t octaves);

  /**
   * @brief Registers a custom biome.
   *
   * @param id              A safe identifier (letters, digits, `-`, `_`).
   * @param temperature     The `{min, max}` climate range.
   * @param humidity        The `{min, max}` climate range.
   * @param continentalness The `{min, max}` climate range.
   * @return true on success. The biome is refused when the id is unsafe or the table is full.
   */
  bool (*register_biome)(const char *id,
                         const float temperature[2],
                         const float humidity[2],
                         const float continentalness[2]);

  /**
   * @brief Registers a structure placement rule.
   *
   * @param id          A safe identifier.
   * @param spacing     Cell size in chunks (one candidate per cell).
   * @param rarity      Chance in `[0, 1]` that a candidate cell places.
   * @param footprint   The `{width, depth}` footprint in chunks.
   * @return true on success.
   */
  bool (*register_structure)(const char *id, int32_t spacing, float rarity, const int32_t footprint[2]);

  /**
   * @brief Overrides the global gravity.
   *
   * @return true when the engine accepted the override (the pack declared `world`).
   */
  bool (*set_gravity)(float x, float y, float z);

  /**
   * @brief Sets a block's physics material.
   *
   * @param friction     The fraction of horizontal speed lost on landing, in `[0, 1]` (0 is ice).
   * @param restitution  The bounce factor, in `[0, 1]` (1 is a perfect trampoline).
   */
  bool (*set_block_material)(uint32_t block_id, float friction, float restitution);

  /**
   * @brief Reads the block at a world coordinate.
   *
   * @return The block id, or 0 (air) when the coordinate is outside a loaded chunk.
   */
  uint32_t (*get_block)(int32_t x, int32_t y, int32_t z);

  /** @brief Logs a line to the loader's log (Logcat tag `SweetLoader.Singularity`). */
  void (*log)(int level, const char *message);
} singularity_host_t;

/** Log levels for `singularity_host_t::log`. */
enum {
  SINGULARITY_LOG_DEBUG = 0,
  SINGULARITY_LOG_INFO = 1,
  SINGULARITY_LOG_WARN = 2,
  SINGULARITY_LOG_ERROR = 3,
};

/**
 * @brief The pack's entry point, called once after `dlopen`.
 *
 * @return 0 on success. Non-zero (or a panic) makes the loader unload the pack and revert to
 *         vanilla world generation.
 */
int32_t universe_init(const singularity_host_t *host);

/** @brief Called once per frame. Generation work runs on the engine's own threads, not here. */
void universe_tick(uint64_t frame, float dt);

/** @brief Called before `dlclose`. */
void universe_shutdown(void);

#ifdef __cplusplus
} // extern "C"
#endif
