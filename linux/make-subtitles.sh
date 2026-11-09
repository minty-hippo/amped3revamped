#!/usr/bin/env bash
# Makes subtitles for the game's dialogue on this machine from your own copy
# of the game: the Linux counterpart of installer/Make-Subtitles.ps1 (see
# there). Downloads whisper.cpp's speech recognition model once and runs
# amped3-subtitles, which writes <install>/subtitles/amped3.subtitles.
#
#   linux/make-subtitles.sh --install-dir <folder with amped3 and game/>
#       [--model base.en|small.en] [--threads N] [--force]
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR=''; MODEL=base.en; THREADS=0; FORCE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --install-dir) INSTALL_DIR="$2"; shift 2 ;;
    --model) MODEL="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --force) FORCE=1; shift ;;
    *) sed -n '2,8p' "$0"; exit 2 ;;
  esac
done
[ -n "$INSTALL_DIR" ] || { echo 'ERROR: --install-dir is required'; exit 2; }
GAME="$INSTALL_DIR/game"
[ -f "$GAME/default.xex" ] || { echo "ERROR: no game in $GAME"; exit 1; }

TOOL=''
for p in "$INSTALL_DIR/amped3-subtitles" "$PROJECT_DIR"/out/build/linux-*-release/amped3-subtitles \
         "$PROJECT_DIR"/out/build/linux-*-relwithdebinfo/amped3-subtitles; do
  [ -x "$p" ] && { TOOL="$p"; break; }
done
[ -n "$TOOL" ] || { echo 'ERROR: amped3-subtitles not found: build the project first (setup.sh / install.sh).'; exit 1; }

# whisper.cpp's models (MIT), with the SHA-1 hashes its models/README.md lists.
case "$MODEL" in
  base.en) FILE=ggml-base.en.bin; SIZE='142 MB'; SHA1=137c40403d78fd54d454da0f9bd998f78703390c ;;
  small.en) FILE=ggml-small.en.bin; SIZE='466 MB'; SHA1=db8a495a91d927739e50b3fc1cc4c6b8f6c2d022 ;;
  *) echo "ERROR: unknown model $MODEL (base.en or small.en)"; exit 2 ;;
esac
DIR="$PROJECT_DIR/out/models"; MODEL_FILE="$DIR/$FILE"
if [ ! -f "$MODEL_FILE" ] || [ "$(sha1sum "$MODEL_FILE" | cut -c1-40)" != "$SHA1" ]; then
  mkdir -p "$DIR"
  echo "    Downloading the speech recognition model ($FILE, $SIZE)"
  curl -fL --retry 3 -o "$MODEL_FILE.part" "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$FILE"
  [ "$(sha1sum "$MODEL_FILE.part" | cut -c1-40)" = "$SHA1" ] || { rm -f "$MODEL_FILE.part"; echo 'ERROR: the model download has unexpected contents.'; exit 1; }
  mv -f "$MODEL_FILE.part" "$MODEL_FILE"
fi

OUT="$INSTALL_DIR/subtitles"
mkdir -p "$OUT"
args=(--game "$GAME" --out "$OUT" --model "$MODEL_FILE")
[ "$THREADS" -gt 0 ] && args+=(--threads "$THREADS")
[ "$FORCE" -eq 1 ] && args+=(--force)
"$TOOL" "${args[@]}" || { echo "ERROR: making subtitles failed. What was found: $OUT/scan.txt"; exit 1; }
echo "    Subtitles are in $OUT/amped3.subtitles (in game: Options > Audio > Subtitles)"
