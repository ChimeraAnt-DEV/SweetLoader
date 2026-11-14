#pragma once

/**
 * @file PresentFrameEffect.hpp
 * @brief The present-frame effect seam: frame generation and motion blur, on the engine's present.
 *
 * <p>Both effects act on the frame the engine is about to present, so both live on the same seam:
 * the {@code eglSwapBuffers} detour (`pl::runtime::InstallSwapFrameBoundary`). This header owns the
 * plan the launcher sends, the pure blend/interpolation math, and the install state.
 *
 * <p><b>Honesty contract.</b> Neither effect raises the game's frame rate. Frame generation blends
 * the previous frame into the current one so the presented image sits <em>between</em> two the game
 * already drew; motion blur blends the previous frame in, which softens the image by design. Neither
 * presents an extra frame: the seam emits one present per rendered frame. {@link ComputeBlendAlpha}
 * and {@link ComputeInterpolationFactor} are pure and host-tested, so the numbers the seam uses are
 * checkable off-device.
 *
 * <p><b>Fail-closed.</b> {@link InstallPresentFrameEffect} reports false when the GL path cannot be
 * set up (no GLES table, no EGL context, or a build with no present hook). {@link
 * IsPresentFrameEffectLive} then reports false and the launcher keeps both modules badged rather
 * than toggling to no effect. Nothing here fabricates a frame.
 */

#include <cstdint>

#include "pl/Export.hpp"
#include "pl/render/PresentFrameMath.hpp"

namespace pl::render {

/**
 * @brief Installs the present-frame effect seam, once. Safe to call repeatedly.
 *
 * <p>Fail-closed: returns false when the engine's present hook is not live, or the GL path the
 * effects need cannot be resolved. A false result leaves {@link IsPresentFrameEffectLive} false, so
 * the launcher keeps both modules badged.
 *
 * @return true when the seam is installed and can act on presented frames.
 */
PL_EXPORT bool InstallPresentFrameEffect();

/** @brief True once {@link InstallPresentFrameEffect} has installed a working seam. */
PL_EXPORT bool IsPresentFrameEffectLive();

/**
 * @brief Stores the plan the effects apply on the next present.
 *
 * Safe to call from the launcher thread; the seam reads the plan on the render thread. Values are
 * clamped so a stored option can never drive an out-of-range blend.
 */
PL_EXPORT void SetPresentFramePlan(const PresentFramePlan &plan);

/** @brief The stored plan, for diagnostics. */
PL_EXPORT PresentFramePlan GetPresentFramePlan();

/**
 * @brief Runs the effects at a present boundary.
 *
 * Called from the {@code eglSwapBuffers} record stub, on the render thread, with the GL context
 * current. A no-op when the seam is not live or no effect is enabled. It never blocks and never
 * allocates on the steady path.
 */
PL_EXPORT void OnPresentBoundary();

} // namespace pl::render
