#include "pl/memory/Signature.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <dlfcn.h>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/memory/AtomicFileWrite.hpp"
#include "pl/memory/ElfBuildId.hpp"
#include "pl/memory/ModulePathMatch.hpp"
#include "pl/memory/SignaturePattern.hpp"
#include "pl/memory/SignatureScanCache.hpp"

#include <filesystem>
#include <fstream>

namespace pl::memory {
namespace {

// The pattern type and its parsing live in the pure, host-tested SignaturePattern.hpp so the rules
// can be exercised without scanning a live process. These aliases keep the rest of this file (the
// /proc walk, the Aho-Corasick scan) reading as before.
using PatternByte = pl::memory::PatternByte;
using ParsedPattern = pl::memory::SignaturePattern;

struct MemoryRegion {
  uintptr_t start = 0;
  uintptr_t end = 0;
  bool executable = false;
  bool writable = false;
};

struct ModuleInfo {
  /** Readable regions, in map order. */
  std::vector<MemoryRegion> regions;
  /** The executable subset of `regions`; code signatures are scanned only here. */
  std::vector<MemoryRegion> execRegions;
  /**
   * The read-only, non-executable subset. A code signature is scanned in `execRegions`; this is the
   * fallback for a module with no executable mapping, so the fallback never reads a writable buffer.
   */
  std::vector<MemoryRegion> readOnlyRegions;
  /** The lowest mapped address, used as the identity of this load. */
  uintptr_t base = 0;
  /** The ELF build id, or empty when it could not be read. Changes on every relink. */
  std::string buildId;
};

struct CompiledPattern {
  std::string signature;
  ParsedPattern pattern;
};

struct AnchorNode {
  std::array<int, 256> next{};
  int failure = 0;
  std::vector<size_t> outputs;

  AnchorNode() { next.fill(-1); }
};

std::unordered_map<std::string, uintptr_t> sigCache;
std::unordered_map<std::string, ParsedPattern> patternCache;
std::shared_mutex cacheMutex;

// Thin adapters over the pure parser in SignaturePattern.hpp, so the scan below reads unchanged.
inline ParsedPattern parsePattern(std::string_view signature) {
  return pl::memory::ParseSignaturePattern(signature);
}

inline bool matches(PatternByte pattern, uint8_t value) {
  return pl::memory::PatternMatches(pattern, value);
}

inline bool isExactByte(PatternByte byte) { return pl::memory::IsExactByte(byte); }

inline void selectAnchor(ParsedPattern &pattern) { pl::memory::SelectAnchor(pattern); }

// Defined below; forward-declared so module collection can probe the ELF header.
size_t ReadRegionSafely(uintptr_t start, size_t size, uint8_t *out);

// Whether a module is still mapped at the cached addresses, so a cached region list can be reused.
//
// Two checks, both cheap and both without re-parsing `/proc/self/maps`:
//   1. every cached region still answers a one-byte fault-safe read, and
//   2. the load base still starts with the ELF magic.
//
// The second is what catches a module that was unmapped and reloaded at a *different* base: the old
// addresses may still be mapped (by another library, or by the reloaded copy at an address that
// happens to cover the probe), so a probe alone is not enough. A non-ELF first word means the cached
// region list no longer describes this module, and the caller re-reads it.
bool RegionsAreMapped(const ModuleInfo &module) {
  if (module.regions.empty()) return false;
  unsigned char probe = 0;
  for (const auto &region : module.regions) {
    iovec local{&probe, 1};
    iovec remote{reinterpret_cast<void *>(region.start), 1};
    if (::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) != 1) {
      return false;
    }
  }

