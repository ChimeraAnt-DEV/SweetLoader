/**
 * @file ModManifestValidationTest.cpp
 * @brief Host test for the pure native-mod manifest validation.
 *
 * A manifest that is wrong must be refused with the field named, so an author can fix it: the whole
 * point of the validation is the *reason*, not the boolean. The rules are pure, so they run on the
 * host; the loader only forwards the JSON's declared fields here.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "pl/internal/ModManifestValidation.hpp"

using namespace pl::internal::mod;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

void expectEq(const std::string &actual, const std::string &expected, const char *what) {
  ++checks;
  if (actual != expected) {
    std::fprintf(stderr, "FAIL: %s (got '%s', expected '%s')\n", what, actual.c_str(),
                 expected.c_str());
    std::abort();
  }
}

ModManifestFields good() {
  ModManifestFields fields;
  fields.id = "example";
  fields.name = "Example";
  fields.version = "1.2.3";
  fields.abi = "arm64-v8a";
  fields.gameVersionRange = "1.21.132-1.26.60.30";
  fields.entryFileName = "libexample.so";
  return fields;
}

void versionShapes() {
  expect(IsSemanticVersion("1"), "a single component is a version");
  expect(IsSemanticVersion("1.0"), "two components are a version");
  expect(IsSemanticVersion("1.0.0.1"), "four components are a version");
  expect(IsSemanticVersion("v1.2.3"), "a leading v is accepted");
  expect(!IsSemanticVersion(""), "an empty value is not a version");
  expect(!IsSemanticVersion("1.x"), "a non-digit component is not a version");
  expect(!IsSemanticVersion("1..0"), "an empty component is not a version");
  expect(!IsSemanticVersion("1.0."), "a trailing dot is not a version");
  expect(!IsSemanticVersion(".1"), "a leading dot is not a version");
  expect(!IsSemanticVersion("one.two"), "words are not a version");
}

void comparisons() {
  expect(CompareVersions("1.20", "1.20.0.0") == 0, "missing components read as zero");
  expect(CompareVersions("26.51", "1.20.0") > 0, "a larger major wins");
  expect(CompareVersions("1.21.132", "1.21.130") > 0, "a larger patch wins");
  expect(CompareVersions("1.20.0", "1.21.0") < 0, "a smaller minor loses");
  expect(CompareVersions("bad", "1.0") == 0, "a malformed value compares equal, never crashes");
}

void ranges() {
  expect(IsVersionInRange("1.21.132", ""), "an empty range accepts everything");
  expect(IsVersionInRange("1.21.132", "1.21.132"), "an exact range matches itself");
  expect(!IsVersionInRange("1.21.133", "1.21.132"), "an exact range rejects a different version");
  expect(IsVersionInRange("1.24.0", "1.21.132-1.26.60.30"), "a version inside the range matches");
  expect(IsVersionInRange("1.21.132", "1.21.132-1.26.60.30"), "the lower bound is inclusive");
  expect(IsVersionInRange("1.26.60.30", "1.21.132-1.26.60.30"), "the upper bound is inclusive");
  expect(!IsVersionInRange("1.20.0", "1.21.132-1.26.60.30"), "below the lower bound is refused");
  expect(!IsVersionInRange("1.27.0", "1.21.132-1.26.60.30"), "above the upper bound is refused");
}

void rangeShapes() {
  expect(ValidateGameVersionRange("").ok, "an empty range is valid (unconstrained)");
  expect(ValidateGameVersionRange("1.21.132").ok, "a single version is a valid range");
  expect(ValidateGameVersionRange("1.21.132-1.26.60.30").ok, "a well-formed range is valid");
  expect(!ValidateGameVersionRange("1.21.132-").ok, "a missing upper bound is refused");
  expect(!ValidateGameVersionRange("-1.26.60.30").ok, "a missing lower bound is refused");
  expect(!ValidateGameVersionRange("abc-1.26").ok, "a non-version bound is refused");
  expect(!ValidateGameVersionRange("1.26-1.21").ok, "min above max is refused");

  const ModManifestValidation failure = ValidateGameVersionRange("1.21.132-");
  expect(failure.field == ModManifestField::GameVersionRange, "the failure names the field");
  expectEq(std::string(ModManifestFieldName(failure.field)), "game_version",
           "the field name is stable");
}

void validManifest() {
  expect(ValidateModManifest(good()).ok, "a complete manifest is valid");
  expect(ValidateModManifest(good()).field == ModManifestField::None,
         "a valid manifest names no failing field");
}

void missingId() {
  ModManifestFields fields = good();
  fields.id = "   ";
  const ModManifestValidation failure = ValidateModManifest(fields);
  expect(!failure.ok, "a blank id is refused");
  expect(failure.field == ModManifestField::Id, "the failure names the id field");
}

void missingVersion() {
  ModManifestFields fields = good();
  fields.version.clear();
  expect(ValidateModManifest(fields).field == ModManifestField::Version,
         "a missing version names the version field");
}

void malformedVersion() {
  ModManifestFields fields = good();
  fields.version = "1.0-beta";
  const ModManifestValidation failure = ValidateModManifest(fields);
  expect(!failure.ok, "a pre-release suffix is not a plain semantic version");
  expect(failure.reason.find("semantic") != std::string::npos,
         "the reason explains what a version must be");
}

void entryMustBeSharedLibrary() {
  ModManifestFields fields = good();
  fields.entryFileName.clear();
  expect(ValidateModManifest(fields).field == ModManifestField::Entry,
         "a missing entry names the entry field");

  fields = good();
  fields.entryFileName = "mod.lua";
  expect(ValidateModManifest(fields).field == ModManifestField::Entry,
         "a non-.so entry names the entry field");

  fields = good();
  fields.entryFileName = "libmod.so";
  expect(ValidateModManifest(fields).ok, "a .so entry is accepted");
}

void abiMustMatchTheShippedBuild() {
  ModManifestFields fields = good();
  fields.abi = "armeabi-v7a";
  const ModManifestValidation failure = ValidateModManifest(fields);
  expect(!failure.ok, "a 32-bit abi is refused");
  expect(failure.field == ModManifestField::Abi, "the failure names the abi field");

  fields = good();
  fields.abi.clear();
  expect(ValidateModManifest(fields).ok, "an unspecified abi is accepted (defaults to arm64)");

  fields = good();
  fields.abi = "arm64-v8a";
  expect(ValidateModManifest(fields).ok, "the shipped abi is accepted");
}

void orderReportsTheMostActionableProblemFirst() {
  ModManifestFields fields = good();
  fields.id.clear();
  fields.version = "nonsense";
  fields.entryFileName = "mod.lua";
  expect(ValidateModManifest(fields).field == ModManifestField::Id,
         "identity is reported before the version and the entry");
}

void fieldNamesAreStable() {
  expectEq(std::string(ModManifestFieldName(ModManifestField::Id)), "id", "id name");
  expectEq(std::string(ModManifestFieldName(ModManifestField::Version)), "version", "version name");
  expectEq(std::string(ModManifestFieldName(ModManifestField::Abi)), "abi", "abi name");
  expectEq(std::string(ModManifestFieldName(ModManifestField::Entry)), "entry", "entry name");
  expectEq(std::string(ModManifestFieldName(ModManifestField::Dependencies)), "dependencies",
           "dependencies name");
}

} // namespace

int main() {
  versionShapes();
  comparisons();
  ranges();
  rangeShapes();
  validManifest();
  missingId();
  missingVersion();
  malformedVersion();
  entryMustBeSharedLibrary();
  abiMustMatchTheShippedBuild();
  orderReportsTheMostActionableProblemFirst();
  fieldNamesAreStable();
  std::printf("ModManifestValidationTest: %d checks passed\n", checks);
  return 0;
}
