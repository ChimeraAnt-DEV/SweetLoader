// Host test for the BerryAPI governor ABI mirror (no device, no mocks).
//
// Pins:
//   * GovernorMirrorMatches accepts only an exact struct-size/ABI match and refuses a zero
//     (unresolved) reading, so a layout drift cannot be used through.
//   * GovernorApiUsable requires status + available and does not require the optional setters.
//   * GovernorThrottling is Serious or worse, never Fair.
//   * GovernorMeanQuality averages the knobs.

#include <cstdio>

#include "pl/runtime/BerryGovernorMirror.hpp"

using namespace pl::berry;

static int checks = 0;
static int failures = 0;

static void expect(bool condition, const char *what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

static GovernorApiMirror CompleteApi() {
    GovernorApiMirror api{};
    api.size = sizeof(GovernorApiMirror);
    api.abi_version = kHostGovernorAbiVersion;
    api.status = [](GovernorStatusMirror *) {};
    api.set_thermal = [](std::int32_t) -> std::int32_t { return 0; };
    api.set_platform = [](std::int32_t) {};
    api.set_ceiling = [](float) {};
    api.available = []() -> std::int32_t { return 1; };
    return api;
}

int main() {
    // --- MirrorMatches ---
    GovernorAbi ok{sizeof(GovernorApiMirror), kHostGovernorAbiVersion};
    expect(GovernorMirrorMatches(ok, sizeof(GovernorApiMirror), kHostGovernorAbiVersion),
           "an exact size/ABI match is accepted");
    expect(!GovernorMirrorMatches({0, kHostGovernorAbiVersion}, sizeof(GovernorApiMirror),
                                  kHostGovernorAbiVersion),
           "an unresolved struct size is refused");
    expect(!GovernorMirrorMatches({sizeof(GovernorApiMirror), 0}, sizeof(GovernorApiMirror),
                                  kHostGovernorAbiVersion),
           "an unresolved ABI version is refused");
    expect(!GovernorMirrorMatches({sizeof(GovernorApiMirror) + 4, kHostGovernorAbiVersion},
                                  sizeof(GovernorApiMirror), kHostGovernorAbiVersion),
           "a struct-size drift is refused");
    expect(!GovernorMirrorMatches({sizeof(GovernorApiMirror), kHostGovernorAbiVersion + 1},
                                  sizeof(GovernorApiMirror), kHostGovernorAbiVersion),
           "an ABI bump is refused");

    // --- GovernorApiUsable ---
    GovernorApiMirror api = CompleteApi();
    expect(GovernorApiUsable(api), "a surface with status and available is usable");
    api.status = nullptr;
    expect(!GovernorApiUsable(api), "a surface without status is not usable");
    api = CompleteApi();
    api.available = nullptr;
    expect(!GovernorApiUsable(api), "a surface without available is not usable");
    // The optional setters may be absent.
    api = CompleteApi();
    api.set_thermal = nullptr;
    api.set_platform = nullptr;
    api.set_ceiling = nullptr;
    expect(GovernorApiUsable(api), "a surface without the optional setters is still usable");

    // --- GovernorThrottling ---
    GovernorStatusMirror st{};
    st.thermal = static_cast<std::int32_t>(ThermalState::Nominal);
    expect(!GovernorThrottling(st), "nominal is not throttling");
    st.thermal = static_cast<std::int32_t>(ThermalState::Fair);
    expect(!GovernorThrottling(st), "fair is warm, not throttling");
    st.thermal = static_cast<std::int32_t>(ThermalState::Serious);
    expect(GovernorThrottling(st), "serious is throttling");
    st.thermal = static_cast<std::int32_t>(ThermalState::Critical);
    expect(GovernorThrottling(st), "critical is throttling");
    st.thermal = static_cast<std::int32_t>(ThermalState::Shutdown);
    expect(GovernorThrottling(st), "shutdown is throttling");

    // --- GovernorMeanQuality ---
    GovernorStatusMirror q{};
    for (int i = 0; i < static_cast<int>(GovernorKnob::Count); ++i) q.quality[i] = 1.0f;
    expect(GovernorMeanQuality(q) > 0.999f, "all-full quality averages to 1");
    for (int i = 0; i < static_cast<int>(GovernorKnob::Count); ++i) q.quality[i] = 0.5f;
    expect(GovernorMeanQuality(q) > 0.49f && GovernorMeanQuality(q) < 0.51f,
           "half quality averages to 0.5");

    std::printf("BerryGovernorMirrorTest: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
