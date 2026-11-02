// Host test for the hook-install prologue rule (item 7). Pure C++, no device.
#include <cstdio>

#include "pl/memory/PrologueVerify.hpp"

using pl::memory::ClassifyInstructionBoundary;
using pl::memory::ClassifyPrologue;
using pl::memory::InstructionBoundary;
using pl::memory::IsA64BranchLike;
using pl::memory::IsPlausibleInstructionBoundary;
using pl::memory::kA64Nop;
using pl::memory::PrologueVerdict;

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
  // A NOP means nothing was patched, whatever else it looks like.
  expect(ClassifyPrologue(kA64Nop) == PrologueVerdict::NotPatched, "NOP is not-patched");

  // The instructions a real install emits: B/BL, BR/BLR, LDR-literal, ADRP.
  expect(ClassifyPrologue(0x14000000U) == PrologueVerdict::Patched, "B is patched");
  expect(ClassifyPrologue(0x94000000U) == PrologueVerdict::Patched, "BL is patched");
  expect(ClassifyPrologue(0xD61F0000U) == PrologueVerdict::Patched, "BR is patched");
  expect(ClassifyPrologue(0xD63F0000U) == PrologueVerdict::Patched, "BLR is patched");
  expect(ClassifyPrologue(0x58000000U) == PrologueVerdict::Patched, "LDR literal is patched");
  expect(ClassifyPrologue(0x90000000U) == PrologueVerdict::Patched, "ADRP is patched");
  // Condition/branch-compare forms.
  expect(ClassifyPrologue(0x54000000U) == PrologueVerdict::Patched, "B.cond is patched");
  expect(ClassifyPrologue(0x34000000U) == PrologueVerdict::Patched, "CBZ is patched");
  expect(ClassifyPrologue(0x36000000U) == PrologueVerdict::Patched, "TBZ is patched");

  // RET must NOT read as patched: an unpatched leaf function starts with one, and counting it would
  // report a failed install as live.
  expect(!IsA64BranchLike(0xD65F03C0U), "RET is not branch-like");
  expect(ClassifyPrologue(0xD65F03C0U) == PrologueVerdict::Unrecognised, "RET is unrecognised");

  // A plausible unpatched prologue (stp x29, x30, [sp, #-16]!) is not a branch.
  expect(ClassifyPrologue(0xA9BF7BFDU) == PrologueVerdict::Unrecognised,
         "a stp prologue is unrecognised");

  // 0 is not a NOP, and not a branch.
  expect(ClassifyPrologue(0U) == PrologueVerdict::Unrecognised, "zero is unrecognised");

  // --- Pre-install instruction-boundary filter (item 451) ------------------------------------
  // A real prologue is plausible.
  expect(ClassifyInstructionBoundary(0xA9BF7BFDU) == InstructionBoundary::Plausible,
         "a stp prologue is a plausible boundary");
  expect(ClassifyInstructionBoundary(0x14000000U) == InstructionBoundary::Plausible,
         "a B is a plausible boundary");
  expect(ClassifyInstructionBoundary(kA64Nop) == InstructionBoundary::Plausible,
         "a NOP is a plausible boundary (it is a real instruction)");

  // The two shapes that always mean a wrong address.
  expect(ClassifyInstructionBoundary(0U) == InstructionBoundary::ZeroWord,
         "an all-zero word is a wrong address");
  expect(ClassifyInstructionBoundary(0xFFFFFFFFU) == InstructionBoundary::AllOnes,
         "an all-ones word is a wrong address");
  expect(ClassifyInstructionBoundary(0x00000000U) == InstructionBoundary::ZeroWord,
         "the zero word is classified before the reserved family");
  expect(ClassifyInstructionBoundary(0x0000FFFFU) == InstructionBoundary::Reserved,
         "the UDF family is reserved");
  expect(!IsPlausibleInstructionBoundary(0U), "zero is not a plausible boundary");
  expect(!IsPlausibleInstructionBoundary(0xFFFFFFFFU), "all-ones is not a plausible boundary");
  expect(IsPlausibleInstructionBoundary(0xA9BF7BFDU), "a real prologue is plausible");

  std::printf("PrologueVerifyTest: %d checks passed\n", checks);
  return failures == 0 ? 0 : 1;
}
