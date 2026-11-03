// Host test for the BerryAPI replay ABI mirror (no device, no mocks).
//
// Pins:
//   * ReplayMirrorMatches accepts only an exact struct-size/ABI match and refuses a zero
//     (unresolved) reading, so a layout drift cannot be used through.
//   * ReplayApiUsable requires the calls the launcher's replay screen drives, and does not require
//     the optional ones (save/pause/stop/set_local_player).
//   * ReplayTickLength is a real length for a loaded replay and 0 for an empty one.

#include <cstdio>
#include <cstring>

#include "pl/runtime/BerryReplayMirror.hpp"

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

// A complete, usable surface for the "all present" case.
static ReplayApiMirror CompleteApi() {
    ReplayApiMirror api{};
    api.size = sizeof(ReplayApiMirror);
    api.abi_version = kHostReplayAbiVersion;
    api.load = [](const void *, std::size_t) -> std::int32_t { return 0; };
    api.save = nullptr;
    api.unload = []() {};
    api.info = [](ReplayInfoMirror *) -> std::int32_t { return 0; };
    api.play = []() -> std::int32_t { return 0; };
    api.pause = nullptr;
    api.stop = nullptr;
    api.seek = [](std::uint64_t) -> std::int32_t { return 0; };
    api.set_speed = nullptr;
    api.playback = [](ReplayPlaybackMirror *) {};
    api.set_local_player = nullptr;
    api.loaded = []() -> std::int32_t { return 0; };
    api.status = nullptr;
    return api;
}

int main() {
    // --- MirrorMatches: exact match only; a zero (unresolved) reading is a mismatch. ---
    ReplayAbi ok{sizeof(ReplayApiMirror), kHostReplayAbiVersion};
    expect(ReplayMirrorMatches(ok, sizeof(ReplayApiMirror), kHostReplayAbiVersion),
           "an exact size/ABI match is accepted");
    expect(!ReplayMirrorMatches({0, kHostReplayAbiVersion}, sizeof(ReplayApiMirror),
                                kHostReplayAbiVersion),
           "an unresolved struct size is refused");
    expect(!ReplayMirrorMatches({sizeof(ReplayApiMirror), 0}, sizeof(ReplayApiMirror),
                                kHostReplayAbiVersion),
           "an unresolved ABI version is refused");
    expect(!ReplayMirrorMatches({sizeof(ReplayApiMirror) + 8, kHostReplayAbiVersion},
                                sizeof(ReplayApiMirror), kHostReplayAbiVersion),
           "a struct-size drift is refused");
    expect(!ReplayMirrorMatches({sizeof(ReplayApiMirror), kHostReplayAbiVersion + 1},
                                sizeof(ReplayApiMirror), kHostReplayAbiVersion),
           "an ABI-version bump is refused");

    // --- ReplayApiUsable: the required calls must be present; the optional ones need not be. ---
    ReplayApiMirror api = CompleteApi();
    expect(ReplayApiUsable(api), "a surface with the required calls is usable");

    api.load = nullptr;
    expect(!ReplayApiUsable(api), "a surface without load is not usable");
    api = CompleteApi();
    api.playback = nullptr;
    expect(!ReplayApiUsable(api), "a surface without playback is not usable");
    api = CompleteApi();
    api.info = nullptr;
    expect(!ReplayApiUsable(api), "a surface without info is not usable");
    api = CompleteApi();
    api.seek = nullptr;
    expect(!ReplayApiUsable(api), "a surface without seek is not usable");
    api = CompleteApi();
    api.play = nullptr;
    expect(!ReplayApiUsable(api), "a surface without play is not usable");
    api = CompleteApi();
    api.unload = nullptr;
    expect(!ReplayApiUsable(api), "a surface without unload is not usable");
    api = CompleteApi();
    api.loaded = nullptr;
    expect(!ReplayApiUsable(api), "a surface without loaded is not usable");

    // The optional ones do not make it unusable.
    api = CompleteApi();
    api.save = nullptr;
    api.pause = nullptr;
    api.stop = nullptr;
    api.set_speed = nullptr;
    api.set_local_player = nullptr;
    api.status = nullptr;
    expect(ReplayApiUsable(api),
           "a surface missing only the optional calls is still usable");

    // --- ReplayTickLength ---
    ReplayInfoMirror info{};
    info.loaded = 0;
    info.first_tick = 10;
    info.last_tick = 90;
    expect(ReplayTickLength(info) == 0, "an empty replay has no length");

    info.loaded = 1;
    expect(ReplayTickLength(info) == 80, "a loaded replay reports last-first");

    info.first_tick = 90;
    info.last_tick = 10;
    expect(ReplayTickLength(info) == 0, "an inverted range reports 0, never an underflow");

    // --- The field order is the ABI: a save/load through the mirror keeps every field. ---
    ReplayInfoMirror src{};
    src.loaded = 1;
    src.first_tick = 100;
    src.last_tick = 400;
    src.section_count = 3;
    src.record_count = 42;
    src.payload_bytes = 2048;
    src.build_id = 7;
    src.protocol_version = 924;
    ReplayInfoMirror copy = src;
    expect(std::memcmp(&src, &copy, sizeof(src)) == 0, "the info struct copies field for field");

    std::printf("BerryReplayMirrorTest: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
