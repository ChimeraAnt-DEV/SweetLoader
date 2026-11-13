#!/usr/bin/env bash
# Cross-compiles the Rust JNI cdylibs for the Android arm64 target.
#
# The client ships a single arm64-v8a APK, so this is the only target built. The linker and the
# `cc` crate's compiler are pointed at the NDK's clang wrapper; the API level matches the client's
# minSdk.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-/tmp/android-ndk-r27c}}"
API="${ANDROID_API_LEVEL:-24}"

toolchain="$(ls -d "$NDK"/toolchains/llvm/prebuilt/* 2>/dev/null | head -1)"
if [ -z "$toolchain" ]; then
  echo "could not find the NDK toolchain under $NDK" >&2
  exit 1
fi

export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$toolchain/bin/aarch64-linux-android${API}-clang"
export CC_aarch64_linux_android="$toolchain/bin/aarch64-linux-android${API}-clang"
export AR_aarch64_linux_android="$toolchain/bin/llvm-ar"

packages=("$@")
if [ "${#packages[@]}" -eq 0 ]; then
  packages=(universe-jni singularity-jni)
fi

cd "$here" || exit 1
for package in "${packages[@]}"; do
  echo "building $package for aarch64-linux-android"
  cargo build -p "$package" --target aarch64-linux-android --release || exit 1
done
echo "Android cdylibs built into target/aarch64-linux-android/release"
