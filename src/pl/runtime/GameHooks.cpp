#include "pl/runtime/GameHooks.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/cosmetics/NativeRenderHook.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/Signature.hpp"
#include "pl/memory/Vtable.hpp"
#include "pl/runtime/FeatureAvailability.hpp"
#include "pl/runtime/GameHookRules.h"
#include "pl/runtime/GameCosmetics.h"
#include "pl/runtime/GameLocalPlayer.h"
#include "pl/runtime/SwapFrameBoundary.h"
#include "pl/runtime/GamePlayerRender.h"
#include "pl/runtime/HookInstallGate.hpp"
#include "pl/runtime/HookSelfTest.hpp"

namespace pl::runtime {
namespace {

std::atomic_bool g_isPauseMenuOpen{false};
std::atomic_bool g_isHudScreenOpen{false};
std::atomic_bool g_isShowingMenu{false};
std::atomic_bool g_forceGlobalModMenu{false};
std::once_flag g_gameHooksOnce;
std::once_flag g_armOnce;

// The deferred-install gate. Armed at sweetloader init; ticked from the game's frame pump. Guarded by
// an atomic flag so the common per-frame path is a single load and returns.
HookInstallGate g_installGate;
std::atomic_bool g_installArmed{false};
std::atomic_bool g_installResolved{false};
std::atomic_int g_installGiveUps{0};
// The overlay-hook batch's outcome, recorded for the per-feature availability report.
// "Attempted" and "installed" are different facts: the batch installs atomically, so a partial
// failure rolls back and the overlay feature is simply unavailable.
std::atomic_bool g_overlayHooksInstalled{false};

long long MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

void (*orig_PauseMenuDtor)(void *) = nullptr;
void hook_PauseMenuDtor(void *_this) {
  g_isPauseMenuOpen.store(false, std::memory_order_relaxed);
  if (orig_PauseMenuDtor) {
    orig_PauseMenuDtor(_this);
  }
}

void (*orig_PauseMenuOpen)(void *) = nullptr;
void hook_PauseMenuOpen(void *_this) {
  g_isPauseMenuOpen.store(true, std::memory_order_relaxed);
  g_isShowingMenu.store(true, std::memory_order_relaxed);
  if (orig_PauseMenuOpen) {
    orig_PauseMenuOpen(_this);
  }
}

void (*orig_HudScreenDtor)(void *) = nullptr;
void hook_HudScreenDtor(void *_this) {
  g_isHudScreenOpen.store(false, std::memory_order_relaxed);
  g_isPauseMenuOpen.store(false, std::memory_order_relaxed);
  g_isShowingMenu.store(false, std::memory_order_relaxed);
  if (orig_HudScreenDtor) {
    orig_HudScreenDtor(_this);
  }
}

void (*orig_HudScreenOpen)(void *) = nullptr;
void hook_HudScreenOpen(void *_this) {
  g_isHudScreenOpen.store(true, std::memory_order_relaxed);
  g_isPauseMenuOpen.store(false, std::memory_order_relaxed);
  g_isShowingMenu.store(false, std::memory_order_relaxed);
  if (orig_HudScreenOpen) {
    orig_HudScreenOpen(_this);
  }
}

bool (*orig_isShowingMenu)(void *) = nullptr;
bool hook_isShowingMenu(void *_this) {
  bool res = false;
  if (orig_isShowingMenu) {
    res = orig_isShowingMenu(_this);
  }
  g_isShowingMenu.store(res, std::memory_order_relaxed);
  return res;
}

uintptr_t ResolveResult(
    const std::unordered_map<std::string, uintptr_t> &results,
    const std::string &signature) {
  auto it = results.find(signature);
  return it == results.end() ? 0 : it->second;
}

bool InstallHook(uintptr_t target, pl::memory::FuncPtr detour,
                 pl::memory::FuncPtr *original,
                 const char *name) {
  if (!target) {
    sweetLoaderLogger.warn("SweetLoader hook target is missing: {}", name);
    return false;
  }

  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(target), detour,
                       original, pl::memory::HookPriority::Normal, name) != 0) {
    sweetLoaderLogger.warn("Failed to install SweetLoader hook: {}", name);
    return false;
  }
  return true;
}

