#pragma once

/**
 * @file universe_api.h
 * @brief The C ABI a `.universe` native mod links against.
 *
 * <p>A `.universe` package carries one or more native libraries (`native_libs`) that the loader
 * `dlopen`s and calls three exported entry points on:
 *
 * ```c
 * int32_t universe_init(const universe_host_t *host);   // called once, after dlopen
 * void    universe_tick(uint64_t frame, float dt);      // called once per frame
 * void    universe_shutdown(void);                      // called before dlclose
 * ```
 *
 * <p>The `host` table is the loader's whole surface. It is passed *by pointer* rather than linked,
 * because the loader resolves it from the mod's own package: a mod never links `libsweetloader.so`
 * and so cannot be broken by a symbol change, and the loader can hand a sandboxed mod a reduced
 * table that omits what the mod did not declare permission for.
 *
 * <p><b>ABI.</b> This header is the contract. `host->size` and `host->abi_version` are checked by
 * the mod's `universe_init` against the numbers it was built with, so a table from a newer or older
 * loader is detected by a number rather than by calling through a moved field. Append new fields at
 * the end and bump `UNIVERSE_HOST_ABI_VERSION`; never reorder or remove one.
 *
 * <p><b>Fail-closed.</b> Every function pointer may be null when the loader refused that capability
 * (the mod did not declare the permission, or the loader could not provide it on this build). A mod
 * must null-check before calling, and treat null as "not available", never as "call anyway".
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The ABI version of the host table. Bump on any change to the layout below. */
#define UNIVERSE_HOST_ABI_VERSION 1u

/** A hook callback: the frame the hook fired on, and an opaque context the mod registered. */
typedef void (*universe_hook_fn)(uint64_t frame, void *context);

/**
 * @brief The loader's host table.
 *
 * <p>Field order is the ABI. A mod reads `size`/`abi_version` first and refuses to run when they do
 * not match what it was built against.
 */
typedef struct universe_host {
  /** `sizeof(universe_host_t)` as the loader built it. */
  uint32_t size;
  /** `UNIVERSE_HOST_ABI_VERSION` as the loader built it. */
  uint32_t abi_version;

  /**
   * @brief Registers a hook on a byte signature.
   *
   * @param signature  The rule-file signature (`..` wildcards), e.g. `"f0 4f 02 a9 .. .. 00 94"`.
   * @param module     The module basename to scan, or null for `libminecraftpe.so`.
   * @param callback   Called once per frame the hooked function runs.
   * @param context    Passed back to `callback`; the mod owns it.
   * @return A non-zero handle on success, 0 on failure (unresolved signature, no permission, or the
   *         hook limit is reached). A 0 handle is "not installed", never a crash.
   */
  uint64_t (*register_hook)(const char *signature,
                            const char *module,
                            universe_hook_fn callback,
                            void *context);

  /** @brief Removes a hook by the handle `register_hook` returned. A no-op for an unknown handle. */
  void (*unregister_hook)(uint64_t handle);

  /**
   * @brief Resolves a byte signature to an address, without hooking it.
   *
   * @return The address, or 0 when the signature did not resolve (or the mod lacks `read_memory`).
   */
  uintptr_t (*resolve_signature)(const char *signature, const char *module);

  /**
   * @brief Reads `length` bytes from `address` into `out`.
   *
   * @return true on success. Returns false, and writes nothing, when `address` is not readable or
   *         the mod lacks `read_memory` — it never faults the game thread.
   */
  bool (*read_memory)(uintptr_t address, void *out, size_t length);

  /**
   * @brief Writes `length` bytes to `address`.
   *
   * @return true on success. Requires the `write_memory` permission, which the loader grants only to
   *         a trusted-signature mod; otherwise it returns false without writing.
   */
  bool (*write_memory)(uintptr_t address, const void *data, size_t length);

  /**
   * @brief The local player's position and view rotation, when the world feed is bound.
   *
   * @param out_position  Receives `{x, y, z}`.
   * @param out_rotation  Receives `{yaw, pitch}`.
   * @return true when both were read. False means "no data this frame" (no world, seam unbound),
   *         not a zero pose.
   */
  bool (*get_local_player)(float out_position[3], float out_rotation[2]);

  /** @brief The BerryAPI version string the loader is talking to, or null when BerryAPI is absent. */
  const char *(*berry_api_version)(void);

  /** @brief Logs a line to the loader's log (the client's Logcat, tag `SweetLoader`). */
  void (*log)(int level, const char *message);
} universe_host_t;

/** Log levels for `universe_host_t::log`. */
enum {
  UNIVERSE_LOG_DEBUG = 0,
  UNIVERSE_LOG_INFO = 1,
  UNIVERSE_LOG_WARN = 2,
  UNIVERSE_LOG_ERROR = 3,
};

/**
 * @brief The mod's entry point, called once after `dlopen`.
 *
 * @param host  The loader's table. The mod must check `host->size >= sizeof(universe_host_t)` and
 *              `host->abi_version == UNIVERSE_HOST_ABI_VERSION` before using it.
 * @return 0 on success. A non-zero return, or a panic inside the call, makes the loader unload the
 *         mod and (after repeated failures) blacklist it. Returning non-zero is the safe way to
 *         refuse to run on an ABI the mod does not understand.
 */
int32_t universe_init(const universe_host_t *host);

/** @brief Called once per frame. Must not block the game thread. */
void universe_tick(uint64_t frame, float dt);

/** @brief Called before `dlclose`. Free the mod's own resources; the loader frees the hooks. */
void universe_shutdown(void);

#ifdef __cplusplus
} // extern "C"
#endif
