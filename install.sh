#!/usr/bin/env bash
# Installs Amped 3 Revamped on Linux from your own Amped 3 (Xbox 360) disc
# image: SteamOS (Steam Deck and other x86_64 PCs), Armada and other ARM
# handhelds (aarch64), or any desktop distribution. The Linux counterpart of
# "Install Amped 3 Revamped.bat".
#
#   ./install.sh [--iso <image.iso>] [--game-dir <extracted game>]
#                [--install-dir <folder>] [--config Release|RelWithDebInfo]
#                [--container | --native] [--upscale-textures yes|no] [--yes]
#
#  1. Extracts the game from the ISO into <install folder>/game and checks it.
#  2. Builds (setup.sh): natively when Clang 20+, CMake and Ninja are there,
#     otherwise inside a podman / docker container (SteamOS, Armada).
#  3. Puts the game together in the install folder (portable: move it
#     anywhere), with a launcher and a desktop entry to add to Steam.
#  4. Optionally upscales the game's textures (x86_64).
# The build takes a long time the first time (hours on a handheld) and needs
# about 40 GB free. Logs go to ./logs/.
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISO=''; GAME_SRC=''; INSTALL_DIR=''; CONFIG=Release; MODE=auto; UPSCALE=ask; YES=0

while [ $# -gt 0 ]; do
  case "$1" in
    --iso) ISO="$2"; shift 2 ;;
    --game-dir) GAME_SRC="$2"; shift 2 ;;
    --install-dir) INSTALL_DIR="$2"; shift 2 ;;
    --config) CONFIG="$2"; shift 2 ;;
    --container) MODE=container; shift ;;
    --native) MODE=native; shift ;;
    --upscale-textures) UPSCALE="$2"; shift 2 ;;
    --yes|-y) YES=1; shift ;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1"; exit 2 ;;
  esac
done

step() { printf '\n\033[36m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '    \033[33m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31mERROR: %s\033[0m\n' "$*"; exit 1; }
ask() {  # ask "question" default(y|n)
  local def="${2:-y}" answer
  if [ "$YES" -eq 1 ]; then [ "$def" = y ]; return; fi
  read -r -p "    $1 [$([ "$def" = y ] && echo Y/n || echo y/N)] " answer || true
  answer="${answer:-$def}"
  [[ "$answer" =~ ^[Yy] ]]
}

printf '\033[32mAmped 3 Revamped installer (Linux, %s)\033[0m\n' "$(uname -m)"
command -v python3 >/dev/null || fail 'python3 is needed (it comes with SteamOS and Fedora-based systems).'

# ---------------------------------------------------------------------------
if [ -z "$INSTALL_DIR" ]; then
  suggested="$HOME/Games/Amped 3 Revamped"
  if [ "$YES" -eq 1 ]; then INSTALL_DIR="$suggested"
  else read -r -p "    Install folder [$suggested] " INSTALL_DIR || true; INSTALL_DIR="${INSTALL_DIR:-$suggested}"; fi
fi
mkdir -p "$INSTALL_DIR"
INSTALL_DIR="$(cd "$INSTALL_DIR" && pwd)"
GAME="$INSTALL_DIR/game"
note "Install folder: $INSTALL_DIR"
for d in "$INSTALL_DIR" "$PROJECT_DIR"; do
  free_gb=$(( $(df -Pk "$d" | awk 'NR==2 {print $4}') / 1048576 ))
  note "$(df -P "$d" | awk 'NR==2 {print $6}'): $free_gb GB free"
  [ "$free_gb" -ge 40 ] || warn 'The build needs roughly 40 GB in total (game, SDK, compiled code); this may run out.'
done

# ---------------------------------------------------------------------------
if [ -n "$GAME_SRC" ]; then
  step 'Checking the game folder'
  python3 "$PROJECT_DIR/linux/xiso.py" check "$GAME_SRC" >/dev/null
  if [ "$(cd "$GAME_SRC" && pwd)" != "$GAME" ]; then
    note "Copying the game files to $GAME"
    mkdir -p "$GAME"; cp -a "$GAME_SRC"/. "$GAME"/
  fi
elif [ -f "$GAME/default.xex" ] && python3 "$PROJECT_DIR/linux/xiso.py" check "$GAME" >/dev/null 2>&1; then
  step 'Game files already extracted'
  note "$GAME"
else
  if [ -z "$ISO" ]; then
    [ "$YES" -eq 0 ] || fail 'Pass --iso <image> (or --game-dir).'
    read -r -p '    Your Amped 3 (Xbox 360) disc image (.iso): ' ISO
  fi
  [ -f "$ISO" ] || fail "Disc image not found: $ISO"
  step "Extracting the game to $GAME"
  python3 "$PROJECT_DIR/linux/xiso.py" extract "$ISO" "$GAME"
fi

# ---------------------------------------------------------------------------
step 'Building - the first build takes a long time'
have_tools() {
  local c
  for c in clang-22 clang-21 clang-20 clang; do
    command -v "$c" >/dev/null || continue
    local m; m="$("$c" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\).*/\1/p' | head -1)"
    [ -n "$m" ] && [ "$m" -ge 20 ] && command -v cmake >/dev/null && command -v ninja >/dev/null && command -v git >/dev/null && return 0
  done
  return 1
}
if [ "$MODE" = auto ]; then
  if have_tools; then MODE=native; else MODE=container; fi
fi
if [ "$MODE" = native ]; then
  note 'Using this system'"'"'s build tools'
  bash "$PROJECT_DIR/setup.sh" --game-dir "$GAME" --config "$CONFIG"
else
  note 'Building in a container (podman / docker): nothing is installed on the system'
  bash "$PROJECT_DIR/linux/container-build.sh" --game-dir "$GAME" --config "$CONFIG"
fi
case "$(uname -m)" in x86_64|amd64) ARCH=amd64 ;; *) ARCH=arm64 ;; esac
case "$CONFIG" in RelWithDebInfo) CFG=relwithdebinfo ;; Debug) CFG=debug ;; *) CFG=release ;; esac
BUILD_DIR="$PROJECT_DIR/out/build/linux-$ARCH-$CFG"
[ -x "$BUILD_DIR/amped3" ] || fail "The build finished but $BUILD_DIR/amped3 is missing."

