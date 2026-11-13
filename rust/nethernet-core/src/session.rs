//! The transport lifecycle, as a state machine that can be driven by tests rather than by a socket.
//!
//! The socket thread is the part that cannot be tested on the build host, so it owns nothing but I/O.
//! Every decision — which transitions are legal, when a state has taken too long, what the user is
//! told when it fails — lives here, where a test can walk the whole graph in microseconds.
//!
//! The states map onto the Kotlin [`ConnectionState`] one-for-one, and [`TransportState::code`] is
//! the integer the C ABI exposes, so the JNI layer never has to interpret a string.

use std::time::{Duration, Instant};

/// Where the transport is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransportState {
    /// Nothing started, or shut down cleanly.
    Idle,
    /// The signaling socket is being opened and the hello sent.
    Signaling,
    /// Waiting for the service to assign a session.
    AwaitingWelcome,
    /// A peer session has been negotiated and the data path is opening.
    Connecting,
    /// The data path is open.
    Connected,
    /// The session ended; the reason is in the last error.
    Closed,
    /// The session failed; the reason is in the last error.
    Failed,
}

impl TransportState {
    /// The integer the C ABI exposes. Stable: a value here is part of the native interface.
    pub fn code(self) -> i32 {
        match self {
            Self::Idle => 0,
            Self::Signaling => 1,
            Self::AwaitingWelcome => 2,
            Self::Connecting => 3,
            Self::Connected => 4,
            Self::Closed => 5,
            Self::Failed => 6,
        }
    }

    /// True while the transport is doing work the UI should show progress for.
    pub fn is_busy(self) -> bool {
        matches!(self, Self::Signaling | Self::AwaitingWelcome | Self::Connecting)
    }

    /// True once no further transition can happen without a new connect.
    pub fn is_terminal(self) -> bool {
        matches!(self, Self::Closed | Self::Failed)
    }
}

/// Why a transport session failed, in the terms the UI speaks.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TransportError {
    /// The signaling host could not be reached or resolved.
    SignalingUnreachable(String),
    /// The signaling service spoke a protocol this client does not.
    ProtocolMismatch { client: u32, server: u32 },
    /// The service refused the connection.
    ServiceRefused { code: String, message: String },
    /// A state outlived its budget.
    Timeout(&'static str),
    /// The peer or service closed the session.
    PeerClosed(String),
    /// The transport was cancelled by the user.
    Cancelled,
    /// A frame could not be framed or parsed.
    Malformed(String),
}

impl TransportError {
    /// A stable, machine-readable tag. The Kotlin side maps this to user-facing copy, so the tag
    /// must not change without a matching change there.
    pub fn tag(&self) -> &'static str {
        match self {
            Self::SignalingUnreachable(_) => "signaling_unreachable",
            Self::ProtocolMismatch { .. } => "protocol_mismatch",
            Self::ServiceRefused { .. } => "service_refused",
            Self::Timeout(_) => "timeout",
            Self::PeerClosed(_) => "peer_closed",
            Self::Cancelled => "cancelled",
            Self::Malformed(_) => "malformed",
        }
    }

    /// True when retrying the same operation could plausibly succeed.
    pub fn is_retryable(&self) -> bool {
        match self {
            Self::SignalingUnreachable(_) | Self::Timeout(_) | Self::PeerClosed(_) => true,
            Self::ProtocolMismatch { .. }
            | Self::ServiceRefused { .. }
            | Self::Cancelled
            | Self::Malformed(_) => false,
        }
    }

    /// The message shown to the user. Never a raw code, never a stack word.
    pub fn user_message(&self) -> String {
        match self {
            Self::SignalingUnreachable(d) => {
                format!("Could not reach the connection service{}.", detail(d))
            }
            Self::ProtocolMismatch { client, server } => format!(
                "The connection service speaks a newer protocol ({server}) than this build ({client}). Update the app."
            ),
            Self::ServiceRefused { code, message } => {
                let detail = if message.is_empty() { code.as_str() } else { message.as_str() };
                format!("The connection service refused the session: {detail}")
            }
            Self::Timeout(step) => format!("Timed out while {step}."),
            Self::PeerClosed(reason) => {
                if reason.is_empty() {
                    "The session was closed by the other side.".to_string()
                } else {
                    format!("The session was closed: {reason}")
                }
            }
            Self::Cancelled => "Connection cancelled.".to_string(),
            Self::Malformed(d) => format!("The connection service sent something unreadable{}.", detail(d)),
        }
    }
}

