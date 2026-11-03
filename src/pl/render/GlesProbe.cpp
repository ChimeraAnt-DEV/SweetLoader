/**
 * @file GlesProbe.cpp
 * @brief Runtime GLES/EGL probe for the diagnostics report.
 *
 * Extracted from the retired cosmetic mesh renderer: the renderer drew with these entry points, the
 * probe only classifies them. Nothing here draws, allocates GL objects, or dereferences a guessed
 * address - it dlopens the GLES/EGL/Vulkan libraries and resolves the symbols the report reads.
 */

#include "pl/render/GlesProbe.hpp"

#include <atomic>
#include <mutex>
#include <string>

#include <dlfcn.h>

#include "pl/Logger.hpp"

namespace pl::render {
namespace {

GlesFunctions g_gles;

void *LoadGlesLibrary() {
  for (const char *candidate : {"libGLESv3.so", "libGLESv2.so", "libGLESv1_CM.so"}) {
    void *handle = dlopen(candidate, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) handle = dlopen(candidate, RTLD_NOW);
    if (handle) return handle;
  }
  return nullptr;
}

void *GlesSymbol(void *handle, const char *name) {
  return handle ? dlsym(handle, name) : nullptr;
}

/// Set when {@link LastGlesError} is called, so a clean run reads 0 without touching GL.
std::atomic<int> g_lastGlesError{0};

// The vendor/renderer strings never change for a process, so they are read once and the
// classification is derived from the cached copy. A mutex guards the one-time read; every later
// caller takes the fast path (a relaxed load of `g_gpuStringsRead`) and pays nothing.
std::mutex g_gpuStringsMutex;
GpuStrings g_gpuStrings;
std::atomic<bool> g_gpuStringsRead{false};
std::atomic<int> g_gpuFamily{-1};

/// Reads a GL string into a std::string, or returns empty when the pointer is null.
std::string ReadGlString(void *fn, GLenum name) {
  if (fn == nullptr) return {};
  using GetStringFn = const unsigned char *(*)(GLenum);
  const auto getString = reinterpret_cast<GetStringFn>(fn);
  const unsigned char *value = getString(name);
  if (value == nullptr) return {};
  return std::string(reinterpret_cast<const char *>(value));
}

} // namespace

bool GlesFunctions::usable() const {
  return GetError && GetIntegerv && Enable && Disable && GenBuffers && BindBuffer &&
         BufferData && GenVertexArrays && BindVertexArray && EnableVertexAttribArray &&
         VertexAttribPointer && UseProgram && GetUniformLocation && UniformMatrix4fv &&
         ActiveTexture && BindTexture && GenTextures && TexParameteri && TexImage2D &&
         DrawElements && Viewport;
}

GlesFunctions ResolveGles() {
  if (g_gles.usable()) return g_gles;

  void *handle = LoadGlesLibrary();
  if (!handle) {
    preloaderLogger.warn("GLES probe: no GLES library loaded (RenderDragon may be Vulkan-only)");
    return g_gles;
  }

#define LOAD(name, member) \
  g_gles.member = reinterpret_cast<decltype(g_gles.member)>(GlesSymbol(handle, name))
  LOAD("glGetError", GetError);
  LOAD("glGetString", GetString);
  LOAD("glGetIntegerv", GetIntegerv);
  LOAD("glEnable", Enable);
  LOAD("glDisable", Disable);
  LOAD("glCullFace", CullFace);
  LOAD("glDepthMask", DepthMask);
  LOAD("glGetBooleanv", GetBooleanv);
  LOAD("glGenBuffers", GenBuffers);
  LOAD("glBindBuffer", BindBuffer);
  LOAD("glBufferData", BufferData);
  LOAD("glGenVertexArrays", GenVertexArrays);
  LOAD("glBindVertexArray", BindVertexArray);
  LOAD("glEnableVertexAttribArray", EnableVertexAttribArray);
  LOAD("glVertexAttribPointer", VertexAttribPointer);
  LOAD("glUseProgram", UseProgram);
  LOAD("glGetUniformLocation", GetUniformLocation);
  LOAD("glUniformMatrix4fv", UniformMatrix4fv);
  LOAD("glUniform4f", Uniform4f);
  LOAD("glUniform3f", Uniform3f);
  LOAD("glUniform1i", Uniform1i);
  LOAD("glActiveTexture", ActiveTexture);
  LOAD("glBindTexture", BindTexture);
  LOAD("glGenTextures", GenTextures);
  LOAD("glTexParameteri", TexParameteri);
  LOAD("glTexImage2D", TexImage2D);
  LOAD("glViewport", Viewport);
  LOAD("glDrawElements", DrawElements);
  LOAD("glDrawArrays", DrawArrays);
  LOAD("glBlendFunc", BlendFunc);
  LOAD("glDeleteBuffers", DeleteBuffers);
  LOAD("glDeleteVertexArrays", DeleteVertexArrays);
  LOAD("glDeleteTextures", DeleteTextures);
#undef LOAD

  if (g_gles.usable()) {
    preloaderLogger.info("GLES probe: surface resolved and usable");
  } else {
    preloaderLogger.warn("GLES probe: GLES library loaded but core symbols missing");
  }
  return g_gles;
}

bool IsEglContextCurrent() {
  // eglGetCurrentContext returns EGL_NO_CONTEXT (0) when no context is current on this thread.
  // Loaded through dlopen so the probe never hard-links EGL and a build without it reads false.
  static void *handle = []() -> void * {
    for (const char *name : {"libEGL.so", "libEGL.so.1"}) {
      void *h = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
      if (!h) h = dlopen(name, RTLD_NOW);
      if (h) return h;
    }
    return nullptr;
  }();
  if (!handle) return false;
  using GetCurrentContextFn = void *(*)();
  auto getCurrentContext = reinterpret_cast<GetCurrentContextFn>(dlsym(handle, "eglGetCurrentContext"));
  if (!getCurrentContext) return false;
  return getCurrentContext() != nullptr;
}

GlBackend GlesBackend() {
  if (g_gles.usable()) return GlBackend::Gles;
  // No GLES table: distinguish "Vulkan-only RenderDragon" from "nothing resolved" by probing for a
  // loaded Vulkan library, which is what a Vulkan-only build links.
  for (const char *name : {"libvulkan.so", "libvulkan.so.1"}) {
    void *h = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen(name, RTLD_NOW);
    if (h) return GlBackend::Vulkan;
  }
  return GlBackend::Unknown;
}

int LastGlesError() {
  return g_lastGlesError.load(std::memory_order_relaxed);
}

GpuStrings ReadGpuStrings() {
  // Fast path: already read, so no lock and no GL call.
  if (g_gpuStringsRead.load(std::memory_order_acquire)) return g_gpuStrings;

  // Reading needs a current context. Resolve the table first (the caller may not have).
  const GlesFunctions gles = ResolveGles();
  if (gles.GetString == nullptr) return g_gpuStrings;

  std::lock_guard<std::mutex> lock(g_gpuStringsMutex);
  if (g_gpuStringsRead.load(std::memory_order_relaxed)) return g_gpuStrings;

  // GL_VENDOR 0x1F00, GL_RENDERER 0x1F01.
  const std::string vendor = ReadGlString(gles.GetString, 0x1F00U);
  const std::string renderer = ReadGlString(gles.GetString, 0x1F01U);
  if (vendor.empty() && renderer.empty()) {
    // No context yet: leave it unread so a later call retries rather than caching an empty answer.
    return g_gpuStrings;
  }
  g_gpuStrings.vendor = vendor;
  g_gpuStrings.renderer = renderer;
  g_gpuStrings.read = true;
  g_gpuFamily.store(static_cast<int>(ClassifyGpuFamily(vendor, renderer)), std::memory_order_relaxed);
  g_gpuStringsRead.store(true, std::memory_order_release);

  const auto family = static_cast<GpuFamily>(g_gpuFamily.load(std::memory_order_relaxed));
  preloaderLogger.info("GLES probe: GPU vendor '{}', renderer '{}' -> family {}",
                       g_gpuStrings.vendor, g_gpuStrings.renderer, GpuFamilyName(family));
  return g_gpuStrings;
}

GpuFamily GpuFamilyOf() {
  // Ensure the strings have been read (or attempted); then answer from the cached classification.
  if (!g_gpuStringsRead.load(std::memory_order_acquire)) {
    ReadGpuStrings();
  }
  const int family = g_gpuFamily.load(std::memory_order_relaxed);
  if (family < 0) return GpuFamily::Unknown;
  return static_cast<GpuFamily>(family);
}

const char *GpuFamilyName() { return pl::render::GpuFamilyName(GpuFamilyOf()); }

} // namespace pl::render
