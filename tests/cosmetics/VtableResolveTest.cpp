// Host test for resolveVtableSlotReferencingString, the "which vtable slot references this string"
// resolver the player-render seam uses to locate its target without a byte signature.
//
// It is the only place the resolver is exercised off-device. The bug it pins: the resolver found the
// typeinfo (`_ZTI`) with findTypeInfo, then walked it as if it were the vtable (`_ZTV`). In the
// Itanium layout those are separate objects, so the walk started at the typeinfo's own fields and
// never reached a virtual slot. The fixture builds the exact object graph the real library has -- a
// typeinfo whose name field points at `18LivePlayerRenderer`, a vtable whose typeinfo-pointer slot
// points back at the typeinfo, and a render function at slot 17 that loads the needle's address -- so
// a walk from the wrong base reads the typeinfo's fields as slots and resolves the wrong index.
//
// The library is stubbed through GlossGetLibSection: a single page holding `.text`, `.rodata` and
// `.data.rel.ro` at fixed offsets, which is what lets the function's `adrp #0` reach the needle.
#include "pl/memory/Signature.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" uintptr_t GlossGetLibSection(const char *lib, const char *sec, size_t *size);
extern "C" void GlossInit(bool init_linker);

namespace {

constexpr size_t kPage = 0x1000;
constexpr uintptr_t kTextOffset = 0x000;
constexpr uintptr_t kNameOffset = 0x200;
constexpr uintptr_t kNeedleOffset = 0x280;
constexpr uintptr_t kTypeInfoOffset = 0x400;
constexpr uintptr_t kVtableOffset = 0x440;
constexpr size_t kExpectedSlot = 17;

const char *kTypeName = "18LivePlayerRenderer";
const char *kNeedle = "variable.player_x_rotation";

alignas(kPage) uint8_t gBlob[kPage];

uintptr_t blobBase() { return reinterpret_cast<uintptr_t>(gBlob); }

void write32(uint8_t *at, uint32_t value) { std::memcpy(at, &value, sizeof(value)); }
void write64(uint8_t *at, uint64_t value) { std::memcpy(at, &value, sizeof(value)); }

} // namespace

extern "C" uintptr_t GlossGetLibSection(const char *lib, const char *sec, size_t *size) {
  if (std::strcmp(lib, "libminecraftpe.so") != 0 || size == nullptr) {
    return 0;
  }
  if (std::strcmp(sec, ".text") == 0) {
    *size = 0x100;
    return blobBase() + kTextOffset;
  }
  if (std::strcmp(sec, ".rodata") == 0) {
    *size = 0x200;
    return blobBase() + kNameOffset;
  }
  if (std::strcmp(sec, ".data.rel.ro") == 0) {
    *size = 0x200;
    return blobBase() + kTypeInfoOffset;
  }
  return 0;
}

extern "C" void GlossInit(bool /*init_linker*/) {}

extern "C" int __android_log_print(int, const char *, const char *, ...) { return 0; }

// Builds the object graph and returns the render function address the resolver must find.
static uintptr_t buildFixture() {
  std::memset(gBlob, 0, sizeof(gBlob));
  const uintptr_t base = blobBase();

  std::memcpy(gBlob + kNameOffset, kTypeName, std::strlen(kTypeName) + 1);
  std::memcpy(gBlob + kNeedleOffset, kNeedle, std::strlen(kNeedle) + 1);
  const uintptr_t nameVa = base + kNameOffset;
  const uintptr_t needleVa = base + kNeedleOffset;

  // The render function: a prologue, then `adrp x0, #0; add x0, x0, #low` -- the arm64 pair that
  // materialises the needle's address. Every object is in one page, so `adrp #0` reaches it.
  const uintptr_t renderFn = base + kTextOffset;
  write32(gBlob + kTextOffset + 0x0, 0xA9BF7BFDu); // stp x29, x30, [sp, #-0x10]!
  write32(gBlob + kTextOffset + 0x4, 0x90000000u); // adrp x0, #0
  const uint32_t low = static_cast<uint32_t>(needleVa & 0xFFF);
  write32(gBlob + kTextOffset + 0x8, 0x91000000u | (low << 10)); // add x0, x0, #low
  write32(gBlob + kTextOffset + 0xC, 0xD65F03C0u);               // ret

  // The typeinfo: word 1 is the name pointer, which is what findTypeInfo keys on.
  const uintptr_t typeInfo = base + kTypeInfoOffset;
  write64(gBlob + kTypeInfoOffset + 0x0, 0);
  write64(gBlob + kTypeInfoOffset + 0x8, nameVa);

  // The vtable: offset-to-top 0, the typeinfo back-pointer, then slot 17 -> render function.
  write64(gBlob + kVtableOffset + 0x0, 0);
  write64(gBlob + kVtableOffset + 0x8, typeInfo);
  write64(gBlob + kVtableOffset + (2 + kExpectedSlot) * 8, renderFn);

  return renderFn;
}

int main() {
  const uintptr_t expected = buildFixture();
  size_t slot = 0;
  const uintptr_t resolved = pl::memory::resolveVtableSlotReferencingString(
      kTypeName, kNeedle, "libminecraftpe.so", 200, &slot);
  if (resolved != expected || slot != kExpectedSlot) {
    std::printf("FAIL: resolved %#lx at slot %zu, expected %#lx at slot %zu\n",
                static_cast<unsigned long>(resolved), slot,
                static_cast<unsigned long>(expected), kExpectedSlot);
    return 1;
  }
  std::printf("PASS: resolved the render function at slot %zu\n", slot);
  return 0;
}
