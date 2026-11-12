#!/usr/bin/env bash
set -euo pipefail
mkdir -p "$HOME/.local" "$HOME/.cache/ccache" "$HOME/.cache/vcpkg/archives"
if [[ ! -d "$HOME/.vcpkg" ]]; then
  git clone --filter=blob:none https://github.com/microsoft/vcpkg.git "$HOME/.vcpkg"
  git -C "$HOME/.vcpkg" checkout --detach 2273a28f34ce5aac8be50b3b6b44da7fc1722e06
fi
if [[ ! -x "$HOME/.vcpkg/vcpkg" ]]; then
  "$HOME/.vcpkg/bootstrap-vcpkg.sh" -disableMetrics
fi
if [[ ! -d "$HOME/.local/vcpkg" ]]; then
  git clone --depth 1 "${OVERLAY_PORTS_REPOSITORY:?Set the overlay ports repository URL}" "$HOME/.local/vcpkg"
fi
