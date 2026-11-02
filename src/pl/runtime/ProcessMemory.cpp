#include "pl/runtime/ProcessMemory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace pl::runtime {
namespace {

// The largest `smaps_rollup` we will read in one go. The real file is a few hundred bytes; the cap
// exists so a pathological kernel cannot make this allocate without bound.
constexpr std::size_t kMaxSmapsBytes = 64 * 1024;

bool ReadWholeFile(const char *path, std::string &out) {
  FILE *file = std::fopen(path, "re");
  if (file == nullptr) return false;
  out.clear();
  char buffer[1024];
  std::size_t total = 0;
  while (total < kMaxSmapsBytes) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    if (read == 0) break;
    out.append(buffer, read);
    total += read;
  }
  std::fclose(file);
  return !out.empty();
}

} // namespace

bool ParseSmapsRollupPss(const char *text, std::size_t length, std::uint64_t &outKb) {
  if (text == nullptr || length == 0) return false;
  // A line looks like "Pss:              12345 kB". Scan for a line starting with "Pss:" at a line
  // boundary so a value named in another field cannot be mistaken for it.
  std::size_t index = 0;
  while (index < length) {
    const bool atLineStart = index == 0 || text[index - 1] == '\n';
    if (atLineStart && length - index >= 4 && std::strncmp(text + index, "Pss:", 4) == 0) {
      std::size_t cursor = index + 4;
      while (cursor < length && (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
      std::uint64_t value = 0;
      bool anyDigit = false;
      while (cursor < length && text[cursor] >= '0' && text[cursor] <= '9') {
        value = value * 10 + static_cast<std::uint64_t>(text[cursor] - '0');
        anyDigit = true;
        ++cursor;
      }
      if (anyDigit) {
        outKb = value;
        return true;
      }
      return false;
    }
    const char *next = static_cast<const char *>(std::memchr(text + index, '\n', length - index));
    if (next == nullptr) break;
    index = static_cast<std::size_t>(next - text) + 1;
  }
  return false;
}

std::uint64_t ResidentPagesToKb(std::uint64_t residentPages, std::uint64_t pageSizeKb) {
  if (residentPages == 0 || pageSizeKb == 0) return 0;
  return residentPages * pageSizeKb;
}

ProcessMemoryReading ReadProcessMemory() {
  ProcessMemoryReading reading;

  std::string smaps;
  if (ReadWholeFile("/proc/self/smaps_rollup", smaps)) {
    std::uint64_t pss = 0;
    if (ParseSmapsRollupPss(smaps.data(), smaps.size(), pss)) {
      reading.proportionalKb = pss;
      reading.hasProportional = true;
    }
  }

  std::string statm;
  if (ReadWholeFile("/proc/self/statm", statm)) {
    // "size resident shared text lib data dt" in pages; the resident count is the second field.
    const char *cursor = statm.c_str();
    char *end = nullptr;
    std::strtoull(cursor, &end, 10); // size, skipped
    if (end != nullptr && *end != '\0') {
      const unsigned long long resident = std::strtoull(end, nullptr, 10);
      const std::uint64_t pageSizeKb =
          static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)) / 1024ULL;
      reading.residentKb = ResidentPagesToKb(resident, pageSizeKb);
    }
  }

  return reading;
}

} // namespace pl::runtime
