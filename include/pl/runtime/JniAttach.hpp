#pragma once

/**
 * @file JniAttach.hpp
 * @brief The rule for using a `JNIEnv` from a thread that may not be attached to the JVM.
 *
 * <p>Every native entry point that calls back into Java can be reached from a thread that is not
 * attached: the render thread, the game's own tick thread, a cosmetic-sync receive loop, a mod's
 * worker. Calling a JNI function with an unattached thread is undefined behaviour, and the previous
 * hand-rolled version of this logic in each function attached, called, then detached only on the
 * happy path — an exception raised inside the call skipped the detach and leaked the attachment and
 * every local reference it held. Repeated calls then exhausted the local reference table and the
 * module silently stopped working.
 *
 * <p>The decision itself is pure (this header, no JNI types), so the invariants are host-tested:
 * a thread we attached is a thread we must detach; a thread that was already attached must not be
 * detached by us; a thread that cannot be attached must be reported as a failure rather than used
 * unattached.
 */

namespace pl::jni {

/** What to do with the current thread's JVM attachment. */
enum class AttachAction {
    /** The thread is already attached: use the env, do not detach it. */
    UseExisting,
    /** The thread was detached and we attached it: use the env, and detach it when done. */
    AttachAndDetach,
    /** The thread could not be attached (or the VM is gone): do not use the env. */
    Unavailable,
};

/**
 * @brief Maps a `JavaVM::GetEnv` result to the action to take.
 *
 * @param getEnvStatus the value `GetEnv` returned (`JNI_OK`, `JNI_EDETACHED`, `JNI_EVERSION`, …)
 * @param haveVm true when a `JavaVM*` was captured at load time
 * @return the action to take
 */
constexpr AttachAction DecideAttach(int getEnvStatus, bool haveVm) {
    // JNI_OK == 0, JNI_EDETACHED == -2, JNI_EVERSION == -3 (the JNI spec's fixed values).
    if (!haveVm) return AttachAction::Unavailable;
    if (getEnvStatus == 0) return AttachAction::UseExisting;
    if (getEnvStatus == -2) return AttachAction::AttachAndDetach;
    return AttachAction::Unavailable;
}

/** True when the action means the caller owns the attachment and must detach it. */
constexpr bool OwnsAttachment(AttachAction action) {
    return action == AttachAction::AttachAndDetach;
}

/** True when the env may be used at all. */
constexpr bool CanUseEnv(AttachAction action) {
    return action != AttachAction::Unavailable;
}

/**
 * @brief The lifetime rule for one thread's JVM attachment.
 *
 * <p>The decision above says what to do on a *call*. This says what to remember between calls: the
 * attachment is resolved once per thread and kept for the thread's whole life, so a thread that
 * calls back into Java repeatedly attaches once instead of once per call, and the detach happens
 * when the thread exits rather than at the end of every call.
 *
 * <p>Two invariants are easy to get wrong and are pinned by {@code JniAttachTest}: a thread we
 * attached must be detached exactly once (at thread exit), and a thread that was already attached
 * when we first saw it — the UI thread — must never be detached by us.
 */
class ThreadAttachmentLifetime {
public:
    /**
     * @brief Records the outcome of the first resolution on this thread.
     *
     * @param action what {@link DecideAttach} returned for this thread
     * @return true when the caller must perform the attach (once), false otherwise
     */
    bool resolve(AttachAction action) {
        mResolved = true;
        mAttachedByUs = OwnsAttachment(action);
        mUsable = CanUseEnv(action);
        return mAttachedByUs;
    }

    /** True once {@link #resolve} has run for this thread; later calls must reuse the answer. */
    bool resolved() const { return mResolved; }

    /** True when the env may be used on this thread. */
    bool usable() const { return mUsable; }

    /** True when the thread exits must detach: only when we attached it. */
    bool mustDetachOnThreadExit() const { return mAttachedByUs; }

private:
    bool mResolved = false;
    bool mAttachedByUs = false;
    bool mUsable = false;
};

} // namespace pl::jni