# ---------------------------------------------------------------------------
step 'Putting the game together'
copied=0
for f in "$BUILD_DIR"/amped3 "$BUILD_DIR"/amped3-texture-upscale "$BUILD_DIR"/*.so "$BUILD_DIR"/*.so.*; do
  [ -e "$f" ] || continue
  cp -a "$f" "$INSTALL_DIR"/; copied=$((copied + 1))
done
if [ -d "$BUILD_DIR/textures" ]; then
  mkdir -p "$INSTALL_DIR/textures"; cp -a "$BUILD_DIR/textures"/. "$INSTALL_DIR/textures"/
fi
[ -f "$INSTALL_DIR/amped3.toml" ] || cp "$PROJECT_DIR/amped3.defaults.toml" "$INSTALL_DIR/amped3.toml"
note "$copied file(s) copied"

# A launcher (what Steam runs) and a desktop entry (Steam's "Add a Non-Steam
# Game" lists these; on SteamOS steamos-add-to-steam adds it directly).
LAUNCHER="$INSTALL_DIR/amped3-revamped.sh"
cat > "$LAUNCHER" <<'EOF'
#!/bin/sh
# Starts Amped 3 Revamped from its own folder (the folder is portable).
cd "$(dirname "$0")" && exec ./amped3 "$@"
EOF
chmod +x "$LAUNCHER"
DESKTOP="$HOME/.local/share/applications/amped3-revamped.desktop"
mkdir -p "$(dirname "$DESKTOP")"
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Type=Application
Name=Amped 3 Revamped
Comment=Amped 3 (Xbox 360), native
Exec="$LAUNCHER"
Path=$INSTALL_DIR
Terminal=false
Categories=Game;
EOF
note "Launcher: $LAUNCHER"
note "Desktop entry: $DESKTOP"
if command -v steamos-add-to-steam >/dev/null && ask 'Add Amped 3 Revamped to Steam now?' y; then
  steamos-add-to-steam "$DESKTOP" || warn 'steamos-add-to-steam failed; add it from Steam (Games > Add a Non-Steam Game).'
fi

# ---------------------------------------------------------------------------
# Optional, not part of --yes: downloads Real-ESRGAN, keeps the GPU busy.
if [ "$UPSCALE" = ask ]; then
  if [ "$YES" -eq 0 ] && [ "$ARCH" = amd64 ] && ask "Upscale the game's menu, HUD and loading screen textures 4x with Real-ESRGAN? (45 MB download, a few minutes on the GPU)" n; then
    UPSCALE=yes
  else
    UPSCALE=no
  fi
fi
if [ "$UPSCALE" = yes ]; then
  step 'Upscaling textures (Real-ESRGAN)'
  bash "$PROJECT_DIR/linux/upscale-textures.sh" --install-dir "$INSTALL_DIR" ||
    warn "Texture upscaling didn't finish (the game runs without it; run linux/upscale-textures.sh to retry)"
fi

printf '\n\033[32mDone. Amped 3 Revamped is in %s\033[0m\n' "$INSTALL_DIR"
note 'Run it with amped3-revamped.sh, or from Steam once added (Game Mode works).'
