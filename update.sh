#!/usr/bin/env bash
# Updates Amped 3 Revamped from GitHub. Run by the game's launcher (and usable
# by hand); the Windows counterpart is update.ps1.
#
#   ./update.sh branches                       lists the repository's branches
#   ./update.sh check <branch> [<base commit>] what's new on <branch> since the
#                                              commit the game was built from
#   ./update.sh update <branch> <game folder>  gets <branch> into this project
#                                              folder (git when it is a git
#                                              checkout, else the branch's zip
#                                              from GitHub), then rebuilds:
#     - <game folder> is an installed copy (install.sh): install.sh runs again
#       for that folder (in its container on SteamOS / Armada);
#     - <game folder> is this project's own build (out/build/...): setup.sh.
#
# The launcher reads lines starting with "::" (::branch::, ::remote::,
# ::behind::, ::ahead::, ::commit::, ::step::, ::error::, ::done::); the rest is
# shown as it is. A git checkout is reset to the branch on GitHub
# (git checkout -f -B): local changes to tracked files are discarded.
#
# Everything is inside main(), so bash has read the whole script before an
# update replaces this file.
main() {
  set -uo pipefail
  local project_dir default_repo
  project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  default_repo='minty-hippo/amped3revamped'

  say() { printf '::%s:: %s\n' "$1" "${2:-}"; }
  fail() { say error "$1"; exit 1; }
  is_git() { [ -e "$project_dir/.git" ] && command -v git >/dev/null; }
  git_() { git -C "$project_dir" -c safe.directory='*' "$@"; }
  repo_slug() {
    local url
    if is_git && url="$(git_ remote get-url origin 2>/dev/null)"; then
      if [[ "$url" =~ github\.com[:/]+([^/]+)/([^/[:space:]]+) ]]; then
        local repo="${BASH_REMATCH[2]}"; repo="${repo%.git}"; repo="${repo%/}"
        printf '%s/%s\n' "${BASH_REMATCH[1]}" "$repo"; return
      fi
    fi
    printf '%s\n' "$default_repo"
  }
  # GET https://api.github.com/repos/<repo><path> to stdout.
  api() {
    command -v curl >/dev/null || fail 'curl is needed to reach GitHub.'
    curl -fsSL --max-time 20 -H 'User-Agent: amped3-revamped-launcher' \
      -H 'Accept: application/vnd.github+json' "https://api.github.com/repos/$(repo_slug)$1"
  }
  # Runs a short Python 3 program over JSON on stdin (python3 is on SteamOS,
  # Armada and the distributions install.sh supports).
  py() { python3 -c "$1" "${@:2}"; }
  # setup.sh's own test for the build tools (clang / clang++ 20+, CMake,
  # Ninja, Git).
  have_tools() {
    local v c cxx m
    command -v cmake >/dev/null && command -v ninja >/dev/null && command -v git >/dev/null || return 1
    for v in 22 21 20 ''; do
      c="clang${v:+-$v}"; cxx="clang++${v:+-$v}"
      command -v "$c" >/dev/null && command -v "$cxx" >/dev/null || continue
      m="$("$c" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\).*/\1/p' | head -1)"
      [ -n "$m" ] && [ "$m" -ge 20 ] && return 0
    done
    return 1
  }

  local action="${1:-}"
  case "$action" in
    branches)
      local json
      json="$(api '/branches?per_page=100')" || fail "Couldn't reach GitHub."
      printf '%s' "$json" | py 'import json,sys
for b in json.load(sys.stdin): print("::branch::", b["name"])'
      say done
      ;;

    check)
      local branch="${2:-main}" base="${3:-}" json
      json="$(api "/commits/$branch")" || fail "Couldn't read branch '$branch' from GitHub."
      local info sha
      info="$(printf '%s' "$json" | py 'import json,sys
c=json.load(sys.stdin)
print(c["sha"], c["commit"]["committer"]["date"][:10], c["commit"]["message"].split("\n")[0].strip())')" ||
        fail "Unexpected reply from GitHub."
      say remote "$info"
      sha="${info%% *}"
      if [ -n "$base" ] && [ "$base" != unknown ]; then
        if [[ "$sha" == "$base"* ]]; then
          say behind 0
        elif json="$(api "/compare/$base...$sha")"; then
          printf '%s' "$json" | py 'import json,sys
