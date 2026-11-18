//! `libcaramel_singularity.so` — the JNI cdylib behind the `.singularity` world-gen engine.
//!
//! Same shape as `universe-jni`: the decisions live in `singularity-core` and this crate adds the
//! JNI signatures, the Android logger and the panic hook. The engine talks to the game through
//! BerryAPI (the chunk hook and the render context); that side is owned by the C++ bridge, which
//! calls the `caramel_singularity_*` C ABI here.

use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};

use jni::objects::{JClass, JString};
use jni::sys::{jboolean, jstring};
use jni::JNIEnv;

/// The log tag every message from this library carries.
pub const LOG_TAG: &str = "SweetLoader.Singularity";

/// The client version this build ships with, used only when the launcher has not supplied the
/// running client's own version. See `universe-jni::CLIENT_VERSION`.
pub const CLIENT_VERSION: &str = env!("CARGO_PKG_VERSION");

/// The running client's version, set once by the launcher at init; the crate version is the
/// fallback. Mirrors the `universe-jni` mechanism so both loaders gate on the same real version.
static CLIENT_VERSION_OVERRIDE: std::sync::RwLock<Option<String>> = std::sync::RwLock::new(None);

/// Records the running client's version, as the launcher knows it (`BETA6`, `OV.2`, ...).
pub fn set_client_version(version: &str) {
    let trimmed = version.trim();
    if trimmed.is_empty() {
        return;
    }
    if let Ok(mut guard) = CLIENT_VERSION_OVERRIDE.write() {
        *guard = Some(trimmed.to_string());
    }
}

/// The version a package's `min_client_version` is checked against.
pub fn running_client_version() -> String {
    CLIENT_VERSION_OVERRIDE
        .read()
        .ok()
        .and_then(|guard| guard.clone())
        .unwrap_or_else(|| CLIENT_VERSION.to_string())
}

/// Set when the engine wants world generation to stop (world unload, hot reload).
static CANCELLED: AtomicBool = AtomicBool::new(false);

/// The BerryAPI API table, set once by `nativeInit`.
static BERRY_API: std::sync::OnceLock<usize> = std::sync::OnceLock::new();

/// The biome registry a world's chunks are generated against.
///
/// A pack's `biomes/*.json` files are registered here once, then read on every chunk. A `RwLock`
/// rather than a `Mutex` because generation reads from many worker threads while registration is a
/// single write at world load, so the common path (generate) never blocks another generator.
static BIOMES: std::sync::OnceLock<std::sync::RwLock<singularity_core::biome::BiomeRegistry>> =
    std::sync::OnceLock::new();

fn biomes() -> &'static std::sync::RwLock<singularity_core::biome::BiomeRegistry> {
    BIOMES.get_or_init(|| {
        std::sync::RwLock::new(singularity_core::biome::BiomeRegistry::new("plains"))
    })
}

pub fn is_cancelled() -> bool {
    CANCELLED.load(Ordering::Acquire)
}

pub fn set_cancelled(cancelled: bool) {
    CANCELLED.store(cancelled, Ordering::Release);
}

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
            log_error(&format!("panic: {info}"));
            previous(info);
        }));
    });
}

pub fn log_info(message: &str) {
    #[cfg(target_os = "android")]
    log::info!("{message}");
    #[cfg(not(target_os = "android"))]
    eprintln!("[{LOG_TAG}] {message}");
}

pub fn log_error(message: &str) {
    #[cfg(target_os = "android")]
    log::error!("{message}");
    #[cfg(not(target_os = "android"))]
    eprintln!("[{LOG_TAG}] error: {message}");
}

fn read_string(env: &mut JNIEnv, value: &JString) -> String {
    if value.is_null() {
        return String::new();
    }
    env.get_string(value).map(String::from).unwrap_or_default()
}

