#pragma once

/**
 * @file HookTiming.hpp
 * @brief Flags a detour callback that is too slow to run on the game's thread (item 388).
 *
 * <p>A hook installed on the render or input path runs inside the game's own frame budget. A
 * callback that takes more than a few tens of microseconds shows up as stutter, but the cost is
 * invisible without measurement: the callback "works", and the frame drop is attributed to the game.
 *
 * <p>This is the pure half of the debug-only timing seam. It decides whether an elapsed time is
 * over budget and formats the report; the caller does the (cheap, monotonic) timing and only calls
 * in when the budget is exceeded, so the hot path pays one comparison and no allocation. The whole
 * rule is host-tested (`HookTimingTest`) because a threshold bug would silently hide the very
 * problem it exists to expose.
 *
 * <p>Only compiled into a debug build: `HookTimingEnabled()` is false under `NDEBUG`, so a release
 * detour carries neither the clock read nor the branch.
 */

#include <cstdint>
#include <string>

namespace pl::runtime {

/**
 * @brief The per-callback time budget, in microseconds.
 *
 * <p>50 us is roughly 0.3% of a 16.6 ms frame at 60 Hz. A callback above that is worth naming: the
 * budget is deliberately tight because a detour runs once per rendered player model or per input
 * event, not once per frame.
 */
inline constexpr std::int64_t kHookTimeBudgetUs = 50;

/** @brief True when the timing seam is compiled in. False in a release build. */
constexpr bool HookTimingEnabled() noexcept {
#ifdef NDEBUG
  return false;
#else
  return true;
#endif
}

/** @brief True when an elapsed time exceeds the budget. A negative (clock wrap) reads as on time. */
inline bool HookOverBudget(std::int64_t elapsedUs) noexcept {
  return elapsedUs > kHookTimeBudgetUs;
}

/**
 * @brief Formats the one-line report for an over-budget callback.
 *
 * <p>Kept separate from the logging call so the wording is testable and a caller cannot build the
 * string on the hot path by accident (it is only reached when the budget was already exceeded).
 */
inline std::string HookBudgetReport(const char *name, std::int64_t elapsedUs) {
  std::string report = "hook callback '";
  report.append(name != nullptr ? name : "<unnamed>");
  report.append("' took ");
  report.append(std::to_string(elapsedUs));
  report.append(" us, over the ");
  report.append(std::to_string(kHookTimeBudgetUs));
  report.append(" us budget");
  return report;
}

} // namespace pl::runtime
