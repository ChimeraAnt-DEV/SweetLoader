//! `libcaramel_universe.so` — the JNI cdylib the `.universe` loader loads into the game process.
//!
//! This crate is a shell. All the decisions (manifest parsing, signature verification, safe
//! extraction, load ordering, permissions, config) live in `universe-core`, where they are
//! unit-tested on the build host. What this crate adds is exactly the three things a `cdylib`
//! inside `libminecraftpe.so` needs and a pure library cannot have:
//!
//! * **Logging** routed to the client's logcat tag, so a load failure is visible in the same place
//!   as the rest of the client.
//! * **A panic hook**, so a Rust panic in a mod entry point is caught and logged rather than
//!   unwinding into the game's C++ frames.
//! * **A cancellation flag**, an `AtomicBool` the loader flips when it wants mods to stop touching
//!   shared state (world unload, hot reload), which mods poll between frames.
//!
//! The JNI functions are thin: they marshal arguments, call a `universe-core` function, and marshal
//! the result. Every fallible call returns a message string; an empty string is success. That is the
//! same contract the C++ side already uses for its loaders, so the Java bridge does not need a new
//! error shape.

use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};

use jni::objects::{JClass, JString};
use jni::sys::{jboolean, jstring};
use jni::JNIEnv;

/// The BerryAPI entry points this loader talks to.
///
/// Only the calls that are *declared* here are called through the pointer; everything else the
/// loader needs from the game goes through the C++ side's own resolved seams. Keeping the surface
/// this small means a BerryAPI without a given call still loads — the corresponding feature is
/// reported unavailable rather than the whole bridge failing.
pub mod berry;

/// The log tag every message from this library carries.
pub const LOG_TAG: &str = "SweetLoader";

/// The client version this build of the loader ships with, checked against a package's
/// `min_client_version`.
pub const CLIENT_VERSION: &str = env!("CARGO_PKG_VERSION");

/// Set when the loader wants mods to stop touching shared state; mods poll it between frames.
static CANCELLED: AtomicBool = AtomicBool::new(false);

/// The BerryAPI API table, set once by `nativeInit`. Read-only afterwards.
static BERRY_API: std::sync::OnceLock<usize> = std::sync::OnceLock::new();

/// True when the loader has asked mods to stop.
pub fn is_cancelled() -> bool {
    CANCELLED.load(Ordering::Acquire)
}

/// Asks mods to stop, or clears the request.
pub fn set_cancelled(cancelled: bool) {
    CANCELLED.store(cancelled, Ordering::Release);
}

/// Installs the panic hook and the Android logger, once.
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
            // A panic inside a mod entry point must not unwind into the game's C++ frames. The
            // entry points are wrapped in catch_unwind; this hook only records the panic so the
            // user sees why a mod stopped.
            log_error(&format!("panic: {info}"));
            previous(info);
        }));
    });
}

/// Logs an informational line under the loader's tag.
pub fn log_info(message: &str) {
    #[cfg(target_os = "android")]
    log::info!("{message}");
    #[cfg(not(target_os = "android"))]
    eprintln!("[{LOG_TAG}] {message}");
}

/// Logs an error line under the loader's tag.
pub fn log_error(message: &str) {
    #[cfg(target_os = "android")]
    log::error!("{message}");
    #[cfg(not(target_os = "android"))]
    eprintln!("[{LOG_TAG}] error: {message}");
}

/// Reads a Java string, returning an empty string for null or unreadable input.
fn read_string(env: &mut JNIEnv, value: &JString) -> String {
    if value.is_null() {
        return String::new();
    }
    env.get_string(value)
        .map(String::from)
        .unwrap_or_default()
}

