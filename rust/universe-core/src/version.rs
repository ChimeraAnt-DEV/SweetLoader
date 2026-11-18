//! Dotted-numeric version parsing and comparison.
//!
//! Minecraft Bedrock versions look like `1.26.60.28`, so the client compares dotted numeric
//! versions rather than strict semver. The rules here match the C++ `ModManifestValidation.hpp` so a
//! package accepted by one side is accepted by the other: a component is a run of digits, an empty
//! component is refused, and a missing component reads as zero so `1.20 == 1.20.0.0`.

use crate::{Error, Result};

/// A parsed dotted-numeric version.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Version {
    components: Vec<u64>,
}

impl Version {
    /// Parses `text`, allowing a single leading `v`/`V`.
    pub fn parse(text: &str) -> Result<Version> {
        let trimmed = text.trim();
        let body = trimmed
            .strip_prefix('v')
            .or_else(|| trimmed.strip_prefix('V'))
            .unwrap_or(trimmed);
        if body.is_empty() {
            return Err(Error::new("version is empty"));
        }

        let mut components = Vec::new();
        for part in body.split('.') {
            if part.is_empty() {
                return Err(Error::new(format!(
                    "version '{text}' has an empty component"
                )));
            }
            if !part.bytes().all(|b| b.is_ascii_digit()) {
                return Err(Error::new(format!(
                    "version '{text}' has a non-numeric component"
                )));
            }
            // A component long enough to overflow is refused rather than wrapped.
            let value: u64 = part
                .parse()
                .map_err(|_| Error::new(format!("version '{text}' has an oversized component")))?;
            components.push(value);
        }
        if components.is_empty() {
            return Err(Error::new(format!("version '{text}' has no components")));
        }
        Ok(Version { components })
    }

    /// Compares two versions component-wise, a missing component reading as zero.
    pub fn compare(&self, other: &Version) -> std::cmp::Ordering {
        let count = self.components.len().max(other.components.len());
        for index in 0..count {
            let left = self.components.get(index).copied().unwrap_or(0);
            let right = other.components.get(index).copied().unwrap_or(0);
            match left.cmp(&right) {
                std::cmp::Ordering::Equal => continue,
                ordering => return ordering,
            }
        }
        std::cmp::Ordering::Equal
    }
}

/// True when `text` is a well-formed dotted-numeric version.
pub fn is_valid(text: &str) -> bool {
    Version::parse(text).is_ok()
}

/// Parses a *release name* the way the client names its own builds.
///
/// The client numbers releases by name (`BETA6`, `OV.2`), not by a dotted version, so the running
/// client version is read as its runs of digits — the same rule the launcher's `UpdateVersion` uses,
/// so the two agree on what "newer" means. `BETA6` is `[6]` and `OV.2` is `[2]`. A name with no
/// digits at all is refused, so a stray string cannot silently satisfy every requirement.
pub fn parse_release(text: &str) -> Result<Version> {
    let trimmed = text.trim();
    let mut components = Vec::new();
    let mut current: u64 = 0;
    let mut in_number = false;
    for ch in trimmed.chars() {
        if ch.is_ascii_digit() {
            // Saturate rather than wrap, so a pathological name cannot read as a small number.
            current = current
                .saturating_mul(10)
                .saturating_add((ch as u64) - ('0' as u64));
            in_number = true;
        } else if in_number {
            components.push(current);
            current = 0;
            in_number = false;
        }
    }
    if in_number {
        components.push(current);
    }
    if components.is_empty() {
        return Err(Error::new(format!(
            "the client version '{text}' names no release number"
        )));
    }
    Ok(Version { components })
}

/// Refuses a package whose `min_client_version` is newer than the running client.
///
/// A package built against a newer client may call entry points or expect manifest fields this
/// client does not have, so it is refused rather than loaded and faulting. An unparseable
/// requirement is also a refusal: "I could not read your requirement" is not a licence to proceed.
///
/// `min_client_version` is the dotted version the manifest contract requires. `client_version` is
/// the *running* client, which the launcher supplies at init and which names a release (`BETA6`),
/// so it is read with [`parse_release`]. Both must parse, or the package is refused.
pub fn check_min_client_version(min_client_version: &str, client_version: &str) -> Result<()> {
    let required = Version::parse(min_client_version)?;
    let running = parse_release(client_version)?;
    if required.compare(&running) == std::cmp::Ordering::Greater {
        return Err(Error::new(format!(
            "package requires client version {min_client_version} or newer, but this client is {client_version}"
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cmp::Ordering;

    #[test]
    fn parses_and_compares_dotted_versions() {
        let a = Version::parse("1.26.60.28").unwrap();
        let b = Version::parse("v1.26.60.28").unwrap();
        assert_eq!(a.compare(&b), Ordering::Equal);
        assert_eq!(
            Version::parse("1.20")
                .unwrap()
                .compare(&Version::parse("1.20.0.0").unwrap()),
            Ordering::Equal
        );
        assert_eq!(
            Version::parse("26.51")
                .unwrap()
                .compare(&Version::parse("1.20.0").unwrap()),
            Ordering::Greater
        );
    }

    #[test]
    fn refuses_malformed_versions() {
        for text in ["", "1..0", "1.", "1.x", "v", "1.0.0-rc1"] {
            assert!(Version::parse(text).is_err(), "{text} should be refused");
        }
    }

    #[test]
    fn min_client_version_gate_is_fail_closed() {
        assert!(check_min_client_version("1.0.0", "1.26.60.28").is_ok());
        assert!(check_min_client_version("1.26.60.28", "1.26.60.28").is_ok());
        assert!(check_min_client_version("1.27.0", "1.26.60.28").is_err());
        // A malformed requirement is a refusal, not a pass.
        assert!(check_min_client_version("banana", "1.26.60.28").is_err());
    }

    #[test]
    fn parses_a_release_name_as_its_digit_runs() {
        assert_eq!(
            parse_release("BETA6").unwrap(),
            Version::parse("6").unwrap()
        );
        assert_eq!(parse_release("OV.2").unwrap(), Version::parse("2").unwrap());
        // A dotted version is itself a valid release name, so the two parsers agree on it.
        assert_eq!(
            parse_release("1.26.60.28").unwrap(),
            Version::parse("1.26.60.28").unwrap()
        );
        // Surrounding whitespace is not part of the number.
        assert_eq!(
            parse_release("  BETA6 ").unwrap(),
            Version::parse("6").unwrap()
        );
    }

    #[test]
    fn refuses_a_release_name_with_no_number() {
        for text in ["", "BETA", "  ", "rc"] {
            assert!(parse_release(text).is_err(), "{text} should be refused");
        }
    }

    #[test]
    fn the_gate_uses_the_running_release_name_not_the_crate_version() {
        // The crate version is 1.0.0, which would pass every requirement up to 1.0.0. Once the real
        // running version (BETA6) is supplied, a package requiring a newer client is refused.
        assert!(check_min_client_version("1.0.0", "BETA6").is_ok());
        assert!(check_min_client_version("6", "BETA6").is_ok());
        assert!(check_min_client_version("7", "BETA6").is_err());
        // An unreadable running version is a refusal, never a silent pass.
        assert!(check_min_client_version("1.0.0", "BETA").is_err());
    }
}
