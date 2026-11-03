/**
 * @file BerryApiJni.cpp
 * @brief JNI surface for the BerryAPI bridge (see BerryApiBridge.h).
 *
 * <p>Every entry is a thin copy of a cached reading; nothing here blocks or allocates beyond one
 * short string for the version and summary. The launcher calls these from the Diagnostics screen and
 * once at launch, not from a per-frame path.
 */

#include <jni.h>

#include <cstring>
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

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeBindWorldSeam(JNIEnv *, jclass) {
  return pl::berry::BindWorldSeam() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeWorldSeamBound(JNIEnv *, jclass) {
  return pl::berry::WorldSeamBound() ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"

// --- Replay surface -------------------------------------------------------------------------
//
// The launcher's replay screen drives these. Each is a thin call onto the resolved surface; a
// missing library or a missing ABI answers the fail-closed default (false / -1 / empty), so the
// screen can show "not available" rather than crash.

#include "pl/runtime/BerryReplayMirror.hpp"

namespace {

const pl::berry::ReplayApiMirror *Replay() { return pl::berry::ReplayApi(); }

} // namespace

extern "C" {

/** True when the replay surface resolved and passed its ABI check. */
JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayAvailable(JNIEnv *, jclass) {
  return pl::berry::ReplayAvailable() ? JNI_TRUE : JNI_FALSE;
}

/** True when a replay is loaded. */
JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayLoaded(JNIEnv *, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  return (api != nullptr && api->loaded != nullptr && api->loaded() != 0) ? JNI_TRUE : JNI_FALSE;
}

/**
 * The replay summary as {loaded, first_tick, last_tick, section_count, record_count,
 * payload_bytes, build_id, protocol_version}, or an empty array when the surface is absent. The
 * ticks are narrowed to jlong, which is exact for any tick this build can reach.
 */
JNIEXPORT jlongArray JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayInfo(JNIEnv *env, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api == nullptr || api->info == nullptr) return env->NewLongArray(0);
  pl::berry::ReplayInfoMirror info{};
  if (api->info(&info) != 0) return env->NewLongArray(0);
  jlong values[8] = {
      info.loaded,
      static_cast<jlong>(info.first_tick),
      static_cast<jlong>(info.last_tick),
      info.section_count,
      info.record_count,
      static_cast<jlong>(info.payload_bytes),
      info.build_id,
      info.protocol_version,
  };
  jlongArray out = env->NewLongArray(8);
  if (out != nullptr) env->SetLongArrayRegion(out, 0, 8, values);
  return out;
}

/** The playback state as {active, playing, active_world, tick, speed_bits}, or empty. */
JNIEXPORT jlongArray JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayPlayback(JNIEnv *env, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api == nullptr || api->playback == nullptr) return env->NewLongArray(0);
  pl::berry::ReplayPlaybackMirror pb{};
  api->playback(&pb);
  float speed = pb.speed;
  std::uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(speed), "speed is a 32-bit float");
  std::memcpy(&bits, &speed, sizeof(bits));
  jlong values[5] = {
      pb.active,
      pb.playing,
      pb.active_world,
      static_cast<jlong>(pb.tick),
      static_cast<jlong>(bits),
  };
  jlongArray out = env->NewLongArray(5);
  if (out != nullptr) env->SetLongArrayRegion(out, 0, 5, values);
  return out;
}

/** Decodes a replay from bytes. Returns the status code (0 = ok). */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayLoad(JNIEnv *env, jclass,
                                                              jbyteArray bytes) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api == nullptr || api->load == nullptr) {
    return static_cast<jint>(pl::berry::ReplayStatus::NotAttached);
  }
  if (bytes == nullptr) return static_cast<jint>(pl::berry::ReplayStatus::BadArgument);
  const jsize len = env->GetArrayLength(bytes);
  if (len <= 0) return static_cast<jint>(pl::berry::ReplayStatus::BadArgument);
  jbyte *raw = env->GetByteArrayElements(bytes, nullptr);
  if (raw == nullptr) return static_cast<jint>(pl::berry::ReplayStatus::BadArgument);
  const jint status = api->load(raw, static_cast<std::size_t>(len));
  env->ReleaseByteArrayElements(bytes, raw, JNI_ABORT);
  return status;
}

/** Starts playback. Returns the status code. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayPlay(JNIEnv *, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api == nullptr || api->play == nullptr) {
    return static_cast<jint>(pl::berry::ReplayStatus::NotAttached);
  }
  return api->play();
}

/** Seeks to an absolute tick. Returns the status code. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplaySeek(JNIEnv *, jclass, jlong tick) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api == nullptr || api->seek == nullptr) {
    return static_cast<jint>(pl::berry::ReplayStatus::NotAttached);
  }
  return api->seek(static_cast<std::uint64_t>(tick));
}

/** Sets the playback speed when the surface provides it; a no-op otherwise. */
JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplaySetSpeed(JNIEnv *, jclass, jfloat speed) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api != nullptr && api->set_speed != nullptr) api->set_speed(speed);
}

/** Stops playback and releases the world. */
JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayStop(JNIEnv *, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api != nullptr && api->stop != nullptr) api->stop();
}

/** Pauses playback. */
JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayPause(JNIEnv *, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api != nullptr && api->pause != nullptr) api->pause();
}

/** Drops the loaded replay. */
JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayUnload(JNIEnv *, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api != nullptr && api->unload != nullptr) api->unload();
}

/** The library's status line, or "" when unavailable. */
JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayStatus(JNIEnv *env, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api == nullptr || api->status == nullptr) return env->NewStringUTF("");
  const char *s = api->status();
  return env->NewStringUTF(s != nullptr ? s : "");
}

} // extern "C"
