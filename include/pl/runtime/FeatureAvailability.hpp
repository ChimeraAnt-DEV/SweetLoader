#pragma once

/**
 * @file FeatureAvailability.hpp
 * @brief Per-feature hook availability, reported to the client as one JSON blob (item 453).
 *
 * <p>The preloader installs a set of independent seams — the pause-menu overlay hooks, the local
 * player feed, the player-render feed, the native cosmetics interception, the skin/cape seams, the
 * texture binder and the join-packet seam. Each resolves separately and each is fail-closed: a
 * build where one signature is missing leaves *that* feature unavailable and everything else
 * working.
 *
 * <p>The problem this solves is that "the preloader loaded" and "this feature works" are different
 * questions. Without a per-feature report the launcher can only say the hooks are on or off, so a
 * single missing signature reads as "nothing works" and a feature that quietly did nothing looks
 * like a launcher bug. Here each feature carries its own state and reason, the whole set is
 * rendered as one small JSON object, and the client's Diagnostics page prints it verbatim.
 *
 * <p>The class is pure and host-tested (`FeatureAvailabilityTest`): the ordering, the state
 * vocabulary, the reason passthrough and the JSON escaping are all decided here so a wiring mistake
 * cannot change the shape of the report.
 *
 * <p><b>Thread-safety.</b> The preloader records availability from the frame tick (one writer) and
 * the client reads it from the JNI thread. Each entry is a small fixed-size record and the write is
 * an ordinary store; the report is a snapshot, so a torn read would at worst report one feature's
 * previous state for one poll. That is acceptable for a diagnostic, and it avoids a lock on the
 * frame tick.
 */

#include <cstddef>
#include <string>
#include <string_view>

