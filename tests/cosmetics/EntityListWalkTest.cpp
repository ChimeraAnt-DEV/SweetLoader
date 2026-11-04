// Host test for the entity-list walker (no device, no mocks).
//
// Pins the rules that make a hand-recovered offset safe to use: a null container, a zero offset
// (the "not recovered yet" sentinel), a misaligned offset, a reversed or non-multiple span, and a
// count over the cap all answer "no entities" rather than reading arbitrary memory. A correct
// vector is walked in order and every element is visited.

#include <cstdio>
#include <cstdint>

#include "pl/runtime/EntityListWalk.hpp"

using namespace pl::runtime;

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
    // --- A zero offset is the "not recovered" sentinel and must refuse. ---
    alignas(8) std::uint8_t object[64] = {};
    expect(ReadVectorRange(object, 0).begin == nullptr, "a zero offset is refused");

    // --- A null container refuses. ---
    expect(ReadVectorRange(nullptr, 16).begin == nullptr, "a null container is refused");

    // --- A well-formed vector at offset 16 is read, in order. ---
    std::uintptr_t elements[3] = {0x1000u, 0x2000u, 0x3000u};
    alignas(8) std::uint8_t holder[64] = {};
    auto *words = reinterpret_cast<std::uintptr_t *>(holder);
    words[2] = reinterpret_cast<std::uintptr_t>(&elements[0]);   // begin  at +0x10
    words[3] = reinterpret_cast<std::uintptr_t>(&elements[3]);   // end    at +0x18
    words[4] = reinterpret_cast<std::uintptr_t>(&elements[3]);   // capacity
    PointerRange range = ReadVectorRange(holder, 16);
    expect(range.begin == &elements[0] && range.end == &elements[3], "a valid vector is read");
    expect(RangeCount(range, 8) == 3, "three elements counted");

    std::uintptr_t visited[3] = {};
    std::size_t n = 0;
    std::size_t walked = WalkVector(holder, 16, 8, [&](const void *p) {
        if (n < 3) visited[n] = *static_cast<const std::uintptr_t *>(p);
        ++n;
    });
    expect(walked == 3 && n == 3, "three elements visited");
    expect(visited[0] == 0x1000u && visited[1] == 0x2000u && visited[2] == 0x3000u,
           "elements visited in order");

    // --- A misaligned offset cannot be a real vector: refuse. ---
    expect(ReadVectorRange(holder, 12).begin == nullptr, "a misaligned offset is refused");

    // --- end before begin: refuse. ---
    words[2] = reinterpret_cast<std::uintptr_t>(&elements[3]);
    words[3] = reinterpret_cast<std::uintptr_t>(&elements[0]);
    expect(ReadVectorRange(holder, 16).begin == nullptr, "a reversed span is refused");

    // --- A span that is not a multiple of the element size: refuse. ---
    words[2] = reinterpret_cast<std::uintptr_t>(&elements[0]);
    words[3] = reinterpret_cast<std::uintptr_t>(reinterpret_cast<std::uint8_t *>(&elements[0]) + 12);
    expect(ReadVectorRange(holder, 16).begin == nullptr, "a non-multiple span is refused");

    // --- An over-cap count: refuse. ---
    words[2] = 0x1000u;
    words[3] = 0x1000u + (kMaxWalkedElements + 1) * 8u;
    expect(ReadVectorRange(holder, 16).begin == nullptr, "an over-cap count is refused");

    // --- RangeCount needs a real element size. ---
    expect(RangeCount(PointerRange{nullptr, nullptr}, 8) == 0, "a null range has no count");
    expect(RangeCount(range, 0) == 0, "a zero element size has no count");

    // --- The offsets value starts incomplete and activates only when fully recovered. ---
    EntitySeamOffsets offsets{};
    expect(!offsets.complete(), "a fresh offsets value is incomplete");
    offsets.levelEntityListOffset = 0x2f8;
    expect(!offsets.complete(), "a partial recovery stays incomplete");
    offsets.entityRuntimeIdOffset = 0x18;
    offsets.entityPositionOffset = 0x230;
    offsets.entityRotationOffset = 0x238;
    expect(offsets.complete(), "a full recovery is complete");

    std::printf("EntityListWalkTest: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
