//! `libsweetloader_nethernet.so` — the JNI bridge for the NetherNet transport.
//!
//! Like the `.universe` and `.singularity` bridges, this crate is a shell: the protocol, the
//! framing, the backoff policy and the session state machine all live in `nethernet-core`, where
//! they are unit-tested on the build host. What this crate adds is exactly the three things a
//! `cdylib` inside `libminecraftpe.so` needs and a pure library cannot have:
//!
//! * **Logging** routed to the client's logcat tag, so a connection failure is visible in the same
//!   place as the rest of the client.
//! * **A panic hook**, so a panic in the transport is caught and logged rather than unwinding into
//!   the game's C++ frames.
//! * **A cancellation flag**, an `AtomicBool` the launcher flips when the user cancels, which the
//!   worker polls between reads.
//!
//! Every JNI entry point is wrapped in `catch_unwind`. A panic crossing the JNI boundary is
//! undefined behaviour, and "the game thread never crashes" is a promise this crate keeps by
//! construction rather than by care.

pub mod transport;

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

use jni::objects::{JClass, JString};
use jni::sys::{jboolean, jint, jstring};
use jni::JNIEnv;
use nethernet_core::session::TransportState;

use transport::Handle;

/// The log tag every message from this library carries.
pub const LOG_TAG: &str = "SweetLoader";

/// The live connection, if any. One at a time: the launcher joins one world.
static CONNECTION: OnceLock<Mutex<Option<Handle>>> = OnceLock::new();

/// Set when the launcher asks the transport to stop.
static CANCELLED: AtomicBool = AtomicBool::new(false);

fn connection() -> &'static Mutex<Option<Handle>> {
    CONNECTION.get_or_init(|| Mutex::new(None))
}

/// Installs the logger and the panic hook, once.
fn install_runtime() {
    static INSTALLED: std::sync::Once = std::sync::Once::new();
    INSTALLED.call_once(|| {
        #[cfg(target_os = "android")]
        android_logger::init_once(
            android_logger::Config::default()
                .with_max_level(log::LevelFilter::Debug)
                .with_tag(LOG_TAG),
        );
        let previous = std::panic::take_hook();
        std::panic::set_hook(Box::new(move |info| {
            // Record the panic so the user sees why the connection stopped, then let the default
            // hook run. The entry points catch the unwind before it reaches Java.
            log_error(&format!("nethernet panic: {info}"));
            previous(info);
        }));
    });
}

fn log_info(message: &str) {
    #[cfg(target_os = "android")]
    log::info!("{message}");
    #[cfg(not(target_os = "android"))]
    eprintln!("[nethernet] {message}");
}

fn log_error(message: &str) {
    #[cfg(target_os = "android")]
    log::error!("{message}");
    #[cfg(not(target_os = "android"))]
    eprintln!("[nethernet] {message}");
}

/// A `jstring` from a Rust string, or a null pointer when the JVM refuses.
fn to_jstring(env: &mut JNIEnv, value: &str) -> jstring {
    match env.new_string(value) {
        Ok(s) => s.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

/// Runs `body`, returning a neutral value if it panics.
fn guarded<T: Default>(what: &str, body: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(value) => value,
        Err(_) => {
            log_error(&format!("{what} panicked; returning the safe default"));
            T::default()
        }
    }
}

/// Starts a connection to the signaling service.
///
/// `url` is a `ws://` or `wss://` endpoint, `network_id` the client's own id. Returns true when the
/// worker started; a connection failure arrives through [`nativeState`], not here.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeConnect(
    mut env: JNIEnv,
    _class: JClass,
    url: JString,
    network_id: JString,
) -> jboolean {
    guarded("nativeConnect", || {
        install_runtime();
        let url: String = match env.get_string(&url) {
            Ok(s) => s.into(),
            Err(_) => {
                log_error("nativeConnect was given an unreadable url");
                return 0;
            }
        };
        let network_id: String = match env.get_string(&network_id) {
            Ok(s) => s.into(),
            Err(_) => {
                log_error("nativeConnect was given an unreadable network id");
                return 0;
            }
        };
        if url.is_empty() || network_id.is_empty() {
            log_error("nativeConnect requires a url and a network id");
            return 0;
        }
        // One connection at a time: shut any previous one down before starting a new one, so a
        // reconnect cannot leave two workers fighting over the shared state.
        let mut slot = connection().lock().unwrap_or_else(|e| e.into_inner());
        if let Some(mut previous) = slot.take() {
            previous.shutdown();
        }
        CANCELLED.store(false, Ordering::Release);
        log_info(&format!("connecting to {url} as network id {network_id}"));
        *slot = Some(Handle::connect(url, network_id));
        1
    })
}

/// The current state, as the stable integer [`TransportState::code`] defines.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeState(
    _env: JNIEnv,
    _class: JClass,
) -> jint {
    guarded("nativeState", || {
        let slot = connection().lock().unwrap_or_else(|e| e.into_inner());
        match slot.as_ref() {
            Some(handle) => handle.snapshot().0.code(),
            None => TransportState::Idle.code(),
        }
    })
}

/// The last error's machine-readable tag, or null when there is none.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeErrorTag(
    mut env: JNIEnv,
    _class: JClass,
) -> jstring {
    guarded("nativeErrorTag", || {
        let slot = connection().lock().unwrap_or_else(|e| e.into_inner());
        let tag = slot
            .as_ref()
            .and_then(|h| h.snapshot().1)
            .map(|e| e.tag().to_string());
        match tag {
            Some(tag) => to_jstring(&mut env, &tag),
            None => std::ptr::null_mut(),
        }
    })
}

