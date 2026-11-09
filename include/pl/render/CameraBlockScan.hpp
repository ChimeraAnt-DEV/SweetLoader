#pragma once

/**
 * @file CameraBlockScan.hpp
 * @brief The pure rule that finds a camera view/projection pair inside a float buffer.
 *
 * The game strips its symbols, so the camera is found by shape: a perspective projection matrix (the
 * characteristic 1/tan(fov) diagonal, a negative near/far remap in m[10] and -1 in m[11]) whose
 * immediately-preceding 16 floats are a rigid view transform (bottom row 0,0,0,1 and an orthonormal
 * unit-length basis). `CameraSource` walks the game's writable ranges with this rule; the rule itself
 * is pure so it is host-tested against a synthetic buffer with a real pair buried in noise.
 *
 * Header-only: no Android, no /proc, no globals. The scan loop lives in `CameraSource`.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pl::render {

/** The value returned when no view/projection pair is present. */
inline constexpr std::size_t kNoCameraPair = static_cast<std::size_t>(-1);

/** Whether a float is finite. */
inline bool CameraFinite(float v) { return std::isfinite(v); }

/**
 * @brief Whether a 16-float block looks like a perspective projection matrix.
 *
 * Column-major: m[0] and m[5] are the focal scales (a little above 1 for a normal FOV), m[10] is the
 * negative near/far remap and m[11] is -1. The off-diagonal entries of a symmetric perspective are
 * zero.
 */
inline bool IsPerspectiveProjection(const float *m) {
  const float m0 = m[0], m5 = m[5], m10 = m[10], m11 = m[11];
  if (!CameraFinite(m0) || !CameraFinite(m5) || !CameraFinite(m10) || !CameraFinite(m11)) return false;
  if (!(m0 > 0.5F && m0 < 6.0F)) return false;
  if (!(m5 > 0.5F && m5 < 6.0F)) return false;
  if (!(m10 < -0.5F && m10 > -2.0F)) return false;
  if (std::fabs(m11 + 1.0F) > 0.25F) return false;
  if (std::fabs(m[1]) > 0.05F || std::fabs(m[4]) > 0.05F) return false;
  return true;
}

/**
 * @brief Whether a 16-float block looks like a rigid view transform.
 *
 * The bottom row is (0,0,0,1) and the upper-left 3x3 is orthonormal; checking the unit length of the
 * three basis vectors and their pairwise orthogonality rejects the random data that otherwise passes
 * a loose check.
 */
inline bool IsRigidView(const float *m) {
  for (int i = 0; i < 16; ++i) {
    if (!CameraFinite(m[i])) return false;
  }
  if (std::fabs(m[3]) > 0.05F || std::fabs(m[7]) > 0.05F || std::fabs(m[11]) > 0.05F) return false;
  if (std::fabs(m[15] - 1.0F) > 0.05F) return false;

  const float cx = m[0], cy = m[1], cz = m[2];
  const float ux = m[4], uy = m[5], uz = m[6];
  const float fx = m[8], fy = m[9], fz = m[10];
  const auto length = [](float x, float y, float z) { return std::sqrt(x * x + y * y + z * z); };
  const float lc = length(cx, cy, cz);
  const float lu = length(ux, uy, uz);
  const float lf = length(fx, fy, fz);
  if (lc < 0.5F || lc > 2.0F) return false;
  if (lu < 0.5F || lu > 2.0F) return false;
  if (lf < 0.5F || lf > 2.0F) return false;
  if (std::fabs(cx * ux + cy * uy + cz * uz) > 0.1F) return false;
  if (std::fabs(cx * fx + cy * fy + cz * fz) > 0.1F) return false;
  if (std::fabs(ux * fx + uy * fy + uz * fz) > 0.1F) return false;
  return true;
}

/**
 * @brief Finds the first index where a projection matrix is preceded by a rigid view matrix.
 *
 * @param floats  The buffer, viewed as floats.
 * @param count   The number of floats in the buffer.
 * @return The index of the view matrix (the projection is 16 floats later), or {@link kNoCameraPair}.
 */
inline std::size_t FindViewProjectionPair(const float *floats, std::size_t count) {
  if (floats == nullptr) return kNoCameraPair;
  if (count < 32) return kNoCameraPair;
  for (std::size_t i = 16; i + 16 <= count; ++i) {
    if (!IsPerspectiveProjection(floats + i)) continue;
    if (!IsRigidView(floats + i - 16)) continue;
    return i - 16;
  }
  return kNoCameraPair;
}

} // namespace pl::render