/// Builds a Java string from a message; an empty message means success.
fn new_message(env: &mut JNIEnv, message: &str) -> jstring {
    match env.new_string(message) {
        Ok(value) => value.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

/// Runs `body`, catching a panic and turning it into an error message.
fn guard<F>(what: &str, body: F) -> Result<(), String>
where
    F: FnOnce() -> Result<(), String>,
{
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(result) => result,
        Err(payload) => {
            let message = payload
                .downcast_ref::<&str>()
                .map(|s| (*s).to_string())
                .or_else(|| payload.downcast_ref::<String>().cloned())
                .unwrap_or_else(|| "unknown panic".to_string());
            let line = format!("{what} panicked: {message}");
            log_error(&line);
            Err(line)
        }
    }
}

/// Wraps a fallible operation so it always returns a message string (empty on success).
fn run_or_message<F>(env: &mut JNIEnv, what: &str, body: F) -> jstring
where
    F: FnOnce() -> Result<(), String>,
{
    let message = match guard(what, body) {
        Ok(()) => String::new(),
        Err(error) => error,
    };
    new_message(env, &message)
}

/// # Safety
/// `env` and `_class` are supplied by the JVM. `pointer` is an opaque pointer handed in by the
/// caller; it is stored, never dereferenced by this function.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_universe_UniverseLoader_nativeInit(
    mut env: JNIEnv,
    _class: JClass,
    pointer: *mut c_void,
) -> jstring {
    install_runtime();
    run_or_message(&mut env, "nativeInit", || {
        if !pointer.is_null() {
            // The pointer is the BerryAPI API table the C++ side already resolved. It is stored
            // for the mod-facing bridge; a null pointer means BerryAPI is absent, which is not an
            // error (the loader runs, the BerryAPI-backed features report unavailable).
            let _ = BERRY_API.set(pointer as usize);
            log_info("BerryAPI pointer received");
        }
        set_cancelled(false);
        log_info(&format!(
            "initialized (client version {CLIENT_VERSION}, abi {})",
            universe_core::SUPPORTED_ABI
        ));
        Ok(())
    })
}

/// # Safety
/// JVM-supplied arguments; the strings are read but not retained.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_universe_UniverseLoader_nativeValidate(
    mut env: JNIEnv,
    _class: JClass,
    file_path: JString,
) -> jstring {
    install_runtime();
    let path = read_string(&mut env, &file_path);
    run_or_message(&mut env, "nativeValidate", || {
        let manifest = universe_core::archive::read_manifest(&path).map_err(|e| e.to_string())?;
        universe_core::version::check_min_client_version(
            &manifest.min_client_version,
            CLIENT_VERSION,
        )
        .map_err(|e| e.to_string())?;
        log_info(&format!(
            "validated {} v{}",
            manifest.display_name(),
            manifest.version
        ));
        Ok(())
    })
}

/// # Safety
/// JVM-supplied arguments; the strings are read but not retained.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_universe_UniverseLoader_nativeExtract(
    mut env: JNIEnv,
    _class: JClass,
    file_path: JString,
    sandbox_root: JString,
    allowed_root: JString,
) -> jstring {
    install_runtime();
    let path = read_string(&mut env, &file_path);
    let sandbox = read_string(&mut env, &sandbox_root);
    let allowed = read_string(&mut env, &allowed_root);
    run_or_message(&mut env, "nativeExtract", || {
        universe_core::archive::validate_sandbox_root(&sandbox, &allowed)
            .map_err(|e| e.to_string())?;
        let directory =
            universe_core::archive::extract(&path, &sandbox).map_err(|e| e.to_string())?;
        log_info(&format!("extracted into {}", directory.display()));
        Ok(())
    })
}

/// # Safety
/// JVM-supplied argument; the string is read but not retained.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_universe_UniverseLoader_nativeLooksLikeUniverse(
    mut env: JNIEnv,
    _class: JClass,
    file_name: JString,
) -> jboolean {
    let name = read_string(&mut env, &file_name);
    universe_core::manifest::looks_like_universe(&name) as jboolean
}

/// # Safety
/// JVM-supplied argument; the string is read but not retained.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_universe_UniverseLoader_nativeSetCancelled(
    _env: JNIEnv,
    _class: JClass,
    cancelled: jboolean,
) {
    set_cancelled(cancelled != 0);
}

/// # Safety
/// JVM-supplied argument; the string is read but not retained.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_universe_UniverseLoader_nativeReadManifest(
    mut env: JNIEnv,
    _class: JClass,
    file_path: JString,
) -> jstring {
    install_runtime();
    let path = read_string(&mut env, &file_path);
    match universe_core::archive::read_manifest(&path) {
        Ok(manifest) => new_message(&mut env, &universe_core::manifest::to_json(&manifest)),
        Err(error) => new_message(&mut env, &error.to_string()),
    }
}

/// # Safety
/// `pointer` must be a valid, NUL-terminated C string or null.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_universe_UniverseLoader_nativeCheckVersion(
    mut env: JNIEnv,
    _class: JClass,
    min_client_version: JString,
) -> jstring {
    let required = read_string(&mut env, &min_client_version);
    run_or_message(&mut env, "nativeCheckVersion", || {
        universe_core::version::check_min_client_version(&required, CLIENT_VERSION)
            .map_err(|e| e.to_string())
    })
}