/// The last error's user-facing message, or null when there is none.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeErrorMessage(
    mut env: JNIEnv,
    _class: JClass,
) -> jstring {
    guarded("nativeErrorMessage", || {
        let slot = connection().lock().unwrap_or_else(|e| e.into_inner());
        let message = slot
            .as_ref()
            .and_then(|h| h.snapshot().1)
            .map(|e| e.user_message());
        match message {
            Some(message) => to_jstring(&mut env, &message),
            None => std::ptr::null_mut(),
        }
    })
}

/// True when the last error could be retried.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeErrorRetryable(
    _env: JNIEnv,
    _class: JClass,
) -> jboolean {
    guarded("nativeErrorRetryable", || {
        let slot = connection().lock().unwrap_or_else(|e| e.into_inner());
        match slot.as_ref().and_then(|h| h.snapshot().1) {
            Some(error) if error.is_retryable() => 1,
            _ => 0,
        }
    })
}

/// The session id the service assigned, or null.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeSessionId(
    mut env: JNIEnv,
    _class: JClass,
) -> jstring {
    guarded("nativeSessionId", || {
        let slot = connection().lock().unwrap_or_else(|e| e.into_inner());
        let id = slot.as_ref().and_then(|h| h.snapshot().2);
        match id {
            Some(id) => to_jstring(&mut env, &id),
            None => std::ptr::null_mut(),
        }
    })
}

/// Waits up to `timeout_ms` for the state to change, then returns the state code.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeAwaitState(
    _env: JNIEnv,
    _class: JClass,
    timeout_ms: jint,
) -> jint {
    guarded("nativeAwaitState", || {
        let timeout = Duration::from_millis(timeout_ms.max(0) as u64);
        let slot = connection().lock().unwrap_or_else(|e| e.into_inner());
        match slot.as_ref() {
            Some(handle) => handle.wait_for_change(timeout).code(),
            None => TransportState::Idle.code(),
        }
    })
}

/// Takes the most recent candidate payload relayed by the service, or null.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeTakeCandidate(
    mut env: JNIEnv,
    _class: JClass,
) -> jstring {
    guarded("nativeTakeCandidate", || {
        let slot = connection().lock().unwrap_or_else(|e| e.into_inner());
        match slot.as_ref().and_then(|h| h.take_candidate()) {
            Some(payload) => to_jstring(&mut env, &payload),
            None => std::ptr::null_mut(),
        }
    })
}

/// Asks the transport to stop and waits for the worker to exit. Idempotent.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeDisconnect(
    _env: JNIEnv,
    _class: JClass,
) {
    guarded("nativeDisconnect", || {
        CANCELLED.store(true, Ordering::Release);
        let mut slot = connection().lock().unwrap_or_else(|e| e.into_inner());
        if let Some(mut handle) = slot.take() {
            handle.shutdown();
            log_info("nethernet connection shut down");
        }
    });
}

/// True when a shutdown has been requested.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeIsCancelled(
    _env: JNIEnv,
    _class: JClass,
) -> jboolean {
    guarded("nativeIsCancelled", || {
        if CANCELLED.load(Ordering::Acquire) {
            1
        } else {
            0
        }
    })
}

/// The state-machine budget for a state code, in milliseconds. 0 when the state has none.
///
/// Exposed so the Kotlin UI can render the same budget the native side enforces, rather than
/// duplicating the number.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeStateBudgetMs(
    _env: JNIEnv,
    _class: JClass,
    state_code: jint,
) -> jint {
    guarded("nativeStateBudgetMs", || {
        let state = match state_code {
            0 => TransportState::Idle,
            1 => TransportState::Signaling,
            2 => TransportState::AwaitingWelcome,
            3 => TransportState::Connecting,
            4 => TransportState::Connected,
            5 => TransportState::Closed,
            6 => TransportState::Failed,
            _ => return 0,
        };
        let session = nethernet_core::session::Session::new();
        match session.budget(state) {
            Some(budget) => budget.as_millis().min(jint::MAX as u128) as jint,
            None => 0,
        }
    })
}

/// The protocol version this client speaks. Exposed for the diagnostics screen.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeProtocolVersion(
    _env: JNIEnv,
    _class: JClass,
) -> jint {
    guarded("nativeProtocolVersion", || {
        nethernet_core::signaling::PROTOCOL_VERSION as jint
    })
}

/// A self-test the launcher can call at startup to confirm the bridge is live.
///
/// Returns a JSON object with the protocol version, the state count, the frame cap and the backoff
/// cap, so a broken bridge is caught by a device test instead of by a failed join.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeSelfTest(
    mut env: JNIEnv,
    _class: JClass,
) -> jstring {
    guarded("nativeSelfTest", || {
        install_runtime();
        let report = format!(
            "{{\"protocol\":{},\"states\":{},\"framing_max\":{},\"backoff_cap_ms\":{}}}",
            nethernet_core::signaling::PROTOCOL_VERSION,
            TransportState::Failed.code() + 1,
            nethernet_core::framing::MAX_FRAME_BYTES,
            nethernet_core::backoff::BackoffPolicy::default().cap_ms(),
        );
        to_jstring(&mut env, &report)
    })
}

/// Logs a message from the Kotlin side into the same tag, so a Java-side note and a native note sit
/// next to each other in logcat.
#[no_mangle]
pub extern "system" fn Java_org_chimeramc_client_core_nethernet_NativeNetherNet_nativeLog(
    mut env: JNIEnv,
    _class: JClass,
    message: JString,
) {
    guarded("nativeLog", || {
        let text: String = match env.get_string(&message) {
            Ok(s) => s.into(),
            Err(_) => return,
        };
        log_info(&text);
    });
}
