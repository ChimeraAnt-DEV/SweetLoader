/**
 * @file AtomicFileWriteTest.cpp
 * @brief Host test for the atomic file write used by the signature scan cache.
 *
 * A truncated cache is worse than a missing one: the parser rejects it, and the previous good cache is
 * gone, so the next launch pays a full scan. These checks pin that a write is all-or-nothing and that a
 * failed write leaves the old file intact.
 */

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>

#include "pl/memory/AtomicFileWrite.hpp"

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

std::string readAll(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int countTempSiblings(const std::filesystem::path &dir, const std::string &stem) {
  int n = 0;
  for (const auto &entry : std::filesystem::directory_iterator(dir)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind(stem + ".tmp.", 0) == 0) ++n;
  }
  return n;
}

} // namespace

int main() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "pl_atomic_write_test";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  const std::filesystem::path target = dir / "cache.bin";

  // A fresh write lands complete and leaves no temporary behind.
  {
    expect(pl::memory::WriteFileAtomically(target, "hello-cache"), "the first write succeeds");
    expect(readAll(target) == "hello-cache", "the content is written whole");
    expect(countTempSiblings(dir, "cache.bin") == 0, "no temporary file is left behind");
  }

  // An overwrite replaces the file in one step.
  {
    expect(pl::memory::WriteFileAtomically(target, "second-version-longer"),
           "the overwrite succeeds");
    expect(readAll(target) == "second-version-longer", "the new content replaces the old");
    expect(countTempSiblings(dir, "cache.bin") == 0, "no temporary file is left behind");
  }

  // A write to a path whose parent does not exist fails and does not create the target.
  {
    const std::filesystem::path missing = dir / "no-such-dir" / "cache.bin";
    expect(!pl::memory::WriteFileAtomically(missing, "nope"), "a write into a missing dir fails");
    expect(!std::filesystem::exists(missing), "a failed write does not create the target");
    expect(countTempSiblings(dir, "cache.bin") == 0, "a failed write leaves no temporary behind");
  }

  // The old file survives a failed rename (the parent is made read-only so the rename cannot land).
  // Skipped when running as root, where the permission bits do not apply.
  if (geteuid() != 0) {
    const std::filesystem::path roDir = dir / "readonly";
    std::filesystem::create_directories(roDir, ec);
    const std::filesystem::path roTarget = roDir / "cache.bin";
    expect(pl::memory::WriteFileAtomically(roTarget, "original"), "the setup write succeeds");
    std::filesystem::permissions(roDir, std::filesystem::perms::owner_read |
                                            std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    const bool wrote = pl::memory::WriteFileAtomically(roTarget, "replacement");
    expect(!wrote, "a write that cannot be renamed reports failure");
    expect(readAll(roTarget) == "original", "the old file survives a failed write");
    std::filesystem::permissions(roDir, std::filesystem::perms::all,
                                 std::filesystem::perm_options::replace, ec);
  }

  std::filesystem::remove_all(dir, ec);
  std::printf("AtomicFileWriteTest: %d checks passed\n", checks);
  return 0;
}