fn detail(d: &str) -> String {
    if d.is_empty() {
        String::new()
    } else {
        format!(" ({d})")
    }
}

impl std::fmt::Display for TransportError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.user_message())
    }
}

impl std::error::Error for TransportError {}

/// The state machine.
///
/// Transitions are explicit and an illegal one is refused rather than followed, so a skipped
/// precondition surfaces as `false` from [`Session::transition`] in a test instead of as a hang in the
/// field. The clock is injected so the timeouts can be asserted without sleeping.
pub struct Session {
    state: TransportState,
    entered_at: Instant,
    /// The budget per state. A state with no entry has no timeout.
    budgets: Vec<(TransportState, Duration)>,
    error: Option<TransportError>,
    /// The session id the service assigned, once there is one.
    session_id: Option<String>,
    now: fn() -> Instant,
}

impl Default for Session {
    fn default() -> Self {
        Self::new()
    }
}

impl Session {
    pub fn new() -> Self {
        Self {
            state: TransportState::Idle,
            entered_at: Instant::now(),
            budgets: vec![
                (TransportState::Signaling, Duration::from_secs(15)),
                (TransportState::AwaitingWelcome, Duration::from_secs(20)),
                (TransportState::Connecting, Duration::from_secs(20)),
            ],
            error: None,
            session_id: None,
            now: Instant::now,
        }
    }

    /// Replaces the clock. For tests only.
    #[cfg(test)]
    fn with_clock(mut self, now: fn() -> Instant) -> Self {
        self.now = now;
        self
    }

    pub fn state(&self) -> TransportState {
        self.state
    }

    pub fn error(&self) -> Option<&TransportError> {
        self.error.as_ref()
    }

    pub fn session_id(&self) -> Option<&str> {
        self.session_id.as_deref()
    }

    /// The budget for a state, if it has one.
    pub fn budget(&self, state: TransportState) -> Option<Duration> {
        self.budgets.iter().find(|(s, _)| *s == state).map(|(_, d)| *d)
    }

