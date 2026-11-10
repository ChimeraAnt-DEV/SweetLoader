#include "pl/runtime/GameHookRules.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include <nlohmann/json.hpp>

#include "pl/Logger.hpp"
#include "pl/runtime/RulesFileCache.hpp"
#include "pl/runtime/GameHookRuleMerge.hpp"
#include "pl/runtime/RuleProbe.hpp"
#include "pl/runtime/VersionCompare.hpp"

namespace pl::runtime {
namespace {

std::mutex g_rulesMutex;
std::string g_rulesPath;
std::string g_minecraftVersion;

// Diagnostics state for the in-app report. Written once per rules load (on the frame-pump thread),
// read from the UI thread; plain atomics hold a small numeric summary and a guarded string copy.
std::atomic<int> g_lastMergeConflictCount{0};
std::atomic<bool> g_lastRuleHadRenderIndex{false};
std::atomic<std::uint64_t> g_lastRulesRevision{0};
std::atomic<bool> g_rulesLoadAttempted{false};
std::atomic<bool> g_rulesLoaded{false};
std::mutex g_lastRuleSummaryMutex;
std::string g_lastRuleSummary;

// One parsed copy of the rules file, shared by every reader. The file is re-read and re-parsed only
// when its path/mtime/size changes, so a single install (which reads the rules for signatures, the
// Optifine slots and the feature gate) reads the file once rather than several times.
std::mutex g_rulesCacheMutex;
RulesFileCache g_rulesCache;

std::string Trim(std::string value) {
  auto isNotSpace = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), isNotSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), isNotSpace).base(),
              value.end());
  return value;
}

std::optional<std::string> ReadTextFile(const std::string &path) {
  if (path.empty()) {
    return std::nullopt;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }

  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

// Returns the parsed rules document for [rulesPath], reading and parsing it only when the file's
// identity (path, mtime, size) changed since the last call. The returned shared pointer is
// immutable, so a caller may keep using it after the lock is released.
std::shared_ptr<const nlohmann::json> LoadRulesDocument(const std::string &rulesPath) {
  if (rulesPath.empty()) return nullptr;

  std::int64_t mtimeMs = -1;
  std::uint64_t size = 0;
  struct stat st {};
  if (::stat(rulesPath.c_str(), &st) == 0) {
    mtimeMs = static_cast<std::int64_t>(st.st_mtime) * 1000LL;
    size = static_cast<std::uint64_t>(st.st_size);
  }

  std::lock_guard<std::mutex> lock(g_rulesCacheMutex);
  return g_rulesCache.get(rulesPath, mtimeMs, size,
                          [&rulesPath]() -> std::shared_ptr<const nlohmann::json> {
                            auto content = ReadTextFile(rulesPath);
                            if (!content) return nullptr;
                            nlohmann::json root = nlohmann::json::parse(*content, nullptr, false);
                            if (root.is_discarded()) return nullptr;
                            return std::make_shared<const nlohmann::json>(std::move(root));
                          });
}

std::optional<std::string> ReadStringField(const nlohmann::json &object,
                                           const char *key) {
  if (!object.is_object()) {
    return std::nullopt;
  }

  auto it = object.find(key);
  if (it == object.end() || !it->is_string()) {
    return std::nullopt;
  }

  std::string value = Trim(it->get<std::string>());
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}


std::optional<std::size_t> ReadSizeField(const nlohmann::json &object,
                                         const char *key) {
  if (!object.is_object()) {
    return std::nullopt;
  }

  auto it = object.find(key);
  if (it == object.end() || !it->is_number_unsigned()) {
    return std::nullopt;
  }

  const auto value = it->get<std::uint64_t>();
  if (value > std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value);
}



std::optional<GameHookSignatures> ParseHookSignatures(const nlohmann::json &rule) {
  const nlohmann::json *sigs = &rule;
  auto sigsIt = rule.find("sigs");
  if (sigsIt != rule.end() && sigsIt->is_object()) {
    sigs = &(*sigsIt);
  }

  auto pauseMenuDtor = ReadStringField(*sigs, "pauseMenuDtorSig");
  auto pauseMenuOpen = ReadStringField(*sigs, "pauseMenuOpenSig");
  auto hudScreenDtor = ReadStringField(*sigs, "hudScreenDtorSig");
  auto hudScreenOpen = ReadStringField(*sigs, "hudScreenOpenSig");
  auto isShowingMenu = ReadStringField(*sigs, "isShowingMenuSig");
  auto isShowingMenuVtableIndex = ReadSizeField(rule, "isShowingMenuVtableIndex");
  auto playerRenderVtableIndex = ReadSizeField(rule, "playerRenderVtableIndex");
  auto imageLoaderSig = ReadStringField(*sigs, "imageLoaderSig");

  if (!pauseMenuDtor || !pauseMenuOpen || !hudScreenDtor || !hudScreenOpen ||
      (!isShowingMenu && !isShowingMenuVtableIndex)) {
    return std::nullopt;
  }

  return GameHookSignatures{
      *pauseMenuDtor,
      *pauseMenuOpen,
      *hudScreenDtor,
      *hudScreenOpen,
      isShowingMenu.value_or(""),
      isShowingMenuVtableIndex,
      playerRenderVtableIndex,
      imageLoaderSig.value_or(""),
  };
}

} // namespace

