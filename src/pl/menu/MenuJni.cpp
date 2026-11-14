/**
 * @file MenuJni.cpp
 * @brief JNI surface for the in-game Mod Menu (org.chimeramc.client.preloader.PreloaderMenu).
 *
 * <p>Deliberately thin: the catalogue and the drained interactions cross as JSON, which keeps the
 * JNI to a handful of symbols and lets the menu's own C++ types stay in C++. The JSON shapes are
 * pinned by the Java-side tests, so a field rename on one side fails the build rather than the app.
 */

#include <jni.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pl/Logger.hpp"
#include "pl/menu/MenuHost.hpp"
#include "pl/menu/MenuTheme.hpp"
#include "pl/runtime/SafeJni.hpp"

namespace {

using nlohmann::json;

std::string JStringToUtf8(JNIEnv *env, jstring value) {
  if (value == nullptr) return {};
  const char *chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) return {};
  std::string out(chars);
  env->ReleaseStringUTFChars(value, chars);
  return out;
}

pl::menu::MenuModuleRow ParseModule(const json &obj) {
  pl::menu::MenuModuleRow row;
  row.stableKey = obj.value("stable_key", std::string{});
  row.name = obj.value("name", std::string{});
  row.description = obj.value("description", std::string{});
  row.groupName = obj.value("group", std::string{});
  row.enabled = obj.value("enabled", false);
  row.favorite = obj.value("favorite", false);
  row.hasConfig = obj.value("has_config", false);
  row.unavailableReason = obj.value("unavailable", std::string{});
  if (obj.contains("configs") && obj["configs"].is_array()) {
    for (const json &cfg : obj["configs"]) {
      pl::menu::MenuConfigRow config;
      config.key = cfg.value("key", std::string{});
      config.label = cfg.value("label", std::string{});
      config.value = cfg.value("value", std::string{});
      const std::string kind = cfg.value("kind", std::string{"toggle"});
      config.kind = kind == "slider"     ? pl::menu::MenuConfigKind::Slider
                    : kind == "readonly" ? pl::menu::MenuConfigKind::ReadOnly
                                         : pl::menu::MenuConfigKind::Toggle;
      config.floatValue = cfg.value("float_value", 0.0f);
      config.floatMin = cfg.value("float_min", 0.0f);
      config.floatMax = cfg.value("float_max", 1.0f);
      row.configs.push_back(std::move(config));
    }
  }
  return row;
}

} // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderMenu_nativeInstallMenu(JNIEnv *, jclass) {
  pl::menu::InstallMenu();
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderMenu_nativeIsMenuLive(JNIEnv *, jclass) {
  return pl::menu::IsMenuLive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderMenu_nativeSetMenuOpen(JNIEnv *, jclass,
                                                                    jboolean open) {
  pl::menu::SetOpen(open == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderMenu_nativeIsMenuOpen(JNIEnv *, jclass) {
  return pl::menu::IsOpen() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderMenu_nativePushTouch(
    JNIEnv *, jclass, jint action, jfloat x, jfloat y, jfloat viewportWidth,
    jfloat viewportHeight) {
  pl::menu::TouchInput touch;
  touch.action = static_cast<int>(action);
  touch.x = static_cast<float>(x);
  touch.y = static_cast<float>(y);
  touch.viewportWidth = static_cast<float>(viewportWidth);
  touch.viewportHeight = static_cast<float>(viewportHeight);
  pl::menu::PushTouch(touch);
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_PreloaderMenu_nativePushBack(JNIEnv *, jclass) {
  pl::menu::PushBack();
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderMenu_nativeSetCatalogue(JNIEnv *env, jclass,
                                                                     jstring jsonText) {
  const std::string text = JStringToUtf8(env, jsonText);
  std::vector<pl::menu::MenuModuleRow> modules;
  std::string emptyReason;
  try {
    const json parsed = json::parse(text.empty() ? "{}" : text);
    if (parsed.contains("modules") && parsed["modules"].is_array()) {
      for (const json &entry : parsed["modules"]) modules.push_back(ParseModule(entry));
    }
    emptyReason = parsed.value("empty_reason", std::string{});
  } catch (const std::exception &e) {
    // A malformed catalogue must not take the render thread down: log and show nothing.
    sweetLoaderLogger.warn("Mod Menu: catalogue JSON rejected: {}", e.what());
    return JNI_FALSE;
  }
  pl::menu::SetCatalogue(std::move(modules), std::move(emptyReason));
  return JNI_TRUE;
}

JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_preloader_PreloaderMenu_nativeDrainEvents(JNIEnv *env, jclass) {
  json array = json::array();
  for (const pl::menu::MenuEvent &event : pl::menu::DrainEvents()) {
    const char *kind = "close";
    switch (event.kind) {
    case pl::menu::MenuEventKind::ToggleModule: kind = "toggle_module"; break;
    case pl::menu::MenuEventKind::SetConfig: kind = "set_config"; break;
    case pl::menu::MenuEventKind::ToggleFavorite: kind = "toggle_favorite"; break;
    case pl::menu::MenuEventKind::Close: kind = "close"; break;
    }
    array.push_back({{"kind", kind}, {"key", event.stableKey}, {"value", event.value}});
  }
  return env->NewStringUTF(array.dump().c_str());
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_PreloaderMenu_nativeRegisterFont(JNIEnv *env, jclass,
                                                                     jint role,
                                                                     jbyteArray data) {
  if (data == nullptr) return JNI_FALSE;
  const jsize length = env->GetArrayLength(data);
  if (length <= 0) return JNI_FALSE;
  std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
  env->GetByteArrayRegion(data, 0, length,
                          reinterpret_cast<jbyte *>(bytes.data()));
  const auto fontRole = static_cast<pl::menu::theme::FontRole>(role);
  return pl::menu::RegisterFont(fontRole, bytes.data(), length) ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
