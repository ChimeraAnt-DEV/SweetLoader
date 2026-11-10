/**
 * @file BerryApiJni.cpp
 * @brief JNI surface for the BerryAPI bridge (see BerryApiBridge.h).
 *
 * <p>Every entry is a thin copy of a cached reading; nothing here blocks or allocates beyond one
 * short string for the version and summary. The launcher calls these from the Diagnostics screen and
 * once at launch, not from a per-frame path.
 */

#include <jni.h>

#include <cstdio>
#include <cstring>
#include <vector>
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

/**
 * Encodes the loaded replay (or the capture the library holds) to container bytes.
 *
 * <p>This is the recorder half: BerryAPI's replay service keeps a capture of the live stream, and
 * its {@code save} exports it — the loaded replay when one is loaded, the capture otherwise. The
 * launcher writes those bytes to a {@code .glowreplay} file, so a recording made inside the game
 * becomes a replay the launcher can list and hand back to BerryAPI to play.
 *
 * <p>Fail-closed: a missing surface, a null {@code save}, or a {@code save} that returns null all
 * answer an empty array, which the Java side reads as "no replay to save" rather than a crash.
 */
JNIEXPORT jbyteArray JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplaySave(JNIEnv *env, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api == nullptr || api->save == nullptr) return env->NewByteArray(0);
  std::size_t len = 0;
  const void *bytes = api->save(&len);
  if (bytes == nullptr || len == 0) return env->NewByteArray(0);
  jbyteArray out = env->NewByteArray(static_cast<jsize>(len));
  if (out != nullptr) {
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(len),
                            reinterpret_cast<const jbyte *>(bytes));
  }
  return out;
}

// --- Replay capture (ABI v2) ----------------------------------------------------------------
//
// The recorder half: a host that can see the game's network stream records it here. A Bedrock
// session's entity spawns, movement, block updates and inventory all arrive as packets, so the
// packet stream is the world data - no game-memory offset is needed to reconstruct it. These are
// thin calls onto the resolved surface; a library built before the capture ABI answers the
// fail-closed default rather than being driven through a null pointer.

/** True when the surface provides the packet capture (ABI v2). */
JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayCaptureAvailable(JNIEnv *, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  return (api != nullptr && pl::berry::ReplayCaptureUsable(*api)) ? JNI_TRUE : JNI_FALSE;
}

/** Begins a capture session. Returns the ABI status (0 = ok). */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayCaptureStart(JNIEnv *, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api == nullptr || api->capture_start == nullptr) {
    return static_cast<jint>(pl::berry::ReplayStatus::NotAttached);
  }
  return api->capture_start();
}

/** Ends a capture session, keeping the captured replay for save. Returns the ABI status. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayCaptureStop(JNIEnv *, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api == nullptr || api->capture_stop == nullptr) {
    return static_cast<jint>(pl::berry::ReplayStatus::NotAttached);
  }
  return api->capture_stop();
}

/** The capture state: 0 idle, 1 recording, 2 throttled, 3 stopped, or -1 when unavailable. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayCaptureState(JNIEnv *, jclass) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api == nullptr || api->capture_state == nullptr) return -1;
  return api->capture_state();
}

/**
 * Records one packet. `dir` is 0 inbound, 1 outbound; `payload` is the body verbatim.
 * Returns the ABI status (0 = stored).
 */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeReplayCapturePacket(
    JNIEnv *env, jclass, jint dir, jint id, jbyteArray payload, jlong tick) {
  const pl::berry::ReplayApiMirror *api = Replay();
  if (api == nullptr || api->capture_packet == nullptr) {
    return static_cast<jint>(pl::berry::ReplayStatus::NotAttached);
  }
  if (payload == nullptr) return static_cast<jint>(pl::berry::ReplayStatus::BadArgument);
  const jsize len = env->GetArrayLength(payload);
  if (len <= 0) return static_cast<jint>(pl::berry::ReplayStatus::BadArgument);
  jbyte *raw = env->GetByteArrayElements(payload, nullptr);
  if (raw == nullptr) return static_cast<jint>(pl::berry::ReplayStatus::BadArgument);
  const jint status = api->capture_packet(static_cast<std::int32_t>(dir),
                                          static_cast<std::uint32_t>(id), raw,
                                          static_cast<std::size_t>(len),
                                          static_cast<std::uint64_t>(tick));
  env->ReleaseByteArrayElements(payload, raw, JNI_ABORT);
  return status;
}

} // extern "C"

