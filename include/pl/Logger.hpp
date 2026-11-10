#pragma once

/**
 * @file Logger.hpp
 * @brief Lightweight Android logger used by native mods.
 */

#include <android/log.h>

#include <fmt/format.h>
#include <fmt/std.h>

#include <atomic>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "pl/runtime/LogRing.hpp"

// The lowest level compiled in. The launcher build can define this to ANDROID_LOG_WARN for a
// release, so an INFO line costs neither the format string nor the branch (audit item 2.8). The
// default keeps every level, matching the previous behaviour.
#ifndef PRELOADER_MIN_LOG_LEVEL
#define PRELOADER_MIN_LOG_LEVEL ANDROID_LOG_DEBUG
#endif

namespace pl::log {

/**
 * @brief Named Android logger with fmt-style messages.
 *
 * <p><b>Cost.</b> Formatting is the expensive part of a log line, and the log sites here are
 * reached from the game's own input and render paths, so a disabled line must not format. Every
 * method checks the level first and returns before `fmt::vformat` runs; a level check is one
 * relaxed atomic load. `debug` and `verbose` additionally compile out entirely when `NDEBUG` is
 * set, so a release build carries neither the format string nor the branch.
 */
class Logger {
public:
  explicit Logger(std::string name) : mLoggerName(std::move(name)) {}

  /**
   * @brief The lowest level that is emitted, as an {@code ANDROID_LOG_*} value.
   *
   * <p>One relaxed atomic, read on every call. The launcher can raise it at runtime to quieten a
   * session without rebuilding; a release build starts at {@code ANDROID_LOG_INFO} and a debug
   * build at {@code ANDROID_LOG_DEBUG}.
   */
  static int level() {
    return sLevel.load(std::memory_order_relaxed);
  }

  /** Sets the minimum level that is emitted. */
  static void setLevel(int androidLevel) {
    sLevel.store(androidLevel, std::memory_order_relaxed);
  }

  /** True when a message at [androidLevel] would be emitted. */
  static bool enabled(int androidLevel) {
    return androidLevel >= sLevel.load(std::memory_order_relaxed);
  }

  /**
   * @brief Returns a process-wide logger for the given tag.
   */
  static Logger &getOrCreate(std::string name) {
    std::lock_guard<std::mutex> lock(sLoggerMutex);

    auto it = sLoggers.find(name);
    if (it != sLoggers.end()) {
      return *it->second;
    }

    auto logger = std::make_unique<Logger>(std::move(name));
    auto &ref = *logger;
    sLoggers.emplace(ref.mLoggerName, std::move(logger));
    return ref;
  }

  /**
   * @brief Writes a verbose log line. Compiled out in release builds.
   */
  template <typename... Args>
  void verbose(std::string_view fmtStr, Args &&...args) const {
    if constexpr (ANDROID_LOG_VERBOSE < PRELOADER_MIN_LOG_LEVEL) {
      (void) fmtStr;
      ((void) args, ...);
    } else {
      log(ANDROID_LOG_VERBOSE, fmtStr, std::forward<Args>(args)...);
    }
  }

  /** @brief Writes a constant verbose message without going through `fmt::vformat`. */
  void verbose(std::string_view message) const {
    if constexpr (ANDROID_LOG_VERBOSE < PRELOADER_MIN_LOG_LEVEL) {
      (void) message;
    } else {
      logLiteral(ANDROID_LOG_VERBOSE, message);
    }
  }

  /**
   * @brief Writes an informational log line.
   */
  template <typename... Args>
  void info(std::string_view fmtStr, Args &&...args) const {
    if constexpr (ANDROID_LOG_INFO < PRELOADER_MIN_LOG_LEVEL) {
      (void) fmtStr;
      ((void) args, ...);
    } else {
      log(ANDROID_LOG_INFO, fmtStr, std::forward<Args>(args)...);
    }
  }

  /** @brief Writes a constant informational message without going through `fmt::vformat`. */
  void info(std::string_view message) const {
    if constexpr (ANDROID_LOG_INFO < PRELOADER_MIN_LOG_LEVEL) {
      (void) message;
    } else {
      logLiteral(ANDROID_LOG_INFO, message);
    }
  }

