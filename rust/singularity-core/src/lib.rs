//! `singularity-core` — the pure logic behind the `.singularity` world-gen and physics engine.
//!
//! The engine replaces Bedrock's world generation, fluid flow and entity physics. It is built in
//! the same shape as `universe-core`: the archive/manifest/version/signature rules are shared from
//! `universe-core`, and this crate adds the world-generation decisions — noise, biome placement,
//! deterministic structure placement — as pure functions tested on the host.

pub mod biome;
pub mod chunk;
pub mod fluid;
pub mod manifest;
pub mod mesher;
pub mod noise;
pub mod physics;
pub mod structure;

pub use universe_core::Error;
pub use universe_core::Result;

/// Hashes a world seed string to a 64-bit seed, using one of the manifest's seed algorithms.
///
/// The hash is what every client folds the seed through, so two players who type the same seed
/// generate the same terrain. `use_sha256` selects SHA-256 (folded by XOR of its 64-bit words) over
/// the cheaper FNV-1a.
pub fn seed_hash(seed_text: &str, use_sha256: bool) -> u64 {
    if use_sha256 {
        use sha2::{Digest, Sha256};
        let digest = Sha256::digest(seed_text.as_bytes());
        let mut folded: u64 = 0;
        for chunk in digest.chunks_exact(8) {
            folded ^= u64::from_be_bytes(chunk.try_into().unwrap());
        }
        folded
    } else {
        let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
        for byte in seed_text.as_bytes() {
            hash ^= *byte as u64;
            hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
        }
        hash
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn seed_hashes_are_deterministic() {
        assert_eq!(seed_hash("hello", true), seed_hash("hello", true));
        assert_eq!(seed_hash("hello", false), seed_hash("hello", false));
        assert_ne!(seed_hash("hello", true), seed_hash("world", true));
    }

    #[test]
    fn the_two_algorithms_differ() {
        assert_ne!(seed_hash("hello", true), seed_hash("hello", false));
    }
}
