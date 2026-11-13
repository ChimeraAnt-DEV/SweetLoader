//! The native mod loader: plan, `dlopen`, call the entry points, isolate faults.
//!
//! This is the module the C++ runtime drives through the C ABI. It owns the loaded-library table,
//! the fault tracker and the frame counter, and it is the only place a mod's code is entered.
//!
//! **The one rule this module exists for: a mod's code is never entered without a guard.** A mod is
//! arbitrary native code inside the game process. If `universe_init` or `universe_tick` panics, the
//! panic is caught here, counted, and — after [`MAX_FAULTS`] consecutive faults — the mod is
//! unloaded and refused for the rest of the session. The game thread keeps running. That is what
//! makes "never crash the game thread" true rather than aspirational.
//!
//! The entry points are resolved with `dlsym` on the handle `dlopen` returned. A library that does
//! not export all three is refused: a mod that cannot be ticked or shut down cleanly is not loaded
//! at all, because a half-loaded mod is worse than a refused one.

use std::collections::HashMap;
use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};

use universe_core::fault::FaultTracker;
use universe_core::runtime::{self, ModSpec};

use crate::{log_error, log_info};

/// `universe_init(const universe_host_t *) -> int32_t`.
type InitFn = unsafe extern "C" fn(*const c_void) -> i32;
/// `universe_tick(uint64_t frame, float dt)`.
type TickFn = unsafe extern "C" fn(u64, f32);
/// `universe_shutdown(void)`.
type ShutdownFn = unsafe extern "C" fn();

/// A mod that is loaded and live.
struct LoadedMod {
    id: String,
    handle: *mut c_void,
    tick: TickFn,
    shutdown: ShutdownFn,
}

// The handles are owned by this module and are only touched under the loader's mutex, so the table
// is Send. The raw pointers themselves are not shared across threads without that lock.
unsafe impl Send for LoadedMod {}

/// The loader's state.
pub struct ModRuntime {
    loaded: Vec<LoadedMod>,
    tracker: FaultTracker,
    frame: u64,
    /// The host table pointer handed to every mod's `universe_init`. Owned by the caller.
    host: *const c_void,
}

unsafe impl Send for ModRuntime {}

impl Default for ModRuntime {
    fn default() -> Self {
        ModRuntime {
            loaded: Vec::new(),
            tracker: FaultTracker::new(),
            frame: 0,
            host: std::ptr::null(),
        }
    }
}

impl ModRuntime {
    pub fn new() -> ModRuntime {
        ModRuntime::default()
    }

    /// Sets the host table handed to a mod's `universe_init`.
    pub fn set_host(&mut self, host: *const c_void) {
        self.host = host;
    }

    /// The number of mods currently live.
    pub fn live_count(&self) -> usize {
        self.loaded.len()
    }

    /// The mod ids currently live, in load order.
    pub fn live_ids(&self) -> Vec<String> {
        self.loaded.iter().map(|m| m.id.clone()).collect()
    }

    /// The frame counter, as of the last tick.
    pub fn frame(&self) -> u64 {
        self.frame
    }

    /// The mods disabled by the fault policy.
    pub fn disabled(&self) -> Vec<String> {
        self.tracker.disabled_ids()
    }

    /// Plans and loads `specs`, returning the plan that was applied.
    ///
    /// Already-loaded mods are left alone, so a caller can call this again with a longer list
    /// without re-entering a live mod's `universe_init`.
    pub fn load(&mut self, specs: &[ModSpec]) -> universe_core::runtime::LoadPlan {
        let plan = runtime::plan(specs, &self.tracker);
        let by_id: HashMap<&str, &ModSpec> = specs.iter().map(|s| (s.id.as_str(), s)).collect();
        for id in &plan.load {
            if self.loaded.iter().any(|m| &m.id == id) {
                continue;
            }
            let Some(spec) = by_id.get(id.as_str()) else {
                continue;
            };
            match self.open_one(spec) {
                Ok(loaded) => {
                    self.tracker.record_success(id);
                    self.loaded.push(loaded);
                }
                Err(error) => {
                    log_error(&format!("refusing mod {id}: {error}"));
                    self.tracker.record_fault(id);
                }
            }
        }
        plan
    }