// --- Governor surface -----------------------------------------------------------------------
//
// The launcher's FPS diagnostics read the adaptive quality governor through these. A missing
// library or a missing ABI answers the fail-closed default, so the screen shows "not available".

#include "pl/runtime/BerryGovernorMirror.hpp"

namespace {

const pl::berry::GovernorApiMirror *Governor() { return pl::berry::GovernorApi(); }

} // namespace

extern "C" {

/** True when the governor surface resolved and passed its ABI check. */
JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeGovernorAvailable(JNIEnv *, jclass) {
  return pl::berry::GovernorAvailable() ? JNI_TRUE : JNI_FALSE;
}

/**
 * The governor reading as {quality x6, thermal, thermallyLimited, platform, ceilingBits,
 * renderDistanceChunks, maxParticles, entityDistanceBits, animationDistanceBits,
 * cullFarEntities, lightingUpdatesPerTick, targetFps}, or an empty array when unavailable. The two
 * floats (ceiling, entity/animation distance) cross as their bit patterns.
 */
JNIEXPORT jlongArray JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeGovernorStatus(JNIEnv *env, jclass) {
  const pl::berry::GovernorApiMirror *api = Governor();
  if (api == nullptr || api->status == nullptr) return env->NewLongArray(0);
  pl::berry::GovernorStatusMirror st{};
  api->status(&st);
  std::uint32_t ceilingBits = 0;
  std::uint32_t entityBits = 0;
  std::uint32_t animBits = 0;
  std::memcpy(&ceilingBits, &st.ceiling, sizeof(ceilingBits));
  std::memcpy(&entityBits, &st.entity_distance, sizeof(entityBits));
  std::memcpy(&animBits, &st.animation_distance, sizeof(animBits));
  jlong values[14] = {
      static_cast<jlong>(st.quality[0]), static_cast<jlong>(st.quality[1]),
      static_cast<jlong>(st.quality[2]), static_cast<jlong>(st.quality[3]),
      static_cast<jlong>(st.quality[4]), static_cast<jlong>(st.quality[5]),
      st.thermal, st.thermally_limited, st.platform,
      static_cast<jlong>(ceilingBits),
      st.render_distance_chunks, st.max_particles,
      static_cast<jlong>(entityBits), static_cast<jlong>(animBits),
  };
  jlongArray out = env->NewLongArray(14);
  if (out != nullptr) env->SetLongArrayRegion(out, 0, 14, values);
  return out;
}

/** Reports platform thermal pressure. Returns the action taken, or -1 when unavailable. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeGovernorSetThermal(JNIEnv *, jclass,
                                                                      jint state) {
  const pl::berry::GovernorApiMirror *api = Governor();
  if (api == nullptr || api->set_thermal == nullptr) return -1;
  return api->set_thermal(state);
}

/** Sets the device class the governor tunes for; a no-op when unavailable. */
JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeGovernorSetPlatform(JNIEnv *, jclass,
                                                                       jint platform) {
  const pl::berry::GovernorApiMirror *api = Governor();
  if (api != nullptr && api->set_platform != nullptr) api->set_platform(platform);
}

/** Sets a hard ceiling; a no-op when unavailable. */
JNIEXPORT void JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeGovernorSetCeiling(JNIEnv *, jclass,
                                                                      jfloat ceiling) {
  const pl::berry::GovernorApiMirror *api = Governor();
  if (api != nullptr && api->set_ceiling != nullptr) api->set_ceiling(ceiling);
}

} // extern "C"

