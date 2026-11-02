#pragma once

#include <string>

namespace pl::runtime {

void ConfigureGameHooks(std::string rulesPath, std::string minecraftVersion);

/**
 * @brief Arms the deferred hook install.
 *
 * <p>Called from the preloader's mod-enable entry, which the game activity reaches in
 * {@code onCreate} — before the engine's graphics backend and the {@code libminecraftpe.so}
 * relocation have finished. The install itself runs from {@link PumpGameHookInstall} on the game's
 * own frame tick, so the RTTI/vtable resolution sees a fully mapped module and the code patch runs
 * on the same thread that calls the render entry. Calling this more than once is a no-op.
 */
void ArmGameHookInstall();

/**
 * @brief Runs the deferred hook install if the runtime is now up. Safe to call every frame.
 *
 * @return true once the install has run (installed or given up).
 */
bool PumpGameHookInstall();

/** True once the deferred install has produced a decision. */
bool GameHookInstallResolved();

/** True when the deferred install has not yet run (diagnostics: "waiting for the render context"). */
bool GameHookInstallPending();

/**
 * @brief The deferred install's state: 0 not armed, 1 armed and waiting, 2 resolved.
 *
 * A single int for the diagnostics report, so "the install never ran" is distinguishable from
 * "it ran and gave up".
 */
int GameHookInstallState();

/** Frame ticks the install gate observed before deciding (diagnostics). */
int GameHookInstallFrameTicks();

/**
 * @brief Installs immediately, for tests and callers that already know the runtime is up.
 *
 * <p>Idempotent: the install runs at most once per process.
 */
void InitGameHooks();

bool IsPauseMenuOpen();
bool IsHudScreenOpen();
bool IsShowingMenu();
bool ShouldForceGlobalModMenu();

/**
 * @brief Recomputes the per-feature availability table from the live hook state (item 453).
 *
 * <p>Each feature's state is derived from the accessor that already answers "did this seam install
 * and is it live", so the report cannot drift from the hooks it describes. Called before the report
 * is read, so a deferred install that has since run is reflected without the client polling twice.
 * Cheap: a handful of atomic loads and no allocation until the report itself is rendered.
 */
void RefreshGameFeatureAvailability();

/**
 * @brief Runs the hook self-test (item 452) against the real resolved targets and returns the report.
 *
 * <p>Resolves each hook site's address (via the same accessors the install uses, so the report
 * cannot describe a different target), round-trips it, and returns the result. It never leaves a
 * site hooked. Called before the real install, when the sites are untouched and the game has not yet
 * begun calling them, so a round-trip cannot disturb a live session.
 *
 * <p>Returns the cached report if one has already run this process, so a second call is cheap and
 * consistent.
 */
std::string RunGameHookSelfTest();

/** True once {@link RunGameHookSelfTest} has run. */
bool GameHookSelfTestHasRun();

} // namespace pl::runtime