  unsigned char magic[4] = {};
  const size_t magicRead = ReadRegionSafely(module.base, sizeof(magic), magic);
  if (magicRead != sizeof(magic)) return false;
  return magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

// Collects a module's readable regions, and its executable subset, by matching the **basename** of
// the maps path column exactly. A `strstr` on the whole line also matches a path that merely
// contains the name (`libpreloader_extra.so` for `libpreloader.so`), which would scan the wrong
// regions and can resolve a pattern into an unrelated library.
bool getModuleInfo(const std::string &name, ModuleInfo &out) {
  FILE *maps = std::fopen("/proc/self/maps", "r");
  if (!maps) return false;

  bool first = true;
  char line[4096];
  while (std::fgets(line, sizeof(line), maps)) {
    MapsLine parsed{};
    if (!ParseMapsLine(line, parsed)) continue;
    if (!MapsPathNamesModule(parsed.path, name)) continue;
    if (!MapsPermsReadable(parsed.perms)) continue;

    const bool executable = MapsPermsExecutable(parsed.perms);
    const bool writable = MapsPermsWritable(parsed.perms);
    if (!out.regions.empty() && out.regions.back().end == parsed.start &&
        out.regions.back().executable == executable &&
        out.regions.back().writable == writable) {
      out.regions.back().end = parsed.end;
    } else {
      out.regions.push_back(MemoryRegion{parsed.start, parsed.end, executable, writable});
    }
    if (executable) {
      if (!out.execRegions.empty() && out.execRegions.back().end == parsed.start &&
          out.execRegions.back().writable == writable) {
        out.execRegions.back().end = parsed.end;
      } else {
        out.execRegions.push_back(MemoryRegion{parsed.start, parsed.end, true, writable});
      }
    } else if (!writable) {
      if (!out.readOnlyRegions.empty() && out.readOnlyRegions.back().end == parsed.start) {
        out.readOnlyRegions.back().end = parsed.end;
      } else {
        out.readOnlyRegions.push_back(MemoryRegion{parsed.start, parsed.end, false, false});
      }
    }
    if (first || parsed.start < out.base) out.base = parsed.start;
    first = false;
  }
  std::fclose(maps);
  if (out.regions.empty()) return false;

  // The ELF build id identifies this exact binary, so a cached scan can be trusted only for the
  // same build. It lives in the first page, so a small fault-safe read of the load base is enough.
  constexpr std::size_t kHeaderProbeBytes = 4096;
  std::vector<std::uint8_t> header(std::min(kHeaderProbeBytes, out.regions.front().end -
                                                                 out.regions.front().start));
  const size_t headerRead =
      ReadRegionSafely(out.regions.front().start, header.size(), header.data());
  if (headerRead > 0) {
    ReadElfBuildId(header.data(), headerRead, out.buildId);
  }

  return true;
}

// Looks a signature up as an exported symbol, for the case where a rule names one.
//
// The handle is opened with `RTLD_NOLOAD` only, so a lookup can never load a library as a side
// effect, and it is closed immediately: the previous version kept the handle in the cached module
// record, so every module the resolver touched leaked a reference for the life of the process and
// nothing ever released it. The symbol address stays valid after the close because the library was
// already loaded by someone else — closing only drops the reference this call added. A failed open
// is not an error: the module is still scanned by byte pattern.
void *ResolveExportedSymbol(std::string_view moduleName, std::string_view signature) {
  // Both are NUL-terminated for the duration of this call: a module name and a signature come from
  // the rules as ordinary strings, and `dlopen`/`dlsym` only read them synchronously.
  const std::string module(moduleName);
  const std::string symbolName(signature);
  void *handle = dlopen(module.c_str(), RTLD_LAZY | RTLD_NOLOAD);
  if (handle == nullptr) return nullptr;
  void *symbol = dlsym(handle, symbolName.c_str());
  dlclose(handle);
  return symbol;
}

// The module cache. A cached entry is re-validated by probing that its regions are still mapped, so
// a module that was unmapped or reloaded at different addresses is re-read instead of answering from
// a stale region list.
std::unordered_map<std::string, ModuleInfo> moduleCache;

// The on-disk scan cache directory. Empty until the launcher sets it, which keeps
// the default behaviour exactly as before: no file is read and none is written.
std::mutex g_cacheDirMutex;
std::string g_cacheDir;

std::string CacheDirectory() {
  std::lock_guard lock(g_cacheDirMutex);
  return g_cacheDir;
}

// One file per module build id. The name is derived from the module name and the build id, both of
// which come from the rules and `/proc/self/maps`; the build id is hex, so it is safe in a path.
std::filesystem::path CacheFilePath(const std::string &directory,
                                    std::string_view moduleName,
                                    std::string_view buildId) {
  std::string fileName;
  fileName.reserve(moduleName.size() + buildId.size() + 16);
  fileName.append(moduleName).append(".").append(buildId).append(".sigcache");
  return std::filesystem::path(directory) / fileName;
}

// Loads the cached offsets for a module and adds the ones inside the module's current load base.
// A cached offset that would fall outside every region is dropped rather than returned: it would
// resolve to an unrelated address after a partial reload.
void ApplyDiskCache(const ModuleInfo &module, std::string_view moduleName,
                    const std::vector<std::string> &signatures,
                    std::unordered_map<std::string, uintptr_t> &results) {
  const std::string directory = CacheDirectory();
  if (directory.empty() || module.buildId.empty()) return;

  const std::filesystem::path path = CacheFilePath(directory, moduleName, module.buildId);
  std::ifstream file(path, std::ios::binary);
  if (!file) return;
  const std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

  std::unordered_map<std::string, uintptr_t> cached;
  if (!ParseSignatureScanCache(text, module.buildId, cached) || cached.empty()) return;

  auto insideModule = [&module](uintptr_t address) {
    for (const auto &region : module.regions) {
      if (address >= region.start && address < region.end) return true;
    }
    return false;
  };

  size_t applied = 0;
  for (const auto &signature : signatures) {
    const auto it = cached.find(signature);
    if (it == cached.end()) continue;
    const uintptr_t address = module.base + it->second;
    if (!insideModule(address)) continue;
    results[signature] = address;
    ++applied;
  }
  if (applied > 0) {
    preloaderLogger.debug("signature scan cache: {} of {} resolved for {} (build {})", applied,
                          signatures.size(), moduleName, module.buildId);
  }
}

// Merges the freshly resolved offsets into the cache file for this module build. Only non-zero
// results are written; a miss is re-derived next launch because a scan is cheaper than trusting a
// stale "not found" after the launcher changes its rules.
void UpdateDiskCache(const ModuleInfo &module, std::string_view moduleName,
                     const std::vector<std::string> &signatures,
                     const std::unordered_map<std::string, uintptr_t> &results) {
  const std::string directory = CacheDirectory();
  if (directory.empty() || module.buildId.empty() || module.base == 0) return;

  const std::filesystem::path path = CacheFilePath(directory, moduleName, module.buildId);

  std::unordered_map<std::string, uintptr_t> merged;
  {
    std::ifstream file(path, std::ios::binary);
    if (file) {
      const std::string text((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
      ParseSignatureScanCache(text, module.buildId, merged);
    }
  }

  bool changed = false;
  for (const auto &signature : signatures) {
    const auto it = results.find(signature);
    if (it == results.end() || it->second == 0) continue;
    const uintptr_t offset = it->second - module.base;
    const auto existing = merged.find(signature);
    if (existing != merged.end() && existing->second == offset) continue;
    merged[signature] = offset;
    changed = true;
  }
  if (!changed) return;

  const std::string text = SerializeSignatureScanCache(module.buildId, merged);
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  // Atomic: a kill mid-write must not truncate the cache the previous launch wrote, or the next
  // launch pays a full scan. WriteFileAtomically renames a complete temporary over the target.
  pl::memory::WriteFileAtomically(path, text);
}

ModuleInfo getModuleInfoFresh(const std::string &moduleName) {
  {
    std::shared_lock lock(cacheMutex);
    const auto it = moduleCache.find(moduleName);
    if (it != moduleCache.end()) {
      ModuleInfo cached = it->second;
      // Cheap identity re-check: the regions must still be mapped. This costs a few
      // `process_vm_readv` calls and catches a module that was unmapped since it was cached.
      if (RegionsAreMapped(cached)) return cached;
      // Fall through to a fresh read; the stale entry is replaced below.
    }
  }

  ModuleInfo module;
  if (!getModuleInfo(moduleName, module)) return {};

  std::unique_lock lock(cacheMutex);
  moduleCache[moduleName] = module;
  return module;
}

ParsedPattern getCachedPattern(const std::string &signature) {
  {
    std::shared_lock lock(cacheMutex);
    const auto it = patternCache.find(signature);
    if (it != patternCache.end()) return it->second;
  }

  ParsedPattern pattern = parsePattern(signature);
  std::unique_lock lock(cacheMutex);
  const auto [it, inserted] = patternCache.emplace(signature, pattern);
  return inserted ? pattern : it->second;
}

// The full "does the signature match here" check lives in SignaturePattern.hpp
// (PatternMatchesAt), so the scan and the host test agree on one definition and an overlapping-hit
// case can be pinned without a live process. `matches` remains for the single-byte masked-anchor
// probe below, which must not build a pattern.

std::vector<CompiledPattern>
compilePatterns(const std::vector<std::string> &signatures,
                std::unordered_map<std::string, uintptr_t> &results) {
  std::vector<CompiledPattern> compiled;
  compiled.reserve(signatures.size());
  for (const auto &signature : signatures) {
    auto pattern = getCachedPattern(signature);
    if (pattern.bytes.empty()) {
      preloaderLogger.warn("invalid signature pattern: {}", signature);
      results[signature] = 0;
      continue;
    }
    selectAnchor(pattern);
    compiled.push_back(CompiledPattern{signature, std::move(pattern)});
  }
  return compiled;
}

std::vector<AnchorNode>
buildAnchorAutomaton(const std::vector<CompiledPattern> &patterns,
                     const std::vector<bool> &active,
                     std::vector<size_t> &maskedPatterns) {
  std::vector<AnchorNode> nodes(1);

  for (size_t index = 0; index < patterns.size(); ++index) {
    if (!active[index]) continue;
    const auto &pattern = patterns[index].pattern;
    bool exact = true;
    for (size_t i = 0; i < pattern.anchorSize; ++i) {
      if (!isExactByte(pattern.bytes[pattern.anchorIndex + i])) {
        exact = false;
        break;
      }
    }
    if (!exact) {
      maskedPatterns.push_back(index);
      continue;
    }

    int state = 0;
    for (size_t i = 0; i < pattern.anchorSize; ++i) {
      const uint8_t value = pattern.bytes[pattern.anchorIndex + i].value;
      int next = nodes[state].next[value];
      if (next == -1) {
        next = static_cast<int>(nodes.size());
        nodes[state].next[value] = next;
        nodes.emplace_back();
      }
      state = next;
    }
    nodes[state].outputs.push_back(index);
  }

  std::queue<int> queue;
  for (size_t value = 0; value < 256; ++value) {
    int &next = nodes[0].next[value];
    if (next == -1) {
      next = 0;
    } else {
      nodes[next].failure = 0;
      queue.push(next);
    }
  }

  while (!queue.empty()) {
    const int state = queue.front();
    queue.pop();
    for (size_t value = 0; value < 256; ++value) {
      int &next = nodes[state].next[value];
      if (next == -1) {
        next = nodes[nodes[state].failure].next[value];
        continue;
      }

      const int failure = nodes[nodes[state].failure].next[value];
      nodes[next].failure = failure;
      const auto &outputs = nodes[failure].outputs;
      nodes[next].outputs.insert(nodes[next].outputs.end(), outputs.begin(),
                                 outputs.end());
      queue.push(next);
    }
  }

  return nodes;
}

// Reads a region into a local buffer through `process_vm_readv`, which faults on the reader's side
// rather than the target's: a region that unmaps between the maps parse and the read makes the
// syscall return short, and the caller skips it instead of dereferencing freed memory. The returned
// count is the number of bytes actually read.
size_t ReadRegionSafely(uintptr_t start, size_t size, uint8_t *out) {
  if (size == 0) return 0;
  iovec local{out, size};
  iovec remote{reinterpret_cast<void *>(start), size};
  const ssize_t read = ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0);
  return read > 0 ? static_cast<size_t>(read) : 0;
}

void scanCompiledPatterns(const std::vector<MemoryRegion> &regions,
                          const std::vector<CompiledPattern> &patterns,
                          std::unordered_map<std::string, uintptr_t> &results) {
  if (patterns.empty()) return;

  std::vector<uintptr_t> found(patterns.size(), 0);
  std::vector<bool> active(patterns.size(), true);
  size_t unresolved = patterns.size();

  if (regions.empty()) {
    unresolved = 0;
  } else {
    for (size_t i = 0; i < patterns.size(); ++i) {
      if (patterns[i].pattern.checkIndices.empty()) {
        found[i] = regions.front().start;
        active[i] = false;
        --unresolved;
      }
    }
  }

  std::vector<size_t> maskedPatterns;
  const auto nodes = buildAnchorAutomaton(patterns, active, maskedPatterns);

  auto tryMatch = [&](uintptr_t bufferBase, const uint8_t *data, size_t bufferSize,
                      size_t anchorOffset, size_t patternIndex) {
    if (!active[patternIndex]) return;
    const auto &pattern = patterns[patternIndex].pattern;
    if (anchorOffset < pattern.anchorIndex) return;

    const size_t candidateOffset = anchorOffset - pattern.anchorIndex;
    if (!pl::memory::PatternMatchesAt(data, bufferSize, candidateOffset, pattern)) {
      return;
    }

    found[patternIndex] = bufferBase + candidateOffset;
    active[patternIndex] = false;
    --unresolved;
  };

  // A region can be large (libminecraftpe's `.text` is ~220 MB), so it is read in windows. Each
  // window is copied out with a fault-safe read; consecutive windows overlap by the longest pattern
  // so a match that straddles a window boundary is still found.
  size_t longestPattern = 1;
  for (const auto &pattern : patterns) {
    longestPattern = std::max(longestPattern, pattern.pattern.bytes.size());
  }
  const size_t overlap = longestPattern > 1 ? longestPattern - 1 : 0;
  constexpr size_t kWindowBytes = 1U << 20;
  const size_t step = kWindowBytes > overlap ? kWindowBytes - overlap : kWindowBytes;
  std::vector<uint8_t> buffer(kWindowBytes);

  for (const auto &region : regions) {
    if (unresolved == 0) break;
    const size_t regionSize = region.end - region.start;

    for (size_t windowStart = 0; windowStart < regionSize && unresolved != 0;
         windowStart += step) {
      const size_t windowSize = std::min(kWindowBytes, regionSize - windowStart);
      const size_t read = ReadRegionSafely(region.start + windowStart, windowSize, buffer.data());
      if (read == 0) {
        // The region unmapped or is unreadable; skip it rather than dereference it.
        preloaderLogger.debug("signature scan: skipped an unreadable region at {:x}",
                              static_cast<unsigned long long>(region.start));
        continue;
      }
      const uintptr_t bufferBase = region.start + windowStart;
      const auto *data = buffer.data();
      int state = 0;

      for (size_t offset = 0; offset < read && unresolved != 0; ++offset) {
        state = nodes[state].next[data[offset]];
        for (const size_t patternIndex : nodes[state].outputs) {
          const auto anchorSize = patterns[patternIndex].pattern.anchorSize;
          if (offset + 1 >= anchorSize) {
            tryMatch(bufferBase, data, read, offset + 1 - anchorSize, patternIndex);
          }
        }

        for (const size_t patternIndex : maskedPatterns) {
          if (!active[patternIndex]) continue;
          const auto &pattern = patterns[patternIndex].pattern;
          if (matches(pattern.bytes[pattern.anchorIndex], data[offset])) {
            tryMatch(bufferBase, data, read, offset, patternIndex);
          }
        }
      }
    }
  }

  for (size_t i = 0; i < patterns.size(); ++i) {
    results[patterns[i].signature] = found[i];
  }
}

// The cache key includes the module's ELF build id, so a scan result is only reused for the exact
// binary it was derived from: a game update relinks libminecraftpe and the offsets are re-derived
// rather than answered from the previous build. Without a build id the module name is the key, which
// is the pre-existing behaviour.
std::string makeSignatureCacheKey(std::string_view moduleName, std::string_view buildId,
                                  std::string_view signature) {
  std::string key;
  key.reserve(moduleName.size() + buildId.size() + signature.size() + 4);
  key.append(moduleName).append("::").append(buildId).append("::").append(signature);
  return key;
}

}

std::unordered_map<std::string, uintptr_t>
resolveSignatures(std::span<const std::string> signatures,
                  std::string_view moduleName) {
  std::unordered_map<std::string, uintptr_t> results;
  std::vector<std::string> pending;
  std::unordered_map<std::string, size_t> pendingLookup;

  if (moduleName.empty()) {
    for (const auto &signature : signatures) results[signature] = 0;
    return results;
  }

  // The module is resolved first, because the cache key carries its build id: a scan result is only
  // reusable for the exact binary it was derived from, so a game update cannot answer from the
  // previous build's offsets.
  const ModuleInfo module = getModuleInfoFresh(std::string(moduleName));

  {
    std::shared_lock lock(cacheMutex);
    for (const auto &signature : signatures) {
      const auto key = makeSignatureCacheKey(moduleName, module.buildId, signature);
      const auto cached = sigCache.find(key);
      if (cached != sigCache.end()) {
        results[signature] = cached->second;
      } else if (pendingLookup.emplace(signature, pending.size()).second) {
        pending.push_back(signature);
      }
    }
  }

  // The on-disk cache is consulted before the scan. It stores offsets from the
  // load base keyed by the ELF build id, so a repeat launch of the same build skips the scan
  // entirely. A cached entry that now falls outside the module is dropped by ApplyDiskCache.
  if (!pending.empty() && !module.regions.empty()) {
    ApplyDiskCache(module, moduleName, pending, results);
    // Memoise the disk hits too, not only the scan results: a disk hit that is not memoised makes
    // every later resolve re-open and re-parse the cache file (audit item 23).
    {
      std::unique_lock lock(cacheMutex);
      for (const auto &signature : pending) {
        const auto it = results.find(signature);
        if (it != results.end() && it->second != 0) {
          sigCache[makeSignatureCacheKey(moduleName, module.buildId, signature)] = it->second;
        }
      }
    }
    pending.erase(std::remove_if(pending.begin(), pending.end(),
                                 [&results](const std::string &signature) {
                                   const auto it = results.find(signature);
                                   return it != results.end() && it->second != 0;
                                 }),
                  pending.end());
  }

  if (pending.empty()) return results;

  if (module.regions.empty()) {
    for (const auto &signature : pending) results[signature] = 0;
  } else {
    std::vector<std::string> patterns;
    patterns.reserve(pending.size());
    for (const auto &signature : pending) {
      if (void *symbol = ResolveExportedSymbol(moduleName, signature)) {
        results[signature] = reinterpret_cast<uintptr_t>(symbol);
        continue;
      }
      patterns.push_back(signature);
    }

    if (!patterns.empty() && module.execRegions.empty() && module.readOnlyRegions.empty()) {
      // A module with no executable mapping and no read-only mapping has nothing safe to scan.
      // Falling back to its writable regions could resolve a code pattern into a mutable buffer.
      preloaderLogger.warn("signature scan: {} has no executable or read-only region to scan",
                           std::string(moduleName));
      for (const auto &signature : patterns) results[signature] = 0;
    } else if (!patterns.empty()) {
      const auto compiled = compilePatterns(patterns, results);
      // Code signatures are scanned only in the module's executable regions. A data section is
      // never the target of a byte-pattern code signature, and including writable mappings both
      // wastes the scan and can resolve a pattern to a mutable buffer that happens to hold the
      // bytes.
      const std::vector<MemoryRegion> &scanRegions =
          module.execRegions.empty() ? module.readOnlyRegions : module.execRegions;
      scanCompiledPatterns(scanRegions, compiled, results);
    }
  }

  {
    std::unique_lock lock(cacheMutex);
    for (const auto &signature : pending) {
      const auto it = results.find(signature);
      // Do not memoise a miss. A miss is often transient: resolution can run before the module is
      // fully mapped (PreLoader.cpp documents the early attempt), so caching the 0 would make the
      // later, correct attempt answer 0 for the life of the process (audit item 32). A genuine
      // "pattern absent" is re-derived each launch by design (see UpdateDiskCache).
      if (it != results.end() && it->second != 0) {
        sigCache[makeSignatureCacheKey(moduleName, module.buildId, signature)] = it->second;
      }
    }
  }

  // Persist the newly resolved offsets for the next launch, outside the cache lock so file IO does
  // not block a concurrent resolve. Only the signatures this call resolved are merged, so a
  // concurrent resolve of a different set cannot clobber them.
  if (!module.regions.empty()) {
    UpdateDiskCache(module, moduleName, pending, results);
  }
  return results;
}

void SetSignatureCacheDirectory(std::string_view directory) {
  std::lock_guard lock(g_cacheDirMutex);
  g_cacheDir.assign(directory);
}

uintptr_t resolveSignature(std::string_view signature,
                           std::string_view moduleName) {
  std::vector<std::string> signatures{std::string(signature)};
  const auto results = resolveSignatures(signatures, moduleName);
  const auto it = results.find(std::string(signature));
  return it == results.end() ? 0 : it->second;
}

bool isExecutableAddress(uintptr_t address, std::string_view moduleName) {
  if (address == 0 || moduleName.empty()) return false;
  const ModuleInfo module = getModuleInfoFresh(std::string(moduleName));
  for (const auto &region : module.execRegions) {
    if (address >= region.start && address < region.end) return true;
  }
  return false;
}

}
