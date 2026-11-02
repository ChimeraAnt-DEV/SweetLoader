#include <jni.h>

#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pl/Logger.hpp"
#include "pl/runtime/ModMenuBridge.h"
#include "pl/runtime/SafeJni.hpp"

static nlohmann::json modulePayload(const pl::runtime::RegisteredModule &mod) {
    nlohmann::json payload = {
            {"module_id", mod.module_id},
            {"display_name", mod.display_name},
            {"description", mod.description},
            {"mod_id", mod.mod_id},
            {"enabled", mod.enabled},
            {"hide_in_hud_editor", mod.hide_in_hud_editor},
            {"config_schema_revision", mod.config_schema_revision},
            {"configs", nlohmann::json::array()},
    };
    for (const auto &cfg : mod.configs) {
        payload["configs"].push_back({
                {"key", cfg.key},
                {"display_name", cfg.display_name},
                {"type", static_cast<int>(cfg.type)},
                {"default_value", cfg.default_value},
                {"min_value", cfg.min_value},
                {"max_value", cfg.max_value},
                {"current_value", cfg.current_value},
                {"depends_on", cfg.depends_on},
        });
    }
    return payload;
}

static nlohmann::json hudEditorElementPayload(
        const pl::runtime::InternalHudEditorElement &element) {
    return {
            {"module_id", element.module_id},
            {"element_id", element.element_id},
            {"display_name", element.display_name},
            {"position_key_x", element.position_key_x},
            {"position_key_y", element.position_key_y},
            {"snap_group", element.snap_group},
            {"x", element.x},
            {"y", element.y},
            {"width", element.width},
            {"height", element.height},
            {"grid_size", element.grid_size},
            {"snap_threshold", element.snap_threshold},
            {"grid_gap", element.grid_gap},
            {"snap_flags", element.snap_flags},
    };
}

