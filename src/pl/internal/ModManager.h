#pragma once
#include <filesystem>
#include <jni.h>
#include <optional>
#include <string>

namespace ModManager {
[[gnu::visibility("hidden")]] bool
LoadModLibrary(const std::filesystem::path &libraryPath,
               const std::optional<std::filesystem::path> &sourceModDirectory,
               JavaVM *vm);
[[gnu::visibility("hidden")]] void EnableLoadedMods();
[[gnu::visibility("hidden")]] void DisableAndUnloadLoadedMods();

/**
 * @brief The reason the most recent {@link LoadModLibrary} call failed, or empty on success.
 *
 * <p>{@code nativeLoadMod} returns a boolean; this is the detail behind a {@code false}, so the
 * launcher can report why a mod did not load rather than that it did not (audit item 6.17).
 */
[[gnu::visibility("hidden")]] std::string LastLoadError();
} // namespace ModManager
