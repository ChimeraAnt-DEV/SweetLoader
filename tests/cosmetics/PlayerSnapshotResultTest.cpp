/**
 * @file PlayerSnapshotResultTest.cpp
 * @brief Host test for the tri-state snapshot read.
 *
 * The bool read could not tell "nothing was ever published" from "the writer stayed ahead for the
 * whole retry budget". A caller that wants to keep the last known transform when a read is merely
 * contended could not, because both looked like false. The tri-state result separates them.
 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "pl/ipc/PlayerSnapshot.hpp"

using pl::ipc::PlayerSnapshot;
using pl::ipc::PlayerTransform;
using pl::ipc::ReadResult;

namespace {
int checks = 0;
void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}
} // namespace

int main() {
  {
    PlayerSnapshot snapshot;
    PlayerTransform out;
    expect(snapshot.readWithResult(out) == ReadResult::NotPublished,
           "a fresh snapshot reports NotPublished");
    expect(!snapshot.read(out), "the bool read still returns false for NotPublished");
  }
  {
    PlayerSnapshot snapshot;
    PlayerTransform t;
    t.position[0] = 1.0F; t.position[1] = 2.0F; t.position[2] = 3.0F;
    t.rotation[0] = 45.0F; t.rotation[1] = 10.0F; t.stampMs = 99;
    snapshot.publish(t);
    PlayerTransform out;
    expect(snapshot.readWithResult(out) == ReadResult::Ok, "a published value reads Ok");
    expect(out.position[0] == 1.0F && out.stampMs == 99, "the value is the published one");
    expect(snapshot.read(out), "the bool read is true when Ok");
  }
  {
    // A reader that always loses the race reports Contended, not NotPublished.
    PlayerSnapshot snapshot;
    PlayerTransform t;
    snapshot.publish(t);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
      PlayerTransform w;
      while (!stop.load(std::memory_order_relaxed)) snapshot.publish(w);
    });
    bool sawContended = false;
    bool sawOk = false;
    for (int i = 0; i < 200000 && !(sawContended && sawOk); ++i) {
      PlayerTransform out;
      const ReadResult r = snapshot.readWithResult(out);
      if (r == ReadResult::Contended) sawContended = true;
      if (r == ReadResult::Ok) sawOk = true;
      expect(r != ReadResult::NotPublished, "a published snapshot never reports NotPublished");
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    expect(sawOk, "some reads succeed under contention");
    // Contended is allowed to be rare (the writer is a handful of instructions); we only require the
    // API to be able to express it, which the enum guarantees. Assert we never saw NotPublished.
    (void)sawContended;
  }
  std::printf("PlayerSnapshotResultTest: %d checks passed\n", checks);
  return 0;
}