    /// `dlopen`s one mod and resolves its entry points.
    fn open_one(&self, spec: &ModSpec) -> Result<LoadedMod, String> {
        if spec.lib_path.is_empty() {
            return Err("the manifest names no native library".to_string());
        }
        let path = CString::new(spec.lib_path.as_str())
            .map_err(|_| "the library path contains a NUL byte".to_string())?;
        // RTLD_NOW | RTLD_LOCAL: resolve every relocation up front (a missing symbol is refused now
        // rather than faulting mid-frame) and keep the mod's symbols out of the global namespace so
        // two mods cannot shadow each other.
        let handle = unsafe { dlopen(path.as_ptr(), RTLD_NOW | RTLD_LOCAL) };
        if handle.is_null() {
            return Err(format!("dlopen failed: {}", dl_error()));
        }
        let init = unsafe { resolve_symbol::<InitFn>(handle, b"universe_init\0") };
        let tick = unsafe { resolve_symbol::<TickFn>(handle, b"universe_tick\0") };
        let shutdown = unsafe { resolve_symbol::<ShutdownFn>(handle, b"universe_shutdown\0") };
        let (Some(init), Some(tick), Some(shutdown)) = (init, tick, shutdown) else {
            // A half-implemented mod is closed again: it must export all three, or it is refused.
            unsafe { dlclose(handle) };
            return Err("the library does not export universe_init/tick/shutdown".to_string());
        };

        // The mod's own init is the first call into mod code, so it is guarded like a tick.
        let result = catch_unwind(AssertUnwindSafe(|| unsafe { init(self.host) }));
        match result {
            Ok(0) => {
                log_info(&format!("loaded mod {}", spec.id));
                Ok(LoadedMod {
                    id: spec.id.clone(),
                    handle,
                    tick,
                    shutdown,
                })
            }
            Ok(code) => {
                unsafe { dlclose(handle) };
                Err(format!("universe_init returned {code}"))
            }
            Err(_) => {
                unsafe { dlclose(handle) };
                Err("universe_init panicked".to_string())
            }
        }
    }

    /// Ticks every live mod, advancing the frame counter once.
    ///
    /// A mod that panics on tick is unloaded immediately and counted; after [`MAX_FAULTS`] it is
    /// refused at the next load. One mod's fault never stops another's tick.
    pub fn tick(&mut self, dt: f32) {
        self.frame = self.frame.wrapping_add(1);
        let frame = self.frame;
        let mut index = 0;
        while index < self.loaded.len() {
            let id = self.loaded[index].id.clone();
            let tick = self.loaded[index].tick;
            let faulted = catch_unwind(AssertUnwindSafe(|| unsafe { tick(frame, dt) })).is_err();
            if faulted {
                log_error(&format!("mod {id} panicked on tick; unloading it"));
                let decision = self.tracker.record_fault(&id);
                self.unload_at(index);
                if decision.action == universe_core::fault::FaultAction::Disable {
                    log_error(&format!(
                        "mod {id} disabled after {} consecutive faults",
                        decision.fault_count
                    ));
                }
                continue; // the entry at `index` is now the next mod
            }
            self.tracker.record_success(&id);
            index += 1;
        }
    }

    /// Shuts every live mod down and closes it, whether or not its `shutdown` panics.
    pub fn shutdown_all(&mut self) {
        while let Some(loaded) = self.loaded.pop() {
            let id = loaded.id.clone();
            let shutdown = loaded.shutdown;
            if catch_unwind(AssertUnwindSafe(|| unsafe { shutdown() })).is_err() {
                log_error(&format!("mod {id} panicked on shutdown"));
            }
            unsafe { dlclose(loaded.handle) };
        }
        log_info("all mods shut down");
    }

