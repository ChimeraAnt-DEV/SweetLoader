/**
 * @file OptifineAllocator.cpp
 * @brief Tier-1 "allocator" item: disabled after a crash-safe review.
 *
 * This file used to define its own `malloc`/`free`/`calloc`/... and forward them to a
 * statically linked mimalloc, with the switch flipped by a load-time constructor. That
 * interposition is removed here because it was unsound, not merely unhelpful:
 *
 *  - **It could not do what the item advertises.** The definitions were hidden by
 *    `-fvisibility=hidden`, so they never reached the game's allocations and gave no speedup.
 *  - **It hijacked the sweetloader's own frees.** The load-time constructor turned mimalloc on
 *    before `JNI_OnLoad`, so every `free` in the sweetloader forwarded to `mi_free`. A block that
 *    came from libc (for example one allocated by `strdup` inside GlossHook's xdl and then freed
 *    by `xdl_close`) faults in mimalloc. An allocator may only free memory it allocated.
 *  - **`thread_local` in `malloc` is itself unsafe on this target.** With minSdk 28 the compiler
 *    uses emulated thread-local storage, which can call `malloc` and recurse into the guard.
 *
 * The only user-visible result was the crash above, so the item is now honestly reported as
 * unavailable instead of misreported as active. The process keeps the system allocator, which
 * is the allocator every other part of the process already uses.
 *
 * Kept as a registered item (rather than deleting the file) so the launcher still receives a
 * status for id `allocator` and no crash-loop counter is consumed.
 */

#include <string>

#include "pl/runtime/OptifineMode.h"

namespace pl::runtime {
namespace {

void ApplyAllocator(bool enabled, OptifineItemState &state) {
  if (!enabled) return;
  state.status = 2;
  state.detail = "disabled: allocator interposition removed after a crash review; "
                 "the system allocator is used";
}

struct Registrar {
  Registrar() { RegisterOptifineItem(OptifineItem::Allocator, false, &ApplyAllocator); }
};
Registrar gRegistrar;

} // namespace
} // namespace pl::runtime
