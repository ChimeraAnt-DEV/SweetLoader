//! The reconnect policy, as arithmetic rather than as a loop inside a socket thread.
//!
//! Keeping this pure is what makes the interesting questions testable: does the sequence reach the
//! cap, does it ever exceed the 20-second ceiling the launcher promises, and is the jitter actually
//! spread rather than a fixed offset that every client in a lobby would share. A retry storm from a
//! few thousand clients reconnecting in lockstep is the failure this exists to prevent, so the
//! jitter is not decoration.

/// Exponential backoff with a cap and full jitter.
///
/// `delay(attempt)` grows by `factor` each attempt from `base_ms`, never exceeding `cap_ms`, and then
/// applies jitter drawn from `[delay/2, delay]`. Full jitter (anywhere in `[0, delay]`) makes some
/// retries nearly immediate, which is wrong for a service that is merely busy; half jitter keeps a
/// floor under the wait while still spreading the herd.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BackoffPolicy {
    base_ms: u64,
    factor: u32,
    cap_ms: u64,
}

impl Default for BackoffPolicy {
    fn default() -> Self {
        // 500ms, 1s, 2s, 4s, 8s, 16s, then the 20s cap. The launcher's whole reconnect budget is
        // sized around this cap: a user who cancels is never more than 20s from the next attempt.
        Self { base_ms: 500, factor: 2, cap_ms: 20_000 }
    }
}

impl BackoffPolicy {
    /// Builds a policy. Panics on a zero base or factor, which would make the sequence constant.
    pub fn new(base_ms: u64, factor: u32, cap_ms: u64) -> Self {
        assert!(base_ms > 0, "backoff base must be non-zero");
        assert!(factor >= 1, "backoff factor must be at least 1");
        assert!(cap_ms >= base_ms, "backoff cap must not be below the base");
        Self { base_ms, factor, cap_ms }
    }

    /// The cap, in milliseconds.
    pub fn cap_ms(&self) -> u64 {
        self.cap_ms
    }

    /// The un-jittered delay for `attempt` (0-based).
    pub fn nominal_ms(&self, attempt: u32) -> u64 {
        let mut delay = self.base_ms;
        for _ in 0..attempt {
            delay = delay.saturating_mul(self.factor as u64);
            if delay >= self.cap_ms {
                return self.cap_ms;
            }
        }
        delay.min(self.cap_ms)
    }

    /// The delay for `attempt`, jittered into `[nominal/2, nominal]` using `rng` in `[0, 1)`.
    ///
    /// `rng` is injected rather than drawn internally so a test can pin the boundaries exactly and
    /// so the transport can use a seeded generator without this module knowing about it.
    pub fn delay_ms(&self, attempt: u32, rng: f64) -> u64 {
        let nominal = self.nominal_ms(attempt);
        let half = nominal / 2;
        let spread = nominal - half;
        let clamped = rng.clamp(0.0, 0.999_999);
        half + (spread as f64 * clamped) as u64
    }

    /// True once the delay has reached the cap, i.e. further attempts wait the same amount.
    pub fn is_saturated(&self, attempt: u32) -> bool {
        self.nominal_ms(attempt) >= self.cap_ms
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_default_sequence_doubles_from_500ms() {
        let p = BackoffPolicy::default();
        assert_eq!(p.nominal_ms(0), 500);
        assert_eq!(p.nominal_ms(1), 1_000);
        assert_eq!(p.nominal_ms(2), 2_000);
        assert_eq!(p.nominal_ms(3), 4_000);
        assert_eq!(p.nominal_ms(4), 8_000);
        assert_eq!(p.nominal_ms(5), 16_000);
    }

    #[test]
    fn the_sequence_saturates_at_the_cap_and_never_exceeds_it() {
        let p = BackoffPolicy::default();
        for attempt in 0..64 {
            assert!(
                p.nominal_ms(attempt) <= 20_000,
                "attempt {attempt} exceeded the cap"
            );
        }
        assert_eq!(p.nominal_ms(6), 20_000);
        assert_eq!(p.nominal_ms(40), 20_000);
        assert!(p.is_saturated(6));
        assert!(!p.is_saturated(5));
    }

    #[test]
    fn jitter_stays_within_the_half_to_full_band() {
        let p = BackoffPolicy::default();
        for attempt in 0..10 {
            let nominal = p.nominal_ms(attempt);
            assert_eq!(p.delay_ms(attempt, 0.0), nominal / 2, "floor at attempt {attempt}");
            // The top of the band is nominal - 1 at the very edge, and nominal for any rng < 1.
            let top = p.delay_ms(attempt, 0.999_999);
            assert!(top <= nominal, "attempt {attempt}: {top} > {nominal}");
            assert!(top >= nominal / 2, "attempt {attempt}: {top} < floor");
        }
    }

    #[test]
    fn jitter_spreads_across_the_band_rather_than_landing_on_one_value() {
        let p = BackoffPolicy::default();
        let samples: std::collections::HashSet<u64> =
            (0..50).map(|i| p.delay_ms(6, i as f64 / 50.0)).collect();
        assert!(
            samples.len() > 10,
            "jitter produced only {} distinct values, so clients would retry in lockstep",
            samples.len()
        );
    }

    #[test]
    fn an_out_of_range_rng_is_clamped_rather_than_wrapping() {
        let p = BackoffPolicy::default();
        let nominal = p.nominal_ms(3);
        // The top of the band is one below the nominal, because the clamp keeps rng strictly < 1.
        assert_eq!(p.delay_ms(3, 5.0), nominal - 1);
        assert_eq!(p.delay_ms(3, -1.0), nominal / 2);
    }

    #[test]
    fn a_large_attempt_does_not_overflow() {
        let p = BackoffPolicy::default();
        assert_eq!(p.nominal_ms(u32::MAX), 20_000);
        assert!(p.delay_ms(u32::MAX, 0.5) <= 20_000);
    }

    #[test]
    fn a_custom_policy_is_honoured() {
        let p = BackoffPolicy::new(100, 3, 1_000);
        assert_eq!(p.nominal_ms(0), 100);
        assert_eq!(p.nominal_ms(1), 300);
        assert_eq!(p.nominal_ms(2), 900);
        assert_eq!(p.nominal_ms(3), 1_000);
        assert_eq!(p.cap_ms(), 1_000);
    }

    #[test]
    #[should_panic(expected = "backoff base must be non-zero")]
    fn a_zero_base_is_refused() {
        BackoffPolicy::new(0, 2, 100);
    }

    #[test]
    #[should_panic(expected = "backoff factor must be at least 1")]
    fn a_zero_factor_is_refused() {
        BackoffPolicy::new(100, 0, 1_000);
    }

    #[test]
    #[should_panic(expected = "backoff cap must not be below the base")]
    fn a_cap_below_the_base_is_refused() {
        BackoffPolicy::new(1_000, 2, 100);
    }

    #[test]
    fn a_factor_of_one_is_constant_and_never_exceeds_the_cap() {
        let p = BackoffPolicy::new(250, 1, 250);
        assert_eq!(p.nominal_ms(0), 250);
        assert_eq!(p.nominal_ms(9), 250);
        assert!(p.is_saturated(0));
    }
}
