#include "ModManifest.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "pl/Logger.hpp"
#include "pl/internal/ModManifestValidation.hpp"

namespace pl::internal::mod {
namespace {
constexpr const char *kManifestFileName = "manifest.json";
constexpr const char *kPreloadNativeType = "preload-native";

struct ParsedModDirectory {
  std::string id;
  std::string displayName;
  std::string author;
  std::string version;
  std::string entryPath;
  std::string entryFileName;
  std::string iconPath;
  std::filesystem::path rootPath;
  std::filesystem::path manifestPath;
  std::vector<std::string> dependencies;
  std::string gameVersionRange;
};

bool endsWithSo(const std::string &filename) {
  return filename.size() > 3 &&
         filename.compare(filename.size() - 3, 3, ".so") == 0;
}

bool isSafeRelativePath(const std::filesystem::path &path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }

  for (const auto &part : path) {
    const auto component = part.string();
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
  }

  return true;
}

std::string getOptionalString(const nlohmann::json &object, const char *key) {
  if (!object.contains(key) || !object[key].is_string()) {
    return {};
  }
  return object[key].get<std::string>();
}

std::optional<std::string> readTextFile(const std::filesystem::path &path) {
  FILE *file = std::fopen(path.string().c_str(), "rb");
  if (!file) {
    return std::nullopt;
  }

  std::string content;
  char buffer[4096];
  while (true) {
    const size_t bytesRead = std::fread(buffer, 1, sizeof(buffer), file);
    if (bytesRead > 0) {
      content.append(buffer, bytesRead);
    }

    if (bytesRead < sizeof(buffer)) {
      if (std::ferror(file)) {
        std::fclose(file);
        return std::nullopt;
      }
      break;
    }
  }

  std::fclose(file);
  return content;
}

std::optional<ParsedModDirectory>
parseModDirectory(const std::filesystem::path &modDirectory) {
  namespace fs = std::filesystem;

  const fs::path manifestPath = modDirectory / kManifestFileName;
  if (!fs::exists(manifestPath) || !fs::is_regular_file(manifestPath)) {
    return std::nullopt;
  }

  const auto manifestContent = readTextFile(manifestPath);
  if (!manifestContent.has_value()) {
    return std::nullopt;
  }

  nlohmann::json manifestJson;
  try {
    manifestJson = nlohmann::json::parse(*manifestContent);
  } catch (const std::exception &ex) {
    sweetLoaderLogger.warn("Failed to parse manifest {}: {}",
                         manifestPath.string(), ex.what());
    return std::nullopt;
  }

  if (!manifestJson.is_object()) {
    return std::nullopt;
  }

  const auto type = getOptionalString(manifestJson, "type");
  if (type != kPreloadNativeType) {
    return std::nullopt;
  }

  const auto rawEntryPath = getOptionalString(manifestJson, "entry");
  if (rawEntryPath.empty()) {
    return std::nullopt;
  }

  std::string entryPath = rawEntryPath;
  std::replace(entryPath.begin(), entryPath.end(), '\\', '/');
  const fs::path relativeEntryPath(entryPath);
  if (!isSafeRelativePath(relativeEntryPath)) {
    sweetLoaderLogger.warn("Rejected invalid mod entry path {} in {}", entryPath,
                         manifestPath.string());
    return std::nullopt;
  }

  const fs::path entryFilePath = modDirectory / relativeEntryPath;
  if (!fs::exists(entryFilePath) || !fs::is_regular_file(entryFilePath)) {
    sweetLoaderLogger.warn("Mod entry {} declared in {} does not exist",
                         entryFilePath.string(), manifestPath.string());
    return std::nullopt;
  }

  const auto entryFileName = entryFilePath.filename().string();
  if (!endsWithSo(entryFileName)) {
    sweetLoaderLogger.warn("Mod entry {} is not a .so file",
                         entryFilePath.string());
    return std::nullopt;
  }

  std::string iconPath = getOptionalString(manifestJson, "icon");
  if (!iconPath.empty()) {
    std::replace(iconPath.begin(), iconPath.end(), '\\', '/');
    const fs::path relativeIconPath(iconPath);
    if (!isSafeRelativePath(relativeIconPath) ||
        !fs::exists(modDirectory / relativeIconPath) ||
        !fs::is_regular_file(modDirectory / relativeIconPath)) {
      sweetLoaderLogger.warn("Ignoring invalid mod icon {} in {}", iconPath,
                           manifestPath.string());
      iconPath.clear();
    }
  }

  std::string displayName = getOptionalString(manifestJson, "name");
  if (displayName.empty()) {
    displayName = modDirectory.filename().string();
  }

  // Validate the declared fields, and refuse with the field named. A manifest whose
  // version is malformed or whose abi is a 32-bit one must not reach the loader, because the
  // failure would otherwise surface later as a bare "mod did not load" with nothing to act on.
  ModManifestFields declared;
  declared.id = modDirectory.filename().string();
  declared.name = displayName;
  declared.version = getOptionalString(manifestJson, "version");
  declared.abi = getOptionalString(manifestJson, "abi");
  declared.gameVersionRange = getOptionalString(manifestJson, "game_version");
  declared.entryFileName = entryFileName;
  const ModManifestValidation validation = ValidateModManifest(declared);
  if (!validation.ok) {
    sweetLoaderLogger.warn("Rejected mod manifest {}: field '{}' {}",
                         manifestPath.string(), ModManifestFieldName(validation.field),
                         validation.reason);
    return std::nullopt;
  }

  // The declared dependencies drive the load order. A malformed entry (a non-string, an
  // empty name) is skipped rather than failing the manifest: a missing dependency is not fatal, and
  // one bad entry must not make an otherwise-loadable mod unloadable.
  std::vector<std::string> dependencies;
  if (manifestJson.contains("dependencies") && manifestJson["dependencies"].is_array()) {
    for (const auto &dependency : manifestJson["dependencies"]) {
      if (dependency.is_string()) {
        const std::string value = dependency.get<std::string>();
        if (IsNonBlank(value)) dependencies.push_back(value);
      }
    }
  }

  return ParsedModDirectory{
      .id = modDirectory.filename().string(),
      .displayName = std::move(displayName),
      .author = getOptionalString(manifestJson, "author"),
      .version = getOptionalString(manifestJson, "version"),
      .entryPath = entryPath,
      .entryFileName = entryFileName,
      .iconPath = std::move(iconPath),
      .rootPath = modDirectory,
      .manifestPath = manifestPath,
      .dependencies = std::move(dependencies),
      .gameVersionRange = declared.gameVersionRange,
  };
}

std::optional<std::filesystem::path>
findModRootForLibraryPath(const std::filesystem::path &libraryPath) {
  namespace fs = std::filesystem;

  if (!fs::exists(libraryPath) || !fs::is_regular_file(libraryPath)) {
    return std::nullopt;
  }

  for (auto current = libraryPath.parent_path(); !current.empty();
       current = current.parent_path()) {
    if (const auto parsed = parseModDirectory(current); parsed.has_value()) {
      return current;
    }

    const auto parent = current.parent_path();
    if (parent == current) {
      break;
    }
  }

  return std::nullopt;
}

void finalizeRuntimeModInfo(RuntimeModInfo &storage) {
  storage.legacyInfo = PLModInfo{
      .size = sizeof(PLModInfo),
      .mod_id = storage.modId.c_str(),
      .display_name = storage.displayName.c_str(),
      .author = storage.author.c_str(),
      .version = storage.version.c_str(),
      .entry_path = storage.entryPath.c_str(),
      .entry_file_name = storage.entryFileName.c_str(),
      .library_path = storage.libraryPath.c_str(),
      .icon_path = storage.iconPath.c_str(),
      .manifest_path = storage.manifestPath.c_str(),
      .mod_root_path = storage.modRootPath.c_str(),
  };
}

} // namespace

