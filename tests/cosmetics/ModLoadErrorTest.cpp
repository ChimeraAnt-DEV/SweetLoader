/**
 * @file ModLoadErrorTest.cpp
 * @brief Host test for the mod-load error slot (audit item 6.17).
 *
 * nativeLoadMod returns a boolean, so the loader records the reason behind a failure in this slot and
 * a second JNI call reads it back. The slot is a single value: a clear drops the previous reason, a
 * set replaces it, and a get copies it out so a later clear cannot change a value already read.
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "pl/internal/ModLoadError.hpp"

using pl::internal::mod::ModLoadErrorStore;

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
  ModLoadErrorStore store;
  expect(store.get().empty(), "a fresh store is empty");

  store.set("the manifest could not be read");
  expect(store.get() == "the manifest could not be read", "a set message reads back");

  store.set("the library could not be loaded");
  expect(store.get() == "the library could not be loaded", "a second set replaces the first");

  // A value already read is a copy; clearing the store does not change it.
  const std::string read = store.get();
  store.clear();
  expect(store.get().empty(), "a clear empties the store");
  expect(read == "the library could not be loaded", "a value read before the clear is unchanged");

  // Concurrent set/get/clear must not tear. Another worker may replace the value between this
  // thread's set and its get, so the read may legitimately see any worker's whole value or an
  // empty slot -- what it must never see is a mixed or truncated one.
  std::vector<std::string> valid;
  for (int i = 0; i < 4; ++i) valid.push_back("worker-" + std::to_string(i));
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&store, &valid, i]() {
      const std::string mine = valid[static_cast<std::size_t>(i)];
      for (int n = 0; n < 2000; ++n) {
        store.set(mine);
        const std::string seen = store.get();
        bool whole = seen.empty();
        for (const std::string &candidate : valid) {
          if (seen == candidate) whole = true;
        }
        if (!whole) {
          std::fprintf(stderr, "FAIL: a concurrent read saw a mixed value: %s\n", seen.c_str());
          std::abort();
        }
        store.clear();
      }
    });
  }
  for (auto &t : threads) t.join();

  std::printf("ModLoadErrorTest: %d checks passed\n", checks);
  return 0;
}
