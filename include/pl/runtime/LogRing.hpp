#pragma once

/**
 * @file LogRing.hpp
 * @brief A bounded, thread-safe in-memory ring of the most recent log lines.
 *
 * <p>Logs go to logcat, which the app cannot read without {@code READ_LOGS} and the dev environment
 * has no {@code adb}. A native crash also takes the logcat buffer's tail with it. Keeping the last N
 * lines in memory (audit item 10.10) lets the launcher's diagnostics page show them through JNI, and
 * gives a future crash handler a breadcrumb trail (audit item 10.1's follow-up).
 *
 * <p>The ring is a fixed-capacity buffer: once full, the oldest line is dropped on each append, so
 * memory is bounded no matter how long a session runs. Every access takes one short mutex; logging is
 * not on a per-frame path, so a lock is cheaper than the complexity of a lock-free ring here.
 *
 * <p>Pure C++, so the eviction and ordering rules are host-tested.
 */

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pl::runtime {

/** @brief The default number of recent log lines retained. */
inline constexpr std::size_t kDefaultLogRingCapacity = 512;

/**
 * @brief A bounded ring of recent log lines, oldest evicted first.
 */
class LogRing {
public:
  explicit LogRing(std::size_t capacity = kDefaultLogRingCapacity)
      : mCapacity(capacity == 0 ? 1 : capacity) {}

  /** @brief Appends a line, evicting the oldest when at capacity. */
  void append(std::string line) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mLines.size() >= mCapacity) {
      mLines.pop_front();
    }
    mLines.push_back(std::move(line));
  }

  /**
   * @brief Returns up to @p maxLines of the most recent lines, oldest first.
   *
   * @param maxLines the number to return; zero returns none, and a value larger than the ring
   *                 returns everything held.
   */
  std::vector<std::string> snapshot(std::size_t maxLines) const {
    std::lock_guard<std::mutex> lock(mMutex);
    if (maxLines == 0 || mLines.empty()) return {};
    const std::size_t take = maxLines < mLines.size() ? maxLines : mLines.size();
    const auto first = mLines.end() - static_cast<std::ptrdiff_t>(take);
    return std::vector<std::string>(first, mLines.end());
  }

  /** @brief The number of lines currently retained. */
  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mLines.size();
  }

  /** @brief The maximum number of lines retained. */
  std::size_t capacity() const { return mCapacity; }

  /** @brief Drops every retained line. */
  void clear() {
    std::lock_guard<std::mutex> lock(mMutex);
    mLines.clear();
  }

private:
  const std::size_t mCapacity;
  mutable std::mutex mMutex;
  std::deque<std::string> mLines;
};

/** @brief The process-wide log ring every logger appends to. */
LogRing &SharedLogRing();

/**
 * @brief A one-character tag for an {@code ANDROID_LOG_*} level, for a ring line's prefix.
 *
 * Pure, so the mapping is host-tested. An unknown level reads as "?" rather than a letter that
 * could be mistaken for a real one.
 */
inline const char *LogLevelTag(int androidLevel) noexcept {
  switch (androidLevel) {
    case 2: return "V";  // ANDROID_LOG_VERBOSE
    case 3: return "D";  // ANDROID_LOG_DEBUG
    case 4: return "I";  // ANDROID_LOG_INFO
    case 5: return "W";  // ANDROID_LOG_WARN
    case 6: return "E";  // ANDROID_LOG_ERROR
    default: return "?";
  }
}

} // namespace pl::runtime
