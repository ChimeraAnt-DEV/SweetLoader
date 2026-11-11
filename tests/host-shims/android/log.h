// Host-test stand-in for <android/log.h>.
//
// Logger.hpp includes it and calls __android_log_print; on the build host there is no such header.
// This provides the level constants Logger.hpp uses and the print function's declaration; the test
// supplies a no-op definition. Placed first on the include path for the one host test that links
// Logger.hpp (see run_tests.sh).
#pragma once

#define ANDROID_LOG_UNKNOWN 0
#define ANDROID_LOG_DEFAULT 1
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_FATAL 7
#define ANDROID_LOG_SILENT 8

extern "C" int __android_log_print(int prio, const char *tag, const char *fmt, ...);
