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
use universe_core::host::{self, UniverseHost};
use universe_core::runtime::{self, ModSpec};

use crate::{log_error, log_info};

/// `universe_init(const universe_host_t *) -> int32_t`.
///
/// `extern "C-unwind"`, not `extern "C"`: a mod written in Rust may panic, and a panic unwinding
/// out of an `extern "C"` function is undefined behaviour (in practice an abort), so the
/// `catch_unwind` around these calls would never get a chance to run. The `-unwind` ABI lets the
/// panic reach the guard, which is the whole point of loading a mod behind one.
type InitFn = unsafe extern "C-unwind" fn(*const c_void) -> i32;
/// `universe_tick(uint64_t frame, float dt)`.
type TickFn = unsafe extern "C-unwind" fn(u64, f32);
/// `universe_shutdown(void)`.
type ShutdownFn = unsafe extern "C-unwind" fn();

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
    /// One reduced host table per live mod, boxed so the pointer stays valid for the session.
    ///
    /// The reduced table is what the mod's `universe_init` receives. It cannot live on the stack of
    /// the call that hands it over: a mod may keep the pointer and read the table from its
    /// `universe_tick`, so it must outlive the init call. One per mod id, kept until the mod is
    /// unloaded, so re-loading the same mod reuses its table rather than leaking a second.
    reduced_hosts: HashMap<String, Box<UniverseHost>>,
}

unsafe impl Send for ModRuntime {}

impl Default for ModRuntime {
    fn default() -> Self {
        ModRuntime {
            loaded: Vec::new(),
            tracker: FaultTracker::new(),
            frame: 0,
            host: std::ptr::null(),
            reduced_hosts: HashMap::new(),
        }
    }
}

impl ModRuntime {
    pub fn new() -> ModRuntime {
        ModRuntime::default()
    }

    /// Sets the host table handed to a mod's `universe_init`.
    ///
    /// The pointer is the *full* table the C++ side built; [`Self::host_for`] reduces it per mod.
    pub fn set_host(&mut self, host: *const c_void) {
        self.host = host;
    }

