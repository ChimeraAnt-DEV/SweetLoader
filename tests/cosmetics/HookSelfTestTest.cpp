// Host test for the hook self-test report.
//
// The runner itself needs a live backend, but the report is what the client prints, so its
// classification, its pass/fail arithmetic and its JSON shape are pinned here. The header is
// Android-free, so this compiles and runs on the build host.

#include <iostream>
#include <string>
#include <vector>

#include "pl/runtime/HookSelfTest.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string &what) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL: " << what << "\n";
  }
}

pl::runtime::HookSelfTestResult MakeSite(const std::string &name, bool resolved, bool executable,
                                         bool installed, bool removed,
                                         const std::string &detail = "") {
  pl::runtime::HookSelfTestResult result;
  result.name = name;
  result.resolved = resolved;
  result.executable = executable;
  result.installed = installed;
  result.removed = removed;
  result.detail = detail;
  return result;
}

void AnUnresolvedSitePasses() {
  pl::runtime::HookSelfTestReport report;
  report.backendOk = true;
  report.sites.push_back(MakeSite("PauseMenuOpen", false, false, false, false));
  Check(report.passed() == 1, "an unresolved site counts as passed");
  Check(report.failed() == 0, "an unresolved site is not a failure");
}

void AResolvedAndRoundTrippedSitePasses() {
  pl::runtime::HookSelfTestReport report;
  report.backendOk = true;
  report.sites.push_back(MakeSite("PauseMenuOpen", true, true, true, true));
  Check(report.passed() == 1, "a round-tripped site passes");
  Check(report.failed() == 0, "a round-tripped site is not a failure");
}

void AResolvedButRefusedSiteFails() {
  pl::runtime::HookSelfTestReport report;
  report.backendOk = true;
  report.sites.push_back(MakeSite("HudScreenOpen", true, true, false, false, "refused"));
  Check(report.passed() == 0, "a refused site does not pass");
  Check(report.failed() == 1, "a refused site fails");
}

void ANonExecutableTargetFails() {
  pl::runtime::HookSelfTestReport report;
  report.backendOk = true;
  report.sites.push_back(MakeSite("Render", true, false, false, false));
  Check(report.failed() == 1, "a non-executable target fails");
  Check(report.summary().find("not-executable") != std::string::npos ||
            report.summary().find("Render") != std::string::npos,
        "the failed site is named in the summary");
}

void ABackendFailureDominatesTheSummary() {
  pl::runtime::HookSelfTestReport report;
  report.backendOk = false;
  report.sites.push_back(MakeSite("PauseMenuOpen", true, true, true, true));
  Check(report.summary().find("backend") != std::string::npos,
        "a broken backend is reported as such");
}

void TheJsonCarriesEverySiteAndItsState() {
  pl::runtime::HookSelfTestReport report;
  report.backendOk = true;
  report.sites.push_back(MakeSite("PauseMenuOpen", true, true, true, true));
  report.sites.push_back(MakeSite("HudScreenOpen", false, false, false, false));
  report.sites.push_back(MakeSite("Render", true, false, false, false));

  const std::string json = report.toJson();
  Check(json.find("\"backend_ok\":true") != std::string::npos, "backend_ok is rendered");
  Check(json.find("\"passed\":2") != std::string::npos, "the pass count is rendered");
  Check(json.find("\"total\":3") != std::string::npos, "the total is rendered");
  Check(json.find("\"state\":\"ok\"") != std::string::npos, "a good site renders ok");
  Check(json.find("\"state\":\"unresolved\"") != std::string::npos, "a missing site renders unresolved");
  Check(json.find("\"state\":\"not-executable\"") != std::string::npos,
        "a bad address renders not-executable");
  Check(json.front() == '{' && json.back() == '}', "the report is one JSON object");
}

void ADetailWithAQuoteStaysValidJson() {
  pl::runtime::HookSelfTestReport report;
  report.backendOk = true;
  report.sites.push_back(MakeSite("Weird\"Name", true, true, false, false, "say \"hi\""));
  const std::string json = report.toJson();
  Check(json.find("Weird\\\"Name") != std::string::npos, "a quote in the name is escaped");
  Check(json.find("say \\\"hi\\\"") != std::string::npos, "a quote in the detail is escaped");
}

void TheSummaryNamesFailedSites() {
  pl::runtime::HookSelfTestReport report;
  report.backendOk = true;
  report.sites.push_back(MakeSite("Good", true, true, true, true));
  report.sites.push_back(MakeSite("Bad", true, true, false, false, "refused"));
  const std::string summary = report.summary();
  Check(summary.find("1/2") != std::string::npos, "the count is in the summary");
  Check(summary.find("Bad") != std::string::npos, "the failing site is named");
  Check(summary.find("Good") == std::string::npos, "a passing site is not named as failed");
}

} // namespace

int main() {
  AnUnresolvedSitePasses();
  AResolvedAndRoundTrippedSitePasses();
  AResolvedButRefusedSiteFails();
  ANonExecutableTargetFails();
  ABackendFailureDominatesTheSummary();
  TheJsonCarriesEverySiteAndItsState();
  ADetailWithAQuoteStaysValidJson();
  TheSummaryNamesFailedSites();

  if (g_failures == 0) {
    std::cout << "HookSelfTestTest: all checks passed\n";
    return 0;
  }
  std::cout << "HookSelfTestTest: " << g_failures << " checks failed\n";
  return 1;
}