void ConfigureGameHookRules(std::string rulesPath, std::string minecraftVersion) {
  // Read the revision here, at delivery time, rather than only inside
  // LoadConfiguredGameHookSignatures. The install is deferred to the game's frame tick and is
  // skipped entirely when the runtime never comes up (or when the path is empty), and in that case
  // the diagnostics report used to print "revision 0" for a file that was in fact delivered and
  // carried a revision. Recording it from the delivered file makes the report say what was actually
  // handed over, independently of whether the hooks armed.
  // A fresh delivery may point at a rewritten file; drop the cache so the next reader re-parses.
  {
    std::lock_guard<std::mutex> cacheLock(g_rulesCacheMutex);
    g_rulesCache.invalidate();
  }

  std::uint64_t deliveredRevision = 0;
  if (auto root = LoadRulesDocument(rulesPath)) {
    if (root->is_object() && root->contains("revision") && (*root)["revision"].is_number_unsigned()) {
      deliveredRevision = (*root)["revision"].get<std::uint64_t>();
    }
  }

  std::lock_guard<std::mutex> lock(g_rulesMutex);
  g_rulesPath = std::move(rulesPath);
  g_minecraftVersion = std::move(minecraftVersion);
  g_lastRulesRevision.store(deliveredRevision, std::memory_order_relaxed);
}

std::optional<GameHookSignatures> LoadConfiguredGameHookSignatures() {
  std::string rulesPath;
  std::string minecraftVersion;
  {
    std::lock_guard<std::mutex> lock(g_rulesMutex);
    rulesPath = g_rulesPath;
    minecraftVersion = g_minecraftVersion;
  }

  g_rulesLoadAttempted.store(true, std::memory_order_release);

  auto document = LoadRulesDocument(rulesPath);
  if (!document) {
    preloaderLogger.warn("Preloader runtime data is unavailable: {}",
                          rulesPath.empty() ? "<empty>" : rulesPath);
    return std::nullopt;
  }

  const nlohmann::json &root = *document;
  if (!root.is_object()) {
    preloaderLogger.warn("Preloader runtime data JSON is invalid");
    return std::nullopt;
  }

  // Record the revision the file declares, for the diagnostics report.
  if (root.contains("revision") && root["revision"].is_number_unsigned()) {
    g_lastRulesRevision.store(root["revision"].get<std::uint64_t>(),
                              std::memory_order_relaxed);
  }

  auto rulesIt = root.find("rules");
  if (rulesIt == root.end() || !rulesIt->is_array()) {
    preloaderLogger.warn("Preloader runtime data missing rules array");
    return std::nullopt;
  }

  // Merge every rule that matches the running version, so a field a matching-but-first rule omits
  // (which is how 1.26.33.1 lost its playerRenderVtableIndex) is inherited from another match. The
  // merge is the pure, host-tested MergeHookRuleForVersion; this function keeps "first match wins
  // for a field" semantics so the diagnostics report and the hook agree.
  int matchCount = 0;
  for (const auto &rule : *rulesIt) {
    if (rule.is_object() && RuleMatchesVersion(rule, minecraftVersion)) {
      ++matchCount;
    }
  }

  auto merged = MergeHookRuleForVersion(root, minecraftVersion);
  if (!merged) {
    preloaderLogger.warn("No valid Preloader runtime data matches Minecraft {}",
                         minecraftVersion.empty() ? "<unknown>" : minecraftVersion);
    return std::nullopt;
  }

  auto signatures = ParseHookSignatures(*merged);
  if (!signatures) {
    preloaderLogger.warn("Merged Preloader runtime data is incomplete for Minecraft {}",
                         minecraftVersion.empty() ? "<unknown>" : minecraftVersion);
    return std::nullopt;
  }

  // Diagnostics: how many rules matched (a count > 1 is where first-match selection used to drop
  // fields) and whether the effective rule carries a render slot.
  g_lastMergeConflictCount.store(matchCount > 1 ? matchCount - 1 : 0,
                                 std::memory_order_relaxed);
  g_lastRuleHadRenderIndex.store(signatures->playerRenderVtableIndex.has_value(),
                                 std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_lastRuleSummaryMutex);
    std::string summary;
    auto append = [&summary](const std::string &token) {
      if (token.empty()) return;
      if (!summary.empty()) summary += ',';
      summary += token;
    };
    if (!signatures->pauseMenuDtor.empty()) append("pauseMenuDtorSig");
    if (!signatures->pauseMenuOpen.empty()) append("pauseMenuOpenSig");
    if (!signatures->hudScreenDtor.empty()) append("hudScreenDtorSig");
    if (!signatures->hudScreenOpen.empty()) append("hudScreenOpenSig");
    if (signatures->isShowingMenuVtableIndex) {
      append("isShowingMenuVtableIndex=" + std::to_string(*signatures->isShowingMenuVtableIndex));
    }
    if (signatures->playerRenderVtableIndex) {
      append("playerRenderVtableIndex=" + std::to_string(*signatures->playerRenderVtableIndex));
    }
    if (!signatures->imageLoaderSig.empty()) append("imageLoaderSig");
    g_lastRuleSummary = summary;
  }

  g_rulesLoaded.store(true, std::memory_order_release);
  preloaderLogger.info("Loaded Preloader runtime data for Minecraft {} ({} matching rule(s))",
                       minecraftVersion.empty() ? "<unknown>" : minecraftVersion, matchCount);
  return signatures;
}

