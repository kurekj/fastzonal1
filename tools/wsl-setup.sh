#!/usr/bin/env bash
# One-time, sudo-free toolchain setup inside WSL (Ubuntu 24.04): cmake and ninja into ~/.local.
# g++ and make come from the distribution. Run: wsl -d Ubuntu-24.04 -- bash tools/wsl-setup.sh
set -euo pipefail
mkdir -p "$HOME/.local/bin" "$HOME/.local/share"
if ! "$HOME/.local/bin/cmake" --version >/dev/null 2>&1; then
    tmp=$(mktemp -d)
    curl -sL https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz | tar xz -C "$tmp"
    cp -r "$tmp"/cmake-3.31.6-linux-x86_64/bin/* "$HOME/.local/bin/"
    cp -r "$tmp"/cmake-3.31.6-linux-x86_64/share/* "$HOME/.local/share/"
    rm -rf "$tmp"
fi
if ! "$HOME/.local/bin/ninja" --version >/dev/null 2>&1; then
    tmp=$(mktemp -d)
    curl -sL https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip -o "$tmp/ninja.zip"
    python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$tmp/ninja.zip" "$HOME/.local/bin"
    chmod +x "$HOME/.local/bin/ninja"
    rm -rf "$tmp"
fi
echo "cmake: $("$HOME/.local/bin/cmake" --version | head -1)"
echo "ninja: $("$HOME/.local/bin/ninja" --version)"
echo "g++:   $(g++ --version | head -1)"
