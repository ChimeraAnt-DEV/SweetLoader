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

  std::printf("ModFaultPolicyTest: %d checks passed\n", checks);
  return 0;
}
