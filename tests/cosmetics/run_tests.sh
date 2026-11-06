#!/usr/bin/env bash
# Host test runner for the native cosmetics modules.
#
# The geometry parser, animation solver and socket protocol are pure C++ with no Android types, so
# they are verified on the build host rather than the device. This is the same reason the Java
# preview pipeline is unit-tested: a parse or interpolation defect is caught without an emulator.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
nlohmann="${NLOHMANN_JSON_INCLUDE:-}"

if [[ -z "$nlohmann" ]]; then
  # Fall back to the copy Gradle already fetched, if present.
  nlohmann="$(find "$root/../../../.cxx" -path '*nlohmann_json-src/single_include' -type d 2>/dev/null | head -1 || true)"
fi
if [[ -z "$nlohmann" || ! -f "$nlohmann/nlohmann/json.hpp" ]]; then
  # Fetch the single-header release into a gitignored cache so the host tests do not depend on a
  # previous native Gradle build. Header-only and pinned by tag, so the run is reproducible.
  nlohmann="$root/../../../../build/host-tools/nlohmann/include"
  if [[ ! -f "$nlohmann/nlohmann/json.hpp" ]]; then
    mkdir -p "$nlohmann/nlohmann"
    echo "Fetching nlohmann/json single header..." >&2
    if ! curl -fsSL -o "$nlohmann/nlohmann/json.hpp" \
        "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp"; then
      echo "nlohmann/json not found; set NLOHMANN_JSON_INCLUDE to its include dir" >&2
      exit 1
    fi
  fi
fi
if [[ -z "$nlohmann" || ! -f "$nlohmann/nlohmann/json.hpp" ]]; then
  echo "nlohmann/json not found; set NLOHMANN_JSON_INCLUDE to its include dir" >&2
  exit 1
fi

cxx="${CXX:-g++}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# zlib headers. AntEggLoader reads egg.json straight out of the ZIP central directory with zlib's
# raw inflate, so its host test needs the header and the library.
#
# The NDK sysroot's include dir must NOT go on the include path: it carries its own libc headers,
# and pulling those in ahead of the host's breaks every other header (os_defines.h and friends
# fail on undefined glibc feature macros). So the zlib headers are copied into a private include
# dir of their own, which adds zlib.h without shadowing anything else.
zlib_include=""
zlib_source=""
zlib_link=""
for candidate in \
    /usr/include \
    "${ZLIB_INCLUDE:-}" \
    "$(ls -d /opt/android-sdk/ndk/*/toolchains/llvm/prebuilt/*/sysroot/usr/include 2>/dev/null | head -1)" \
    "$(ls -d /workspace/android-sdk/ndk/*/toolchains/llvm/prebuilt/*/sysroot/usr/include 2>/dev/null | head -1)" \
    "$root/../libHttpClient/External/zlib"; do
  if [[ -n "$candidate" && -f "$candidate/zlib.h" ]]; then
    zlib_source="$candidate"
    if [[ "$candidate" == "/usr/include" ]]; then
      zlib_include="$candidate"
    else
      zlib_include="$work/zlib-include"
      mkdir -p "$zlib_include"
      cp "$candidate/zlib.h" "$zlib_include/"
      [[ -f "$candidate/zconf.h" ]] && cp "$candidate/zconf.h" "$zlib_include/"
    fi
    break
  fi
done

# Resolve the library to link. A distro often ships only the versioned soname (libz.so.1) with no
# libz.so for `-lz`, so link the versioned file directly when that is all there is; the bundled
# libHttpClient copy is source-only, so compile it in.
if [[ "$zlib_source" == *libHttpClient* ]]; then
  zlib_link="$zlib_source/zlib.c"
