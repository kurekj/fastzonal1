#!/usr/bin/env bash
# Run a command inside the repository from WSL with the ~/.local toolchain on PATH.
# Usage: wsl -d Ubuntu-24.04 -- bash tools/wsl-run.sh <command> [args...]
# The repository is reached through its /mnt/<drive> path; builds go to build/wsl-* so they never
# collide with Windows or deep3 build directories.
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")/.."
exec "$@"
