#include "pl/runtime/GamePlayerRender.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <span>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/Render.hpp"
#include "pl/ipc/SnapshotChannel.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/Signature.hpp"
#include "pl/memory/Vtable.hpp"
#include "pl/render/FramePacing.hpp"
#include "pl/render/FrameTickClock.hpp"
#include "pl/render/RenderDispatch.h"
#include "pl/runtime/GameHookRules.h"
#include "pl/runtime/HookTiming.hpp"
#include "pl/runtime/OnceReport.hpp"
#include "pl/runtime/SwapFrameBoundary.h"

namespace pl::runtime {
namespace {

constexpr const char *kGameModule = "libminecraftpe.so";

// LivePlayerRenderer's render entry. Slot 17 was confirmed against the shipped
// 1.26.60.28 binary (see docs/player-render-hook.md): it is the only slot whose body
// references variable.player_x_rotation / variable.is_first_person / variable.is_using_vr,
// i.e. the routine that poses and draws the player model. The index is overridable from
// the per-version signature rules; the RTTI name resolution means no code address is baked in.
constexpr std::size_t kDefaultRenderSlot = 17;

// The slot was verified on the 1.26.60.x builds. RTTI name resolution means the type is found on
// other builds too, but the slot index can differ; the render hook therefore only installs when the
// slot was confirmed, either from the bundled per-version rules or from a successful runtime probe.
// The gate is deliberately a *floor* rather than a narrow window: builds newer than the verified
// one keep the hook, because the RTTI name still resolves to `LivePlayerRenderer` and a stale index
// is caught by the runtime check below (the slot must resolve to a real function). Widening the
// range is what lets versions like 26.50/26.52 use the native path instead of "nothing draws".
constexpr const char *kVerifiedMinVersion = "1.21.132";
// No upper bound: an empty bound means "unbounded on that side" (see MatchesConfiguredVersion).
constexpr const char *kVerifiedMaxVersion = "";

// Passthrough hook arity. The exact C++ signature of the render entry is not known (the
// game strips its symbols), so the detour forwards the full integer argument register set
// and never inspects any argument. That is what keeps an unknown ABI safe: nothing is
// dereferenced, so a wrong guess cannot read freed memory or mis-handle an object.
using RenderFn = void (*)(void *, void *, void *, void *, void *, void *, void *, void *);

RenderFn g_origRender = nullptr;
std::atomic_bool g_hookInstalled{false};

std::atomic<std::uint32_t> g_renderTick{0};   // increments once per frame (first call)
std::atomic<std::uint32_t> g_frameCalls{0};   // calls seen in the current frame
std::atomic<std::uint32_t> g_totalCalls{0};   // lifetime call count
std::atomic<long long> g_lastRenderMs{0};

// The off-render-thread hand-off. The render thread only ever *publishes* a small self-contained
// record here (a seqlock write, no lock, no allocation); a consumer on another thread reads a
// consistent copy and does its heavier work against that. Without it a consumer reading the game's
// live per-frame state directly would race the render thread — the torn read / use-after-free shape
// that killed modules. The payload is {renderTick, callsThisFrame, totalCalls, lastRenderMs}.
pl::ipc::SnapshotChannel g_frameSnapshot(4 * sizeof(std::uint64_t));

// The frame-boundary clock. The render entry runs once per rendered player model, so the tick is
// inferred from the call timestamps (see FrameTickClock) rather than from a one-second stall.
pl::render::FrameTickClock g_frameClock;

// Which slot the hook resolved to, and how, for the in-app diagnostics report. Set once at install.
std::atomic<std::size_t> g_resolvedSlot{0};
std::atomic_bool g_resolvedByString{false};
std::atomic_bool g_slotKnown{false};

// The resolved render entry address, kept for the hook self-test. 0 until resolved.
std::atomic<std::uintptr_t> g_renderTarget{0};

// The exact gate that refused the render hook, so the diagnostics
// report names "version outside the verified range" rather than a generic "unavailable". 0 = no
// failure (installed or still pending).
std::atomic_int g_renderFailureReason{0};

// The single post-render seam. A fixed small table so registration allocates nothing and the
// render thread never locks. At most 4 consumers (cosmetics + future mods).
constexpr std::size_t kMaxPostRenderCallbacks = 4;
std::atomic<PostRenderCallback> g_postRender[kMaxPostRenderCallbacks];
std::atomic<std::size_t> g_postRenderCount{0};

long long MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

long long MonotonicUs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000LL;
}

// The render thread's own frame-time reading. Fed from the render tick below and read by
// the diagnostics; it measures and reports only, it does not reschedule the engine's frame
// submission (that needs the engine's swap/present call, which the stripped binary does not expose).
pl::render::FramePacingAnalyzer g_framePacing;
std::atomic<int> g_displayRefreshHz{0};
std::atomic<long long> g_framePacingAverageUs{0};
std::atomic<long long> g_framePacingWorstUs{0};
std::atomic<int> g_framePacingFlags{0}; // bit0 jittering, bit1 vsync-locked

// The detour. It runs on the render thread, once per rendered player model. It records the
// tick and call counts, forwards every argument unchanged, then runs the registered post-render
// callbacks.
//
// <b>The callbacks must run on every call, not only on a "new frame".</b> The renderer is invoked
// once per rendered player model, so a frame boundary has to be inferred (see FrameTickClock, which
// uses a 3ms floor). The previous version inferred it as a gap of more than one second, which is
// wrong: during play the renderer runs every ~16 ms, so the "first call of the frame" test was true
// only for the very first frame after a second-long stall — the cosmetics callback therefore ran once
// at startup and then never again, so nothing appeared in game even though the preview worked. The
// cosmetic draw callback dedupes per published frame version itself, so running it on every call draws
// exactly once per launcher tick.
void HookRender(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h) {
  // One clock read per call: the millisecond value the snapshot carries is derived from it, so the
  // detour does not pay a second clock_gettime (audit item 53).
  const long long nowUs = MonotonicUs();
  const long long nowMs = nowUs / 1000;

  // Infer the frame boundary. The entry runs once per rendered player model, so several calls can
  // fall inside one frame; a 3ms floor collapses them to a single tick while still advancing every
  // real frame. The previous "gap beyond 1s" rule left the tick frozen during play, which starved
  // every consumer that keys on it — cape/pet physics and the cosmetics frame version (audit item 1).
  //
  // When the present-call hook is live it already counted this frame, so the tick is taken from it
  // instead: the real boundary, exact even when the visible-model count changes (audit item 113).
  const std::uint64_t swapPresents = pl_runtime_swap_buffers_count.load(std::memory_order_relaxed);
  const bool swapBoundary = pl::runtime::IsSwapFrameBoundaryLive() && swapPresents != 0;
  // Whether this call is the first of a new frame. The render entry runs once per rendered player
  // model, so once-per-frame work is gated on this instead of running once per model.
  bool frameStarted = true;
  if (swapBoundary) {
    // The present hook counts the real frame, so a present count that moved since the last call
    // means this call begins a new frame.
    frameStarted = swapPresents != g_renderTick.load(std::memory_order_relaxed);
    g_frameCalls.store(1, std::memory_order_relaxed);
    g_renderTick.store(static_cast<std::uint32_t>(swapPresents), std::memory_order_relaxed);
    g_frameClock.onRenderCall(nowUs); // keep the fallback clock warm for a later detach
  } else if (g_frameClock.onRenderCall(nowUs, pl::render::FrameTickClock::kDefaultMinIntervalUs,
                                       &frameStarted)) {
    g_frameCalls.store(1, std::memory_order_relaxed);
    g_renderTick.store(g_frameClock.tick(), std::memory_order_relaxed);
  } else {
    g_frameCalls.fetch_add(1, std::memory_order_relaxed);
  }
  g_totalCalls.fetch_add(1, std::memory_order_relaxed);
  g_lastRenderMs.store(nowMs, std::memory_order_relaxed);

  // Feed the frame-time analyzer. The render entry runs once per rendered player model, so
  // a 3ms floor collapses several draws inside one frame to a single sample; the reading is then the
  // real frame interval. The analyzer is plain (render-thread only); the snapshot is atomics.
  {
    const pl::render::FramePacingReading pacing =
        g_framePacing.onFrame(nowUs, g_displayRefreshHz.load(std::memory_order_relaxed), 3000);
    g_framePacingAverageUs.store(pacing.averageFrameUs, std::memory_order_relaxed);
    g_framePacingWorstUs.store(pacing.worstFrameUs, std::memory_order_relaxed);
    int flags = 0;
    if (pacing.jittering) flags |= 1;
    if (pacing.vsyncLocked) flags |= 2;
    g_framePacingFlags.store(flags, std::memory_order_relaxed);
  }

  // Publish a self-contained frame record for off-thread consumers. The render thread does only this
  // bounded seqlock write (no lock, no allocation); the consumer reads a consistent copy elsewhere.
  {
    std::uint64_t record[4];
    record[0] = g_renderTick.load(std::memory_order_relaxed);
    record[1] = g_frameCalls.load(std::memory_order_relaxed);
    record[2] = g_totalCalls.load(std::memory_order_relaxed);
    record[3] = static_cast<std::uint64_t>(nowMs);
    g_frameSnapshot.publish(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(record), sizeof(record)));
  }

