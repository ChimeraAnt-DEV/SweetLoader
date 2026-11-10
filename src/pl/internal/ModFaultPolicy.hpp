#pragma once

/**
 * @file ModFaultPolicy.hpp
 * @brief Per-mod fault isolation: one bad mod must not take the others down.
 *
 * <p>A native mod runs inside the game process and its lifecycle entry points (`PLMod_Load`,
 * `PLMod_Enable`, the C++ registration hooks) are arbitrary code. A mod that returns failure, or
 * that throws, must be isolated: the remaining mods still load and enable, and the failing mod is
 * reported by name rather than silently retried on every launch. A mod that faults on *every*
 * launch is worse than a missing one -- it costs the user a launch attempt each time and looks like
 * a launcher bug -- so after {@link kMaxModFaults} consecutive faults it is disabled until the user
 * re-enables it.
 *
 * <p>The decision is a pure function of the fault count, and the tracker is a plain map, so the
 * whole rule is host-tested without a device or a real mod (`ModFaultPolicyTest`).
 */

#include <cstddef>
#include <string>
#include <unordered_map>

namespace pl::internal::mod {

/**
 * @brief How many consecutive faults a mod may have before it is disabled.
 *
 * <p>Three, not one: a transient failure (a missing dependency that has not loaded yet, a race on
 * the first launch) should be retried, but a mod that has failed three launches in a row is broken.
 */
inline constexpr int kMaxModFaults = 3;

/** @brief What to do after a mod faulted. */
enum class ModFaultAction {
  /** The mod has faults left in its budget; it is retried on the next launch. */
  Retry,
  /** The mod has exhausted its budget; it is disabled until the user re-enables it. */
  Disable,
};

/** @brief The outcome of recording one fault. */
struct ModFaultDecision {
  ModFaultAction action{ModFaultAction::Retry};
  /** The mod's consecutive fault count after this fault. */
  int faultCount{0};
};

/**
 * @brief The pure rule: does a mod with @p faultCount consecutive faults stay enabled?
 *
 * <p>Counts are 1-based at the call site (the first fault passes 1). A non-positive count is a
 * programming error and reads as "no faults yet", i.e. retry.
 */
inline ModFaultDecision DecideAfterFault(int faultCount) {
  if (faultCount >= kMaxModFaults) {
    return ModFaultDecision{ModFaultAction::Disable, faultCount};
  }
  return ModFaultDecision{ModFaultAction::Retry, faultCount};
}

/**
 * @brief Tracks consecutive faults per mod and answers whether a mod is currently disabled.
 *
 * <p>Not on any hot path: a fault is recorded once per failed load/enable, so a small map is the
 * right shape. A success clears the mod's count, so a mod that failed twice and then worked is back
 * to a full budget rather than being disabled on its next unrelated fault.
 */
class ModFaultTracker {
public:
  /**
   * @brief Seeds a mod's persisted budget, restoring a fault count earned in an earlier session.
   *
   * <p>Called once per mod at load, before the first fault is recorded, so a mod that faulted three
   * times across three launches is disabled on the third rather than retried forever (audit item
   * 6.5). A zero count with `disabled` false is a no-op; a disabled flag alone disables the mod.
   */
  void seedFaults(const std::string &modId, int faultCount, bool disabled) {
    if (faultCount > 0) {
      mFaults[modId] = faultCount;
    } else {
      mFaults.erase(modId);
    }
    if (disabled) {
      mDisabled[modId] = true;
    } else {
      mDisabled.erase(modId);
    }
  }

  /** Records one fault and returns the decision for that mod. */
  ModFaultDecision recordFault(const std::string &modId) {
    const int count = ++mFaults[modId];
    const ModFaultDecision decision = DecideAfterFault(count);
    if (decision.action == ModFaultAction::Disable) {
      mDisabled[modId] = true;
    }
    return decision;
  }

  /**
   * Records a success, clearing the mod's fault budget.
   *
   * <p>Deliberately does <em>not</em> re-enable a disabled mod. A disabled mod is one that exhausted
   * its fault budget, and it is disabled before its entry points are ever called again -- so it can
   * never reach this method with a success, and reaching it would mean the mod ran despite being
   * disabled. Re-enabling is a user action, not a side effect of a successful call: the user fixes
   * the mod (or a missing dependency) and re-enables it explicitly through {@link #clear}, which
   * resets both the fault count and the disabled flag. Clearing the fault count here, without
   * clearing the flag, is exactly the intended split.
   */
  void recordSuccess(const std::string &modId) { mFaults.erase(modId); }

  /** The mod's consecutive fault count, or 0 when it has none. */
  int faultsFor(const std::string &modId) const {
    const auto it = mFaults.find(modId);
    return it == mFaults.end() ? 0 : it->second;
  }

  /** True when the mod exhausted its fault budget and must not be loaded again. */
  bool isDisabled(const std::string &modId) const {
    const auto it = mDisabled.find(modId);
    return it != mDisabled.end() && it->second;
  }

  /** Re-enables a mod the user asked for, giving it a fresh fault budget. */
  void clear(const std::string &modId) {
    mFaults.erase(modId);
    mDisabled.erase(modId);
  }

  /** Forgets every mod. Used when the mod set changes, so a removed mod leaves no state behind. */
  void reset() {
    mFaults.clear();
    mDisabled.clear();
  }

  /** The number of mods currently disabled by this policy. */
  std::size_t disabledCount() const {
    std::size_t count = 0;
    for (const auto &entry : mDisabled) {
      if (entry.second) ++count;
    }
    return count;
  }

private:
  std::unordered_map<std::string, int> mFaults;
  std::unordered_map<std::string, bool> mDisabled;
};

} // namespace pl::internal::mod
