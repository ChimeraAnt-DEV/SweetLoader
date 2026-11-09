/**
 * @file RenderApi.cpp
 * @brief Public per-frame render callback + camera matrix API (pl/Render.hpp).
 *
 * The render seam itself is the existing player-render detour: it already runs on the render thread
 * with the GL context current. This file keeps the callback registry and the camera resolution that
 * the seam dispatches to, and exposes the fallback camera math.
 */

#include "pl/Render.hpp"

#include <atomic>
#include <cmath>
#include <ctime>
#include <numbers>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/render/CameraSource.h"
#include "pl/render/GlesProbe.hpp"
#include "pl/render/RenderDispatch.h"
#include "pl/runtime/CallbackSnapshot.hpp"
#include "pl/runtime/GameLocalPlayer.h"
#include "pl/runtime/HighWaterMark.hpp"
#include "pl/runtime/HookTiming.hpp"
#include "pl/runtime/OnceReport.hpp"

namespace pl::render {
namespace {

// The registry is a lock-free snapshot, not a mutex plus a vector.
//
// The dispatch runs on the render thread on every frame; a lock and a vector copy there is a cost
// on the frame budget for a list that changes maybe twice in a session. Publishing an immutable
// list means the dispatch is one atomic load and a const-reference loop, and a registration cannot
// block a frame. This is the same store the input bridge uses, so the two hot paths share one
// mechanism.
pl::runtime::CallbackSnapshot<RenderCallback> g_callbacks;

/// Reads the current GL viewport, or {0,0} when the GLES surface is unavailable. Cached table.
void QueryViewport(int &width, int &height) {
  width = 0;
  height = 0;
  const GlesFunctions gles = ResolveGles();
  if (!gles.GetIntegerv) return;
  using GetIntegervFn = void (*)(std::uint32_t, std::int32_t *);
  auto getIntegerv = reinterpret_cast<GetIntegervFn>(gles.GetIntegerv);
  std::int32_t viewport[4] = {0, 0, 0, 0};
  getIntegerv(0x0BA2 /* GL_VIEWPORT */, viewport);
  width = viewport[2];
  height = viewport[3];
}

std::atomic<std::uint64_t> g_frameTick{0};
std::atomic<bool> g_live{false};

/// Set once so the (expensive) camera scan runs off the render thread at first registration.
std::atomic<bool> g_cameraResolveRequested{false};

} // namespace

bool RegisterRenderCallback(RenderCallback callback) {
  if (!callback) return false;
  g_callbacks.add(std::move(callback));
  // Resolve the real camera once, lazily. Do it here rather than in the render path: the scan walks
  // the game's whole writable data and must never run on the render thread.
  if (!g_cameraResolveRequested.exchange(true, std::memory_order_acq_rel)) {
    CameraSource::ResolveOnce();
  }
  return true;
}

std::size_t UnregisterRenderCallbacks(bool all) {
  if (all) {
    const std::size_t removed = g_callbacks.size();
    g_callbacks.clear();
    return removed;
  }
  return g_callbacks.removeLast() ? 1 : 0;
}

bool IsRenderCallbackLive() { return g_live.load(std::memory_order_acquire); }

std::uint64_t RenderFrameTick() { return g_frameTick.load(std::memory_order_acquire); }

// --- fallback camera math --------------------------------------------------------------

bool MakePerspective(float fovDegrees, float aspectRatio, float nearPlane, float farPlane,
                     Mat4 &out) {
  if (!(fovDegrees > 1.0F && fovDegrees < 179.0F)) return false;
  if (!(aspectRatio > 0.0F)) return false;
  if (!(nearPlane > 0.0F) || !(farPlane > nearPlane)) return false;

  const float radians = fovDegrees * static_cast<float>(std::numbers::pi) / 180.0F;
  const float f = 1.0F / std::tan(radians * 0.5F);
  out = Mat4{};
  out.m[0] = f / aspectRatio;
  out.m[5] = f;
  out.m[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
  out.m[11] = -1.0F;
  out.m[14] = (2.0F * farPlane * nearPlane) / (nearPlane - farPlane);
  out.m[15] = 0.0F;
  return true;
}

bool MakeView(const Vec3 &eye, const Vec3 &forward, const Vec3 &up, Mat4 &out) {
  auto normalize = [](const Vec3 &v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length <= 1e-6F) return Vec3{0.0F, 0.0F, 0.0F};
    return Vec3{v.x / length, v.y / length, v.z / length};
  };
  auto cross = [](const Vec3 &a, const Vec3 &b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
  };
  auto dot = [](const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; };

  const Vec3 f = normalize(forward);
  if (f.x == 0.0F && f.y == 0.0F && f.z == 0.0F) return false;
  // Right = forward x up, then re-orthogonalise up against forward.
  const Vec3 s = normalize(cross(f, up));
  if (s.x == 0.0F && s.y == 0.0F && s.z == 0.0F) return false;
  const Vec3 u = cross(s, f);

  // Column-major right-handed look-at: rows are the basis, translation is -basis*eye.
  out = Mat4{};
  out.m[0] = s.x;  out.m[4] = s.y;  out.m[8]  = s.z;
  out.m[1] = u.x;  out.m[5] = u.y;  out.m[9]  = u.z;
  out.m[2] = -f.x; out.m[6] = -f.y; out.m[10] = -f.z;
  out.m[12] = -dot(s, eye);
  out.m[13] = -dot(u, eye);
  out.m[14] = dot(f, eye);
  out.m[15] = 1.0F;
  return true;
}

bool ReadLocalPlayerPosition(float out[3]) {
  return pl::runtime::ReadLocalPlayerPosition(out);
}

bool ReadLocalPlayerRotation(float out[2]) {
  return pl::runtime::ReadLocalPlayerRotation(out);
}

bool ComputeViewProjection(float fovDegrees, float aspectRatio, float nearPlane, float farPlane,
                           Mat4 &out) {
  float position[3] = {0.0F, 0.0F, 0.0F};
  float rotation[2] = {0.0F, 0.0F};
  if (!pl::runtime::ReadLocalPlayerPosition(position)) return false;
  if (!pl::runtime::ReadLocalPlayerRotation(rotation)) return false;

  // Bedrock reports yaw/pitch in degrees. Yaw is measured clockwise from -Z (the usual Minecraft
  // convention), so forward = (-sin(yaw), -sin(pitch)... ) with pitch positive looking down.
  const float yaw = rotation[0] * static_cast<float>(std::numbers::pi) / 180.0F;
  const float pitch = rotation[1] * static_cast<float>(std::numbers::pi) / 180.0F;
  const float cosPitch = std::cos(pitch);
  const Vec3 forward{
      -std::sin(yaw) * cosPitch,
      -std::sin(pitch),
      -std::cos(yaw) * cosPitch,
  };
  // The eye sits at the player's feet plus eye height (~1.62 blocks), the same offset Bedrock uses.
  const Vec3 eye{position[0], position[1] + 1.62F, position[2]};

  Mat4 view;
  if (!MakeView(eye, forward, Vec3{0.0F, 1.0F, 0.0F}, view)) return false;
  Mat4 projection;
  if (!MakePerspective(fovDegrees, aspectRatio, nearPlane, farPlane, projection)) return false;

  out = Mat4::multiply(projection, view);
  return out.isFinite();
}

// --- render-callback timing (audit item 3.12) ------------------------------------------

namespace {

// The dispatch times every callback with one monotonic clock read each. The read is always on (not
// gated to a debug build) because it is one cheap read per callback and the aggregate is what lets
// the launcher name a mod that stutters the frame; the *report* is rate-limited to once, so a
// callback that is over budget every frame does not flood the log.
long long MonotonicUsNow() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000LL;
}

std::atomic<std::uint32_t> g_lastFrameCallbackCount{0};
std::atomic<std::uint32_t> g_dispatchPasses{0};
std::atomic<std::uint32_t> g_worstCallbackUs{0};
std::atomic<std::uint32_t> g_overBudgetCallbackCount{0};

} // namespace