c=json.load(sys.stdin)
print("::behind::", c.get("ahead_by", "?"))
print("::ahead::", c.get("behind_by", "?"))
for x in list(reversed(c.get("commits", [])))[:20]:
    print("::commit::", x["sha"][:7], x["commit"]["message"].split("\n")[0].strip())'
        else
          say behind '?'
        fi
      fi
      say done
      ;;

    update)
      local branch="${2:-}" exe_dir="${3:-}"
      [ -n "$branch" ] && [ -n "$exe_dir" ] || fail 'usage: update.sh update <branch> <game folder>'
      exe_dir="$(cd "$exe_dir" && pwd)" || fail "No folder $3."
      local dev_build=0
      case "$exe_dir/" in "$project_dir/out/"*) dev_build=1 ;; esac
      echo "Game folder: $exe_dir"
      echo "Project folder: $project_dir"
      [ "$dev_build" -eq 1 ] && echo 'This is the project'"'"'s own build (out/build): setup.sh rebuilds it.' ||
        echo 'This is an installed copy: install.sh rebuilds and reinstalls it.'

      say step "Getting the $branch branch"
      if is_git; then
        git_ fetch origin "$branch" 2>&1 || fail "git fetch failed."
        git_ checkout -f -B "$branch" "origin/$branch" 2>&1 || fail "git checkout failed."
        git_ log -1 --format='%h %cs %s' 2>&1
      else
        local repo json sha date tmp
        repo="$(repo_slug)"
        json="$(api "/commits/$branch")" || fail "Couldn't read branch '$branch' from GitHub."
        read -r sha date < <(printf '%s' "$json" | py 'import json,sys
c=json.load(sys.stdin); print(c["sha"], c["commit"]["committer"]["date"][:10])')
        tmp="$(mktemp -d)"
        echo "Downloading https://github.com/$repo/archive/$sha.zip"
        if ! curl -fsSL --max-time 600 -H 'User-Agent: amped3-revamped-launcher' \
             -o "$tmp/src.zip" "https://codeload.github.com/$repo/zip/$sha"; then
          rm -rf "$tmp"; fail 'The download failed.'
        fi
        # Extracted with Python (unzip isn't on every handheld), modes kept.
        py 'import os,sys,zipfile
z=zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    p=z.extract(i, sys.argv[2])
    m=(i.external_attr>>16)&0o777
    if m and not i.is_dir(): os.chmod(p, m)' "$tmp/src.zip" "$tmp/x" || { rm -rf "$tmp"; fail 'Extracting failed.'; }
        local top
        top="$(find "$tmp/x" -mindepth 1 -maxdepth 1 -type d | head -1)"
        echo "Copying into $project_dir"
        cp -a "$top"/. "$project_dir"/ || { rm -rf "$tmp"; fail 'Copying the new sources failed.'; }
        rm -rf "$tmp"
        printf 'branch=%s\ncommit=%s\ndate=%s\n' "$branch" "$sha" "$date" > "$project_dir/.amped3-source"
        echo "${sha:0:7} $date"
      fi

      # The running game has its exe and libraries mapped: they are moved
      # aside (not overwritten in place, which would crash it); the game
      # deletes the *.old files when it next starts.
      local f
      for f in "$exe_dir"/amped3 "$exe_dir"/*.so "$exe_dir"/*.so.*; do
        [ -f "$f" ] || continue
        case "$f" in *.old) continue ;; esac
        mv -f "$f" "$f.old"
      done

      local code=0
      if [ "$dev_build" -eq 1 ]; then
        local args=()
        [ -f "$exe_dir/game.path" ] && args+=(--game-dir "$(head -n1 "$exe_dir/game.path")")
        # Without the build tools on the system (SteamOS, Armada), in the
        # build container, as install.sh does.
        if have_tools; then
          say step 'Building (setup.sh)'
          bash "$project_dir/setup.sh" "${args[@]}" 2>&1 || code=$?
        else
          say step 'Building in the container (setup.sh)'
          bash "$project_dir/linux/container-build.sh" "${args[@]}" 2>&1 || code=$?
        fi
      else
        say step 'Building and installing (install.sh)'
        local extra=()
        # (an older install.sh, on a branch from before the launcher, lacks it)
        grep -q -- '--update)' "$project_dir/install.sh" && extra+=(--update)
        bash "$project_dir/install.sh" --game-dir "$exe_dir/game" --install-dir "$exe_dir" \
          --yes "${extra[@]}" --upscale-textures no 2>&1 || code=$?
      fi
      # Whatever the build didn't put back comes back.
      for f in "$exe_dir"/*.old; do
        [ -f "$f" ] || continue
        [ -e "${f%.old}" ] || cp -a "$f" "${f%.old}"
      done
      [ "$code" -eq 0 ] || fail "The build failed (exit $code); its logs are in $project_dir/logs."
      say done
      ;;

    *)
      sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 2
      ;;
  esac
}
main "$@"; exit $?
