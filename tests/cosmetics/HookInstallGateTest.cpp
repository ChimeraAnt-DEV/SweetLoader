/**
 * @file HookInstallGateTest.cpp
 * @brief Host test for the deferred-hook-install state machine.
 *
 * The hooks used to install during the game activity's onCreate, before the runtime was up, and the
 * install bailed ("Hook installed: no"). The gate defers the install to the frame pump. Its
 * invariants — wait out a short grace, install exactly once, never wait forever, never install twice
 * — are pinned here so a regression cannot install before the runtime is settled, leave the state
 * pending, or install the detour twice.
 */

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "pl/runtime/HookInstallGate.hpp"

using pl::runtime::HookInstallGate;
using pl::runtime::InstallDecision;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

/** Ticks the gate through its whole grace and returns the decision on the completing tick. */
InstallDecision tickThroughGrace(HookInstallGate &gate, long long baseMs) {
  InstallDecision d = InstallDecision::Wait;
  for (int i = 0; i < HookInstallGate::kMinFrameTicks; ++i) {
    d = gate.tick(baseMs + i * 16);
  }
  return d;
}

} // namespace

int main() {
  // A freshly armed gate waits its grace of frames (so the library is fully mapped and relocated)
  // and then installs exactly once. Installing on the very first tick was the bug: ArmGameHookInstall
  // runs from onCreate, and the first frame can arrive before libminecraftpe.so is settled.
  {
    HookInstallGate gate;
    gate.start(1000);
    expect(!gate.isResolved(), "armed gate is not resolved");
    for (int i = 0; i + 1 < HookInstallGate::kMinFrameTicks; ++i) {
      expect(gate.tick(1000 + i * 16) == InstallDecision::Wait,
             "a frame inside the grace does not install");
    }
    expect(gate.tick(1000 + (HookInstallGate::kMinFrameTicks - 1) * 16) ==
               InstallDecision::Install,
           "the gate installs once the grace is past");
    expect(gate.isResolved(), "gate is resolved after installing");
    expect(gate.tick(1048) == InstallDecision::Wait, "a resolved gate never re-installs");
    expect(gate.tick(2000) == InstallDecision::Wait, "a resolved gate stays resolved");
  }

  // The give-up deadline is honoured even while the frame grace is still running: a runtime that
  // never delivers enough frames settles instead of pending forever.
  {
    HookInstallGate gate;
    gate.start(0);
    expect(gate.tick(HookInstallGate::kGiveUpMs + 1) == InstallDecision::GiveUp,
           "a tick past the give-up deadline gives up even on the first frame");
    expect(gate.isResolved(), "a given-up gate is resolved");
  }

  // start() re-arms: a second session must be able to install again in a fresh gate.
  {
    HookInstallGate gate;
    gate.start(0);
    expect(tickThroughGrace(gate, 0) == InstallDecision::Install, "fresh gate installs");
    gate.start(5000);
    expect(!gate.isResolved(), "re-armed gate is unresolved");
    expect(tickThroughGrace(gate, 5000) == InstallDecision::Install,
           "re-armed gate installs again after its grace");
  }

  // Frame ticks are counted, so the diagnostics line can report how long the wait was.
  {
    HookInstallGate gate;
    gate.start(0);
    gate.tick(0);
    expect(gate.frameTicks() == 1, "one tick observed");
    expect(!gate.isResolved(), "a gate inside its grace is not yet resolved");
  }

  // A deferral re-arms the gate so a later tick tries the install again instead of the gate staying
  // resolved and reporting Wait forever (which lost the install for the session).
  {
    HookInstallGate gate;
    gate.start(0);
    expect(tickThroughGrace(gate, 0) == InstallDecision::Install, "first attempt decides install");
    expect(gate.isResolved(), "gate resolved after the first attempt");
    expect(gate.defer(16) == 1, "first defer is counted");
    expect(!gate.isResolved(), "a deferred gate is unresolved again");
    // A defer is a retry once the runtime is already up (the grace has been observed), so the next
    // frame tick retries the install rather than waiting the grace again.
    expect(gate.tick(32) == InstallDecision::Install, "a deferred gate retries on the next tick");
  }

  // A gate that keeps deferring stops at kMaxDefers so a runtime that never delivers the rules
  // settles instead of retrying forever.
  {
    HookInstallGate gate;
    gate.start(0);
    int last = 0;
    for (int i = 0; i < HookInstallGate::kMaxDefers; ++i) {
      tickThroughGrace(gate, i * 16);
      last = gate.defer(i * 16 + 1);
    }
    expect(last == HookInstallGate::kMaxDefers, "defers are bounded by kMaxDefers");
    expect(gate.deferredCount() == HookInstallGate::kMaxDefers, "defer count matches");
  }

  // start() clears the defer count, so a fresh session is not penalised by an earlier one.
  {
    HookInstallGate gate;
    gate.start(0);
    tickThroughGrace(gate, 0);
    gate.defer(1);
    expect(gate.deferredCount() == 1, "one defer recorded");
    gate.start(100);
    expect(gate.deferredCount() == 0, "start() clears the defer count");
  }

  // The runtime-up probe: the gate must NOT install on the frame floor alone if the probe says the
  // runtime is not yet up. A frame tick is not proof the engine's present call has run; the probe is.
  {
    HookInstallGate gate;
    gate.start(0);
    for (int i = 0; i < HookInstallGate::kMinFrameTicks + 2; ++i) {
      expect(gate.tick(1000 + i * 16, /*runtimeUp=*/false) == InstallDecision::Wait,
             "the gate waits while the runtime-up probe is not satisfied");
    }
    expect(!gate.isResolved(), "waiting on the probe does not resolve the gate");
    expect(gate.tick(2000, /*runtimeUp=*/true) == InstallDecision::Install,
           "the gate installs once the probe is satisfied");
    expect(gate.isResolved(), "gate resolves after installing");
  }

  // A frame floor that is satisfied with the probe already up still installs on the same tick as
  // before, so the probe does not add latency on the normal path.
  {
    HookInstallGate gate;
    gate.start(0);
    InstallDecision d = InstallDecision::Wait;
    for (int i = 0; i < HookInstallGate::kMinFrameTicks; ++i) {
      d = gate.tick(i * 16, /*runtimeUp=*/true);
    }
    expect(d == InstallDecision::Install, "probe up + grace past installs on the completing tick");
  }

  // The deadline still fires with the probe never satisfied, so a runtime that never presents
  // settles rather than pending forever.
  {
    HookInstallGate gate;
    gate.start(0);
    expect(gate.tick(HookInstallGate::kGiveUpMs + 1, /*runtimeUp=*/false) == InstallDecision::GiveUp,
           "the deadline gives up even when the probe is never satisfied");
  }

  // The probe derivation is pure: a live present probe requires a real present; an unavailable
  // probe does not block (the frame floor still gates the install).
  {
    expect(pl::runtime::RuntimeProbeSatisfied(/*presentProbeLive=*/true, 0) == false,
           "a live probe with no present is not satisfied");
    expect(pl::runtime::RuntimeProbeSatisfied(/*presentProbeLive=*/true, 1) == true,
           "a live probe with a present is satisfied");
    expect(pl::runtime::RuntimeProbeSatisfied(/*presentProbeLive=*/false, 0) == true,
           "an unavailable probe does not block the install");
  }

  std::printf("HookInstallGateTest: %d checks passed\n", checks);
  return 0;
}
