// Host test for the BerryAPI cosmetic ABI mirror (no device, no mocks).
//
// Pins:
//   * CosmeticMirrorMatches accepts only an exact struct-size/ABI match and refuses a zero
//     (unresolved) reading, so a layout drift cannot be used through.
//   * CosmeticReaderUsable requires the size, the ABI and a bone callback, so a half-filled reader
//     is refused before it crosses the boundary.
//   * CosmeticApiUsable requires the calls the launcher's cosmetics screen drives, and does not
//     require the optional ones (reload_packs, the canonical-loadout pair).
//   * The status enum and the slot enum match the library's numbering, which is the ABI.

#include <cstdio>

#include "pl/runtime/BerryCosmeticMirror.hpp"

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
static CosmeticApiMirror CompleteApi() {
    CosmeticApiMirror api{};
    api.size = sizeof(CosmeticApiMirror);
    api.abi_version = kHostCosmeticAbiVersion;
    api.reload_packs = [](const char *, const char *) -> std::uint32_t { return 0; };
    api.list = [](CosmeticAssetMirror *, std::size_t, std::size_t *) -> std::int32_t { return 0; };
    api.registry_count = []() -> std::uint32_t { return 0; };
    api.equip = [](const char *) -> std::int32_t { return 0; };
    api.unequip = [](std::int32_t) -> std::int32_t { return 0; };
    api.equipped = [](std::int32_t, char *, std::size_t) -> std::int32_t { return 0; };
    api.clear_loadout = []() -> std::int32_t { return 0; };
    api.loadout_canonical = nullptr;
    api.set_loadout_canonical = nullptr;
    api.save_preset = [](const char *) -> std::int32_t { return 0; };
    api.apply_preset = [](const char *) -> std::int32_t { return 0; };
    api.has_preset = [](const char *) -> std::int32_t { return 0; };
    api.preset_names = [](char *, std::size_t, std::size_t, std::size_t *) -> std::int32_t {
        return 0;
    };
    api.set_local_only = [](std::int32_t) -> std::int32_t { return 0; };
    api.local_only = []() -> std::int32_t { return 1; };
    api.stats = [](CosmeticStatsMirror *) {};
    api.set_player_model_hook = [](void *, const PlayerModelBoneReaderMirror *) -> std::int32_t {
        return 0;
    };
    api.clear_player_model_hook = []() {};
    api.has_player_model_hook = []() -> std::int32_t { return 0; };
    return api;
}

