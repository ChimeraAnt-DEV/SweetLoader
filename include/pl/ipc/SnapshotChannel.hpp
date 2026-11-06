#pragma once

/**
 * @file SnapshotChannel.hpp
 * @brief A lock-free single-writer snapshot channel for handing game data off the render thread.
 *
 * <p>The render seam runs on the render thread and a frame budget is unforgiving: any module that
 * wants to read the game's per-frame state (camera, player transform, cosmetic frame) must not make
 * the render thread wait on it, and must not hold a pointer into data the render thread can free
 * underneath it. The previous shape — the render thread publishing a buffer and a consumer reading it
 * while it was being rewritten — is what a torn read (a new header with an old payload) looks like,
 * and a consumer that dereferenced the render thread's live buffer is what a SIGABRT looks like.
 *
 * <p>This channel is the separation: the render thread only ever <em>writes</em> a self-contained
 * snapshot (a seqlock publish, no allocation, no lock), and a consumer on any thread only ever
 * <em>reads</em> a consistent copy. The heavy module work then runs off the render thread against the
 * copy. There is no shared pointer across the boundary — the channel owns its buffer.
 *
 * <p><b>Memory ordering.</b> A seqlock: the writer bumps the sequence to odd before touching the
 * buffer and to even after; the reader reads the sequence, copies, then re-reads it and retries if it
 * changed or was odd. Two fences make the ordering edge real on a weakly ordered CPU (ARM64):
 *
 * <ul>
 *   <li>the writer issues a <em>release fence</em> after the odd sequence store, so the payload
 *       stores cannot be reordered before it;</li>
 *   <li>the reader issues an <em>acquire fence</em> before re-reading the sequence, so the payload
 *       loads cannot be reordered after it.</li>
 * </ul>
 *
 * <p>The payload itself is copied word-at-a-time through `std::atomic_ref` with relaxed ordering.
 * A plain `memcpy` racing another thread's plain `memcpy` is a data race — undefined behaviour —
 * even when the sequence check discards the result, and on ARM64 it can let a reader observe a
 * half-written payload that still carries an even sequence. Atomic relaxed accesses make the race
 * well-defined; the fences above supply the ordering that makes the observed value consistent.
 *
 * <p>The class is pure (no Android, no JNI), so the ordering rules are host-tested — see
 * `tests/cosmetics/SnapshotChannelTest.cpp` and the ThreadSanitizer run in
 * `tests/cosmetics/SnapshotChannelTsanTest.cpp`.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace pl::ipc {

/**
 * @brief A single-writer / multi-reader snapshot channel of fixed-capacity byte payloads.
 *
 * <p>The writer is expected to be the render thread; readers may be any thread. Capacity is fixed at
 * construction, so publishing never allocates.
 */
class SnapshotChannel {
public:
  /** @brief A publish result. */
  enum class PublishResult {
    Ok,       ///< the payload was published
    TooLarge, ///< the payload exceeds the channel capacity; nothing was written
  };

  /** @brief Creates a channel with room for @p capacity payload bytes. */
  explicit SnapshotChannel(std::size_t capacity)
      : capacity_(capacity), buffer_(capacity, 0) {}

  /** @brief The maximum payload size this channel accepts. */
  [[nodiscard]] std::size_t capacity() const { return capacity_; }

  /**
   * @brief Publishes a payload. Call only from the writer thread.
   *
   * @return {@code Ok}, or {@code TooLarge} when the payload does not fit (the channel is left
   *         untouched, so a malformed payload cannot publish a truncated snapshot).
   */
  PublishResult publish(std::span<const std::uint8_t> payload) {
    if (payload.size() > capacity_) return PublishResult::TooLarge;

    // Odd sequence => a write is in progress. Release so the payload stores below are visible to a
    // reader that observes the even sequence afterwards.
    const std::uint32_t start = sequence_.load(std::memory_order_relaxed);
    sequence_.store(start + 1, std::memory_order_release);
    // The release store orders the *sequence* store, not the following payload stores: on ARM64 the
    // payload stores can still be hoisted above it. The release fence is what keeps a reader that
    // sees the odd sequence from also seeing payload bytes from before the bump.
    std::atomic_thread_fence(std::memory_order_release);

    if (!payload.empty()) {
      CopyWords(buffer_.data(), payload.data(), payload.size());
    }
    length_.store(payload.size(), std::memory_order_relaxed);

    // Even sequence => the write is complete and visible.
    sequence_.store(start + 2, std::memory_order_release);
    return PublishResult::Ok;
  }

