#!/usr/bin/env bash
# Upscales the game's textures 4x with Real-ESRGAN (ncnn / Vulkan) for the
# native renderer: the Linux counterpart of installer/Upscale-Textures.ps1
# (see there). Real-ESRGAN's official Linux build is x86_64 only; on ARM pass
# --esrgan <realesrgan-ncnn-vulkan built for aarch64>.
#
#   linux/upscale-textures.sh --install-dir <folder with amped3 and game/>
#       [--set ui|world|all] [--model realesrgan-x4plus-anime|realesrgan-x4plus]
#       [--tile N] [--force] [--esrgan <executable>]
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR=''; SET=ui; MODEL=realesrgan-x4plus-anime; TILE=0; FORCE=0; ESRGAN=''
while [ $# -gt 0 ]; do
  case "$1" in
    --install-dir) INSTALL_DIR="$2"; shift 2 ;;
    --set) SET="$2"; shift 2 ;;
    --model) MODEL="$2"; shift 2 ;;
    --tile) TILE="$2"; shift 2 ;;
    --force) FORCE=1; shift ;;
    --esrgan) ESRGAN="$2"; shift 2 ;;
    *) sed -n '2,10p' "$0"; exit 2 ;;
  esac
done
[ -n "$INSTALL_DIR" ] || { echo 'ERROR: --install-dir is required'; exit 2; }
GAME="$INSTALL_DIR/game"
[ -f "$GAME/default.xex" ] || { echo "ERROR: no game in $GAME"; exit 1; }

TOOL=''
for p in "$INSTALL_DIR/amped3-texture-upscale" "$PROJECT_DIR"/out/build/linux-*-release/amped3-texture-upscale \
         "$PROJECT_DIR"/out/build/linux-*-relwithdebinfo/amped3-texture-upscale; do
  [ -x "$p" ] && { TOOL="$p"; break; }
done
[ -n "$TOOL" ] || { echo 'ERROR: amped3-texture-upscale not found: build the project first (setup.sh / install.sh).'; exit 1; }

if [ -z "$ESRGAN" ]; then
  if [ "$(uname -m)" != x86_64 ]; then
    echo "Real-ESRGAN's official Linux build is x86_64 only. On $(uname -m), build realesrgan-ncnn-vulkan"
    echo 'from https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan and pass --esrgan <path>.'
    exit 1
  fi
  ZIP_URL='https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip'
  ZIP_HASH='e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96'
  DL="$PROJECT_DIR/out/downloads"; DIR="$PROJECT_DIR/out/tools/realesrgan"
  ESRGAN="$DIR/realesrgan-ncnn-vulkan"
  if [ ! -x "$ESRGAN" ]; then
    mkdir -p "$DL"
    ZIP="$DL/$(basename "$ZIP_URL")"
    if [ ! -f "$ZIP" ] || [ "$(sha256sum "$ZIP" | cut -c1-64)" != "$ZIP_HASH" ]; then
      echo '    Downloading Real-ESRGAN (45 MB)'
      curl -fL --retry 3 -o "$ZIP" "$ZIP_URL"
      [ "$(sha256sum "$ZIP" | cut -c1-64)" = "$ZIP_HASH" ] || { rm -f "$ZIP"; echo 'ERROR: the Real-ESRGAN download has unexpected contents.'; exit 1; }
    fi
    rm -rf "$DIR"; mkdir -p "$DIR"
    python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$ZIP" "$DIR"
    chmod +x "$ESRGAN"
  fi
fi

mkdir -p "$INSTALL_DIR/textures"
args=(--game "$GAME" --textures "$INSTALL_DIR/textures" --esrgan "$ESRGAN" --set "$SET" --model "$MODEL")
[ "$TILE" -gt 0 ] && args+=(--tile "$TILE")
[ "$FORCE" -eq 1 ] && args+=(--force)
"$TOOL" "${args[@]}"
echo "    Upscaled textures are in $INSTALL_DIR/textures/upscaled"
