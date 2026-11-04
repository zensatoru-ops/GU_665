# Shared by ci/*.sh (Linux): where the NDK is and which tools build for Android arm64. Source it, do not run it.
# The repository's own deps/*.sh and native/build_native.sh are written for Git Bash on Windows (cygpath, LOCALAPPDATA,
# the NDK's windows-x86_64 tools): these are their Linux counterparts, with the same versions, flags and outputs.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ABI=arm64-v8a
DEPS_API=29      # deps/build_deps.sh, deps/build_ffmpeg.sh
NATIVE_API=28    # native/build_native.sh (the app's minSdk)
NDK_VERSION=27.0.12077973

# NDK: ANDROID_NDK_HOME / ANDROID_NDK_ROOT, else the SDK's ndk/<version>
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [ -z "$NDK" ] && [ -n "${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}" ]; then
  NDK="${ANDROID_HOME:-$ANDROID_SDK_ROOT}/ndk/$NDK_VERSION"
fi
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo "NDK $NDK_VERSION not found (set ANDROID_NDK_HOME)" >&2; exit 1; }
TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
TCB="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
JOBS=${JOBS:-$(nproc)}

DEPS_SRC="$ROOT/deps/src"
DEPS_OUT="$ROOT/deps/out/$ABI"

# (newer CMake refuses the old minimum versions some dependencies still declare)
CMAKE_COMPAT=(-DCMAKE_POLICY_VERSION_MINIMUM=3.5)