  /**
   * @brief Writes a debug log line. Compiled out in release builds.
   */
  template <typename... Args>
  void debug(std::string_view fmtStr, Args &&...args) const {
    if constexpr (ANDROID_LOG_DEBUG < PRELOADER_MIN_LOG_LEVEL) {
      (void) fmtStr;
      ((void) args, ...);
    } else {
      log(ANDROID_LOG_DEBUG, fmtStr, std::forward<Args>(args)...);
    }
  }

  /** @brief Writes a constant debug message without going through `fmt::vformat`. */
  void debug(std::string_view message) const {
    if constexpr (ANDROID_LOG_DEBUG < PRELOADER_MIN_LOG_LEVEL) {
      (void) message;
    } else {
      logLiteral(ANDROID_LOG_DEBUG, message);
    }
  }

  /**
   * @brief Writes a warning log line.
   */
  template <typename... Args>
  void warn(std::string_view fmtStr, Args &&...args) const {
    log(ANDROID_LOG_WARN, fmtStr, std::forward<Args>(args)...);
  }

  /** @brief Writes a constant warning without going through `fmt::vformat`. */
  void warn(std::string_view message) const { logLiteral(ANDROID_LOG_WARN, message); }

  /**
   * @brief Writes an error log line.
   */
  template <typename... Args>
  void error(std::string_view fmtStr, Args &&...args) const {
    log(ANDROID_LOG_ERROR, fmtStr, std::forward<Args>(args)...);
  }

  /** @brief Writes a constant error without going through `fmt::vformat`. */
  void error(std::string_view message) const { logLiteral(ANDROID_LOG_ERROR, message); }

private:
  std::string mLoggerName;

  inline static std::unordered_map<std::string, std::unique_ptr<Logger>>
      sLoggers{};
  inline static std::mutex sLoggerMutex{};
#ifdef NDEBUG
  inline static std::atomic<int> sLevel{ANDROID_LOG_INFO};
#else
  inline static std::atomic<int> sLevel{ANDROID_LOG_DEBUG};
#endif

  // Records a formatted line in the shared in-memory ring (audit item 10.10) and writes it to
  // logcat. The ring keeps the last few hundred lines readable from the app without `adb`.
  void emit(int androidLevel, std::string_view message) const {
    __android_log_print(androidLevel, mLoggerName.c_str(), "%s", message.data());
    std::string line;
    line.reserve(mLoggerName.size() + message.size() + 4);
    line.append(pl::runtime::LogLevelTag(androidLevel));
    line.push_back(' ');
    line.append(mLoggerName).append(": ").append(message);
    pl::runtime::SharedLogRing().append(std::move(line));
  }

  // The constant-message path: no `fmt::vformat`, no argument pack, one ring append.
  void logLiteral(int androidLevel, std::string_view message) const {
    if (!enabled(androidLevel)) {
      return;
    }
    emit(androidLevel, message);
  }

  template <typename... Args>
  void log(int androidLevel, std::string_view fmtStr, Args &&...args) const {
    // The level check comes before any formatting, so a disabled line costs one atomic load.
    if (!enabled(androidLevel)) {
      return;
    }
    try {
      const auto message =
          fmt::vformat(fmt::string_view(fmtStr.data(), fmtStr.size()),
                       fmt::make_format_args(args...));
      emit(androidLevel, message);
    } catch (const std::exception &ex) {
      __android_log_print(ANDROID_LOG_ERROR, mLoggerName.c_str(),
                          "Failed to format log message: %s", ex.what());
      const int length =
          fmtStr.size() > static_cast<size_t>(std::numeric_limits<int>::max())
              ? std::numeric_limits<int>::max()
              : static_cast<int>(fmtStr.size());
      __android_log_print(androidLevel, mLoggerName.c_str(), "%.*s", length,
                          fmtStr.data() ? fmtStr.data() : "");
    }
  }
};

} // namespace pl::log

/**
 * @brief Shared logger used by the preloader runtime itself.
 */
inline auto &preloaderLogger = pl::log::Logger::getOrCreate("Preloader");
