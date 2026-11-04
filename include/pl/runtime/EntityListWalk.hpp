#pragma once

/**
 * @file EntityListWalk.hpp
 * @brief Walk a C++ container of pointers given its offsets, so the entity seam is a data change.
 *
 * <p>The world reconstruction needs BerryAPI's `all_entities` seam slot filled, which means reading
 * the game's entity list out of the level. The list is a C++ container inside `Level`/`Dimension`,
 * and recovering *which* container and *what* its offset is is a disassembly job (see
 * `docs/agents/world-reconstruction-re-groundwork.md`). This header is the other half: once the
 * offsets are known, the walk is a small, verified function rather than new logic written on the
 * game thread.
 *
 * <p>It is pure C++ (no Android, no game types), so the pointer arithmetic, the bounds checks and
 * the fail-closed rules are host-tested. The container layout is the libc++ `std::vector` one —
 * three pointers `{begin, end, capacity}` — which is what the engine is built against.
 *
 * <p><b>Fail-closed by construction.</b> A null container, a zero offset, an `end` before `begin`,
 * an element size of zero, or a count over the cap all answer "no entities" rather than reading
 * arbitrary memory. That matters because these offsets are recovered by hand: a wrong one must be a
 * no-op, not a fault on the game thread.
 */

#include <cstddef>
#include <cstdint>

namespace pl::runtime {

/** A libc++ `std::vector`'s two bounding pointers, as read from a container's storage. */
struct PointerRange {
    const void *begin = nullptr;
    const void *end = nullptr;
};

/**
 * @brief The largest list this walker will accept, so a wrong offset cannot make it run away.
 *
 * <p>Bedrock's runtime actor list is at most a few thousand entries; a value far above that means
 * the offset is wrong and the walk is reading garbage, so it is refused rather than iterated.
 */
constexpr std::size_t kMaxWalkedElements = 65536;

/**
 * @brief Reads the `{begin, end}` pair of a libc++ vector member at `container + offset`.
 *
 * @param container the object holding the vector (a `Level` or `Dimension`)
 * @param offset    the byte offset of the vector member within it
 * @return the range, or an empty range when the read is not sane
 */
inline PointerRange ReadVectorRange(const void *container, std::size_t offset) {
    PointerRange range{};
    if (container == nullptr) return range;
    // A zero offset is the "not recovered yet" sentinel: refuse rather than read the object's head.
    if (offset == 0) return range;
    const auto *base = static_cast<const std::uint8_t *>(container) + offset;
    // The three pointers are 8-byte aligned on arm64; a misaligned offset cannot be a real vector.
    if ((reinterpret_cast<std::uintptr_t>(base) & 0x7u) != 0u) return range;
    const auto *words = reinterpret_cast<const std::uintptr_t *>(base);
    const std::uintptr_t begin = words[0];
    const std::uintptr_t end = words[1];
    if (begin == 0u || end == 0u) return range;
    if (end < begin) return range;
    const std::uintptr_t bytes = end - begin;
    if (bytes % 8u != 0u) return range;          // element size must divide the span
    if (bytes / 8u > kMaxWalkedElements) return range;
    range.begin = reinterpret_cast<const void *>(begin);
    range.end = reinterpret_cast<const void *>(end);
    return range;
}

/** The number of elements in a range, given the element size. 0 when the size is unusable. */
inline std::size_t RangeCount(const PointerRange &range, std::size_t elementSize) {
    if (range.begin == nullptr || range.end == nullptr || elementSize == 0u) return 0u;
    const auto *b = static_cast<const std::uint8_t *>(range.begin);
    const auto *e = static_cast<const std::uint8_t *>(range.end);
    if (e < b) return 0u;
    const std::size_t bytes = static_cast<std::size_t>(e - b);
    const std::size_t count = bytes / elementSize;
    return count > kMaxWalkedElements ? 0u : count;
}

/**
 * @brief Walks a vector member, calling `visit` with each element's address.
 *
 * @param container   the object holding the vector
 * @param offset      the vector member's byte offset
 * @param elementSize the element's size in bytes (8 for a vector of pointers)
 * @param visit       called once per element with its address
 * @return how many elements were visited (0 when the read was refused)
 */
template <typename Visit>
inline std::size_t WalkVector(const void *container, std::size_t offset,
                              std::size_t elementSize, Visit visit) {
    const PointerRange range = ReadVectorRange(container, offset);
    const std::size_t count = RangeCount(range, elementSize);
    if (count == 0u) return 0u;
    const auto *base = static_cast<const std::uint8_t *>(range.begin);
    for (std::size_t i = 0; i < count; ++i) {
        visit(base + i * elementSize);
    }
    return count;
}

/**
 * @brief The offsets the entity seam needs, as data.
 *
 * <p>Kept as one value so the recovery result is a config change: fill `levelEntityListOffset` (and
 * the rest) from the disassembly pass and the seam activates. While they are zero the seam stays
 * null, which is BerryAPI's documented fail-closed default, so nothing is claimed before it is
 * known.
 */
struct EntitySeamOffsets {
    /** `Level`'s entity-list vector offset, or 0 when not recovered. */
    std::size_t levelEntityListOffset = 0;
    /** The offset of the runtime id inside one entity, or 0 when not recovered. */
    std::size_t entityRuntimeIdOffset = 0;
    /** The offset of the position inside one entity, or 0 when not recovered. */
    std::size_t entityPositionOffset = 0;
    /** The offset of the view rotation inside one entity, or 0 when not recovered. */
    std::size_t entityRotationOffset = 0;

    /** True when every offset needed to enumerate and read an entity has been recovered. */
    bool complete() const {
        return levelEntityListOffset != 0 && entityRuntimeIdOffset != 0 &&
               entityPositionOffset != 0 && entityRotationOffset != 0;
    }
};

} // namespace pl::runtime