// One hook in a batch. Kept together with its rollback inputs so an install that
// fails part-way can be fully undone instead of leaving the process half-patched.
struct PendingHook {
  uintptr_t target;
  pl::memory::FuncPtr detour;
  pl::memory::FuncPtr *original;
  const char *name;
};

bool InstallHookBatch(const std::vector<PendingHook> &batch) {
  std::vector<const PendingHook *> installed;
  installed.reserve(batch.size());

  for (const auto &h : batch) {
    if (!InstallHook(h.target, h.detour, h.original, h.name)) {
      sweetLoaderLogger.error(
          "SweetLoader hook install aborted at '{}'; rolling back {} hook(s) already "
          "installed",
          h.name, installed.size());
      for (const auto *done : installed) {
        pl::memory::unhook(reinterpret_cast<pl::memory::FuncPtr>(done->target),
                           done->detour);
      }
      return false;
    }
    installed.push_back(&h);
  }
  return true;
}

} // namespace

/** Defined below; forward-declared so the install can round-trip the sites before hooking them. */
void CacheHookSelfTest(const std::vector<HookSite> &sites);

void ConfigureGameHooks(std::string rulesPath, std::string minecraftVersion) {
  ConfigureGameHookRules(std::move(rulesPath), std::move(minecraftVersion));
  g_forceGlobalModMenu.store(false, std::memory_order_relaxed);
}

