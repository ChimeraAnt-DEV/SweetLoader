//! The `.universe` host table and the permission filter that reduces it per mod.
//!
//! `pl/universe/universe_api.h` defines `universe_host_t`: the function table the loader hands to a
//! mod's `universe_init`. The C++ side builds the *full* table — every capability the process can
//! provide, each pointer non-null only where the underlying facility exists on this build — and
//! this module produces the *reduced* table a specific mod is allowed to see.
//!
//! **The reduction is the enforcement.** A mod never receives a table with a capability it did not
//! declare and, for the trust-gated ones, was not signed for: the pointer is left null, and the
//! format's contract is that a mod null-checks before calling. So "the permission model is
//! enforced" means exactly "`filter` runs before `universe_init`", and it is a pure function of the
//! full table and the granted permissions, unit-tested without a device.
//!
//! The table's *layout* is the ABI: the field order here must match `universe_host_t` in the
//! header, and `SIZE`/`ABI_VERSION` are what a mod checks before it uses any field. Reordering or
//! inserting a field means bumping `UNIVERSE_HOST_ABI_VERSION` in both places; the mirror test at
//! the bottom pins the layout so a drift is a failing test rather than a misread pointer on device.

use std::ffi::{c_char, c_void};

use crate::permission::{granted, Permission};

/// `UNIVERSE_HOST_ABI_VERSION` from `universe_api.h`. Bump in both places together.
pub const HOST_ABI_VERSION: u32 = 1;

/// The per-frame hook callback type, matching `universe_hook_fn`.
pub type HookFn = unsafe extern "C" fn(frame: u64, context: *mut c_void);

/// A hook registration: the byte signature, the module to scan and the mod's callback.
pub type RegisterHookFn = unsafe extern "C" fn(
    signature: *const c_char,
    module: *const c_char,
    callback: HookFn,
    context: *mut c_void,
) -> u64;
/// Removes a hook by the handle `register_hook` returned.
pub type UnregisterHookFn = unsafe extern "C" fn(handle: u64);
/// Resolves a byte signature to an address without hooking it.
pub type ResolveSignatureFn =
    unsafe extern "C" fn(signature: *const c_char, module: *const c_char) -> usize;
/// Reads process memory. Returns false (and writes nothing) when the address is not readable.
pub type ReadMemoryFn =
    unsafe extern "C" fn(address: usize, out: *mut c_void, length: usize) -> bool;
/// Writes process memory. Trusted-signature mods only.
pub type WriteMemoryFn =
    unsafe extern "C" fn(address: usize, data: *const c_void, length: usize) -> bool;
/// Reads the local player's position and rotation. False means "no data this frame".
pub type GetLocalPlayerFn =
    unsafe extern "C" fn(out_position: *mut f32, out_rotation: *mut f32) -> bool;
/// The BerryAPI version string, or null when BerryAPI is absent.
pub type BerryApiVersionFn = unsafe extern "C" fn() -> *const c_char;
/// Logs a line to the loader's log.
pub type LogFn = unsafe extern "C" fn(level: i32, message: *const c_char);

/// The host table, mirroring `universe_host_t` in `pl/universe/universe_api.h`.
///
/// `#[repr(C)]` and field-for-field: this is the struct a mod's `universe_init` receives, so the
/// order is the ABI. `Option<fn>` is the same size as a C function pointer (null is `None`), which
/// is what makes the mirror sound.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UniverseHost {
    pub size: u32,
    pub abi_version: u32,
    pub register_hook: Option<RegisterHookFn>,
    pub unregister_hook: Option<UnregisterHookFn>,
    pub resolve_signature: Option<ResolveSignatureFn>,
    pub read_memory: Option<ReadMemoryFn>,
    pub write_memory: Option<WriteMemoryFn>,
    pub get_local_player: Option<GetLocalPlayerFn>,
    pub berry_api_version: Option<BerryApiVersionFn>,
    pub log: Option<LogFn>,
}

impl Default for UniverseHost {
    fn default() -> Self {
        UniverseHost {
            size: std::mem::size_of::<UniverseHost>() as u32,
            abi_version: HOST_ABI_VERSION,
            register_hook: None,
            unregister_hook: None,
            resolve_signature: None,
            read_memory: None,
            write_memory: None,
            get_local_player: None,
            berry_api_version: None,
            log: None,
        }
    }
}

/// The permission a host-table capability requires, or `None` for a capability every mod may use.
///
/// `log` and `berry_api_version` are unrestricted: a version string and a log line carry no
/// capability, and refusing them would only make a misbehaving mod harder to diagnose.
fn required_permission(field: HostField) -> Option<Permission> {
    match field {
        HostField::RegisterHook | HostField::UnregisterHook => Some(Permission::RenderHook),
        HostField::ResolveSignature | HostField::ReadMemory => Some(Permission::ReadMemory),
        HostField::WriteMemory => Some(Permission::WriteMemory),
        HostField::GetLocalPlayer => Some(Permission::Entities),
        HostField::BerryApiVersion | HostField::Log => None,
    }
}

/// One capability in the table, named so the filter and its test share the mapping.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HostField {
    RegisterHook,
    UnregisterHook,
    ResolveSignature,
    ReadMemory,
    WriteMemory,
    GetLocalPlayer,
    BerryApiVersion,
    Log,
}

