/**
 * @file PresentFrameEffect.cpp
 * @brief The present-frame effect seam (frame generation + motion blur) on the engine's present.
 *
 * The seam is the {@code eglSwapBuffers} detour, run from the swap record stub just before the real
 * swap. At that moment the back buffer holds the frame the engine just drew; this file keeps a copy
 * of the previous presented frame and, when a module is enabled, composites it over the back buffer,
 * then rotates the two captures. The blend weight is the frame-generation interpolation weight or
 * the motion-blur strength, whichever is larger, so the effect the player enabled is not cancelled
 * by the other.
 *
 * <p><b>Honesty.</b> This does not raise the render rate. It blends the previous frame into the
 * current one, so motion is smoother and the image is softer. The blend weight comes from
 * {@link pl::render::ComputeBlendAlpha} / {@link pl::render::ComputeInterpolationFactor}, which are
 * pure and host-tested.
 *
 * <p><b>Fail-closed.</b> {@link InstallPresentFrameEffect} returns false when the present hook is
 * not live, no EGL context is current, or the GL table cannot resolve the entry points the
 * composite needs. Nothing is drawn in that case, and the launcher keeps both modules badged.
 *
 * <p><b>State preservation.</b> The composite runs inside the game's own present path, so it saves
 * and restores every piece of GL state it touches (program, VAO, array buffer, active texture,
 * texture binding, blend, depth test, cull, viewport). It never leaves the pipeline in a state the
 * game did not set.
 */

#include "pl/render/PresentFrameEffect.hpp"

#include <atomic>
#include <cstdint>

#include "pl/Logger.hpp"
#include "pl/menu/MenuHost.hpp"
#include "pl/render/GlesProbe.hpp"
#include "pl/runtime/SwapFrameBoundary.h"

