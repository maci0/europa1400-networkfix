#!/bin/bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
if [[ -f d3d8.dll && -f dxwrapper.dll ]]; then echo "dxwrapper already present"; exit 0; fi
echo "Fetching dxwrapper v1.7.8400.25 dx8.games.zip..."
URL="https://github.com/elishacloud/dxwrapper/releases/download/v1.7.8400.25/dx8.games.zip"
SHA256="6d301afdef0f4ba09cacf23275718d4b4541a97acab34e41ab08f5e43d15f9f9"
ZIP="/tmp/dx8.zip"

# Runs on the developer host (Linux/macOS/Git Bash, see docs/development-guide.md).
# Download: curl ships with macOS and Git Bash; wget is the Linux default.
if command -v curl >/dev/null 2>&1; then
  curl -fsSL -o "$ZIP" "$URL"
elif command -v wget >/dev/null 2>&1; then
  wget -qO "$ZIP" "$URL"
else
  echo "error: need curl or wget in PATH" >&2
  exit 1
fi

# Checksum: coreutils sha256sum on Linux/Git Bash, Perl shasum on stock macOS
# (same fallback as the Makefile SHA256SUM detection).
CHECK_LINE="$SHA256  $ZIP"
if command -v sha256sum >/dev/null 2>&1; then
  printf '%s\n' "$CHECK_LINE" | sha256sum --check --strict
elif command -v shasum >/dev/null 2>&1; then
  printf '%s\n' "$CHECK_LINE" | shasum -a 256 --check --strict
else
  echo "error: need sha256sum or shasum in PATH to verify the download" >&2
  rm -f "$ZIP"
  exit 1
fi
unzip -o "$ZIP" -d /tmp/dxwrapper_fetch
cp /tmp/dxwrapper_fetch/d3d8.dll /tmp/dxwrapper_fetch/dxwrapper.dll ./
# two fixed filenames, display only
# shellcheck disable=SC2012
echo "Fetched $(ls -lh d3d8.dll dxwrapper.dll | awk '{print $9, $5}')"
rm -f /tmp/dx8.zip
