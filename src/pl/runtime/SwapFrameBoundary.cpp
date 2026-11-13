/**
 * @file SwapFrameBoundary.cpp
 * @brief Installs a hook on the engine's present call to get the true frame boundary.
 *
 * The player-render detour fires once per rendered player model, so the frame boundary has to be
 * inferred and the inference is wrong whenever the visible-model count changes. `eglSwapBuffers` is
 * called exactly once per presented frame and is an exported libEGL symbol, so hooking it gives the
 * real boundary. This is audit item 111, and it is the input every other frame-time item needs.
 *
 * The detour target is the register-preserving veneer, not a C++ function: the present call returns
 * an `EGLBoolean` in `w0` and may take an FP argument, and a C++ detour would clobber both.
 */

#include "pl/runtime/SwapFrameBoundary.h"

#include <dlfcn.h>

#include <atomic>

#include "pl/Logger.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/runtime/RegisterAwareHook.h"

namespace pl::runtime {
namespace {

std::atomic_bool g_installed{false};
std::atomic_bool g_live{false};
std::atomic<const char *> g_symbol{""};

// Resolves the first present symbol libEGL exports. `eglSwapBuffersWithDamageKHR` is preferred when
// present because some drivers route the real present through it, but the plain `eglSwapBuffers` is
// the one every driver has, so it is the fallback rather than the other way round.
void *ResolveSwapSymbol(const char **outName) {
  void *handle = nullptr;
  for (const char *name : {"libEGL.so", "libEGL.so.1"}) {
    handle = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) handle = dlopen(name, RTLD_NOW);
    if (handle) break;
  }
  if (handle == nullptr) return nullptr;

  for (const char *name : {"eglSwapBuffers", "eglSwapBuffersWithDamageKHR"}) {
    void *symbol = dlsym(handle, name);
    if (symbol != nullptr) {
      *outName = name;
      return symbol;
    }
  }
  return nullptr;
}

} // namespace

bool InstallSwapFrameBoundary() {
  if (g_installed.exchange(true, std::memory_order_relaxed)) {
    return g_live.load(std::memory_order_relaxed);
  }

  const char *name = "";
  void *target = ResolveSwapSymbol(&name);
  if (target == nullptr) {
    // Vulkan-only RenderDragon builds have no libEGL present call. That is a normal outcome, not an
    // error: the caller keeps the inferred frame clock.
    sweetLoaderLogger.info(
        "Swap frame boundary: no libEGL eglSwapBuffers to hook; keeping the inferred frame clock");
    return false;
  }

  // The veneer is the detour target; the trampoline goes into the veneer's own context so the veneer
  // can tail-branch to the original.
  void (*original)(void) = nullptr;
  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(target),
                       reinterpret_cast<pl::memory::FuncPtr>(
                           pl_runtime_passthrough_swapBuffers_addr),
                       reinterpret_cast<pl::memory::FuncPtr *>(&original),
                       pl::memory::HookPriority::Normal, "eglSwapBuffers") != 0) {
    sweetLoaderLogger.warn("Swap frame boundary: hook install failed for {}", name);
    return false;
  }
  pl_runtime_passthrough_ctx_swapBuffers.original = original;
  pl_runtime_passthrough_ctx_swapBuffers.label = "eglSwapBuffers";
  g_symbol.store(name, std::memory_order_relaxed);
  g_live.store(true, std::memory_order_relaxed);
  sweetLoaderLogger.info("Swap frame boundary: hooked {}", name);
  return true;
}

bool IsSwapFrameBoundaryLive() { return g_live.load(std::memory_order_relaxed); }

SwapFrameBoundaryStats ReadSwapFrameBoundaryStats() {
  SwapFrameBoundaryStats stats;
  stats.installed = g_live.load(std::memory_order_relaxed);
  stats.symbol = g_symbol.load(std::memory_order_relaxed);
  stats.presents = pl_runtime_swap_buffers_count.load(std::memory_order_relaxed);
  stats.lastPresentUs = pl_runtime_swap_buffers_last_us.load(std::memory_order_relaxed);
  return stats;
}

} // namespace pl::runtime
