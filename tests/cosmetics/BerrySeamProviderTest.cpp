// Host test for the BerryAPI world-seam provider mirror (no device, no mocks).
//
// Pins:
//   * MirrorMatches accepts only an exact struct-size/ABI match, and refuses a zero (unresolved)
//     reading, so a layout drift cannot be bound through.
//   * BuildLocalPlayerState reports valid only for a real reading, carries the pose through, and
//     leaves the unreachable stats at their fail-closed defaults.
//   * A freshly built seam sets no slot other than get_local_player, so every other read fails
//     closed on the BerryAPI side.

#include <cstdio>

#include "pl/runtime/BerrySeamProvider.hpp"

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

int main() {
    // --- MirrorMatches: exact match only; a zero (unresolved) reading is a mismatch. ---
    SeamAbi ok{sizeof(SeamMirror), 8};
    expect(MirrorMatches(ok, sizeof(SeamMirror), 8), "exact size + abi matches");
    expect(!MirrorMatches(ok, sizeof(SeamMirror) - 1, 8), "a short size is refused");
    expect(!MirrorMatches(ok, sizeof(SeamMirror), 9), "a different abi is refused");
    expect(!MirrorMatches({0, 8}, sizeof(SeamMirror), 8), "an unresolved size is refused");
    expect(!MirrorMatches({sizeof(SeamMirror), 0}, sizeof(SeamMirror), 8), "an unresolved abi is refused");
    expect(!MirrorMatches({0, 0}, sizeof(SeamMirror), 8), "both zero is refused");

    // --- BuildLocalPlayerState: no reading -> invalid and every stat at its default. ---
    float pos[3] = {1.0f, 2.0f, 3.0f};
    float rot[2] = {45.0f, 10.0f};
    LocalPlayerMirror none = BuildLocalPlayerState(false, pos, rot);
    expect(none.valid == 0, "no reading -> invalid");
    expect(none.px == 0.0f && none.py == 0.0f && none.pz == 0.0f, "no reading -> origin pose");
    expect(none.hunger == 0 && none.armor_points == 0, "no reading -> default stats");

    // --- A real reading carries the pose and marks valid. ---
    LocalPlayerMirror have = BuildLocalPlayerState(true, pos, rot);
    expect(have.valid == 1, "a reading -> valid");
    expect(have.px == 1.0f && have.py == 2.0f && have.pz == 3.0f, "position carried through");
    expect(have.yaw == 45.0f && have.pitch == 10.0f, "yaw/pitch mapped from {yaw, pitch}");
    expect(have.roll == 0.0f, "roll defaults to 0");
    // Unreachable stats stay fail-closed even with a valid reading.
    expect(have.hunger == 0 && have.armor_points == 0 && have.gamemode == 0, "stats stay default");

    // --- A built seam sets only get_local_player. ---
    SeamMirror seam{};
    seam.size = sizeof(SeamMirror);
    seam.abi_version = 8;
    seam.get_local_player = [](LocalPlayerMirror *out, void *) -> std::int32_t {
        if (out == nullptr) return 0;
        float p[3] = {0, 0, 0};
        float r[2] = {0, 0};
        *out = BuildLocalPlayerState(true, p, r);
        return 1;
    };
    expect(!HasUnexpectedSlots(seam), "only the local-player slot is set");
    // The one slot we did set works through the mirror.
    LocalPlayerMirror viaCallback{};
    expect(seam.get_local_player(&viaCallback, nullptr) == 1, "the local-player callback answers");
    expect(viaCallback.valid == 1, "the callback reports a valid state");

    // --- HasUnexpectedSlots flags a stray slot. ---
    SeamMirror stray{};
    stray.get_block = [](std::int32_t, std::int32_t, std::int32_t, void *) -> std::uint32_t { return 0; };
    expect(HasUnexpectedSlots(stray), "a stray slot is flagged");

    if (failures == 0) {
        std::printf("BerrySeamProviderTest: %d checks, ALL OK\n", checks);
        return 0;
    }
    std::printf("BerrySeamProviderTest: %d checks, %d FAILURE(S)\n", checks, failures);
    return 1;
}
