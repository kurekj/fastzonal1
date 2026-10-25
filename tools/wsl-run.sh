#!/usr/bin/env bash
# Run a command inside the repository from WSL with the ~/.local toolchain on PATH.
# Usage: wsl -d Ubuntu-24.04 -- bash tools/wsl-run.sh <command> [args...]
# The repository is reached through its /mnt/<drive> path. Build directories are the ones the
# presets name (build/release, build/debug); do not share them with a Windows build.
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")/.."
exec "$@"
