#pragma once

/**
 * @file RenderDispatch.h
 * @brief Internal seam between the render hook and the public render-callback registry.
 *
 * The player-render detour (NativeRenderHook.cpp) owns the only render-thread, GL-context-current
 * point in the process, so it is what dispatches the public callbacks registered through
 * `pl/Render.hpp`. This header carries just that one call across the two translation units.
 */

#include <cstdint>

namespace pl::render::detail {

/// Invokes every registered render callback for this frame. Render thread only.
void DispatchRenderCallbacks();

/**
 * @brief Reads the render-callback timing aggregate (audit item 3.12).
 *
 * <p>A callback that blocks the render thread stutters the frame, and the cost is invisible without
 * measurement. The dispatch times every callback (one monotonic clock read each, always on) and
 * publishes a small aggregate here so a diagnostics reader can name the stutter.
 *
 * @param out [0] the number of callbacks dispatched in the last frame, [1] the number of dispatch
 *            passes, [2] the worst single-callback time seen in microseconds, [3] the number of
 *            callbacks that exceeded the time budget.
 */
void ReadRenderCallbackStats(std::uint32_t out[4]);

} // namespace pl::render::detail
