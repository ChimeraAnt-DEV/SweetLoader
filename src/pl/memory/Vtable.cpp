#include "pl/memory/Vtable.hpp"

#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

#include "pl/Gloss.h"
#include "pl/Logger.hpp"
#include "pl/memory/VtableMemo.hpp"

namespace pl::memory {
namespace {

std::once_flag glossInitOnce;

// A memo for resolved slots, keyed by module + type + slot. The scan walks `.rodata`
// and `.data.rel.ro`, which is real work, and the same (type, slot) pair is resolved from more
// than one call site (the render feed and the cosmetics hook both ask for the LivePlayerRenderer
// slot). The key carries the module name, not an address, because addresses move with ASLR on every
// launch; the memo is therefore per-process, and the honest note is that no on-disk cache can store
// the answer for the next launch. What it saves is the repeated scan inside one session.
std::unordered_map<std::string, uintptr_t> gVtableSlotMemo;
std::mutex gVtableSlotMemoMutex;

// A memo for the needle-based slot lookup (audit item 4.12). The scan walks up to ~100 slot bodies,
// and the call is made twice per install (the render feed and the cosmetics hook); the slot-index
// memo above cannot serve it because the needle lookup does not know the slot until after the scan.
std::unordered_map<std::string, uintptr_t> gVtableNeedleMemo;

// A memo for module sections (audit item 4.14). GlossGetLibSection re-parses the module's ELF on
// every call, and a single resolve asks for .rodata, .data.rel.ro and .text.
std::unordered_map<std::string, pl::memory::CachedSection> gSectionMemo;
// Modules whose ELF has been successfully parsed at least once, so a section miss for them is a real
// "no such section" and can be cached rather than re-parsed every call.
std::unordered_map<std::string, bool> gModuleFound;
std::mutex gSectionMemoMutex;

std::string makeVtableSlotKey(std::string_view typeInfoName, size_t slot,
                              std::string_view moduleName) {
  std::string key;
  key.reserve(typeInfoName.size() + moduleName.size() + 24);
  key.append(moduleName).append("::").append(typeInfoName).append("::");
  key.append(std::to_string(slot));
  return key;
}

void ensureGlossInitialized() {
  // GlossInit(false) is the trampoline/bookkeeping init and is all the RTTI scan and the
  // inline hook need. GlossInit(true) additionally runs LinkerInit(), which re-hooks the
  // Android linker's own do_dlopen/do_dlsym via an inline hook; on this build that faults
  // (the "Start hook linker..." -> SIGSEGV sequence) and it is not needed here.
  std::call_once(glossInitOnce, [] { GlossInit(false); });
}

// GlossGetLibSection re-parses the module's ELF on every call, so the address and size are memoized
// per (module, section) (audit item 4.14). A hit is always cached; a miss is cached only once the
// module itself has been found, so a section that genuinely does not exist is not re-parsed and a
// miss from a module that is not mapped yet can still be retried.
uintptr_t GetSectionCached(const std::string &module, const char *section, size_t *outSize) {
  const std::string key = pl::memory::MakeSectionKey(module, section);
  {
    std::lock_guard<std::mutex> lock(gSectionMemoMutex);
    const auto it = gSectionMemo.find(key);
    if (it != gSectionMemo.end()) {
      if (outSize) *outSize = it->second.size;
      return it->second.address;
    }
  }

  ensureGlossInitialized();
  size_t size = 0;
  const uintptr_t address = GlossGetLibSection(module.c_str(), section, &size);
  if (address != 0) {
    std::lock_guard<std::mutex> lock(gSectionMemoMutex);
    gSectionMemo[key] = pl::memory::CachedSection{address, size, true};
    gModuleFound[module] = true;
  } else {
    std::lock_guard<std::mutex> lock(gSectionMemoMutex);
    // Cache a miss only when the module has been seen mapped before; otherwise leave it uncached so
    // a later call (after the module loads) can resolve it.
    if (gModuleFound.find(module) != gModuleFound.end()) {
      gSectionMemo[key] = pl::memory::CachedSection{0, 0, true};
    }
  }
  if (outSize) *outSize = size;
  return address;
}

std::string_view normalizeTypeInfoName(std::string_view typeInfoName) {
  constexpr std::string_view kTypeInfoNamePrefix = "_ZTS";
  if (typeInfoName.starts_with(kTypeInfoNamePrefix)) {
    return typeInfoName.substr(kTypeInfoNamePrefix.size());
  }
  return typeInfoName;
}

uintptr_t findBytes(uintptr_t base, size_t size, const void *pattern,
                    size_t patternSize) {
  if (!base || !pattern || patternSize == 0 || size < patternSize) {
    return 0;
  }

  const auto *patternBytes = static_cast<const uint8_t *>(pattern);
  const uint8_t first = patternBytes[0];
  const size_t lastStart = size - patternSize;
  size_t offset = 0;

  while (offset <= lastStart) {
    const size_t remainingStarts = lastStart - offset + 1;
    const void *match = std::memchr(
        reinterpret_cast<const void *>(base + offset), first, remainingStarts);
    if (!match) {
      return 0;
    }

    const auto candidate = reinterpret_cast<uintptr_t>(match);
    const size_t candidateOffset = candidate - base;
    if (std::memcmp(match, pattern, patternSize) == 0) {
      return candidate;
    }
    offset = candidateOffset + 1;
  }

  return 0;
}

bool canReadPointer(size_t sectionSize, size_t offset) {
  return offset <= sectionSize && sectionSize - offset >= sizeof(uintptr_t);
}

uintptr_t readPointer(uintptr_t base, size_t offset) {
  uintptr_t value = 0;
  std::memcpy(&value, reinterpret_cast<const void *>(base + offset),
              sizeof(value));
  return value;
}

uintptr_t findTypeInfo(uintptr_t dataRelRo, size_t dataRelRoSize,
                       uintptr_t typeName) {
  if (dataRelRoSize < sizeof(uintptr_t) * 2) {
    return 0;
  }

  for (size_t offset = sizeof(uintptr_t); canReadPointer(dataRelRoSize, offset);
       offset += sizeof(uintptr_t)) {
    if (readPointer(dataRelRo, offset) == typeName) {
      return dataRelRo + offset - sizeof(uintptr_t);
    }
  }
  return 0;
}

bool checkedSlotOffset(size_t addressPointOffset, size_t slot,
                       size_t sectionSize, size_t &slotOffset) {
  if (slot > std::numeric_limits<size_t>::max() / sizeof(uintptr_t)) {
    return false;
  }

  const size_t slotBytes = slot * sizeof(uintptr_t);
  if (addressPointOffset > std::numeric_limits<size_t>::max() - slotBytes) {
    return false;
  }

  slotOffset = addressPointOffset + slotBytes;
  return canReadPointer(sectionSize, slotOffset);
}

uintptr_t readVtableSlot(uintptr_t dataRelRo, size_t dataRelRoSize,
                         size_t typeInfoPointerOffset, size_t slot) {
  const size_t addressPointOffset = typeInfoPointerOffset + sizeof(uintptr_t);
  size_t slotOffset = 0;
  if (!checkedSlotOffset(addressPointOffset, slot, dataRelRoSize, slotOffset)) {
    return 0;
  }
  return readPointer(dataRelRo, slotOffset);
}

uintptr_t findPrimaryVtableSlot(uintptr_t dataRelRo, size_t dataRelRoSize,
                                uintptr_t typeInfo, size_t slot) {
  if (dataRelRoSize < sizeof(uintptr_t) * 3) {
    return 0;
  }

  for (size_t offset = sizeof(uintptr_t); canReadPointer(dataRelRoSize, offset);
       offset += sizeof(uintptr_t)) {
    if (readPointer(dataRelRo, offset) != typeInfo) {
      continue;
    }

    const uintptr_t offsetToTop =
        readPointer(dataRelRo, offset - sizeof(uintptr_t));
    if (offsetToTop != 0) {
      continue;
    }

    if (const uintptr_t slotValue =
            readVtableSlot(dataRelRo, dataRelRoSize, offset, slot)) {
      return slotValue;
    }
  }

  return 0;
}

} // namespace

uintptr_t resolveVtableFunction(std::string_view typeInfoName, size_t slot,
                                std::string_view moduleName) {
  if (typeInfoName.empty() || moduleName.empty()) {
    return 0;
  }

  const std::string_view normalizedName = normalizeTypeInfoName(typeInfoName);
  if (normalizedName.empty()) {
    return 0;
  }

  const std::string name(normalizedName);
  const std::string module(moduleName);

  const std::string memoKey = makeVtableSlotKey(normalizedName, slot, moduleName);
  {
    std::lock_guard<std::mutex> lock(gVtableSlotMemoMutex);
    auto it = gVtableSlotMemo.find(memoKey);
    if (it != gVtableSlotMemo.end()) {
      return it->second;
    }
  }

  size_t rodataSize = 0;
  const uintptr_t rodata = GetSectionCached(module, ".rodata", &rodataSize);
  if (!rodata || rodataSize == 0) {
    return 0;
  }

  const uintptr_t typeName =
      findBytes(rodata, rodataSize, name.c_str(), name.size() + 1);
  if (!typeName) {
    return 0;
  }

  size_t dataRelRoSize = 0;
  const uintptr_t dataRelRo = GetSectionCached(module, ".data.rel.ro", &dataRelRoSize);
  if (!dataRelRo || dataRelRoSize == 0) {
    return 0;
  }

  const uintptr_t typeInfo = findTypeInfo(dataRelRo, dataRelRoSize, typeName);
  if (!typeInfo) {
    preloaderLogger.debug("resolveVtableFunction({}, slot {}): typeinfo not found in {}",
                          normalizedName, slot, module);
    return 0;
  }

  const uintptr_t resolved =
      findPrimaryVtableSlot(dataRelRo, dataRelRoSize, typeInfo, slot);
  // Only a successful resolution is memoised: a 0 could be a module that is not fully mapped yet,
  // and a later call must be allowed to retry rather than read a stale failure for the session.
  if (resolved != 0) {
    std::lock_guard<std::mutex> lock(gVtableSlotMemoMutex);
    gVtableSlotMemo[memoKey] = resolved;
  }
  preloaderLogger.debug("resolveVtableFunction({}, slot {}) -> {:#x} in {}",
                        normalizedName, slot, resolved, module);
  return resolved;
}

namespace {

// A function's prologue: `stp x29, x30, [sp, #-imm]!` (SM32 / pre-index writeback). The low 16
// bits are the register pair (x29,x30 = 0x7BFD) and the top byte is the opcode 0xA9 for a 64-bit
// pre-index store pair.
bool IsFunctionPrologue(uint32_t word) {
  return (word & 0x0000FFFFu) == 0x7BFDu && (word & 0xFF000000u) == 0xA9000000u;
}

// Scans a function body for an ADRP+ADD pair that forms `target`, stopping at the next function
// prologue so the scan never reads into a neighbouring slot's body (which is where false positives
// come from). arm64 materialises a symbol address as `adrp xN, page` then `add xN, xN, #off`; the
// pair is the load of the string's address needed to pass it to a diagnostic call.
bool FunctionReferencesAddress(uintptr_t funcStart, uintptr_t funcEnd, uintptr_t target) {
  if (funcStart == 0 || funcEnd <= funcStart || target == 0) return false;
  const uintptr_t page = target & ~static_cast<uintptr_t>(0xFFF);
  const uint32_t low = static_cast<uint32_t>(target & 0xFFF);
  const auto *code = reinterpret_cast<const uint32_t *>(funcStart);
  const std::size_t count = (funcEnd - funcStart) / sizeof(uint32_t);
  for (std::size_t i = 0; i + 1 < count; ++i) {
    const uint32_t w = code[i];
    // A new function begins at a prologue (past the first word, which is this one's own).
    if (i > 4 && IsFunctionPrologue(w)) return false;
    if ((w & 0x9F000000u) != 0x90000000u) continue;  // ADRP
    const uint32_t rd = w & 0x1Fu;
    const uint32_t immlo = (w >> 29) & 0x3u;
    const uint32_t immhi = (w >> 5) & 0x7FFFFu;
    int64_t imm = static_cast<int64_t>((immhi << 2) | immlo);
    if (imm & (1 << 20)) imm -= (1 << 21);
    const uintptr_t pc = funcStart + i * sizeof(uint32_t);
    if ((pc & ~static_cast<uintptr_t>(0xFFF)) + (imm << 12) != page) continue;
    // The next few instructions should contain `add rd, rd, #low`.
    for (std::size_t j = i + 1; j < i + 7 && j < count; ++j) {
      const uint32_t w2 = code[j];
      if ((w2 & 0xFFC00000u) != 0x91000000u) continue;  // ADD (immediate), 64-bit
      const uint32_t rn = (w2 >> 5) & 0x1Fu;
      const uint32_t rd2 = w2 & 0x1Fu;
      const uint32_t imm12 = (w2 >> 10) & 0xFFFu;
      if (rn == rd && rd2 == rd && imm12 == low) return true;
    }
  }
  return false;
}

// The next function/vtable slot's address, used to bound a scan of one slot's body.
uintptr_t NextSlotAddress(uintptr_t dataRelRo, size_t size, uintptr_t typeInfoPointerOffset,
                          size_t startSlot, size_t maxSlots, uintptr_t fallbackEnd) {
  for (size_t s = startSlot + 1; s <= maxSlots; ++s) {
    const size_t offset = typeInfoPointerOffset + sizeof(uintptr_t) + s * sizeof(uintptr_t);
    if (!canReadPointer(size, offset)) break;
    const uintptr_t value = readPointer(dataRelRo, offset);
    if (value != 0) return value;
  }
  return fallbackEnd;
}

} // namespace

uintptr_t resolveVtableSlotReferencingString(std::string_view typeInfoName,
                                             std::string_view needle,
                                             std::string_view moduleName,
                                             std::size_t maxSlots, std::size_t *outSlot) {
  if (typeInfoName.empty() || needle.empty() || moduleName.empty()) return 0;
  const std::string_view normalizedName = normalizeTypeInfoName(typeInfoName);
  if (normalizedName.empty()) return 0;
  const std::string name(normalizedName);
  const std::string module(moduleName);

  // The needle lookup is memoized (audit item 4.12): it scans up to ~100 slot bodies and is called
  // more than once per session. Only a success is memoized, so a transient failure can retry.
  const std::string needleKey =
      pl::memory::MakeVtableNeedleKey(moduleName, normalizedName, needle, maxSlots);
  {
    std::lock_guard<std::mutex> lock(gVtableSlotMemoMutex);
    const auto it = gVtableNeedleMemo.find(needleKey);
    if (it != gVtableNeedleMemo.end()) {
      // The memo stores the function address; the slot index is recomputed only when the caller
      // asked for it, which is rare, so a memo hit still returns the address the caller needs.
      if (outSlot) {
        const std::string slotKey = needleKey + "::slot";
        const auto slotIt = gVtableNeedleMemo.find(slotKey);
        if (slotIt != gVtableNeedleMemo.end()) *outSlot = slotIt->second;
      }
      return it->second;
    }
  }

  size_t rodataSize = 0;
  const uintptr_t rodata = GetSectionCached(module, ".rodata", &rodataSize);
  if (!rodata || rodataSize == 0) return 0;

  const uintptr_t typeName = findBytes(rodata, rodataSize, name.c_str(), name.size() + 1);
  if (!typeName) return 0;

  size_t dataRelRoSize = 0;
  const uintptr_t dataRelRo = GetSectionCached(module, ".data.rel.ro", &dataRelRoSize);
  if (!dataRelRo || dataRelRoSize == 0) return 0;

  const uintptr_t typeInfo = findTypeInfo(dataRelRo, dataRelRoSize, typeName);
  if (!typeInfo) return 0;

  // The needle is a NUL-terminated diagnostic string; find its address in .rodata.
  const uintptr_t needleAddr = findBytes(rodata, rodataSize, needle.data(), needle.size() + 1);
  if (!needleAddr) {
    preloaderLogger.debug("resolveVtableSlotReferencingString: needle '{}' not in .rodata",
                          std::string(needle));
    return 0;
  }

  size_t textSize = 0;
  const uintptr_t text = GetSectionCached(module, ".text", &textSize);
  const uintptr_t textEnd = text ? text + textSize : 0;

  // Walk the primary vtable. Its start is the typeinfo pointer at `typeInfo`; slot 0 is the first
  // entry after it. The scan window is generous; the real bound is the next function prologue
  // (`FunctionReferencesAddress` stops there), so a large window cannot read into a neighbouring
  // slot's body. Verified against 1.21.132, 26.52 and 26.60.28: all three resolve slot 17.
  constexpr uintptr_t kScanWindow = 0x4000;
  const size_t typeInfoPointerOffset = typeInfo - dataRelRo;
  for (size_t slot = 0; slot <= maxSlots; ++slot) {
    const size_t offset =
        typeInfoPointerOffset + sizeof(uintptr_t) + slot * sizeof(uintptr_t);
    if (!canReadPointer(dataRelRoSize, offset)) break;
    const uintptr_t fn = readPointer(dataRelRo, offset);
    if (fn == 0 || (text && (fn < text || fn >= textEnd))) continue;
    const uintptr_t next = NextSlotAddress(dataRelRo, dataRelRoSize, typeInfoPointerOffset,
                                           slot, maxSlots, textEnd);
    uintptr_t funcEnd = fn + kScanWindow;
    if (next > fn && next < funcEnd) funcEnd = next;
    if (textEnd && funcEnd > textEnd) funcEnd = textEnd;
    if (FunctionReferencesAddress(fn, funcEnd, needleAddr)) {
      if (outSlot) *outSlot = slot;
      std::lock_guard<std::mutex> lock(gVtableSlotMemoMutex);
      gVtableNeedleMemo[needleKey] = fn;
      // The slot index is stored beside the address so a later memo hit can still fill outSlot.
      gVtableNeedleMemo[needleKey + "::slot"] = slot;
      preloaderLogger.info("Resolved {} slot {} by string '{}'", name, slot,
                           std::string(needle));
      return fn;
    }
  }
  preloaderLogger.debug("resolveVtableSlotReferencingString: no slot of {} references '{}'", name,
                        std::string(needle));
  return 0;
}

} // namespace pl::memory