  if (g_origRender) {
    g_origRender(a, b, c, d, e, f, g, h);
  }

  // Run the post-render seam on every call. Consumers that draw once per frame dedupe internally
  // (the cosmetics callback keys on the published frame version).
  const std::size_t count = g_postRenderCount.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count && i < kMaxPostRenderCallbacks; ++i) {
    PostRenderCallback callback = g_postRender[i].load(std::memory_order_acquire);
    if (callback == nullptr) continue;
    // In a debug build, time each callback and report one that runs long on the render
    // thread. The clock read and the comparison compile out of a release build (HookTimingEnabled),
    // so the shipped detour is unchanged; the report is only built when the budget was already
    // exceeded, so it never allocates on a steady frame.
    if (pl::runtime::HookTimingEnabled()) {
      const long long startedUs = MonotonicUs();
      callback();
      const long long elapsedUs = MonotonicUs() - startedUs;
      if (pl::runtime::HookOverBudget(elapsedUs)) {
        static OnceReport reported;
        if (reported.shouldReport()) {
          preloaderLogger.warn("{}", pl::runtime::HookBudgetReport("post-render", elapsedUs));
        }
      }
    } else {
      callback();
    }
  }
  // Dispatch the public mod render callbacks (pl/Render.hpp) once per frame, not once per rendered
  // model. The entry runs per model, so dispatching on every call multiplied a mod's per-frame work
  // by the number of visible players (audit item 2.2); the render context (camera, viewport) is the
  // same for every model in a frame, so once is both correct and cheaper.
  if (frameStarted) {
    ::pl::render::detail::DispatchRenderCallbacks();
  }
}

} // namespace

