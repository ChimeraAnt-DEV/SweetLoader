/**
 * @file PlayerSnapshot.hpp
 * @brief A lock-free single-writer snapshot of the local player's transform.
 *
 * <p>The game thread publishes the local player's position and rotation from inside the
 * `ClientInstance::getLocalPlayer` hook; the Java UI thread reads it. The previous shape kept a plain
 * `float[3]` / `float[2]` next to the sequence counter, so:
 *
 * <ul>
 *   <li>the reader and the writer raced on the plain floats (undefined behaviour, and on ARM64 a
 *       torn cross-field read — a new x with an old y — is observable);</li>
 *   <li>the acquire half of the writer's `acq_rel` `fetch_add` does not keep the later plain float
 *       stores after the store half, so the odd sequence could be published before the payload;</li>
 *   <li>the reader returned "no player" the instant it landed inside a write window, so the UI
 *       flickered a position out of existence whenever it happened to read during a publish.</li>
 * </ul>
 *
 * <p>This class fixes all three in one place: the fields are `std::atomic<float>` read and written
 * with relaxed ordering (well-defined, and no torn cross-field read once the fences below are in
 * play), the writer issues a release fence after the odd sequence store, the reader issues an acquire
 * fence before re-reading the sequence, and the reader retries a small budget instead of giving up.
 *
 * <p>Pure (no Android, no JNI), so the ordering rules are host-tested — see
 * `tests/cosmetics/PlayerSnapshotTest.cpp` and the ThreadSanitizer run in
 * `tests/cosmetics/PlayerSnapshotTsanTest.cpp`.
 */

#include <atomic>
#include <cstdint>

namespace pl::ipc {

/** @brief One consistent local-player transform. */
struct PlayerTransform {
  /** The entity origin (a Vec3). */
  float position[3] = {0.0f, 0.0f, 0.0f};
  /** The view rotation (yaw, pitch, in degrees). */
  float rotation[2] = {0.0f, 0.0f};
  /** A monotonic millisecond stamp taken by the writer, so a reader can judge freshness. */
  long long stampMs = 0;
};

/**
 * @brief A single-writer / multi-reader snapshot of {@link PlayerTransform}.
 *
 * <p>The writer is the game thread; readers may be any thread. A read never blocks the writer and
 * never observes a half-written transform.
 */
class PlayerSnapshot {
public:
  PlayerSnapshot() = default;

  PlayerSnapshot(const PlayerSnapshot &) = delete;
  PlayerSnapshot &operator=(const PlayerSnapshot &) = delete;

  /** @brief Publishes a transform. Call only from the writer thread. */
  void publish(const PlayerTransform &in) {
    const std::uint32_t start = sequence_.load(std::memory_order_relaxed);
    sequence_.store(start + 1, std::memory_order_release);
    // The release store orders the sequence store; the payload stores below can still be hoisted
    // above it on ARM64, so a reader that sees the odd sequence could also see a new payload byte.
    // The release fence is what closes that window.
    std::atomic_thread_fence(std::memory_order_release);

    for (int i = 0; i < 3; ++i) {
      position_[i].store(in.position[i], std::memory_order_relaxed);
    }
    for (int i = 0; i < 2; ++i) {
      rotation_[i].store(in.rotation[i], std::memory_order_relaxed);
    }
    stampMs_.store(in.stampMs, std::memory_order_relaxed);

    sequence_.store(start + 2, std::memory_order_release);
  }

  /**
   * @brief Reads the latest consistent transform.
   *
   * @return true when a transform was read; false only when nothing has ever been published, or the
   *         writer stayed ahead for the whole retry budget (in which case @p out is untouched). A
   *         read that merely lands inside a write window retries rather than reporting "no player".
   */
  bool read(PlayerTransform &out) const {
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
      const std::uint32_t before = sequence_.load(std::memory_order_acquire);
      if (before == 0) return false;            // Never published.
      if ((before & 1u) != 0u) continue;        // A write is in progress; retry, do not give up.

      PlayerTransform next;
      for (int i = 0; i < 3; ++i) {
        next.position[i] = position_[i].load(std::memory_order_relaxed);
      }
      for (int i = 0; i < 2; ++i) {
        next.rotation[i] = rotation_[i].load(std::memory_order_relaxed);
      }
      next.stampMs = stampMs_.load(std::memory_order_relaxed);

      // Acquire fence before re-reading the sequence, so the payload loads above cannot be sunk
      // below it. This is the reader half of the seqlock edge.
      std::atomic_thread_fence(std::memory_order_acquire);
      const std::uint32_t after = sequence_.load(std::memory_order_relaxed);
      if (before == after) {
        out = next;
        return true;
      }
    }
    return false;
  }

  /** @brief True once at least one transform has been published. */
  [[nodiscard]] bool hasSnapshot() const {
    return sequence_.load(std::memory_order_acquire) != 0;
  }

private:
  // A publish is a handful of instructions; a reader that loses this many times is contending with a
  // very hot writer and should skip this frame rather than spin.
  static constexpr int kMaxRetries = 8;

  std::atomic<float> position_[3] = {};
  std::atomic<float> rotation_[2] = {};
  std::atomic<long long> stampMs_{0};
  std::atomic<std::uint32_t> sequence_{0};
};

static_assert(std::atomic<float>::is_always_lock_free,
              "the player snapshot relies on lock-free float atomics on the target");
static_assert(std::atomic<long long>::is_always_lock_free,
              "the player snapshot relies on lock-free 64-bit atomics on the target");

} // namespace pl::ipc