fn new_message(env: &mut JNIEnv, message: &str) -> jstring {
    match env.new_string(message) {
        Ok(value) => value.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

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
/// `pointer` is stored, never dereferenced.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_singularity_SingularityEngine_nativeInit(
    mut env: JNIEnv,
    _class: JClass,
    pointer: *mut c_void,
    client_version: JString,
) -> jstring {
    install_runtime();
    let version = read_string(&mut env, &client_version);
    run_or_message(&mut env, "nativeInit", || {
        if !pointer.is_null() {
            let _ = BERRY_API.set(pointer as usize);
            log_info("BerryAPI pointer received");
        }
        set_client_version(&version);
        set_cancelled(false);
        log_info(&format!(
            "initialized (client version {})",
            running_client_version()
        ));
        Ok(())
    })
}

/// # Safety
/// JVM-supplied argument; the string is read but not retained.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_singularity_SingularityEngine_nativeValidate(
    mut env: JNIEnv,
    _class: JClass,
    file_path: JString,
) -> jstring {
    install_runtime();
    let path = read_string(&mut env, &file_path);
    run_or_message(&mut env, "nativeValidate", || {
        let text = universe_core::archive::read_manifest_text(&path).map_err(|e| e.to_string())?;
        let manifest = singularity_core::manifest::parse(&text).map_err(|e| e.to_string())?;
        universe_core::version::check_min_client_version(
            &manifest.min_client_version,
            &running_client_version(),
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
pub unsafe extern "system" fn Java_org_chimeramc_client_singularity_SingularityEngine_nativeExtract(
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
        universe_core::archive::extract(&path, &sandbox).map_err(|e| e.to_string())?;
        Ok(())
    })
}

/// # Safety
/// JVM-supplied argument; the string is read but not retained.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_singularity_SingularityEngine_nativeLooksLikeSingularity(
    mut env: JNIEnv,
    _class: JClass,
    file_name: JString,
) -> jboolean {
    let name = read_string(&mut env, &file_name);
    singularity_core::manifest::looks_like_singularity(&name) as jboolean
}

/// # Safety
/// JVM-supplied argument.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_singularity_SingularityEngine_nativeSetCancelled(
    _env: JNIEnv,
    _class: JClass,
    cancelled: jboolean,
) {
    set_cancelled(cancelled != 0);
}

/// # Safety
/// JVM-supplied argument; the string is read but not retained.
#[no_mangle]
pub unsafe extern "system" fn Java_org_chimeramc_client_singularity_SingularityEngine_nativeParseManifest(
    mut env: JNIEnv,
    _class: JClass,
    manifest_json: JString,
) -> jstring {
    install_runtime();
    let json = read_string(&mut env, &manifest_json);
    match singularity_core::manifest::parse(&json) {
        Ok(manifest) => new_message(&mut env, &singularity_core::manifest::to_json(&manifest)),
        Err(error) => new_message(&mut env, &error.to_string()),
    }
}

/// # Safety
/// `pointer` must be null or a NUL-terminated string.
pub unsafe fn cstr(pointer: *const c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }
    CStr::from_ptr(pointer).to_string_lossy().into_owned()
}

fn message_to_c(message: String) -> *mut c_char {
    match CString::new(message) {
        Ok(value) => value.into_raw(),
        Err(_) => std::ptr::null_mut(),
    }
}

/// The C ABI the C++ bridge calls. A null return means success; free a message with
/// [`caramel_singularity_free`].
///
/// # Safety
/// `berry_api` must be null or an opaque pointer the caller owns. `client_version` must be null or
/// a valid NUL-terminated string naming the running client's release (`BETA6`).
#[no_mangle]
pub unsafe extern "C" fn caramel_singularity_init(
    berry_api: *mut c_void,
    client_version: *const c_char,
) -> *mut c_char {
    install_runtime();
    if !berry_api.is_null() {
        let _ = BERRY_API.set(berry_api as usize);
    }
    set_client_version(&cstr(client_version));
    set_cancelled(false);
    log_info("initialized via C ABI");
    std::ptr::null_mut()
}

