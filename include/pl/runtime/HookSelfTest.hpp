#pragma once

/**
 * @file HookSelfTest.hpp
 * @brief A one-shot install/remove round-trip over every hook site (item 452).
 *
 * <p>A hook can fail for reasons that only show up at the moment of patching: the target address
 * resolved to data rather than code, the trampoline could not be built, or the original bytes could
 * not be restored. Today the only signal is that the feature does nothing, which is
 * indistinguishable from a feature the user turned off. The self-test patches each site, verifies
 * the backend observed the hook, removes it again, and reports the outcome per site — so "this hook
 * cannot install on this build" is a fact the client can show, not something to infer.
 *
 * <p><b>Safety.</b> The round-trip runs on the loading path, immediately before the real install, so
 * the sites are not yet hooked (no double-hook) and the game has not started calling them (the
 * overlay hooks only matter once a world or a menu is up). Each site is restored before the next is
 * touched, and the report's `removed` flag is what proves the restore happened.
 *
 * <p>The report half is pure and host-tested (`HookSelfTestTest`): the classification, the pass
 * count and the JSON shape are decided here, so a wiring mistake cannot change what the client
 * prints. Only `RunHookSelfTest` touches the backend.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pl/Export.hpp"
#include "pl/memory/Hook.hpp"

namespace pl::runtime {
namespace detail {

/** The wire name of a site's outcome. */
inline const char *HookSiteStateName(bool resolved, bool executable, bool installed,
                                     bool removed) noexcept {
  if (!resolved) return "unresolved";
  if (!executable) return "not-executable";
  if (installed && removed) return "ok";
  return "failed";
}

/** Escapes a string for a JSON literal, so a detail can never produce a malformed document. */
inline std::string HookSelfTestJsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char ch : text) {
    switch (ch) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20U) {
          static const char kHex[] = "0123456789abcdef";
          out.append("\\u00");
          out.push_back(kHex[(static_cast<unsigned char>(ch) >> 4U) & 0xfU]);
          out.push_back(kHex[static_cast<unsigned char>(ch) & 0xfU]);
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

} // namespace detail

/** @brief One resolved hook site to round-trip. */
struct HookSite {
  /** Diagnostic label, e.g. `"PauseMenuOpen"`. Never null. */
  const char *name = "";
  /** The resolved target address; 0 when the signature/vtable slot did not resolve. */
  std::uintptr_t target = 0;
  /** The detour that would be installed in production. */
  pl::memory::FuncPtr detour = nullptr;
  /** Where the backend writes the trampoline pointer. */
  pl::memory::FuncPtr *original = nullptr;
  /** The module the target belongs to, for the executability check. Defaults to the game. */
  const char *module = "libminecraftpe.so";
};

/** @brief The outcome of one site's round-trip. */
struct HookSelfTestResult {
  std::string name;
  /** The target address was non-zero. */
  bool resolved = false;
  /** The target is inside an executable mapping for the module. */
  bool executable = false;
  /** The backend accepted the detour. */
  bool installed = false;
  /** The backend removed the detour and the hook count fell back. */
  bool removed = false;
  /** A short explanation, empty on success. */
  std::string detail;

  /** True when the site either round-tripped cleanly or was legitimately unresolved. */
  [[nodiscard]] bool passed() const {
    if (!resolved) return true;  // an unresolved signature is a build limitation, not a failure
    return executable && installed && removed;
  }
};

/** @brief The whole run. */
struct HookSelfTestReport {
  /**
   * True when the backend round-tripped a private stub function. This is the control: if it is
   * false, the hook backend itself is broken and every site result below is meaningless.
   */
  bool backendOk = false;
  std::vector<HookSelfTestResult> sites;

  /** Sites that resolved and round-tripped, plus sites that legitimately did not resolve. */
  [[nodiscard]] std::size_t passed() const {
    std::size_t count = 0;
    for (const auto &site : sites) {
      if (site.passed()) ++count;
    }
    return count;
  }

  /** Sites that resolved and failed to round-trip. */
  [[nodiscard]] std::size_t failed() const {
    std::size_t count = 0;
    for (const auto &site : sites) {
      if (site.resolved && !site.passed()) ++count;
    }
    return count;
  }

  [[nodiscard]] std::size_t total() const { return sites.size(); }

  /**
   * @brief Renders the report as one JSON object.
   *
   * <p>Shape: `{"backend_ok":<bool>,"passed":N,"total":M,"sites":[{"name":"...","state":"...",
   * "detail":"..."}]}`. `state` is one of `ok`, `unresolved` or `failed`, so a reader does not have
   * to re-derive it from the flags. Inline so the shape is host-tested without the logger.
   */
  [[nodiscard]] std::string toJson() const {
    std::string out;
    out.reserve(256);
    out.append("{\"backend_ok\":");
    out.append(backendOk ? "true" : "false");
    out.append(",\"passed\":");
    out.append(std::to_string(passed()));
    out.append(",\"total\":");
    out.append(std::to_string(total()));
    out.append(",\"sites\":[");
    for (std::size_t i = 0; i < sites.size(); ++i) {
      if (i != 0) out.push_back(',');
      const HookSelfTestResult &site = sites[i];
      out.append("{\"name\":\"");
      out.append(detail::HookSelfTestJsonEscape(site.name));
      out.append("\",\"state\":\"");
      out.append(detail::HookSiteStateName(site.resolved, site.executable, site.installed,
                                           site.removed));
      out.push_back('"');
      if (!site.detail.empty()) {
        out.append(",\"detail\":\"");
        out.append(detail::HookSelfTestJsonEscape(site.detail));
        out.push_back('"');
      }
      out.push_back('}');
    }
    out.append("]}");
    return out;
  }

  /** A one-line summary for a diagnostics row. */
  [[nodiscard]] std::string summary() const {
    if (!backendOk) {
      return "hook backend self-test FAILED; no hook can install on this build";
    }
    std::string out =
        std::to_string(passed()) + "/" + std::to_string(total()) + " hook sites round-tripped";
    if (failed() != 0) {
      out.append("; failed: ");
      bool first = true;
      for (const auto &site : sites) {
        if (!site.resolved || site.passed()) continue;
        if (!first) out.append(", ");
        first = false;
        out.append(site.name.empty() ? "?" : site.name);
      }
    }
    return out;
  }
};

/**
 * @brief Installs and removes each site once, plus a private stub, and reports the outcome.
 *
 * <p>Never throws and never leaves a site hooked: every install is paired with a removal, and the
 * removal is verified by comparing the backend's hook count before and after. A site whose target
 * is 0 is recorded as unresolved rather than attempted.
 */
PL_EXPORT HookSelfTestReport RunHookSelfTest(const std::vector<HookSite> &sites);

/** @brief The most recent report, so the client can read it without re-running the round-trip. */
PL_EXPORT const HookSelfTestReport &LastHookSelfTestReport();

/** @brief True once a self-test has run. */
PL_EXPORT bool HookSelfTestHasRun();

} // namespace pl::runtime
