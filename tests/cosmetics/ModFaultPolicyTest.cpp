/**
 * @file ModFaultPolicyTest.cpp
 * @brief Host test for the per-mod fault-isolation rule.
 *
 * A mod that faults must not take the others down, and a mod that faults on every launch must
 * eventually be disabled rather than retried forever. Both rules are pure, so they run on the host.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "pl/internal/ModFaultPolicy.hpp"

using namespace pl::internal::mod;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

void aSingleFaultIsRetried() {
  const ModFaultDecision decision = DecideAfterFault(1);
  expect(decision.action == ModFaultAction::Retry, "one fault is retried");
  expect(decision.faultCount == 1, "the count is reported");
}

void theBudgetIsThreeConsecutiveFaults() {
  expect(DecideAfterFault(2).action == ModFaultAction::Retry, "two faults are retried");
  expect(DecideAfterFault(3).action == ModFaultAction::Disable, "three faults disable");
  expect(DecideAfterFault(4).action == ModFaultAction::Disable, "more faults stay disabled");
}

void aNonPositiveCountReadsAsRetry() {
  expect(DecideAfterFault(0).action == ModFaultAction::Retry, "zero faults reads as retry");
  expect(DecideAfterFault(-5).action == ModFaultAction::Retry, "a negative count reads as retry");
}

void theTrackerCountsConsecutiveFaults() {
  ModFaultTracker tracker;
  expect(tracker.faultsFor("a") == 0, "a fresh mod has no faults");
  expect(tracker.recordFault("a").action == ModFaultAction::Retry, "first fault retries");
  expect(tracker.recordFault("a").action == ModFaultAction::Retry, "second fault retries");
  expect(tracker.faultsFor("a") == 2, "the count accumulates");
  expect(!tracker.isDisabled("a"), "a retried mod is not disabled");
  expect(tracker.recordFault("a").action == ModFaultAction::Disable, "third fault disables");
  expect(tracker.isDisabled("a"), "the mod is disabled after the budget");
  expect(tracker.disabledCount() == 1, "one mod is disabled");
}

void aSuccessClearsTheBudget() {
  ModFaultTracker tracker;
  tracker.recordFault("a");
  tracker.recordFault("a");
  tracker.recordSuccess("a");
  expect(tracker.faultsFor("a") == 0, "a success clears the fault count");
  expect(tracker.recordFault("a").action == ModFaultAction::Retry,
         "the budget restarts after a success");
  expect(!tracker.isDisabled("a"), "a recovered mod is not disabled");
}

void aSuccessDoesNotReEnableADisabledMod() {
  ModFaultTracker tracker;
  tracker.recordFault("a");
  tracker.recordFault("a");
  tracker.recordFault("a");
  expect(tracker.isDisabled("a"), "the mod is disabled");
  tracker.recordSuccess("a");
  expect(tracker.isDisabled("a"),
         "a success does not silently re-enable a mod the policy disabled");
}

void theUserCanReEnable() {
  ModFaultTracker tracker;
  tracker.recordFault("a");
  tracker.recordFault("a");
  tracker.recordFault("a");
  tracker.clear("a");
  expect(!tracker.isDisabled("a"), "an explicit clear re-enables the mod");
  expect(tracker.faultsFor("a") == 0, "and gives it a fresh budget");
}

void faultsArePerMod() {
  ModFaultTracker tracker;
  tracker.recordFault("a");
  tracker.recordFault("a");
  tracker.recordFault("a");
  expect(tracker.isDisabled("a"), "a is disabled");
  expect(!tracker.isDisabled("b"), "b is unaffected");
  expect(tracker.faultsFor("b") == 0, "b has no faults");
  expect(tracker.recordFault("b").action == ModFaultAction::Retry, "b still has a full budget");
}

void resetForgetsEverything() {
  ModFaultTracker tracker;
  tracker.recordFault("a");
  tracker.recordFault("a");
  tracker.recordFault("a");
  tracker.reset();
  expect(!tracker.isDisabled("a"), "reset clears the disabled set");
  expect(tracker.faultsFor("a") == 0, "reset clears the counts");
  expect(tracker.disabledCount() == 0, "reset leaves nothing disabled");
}

} // namespace

int main() {
  aSingleFaultIsRetried();
  theBudgetIsThreeConsecutiveFaults();
  aNonPositiveCountReadsAsRetry();
  theTrackerCountsConsecutiveFaults();
  aSuccessClearsTheBudget();
  aSuccessDoesNotReEnableADisabledMod();
  theUserCanReEnable();
  faultsArePerMod();
  resetForgetsEverything();

  // Seeding a persisted budget restores a fault count from an earlier session (audit item 6.5).
  {
    ModFaultTracker tracker;
    tracker.seedFaults("mod.seeded", 2, false);
    expect(tracker.faultsFor("mod.seeded") == 2, "a seeded fault count is restored");
    expect(!tracker.isDisabled("mod.seeded"), "a seeded count below the budget does not disable");
    // One more fault takes it to the budget and disables it.
    expect(tracker.recordFault("mod.seeded").action == ModFaultAction::Disable,
           "a seeded count at the budget minus one disables on the next fault");

    ModFaultTracker disabledTracker;
    disabledTracker.seedFaults("mod.disabled", 3, true);
    expect(disabledTracker.isDisabled("mod.disabled"), "a seeded disabled flag disables the mod");
    expect(disabledTracker.faultsFor("mod.disabled") == 3, "the seeded count is kept with the flag");

    // Seeding with zero and not-disabled clears any prior state.
    ModFaultTracker cleared;
    cleared.seedFaults("mod.cleared", 0, false);
    expect(cleared.faultsFor("mod.cleared") == 0, "seeding zero clears the count");
    expect(!cleared.isDisabled("mod.cleared"), "seeding not-disabled clears the flag");
  }

  std::printf("ModFaultPolicyTest: %d checks passed\n", checks);
  return 0;
}
