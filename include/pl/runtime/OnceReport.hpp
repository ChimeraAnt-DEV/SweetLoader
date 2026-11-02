#pragma once

/**
 * @file OnceReport.hpp
 * @brief Reports the first occurrence of a repeated, non-fatal condition exactly once.
 *
 * A failure that repeats on a hot path (a mod callback that throws on every event, a hook that
 * fails on every install attempt) must be visible in the log without flooding it: a line per event
 * at 60-120 events a second is unreadable, and a line per frame is worse. The rule is therefore
 * "report the first one, stay quiet after".
 *
 * It is a small class rather than an inline `static bool` at each site so the rule is host-testable
 * and so a site cannot accidentally reset its flag. The check is one relaxed atomic exchange, so a
 * hot path can afford it; `reported()` lets a diagnostics screen ask whether the condition has
 * happened at all.
 *
 * Thread-safe: the exchange is atomic, so two threads racing the first occurrence produce exactly
 * one report.
 */

#include <atomic>

namespace pl::runtime {

/** @brief Reports the first occurrence of a repeated condition, and nothing after. */
class OnceReport {
public:
  /**
   * @brief Consumes the one-shot report.
   *
   * @return true for the first caller only; false for every later caller.
   */
  bool shouldReport() noexcept { return !mReported.exchange(true, std::memory_order_relaxed); }

  /** @brief Whether the condition has been reported at least once. */
  [[nodiscard]] bool reported() const noexcept {
    return mReported.load(std::memory_order_relaxed);
  }

  /** @brief Re-arms the report, for a condition that is expected to recur across sessions. */
  void reset() noexcept { mReported.store(false, std::memory_order_relaxed); }

private:
  std::atomic<bool> mReported{false};
};

} // namespace pl::runtime