namespace pl::runtime {

/**
 * @brief The availability state of one feature.
 *
 * <p>`Disabled` is distinct from `Unavailable`: a feature the user turned off is not a failure, so
 * the diagnostics report must not present it as one.
 */
enum class FeatureState {
  /** The feature's seam resolved and its hook installed. */
  Available,
  /** The seam did not resolve on this build; the feature is off but the launcher still works. */
  Unavailable,
  /** The user disabled the feature, so no install was attempted. */
  Disabled,
  /** The install was attempted and failed (a hook call returned an error). */
  Failed,
  /** Not yet attempted — the deferred install has not run for this feature. */
  Pending,
};

/** @brief The wire name of a state, used as the JSON value. */
inline const char *FeatureStateName(FeatureState state) noexcept {
  switch (state) {
    case FeatureState::Available:
      return "available";
    case FeatureState::Unavailable:
      return "unavailable";
    case FeatureState::Disabled:
      return "disabled";
    case FeatureState::Failed:
      return "failed";
    case FeatureState::Pending:
      return "pending";
  }
  return "unknown";
}

/**
 * @brief The features the preloader reports on.
 *
 * <p>The order is the report order, so the client can rely on a stable sequence. Adding a feature is
 * one enumerator here plus its `FeatureName`; the test asserts every enumerator has a name and that
 * the names are unique, so a new one cannot silently report as `"unknown"`.
 */
enum class Feature {
  OverlayHooks,
  LocalPlayerFeed,
  PlayerRenderFeed,
  NativeCosmetics,
  SkinCapeSeam,
  TextureBindSeam,
  JoinPacketSeam,
  /** Not a feature: the count of enumerators, so arrays can be sized. */
  Count,
};

/** @brief The wire name of a feature. */
inline const char *FeatureName(Feature feature) noexcept {
  switch (feature) {
    case Feature::OverlayHooks:
      return "overlay_hooks";
    case Feature::LocalPlayerFeed:
      return "local_player_feed";
    case Feature::PlayerRenderFeed:
      return "player_render_feed";
    case Feature::NativeCosmetics:
      return "native_cosmetics";
    case Feature::SkinCapeSeam:
      return "skin_cape_seam";
    case Feature::TextureBindSeam:
      return "texture_bind_seam";
    case Feature::JoinPacketSeam:
      return "join_packet_seam";
    case Feature::Count:
      break;
  }
  return "unknown";
}

/** @brief The number of reportable features. */
inline constexpr std::size_t kFeatureCount = static_cast<std::size_t>(Feature::Count);

/**
 * @brief One feature's recorded state and its optional reason.
 *
 * <p>The reason is a short human-readable sentence for the diagnostics page ("render slot did not
 * resolve on this build"). It is stored as a fixed-capacity string view into a caller-owned literal
 * where possible; here it is a small `std::string` because the reason can be assembled from a
 * version string. The report only includes a reason when one was set.
 */
struct FeatureEntry {
  FeatureState state = FeatureState::Pending;
  std::string reason;
};

/** @brief Escapes a string for inclusion in a JSON string literal. */
inline std::string JsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char ch : text) {
    switch (ch) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          // A control character cannot appear literally in JSON; emit a unicode escape so a reason
          // built from arbitrary text can never produce a malformed document.
          static const char kHex[] = "0123456789abcdef";
          out.append("\\u00");
          out.push_back(kHex[(static_cast<unsigned char>(ch) >> 4U) & 0xfU]);
          out.push_back(kHex[static_cast<unsigned char>(ch) & 0xfU]);
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

/**
 * @brief The feature availability table.
 *
 * <p>A plain fixed array indexed by the `Feature` enumerator: recording a state is an assignment,
 * and rendering the report walks the array in enumerator order. No allocation on the record path,
 * one allocation for the report string.
 */
class FeatureAvailability {
public:
  /** Records a feature's state with no reason. */
  void set(Feature feature, FeatureState state) { set(feature, state, {}); }

  /** Records a feature's state and a short reason. */
  void set(Feature feature, FeatureState state, std::string_view reason) {
    const std::size_t index = static_cast<std::size_t>(feature);
    if (index >= kFeatureCount) return;
    mEntries[index].state = state;
    mEntries[index].reason.assign(reason.data(), reason.size());
  }

  /** The recorded state of a feature (Pending before anything is recorded). */
  [[nodiscard]] FeatureState state(Feature feature) const {
    const std::size_t index = static_cast<std::size_t>(feature);
    return index < kFeatureCount ? mEntries[index].state : FeatureState::Pending;
  }

  /** The recorded reason of a feature, or an empty view when none was set. */
  [[nodiscard]] std::string_view reason(Feature feature) const {
    const std::size_t index = static_cast<std::size_t>(feature);
    return index < kFeatureCount ? std::string_view(mEntries[index].reason) : std::string_view{};
  }

  /** True when every feature resolved. */
  [[nodiscard]] bool allAvailable() const {
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
      if (mEntries[i].state != FeatureState::Available) return false;
    }
    return true;
  }

  /** The number of features that resolved. */
  [[nodiscard]] std::size_t availableCount() const {
    std::size_t count = 0;
    for (const auto &entry : mEntries) {
      if (entry.state == FeatureState::Available) ++count;
    }
    return count;
  }

  /**
   * @brief Renders the whole table as one JSON object.
   *
   * <p>The shape is stable: an object keyed by feature name, each value an object with `state` and,
   * when one was recorded, `reason`. The client prints this verbatim in Diagnostics, so a reader
   * (and a bug report) sees exactly which feature failed and why without the launcher having to know
   * the feature list.
   */
  [[nodiscard]] std::string toJson() const {
    std::string out;
    out.reserve(256);
    out.push_back('{');
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
      if (i != 0) out.push_back(',');
      out.push_back('"');
      out.append(FeatureName(static_cast<Feature>(i)));
      out.append("\":{\"state\":\"");
      out.append(FeatureStateName(mEntries[i].state));
      out.push_back('"');
      if (!mEntries[i].reason.empty()) {
        out.append(",\"reason\":\"");
        out.append(JsonEscape(mEntries[i].reason));
        out.push_back('"');
      }
      out.push_back('}');
    }
    out.push_back('}');
    return out;
  }

private:
  FeatureEntry mEntries[kFeatureCount]{};
};

/** @brief The process-wide availability table the install path records into. */
FeatureAvailability &GameFeatureAvailability();

} // namespace pl::runtime