int main() {
    // --- The ABI numbers the library reports are pinned ---
    expect(kHostCosmeticAbiVersion == 1, "the mirrored ABI version is 1");

    // --- The mirror check is exact and refuses an unresolved reading ---
    CosmeticAbi good{sizeof(CosmeticApiMirror), kHostCosmeticAbiVersion};
    expect(CosmeticMirrorMatches(good, sizeof(CosmeticApiMirror), kHostCosmeticAbiVersion),
           "an exact struct-size/ABI match is accepted");
    expect(!CosmeticMirrorMatches({0, kHostCosmeticAbiVersion}, sizeof(CosmeticApiMirror),
                                  kHostCosmeticAbiVersion),
           "a zero struct size (unresolved) is refused");
    expect(!CosmeticMirrorMatches({sizeof(CosmeticApiMirror), 0}, sizeof(CosmeticApiMirror),
                                  kHostCosmeticAbiVersion),
           "a zero ABI version (unresolved) is refused");
    expect(!CosmeticMirrorMatches({sizeof(CosmeticApiMirror) + 8, kHostCosmeticAbiVersion},
                                  sizeof(CosmeticApiMirror), kHostCosmeticAbiVersion),
           "a struct-size drift is refused");
    expect(!CosmeticMirrorMatches({sizeof(CosmeticApiMirror), kHostCosmeticAbiVersion + 1},
                                  sizeof(CosmeticApiMirror), kHostCosmeticAbiVersion),
           "an ABI-version drift is refused");

    // --- The reader rule ---
    PlayerModelBoneReaderMirror reader{};
    reader.size = sizeof(PlayerModelBoneReaderMirror);
    reader.abi_version = kHostCosmeticAbiVersion;
    reader.bone_transform = [](std::uint64_t, std::uint16_t, std::int32_t,
                               CosmeticBoneTransformMirror *, void *) -> std::int32_t { return 1; };
    expect(CosmeticReaderUsable(reader), "a complete reader is usable");
    PlayerModelBoneReaderMirror noBone = reader;
    noBone.bone_transform = nullptr;
    expect(!CosmeticReaderUsable(noBone), "a reader with no bone callback is refused");
    PlayerModelBoneReaderMirror shortReader = reader;
    shortReader.size = sizeof(PlayerModelBoneReaderMirror) - 8;
    expect(!CosmeticReaderUsable(shortReader), "a short reader is refused");
    PlayerModelBoneReaderMirror badAbi = reader;
    badAbi.abi_version = 999;
    expect(!CosmeticReaderUsable(badAbi), "an ABI-mismatched reader is refused");

    // --- The usable-surface rule: the required calls are required, the optional are not ---
    expect(CosmeticApiUsable(CompleteApi()), "a complete surface is usable");
    {
        CosmeticApiMirror api = CompleteApi();
        api.list = nullptr;
        expect(!CosmeticApiUsable(api), "a surface with no catalog list is refused");
    }
    {
        CosmeticApiMirror api = CompleteApi();
        api.equip = nullptr;
        expect(!CosmeticApiUsable(api), "a surface with no equip is refused");
    }
    {
        CosmeticApiMirror api = CompleteApi();
        api.set_player_model_hook = nullptr;
        expect(!CosmeticApiUsable(api), "a surface with no player-model hook is refused");
    }
    {
        // The optional calls may be absent and the surface is still usable.
        CosmeticApiMirror api = CompleteApi();
        api.reload_packs = nullptr;
        api.loadout_canonical = nullptr;
        api.set_loadout_canonical = nullptr;
        expect(CosmeticApiUsable(api), "the optional calls may be absent");
    }

    // --- The slot rule clamps an out-of-range value ---
    expect(CosmeticSlotFromInt(0) == CosmeticSlot::None, "slot 0 is None");
    expect(CosmeticSlotFromInt(1) == CosmeticSlot::Head, "slot 1 is Head");
    expect(CosmeticSlotFromInt(7) == CosmeticSlot::Emote, "slot 7 is Emote");
    expect(CosmeticSlotFromInt(8) == CosmeticSlot::None, "slot 8 (Count) clamps to None");
    expect(CosmeticSlotFromInt(-3) == CosmeticSlot::None, "a negative slot clamps to None");
    expect(CosmeticSlotFromInt(999) == CosmeticSlot::None, "a large slot clamps to None");

    // --- The status and slot enums are the ABI; pin every value ---
    expect(static_cast<std::int32_t>(CosmeticStatus::Ok) == 0, "status Ok is 0");
    expect(static_cast<std::int32_t>(CosmeticStatus::NotAttached) == 1, "status NotAttached is 1");
    expect(static_cast<std::int32_t>(CosmeticStatus::BadArgument) == 2, "status BadArgument is 2");
    expect(static_cast<std::int32_t>(CosmeticStatus::UnknownAsset) == 3, "status UnknownAsset is 3");
    expect(static_cast<std::int32_t>(CosmeticStatus::NotOwned) == 4, "status NotOwned is 4");
    expect(static_cast<std::int32_t>(CosmeticStatus::BadSlot) == 5, "status BadSlot is 5");
    expect(static_cast<std::int32_t>(CosmeticStatus::BadFormat) == 6, "status BadFormat is 6");
    expect(static_cast<std::int32_t>(CosmeticStatus::UnknownPreset) == 7,
           "status UnknownPreset is 7");
    expect(static_cast<std::int32_t>(CosmeticStatus::BufferTooSmall) == 8,
           "status BufferTooSmall is 8");
    expect(static_cast<std::int32_t>(CosmeticStatus::NoHookBackend) == 9,
           "status NoHookBackend is 9");
    expect(static_cast<std::int32_t>(CosmeticStatus::HookRefused) == 10, "status HookRefused is 10");
    expect(static_cast<std::int32_t>(CosmeticStatus::AlreadyHooked) == 11,
           "status AlreadyHooked is 11");
    expect(static_cast<std::int32_t>(CosmeticStatus::BadReader) == 12, "status BadReader is 12");
    expect(static_cast<std::int32_t>(CosmeticSlot::Count) == 8, "slot Count is 8");

    // --- The string capacities are the ABI too ---
    expect(kCosmeticMaxId == 128, "the asset-id capacity is 128");
    expect(kCosmeticMaxAnimation == 64, "the animation-name capacity is 64");

    if (failures == 0) {
        std::printf("BerryCosmeticMirrorTest: %d checks passed\n", checks);
        return 0;
    }
    std::printf("BerryCosmeticMirrorTest: %d/%d checks failed\n", failures, checks);
    return 1;
}