// --- Cosmetics surface ----------------------------------------------------------------------
//
// The launcher's cosmetics screen drives BerryAPI's own cosmetics system through these: the
// catalog, the local loadout, the wardrobe presets, the remote-render gate and the service
// counters. A missing library or a missing ABI answers the fail-closed default, so the screen shows
// "not available" rather than crashing. The player-model hook is installed by the preloader itself
// (see CosmeticHook.cpp) once the game's player-render target is resolved; these JNI entries only
// expose its state, so Java never has to own a raw game pointer.

#include "pl/runtime/BerryCosmeticMirror.hpp"

namespace {

const pl::berry::CosmeticApiMirror *Cosmetic() { return pl::berry::CosmeticApi(); }

/** Reads a fixed-size C string into a Java string, stopping at the buffer's NUL. */
jstring FixedString(JNIEnv *env, const char *value, std::size_t cap) {
  if (value == nullptr) return env->NewStringUTF("");
  std::size_t len = 0;
  while (len < cap && value[len] != '\0') ++len;
  std::string s(value, len);
  return env->NewStringUTF(s.c_str());
}

} // namespace

extern "C" {

/** True when the cosmetics surface resolved and passed its ABI check. */
JNIEXPORT jboolean JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticAvailable(JNIEnv *, jclass) {
  return pl::berry::CosmeticAvailable() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Reloads the catalog from a pack directory and an asset directory. Returns the number of assets
 * registered, or 0 when the surface is absent or the load was refused.
 */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticReload(JNIEnv *env, jclass,
                                                                 jstring packDir, jstring assetDir) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->reload_packs == nullptr) return 0;
  const char *pack = packDir != nullptr ? env->GetStringUTFChars(packDir, nullptr) : nullptr;
  const char *asset = assetDir != nullptr ? env->GetStringUTFChars(assetDir, nullptr) : nullptr;
  const std::uint32_t count = api->reload_packs(pack, asset);
  if (pack != nullptr) env->ReleaseStringUTFChars(packDir, pack);
  if (asset != nullptr) env->ReleaseStringUTFChars(assetDir, asset);
  return static_cast<jint>(count);
}

/** The number of registered assets, or 0 when unavailable. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticRegistryCount(JNIEnv *, jclass) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->registry_count == nullptr) return 0;
  return static_cast<jint>(api->registry_count());
}

/**
 * The catalog as an array of String[20] records, or an empty array when unavailable. Each record is
 * one asset, every field rendered as text so no numeric width crosses the boundary:
 * {id, slot, bone, geometry, texture, offsetX, offsetY, offsetZ, pitch, yaw, roll, scale,
 *  tintR, tintG, tintB, trail, animation, animationSpeed, pack, author}.
 */
JNIEXPORT jobjectArray JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticCatalog(JNIEnv *env, jclass) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  jclass stringClass = env->FindClass("java/lang/String");
  if (api == nullptr || api->list == nullptr || stringClass == nullptr) {
    return env->NewObjectArray(0, stringClass != nullptr ? stringClass : env->FindClass("java/lang/Object"), nullptr);
  }
  const std::uint32_t count = api->registry_count != nullptr ? api->registry_count() : 0;
  if (count == 0) return env->NewObjectArray(0, stringClass, nullptr);
  std::vector<pl::berry::CosmeticAssetMirror> records(count);
  std::size_t written = 0;
  if (api->list(records.data(), records.size(), &written) != 0) {
    return env->NewObjectArray(0, stringClass, nullptr);
  }
  constexpr jsize kFields = 20;
  jobjectArray out = env->NewObjectArray(static_cast<jsize>(written), stringClass, nullptr);
  if (out == nullptr) return env->NewObjectArray(0, stringClass, nullptr);
  auto num = [](double v) { char b[32]; std::snprintf(b, sizeof(b), "%g", v); return std::string(b); };
  for (jsize i = 0; i < static_cast<jsize>(written); ++i) {
    const pl::berry::CosmeticAssetMirror &r = records[static_cast<std::size_t>(i)];
    std::string fields[kFields] = {
        std::string(r.id), num(r.slot), num(r.bone), num(static_cast<double>(r.geometry)),
        num(static_cast<double>(r.texture)), num(r.offset_x), num(r.offset_y), num(r.offset_z),
        num(r.rotation_pitch), num(r.rotation_yaw), num(r.rotation_roll), num(r.scale),
        num(r.tint_r), num(r.tint_g), num(r.tint_b), num(r.trail), std::string(r.animation),
        num(r.animation_speed), std::string(r.pack), std::string(r.author)};
    jobjectArray record = env->NewObjectArray(kFields, stringClass, nullptr);
    if (record == nullptr) continue;
    for (jsize f = 0; f < kFields; ++f) {
      jstring s = env->NewStringUTF(fields[f].c_str());
      if (s != nullptr) env->SetObjectArrayElement(record, f, s);
    }
    env->SetObjectArrayElement(out, i, record);
  }
  return out;
}

