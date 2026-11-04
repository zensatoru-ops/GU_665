#!/usr/bin/env bash
# native/build_native.sh for Linux: libmain.so (the runtime), libmhguffmpeg.so, SDL3, the driver hooks and libc++_shared.so
# into android/app/src/main/jniLibs/arm64-v8a. libmhgugame.so (the game's code) is not built: it is made from the
# player's own game files with tools/build_game.py, and the public APK leaves it out.
# Usage: ci/build_native.sh [Release|RelWithDebInfo]       MHGU_DEV=1: a test build (files/env.txt, files/control.txt)
source "$(dirname "$0")/env.sh"
TYPE=${1:-Release}
OUT="$ROOT/native/build/$ABI"
[ -f "$DEPS_OUT/shaderc/.built" ] && [ -f "$DEPS_OUT/sdl3/.built" ] && [ -f "$DEPS_OUT/ffmpeg/.built" ] \
  || { echo "dependencies missing: run ci/fetch_deps.sh and ci/build_deps.sh" >&2; exit 1; }

# the same ids as native/build_native.sh: both sides of the runtime <-> game code interface must agree
GENFLAGS="android-$ABI -O2 -nossp -fno-strict-aliasing -ffp-contract=off -fno-math-errno"
cd "$ROOT"
GAMELIB_ID=$( (cat tools/gen.py tools/lifter.py tools/neon.py tools/nso.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
               runtime/cpu.h runtime/vfp.h runtime/neon_rt.h runtime/gamelib.h | tr -d '\r'; echo "$GENFLAGS") | sha256sum | cut -c1-16)
SHADER_ID=$(cat runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h | tr -d '\r' | sha256sum | cut -c1-16)
VERSION="$(cat "$ROOT/VERSION" 2>/dev/null || echo dev)"
COMMITTED_ID=$(cat android/app/src/main/assets/gamelib_id.txt 2>/dev/null || true)
echo "GAMELIB_ID $GAMELIB_ID (committed: ${COMMITTED_ID:-none}), SHADER_ID $SHADER_ID"
if [ -n "$COMMITTED_ID" ] && [ "$COMMITTED_ID" != "$GAMELIB_ID" ] && [ -z "${ALLOW_GAMELIB_ID_CHANGE:-}" ]; then
  echo "GAMELIB_ID changed: everyone's libmhgugame.so would have to be made again (CLAUDE.md, Building). If that is meant, set ALLOW_GAMELIB_ID_CHANGE=1." >&2
  exit 1
fi

mkdir -p "$OUT"
cmake -S "$ROOT/native" -B "$OUT" -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN_FILE" -DANDROID_ABI=$ABI \
  -DANDROID_PLATFORM=android-$NATIVE_API -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE="$TYPE" "${CMAKE_COMPAT[@]}" \
  -DMHGU_SRC="$ROOT" -DDEPS="$DEPS_OUT" -DGEN="$ROOT/generated" \
  -DGAMELIB_ID="$GAMELIB_ID" -DSHADER_ID="$SHADER_ID" -DMHGU_VERSION="$VERSION" -DMHGU_DEV="${MHGU_DEV:-OFF}" \
  > "$OUT.configure.log" 2>&1 || { tail -40 "$OUT.configure.log"; exit 1; }
cmake --build "$OUT" --parallel "$JOBS" > "$OUT.build.log" 2>&1 || { grep -E "error|FAILED" "$OUT.build.log" | head -40; tail -20 "$OUT.build.log"; exit 1; }

J="$ROOT/android/app/src/main/jniLibs/$ABI"
mkdir -p "$J" "$ROOT/android/app/src/main/assets"
printf %s "$GAMELIB_ID" > "$ROOT/android/app/src/main/assets/gamelib_id.txt"  # (GameFolderActivity: which game code fits)
cp "$OUT/libmain.so" "$OUT/libmhguffmpeg.so" "$DEPS_OUT/sdl3/lib/libSDL3.so" "$J/"
cp "$TCB/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$J/"
# (a driver of the player's own: runtime/platform/android_gpu.c; found in the app's nativeLibraryDir)
cp "$OUT"/adrenotools/src/hook/lib*_hook.so "$OUT/adrenotools/src/hook/libhook_impl.so" "$J/"
echo "native $ABI: ok (GAMELIB_ID $GAMELIB_ID)"
ls -la "$J"
