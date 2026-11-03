#pragma once

#include <jni.h>
#include <string>
#include <string_view>
#include "pl/Platform.hpp"
#include "pl/runtime/JniAttach.hpp"

namespace pl::runtime {

void SetJavaVm(JavaVM *vm);
JavaVM *GetJavaVm();

void SetActivity(JNIEnv *env, jobject activity);
void ClearActivity(JNIEnv *env);
void CallActivityVoidMethod(const char *methodName);
bool CallActivityStringMethod(const char *methodName, const std::string &value);
pl::platform::HttpResponse HttpGetImpl(std::string_view url, int timeoutMs);

/**
 * @brief A scoped JVM attachment: acquires a `JNIEnv` for the current thread and releases it on
 *        destruction.
 *
 * <p>Use this in every native function that calls back into Java. The attachment itself is held per
 * native thread for that thread's whole life: the first call on a detached thread
 * attaches it, and a {@code thread_local} destructor detaches it when the thread exits. A thread
 * that was already attached (the UI thread) is never detached by us. The scope's job is therefore
 * only to bound the local reference table with {@code PushLocalFrame} — an early return or an
 * exception in the middle of the call can no longer leak references and silently disable the
 * module, and a repeated call no longer pays an Attach/Detach pair.
 *
 * <p>{@link #valid()} is false when no `JavaVM` was captured or the thread could not be attached;
 * callers must check it and fail closed rather than using an unattached env.
 */
class ScopedJniEnv {
public:
    ScopedJniEnv();
    ~ScopedJniEnv();

    ScopedJniEnv(const ScopedJniEnv &) = delete;
    ScopedJniEnv &operator=(const ScopedJniEnv &) = delete;

    /** The env, or nullptr when the thread could not be attached. */
    JNIEnv *env() const { return env_; }

    /** True when the env is usable. */
    bool valid() const { return env_ != nullptr; }

    /**
     * @brief Pops the local frame if one was pushed. Never detaches the thread.
     *        Safe to call more than once; also runs from the destructor.
     */
    void release();

private:
    JNIEnv *env_ = nullptr;
    bool framePushed_ = false;
    bool released_ = false;
};

} // namespace pl::runtime