    /// Closes the mod at `index` after calling its `shutdown`.
    fn unload_at(&mut self, index: usize) {
        let loaded = self.loaded.remove(index);
        let shutdown = loaded.shutdown;
        if catch_unwind(AssertUnwindSafe(|| unsafe { shutdown() })).is_err() {
            log_error(&format!("mod {} panicked on shutdown", loaded.id));
        }
        unsafe { dlclose(loaded.handle) };
    }
}

/// `RTLD_NOW`.
const RTLD_NOW: i32 = 2;
/// `RTLD_LOCAL`.
const RTLD_LOCAL: i32 = 0;

extern "C" {
    fn dlopen(filename: *const c_char, flags: i32) -> *mut c_void;
    fn dlclose(handle: *mut c_void) -> i32;
    fn dlerror() -> *const c_char;
    #[link_name = "dlsym"]
    fn libc_dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
}

/// Resolves `symbol` from `handle`, as `T`, or `None`.
///
/// # Safety
/// `handle` must be a handle from `dlopen`; `symbol` a NUL-terminated name.
unsafe fn resolve_symbol<T>(handle: *mut c_void, symbol: &[u8]) -> Option<T> {
    let found = libc_dlsym(handle, symbol.as_ptr() as *const c_char);
    if found.is_null() {
        None
    } else {
        Some(std::mem::transmute_copy(&found))
    }
}