std::string normalizeLibraryPath(const std::filesystem::path &libraryPath) {
  namespace fs = std::filesystem;

  std::error_code errorCode;
  const fs::path canonicalPath = fs::weakly_canonical(libraryPath, errorCode);
  if (!errorCode && !canonicalPath.empty()) {
    return canonicalPath.string();
  }

  return libraryPath.lexically_normal().string();
}

bool createRuntimeModInfo(
    const std::filesystem::path &libraryPath,
    const std::optional<std::filesystem::path> &sourceModDirectory,
    RuntimeModInfo &storage) {
  namespace fs = std::filesystem;

  const auto modRoot = sourceModDirectory.has_value()
                           ? sourceModDirectory
                           : findModRootForLibraryPath(libraryPath);
  if (!modRoot.has_value()) {
    sweetLoaderLogger.error("Failed to resolve mod root for library {}",
                          libraryPath.string());
    return false;
  }

  const auto parsedMod = parseModDirectory(*modRoot);
  if (!parsedMod.has_value()) {
    sweetLoaderLogger.error("Failed to parse mod manifest under {}",
                          modRoot->string());
    return false;
  }

  const fs::path expectedLibraryPath =
      (parsedMod->rootPath / parsedMod->entryPath).lexically_normal();

  if (!fs::exists(expectedLibraryPath) ||
      !fs::is_regular_file(expectedLibraryPath)) {
    sweetLoaderLogger.error("Resolved mod library {} does not exist",
                          expectedLibraryPath.string());
    return false;
  }

  storage.modId = parsedMod->id;
  storage.displayName = parsedMod->displayName;
  storage.author = parsedMod->author;
  storage.version = parsedMod->version;
  storage.entryPath = parsedMod->entryPath;
  storage.entryFileName = parsedMod->entryFileName;
  storage.libraryPath = expectedLibraryPath.string();
  storage.iconPath = parsedMod->iconPath;
  storage.manifestPath = parsedMod->manifestPath.string();
  storage.modRootPath = parsedMod->rootPath.string();
  storage.dependencies = parsedMod->dependencies;
  storage.gameVersionRange = parsedMod->gameVersionRange;
  finalizeRuntimeModInfo(storage);
  return true;
}

pl::mod::ModInfo toCppModInfo(const RuntimeModInfo &storage) {
  return pl::mod::ModInfo{
      .id = storage.modId,
      .displayName = storage.displayName,
      .author = storage.author,
      .version = storage.version,
      .entryPath = storage.entryPath,
      .entryFileName = storage.entryFileName,
      .libraryPath = storage.libraryPath,
      .iconPath = storage.iconPath,
      .manifestPath = storage.manifestPath,
      .modRootPath = storage.modRootPath,
  };
}

} // namespace pl::internal::mod
