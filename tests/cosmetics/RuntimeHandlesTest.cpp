/**
 * @file RuntimeHandlesTest.cpp
 * @brief Host test for the atomic JVM/activity handles and the thread-exit detach rule.
 *
 * JavaRuntime used plain pointers for the VM and activity, read from several threads with no
 * ordering (audit items 3.7-3.9). The handles now live in RuntimeHandles (atomics), and a thread
 * must not detach during JVM teardown (audit item 3.10). Both rules are pure, so they are pinned
 * here without a device. A jni.h stub provides JavaVM/jobject for the host build.
 */

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "pl/runtime/RuntimeHandles.hpp"

using pl::runtime::RuntimeHandles;
using pl::runtime::ShouldDetachOnThreadExit;

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
  // The detach rule: detach only when we own the attachment and the VM is alive.
  expect(ShouldDetachOnThreadExit(true, false), "detach our own attachment while the VM is alive");
  expect(!ShouldDetachOnThreadExit(false, false), "never detach a thread we did not attach");
  expect(!ShouldDetachOnThreadExit(true, true), "do not detach during JVM teardown");
  expect(!ShouldDetachOnThreadExit(false, true), "no detach when we do not own it either");

  // The handles start empty.
  {
    RuntimeHandles handles;
    expect(handles.GetVm() == nullptr, "no VM before JNI_OnLoad");
    expect(handles.GetActivity() == nullptr, "no activity before it is set");
    expect(!handles.IsVmShuttingDown(), "not shutting down initially");
  }

  // The activity and shutdown flag round-trip through the atomics.
  {
    RuntimeHandles handles;
    auto *fakeActivity = reinterpret_cast<jobject>(static_cast<std::uintptr_t>(0x1234));
    handles.activity.store(fakeActivity, std::memory_order_release);
    expect(handles.GetActivity() == fakeActivity, "the activity round-trips");
    handles.vmShuttingDown.store(true, std::memory_order_release);
    expect(handles.IsVmShuttingDown(), "the shutdown flag round-trips");
  }

  // A concurrent reader sees either the old or the new activity, never a torn value. With atomics
  // the value is always one of the two pointers the writer stored.
  {
    RuntimeHandles handles;
    auto *a = reinterpret_cast<jobject>(static_cast<std::uintptr_t>(0x1000));
    auto *b = reinterpret_cast<jobject>(static_cast<std::uintptr_t>(0x2000));
    handles.activity.store(a, std::memory_order_release);
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::thread writer([&]() {
      for (int i = 0; i < 200000 && !stop.load(std::memory_order_relaxed); ++i) {
        handles.activity.store((i & 1) ? a : b, std::memory_order_release);
      }
      stop.store(true, std::memory_order_relaxed);
    });
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
      readers.emplace_back([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
          jobject seen = handles.GetActivity();
          if (seen != a && seen != b) bad.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    writer.join();
    for (auto &t : readers) t.join();
    expect(bad.load() == 0, "a concurrent reader never sees a torn activity pointer");
  }

  std::printf("RuntimeHandlesTest: %d checks passed\n", checks);
  return 0;
}
