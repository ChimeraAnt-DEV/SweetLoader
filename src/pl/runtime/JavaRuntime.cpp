#include "pl/runtime/JavaRuntime.h"
#include "pl/runtime/JniAttach.hpp"
#include "pl/runtime/RuntimeHandles.hpp"
#include "pl/runtime/SafeJni.hpp"
#include "pl/Platform.hpp"
#include <algorithm>
#include <string_view>

namespace pl::runtime {
namespace {

// The JVM and activity handles cross threads (JNI_OnLoad / nativeSetActivity on one, ScopedJniEnv /
// the call helpers on any), so they live in one struct of atomics (audit items 3.7-3.9) together
// with the teardown flag that stands a thread-exit detach down during JVM shutdown (item 3.10).
pl::runtime::RuntimeHandles g_runtime;

// One attachment per native thread, kept for the thread's whole life.
//
// A native thread that calls back into Java repeatedly (the cosmetics receive loop, a mod's worker)
// used to attach on every call and detach at the end of it, so each call paid an Attach/Detach pair.
// The attachment is now held in a `thread_local` whose destructor detaches when the thread exits.
// The JNI spec allows a thread to stay attached for as long as it likes; detaching per call is what
// is unnecessary. A thread that was already attached when we first saw it (the UI thread) is marked
// as not ours, so the destructor never detaches a thread we did not attach.
struct ThreadAttachment {
  JNIEnv *env = nullptr;
  pl::jni::ThreadAttachmentLifetime lifetime;

  ~ThreadAttachment() {
    JavaVM *vm = g_runtime.GetVm();
    if (vm != nullptr &&
        pl::runtime::ShouldDetachOnThreadExit(lifetime.mustDetachOnThreadExit(),
                                              g_runtime.IsVmShuttingDown())) {
      vm->DetachCurrentThread();
    }
  }
};

thread_local ThreadAttachment t_attachment;

// Resolves this thread's env once. Returns nullptr when the thread cannot be attached.
JNIEnv *AcquireThreadEnv() {
  // No VM yet: do not resolve, so a later SetJavaVm is still picked up by the next call.
  JavaVM *vm = g_runtime.GetVm();
  if (!vm) return nullptr;
  ThreadAttachment &attachment = t_attachment;
  if (attachment.lifetime.resolved()) return attachment.env;

  JNIEnv *env = nullptr;
  const jint status = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_4);
  const pl::jni::AttachAction action = pl::jni::DecideAttach(static_cast<int>(status), true);
  // The lifetime rule (which thread owns the detach) is the pure, host-tested part; this is only
  // the JNI call it decides on.
  if (attachment.lifetime.resolve(action)) {
#if defined(__ANDROID__)
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
#else
    if (vm->AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr) != JNI_OK) return nullptr;
#endif
  }
  if (!attachment.lifetime.usable()) return nullptr;
  attachment.env = env;
  return env;
}

} // namespace

void SetJavaVm(JavaVM *vm) { g_runtime.vm.store(vm, std::memory_order_release); }

JavaVM *GetJavaVm() { return g_runtime.GetVm(); }

void MarkJavaVmShuttingDown() {
  g_runtime.vmShuttingDown.store(true, std::memory_order_release);
}

ScopedJniEnv::ScopedJniEnv() {
  JNIEnv *env = AcquireThreadEnv();
  if (env == nullptr) return;
  // The thread owns its attachment now, not this scope: the scope only bounds the local reference
  // table (see release()), so a thread that outlives it is never detached here.
  env_ = env;
  // Bound the local reference table for the duration. A long-running caller (a receive loop) that
  // never pushed a frame would otherwise accumulate references and eventually fail every call.
  if (env_ != nullptr && env_->PushLocalFrame(64) == 0) {
    framePushed_ = true;
  }
}

void ScopedJniEnv::release() {
  if (released_) return;
  released_ = true;
  if (env_ != nullptr && framePushed_) {
    env_->PopLocalFrame(nullptr);
  }
  // The thread's attachment is released by `ThreadAttachment`'s destructor at thread exit (item
  // 364), never here: a scope must not detach a thread that other calls on the same thread will
  // reuse.
}

ScopedJniEnv::~ScopedJniEnv() { release(); }

void SetActivity(JNIEnv *env, jobject activity) {
  ClearActivity(env);
  if (activity) {
    g_runtime.activity.store(env->NewGlobalRef(activity), std::memory_order_release);
  }
}

void ClearActivity(JNIEnv *env) {
  jobject previous = g_runtime.activity.exchange(nullptr, std::memory_order_acq_rel);
  if (previous) {
    env->DeleteGlobalRef(previous);
  }
}

void CallActivityVoidMethod(const char *methodName) {
  jobject activity = g_runtime.GetActivity();
  if (!activity) return;

  ScopedJniEnv scope;
  if (!scope.valid()) return;
  JNIEnv *env = scope.env();

  jclass cls = env->GetObjectClass(activity);
  if (cls) {
    jmethodID mid = env->GetMethodID(cls, methodName, "()V");
    if (mid) {
      env->CallVoidMethod(activity, mid);
    }
  }
  // The scope's destructor pops the local frame (the thread's attachment outlives it); an
  // exception is cleared before the frame pop so a pending throwable cannot make PopLocalFrame
  // abort.
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
}

