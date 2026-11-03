// Host test for the pure signature-pattern parser and anchor selection.
//
// The parser is what decides which bytes a scan searches for and which it checks at a hit, so a
// defect here is a signature that resolves to the wrong address or never resolves at all - and it is
// invisible until a device scans a real module. Exercised here with the shapes the shipped rules
// actually use, including the ones that used to have no coverage: a wildcard at either end, a fully
// wild pattern, an empty pattern, a non-hex token and an odd-length token.

#include "pl/memory/SignaturePattern.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

using pl::memory::FindAllPatternMatches;
using pl::memory::IsExactByte;
using pl::memory::PatternMatchesAt;
using pl::memory::kMaxExactAnchorSize;
using pl::memory::PatternByte;
using pl::memory::PatternMatches;
using pl::memory::ParseSignaturePattern;
using pl::memory::SelectAnchor;
using pl::memory::SignaturePattern;

static int g_failures = 0;

static void check(bool ok, const std::string &what) {
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", what.c_str());
  }
}

int main() {
  // --- Exact bytes --------------------------------------------------------------------------
  {
    const SignaturePattern p = ParseSignaturePattern("48 8b c0");
    check(p.valid(), "a plain signature parses");
    check(p.bytes.size() == 3, "three tokens produce three bytes");
    check(IsExactByte(p.bytes[0]) && p.bytes[0].value == 0x48, "48 is exact");
    check(IsExactByte(p.bytes[2]) && p.bytes[2].value == 0xC0, "c0 is exact");
    // `checkIndices` records every byte with a nonzero mask - that is, every byte that compares
    // anything at all. A fully wild byte is absent from it; the anchor covers the rest implicitly.
    check(p.checkIndices.size() == 3, "every exact byte is recorded as a checkable index");
  }

  // A run of hex with no spaces is split into byte pairs, the shape `tools/gen-rules` emits.
  {
    const SignaturePattern p = ParseSignaturePattern("488bc0");
    check(p.bytes.size() == 3, "a compact hex run splits into pairs");
    check(p.bytes[1].value == 0x8B, "the middle byte is 8b");
  }

  // --- Wildcards ----------------------------------------------------------------------------
  {
    const SignaturePattern p = ParseSignaturePattern("4? ?8");
    check(p.bytes.size() == 2, "two tokens produce two bytes");
    check(!IsExactByte(p.bytes[0]) && p.bytes[0].mask == 0xF0, "4? keeps the high nibble only");
    check(p.bytes[0].value == 0x40, "4? compares against 0x4_");
    check(!IsExactByte(p.bytes[1]) && p.bytes[1].mask == 0x0F, "?8 keeps the low nibble only");
    check(p.checkIndices.size() == 2, "both masked bytes are checked");
    check(PatternMatches(p.bytes[0], 0x4A) && !PatternMatches(p.bytes[0], 0x5A),
          "4? matches only the 0x4_ nibble");
    check(PatternMatches(p.bytes[1], 0xA8) && !PatternMatches(p.bytes[1], 0xA9),
          "?8 matches only the _8 nibble");
  }

  // A fully wild token matches anything, at either spelling.
  {
    const SignaturePattern q = ParseSignaturePattern("?");
    check(q.valid() && q.bytes.size() == 1, "a lone ? is one wild byte");
    check(q.bytes[0].mask == 0 && q.bytes[0].value == 0, "? has no mask bits");
    check(PatternMatches(q.bytes[0], 0x00) && PatternMatches(q.bytes[0], 0xFF),
          "? matches every value");

    const SignaturePattern qq = ParseSignaturePattern("??");
    check(qq.bytes.size() == 1 && qq.bytes[0].mask == 0, "?? is one wild byte");
  }

  // A wildcard at the start or the end is legal: the anchor must come from the exact run.
  {
    SignaturePattern p = ParseSignaturePattern("? ? 48 8b c0 ? ?");
    check(p.valid() && p.bytes.size() == 7, "wildcards at both ends parse");
    SelectAnchor(p);
    check(p.anchorIndex == 2 && p.anchorSize == 3,
          "the anchor is the interior exact run, not a wild byte (" + std::to_string(p.anchorIndex)
              + "/" + std::to_string(p.anchorSize) + ")");
  }

  // --- Anchor selection ---------------------------------------------------------------------
  {
    // One unbroken exact run: the whole run is the anchor.
    SignaturePattern p = ParseSignaturePattern("aa bb 11 22");
    SelectAnchor(p);
    check(p.anchorIndex == 0 && p.anchorSize == 4, "a single exact run is the anchor");

    // Two exact runs separated by a wildcard: the longer one wins.
    SignaturePattern q = ParseSignaturePattern("aa bb ? 11 22 33 44");
    SelectAnchor(q);
    check(q.anchorIndex == 3 && q.anchorSize == 4, "the longer of two exact runs is the anchor");

    // Equal-length runs: the later one wins, so the anchor sits as late as possible.
    SignaturePattern s = ParseSignaturePattern("aa bb ? 11 22");
    SelectAnchor(s);
    check(s.anchorIndex == 3 && s.anchorSize == 2, "the later of two equal runs wins");

    // An exact run longer than the cap is trimmed to its tail.
    SignaturePattern r = ParseSignaturePattern("01 02 03 04 05 06 07 08 09 0a");
    SelectAnchor(r);
    check(r.anchorSize == kMaxExactAnchorSize, "an over-long exact run is capped");
    check(r.anchorIndex == 2, "the capped anchor is the last kMaxExactAnchorSize bytes");
  }

  // No exact byte at all: the byte with the most mask bits wins, and the later one on a tie.
  {
    SignaturePattern p = ParseSignaturePattern("? 4? 8?");
    SelectAnchor(p);
    check(p.anchorSize == 1, "a masked-only pattern anchors on one byte");
    check(p.anchorIndex == 2, "the last equally-masked byte wins");
    check(p.bytes[p.anchorIndex].mask == 0xF0, "the anchor is a half-masked byte");
  }

  // A fully wild pattern keeps the default anchor: every position is a candidate.
  {
    SignaturePattern p = ParseSignaturePattern("? ? ?");
    SelectAnchor(p);
    check(p.anchorIndex == 0 && p.anchorSize == 1, "a fully wild pattern anchors at index 0");
  }

  // --- Matching at a position, and overlapping hits ----------------------------------------
  //
  // A scan advances one byte at a time, so two matches may overlap. That is the case the old
  // in-file matcher had no coverage for, and it is the shape a two-byte signature produces in a
  // real module. The helpers are now the scan's own definition, so these pin what the scan does.
  {
    const SignaturePattern p = ParseSignaturePattern("aa bb");
    const std::uint8_t buffer[] = {0xAA, 0xBB, 0xAA, 0xBB};
    check(PatternMatchesAt(buffer, 4, 0, p), "a match at offset 0 is found");
    check(PatternMatchesAt(buffer, 4, 2, p), "a match at offset 2 is found");
    check(!PatternMatchesAt(buffer, 4, 1, p), "a mismatch at offset 1 is rejected");
    const auto hits = FindAllPatternMatches(buffer, 4, p);
    check(hits.size() == 2 && hits[0] == 0 && hits[1] == 2,
          "both non-overlapping hits are listed in order");

    // A run of one repeated byte: `aa aa` in `aa aa aa` overlaps at offsets 0 and 1.
    const SignaturePattern run = ParseSignaturePattern("aa aa");
    const std::uint8_t repeated[] = {0xAA, 0xAA, 0xAA};
    const auto overlapping = FindAllPatternMatches(repeated, 3, run);
    check(overlapping.size() == 2 && overlapping[0] == 0 && overlapping[1] == 1,
          "overlapping hits are both reported, one byte apart");
  }

  // A candidate that would run past the end of the buffer is not a match, so a truncated read can
  // never index out of bounds.
  {
    const SignaturePattern p = ParseSignaturePattern("aa bb cc");
    const std::uint8_t buffer[] = {0xAA, 0xBB};
    check(!PatternMatchesAt(buffer, 2, 0, p), "a pattern longer than the buffer cannot match");
    check(FindAllPatternMatches(buffer, 2, p).empty(), "and yields no hits");
    check(!PatternMatchesAt(buffer, 2, 5, p), "an offset past the end is not a match");
    check(!PatternMatchesAt(nullptr, 0, 0, p), "a null buffer is not a match");
  }

  // A wildcard inside the pattern is honoured at a position, not only by the anchor.
  {
    const SignaturePattern p = ParseSignaturePattern("48 ? c0");
    const std::uint8_t yes[] = {0x48, 0x00, 0xC0};
    const std::uint8_t no[] = {0x48, 0x00, 0xC1};
    check(PatternMatchesAt(yes, 3, 0, p), "a wildcard byte matches any value");
    check(!PatternMatchesAt(no, 3, 0, p), "an exact byte after the wildcard is still checked");
  }

  // An invalid pattern matches nothing, however it is asked.
  {
    const SignaturePattern bad = ParseSignaturePattern("zz");
    const std::uint8_t buffer[] = {0x00};
    check(!PatternMatchesAt(buffer, 1, 0, bad), "an invalid pattern matches nothing");
    check(FindAllPatternMatches(buffer, 1, bad).empty(), "and enumerates nothing");
  }

  // --- Malformed input ----------------------------------------------------------------------
  {
    const SignaturePattern empty = ParseSignaturePattern("");
    check(!empty.valid(), "an empty signature is invalid, not a zero-byte pattern");

    const SignaturePattern blank = ParseSignaturePattern("   \n\t  ");
    check(!blank.valid(), "whitespace-only is invalid");

    const SignaturePattern bad = ParseSignaturePattern("48 zz c0");
    check(!bad.valid(), "a non-hex token invalidates the whole pattern");
    check(bad.checkIndices.empty(), "a rejected pattern leaves no check indices behind");

    const SignaturePattern odd = ParseSignaturePattern("488");
    check(!odd.valid(), "an odd-length hex run is rejected");

    const SignaturePattern three = ParseSignaturePattern("4?8");
    check(!three.valid(), "a three-character nibble token is rejected");

    // A valid prefix followed by a bad token must not leave a partial pattern behind.
    const SignaturePattern partial = ParseSignaturePattern("48 8b zz");
    check(!partial.valid(), "a bad token after good ones rejects the whole signature");
  }

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("SignaturePatternTest: all checks passed\n");
  return 0;
}
