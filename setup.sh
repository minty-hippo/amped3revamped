#!/usr/bin/env bash
# Builds the ReXGlue SDK and Amped 3 Revamped on Linux, natively, for the
# machine's own architecture: x86_64 (SteamOS, desktop distributions) or
# aarch64 (Armada and other ARM handhelds). The Linux counterpart of setup.ps1.
#
#   ./setup.sh [--game-dir <folder with default.xex>] [--sdk-dir <folder>]
#              [--config Release|RelWithDebInfo|Debug] [--skip-sdk] [--jobs N] [--run]
#
# Needs Clang 20+, CMake 3.25+, Ninja, Git, Python 3 and the X11 / Wayland /
# audio development packages. On SteamOS and Armada (read-only systems) use
# ./install.sh, which runs this inside a container that has all of them.
# Every step logs to ./logs/.
set -euo pipefail

SDK_REPO='https://github.com/rexglue/rexglue-sdk.git'
SDK_COMMIT='c94f5ebdcb3c9d1a460ca48e04f9758448f8d518'

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(dirname "$PROJECT_DIR")"
GAME_DIR=''
SDK_DIR=''
CONFIG='Release'
SKIP_SDK=0
RUN=0
JOBS=''

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir) GAME_DIR="$2"; shift 2 ;;
    --sdk-dir) SDK_DIR="$2"; shift 2 ;;
    --config) CONFIG="$2"; shift 2 ;;
    --skip-sdk) SKIP_SDK=1; shift ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --run) RUN=1; shift ;;
    -h|--help) usage 0 ;;
    *) echo "Unknown option: $1"; usage 2 ;;
  esac
done

LOG_DIR="$PROJECT_DIR/logs"
mkdir -p "$LOG_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
step() { printf '\n\033[36m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '    \033[33m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31mERROR: %s\033[0m\n' "$*"; exit 1; }
LAST_LOG=''
# Runs a command with its output in logs/<stamp>-<name>.log; stops on failure
# unless --allow-fail is given (then returns its status).
run_logged() {
  local allow=0
  if [ "$1" = '--allow-fail' ]; then allow=1; shift; fi
  local name="$1"; shift
  LAST_LOG="$LOG_DIR/$STAMP-$name.log"
  printf '    \033[90m(log: %s)\033[0m\n' "$LAST_LOG"
  set +e
  "$@" 2>&1 | tee "$LAST_LOG" | sed 's/^/    /'
  local rc=${PIPESTATUS[0]}
  set -e
  if [ "$rc" -ne 0 ]; then
    [ "$allow" -eq 1 ] && return "$rc"
    fail "$name failed (exit $rc). See $LAST_LOG"
  fi
  return 0
}

case "$CONFIG" in
  Release) CFG=release ;; RelWithDebInfo) CFG=relwithdebinfo ;; Debug) CFG=debug ;;
  *) fail "--config must be Release, RelWithDebInfo or Debug" ;;
esac
case "$(uname -m)" in
  x86_64|amd64) ARCH=amd64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  *) fail "Unsupported CPU architecture $(uname -m) (x86_64 and aarch64 are)." ;;
esac

# ---------------------------------------------------------------------------
# The game: any folder inside the project, then beside it, holding default.xex.
if [ -z "$GAME_DIR" ]; then
  for d in "$PROJECT_DIR"/*/ "$PARENT_DIR"/*/; do
    case "$(basename "$d")" in out|generated|logs|src|third_party|tools|patches) continue ;; esac
    if [ -f "$d/default.xex" ]; then GAME_DIR="${d%/}"; break; fi
  done
  [ -n "$GAME_DIR" ] || fail "Couldn't find Amped 3's default.xex in or next to $PROJECT_DIR. Pass --game-dir <folder>."
fi
GAME_DIR="$(cd "$GAME_DIR" && pwd)"
XEX="$GAME_DIR/default.xex"
[ -f "$XEX" ] || fail "No default.xex in $GAME_DIR."
step "Game: $GAME_DIR"
python3 "$PROJECT_DIR/linux/xiso.py" check "$GAME_DIR" | sed 's/^/    /' || fail "That isn't Amped 3 (USA)."

# ---------------------------------------------------------------------------
step "Checking the build tools ($ARCH)"
clang_major() { "$1" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\).*/\1/p' | head -1; }
CC=''; CXX=''
for v in 22 21 20 ''; do
  c="clang${v:+-$v}"; cxx="clang++${v:+-$v}"
  if command -v "$c" >/dev/null && command -v "$cxx" >/dev/null; then
    m="$(clang_major "$c")"
    if [ -n "$m" ] && [ "$m" -ge 20 ]; then CC="$(command -v "$c")"; CXX="$(command -v "$cxx")"; break; fi
  fi
