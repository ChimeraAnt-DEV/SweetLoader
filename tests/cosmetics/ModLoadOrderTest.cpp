/**
 * @file ModLoadOrderTest.cpp
 * @brief Host test for deterministic, dependency-ordered mod loading (item 447).
 *
 * The order a set of mods loads in decides which mod's hook runs first and which one is torn down
 * first, so it has to be reproducible and a cycle has to be named rather than broken at an
 * arbitrary point. Both rules are pure, so they run on the host.
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pl/internal/ModLoadOrder.hpp"

using namespace pl::internal::mod;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

std::vector<std::string> orderOf(const std::vector<ModLoadNode> &mods) {
  return ResolveModLoadOrder(mods).order;
}

void indexOf(const std::vector<std::string> &order, const std::string &id, const char *what) {
  expect(std::find(order.begin(), order.end(), id) != order.end(), what);
}

void noDependenciesKeepsInputOrder() {
  const std::vector<std::string> order =
      orderOf({{"a", {}}, {"b", {}}, {"c", {}}});
  expect(order == std::vector<std::string>({"a", "b", "c"}),
         "a set with no dependencies keeps its input order");
}

void aDependencyLoadsFirst() {
  // c depends on a, so a must be emitted before c even though c came first.
  const std::vector<std::string> order =
      orderOf({{"c", {"a"}}, {"b", {}}, {"a", {}}});
  const auto posA = std::find(order.begin(), order.end(), "a");
  const auto posC = std::find(order.begin(), order.end(), "c");
  expect(posA != order.end() && posC != order.end(), "both mods are ordered");
  expect(posA < posC, "a dependency is emitted before its dependent");
}

void aChainIsOrderedTransitively() {
  const std::vector<std::string> order =
      orderOf({{"c", {"b"}}, {"b", {"a"}}, {"a", {}}});
  expect(order == std::vector<std::string>({"a", "b", "c"}),
         "a chain orders bottom-up");
}

void aDiamondIsOrdered() {
  // d depends on b and c; both depend on a.
  const std::vector<std::string> order =
      orderOf({{"d", {"b", "c"}}, {"b", {"a"}}, {"c", {"a"}}, {"a", {}}});
  const auto posA = std::find(order.begin(), order.end(), "a");
  const auto posB = std::find(order.begin(), order.end(), "b");
  const auto posC = std::find(order.begin(), order.end(), "c");
  const auto posD = std::find(order.begin(), order.end(), "d");
  expect(posA < posB && posA < posC, "the shared dependency is first");
  expect(posB < posD && posC < posD, "the join is last");
}

void absentDependencyIsNotAnError() {
  const ModLoadOrder result = ResolveModLoadOrder({{"a", {"missing"}}, {"b", {}}});
  expect(!result.hasCycle, "a missing dependency is not a cycle");
  expect(result.order.size() == 2, "both present mods are still ordered");
  indexOf(result.order, "a", "the dependent still loads");
  indexOf(result.order, "b", "the unrelated mod still loads");
}

void aCycleIsReportedWithItsMembers() {
  const ModLoadOrder result =
      ResolveModLoadOrder({{"a", {"b"}}, {"b", {"c"}}, {"c", {"a"}}});
  expect(result.hasCycle, "a cycle is detected");
  expect(result.cycle.size() >= 2, "the cycle names its members");
  indexOf(result.cycle, "a", "a is named in the cycle");
  indexOf(result.cycle, "b", "b is named in the cycle");
  indexOf(result.cycle, "c", "c is named in the cycle");
  expect(result.order.size() == 3, "every mod is still ordered despite the cycle");
}

void aSelfCycleIsReported() {
  const ModLoadOrder result = ResolveModLoadOrder({{"a", {"a"}}});
  expect(result.hasCycle, "a mod depending on itself is a cycle");
  expect(!result.cycle.empty(), "the self-cycle names the mod");
}

void aCycleDoesNotHideTheRest() {
  const ModLoadOrder result =
      ResolveModLoadOrder({{"a", {"b"}}, {"b", {"a"}}, {"c", {}}});
  expect(result.hasCycle, "the cycle is detected");
  indexOf(result.order, "c", "an unrelated mod is still ordered");
}

void duplicateIdsKeepTheFirst() {
  const std::vector<std::string> order =
      orderOf({{"a", {}}, {"a", {}}, {"b", {}}});
  expect(order == std::vector<std::string>({"a", "b"}), "a duplicate id is loaded once");
}

void anEmptyIdIsSkipped() {
  const std::vector<std::string> order = orderOf({{"", {}}, {"a", {}}});
  expect(order == std::vector<std::string>({"a"}), "an empty id is skipped");
}

void anEmptySetOrdersToNothing() {
  expect(orderOf({}).empty(), "an empty set orders to nothing");
}

void determinismIsStable() {
  const std::vector<ModLoadNode> mods = {{"c", {"a"}}, {"b", {}}, {"a", {}}};
  const std::vector<std::string> first = orderOf(mods);
  const std::vector<std::string> second = orderOf(mods);
  expect(first == second, "the same input orders the same way every time");
}

} // namespace

int main() {
  noDependenciesKeepsInputOrder();
  aDependencyLoadsFirst();
  aChainIsOrderedTransitively();
  aDiamondIsOrdered();
  absentDependencyIsNotAnError();
  aCycleIsReportedWithItsMembers();
  aSelfCycleIsReported();
  aCycleDoesNotHideTheRest();
  duplicateIdsKeepTheFirst();
  anEmptyIdIsSkipped();
  anEmptySetOrdersToNothing();
  determinismIsStable();
  std::printf("ModLoadOrderTest: %d checks passed\n", checks);
  return 0;
}