bool SetClipboardTextImpl(std::string_view text) {
  jobject activity = g_runtime.GetActivity();
  if (!activity) return false;

  ScopedJniEnv scope;
  if (!scope.valid()) return false;
  JNIEnv *env = scope.env();

  bool success = false;
  jclass activityClass = env->GetObjectClass(activity);
  jstring serviceName = pl::jni::NewString(env, "clipboard");
  jobject manager = nullptr;
  if (activityClass && serviceName) {
    jmethodID getSystemService = env->GetMethodID(
        activityClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    if (getSystemService) manager = env->CallObjectMethod(activity, getSystemService, serviceName);
  }

  jclass clipDataClass = env->FindClass("android/content/ClipData");
  jclass clipboardClass = env->FindClass("android/content/ClipboardManager");
  jstring label = pl::jni::NewString(env, "");
  jstring content = pl::jni::NewString(env, text);
  jobject clip = nullptr;
  if (manager && clipDataClass && label && content) {
    jmethodID newPlainText = env->GetStaticMethodID(
        clipDataClass, "newPlainText",
        "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
    if (newPlainText) clip = env->CallStaticObjectMethod(clipDataClass, newPlainText, label, content);
  }
  if (manager && clipboardClass && clip) {
    jmethodID setPrimaryClip = env->GetMethodID(
        clipboardClass, "setPrimaryClip", "(Landroid/content/ClipData;)V");
    if (setPrimaryClip) {
      env->CallVoidMethod(manager, setPrimaryClip, clip);
      success = !env->ExceptionCheck();
    }
  }

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    success = false;
  }
  // No per-reference DeleteLocalRef calls: the scope's frame pop releases them all at once, which is
  // both cheaper and correct even on the early-return paths above.
  return success;
}

bool CallActivityStringMethod(const char *methodName, const std::string &value) {
  jobject activity = g_runtime.GetActivity();
  if (!activity) return false;

  ScopedJniEnv scope;
  if (!scope.valid()) return false;
  JNIEnv *env = scope.env();

  bool called = false;
  jclass cls = env->GetObjectClass(activity);
  if (cls) {
    jmethodID mid = env->GetMethodID(cls, methodName, "(Ljava/lang/String;)V");
    if (mid) {
      jstring argument = pl::jni::NewString(env, value);
      if (argument) {
        env->CallVoidMethod(activity, mid, argument);
        called = !env->ExceptionCheck();
      }
    }
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    called = false;
  }
  return called;
}

pl::platform::HttpResponse HttpGetImpl(std::string_view url, int timeoutMs) {
  pl::platform::HttpResponse result;
  if (url.empty()) return result;
  if (!url.starts_with("https://") && !url.starts_with("http://")) return result;

  // The scoped attachment replaces the hand-rolled GetEnv/Attach/Detach here: it attaches only when
  // the thread was detached and detaches on every exit path (including the early returns inside the
  // lambda below), and its local frame bounds the reference table for the whole request.
  ScopedJniEnv scope;
  if (!scope.valid()) return result;
  JNIEnv *env = scope.env();

  jobject connection = nullptr;
  jobject input = nullptr;
  jmethodID close = nullptr;
  jmethodID disconnect = nullptr;
  auto findClass = [&](const char *name) -> jclass {
    return env->ExceptionCheck() ? nullptr : env->FindClass(name);
  };
  auto method = [&](jclass cls, const char *name, const char *signature) -> jmethodID {
    return !cls || env->ExceptionCheck() ? nullptr : env->GetMethodID(cls, name, signature);
  };
  auto string = [&](const char *value) -> jstring {
    return env->ExceptionCheck() ? nullptr : pl::jni::NewString(env, value);
  };

  const bool success = [&]() -> bool {
    jclass urlClass = findClass("java/net/URL");
    jclass connectionClass = findClass("java/net/HttpURLConnection");
    jclass inputClass = findClass("java/io/InputStream");
    jclass outputClass = findClass("java/io/ByteArrayOutputStream");
    if (!urlClass || !connectionClass || !inputClass || !outputClass) return false;

    close = method(inputClass, "close", "()V");
    disconnect = method(connectionClass, "disconnect", "()V");
    jmethodID urlCtor = method(urlClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID openConnection = method(urlClass, "openConnection", "()Ljava/net/URLConnection;");
    jmethodID setConnectTimeout = method(connectionClass, "setConnectTimeout", "(I)V");
    jmethodID setReadTimeout = method(connectionClass, "setReadTimeout", "(I)V");
    jmethodID setRequestMethod = method(connectionClass, "setRequestMethod", "(Ljava/lang/String;)V");
    jmethodID setRequestProperty = method(connectionClass, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
    jmethodID getResponseCode = method(connectionClass, "getResponseCode", "()I");
    jmethodID getHeaderField = method(connectionClass, "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;");
    jmethodID getInputStream = method(connectionClass, "getInputStream", "()Ljava/io/InputStream;");
    jmethodID getErrorStream = method(connectionClass, "getErrorStream", "()Ljava/io/InputStream;");
    jmethodID outputCtor = method(outputClass, "<init>", "()V");
    jmethodID read = method(inputClass, "read", "([B)I");
    jmethodID write = method(outputClass, "write", "([BII)V");
    jmethodID toByteArray = method(outputClass, "toByteArray", "()[B");
    if (!close || !disconnect || !urlCtor || !openConnection || !setConnectTimeout ||
        !setReadTimeout || !setRequestMethod || !setRequestProperty || !getResponseCode ||
        !getHeaderField || !getInputStream || !getErrorStream || !outputCtor ||
        !read || !write || !toByteArray || env->ExceptionCheck()) return false;

    std::string urlString(url);
    jstring jUrl = string(urlString.c_str());
    jstring get = string("GET");
    jstring acceptKey = string("Accept");
    jstring acceptValue = string("application/json");
    jstring agentKey = string("User-Agent");
    jstring agentValue = string("ChimeraPreloader/1");
    jstring retryKey = string("Retry-After");
    if (!jUrl || !get || !acceptKey || !acceptValue || !agentKey || !agentValue || !retryKey) return false;

    jobject urlObject = env->NewObject(urlClass, urlCtor, jUrl);
    if (env->ExceptionCheck() || !urlObject) return false;
    connection = env->CallObjectMethod(urlObject, openConnection);
    if (env->ExceptionCheck() || !connection) return false;
    const int safeTimeout = std::clamp(timeoutMs <= 0 ? 5000 : timeoutMs, 1000, 30000);
    env->CallVoidMethod(connection, setConnectTimeout, safeTimeout);
    if (env->ExceptionCheck()) return false;
    env->CallVoidMethod(connection, setReadTimeout, safeTimeout);
    if (env->ExceptionCheck()) return false;
    env->CallVoidMethod(connection, setRequestMethod, get);
    if (env->ExceptionCheck()) return false;
    env->CallVoidMethod(connection, setRequestProperty, acceptKey, acceptValue);
    if (env->ExceptionCheck()) return false;
    env->CallVoidMethod(connection, setRequestProperty, agentKey, agentValue);
    if (env->ExceptionCheck()) return false;

    result.status = env->CallIntMethod(connection, getResponseCode);
    if (env->ExceptionCheck()) return false;
    jstring retry = static_cast<jstring>(env->CallObjectMethod(connection, getHeaderField, retryKey));
    if (env->ExceptionCheck()) return false;
    if (retry) {
      const char *chars = env->GetStringUTFChars(retry, nullptr);
      if (env->ExceptionCheck() || !chars) return false;
      result.retryAfter = chars;
      env->ReleaseStringUTFChars(retry, chars);
    }

    input = env->CallObjectMethod(connection, result.status >= 400 ? getErrorStream : getInputStream);
    if (env->ExceptionCheck()) return false;
    if (!input) return true; // HTTP errors can legitimately have no response body.
    jobject output = env->NewObject(outputClass, outputCtor);
    if (env->ExceptionCheck() || !output) return false;
    jbyteArray buffer = env->NewByteArray(8192);
    if (env->ExceptionCheck() || !buffer) return false;

    constexpr std::size_t maxResponseBytes = 8 * 1024 * 1024;
    std::size_t total = 0;
    for (;;) {
      jint count = env->CallIntMethod(input, read, buffer);
      if (env->ExceptionCheck()) return false;
      if (count <= 0) break;
      total += static_cast<std::size_t>(count);
      if (total > maxResponseBytes) return false;
      env->CallVoidMethod(output, write, buffer, 0, count);
      if (env->ExceptionCheck()) return false;
    }
    jbyteArray bytes = static_cast<jbyteArray>(env->CallObjectMethod(output, toByteArray));
    if (env->ExceptionCheck() || !bytes) return false;
    jsize length = env->GetArrayLength(bytes);
    if (length > 0) {
      jbyte *data = env->GetByteArrayElements(bytes, nullptr);
      if (env->ExceptionCheck() || !data) return false;
      result.body.assign(reinterpret_cast<const char *>(data), static_cast<std::size_t>(length));
      env->ReleaseByteArrayElements(bytes, data, JNI_ABORT);
    }
    return true;
  }();

  if (env->ExceptionCheck()) env->ExceptionClear();
  if (!success) result = {};
  if (input && close) {
    env->CallVoidMethod(input, close);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  if (connection && disconnect) {
    env->CallVoidMethod(connection, disconnect);
    if (env->ExceptionCheck()) env->ExceptionClear();
  }
  // The scope pops the local frame; the thread's attachment outlives it.
  return result;
}

} // namespace pl::runtime

namespace pl::platform {

bool setClipboardText(std::string_view text) {
  return pl::runtime::SetClipboardTextImpl(text);
}

HttpResponse httpGet(std::string_view url, int timeoutMs) {
  return pl::runtime::HttpGetImpl(url, timeoutMs);
}

}