namespace {

// Item 455: an id/key/value string from Java is untrusted input. A sane ceiling bounds the work
// the registry does with it and stops a multi-megabyte string from being copied into a lookup;
// anything longer is refused rather than truncated, because a truncated id is a different id.
constexpr jsize kMaxIdentifierChars = 4096;
constexpr jsize kMaxValueChars = 64 * 1024;

/**
 * Reads a jstring into a std::string, refusing an over-long one.
 *
 * @param maxChars the longest acceptable string; a longer one reads as absent (false)
 * @return true when the string was read; false when it was null, over-long, or unreadable
 */
bool ReadString(JNIEnv *env, jstring value, jsize maxChars, std::string &out) {
    if (!value) return false;
    const jsize length = env->GetStringUTFLength(value);
    if (length < 0 || length > maxChars) return false;
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return false;
    out.assign(chars, static_cast<size_t>(length));
    env->ReleaseStringUTFChars(value, chars);
    return true;
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetExternalModCount(
        JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return pl::runtime::GetRegisteredModuleCount();
}

JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetExternalModInfo(
        JNIEnv *env, jclass clazz, jint index) {
    (void)clazz;
    pl::runtime::RegisteredModule mod;
    if (!pl::runtime::GetRegisteredModuleInfo(index, mod)) {
        return pl::jni::NewString(env, "{}");
    }

    const std::string json = modulePayload(mod).dump();
    return pl::jni::NewString(env, json.c_str());
}

JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetExternalModsInfo(
        JNIEnv *env, jclass clazz) {
    (void)clazz;
    std::vector<pl::runtime::RegisteredModule> modules;
    pl::runtime::GetRegisteredModulesInfo(modules);
    nlohmann::json payload = nlohmann::json::array();
    for (const auto &module : modules) payload.push_back(modulePayload(module));
    const std::string json = payload.dump();
    return pl::jni::NewString(env, json.c_str());
}

JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetExternalModConfigSchema(
        JNIEnv *env, jclass clazz, jstring moduleId) {
    (void)clazz;
    std::string id;
    if (!ReadString(env, moduleId, kMaxIdentifierChars, id)) {
        return pl::jni::NewString(env, "");
    }
    std::string schema;
    pl::runtime::GetRegisteredModuleConfigSchema(id.c_str(), schema);
    return pl::jni::NewString(env, schema.c_str());
}

JNIEXPORT jlong JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetExternalModConfigSchemaRevision(
        JNIEnv *env, jclass clazz, jstring moduleId) {
    (void)clazz;
    std::string id;
    if (!ReadString(env, moduleId, kMaxIdentifierChars, id)) return 0;
    return static_cast<jlong>(pl::runtime::GetRegisteredModuleConfigSchemaRevision(id.c_str()));
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeToggleExternalMod(
        JNIEnv *env, jclass clazz, jstring moduleId, jboolean enabled) {
(void)clazz;
std::string id;
if (!ReadString(env, moduleId, kMaxIdentifierChars, id)) {
return;
}
pl::runtime::ToggleRegisteredModule(id.c_str(), enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeSetExternalModConfig(
        JNIEnv *env, jclass clazz, jstring moduleId, jstring key, jstring value) {
(void)clazz;
std::string id;
std::string configKey;
if (!ReadString(env, moduleId, kMaxIdentifierChars, id) ||
    !ReadString(env, key, kMaxIdentifierChars, configKey)) {
    return;
}
// A null value is legal (it clears the key); a present but over-long one is refused.
std::string configValue;
if (value && !ReadString(env, value, kMaxValueChars, configValue)) {
    return;
}
pl::runtime::SetRegisteredModuleConfig(id.c_str(), configKey.c_str(), configValue.c_str());
}

JNIEXPORT jint JNICALL
        Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetExternalButtonCount(
        JNIEnv *env, jclass clazz) {
(void)env;
(void)clazz;
return pl::runtime::GetRegisteredButtonCount();
}

JNIEXPORT jstring JNICALL
        Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetExternalButtonInfo(
        JNIEnv *env, jclass clazz, jint index) {
(void)clazz;
pl::runtime::RegisteredButton button;
if (!pl::runtime::GetRegisteredButtonInfo(index, button)) {
return pl::jni::NewString(env, "{}");
}

nlohmann::json payload = {
        {"button_id", button.button_id},
        {"module_id", button.module_id},
        {"display_name", button.display_name},
        {"mod_id", button.mod_id},
        {"label", button.label},
        {"android_key_code", button.android_key_code},
        {"behavior", static_cast<int>(button.behavior)},
        {"default_visible", button.default_visible},
        {"module_enabled", button.module_enabled},
        {"has_icon", !button.icon_data.empty()},
        {"icon_format", static_cast<int>(button.icon_format)},
        {"hide_label_when_icon_present", button.hide_label_when_icon_present},
        {"style",
                {{"preset", static_cast<int>(button.style_preset)},
                              {"normal_bg_color", button.normal_bg_color},
                              {"active_bg_color", button.active_bg_color},
                              {"border_color", button.border_color},
                              {"text_color", button.text_color},
                              {"active_text_color", button.active_text_color},
                              {"width_scale", button.width_scale},
                              {"height_scale", button.height_scale}}},
};
const std::string json = payload.dump();
return pl::jni::NewString(env, json.c_str());
}

JNIEXPORT jbyteArray JNICALL
        Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetExternalButtonIconBytes(
        JNIEnv *env, jclass clazz, jstring buttonId, jint width, jint height, jboolean active) {
(void)clazz;
std::string id;
if (!ReadString(env, buttonId, kMaxIdentifierChars, id)) {
    return nullptr;
}

std::vector<unsigned char> iconData;
const bool found =
        pl::runtime::GetRegisteredButtonIconBytes(id.c_str(), width, height, active, iconData);

if (!found || iconData.empty()) {
return nullptr;
}

const size_t maxArrayLength =
        static_cast<size_t>(std::numeric_limits<jsize>::max());
if (iconData.size() > maxArrayLength) {
preloaderLogger.error("Registered button icon is too large to marshal to "
"Java: {}",
iconData.size());
return nullptr;
}

const jsize byteCount = static_cast<jsize>(iconData.size());
jbyteArray result = env->NewByteArray(byteCount);
if (!result) {
return nullptr;
}
env->SetByteArrayRegion(result, 0, byteCount,
reinterpret_cast<const jbyte *>(iconData.data()));
if (env->ExceptionCheck()) {
return nullptr;
}
return result;
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeDispatchExternalButtonEvent(
        JNIEnv *env, jclass clazz, jstring buttonId, jint event, jfloat value) {
(void)clazz;
std::string id;
if (!ReadString(env, buttonId, kMaxIdentifierChars, id)) {
    return;
}
pl::runtime::DispatchRegisteredButtonEvent(
        id.c_str(), static_cast<pl::modmenu::ButtonEvent>(event), value);
}

JNIEXPORT jbyteArray JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_MoreButtonsSvgBridge_nativeRenderSvgToPng(
        JNIEnv *env, jclass clazz, jbyteArray svgData, jint width, jint height) {
    (void)clazz;
    if (!svgData || width <= 0 || height <= 0) {
        return nullptr;
    }

    const jsize length = env->GetArrayLength(svgData);
    if (length <= 0 || length > 4 * 1024 * 1024) {
        return nullptr;
    }

    std::vector<unsigned char> svg(static_cast<size_t>(length));
    env->GetByteArrayRegion(svgData, 0, length, reinterpret_cast<jbyte *>(svg.data()));
    if (env->ExceptionCheck()) {
        return nullptr;
    }

    std::vector<unsigned char> png;
    if (!pl::runtime::RenderSvgBytesToPng(svg.data(), svg.size(), width, height, png) || png.empty()) {
        return nullptr;
    }

    if (png.size() > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
        return nullptr;
    }
    jbyteArray result = env->NewByteArray(static_cast<jsize>(png.size()));
    if (!result) {
        return nullptr;
    }
    env->SetByteArrayRegion(result, 0, static_cast<jsize>(png.size()),
                            reinterpret_cast<const jbyte *>(png.data()));
    return env->ExceptionCheck() ? nullptr : result;
}

JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetHudEditorElementsInfo(
        JNIEnv *env, jclass clazz) {
    (void)clazz;
    std::vector<pl::runtime::InternalHudEditorElement> elements;
    pl::runtime::GetHudEditorElements(elements);
    nlohmann::json payload = nlohmann::json::array();
    for (const auto &element : elements) {
        payload.push_back(hudEditorElementPayload(element));
    }
    const std::string json = payload.dump();
    return pl::jni::NewString(env, json.c_str());
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeSetHudSurfaceSize(
        JNIEnv *env, jclass clazz, jfloat width, jfloat height) {
    (void)env;
    (void)clazz;
    pl::runtime::SetHudSurfaceSize(width, height);
}

JNIEXPORT jlong JNICALL
Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetDrawCommandsRevision(
        JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return static_cast<jlong>(pl::runtime::GetDrawCommandsRevision());
}

JNIEXPORT jobjectArray JNICALL
        Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetDrawCommands(
        JNIEnv *env, jclass clazz) {
(void)clazz;
std::vector<pl::runtime::InternalDrawCommand> cmds;
pl::runtime::GetDrawCommands(cmds);

constexpr size_t kRectFieldsPerCommand = 6;
constexpr jsize kResultFieldCount = 8;
const size_t commandCount = cmds.size();
const size_t maxArrayLength =
        static_cast<size_t>(std::numeric_limits<jsize>::max());
if (commandCount > maxArrayLength ||
commandCount > maxArrayLength / kRectFieldsPerCommand) {
preloaderLogger.error("Too many draw commands to marshal to Java: {}",
commandCount);
return nullptr;
}

const jsize n = static_cast<jsize>(commandCount);
const jsize rectCount =
        static_cast<jsize>(commandCount * kRectFieldsPerCommand);

jclass stringClass = env->FindClass("java/lang/String");
if (!stringClass) {
return nullptr;
}
jclass objectClass = env->FindClass("java/lang/Object");
if (!objectClass) {
env->DeleteLocalRef(stringClass);
return nullptr;
}

jintArray typesArray = env->NewIntArray(n);
jfloatArray rectsArray = env->NewFloatArray(rectCount);
jintArray colorsArray = env->NewIntArray(n);
jfloatArray sizesArray = env->NewFloatArray(n);
jobjectArray textsArray = env->NewObjectArray(n, stringClass, nullptr);
jobjectArray modulesArray = env->NewObjectArray(n, stringClass, nullptr);
jobjectArray fontsArray = env->NewObjectArray(n, stringClass, nullptr);
jobjectArray imagesArray = env->NewObjectArray(n, stringClass, nullptr);

auto cleanupRefs = [&] {
    if (typesArray) {
        env->DeleteLocalRef(typesArray);
    }
    if (rectsArray) {
        env->DeleteLocalRef(rectsArray);
    }
    if (colorsArray) {
        env->DeleteLocalRef(colorsArray);
    }
    if (sizesArray) {
        env->DeleteLocalRef(sizesArray);
    }
    if (textsArray) {
        env->DeleteLocalRef(textsArray);
    }
    if (modulesArray) {
        env->DeleteLocalRef(modulesArray);
    }
    if (fontsArray) {
        env->DeleteLocalRef(fontsArray);
    }
    if (imagesArray) {
        env->DeleteLocalRef(imagesArray);
    }
    env->DeleteLocalRef(objectClass);
    env->DeleteLocalRef(stringClass);
};

if (!typesArray || !rectsArray || !colorsArray || !sizesArray ||
!textsArray || !modulesArray || !fontsArray || !imagesArray) {
cleanupRefs();
return nullptr;
}

auto setStringElement = [&](jobjectArray array, jsize index,
                            const std::string &value) {
    if (value.empty()) {
        return true;
    }
    jstring str = pl::jni::NewString(env, value.c_str());
    if (!str) {
        return false;
    }
    env->SetObjectArrayElement(array, index, str);
    env->DeleteLocalRef(str);
    return !env->ExceptionCheck();
};

if (commandCount > 0) {
std::vector<jint> types(commandCount);
std::vector<jfloat> rects(static_cast<size_t>(rectCount));
std::vector<jint> colors(commandCount);
std::vector<jfloat> sizes(commandCount);
for (size_t i = 0; i < commandCount; ++i) {
const size_t rectOffset = i * kRectFieldsPerCommand;
types[i] = static_cast<jint>(cmds[i].type);
rects[rectOffset + 0] = cmds[i].x;
rects[rectOffset + 1] = cmds[i].y;
rects[rectOffset + 2] = cmds[i].w;
rects[rectOffset + 3] = cmds[i].h;
rects[rectOffset + 4] = cmds[i].x3;
rects[rectOffset + 5] = cmds[i].y3;
colors[i] = static_cast<jint>(cmds[i].color);
sizes[i] = cmds[i].size;
const jsize index = static_cast<jsize>(i);
if (!setStringElement(textsArray, index, cmds[i].text) ||
!setStringElement(modulesArray, index, cmds[i].module_id) ||
!setStringElement(fontsArray, index, cmds[i].font_id) ||
!setStringElement(imagesArray, index, cmds[i].image_id)) {
cleanupRefs();
return nullptr;
}
}
env->SetIntArrayRegion(typesArray, 0, n, types.data());
env->SetFloatArrayRegion(rectsArray, 0, rectCount, rects.data());
env->SetIntArrayRegion(colorsArray, 0, n, colors.data());
env->SetFloatArrayRegion(sizesArray, 0, n, sizes.data());
if (env->ExceptionCheck()) {
cleanupRefs();
return nullptr;
}
}

jobjectArray result =
        env->NewObjectArray(kResultFieldCount, objectClass, nullptr);
if (!result) {
cleanupRefs();
return nullptr;
}

env->SetObjectArrayElement(result, 0, typesArray);
env->SetObjectArrayElement(result, 1, rectsArray);
env->SetObjectArrayElement(result, 2, colorsArray);
env->SetObjectArrayElement(result, 3, sizesArray);
env->SetObjectArrayElement(result, 4, textsArray);
env->SetObjectArrayElement(result, 5, modulesArray);
env->SetObjectArrayElement(result, 6, fontsArray);
env->SetObjectArrayElement(result, 7, imagesArray);

if (env->ExceptionCheck()) {
env->DeleteLocalRef(result);
cleanupRefs();
return nullptr;
}

cleanupRefs();

return result;
}

JNIEXPORT jbyteArray JNICALL
        Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetRegisteredFontBytes(
        JNIEnv *env, jclass clazz, jstring fontId) {
(void)clazz;
std::string idStr;
if (!ReadString(env, fontId, kMaxIdentifierChars, idStr)) {
    return nullptr;
}

std::vector<unsigned char> fontData;
const bool found = pl::runtime::GetRegisteredFontBytes(idStr.c_str(), fontData);

if (!found || fontData.empty()) {
return nullptr;
}

const size_t maxArrayLength =
        static_cast<size_t>(std::numeric_limits<jsize>::max());
if (fontData.size() > maxArrayLength) {
preloaderLogger.error(
"Registered font is too large to marshal to Java: {}", fontData.size());
return nullptr;
}

const jsize byteCount = static_cast<jsize>(fontData.size());
jbyteArray result = env->NewByteArray(byteCount);
if (!result) {
return nullptr;
}
env->SetByteArrayRegion(result, 0, byteCount,
reinterpret_cast<const jbyte *>(fontData.data()));
if (env->ExceptionCheck()) {
return nullptr;
}
return result;
}

JNIEXPORT jobjectArray JNICALL
        Java_org_chimeramc_client_core_mods_inbuilt_ExternalModBridge_nativeGetRegisteredImage(
        JNIEnv *env, jclass clazz, jstring imageId) {
(void)clazz;
std::string idStr;
if (!ReadString(env, imageId, kMaxIdentifierChars, idStr)) {
    return nullptr;
}

std::vector<unsigned char> imageData;
int width = 0;
int height = 0;
const bool found =
        pl::runtime::GetRegisteredImageBytes(idStr.c_str(), imageData, width, height);

if (!found || imageData.empty() || width <= 0 || height <= 0) {
return nullptr;
}

const size_t maxArrayLength =
        static_cast<size_t>(std::numeric_limits<jsize>::max());
if (imageData.size() > maxArrayLength) {
preloaderLogger.error("Registered image is too large to marshal to Java: {}",
imageData.size());
return nullptr;
}

jclass objectClass = env->FindClass("java/lang/Object");
if (!objectClass) {
return nullptr;
}
jobjectArray result = env->NewObjectArray(2, objectClass, nullptr);
env->DeleteLocalRef(objectClass);
if (!result) {
return nullptr;
}

const jsize byteCount = static_cast<jsize>(imageData.size());
jbyteArray bytesArray = env->NewByteArray(byteCount);
if (!bytesArray) {
env->DeleteLocalRef(result);
return nullptr;
}
env->SetByteArrayRegion(bytesArray, 0, byteCount,
reinterpret_cast<const jbyte *>(imageData.data()));
if (env->ExceptionCheck()) {
env->DeleteLocalRef(bytesArray);
env->DeleteLocalRef(result);
return nullptr;
}

jintArray dimensionsArray = env->NewIntArray(2);
if (!dimensionsArray) {
env->DeleteLocalRef(bytesArray);
env->DeleteLocalRef(result);
return nullptr;
}
const jint dimensions[] = {static_cast<jint>(width),
                           static_cast<jint>(height)};
env->SetIntArrayRegion(dimensionsArray, 0, 2, dimensions);
if (env->ExceptionCheck()) {
env->DeleteLocalRef(dimensionsArray);
env->DeleteLocalRef(bytesArray);
env->DeleteLocalRef(result);
return nullptr;
}

env->SetObjectArrayElement(result, 0, bytesArray);
env->SetObjectArrayElement(result, 1, dimensionsArray);
env->DeleteLocalRef(dimensionsArray);
env->DeleteLocalRef(bytesArray);
if (env->ExceptionCheck()) {
env->DeleteLocalRef(result);
return nullptr;
}
return result;
}

} // extern "C"
