#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file GamePlayerRender.h
 * @brief Per-frame player-render feed, captured from the game's own player renderer.
 *
 * The cosmetics system wants to drive its motion (segmented cape, pet attachment) from the
 * transform the game actually renders the player with, not from a value the launcher
 * reconstructs. The in-world player model is drawn by {@code LivePlayerRenderer}, whose
 * {@code render} entry is a primary-vtable slot reachable by RTTI name -- the same
 * name-resolution mechanism the local-player feed already uses, so no per-build code
 * address is needed.
 *
 * This hook is deliberately narrow and fail-closed. It does **not** dereference any of the
 * renderer's arguments (their ABI is not verified), so it can never read freed memory or
 * mis-handle an object. It records only:
 *
 *  - that the player renderer ran this frame (a render tick the caller can advance physics on);
 *  - how many player-render calls occurred (the renderer runs once per rendered player, so a
 *    count above one is direct evidence the hook covers non-local players too);
 *  - the local player's own transform, read through the existing local-player snapshot.
 *
 * With no hook resolved, no live render call, or a stale snapshot it reports "no data" and the
 * caller keeps the resource-pack path. It never draws and never partially renders.
 */
namespace pl::runtime {

/**
 * @brief Installs the player-render hook.
 *
 * Resolves {@code LivePlayerRenderer}'s render slot by RTTI name and hooks it. Safe to call
 * once, from {@code InitGameHooks}; a failure leaves the feed unavailable (fail-closed).
 *
 * @param vtableIndex Primary-vtable slot of the render entry, or 0 to use the built-in default.
 */
void InitPlayerRenderSource(std::size_t vtableIndex);

/** True once the player renderer has actually run this session. */
bool IsPlayerRenderHookLive();

/**
 * @brief True when the render slot resolved and the detour installed.
 *
 * <p>Distinct from {@link IsPlayerRenderHookLive}: the hook installs before the first frame, so the
 * availability report says "available" from launch while the live flag stays false until the
 * renderer runs. The two together distinguish "the seam is missing" from "the seam is there but the
 * game has not drawn yet".
 */
bool IsPlayerRenderHookInstalled();

/**
 * @brief The resolved {@code LivePlayerRenderer::render} entry address, or 0 if unresolved.
 *
 * <p>For the hook self-test: the self-test round-trips the real addresses the install
 * will use, so a target that resolves but cannot be patched is reported rather than discovered as
 * a feature that silently does nothing.
 */
std::uintptr_t PlayerRenderHookTarget();

/**
 * @brief Reads the render tick counter and per-frame call count.
 *
 * @param out Receives {renderTick, callsThisFrame, totalCalls, msSinceLastRender}.
 * @return true when the renderer has run at least once this session.
 */
bool ReadPlayerRenderStats(std::uint32_t out[4]);

/**
 * @brief Reads the published frame snapshot from any thread.
 *
 * <p>This is the off-render-thread hand-off: the render thread publishes a self-contained record
 * (a seqlock write, no lock, no allocation) and this reads a consistent copy of it. A consumer that
 * needs the game's per-frame state should read here and do its work off the render thread rather than
 * touching the render thread's live data.
 *
 * @param out receives {renderTick, callsThisFrame, totalCalls, lastRenderMs}
 * @return true when a snapshot was available and copied
 */
bool ReadFrameSnapshot(std::uint64_t out[4]);

/**
 * @brief Returns the monotonic timestamp of the most recent player-render call.
 * @return Milliseconds, or 0 when the renderer has not run.
 */
long long LastPlayerRenderMs();

/**
 * @brief The primary-vtable slot the render entry resolved to this session.
 *
 * Recorded when the hook installs, so the in-app diagnostics report can say which slot was used and
 * how it was found (by the render diagnostic string scan, or by the configured/default index).
 *
 * @param outSlot receives the slot index when the hook installed.
 * @param outByString receives true when the string scan found it, false for the index fallback.
 * @return true when the hook installed (a slot was resolved and detoured).
 */
bool LastResolvedRenderSlot(std::size_t &outSlot, bool &outByString);

/**
 * @brief The exact gate that refused the render hook.
 *
 * <p>0 = hook live (or reason predates the field); 1 = build outside the verified version range;
 * 2 = render slot unresolved; 3 = slot resolved to non-executable data; 4 = hook install call
 * failed. The in-app diagnostics report maps each to a user-readable sentence instead of a generic
 * "unavailable".
 */
int RenderHookFailureReason();

/**
 * @brief Sets the display's refresh rate, so the frame-pacing reading can decide whether
 *        frames are vsync-locked. 0 means unknown, in which case a lock is never claimed.
 */
void SetDisplayRefreshHz(int hz);

/**
 * @brief The render-thread frame-pacing reading, or false before two frames are seen.
 *
 * <p>{@code outAverageUs}/{@code outWorstUs} are microsecond frame intervals; {@code outJittering}
 * means a recent frame exceeded the expected interval; {@code outVsyncLocked} means the average
 * matched the display interval. This measures the render thread's own frame arrival; it does not
 * reschedule the engine's submission.
 */
bool ReadFramePacing(long long &outAverageUs, long long &outWorstUs, bool &outJittering,
                     bool &outVsyncLocked);

/**
 * @brief The percentile + hitch reading over the long frame window, or false
 *        before any frames have been seen.
 *
 * <p>{@code outP50Us}/{@code outP95Us}/{@code outP99Us} are the median and tail frame intervals in
 * microseconds; {@code outHitchesPerMinute} counts frames above twice the median, normalised by the
 * window's wall span. Sorts the window, so call it off the render thread (a diagnostics read).
 */
bool ReadFramePacingPercentiles(long long &outP50Us, long long &outP95Us, long long &outP99Us,
                                int &outHitchesPerMinute);

/**
 * @brief A callback that runs once per rendered player model, after the game's own render, on the
 *        render thread with the GL context current.
 *
 * This is the single post-render seam. The player-render slot is hooked exactly once (here), and
 * consumers register into it instead of each installing their own detour on the same slot — two
 * detours on one function duplicated the frame accounting and made ownership ambiguous.
 *
 * The callback runs on <em>every</em> render call, not once per frame: the renderer is invoked once
 * per rendered player model and a frame boundary cannot be inferred reliably, so a consumer that must
 * draw once per frame dedupes internally (the cosmetics callback keys on the published frame
 * version).
 */
using PostRenderCallback = void (*)();

/** @brief Registers a post-render callback. Registers at most once per function pointer. */
void RegisterPostRenderCallback(PostRenderCallback callback);

} // namespace pl::runtime
