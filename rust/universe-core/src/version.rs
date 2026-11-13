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

/// Refuses a package whose `min_client_version` is newer than the running client.
///
/// A package built against a newer client may call entry points or expect manifest fields this
/// client does not have, so it is refused rather than loaded and faulting. An unparseable
/// requirement is also a refusal: "I could not read your requirement" is not a licence to proceed.
pub fn check_min_client_version(min_client_version: &str, client_version: &str) -> Result<()> {
    let required = Version::parse(min_client_version)?;
    let running = Version::parse(client_version)?;
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
}
