#pragma once

/**
 * @file SignaturePattern.hpp
 * @brief Pure parsing, matching and anchor selection for byte signatures.
 *
 * A signature is a whitespace-separated list of byte tokens; a token may be exact (`48`), a nibble
 * wildcard (`4?`) or fully wild (``?`` / `??`). Scanning a module for one is the expensive half of
 * resolution, and the whole cost model rests on picking an anchor: a run of exact bytes the scan can
 * search for quickly, with the masked bytes checked only at a candidate hit.
 *
 * That logic used to live in an anonymous namespace inside `Signature.cpp`, next to the
 * `/proc/self/maps` walk and the `dlopen` fallback, so the only way to exercise it was to scan a
 * real process. It is a pure function over a string, so it lives here and is host-tested
 * (`SignaturePatternTest`) with the shapes that actually occur in the rules: wildcards at either
 * end, a fully wild pattern, an empty pattern, a token that is not hex, and an odd-length token.
 *
 * Header-only and Android-free on purpose: `Signature.cpp` and the host test compile the same code.
 */

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace pl::memory {

/** The longest run of exact bytes used as a scan anchor. */
inline constexpr std::size_t kMaxExactAnchorSize = 8;

/** One pattern byte: the value to compare and the mask of bits that matter. */
struct PatternByte {
  std::uint8_t value = 0;
  std::uint8_t mask = 0;
};

/** A compiled signature: the bytes, the indices that need checking, and the chosen anchor. */
struct SignaturePattern {
  std::vector<PatternByte> bytes;
  /** Indices whose mask is not `0xFF`; the anchor covers the rest implicitly. */
  std::vector<std::size_t> checkIndices;
  std::size_t anchorIndex = 0;
  std::size_t anchorSize = 1;

  /** False for an empty or unparseable signature, which must never be scanned. */
  [[nodiscard]] bool valid() const { return !bytes.empty(); }
};

/** The value of one hex digit, or -1 when the character is not hex. */
inline int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

/**
 * @brief Parses one token (`48`, `4?`, `?`) into a pattern byte.
 *
 * A `?` nibble leaves that nibble's mask bits clear, so only the other nibble is compared; a token
 * of exactly `?` or `??` is fully wild and matches anything.
 */
inline bool ParsePatternToken(std::string_view token, PatternByte &out) {
  if (token == "?" || token == "??") {
    out = PatternByte{0, 0};
    return true;
  }
  if (token.size() != 2) return false;

  std::uint8_t value = 0;
  std::uint8_t mask = 0;
  for (std::size_t i = 0; i < token.size(); ++i) {
    const char ch = token[i];
    const auto shift = static_cast<std::uint8_t>((1 - i) * 4);
    if (ch == '?') continue;
    const int digit = HexValue(ch);
    if (digit < 0) return false;
    value |= static_cast<std::uint8_t>(digit << shift);
    mask |= static_cast<std::uint8_t>(0xF << shift);
  }

  out = PatternByte{value, mask};
  return true;
}

/** Appends one byte, recording it in `checkIndices` when it is not fully exact. */
inline void AppendPatternByte(SignaturePattern &pattern, PatternByte byte) {
  const std::size_t index = pattern.bytes.size();
  if (byte.mask != 0) pattern.checkIndices.push_back(index);
  pattern.bytes.push_back(byte);
}

/** Appends one whitespace-delimited token, which may be a run of up to `kMaxExactAnchorSize` bytes. */
inline bool AppendPatternToken(std::string_view token, SignaturePattern &pattern) {
  PatternByte byte{};
  if (ParsePatternToken(token, byte)) {
    AppendPatternByte(pattern, byte);
    return true;
  }
  if (token.empty() || token.size() % 2 != 0) return false;

  for (std::size_t pos = 0; pos < token.size(); pos += 2) {
    if (!ParsePatternToken(token.substr(pos, 2), byte)) return false;
    AppendPatternByte(pattern, byte);
  }
  return true;
}

/**
 * @brief Parses a whole signature.
 *
 * A malformed token aborts the parse and leaves the pattern empty, so a bad rule cannot produce a
 * partial pattern that scans for something unintended. Whitespace (including newlines) separates
 * tokens.
 */
inline SignaturePattern ParseSignaturePattern(std::string_view signature) {
  SignaturePattern pattern;
  std::size_t pos = 0;
  while (pos < signature.size()) {
    while (pos < signature.size() &&
           std::isspace(static_cast<unsigned char>(signature[pos]))) {
      ++pos;
    }
    if (pos >= signature.size()) break;

    const std::size_t start = pos;
    while (pos < signature.size() &&
           !std::isspace(static_cast<unsigned char>(signature[pos]))) {
      ++pos;
    }
    if (!AppendPatternToken(signature.substr(start, pos - start), pattern)) {
      pattern.bytes.clear();
      pattern.checkIndices.clear();
      break;
    }
  }
  return pattern;
}

/** True when [value] satisfies [pattern]. */
inline bool PatternMatches(PatternByte pattern, std::uint8_t value) {
  return (value & pattern.mask) == pattern.value;
}

/**
 * @brief True when `pattern` matches `data` starting exactly at `candidateOffset`.
 *
 * The single definition of "this signature is here": the anchor run is compared first (it is what
 * the scan searched for) and then every masked byte outside the anchor. Keeping it here rather than
 * beside the `/proc` walk is what makes an overlapping-hit case - two matches at offsets one byte
 * apart - something a host test can pin, which is the shape a short signature produces in a real
 * module and the one that used to have no coverage at all.
 *
 * @param size the number of readable bytes at `data`; a candidate whose pattern would run past it
 *             is not a match, so a truncated buffer can never read out of bounds
 */
inline bool PatternMatchesAt(const std::uint8_t *data, std::size_t size,
                             std::size_t candidateOffset, const SignaturePattern &pattern) {
  if (data == nullptr || !pattern.valid()) return false;
  if (candidateOffset > size) return false;
  if (pattern.bytes.size() > size - candidateOffset) return false;

  const std::uint8_t *at = data + candidateOffset;
  for (std::size_t i = 0; i < pattern.anchorSize; ++i) {
    if (!PatternMatches(pattern.bytes[pattern.anchorIndex + i],
                        at[pattern.anchorIndex + i])) {
      return false;
    }
  }
  for (const std::size_t index : pattern.checkIndices) {
    if (index >= pattern.anchorIndex &&
        index < pattern.anchorIndex + pattern.anchorSize) {
      continue;
    }
    if (!PatternMatches(pattern.bytes[index], at[index])) return false;
  }
  return true;
}

/**
 * @brief Every offset in `data` where `pattern` matches, in ascending order.
 *
 * Consecutive matches may overlap: the scan advances one byte at a time, so `aa aa` in `aa aa aa`
 * is two hits, not one. The list is what a scan over a buffer produces, so a test can assert on
 * the whole result rather than on a single first hit.
 */
inline std::vector<std::size_t> FindAllPatternMatches(const std::uint8_t *data,
                                                      std::size_t size,
                                                      const SignaturePattern &pattern) {
  std::vector<std::size_t> hits;
  if (data == nullptr || !pattern.valid() || pattern.bytes.size() > size) return hits;
  const std::size_t last = size - pattern.bytes.size();
  for (std::size_t offset = 0; offset <= last; ++offset) {
    if (PatternMatchesAt(data, size, offset, pattern)) hits.push_back(offset);
  }
  return hits;
}

/**
 * @brief The first two offsets in `data` where `pattern` matches, in ascending order.
 *
 * A scan that takes the first hit is only correct when the pattern identifies one function. A short
 * signature - and every signature that spans an `adrp`/`add` pair, because those encode a link-time
 * address and change whenever the layout does - can match many places, and silently patching the
 * wrong one is the worst failure this code has: the hook installs, the game keeps running, and
 * something unrelated is corrupted.
 *
 * So the ambiguity check needs to answer one question - "is there a second match?" - and it must not
 * pay for a full enumeration to do it. This returns at most two offsets and stops, which is exactly
 * the decision the caller makes.
 *
 * Overlapping matches count as two: `aa aa` in `aa aa aa` is ambiguous even though the matches
 * share a byte, because the caller still has to choose an address.
 */
inline std::vector<std::size_t> FindFirstTwoPatternMatches(const std::uint8_t *data,
                                                          std::size_t size,
                                                          const SignaturePattern &pattern) {
  std::vector<std::size_t> hits;
  if (data == nullptr || !pattern.valid() || pattern.bytes.size() > size) return hits;
  const std::size_t last = size - pattern.bytes.size();
  for (std::size_t offset = 0; offset <= last; ++offset) {
    if (PatternMatchesAt(data, size, offset, pattern)) {
      hits.push_back(offset);
      if (hits.size() == 2) return hits;
    }
  }
  return hits;
}

/** True when `pattern` matches `data` in more than one place. */
inline bool PatternIsAmbiguous(const std::uint8_t *data, std::size_t size,
                               const SignaturePattern &pattern) {
  return FindFirstTwoPatternMatches(data, size, pattern).size() > 1;
}

/** True when the byte must match exactly, with no wildcard bits. */
inline bool IsExactByte(PatternByte byte) { return byte.mask == 0xFF; }

/** The number of bits [mask] compares. */
inline int MaskBits(std::uint8_t mask) {
  int count = 0;
  while (mask != 0) {
    mask &= static_cast<std::uint8_t>(mask - 1);
    ++count;
  }
  return count;
}

/**
 * @brief Chooses the bytes a scan searches for.
 *
 * Preference order: the longest run of exact bytes (capped at `kMaxExactAnchorSize`, taking the
 * last such run of the winning length so the anchor sits as late as possible in the pattern), then
 * the single byte with the most mask bits set, then the last such byte. A fully wild pattern keeps
 * the default one-byte anchor at index 0, which is what "check every position" means.
 */
inline void SelectAnchor(SignaturePattern &pattern) {
  if (pattern.checkIndices.empty()) return;

  std::size_t bestStart = 0;
  std::size_t bestSize = 0;
  for (std::size_t runStart = 0; runStart < pattern.bytes.size();) {
    if (!IsExactByte(pattern.bytes[runStart])) {
      ++runStart;
      continue;
    }

    std::size_t runEnd = runStart + 1;
    while (runEnd < pattern.bytes.size() && IsExactByte(pattern.bytes[runEnd])) {
      ++runEnd;
    }

    const std::size_t size = (std::min)(runEnd - runStart, kMaxExactAnchorSize);
    const std::size_t start = runEnd - size;
    if (size > bestSize || (size == bestSize && start > bestStart)) {
      bestStart = start;
      bestSize = size;
    }
    runStart = runEnd;
  }

  if (bestSize != 0) {
    pattern.anchorIndex = bestStart;
    pattern.anchorSize = bestSize;
    return;
  }

  std::size_t bestIndex = pattern.checkIndices.front();
  int bestBits = MaskBits(pattern.bytes[bestIndex].mask);
  for (const std::size_t index : pattern.checkIndices) {
    const int bits = MaskBits(pattern.bytes[index].mask);
    if (bits > bestBits || (bits == bestBits && index > bestIndex)) {
      bestIndex = index;
      bestBits = bits;
    }
  }
  pattern.anchorIndex = bestIndex;
  pattern.anchorSize = 1;
}

} // namespace pl::memory
