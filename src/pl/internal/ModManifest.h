#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "pl/Mod.hpp"
#include "pl/legacy/LegacyMod.h"

namespace pl::internal::mod {

struct RuntimeModInfo {
  std::string modId;
  std::string displayName;
  std::string author;
  std::string version;
  std::string entryPath;
  std::string entryFileName;
  std::string libraryPath;
  std::string iconPath;
  std::string manifestPath;
  std::string modRootPath;
  /** The mod ids this mod declares it needs loaded first. Absent ids are not an error. */
  std::vector<std::string> dependencies;
  /** The game-version range this mod declares, or empty when unconstrained. */
  std::string gameVersionRange;
  PLModInfo legacyInfo{};
};

std::string normalizeLibraryPath(const std::filesystem::path &libraryPath);

bool createRuntimeModInfo(
    const std::filesystem::path &libraryPath,
    const std::optional<std::filesystem::path> &sourceModDirectory,
    RuntimeModInfo &storage);

pl::mod::ModInfo toCppModInfo(const RuntimeModInfo &storage);

} // namespace pl::internal::mod
