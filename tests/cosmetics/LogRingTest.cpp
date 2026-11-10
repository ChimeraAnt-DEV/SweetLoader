/**
 * @file LogRingTest.cpp
 * @brief Host test for the bounded log ring and the level tag.
 *
 * The ring keeps the last N log lines in memory so the launcher can read them without `adb` (audit
 * item 10.10). Memory must stay bounded however long a session runs, the snapshot must be oldest
 * first, and a request for more lines than are held must return everything rather than misbehave.
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pl/runtime/LogRing.hpp"

using pl::runtime::LogLevelTag;
using pl::runtime::LogRing;

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
  // A ring with capacity 3 keeps only the newest three, oldest first.
  LogRing ring(3);
  ring.append("a");
  ring.append("b");
  ring.append("c");
  expect(ring.size() == 3, "the ring fills to capacity");
  ring.append("d");
  expect(ring.size() == 3, "the ring stays at capacity after overflowing");
  {
    const std::vector<std::string> all = ring.snapshot(10);
    expect(all.size() == 3, "a snapshot larger than the ring returns the whole ring");
    expect(all[0] == "b" && all[1] == "c" && all[2] == "d",
           "the oldest line is evicted and the rest are oldest-first");
  }

  // Asking for fewer lines returns the newest ones.
  {
    const std::vector<std::string> last = ring.snapshot(2);
    expect(last.size() == 2 && last[0] == "c" && last[1] == "d",
           "a partial snapshot returns the newest lines oldest-first");
  }

  // A zero request returns nothing, and an empty ring is not an error.
  expect(ring.snapshot(0).empty(), "a zero-line request returns nothing");
  LogRing empty(4);
  expect(empty.snapshot(4).empty(), "an empty ring returns nothing");
  expect(empty.size() == 0, "an empty ring reports zero");

  // Capacity zero is clamped to one, so the ring always holds something.
  LogRing degenerate(0);
  degenerate.append("only");
  degenerate.append("newest");
  expect(degenerate.size() == 1, "a zero-capacity ring is clamped to one");
  expect(degenerate.snapshot(1)[0] == "newest", "the clamped ring keeps the newest line");

  // clear() drops everything.
  ring.clear();
  expect(ring.size() == 0, "clear drops every line");

  // The level tag maps the Android levels and refuses to guess an unknown one.
  expect(std::string(LogLevelTag(2)) == "V", "verbose maps to V");
  expect(std::string(LogLevelTag(3)) == "D", "debug maps to D");
  expect(std::string(LogLevelTag(4)) == "I", "info maps to I");
  expect(std::string(LogLevelTag(5)) == "W", "warn maps to W");
  expect(std::string(LogLevelTag(6)) == "E", "error maps to E");
  expect(std::string(LogLevelTag(0)) == "?", "an unknown level reads as ?");
  expect(std::string(LogLevelTag(99)) == "?", "an out-of-range level reads as ?");

  std::printf("LogRingTest: %d checks passed\n", checks);
  return 0;
}
