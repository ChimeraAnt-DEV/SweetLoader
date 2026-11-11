// Host-test stand-in for the Android-only GlossHook header.
//
// VtableResolveTest compiles the real src/pl/memory/Vtable.cpp on the build host, and that file
// includes the real pl/Gloss.h, which is `#error`-guarded to Android/arm and pulls in Android
// headers. This shim declares only the two symbols Vtable.cpp uses, so the resolver can be exercised
// off-device; the test supplies their definitions. It is placed first on the include path for that
// one compile (see run_tests.sh), so it never shadows the real header for anything else.
#pragma once

#include <cstddef>
#include <cstdint>

#define GLOSS_API

extern "C" GLOSS_API uintptr_t GlossGetLibSection(const char *lib_name,
                                                  const char *sec_name,
                                                  size_t *sec_size);
extern "C" GLOSS_API void GlossInit(bool is_init_linker);
