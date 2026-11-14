#pragma once

/**
 * @file GlesProbe.hpp
 * @brief Runtime probe of the device's graphics backend, for diagnostics only.
 *
 * <p>The GLES entry-point table and the backend classification used to live in the cosmetic mesh
 * renderer. That renderer is gone (the engine texture swap is the only route a cosmetic takes now),
 * but the diagnostics report still has to answer two questions a device run depends on: which
 * backend the game is drawing with, and whether a GLES surface is even available. This module keeps
 * exactly that probe and nothing else - it resolves symbols, it never draws.
 *
 * <p><b>Fail-closed.</b> If no GLES library can be loaded (Vulkan-only RenderDragon builds), every
 * pointer is null, {@link GlesFunctions::usable} is false, and the backend reads as Vulkan or
 * unknown. Nothing here dereferences a guessed address.
 */

#include <cstdint>
#include <string>

#include "pl/Export.hpp"
#include "pl/render/GpuFamily.hpp"

namespace pl::render {

/**
 * @brief Minimal GL scalar types.
 *
 * Deliberately self-contained: the sweetloader does not include a GLES header (the host test runner
 * has none), and the probe only needs the scalar/enum integers to marshal the runtime-resolved
 * GLES function pointers.
 */
using GLenum = std::uint32_t;
using GLboolean = std::uint8_t;
using GLint = std::int32_t;
using GLsizei = std::int32_t;
using GLuint = std::uint32_t;
using GLsizeiptr = std::intptr_t;
using GLfloat = float;

/**
 * @brief The runtime-resolved GLES entry points the probe exposes.
 *
 * Resolved once from the game's active GLES library; a null entry point makes that primitive a
 * no-op, so a build with a partial context still fails closed. Using function pointers keeps the
 * sweetloader from hard-linking to a GLES version that may not exist on the device's renderer.
 */
struct GlesFunctions {
  void *GetError = nullptr;
  void *GetString = nullptr;
  void *GetIntegerv = nullptr;
  void *Enable = nullptr;
  void *Disable = nullptr;
  void *CullFace = nullptr;
  void *DepthMask = nullptr;
  void *GetBooleanv = nullptr;
  void *GenBuffers = nullptr;
  void *BindBuffer = nullptr;
  void *BufferData = nullptr;
  void *GenVertexArrays = nullptr;
  void *BindVertexArray = nullptr;
  void *EnableVertexAttribArray = nullptr;
  void *DisableVertexAttribArray = nullptr;
  void *VertexAttribPointer = nullptr;
  void *UseProgram = nullptr;
  void *GetUniformLocation = nullptr;
  void *UniformMatrix4fv = nullptr;
  void *Uniform4f = nullptr;
  void *Uniform3f = nullptr;
  void *Uniform1i = nullptr;
  void *Uniform1f = nullptr;
  void *Uniform2f = nullptr;
  void *ActiveTexture = nullptr;
  void *BindTexture = nullptr;
  void *GenTextures = nullptr;
  void *TexParameteri = nullptr;
  void *TexImage2D = nullptr;
  void *Viewport = nullptr;
  void *DrawElements = nullptr;
  void *DrawArrays = nullptr;
  void *BlendFunc = nullptr;
  void *BlendFuncSeparate = nullptr;
  void *DeleteBuffers = nullptr;
  void *DeleteVertexArrays = nullptr;
  void *DeleteTextures = nullptr;
  // Present-frame effect path (frame generation / motion blur): the shader program that samples the
  // captured frame, and the framebuffer/texture copy used to keep it. Resolved here rather than in
  // the effect so the probe stays the single GL symbol table.
  void *CreateShader = nullptr;
  void *ShaderSource = nullptr;
  void *CompileShader = nullptr;
  void *GetShaderiv = nullptr;
  void *CreateProgram = nullptr;
  void *AttachShader = nullptr;
  void *LinkProgram = nullptr;
  void *GetProgramiv = nullptr;
  void *GetAttribLocation = nullptr;
  void *GenFramebuffers = nullptr;
  void *BindFramebuffer = nullptr;
  void *FramebufferTexture2D = nullptr;
  void *CopyTexImage2D = nullptr;
  void *TexParameterf = nullptr;
  void *ClearColor = nullptr;
  void *Clear = nullptr;

  /** @brief True when the core query path is available (nothing essential is null). */
  [[nodiscard]] bool usable() const;
};

/**
 * @brief Resolves the active GLES context's entry points via the platform loader.
 *
 * Fail-closed: if no GLES library can be loaded (Vulkan-only RenderDragon builds), every pointer is
 * null and {@link GlesFunctions::usable} is false.
 */
PL_EXPORT GlesFunctions ResolveGles();

/** @brief GL backend classification for the diagnostics report. */
enum class GlBackend : int {
  Unknown = 0,  ///< no GLES library resolved and no Vulkan library found
  Gles = 1,     ///< the GLES function table resolved (an ES renderer)
  Vulkan = 2,   ///< no GLES, but libvulkan.so is loaded (a Vulkan-only RenderDragon build)
};

/**
 * @brief True when an EGL context is current on this thread.
 *
 * Resolved via EGL's {@code eglGetCurrentContext}; when the EGL symbol is unavailable this returns
 * false (fail-closed: never claim a context we cannot confirm).
 */
PL_EXPORT bool IsEglContextCurrent();

/** @brief The GL backend classification, for the diagnostics report. */
PL_EXPORT GlBackend GlesBackend();

/**
 * @brief The device GPU's vendor and renderer strings, read once and cached.
 *
 * The GLES spec guarantees both are available through `glGetString` while a context is current, and
 * they never change for a process, so they are read once and kept. An empty string means the read
 * did not happen (no context, or no GLES table), never a fabricated value.
 */
struct GpuStrings {
  std::string vendor;
  std::string renderer;
  /** True when at least one of the two was actually read from the driver. */
  bool read = false;
};

/**
 * @brief The cached vendor/renderer strings, reading them on the first call that has a context.
 *
 * Reading them needs a current GLES context, so a call before the surface is up returns what it has
 * (possibly empty) and a later call retries; once read, the values are kept for the process.
 */
PL_EXPORT GpuStrings ReadGpuStrings();

/**
 * @brief The classified GPU family from the cached strings.
 *
 * Falls back to {@link GpuFamily::Unknown} when the strings have not been read, so a caller never
 * gets a family derived from nothing.
 */
PL_EXPORT GpuFamily GpuFamilyOf();

/** @brief A stable lower-case name for the classified GPU family, for a report field. */
PL_EXPORT const char *GpuFamilyName();

/** @brief The last GLES error code observed, or 0 when clean. */
PL_EXPORT int LastGlesError();

} // namespace pl::render
