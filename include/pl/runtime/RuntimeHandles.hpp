#pragma once

/**
 * @file RuntimeHandles.hpp
 * @brief The JVM/activity handles the preloader reads from several threads, made atomic.
 *
 * <p>{@code JavaRuntime.cpp} used to keep {@code JavaVM *g_vm} and {@code jobject g_activity} as
 * plain pointers. `g_vm` is written once from `JNI_OnLoad` and read from every `ScopedJniEnv`; the
 * activity is written from `nativeSetActivity` (UI thread) and read from `CallActivityVoidMethod`
 * (any thread). Neither write/read pair was ordered, so the compiler could cache the value or a
 * reader could observe a half-published pointer (audit items 3.7–3.9).
 *
 * <p>Both handles now live in one struct of atomics, so the intent ("these cross threads") is in the
 * type, and a single shutdown flag tells a thread-exit detach to stand down during JVM teardown
 * (audit item 3.10: detaching from a dead VM is undefined).
 *
 * <p>The detach decision is a pure function so it is host-tested without a device.
 */

#include <atomic>

#include <jni.h>

namespace pl::runtime {

/**
 * @brief Whether a thread that we attached should detach as it exits.
 *
 * <p>False when we did not attach the thread (the UI thread owns its own attachment), and false
 * during JVM shutdown: the VM may already be gone, and `DetachCurrentThread` on a dead VM is
 * undefined.
 */
[[nodiscard]] inline bool ShouldDetachOnThreadExit(bool mustDetachOnThreadExit,
                                                   bool vmShuttingDown) {
  return mustDetachOnThreadExit && !vmShuttingDown;
}

/** @brief The two cross-thread JVM handles plus the teardown flag, all atomically accessed. */
struct RuntimeHandles {
  std::atomic<JavaVM *> vm{nullptr};
  std::atomic<jobject> activity{nullptr};
  /** Set once JVM teardown begins, so a thread-exit detach stands down. */
  std::atomic<bool> vmShuttingDown{false};

  /** @return the captured VM, or nullptr before `JNI_OnLoad`. */
  [[nodiscard]] JavaVM *GetVm() const { return vm.load(std::memory_order_acquire); }

  /** @return the current activity global ref, or nullptr when none is set. */
  [[nodiscard]] jobject GetActivity() const { return activity.load(std::memory_order_acquire); }

  /** @return whether JVM teardown has begun. */
  [[nodiscard]] bool IsVmShuttingDown() const {
    return vmShuttingDown.load(std::memory_order_acquire);
  }
};

} // namespace pl::runtime
