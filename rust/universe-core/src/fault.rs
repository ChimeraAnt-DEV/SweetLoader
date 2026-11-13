//! Per-mod fault isolation: one bad mod must not take the others down, and a mod that faults on
//! every launch is disabled until the user re-enables it.
//!
//! A `.universe` mod runs arbitrary native code inside the game process. A mod that panics, returns
//! failure, or crashes the tick must be isolated: the remaining mods keep running, and the failing
//! mod is reported by name rather than silently retried on every launch. After [`MAX_FAULTS`]
//! consecutive faults the mod is blacklisted for the session.
//!
//! The decision is a pure function of the fault count, and the tracker is a plain map, so the whole
//! rule is unit-tested without a device or a real mod.

use std::collections::HashMap;

/// How many consecutive faults a mod may have before it is disabled.
///
/// Three, not one: a transient failure (a dependency that has not loaded yet, a first-launch race)
/// should be retried, but a mod that has failed three launches in a row is broken.
pub const MAX_FAULTS: u32 = 3;

/// What to do after a mod faulted.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FaultAction {
    /// The mod has faults left in its budget; it is retried.
    Retry,
    /// The mod has exhausted its budget; it is disabled until the user re-enables it.
    Disable,
}

/// The outcome of recording one fault.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FaultDecision {
    pub action: FaultAction,
    /// The mod's consecutive fault count after this fault.
    pub fault_count: u32,
}

/// The pure rule: does a mod with `fault_count` consecutive faults stay enabled?
pub fn decide_after_fault(fault_count: u32) -> FaultDecision {
    if fault_count >= MAX_FAULTS {
        FaultDecision {
            action: FaultAction::Disable,
            fault_count,
        }
    } else {
        FaultDecision {
            action: FaultAction::Retry,
            fault_count,
        }
    }
}

/// Tracks consecutive faults per mod and answers whether a mod is currently disabled.
#[derive(Debug, Default)]
pub struct FaultTracker {
    faults: HashMap<String, u32>,
    disabled: HashMap<String, bool>,
}

impl FaultTracker {
    pub fn new() -> FaultTracker {
        FaultTracker::default()
    }

    /// Records one fault and returns the decision for that mod.
    pub fn record_fault(&mut self, mod_id: &str) -> FaultDecision {
        let count = self.faults.entry(mod_id.to_string()).or_insert(0);
        *count += 1;
        let decision = decide_after_fault(*count);
        if decision.action == FaultAction::Disable {
            self.disabled.insert(mod_id.to_string(), true);
        }
        decision
    }

    /// Records a success, clearing the mod's fault budget.
    ///
    /// Deliberately does not re-enable a disabled mod: a disabled mod is not called again, so
    /// reaching this with a success would mean it ran despite being disabled. Re-enabling is a user
    /// action ([`FaultTracker::clear`]).
    pub fn record_success(&mut self, mod_id: &str) {
        self.faults.remove(mod_id);
    }

    /// The mod's consecutive fault count, or 0 when it has none.
    pub fn faults_for(&self, mod_id: &str) -> u32 {
        self.faults.get(mod_id).copied().unwrap_or(0)
    }

    /// True when the mod exhausted its fault budget and must not be loaded again.
    pub fn is_disabled(&self, mod_id: &str) -> bool {
        self.disabled.get(mod_id).copied().unwrap_or(false)
    }

    /// Re-enables a mod the user asked for, giving it a fresh fault budget.
    pub fn clear(&mut self, mod_id: &str) {
        self.faults.remove(mod_id);
        self.disabled.remove(mod_id);
    }

    /// Forgets every mod, so a removed mod leaves no state behind.
    pub fn reset(&mut self) {
        self.faults.clear();
        self.disabled.clear();
    }

    /// The number of mods currently disabled by this policy.
    pub fn disabled_count(&self) -> usize {
        self.disabled.values().filter(|disabled| **disabled).count()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_mod_is_retried_until_it_exhausts_its_budget() {
        let mut tracker = FaultTracker::new();
        assert_eq!(tracker.record_fault("m").action, FaultAction::Retry);
        assert_eq!(tracker.record_fault("m").action, FaultAction::Retry);
        assert_eq!(tracker.record_fault("m").action, FaultAction::Disable);
        assert!(tracker.is_disabled("m"));
        assert_eq!(tracker.disabled_count(), 1);
    }

    #[test]
    fn a_success_resets_the_budget() {
        let mut tracker = FaultTracker::new();
        tracker.record_fault("m");
        tracker.record_fault("m");
        tracker.record_success("m");
        assert_eq!(tracker.faults_for("m"), 0);
        assert_eq!(tracker.record_fault("m").action, FaultAction::Retry);
    }

    #[test]
    fn a_success_does_not_re_enable_a_disabled_mod() {
        let mut tracker = FaultTracker::new();
        for _ in 0..MAX_FAULTS {
            tracker.record_fault("m");
        }
        tracker.record_success("m");
        assert!(tracker.is_disabled("m"), "only a user action re-enables");
    }

    #[test]
    fn clearing_re_enables_a_mod() {
        let mut tracker = FaultTracker::new();
        for _ in 0..MAX_FAULTS {
            tracker.record_fault("m");
        }
        tracker.clear("m");
        assert!(!tracker.is_disabled("m"));
        assert_eq!(tracker.faults_for("m"), 0);
    }

    #[test]
    fn faults_are_tracked_per_mod() {
        let mut tracker = FaultTracker::new();
        for _ in 0..MAX_FAULTS {
            tracker.record_fault("bad");
        }
        tracker.record_fault("good");
        assert!(tracker.is_disabled("bad"));
        assert!(!tracker.is_disabled("good"));
        assert_eq!(tracker.disabled_count(), 1);
    }

    #[test]
    fn the_threshold_boundary_is_exact() {
        assert_eq!(decide_after_fault(1).action, FaultAction::Retry);
        assert_eq!(decide_after_fault(2).action, FaultAction::Retry);
        assert_eq!(decide_after_fault(3).action, FaultAction::Disable);
        assert_eq!(decide_after_fault(99).action, FaultAction::Disable);
    }
}
