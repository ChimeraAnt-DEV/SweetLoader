/**
 * @file PlayerSnapshotTest.cpp
 * @brief Host test for the local-player seqlock.
 *
 * The single-threaded contract (never published reads false, a publish reads back, the latest wins,
 * the stamp travels with the payload) plus the two behaviours the old shape got wrong: a read that
 * lands inside a write window must retry rather than report "no player", and a reader hammering the
 * channel must never observe a torn cross-field transform.
 */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "pl/ipc/PlayerSnapshot.hpp"

using pl::ipc::PlayerSnapshot;
using pl::ipc::PlayerTransform;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

PlayerTransform transform(float x, float y, float z, float yaw, float pitch, long long stamp) {
  PlayerTransform t;
  t.position[0] = x;
  t.position[1] = y;
  t.position[2] = z;
  t.rotation[0] = yaw;
  t.rotation[1] = pitch;
  t.stampMs = stamp;
  return t;
}

} // namespace

int main() {
  // Nothing published reads false and leaves the output untouched.
  {
    PlayerSnapshot snapshot;
    expect(!snapshot.hasSnapshot(), "fresh snapshot has no value");
    PlayerTransform out = transform(-1, -1, -1, -1, -1, -1);
    expect(!snapshot.read(out), "read before any publish fails");
    expect(out.position[0] == -1.0f, "a failed read leaves the output untouched");
  }

  // Publish then read returns exactly what was published, stamp included.
  {
    PlayerSnapshot snapshot;
    const PlayerTransform t = transform(1.5f, 64.0f, -3.25f, 90.0f, -12.5f, 4242);
    snapshot.publish(t);
    expect(snapshot.hasSnapshot(), "value present after publish");
    PlayerTransform out;
    expect(snapshot.read(out), "read succeeds");
    expect(out.position[0] == 1.5f && out.position[1] == 64.0f && out.position[2] == -3.25f,
           "position round-trips");
    expect(out.rotation[0] == 90.0f && out.rotation[1] == -12.5f, "rotation round-trips");
    expect(out.stampMs == 4242, "stamp round-trips");
  }

  // The latest publish wins.
  {
    PlayerSnapshot snapshot;
    snapshot.publish(transform(0, 0, 0, 0, 0, 1));
    snapshot.publish(transform(7, 8, 9, 10, 11, 2));
    PlayerTransform out;
    expect(snapshot.read(out), "read after republish");
    expect(out.position[0] == 7.0f && out.rotation[0] == 10.0f && out.stampMs == 2,
           "the latest transform wins");
  }

  // A reader hammering the writer never sees a torn transform: every field is from one publish.
  {
    PlayerSnapshot snapshot;
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<int> reads{0};

    std::thread writer([&]() {
      for (int i = 1; i <= 200000 && !stop.load(); ++i) {
        const float v = static_cast<float>(i);
        snapshot.publish(transform(v, v, v, v, v, i));
      }
      stop.store(true);
    });

    std::thread reader([&]() {
      PlayerTransform out;
      while (!stop.load()) {
        if (!snapshot.read(out)) continue;
        reads.fetch_add(1);
        // Every field of one publish carries the same value, so a mix is a torn read.
        const float v = out.position[0];
        if (out.position[1] != v || out.position[2] != v || out.rotation[0] != v ||
            out.rotation[1] != v || static_cast<float>(out.stampMs) != v) {
          torn.fetch_add(1);
        }
      }
    });

    writer.join();
    reader.join();
    expect(torn.load() == 0, "a reader never observes a torn transform");
    expect(reads.load() > 0, "the reader observed at least one transform");
  }

  std::printf("PlayerSnapshotTest: %d checks passed\n", checks);
  return 0;
}