  /**
   * @brief Copies the most recent consistent snapshot into @p out. Safe from any thread.
   *
   * @return true when a snapshot was copied (false only when the channel has never been published to,
   *         or the writer stayed ahead for the retry budget — in which case @p out is untouched)
   */
  bool read(std::vector<std::uint8_t> &out) const {
    if (!hasSnapshot()) return false; // Nothing has ever been published.
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
      const std::uint32_t before = sequence_.load(std::memory_order_acquire);
      if ((before & 1u) != 0u) continue; // A write is in progress; retry.
      const std::size_t len = length_.load(std::memory_order_relaxed);
      if (len > capacity_) continue; // Defensive: never trust a length past capacity.
      out.resize(len);
      if (len > 0) CopyWords(out.data(), buffer_.data(), len);
      // Acquire fence before re-reading the sequence: without it the payload loads above can be
      // reordered after the load below, so the copy is not ordered against the sequence the writer
      // publishes with. This is the reader half of the seqlock edge on ARM64.
      std::atomic_thread_fence(std::memory_order_acquire);
      const std::uint32_t after = sequence_.load(std::memory_order_relaxed);
      if (before == after) return true; // No write interleaved: the copy is consistent.
    }
    return false;
  }

  /** @brief True once at least one payload has been published. */
  [[nodiscard]] bool hasSnapshot() const {
    return sequence_.load(std::memory_order_acquire) != 0;
  }

  /** @brief The length of the most recent published payload, or 0 when none. */
  [[nodiscard]] std::size_t lastLength() const { return length_.load(std::memory_order_relaxed); }

private:
  // A writer is expected to complete a publish in a few instructions; a reader that loses this many
  // times is contending with a very hot writer and should just skip this frame rather than spin.
  static constexpr int kMaxRetries = 8;

  /**
   * @brief Copies @p n bytes with relaxed atomic accesses.
   *
   * A plain `memcpy` here would race the other thread's access to the same bytes, which is undefined
   * behaviour even though the seqlock discards a torn result (and ThreadSanitizer reports it). Both
   * sides therefore go through the compiler's `__atomic` builtins with relaxed ordering, which makes
   * the access well-defined; the surrounding fences supply the ordering that makes a copied value
   * consistent.
   *
   * `__atomic_*` is used rather than `std::atomic_ref`, which the NDK's libc++ does not implement —
   * the builtins are the same atomics and are understood by both clang and gcc.
   *
   * 8-byte words are used only while <em>both</em> pointers are word-aligned — the `publish` source is
   * a caller span that may start anywhere — and byte accesses cover the head and tail. The `__atomic`
   * builtins take a non-const pointer, so the source is `const_cast`; the storage is genuinely shared
   * and mutated by the writer, which is exactly why the access must be atomic.
   */
  static void CopyWords(std::uint8_t *dst, const std::uint8_t *src, std::size_t n) {
    using Word = std::uint64_t;
    auto *s = const_cast<std::uint8_t *>(src);
    const auto wordAligned = [](const std::uint8_t *p) {
      return (reinterpret_cast<std::uintptr_t>(p) % alignof(Word)) == 0;
    };
    std::size_t i = 0;
    for (; i < n && !(wordAligned(dst + i) && wordAligned(s + i)); ++i) {
      CopyByte(dst, s, i);
    }
    for (; i + sizeof(Word) <= n; i += sizeof(Word)) {
      const Word word =
          __atomic_load_n(reinterpret_cast<Word *>(s + i), __ATOMIC_RELAXED);
      __atomic_store_n(reinterpret_cast<Word *>(dst + i), word, __ATOMIC_RELAXED);
    }
    for (; i < n; ++i) {
      CopyByte(dst, s, i);
    }
  }

  static void CopyByte(std::uint8_t *dst, std::uint8_t *src, std::size_t i) {
    const std::uint8_t value = __atomic_load_n(src + i, __ATOMIC_RELAXED);
    __atomic_store_n(dst + i, value, __ATOMIC_RELAXED);
  }

  const std::size_t capacity_;
  std::vector<std::uint8_t> buffer_;
  std::atomic<std::uint32_t> sequence_{0};
  std::atomic<std::size_t> length_{0};
};

} // namespace pl::ipc
