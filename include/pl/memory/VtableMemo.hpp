#pragma once

/**
 * @file VtableMemo.hpp
 * @brief Cache keys and rules for the vtable resolver's memoization.
 *
 * <p>Two things were resolved more than once per session (audit items 4.12 and 4.14):
 *
 *  - {@code resolveVtableSlotReferencingString} scans up to ~100 slot bodies per call, but its
 *    result was not memoized at all — the existing memo is keyed on the slot *index*, which a
 *    needle-based lookup does not know until after the scan; and
 *  - {@code GlossGetLibSection} re-parses the module's ELF on every call, and the resolver asks for
 *    {@code .rodata}, {@code .data.rel.ro} and {@code .text} on every resolve.
 *
 * <p>The keys are pure, so the "two different lookups must not share a key" property is host-tested.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pl::memory {

/** @brief The memo key for a needle-based slot lookup. Includes the slot bound, which changes the
 *         scan and so can change the answer. */
inline std::string MakeVtableNeedleKey(std::string_view moduleName, std::string_view typeInfoName,
                                       std::string_view needle, std::size_t maxSlots) {
  std::string key;
  key.reserve(moduleName.size() + typeInfoName.size() + needle.size() + 32);
  key.append(moduleName).append("::").append(typeInfoName).append("::").append(needle)
      .append("::").append(std::to_string(maxSlots));
  return key;
}

/** @brief The cache key for a module section, e.g. {@code libminecraftpe.so::.rodata}. */
inline std::string MakeSectionKey(std::string_view moduleName, std::string_view sectionName) {
  std::string key;
  key.reserve(moduleName.size() + sectionName.size() + 2);
  key.append(moduleName).append("::").append(sectionName);
  return key;
}

/** @brief A cached module section: an address and size. `address == 0` is a remembered miss. */
struct CachedSection {
  std::uintptr_t address = 0;
  std::size_t size = 0;
  /** True once this entry has been populated (a miss is cached too, so it is not re-parsed). */
  bool present = false;
};

/**
 * @brief Whether a section lookup result is worth remembering for the session.
 *
 * <p>A section's address and size are fixed for a loaded module's lifetime, so a hit is always worth
 * caching. A miss is also cached — a section that does not exist in this build will not appear
 * later, and re-parsing the ELF to rediscover that is pure waste — but only when the module was
 * actually found, so a miss from a module that is not mapped yet is left uncached and can be retried.
 */
inline bool ShouldCacheSection(bool moduleFound, std::uintptr_t address) noexcept {
  (void)address;
  return moduleFound;
}

} // namespace pl::memory
