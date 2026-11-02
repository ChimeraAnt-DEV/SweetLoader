#include "pl/runtime/AntEggLoader.h"
#include "pl/runtime/JavaRuntime.h"
#include "pl/runtime/SafeJni.hpp"

#include <jni.h>

#include <string>

namespace {

std::string ToStdString(JNIEnv* env, jstring value) {
  if (!value) return {};
  const char* chars = env->GetStringUTFChars(value, nullptr);
  if (!chars) return {};
  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

jstring NewErrorMessage(JNIEnv* env, const std::string& error) {
  // An empty string means success; the Java side checks isEmpty() rather than null so the
  // common success path never allocates a message.
  return pl::jni::NewString(env, error);
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_core_antegg_AntEggBridge_nativeValidate(JNIEnv* env, jclass,
                                                                  jstring file_path) {
  pl::runtime::AntEggManifest manifest;
  std::string error;
  if (!pl::runtime::AntEggLoader::ReadManifest(ToStdString(env, file_path), manifest, error)) {
    return NewErrorMessage(env, error);
  }
  return NewErrorMessage(env, "");
}

JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_core_antegg_AntEggBridge_nativeLoadMod(JNIEnv* env, jclass,
                                                                 jstring file_path,
                                                                 jstring sandbox_root,
                                                                 jstring allowed_root) {
  std::string error;
  // Item 454: the destination is checked here, before anything is written, against the root the
  // caller owns. The argument arrives from Java, but this is the code that writes files, so it
  // verifies rather than trusts: a sandbox root outside the app's private directory is refused.
  if (!pl::runtime::AntEggLoader::ValidateSandboxRoot(ToStdString(env, sandbox_root),
                                                      ToStdString(env, allowed_root), error)) {
    return NewErrorMessage(env, error);
  }
  if (!pl::runtime::AntEggLoader::LoadMod(ToStdString(env, file_path),
                                          ToStdString(env, sandbox_root), error)) {
    return NewErrorMessage(env, error);
  }
  return NewErrorMessage(env, "");
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_core_antegg_AntEggBridge_nativeLooksLikeAntEgg(JNIEnv* env, jclass,
                                                                        jstring file_name) {
  return pl::runtime::AntEggLoader::LooksLikeAntEgg(ToStdString(env, file_name)) ? JNI_TRUE
                                                                                 : JNI_FALSE;
}

}  // extern "C"