    /// The host table pointer a specific mod receives, reduced to the capabilities it declared.
    ///
    /// Returns the full table's null pointer when no full table was supplied (the C++ side passes
    /// null until its own facilities resolve), in which case the mod reports every capability
    /// unavailable itself. When a full table exists, the reduced table is boxed and kept in
    /// [`Self::reduced_hosts`] under the mod's id so the pointer stays valid for the mod's whole
    /// life; every capability the mod did not declare (or that needs a trusted signature it does not
    /// have) is nulled out — the enforcement the format documents. A mod whose declared permissions
    /// do not parse gets the fully-reduced table (fail-closed), because the manifest gate already
    /// refused it.
    fn host_for(&mut self, spec: &ModSpec) -> *const c_void {
        if self.host.is_null() {
            return std::ptr::null();
        }
        // Safety: the pointer is the full table the C++ side owns and keeps alive for the session;
        // it is read, never written, and only under the loader's mutex.
        let full = unsafe { &*(self.host as *const UniverseHost) };
        let declared = spec.parsed_permissions().unwrap_or_default();
        let reduced = host::filter(full, &declared, spec.trusted);
        let boxed = self
            .reduced_hosts
            .entry(spec.id.clone())
            .or_insert_with(|| Box::new(reduced));
        // Refresh the entry in place so a re-load with different permissions sees the new grant.
        **boxed = reduced;
        boxed.as_ref() as *const UniverseHost as *const c_void
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
        // Unload any live mod that is no longer in the list, so a mod the user removed stops
        // running: otherwise its hooks and threads outlive the entry in the mod list.
        let wanted: std::collections::HashSet<&str> = specs.iter().map(|s| s.id.as_str()).collect();
        let mut index = 0;
        while index < self.loaded.len() {
            if wanted.contains(self.loaded[index].id.as_str()) {
                index += 1;
            } else {
                log_info(&format!(
                    "unloading mod {}: it is no longer in the mod list",
                    self.loaded[index].id
                ));
                self.unload_at(index);
            }
        }
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
    fn open_one(&mut self, spec: &ModSpec) -> Result<LoadedMod, String> {
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

        // The mod's own init is the first call into mod code, so it is guarded like a tick. The
        // host table is reduced to the mod's declared permissions first: this is where the
        // permission model is enforced, not merely validated.
        let host_pointer = self.host_for(spec);
        let result = catch_unwind(AssertUnwindSafe(|| unsafe { init(host_pointer) }));
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
    ///
    /// Each mod's own tick time is also measured against
    /// [`universe_core::budget::TICK_BUDGET_US`]: a running native call cannot be interrupted, so a
    /// mod that blocks *inside* one tick cannot be stopped here, but a mod that overruns on every
    /// frame is counted as a fault and disabled after [`MAX_FAULTS`] consecutive overruns rather
    /// than taxing every frame forever. The clock is the loader's (the device side); the budget
    /// comparison is the pure [`universe_core::budget`] rule, so the policy is unit-tested off-device
    /// and only the measurement is device-only.
    pub fn tick(&mut self, dt: f32) {
        self.frame = self.frame.wrapping_add(1);
        let frame = self.frame;
        let mut index = 0;
        while index < self.loaded.len() {
            let id = self.loaded[index].id.clone();
            let tick = self.loaded[index].tick;
            let started = std::time::Instant::now();
            let faulted = catch_unwind(AssertUnwindSafe(|| unsafe { tick(frame, dt) })).is_err();
            let elapsed_us = started.elapsed().as_micros() as u64;
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
            if let Some(reason) = universe_core::budget::overrun_reason(elapsed_us) {
                // The tick returned, but it overran. Count it as a fault and drop the mod once the
                // policy disables it, so a slow-but-returning mod cannot tax every frame forever.
                log_error(&format!("mod {id} overran its tick budget: {reason}"));
                let decision = self.tracker.record_fault(&id);
                if decision.action == universe_core::fault::FaultAction::Disable {
                    log_error(&format!(
                        "mod {id} disabled after {} consecutive overruns",
                        decision.fault_count
                    ));
                    self.unload_at(index);
                    continue;
                }
                index += 1;
                continue;
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
            // The reduced table is only reachable through the mod that was just closed.
            self.reduced_hosts.remove(&id);
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
        self.reduced_hosts.remove(&loaded.id);
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
            permissions: Vec::new(),
            trusted: false,
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
    fn a_mod_that_overruns_every_tick_is_disabled() {
        // The budget rule is pure and tested in `universe-core::budget`; here we prove the loader
        // counts an overrun as a fault and stops calling the mod once the policy disables it. The
        // fixture blocks past the budget on every tick, so after MAX_FAULTS ticks it must be gone.
        const SLOW: &str = r#"
#include <stdint.h>
#include <time.h>
static uint64_t g_frames = 0;
int32_t universe_init(const void *host) { (void)host; return 0; }
void universe_tick(uint64_t frame, float dt) {
    (void)frame; (void)dt; g_frames++;
    struct timespec ts = { 0, 8000000 }; // 8 ms, well over the 2 ms budget
    nanosleep(&ts, 0);
}
void universe_shutdown(void) {}
uint64_t universe_test_frames(void) { return g_frames; }
"#;
        let Some(lib) = compile_source("slow", SLOW) else {
            eprintln!("skipping: no host C compiler");
            return;
        };
        let mut runtime = ModRuntime::new();
        runtime.load(&[spec("slow", &lib, &[])]);
        assert_eq!(runtime.live_count(), 1, "the slow fixture loads");
        for _ in 0..universe_core::fault::MAX_FAULTS {
            runtime.tick(0.016);
        }
        assert_eq!(
            runtime.live_count(),
            0,
            "a mod that overruns on every tick is disabled"
        );
        assert!(runtime.disabled().contains(&"slow".to_string()));
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

    #[test]
    fn a_mod_dropped_from_the_list_is_unloaded() {
        // A mod removed from the list must stop running, not linger with live hooks and threads.
        let Some(lib) = compile_fixture() else {
            eprintln!("skipping: no host C compiler");
            return;
        };
        let mut runtime = ModRuntime::new();
        runtime.load(&[spec("fixture", &lib, &[])]);
        assert_eq!(runtime.live_count(), 1);
        // Re-planning with an empty list must unload the fixture.
        runtime.load(&[]);
        assert_eq!(
            runtime.live_count(),
            0,
            "a mod absent from the new list must be unloaded"
        );
    }

    #[test]
    fn the_host_table_is_reduced_to_the_mods_declared_permissions() {
        // The enforcement, proved end to end: the loader hands the mod a table whose granted
        // pointers are non-null and whose ungranted ones are null, and the mod can read that back.
        // The C++ side builds the *full* table; here the test builds one itself so it needs no
        // device.
        use universe_core::host::{UniverseHost, HOST_ABI_VERSION};
        // Stand-in capabilities: real no-op functions so the table needs no unsafe pointer surgery.
        unsafe extern "C" fn noop_read(_a: usize, _o: *mut c_void, _l: usize) -> bool {
            true
        }
        unsafe extern "C" fn noop_write(_a: usize, _d: *const c_void, _l: usize) -> bool {
            true
        }
        unsafe extern "C" fn noop_log(_level: i32, _message: *const c_char) {}
        let mut full = UniverseHost::default();
        full.read_memory = Some(noop_read);
        full.write_memory = Some(noop_write);
        full.log = Some(noop_log);
        full.size = std::mem::size_of::<UniverseHost>() as u32;

        const OBSERVER: &str = r#"
#include <stdint.h>
#include <stddef.h>
typedef struct universe_host {
  uint32_t size; uint32_t abi_version;
  void *register_hook; void *unregister_hook; void *resolve_signature;
  void *read_memory; void *write_memory; void *get_local_player;
  void *berry_api_version; void *log;
} universe_host_t;
static uint32_t g_abi = 0;
static int g_read = -1;
static int g_write = -1;
static int g_log = -1;
int32_t universe_init(const void *host_ptr) {
  const universe_host_t *h = (const universe_host_t *)host_ptr;
  g_abi = h->abi_version;
  g_read = h->read_memory != 0;
  g_write = h->write_memory != 0;
  g_log = h->log != 0;
  return 0;
}
void universe_tick(uint64_t frame, float dt) { (void)frame; (void)dt; }
void universe_shutdown(void) {}
uint32_t universe_test_abi(void) { return g_abi; }
int universe_test_read(void) { return g_read; }
int universe_test_write(void) { return g_write; }
int universe_test_log(void) { return g_log; }
"#;
        let Some(lib) = compile_source("observer", OBSERVER) else {
            eprintln!("skipping: no host C compiler");
            return;
        };
        let read_int = |name: &str| -> i32 {
            let cname = CString::new(lib.clone()).unwrap();
            let handle = unsafe { dlopen(cname.as_ptr(), RTLD_NOW | RTLD_LOCAL) };
            let symbol = CString::new(name).unwrap();
            let f = unsafe {
                resolve_symbol::<unsafe extern "C" fn() -> i32>(handle, symbol.as_bytes_with_nul())
            };
            let value = unsafe { f.unwrap()() };
            unsafe { dlclose(handle) };
            value
        };
        let read_abi = || -> u32 {
            let cname = CString::new(lib.clone()).unwrap();
            let handle = unsafe { dlopen(cname.as_ptr(), RTLD_NOW | RTLD_LOCAL) };
            let f = unsafe {
                resolve_symbol::<unsafe extern "C" fn() -> u32>(handle, b"universe_test_abi\0")
            };
            let value = unsafe { f.unwrap()() };
            unsafe { dlclose(handle) };
            value
        };

        let mut runtime = ModRuntime::new();
        runtime.set_host(&full as *const UniverseHost as *const c_void);
        let mut mod_spec = spec("observer", &lib, &[]);
        // Declare read_memory only; write_memory is trust-gated and not declared, so it must be
        // nulled; log needs no permission, so it stays.
        mod_spec.permissions = vec!["read_memory".to_string()];
        runtime.load(&[mod_spec]);
        assert_eq!(runtime.live_count(), 1, "the observer mod loads");

        assert_eq!(read_abi(), HOST_ABI_VERSION, "the ABI version survives");
        assert_eq!(
            read_int("universe_test_read"),
            1,
            "a declared capability is kept"
        );
        assert_eq!(
            read_int("universe_test_write"),
            0,
            "an undeclared, trust-gated capability is dropped"
        );
        assert_eq!(
            read_int("universe_test_log"),
            1,
            "an unrestricted capability is kept"
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
