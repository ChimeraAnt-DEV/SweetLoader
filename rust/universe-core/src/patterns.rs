//! Byte-pattern scanning with wildcards, for finding a function inside a stripped library.
//!
//! A signature is a byte string where one or more positions are unknown (`??`). The game is
//! stripped, so the only way to a function is a run of bytes around it that survived the build; a
//! signature therefore has to tolerate the bytes a minor update may change, which is what the
//! wildcards are for.
//!
//! This is a pure scanner over a byte slice. It does not read process memory, open a library or
//! install a hook: the caller owns the bytes, which is what makes the scan testable on the build
//! host and keeps the unsafe part of the loader confined to the JNI shell.
//!
//! The search is Boyer-Moore-Horspool. The classic skip table is built from the needle's last byte,
//! but a wildcard matches anything, so the table is built from the last *concrete* byte instead —
//! the shift is only as large as the nearest fixed position guarantees, which is what keeps the
//! scan linear on ordinary signatures without ever skipping a real match.

use crate::{Error, Result};

/// One position in a signature: a known byte, or "any byte".
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Byte {
    /// This byte must match exactly.
    Exact(u8),
    /// Any byte matches.
    Wildcard,
}

impl Byte {
    #[inline]
    fn matches(self, other: u8) -> bool {
        match self {
            Byte::Exact(value) => value == other,
            Byte::Wildcard => true,
        }
    }
}

/// A compiled signature.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Pattern {
    bytes: Vec<Byte>,
    /// The skip table, keyed by byte value, built from the last concrete position.
    skip: [usize; 256],
}

impl Pattern {
    /// Compiles a signature from bytes, where `None` is a wildcard.
    ///
    /// An empty signature is refused: it would match at every offset, which is never what a caller
    /// meant and would make the first hit meaningless.
    pub fn from_bytes(bytes: &[Option<u8>]) -> Result<Self> {
        if bytes.is_empty() {
            return Err(Error::new("a signature must have at least one byte"));
        }
        let compiled: Vec<Byte> = bytes
            .iter()
            .map(|byte| match byte {
                Some(value) => Byte::Exact(*value),
                None => Byte::Wildcard,
            })
            .collect();
        Ok(Self::compile(compiled))
    }

    /// Parses the `"48 8B ?? E5"` form: hex byte pairs separated by whitespace, `??` or `?` for a
    /// wildcard. Upper and lower case hex are both accepted, because signatures are copied out of
    /// disassemblers that disagree.
    pub fn parse(text: &str) -> Result<Self> {
        let mut bytes = Vec::new();
        for token in text.split_whitespace() {
            if token == "?" || token == "??" {
                bytes.push(None);
                continue;
            }
            if token.len() != 2 {
                return Err(Error::new(format!(
                    "signature token {token:?} is not a byte or a wildcard"
                )));
            }
            bytes.push(Some(u8::from_str_radix(token, 16).map_err(|_| {
                Error::new(format!("signature token {token:?} is not hex"))
            })?));
        }
        Self::from_bytes(&bytes)
    }

    fn compile(bytes: Vec<Byte>) -> Self {
        let len = bytes.len();
        let mut skip = [len; 256];
        // The shift for a byte is bounded by the distance from the last concrete position that does
        // not equal it. Wildcards past that position never constrain the shift.
        let mut last_concrete = None;
        for (index, byte) in bytes.iter().enumerate() {
            if let Byte::Exact(value) = byte {
                last_concrete = Some((index, *value));
            }
        }
        if let Some((position, value)) = last_concrete {
            let distance = len - 1 - position;
            for candidate in 0..256usize {
                if candidate as u8 != value {
                    skip[candidate] = distance.max(1);
                }
            }
            // Bytes between the last concrete position and the end shift by the distance to it.
            for index in position..len - 1 {
                if let Byte::Exact(value) = bytes[index] {
                    let distance = len - 1 - index;
                    if skip[value as usize] > distance {
                        skip[value as usize] = distance;
                    }
                }
            }
        }
        Pattern { bytes, skip }
    }

    /// The signature length in bytes.
    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    /// Whether the signature is empty. Always false for a compiled pattern.
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// Whether `haystack` matches at `offset`.
    pub fn matches_at(&self, haystack: &[u8], offset: usize) -> bool {
        if offset + self.bytes.len() > haystack.len() {
            return false;
        }
        self.bytes
            .iter()
            .enumerate()
            .all(|(index, byte)| byte.matches(haystack[offset + index]))
    }

    /// The first offset in `haystack` where the signature matches, or `None`.
    pub fn find(&self, haystack: &[u8]) -> Option<usize> {
        self.find_from(haystack, 0)
    }

    /// The first offset at or after `start` where the signature matches, or `None`.
    pub fn find_from(&self, haystack: &[u8], start: usize) -> Option<usize> {
        let len = self.bytes.len();
        if len == 0 || haystack.len() < len {
            return None;
        }
        let last = len - 1;
        let mut offset = start;
        while offset + len <= haystack.len() {
            // Compare from the last byte backwards, so a mismatch on the tail (the common case)
            // exits immediately.
            let mut index = last;
            loop {
                if self.bytes[index].matches(haystack[offset + index]) {
                    if index == 0 {
                        return Some(offset);
                    }
                    index -= 1;
                } else {
                    break;
                }
            }
            let skip = self.skip[haystack[offset + last] as usize];
            offset += skip.max(1);
        }
        None
    }