/// # Safety
/// `env` and `_class` are supplied by the JVM.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_universe_UniverseLoader_nativeClientVersion(
    mut env: JNIEnv,
    _class: JClass,
) -> jstring {
    new_message(&mut env, CLIENT_VERSION)
}

/// A C-string helper used by the small C ABI below.
///
/// # Safety
/// `pointer` must be null or a valid NUL-terminated string.
pub unsafe fn cstr(pointer: *const c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }
    CStr::from_ptr(pointer).to_string_lossy().into_owned()
}

/// The C ABI the C++ preloader calls, so it does not need a JVM to reach the loader's logic.
///
/// These mirror the JNI functions but take `const char*` and return a heap-allocated message that
/// the caller frees with [`caramel_universe_free`]. A null return means success.
///
/// # Safety
/// Every `*const c_char` must be null or a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn caramel_universe_init(berry_api: *mut c_void) -> *mut c_char {
    install_runtime();
    if !berry_api.is_null() {
        let _ = BERRY_API.set(berry_api as usize);
    }
    set_cancelled(false);
    log_info(&format!("initialized via C ABI (client version {CLIENT_VERSION})"));
    std::ptr::null_mut()
}

/// Validates a package's manifest and version gate. See [`caramel_universe_init`].
///
/// # Safety
/// `archive_path` must be null or a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn caramel_universe_validate(archive_path: *const c_char) -> *mut c_char {
    let path = cstr(archive_path);
    match universe_core::archive::read_manifest(&path) {
        Ok(manifest) => match universe_core::version::check_min_client_version(
            &manifest.min_client_version,
            CLIENT_VERSION,
        ) {
            Ok(()) => std::ptr::null_mut(),
            Err(error) => message_to_c(error.to_string()),
        },
        Err(error) => message_to_c(error.to_string()),
    }
}

/// Extracts a package. See [`caramel_universe_init`].
///
/// # Safety
/// The arguments must be null or valid NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn caramel_universe_extract(
    archive_path: *const c_char,
    sandbox_root: *const c_char,
    allowed_root: *const c_char,
) -> *mut c_char {
    let path = cstr(archive_path);
    let sandbox = cstr(sandbox_root);
    let allowed = cstr(allowed_root);
    if let Err(error) = universe_core::archive::validate_sandbox_root(&sandbox, &allowed) {
        return message_to_c(error.to_string());
    }
    match universe_core::archive::extract(&path, &sandbox) {
        Ok(_) => std::ptr::null_mut(),
        Err(error) => message_to_c(error.to_string()),
    }
}

/// Frees a message returned by the C ABI above.
///
/// # Safety
/// `pointer` must be a pointer previously returned by this library, or null.
#[no_mangle]
pub unsafe extern "C" fn caramel_universe_free(pointer: *mut c_char) {
    if pointer.is_null() {
        return;
    }
    drop(CString::from_raw(pointer));
}

/// True when the loader has been asked to stop.
#[no_mangle]
pub extern "C" fn caramel_universe_is_cancelled() -> bool {
    is_cancelled()
}

/// Sets the loader's cancellation flag (unload, safe mode, shutdown).
#[no_mangle]
pub extern "C" fn caramel_universe_set_cancelled(cancelled: bool) {
    set_cancelled(cancelled);
}

/// The loader's client version, as a static NUL-terminated string.
///
/// Static storage, so the caller must not free it.
#[no_mangle]
pub extern "C" fn caramel_universe_client_version() -> *const c_char {
    static VERSION: &str = concat!(env!("CARGO_PKG_VERSION"), "\0");
    VERSION.as_ptr() as *const c_char
}

/// Reads a package's bytes, refusing one over the archive size limit.
///
/// The signature is over the whole file, so verification must read it exactly as the packer wrote
/// it — no re-zipping, no normalisation.
fn read_package_bytes(path: &str) -> Result<Vec<u8>, String> {
    let metadata = std::fs::metadata(path)
        .map_err(|e| format!("could not stat the package: {e}"))?;
    if metadata.len() > universe_core::archive::MAX_ARCHIVE_BYTES {
        return Err("the package is larger than the size limit".to_string());
    }
    std::fs::read(path).map_err(|e| format!("could not read the package: {e}"))
}

/// Parses a comma-separated list, trimming blanks.
fn split_csv(text: &str) -> Vec<String> {
    text.split(',')
        .map(str::trim)
        .filter(|item| !item.is_empty())
        .map(str::to_string)
        .collect()
}

