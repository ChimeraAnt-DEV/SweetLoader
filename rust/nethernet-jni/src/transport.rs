//! The blocking NetherNet signaling transport.
//!
//! One worker thread owns one WebSocket. The worker's only jobs are I/O and reporting: it opens the
//! socket, sends the hello, feeds received frames to [`nethernet_core::signaling`], and publishes
//! every state change into a shared [`Handle`]. Every decision about *what a message means* lives in
//! `nethernet-core`, which is tested without a socket.
//!
//! The thread is deliberately dumb, because it is the part that cannot be tested on the build host.
//! It is also deliberately unable to panic out into the caller: the JNI entry points wrap it in
//! `catch_unwind`, and the worker itself turns a panic in a message handler into a `Failed` state
//! rather than an abort.
//!
//! ## Why blocking rather than async
//!
//! The JNI boundary is blocking — Java calls `connect`, later calls `poll`. A blocking worker on its
//! own thread therefore fits the caller with no runtime to own, no executor to shut down, and no
//! `tokio` in the game process. The cost is one thread per connection, which for a client with one
//! multiplayer session is the right trade.

use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use nethernet_core::backoff::BackoffPolicy;
use nethernet_core::session::{Session, TransportError, TransportState};
use nethernet_core::signaling::{self, Signal};
use tungstenite::stream::MaybeTlsStream;
use tungstenite::{Message, WebSocket};

/// What the JNI layer reads. Guarded by a mutex and a condvar so `poll` can block until something
/// changes instead of spinning.
pub struct Shared {
    pub state: TransportState,
    pub error: Option<TransportError>,
    pub session_id: Option<String>,
    /// The most recent candidate payload the service relayed, waiting for the caller to take it.
    pub pending_candidate: Option<String>,
    /// A monotonically increasing counter bumped on every change, so a caller can tell "nothing new"
    /// from "same value".
    pub revision: u64,
}

impl Default for Shared {
    fn default() -> Self {
        Self {
            state: TransportState::Idle,
            error: None,
            session_id: None,
            pending_candidate: None,
            revision: 0,
        }
    }
}

/// The handle the caller holds. Owns the worker and the shared state.
pub struct Handle {
    shared: Arc<(Mutex<Shared>, Condvar)>,
    cancelled: Arc<AtomicBool>,
    worker: Option<std::thread::JoinHandle<()>>,
}

