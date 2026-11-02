/**
 * @file OnceReportTest.cpp
 * @brief Host test for the report-once rule on a hot path.
 *
 * A condition that repeats every event must be visible in the log exactly once: a line per event is
 * unreadable and a line per frame is worse. The property that matters is that exactly one caller -
 * even under a race - gets the report, and that the flag is observable afterwards.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "pl/runtime/OnceReport.hpp"

using pl::runtime::OnceReport;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

} // namespace

int main() {
  OnceReport report;
  expect(!report.reported(), "a fresh report has not fired");
  expect(report.shouldReport(), "the first call reports");
  expect(report.reported(), "the report is observable afterwards");
  expect(!report.shouldReport(), "every later call stays quiet");
  expect(!report.shouldReport(), "and keeps staying quiet");

  report.reset();
  expect(!report.reported(), "reset re-arms the report");
  expect(report.shouldReport(), "the re-armed report fires again");

  // Two threads racing the first occurrence produce exactly one report.
  OnceReport shared;
  std::atomic<int> fired{0};
  std::thread a([&] { if (shared.shouldReport()) fired.fetch_add(1); });
  std::thread b([&] { if (shared.shouldReport()) fired.fetch_add(1); });
  a.join();
  b.join();
  expect(fired.load() == 1, "exactly one caller wins the report even under a race");

  std::printf("OnceReportTest: %d checks passed\n", checks);
  return 0;
}
