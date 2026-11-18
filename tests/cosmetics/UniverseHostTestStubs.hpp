#pragma once

// Shared host-test stand-ins for the engine facilities `pl/engine/UniverseHostTable.cpp` (and, through
// it, `pl/engine/UniverseBridge.cpp`) link against. The real facilities read and write the game's
// memory, resolve signatures in `libminecraftpe.so` and read the local player — none of which exist
// on the build host. The definitions live in UniverseHostTestStubs.cpp, which every host test that
// links the host table compiles in, so the stand-ins are shared rather than copied per test.
//
// The flags below let a test drive the stubs: choose whether a signature resolves, whether a memory
// range reads, whether the local player is available, and read back what the table asked for.

#include <cstdint>
#include <string>

namespace pl::universe_test {

// Stub controls, set by a test before it exercises the table.
extern bool signatureResolves;
extern bool readSucceeds;
extern bool writeSucceeds;
extern bool playerAvailable;
extern int hookInstalls;

// What the table passed through to the stubs, read by a test afterwards.
extern std::string signatureSeen;
extern std::string moduleSeen;
extern std::uint64_t frameTick;

} // namespace pl::universe_test