/// Validates a `.singularity` manifest's JSON. See [`caramel_singularity_init`].
///
/// # Safety
/// `manifest_json` must be null or a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn caramel_singularity_validate_manifest(
    manifest_json: *const c_char,
) -> *mut c_char {
    let json = cstr(manifest_json);
    match singularity_core::manifest::parse(&json) {
        Ok(manifest) => {
            match universe_core::version::check_min_client_version(
                &manifest.min_client_version,
                &running_client_version(),
            ) {
                Ok(()) => std::ptr::null_mut(),
                Err(error) => message_to_c(error.to_string()),
            }
        }
        Err(error) => message_to_c(error.to_string()),
    }
}

/// Generates a chunk's block array, returning the number of blocks written into `out_blocks`.
///
/// `out_capacity` is the number of bytes the caller has actually reserved at `out_blocks`; the
/// engine writes only when `16 * height * 16` fits inside it, so a caller whose `height` disagrees
/// with its own buffer is refused (`-1`) rather than overflowing. Passing the wrong capacity is the
/// caller's bug, but it must be a refused call, not a heap corruption in the game process.
///
/// `default_biome` is the pack's own fallback biome id, or null/empty for `"plains"`.
///
/// # Safety
/// `out_blocks` must point to at least `out_capacity` writable bytes; `default_biome` must be null
/// or a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn caramel_singularity_generate_chunk(
    seed: u64,
    base_height: i32,
    amplitude: i32,
    sea_level: i32,
    height: i32,
    octaves: u32,
    warp: f32,
    chunk_x: i32,
    chunk_z: i32,
    out_blocks: *mut u8,
    out_capacity: usize,
    default_biome: *const c_char,
) -> i32 {
    if out_blocks.is_null() || height < 1 || height > 2048 {
        return -1;
    }
    let needed = (16 * 16 * height) as usize;
    // Refuse before generating: a mismatched capacity must not reach the copy below.
    if out_capacity < needed {
        log_error(&format!(
            "chunk buffer too small: {out_capacity} bytes for {needed}"
        ));
        return -1;
    }
    let biome_id = cstr(default_biome);
    let default_biome = if biome_id.trim().is_empty() {
        "plains".to_string()
    } else {
        biome_id
    };
    let config = singularity_core::chunk::TerrainConfig {
        seed,
        base_height,
        amplitude,
        sea_level,
        height,
        octaves,
        warp,
        default_biome,
    };
    // Read the registered biomes, so a pack's biomes/*.json actually shape the terrain. The read
    // lock is held only for the generation call; registration takes the write lock once at load.
    let registry = biomes()
        .read()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let chunk = singularity_core::chunk::generate_chunk(
        &config,
        &registry,
        singularity_core::chunk::ChunkPos {
            x: chunk_x,
            z: chunk_z,
        },
    );
    let expected = chunk.blocks.len();
    if expected != needed {
        return -1;
    }
    std::ptr::copy_nonoverlapping(chunk.blocks.as_ptr(), out_blocks, expected);
    expected as i32
}

/// Registers a pack's biomes from a JSON array of biome definition objects.
///
/// Each element is the same shape as a `biomes/*.json` file (`id`, `temperature`, `humidity`,
/// `continentalness`, `weight`). Registration **merges** into the current set: a definition whose id
/// is already registered replaces that entry, a new id is added, so two packs that both define
/// `plains` do not clobber each other's whole biome list. Returns the number of biomes registered,
/// or -1 when a definition is malformed (in which case the previous set is left untouched — the
/// caller gets all-or-nothing).
///
/// # Safety
/// `biomes_json` must be null or a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn caramel_singularity_register_biomes(biomes_json: *const c_char) -> i32 {
    let json = cstr(biomes_json);
    match singularity_core::biome::parse_definitions(&json) {
        Ok(definitions) => {
            let mut registry = biomes()
                .write()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            for definition in &definitions {
                registry.register_or_replace(definition.clone());
            }
            definitions.len() as i32
        }
        Err(error) => {
            log_error(&format!("could not register biomes: {error}"));
            -1
        }
    }
}

