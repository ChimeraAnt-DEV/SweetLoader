#pragma once

/**
 * @file BerryApiBridge.h
 * @brief Loads the bundled BerryAPI native runtime and reports what it can do.
 *
 * <p>BerryAPI (`libBerryAPI.so`, https://github.com/Lynxifyy/BerryAPI) is a Bedrock client runtime:
 * it installs its own hook backend, drives a per-frame render seam and owns the voice, cosmetics,
 * replay and script subsystems. It is a *separate* library with its own version, so the launcher
 * never links it at build time — this bridge `dlopen`s it and talks to it through the small C ABI
 * BerryAPI exports (`BERRY_Inject`/`BERRY_Detach`/`BERRY_IsAttached`/`BERRY_Version`).
 *
 * <p><b>Fail-closed.</b> Every entry point is resolved by name; a missing library, a missing symbol
 * or a version this build does not understand leaves {@link Available} false and every capability
 * false. Nothing here dereferences a guessed address or claims a subsystem it cannot confirm. When
 * the library is present, its ELF constructor auto-attaches it, so {@link Attached} reflects a live
 * hook backend rather than a promise.
 *
 * <p><b>What this does not do.</b> BerryAPI's World and Combat modules read game state through a
 * `LevelSeam` the embedder supplies; BerryAPI itself ships no Bedrock signature table (see its
 * `ResolveCoreSignatures`, which deliberately closes the table until a module registers one). So
 * {@link Capability::WorldFeed} is false until a seam is bound from a signature pack, and the
 * modules that need an entity feed stay honestly unavailable rather than reporting a reading that
 * was never taken. This bridge is the reporting half; binding the seam is a separate, per-build
 * step.
 */

#include <cstdint>
#include <string>

#include "pl/Export.hpp"

namespace pl::berry {

/**
 * @brief A capability the client can grey a feature on.
 *
 * <p>Only bits this bridge can actually confirm are ever set. `WorldFeed` is present so the client
 * has one place to ask, and it stays clear until an embedder seam is bound.
 */
enum class Capability : std::uint32_t {
  None = 0,
  /** The bundled `libBerryAPI.so` loaded and its C ABI resolved. */
  LibraryLoaded = 1u << 0,
  /** BerryAPI reports an attached hook backend (`BERRY_IsAttached`). */
  Attached = 1u << 1,
  /** A `LevelSeam` is bound, so world/entity reads can return real values. Not yet provided. */
  WorldFeed = 1u << 2,
};

/** Bitwise-or for the capability mask. */
PL_EXPORT Capability operator|(Capability a, Capability b);

/** Bitwise-and, so a caller can test one bit. */
PL_EXPORT Capability operator&(Capability a, Capability b);

/** True when every bit in `flag` is set in `mask`. */
PL_EXPORT bool HasCapability(Capability mask, Capability flag);

/**
 * @brief Loads the bundled BerryAPI library once, and reports whether it is usable.
 *
 * <p>Idempotent: the first call `dlopen`s the library and resolves the ABI; every later call
 * returns the cached answer. Safe to call from any thread. The library is never unloaded, because
 * BerryAPI owns threads and hooks once attached and unloading it would tear them down under a
 * running game.
 */
PL_EXPORT bool EnsureLoaded();

/** True when the library loaded and its C ABI resolved. */
PL_EXPORT bool Available();

/** BerryAPI's own version string (`BERRY_Version`), or an empty string when unavailable. */
PL_EXPORT std::string Version();

/**
 * @brief Whether BerryAPI reports a live hook backend.
 *
 * <p>Resolved through `BERRY_IsAttached`. False when the library is absent or its own attach
 * failed (for example, no usable hook backend on the device).
 */
PL_EXPORT bool Attached();

/**
 * @brief Attaches BerryAPI if it is not already attached, at most once per process.
 *
 * <p>Loads the library if needed, then calls `BERRY_Inject`. Idempotent: a second call after a
 * successful attach is a cheap query, and a second call after a failure does not retry (a retry
 * would install a half-built hook set). Returns whether BerryAPI now reports an attached hook
 * backend. Safe to call from the launcher's session start; a failure is reported, never fatal.
 */
PL_EXPORT bool EnsureAttached();

/**
 * @brief The capability mask, built only from readings that succeeded.
 *
 * <p>`LibraryLoaded` is set once the ABI resolves; `Attached` mirrors {@link Attached}. `WorldFeed`
 * is deliberately never set here: BerryAPI's World module has no seam bound in this build, so the
 * launcher must not present its world/entity features as working.
 */
PL_EXPORT Capability Capabilities();

/**
 * @brief A one-line status for the Diagnostics screen and the About page.
 *
 * <p>Never empty: an unavailable library reports why (for example "BerryAPI: not bundled"), so a
 * missing backend is visible before a launch rather than silently absent.
 */
PL_EXPORT std::string Summary();

} // namespace pl::berry