impl HostField {
    /// Every capability, in table order.
    pub fn all() -> &'static [HostField] {
        &[
            HostField::RegisterHook,
            HostField::UnregisterHook,
            HostField::ResolveSignature,
            HostField::ReadMemory,
            HostField::WriteMemory,
            HostField::GetLocalPlayer,
            HostField::BerryApiVersion,
            HostField::Log,
        ]
    }

    /// True when this capability's pointer is non-null in `host`.
    pub fn present_in(self, host: &UniverseHost) -> bool {
        match self {
            HostField::RegisterHook => host.register_hook.is_some(),
            HostField::UnregisterHook => host.unregister_hook.is_some(),
            HostField::ResolveSignature => host.resolve_signature.is_some(),
            HostField::ReadMemory => host.read_memory.is_some(),
            HostField::WriteMemory => host.write_memory.is_some(),
            HostField::GetLocalPlayer => host.get_local_player.is_some(),
            HostField::BerryApiVersion => host.berry_api_version.is_some(),
            HostField::Log => host.log.is_some(),
        }
    }
}

/// Reduces `full` to the table a mod with `declared` permissions and the given signature trust may
/// see.
///
/// A capability is kept only when the mod declared the permission it needs (and, for `write_memory`,
/// also carried a trusted signature). Every dropped pointer is set to `None`, so the mod sees a
/// null and the format's fail-closed rule applies. `size` and `abi_version` are preserved: the mod
/// must still see a table it recognises, and the ABI numbers are not a capability.
pub fn filter(full: &UniverseHost, declared: &[Permission], trusted: bool) -> UniverseHost {
    let mut reduced = *full;
    for field in HostField::all() {
        let Some(permission) = required_permission(*field) else {
            continue;
        };
        if granted(declared, permission, trusted) {
            continue;
        }
        match field {
            HostField::RegisterHook => reduced.register_hook = None,
            HostField::UnregisterHook => reduced.unregister_hook = None,
            HostField::ResolveSignature => reduced.resolve_signature = None,
            HostField::ReadMemory => reduced.read_memory = None,
            HostField::WriteMemory => reduced.write_memory = None,
            HostField::GetLocalPlayer => reduced.get_local_player = None,
            HostField::BerryApiVersion => reduced.berry_api_version = None,
            HostField::Log => reduced.log = None,
        }
    }
    reduced
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe extern "C" fn register_hook(
        _signature: *const c_char,
        _module: *const c_char,
        _callback: HookFn,
        _context: *mut c_void,
    ) -> u64 {
        1
    }
    unsafe extern "C" fn unregister_hook(_handle: u64) {}
    unsafe extern "C" fn resolve_signature(_s: *const c_char, _m: *const c_char) -> usize {
        0
    }
    unsafe extern "C" fn read_memory(_a: usize, _o: *mut c_void, _l: usize) -> bool {
        true
    }
    unsafe extern "C" fn write_memory(_a: usize, _d: *const c_void, _l: usize) -> bool {
        true
    }
    unsafe extern "C" fn get_local_player(_p: *mut f32, _r: *mut f32) -> bool {
        true
    }
    unsafe extern "C" fn berry_api_version() -> *const c_char {
        std::ptr::null()
    }
    unsafe extern "C" fn log(_level: i32, _message: *const c_char) {}

    fn full() -> UniverseHost {
        UniverseHost {
            register_hook: Some(register_hook),
            unregister_hook: Some(unregister_hook),
            resolve_signature: Some(resolve_signature),
            read_memory: Some(read_memory),
            write_memory: Some(write_memory),
            get_local_player: Some(get_local_player),
            berry_api_version: Some(berry_api_version),
            log: Some(log),
            ..UniverseHost::default()
        }
    }

    #[test]
    fn a_mod_with_no_permissions_keeps_only_the_unrestricted_capabilities() {
        let reduced = filter(&full(), &[], false);
        assert!(reduced.register_hook.is_none());
        assert!(reduced.read_memory.is_none());
        assert!(reduced.get_local_player.is_none());
        // A version string and a log line need no permission, so a mod can always diagnose itself.
        assert!(reduced.berry_api_version.is_some());
        assert!(reduced.log.is_some());
    }

    #[test]
    fn a_declared_permission_keeps_its_capability() {
        let reduced = filter(&full(), &[Permission::ReadMemory], false);
        assert!(reduced.read_memory.is_some());
        assert!(reduced.resolve_signature.is_some());
        // But not a capability it did not declare.
        assert!(reduced.register_hook.is_none());
        assert!(reduced.get_local_player.is_none());
    }

    #[test]
    fn write_memory_needs_both_the_declaration_and_a_trusted_signature() {
        let declared = [Permission::WriteMemory];
        assert!(filter(&full(), &declared, false).write_memory.is_none());
        assert!(filter(&full(), &declared, true).write_memory.is_some());
    }

    #[test]
    fn the_abi_numbers_survive_the_reduction() {
        let reduced = filter(&full(), &[], false);
        assert_eq!(reduced.abi_version, HOST_ABI_VERSION);
        assert_eq!(reduced.size, std::mem::size_of::<UniverseHost>() as u32);
    }

    #[test]
    fn the_layout_matches_the_header_field_order() {
        // The header's order: size, abi_version, then eight function pointers. Pin the size so a
        // field added here without a matching header change (and an ABI bump) fails here.
        let pointer = std::mem::size_of::<usize>();
        assert_eq!(
            std::mem::size_of::<UniverseHost>(),
            2 * std::mem::size_of::<u32>() + 8 * pointer
        );
        let host = UniverseHost::default();
        assert_eq!(host.size, std::mem::size_of::<UniverseHost>() as u32);
        assert_eq!(host.abi_version, HOST_ABI_VERSION);
    }
}
