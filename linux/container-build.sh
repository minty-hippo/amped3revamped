#!/usr/bin/env bash
# Runs ./setup.sh inside the build container (linux/Containerfile), for
# systems without the build tools: SteamOS, Armada and other read-only ones.
# Uses podman (or docker). The container sees the project folder, the SDK
# beside it and the game folder at the same paths as the host, so the result
# runs on the host as it is.
#
#   linux/container-build.sh [setup.sh options...]
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PARENT_DIR="$(dirname "$PROJECT_DIR")"

if command -v podman >/dev/null; then ENGINE=podman
elif command -v docker >/dev/null; then ENGINE=docker
else
  echo 'ERROR: neither podman nor docker is installed. SteamOS 3.5+ and Fedora-based systems such as'
  echo 'Armada include podman; elsewhere install podman, or install Clang 20+, CMake and Ninja and run ./setup.sh.'
  exit 1
fi

# The image, rebuilt when the Containerfile changes.
TAG="amped3-revamped-build:$(sha256sum "$PROJECT_DIR/linux/Containerfile" | cut -c1-12)"
if ! "$ENGINE" image exists "$TAG" 2>/dev/null && [ -z "$("$ENGINE" images -q "$TAG" 2>/dev/null)" ]; then
  echo "==> Building the build container ($TAG) - once, a few minutes"
  "$ENGINE" build -t "$TAG" -f "$PROJECT_DIR/linux/Containerfile" "$PROJECT_DIR/linux"
fi

# Folders to share: the project's parent (project + SDK) and the game's, if
# given and elsewhere.
mounts=(-v "$PARENT_DIR:$PARENT_DIR")
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
  case "${args[$i]}" in
    --game-dir|--sdk-dir)
      d="$(cd "${args[$((i + 1))]}" 2>/dev/null && pwd || true)"
      if [ -n "$d" ]; then
        args[$((i + 1))]="$d"
        case "$d/" in "$PARENT_DIR"/*) ;; *) mounts+=(-v "$d:$d") ;; esac
      fi ;;
  esac
done

user_opts=()
if [ "$ENGINE" = podman ]; then
  user_opts=(--userns=keep-id -e HOME=/tmp)   # files come out owned by you
else
  user_opts=(--user "$(id -u):$(id -g)" -e HOME=/tmp)
fi
tty_opts=()
[ -t 0 ] && tty_opts=(-it)
# label=disable: SELinux (Fedora / Armada) would otherwise block the mounts.
exec "$ENGINE" run --rm "${tty_opts[@]}" "${user_opts[@]}" --security-opt label=disable \
  "${mounts[@]}" -w "$PROJECT_DIR" "$TAG" bash ./setup.sh "${args[@]}"
