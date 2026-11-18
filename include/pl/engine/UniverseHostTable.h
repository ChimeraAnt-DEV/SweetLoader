#pragma once

/**
 * @file UniverseHostTable.h
 * @brief Builds the `universe_host_t` table a `.universe` mod receives.
 *
 * <p>The table is always fully populated: every function pointer is non-null, because each function
 * reports its own "not available on this build" per call (an unresolved signature returns a 0
 * handle, a missing BerryAPI returns a null version string). The *reduction* to a specific mod's
 * declared permissions is what removes a capability, and it happens in the loader.
 *
 * <p><b>Where permissions are enforced.</b> Not here. This builds the *full* table. The reduction
 * to a specific mod's declared permissions happens in the loader (the Rust `universe-core::host`
 * filter), immediately before the mod's `universe_init`, so the enforcement lives next to the call
 * it guards and is unit-tested off-device. This module only has to make each capability correct and
 * safe on its own.
 *
 * <p><b>Lifetime.</b> The table is built once and lives for the process; the pointer is stable, so
 * the loader may keep it. The mod's reduced copy is owned by the loader.
 */

#include "pl/universe/universe_api.h"

/**
 * @brief The detour veneer's record stub.
 *
 * <p>Each register-preserving veneer calls this with its slot index when the hooked function runs;
 * it marks that slot pending (and returns the original trampoline the veneer tail-branches to). It
 * is `extern "C"` and at global scope because the veneer's hand-written asm calls it by name.
 */
extern "C" void *pl_universe_host_record(long index);

namespace pl::engine {

/**
 * @brief The full host table, built once.
 *
 * <p>Returns a pointer to a process-lifetime `universe_host_t` whose `size`/`abi_version` describe
 * this build. Never null, and every capability pointer is non-null — each function reports its own
 * "unavailable on this build" per call. A mod's *reduced* copy (from the Rust loader) is where a
 * capability is withdrawn.
 */
const universe_host_t *UniverseFullHostTable();

/**
 * @brief Fires each registered mod hook's callback once if its hooked function ran since the last
 *        call, then clears the pending flag.
 *
 * <p>A `.universe` mod registers a hook on a byte signature through `register_hook`; the detour
 * that lands on that signature is a register-preserving passthrough, so the mod's own callback is
 * never entered from the hooked function's (unknown) ABI. Instead the passthrough records that the
 * function ran, and this pump — called from the loader's frame tick — invokes the callback once per
 * frame the function fired, with the game's frame number. That is what "called once per frame the
 * hooked function runs" means, and it keeps an unknown-signature hook from ever calling into mod
 * code on the hooked thread.
 *
 * @param frame the game's monotonic frame counter, passed to the mod callback.
 */
void UniversePumpHostHooks(unsigned long long frame);

/** Removes every registered host hook. For session teardown and tests. */
void UniverseResetHostHooks();

} // namespace pl::engine