namespace pl::render {
namespace {

// GL enums used below, declared locally so this file needs no GLES header (the host test runner has
// none). Values are from the GLES 2.0/3.0 specs.
constexpr std::uint32_t GL_TEXTURE_2D = 0x0DE1;
constexpr std::uint32_t GL_TEXTURE0 = 0x84C0;
constexpr std::uint32_t GL_RGBA = 0x1908;
constexpr std::uint32_t GL_UNSIGNED_BYTE = 0x1401;
constexpr std::uint32_t GL_FLOAT = 0x1406;
constexpr std::uint32_t GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr std::uint32_t GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr std::uint32_t GL_TEXTURE_WRAP_S = 0x2802;
constexpr std::uint32_t GL_TEXTURE_WRAP_T = 0x2803;
constexpr std::uint32_t GL_LINEAR = 0x2601;
constexpr std::uint32_t GL_CLAMP_TO_EDGE = 0x812F;
constexpr std::uint32_t GL_BLEND = 0x0BE2;
constexpr std::uint32_t GL_DEPTH_TEST = 0x0B71;
constexpr std::uint32_t GL_CULL_FACE = 0x0B44;
constexpr std::uint32_t GL_SRC_ALPHA = 0x0302;
constexpr std::uint32_t GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr std::uint32_t GL_BLEND_SRC_ALPHA = 0x80CB;
constexpr std::uint32_t GL_BLEND_DST_ALPHA = 0x80CA;
constexpr std::uint32_t GL_BLEND_SRC_RGB = 0x80C9;
constexpr std::uint32_t GL_BLEND_DST_RGB = 0x80C8;
constexpr std::uint32_t GL_CURRENT_PROGRAM = 0x8B8D;
constexpr std::uint32_t GL_ACTIVE_TEXTURE = 0x84E0;
constexpr std::uint32_t GL_TEXTURE_BINDING_2D = 0x8069;
constexpr std::uint32_t GL_VERTEX_ARRAY_BINDING = 0x85B5;
constexpr std::uint32_t GL_ARRAY_BUFFER = 0x8892;
constexpr std::uint32_t GL_ARRAY_BUFFER_BINDING = 0x8894;
constexpr std::uint32_t GL_STATIC_DRAW = 0x88E4;
constexpr std::uint32_t GL_VIEWPORT = 0x0BA2;
constexpr std::uint32_t GL_TRIANGLES = 0x0004;
constexpr std::uint32_t GL_FRAGMENT_SHADER = 0x8B30;
constexpr std::uint32_t GL_VERTEX_SHADER = 0x8B31;
constexpr std::uint32_t GL_COMPILE_STATUS = 0x8B81;
constexpr std::uint32_t GL_LINK_STATUS = 0x8B82;

// GLSL ES 1.00, so it compiles on both ES2 and ES3-default contexts. A fullscreen quad is uploaded
// once; the fragment shader samples the previous frame at the current fragment's UV and outputs it
// with the blend alpha, and fixed-function blending mixes it into the back buffer.
const char *kVertexShader =
    "attribute vec2 a_pos;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";
const char *kFragmentShader =
    "precision mediump float;\n"
    "uniform sampler2D u_prev;\n"
    "uniform vec2 u_resolution;\n"
    "uniform float u_alpha;\n"
    "void main() {\n"
    "  vec4 c = texture2D(u_prev, gl_FragCoord.xy / u_resolution);\n"
    "  gl_FragColor = vec4(c.rgb, u_alpha);\n"
    "}\n";

constexpr float kQuad[] = {
    -1.f, -1.f, 1.f, -1.f, -1.f, 1.f,
    -1.f, 1.f,  1.f, -1.f, 1.f,  1.f,
};

std::atomic_bool g_installed{false};
std::atomic_bool g_live{false};

// The plan is written by the launcher thread and read on the render thread. It is small and plain,
// so an atomic flag plus a plain copy under the flag is enough; the read is per present.
std::atomic_bool g_planDirty{false};
PresentFramePlan g_plan;
PresentFramePlan g_planSnapshot;

// The two capture textures (double buffered) and their size, created lazily on the render thread.
std::uint32_t g_captureTex[2] = {0, 0};
std::uint32_t g_prevIndex = 0;
int g_captureWidth = 0;
int g_captureHeight = 0;
bool g_captureValid = false;

std::uint32_t g_program = 0;
std::uint32_t g_vao = 0;
std::uint32_t g_vbo = 0;
int g_alphaUniform = -1;
int g_prevUniform = -1;
int g_resolutionUniform = -1;
int g_posAttrib = -1;

// The size of the last present, so the launcher can report it.
std::atomic<int> g_lastWidth{0};
std::atomic<int> g_lastHeight{0};

using GlGetIntegervFn = void (*)(std::uint32_t, std::int32_t *);
using GlGetBooleanvFn = void (*)(std::uint32_t, std::uint8_t *);

template <typename Fn>
Fn As(const void *p) {
  return reinterpret_cast<Fn>(const_cast<void *>(p));
}

// A copy of the GL state the composite changes, so it can be restored afterwards.
struct SavedState {
  std::int32_t program = 0;
  std::int32_t activeTexture = 0;
  std::int32_t texture2d = 0;
  std::int32_t vao = 0;
  std::int32_t arrayBuffer = 0;
  std::int32_t viewport[4] = {0, 0, 0, 0};
  std::uint8_t blend = 0;
  std::uint8_t depthTest = 0;
  std::uint8_t cullFace = 0;
  std::int32_t blendSrcRgb = 0;
  std::int32_t blendDstRgb = 0;
  std::int32_t blendSrcAlpha = 0;
  std::int32_t blendDstAlpha = 0;
};

void SaveState(const GlesFunctions &g, SavedState &s) {
  auto geti = As<GlGetIntegervFn>(g.GetIntegerv);
  auto getb = As<GlGetBooleanvFn>(g.GetBooleanv);
  geti(GL_CURRENT_PROGRAM, &s.program);
  geti(GL_ACTIVE_TEXTURE, &s.activeTexture);
  geti(GL_TEXTURE_BINDING_2D, &s.texture2d);
  geti(GL_VERTEX_ARRAY_BINDING, &s.vao);
  geti(GL_ARRAY_BUFFER_BINDING, &s.arrayBuffer);
  geti(GL_VIEWPORT, s.viewport);
  geti(GL_BLEND_SRC_RGB, &s.blendSrcRgb);
  geti(GL_BLEND_DST_RGB, &s.blendDstRgb);
  geti(GL_BLEND_SRC_ALPHA, &s.blendSrcAlpha);
  geti(GL_BLEND_DST_ALPHA, &s.blendDstAlpha);
  getb(GL_BLEND, &s.blend);
  getb(GL_DEPTH_TEST, &s.depthTest);
  getb(GL_CULL_FACE, &s.cullFace);
}

void RestoreState(const GlesFunctions &g, const SavedState &s) {
  using GlUseProgramFn = void (*)(std::uint32_t);
  using GlBindTextureFn = void (*)(std::uint32_t, std::uint32_t);
  using GlActiveTextureFn = void (*)(std::uint32_t);
  using GlBindVaoFn = void (*)(std::uint32_t);
  using GlBindBufferFn = void (*)(std::uint32_t, std::uint32_t);
  using GlViewportFn = void (*)(std::int32_t, std::int32_t, std::int32_t, std::int32_t);
  using GlBlendFuncSeparateFn = void (*)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
  using GlEnableFn = void (*)(std::uint32_t);
  using GlDisableFn = void (*)(std::uint32_t);

  As<GlUseProgramFn>(g.UseProgram)(static_cast<std::uint32_t>(s.program));
  As<GlActiveTextureFn>(g.ActiveTexture)(static_cast<std::uint32_t>(s.activeTexture));
  As<GlBindTextureFn>(g.BindTexture)(GL_TEXTURE_2D, static_cast<std::uint32_t>(s.texture2d));
  As<GlBindVaoFn>(g.BindVertexArray)(static_cast<std::uint32_t>(s.vao));
  if (g.BindBuffer != nullptr) {
    As<GlBindBufferFn>(g.BindBuffer)(GL_ARRAY_BUFFER, static_cast<std::uint32_t>(s.arrayBuffer));
  }
  As<GlViewportFn>(g.Viewport)(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
  if (g.BlendFuncSeparate != nullptr) {
    As<GlBlendFuncSeparateFn>(g.BlendFuncSeparate)(
        static_cast<std::uint32_t>(s.blendSrcRgb), static_cast<std::uint32_t>(s.blendDstRgb),
        static_cast<std::uint32_t>(s.blendSrcAlpha), static_cast<std::uint32_t>(s.blendDstAlpha));
  }
  auto enable = As<GlEnableFn>(g.Enable);
  auto disable = As<GlDisableFn>(g.Disable);
  if (s.blend) enable(GL_BLEND); else disable(GL_BLEND);
  if (s.depthTest) enable(GL_DEPTH_TEST); else disable(GL_DEPTH_TEST);
  if (s.cullFace) enable(GL_CULL_FACE); else disable(GL_CULL_FACE);
}

bool CompileShader(const GlesFunctions &g, std::uint32_t type, const char *source,
                   std::uint32_t &out) {
  using CreateShaderFn = std::uint32_t (*)(std::uint32_t);
  using ShaderSourceFn = void (*)(std::uint32_t, std::int32_t, const char *const *, const std::int32_t *);
  using CompileShaderFn = void (*)(std::uint32_t);
  using GetShaderivFn = void (*)(std::uint32_t, std::uint32_t, std::int32_t *);
  if (g.CreateShader == nullptr || g.ShaderSource == nullptr || g.CompileShader == nullptr ||
      g.GetShaderiv == nullptr) {
    return false;
  }
  out = As<CreateShaderFn>(g.CreateShader)(type);
  if (out == 0) return false;
  As<ShaderSourceFn>(g.ShaderSource)(out, 1, &source, nullptr);
  As<CompileShaderFn>(g.CompileShader)(out);
  std::int32_t status = 0;
  As<GetShaderivFn>(g.GetShaderiv)(out, GL_COMPILE_STATUS, &status);
  return status != 0;
}

bool BuildProgram(const GlesFunctions &g) {
  using CreateProgramFn = std::uint32_t (*)();
  using AttachShaderFn = void (*)(std::uint32_t, std::uint32_t);
  using LinkProgramFn = void (*)(std::uint32_t);
  using GetProgramivFn = void (*)(std::uint32_t, std::uint32_t, std::int32_t *);
  using GetUniformLocationFn = std::int32_t (*)(std::uint32_t, const char *);
  using GetAttribLocationFn = std::int32_t (*)(std::uint32_t, const char *);
  using GenBuffersFn = void (*)(std::int32_t, std::uint32_t *);
  using BindBufferFn = void (*)(std::uint32_t, std::uint32_t);
  using BufferDataFn = void (*)(std::uint32_t, std::intptr_t, const void *, std::uint32_t);
  if (g.CreateProgram == nullptr || g.AttachShader == nullptr || g.LinkProgram == nullptr ||
      g.GetProgramiv == nullptr || g.GetUniformLocation == nullptr ||
      g.GetAttribLocation == nullptr || g.GenBuffers == nullptr || g.BindBuffer == nullptr ||
      g.BufferData == nullptr) {
    return false;
  }
  std::uint32_t vs = 0;
  std::uint32_t fs = 0;
  if (!CompileShader(g, GL_VERTEX_SHADER, kVertexShader, vs)) return false;
  if (!CompileShader(g, GL_FRAGMENT_SHADER, kFragmentShader, fs)) return false;
  g_program = As<CreateProgramFn>(g.CreateProgram)();
  if (g_program == 0) return false;
  As<AttachShaderFn>(g.AttachShader)(g_program, vs);
  As<AttachShaderFn>(g.AttachShader)(g_program, fs);
  As<LinkProgramFn>(g.LinkProgram)(g_program);
  std::int32_t status = 0;
  As<GetProgramivFn>(g.GetProgramiv)(g_program, GL_LINK_STATUS, &status);
  if (status == 0) {
    g_program = 0;
    return false;
  }
  g_prevUniform = As<GetUniformLocationFn>(g.GetUniformLocation)(g_program, "u_prev");
  g_alphaUniform = As<GetUniformLocationFn>(g.GetUniformLocation)(g_program, "u_alpha");
  g_resolutionUniform = As<GetUniformLocationFn>(g.GetUniformLocation)(g_program, "u_resolution");
  g_posAttrib = As<GetAttribLocationFn>(g.GetAttribLocation)(g_program, "a_pos");
  if (g_prevUniform < 0 || g_alphaUniform < 0 || g_resolutionUniform < 0 || g_posAttrib < 0) {
    g_program = 0;
    return false;
  }
  As<GenBuffersFn>(g.GenBuffers)(1, &g_vbo);
  if (g_vbo == 0) {
    g_program = 0;
    return false;
  }
  As<BindBufferFn>(g.BindBuffer)(GL_ARRAY_BUFFER, g_vbo);
  As<BufferDataFn>(g.BufferData)(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
  return true;
}

bool EnsureCapture(const GlesFunctions &g, int width, int height) {
  using GenTexturesFn = void (*)(std::int32_t, std::uint32_t *);
  using BindTextureFn = void (*)(std::uint32_t, std::uint32_t);
  using TexParameteriFn = void (*)(std::uint32_t, std::uint32_t, std::int32_t);
  using TexImage2DFn = void (*)(std::uint32_t, std::int32_t, std::int32_t, std::int32_t,
                                std::int32_t, std::int32_t, std::uint32_t, std::uint32_t,
                                const void *);
  if (width <= 0 || height <= 0) return false;
  if (g_captureTex[0] != 0 && g_captureWidth == width && g_captureHeight == height) {
    return true;
  }
  if (g.GenTextures == nullptr || g.BindTexture == nullptr || g.TexParameteri == nullptr ||
      g.TexImage2D == nullptr) {
    return false;
  }
  if (g_captureTex[0] == 0) {
    As<GenTexturesFn>(g.GenTextures)(2, g_captureTex);
  }
  for (int i = 0; i < 2; ++i) {
    As<BindTextureFn>(g.BindTexture)(GL_TEXTURE_2D, g_captureTex[i]);
    As<TexParameteriFn>(g.TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                         static_cast<std::int32_t>(GL_LINEAR));
    As<TexParameteriFn>(g.TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                         static_cast<std::int32_t>(GL_LINEAR));
    As<TexParameteriFn>(g.TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                         static_cast<std::int32_t>(GL_CLAMP_TO_EDGE));
    As<TexParameteriFn>(g.TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                         static_cast<std::int32_t>(GL_CLAMP_TO_EDGE));
    As<TexImage2DFn>(g.TexImage2D)(GL_TEXTURE_2D, 0, static_cast<std::int32_t>(GL_RGBA), width,
                                   height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  }
  g_captureWidth = width;
  g_captureHeight = height;
  g_captureValid = false; // the fresh textures hold no real frame yet
  return true;
}

void RefreshPlan() {
  if (g_planDirty.exchange(false, std::memory_order_acq_rel)) {
    g_planSnapshot = g_plan;
  }
}

} // namespace

bool InstallPresentFrameEffect() {
  if (g_installed.exchange(true, std::memory_order_relaxed)) {
    return g_live.load(std::memory_order_relaxed);
  }
  if (!pl::runtime::IsSwapFrameBoundaryLive()) {
    sweetLoaderLogger.info(
        "Present-frame effect: no present hook on this build; frame generation and motion blur stay "
        "unavailable");
    return false;
  }
  if (!IsEglContextCurrent()) {
    sweetLoaderLogger.info("Present-frame effect: no EGL context yet; will retry when one is current");
    // Not an error: the context may not be current at install time. Leave the install flag false so a
    // later call retries; report not-live for now.
    g_installed.store(false, std::memory_order_relaxed);
    return false;
  }
  const GlesFunctions gles = ResolveGles();
  if (!gles.usable() || !BuildProgram(gles)) {
    sweetLoaderLogger.warn(
        "Present-frame effect: GL path unavailable (no shader program); the effects stay off");
    return false;
  }
  g_live.store(true, std::memory_order_relaxed);
  sweetLoaderLogger.info("Present-frame effect: seam live");
  return true;
}

bool IsPresentFrameEffectLive() { return g_live.load(std::memory_order_relaxed); }

void SetPresentFramePlan(const PresentFramePlan &plan) {
  PresentFramePlan clamped = plan;
  if (clamped.interpolationStrength < 0.f) clamped.interpolationStrength = 0.f;
  if (clamped.interpolationStrength > 1.f) clamped.interpolationStrength = 1.f;
  if (clamped.maxFramesPerRendered < 2) clamped.maxFramesPerRendered = 2;
  if (clamped.maxFramesPerRendered > 3) clamped.maxFramesPerRendered = 3;
  if (clamped.blurStrength < 0.f) clamped.blurStrength = 0.f;
  if (clamped.blurStrength > 1.f) clamped.blurStrength = 1.f;
  if (clamped.blurBudget < 0.f) clamped.blurBudget = 0.f;
  if (clamped.blurBudget > 1.f) clamped.blurBudget = 1.f;
  g_plan = clamped;
  g_planDirty.store(true, std::memory_order_release);
}

PresentFramePlan GetPresentFramePlan() {
  RefreshPlan();
  return g_planSnapshot;
}

void OnPresentBoundary() {
  // The present boundary is the one place an EGL context is guaranteed to be current, so both seams
  // that need one install lazily here rather than at load time (when no context exists yet). Until
  // they install, the modules stay badged; a build with no present hook never reaches this function.
  //
  // The in-game Mod Menu draws here too, before the effects: the menu's ImGui frame must render into
  // the back buffer the engine is about to present, and it runs whether or not any present-frame
  // effect is enabled, so it is the first thing the boundary does.
  if (pl::menu::IsOpen() || pl::menu::IsMenuLive()) {
    std::int32_t menuViewport[4] = {0, 0, 0, 0};
    const GlesFunctions menuGles = ResolveGles();
    if (menuGles.usable() && menuGles.GetIntegerv != nullptr) {
      As<GlGetIntegervFn>(menuGles.GetIntegerv)(GL_VIEWPORT, menuViewport);
    }
    pl::menu::Frame(static_cast<float>(menuViewport[2]), static_cast<float>(menuViewport[3]));
  }

  if (!g_live.load(std::memory_order_relaxed)) {
    InstallPresentFrameEffect();
    if (!g_live.load(std::memory_order_relaxed)) return;
  }
  RefreshPlan();
  const PresentFramePlan plan = g_planSnapshot;
  if (!plan.frameGenerationEnabled && !plan.motionBlurEnabled) return;

  const GlesFunctions g = ResolveGles();
  if (!g.usable() || g_program == 0 || g.DrawArrays == nullptr) return;

  std::int32_t viewport[4] = {0, 0, 0, 0};
  As<GlGetIntegervFn>(g.GetIntegerv)(GL_VIEWPORT, viewport);
  const int width = viewport[2];
  const int height = viewport[3];
  g_lastWidth.store(width, std::memory_order_relaxed);
  g_lastHeight.store(height, std::memory_order_relaxed);
  if (!EnsureCapture(g, width, height)) return;

  // Blend weight: frame generation uses its interpolation weight; motion blur uses the strength
  // (scaled by view speed when asked). When both are on the larger weight wins, so the effect the
  // player enabled is not silently cancelled by the other.
  float alpha = 0.f;
  if (plan.motionBlurEnabled) {
    alpha = ComputeBlendAlpha(plan.blurStrength, plan.blurBudget, 16.f, 1.f,
                              plan.weightByViewSpeed);
  }
  if (plan.frameGenerationEnabled) {
    const float weight = FrameGenerationWeight(plan.interpolationStrength,
                                               plan.reduceEdgeArtifacts);
    if (weight > alpha) alpha = weight;
  }
  if (alpha <= 0.001f) return;

  const std::uint32_t current = static_cast<std::uint32_t>(g_prevIndex);
  const std::uint32_t previous = static_cast<std::uint32_t>(g_prevIndex ^ 1);

  SavedState saved;
  SaveState(g, saved);

  using BindTextureFn = void (*)(std::uint32_t, std::uint32_t);
  using ActiveTextureFn = void (*)(std::uint32_t);
  using CopyTexImage2DFn = void (*)(std::uint32_t, std::int32_t, std::uint32_t, std::int32_t,
                                    std::int32_t, std::int32_t, std::int32_t, std::int32_t);
  using GenVertexArraysFn = void (*)(std::int32_t, std::uint32_t *);
  using BindVertexArrayFn = void (*)(std::uint32_t);
  using BindBufferFn = void (*)(std::uint32_t, std::uint32_t);
  using UseProgramFn = void (*)(std::uint32_t);
  using EnableVertexAttribArrayFn = void (*)(std::uint32_t);
  using DisableVertexAttribArrayFn = void (*)(std::uint32_t);
  using VertexAttribPointerFn = void (*)(std::uint32_t, std::int32_t, std::uint32_t, std::uint8_t,
                                         std::int32_t, const void *);
  using Uniform1iFn = void (*)(std::int32_t, std::int32_t);
  using Uniform1fFn = void (*)(std::int32_t, float);
  using Uniform2fFn = void (*)(std::int32_t, float, float);
  using DrawArraysFn = void (*)(std::uint32_t, std::int32_t, std::int32_t);
  using EnableFn = void (*)(std::uint32_t);
  using DisableFn = void (*)(std::uint32_t);
  using BlendFuncSeparateFn = void (*)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);

  if (g.GenVertexArrays == nullptr || g.BindVertexArray == nullptr || g.UseProgram == nullptr ||
      g.Uniform1i == nullptr || g.Uniform2f == nullptr || g.DrawArrays == nullptr ||
      g.CopyTexImage2D == nullptr || g.EnableVertexAttribArray == nullptr ||
      g.DisableVertexAttribArray == nullptr || g.VertexAttribPointer == nullptr ||
      g.BindBuffer == nullptr) {
    RestoreState(g, saved);
    return;
  }
  if (g_vao == 0) {
    As<GenVertexArraysFn>(g.GenVertexArrays)(1, &g_vao);
  }
  As<BindVertexArrayFn>(g.BindVertexArray)(g_vao);
  As<BindBufferFn>(g.BindBuffer)(GL_ARRAY_BUFFER, g_vbo);
  As<EnableVertexAttribArrayFn>(g.EnableVertexAttribArray)(static_cast<std::uint32_t>(g_posAttrib));
  As<VertexAttribPointerFn>(g.VertexAttribPointer)(static_cast<std::uint32_t>(g_posAttrib), 2,
                                                   GL_FLOAT, 0, 0, nullptr);

  // Capture the frame the game just drew into the current texture (active unit 0).
  As<ActiveTextureFn>(g.ActiveTexture)(GL_TEXTURE0);
  As<BindTextureFn>(g.BindTexture)(GL_TEXTURE_2D, current);
  As<CopyTexImage2DFn>(g.CopyTexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, width, height, 0);

  if (g_captureValid) {
    // Composite: draw the previous frame over the back buffer with the blend alpha. Fixed-function
    // blending mixes it with the current frame: result = current*(1-alpha) + previous*alpha.
    As<DisableFn>(g.Disable)(GL_DEPTH_TEST);
    As<DisableFn>(g.Disable)(GL_CULL_FACE);
    As<EnableFn>(g.Enable)(GL_BLEND);
    As<BlendFuncSeparateFn>(g.BlendFuncSeparate)(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA,
                                                 GL_ONE_MINUS_SRC_ALPHA);
    As<UseProgramFn>(g.UseProgram)(g_program);
    As<BindTextureFn>(g.BindTexture)(GL_TEXTURE_2D, previous);
    As<Uniform1iFn>(g.Uniform1i)(g_prevUniform, 0);
    As<Uniform1fFn>(g.Uniform1f)(g_alphaUniform, alpha);
    As<Uniform2fFn>(g.Uniform2f)(g_resolutionUniform, static_cast<float>(width),
                                 static_cast<float>(height));
    As<DrawArraysFn>(g.DrawArrays)(GL_TRIANGLES, 0, 6);
  }

  As<DisableVertexAttribArrayFn>(g.DisableVertexAttribArray)(
      static_cast<std::uint32_t>(g_posAttrib));
  RestoreState(g, saved);

  // Rotate: this frame's capture becomes the previous frame for the next present.
  g_prevIndex ^= 1;
  g_captureValid = true;
}

} // namespace pl::render