std::size_t ReadConfiguredOptifineSlot(const char *key) {
  if (key == nullptr || *key == '\0') return 0;

  std::string rulesPath;
  std::string minecraftVersion;
  {
    std::lock_guard<std::mutex> lock(g_rulesMutex);
    rulesPath = g_rulesPath;
    minecraftVersion = g_minecraftVersion;
  }

  auto document = LoadRulesDocument(rulesPath);
  if (!document || !document->is_object()) return 0;

  auto rulesIt = document->find("rules");
  if (rulesIt == document->end() || !rulesIt->is_array()) return 0;

  for (const auto &rule : *rulesIt) {
    if (!rule.is_object() || !RuleMatchesVersion(rule, minecraftVersion)) continue;
    if (auto slot = ReadSizeField(rule, key)) return *slot;
  }
  return 0;
}

bool MatchesConfiguredVersion(const std::string &minVersion,
                              const std::string &maxVersion) {
  std::string minecraftVersion;
  {
    std::lock_guard<std::mutex> lock(g_rulesMutex);
    minecraftVersion = g_minecraftVersion;
  }

  if (minecraftVersion.empty()) {
    return false;
  }
  if (!minVersion.empty() && CompareVersions(minecraftVersion, minVersion) < 0) {
    return false;
  }
  if (!maxVersion.empty() && CompareVersions(minecraftVersion, maxVersion) > 0) {
    return false;
  }
  return true;
}

bool GameHookRulesConfigured() {
  std::lock_guard<std::mutex> lock(g_rulesMutex);
  return !g_rulesPath.empty();
}

bool GameHookRulesLoadAttempted() {
  return g_rulesLoadAttempted.load(std::memory_order_acquire);
}

bool GameHookRulesLoaded() { return g_rulesLoaded.load(std::memory_order_acquire); }

std::uint64_t GameHookRulesRevision() {
  return g_lastRulesRevision.load(std::memory_order_relaxed);
}

int GameHookRulesMergeConflicts() {
  return g_lastMergeConflictCount.load(std::memory_order_relaxed);
}

bool GameHookRulesHaveRenderIndex() {
  return g_lastRuleHadRenderIndex.load(std::memory_order_relaxed);
}

std::string GameHookRulesSummary() {
  std::lock_guard<std::mutex> lock(g_lastRuleSummaryMutex);
  return g_lastRuleSummary;
}

FeatureGate EvaluateConfiguredFeatureGate(HookFeature feature) {
  std::string rulesPath;
  std::string minecraftVersion;
  {
    std::lock_guard<std::mutex> lock(g_rulesMutex);
    rulesPath = g_rulesPath;
    minecraftVersion = g_minecraftVersion;
  }

  auto document = LoadRulesDocument(rulesPath);
  if (!document || !document->is_object()) {
    return FeatureGate::NoRuleSlot;
  }

  auto merged = MergeHookRuleForVersion(*document, minecraftVersion);
  if (!merged) {
    return FeatureGate::NoRuleSlot;
  }
  return EvaluateFeatureGate(*merged, feature);
}

int ConfiguredRenderGateReason() {
  // The report's numeric gate code mirrors the render hook's own refusal reasons where they
  // overlap: 0 means the feature is enabled. A gate that leaves it off is reported as its own
  // positive code so the report can name the exact link that broke.
  switch (EvaluateConfiguredFeatureGate(HookFeature::PlayerRender)) {
    case FeatureGate::Enabled: return 0;
    case FeatureGate::NoRuleSlot: return 5;
    case FeatureGate::ProbeMissing: return 6;
    case FeatureGate::ProbeUnresolved: return 7;
  }
  return 8;
}

} // namespace pl::runtime
