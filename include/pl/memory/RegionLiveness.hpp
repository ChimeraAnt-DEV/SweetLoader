#pragma once

/**
 * @file RegionLiveness.hpp
 * @brief Decides whether a cached module region list is still valid, without re-parsing maps.
 *
 * <p>The signature scanner caches a module's region list and reuses it across scans. Before reuse it
 * must confirm the module is still mapped where it was cached. The check is two parts: every cached
 * region still answers a one-byte fault-safe read, and the load base still begins with the ELF magic.
 * The second part is what catches a module that was unmapped and reloaded at a different base: the
 * old addresses may still be mapped (by another library, or by the reloaded copy at an address that
 * happens to cover a probe), so a probe alone is not enough.
 *
 * <p>The rule is pure: the caller supplies a reader (the live path passes one built on
 * {@code process_vm_readv}), so the decision is host-tested with a fake reader.
 */

#include <cstddef>
#include <cstdint>

namespace pl::memory {

/**
 * @brief A fault-safe read: reads @p size bytes at @p address into @p out.
 *
 * @return true when all @p size bytes were read; false when the address is unmapped (a short read)
 *         or the read otherwise failed.
 */
using RegionReader = bool (*)(std::uintptr_t address, std::size_t size, std::uint8_t *out);

/** @brief The ELF magic every mapped module base must begin with. */
inline bool IsElfMagic(const std::uint8_t *bytes) noexcept {
  return bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F';
}

/**
 * @brief Whether every cached region is still mapped and the base still starts with the ELF magic.
 *
 * @param regionStarts the start address of each cached region.
 * @param count        the number of regions. Zero is "not mapped" (an empty list is never valid).
 * @param base         the cached load base.
 * @param reader       the fault-safe reader.
 */
inline bool RegionsStillMapped(const std::uintptr_t *regionStarts, std::size_t count,
                               std::uintptr_t base, const RegionReader reader) noexcept {
  if (count == 0 || reader == nullptr) return false;
  std::uint8_t probe = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (!reader(regionStarts[i], 1, &probe)) return false;
  }
  std::uint8_t magic[4] = {};
  if (!reader(base, sizeof(magic), magic)) return false;
  return IsElfMagic(magic);
}

} // namespace pl::memory
