#pragma once

/**
 * @file Export.hpp
 * @brief Symbol visibility helpers for the sweetloader SDK.
 */

#if defined(__GNUC__) || defined(__clang__)
#define PL_EXPORT [[maybe_unused]] __attribute__((visibility("default")))
#else
#define PL_EXPORT [[maybe_unused]]
#endif

