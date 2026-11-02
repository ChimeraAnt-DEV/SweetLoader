#pragma once

/**
 * @file GpuFamily.hpp
 * @brief Classifies the device's GPU from the GL vendor/renderer strings, and picks safe defaults.
 *
 * The diagnostics report names the backend (GLES or Vulkan), but a backend name is not enough to
 * choose safe defaults: an Adreno, a Mali and a software rasteriser all report `GLES`, and they
 * differ in the texture-size limits and precision quirks a cosmetic path cares about. The vendor
 * and renderer strings are the only thing a driver exposes that distinguishes them, so this module
 * turns those two strings into a family and a small set of conservative defaults.
 *
 * Pure C++ (no Android, no GLES header), so the classification and the defaults are host-testable:
 * a driver string is untrusted input and a wrong classification is invisible until it changes what
 * the launcher decides to draw.
 */

#include <cctype>
#include <cstdint>
#include <string_view>

namespace pl::render {

/** @brief The GPU vendor family a driver string names, when it can be told apart. */
enum class GpuFamily : int {
  Unknown = 0,  ///< no vendor/renderer string, or one we cannot place
  Adreno = 1,   ///< Qualcomm Adreno
  Mali = 2,     ///< Arm Mali
  Immortalis = 3, ///< Arm Immortalis (Mali's higher tier, reported separately)
  PowerVr = 4,  ///< Imagination PowerVR
  Xclipse = 5,  ///< Samsung Xclipse (AMD RDNA-based)
  Nvidia = 6,   ///< NVIDIA (Tegra and desktop)
  Apple = 7,    ///< Apple GPU (a macOS/CI or emulated host)
  Software = 8, ///< SwiftShader / llvmpipe / a software rasteriser
  Generic = 9,  ///< a real GPU we cannot name
};

/**
 * @brief The conservative defaults that follow from a family.
 *
 * These are *floor* values, not tuning: they say what the launcher may assume without measuring.
 * `maxTextureEdge` is the smallest 2D texture size the family is guaranteed to accept, so a caller
 * that downsamples to fit never asks for more than the driver will give. `software` marks a
 * rasteriser, where a caller should prefer the cheapest path.
 */
struct GpuDefaults {
  /** The largest texture edge, in texels, that is safe to request without querying the driver. */
  std::uint32_t maxTextureEdge = 4096;
  /** True for a software rasteriser (SwiftShader / llvmpipe), where every draw is expensive. */
  bool software = false;
  /** True when the family is known well enough to trust `maxTextureEdge` as more than a floor. */
  bool known = false;
};

namespace detail {

/** Case-insensitive substring test over ASCII, so a driver string is never trusted for its case. */
inline bool containsAsciiInsensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty() || haystack.size() < needle.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      const auto a = static_cast<unsigned char>(haystack[i + j]);
      const auto b = static_cast<unsigned char>(needle[j]);
      if (std::tolower(a) != std::tolower(b)) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

} // namespace detail

/**
 * @brief Classifies a GPU from the GL vendor and renderer strings.
 *
 * The vendor string is checked first because it is the more reliable of the two (some drivers
 * report a generic renderer but always a correct vendor), then the renderer for the cases where a
 * vendor reports its own name only there. An empty pair reads as {@link GpuFamily::Unknown} rather
 * than {@link GpuFamily::Generic}: "we did not look" is not the same as "a real GPU we cannot name".
 *
 * The software check runs first: a software rasteriser reports a plausible vendor
 * (`Google`/`Mesa`) but must never be treated as hardware.
 */
inline GpuFamily ClassifyGpuFamily(std::string_view vendor, std::string_view renderer) {
  const bool anyText = !vendor.empty() || !renderer.empty();
  if (!anyText) return GpuFamily::Unknown;

  // Software rasterisers first: SwiftShader is the Android emulator's GLES, llvmpipe is Mesa's.
  if (detail::containsAsciiInsensitive(renderer, "swiftshader") ||
      detail::containsAsciiInsensitive(vendor, "swiftshader") ||
      detail::containsAsciiInsensitive(renderer, "llvmpipe") ||
      detail::containsAsciiInsensitive(renderer, "softpipe") ||
      detail::containsAsciiInsensitive(renderer, "software rasterizer")) {
    return GpuFamily::Software;
  }

  // Immortalis before Mali: its renderer string also contains "Mali", so the more specific match
  // must win or the tier is lost.
  if (detail::containsAsciiInsensitive(renderer, "immortalis")) return GpuFamily::Immortalis;
  if (detail::containsAsciiInsensitive(vendor, "adreno") ||
      detail::containsAsciiInsensitive(renderer, "adreno")) {
    return GpuFamily::Adreno;
  }
  if (detail::containsAsciiInsensitive(vendor, "arm") ||
      detail::containsAsciiInsensitive(renderer, "mali")) {
    return GpuFamily::Mali;
  }
  if (detail::containsAsciiInsensitive(vendor, "imagination") ||
      detail::containsAsciiInsensitive(renderer, "powervr")) {
    return GpuFamily::PowerVr;
  }
  if (detail::containsAsciiInsensitive(renderer, "xclipse")) return GpuFamily::Xclipse;
  if (detail::containsAsciiInsensitive(vendor, "nvidia") ||
      detail::containsAsciiInsensitive(renderer, "nvidia") ||
      detail::containsAsciiInsensitive(renderer, "tegra")) {
    return GpuFamily::Nvidia;
  }
  if (detail::containsAsciiInsensitive(vendor, "apple") ||
      detail::containsAsciiInsensitive(renderer, "apple")) {
    return GpuFamily::Apple;
  }
  return GpuFamily::Generic;
}

/**
 * @brief The conservative defaults for a family.
 *
 * Every value is a *floor* that holds without querying the driver. The texture edge is 4096 for
 * hardware (the GLES 3.0 minimum is 2048 and every shipping part exceeds it; 4096 is a value no
 * supported device will refuse) and 2048 for a software rasteriser, which is the slowest to upload
 * and the most likely to be capped.
 */
inline GpuDefaults DefaultsFor(GpuFamily family) {
  GpuDefaults defaults;
  switch (family) {
    case GpuFamily::Software:
      defaults.maxTextureEdge = 2048;
      defaults.software = true;
      defaults.known = true;
      break;
    case GpuFamily::Unknown:
      defaults.known = false;
      break;
    default:
      defaults.known = true;
      break;
  }
  return defaults;
}

/** @brief A stable, lower-case name for a family, for a log line or a report field. */
inline const char *GpuFamilyName(GpuFamily family) {
  switch (family) {
    case GpuFamily::Adreno:
      return "adreno";
    case GpuFamily::Mali:
      return "mali";
    case GpuFamily::Immortalis:
      return "immortalis";
    case GpuFamily::PowerVr:
      return "powervr";
    case GpuFamily::Xclipse:
      return "xclipse";
    case GpuFamily::Nvidia:
      return "nvidia";
    case GpuFamily::Apple:
      return "apple";
    case GpuFamily::Software:
      return "software";
    case GpuFamily::Generic:
      return "generic";
    case GpuFamily::Unknown:
    default:
      return "unknown";
  }
}

} // namespace pl::render