void InitGameHooks() {
  // Rules gate BEFORE the once, deliberately: if the launcher has not delivered the signature
  // rules yet (the library was loaded after the rules call, a stale build, or a different
  // process), install nothing AND do not consume the once. A later PumpGameHookInstall after
  // delivery then gets a clean attempt instead of permanently skipping cosmetics for the session.
  if (!GameHookRulesConfigured()) {
    sweetLoaderLogger.warn(
        "SweetLoader signature rules were not delivered; deferring hook install (will retry "
        "when they arrive)");
    return;
  }

  std::call_once(g_gameHooksOnce, [] {
    g_installResolved.store(true, std::memory_order_release);
    g_forceGlobalModMenu.store(false, std::memory_order_relaxed);

    // The local-player feed is independent of the pause/HUD hooks and is useful on
    // its own (the voice nametag icons), so it is installed even when the overlay
    // hooks below cannot be resolved.
    InitLocalPlayerSource();
    auto signatures = LoadConfiguredGameHookSignatures();
    if (!signatures) {
      return;
    }

    // The player-render feed is independent of the overlay hooks: the native cosmetics path
    // uses it to know the renderer is live and to drive its physics off a real render tick. It
    // is fail-closed, so a build where the slot does not resolve simply keeps the pack path.
    InitPlayerRenderSource(signatures->playerRenderVtableIndex.value_or(0));

    // The true frame boundary, from the engine's present call (audit item 111). Installed before the
    // cosmetics hook so the frame clock it feeds is live as soon as the renderer is. Fail-closed: a
    // Vulkan-only build with no libEGL present call keeps the inferred frame clock.
    pl::runtime::InstallSwapFrameBoundary();

    // The native cosmetics render interception (Task 2). It hooks the same LivePlayerRenderer slot
    // the render feed above uses, which is why the hook backend chains: the two detours both run,
    // in install order. Ownership is fail-closed, so a build outside the verified range keeps the
    // resource-pack path.
    pl::cosmetics::InitNativeCosmeticRenderHook(
        signatures->playerRenderVtableIndex.value_or(0));

    // The native cosmetics seams (skin/cape retrieval and the texture binder). Like the render
    // feed they are fail-closed: a build whose slot does not resolve keeps the resource-pack path.
    InitCosmeticsHooks(0, 0, 0);

    std::vector<std::string> requestedSignatures{
        signatures->pauseMenuDtor, signatures->pauseMenuOpen,
        signatures->hudScreenDtor, signatures->hudScreenOpen};
    if (!signatures->isShowingMenu.empty()) {
      requestedSignatures.push_back(signatures->isShowingMenu);
    }
    auto results = pl::memory::resolveSignatures(
        requestedSignatures, "libminecraftpe.so");

    uintptr_t pauseDtor = ResolveResult(results, signatures->pauseMenuDtor);
    uintptr_t pauseOpen = ResolveResult(results, signatures->pauseMenuOpen);
    uintptr_t hudDtor = ResolveResult(results, signatures->hudScreenDtor);
    uintptr_t hudOpen = ResolveResult(results, signatures->hudScreenOpen);
    uintptr_t isShowingMenuAddr = 0;
    if (signatures->isShowingMenuVtableIndex) {
      isShowingMenuAddr = pl::memory::resolveVtableFunction(
          "14ClientInstance", *signatures->isShowingMenuVtableIndex,
          "libminecraftpe.so");
    } else if (!signatures->isShowingMenu.empty()) {
      isShowingMenuAddr = ResolveResult(results, signatures->isShowingMenu);
    }

    if (!pauseDtor || !pauseOpen || !hudDtor || !hudOpen ||
        !isShowingMenuAddr) {
      sweetLoaderLogger.warn(
          "SweetLoader runtime data is incomplete; game-only overlays remain hidden");
      return;
    }

    // Install the overlay hooks as one atomic batch: abort on the first failure, roll
    // back the hooks already installed, and never leave the process half-patched.
    const std::vector<PendingHook> overlayHooks{
        {pauseDtor, (pl::memory::FuncPtr)hook_PauseMenuDtor,
         (pl::memory::FuncPtr *)&orig_PauseMenuDtor, "PauseMenuDtor"},
        {pauseOpen, (pl::memory::FuncPtr)hook_PauseMenuOpen,
         (pl::memory::FuncPtr *)&orig_PauseMenuOpen, "PauseMenuOpen"},
        {hudDtor, (pl::memory::FuncPtr)hook_HudScreenDtor,
         (pl::memory::FuncPtr *)&orig_HudScreenDtor, "HudScreenDtor"},
        {hudOpen, (pl::memory::FuncPtr)hook_HudScreenOpen,
         (pl::memory::FuncPtr *)&orig_HudScreenOpen, "HudScreenOpen"},
        {isShowingMenuAddr, (pl::memory::FuncPtr)hook_isShowingMenu,
         (pl::memory::FuncPtr *)&orig_isShowingMenu, "isShowingMenu"},
    };

    // Round-trip every resolved site once, before the real install. The targets are
    // untouched at this point, so the round-trip tests the original code rather than chaining a
    // detour onto one already installed. The result is cached for the diagnostics report.
    CacheHookSelfTest({
        {"LivePlayerRenderer::render", PlayerRenderHookTarget(), nullptr, nullptr,
         "libminecraftpe.so"},
        {"ClientInstance::getLocalPlayer", LocalPlayerHookTarget(), nullptr, nullptr,
         "libminecraftpe.so"},
        {"PauseMenuDtor", pauseDtor, nullptr, nullptr, "libminecraftpe.so"},
        {"PauseMenuOpen", pauseOpen, nullptr, nullptr, "libminecraftpe.so"},
        {"HudScreenDtor", hudDtor, nullptr, nullptr, "libminecraftpe.so"},
        {"HudScreenOpen", hudOpen, nullptr, nullptr, "libminecraftpe.so"},
        {"isShowingMenu", isShowingMenuAddr, nullptr, nullptr, "libminecraftpe.so"},
    });

    if (!InstallHookBatch(overlayHooks)) {
      sweetLoaderLogger.warn(
          "SweetLoader runtime hooks are incomplete; game-only overlays remain hidden");
      return;
    }

    g_overlayHooksInstalled.store(true, std::memory_order_relaxed);
    sweetLoaderLogger.info("SweetLoader runtime hooks installed (5/5)");
  });
}

void ArmGameHookInstall() {
  std::call_once(g_armOnce, [] {
    g_installGate.start(MonotonicMs());
    g_installArmed.store(true, std::memory_order_release);
    sweetLoaderLogger.info(
        "SweetLoader hook install armed; deferring until the game runtime is up");
  });
}

namespace {
// How many give-ups are tolerated before the install is marked resolved permanently. The gate's
// kGiveUpMs is a safety net for a stuck loading screen; a handful of retries with a backoff covers
// a runtime that starts slowly (or rules that arrive late) without spinning forever.
constexpr int kMaxGiveUps = 3;
constexpr long long kGiveUpBackoffMs = 5'000;
} // namespace

