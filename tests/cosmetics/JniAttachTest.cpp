/**
 * @file JniAttachTest.cpp
 * @brief Host test for the JVM attachment decision used by every native -> Java call.
 *
 * A thread we attach is a thread we must detach; a thread that was already attached must not be
 * detached by us; a thread that cannot be attached must be reported as unusable rather than used
 * unattached. The old per-function code got the first two wrong on an exception path, leaking the
 * attachment and its local references until the module stopped working.
 */

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "pl/runtime/JniAttach.hpp"

using pl::jni::AttachAction;
using pl::jni::CanUseEnv;
using pl::jni::DecideAttach;
using pl::jni::OwnsAttachment;

namespace {

int checks = 0;

void expect(bool condition, const char *what) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

} // namespace

int main() {
  // JNI_OK (0) with a VM: the thread is already attached, we must not detach it.
  expect(DecideAttach(0, true) == AttachAction::UseExisting, "JNI_OK -> use existing");
  expect(!OwnsAttachment(DecideAttach(0, true)), "already-attached thread is not ours to detach");
  expect(CanUseEnv(DecideAttach(0, true)), "already-attached env is usable");

  // JNI_EDETACHED (-2): we attach it, so we own the detach.
  expect(DecideAttach(-2, true) == AttachAction::AttachAndDetach, "JNI_EDETACHED -> attach and detach");
  expect(OwnsAttachment(DecideAttach(-2, true)), "a thread we attach is ours to detach");
  expect(CanUseEnv(DecideAttach(-2, true)), "an attachable env is usable");

  // No VM: unusable, no attach.
  expect(DecideAttach(0, false) == AttachAction::Unavailable, "no VM -> unavailable");
  expect(DecideAttach(-2, false) == AttachAction::Unavailable, "no VM -> unavailable even if detached");
  expect(!CanUseEnv(DecideAttach(-2, false)), "unavailable env must not be used");

  // A version mismatch or any other error is unusable, not "attach anyway".
  expect(DecideAttach(-3, true) == AttachAction::Unavailable, "JNI_EVERSION -> unavailable");
  expect(!CanUseEnv(DecideAttach(-3, true)), "a version error env must not be used");

  // --- The per-thread lifetime rule -------------------------------------------------
  // The attachment is resolved once per thread and kept; the detach happens at thread exit, and
  // only for a thread we attached.
  {
    pl::jni::ThreadAttachmentLifetime lifetime;
    expect(!lifetime.resolved(), "a fresh thread is unresolved");
    // A detached thread: we attach it, so it must be detached at thread exit.
    expect(lifetime.resolve(DecideAttach(-2, true)), "a detached thread must be attached by us");
    expect(lifetime.resolved(), "resolving marks the thread resolved");
    expect(lifetime.usable(), "an attached thread is usable");
    expect(lifetime.mustDetachOnThreadExit(), "a thread we attached is ours to detach at exit");
  }
  {
    pl::jni::ThreadAttachmentLifetime lifetime;
    // The UI thread is already attached: usable, but never detached by us.
    expect(!lifetime.resolve(DecideAttach(0, true)), "an already-attached thread is not attached again");
    expect(lifetime.usable(), "an already-attached thread is usable");
    expect(!lifetime.mustDetachOnThreadExit(),
           "a thread we did not attach must never be detached at exit");
  }
  {
    pl::jni::ThreadAttachmentLifetime lifetime;
    // No VM: unusable, and there is nothing to detach.
    expect(!lifetime.resolve(DecideAttach(-2, false)), "no VM means we do not attach");
    expect(!lifetime.usable(), "no VM means the env is unusable");
    expect(!lifetime.mustDetachOnThreadExit(), "no VM means nothing to detach");
  }

  std::printf("JniAttachTest: %d checks passed\n", checks);
  return 0;
}
