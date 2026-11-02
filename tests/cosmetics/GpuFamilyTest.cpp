/**
 * @file GpuFamilyTest.cpp
 * @brief Host test for the GPU-family classification and its conservative defaults.
 *
 * A driver string is untrusted input: it is whatever the vendor chose to write, in whatever case,
 * and a wrong classification silently changes what the launcher decides to draw. The rules that
 * matter are the specific-before-general ones (Immortalis also says "Mali"; a software rasteriser
 * also names a plausible vendor) and that "we did not look" is distinct from "unknown hardware".
 */

#include <cstdio>
#include <cstdlib>

#include "pl/render/GpuFamily.hpp"

using pl::render::ClassifyGpuFamily;
using pl::render::DefaultsFor;
using pl::render::GpuDefaults;
using pl::render::GpuFamily;
using pl::render::GpuFamilyName;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

} // namespace

int main() {
  // "We did not look" is not "a GPU we cannot name".
  expect(ClassifyGpuFamily("", "") == GpuFamily::Unknown, "an empty pair is unknown");
  expect(ClassifyGpuFamily("", "some gpu") == GpuFamily::Generic,
         "an unrecognised non-empty string is generic, not unknown");

  // Vendor-first classification, and case-insensitive matching.
  expect(ClassifyGpuFamily("Qualcomm", "Adreno (TM) 740") == GpuFamily::Adreno, "Adreno by vendor");
  expect(ClassifyGpuFamily("", "ADRENO 660") == GpuFamily::Adreno, "Adreno by renderer, upper-case");
  expect(ClassifyGpuFamily("ARM", "Mali-G78") == GpuFamily::Mali, "Mali by vendor");
  expect(ClassifyGpuFamily("", "mali-g57 mc2") == GpuFamily::Mali, "Mali by renderer, lower-case");

  // Immortalis is a Mali-tier part whose renderer string also contains "Mali"; the specific match
  // must win or the tier is lost.
  expect(ClassifyGpuFamily("ARM", "Immortalis-G715") == GpuFamily::Immortalis,
         "Immortalis wins over the Mali substring in its own renderer");
  expect(ClassifyGpuFamily("", "Mali-Immortalis") == GpuFamily::Immortalis,
         "Immortalis is matched from the renderer alone");

  expect(ClassifyGpuFamily("Imagination Technologies", "PowerVR Rogue GE8320") == GpuFamily::PowerVr,
         "PowerVR by vendor");
  expect(ClassifyGpuFamily("", "PowerVR Rogue") == GpuFamily::PowerVr, "PowerVR by renderer");
  expect(ClassifyGpuFamily("", "Xclipse 920") == GpuFamily::Xclipse, "Xclipse by renderer");
  expect(ClassifyGpuFamily("NVIDIA Corporation", "NVIDIA Tegra") == GpuFamily::Nvidia,
         "NVIDIA by vendor");
  expect(ClassifyGpuFamily("", "Tegra X1") == GpuFamily::Nvidia, "Tegra by renderer");
  expect(ClassifyGpuFamily("Apple", "Apple M2") == GpuFamily::Apple, "Apple by vendor");

  // Software wins over a plausible vendor: the emulator's GLES is SwiftShader and must never be
  // treated as hardware.
  expect(ClassifyGpuFamily("Google", "SwiftShader Device") == GpuFamily::Software,
         "SwiftShader is software even with a vendor string");
  expect(ClassifyGpuFamily("Mesa", "llvmpipe (LLVM 15.0.7, 128 bits)") == GpuFamily::Software,
         "llvmpipe is software");
  expect(ClassifyGpuFamily("", "Software Rasterizer") == GpuFamily::Software,
         "an explicit software rasteriser is software");

  // Defaults are conservative floors, and a software rasteriser is marked as such.
  const GpuDefaults hw = DefaultsFor(GpuFamily::Adreno);
  expect(hw.known && !hw.software && hw.maxTextureEdge == 4096, "hardware defaults are 4096, known");
  const GpuDefaults sw = DefaultsFor(GpuFamily::Software);
  expect(sw.known && sw.software && sw.maxTextureEdge == 2048,
         "software defaults cap the texture edge and flag software");
  const GpuDefaults unknown = DefaultsFor(GpuFamily::Unknown);
  expect(!unknown.known && !unknown.software && unknown.maxTextureEdge == 4096,
         "an unknown family keeps the safe floor but is not claimed as known");

  // Every family has a distinct, non-empty name (a report field must not collide).
  const GpuFamily all[] = {GpuFamily::Unknown, GpuFamily::Adreno,    GpuFamily::Mali,
                           GpuFamily::Immortalis, GpuFamily::PowerVr, GpuFamily::Xclipse,
                           GpuFamily::Nvidia,  GpuFamily::Apple,   GpuFamily::Software,
                           GpuFamily::Generic};
  for (std::size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
    const char *name = GpuFamilyName(all[i]);
    expect(name != nullptr && name[0] != '\0', "every family has a name");
    for (std::size_t j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j) {
      expect(!(name[0] == GpuFamilyName(all[j])[0] && name[1] == GpuFamilyName(all[j])[1] &&
               name[2] == GpuFamilyName(all[j])[2] && name[3] == GpuFamilyName(all[j])[3]),
             "family names are distinct");
    }
  }

  std::printf("GpuFamilyTest: %d checks passed\n", checks);
  return 0;
}