void RegisterPostRenderCallback(PostRenderCallback callback) {
  if (callback == nullptr) return;
  const std::size_t count = g_postRenderCount.load(std::memory_order_acquire);
  // Idempotent per function pointer, and bounded.
  for (std::size_t i = 0; i < count && i < kMaxPostRenderCallbacks; ++i) {
    if (g_postRender[i].load(std::memory_order_acquire) == callback) return;
  }
  if (count >= kMaxPostRenderCallbacks) return;
  g_postRender[count].store(callback, std::memory_order_release);
  g_postRenderCount.store(count + 1, std::memory_order_release);
}

void InitPlayerRenderSource(std::size_t vtableIndex) {
  if (g_hookInstalled.exchange(true, std::memory_order_relaxed)) {
    return;
  }

  // Record exactly which gate refused the hook. 0 means the hook is live (or a refusal
  // reason that predates the field, which prints as "unknown" on the Java side).
  auto refuse = [](int reason) {
    g_renderFailureReason.store(reason, std::memory_order_relaxed);
  };
  constexpr int kReasonVersionGate = 1;
  constexpr int kReasonSlotUnresolved = 2;
  constexpr int kReasonNotExecutable = 3;
  constexpr int kReasonHookFailed = 4;
  // The matching rule declares a render slot but its probe did not resolve the seam to
  // executable code (or the probes block omits it). A probe-backed rule is authoritative, so the
  // feature stays off rather than detouring an unverified index.
  constexpr int kReasonProbeGate = 5;

  // Probe-backed rule gate. Runs before the version check so a rule that carries a
  // failing probe reports that exact reason instead of the generic version-gate one. A rule with
  // no probes block (a legacy/hand-derived rule) passes here and keeps its old behaviour.
  const FeatureGate gate =
      pl::runtime::EvaluateConfiguredFeatureGate(pl::runtime::HookFeature::PlayerRender);
  if (gate == FeatureGate::ProbeUnresolved || gate == FeatureGate::ProbeMissing) {
    refuse(kReasonProbeGate);
    preloaderLogger.warn(
        "Player-render feed: the matching rule's probe for the render seam did not resolve ({}); "
        "native cosmetics stay on the resource-pack path",
        pl::runtime::FeatureGateLabel(gate));
    return;
  }

  // Resolve the render entry first by the diagnostic string its body references, so a build whose
  // slot index moved can still resolve without a per-build number. When the string scan finds
  // nothing (verified: the render diagnostic is not referenced by any LivePlayerRenderer vtable
  // slot on 26.52, so the scan correctly declines there), fall back to the configured index and
  // then the default.
  //
  // The version gate guards only the fallback. A successful live probe (the slot's body
  // references the render diagnostic, and the address is executable) is stronger evidence than the
  // version string, so it installs on any build without a per-build rule. The version floor then
  // only applies to the weaker configured-index path, where a stale index is the risk.
  std::size_t resolvedSlot = 0;
  std::uintptr_t renderFn = pl::memory::resolveVtableSlotReferencingString(
      "18LivePlayerRenderer", "variable.player_x_rotation", kGameModule, 200, &resolvedSlot);
  std::size_t slot = resolvedSlot;
  bool byString = renderFn != 0;
  if (!renderFn) {
    // Fallback path: only the builds whose slot index is verified may be hooked. An unverified
    // build fails safe to the resource-pack path rather than installing a detour on an unrelated
    // slot.
    if (!MatchesConfiguredVersion(kVerifiedMinVersion, kVerifiedMaxVersion)) {
      refuse(kReasonVersionGate);
      preloaderLogger.warn(
          "Player-render feed: running build is outside the verified range {}-{} and the live "
          "probe found no render slot; native cosmetics stay on the resource-pack path",
          kVerifiedMinVersion, kVerifiedMaxVersion);
      return;
    }
    slot = vtableIndex != 0 ? vtableIndex : kDefaultRenderSlot;
    renderFn = pl::memory::resolveVtableFunction("18LivePlayerRenderer", slot, kGameModule);
  }
  if (!renderFn) {
    refuse(kReasonSlotUnresolved);
    preloaderLogger.warn(
        "Player-render feed: LivePlayerRenderer slot unresolved; native cosmetics "
        "stay on the resource-pack path");
    return;
  }
  // The RTTI name resolves on every build, but the slot index can differ; on an unverified build a
  // stale index can land on a data pointer. Refuse to detour anything that is not executable code,
  // so the widened version gate cannot turn a wrong slot into a render-thread fault.
  if (!pl::memory::isExecutableAddress(renderFn, kGameModule)) {
    refuse(kReasonNotExecutable);
    preloaderLogger.warn(
        "Player-render feed: LivePlayerRenderer slot {} did not resolve to executable code; "
        "skipping the hook",
        slot);
    return;
  }

  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(renderFn),
                       reinterpret_cast<pl::memory::FuncPtr>(HookRender),
                       reinterpret_cast<pl::memory::FuncPtr *>(&g_origRender),
                       pl::memory::HookPriority::Normal,
                       "LivePlayerRenderer::render") != 0) {
    refuse(kReasonHookFailed);
    preloaderLogger.warn(
        "Player-render feed: hook install failed; native cosmetics stay on the "
        "resource-pack path");
  } else {
    preloaderLogger.info("Player-render feed: LivePlayerRenderer::render hooked (slot {})",
                         slot);
    g_renderFailureReason.store(0, std::memory_order_relaxed);
    g_resolvedSlot.store(slot, std::memory_order_relaxed);
    g_resolvedByString.store(byString, std::memory_order_relaxed);
    g_slotKnown.store(true, std::memory_order_relaxed);
    g_renderTarget.store(renderFn, std::memory_order_relaxed);
  }
}