/** Equips an asset on the local player. Returns the ABI status (0 = ok). */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticEquip(JNIEnv *env, jclass, jstring id) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->equip == nullptr) {
    return static_cast<jint>(pl::berry::CosmeticStatus::NotAttached);
  }
  if (id == nullptr) return static_cast<jint>(pl::berry::CosmeticStatus::BadArgument);
  const char *asset = env->GetStringUTFChars(id, nullptr);
  if (asset == nullptr) return static_cast<jint>(pl::berry::CosmeticStatus::BadArgument);
  const jint status = api->equip(asset);
  env->ReleaseStringUTFChars(id, asset);
  return status;
}

/** Removes whatever is in a slot. Returns the ABI status. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticUnequip(JNIEnv *, jclass, jint slot) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->unequip == nullptr) {
    return static_cast<jint>(pl::berry::CosmeticStatus::NotAttached);
  }
  return api->unequip(slot);
}

/** The asset id equipped in a slot, or "" when empty or unavailable. */
JNIEXPORT jstring JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticEquipped(JNIEnv *env, jclass, jint slot) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->equipped == nullptr) return env->NewStringUTF("");
  char buf[pl::berry::kCosmeticMaxId];
  if (api->equipped(slot, buf, sizeof(buf)) != 0) return env->NewStringUTF("");
  return env->NewStringUTF(buf);
}

