#!/usr/bin/env bash
# Downloads the dependencies' sources into deps/src (the versions CLAUDE.md lists). Nothing of the game: only SDL3,
# FFmpeg and shaderc (with glslang, SPIRV-Tools and SPIRV-Headers), all from their upstream projects.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/deps/src"
SDL_VERSION=3.4.18
FFMPEG_VERSION=9.0.2
SHADERC_VERSION=2026.4
GLSLANG_VERSION=16.6.0
VULKAN_SDK_TAG=vulkan-sdk-1.4.363.0   # SPIRV-Tools and SPIRV-Headers 1.4.363.0

fetch() {  # <name> <url> <folder under deps/src>
  local name=$1 url=$2 dest="$SRC/$3"
  if [ -f "$dest/.fetched" ]; then echo "$name: already there"; return; fi
  echo "== $name"
  rm -rf "$dest"; mkdir -p "$dest"
  curl -fsSL --retry 4 --retry-delay 5 "$url" | tar xz -C "$dest" --strip-components=1
  touch "$dest/.fetched"
}

mkdir -p "$SRC"
fetch SDL3      "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz" "SDL3-$SDL_VERSION"
fetch FFmpeg    "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n$FFMPEG_VERSION.tar.gz" ffmpeg
fetch shaderc   "https://github.com/google/shaderc/archive/refs/tags/v$SHADERC_VERSION.tar.gz" shaderc
fetch glslang   "https://github.com/KhronosGroup/glslang/archive/refs/tags/$GLSLANG_VERSION.tar.gz" shaderc/third_party/glslang
fetch SPIRV-Tools   "https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/$VULKAN_SDK_TAG.tar.gz" shaderc/third_party/spirv-tools
fetch SPIRV-Headers "https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/$VULKAN_SDK_TAG.tar.gz" shaderc/third_party/spirv-headers
echo "deps sources: ok"
