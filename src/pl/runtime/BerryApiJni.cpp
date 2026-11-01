/**
 * @file BerryApiJni.cpp
 * @brief JNI surface for the BerryAPI bridge (see BerryApiBridge.h).
 *
 * <p>Every entry is a thin copy of a cached reading; nothing here blocks or allocates beyond one
 * short string for the version and summary. The launcher calls these from the Diagnostics screen and
 * once at launch, not from a per-frame path.
 */

#include <jni.h>

#include <string>

#include "pl/runtime/BerryApiBridge.h"

namespace {

jstring NewJString(JNIEnv *env, const std::string &value) {
  return env->NewStringUTF(value.c_str());
}

} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeEnsureLoaded(JNIEnv *, jclass) {
  return pl::berry::EnsureLoaded() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeIsAvailable(JNIEnv *, jclass) {
  return pl::berry::Available() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeIsAttached(JNIEnv *, jclass) {
  return pl::berry::Attached() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeEnsureAttached(JNIEnv *, jclass) {
  return pl::berry::EnsureAttached() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeVersion(JNIEnv *env, jclass) {
  return NewJString(env, pl::berry::Version());
}

/** The capability mask as a plain int, so the Java side maps bits without a shared enum. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCapabilities(JNIEnv *, jclass) {
  return static_cast<jint>(static_cast<std::uint32_t>(pl::berry::Capabilities()));
}

JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeSummary(JNIEnv *env, jclass) {
  return NewJString(env, pl::berry::Summary());
}

} // extern "C"
