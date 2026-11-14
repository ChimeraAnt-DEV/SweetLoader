#pragma once

/**
 * @file PresentFrameMath.hpp
 * @brief The pure math behind the present-frame effect modules (frame generation, motion blur).
 *
 * <p>Kept apart from {@link PresentFrameEffect} on purpose: the effect file pulls in the GL probe
 * and the logger (and therefore the Android shims), while these two functions are plain arithmetic
 * with no dependencies. That is what lets the host test runner pin them directly, off-device, which
 * is the same reason the other pure rules in this tree live in their own headers.
 *
 * <p><b>Honesty.</b> Neither function measures or promises a frame rate. Frame generation returns
 * how far the blended image sits toward the previous real frame; motion blur returns how much of the
 * previous frame to mix in. Both are clamped to [0, 1]. Neither inserts a frame: the seam presents
 * one frame per rendered frame.
 */

#include <cstdint>

namespace pl::render {

/** @brief The plan the launcher sends from the module options. All values are clamped on receipt. */
struct PresentFramePlan {
  /** Whether frame generation is enabled (the module switch and its own toggle). */
  bool frameGenerationEnabled = false;
  /** 0..1: how far the blended image is pushed toward the previous of the two real frames. */
  float interpolationStrength = 0.f;
  /**
   * 2..3: reserved. The seam blends the previous frame into the current one and presents a single
   * frame, so it does not act on this ceiling yet; it is kept for a future true frame-insertion
   * seam. Stored and clamped so the option round-trips, but nothing reads it to emit extra presents.
   */
  int maxFramesPerRendered = 2;
  /** Bias the blend to reuse less of the previous frame near edges. */
  bool reduceEdgeArtifacts = false;

  /** Whether motion blur is enabled (the module switch and its own toggle). */
  bool motionBlurEnabled = false;
  /** 0..1: how much of the previous frame is blended in. */
  float blurStrength = 0.f;
  /** 0..1: the most of the frame time the blend may take. */
  float blurBudget = 0.f;
  /** Scale the blur by camera speed, so a still view stays sharp. */
  bool weightByViewSpeed = false;
};

/**
 * @brief The blend alpha for motion blur this frame.
 *
 * The alpha is the fraction of the previous frame mixed into the current one. It is the configured
 * strength, scaled by how fast the view is moving when {@code weightByViewSpeed} is set, and it is
 * additionally capped by the budget: a frame interval longer than the budget leaves no room for the
 * blend, so the alpha falls proportionally rather than the effect overrunning the frame.
 *
 * @param strength        configured blur strength, 0..1
 * @param budget          configured blend budget as a fraction of the frame time, 0..1
 * @param frameIntervalMs the last measured frame interval, milliseconds (<= 0 means unknown)
 * @param viewSpeed01     camera speed normalised to 0..1 (0 when unknown)
 * @param weightByViewSpeed whether to scale by {@p viewSpeed01}
 * @return the blend alpha in [0, 1]
 */
inline float ComputeBlendAlpha(float strength, float budget, float frameIntervalMs,
                               float viewSpeed01, bool weightByViewSpeed) {
  if (strength <= 0.f) return 0.f;
  float alpha = strength;
  if (weightByViewSpeed) {
    const float speed = viewSpeed01 < 0.f ? 0.f : (viewSpeed01 > 1.f ? 1.f : viewSpeed01);
    alpha *= speed;
  }
  if (budget > 0.f && frameIntervalMs > 0.f) {
    const float budgetMs = budget * frameIntervalMs;
    if (budgetMs <= 0.f) return 0.f;
    if (frameIntervalMs > budgetMs) alpha *= budgetMs / frameIntervalMs;
  }
  if (alpha < 0.f) return 0.f;
  if (alpha > 1.f) return 1.f;
  return alpha;
}

/**
 * @brief The interpolation factor for frame generation this frame.
 *
 * The factor is where the blended frame sits between the previous real frame (0) and the current
 * one (1): 0.5 is the midpoint. At full strength the factor is the midpoint; as strength falls it
 * moves toward the current frame the game actually drew, which is the low-artefact end. With
 * {@code reduceEdgeArtifacts} the factor is pulled further toward that real frame.
 *
 * <p><b>Honesty.</b> This factor is a position between two real frames, not a promise that a third
 * frame is presented. The seam uses {@link FrameGenerationWeight} (its complement) as the blend
 * weight: it mixes the previous frame into the current one, it does not insert a frame.
 *
 * @param strength            configured interpolation strength, 0..1
 * @param reduceEdgeArtifacts bias toward the current real frame near edges
 * @return the interpolation factor in [0, 1]
 */
inline float ComputeInterpolationFactor(float strength, bool reduceEdgeArtifacts) {
  const float s = strength < 0.f ? 0.f : (strength > 1.f ? 1.f : strength);
  float factor = 0.5f + 0.5f * (1.f - s);
  if (reduceEdgeArtifacts) {
    factor = factor * 0.85f + 0.15f;
  }
  if (factor < 0.f) return 0.f;
  if (factor > 1.f) return 1.f;
  return factor;
}

/**
 * @brief The blend weight frame generation contributes: its distance from the current real frame.
 *
 * This is what the seam actually blends. It is derived from {@link ComputeInterpolationFactor} so
 * the two never drift, and it is the value the seam compares against the motion-blur alpha.
 */
inline float FrameGenerationWeight(float strength, bool reduceEdgeArtifacts) {
  return 1.f - ComputeInterpolationFactor(strength, reduceEdgeArtifacts);
}

} // namespace pl::render
