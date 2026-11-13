/**
 * @file CameraSource.cpp
 * @brief One-time camera matrix resolution + cheap per-frame read.
 */

#include "pl/render/CameraSource.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/memory/ModulePathMatch.hpp"
#include "pl/render/CameraBlockScan.hpp"

namespace pl::render {
namespace {

constexpr const char *kGameModule = "libminecraftpe.so";

struct Region {
  std::uintptr_t start = 0;
  std::uintptr_t end = 0;
};

// The resolved camera block: a view matrix immediately followed by a projection matrix, in the
// game's writable data. 0 when nothing was found.
std::atomic<std::uintptr_t> g_viewAddress{0};
std::atomic<std::uintptr_t> g_projectionAddress{0};
std::once_flag g_scanOnce;

void ReadableRegionsFor(const std::string &moduleName, std::vector<Region> &out) {
  std::FILE *maps = std::fopen("/proc/self/maps", "r");
  if (!maps) return;
  char line[4096];
  while (std::fgets(line, sizeof(line), maps)) {
    // Match the module by the basename of the maps path column, not `strstr`: the latter also
    // matches `libminecraftpe.so.bak`, a path that merely contains the name, or a tag such as
    // `[anon:libminecraftpe.so]`, which would scan the wrong regions.
    pl::memory::MapsLine parsed{};
    if (!pl::memory::ParseMapsLine(line, parsed)) continue;
    if (!pl::memory::MapsPathNamesModule(parsed.path, moduleName)) continue;
    if (parsed.end <= parsed.start) continue;
    // The camera lives in writable data (.data / .bss), not code. Scanning r-xp as well would
    // only add false positives from constant pools.
    if (!pl::memory::MapsPermsReadable(parsed.perms) ||
        !pl::memory::MapsPermsWritable(parsed.perms)) {
      continue;
    }
    out.push_back(Region{parsed.start, parsed.end});
  }
  std::fclose(maps);
}

bool Finite(float v) { return std::isfinite(v); }

} // namespace

bool CameraSource::ValidateProjection(const float *m) {
  return IsPerspectiveProjection(m);
}

bool CameraSource::ValidateView(const float *m) {
  return IsRigidView(m);
}

void CameraSource::ResolveOnce() {
  std::call_once(g_scanOnce, [] {
    std::vector<Region> regions;
    ReadableRegionsFor(kGameModule, regions);
    if (regions.empty()) {
      sweetLoaderLogger.info("Camera source: no writable game regions to scan");
      return;
    }

    for (const Region &region : regions) {
      const std::size_t count = (region.end - region.start) / sizeof(float);
      if (count < 32) continue;
      const float *base = reinterpret_cast<const float *>(region.start);
      const std::size_t found = FindViewProjectionPair(base, count);
      if (found == kNoCameraPair) continue;

      const std::uintptr_t viewAddr = region.start + found * sizeof(float);
      const std::uintptr_t projAddr = viewAddr + 16 * sizeof(float);
      g_viewAddress.store(viewAddr, std::memory_order_release);
      g_projectionAddress.store(projAddr, std::memory_order_release);
      sweetLoaderLogger.info("Camera source: resolved view {:x} projection {:x}", viewAddr, projAddr);
      return;
    }
    sweetLoaderLogger.info(
        "Camera source: no view/projection pair found; callers fall back to ComputeViewProjection");
  });
}

void CameraSource::ResolveAsync() {
  // The scan walks hundreds of megabytes of writable data and must never run on the render thread.
  // A detached worker runs the one-time scan; the render path reads whatever is published when it is
  // ready and falls back to ComputeViewProjection until then. call_once inside ResolveOnce makes a
  // later synchronous ResolveOnce (and this worker) agree on a single scan.
  static std::once_flag s_requested;
  std::call_once(s_requested, [] {
    std::thread([] {
      sweetLoaderLogger.info("Camera source: background scan started");
      ResolveOnce();
    }).detach();
  });
}

bool CameraSource::HasResolvedAddress() {
  return g_viewAddress.load(std::memory_order_acquire) != 0 &&
         g_projectionAddress.load(std::memory_order_acquire) != 0;
}

CameraSource::Frame CameraSource::ReadFrame() {
  Frame frame;
  const std::uintptr_t viewAddr = g_viewAddress.load(std::memory_order_acquire);
  const std::uintptr_t projAddr = g_projectionAddress.load(std::memory_order_acquire);
  if (viewAddr == 0 || projAddr == 0) return frame;

  const float *view = reinterpret_cast<const float *>(viewAddr);
  const float *projection = reinterpret_cast<const float *>(projAddr);
  // Re-validate every frame: the cached address could have been freed or repurposed if the
  // renderer was torn down, and a stale read must not become a wrong matrix.
  if (!ValidateView(view) || !ValidateProjection(projection)) return frame;

  std::memcpy(frame.view.m, view, sizeof(float) * 16);
  std::memcpy(frame.projection.m, projection, sizeof(float) * 16);
  frame.hasView = true;
  frame.hasProjection = true;
  frame.viewProjection = pl::cosmetics::Mat4::multiply(frame.projection, frame.view);
  frame.hasViewProjection = frame.viewProjection.isFinite();
  return frame;
}

} // namespace pl::render