/** Empties the local loadout. Returns the ABI status. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticClearLoadout(JNIEnv *, jclass) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->clear_loadout == nullptr) {
    return static_cast<jint>(pl::berry::CosmeticStatus::NotAttached);
  }
  return api->clear_loadout();
}

/** Saves the current loadout as a preset. Returns the ABI status. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticSavePreset(JNIEnv *env, jclass,
                                                                     jstring name) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->save_preset == nullptr) {
    return static_cast<jint>(pl::berry::CosmeticStatus::NotAttached);
  }
  if (name == nullptr) return static_cast<jint>(pl::berry::CosmeticStatus::BadArgument);
  const char *n = env->GetStringUTFChars(name, nullptr);
  if (n == nullptr) return static_cast<jint>(pl::berry::CosmeticStatus::BadArgument);
  const jint status = api->save_preset(n);
  env->ReleaseStringUTFChars(name, n);
  return status;
}

/** Replaces the loadout with a preset. Returns the ABI status. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticApplyPreset(JNIEnv *env, jclass,
                                                                      jstring name) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->apply_preset == nullptr) {
    return static_cast<jint>(pl::berry::CosmeticStatus::NotAttached);
  }
  if (name == nullptr) return static_cast<jint>(pl::berry::CosmeticStatus::BadArgument);
  const char *n = env->GetStringUTFChars(name, nullptr);
  if (n == nullptr) return static_cast<jint>(pl::berry::CosmeticStatus::BadArgument);
  const jint status = api->apply_preset(n);
  env->ReleaseStringUTFChars(name, n);
  return status;
}

/** 1 when a preset exists, 0 when not, -1 when unavailable. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticHasPreset(JNIEnv *env, jclass,
                                                                    jstring name) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->has_preset == nullptr) return -1;
  if (name == nullptr) return -1;
  const char *n = env->GetStringUTFChars(name, nullptr);
  if (n == nullptr) return -1;
  const jint has = api->has_preset(n);
  env->ReleaseStringUTFChars(name, n);
  return has;
}

/** The preset names as a String[], or an empty array when unavailable. */
JNIEXPORT jobjectArray JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticPresetNames(JNIEnv *env, jclass) {
  jclass stringClass = env->FindClass("java/lang/String");
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->preset_names == nullptr) {
    return env->NewObjectArray(0, stringClass != nullptr ? stringClass : env->FindClass("java/lang/Object"), nullptr);
  }
  const std::uint32_t presetCount = api->stats != nullptr ? [&] {
    pl::berry::CosmeticStatsMirror st{};
    api->stats(&st);
    return st.preset_count;
  }() : 0;
  if (presetCount == 0) return env->NewObjectArray(0, stringClass, nullptr);
  constexpr std::size_t kNameCap = 128;
  std::vector<char> names(static_cast<std::size_t>(presetCount) * kNameCap, '\0');
  std::size_t written = 0;
  if (api->preset_names(names.data(), kNameCap, presetCount, &written) != 0) {
    return env->NewObjectArray(0, stringClass, nullptr);
  }
  jobjectArray out = env->NewObjectArray(static_cast<jsize>(written), stringClass, nullptr);
  if (out == nullptr) return env->NewObjectArray(0, stringClass, nullptr);
  for (jsize i = 0; i < static_cast<jsize>(written); ++i) {
    jstring s = FixedString(env, names.data() + static_cast<std::size_t>(i) * kNameCap, kNameCap);
    if (s != nullptr) env->SetObjectArrayElement(out, i, s);
  }
  return out;
}

/** Sets the local-only render gate. Returns the ABI status. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticSetLocalOnly(JNIEnv *, jclass,
                                                                        jboolean localOnly) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->set_local_only == nullptr) {
    return static_cast<jint>(pl::berry::CosmeticStatus::NotAttached);
  }
  return api->set_local_only(localOnly == JNI_TRUE ? 1 : 0);
}

/** The local-only render gate (1/0), or -1 when unavailable. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticLocalOnly(JNIEnv *, jclass) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->local_only == nullptr) return -1;
  return api->local_only();
}

/**
 * The service counters as long[8]: {frames, submitted, skippedNoBone, skippedNotOwned, cacheHits,
 * cacheMisses, registryCount, presetCount}, or an empty array when unavailable.
 */
JNIEXPORT jlongArray JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticStats(JNIEnv *env, jclass) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->stats == nullptr) return env->NewLongArray(0);
  pl::berry::CosmeticStatsMirror st{};
  api->stats(&st);
  jlong values[8] = {
      static_cast<jlong>(st.frames), static_cast<jlong>(st.submitted),
      static_cast<jlong>(st.skipped_no_bone), static_cast<jlong>(st.skipped_not_owned),
      static_cast<jlong>(st.cache_hits), static_cast<jlong>(st.cache_misses),
      static_cast<jlong>(st.registry_count), static_cast<jlong>(st.preset_count)};
  jlongArray out = env->NewLongArray(8);
  if (out != nullptr) env->SetLongArrayRegion(out, 0, 8, values);
  return out;
}

/** 1 when a player-model hook is installed, 0 when not, -1 when unavailable. */
JNIEXPORT jint JNICALL
Java_org_chimeramc_client_preloader_BerryApi_nativeCosmeticHasPlayerModelHook(JNIEnv *, jclass) {
  const pl::berry::CosmeticApiMirror *api = Cosmetic();
  if (api == nullptr || api->has_player_model_hook == nullptr) return -1;
  return api->has_player_model_hook();
}

} // extern "C"
