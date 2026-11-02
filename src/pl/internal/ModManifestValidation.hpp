#pragma once

/**
 * @file ModManifestValidation.hpp
 * @brief Pure validation of the fields a native mod manifest declares (item 445).
 *
 * <p>A mod is loaded by resolving its manifest, and a manifest that is wrong in any of these
 * fields must be refused *with the field named*: "the mod did not load" is not actionable, while
 * "version '1.0' is not a semantic version" tells the author what to fix. The rules live here, as a
 * pure header, so they are host-tested without a device and shared by the loader.
 *
 * <p>Semantic-version and version-range parsing are deliberately small: the preloader only needs to
 * decide whether a value is well formed and whether the running game version falls inside the
 * declared range, not to implement the whole semver specification.
 */

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace pl::internal::mod {

/** @brief Which manifest field a validation failure concerns, so the caller can name it. */
enum class ModManifestField {
  None,
  Id,
  Name,
  Version,
  Abi,
  GameVersionRange,
  Entry,
  Dependencies,
};

/** @brief The outcome of validating a manifest's declared fields. */
struct ModManifestValidation {
  bool ok{true};
  ModManifestField field{ModManifestField::None};
  std::string reason;
};

inline ModManifestValidation ManifestOk() { return {}; }

inline ModManifestValidation ManifestFailure(ModManifestField field, std::string reason) {
  ModManifestValidation result;
  result.ok = false;
  result.field = field;
  result.reason = std::move(reason);
  return result;
}

/** @brief The lower-case name of a field, for a log line or a diagnostics row. */
inline const char *ModManifestFieldName(ModManifestField field) {
  switch (field) {
    case ModManifestField::None:
      return "none";
    case ModManifestField::Id:
      return "id";
    case ModManifestField::Name:
      return "name";
    case ModManifestField::Version:
      return "version";
    case ModManifestField::Abi:
      return "abi";
    case ModManifestField::GameVersionRange:
      return "game_version";
    case ModManifestField::Entry:
      return "entry";
    case ModManifestField::Dependencies:
      return "dependencies";
  }
  return "unknown";
}

/** @brief True when the text is non-empty after trimming ASCII whitespace. */
inline bool IsNonBlank(std::string_view value) {
  for (char ch : value) {
    if (!std::isspace(static_cast<unsigned char>(ch))) return true;
  }
  return false;
}

/**
 * @brief True when every dot-separated component is a run of digits.
 *
 * <p>A single component ("1"), two ("1.0") and four ("1.0.0.1") are all accepted; an empty
 * component ("1..0"), a trailing dot and a non-digit ("1.x") are not.
 */
inline bool IsNumericVersion(std::string_view value) {
  if (value.empty()) return false;
  size_t components = 0;
  size_t index = 0;
  while (index < value.size()) {
    const size_t start = index;
    while (index < value.size() && value[index] != '.') {
      if (!std::isdigit(static_cast<unsigned char>(value[index]))) return false;
      ++index;
    }
    if (index == start) return false; // an empty component
    ++components;
    if (index < value.size()) ++index; // skip the dot
    else break;
    if (index == value.size()) return false; // a trailing dot
  }
  return components > 0;
}

/**
 * @brief True when the value is a dotted numeric version with an optional leading 'v'.
 *
 * <p>Only the shape is checked here; comparison is a separate concern so the two cannot be confused.
 */
inline bool IsSemanticVersion(std::string_view value) {
  if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) {
    value.remove_prefix(1);
  }
  return IsNumericVersion(value);
}

/** @brief Parses a dotted numeric version into components, or an empty vector when malformed. */
inline std::vector<int> ParseVersionComponents(std::string_view value) {
  std::vector<int> components;
  if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) {
    value.remove_prefix(1);
  }
  if (value.empty()) return components;

  size_t index = 0;
  while (index < value.size()) {
    const size_t start = index;
    int number = 0;
    while (index < value.size() && value[index] != '.') {
      if (!std::isdigit(static_cast<unsigned char>(value[index]))) return {};
      // A component long enough to overflow is refused rather than wrapped.
      if (number > 100000) return {};
      number = number * 10 + (value[index] - '0');
      ++index;
    }
    if (index == start) return {};
    components.push_back(number);
    if (index < value.size()) ++index;
    else break;
  }
  return components;
}

/**
 * @brief Compares two dotted numeric versions component-wise, missing components read as zero.
 *
 * <p>So {@code 1.20 == 1.20.0.0} and {@code 26.51 > 1.20.0}, matching how the launcher compares a
 * game version against a rule's floor.
 */
