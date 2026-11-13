//! The BerryAPI surface the `.universe` loader calls.
//!
//! BerryAPI (`libBerryAPI.so`) is the client's existing native runtime: it owns the hook backend,
//! the per-frame render seam and the cosmetic, replay and governor subsystems. `.universe` mods do
//! not re-implement any of that; they reach it through this module, which resolves the BerryAPI C
//! ABI once and hands back null when a call is absent.
//!
//! Two rules keep this safe:
//!
//! * **Resolve once, fail closed.** Every entry point is `Option<unsafe extern "C" fn>`; a call the
//!   loaded BerryAPI does not export is `None`, and the feature that needs it reports unavailable
//!   rather than calling through a null pointer.
//! * **Never assume the ABI.** The struct-size and ABI-version queries BerryAPI exposes are checked
//!   before a mirrored struct is read, so a layout drift is refused by a number rather than a
//!   misread pointer.

use std::ffi::{c_char, c_void, CStr};

/// The raw BerryAPI entry points the loader may call.
///
/// The names and signatures mirror the symbols BerryAPI exports. `BERRY_Inject` and
/// `BERRY_IsAttached` return `int` (0/1), not a Rust `bool`: BerryAPI's own header documents that a
/// one-byte bool return leaves the upper result-register bytes undefined.
#[derive(Default)]
pub struct BerryApi {
    pub version: Option<unsafe extern "C" fn() -> *const c_char>,
    pub is_attached: Option<unsafe extern "C" fn() -> i32>,
    pub inject: Option<unsafe extern "C" fn() -> i32>,
    pub set_world_seam: Option<unsafe extern "C" fn(*const c_void) -> i32>,
    pub has_world_seam: Option<unsafe extern "C" fn() -> i32>,
    pub world_seam_struct_size: Option<unsafe extern "C" fn() -> u32>,
    pub world_seam_abi_version: Option<unsafe extern "C" fn() -> u32>,
}

impl BerryApi {
    /// Reads the version string, when the call is present.
    pub fn version_string(&self) -> Option<String> {
        let call = self.version?;
        let pointer = unsafe { call() };
        if pointer.is_null() {
            return None;
        }
        Some(unsafe { CStr::from_ptr(pointer) }.to_string_lossy().into_owned())
    }

    /// True when BerryAPI reports itself attached to the game.
    pub fn attached(&self) -> bool {
        match self.is_attached {
            Some(call) => (unsafe { call() }) != 0,
            None => false,
        }
    }

    /// True when the world seam is bound.
    pub fn has_world_seam(&self) -> bool {
        match self.has_world_seam {
            Some(call) => (unsafe { call() }) != 0,
            None => false,
        }
    }

    /// The seam ABI numbers BerryAPI reports, when it exposes them.
    pub fn seam_abi(&self) -> Option<(u32, u32)> {
        let size = self.world_seam_struct_size?;
        let version = self.world_seam_abi_version?;
        Some((unsafe { size() }, unsafe { version() }))
    }
}

/// Resolves a symbol from an already-open library handle, using the process's `dlsym`.
///
/// The loader does not `dlopen` BerryAPI itself: the C++ bridge owns that handle and passes the
/// resolved pointer table in. This function exists for the unit test, which supplies a fake handle.
///
/// # Safety
/// `handle` must be a valid handle returned by `dlopen`, or null.
pub unsafe fn resolve<T>(handle: *mut c_void, name: &CStr) -> Option<T> {
    let symbol = libc_dlsym(handle, name.as_ptr());
    if symbol.is_null() {
        None
    } else {
        Some(std::mem::transmute_copy(&symbol))
    }
}

// The loader calls dlsym through the C++ bridge in practice; this declaration exists so the crate
// compiles without the NDK's dl library on the host and links against the real one on Android.
extern "C" {
    #[link_name = "dlsym"]
    fn libc_dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_absent_call_reports_unavailable() {
        let api = BerryApi::default();
        assert!(!api.attached());
        assert!(!api.has_world_seam());
        assert!(api.seam_abi().is_none());
        assert!(api.version_string().is_none());
    }

    #[test]
    fn present_calls_are_read() {
        unsafe extern "C" fn attached() -> i32 {
            1
        }
        unsafe extern "C" fn seam_size() -> u32 {
            48
        }
        unsafe extern "C" fn seam_version() -> u32 {
            8
        }
        let api = BerryApi {
            is_attached: Some(attached),
            world_seam_struct_size: Some(seam_size),
            world_seam_abi_version: Some(seam_version),
            ..Default::default()
        };
        assert!(api.attached());
        assert_eq!(api.seam_abi(), Some((48, 8)));
    }
}