done
[ -n "$CC" ] || fail "Clang 20 or newer not found. On SteamOS / Armada use ./install.sh (it builds in a container); elsewhere install LLVM 20+ (https://apt.llvm.org, or your distribution's clang)."
note "clang: $CC (v$(clang_major "$CC"))"
for t in git cmake ninja python3; do
  command -v "$t" >/dev/null || fail "$t not found; install it (or use ./install.sh to build in a container)."
done
note "cmake: $(cmake --version | head -1)"

# Parallel jobs: the recompiled game's files are large, so about one compile
# per 2.5 GB of memory (a handheld runs out of memory long before cores).
if [ -z "$JOBS" ]; then
  mem_kb="$(awk '/MemTotal/ {print $2}' /proc/meminfo)"
  by_mem=$(( mem_kb / 2600000 )); [ "$by_mem" -ge 1 ] || by_mem=1
  JOBS="$(nproc)"; [ "$JOBS" -le "$by_mem" ] || JOBS="$by_mem"
fi
export CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"
note "parallel jobs: $JOBS"

# ---------------------------------------------------------------------------
[ -n "$SDK_DIR" ] || SDK_DIR="$PARENT_DIR/rexglue-sdk"
mkdir -p "$(dirname "$SDK_DIR")"
SDK_DIR="$(cd "$(dirname "$SDK_DIR")" && pwd)/$(basename "$SDK_DIR")"
SDK_PRESET="linux-$ARCH"
SDK_INSTALL="$SDK_DIR/out/install/$SDK_PRESET"
REXGLUE="$SDK_INSTALL/bin/rexglue"

