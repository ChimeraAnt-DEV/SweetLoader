#include "pl/runtime/AntEggLoader.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include "pl/runtime/AntEggZip.hpp"

namespace pl::runtime {
namespace {

namespace fs = std::filesystem;

constexpr const char* kManifestName = "egg.json";
constexpr const char* kExtension = ".antegg";
constexpr uint64_t kMaxUncompressedBytes = 256ull * 1024ull * 1024ull;
// The archive itself is capped too. ReadWholeFile resizes its buffer to the file's size before it
// reads a byte, so without this a multi-gigabyte (or sparse) "package" is an allocation the caller
// cannot refuse. A real .AntEgg is a script or a small shared object; the ceiling is generous.
constexpr uint64_t kMaxArchiveBytes = 512ull * 1024ull * 1024ull;
constexpr size_t kMaxEntries = 4096;
// The longest path a package or sandbox root may name. Linux PATH_MAX; a longer one is refused
// rather than truncated, because a truncated path is a different path.
constexpr size_t kMaxPathLength = 4096;

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string Trim(const std::string& value) {
  size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

/// Lowercase, dash-separated id. Mirrors AntEggManifest.slug on the Kotlin side.
std::string Slug(const std::string& value) {
  std::string out;
  bool last_dash = false;
  for (unsigned char c : value) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      last_dash = false;
    } else if (!last_dash && !out.empty()) {
      out.push_back('-');
      last_dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "mod" : out;
}

bool IsSemanticVersion(const std::string& value) {
  std::string core = value;
  size_t suffix = core.find_first_of("-+");
  if (suffix != std::string::npos) core = core.substr(0, suffix);
  int parts = 0;
  size_t start = 0;
  while (start <= core.size()) {
    size_t dot = core.find('.', start);
    std::string part = core.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (part.empty()) return false;
    for (unsigned char c : part) {
      if (!std::isdigit(c)) return false;
    }
    ++parts;
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return parts == 3;
}

uint16_t ReadU16(const std::vector<uint8_t>& data, size_t offset) {
  return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
}

uint32_t ReadU32(const std::vector<uint8_t>& data, size_t offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) |
         (static_cast<uint32_t>(data[offset + 3]) << 24);
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  file.seekg(0, std::ios::end);
  std::streamoff size = file.tellg();
  if (size < 0) return false;
  // Refuse before resizing: the size is attacker-controlled (it is the file's own length), and a
  // resize to it is the allocation the cap exists to prevent.
  if (static_cast<uint64_t>(size) > kMaxArchiveBytes) return false;
  file.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  if (size > 0) file.read(reinterpret_cast<char*>(out.data()), size);
  return file.good() || file.eof();
}

/// True when `value` is a non-empty, absolute path with no NUL and no `..` segment. Cheap, and it
/// runs before any filesystem call, so a crafted path is refused rather than resolved.
bool IsSafeAbsolutePath(const std::string& value) {
  if (value.empty() || value.size() > kMaxPathLength) return false;
  if (value.find('\0') != std::string::npos) return false;
  if (value.front() != '/') return false;
  // Reject a `..` segment anywhere. A literal `..` in a resolved path is either an escape attempt
  // or a legitimate directory named "..", and neither belongs in a package or sandbox path.
  size_t start = 0;
  while (start <= value.size()) {
    size_t slash = value.find('/', start);
    std::string segment =
        value.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (segment == "..") return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

struct ZipEntryRecord {
  std::string name;
  uint16_t method = 0;
  uint32_t crc = 0;
  uint32_t compressed_size = 0;
  uint32_t uncompressed_size = 0;
  uint32_t local_offset = 0;
  bool is_directory = false;
};

/// Locates the End Of Central Directory and parses the central directory entry table.
bool ParseCentralDirectory(const std::vector<uint8_t>& data, std::vector<ZipEntryRecord>& out) {
  if (data.size() < 22) return false;
  const size_t search_start = data.size() > (0xFFFF + 22) ? data.size() - (0xFFFF + 22) : 0;
  size_t eocd = std::string::npos;
  for (size_t i = data.size() - 22 + 1; i-- > search_start;) {
    if (data[i] == 0x50 && data[i + 1] == 0x4b && data[i + 2] == 0x05 && data[i + 3] == 0x06) {
      eocd = i;
      break;
    }
  }
  if (eocd == std::string::npos) return false;

  // A ZIP64 archive uses 64-bit entry counts and offsets in a record this parser does not read, so
  // the 32-bit fields below would be misread. Refuse it rather than extract the wrong bytes
  // (audit item 12.3).
  if (HasZip64EndOfCentralDirectory(data.data(), data.size())) return false;

  uint16_t entry_count = ReadU16(data, eocd + 10);
  uint32_t cd_offset = ReadU32(data, eocd + 16);
  if (cd_offset >= data.size()) return false;

  size_t cursor = cd_offset;
  for (uint16_t i = 0; i < entry_count; ++i) {
    if (cursor + 46 > data.size()) return false;
    if (ReadU32(data, cursor) != 0x02014b50) return false;
    ZipEntryRecord record;
    record.method = ReadU16(data, cursor + 10);
    record.crc = ReadU32(data, cursor + 16);
    record.compressed_size = ReadU32(data, cursor + 20);
    record.uncompressed_size = ReadU32(data, cursor + 24);
    uint16_t name_len = ReadU16(data, cursor + 28);
    uint16_t extra_len = ReadU16(data, cursor + 30);
    uint16_t comment_len = ReadU16(data, cursor + 32);
    record.local_offset = ReadU32(data, cursor + 42);
    if (cursor + 46 + name_len > data.size()) return false;
    record.name.assign(reinterpret_cast<const char*>(data.data() + cursor + 46), name_len);
    std::replace(record.name.begin(), record.name.end(), '\\', '/');
    record.is_directory = !record.name.empty() && record.name.back() == '/';
    out.push_back(record);
    cursor += 46 + name_len + extra_len + comment_len;
  }
  return true;
}

/// Inflates one entry. `method` 0 is stored, 8 is deflate; anything else is unsupported.
bool ExtractEntryData(const std::vector<uint8_t>& data, const ZipEntryRecord& entry,
                      std::vector<uint8_t>& out) {
  if (entry.local_offset + 30 > data.size()) return false;
  if (ReadU32(data, entry.local_offset) != 0x04034b50) return false;
  uint16_t name_len = ReadU16(data, entry.local_offset + 26);
  uint16_t extra_len = ReadU16(data, entry.local_offset + 28);
  size_t start = entry.local_offset + 30 + name_len + extra_len;
  if (start + entry.compressed_size > data.size()) return false;

  if (entry.method == 0) {
    if (entry.compressed_size > kMaxUncompressedBytes) return false;
    out.assign(data.begin() + start, data.begin() + start + entry.compressed_size);
    // Verify the stored CRC before the bytes are used: a corrupted entry must not be extracted as
    // if it were the mod (audit item 12.5).
    const uLong computed = ::crc32(0L, out.data(), static_cast<uInt>(out.size()));
    return ZipCrcMatches(entry.crc, static_cast<uint32_t>(computed));
  }
  if (entry.method != 8) return false;

  // Bound the allocation by the entry's *declared* size before inflating into it: the declared
  // size is attacker-controlled and drives the resize, so an unchecked one is a zip bomb.
  if (entry.uncompressed_size > kMaxUncompressedBytes) return false;
  out.resize(entry.uncompressed_size);
  z_stream stream{};
  stream.next_in = const_cast<Bytef*>(data.data() + start);
  stream.avail_in = entry.compressed_size;
  stream.next_out = out.data();
  stream.avail_out = entry.uncompressed_size;
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
  int status = inflate(&stream, Z_FINISH);
  inflateEnd(&stream);
  if (status != Z_STREAM_END) return false;
  out.resize(stream.total_out);
  // Verify the stored CRC before the bytes are used (audit item 12.5).
  const uLong computed = ::crc32(0L, out.data(), static_cast<uInt>(out.size()));
  return ZipCrcMatches(entry.crc, static_cast<uint32_t>(computed));
}

// A staging or backup directory left behind by a crash is removed after this long. The window is
// generous: an extraction in progress must never be mistaken for a leak, and a day is far longer
// than any real extraction takes (audit item 12.6).
constexpr auto kStaleStagingAge = std::chrono::hours(24);

/// Removes `.X.extracting` and `.X.old` leftovers under `root` that are older than the window.
void CleanStaleStagingDirs(const fs::path& root) {
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return;
  const auto now = fs::file_time_type::clock::now();
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (ec) return;
    const std::string name = entry.path().filename().string();
    const bool staged = name.size() > 1 && name.front() == '.' &&
                        (name.ends_with(".extracting") || name.ends_with(".old"));
    if (!staged) continue;
    std::error_code timeEc;
    const auto written = fs::last_write_time(entry.path(), timeEc);
    if (timeEc) continue;
    if (now - written < kStaleStagingAge) continue;
    std::error_code removeEc;
    fs::remove_all(entry.path(), removeEc);
  }
}

/// True when `candidate` is `root` or lives under it, on canonical paths.
bool IsInside(const fs::path& root, const fs::path& candidate) {
  std::error_code ec;
  fs::path canonical_root = fs::weakly_canonical(root, ec);
  if (ec) return false;
  fs::path canonical_candidate = fs::weakly_canonical(candidate, ec);
  if (ec) return false;
  auto root_it = canonical_root.begin();
  auto cand_it = canonical_candidate.begin();
  for (; root_it != canonical_root.end(); ++root_it, ++cand_it) {
    if (cand_it == canonical_candidate.end() || *root_it != *cand_it) return false;
  }
  return true;
}

}  // namespace

std::string AntEggManifest::Id() const { return Slug(name); }

bool AntEggLoader::ValidateSandboxRoot(const std::string& sandbox_root,
                                       const std::string& allowed_root,
                                       std::string& error) {
  if (!IsSafeAbsolutePath(sandbox_root)) {
    error = "the sandbox root must be an absolute path with no parent segments";
    return false;
  }
  if (!IsSafeAbsolutePath(allowed_root)) {
    error = "the allowed root must be an absolute path with no parent segments";
    return false;
  }
  // The extraction target must live under the root the caller owns (the app's private directory).
  // Checked on the raw paths first, so a root outside the caller's storage is refused without any
  // filesystem call; the canonical check then catches a symlink that resolves outside.
  if (!IsInside(fs::path(allowed_root), fs::path(sandbox_root))) {
    error = "the sandbox root must live inside the app's private directory";
    return false;
  }
  return true;
}

bool AntEggLoader::LooksLikeAntEgg(const std::string& file_name) {
  std::string lower = ToLower(file_name);
  return lower.size() > std::strlen(kExtension) &&
         lower.compare(lower.size() - std::strlen(kExtension), std::strlen(kExtension),
                       kExtension) == 0;
}

bool AntEggLoader::ParseManifest(const std::string& json, AntEggManifest& out,
                                 std::string& error) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(json, nullptr, true, true);
  } catch (const std::exception& e) {
    error = std::string("egg.json is not valid JSON: ") + e.what();
    return false;
  }
  if (!root.is_object()) {
    error = "egg.json must contain a JSON object";
    return false;
  }
  auto require_string = [&](const char* key, std::string& target) -> bool {
    auto it = root.find(key);
    if (it == root.end()) {
      error = std::string("missing required field: ") + key;
      return false;
    }
    if (!it->is_string()) {
      error = std::string(key) + " must be a string";
      return false;
    }
    target = Trim(it->get<std::string>());
    if (target.empty()) {
      error = std::string("missing required field: ") + key;
      return false;
    }
    return true;
  };