    /// The legal next states from the current one.
    pub fn allowed_edges(&self) -> &'static [TransportState] {
        use TransportState::*;
        match self.state {
            Idle => &[Signaling, Failed, Closed],
            Signaling => &[AwaitingWelcome, Failed, Closed],
            AwaitingWelcome => &[Connecting, Failed, Closed],
            Connecting => &[Connected, Failed, Closed],
            Connected => &[Closed, Failed],
            Closed | Failed => &[Idle],
        }
    }

    /// True when `to` is a legal transition from the current state.
    pub fn can_transition_to(&self, to: TransportState) -> bool {
        self.allowed_edges().contains(&to)
    }

    /// Moves to `to`, or refuses.
    pub fn transition(&mut self, to: TransportState) -> bool {
        if !self.can_transition_to(to) {
            return false;
        }
        self.state = to;
        self.entered_at = (self.now)();
        if !to.is_terminal() {
            self.error = None;
        }
        true
    }

    /// Moves to [`TransportState::Failed`] with a reason.
    pub fn fail(&mut self, error: TransportError) -> bool {
        if !self.can_transition_to(TransportState::Failed) {
            return false;
        }
        self.state = TransportState::Failed;
        self.entered_at = (self.now)();
        self.error = Some(error);
        true
    }

    /// Records the service-assigned session id.
    pub fn set_session_id(&mut self, id: impl Into<String>) {
        self.session_id = Some(id.into());
    }

    /// How long the current state has been held.
    pub fn elapsed(&self) -> Duration {
        (self.now)().saturating_duration_since(self.entered_at)
    }

    /// Fails the session when the current state has outlived its budget. Returns the error it raised.
    ///
    /// Called from the worker's poll loop rather than by a timer thread, so there is one place that
    /// can end a stalled session and no second thread to race with it.
    pub fn check_timeout(&mut self) -> Option<TransportError> {
        let budget = self.budget(self.state)?;
        if self.elapsed() < budget {
            return None;
        }
        let step = match self.state {
            TransportState::Signaling => "opening the connection",
            TransportState::AwaitingWelcome => "waiting for the connection service",
            TransportState::Connecting => "connecting to the session",
            _ => "waiting",
        };
        let error = TransportError::Timeout(step);
        self.fail(error.clone());
        Some(error)
    }

    /// Resets to [`TransportState::Idle`] for a new connect, clearing the previous error.
    pub fn reset(&mut self) {
        self.state = TransportState::Idle;
        self.entered_at = (self.now)();
        self.error = None;
        self.session_id = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    /// A clock the test drives, so no test sleeps.
    static FAKE_NANOS: AtomicU64 = AtomicU64::new(0);

    fn fake_now() -> Instant {
        Instant::now() + Duration::from_nanos(FAKE_NANOS.load(Ordering::Relaxed))
    }

    fn advance_ms(ms: u64) {
        FAKE_NANOS.fetch_add(ms * 1_000_000, Ordering::Relaxed);
    }

    fn session() -> Session {
        FAKE_NANOS.store(0, Ordering::Relaxed);
        Session::new().with_clock(fake_now)
    }

    #[test]
    fn the_happy_path_walks_every_state() {
        let mut s = session();
        assert_eq!(s.state(), TransportState::Idle);
        assert!(s.transition(TransportState::Signaling));
        assert!(s.transition(TransportState::AwaitingWelcome));
        s.set_session_id("s-1");
        assert!(s.transition(TransportState::Connecting));
        assert!(s.transition(TransportState::Connected));
        assert!(s.transition(TransportState::Closed));
        assert_eq!(s.session_id(), Some("s-1"));
    }

    #[test]
    fn idle_cannot_jump_straight_to_connected() {
        let mut s = session();
        assert!(!s.transition(TransportState::Connected));
        assert_eq!(s.state(), TransportState::Idle, "a refused transition must not move");
    }

    #[test]
    fn every_non_terminal_state_can_fail_or_close() {
        for start in [
            TransportState::Idle,
            TransportState::Signaling,
            TransportState::AwaitingWelcome,
            TransportState::Connecting,
            TransportState::Connected,
        ] {
            let mut s = session();
            // Walk to `start`.
            for step in [
                TransportState::Signaling,
                TransportState::AwaitingWelcome,
                TransportState::Connecting,
                TransportState::Connected,
            ] {
                if s.state() == start {
                    break;
                }
                assert!(s.transition(step), "could not reach {start:?}");
            }
            assert_eq!(s.state(), start);
            let mut f = s.clone_state();
            assert!(f.fail(TransportError::Cancelled) || f.transition(TransportState::Closed));
        }
    }

    impl Session {
        fn clone_state(&self) -> Session {
            Session {
                state: self.state,
                entered_at: self.entered_at,
                budgets: self.budgets.clone(),
                error: self.error.clone(),
                session_id: self.session_id.clone(),
                now: self.now,
            }
        }
    }

    #[test]
    fn a_terminal_state_returns_to_idle_for_a_new_connect() {
        let mut s = session();
        s.transition(TransportState::Signaling);
        s.fail(TransportError::Cancelled);
        assert!(s.transition(TransportState::Idle));
        assert!(s.error().is_none(), "a new connect must clear the previous failure");
    }

    #[test]
    fn a_stalled_state_times_out_at_its_budget() {
        let mut s = session();
        s.transition(TransportState::Signaling);
        advance_ms(14_999);
        assert!(s.check_timeout().is_none(), "must not fire early");
        advance_ms(1);
        let error = s.check_timeout().expect("the budget must fire at exactly 15s");
        assert_eq!(error, TransportError::Timeout("opening the connection"));
        assert_eq!(s.state(), TransportState::Failed);
    }

    #[test]
    fn a_state_without_a_budget_never_times_out() {
        let mut s = session();
        s.transition(TransportState::Signaling);
        s.transition(TransportState::AwaitingWelcome);
        s.transition(TransportState::Connecting);
        s.transition(TransportState::Connected);
        advance_ms(60 * 60 * 1_000);
        assert!(s.check_timeout().is_none(), "a connected session has no budget");
    }

    #[test]
    fn the_state_codes_are_stable_and_distinct() {
        let states = [
            TransportState::Idle,
            TransportState::Signaling,
            TransportState::AwaitingWelcome,
            TransportState::Connecting,
            TransportState::Connected,
            TransportState::Closed,
            TransportState::Failed,
        ];
        let codes: Vec<i32> = states.iter().map(|s| s.code()).collect();
        assert_eq!(codes, vec![0, 1, 2, 3, 4, 5, 6]);
        let unique: std::collections::HashSet<i32> = codes.iter().copied().collect();
        assert_eq!(unique.len(), states.len());
    }

    #[test]
    fn busy_and_terminal_classify_every_state() {
        for state in [
            TransportState::Idle,
            TransportState::Signaling,
            TransportState::AwaitingWelcome,
            TransportState::Connecting,
            TransportState::Connected,
            TransportState::Closed,
            TransportState::Failed,
        ] {
            assert!(!(state.is_busy() && state.is_terminal()), "{state:?} is both");
        }
        assert!(TransportState::Signaling.is_busy());
        assert!(TransportState::AwaitingWelcome.is_busy());
        assert!(TransportState::Connecting.is_busy());
        assert!(!TransportState::Connected.is_busy());
        assert!(TransportState::Closed.is_terminal());
        assert!(TransportState::Failed.is_terminal());
    }

    #[test]
    fn only_transient_failures_are_retryable() {
        assert!(TransportError::SignalingUnreachable(String::new()).is_retryable());
        assert!(TransportError::Timeout("x").is_retryable());
        assert!(TransportError::PeerClosed(String::new()).is_retryable());
        assert!(!TransportError::Cancelled.is_retryable());
        assert!(!TransportError::ProtocolMismatch { client: 1, server: 2 }.is_retryable());
        assert!(!TransportError::ServiceRefused { code: "b".into(), message: String::new() }.is_retryable());
        assert!(!TransportError::Malformed(String::new()).is_retryable());
    }

    #[test]
    fn every_error_tag_is_distinct_and_snake_case() {
        let errors = [
            TransportError::SignalingUnreachable(String::new()),
            TransportError::ProtocolMismatch { client: 1, server: 2 },
            TransportError::ServiceRefused { code: "c".into(), message: String::new() },
            TransportError::Timeout("x"),
            TransportError::PeerClosed(String::new()),
            TransportError::Cancelled,
            TransportError::Malformed(String::new()),
        ];
        let tags: Vec<&str> = errors.iter().map(|e| e.tag()).collect();
        let unique: std::collections::HashSet<&str> = tags.iter().copied().collect();
        assert_eq!(unique.len(), errors.len(), "tags must be distinct");
        for tag in tags {
            assert!(tag.chars().all(|c| c.is_ascii_lowercase() || c == '_'), "{tag}");
        }
    }

    #[test]
    fn user_copy_never_leaks_a_code_or_a_stack_word() {
        let errors = [
            TransportError::SignalingUnreachable("dns".into()),
            TransportError::ProtocolMismatch { client: 1, server: 9 },
            TransportError::ServiceRefused { code: "busy".into(), message: "try later".into() },
            TransportError::Timeout("connecting"),
            TransportError::PeerClosed("host left".into()),
            TransportError::Cancelled,
            TransportError::Malformed("bad json".into()),
        ];
        for error in errors {
            let text = error.user_message();
            assert!(!text.is_empty());
            for word in ["panic", "null", "unwrap", "Exception", "SIGSEGV"] {
                assert!(!text.contains(word), "{text} contains {word}");
            }
        }
    }

    #[test]
    fn the_protocol_mismatch_copy_names_both_versions() {
        let text = TransportError::ProtocolMismatch { client: 1, server: 3 }.user_message();
        assert!(text.contains('3'));
        assert!(text.contains('1'));
    }

    #[test]
    fn reset_clears_the_session_and_the_error() {
        let mut s = session();
        s.transition(TransportState::Signaling);
        s.set_session_id("s-9");
        s.fail(TransportError::Cancelled);
        s.reset();
        assert_eq!(s.state(), TransportState::Idle);
        assert!(s.error().is_none());
        assert!(s.session_id().is_none());
    }
}