/** The cached self-test JSON, plus the once-guard that produces it. */
std::once_flag g_selfTestOnce;
std::string g_selfTestJson;

void CacheHookSelfTest(const std::vector<HookSite> &sites) {
  std::call_once(g_selfTestOnce, [&sites] {
    g_selfTestJson = RunHookSelfTest(sites).toJson();
  });
}

std::string RunGameHookSelfTest() {
  // The self-test normally runs inside InitGameHooks, before the real install, so the targets are
  // pristine. This fallback covers a caller that reads the report before an install has happened:
  // it round-trips whatever resolved so far, which at worst reports the sites as unresolved.
  CacheHookSelfTest({
      {"LivePlayerRenderer::render", PlayerRenderHookTarget(), nullptr, nullptr,
       "libminecraftpe.so"},
      {"ClientInstance::getLocalPlayer", LocalPlayerHookTarget(), nullptr, nullptr,
       "libminecraftpe.so"},
  });
  return g_selfTestJson;
}

bool GameHookSelfTestHasRun() { return HookSelfTestHasRun(); }

bool PumpGameHookInstall() {
  if (g_installResolved.load(std::memory_order_acquire)) return true;
  if (!g_installArmed.load(std::memory_order_acquire)) return false;

  const long long nowMs = MonotonicMs();
  // The runtime-up probe. A frame tick is not proof the engine has settled: ArmGameHookInstall runs
  // from the activity's onCreate, and a tick can arrive before libminecraftpe.so is fully mapped and
  // relocated. When the present hook is live, require it to have observed a real present; when the
  // present hook could not be installed (a Vulkan-only build, no libEGL), the probe does not block
  // and the frame floor alone gates the install.
  const bool runtimeUp = RuntimeProbeSatisfied(
      pl::runtime::IsSwapFrameBoundaryLive(),
      pl_runtime_swap_buffers_count.load(std::memory_order_relaxed));
  const InstallDecision decision = g_installGate.tick(nowMs, runtimeUp);
  if (decision == InstallDecision::Wait) return false;

  if (decision == InstallDecision::GiveUp) {
    // The runtime did not come up within the gate's budget. Retry after a backoff instead of
    // giving up permanently: a slow start, or signature rules delivered after the library was
    // loaded, can be resolved by a later attempt. Only after kMaxGiveUps is the state sealed,
    // leaving the game vanilla rather than patching something half-mapped.
    if (g_installGiveUps.fetch_add(1, std::memory_order_relaxed) + 1 >= kMaxGiveUps) {
      g_installResolved.store(true, std::memory_order_release);
      sweetLoaderLogger.warn(
          "SweetLoader hook install gave up after {} attempts; the runtime did not come up",
          g_installGiveUps.load(std::memory_order_relaxed));
      return true;
    }
    g_installGate.start(nowMs + kGiveUpBackoffMs);
    sweetLoaderLogger.warn(
        "SweetLoader hook install wait exceeded the gate budget after {} frame tick(s); retrying in "
        "{}ms",
        g_installGate.frameTicks(), kGiveUpBackoffMs);
    return false;
  }

  // Install path. InitGameHooks defers when the signature rules have not been delivered yet; a
  // deferral is not a permanent skip, so the gate is re-armed to schedule another attempt. Without
  // the re-arm the gate would already be resolved and every later tick would report Wait, losing the
  // install for the whole session even after the rules arrived.
  sweetLoaderLogger.info("SweetLoader hook install running after {} frame tick(s)",
                       g_installGate.frameTicks());
  InitGameHooks();
  if (GameHookInstallResolved()) return true;
  if (g_installGate.deferredCount() >= HookInstallGate::kMaxDefers) {
    g_installResolved.store(true, std::memory_order_release);
    sweetLoaderLogger.warn(
        "SweetLoader hook install deferred {} times; the signature rules never arrived, leaving the "
        "game unpatched", g_installGate.deferredCount());
    return true;
  }
  g_installGate.defer(nowMs);
  sweetLoaderLogger.warn(
      "SweetLoader hook install deferred (signature rules not delivered yet); retrying on a later "
      "frame tick");
  return false;
}

