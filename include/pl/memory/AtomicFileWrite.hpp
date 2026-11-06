#pragma once

/**
 * @file AtomicFileWrite.hpp
 * @brief Writes a file so a reader never sees a truncated version of it.
 *
 * <p>The signature scan cache is written by `std::ofstream(..., trunc)`. That truncates the target
 * first, so a kill between the truncate and the last byte leaves a half-written cache. The next launch
 * then parses a truncated file; the parser rejects it, but the write also destroyed the previous good
 * cache, so the launch pays a full scan.
 *
 * <p>This helper writes to a sibling temporary file and renames it over the target. `rename(2)` within
 * one directory is atomic, so a reader sees either the old file or the complete new one, never a
 * partial write.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#if defined(__unix__) || defined(__ANDROID__) || defined(__linux__)
#include <unistd.h>
#endif

namespace pl::memory {

namespace detail {

/** A per-process counter so two writers never pick the same temporary name. */
inline std::atomic<std::uint64_t> &TempCounter() {
  static std::atomic<std::uint64_t> counter{0};
  return counter;
}

inline long CurrentPid() {
#if defined(__unix__) || defined(__ANDROID__) || defined(__linux__)
  return static_cast<long>(::getpid());
#else
  return 0;
#endif
}

} // namespace detail

/**
 * @brief Writes @p content to @p path atomically.
 *
 * @return true on success. On failure the target file is left untouched and the temporary is removed.
 */
inline bool WriteFileAtomically(const std::filesystem::path &path, std::string_view content) {
  const std::uint64_t n = detail::TempCounter().fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path temp = path;
  temp += ".tmp." + std::to_string(detail::CurrentPid()) + "." + std::to_string(n);

  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out) {
      std::error_code ignored;
      std::filesystem::remove(temp, ignored);
      return false;
    }
  }

  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    return false;
  }
  return true;
}

} // namespace pl::memory