int RenderHookFailureReason() {
  return g_renderFailureReason.load(std::memory_order_relaxed);
}

std::uintptr_t PlayerRenderHookTarget() {
  return g_renderTarget.load(std::memory_order_relaxed);
}

bool IsPlayerRenderHookInstalled() {
  return g_hookInstalled.load(std::memory_order_relaxed) &&
         g_renderFailureReason.load(std::memory_order_relaxed) == 0;
}

void SetDisplayRefreshHz(int hz) {
  g_displayRefreshHz.store(hz > 0 ? hz : 0, std::memory_order_relaxed);
}

bool ReadFramePacing(long long &outAverageUs, long long &outWorstUs, bool &outJittering,
                     bool &outVsyncLocked) {
  const long long average = g_framePacingAverageUs.load(std::memory_order_relaxed);
  if (average <= 0) return false;
  outAverageUs = average;
  outWorstUs = g_framePacingWorstUs.load(std::memory_order_relaxed);
  const int flags = g_framePacingFlags.load(std::memory_order_relaxed);
  outJittering = (flags & 1) != 0;
  outVsyncLocked = (flags & 2) != 0;
  return true;
}

bool ReadFramePacingPercentiles(long long &outP50Us, long long &outP95Us, long long &outP99Us,
                                int &outHitchesPerMinute) {
  const pl::render::FramePacingReading reading = g_framePacing.percentiles();
  if (reading.sampleCount == 0 || reading.p50Us <= 0) return false;
  outP50Us = reading.p50Us;
  outP95Us = reading.p95Us;
  outP99Us = reading.p99Us;
  outHitchesPerMinute = reading.hitchesPerMinute;
  return true;
}

