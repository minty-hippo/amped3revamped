#!/usr/bin/env bash
# Makes subtitles for the game's dialogue on this machine from your own copy
# of the game: the Linux counterpart of installer/Make-Subtitles.ps1 (see
# there). Downloads whisper.cpp's speech recognition model once and runs
# amped3-subtitles, which writes <install>/subtitles/amped3.subtitles.
#
#   linux/make-subtitles.sh --install-dir <folder with amped3 and game/>
#       [--language English|French|German] [--model base|small] [--threads N] [--force]
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR=''; MODEL=base; LANGUAGE=English; THREADS=0; FORCE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --install-dir) INSTALL_DIR="$2"; shift 2 ;;
    --model) MODEL="${2%.en}"; shift 2 ;;
    --language) LANGUAGE="$2"; shift 2 ;;
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
case "$LANGUAGE" in English|French|German) ;; *) echo "ERROR: unknown language $LANGUAGE"; exit 2 ;; esac
# English: the English-only models (better for it).
[ "$LANGUAGE" = English ] && MODEL="$MODEL.en"
case "$MODEL" in
  base.en) FILE=ggml-base.en.bin; SIZE='142 MB'; SHA1=137c40403d78fd54d454da0f9bd998f78703390c ;;
  small.en) FILE=ggml-small.en.bin; SIZE='466 MB'; SHA1=db8a495a91d927739e50b3fc1cc4c6b8f6c2d022 ;;
  base) FILE=ggml-base.bin; SIZE='142 MB'; SHA1=465707469ff3a37a2b9b8d8f89f2f99de7299dac ;;
  small) FILE=ggml-small.bin; SIZE='466 MB'; SHA1=55356645c2b361a969dfd0ef2c5a50d530afd8d5 ;;
  *) echo "ERROR: unknown model $MODEL (base or small)"; exit 2 ;;
esac
DIR="$PROJECT_DIR/out/models"; MODEL_FILE="$DIR/$FILE"
if [ ! -f "$MODEL_FILE" ] || [ "$(sha1sum "$MODEL_FILE" | cut -c1-40)" != "$SHA1" ]; then
  mkdir -p "$DIR"
  echo "    Downloading the speech recognition model ($FILE, $SIZE)"
  curl -fL --retry 3 -o "$MODEL_FILE.part" "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$FILE"
  [ "$(sha1sum "$MODEL_FILE.part" | cut -c1-40)" = "$SHA1" ] || { rm -f "$MODEL_FILE.part"; echo 'ERROR: the model download has unexpected contents.'; exit 1; }
  mv -f "$MODEL_FILE.part" "$MODEL_FILE"
fi

# Who says what (speaker colours): Resemblyzer's voice encoder (Apache 2.0),
# 17 MB, at a pinned commit, checked against its hash. Optional.
SPK="$DIR/resemblyzer-pretrained.pt"
SPK_SHA=39373b86598fa3da9fcddee6142382efe09777e8d37dc9c0561f41f0070f134e
if [ ! -f "$SPK" ] || [ "$(sha256sum "$SPK" | cut -c1-64)" != "$SPK_SHA" ]; then
  echo '    Downloading the voice model for speaker colours (Resemblyzer, 17 MB)'
  if curl -fL --retry 3 -o "$SPK.part" \
       "https://raw.githubusercontent.com/resemble-ai/Resemblyzer/15d828edebe06bc72b9cabc8ef8ca5ab2cb457ce/resemblyzer/pretrained.pt" &&
     [ "$(sha256sum "$SPK.part" | cut -c1-64)" = "$SPK_SHA" ]; then
    mv -f "$SPK.part" "$SPK"
  else
    rm -f "$SPK.part"
    echo "    Couldn't get it; subtitles without speaker colours."
  fi
fi

OUT="$INSTALL_DIR/subtitles"
mkdir -p "$OUT"
args=(--game "$GAME" --out "$OUT" --model "$MODEL_FILE" --language "$LANGUAGE")
[ -f "$SPK" ] && args+=(--speaker-model "$SPK")
[ "$THREADS" -gt 0 ] && args+=(--threads "$THREADS")
[ "$FORCE" -eq 1 ] && args+=(--force)
"$TOOL" "${args[@]}" || { echo "ERROR: making subtitles failed. What was found: $OUT/scan.txt"; exit 1; }
echo "    Subtitles are in $OUT/amped3.subtitles (in game: Options > Audio > Subtitles)"
