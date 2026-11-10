/**
 * @file ModFaultStoreTest.cpp
 * @brief Host test for the persisted per-mod fault budget.
 *
 * The in-memory fault tracker forgets every fault on exit, so a mod that faults on every launch was
 * retried forever (audit item 6.5). The persisted store fixes that, and its key/serialize/parse
 * rules are pure. The important properties: the version is part of the key (an update gets a fresh
 * budget), a round trip preserves the state, and a malformed or future-schema file is refused rather
 * than partially understood.
 */

#include <cstdio>
#include <cstdlib>
#include <string>

#include "pl/internal/ModFaultStore.hpp"

using pl::internal::mod::kModFaultStoreSchema;
using pl::internal::mod::MakeModFaultKey;
using pl::internal::mod::ModFaultRecord;
using pl::internal::mod::ModFaultStore;
using pl::internal::mod::ParseModFaultStore;
using pl::internal::mod::SerializeModFaultStore;
using pl::internal::mod::ShouldApplyPersistedFaults;

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
  // The key carries the version, so the same mod at a new version is a different key.
  expect(MakeModFaultKey("com.example.mod", "1.0") == "com.example.mod@1.0", "the key joins id and version");
  expect(MakeModFaultKey("com.example.mod", "1.0") != MakeModFaultKey("com.example.mod", "1.1"),
         "a version change changes the key");
  expect(MakeModFaultKey("a", "") != MakeModFaultKey("a", "1"), "an empty version differs from a set one");

  // A round trip preserves the store.
  ModFaultStore store;
  store[MakeModFaultKey("mod.a", "2.0")] = ModFaultRecord{3, true};
  store[MakeModFaultKey("mod.b", "0.1")] = ModFaultRecord{1, false};
  const std::string text = SerializeModFaultStore(store);
  const ModFaultStore round = ParseModFaultStore(text);
  expect(round.size() == 2, "the round trip keeps both records");
  expect(round.at("mod.a@2.0").faults == 3 && round.at("mod.a@2.0").disabled,
         "the round trip keeps a disabled record");
  expect(round.at("mod.b@0.1").faults == 1 && !round.at("mod.b@0.1").disabled,
         "the round trip keeps an enabled record");

  // A lookup applies only an exact key.
  ModFaultRecord out;
  expect(ShouldApplyPersistedFaults(round, "mod.a@2.0", out), "an exact key is applied");
  expect(out.faults == 3 && out.disabled, "the applied record matches");
  expect(!ShouldApplyPersistedFaults(round, "mod.a@2.1", out),
         "a version change does not apply the old record");
  expect(!ShouldApplyPersistedFaults(round, "mod.missing@1", out), "an unknown key is not applied");

  // Fail-closed parsing.
  expect(ParseModFaultStore("").empty(), "empty text is an empty store");
  expect(ParseModFaultStore("not json").empty(), "malformed text is refused");
  expect(ParseModFaultStore("[1,2,3]").empty(), "a non-object document is refused");
  expect(ParseModFaultStore("{\"mods\":{}}").empty(), "a document without a version is refused");
  expect(ParseModFaultStore("{\"version\":999,\"mods\":{\"a@1\":{\"faults\":2}}}").empty(),
         "a future schema version is refused");
  expect(ParseModFaultStore("{\"version\":1}").empty(), "a document without a mods object is refused");
  // A record with the wrong value type is skipped, not guessed.
  expect(ParseModFaultStore("{\"version\":1,\"mods\":{\"a@1\":{\"faults\":\"x\"}}}").empty(),
         "a record with a non-numeric fault count is skipped");
  // A negative fault count is clamped to zero.
  expect(ParseModFaultStore("{\"version\":1,\"mods\":{\"a@1\":{\"faults\":-5}}}").at("a@1").faults == 0,
         "a negative fault count is clamped");
  // A disabled flag of the wrong type reads as not disabled.
  expect(!ParseModFaultStore("{\"version\":1,\"mods\":{\"a@1\":{\"faults\":1,\"disabled\":\"yes\"}}}")
              .at("a@1").disabled,
         "a non-boolean disabled flag reads as not disabled");
  expect(kModFaultStoreSchema == 1, "the schema version is 1");

  std::printf("ModFaultStoreTest: %d checks passed\n", checks);
  return 0;
}
