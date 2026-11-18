//! Frame-budget policy: a mod that overruns the tick budget is a fault.
//!
//! The loader calls a mod's `universe_tick` from the game's frame thread. A mod that blocks or
//! loops there freezes the frame, which is the same failure the whole format exists to avoid. A
//! running native call cannot be interrupted from another thread, so the loader cannot *stop* an
//! overrunning tick — but it can measure it, and a mod that overruns on every frame must not keep
//! being called.
//!
//! This module is the pure half: given a tick's elapsed time and the budget, is that a fault? The
//! loader supplies the clock and counts the faults through the existing [`crate::fault`] tracker, so
//! a mod that overruns [`MAX_FAULTS`](crate::fault::MAX_FAULTS) frames in a row is disabled exactly
//! as a panicking mod is. The policy is unit-tested without a device, a clock or a real mod.

/// The per-frame budget a mod's tick should stay within, in microseconds.
///
/// 2 ms: comfortably inside a 60 fps frame (16.6 ms) while leaving room for the game's own work, and
/// generous enough that a mod doing real per-frame math is not tripped by ordinary jitter. A tick
/// that exceeds this on every frame is not "slow", it is stuck.
pub const TICK_BUDGET_US: u64 = 2_000;

/// Whether a tick that took `elapsed_us` is within budget.
///
/// A tick exactly on the budget is within it: the budget is an upper bound, not a strict inequality,
/// so the boundary is not a source of off-by-one disagreements between a test and the device.
pub fn within_budget(elapsed_us: u64) -> bool {
    elapsed_us <= TICK_BUDGET_US
}

/// The reason a tick is a fault, for the log line, or `None` when it is within budget.
///
/// Returned as an owned string rather than a bool so the loader can log *why* a mod was faulted
/// without recomputing the comparison, and so the copy lives in one place.
pub fn overrun_reason(elapsed_us: u64) -> Option<String> {
    if within_budget(elapsed_us) {
        None
    } else {
        Some(format!(
            "tick took {elapsed_us}us, over the {TICK_BUDGET_US}us budget"
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_fast_tick_is_within_budget() {
        assert!(within_budget(0));
        assert!(within_budget(500));
        assert!(overrun_reason(500).is_none());
    }

    #[test]
    fn a_tick_exactly_on_the_budget_is_within_it() {
        assert!(within_budget(TICK_BUDGET_US));
        assert!(overrun_reason(TICK_BUDGET_US).is_none());
    }

    #[test]
    fn a_tick_past_the_budget_is_an_overrun() {
        assert!(!within_budget(TICK_BUDGET_US + 1));
        let reason = overrun_reason(TICK_BUDGET_US + 1).expect("an overrun has a reason");
        assert!(reason.contains(&(TICK_BUDGET_US + 1).to_string()));
        assert!(reason.contains("over the"));
    }

    #[test]
    fn a_stuck_tick_is_reported_with_its_own_time() {
        let reason = overrun_reason(250_000).unwrap();
        assert!(reason.contains("250000us"));
    }
}
