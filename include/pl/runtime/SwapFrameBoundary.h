#pragma once

#include <atomic>
#include <cstdint>

#include "pl/Export.hpp"

/**
 * @file SwapFrameBoundary.h
 * @brief The true frame boundary, taken from the engine's present call.
 *
 * <p>The player-render detour runs once per rendered player model, so a frame boundary can only be
 * inferred from it (see `pl::render::FrameTickClock`). That inference is wrong whenever the number of
 * visible models changes, and it cannot see a frame in which nothing is drawn. `eglSwapBuffers` is
 * called exactly once per presented frame, and it is an exported libEGL symbol, so it can be hooked
 * to give the real boundary and the real frame interval.
 *
 * <p><b>Fail-closed.</b> If libEGL cannot be loaded, or neither `eglSwapBuffers` nor
 * `eglSwapBuffersWithDamageKHR` resolves, {@link InstallSwapFrameBoundary} returns false and the
 * preloader keeps using the inferred clock. Nothing here guesses an address.
 *
 * <p>The hook itself is the return-transparent veneer in `RegisterAwareHook.cpp`, so the present
 * call's `EGLBoolean` result and any FP/SIMD argument survive the detour.
 */
/**
 * @brief The frame-boundary counters the swap veneer writes.
 *
 * Declared here (and defined in RegisterAwareHook.cpp beside the veneer) so a consumer of this
 * header can read the true boundary without pulling in the veneer machinery.
 */
extern "C" std::atomic<std::uint64_t> pl_runtime_swap_buffers_count;
extern "C" std::atomic<long long> pl_runtime_swap_buffers_last_us;

namespace pl::runtime {

/** @brief The frame-boundary counters, or nulls when the swap hook is not installed. */
struct SwapFrameBoundaryStats {
  /** True once the hook is installed (not necessarily that a frame has been presented yet). */
  bool installed = false;
  /** The libEGL symbol that was hooked, for the diagnostics report. */
  const char *symbol = "";
  /** The number of presents seen since install. */
  std::uint64_t presents = 0;
  /** The monotonic microsecond stamp of the most recent present, or 0 before the first. */
  long long lastPresentUs = 0;
};

/**
 * @brief Installs the swap-buffers hook, once. Safe to call repeatedly.
 *
 * @return true when the hook is installed (this call or an earlier one); false when libEGL or the
 *         symbol is unavailable, in which case the caller keeps the inferred frame clock.
 */
PL_EXPORT bool InstallSwapFrameBoundary();

/** @brief True once {@link InstallSwapFrameBoundary} has installed the hook. */
PL_EXPORT bool IsSwapFrameBoundaryLive();

/** @brief The current counters, for the diagnostics report. */
PL_EXPORT SwapFrameBoundaryStats ReadSwapFrameBoundaryStats();

} // namespace pl::runtime
