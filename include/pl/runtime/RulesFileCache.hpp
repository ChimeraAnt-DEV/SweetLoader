#pragma once

/**
 * @file RulesFileCache.hpp
 * @brief Caches the parsed signature-rules JSON, keyed on the file's path + mtime + size.
 *
 * `ReadConfiguredOptifineSlot`, `EvaluateConfiguredFeatureGate` and
 * `LoadConfiguredGameHookSignatures` each reopened and reparsed the rules file on every call, so a
 * single install re-read and re-parsed the same JSON several times. The parse is the expensive part
 * (nlohmann::json over a file that carries many rules), so the cache keeps the parsed document and
 * serves it until the file changes.
 *
 * The key is the path plus the file's mtime and size, so a rules file rewritten by a launcher update
 * is picked up without a process restart, and a repeat call with the same key is one map lookup. The
 * loader is injected so the whole rule is host-tested without touching the disk
 * (`RulesFileCacheTest`).
 *
 * Not thread-safe on its own: the caller holds its own mutex, exactly as `GameHookRules` already
 * guards `g_rulesPath`.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace pl::runtime {

/**
 * @brief Holds one parsed rules document and the file identity it was parsed from.
 */
class RulesFileCache {
public:
  using Document = std::shared_ptr<const nlohmann::json>;

  /**
   * @brief Returns the parsed document for @p path, loading it only when the file changed.
   *
   * @param path    the rules file path; empty returns null and never loads
   * @param mtimeMs the file's modification time in milliseconds
   * @param size    the file's size in bytes
   * @param load    a callable returning the parsed document (or null on failure)
   * @return the cached or freshly loaded document, or null when the load failed
   *
   * A null load is not cached, so a file that is briefly missing (written a moment later) is retried
   * on the next call rather than treated as "empty rules" for the rest of the session.
   */
  template <typename LoadFn>
  Document get(const std::string &path, std::int64_t mtimeMs, std::uint64_t size, LoadFn &&load) {
    if (path.empty()) return nullptr;
    if (mLoaded && path == mPath && mtimeMs == mMtimeMs && size == mSize) {
      return mDocument;
    }
    Document document = load();
    if (document == nullptr) return nullptr;
    mPath = path;
    mMtimeMs = mtimeMs;
    mSize = size;
    mDocument = document;
    mLoaded = true;
    return document;
  }

  /** Forces the next call to reload, used when the launcher signals a rules update. */
  void invalidate() {
    mLoaded = false;
    mDocument.reset();
  }

private:
  bool mLoaded = false;
  std::string mPath;
  std::int64_t mMtimeMs = 0;
  std::uint64_t mSize = 0;
  Document mDocument;
};

} // namespace pl::runtime
