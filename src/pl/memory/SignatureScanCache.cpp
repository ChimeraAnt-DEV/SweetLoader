#include "pl/memory/SignatureScanCache.hpp"

#include <charconv>
#include <cstdio>

namespace pl::memory {
namespace {

// Signatures are rule names, never multi-line, but the writer refuses rather than emits a file the
// parser would misread if one ever were.
bool IsSerialisable(std::string_view signature) {
  if (signature.empty()) return false;
  for (const char ch : signature) {
    if (ch == '\n' || ch == '\r') return false;
  }
  return true;
}

std::string HexOffset(std::uintptr_t value) {
  char buffer[2 * sizeof(std::uintptr_t) + 1];
  const int written = std::snprintf(buffer, sizeof(buffer), "%llx",
                                    static_cast<unsigned long long>(value));
  if (written <= 0) return "0";
  return std::string(buffer, static_cast<std::size_t>(written));
}

bool ParseHexOffset(std::string_view text, std::uintptr_t &out) {
  if (text.empty()) return false;
  std::uintptr_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
  out = value;
  return true;
}

std::string_view NextLine(std::string_view &rest) {
  const std::size_t newline = rest.find('\n');
  std::string_view line =
      newline == std::string_view::npos ? rest : rest.substr(0, newline);
  rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  return line;
}

} // namespace

std::string SerializeSignatureScanCache(
    std::string_view buildId, const std::unordered_map<std::string, std::uintptr_t> &entries) {
  std::string out;
  out.reserve(64 + entries.size() * 48);
  out.append(kSignatureScanCacheMagic).push_back('\n');
  out.append(buildId).push_back('\n');
  for (const auto &[signature, offset] : entries) {
    if (offset == 0 || !IsSerialisable(signature)) continue;
    out.append(HexOffset(offset)).push_back(' ');
    out.append(signature).push_back('\n');
  }
  return out;
}

bool ParseSignatureScanCache(std::string_view text, std::string_view expectedBuildId,
                             std::unordered_map<std::string, std::uintptr_t> &out) {
  out.clear();
  if (text.empty() || expectedBuildId.empty()) return false;

  std::string_view rest = text;
  const std::string_view magic = NextLine(rest);
  if (magic != kSignatureScanCacheMagic) return false;

  const std::string_view buildId = NextLine(rest);
  if (buildId != expectedBuildId) return false;

  while (!rest.empty()) {
    const std::string_view line = NextLine(rest);
    if (line.empty()) continue;
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos || space == 0) continue;

    std::uintptr_t offset = 0;
    if (!ParseHexOffset(line.substr(0, space), offset) || offset == 0) continue;

    const std::string_view signature = line.substr(space + 1);
    if (!IsSerialisable(signature)) continue;
    out.emplace(std::string(signature), offset);
  }
  return true;
}

} // namespace pl::memory