impl Handle {
    /// Starts a worker that connects to `url` and announces `network_id`.
    ///
    /// Returns immediately. A connection failure is reported through the state, never as an error
    /// from this call, because the caller's UI is already showing "connecting".
    pub fn connect(url: String, network_id: String) -> Self {
        let shared = Arc::new((Mutex::new(Shared::default()), Condvar::new()));
        let cancelled = Arc::new(AtomicBool::new(false));
        let worker_shared = Arc::clone(&shared);
        let worker_cancelled = Arc::clone(&cancelled);

        let worker = std::thread::Builder::new()
            .name("nethernet-signaling".to_string())
            .spawn(move || {
                // A panic here would take down the process; turn it into a Failed state instead.
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    run_worker(url, network_id, &worker_shared, &worker_cancelled);
                }));
                if result.is_err() {
                    publish(
                        &worker_shared,
                        TransportState::Failed,
                        Some(TransportError::Malformed("the connection worker failed".into())),
                        None,
                    );
                }
            })
            .expect("spawning the signaling worker cannot fail");

        Self { shared, cancelled, worker: Some(worker) }
    }

    /// A snapshot of the shared state.
    pub fn snapshot(&self) -> (TransportState, Option<TransportError>, Option<String>) {
        let (lock, _) = &*self.shared;
        let guard = lock.lock().unwrap_or_else(|e| e.into_inner());
        (guard.state, guard.error.clone(), guard.session_id.clone())
    }

    /// Blocks until the state changes or `timeout` passes. Returns the new state.
    pub fn wait_for_change(&self, timeout: Duration) -> TransportState {
        let (lock, condvar) = &*self.shared;
        let guard = lock.lock().unwrap_or_else(|e| e.into_inner());
        let (guard, _) = condvar
            .wait_timeout(guard, timeout)
            .unwrap_or_else(|e| e.into_inner());
        guard.state
    }

    /// Takes the pending candidate payload, if any.
    pub fn take_candidate(&self) -> Option<String> {
        let (lock, _) = &*self.shared;
        let mut guard = lock.lock().unwrap_or_else(|e| e.into_inner());
        guard.pending_candidate.take()
    }

    /// Asks the worker to stop and waits for it. Idempotent.
    pub fn shutdown(&mut self) {
        self.cancelled.store(true, Ordering::Release);
        {
            let (lock, condvar) = &*self.shared;
            let mut guard = lock.lock().unwrap_or_else(|e| e.into_inner());
            guard.revision = guard.revision.wrapping_add(1);
            condvar.notify_all();
        }
        if let Some(worker) = self.worker.take() {
            // The worker checks the flag at most once per poll interval, so this returns quickly.
            let _ = worker.join();
        }
    }

    /// True when the worker has been asked to stop.
    pub fn is_cancelled(&self) -> bool {
        self.cancelled.load(Ordering::Acquire)
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// Publishes a state change and wakes any waiter.
fn publish(
    shared: &Arc<(Mutex<Shared>, Condvar)>,
    state: TransportState,
    error: Option<TransportError>,
    session_id: Option<String>,
) {
    let (lock, condvar) = &**shared;
    let mut guard = lock.lock().unwrap_or_else(|e| e.into_inner());
    guard.state = state;
    guard.error = error;
    if session_id.is_some() {
        guard.session_id = session_id;
    }
    guard.revision = guard.revision.wrapping_add(1);
    condvar.notify_all();
}

/// The worker body: connect, hello, then pump frames until cancelled or closed.
fn run_worker(
    url: String,
    network_id: String,
    shared: &Arc<(Mutex<Shared>, Condvar)>,
    cancelled: &Arc<AtomicBool>,
) {
    let mut session = Session::new();
    session.transition(TransportState::Signaling);
    publish(shared, TransportState::Signaling, None, None);

    // The socket is opened with a bounded retry: a signaling host that is briefly unreachable is the
    // common case, and a single attempt would fail a session that a 500ms retry would have saved.
    let policy = BackoffPolicy::default();
    let mut socket = None;
    let mut attempt = 0u32;
    while socket.is_none() {
        if cancelled.load(Ordering::Acquire) {
            session.fail(TransportError::Cancelled);
            publish(shared, TransportState::Failed, session.error().cloned(), None);
            return;
        }
        if attempt > 0 && policy.is_saturated(attempt) {
            // Two attempts at the cap is enough; past that the host is down, not busy.
            break;
        }
        match open_socket(&url) {
            Ok(ws) => socket = Some(ws),
            Err(e) => {
                let delay = policy.delay_ms(attempt, rng_fraction(attempt));
                if delay > 0 && !sleep_cancellable(Duration::from_millis(delay), cancelled) {
                    session.fail(TransportError::Cancelled);
                    publish(shared, TransportState::Failed, session.error().cloned(), None);
                    return;
                }
                attempt += 1;
                if attempt > 8 {
                    let _ = e;
                    break;
                }
            }
        }
    }

    let Some(mut socket) = socket else {
        session.fail(TransportError::SignalingUnreachable(String::new()));
        publish(shared, TransportState::Failed, session.error().cloned(), None);
        return;
    };

    // Hello, then await the welcome.
    if let Err(e) = socket.send(Message::Text(signaling::encode_client_signal(&signaling::hello(&network_id)))) {
        session.fail(TransportError::SignalingUnreachable(e.to_string()));
        publish(shared, TransportState::Failed, session.error().cloned(), None);
        return;
    }
    session.transition(TransportState::AwaitingWelcome);
    publish(shared, TransportState::AwaitingWelcome, None, None);

    // The socket is non-blocking-friendly: a read timeout lets the loop notice cancellation and the
    // session's own timeout instead of blocking forever inside `read`.
    if let MaybeTlsStream::Plain(stream) = socket.get_ref() {
        let _ = stream.set_read_timeout(Some(Duration::from_millis(250)));
    }

    loop {
        if cancelled.load(Ordering::Acquire) {
            session.fail(TransportError::Cancelled);
            publish(shared, TransportState::Failed, session.error().cloned(), None);
            let _ = socket.close(None);
            return;
        }
        if let Some(error) = session.check_timeout() {
            publish(shared, TransportState::Failed, Some(error), None);
            let _ = socket.close(None);
            return;
        }

        match socket.read() {
            Ok(Message::Text(text)) => {
                if !handle_frame(&mut session, text.as_bytes(), shared) {
                    let _ = socket.close(None);
                    return;
                }
            }
            Ok(Message::Binary(bytes)) => {
                if !handle_frame(&mut session, &bytes, shared) {
                    let _ = socket.close(None);
                    return;
                }
            }
            Ok(Message::Close(_)) => {
                session.fail(TransportError::PeerClosed(String::new()));
                publish(shared, TransportState::Failed, session.error().cloned(), None);
                return;
            }
            Ok(_) => {}
            Err(tungstenite::Error::Io(e))
                if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut =>
            {
                // The read timeout is how the loop stays responsive; not an error.
            }
            Err(tungstenite::Error::ConnectionClosed) => {
                session.fail(TransportError::PeerClosed(String::new()));
                publish(shared, TransportState::Failed, session.error().cloned(), None);
                return;
            }
            Err(e) => {
                session.fail(TransportError::SignalingUnreachable(e.to_string()));
                publish(shared, TransportState::Failed, session.error().cloned(), None);
                return;
            }
        }
    }
}

/// Applies one received frame. Returns false when the session should end.
fn handle_frame(
    session: &mut Session,
    frame: &[u8],
    shared: &Arc<(Mutex<Shared>, Condvar)>,
) -> bool {
    match signaling::parse_server_signal(frame) {
        Ok(Signal::Welcome { session: id, .. }) => {
            session.set_session_id(id.clone());
            session.transition(TransportState::Connecting);
            publish(shared, TransportState::Connecting, None, Some(id));
            true
        }
        Ok(Signal::Connect { session: id, .. }) => {
            session.set_session_id(id.clone());
            if session.state() == TransportState::AwaitingWelcome {
                session.transition(TransportState::Connecting);
            }
            if session.state() == TransportState::Connecting {
                session.transition(TransportState::Connected);
            }
            publish(shared, session.state(), None, Some(id));
            true
        }
        Ok(Signal::Candidate { payload, .. }) => {
            let (lock, condvar) = &**shared;
            let mut guard = lock.lock().unwrap_or_else(|e| e.into_inner());
            guard.pending_candidate = Some(payload);
            guard.revision = guard.revision.wrapping_add(1);
            condvar.notify_all();
            true
        }
        Ok(Signal::Disconnect { reason, .. }) => {
            session.fail(TransportError::PeerClosed(reason));
            publish(shared, TransportState::Failed, session.error().cloned(), None);
            false
        }
        Ok(Signal::ServiceError { code, message }) => {
            session.fail(TransportError::ServiceRefused { code, message });
            publish(shared, TransportState::Failed, session.error().cloned(), None);
            false
        }
        Err(nethernet_core::signaling::SignalError::ProtocolMismatch { client, server }) => {
            session.fail(TransportError::ProtocolMismatch { client, server });
            publish(shared, TransportState::Failed, session.error().cloned(), None);
            false
        }
        Err(e) => {
            // An unreadable frame from an otherwise live service is not fatal on its own: log it and
            // keep going, because a future protocol addition must not kill an established session.
            let _ = e;
            true
        }
    }
}

/// Opens a WebSocket. `ws://` and `wss://` are both handled.
fn open_socket(url: &str) -> Result<WebSocket<MaybeTlsStream<TcpStream>>, tungstenite::Error> {
    let (socket, _response) = tungstenite::connect(url)?;
    Ok(socket)
}

/// Sleeps in small slices so a cancel is honoured promptly. Returns false when cancelled.
fn sleep_cancellable(total: Duration, cancelled: &Arc<AtomicBool>) -> bool {
    let deadline = Instant::now() + total;
    while Instant::now() < deadline {
        if cancelled.load(Ordering::Acquire) {
            return false;
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        std::thread::sleep(remaining.min(Duration::from_millis(50)));
    }
    !cancelled.load(Ordering::Acquire)
}

/// A cheap deterministic fraction in `[0, 1)` for jitter. Seeded from the attempt so two clients that
/// start together do not retry in lockstep, and derived without a dependency on a global RNG.
fn rng_fraction(attempt: u32) -> f64 {
    let now = Instant::now();
    let nanos = now.elapsed().as_nanos() as u64;
    let mixed = nanos
        .wrapping_mul(6_364_136_223_846_793_005)
        .wrapping_add(attempt as u64)
        .rotate_left(17);
    (mixed % 1_000_000) as f64 / 1_000_000.0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_handle_starts_idle_and_shuts_down_cleanly() {
        // No worker is started here: the handle is constructed directly so the test never opens a
        // socket. What is under test is the shared state and the shutdown contract.
        let shared = Arc::new((Mutex::new(Shared::default()), Condvar::new()));
        let mut handle = Handle {
            shared: Arc::clone(&shared),
            cancelled: Arc::new(AtomicBool::new(false)),
            worker: None,
        };
        let (state, error, session) = handle.snapshot();
        assert_eq!(state, TransportState::Idle);
        assert!(error.is_none());
        assert!(session.is_none());
        handle.shutdown();
        assert!(handle.is_cancelled());
    }

    #[test]
    fn publishing_wakes_a_waiter() {
        let shared = Arc::new((Mutex::new(Shared::default()), Condvar::new()));
        let waiter = Arc::clone(&shared);
        let thread = std::thread::spawn(move || {
            let (lock, condvar) = &*waiter;
            let guard = lock.lock().unwrap();
            let (guard, _) = condvar
                .wait_timeout(guard, Duration::from_secs(5))
                .unwrap();
            guard.state
        });
        std::thread::sleep(Duration::from_millis(50));
        publish(&shared, TransportState::AwaitingWelcome, None, None);
        assert_eq!(thread.join().unwrap(), TransportState::AwaitingWelcome);
    }

    #[test]
    fn a_candidate_is_taken_exactly_once() {
        let shared = Arc::new((Mutex::new(Shared::default()), Condvar::new()));
        let handle = Handle {
            shared: Arc::clone(&shared),
            cancelled: Arc::new(AtomicBool::new(false)),
            worker: None,
        };
        {
            let (lock, _) = &*shared;
            lock.lock().unwrap().pending_candidate = Some("payload".into());
        }
        assert_eq!(handle.take_candidate().as_deref(), Some("payload"));
        assert_eq!(handle.take_candidate(), None);
    }

    #[test]
    fn the_rng_fraction_is_in_range() {
        for attempt in 0..100 {
            let value = rng_fraction(attempt);
            assert!((0.0..1.0).contains(&value), "attempt {attempt}: {value}");
        }
    }

    #[test]
    fn a_cancelled_sleep_returns_immediately() {
        let cancelled = Arc::new(AtomicBool::new(true));
        let start = Instant::now();
        assert!(!sleep_cancellable(Duration::from_secs(30), &cancelled));
        assert!(start.elapsed() < Duration::from_secs(1), "cancel must not wait");
    }

    #[test]
    fn an_uncancelled_short_sleep_returns_true() {
        let cancelled = Arc::new(AtomicBool::new(false));
        assert!(sleep_cancellable(Duration::from_millis(20), &cancelled));
    }
}
