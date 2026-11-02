/**
 * @file FeatureAvailabilityTest.cpp
 * @brief Host test for the per-feature availability report (item 453).
 *
 * The report is what the Diagnostics page prints verbatim, so its shape, its state vocabulary and
 * its JSON escaping are pinned here. A wiring mistake must not be able to change the shape.
 */

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "pl/runtime/FeatureAvailability.hpp"

using namespace pl::runtime;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

void everyFeatureHasAUniqueName() {
  std::set<std::string> names;
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    const char *name = FeatureName(static_cast<Feature>(i));
    expect(std::string(name) != "unknown", "every enumerator has a name");
    expect(names.insert(name).second, "feature names are unique");
  }
  expect(names.size() == kFeatureCount, "one name per feature");
}

void everyStateHasAWireName() {
  const FeatureState states[] = {FeatureState::Available, FeatureState::Unavailable,
                                 FeatureState::Disabled, FeatureState::Failed,
                                 FeatureState::Pending};
  std::set<std::string> names;
  for (const FeatureState state : states) {
    const char *name = FeatureStateName(state);
    expect(std::string(name) != "unknown", "every state has a wire name");
    expect(names.insert(name).second, "state names are unique");
  }
}

void aFreshTableIsPendingAndNotAvailable() {
  FeatureAvailability table;
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    expect(table.state(static_cast<Feature>(i)) == FeatureState::Pending,
           "a fresh feature reads as pending");
  }
  expect(!table.allAvailable(), "a fresh table is not all-available");
  expect(table.availableCount() == 0, "a fresh table has zero available");
}

void recordingAStateIsReadBack() {
  FeatureAvailability table;
  table.set(Feature::OverlayHooks, FeatureState::Available);
  expect(table.state(Feature::OverlayHooks) == FeatureState::Available,
         "an available feature reads back available");
  table.set(Feature::NativeCosmetics, FeatureState::Unavailable, "slot did not resolve");
  expect(table.state(Feature::NativeCosmetics) == FeatureState::Unavailable,
         "an unavailable feature reads back unavailable");
  expect(table.reason(Feature::NativeCosmetics) == "slot did not resolve",
         "the reason reads back");
  expect(table.availableCount() == 1, "exactly one feature is available");
  expect(!table.allAvailable(), "the table is not all-available");
}

void aDisabledFeatureIsNotAFailure() {
  FeatureAvailability table;
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    table.set(static_cast<Feature>(i), FeatureState::Available);
  }
  expect(table.allAvailable(), "all-available when every feature is available");
  table.set(Feature::TextureBindSeam, FeatureState::Disabled);
  expect(!table.allAvailable(), "a disabled feature is not available");
  // The distinction matters: Disabled must not be rendered as Failed.
  expect(table.state(Feature::TextureBindSeam) == FeatureState::Disabled,
         "disabled is its own state, not failed");
}

void theJsonCarriesEveryFeatureInOrder() {
  FeatureAvailability table;
  table.set(Feature::OverlayHooks, FeatureState::Available);
  table.set(Feature::PlayerRenderFeed, FeatureState::Failed, "hook call failed");
  const std::string json = table.toJson();

  std::size_t previous = 0;
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    const std::string name = FeatureName(static_cast<Feature>(i));
    const std::size_t at = json.find("\"" + name + "\"");
    expect(at != std::string::npos, "every feature appears in the report");
    if (i != 0) expect(at > previous, "features appear in enumerator order");
    previous = at;
  }
  expect(json.front() == '{' && json.back() == '}', "the report is one JSON object");
}

void theJsonCarriesTheStateAndTheReason() {
  FeatureAvailability table;
  table.set(Feature::JoinPacketSeam, FeatureState::Unavailable, "packet slot absent");
  const std::string json = table.toJson();
  expect(json.find("\"join_packet_seam\":{\"state\":\"unavailable\"") != std::string::npos,
         "the state is the wire name");
  expect(json.find("\"reason\":\"packet slot absent\"") != std::string::npos,
         "the reason is included");
}

void aFeatureWithNoReasonOmitsTheField() {
  FeatureAvailability table;
  table.set(Feature::SkinCapeSeam, FeatureState::Available);
  const std::string json = table.toJson();
  expect(json.find("\"skin_cape_seam\":{\"state\":\"available\"}") != std::string::npos,
         "no reason means no reason field");
}

void aReasonWithQuotesAndControlsStaysValid() {
  FeatureAvailability table;
  // A reason assembled from a version string or a build tag could carry a quote or a tab; the report
  // must stay a valid document so the client can print it.
  table.set(Feature::LocalPlayerFeed, FeatureState::Failed, "say \"hi\"\tnow");
  const std::string json = table.toJson();
  expect(json.find("say \\\"hi\\\"\\tnow") != std::string::npos,
         "quotes and tabs are escaped");
  expect(json.find('\t') == std::string::npos, "no raw tab survives into the document");
}

void jsonEscapeHandlesControlCharacters() {
  expect(JsonEscape("plain") == "plain", "plain text is unchanged");
  expect(JsonEscape("a\"b") == "a\\\"b", "a quote is escaped");
  expect(JsonEscape("a\\b") == "a\\\\b", "a backslash is escaped");
  expect(JsonEscape(std::string("a\x01") + "b") == "a\\u0001b",
         "a control character becomes a unicode escape");
}

void anOutOfRangeFeatureIsIgnored() {
  FeatureAvailability table;
  // Defensive: an enumerator past the count must not write out of bounds.
  table.set(Feature::Count, FeatureState::Available, "bogus");
  expect(table.state(Feature::Count) == FeatureState::Pending,
         "the count sentinel is not a feature");
}

} // namespace

int main() {
  everyFeatureHasAUniqueName();
  everyStateHasAWireName();
  aFreshTableIsPendingAndNotAvailable();
  recordingAStateIsReadBack();
  aDisabledFeatureIsNotAFailure();
  theJsonCarriesEveryFeatureInOrder();
  theJsonCarriesTheStateAndTheReason();
  aFeatureWithNoReasonOmitsTheField();
  aReasonWithQuotesAndControlsStaysValid();
  jsonEscapeHandlesControlCharacters();
  anOutOfRangeFeatureIsIgnored();

  std::printf("FeatureAvailabilityTest: %d checks passed\n", checks);
  return 0;
}
