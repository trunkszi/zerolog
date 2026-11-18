#!/usr/bin/env bash
set -euo pipefail

export VCPKG_OVERLAY_PORTS="${VCPKG_OVERLAY_PORTS:-$HOME/.local/vcpkg}"
export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache}"
export VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-$HOME/.cache/vcpkg/archives}"
mkdir -p "$VCPKG_OVERLAY_PORTS" "$CCACHE_DIR" "$VCPKG_DEFAULT_BINARY_CACHE"

vcpkg_root="$HOME/.vcpkg"
if [[ ! -e "$vcpkg_root" || ( -d "$vcpkg_root" && -z "$(ls -A "$vcpkg_root")" ) ]]; then
  # Publish only a bootstrapped checkout so a failed install can be retried.
  vcpkg_stage=$(mktemp -d "$HOME/.vcpkg-ci.XXXXXX")
  trap 'rm -rf -- "$vcpkg_stage"' EXIT
  git clone --filter=blob:none https://github.com/microsoft/vcpkg.git "$vcpkg_stage"
  git -C "$vcpkg_stage" checkout --detach 2273a28f34ce5aac8be50b3b6b44da7fc1722e06
  "$vcpkg_stage/bootstrap-vcpkg.sh" -disableMetrics
  mv -T -- "$vcpkg_stage" "$vcpkg_root"
fi
if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
  printf 'Existing vcpkg directory has no executable: %s\n' "$vcpkg_root/vcpkg" >&2
  exit 1
fi

ports_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../ci-ports" && pwd)
for port in "$ports_root"/*/; do
  port=${port%/}
  dest="$VCPKG_OVERLAY_PORTS/${port##*/}"
  [[ -n "$(ls -A "$dest" 2>/dev/null)" ]] && continue
  mkdir -p "$dest"
  cp -a -- "$port/." "$dest/"
done

if [[ -n "${GITHUB_ENV:-}" ]]; then
  printf '%s\n' \
    "VCPKG_OVERLAY_PORTS=$VCPKG_OVERLAY_PORTS" \
    "CCACHE_DIR=$CCACHE_DIR" \
    "VCPKG_DEFAULT_BINARY_CACHE=$VCPKG_DEFAULT_BINARY_CACHE" >>"$GITHUB_ENV"
fi
