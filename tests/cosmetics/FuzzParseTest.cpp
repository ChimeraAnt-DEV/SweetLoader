// Fuzz-shaped host tests for the parsers that read untrusted data (item 458).
//
// The signature parser reads the shipped rule file and the geometry parser reads a model downloaded
// with a cosmetic. Both are reachable with data this project does not control, so a crash in one is
// a crash the launcher did not earn. The .universe manifest reader is covered by its own test,
// UniversePackageTest, against the same invariants.
//
// This is not libFuzzer: it is a deterministic, seeded generator over the *shapes* those parsers
// accept, run to a fixed iteration count so it can live in CI next to the other host tests. The
// assertions are the invariants a parser must hold on any input at all - it returns, it does not
// read out of bounds, and a pattern it accepts is internally consistent. A real libFuzzer target
// can wrap the same entry points later; the point here is that a regression fails the build today.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pl/cosmetics/geometry/BedrockGeometry.hpp"
#include "pl/memory/SignaturePattern.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string &what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  }
}

/// A tiny deterministic PRNG (xorshift64*), so a failure is reproducible from the seed alone.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

  std::uint32_t next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return static_cast<std::uint32_t>((state_ * 0x2545F4914F6CDD1Dull) >> 32);
  }

  std::uint32_t below(std::uint32_t bound) { return bound == 0 ? 0 : next() % bound; }

 private:
  std::uint64_t state_;
};

/// A short random string drawn from an alphabet that includes the parsers' own syntax characters,
/// so generated input lands on their tokenisers rather than on English text.
std::string RandomText(Rng &rng, std::size_t maxLength) {
  static const char kAlphabet[] =
      "0123456789abcdefABCDEF? \t\n{}[]\":,.-_/\\<>&%$#@!*()=+;'`~^|";
  const std::size_t length = rng.below(static_cast<std::uint32_t>(maxLength + 1));
  std::string out;
  out.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    out.push_back(kAlphabet[rng.below(sizeof(kAlphabet) - 1)]);
  }
  return out;
}

void TheSignatureParserHoldsItsInvariantsOnAnyInput() {
  Rng rng(0x5A17A17Aull);
  for (int i = 0; i < 20000; ++i) {
    const std::string text = RandomText(rng, 40);
    const pl::memory::SignaturePattern pattern = pl::memory::ParseSignaturePattern(text);
    if (!pattern.valid()) {
      Check(pattern.bytes.empty() && pattern.checkIndices.empty(),
            "a rejected pattern leaves nothing behind: '" + text + "'");
      continue;
    }
    // An accepted pattern must be internally consistent: the anchor is inside the bytes, its size
    // is at least one, and every recorded check index is addressable.
    Check(pattern.anchorSize >= 1, "an accepted pattern has a non-empty anchor");
    Check(pattern.anchorIndex + pattern.anchorSize <= pattern.bytes.size(),
          "the anchor fits inside the pattern: '" + text + "'");
    for (const std::size_t index : pattern.checkIndices) {
      Check(index < pattern.bytes.size(), "every check index is inside the pattern");
      Check(pattern.bytes[index].mask != 0, "a check index carries mask bits");
    }
  }
}

void TheGeometryParserHoldsItsInvariantsOnAnyInput() {
  Rng rng(0xB0D1E5ull);
  for (int i = 0; i < 8000; ++i) {
    const std::string text = RandomText(rng, 160);
    pl::cosmetics::geometry::BedrockGeometry geometry;
    std::string error;
    // A malformed model must return false and never throw; the parser's contract is fail-closed.
    const bool ok =
        pl::cosmetics::geometry::BedrockGeometryParser::parse(text, geometry, &error);
    if (!ok) {
      Check(!error.empty(), "a refused model explains why: '" + text + "'");
    }
  }

  // A structurally plausible but semantically wrong document: the bones array holds junk.
  static const char *kShaped[] = {
      "{\"format_version\":\"1.12.0\"}",
      "{\"format_version\":\"1.12.0\",\"minecraft:geometry\":[]}",
      "{\"format_version\":\"1.12.0\",\"minecraft:geometry\":[{}]}",
      "{\"format_version\":\"1.12.0\",\"minecraft:geometry\":[{\"description\":{},"
      "\"bones\":[{\"name\":\"\",\"cubes\":[{\"origin\":[0],\"size\":[1,2]}]}]}]}",
      "{\"format_version\":\"1.21.0\",\"minecraft:geometry\":[{\"description\":"
      "{\"texture_width\":0,\"texture_height\":0},\"bones\":[{\"name\":\"b\",\"parent\":\"b\"}]}]}",
  };
  for (const char *seed : kShaped) {
    pl::cosmetics::geometry::BedrockGeometry geometry;
    std::string error;
    const bool ok =
        pl::cosmetics::geometry::BedrockGeometryParser::parse(seed, geometry, &error);
    Check(ok || !error.empty(), std::string("a shaped model is accepted or explained: ") + seed);
    // Building a mesh from whatever parsed must also not fault: the builder is handed the result
    // whether or not it is empty.
    const auto meshes = pl::cosmetics::geometry::GeometryMeshBuilder::build(geometry);
    (void)meshes;
  }
}

}  // namespace

int main() {
  TheSignatureParserHoldsItsInvariantsOnAnyInput();
  TheGeometryParserHoldsItsInvariantsOnAnyInput();

  if (g_failures != 0) {
    std::printf("FuzzParseTest: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("FuzzParseTest: all %d checks passed\n", g_checks);
  return 0;
}
