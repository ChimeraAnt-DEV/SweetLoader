#include <jni.h>

#include <filesystem>
#include <optional>
#include <string>

#include "pl/Export.hpp"
#include "pl/Logger.hpp"
#include "pl/internal/ModManager.h"
#include "pl/runtime/FeatureAvailability.hpp"
#include "pl/runtime/GameHooks.h"
#include "pl/runtime/GameHookRules.h"
#include "pl/runtime/JavaRuntime.h"
#include "pl/runtime/ModMenuBridge.h"

namespace {

jboolean LoadModFromJava(JNIEnv *env, jstring libPath, jstring modRootPath) {
  JavaVM *vm = pl::runtime::GetJavaVm();
  if (!vm) {
    preloaderLogger.error("JavaVM is not initialized");
    return JNI_FALSE;
  }

  const char *path = env->GetStringUTFChars(libPath, nullptr);
  if (!path) {
    preloaderLogger.error("Failed to access mod library path");
    return JNI_FALSE;
  }

  std::optional<std::filesystem::path> sourceModDirectory;
  const char *sourcePath = nullptr;
  if (modRootPath) {
    sourcePath = env->GetStringUTFChars(modRootPath, nullptr);
    if (!sourcePath) {
      env->ReleaseStringUTFChars(libPath, path);
      preloaderLogger.error("Failed to access original mod root path");
      return JNI_FALSE;
    }
    sourceModDirectory = std::filesystem::path(sourcePath);
  }

  const bool loaded = ModManager::LoadModLibrary(path, sourceModDirectory, vm);

  if (sourcePath) {
    env->ReleaseStringUTFChars(modRootPath, sourcePath);
  }
  env->ReleaseStringUTFChars(libPath, path);
  return loaded ? JNI_TRUE : JNI_FALSE;
}

} // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  pl::runtime::SetJavaVm(vm);
  return JNI_VERSION_1_4;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_core_mods_ModManager_nativeLoadMod__Ljava_lang_String_2Lorg_chimeramc_client_core_mods_Mod_2(
    JNIEnv *env, jclass clazz, jstring libPath, jobject modObj) {
  (void)clazz;
  (void)modObj;
  return LoadModFromJava(env, libPath, nullptr);
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_core_mods_ModManager_nativeLoadMod__Ljava_lang_String_2Ljava_lang_String_2Lorg_chimeramc_client_core_mods_Mod_2(
    JNIEnv *env, jclass clazz, jstring libPath, jstring modRootPath,
    jobject modObj) {
  (void)clazz;
  (void)modObj;
  return LoadModFromJava(env, libPath, modRootPath);
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_core_mods_ModManager_nativeEnableLoadedMods(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  ModManager::EnableLoadedMods();
  // The hooks must not install here: this runs in the game activity's onCreate, before the engine's
  // graphics backend and the libminecraftpe.so relocation have finished, so the RTTI/vtable
  // resolution sees a partially mapped module and the install silently bails ("Hook installed: no").
  // Arm the deferred install instead; it runs from the game's own frame tick via
  // PumpGameHookInstall, by which point the runtime is up and the code patch runs on the render
  // thread rather than racing it.
  pl::runtime::ArmGameHookInstall();
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_core_mods_ModManager_nativePumpGameHookInstall(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  // Cheap on the common path: a single atomic load once the install has run.
  return pl::runtime::PumpGameHookInstall() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_core_mods_ModManager_nativeIsGameHookInstallPending(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return pl::runtime::GameHookInstallPending() ? JNI_TRUE : JNI_FALSE;
}

// The deferred-install state for the diagnostics report: 0 not armed, 1 armed/waiting, 2 resolved,
// packed with the frame ticks the gate observed so "gave up" is distinguishable from "installed".
JNIEXPORT jintArray JNICALL
Java_org_chimeramc_client_core_mods_ModManager_nativeGameHookInstallState(JNIEnv *env, jclass) {
  jint values[2];
  values[0] = pl::runtime::GameHookInstallState();
  values[1] = pl::runtime::GameHookInstallFrameTicks();
  jintArray out = env->NewIntArray(2);
  if (out == nullptr) return nullptr;
  env->SetIntArrayRegion(out, 0, 2, values);
  return out;
}

// The effective hook rule's resolved fields, for the diagnostics report (which rule and slots the
// running build actually used).
JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_core_mods_ModManager_nativeGameHookRulesSummary(JNIEnv *env, jclass) {
  const std::string summary = pl::runtime::GameHookRulesSummary();
  return env->NewStringUTF(summary.c_str());
}

// Per-feature hook availability, as one JSON object the diagnostics page prints verbatim.
// "The preloader loaded" and "this feature works" are different questions; this answers the second
// one so a single missing signature does not read as "nothing works".
JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_core_mods_ModManager_nativeFeatureAvailability(
    JNIEnv *env, jclass) {
  // Recompute from the live hook state first, so a deferred install that has since run is
  // reflected and the report can never describe a stale hook set.
  pl::runtime::RefreshGameFeatureAvailability();
  const std::string report = pl::runtime::GameFeatureAvailability().toJson();
  return env->NewStringUTF(report.c_str());
}

// Install-and-remove every hook once and report the result, so "this hook cannot install
// on this build" is a fact the diagnostics page can print rather than something to infer from a
// feature that does nothing. Runs on the loading path before the real install.
JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_core_mods_ModManager_nativeHookSelfTest(JNIEnv *env, jclass) {
  const std::string report = pl::runtime::RunGameHookSelfTest();
  return env->NewStringUTF(report.c_str());
}

JNIEXPORT void JNICALL
Java_org_chimeramc_client_core_mods_ModManager_nativeDisableAndUnloadLoadedMods(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  ModManager::DisableAndUnloadLoadedMods();
}

// Fail-safe health read for the replay death trigger. This build has no verified health field
// offset, so it reports "no data" (null) rather than inventing a reading -- the caller's
// DeathWatcher treats null as unavailable and never fires a highlight from it. The signature is
// float[] and it is read on the UI thread, so it must not throw or block.
JNIEXPORT jfloatArray JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeReadLocalPlayerHealth(
    JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Stable C ABI surface
// ---------------------------------------------------------------------------------------------
// These are the entry points a mod or embedder may rely on by name. They are versioned so a
// mismatch is refused with a clear message rather than a missing symbol or a silent misread.

/// The preloader's C ABI version. Bump when a signature in this file changes meaning.
#define PL_PRELOADER_ABI_VERSION 1

// Returns the ABI version this preloader was built with. A host that loaded a preloader expecting a
// newer ABI compares this and refuses to use the surface rather than calling into it blind.
extern "C" PL_EXPORT int pl_get_abi_version() { return PL_PRELOADER_ABI_VERSION; }

// The Java-side read of the same value, so the launcher can compare before it uses the surface.
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_PreloaderInput_nativeAbiVersion(JNIEnv *, jclass) {
  return PL_PRELOADER_ABI_VERSION;
}

} // extern "C"