/// Verifies a package's detached Ed25519 signature against a set of trusted public keys.
///
/// Returns null when the signature verifies against one of `trusted_keys_hex` (a comma-separated
/// list). A non-null message means the package is refused: a bad signature, a malformed key, or an
/// *empty* trusted set, which is fail-closed — an unknown key must never be treated as trusted.
///
/// # Safety
/// Every `*const c_char` must be null or a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn caramel_universe_verify_signature(
    archive_path: *const c_char,
    signature_hex: *const c_char,
    trusted_keys_hex: *const c_char,
) -> *mut c_char {
    let path = cstr(archive_path);
    let signature_text = cstr(signature_hex);
    let keys_text = cstr(trusted_keys_hex);
    match verify_signature(&path, &signature_text, &keys_text) {
        Ok(()) => std::ptr::null_mut(),
        Err(error) => message_to_c(error),
    }
}

fn verify_signature(path: &str, signature_text: &str, keys_text: &str) -> Result<(), String> {
    use universe_core::signature::{PublicKey, RawSignature, TrustedKeys};

    let mut trusted = TrustedKeys::new();
    for (index, key_hex) in split_csv(keys_text).iter().enumerate() {
        let key = PublicKey::from_hex(key_hex)
            .map_err(|e| format!("trusted key {index} is not a valid public key: {e}"))?;
        trusted.add(format!("key{index}"), key);
    }
    if trusted.is_empty() {
        // Fail-closed: with no trusted key there is nothing that could vouch for the package.
        return Err("no trusted keys are configured; refusing to accept a signature".to_string());
    }
    let signature = RawSignature::from_hex(signature_text)
        .map_err(|e| format!("the signature is not valid hex: {e}"))?;
    let body = read_package_bytes(path)?;
    if trusted.verifies(&body, &signature) {
        Ok(())
    } else {
        Err("the signature does not verify against any trusted key".to_string())
    }
}

/// Checks that a mod's declared permissions cover the capabilities it requests.
///
/// Returns null when every requested capability is granted. A non-null message names the first
/// denied capability. `trusted` is whether the package carried a trusted signature; a trust-gated
/// capability (`write_memory`) is denied without it.
///
/// # Safety
/// Every `*const c_char` must be null or a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn caramel_universe_check_permissions(
    declared_csv: *const c_char,
    requested_csv: *const c_char,
    trusted: bool,
) -> *mut c_char {
    match check_permissions(&cstr(declared_csv), &cstr(requested_csv), trusted) {
        Ok(()) => std::ptr::null_mut(),
        Err(error) => message_to_c(error),
    }
}

fn check_permissions(declared_csv: &str, requested_csv: &str, trusted: bool) -> Result<(), String> {
    use universe_core::permission::{self, Permission};

    let parse_all = |text: &str| -> Result<Vec<Permission>, String> {
        split_csv(text)
            .iter()
            .map(|name| Permission::parse(name).map_err(|e| e.to_string()))
            .collect()
    };
    let declared = parse_all(declared_csv)?;
    let requested = parse_all(requested_csv)?;
    match permission::first_denied(&declared, &requested, trusted) {
        None => Ok(()),
        Some(denied) => Err(format!(
            "permission '{}' is not granted to this package",
            denied.as_str()
        )),
    }
}

/// Resolves the load order for a set of mods from a JSON array of
/// `{"id": "...", "dependencies": ["..."]}` objects.
///
/// Returns a JSON object `{"order": [...], "cycle": [...]}` — always non-null, so the caller parses
/// one shape. `order` is the deterministic load sequence; a non-empty `cycle` names the mods that
/// form a dependency cycle, which the caller refuses. A malformed input returns `{"error": "..."}`.
///
/// # Safety
/// `nodes_json` must be null or a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn caramel_universe_resolve_load_order(nodes_json: *const c_char) -> *mut c_char {
    let json = cstr(nodes_json);
    message_to_c(universe_core::report::resolve_load_order_json(&json))
}