  // Schema version (audit item 12.8): absent is the legacy value 1; a present value must be the one
  // this build understands, so a newer manifest is refused rather than misread.
  auto schema = root.find("schema_version");
  if (schema != root.end()) {
    if (!schema->is_number_integer()) {
      error = "schema_version must be an integer";
      return false;
    }
    out.schema_version = schema->get<int>();
    if (out.schema_version != 1) {
      error = "unsupported schema_version; this build reads version 1";
      return false;
    }
  }

  if (!require_string("name", out.name)) return false;
  if (!require_string("version", out.version)) return false;
  if (!require_string("author", out.author)) return false;
  if (!require_string("type", out.type)) return false;
  if (!require_string("entry_point", out.entry_point)) return false;

  if (out.type != "native" && out.type != "script") {
    error = "type must be \"native\" or \"script\"";
    return false;
  }
  if (!IsSemanticVersion(out.version)) {
    error = "version must be a semantic version such as 1.0.0";
    return false;
  }

  std::replace(out.entry_point.begin(), out.entry_point.end(), '\\', '/');
  if (out.entry_point.front() == '/' || out.entry_point.find("..") != std::string::npos) {
    error = "entry_point must stay inside the package";
    return false;
  }
  std::string lower_entry = ToLower(out.entry_point);
  if (out.type == "native" &&
      lower_entry.compare(lower_entry.size() >= 3 ? lower_entry.size() - 3 : 0, 3, ".so") != 0) {
    error = "a native mod's entry_point must be a .so file";
    return false;
  }
  if (out.type == "script" && (lower_entry.size() < 4 ||
                               lower_entry.compare(lower_entry.size() - 4, 4, ".lua") != 0)) {
    error = "a script mod's entry_point must be a .lua file";
    return false;
  }