inline int CompareVersions(std::string_view left, std::string_view right) {
  const std::vector<int> a = ParseVersionComponents(left);
  const std::vector<int> b = ParseVersionComponents(right);
  if (a.empty() || b.empty()) return 0;
  const size_t count = a.size() > b.size() ? a.size() : b.size();
  for (size_t i = 0; i < count; ++i) {
    const int av = i < a.size() ? a[i] : 0;
    const int bv = i < b.size() ? b[i] : 0;
    if (av != bv) return av < bv ? -1 : 1;
  }
  return 0;
}

/**
 * @brief True when [version] falls inside a range written as {@code "min"} or {@code "min-max"}.
 *
 * <p>An empty range accepts everything (the mod did not constrain the game version). A malformed
 * range is refused by {@link ValidateGameVersionRange} before this is reached, so a value that
 * reaches here and does not parse reads as "not contained" rather than "contained by accident".
 */
inline bool IsVersionInRange(std::string_view version, std::string_view range) {
  if (!IsNonBlank(range)) return true;
  const size_t dash = range.find('-');
  if (dash == std::string_view::npos) {
    return CompareVersions(version, range) == 0;
  }
  const std::string_view minimum = range.substr(0, dash);
  const std::string_view maximum = range.substr(dash + 1);
  if (!IsNonBlank(minimum) || !IsNonBlank(maximum)) return false;
  return CompareVersions(version, minimum) >= 0 && CompareVersions(version, maximum) <= 0;
}

/** @brief Validates a {@code "min"} or {@code "min-max"} range for shape, not for the running build. */
inline ModManifestValidation ValidateGameVersionRange(std::string_view range) {
  if (!IsNonBlank(range)) return ManifestOk();
  const size_t dash = range.find('-');
  if (dash == std::string_view::npos) {
    if (!IsSemanticVersion(range)) {
      return ManifestFailure(ModManifestField::GameVersionRange,
                             "game_version '" + std::string(range) + "' is not a version");
    }
    return ManifestOk();
  }
  const std::string_view minimum = range.substr(0, dash);
  const std::string_view maximum = range.substr(dash + 1);
  if (!IsNonBlank(minimum) || !IsNonBlank(maximum)) {
    return ManifestFailure(ModManifestField::GameVersionRange,
                           "game_version '" + std::string(range) + "' has an empty bound");
  }
  if (!IsSemanticVersion(minimum) || !IsSemanticVersion(maximum)) {
    return ManifestFailure(ModManifestField::GameVersionRange,
                           "game_version '" + std::string(range) + "' has a non-version bound");
  }
  if (CompareVersions(minimum, maximum) > 0) {
    return ManifestFailure(ModManifestField::GameVersionRange,
                           "game_version '" + std::string(range) + "' has min above max");
  }
  return ManifestOk();
}

/** @brief The declared fields a manifest validation needs, already read from the JSON. */
struct ModManifestFields {
  std::string id;
  std::string name;
  std::string version;
  std::string abi;
  std::string gameVersionRange;
  std::string entryFileName;
};

/**
 * @brief Validates a manifest's declared fields in a fixed order, returning the first failure.
 *
 * <p>The order is deliberate: identity first, then the entry, then compatibility, so the most
 * actionable problem is the one reported. An empty field that is optional (name, abi,
 * game_version) is accepted; only the required fields (id, version, entry) must be present.
 */
inline ModManifestValidation ValidateModManifest(const ModManifestFields &fields) {
  if (!IsNonBlank(fields.id)) {
    return ManifestFailure(ModManifestField::Id, "the manifest declares no mod id");
  }
  if (!IsNonBlank(fields.version)) {
    return ManifestFailure(ModManifestField::Version, "the manifest declares no version");
  }
  if (!IsSemanticVersion(fields.version)) {
    return ManifestFailure(ModManifestField::Version,
                           "version '" + fields.version + "' is not a semantic version");
  }
  if (!IsNonBlank(fields.entryFileName)) {
    return ManifestFailure(ModManifestField::Entry, "the manifest declares no entry point");
  }
  const size_t dot = fields.entryFileName.rfind('.');
  if (dot == std::string::npos || fields.entryFileName.substr(dot) != ".so") {
    return ManifestFailure(ModManifestField::Entry,
                           "entry '" + fields.entryFileName + "' is not a .so library");
  }
  if (IsNonBlank(fields.abi) && fields.abi != "arm64-v8a") {
    return ManifestFailure(ModManifestField::Abi,
                           "abi '" + fields.abi + "' is not arm64-v8a");
  }
  return ValidateGameVersionRange(fields.gameVersionRange);
}

} // namespace pl::internal::mod