else
  zlib_link="-lz"
  for lib in /usr/lib/*/libz.so /usr/lib/*/libz.a /usr/lib/*/libz.so.*; do
    if [[ -e "$lib" ]]; then
      zlib_link="$lib"
      break
    fi
  done
fi

compile_run() {
  local name="$1"; shift
  echo "== $name =="
  "$cxx" -std=c++20 -O1 -g \
    -I"$root/include" -I"$root/src" -I"$nlohmann" ${zlib_include:+-I"$zlib_include"} \
    "$@" -o "$work/$name"
  "$work/$name"
}

compile_run BedrockModelParserTest \
  "$root/src/pl/cosmetics/geometry/BedrockModelParser.cpp" \
  "$here/BedrockModelParserTest.cpp"

compile_run NativeModelMatrixTest \
  "$root/src/pl/cosmetics/NativeModelMatrix.cpp" \
  "$here/NativeModelMatrixTest.cpp"

compile_run AnimationSolverTest \
  "$root/src/pl/cosmetics/animation/AnimationSolver.cpp" \
  "$here/AnimationSolverTest.cpp"

compile_run CosmeticSocketProtocolTest \
  "$root/src/pl/cosmetics/network/CosmeticSocketProtocol.cpp" \
  "$here/CosmeticSocketProtocolTest.cpp"

compile_run NativeCosmeticDataTest \
  "$root/src/pl/cosmetics/NativeCosmeticData.cpp" \
  "$root/src/pl/cosmetics/NativeModelMatrix.cpp" \
  "$root/src/pl/cosmetics/geometry/BedrockModelParser.cpp" \
  "$here/NativeCosmeticDataTest.cpp"

compile_run VersionCompareTest \
  "$here/VersionCompareTest.cpp"

# The input bridge's callback snapshot is a pure header, so it needs no source file of its own.
compile_run CallbackSnapshotTest \
  "$here/CallbackSnapshotTest.cpp"

compile_run OnceReportTest \
  "$here/OnceReportTest.cpp"

compile_run GpuFamilyTest \
  "$here/GpuFamilyTest.cpp"

# The image-override stores are a pure header too.
compile_run ImageOverrideRegistryTest \
  "$here/ImageOverrideRegistryTest.cpp"

# The engine allocator is a pure header as well.
compile_run EngineAllocatorTest \
  "$here/EngineAllocatorTest.cpp"

# The signature pattern parser and anchor selection are pure: it decides which bytes a scan searches
# for, so it is exercised here with wildcards at either end, a fully wild pattern and malformed input.
compile_run SignaturePatternTest \
  "$here/SignaturePatternTest.cpp"

# The maps-line parser and basename match are pure too.
compile_run ModulePathMatchTest \
  "$here/ModulePathMatchTest.cpp" \
  "$root/src/pl/memory/ModulePathMatch.cpp"

# The ELF build-id reader is pure as well.
compile_run ElfBuildIdTest \
  "$here/ElfBuildIdTest.cpp" \
  "$root/src/pl/memory/ElfBuildId.cpp"

# The on-disk scan cache is a pure serialiser: it stores offsets from the load
# base, keyed by the ELF build id, so a repeat launch skips the scan and a game update cannot read
# the previous build's offsets.
compile_run SignatureScanCacheTest \
  "$root/src/pl/memory/SignatureScanCache.cpp" \
  "$here/SignatureScanCacheTest.cpp"

# The native-mod manifest validation is a pure header: it decides whether a manifest is
# well formed and names the field that failed, so an author has something to fix.
compile_run ModManifestValidationTest \
  -I"$root/src" \
  "$here/ModManifestValidationTest.cpp"

# The dependency-ordered load rule is a pure header too: determinism and cycle detection
# decide which mod's hook runs first and which is torn down first.
compile_run ModLoadOrderTest \
  -I"$root/src" \
  "$here/ModLoadOrderTest.cpp"

# The exported C ABI is compile-time-pinned, so a rename fails the build.
compile_run LegacyAbiTest \
  "$here/LegacyAbiTest.cpp"

compile_run Utf8Test \
  "$here/Utf8Test.cpp"

compile_run HookInstallGateTest \
  "$here/HookInstallGateTest.cpp"

compile_run JniAttachTest \
  "$here/JniAttachTest.cpp"

compile_run SnapshotChannelTest \
  "$here/SnapshotChannelTest.cpp" -pthread

compile_run GameHookRuleMergeTest \
  "$here/GameHookRuleMergeTest.cpp"

compile_run RuleProbeTest \
  "$here/RuleProbeTest.cpp"

compile_run FramePacingTest \
  "$here/FramePacingTest.cpp"

# The frame-boundary clock that replaced the "gap beyond one second" rule, which froze the render tick
# during play and starved cape/pet physics. Pure header.
compile_run FrameTickClockTest \
  "$here/FrameTickClockTest.cpp"

compile_run ProcessMemoryTest \
  -I"$root/src" \
  "$root/src/pl/runtime/ProcessMemory.cpp" \
  "$here/ProcessMemoryTest.cpp"

compile_run PrologueVerifyTest \
  "$here/PrologueVerifyTest.cpp"

compile_run OptifineConfigBoundsTest \
  -I"$root/src" \
  "$root/src/pl/runtime/OptifineConfig.cpp" \
  "$here/OptifineConfigBoundsTest.cpp"

# The dynamic-render-distance hysteresis rule. The governor is a header + one
# translation unit with no Android dependency, so the step, dwell and change-count rules are
# exercised on the host.
compile_run RenderDistanceGovernorTest \
  "$root/src/pl/runtime/optifine/OptifineGovernor.cpp" \
  "$here/RenderDistanceGovernorTest.cpp"

compile_run SkinSeamObservationTest \
  "$here/SkinSeamObservationTest.cpp"

# The BerryAPI world-seam provider is a pure header, so it needs no source file of its own.
compile_run BerrySeamProviderTest \
  "$here/BerrySeamProviderTest.cpp"

# The entity-list walker is a pure header too: the container read, the bounds checks and the
# fail-closed rules are exercised on the host, so a hand-recovered offset is verified before it is
# ever used on the game thread.
compile_run EntityListWalkTest \
  "$here/EntityListWalkTest.cpp"

# The BerryAPI replay ABI mirror is a pure header too: the mirror check and the usable-call rule
# are host-tested without a device.
compile_run BerryReplayMirrorTest \
  "$here/BerryReplayMirrorTest.cpp"

# The BerryAPI governor ABI mirror, same shape.
compile_run BerryGovernorMirrorTest \
  "$here/BerryGovernorMirrorTest.cpp"

compile_run ModuleMapTest \
  "$root/src/pl/runtime/ModuleMap.cpp" \
  "$here/ModuleMapTest.cpp"

# The per-mod fault-isolation rule. Pure header, no source file of its own.
compile_run ModFaultPolicyTest \
  -I"$root/src" \
  "$here/ModFaultPolicyTest.cpp"

# The debug-only hook time budget. Pure header.
compile_run HookTimingTest \
  "$here/HookTimingTest.cpp"

# Per-feature hook availability, rendered as one JSON report.
compile_run FeatureAvailabilityTest \
  -I"$root/src" \
  "$root/src/pl/runtime/FeatureAvailability.cpp" \
  "$here/FeatureAvailabilityTest.cpp"

# The hook self-test report. Pure header; the runner itself needs a live backend.
compile_run HookSelfTestTest \
  "$here/HookSelfTestTest.cpp"

# The native egg.json parser. Needs zlib (the loader reads the manifest straight out of the ZIP
# central directory); `zlib_link` above resolves it for the host.
compile_run AntEggManifestTest \
  -I"$root/src" \
  "$root/src/pl/runtime/AntEggLoader.cpp" \
  "$here/AntEggManifestTest.cpp" "$zlib_link"

# Fuzz-shaped invariants for the three parsers that read untrusted data (item 458): the signature
# parser, the egg.json reader and the Bedrock geometry parser. Deterministic and seeded, so a
# failure reproduces from the seed alone.
compile_run FuzzParseTest \
  -I"$root/src" \
  "$root/src/pl/runtime/AntEggLoader.cpp" \
  "$root/src/pl/cosmetics/geometry/BedrockModelParser.cpp" \
  "$here/FuzzParseTest.cpp" "$zlib_link"

# The seqlock's memory model, under ThreadSanitizer. The plain run above proves the payload is never
# torn; this run proves the reader and the writer never touch the buffer without an ordering edge,
# which is the half a torn-read check cannot see and the half that bites on ARM64. TSAN needs ASLR
# disabled on this kernel, so it runs through `setarch -R` and is skipped (with a notice) where the
# sanitizer or `setarch` is unavailable, so the suite still passes on a plain host.
tsan_available=""
if "$cxx" -std=c++20 -fsanitize=thread -x c++ - -o "$work/tsan-probe" >/dev/null 2>&1 <<<'int main(){return 0;}'; then
  if command -v setarch >/dev/null 2>&1 &&
     setarch "$(uname -m)" -R "$work/tsan-probe" >/dev/null 2>&1; then
    tsan_available=1
  fi
fi
if [[ -n "$tsan_available" ]]; then
  echo "== SnapshotChannelTsanTest =="
  # -Wno-tsan: the fences are a no-op under the sanitizer's own model, so the "not supported with
  # -fsanitize=thread" warning is expected and would otherwise hide a real warning in the log.
  "$cxx" -std=c++20 -O1 -g -fsanitize=thread -Wno-tsan \
    -I"$root/include" -I"$root/src" \
    "$here/SnapshotChannelTsanTest.cpp" -o "$work/SnapshotChannelTsanTest"
  setarch "$(uname -m)" -R "$work/SnapshotChannelTsanTest"
else
  echo "== SnapshotChannelTsanTest == (skipped: ThreadSanitizer/setarch unavailable)"
fi

# The local-player seqlock, extracted from GameLocalPlayer.cpp into one shared class so the ordering
# rules are pinned once. The plain run proves the transform is never torn and that a read landing in
# a write window retries rather than reporting "no player"; the TSAN run proves the reader and the
# writer share no unsynchronised access, which is the half the old plain-float fields failed.
compile_run PlayerSnapshotTest \
  "$here/PlayerSnapshotTest.cpp"

if [[ -n "$tsan_available" ]]; then
  echo "== PlayerSnapshotTsanTest =="
  "$cxx" -std=c++20 -O1 -g -fsanitize=thread -Wno-tsan \
    -I"$root/include" -I"$root/src" \
    "$here/PlayerSnapshotTsanTest.cpp" -o "$work/PlayerSnapshotTsanTest"
  setarch "$(uname -m)" -R "$work/PlayerSnapshotTsanTest"
else
  echo "== PlayerSnapshotTsanTest == (skipped: ThreadSanitizer/setarch unavailable)"
fi

echo "all cosmetics host tests passed"