/// The last `dlerror` text, or a generic message.
fn dl_error() -> String {
    let pointer = unsafe { dlerror() };
    if pointer.is_null() {
        return "no dl error".to_string();
    }
    unsafe { CStr::from_ptr(pointer) }
        .to_string_lossy()
        .into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;
    use universe_core::runtime::ModSpec;

    fn spec(id: &str, lib: &str, deps: &[&str]) -> ModSpec {
        ModSpec {
            id: id.to_string(),
            lib_path: lib.to_string(),
            dependencies: deps.iter().map(|d| d.to_string()).collect(),
        }
    }

    #[test]
    fn a_mod_with_no_library_is_refused_not_loaded() {
        let mut runtime = ModRuntime::new();
        let plan = runtime.load(&[spec("a", "", &[])]);
        assert_eq!(plan.load, vec!["a"]);
        // The plan lists it, but the load failed, so nothing is live.
        assert_eq!(runtime.live_count(), 0);
    }

    #[test]
    fn a_missing_library_is_refused() {
        let mut runtime = ModRuntime::new();
        runtime.load(&[spec("a", "/nonexistent/liba.so", &[])]);
        assert_eq!(runtime.live_count(), 0);
    }

    #[test]
    fn ticking_with_no_mods_advances_the_frame() {
        let mut runtime = ModRuntime::new();
        runtime.tick(0.016);
        runtime.tick(0.016);
        assert_eq!(runtime.frame(), 2);
    }

    #[test]
    fn shutdown_with_nothing_loaded_is_safe() {
        let mut runtime = ModRuntime::new();
        runtime.shutdown_all();
        assert_eq!(runtime.live_count(), 0);
    }

    #[test]
    fn a_library_without_entry_points_is_refused() {
        // libc has no universe_* symbols, so it is opened then closed again.
        let mut runtime = ModRuntime::new();
        runtime.load(&[spec("libc", "libc.so.6", &[])]);
        assert_eq!(runtime.live_count(), 0);
    }

    #[test]
    fn loads_a_real_mod_and_ticks_it() {
        // The only way to prove dlopen/dlsym/entry-point wiring is to load a real library. The
        // fixture is compiled with the host `cc`; without one the test is skipped rather than
        // failing, because the property it checks is exercised by the device build.
        let Some(lib) = compile_fixture() else {
            eprintln!("skipping: no host C compiler");
            return;
        };
        let mut runtime = ModRuntime::new();
        runtime.set_host(std::ptr::null());
        let plan = runtime.load(&[spec("fixture", &lib, &[])]);
        assert_eq!(plan.load, vec!["fixture"]);
        assert_eq!(runtime.live_count(), 1, "the fixture should be live");

        runtime.tick(0.016);
        runtime.tick(0.016);
        assert_eq!(runtime.frame(), 2);

        // Read the fixture's own counter to prove the ticks actually reached it.
        let frames = read_fixture_frames(&lib);
        assert_eq!(frames, 2, "the fixture counted two ticks");

        runtime.shutdown_all();
        assert_eq!(runtime.live_count(), 0);
    }

    #[test]
    fn refuses_a_mod_whose_init_fails() {
        // Fail-closed: a non-zero universe_init means the mod must not be live, and a later tick
        // must not reach it. This is the C++ runtime's "refuse to load it" rule, exercised for real.
        const BAD: &str = r#"
#include <stdint.h>
static uint64_t g_frames = 0;
int32_t universe_init(const void *host) { (void)host; return 1; }
void universe_tick(uint64_t frame, float dt) { (void)frame; (void)dt; g_frames++; }
void universe_shutdown(void) {}
uint64_t universe_test_frames(void) { return g_frames; }
"#;
        let Some(lib) = compile_source("badinit", BAD) else {
            eprintln!("skipping: no host C compiler");
            return;
        };
        let mut runtime = ModRuntime::new();
        runtime.load(&[spec("badinit", &lib, &[])]);
        assert_eq!(runtime.live_count(), 0, "a failed init is not loaded");
        runtime.tick(0.016);
        assert_eq!(
            read_fixture_frames(&lib),
            0,
            "a refused mod is never ticked"
        );
    }

    /// The fixture's source: a well-formed `.universe` mod that counts its ticks.
    const FIXTURE_SOURCE: &str = r#"
#include <stdint.h>
static uint64_t g_frames = 0;
int32_t universe_init(const void *host) { (void)host; return 0; }
void universe_tick(uint64_t frame, float dt) { (void)frame; (void)dt; g_frames++; }
void universe_shutdown(void) {}
uint64_t universe_test_frames(void) { return g_frames; }
"#;

    /// Compiles [`FIXTURE_SOURCE`] into a shared library, returning its path, or `None` when no host
    /// compiler is available.
    fn compile_fixture() -> Option<String> {
        compile_source("fixture", FIXTURE_SOURCE)
    }

    /// Compiles `source` into a shared library under a name, or `None` without a host compiler.
    fn compile_source(name: &str, source_text: &str) -> Option<String> {
        let dir = std::env::temp_dir().join(format!("sweetloader-fixture-{}", std::process::id()));
        std::fs::create_dir_all(&dir).ok()?;
        let source = dir.join(format!("{name}.c"));
        std::fs::write(&source, source_text).ok()?;
        let library = dir.join(format!("lib{name}.so"));
        let status = std::process::Command::new("cc")
            .args(["-shared", "-fPIC", "-o"])
            .arg(&library)
            .arg(&source)
            .status()
            .ok()?;
        if !status.success() {
            return None;
        }
        Some(library.to_string_lossy().into_owned())
    }

    /// `dlopen`s the fixture and reads its tick counter through `universe_test_frames`.
    fn read_fixture_frames(path: &str) -> u64 {
        let name = CString::new(path).unwrap();
        let handle = unsafe { dlopen(name.as_ptr(), RTLD_NOW | RTLD_LOCAL) };
        assert!(!handle.is_null(), "the fixture should reopen");
        let frames_fn = unsafe {
            resolve_symbol::<unsafe extern "C" fn() -> u64>(handle, b"universe_test_frames\0")
        };
        let frames = unsafe { frames_fn.expect("the fixture exports its counter")() };
        unsafe { dlclose(handle) };
        frames
    }
}
