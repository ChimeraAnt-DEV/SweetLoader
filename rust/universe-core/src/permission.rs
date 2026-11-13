//! The `.universe` permission model.
//!
//! A mod declares the capabilities it needs in its manifest; the user grants them at install time.
//! The loader then checks the grant before a mod reaches a capability, so an undeclared call is
//! refused rather than quietly performed. Two rules are deliberately stricter than "declared means
//! granted":
//!
//! * `write_memory` is never granted to an unsigned mod, whatever it declares. Writing arbitrary
//!   process memory is the capability that turns a cosmetic mod into a game-breaking one, so it is
//!   reserved for a package signed by a trusted developer key.
//! * An unknown capability name is a manifest error, not an ignored entry, so a typo cannot silently
//!   widen or narrow the grant.

use std::fmt;

use crate::{Error, Result};

/// A capability a `.universe` mod can request.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Permission {
    /// Read process memory (signature scans, entity reads).
    ReadMemory,
    /// Write process memory. Trusted-signed packages only.
    WriteMemory,
    /// Install render hooks and inject geometry.
    RenderHook,
    /// Send and receive custom network packets.
    Network,
    /// Read files inside the mod's own sandbox directory.
    FileSystem,
    /// Draw to the game's UI layer.
    Ui,
    /// Read and drive entities and the local player.
    Entities,
    /// Read and modify the world (blocks, fluids, physics).
    World,
    /// Play and mix audio.
    Audio,
}

impl Permission {
    /// The canonical manifest name.
    pub fn as_str(self) -> &'static str {
        match self {
            Permission::ReadMemory => "read_memory",
            Permission::WriteMemory => "write_memory",
            Permission::RenderHook => "render_hook",
            Permission::Network => "network",
            Permission::FileSystem => "file_system",
            Permission::Ui => "ui",
            Permission::Entities => "entities",
            Permission::World => "world",
            Permission::Audio => "audio",
        }
    }

    /// Parses a manifest capability name, refusing an unknown one.
    pub fn parse(name: &str) -> Result<Permission> {
        let normalised = name.trim().to_ascii_lowercase();
        match normalised.as_str() {
            "read_memory" => Ok(Permission::ReadMemory),
            "write_memory" => Ok(Permission::WriteMemory),
            "render_hook" => Ok(Permission::RenderHook),
            "network" => Ok(Permission::Network),
            "file_system" | "filesystem" => Ok(Permission::FileSystem),
            "ui" => Ok(Permission::Ui),
            "entities" => Ok(Permission::Entities),
            "world" => Ok(Permission::World),
            "audio" => Ok(Permission::Audio),
            other => Err(Error::new(format!("unknown permission '{other}'"))),
        }
    }

    /// True when granting this capability must be gated on a trusted signature.
    pub fn requires_trusted_signature(self) -> bool {
        matches!(self, Permission::WriteMemory)
    }

    /// Every permission the loader understands, for the UI's install prompt.
    pub fn all() -> &'static [Permission] {
        &[
            Permission::ReadMemory,
            Permission::WriteMemory,
            Permission::RenderHook,
            Permission::Network,
            Permission::FileSystem,
            Permission::Ui,
            Permission::Entities,
            Permission::World,
            Permission::Audio,
        ]
    }
}

impl fmt::Display for Permission {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// True when `declared` (the user's grant) covers `requested` under the trust policy.
///
/// The two conditions are independent and both required: the capability must be declared, and if it
/// is trust-gated the package must be trusted-signed. A trusted signature does not grant an
/// undeclared capability, and a declaration does not grant a trust-gated one.
pub fn granted(declared: &[Permission], requested: Permission, trusted_signature: bool) -> bool {
    if !declared.contains(&requested) {
        return false;
    }
    if requested.requires_trusted_signature() && !trusted_signature {
        return false;
    }
    true
}

/// The first permission in `requested` that is not granted, if any.
pub fn first_denied(
    declared: &[Permission],
    requested: &[Permission],
    trusted_signature: bool,
) -> Option<Permission> {
    requested
        .iter()
        .copied()
        .find(|permission| !granted(declared, *permission, trusted_signature))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_known_permissions() {
        assert_eq!(Permission::parse("read_memory").unwrap(), Permission::ReadMemory);
        assert_eq!(Permission::parse("Render_Hook").unwrap(), Permission::RenderHook);
        assert_eq!(Permission::parse("filesystem").unwrap(), Permission::FileSystem);
    }

    #[test]
    fn refuses_unknown_permissions() {
        assert!(Permission::parse("become_root").is_err());
        assert!(Permission::parse("").is_err());
    }

    #[test]
    fn write_memory_requires_a_trusted_signature() {
        let declared = [Permission::WriteMemory];
        assert!(!granted(&declared, Permission::WriteMemory, false));
        assert!(granted(&declared, Permission::WriteMemory, true));
    }

    #[test]
    fn an_undeclared_permission_is_never_granted() {
        let declared = [Permission::ReadMemory];
        assert!(!granted(&declared, Permission::RenderHook, true));
    }

    #[test]
    fn trust_does_not_grant_an_undeclared_capability() {
        let declared = [Permission::Ui];
        assert!(first_denied(&declared, &[Permission::Ui], true).is_none());
        assert_eq!(
            first_denied(&declared, &[Permission::Network], true),
            Some(Permission::Network)
        );
    }

    #[test]
    fn every_permission_round_trips_through_its_name() {
        for permission in Permission::all() {
            assert_eq!(Permission::parse(permission.as_str()).unwrap(), *permission);
        }
    }
}