fn message_to_c(message: String) -> *mut c_char {
    match CString::new(message) {
        Ok(value) => value.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cancellation_flag_round_trips() {
        set_cancelled(false);
        assert!(!is_cancelled());
        set_cancelled(true);
        assert!(is_cancelled());
        set_cancelled(false);
        assert!(!is_cancelled());
    }

    #[test]
    fn guard_turns_a_panic_into_an_error() {
        let result = guard("test", || panic!("boom"));
        assert!(result.is_err());
        assert!(result.unwrap_err().contains("boom"));
    }

    #[test]
    fn guard_passes_through_a_clean_result() {
        assert!(guard("test", || Ok(())).is_ok());
        assert!(guard("test", || Err("nope".to_string())).is_err());
    }

    #[test]
    fn client_version_is_well_formed() {
        assert!(universe_core::version::is_valid(CLIENT_VERSION));
    }

    #[test]
    fn the_c_abi_reports_a_missing_package() {
        let missing = CString::new("/nonexistent/package.universe").unwrap();
        let message = unsafe { caramel_universe_validate(missing.as_ptr()) };
        assert!(!message.is_null());
        unsafe { caramel_universe_free(message) };
    }

    #[test]
    fn cstr_handles_null_and_text() {
        assert_eq!(unsafe { cstr(std::ptr::null()) }, "");
        let text = CString::new("hello").unwrap();
        assert_eq!(unsafe { cstr(text.as_ptr()) }, "hello");
    }

    #[test]
    fn permission_check_is_fail_closed() {
        let declared = CString::new("read_memory,render_hook").unwrap();
        let requested = CString::new("read_memory").unwrap();
        // A declared, untrusted capability is granted.
        assert!(unsafe { caramel_universe_check_permissions(declared.as_ptr(), requested.as_ptr(), false) }
            .is_null());
        // A trust-gated capability is denied without a trusted signature.
        let write = CString::new("write_memory").unwrap();
        let denied = unsafe {
            caramel_universe_check_permissions(declared.as_ptr(), write.as_ptr(), false)
        };
        assert!(!denied.is_null());
        unsafe { caramel_universe_free(denied) };
        // A capability the mod never declared is denied.
        let network = CString::new("network").unwrap();
        let denied = unsafe {
            caramel_universe_check_permissions(declared.as_ptr(), network.as_ptr(), true)
        };
        assert!(!denied.is_null());
        unsafe { caramel_universe_free(denied) };
    }

    #[test]
    fn signature_verification_refuses_without_a_trusted_key() {
        // An empty trusted set must refuse rather than accept an unknown signer.
        let package = CString::new("/nonexistent/package.universe").unwrap();
        let signature = CString::new("00").unwrap();
        let keys = CString::new("").unwrap();
        let message = unsafe {
            caramel_universe_verify_signature(package.as_ptr(), signature.as_ptr(), keys.as_ptr())
        };
        assert!(!message.is_null());
        unsafe { caramel_universe_free(message) };
    }

    #[test]
    fn load_order_returns_a_json_object() {
        let nodes = CString::new(r#"[{"id":"app","dependencies":["lib"]},{"id":"lib"}]"#).unwrap();
        let message = unsafe { caramel_universe_resolve_load_order(nodes.as_ptr()) };
        assert!(!message.is_null());
        let json = unsafe { CStr::from_ptr(message) }.to_str().unwrap().to_string();
        unsafe { caramel_universe_free(message) };
        assert!(json.contains("\"order\""));
        assert!(json.contains("\"cycle\""));
        let lib = json.find("lib").unwrap();
        let app = json.find("app").unwrap();
        assert!(lib < app, "lib must precede app: {json}");
    }

    #[test]
    fn load_order_reports_a_cycle() {
        let nodes = CString::new(r#"[{"id":"a","dependencies":["b"]},{"id":"b","dependencies":["a"]}]"#).unwrap();
        let message = unsafe { caramel_universe_resolve_load_order(nodes.as_ptr()) };
        let json = unsafe { CStr::from_ptr(message) }.to_str().unwrap().to_string();
        unsafe { caramel_universe_free(message) };
        assert!(json.contains("\"cycle\":[\"a\",\"b\"]") || json.contains("\"cycle\":[\"b\",\"a\"]"), "{json}");
    }

    #[test]
    fn load_order_reports_bad_json_as_an_error_object() {
        let nodes = CString::new("not json").unwrap();
        let message = unsafe { caramel_universe_resolve_load_order(nodes.as_ptr()) };
        let json = unsafe { CStr::from_ptr(message) }.to_str().unwrap().to_string();
        unsafe { caramel_universe_free(message) };
        assert!(json.contains("\"error\""), "{json}");
    }

    #[test]
    fn escape_json_quotes_and_controls() {
        // Escaping lives in universe-core::report; this pins the delegation stays wired.
        assert_eq!(universe_core::report::escape_json("a\"b"), "a\\\"b");
    }
}
