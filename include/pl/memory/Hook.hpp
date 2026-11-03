#pragma once

/**
 * @file Hook.hpp
 * @brief Memory hook API.
 */

#include <cstddef>
#include <string_view>
#include <utility>

#include "pl/Export.hpp"

namespace pl::memory {

using FuncPtr = void *;

/**
 * @brief Hook chain priority. Lower values run earlier.
 */
enum class HookPriority : int {
  Highest = 0,
  High = 100,
  Normal = 200,
  Low = 300,
  Lowest = 400,
};

/**
 * @brief Installs a detour for a target function.
 *
 * @param name Diagnostic label used in log lines (e.g. "PauseMenuOpen"). May be empty.
 * @return 0 on success, -1 on a failed/refused install.
 *
 * The name is a parameter rather than a default argument: a default argument is resolved at the
 * call site and does not emit a distinct symbol, so a library built against a shorter signature
 * would import a symbol this library does not export and fail to load. There is one exported
 * signature.
 */
PL_EXPORT int hook(FuncPtr target, FuncPtr detour, FuncPtr *originalFunc,
                   HookPriority priority, std::string_view name);

/**
 * @brief Removes a detour from a target function.
 */
PL_EXPORT bool unhook(FuncPtr target, FuncPtr detour);

/**
 * @brief Removes every detour from every target and returns how many were removed.
 *
 * <p>Hot reload needs a way back to a clean slate. Reinstalling a detour on a target that
 * still carries the previous one would chain the new detour onto the old trampoline, so a reload
 * that only adds hooks accumulates them. This tears them all down first; the game's original bytes
 * are restored by the backend as each hook is deleted. It is safe to call when nothing is hooked.
 */
PL_EXPORT std::size_t unhookAll();

/**
 * @brief The number of distinct targets that currently carry a detour.
 *
 * <p>Diagnostics use this to prove the install happened once and stayed installed: the overlay
 * hooks install through a `std::call_once`, so a second init attempt must not raise the count. A
 * count that is higher than the number of hook sites means a target was hooked twice (the backend
 * chains, so that is legal, but it is worth seeing), and a count that drops to zero after an install
 * means something unhooked it.
 */
PL_EXPORT std::size_t hookCount();

/**
 * @brief RAII owner for an installed hook.
 */
class HookHandle {
public:
  HookHandle() = default;

  HookHandle(FuncPtr target, FuncPtr detour, FuncPtr *originalFunc,
             HookPriority priority = HookPriority::Normal, std::string_view name = {})
      : mTarget(target), mDetour(detour),
        mInstalled(hook(target, detour, originalFunc, priority, name) == 0) {}

  HookHandle(const HookHandle &) = delete;
  HookHandle &operator=(const HookHandle &) = delete;

  HookHandle(HookHandle &&other) noexcept { swap(other); }

  HookHandle &operator=(HookHandle &&other) noexcept {
    if (this != &other) {
      reset();
      swap(other);
    }
    return *this;
  }

  ~HookHandle() { reset(); }

  [[nodiscard]] bool installed() const noexcept { return mInstalled; }

  void reset() {
    if (mInstalled) {
      unhook(mTarget, mDetour);
      mInstalled = false;
    }
    mTarget = nullptr;
    mDetour = nullptr;
  }

  void swap(HookHandle &other) noexcept {
    std::swap(mTarget, other.mTarget);
    std::swap(mDetour, other.mDetour);
    std::swap(mInstalled, other.mInstalled);
  }

private:
  FuncPtr mTarget{};
  FuncPtr mDetour{};
  bool mInstalled{};
};

} // namespace pl::memory