/// The seed hash for a world seed string, using the requested algorithm.
///
/// # Safety
/// `seed_text` must be null or a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn caramel_singularity_seed_hash(
    seed_text: *const c_char,
    use_sha256: bool,
) -> u64 {
    let text = cstr(seed_text);
    singularity_core::seed_hash(&text, use_sha256)
}

/// Frees a message returned by the C ABI above.
///
/// # Safety
/// `pointer` must be a pointer previously returned by this library, or null.
#[no_mangle]
pub unsafe extern "C" fn caramel_singularity_free(pointer: *mut c_char) {
    if pointer.is_null() {
        return;
    }
    drop(CString::from_raw(pointer));
}

/// True when the engine has been asked to stop.
#[no_mangle]
pub extern "C" fn caramel_singularity_is_cancelled() -> bool {
    is_cancelled()
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
    }

    #[test]
    fn guard_turns_a_panic_into_an_error() {
        assert!(guard("test", || panic!("boom")).is_err());
    }

    #[test]
    fn the_c_abi_refuses_a_bad_manifest() {
        let json = CString::new("{ not valid").unwrap();
        let message = unsafe { caramel_singularity_validate_manifest(json.as_ptr()) };
        assert!(!message.is_null());
        unsafe { caramel_singularity_free(message) };
    }

    #[test]
    fn chunk_generation_writes_the_expected_size() {
        let mut blocks = vec![0u8; 16 * 16 * 64];
        let capacity = blocks.len();
        let written = unsafe {
            caramel_singularity_generate_chunk(
                42,
                32,
                8,
                30,
                64,
                4,
                0.0,
                0,
                0,
                blocks.as_mut_ptr(),
                capacity,
                std::ptr::null(),
            )
        };
        assert_eq!(written, (16 * 16 * 64) as i32);
    }

    #[test]
    fn chunk_generation_refuses_a_null_pointer() {
        let written = unsafe {
            caramel_singularity_generate_chunk(
                1,
                32,
                8,
                30,
                64,
                4,
                0.0,
                0,
                0,
                std::ptr::null_mut(),
                16 * 16 * 64,
                std::ptr::null(),
            )
        };
        assert_eq!(written, -1);
    }

    #[test]
    fn chunk_generation_refuses_a_buffer_smaller_than_the_chunk() {
        // The buffer must be checked against the capacity the caller actually reserved, not the
        // height it claims, so a mismatched caller is refused rather than overflowing.
        let mut blocks = vec![0u8; 16 * 16 * 63];
        let capacity = blocks.len();
        let written = unsafe {
            caramel_singularity_generate_chunk(
                1,
                32,
                8,
                30,
                64,
                4,
                0.0,
                0,
                0,
                blocks.as_mut_ptr(),
                capacity,
                std::ptr::null(),
            )
        };
        assert_eq!(written, -1, "a short buffer must be refused");
    }

    #[test]
    fn registering_biomes_merges_and_refuses_a_bad_list() {
        // One test, not two: the registry is process-global, so two tests mutating it would race
        // under the parallel test runner.
        let two = CString::new(r#"[{"id":"a"},{"id":"b"}]"#).unwrap();
        assert_eq!(
            unsafe { caramel_singularity_register_biomes(two.as_ptr()) },
            2
        );
        // A second call adds a new id and keeps the existing ones, rather than clobbering the set.
        let one = CString::new(r#"[{"id":"c"}]"#).unwrap();
        assert_eq!(
            unsafe { caramel_singularity_register_biomes(one.as_ptr()) },
            1
        );
        // A malformed list is refused and leaves the previous set intact.
        let bad = CString::new(r#"[{"id":"../evil"}]"#).unwrap();
        assert_eq!(
            unsafe { caramel_singularity_register_biomes(bad.as_ptr()) },
            -1
        );

        let registry = biomes().read().unwrap();
        assert_eq!(registry.len(), 3, "a, b and c all survive");
        assert!(
            registry
                .select(singularity_core::biome::Climate {
                    temperature: 0.5,
                    humidity: 0.5,
                    continentalness: 0.5,
                })
                .len()
                > 0
        );
    }
}