// --- dispatch (called from the render seam) --------------------------------------------

namespace detail {

/**
 * @brief Builds the context and invokes every registered callback for this frame.
 *
 * Called from the render detour after the game's own draw, on the render thread. The published list
 * is read with one atomic load and iterated by const reference, so a callback that
 * (un)registers another callback cannot deadlock and a slow callback cannot block registration.
 */
void DispatchRenderCallbacks() {
  if (g_callbacks.size() == 0) return;

  RenderContext context;
  context.frameTick = g_frameTick.fetch_add(1, std::memory_order_acq_rel) + 1;
  QueryViewport(context.viewportWidth, context.viewportHeight);

  const CameraSource::Frame camera = CameraSource::ReadFrame();
  if (camera.hasViewProjection) {
    context.viewProjection = camera.viewProjection;
    context.hasViewProjection = true;
  }
  if (camera.hasView) {
    context.view = camera.view;
    context.hasView = true;
  }
  if (camera.hasProjection) {
    context.projection = camera.projection;
    context.hasProjection = true;
  }
  // The camera eye is the inverse-translation of the view matrix: -R^T * t.
  if (camera.hasView) {
    const Mat4 &v = camera.view;
    context.cameraPosition = Vec3{
        -(v.m[0] * v.m[12] + v.m[1] * v.m[13] + v.m[2] * v.m[14]),
        -(v.m[4] * v.m[12] + v.m[5] * v.m[13] + v.m[6] * v.m[14]),
        -(v.m[8] * v.m[12] + v.m[9] * v.m[13] + v.m[10] * v.m[14]),
    };
    context.hasCameraPosition = true;
    context.cameraForward = Vec3{-v.m[2], -v.m[6], -v.m[10]};
    context.hasCameraForward = true;
  }

  g_live.store(true, std::memory_order_release);

  // Each callback is timed (one monotonic read each) and isolated: a throw is swallowed so the
  // remaining callbacks and the frame continue, and the report is rate-limited so a callback that
  // throws every frame does not flood the log with a per-frame fmt::vformat allocation (audit item
  // 3.11). The worst single-callback time is published for the diagnostics reader (item 3.12).
  static pl::runtime::OnceReport throwReported;
  std::uint32_t dispatched = 0;
  std::uint32_t worstUs = 0;
  g_callbacks.forEach([&](const RenderCallback &callback) {
    const long long startedUs = MonotonicUsNow();
    try {
      callback(context);
    } catch (...) {
      if (throwReported.shouldReport()) {
        preloaderLogger.warn("Render callback threw; frame continues (reported once)");
      }
    }
    const long long elapsedUs = MonotonicUsNow() - startedUs;
    ++dispatched;
    if (elapsedUs > 0 && static_cast<std::uint32_t>(elapsedUs) > worstUs) {
      worstUs = static_cast<std::uint32_t>(elapsedUs);
    }
    if (pl::runtime::HookOverBudget(elapsedUs)) {
      g_overBudgetCallbackCount.fetch_add(1, std::memory_order_relaxed);
    }
  });

  g_lastFrameCallbackCount.store(dispatched, std::memory_order_relaxed);
  g_dispatchPasses.fetch_add(1, std::memory_order_relaxed);
  // Publish the worst observed time, not just this frame's, so a one-off stall is not overwritten by
  // the next cheap frame before the launcher polls.
  pl::runtime::RaiseHighWaterMark(g_worstCallbackUs, worstUs);
}

void ReadRenderCallbackStats(std::uint32_t out[4]) {
  if (out == nullptr) return;
  out[0] = g_lastFrameCallbackCount.load(std::memory_order_relaxed);
  out[1] = g_dispatchPasses.load(std::memory_order_relaxed);
  out[2] = g_worstCallbackUs.load(std::memory_order_relaxed);
  out[3] = g_overBudgetCallbackCount.load(std::memory_order_relaxed);
}

} // namespace detail

} // namespace pl::render
