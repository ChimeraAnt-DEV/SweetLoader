#include <jni.h>

#include "pl/render/RenderDispatch.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pl/runtime/GameHooks.h"
#include "pl/runtime/GameLocalPlayer.h"
#include "pl/runtime/GamePlayerRender.h"
#include "pl/runtime/GameResourcePackReload.h"
#include "pl/memory/Signature.hpp"
#include "pl/runtime/InputBridge.h"
#include "pl/runtime/LogRing.hpp"
#include "pl/runtime/SafeJni.hpp"
#include "pl/Logger.hpp"
#include "pl/runtime/JavaRuntime.h"

namespace {

struct DecodedText {
  std::string utf8;
  std::vector<unsigned int> codePoints;
};

void AppendUtf8(std::string &result, std::uint32_t codePoint) {
  if (codePoint <= 0x7fU) {
    result.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7ffU) {
    result.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
    result.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
  } else if (codePoint <= 0xffffU) {
    result.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
    result.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
    result.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
  } else {
    result.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
    result.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
    result.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
    result.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
  }
}

DecodedText DecodeText(JNIEnv *env, jstring value) {
  DecodedText result;
  if (value == nullptr) {
    return result;
  }

  const jsize length = env->GetStringLength(value);
  const jchar *chars = env->GetStringChars(value, nullptr);
  if (chars == nullptr) {
    return result;
  }

  result.utf8.reserve(static_cast<std::size_t>(length) * 3U);
  result.codePoints.reserve(static_cast<std::size_t>(length));
  for (jsize i = 0; i < length; ++i) {
    std::uint32_t codePoint = chars[i];
    if (codePoint >= 0xd800U && codePoint <= 0xdbffU && i + 1 < length) {
      const std::uint32_t low = chars[i + 1];
      if (low >= 0xdc00U && low <= 0xdfffU) {
        codePoint = 0x10000U + ((codePoint - 0xd800U) << 10U) + (low - 0xdc00U);
        ++i;
      } else {
        codePoint = 0xfffdU;
      }
    } else if (codePoint >= 0xd800U && codePoint <= 0xdfffU) {
      codePoint = 0xfffdU;
    }

    AppendUtf8(result.utf8, codePoint);
    result.codePoints.push_back(codePoint);
  }

  env->ReleaseStringChars(value, chars);
  return result;
}

std::string ToStdString(JNIEnv *env, jstring value) {
  if (!value) {
    return {};
  }

  const char *chars = env->GetStringUTFChars(value, nullptr);
  if (!chars) {
    return {};
  }

  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeOnTouch(
    JNIEnv *env, jclass clazz, jint action, jint pointerId, jfloat x,
    jfloat y) {
  (void)env;
  (void)clazz;

  const bool consumed = pl::runtime::DispatchTouch(action, pointerId, x, y);
  return consumed ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeOnKeyEvent(
    JNIEnv *env, jclass clazz, jint keyCode, jint unicodeChar,
    jboolean isKeyDown) {
  (void)env;
  (void)clazz;

  const bool consumed = pl::runtime::DispatchKeyEvent(
      static_cast<int>(keyCode), static_cast<unsigned int>(unicodeChar),
      isKeyDown == JNI_TRUE);
  return consumed ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeOnTextInput(
    JNIEnv *env, jclass clazz, jstring text) {
  (void)clazz;

  auto decoded = DecodeText(env, text);
  bool consumed = pl::runtime::DispatchTextInput(decoded.utf8);
  if (!consumed) {
    // The synthetic key fallback is defined for printable ASCII only. A committed control byte
    // (Enter arrives as '\n' from several IMEs) must not be replayed as a "character" key --
    // that is the input that took the instance down when Enter was pressed in chat. The
    // dedicated key code still delivers the Enter press through DispatchKeyEvent above.
    for (const unsigned int codePoint : decoded.codePoints) {
      if (codePoint < 0x20U || codePoint > 0x7eU) {
        continue;
      }
      consumed |= pl::runtime::DispatchKeyEvent(0, codePoint, true);
    }
  }
  return consumed ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeOnMouse(
    JNIEnv *env, jclass clazz, jint button, jboolean isDown) {
  (void)env;
  (void)clazz;

  const bool consumed =
      pl::runtime::DispatchMouse(static_cast<int>(button), isDown == JNI_TRUE);
  return consumed ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeOnDocumentResult(
    JNIEnv *env, jclass clazz, jboolean success, jstring path,
    jstring displayName, jstring error) {
  (void)clazz;
  pl::runtime::DispatchDocumentResult({
      .success = success == JNI_TRUE,
      .path = ToStdString(env, path),
      .displayName = ToStdString(env, displayName),
      .error = ToStdString(env, error),
  });
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSetActivity(
    JNIEnv *env, jclass clazz, jobject activity) {
  (void)clazz;

  pl::runtime::SetActivity(env, activity);
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeClearActivity(
    JNIEnv *env, jclass clazz) {
  (void)clazz;

  pl::runtime::ClearDocumentRequest();
  pl::runtime::ClearActivity(env);
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeIsPauseMenuOpen(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return pl::runtime::IsPauseMenuOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeIsHudScreenOpen(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return pl::runtime::IsHudScreenOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeIsShowingMenu(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return pl::runtime::IsShowingMenu() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeShouldForceGlobalModMenu(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return pl::runtime::ShouldForceGlobalModMenu() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeConfigureSignatureRules(
    JNIEnv *env, jclass clazz, jstring rulesPath, jstring minecraftVersion) {
  (void)clazz;

  pl::runtime::ConfigureGameHooks(ToStdString(env, rulesPath),
                                  ToStdString(env, minecraftVersion));
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSetSignatureCacheDir(JNIEnv *env,
                                                                             jclass clazz,
                                                                             jstring directory) {
  (void)clazz;
  pl::memory::SetSignatureCacheDirectory(ToStdString(env, directory));
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeIsLocalPlayerAvailable(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return pl::runtime::IsLocalPlayerAvailable() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReloadResourcePacks(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return pl::runtime::ReloadResourcePacks() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeIsPlayerRenderHookLive(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return pl::runtime::IsPlayerRenderHookLive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jintArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadPlayerRenderStats(
    JNIEnv *env, jclass clazz) {
  (void)clazz;
  std::uint32_t stats[4] = {0, 0, 0, 0};
  if (!pl::runtime::ReadPlayerRenderStats(stats)) {
    return nullptr;
  }
  jintArray result = env->NewIntArray(4);
  if (result != nullptr) {
    jint values[4] = {static_cast<jint>(stats[0]), static_cast<jint>(stats[1]),
                      static_cast<jint>(stats[2]), static_cast<jint>(stats[3])};
    env->SetIntArrayRegion(result, 0, 4, values);
  }
  return result;
}

JNIEXPORT jintArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadRenderCallbackStats(
    JNIEnv *env, jclass clazz) {
  (void)clazz;
  std::uint32_t stats[4] = {0, 0, 0, 0};
  pl::render::detail::ReadRenderCallbackStats(stats);
  jintArray result = env->NewIntArray(4);
  if (result != nullptr) {
    jint values[4] = {static_cast<jint>(stats[0]), static_cast<jint>(stats[1]),
                      static_cast<jint>(stats[2]), static_cast<jint>(stats[3])};
    env->SetIntArrayRegion(result, 0, 4, values);
  }
  return result;
}

JNIEXPORT jobjectArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeGetRecentLogs(
    JNIEnv *env, jclass clazz, jint count) {
  (void)clazz;
  const std::size_t wanted = count <= 0 ? 0 : static_cast<std::size_t>(count);
  const std::vector<std::string> lines = pl::runtime::SharedLogRing().snapshot(wanted);
  jclass stringClass = env->FindClass("java/lang/String");
  if (stringClass == nullptr) {
    env->ExceptionClear();
    return nullptr;
  }
  jobjectArray result = env->NewObjectArray(static_cast<jsize>(lines.size()), stringClass, nullptr);
  if (result == nullptr) return nullptr;
  for (jsize i = 0; i < static_cast<jsize>(lines.size()); ++i) {
    jstring line = pl::jni::NewString(env, lines[static_cast<std::size_t>(i)]);
    if (line == nullptr) continue;
    env->SetObjectArrayElement(result, i, line);
    env->DeleteLocalRef(line);
  }
  return result;
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeSetLogLevel(JNIEnv *env, jclass clazz,
                                                                     jint level) {
  (void)env;
  (void)clazz;
  // Clamp to the Android log levels, so a caller cannot silence errors or enable a level that does
  // not exist. WARN (5) is the quietest level that still reports a real problem.
  int clamped = level;
  if (clamped < 2) clamped = 2;   // ANDROID_LOG_VERBOSE
  if (clamped > 6) clamped = 6;   // ANDROID_LOG_ERROR
  pl::log::Logger::setLevel(clamped);
}

JNIEXPORT jfloatArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadLocalPlayerPosition(
    JNIEnv *env, jclass clazz) {
  (void)clazz;
  float position[3] = {0.0f, 0.0f, 0.0f};
  if (!pl::runtime::ReadLocalPlayerPosition(position)) {
    return nullptr;
  }
  jfloatArray result = env->NewFloatArray(3);
  if (result != nullptr) {
    env->SetFloatArrayRegion(result, 0, 3, position);
  }
  return result;
}

JNIEXPORT jfloatArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadLocalPlayerRotation(
    JNIEnv *env, jclass clazz) {
  (void)clazz;
  float rotation[2] = {0.0f, 0.0f};
  if (!pl::runtime::ReadLocalPlayerRotation(rotation)) {
    return nullptr;
  }
  jfloatArray result = env->NewFloatArray(2);
  if (result != nullptr) {
    env->SetFloatArrayRegion(result, 0, 2, rotation);
  }
  return result;
}

} // extern "C"