  auto deps = root.find("dependencies");
  if (deps != root.end()) {
    if (!deps->is_array()) {
      error = "dependencies must be an array of mod names";
      return false;
    }
    for (const auto& item : *deps) {
      if (!item.is_string()) {
        error = "each dependency must be a string";
        return false;
      }
      std::string dep = Trim(item.get<std::string>());
      if (!dep.empty()) out.dependencies.push_back(dep);
    }
  }

  // Item C3: the permissions slot. Optional (a hand-written package may omit it, and the Java
  // writer emits an empty array), but when present it must be an array of strings, mirroring the
  // Kotlin parser - a package accepted by one side and rejected by the other is the exact drift
  // this struct's doc-comment warns about. Blank entries are dropped rather than stored.
  auto perms = root.find("permissions");
  if (perms != root.end()) {
    if (!perms->is_array()) {
      error = "permissions must be an array of capability names";
      return false;
    }
    for (const auto& item : *perms) {
      if (!item.is_string()) {
        error = "each permission must be a string";
        return false;
      }
      std::string permission = Trim(item.get<std::string>());
      if (!permission.empty()) out.permissions.push_back(permission);
    }
  }
  return true;
}

bool AntEggLoader::ReadManifest(const std::string& archive_path, AntEggManifest& out,
                                std::string& error) {
  std::vector<uint8_t> data;
  if (!ReadWholeFile(archive_path, data)) {
    error = "could not read package file";
    return false;
  }
  std::vector<ZipEntryRecord> entries;
  if (!ParseCentralDirectory(data, entries)) {
    error = "package is not a valid ZIP archive";
    return false;
  }
  const ZipEntryRecord* manifest_entry = nullptr;
  for (const auto& entry : entries) {
    if (entry.name == kManifestName) {
      manifest_entry = &entry;
      break;
    }
    if (entry.name.size() > std::strlen(kManifestName) &&
        entry.name.compare(entry.name.size() - std::strlen(kManifestName) - 1,
                           std::strlen(kManifestName) + 1,
                           std::string("/") + kManifestName) == 0) {
      if (!manifest_entry) manifest_entry = &entry;
    }
  }
  if (!manifest_entry) {
    error = std::string("package has no ") + kManifestName;
    return false;
  }
  std::vector<uint8_t> raw;
  if (!ExtractEntryData(data, *manifest_entry, raw)) {
    error = "could not decompress egg.json";
    return false;
  }
  return ParseManifest(std::string(raw.begin(), raw.end()), out, error);
}

bool AntEggLoader::Extract(const std::string& archive_path, const std::string& sandbox_root,
                           std::string& error) {
  AntEggManifest manifest;
  if (!ReadManifest(archive_path, manifest, error)) return false;

  std::vector<uint8_t> data;
  if (!ReadWholeFile(archive_path, data)) {
    error = "could not read package file";
    return false;
  }
  std::vector<ZipEntryRecord> entries;
  if (!ParseCentralDirectory(data, entries)) {
    error = "package is not a valid ZIP archive";
    return false;
  }
  if (entries.size() > kMaxEntries) {
    error = "package has too many entries";
    return false;
  }

  std::error_code ec;
  fs::path root(sandbox_root);
  fs::create_directories(root, ec);
  // Drop staging/backup directories a previous crash left behind, so they do not accumulate
  // (audit item 12.6). Only ones old enough to be certainly abandoned are touched.
  CleanStaleStagingDirs(root);
  fs::path stage = root / ("." + manifest.Id() + ".extracting");
  fs::path target = root / manifest.Id();
  fs::path backup = root / ("." + manifest.Id() + ".old");

  // Recovery (audit item 12.7): a crash between the two renames of the swap below leaves the
  // previous mod under `backup`. If the target is missing, restore it so the installed mod is never
  // lost; either way drop a stale backup so the swap starts clean.
  if (fs::exists(backup) && !fs::exists(target)) {
    fs::rename(backup, target, ec);
    ec.clear();
  }
  fs::remove_all(backup, ec);
  ec.clear();

  fs::remove_all(stage, ec);
  fs::create_directories(stage, ec);
  if (ec) {
    error = "could not create sandbox directory";
    return false;
  }

  uint64_t total = 0;
  for (const auto& entry : entries) {
    if (entry.name.empty()) continue;
    fs::path destination = stage / entry.name;
    if (!IsInside(stage, destination)) {
      fs::remove_all(stage, ec);
      error = "package entry escapes the sandbox: " + entry.name;
      return false;
    }
    if (entry.is_directory) {
      fs::create_directories(destination, ec);
      continue;
    }
    fs::create_directories(destination.parent_path(), ec);
    std::vector<uint8_t> content;
    if (!ExtractEntryData(data, entry, content)) {
      fs::remove_all(stage, ec);
      error = "could not decompress: " + entry.name;
      return false;
    }
    total += content.size();
    if (total > kMaxUncompressedBytes) {
      fs::remove_all(stage, ec);
      error = "package expands beyond the size limit";
      return false;
    }
    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    if (!file) {
      fs::remove_all(stage, ec);
      error = "could not write: " + entry.name;
      return false;
    }
    file.write(reinterpret_cast<const char*>(content.data()),
               static_cast<std::streamsize>(content.size()));
  }

  if (!fs::exists(stage / manifest.entry_point)) {
    fs::remove_all(stage, ec);
    error = "entry_point is missing from the package: " + manifest.entry_point;
    return false;
  }

  // Atomic swap (audit item 12.7): removing the target then renaming the stage into place leaves a
  // window where the target does not exist, so a crash there loses the installed mod. Move the old
  // target aside first, move the new one in, then drop the old one. If the second rename fails the
  // old mod is restored, so the swap is a no-op rather than a loss.
  ec.clear();
  fs::remove_all(backup, ec);
  ec.clear();
  if (fs::exists(target)) {
    fs::rename(target, backup, ec);
    if (ec) {
      fs::remove_all(stage, ec);
      error = "could not move the existing mod aside";
      return false;
    }
  }
  fs::rename(stage, target, ec);
  if (ec) {
    if (fs::exists(backup)) {
      std::error_code restoreEc;
      fs::rename(backup, target, restoreEc);
    }
    std::error_code cleanupEc;
    fs::remove_all(stage, cleanupEc);
    error = "could not move the extracted mod into place";
    return false;
  }
  std::error_code dropEc;
  fs::remove_all(backup, dropEc);
  return true;
}

bool AntEggLoader::LoadMod(const std::string& archive_path, const std::string& sandbox_root,
                           std::string& error) {
  return Extract(archive_path, sandbox_root, error);
}

}  // namespace pl::runtime
