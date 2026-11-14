/**
 * @file PresentFrameMathTest.cpp
 * @brief Pins the present-frame effect math (frame generation + motion blur).
 *
 * These two functions are the whole honest content of the two modules: they decide how much of the
 * previous frame is mixed in and where the interpolated frame sits. They are pure arithmetic, so
 * they are verified here rather than on a device, exactly like the other pure rules in this tree.
 *
 * The contract under test:
 *   - neither function can return a value outside [0, 1];
 *   - a zero strength yields a zero blend (the module off is a no-op);
 *   - the budget caps the blend rather than being ignored;
 *   - a full-strength frame generation sits at the midpoint (weight 0.5).
 */

#include <cstdio>
#include <cstdlib>

#include "pl/render/PresentFrameMath.hpp"

using pl::render::ComputeBlendAlpha;
using pl::render::ComputeInterpolationFactor;
using pl::render::FrameGenerationWeight;

namespace {

int g_failures = 0;

void ExpectNear(const char *what, float actual, float expected, float tol = 1e-5f) {
  if (actual < expected - tol || actual > expected + tol) {
    std::printf("FAIL %s: expected %.6f, got %.6f\n", what, expected, actual);
    ++g_failures;
  }
}

void ExpectInUnitRange(const char *what, float value) {
  if (value < 0.f || value > 1.f) {
    std::printf("FAIL %s: out of [0,1]: %.6f\n", what, value);
    ++g_failures;
  }
}

void BlendAlphaRules() {
  // Zero strength is a no-op regardless of the other inputs.
  ExpectNear("zero strength", ComputeBlendAlpha(0.f, 0.6f, 16.f, 1.f, false), 0.f);

  // Strength passes through when the budget is generous and speed weighting is off.
  ExpectNear("plain strength", ComputeBlendAlpha(0.35f, 1.0f, 16.f, 1.f, false), 0.35f);

  // Speed weighting scales the alpha by the normalised speed.
  ExpectNear("speed weighted", ComputeBlendAlpha(0.8f, 1.0f, 16.f, 0.5f, true), 0.4f);
  // A speed above 1 is clamped, so the alpha never exceeds the strength.
  ExpectNear("speed clamped", ComputeBlendAlpha(0.8f, 1.0f, 16.f, 5.f, true), 0.8f);

  // A tight budget scales the alpha down proportionally: 6ms budget of a 12ms frame is half.
  ExpectNear("budget halves", ComputeBlendAlpha(0.8f, 0.5f, 12.f, 1.f, false), 0.4f);

  // A zero budget with a real interval leaves no room for the blend.
  ExpectNear("zero budget", ComputeBlendAlpha(0.8f, 0.f, 12.f, 1.f, false), 0.8f);

  // Unknown interval (<= 0) imposes no budget cut.
  ExpectNear("unknown interval", ComputeBlendAlpha(0.8f, 0.1f, 0.f, 1.f, false), 0.8f);

  // The result is always in range, even for absurd inputs.
  ExpectInUnitRange("clamp high", ComputeBlendAlpha(9.f, 9.f, 1.f, 1.f, false));
  ExpectInUnitRange("clamp low", ComputeBlendAlpha(-1.f, -1.f, -1.f, -1.f, true));
}

void InterpolationRules() {
  // Full strength is the midpoint; the weight (distance from the current frame) is 0.5.
  ExpectNear("full strength factor", ComputeInterpolationFactor(1.f, false), 0.5f);
  ExpectNear("full strength weight", FrameGenerationWeight(1.f, false), 0.5f);

  // Zero strength biases fully to the current real frame (factor 1, weight 0): the low-artefact end.
  ExpectNear("zero strength factor", ComputeInterpolationFactor(0.f, false), 1.f);
  ExpectNear("zero strength weight", FrameGenerationWeight(0.f, false), 0.f);

  // The edge-artefact bias pulls the factor toward the current real frame (larger factor).
  ExpectInUnitRange("edge bias", ComputeInterpolationFactor(0.5f, true));
  if (ComputeInterpolationFactor(0.5f, true) <= ComputeInterpolationFactor(0.5f, false)) {
    std::printf("FAIL edge bias: expected the factor to move toward the current real frame\n");
    ++g_failures;
  }

  // Out-of-range strengths are clamped, and the factor stays in range.
  ExpectInUnitRange("strength above one", ComputeInterpolationFactor(9.f, false));
  ExpectInUnitRange("strength below zero", ComputeInterpolationFactor(-9.f, true));
  ExpectNear("strength above one clamps", ComputeInterpolationFactor(9.f, false), 0.5f);
}

} // namespace

int main() {
  BlendAlphaRules();
  InterpolationRules();
  if (g_failures != 0) {
    std::printf("%d present-frame math assertion(s) failed\n", g_failures);
    return 1;
  }
  std::printf("PresentFrameMathTest: all assertions passed\n");
  return 0;
}