bool GameHookInstallResolved() { return g_installResolved.load(std::memory_order_acquire); }

bool GameHookInstallPending() {
  return g_installArmed.load(std::memory_order_acquire)
      && !g_installResolved.load(std::memory_order_acquire);
}

int GameHookInstallState() {
  if (!g_installArmed.load(std::memory_order_acquire)) return 0;
  return g_installResolved.load(std::memory_order_acquire) ? 2 : 1;
}

int GameHookInstallFrameTicks() { return g_installGate.frameTicks(); }

bool IsPauseMenuOpen() {
  return g_isPauseMenuOpen.load(std::memory_order_relaxed);
}

bool IsHudScreenOpen() {
  return g_isHudScreenOpen.load(std::memory_order_relaxed);
}

bool IsShowingMenu() {
  return g_isShowingMenu.load(std::memory_order_relaxed);
}

bool ShouldForceGlobalModMenu() {
  return g_forceGlobalModMenu.load(std::memory_order_relaxed);
}

void RefreshGameFeatureAvailability() {
  FeatureAvailability &table = GameFeatureAvailability();

  // Overlay hooks: installed as one atomic batch, so the feature is either on or off.
  if (g_overlayHooksInstalled.load(std::memory_order_relaxed)) {
    table.set(Feature::OverlayHooks, FeatureState::Available);
  } else if (GameHookInstallPending()) {
    table.set(Feature::OverlayHooks, FeatureState::Pending);
  } else {
    table.set(Feature::OverlayHooks, FeatureState::Unavailable,
              "pause/HUD signature or the menu slot did not resolve on this build");
  }

  // Local-player feed: the slot resolved and the detour installed.
  if (IsLocalPlayerHookInstalled()) {
    table.set(Feature::LocalPlayerFeed, FeatureState::Available);
  } else if (GameHookInstallPending()) {
    table.set(Feature::LocalPlayerFeed, FeatureState::Pending);
  } else {
    table.set(Feature::LocalPlayerFeed, FeatureState::Unavailable,
              "ClientInstance local-player slot did not resolve on this build");
  }

  // Player-render feed: installed is not the same as "has run"; report installed, and note the
  // frame state in the reason so a loading screen is not mistaken for a failure.
  if (IsPlayerRenderHookInstalled()) {
    table.set(Feature::PlayerRenderFeed, FeatureState::Available,
              IsPlayerRenderHookLive() ? "" : "hook installed; the renderer has not drawn yet");
  } else if (GameHookInstallPending()) {
    table.set(Feature::PlayerRenderFeed, FeatureState::Pending);
  } else {
    table.set(Feature::PlayerRenderFeed, FeatureState::Unavailable,
              "LivePlayerRenderer render slot did not resolve on this build");
  }

  // Native cosmetics interception rides the same render slot as the feed.
  if (pl::cosmetics::IsNativeCosmeticRenderHookLive()) {
    table.set(Feature::NativeCosmetics, FeatureState::Available);
  } else if (IsPlayerRenderHookInstalled()) {
    table.set(Feature::NativeCosmetics, FeatureState::Available,
              "registered on the render feed; no frame drawn yet");
  } else if (GameHookInstallPending()) {
    table.set(Feature::NativeCosmetics, FeatureState::Pending);
  } else {
    table.set(Feature::NativeCosmetics, FeatureState::Unavailable,
              "render slot did not resolve; cosmetics stay preview-only");
  }

  // Skin/cape seam is the render entry, reported through the same accessor the launcher uses.
  table.set(Feature::SkinCapeSeam,
            IsSkinCapeHookLive() ? FeatureState::Available : FeatureState::Unavailable,
            IsSkinCapeHookLive() ? "" : "render entry not live on this build");

  // Texture-bind seam.
  table.set(Feature::TextureBindSeam,
            IsTextureHookLive() ? FeatureState::Available : FeatureState::Unavailable,
            IsTextureHookLive() ? "" : "texture-bind slot did not resolve on this build");

  // Join-packet seam.
  table.set(Feature::JoinPacketSeam,
            IsPacketHookLive() ? FeatureState::Available : FeatureState::Unavailable,
            IsPacketHookLive() ? "" : "player-join packet slot did not resolve on this build");
}

} // namespace pl::runtime
