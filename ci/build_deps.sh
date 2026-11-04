#!/usr/bin/env bash
# deps/build_deps.sh and deps/build_ffmpeg.sh for Linux: SDL3 (shared), shaderc (static) and FFmpeg (static), arm64-v8a,
# into deps/out/arm64-v8a (what native/CMakeLists.txt expects). Needs ci/fetch_deps.sh first.
source "$(dirname "$0")/env.sh"
mkdir -p "$DEPS_OUT"
COMMON=(-G Ninja "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN_FILE" -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$DEPS_API
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON "${CMAKE_COMPAT[@]}")
what=${1:-all}

if [[ $what == all || $what == sdl ]] && [ ! -f "$DEPS_OUT/sdl3/.built" ]; then
  echo "== SDL3"
  cmake -S "$DEPS_SRC/SDL3-3.4.18" -B "$DEPS_OUT/sdl3/build" "${COMMON[@]}" \
    -DCMAKE_INSTALL_PREFIX="$DEPS_OUT/sdl3" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384 > "$DEPS_OUT/sdl3.configure.log" 2>&1 \
    || { tail -30 "$DEPS_OUT/sdl3.configure.log"; exit 1; }
  cmake --build "$DEPS_OUT/sdl3/build" --target install --parallel "$JOBS" > "$DEPS_OUT/sdl3.build.log" 2>&1 \
    || { tail -30 "$DEPS_OUT/sdl3.build.log"; exit 1; }
  touch "$DEPS_OUT/sdl3/.built"
fi

if [[ $what == all || $what == shaderc ]] && [ ! -f "$DEPS_OUT/shaderc/.built" ]; then
  echo "== shaderc"
  cmake -S "$DEPS_SRC/shaderc" -B "$DEPS_OUT/shaderc/build" "${COMMON[@]}" \
    -DANDROID_STL=c++_static -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
    -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON \
    -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF > "$DEPS_OUT/shaderc.configure.log" 2>&1 \
    || { tail -30 "$DEPS_OUT/shaderc.configure.log"; exit 1; }
  cmake --build "$DEPS_OUT/shaderc/build" --target shaderc_combined --parallel "$JOBS" > "$DEPS_OUT/shaderc.build.log" 2>&1 \
    || { tail -30 "$DEPS_OUT/shaderc.build.log"; exit 1; }
  mkdir -p "$DEPS_OUT/shaderc/lib" "$DEPS_OUT/shaderc/include"
  cp "$DEPS_OUT/shaderc/build/libshaderc/libshaderc_combined.a" "$DEPS_OUT/shaderc/lib/"
  cp -r "$DEPS_SRC/shaderc/libshaderc/include/shaderc" "$DEPS_OUT/shaderc/include/"
  touch "$DEPS_OUT/shaderc/.built"
fi

if [[ $what == all || $what == ffmpeg ]] && [ ! -f "$DEPS_OUT/ffmpeg/.built" ]; then
  echo "== FFmpeg (MP4 + H.264 + AAC only, as deps/build_ffmpeg.sh)"
  FF_OUT="$DEPS_OUT/ffmpeg"; FF_BUILD="$DEPS_OUT/ffmpeg-build"
  rm -rf "$FF_BUILD"; mkdir -p "$FF_BUILD" "$FF_OUT"
  (cd "$FF_BUILD" && "$DEPS_SRC/ffmpeg/configure" --prefix="$FF_OUT" \
    --enable-cross-compile --target-os=android --arch=aarch64 \
    --cc="$TCB/bin/aarch64-linux-android$DEPS_API-clang" --cxx="$TCB/bin/aarch64-linux-android$DEPS_API-clang++" \
    --ar="$TCB/bin/llvm-ar" --nm="$TCB/bin/llvm-nm" --ranlib="$TCB/bin/llvm-ranlib" --strip="$TCB/bin/llvm-strip" \
    --sysroot="$TCB/sysroot" --enable-pic \
    --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-debug --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
    --enable-static --disable-shared --enable-protocol=file --enable-demuxer=mov \
    --enable-parser=h264,aac --enable-decoder=h264,aac \
    "--extra-cflags=-ffile-prefix-map=$DEPS_SRC/ffmpeg=ffmpeg" > configure.log 2>&1) \
    || { tail -30 "$FF_BUILD/configure.log"; tail -40 "$FF_BUILD/ffbuild/config.log"; exit 1; }
  sed -i 's|^#define FFMPEG_CONFIGURATION .*|#define FFMPEG_CONFIGURATION ""|' "$FF_BUILD/config.h"  # (no paths of this machine in the library)
  (cd "$FF_BUILD" && make -j"$JOBS" install > build.log 2>&1) || { tail -40 "$FF_BUILD/build.log"; exit 1; }
  touch "$FF_OUT/.built"
fi
echo "deps $ABI: ok"
