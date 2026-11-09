#pragma once

/**
 * @file HighWaterMark.hpp
 * @brief Raises an atomic high-water mark without ever lowering it.
 *
 * <p>Used by the render-callback timing aggregate (audit item 3.12): the published "worst single
 * callback time" must reflect the worst value ever observed, not just the last frame's, so a one-off
 * stall is not overwritten by the next cheap frame before a diagnostics reader polls it.
 *
 * <p>Pure, so the never-lower rule and its concurrency are host-tested.
 */

#include <atomic>
#include <cstdint>

namespace pl::runtime {

/**
 * @brief Raises @p mark to @p value if @p value is greater, leaving it unchanged otherwise.
 *
 * <p>The compare-exchange loop is safe under concurrent raisers: the mark only ever increases, so a
 * reader never observes it go backwards.
 */
inline void RaiseHighWaterMark(std::atomic<std::uint32_t> &mark, std::uint32_t value) noexcept {
  std::uint32_t previous = mark.load(std::memory_order_relaxed);
  while (value > previous &&
         !mark.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
  }
}

} // namespace pl::runtime
