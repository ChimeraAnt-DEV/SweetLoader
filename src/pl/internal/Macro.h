#pragma once

#ifdef __cplusplus
#define SWEETLOADER_MAYBE_UNUSED [[maybe_unused]]
#else
#define SWEETLOADER_MAYBE_UNUSED
#endif

#ifdef SWEETLOADER_EXPORT
#define PLAPI [[maybe_unused]] __attribute__((visibility("default")))
#else
#define PLAPI [[maybe_unused]]
#endif

#ifdef __cplusplus
#define PLCAPI extern "C" PLAPI
#else
#define PLCAPI extern PLAPI
#endif