if [ "$SKIP_SDK" -eq 0 ] || [ ! -x "$REXGLUE" ]; then
  step "Getting the ReXGlue SDK (${SDK_COMMIT:0:7})"
  [ -d "$SDK_DIR/.git" ] || run_logged sdk-clone git clone "$SDK_REPO" "$SDK_DIR"
  cd "$SDK_DIR"
  if [ "$(git rev-parse HEAD)" != "$SDK_COMMIT" ]; then
    if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
      warn 'Resetting local SDK changes (patches are re-applied below)'
      git checkout -- . >/dev/null 2>&1
    fi
    rm -f "$SDK_DIR/.amped3-patches"
    run_logged sdk-fetch git fetch origin
    run_logged sdk-checkout git -c advice.detachedHead=false checkout "$SDK_COMMIT"
  fi
  run_logged sdk-submodules git submodule update --init --recursive --force

  step 'Applying SDK fixes'
  patches=("$PROJECT_DIR"/patches/rexglue/*.patch)
  wanted="$(for p in "${patches[@]}"; do printf '%s %s\n' "$(basename "$p")" "$(sha256sum "$p" | cut -c1-64)"; done)"
  applied="$(cat "$SDK_DIR/.amped3-patches" 2>/dev/null || true)"
  if [ "$applied" = "$wanted" ]; then
    note "all ${#patches[@]} SDK fixes already applied"
  else
    if [ -n "$(git status --porcelain --untracked-files=no --ignore-submodules=all)" ]; then
      warn 'SDK fixes changed; resetting the SDK source before re-applying them'
      git checkout -- . >/dev/null 2>&1
    fi
    # Files the patches add are untracked: remove them so they apply again.
    for p in "${patches[@]}"; do
      awk 'prev == "--- /dev/null" && /^\+\+\+ b\// { sub(/^\+\+\+ b\//, ""); print } { prev = $0 }' "$p" |
        while read -r added; do rm -f "$SDK_DIR/$added"; done
    done
    rm -f "$SDK_DIR/.amped3-patches"
    for p in "${patches[@]}"; do
      if git apply --check --reverse "$p" >/dev/null 2>&1; then note "already applied: $(basename "$p")"; continue; fi
      git apply "$p" || fail "Could not apply $(basename "$p") to the SDK."
      note "applied: $(basename "$p")"
    done
    printf '%s\n' "$wanted" > "$SDK_DIR/.amped3-patches"
  fi

  step "Building the SDK ($CONFIG) - the first time takes a while"
  run_logged sdk-configure cmake --preset "$SDK_PRESET" \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=OFF
  # The codegen tool installs with Release only, so Release is always built.
  run_logged sdk-build-release cmake --build "out/build/$SDK_PRESET" --config Release --target install
  if [ "$CONFIG" != Release ]; then
    run_logged sdk-build cmake --build "out/build/$SDK_PRESET" --config "$CONFIG" --target install
  fi
  cd "$PROJECT_DIR"
fi
[ -x "$REXGLUE" ] || fail "SDK install did not produce $REXGLUE"

# ---------------------------------------------------------------------------
step 'Setting up the Amped 3 project'
cd "$PROJECT_DIR"
MANIFEST="$PROJECT_DIR/amped3_manifest.toml"
AUTO_HINTS="$PROJECT_DIR/amped3_functions_auto.toml"
[ -f amped3_functions_scan.toml ] || echo '[functions]' > amped3_functions_scan.toml
if [ ! -f "$AUTO_HINTS" ]; then
  cat > "$AUTO_HINTS" <<'EOF'
# Written by setup.ps1 / setup.sh. Function entry points the recompiler could
# not find on its own (branch targets outside every known function). No size
# given, so the analysis works out where each one ends. Safe to delete: it
# gets rebuilt.

[functions]
EOF
fi
add_hint() { printf '"0x%s" = {}  # %s\n' "$1" "$2" >> "$AUTO_HINTS"; }
# Entry points already found on other machines (see setup.ps1).
while read -r addr why; do
  grep -qi "0x$addr" "$AUTO_HINTS" || add_hint "$addr" "$why"
done <<'EOF'
82748350 branch from 0x82751E44
8274BD28 branch from 0x827503A4
827FDBC8 branch from 0x8283A160
82831090 called through a pointer at runtime
826C8FE8 called through a pointer at runtime
821E6F20 called through a pointer at runtime
82749450 called through a pointer at runtime
EOF
INCLUDES='includes = ["amped3_functions_scan.toml", "amped3_functions_auto.toml", "amped3_d3d_names.toml", "amped3_functions.toml"]'
if [ ! -f "$MANIFEST" ]; then
  run_logged init "$REXGLUE" init --project-name amped3 --xex-path "$XEX" --game-root "$GAME_DIR" --project-root "$PROJECT_DIR"
fi
# The entrypoint's includes line, rewritten only when it differs (a changed
# manifest means a full recompile). Also the game's paths: rexglue init
# writes them lower-cased (harmless on Windows, wrong on Linux, where
# "Games/Amped 3 Revamped" isn't "games/amped 3 revamped"), so they're set to
# the real ones here.
INCLUDES="$INCLUDES" GAME_ROOT="$GAME_DIR" XEX_PATH="$XEX" python3 - "$MANIFEST" <<'EOF'
import os, re, sys
p = sys.argv[1]
text = open(p, encoding='utf-8').read()
new = re.sub(r'includes = \[\]', os.environ['INCLUDES'], text, count=1)
new = re.sub(r'includes = \[[^\]\r\n]*"amped3_functions\.toml"\]', os.environ['INCLUDES'], new, count=1)

def toml_str(v):
    return '"' + v.replace('\\', '\\\\').replace('"', '\\"') + '"'

def set_key(text, section, key, value):
    # The key's first line inside [section] (before the next table header).
    m = re.search(r'^\[' + re.escape(section) + r'\][^\n]*\n', text, re.M)
    if not m:
        return text
    end = re.search(r'^\[', text[m.end():], re.M)
    stop = m.end() + (end.start() if end else len(text) - m.end())
    body = text[m.end():stop]
    line = key + ' = ' + toml_str(value)
    body2, n = re.subn(r'^' + re.escape(key) + r'\s*=.*$', lambda _: line, body, count=1, flags=re.M)
    if n == 0:
        body2 = line + '\n' + body
    return text[:m.end()] + body2 + text[stop:]

new = set_key(new, 'project', 'game_root', os.environ['GAME_ROOT'])
new = set_key(new, 'entrypoint', 'file_path', os.environ['XEX_PATH'])
if new != text:
    open(p, 'w', encoding='utf-8', newline='').write(new)
EOF
cp overlay/CMakeLists.txt CMakeLists.txt

# ---------------------------------------------------------------------------
PRESET="linux-$ARCH-$CFG"
CONFIGURE_ARGS=(-DCMAKE_PREFIX_PATH="$SDK_INSTALL" -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX")
if [ -f "$SDK_DIR/thirdparty/glslang/CMakeLists.txt" ]; then
  CONFIGURE_ARGS+=(-DAMPED3_GLSLANG_DIR="$SDK_DIR/thirdparty/glslang" -DAMPED3_SDK_SOURCE_DIR="$SDK_DIR")
else
  warn "glslang not found in the SDK checkout; the native renderer will be unavailable"
fi
BUILD_DIR="$PROJECT_DIR/out/build/$PRESET"
EXE="$BUILD_DIR/amped3"

# A build folder configured from another place (the project was moved or
# renamed): start its configuration over (everything recompiles once).
reset_moved_build() {
  local cache="$BUILD_DIR/CMakeCache.txt"
  [ -f "$cache" ] || return 0
  local was; was="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")"
  [ -n "$was" ] && [ "$(cd "$was" 2>/dev/null && pwd || echo "$was")" != "$PROJECT_DIR" ] || return 0
  warn "The project was set up in $was; starting $PRESET's build configuration over (a full rebuild)."
  rm -rf "$cache" "$BUILD_DIR/CMakeFiles" "$BUILD_DIR/build.ninja" "$BUILD_DIR/.ninja_deps" "$BUILD_DIR/.ninja_log"
}

step "Recompiling the game's code ($PRESET)"
reset_moved_build
run_logged build-configure cmake --preset "$PRESET" "${CONFIGURE_ARGS[@]}"
generated=0
for round in 1 2 3 4 5 6 7 8 9 10; do
  if run_logged --allow-fail "build-codegen-$round" cmake --build --preset "$PRESET" --target amped3_codegen; then
    generated=1; break
  fi
  # Branches into code that isn't inside any known function.
  new="$(grep -oE '0x[0-9A-Fa-f]{8} from 0x[0-9A-Fa-f]{8}: bl? 0x[0-9A-Fa-f]{8} from 0x[0-9A-Fa-f]{8}.*target not in any function' "$LAST_LOG" |
         sed -E 's/^0x([0-9A-Fa-f]{8}) from 0x([0-9A-Fa-f]{8}).*/\1 \2/' | tr 'a-f' 'A-F' | sort -u || true)"
  added=0
  while read -r target site; do
    [ -n "$target" ] || continue
    grep -qi "0x$target" "$AUTO_HINTS" && continue
    note "0x$target  (branched to from 0x$site)"
    add_hint "$target" "branch from 0x$site"
    added=$((added + 1))
  done <<< "$new"
  [ "$added" -gt 0 ] || fail "Recompiling failed. See $LAST_LOG"
  step "Round $round: added $added function entry point(s) the recompiler missed"
done
[ "$generated" -eq 1 ] || fail "Recompiling still failing after 10 rounds. See $LAST_LOG"
[ -f generated/default/sources.cmake ] || fail 'Recompiler finished but generated/default/sources.cmake is missing.'

step "Building amped3 ($PRESET) - compiling the recompiled game takes a while"
run_logged build-reconfigure cmake --preset "$PRESET" "${CONFIGURE_ARGS[@]}"
run_logged build cmake --build --preset "$PRESET"
[ -x "$EXE" ] || fail "Build finished but $EXE is missing."

# AMD FSR 3.1 (Upscaler option): AMD ships its runtime for Windows only, so
# it's built here from the FidelityFX SDK's sources (linux/fidelityfx). Only
# the first time or when those files change; the game runs without it.
if [ -f "$SDK_DIR/thirdparty/glslang/CMakeLists.txt" ]; then
  step 'AMD FSR 3.1 upscaler (built from the FidelityFX SDK sources)'
  if ! run_logged --allow-fail fsr env CC="$CC" CXX="$CXX" bash "$PROJECT_DIR/linux/fidelityfx/build.sh" \
         --dest "$BUILD_DIR" --glslang "$SDK_DIR/thirdparty/glslang" --jobs "$JOBS"; then
    warn "FSR 3.1 couldn't be built (see $LAST_LOG); the game works without it (Upscaler stays Off)."
  fi
fi
# Lets the build be run in place: amped3 reads the game folder from here when
# there's no game/ folder next to it (install.sh's install folder has one).
printf '%s\n' "$GAME_DIR" > "$BUILD_DIR/game.path"
# Where the sources are, for the launcher's Update (update.sh).
printf '%s\n' "$PROJECT_DIR" > "$BUILD_DIR/source.path"

printf '\n\033[32mBuilt: %s\033[0m\n' "$EXE"
note 'In game: Back+Start (or F1) opens the options menu. F3 performance overlay, F4 every setting.'
if [ "$RUN" -eq 1 ]; then
  step 'Launching'
  cd "$BUILD_DIR" && exec "$EXE" --game_data_root="$GAME_DIR"
fi
