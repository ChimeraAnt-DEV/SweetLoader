#pragma once

/**
 * @file PublishedBlob.hpp
 * @brief Publishes an immutable byte buffer that a reader can snapshot without a lock.
 *
 * The cosmetic frame buffer and the render geometry blob are written by the launcher thread (JNI)
 * and read by the render thread every frame. The previous shape swapped a `std::vector` in place
 * under a mutex while the render thread read the same vector lock-free: a data race, which is
 * undefined behaviour and, on ARM64, can observe a torn pointer pair and abort.
 *
 * This store publishes an immutable buffer instead. A reader takes one atomic load and holds the
 * buffer alive for the whole read; a writer builds the next buffer and swaps it in with one atomic
 * store. A reader therefore never blocks a writer, never observes a half-updated buffer, and never
 * touches the writer's lock.
 *
 * `std::atomic<std::shared_ptr<T>>` is not implemented in the NDK's libc++, so the pre-C++20
 * `std::atomic_load` / `std::atomic_store` free functions are used on a plain `shared_ptr` (the
 * same idiom as `CallbackSnapshot`).
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace pl::runtime {

/**
 * @brief An atomically published, immutable byte buffer.
 *
 * A default-constructed store publishes an empty buffer, so a reader before the first publish sees
 * "nothing published" rather than a null dereference.
 */
class PublishedBlob {
public:
  PublishedBlob() : mValue(std::make_shared<const std::vector<std::uint8_t>>()) {}

  PublishedBlob(const PublishedBlob &) = delete;
  PublishedBlob &operator=(const PublishedBlob &) = delete;

  /**
   * @brief Publishes a copy of @p data as the new buffer.
   *
   * A null pointer or zero size publishes an empty buffer, which is how the writer clears the
   * previous frame so the reader stops drawing it.
   */
  void publish(const std::uint8_t *data, std::size_t size) {
    auto next = std::make_shared<const std::vector<std::uint8_t>>();
    if (data != nullptr && size != 0) {
      next = std::make_shared<const std::vector<std::uint8_t>>(data, data + size);
    }
    std::atomic_store(&mValue, next);
  }

  /**
   * @brief Copies the published buffer into @p out.
   *
   * @return true when a non-empty buffer was published and copied; false (with @p out cleared)
   *         otherwise.
   */
  bool snapshot(std::vector<std::uint8_t> &out) const {
    const std::shared_ptr<const std::vector<std::uint8_t>> value = std::atomic_load(&mValue);
    if (value == nullptr || value->empty()) {
      out.clear();
      return false;
    }
    out = *value;
    return true;
  }

  /** The published size in bytes, or 0 when none has been published. */
  std::size_t size() const {
    const std::shared_ptr<const std::vector<std::uint8_t>> value = std::atomic_load(&mValue);
    return value == nullptr ? 0 : value->size();
  }

  /** True when a non-empty buffer is published. */
  bool empty() const { return size() == 0; }

  /**
   * @brief Pins the published buffer and returns it, so a caller can read in place without a copy.
   *
   * The returned shared pointer keeps the buffer alive for as long as the caller holds it, so the
   * writer can publish the next buffer without the reader ever seeing it change mid-read. Null is
   * never returned (a store always holds at least an empty buffer).
   */
  std::shared_ptr<const std::vector<std::uint8_t>> acquire() const {
    return std::atomic_load(&mValue);
  }

private:
  std::shared_ptr<const std::vector<std::uint8_t>> mValue;
};

} // namespace pl::runtime

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
