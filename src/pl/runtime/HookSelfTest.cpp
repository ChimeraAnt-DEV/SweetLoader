/**
 * @file HookSelfTest.cpp
 * @brief The hook self-test runner and report.
 */

#include "pl/runtime/HookSelfTest.hpp"

#include "pl/Logger.hpp"
#include "pl/memory/Signature.hpp"

namespace pl::runtime {
namespace {

// The two functions the control round-trip uses. `Target` is the site that gets patched; `Detour`
// is the replacement. They are separate functions so the detour is never the target (hooking a
// function onto itself would recurse the moment anything called it) and both are `noinline` so each
// has a real entry address the backend can patch.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void SelfTestTarget() {
  asm volatile("" ::: "memory");
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void SelfTestDetour() {
  asm volatile("" ::: "memory");
}

HookSelfTestReport g_lastReport;
bool g_hasRun = false;

/** The outcome of one round-trip. */
struct RoundTripResult {
  bool installed = false;
  bool removed = false;
  std::string detail;
};

/**
 * @brief Round-trips one target: install, then remove, verifying the removal by hook count.
 *
 * <p>The removal is verified by the backend's hook count returning to its previous value, not by
 * the return code: a backend that reports success but leaves the trampoline in place would
 * otherwise pass.
 */
RoundTripResult RoundTrip(pl::memory::FuncPtr target, pl::memory::FuncPtr detour) {
  RoundTripResult out;
  const std::size_t before = pl::memory::hookCount();
  void *original = nullptr;
  if (pl::memory::hook(target, detour, &original, pl::memory::HookPriority::Normal,
                       "HookSelfTest") != 0) {
    out.detail = "the backend refused the detour";
    return out;
  }
  out.installed = true;
  const bool removedOk = pl::memory::unhook(target, detour);
  if (!removedOk || pl::memory::hookCount() != before) {
    out.detail = "the hook was installed but not fully removed";
    return out;
  }
  out.removed = true;
  return out;
}

} // namespace

const HookSelfTestReport &LastHookSelfTestReport() { return g_lastReport; }

bool HookSelfTestHasRun() { return g_hasRun; }

HookSelfTestReport RunHookSelfTest(const std::vector<HookSite> &sites) {
  HookSelfTestReport report;

  // Control first: round-trip two private functions so a broken backend is reported as such rather
  // than as "every site failed", which would send a reader looking at the signatures.
  const RoundTripResult control = RoundTrip(reinterpret_cast<pl::memory::FuncPtr>(&SelfTestTarget),
                                            reinterpret_cast<pl::memory::FuncPtr>(&SelfTestDetour));
  report.backendOk = control.installed && control.removed;

  for (const HookSite &site : sites) {
    HookSelfTestResult result;
    result.name = site.name != nullptr ? site.name : "";

    if (site.target == 0 || site.detour == nullptr) {
      // An unresolved signature is a build limitation, not a failure: report it as unresolved so
      // the client can grey out the feature rather than call it broken.
      result.resolved = false;
      result.detail = "target did not resolve on this build";
      report.sites.push_back(std::move(result));
      continue;
    }
    result.resolved = true;

    // The target must be executable code. Checked before patching so a resolved-but-wrong address
    // (a stale slot that landed in data) is refused rather than written to.
    result.executable =
        pl::memory::isExecutableAddress(site.target, site.module != nullptr ? site.module : "");
    if (!result.executable) {
      result.detail = "the resolved address is not executable code";
      report.sites.push_back(std::move(result));
      continue;
    }

    // A site may leave the detour unset; the runner then uses its own no-op stub. The self-test
    // asks "can this address be patched and restored", which does not need the production detour
    // and is safer without it: patching in a real detour before the real install would run it if
    // the target were called between the two.
    const pl::memory::FuncPtr detour =
        site.detour != nullptr ? site.detour : reinterpret_cast<pl::memory::FuncPtr>(&SelfTestDetour);
    const RoundTripResult trip =
        RoundTrip(reinterpret_cast<pl::memory::FuncPtr>(site.target), detour);
    result.installed = trip.installed;
    result.removed = trip.removed;
    result.detail = trip.detail;
    report.sites.push_back(std::move(result));
  }

  g_lastReport = report;
  g_hasRun = true;
  if (!report.backendOk) {
    preloaderLogger.warn(
        "Hook self-test: the hook backend failed its control round-trip; hook installs are not "
        "reliable on this build");
  } else {
    preloaderLogger.info("Hook self-test: {}", report.summary());
  }
  return report;
}

} // namespace pl::runtime