bool LastResolvedRenderSlot(std::size_t &outSlot, bool &outByString) {
  if (!g_slotKnown.load(std::memory_order_relaxed)) return false;
  outSlot = g_resolvedSlot.load(std::memory_order_relaxed);
  outByString = g_resolvedByString.load(std::memory_order_relaxed);
  return true;
}

bool IsPlayerRenderHookLive() {
  return g_lastRenderMs.load(std::memory_order_relaxed) != 0;
}

bool ReadPlayerRenderStats(std::uint32_t out[4]) {
  const long long last = g_lastRenderMs.load(std::memory_order_relaxed);
  if (last == 0) {
    return false;
  }
  out[0] = g_renderTick.load(std::memory_order_relaxed);
  out[1] = g_frameCalls.load(std::memory_order_relaxed);
  out[2] = g_totalCalls.load(std::memory_order_relaxed);
  const long long delta = MonotonicMs() - last;
  out[3] = delta < 0 ? 0 : static_cast<std::uint32_t>(delta);
  return true;
}

long long LastPlayerRenderMs() {
  return g_lastRenderMs.load(std::memory_order_relaxed);
}

bool ReadFrameSnapshot(std::uint64_t out[4]) {
  if (out == nullptr) return false;
  std::vector<std::uint8_t> copy;
  if (!g_frameSnapshot.read(copy) || copy.size() != 4 * sizeof(std::uint64_t)) return false;
  std::memcpy(out, copy.data(), sizeof(std::uint64_t) * 4);
  return true;
}

} // namespace pl::runtime
