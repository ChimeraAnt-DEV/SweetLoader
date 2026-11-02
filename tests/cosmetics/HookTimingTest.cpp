/**
 * @file HookTimingTest.cpp
 * @brief Host test for the debug-only hook time budget (item 388).
 *
 * A hook callback that runs long on the game's thread shows up as stutter that gets blamed on the
 * game, so the budget rule has to be exact. It is pure, so it runs on the host.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "pl/runtime/HookTiming.hpp"

using namespace pl::runtime;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

void aFastCallbackIsOnBudget() {
  expect(!HookOverBudget(0), "zero is on budget");
  expect(!HookOverBudget(1), "one microsecond is on budget");
  expect(!HookOverBudget(kHookTimeBudgetUs), "exactly the budget is on budget");
}

void aSlowCallbackIsFlagged() {
  expect(HookOverBudget(kHookTimeBudgetUs + 1), "one over the budget is flagged");
  expect(HookOverBudget(5000), "a millisecond-scale callback is flagged");
}

void aBackwardsClockReadsAsOnBudget() {
  expect(!HookOverBudget(-1), "a negative elapsed time (clock wrap) reads as on budget");
}

void theBudgetIsFiftyMicroseconds() {
  expect(kHookTimeBudgetUs == 50, "the budget is 50 us");
}

void theReportNamesTheCallbackAndTheCost() {
  const std::string report = HookBudgetReport("HookRender", 120);
  expect(report.find("HookRender") != std::string::npos, "the report names the callback");
  expect(report.find("120") != std::string::npos, "the report states the elapsed time");
  expect(report.find("50") != std::string::npos, "the report states the budget");
}

void anUnnamedCallbackIsStillReported() {
  const std::string report = HookBudgetReport(nullptr, 999);
  expect(report.find("<unnamed>") != std::string::npos, "a null name reads as <unnamed>");
  expect(report.find("999") != std::string::npos, "the cost is still reported");
}

void timingIsCompiledOutInRelease() {
#ifdef NDEBUG
  expect(!HookTimingEnabled(), "the timing seam is compiled out under NDEBUG");
#else
  expect(HookTimingEnabled(), "the timing seam is present in a debug build");
#endif
}

} // namespace

int main() {
  aFastCallbackIsOnBudget();
  aSlowCallbackIsFlagged();
  aBackwardsClockReadsAsOnBudget();
  theBudgetIsFiftyMicroseconds();
  theReportNamesTheCallbackAndTheCost();
  anUnnamedCallbackIsStillReported();
  timingIsCompiledOutInRelease();

  std::printf("HookTimingTest: %d checks passed\n", checks);
  return 0;
}
