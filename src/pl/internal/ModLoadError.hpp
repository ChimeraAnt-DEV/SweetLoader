#pragma once

/**
 * @file ModLoadError.hpp
 * @brief The reason the last mod load failed, kept for the launcher to read.
 *
 * <p>{@code nativeLoadMod} returns a boolean, so a launcher that only sees {@code false} has to
 * guess: a missing file, a manifest that does not parse, a library that will not {@code dlopen}, and
 * a lifecycle entry that returned failure all look the same (audit item 6.17). The loader records
 * the reason here as it fails, and a second JNI call reads it back, so the UI can say what went
 * wrong instead of "could not load".
 *
 * <p>The store is a single slot: a load is a foreground, one-at-a-time operation, so the last error
 * is the one that belongs to the caller's failed load. It is cleared at the start of every attempt,
 * so a stale message can never be attributed to a later success.
 */

#include <mutex>
#include <string>
#include <utility>

namespace pl::internal::mod {

class ModLoadErrorStore {
 public:
  /** Replaces the stored message. */
  void set(std::string message) {
    std::lock_guard<std::mutex> lock(mMutex);
    mMessage = std::move(message);
  }

  /** Clears the stored message; called at the start of each load attempt. */
  void clear() {
    std::lock_guard<std::mutex> lock(mMutex);
    mMessage.clear();
  }

  /** The stored message, empty when the last attempt did not record one. */
  std::string get() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mMessage;
  }

 private:
  mutable std::mutex mMutex;
  std::string mMessage;
};

} // namespace pl::internal::mod
