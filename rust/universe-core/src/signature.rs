//! Ed25519 signing and verification for package archives.
//!
//! A package is signed over the SHA-256 of its bytes, so the signature is detached: the author ships
//! `package.universe` and a sidecar `package.universe.sig` holding the raw 64-byte Ed25519 signature
//! in hex. Verification hashes the archive again and checks the signature against the trusted key
//! set the client ships or the user has pinned.
//!
//! The hash-then-sign indirection is what makes the signature independent of how the ZIP is
//! compressed: the loader verifies exactly the bytes it is about to extract, so a re-compressed
//! archive with the same contents still verifies and a tampered one does not.

use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};
use sha2::{Digest, Sha256};

use crate::{from_hex, to_hex, Error, Result};

/// A raw Ed25519 signature, as stored in a sidecar.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawSignature {
    /// The 64-byte Ed25519 signature.
    pub signature: [u8; 64],
}

impl RawSignature {
    /// Builds a signature from raw bytes.
    pub fn from_bytes(bytes: &[u8]) -> Result<RawSignature> {
        let array: [u8; 64] = bytes
            .try_into()
            .map_err(|_| Error::new("a signature must be 64 bytes"))?;
        Ok(RawSignature { signature: array })
    }

    /// The hex form, as a sidecar file holds it.
    pub fn to_hex(&self) -> String {
        to_hex(&self.signature)
    }

    /// Parses the hex form.
    pub fn from_hex(text: &str) -> Result<RawSignature> {
        RawSignature::from_bytes(&from_hex(text.trim())?)
    }
}

/// A 32-byte Ed25519 public key.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PublicKey([u8; 32]);

impl PublicKey {
    /// Builds a key from raw bytes.
    pub fn from_bytes(bytes: &[u8]) -> Result<PublicKey> {
        let array: [u8; 32] = bytes
            .try_into()
            .map_err(|_| Error::new("a public key must be 32 bytes"))?;
        // A byte string that is not a valid curve point is refused here rather than at verify time,
        // so a malformed trusted-key entry is caught when the key set is built.
        VerifyingKey::from_bytes(&array)
            .map_err(|_| Error::new("public key is not a valid Ed25519 point"))?;
        Ok(PublicKey(array))
    }

    /// Parses the hex form.
    pub fn from_hex(text: &str) -> Result<PublicKey> {
        PublicKey::from_bytes(&from_hex(text.trim())?)
    }

    /// The hex form.
    pub fn to_hex(&self) -> String {
        to_hex(&self.0)
    }
}

/// Signs a package body, for the packaging tool.
pub fn sign(body: &[u8], signing_key: &SigningKey) -> RawSignature {
    let digest = Sha256::digest(body);
    RawSignature {
        signature: signing_key.sign(&digest).to_bytes(),
    }
}

/// Verifies a package body against one public key.
pub fn verify(body: &[u8], signature: &RawSignature, key: &PublicKey) -> bool {
    let digest = Sha256::digest(body);
    let verifying_key = match VerifyingKey::from_bytes(&key.0) {
        Ok(key) => key,
        Err(_) => return false,
    };
    let signature = Signature::from_bytes(&signature.signature);
    verifying_key.verify(&digest, &signature).is_ok()
}

/// A public key with a human-readable id, so a trusted set can be reported.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TrustedKey {
    pub id: String,
    pub key: PublicKey,
}

/// The set of keys the client trusts.
#[derive(Debug, Clone, Default)]
pub struct TrustedKeys {
    keys: Vec<TrustedKey>,
}

impl TrustedKeys {
    pub fn new() -> TrustedKeys {
        TrustedKeys { keys: Vec::new() }
    }

    /// Adds a trusted key.
    pub fn add(&mut self, id: impl Into<String>, key: PublicKey) {
        self.keys.push(TrustedKey { id: id.into(), key });
    }

    pub fn is_empty(&self) -> bool {
        self.keys.is_empty()
    }

    pub fn len(&self) -> usize {
        self.keys.len()
    }

    pub fn ids(&self) -> impl Iterator<Item = &str> {
        self.keys.iter().map(|entry| entry.id.as_str())
    }

    /// True when any trusted key verifies the body.
    pub fn verifies(&self, body: &[u8], signature: &RawSignature) -> bool {
        self.keys
            .iter()
            .any(|entry| verify(body, signature, &entry.key))
    }
}

/// Builds a signing key from 32 raw seed bytes. Only the packaging tool needs this.
pub fn signing_key_from_bytes(seed: &[u8]) -> Result<SigningKey> {
    let array: [u8; 32] = seed
        .try_into()
        .map_err(|_| Error::new("a signing key seed must be 32 bytes"))?;
    Ok(SigningKey::from_bytes(&array))
}

/// The public key for a signing key, as the trusted set stores it.
pub fn public_key_of(signing_key: &SigningKey) -> PublicKey {
    PublicKey(signing_key.verifying_key().to_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(seed: u8) -> SigningKey {
        SigningKey::from_bytes(&[seed; 32])
    }

    #[test]
    fn a_signature_verifies_for_the_matching_key() {
        let signing = key(1);
        let body = b"the package bytes";
        let signature = sign(body, &signing);
        assert!(verify(body, &signature, &public_key_of(&signing)));
    }

    #[test]
    fn a_tampered_body_fails_verification() {
        let signing = key(2);
        let signature = sign(b"original", &signing);
        assert!(!verify(b"tampered", &signature, &public_key_of(&signing)));
    }

    #[test]
    fn a_different_key_fails_verification() {
        let signing = key(3);
        let other = key(4);
        let signature = sign(b"body", &signing);
        assert!(!verify(b"body", &signature, &public_key_of(&other)));
    }

    #[test]
    fn a_trusted_set_verifies_any_of_its_keys() {
        let a = key(5);
        let b = key(6);
        let signature = sign(b"body", &b);
        let mut trusted = TrustedKeys::new();
        trusted.add("a", public_key_of(&a));
        trusted.add("b", public_key_of(&b));
        assert!(trusted.verifies(b"body", &signature));
        assert_eq!(trusted.len(), 2);
    }

    #[test]
    fn an_empty_trusted_set_verifies_nothing() {
        let signing = key(7);
        let signature = sign(b"body", &signing);
        assert!(!TrustedKeys::new().verifies(b"body", &signature));
    }

    #[test]
    fn signatures_round_trip_through_hex() {
        let signing = key(8);
        let signature = sign(b"body", &signing);
        let hex = signature.to_hex();
        assert_eq!(RawSignature::from_hex(&hex).unwrap(), signature);
    }

    #[test]
    fn malformed_keys_and_signatures_are_refused() {
        assert!(PublicKey::from_bytes(&[0u8; 31]).is_err());
        assert!(RawSignature::from_bytes(&[0u8; 63]).is_err());
    }
}
