// Definitions for the host-test facility stand-ins declared in UniverseHostTestStubs.hpp.
//
// Compiled into every host test that links the `.universe` host table, so the table's calls resolve
// on the build host (where the real facilities — signature scans in libminecraftpe.so, process
// memory access, the local player, BerryAPI — do not exist).

#include "UniverseHostTestStubs.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pl/memory/Hook.hpp"
#include "pl/memory/Patch.hpp"
#include "pl/memory/Signature.hpp"

namespace pl::universe_test {

bool signatureResolves = false;
bool readSucceeds = false;
bool writeSucceeds = false;
bool playerAvailable = false;
int hookInstalls = 0;
std::string signatureSeen;
std::string moduleSeen;
std::uint64_t frameTick = 0;

} // namespace pl::universe_test

namespace pl::memory {

std::uintptr_t resolveSignature(std::string_view signature, std::string_view module) {
  pl::universe_test::signatureSeen.assign(signature);
  pl::universe_test::moduleSeen.assign(module);
  return pl::universe_test::signatureResolves ? 0x1000u : 0u;
}

std::vector<std::uint8_t> readBytes(std::uintptr_t, std::size_t length) {
  if (!pl::universe_test::readSucceeds) return {};
  return std::vector<std::uint8_t>(length, 0xAB);
}

bool writeBytes(std::uintptr_t, std::span<const std::uint8_t>, std::string_view) {
  return pl::universe_test::writeSucceeds;
}

int hook(FuncPtr, FuncPtr, FuncPtr *originalFunc, HookPriority, std::string_view) {
  ++pl::universe_test::hookInstalls;
  if (originalFunc != nullptr) *originalFunc = reinterpret_cast<FuncPtr>(0x2000u);
  return 0;
}

bool unhook(FuncPtr, FuncPtr) { return true; }

} // namespace pl::memory

namespace pl::runtime {

bool ReadLocalPlayerPosition(float out[3]) {
  if (!pl::universe_test::playerAvailable) return false;
  out[0] = 1.0f;
  out[1] = 2.0f;
  out[2] = 3.0f;
  return true;
}

bool ReadLocalPlayerRotation(float out[2]) {
  if (!pl::universe_test::playerAvailable) return false;
  out[0] = 4.0f;
  out[1] = 5.0f;
  return true;
}

} // namespace pl::runtime

namespace pl::berry {

std::string Version() { return "9.9.9-test"; }

} // namespace pl::berry

namespace pl::render {

std::uint64_t RenderFrameTick() { return pl::universe_test::frameTick; }

} // namespace pl::render

// The logger the host table writes through compiles against the host stand-in for <android/log.h>
// (see tests/host-shims) and needs the print function defined; this one definition satisfies it.
extern "C" int __android_log_print(int, const char *, const char *, ...) { return 0; }