    /// Every offset where the signature matches, in ascending order.
    pub fn find_all(&self, haystack: &[u8]) -> Vec<usize> {
        let mut hits = Vec::new();
        let mut cursor = 0;
        while let Some(offset) = self.find_from(haystack, cursor) {
            hits.push(offset);
            cursor = offset + 1;
        }
        hits
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_concrete_signature_finds_its_offset() {
        let pattern = Pattern::parse("de ad be ef").unwrap();
        let haystack = [0x00, 0x11, 0xde, 0xad, 0xbe, 0xef, 0x22];
        assert_eq!(pattern.find(&haystack), Some(2));
    }

    #[test]
    fn a_wildcard_matches_any_byte() {
        let pattern = Pattern::parse("de ?? be").unwrap();
        assert_eq!(pattern.find(&[0xde, 0x00, 0xbe]), Some(0));
        assert_eq!(pattern.find(&[0xde, 0xff, 0xbe]), Some(0));
        assert_eq!(pattern.find(&[0xde, 0xff, 0xbf]), None);
    }

    #[test]
    fn a_leading_wildcard_matches_at_zero() {
        let pattern = Pattern::parse("?? ff").unwrap();
        assert_eq!(pattern.find(&[0x01, 0xff, 0x02]), Some(0));
    }

    #[test]
    fn a_trailing_wildcard_still_requires_the_prefix() {
        let pattern = Pattern::parse("ff ??").unwrap();
        assert_eq!(pattern.find(&[0x00, 0xff, 0x99]), Some(1));
        assert_eq!(pattern.find(&[0x00, 0xfe, 0x99]), None);
    }

    #[test]
    fn every_offset_is_found() {
        let pattern = Pattern::parse("aa").unwrap();
        assert_eq!(pattern.find_all(&[0xaa, 0xbb, 0xaa, 0xaa]), vec![0, 2, 3]);
    }

    #[test]
    fn a_pattern_longer_than_the_haystack_does_not_match() {
        let pattern = Pattern::parse("01 02 03").unwrap();
        assert_eq!(pattern.find(&[0x01, 0x02]), None);
    }

    #[test]
    fn an_empty_signature_is_refused() {
        assert!(Pattern::from_bytes(&[]).is_err());
        assert!(Pattern::parse("").is_err());
    }

    #[test]
    fn a_malformed_signature_is_refused() {
        assert!(Pattern::parse("1").is_err());
        assert!(Pattern::parse("zz").is_err());
        assert!(Pattern::parse("1234").is_err());
    }

    #[test]
    fn all_wildcards_match_at_zero() {
        let pattern = Pattern::parse("?? ?? ?? ?? ?? ?? ?? ??").unwrap();
        assert_eq!(pattern.find(&[1, 2, 3, 4, 5, 6, 7, 8]), Some(0));
        assert_eq!(pattern.find(&[1, 2, 3]), None);
    }

    #[test]
    fn a_match_at_the_very_end_is_found() {
        let pattern = Pattern::parse("be ef").unwrap();
        assert_eq!(pattern.find(&[0x00, 0xbe, 0xef]), Some(1));
    }

    #[test]
    fn find_from_skips_earlier_hits() {
        let pattern = Pattern::parse("aa").unwrap();
        let haystack = [0xaa, 0xaa, 0xaa];
        assert_eq!(pattern.find_from(&haystack, 0), Some(0));
        assert_eq!(pattern.find_from(&haystack, 1), Some(1));
        assert_eq!(pattern.find_from(&haystack, 3), None);
    }

    #[test]
    fn matches_at_checks_a_single_offset() {
        let pattern = Pattern::parse("aa bb").unwrap();
        let haystack = [0xaa, 0xbb, 0xaa];
        assert!(pattern.matches_at(&haystack, 0));
        assert!(!pattern.matches_at(&haystack, 1));
        assert!(
            !pattern.matches_at(&haystack, 2),
            "a truncated tail must not match"
        );
    }

    #[test]
    fn the_scanner_agrees_with_a_naive_scan() {
        // The whole point of the skip table is that it is only ever an optimisation; a disagreement
        // here is a wrong hook address. Cross-check against the obvious implementation on a haystack
        // with plenty of near-misses.
        let pattern = Pattern::parse("7f ?? 01 00").unwrap();
        let mut haystack = Vec::new();
        for index in 0..512u32 {
            haystack.push((index % 7) as u8);
            if index % 5 == 0 {
                haystack.push(0x7f);
            }
            haystack.push((index % 3) as u8);
            haystack.push(0x01);
            haystack.push(0x00);
        }
        let expected: Vec<usize> = (0..haystack.len())
            .filter(|offset| pattern.matches_at(&haystack, *offset))
            .collect();
        assert_eq!(pattern.find_all(&haystack), expected);
        assert!(!expected.is_empty(), "the fixture must contain matches");
    }

    #[test]
    fn a_signature_with_no_concrete_byte_is_still_bounded() {
        let pattern = Pattern::parse("?? ?? ?? ?? ?? ?? ?? ??").unwrap();
        // Every offset shifts by one, so the scan terminates and finds the first offset.
        assert_eq!(pattern.find(&[1, 2, 3, 4, 5, 6, 7, 8, 9]), Some(0));
    }
}
