#!/usr/bin/env bash
# AMD FSR 3.1 for the Linux build: libamd_fidelityfx_vk.so, built from the
# FidelityFX SDK v1.1.4 sources (AMD ships that runtime for Windows only).
# setup.sh runs this after building the game; it can also run on its own:
#
#   linux/fidelityfx/build.sh --dest <folder for the .so> --glslang <glslang sources>
#                             [--jobs N] [--force]
#
# Takes only the needed folders of AMD's repository (a sparse clone, about
# 25 MB), applies fidelityfx-sdk-linux.patch, then builds with the CMake
# project here (CMakeLists.txt says what it does). Skips everything when the
# library in --dest was built from the same files.
set -euo pipefail

FFX_REPO='https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git'
FFX_TAG='v1.1.4'

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$HERE/../.." && pwd)"
DEST=''; GLSLANG=''; JOBS=''; FORCE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --dest) DEST="$2"; shift 2 ;;
    --glslang) GLSLANG="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --force) FORCE=1; shift ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done
[ -n "$DEST" ] || { echo '--dest is required' >&2; exit 2; }
[ -f "$GLSLANG/CMakeLists.txt" ] || { echo "No glslang sources at '$GLSLANG'" >&2; exit 2; }
mkdir -p "$DEST"

case "$(uname -m)" in x86_64|amd64) ARCH=amd64 ;; aarch64|arm64) ARCH=arm64 ;; *) ARCH="$(uname -m)" ;; esac
LIB="$DEST/libamd_fidelityfx_vk.so"
STAMP_FILE="$DEST/libamd_fidelityfx_vk.stamp"
STAMP="$FFX_TAG $(cat "$HERE"/CMakeLists.txt "$HERE"/*.h "$HERE"/*.cpp "$HERE"/*.patch | sha256sum | cut -c1-16)"
if [ "$FORCE" -eq 0 ] && [ -f "$LIB" ] && [ "$(cat "$STAMP_FILE" 2>/dev/null)" = "$STAMP" ]; then
  echo "FSR 3.1 library up to date ($LIB)"
  exit 0
fi

SRC="$PROJECT_DIR/third_party/fidelityfx-sdk"
if [ ! -d "$SRC/.git" ]; then
  echo "Getting the FidelityFX SDK $FFX_TAG sources (sparse clone)"
  rm -rf "$SRC"
  git clone --quiet --depth 1 --branch "$FFX_TAG" --filter=blob:none --sparse "$FFX_REPO" "$SRC"
  git -C "$SRC" sparse-checkout set --no-cone \
    /sdk/LICENSE.txt /sdk/include/ /sdk/src/ /ffx-api/include/ /ffx-api/src/ \
    /sdk/tools/ffx_shader_compiler/src/ \
    /sdk/tools/ffx_shader_compiler/libs/MD5/ \
    /sdk/tools/ffx_shader_compiler/libs/SPIRV-Reflect/ \
    /sdk/tools/ffx_shader_compiler/libs/tiny-process-library/
fi
# Back to AMD's files, then the Linux changes.
git -C "$SRC" checkout --quiet -- .
git -C "$SRC" apply --whitespace=nowarn "$HERE/fidelityfx-sdk-linux.patch"

BUILD="$PROJECT_DIR/out/build/fidelityfx-$ARCH"
CMAKE_ARGS=(-S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
            -DFFX_SDK_DIR="$SRC" -DGLSLANG_SOURCE_DIR="$GLSLANG"
            -DVULKAN_INCLUDE_DIR="$PROJECT_DIR/third_party/vulkan")
[ -n "${CC:-}" ] && CMAKE_ARGS+=(-DCMAKE_C_COMPILER="$CC")
[ -n "${CXX:-}" ] && CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER="$CXX")
cmake "${CMAKE_ARGS[@]}"
if [ -n "$JOBS" ]; then
  cmake --build "$BUILD" --target amd_fidelityfx_vk -j "$JOBS"
else
  cmake --build "$BUILD" --target amd_fidelityfx_vk
fi
# The game may be running from $DEST (the launcher's Update): replace the file.
cp --remove-destination "$BUILD/libamd_fidelityfx_vk.so" "$LIB"
cp "$SRC/sdk/LICENSE.txt" "$DEST/FidelityFX-LICENSE.txt"
printf '%s\n' "$STAMP" > "$STAMP_FILE"
echo "Built $LIB"
