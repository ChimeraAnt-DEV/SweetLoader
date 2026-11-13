//! `universe-core` — the pure logic behind CaramelClient's `.universe` mod format.
//!
//! Nothing in this crate touches Android, the game process, or the filesystem beyond the paths it
//! is handed. That is deliberate: the loader runs inside `libminecraftpe.so`, where a mistake is a
//! crash the user sees, so every decision it makes — does this manifest parse, is this package
//! signed by a trusted key, does this entry stay inside the sandbox, what order do these mods load
//! in — is a pure function that is unit-tested on the build host.
//!
//! The JNI cdylib (`sweetloader-jni`) is a thin shell over these functions; it adds logging, a
//! panic hook and the JNI signatures, and no policy of its own.

pub mod archive;
pub mod config;
pub mod fault;
pub mod loadorder;
pub mod manifest;
pub mod patterns;
pub mod permission;
pub mod report;
pub mod runtime;
pub mod signature;
pub mod version;

use std::fmt;

/// The error type shared by every fallible function in the crate.
///
/// A `String` payload rather than a struct-of-fields: the loader's whole job is to produce a
/// *reason* a package was refused, and that reason is logged and shown to the user. Keeping it a
/// plain message means no failure can lose the explanation on its way out.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error(pub String);

impl Error {
    pub fn new(message: impl Into<String>) -> Self {
        Error(message.into())
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

impl From<std::io::Error> for Error {
    fn from(value: std::io::Error) -> Self {
        Error(value.to_string())
    }
}

impl From<serde_json::Error> for Error {
    fn from(value: serde_json::Error) -> Self {
        Error(value.to_string())
    }
}

/// The crate's result alias.
pub type Result<T> = std::result::Result<T, Error>;

/// The ABI string a package must declare to load into this client.
///
/// Only `arm64-v8a` is supported: the client ships a single arm64 APK and the vendored hook backend
/// is arm64-only, so a 32-bit package is refused rather than loaded and faulting.
pub const SUPPORTED_ABI: &str = "arm64-v8a";

/// Encodes bytes as lower-case hex.
pub fn to_hex(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        out.push(char::from_digit((byte >> 4) as u32, 16).unwrap());
        out.push(char::from_digit((byte & 0x0f) as u32, 16).unwrap());
    }
    out
}

/// The SHA-256 of `bytes`, as lower-case hex. The packaging tool prints this; the loader verifies
/// the signature over the same digest.
pub fn sha256_hex(bytes: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    to_hex(&Sha256::digest(bytes))
}

/// Decodes lower-case or upper-case hex, refusing an odd length or a non-hex digit.
pub fn from_hex(text: &str) -> Result<Vec<u8>> {
    let bytes = text.as_bytes();
    if bytes.len() % 2 != 0 {
        return Err(Error::new("hex text has an odd number of digits"));
    }
    let mut out = Vec::with_capacity(bytes.len() / 2);
    let mut index = 0;
    while index < bytes.len() {
        let high = hex_digit(bytes[index])?;
        let low = hex_digit(bytes[index + 1])?;
        out.push((high << 4) | low);
        index += 2;
    }
    Ok(out)
}

fn hex_digit(byte: u8) -> Result<u8> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        b'A'..=b'F' => Ok(byte - b'A' + 10),
        _ => Err(Error::new("hex text contains a non-hex digit")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_round_trips() {
        let bytes = [0x00u8, 0x0f, 0xa5, 0xff];
        assert_eq!(to_hex(&bytes), "000fa5ff");
        assert_eq!(from_hex("000fa5ff").unwrap(), bytes);
        assert_eq!(from_hex("000FA5FF").unwrap(), bytes);
    }

    #[test]
    fn hex_refuses_malformed_text() {
        assert!(from_hex("abc").is_err());
        assert!(from_hex("zz").is_err());
    }
}
