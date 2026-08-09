#!/bin/bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
if [[ -f d3d8.dll && -f dxwrapper.dll ]]; then echo "dxwrapper already present"; exit 0; fi
echo "Fetching dxwrapper v1.7.8400.25 dx8.games.zip..."
wget -qO /tmp/dx8.zip https://github.com/elishacloud/dxwrapper/releases/download/v1.7.8400.25/dx8.games.zip
echo "6d301afdef0f4ba09cacf23275718d4b4541a97acab34e41ab08f5e43d15f9f9  /tmp/dx8.zip" | sha256sum --check --strict
unzip -o /tmp/dx8.zip -d /tmp/dxwrapper_fetch
cp /tmp/dxwrapper_fetch/d3d8.dll /tmp/dxwrapper_fetch/dxwrapper.dll ./
echo "Fetched $(ls -lh d3d8.dll dxwrapper.dll | awk '{print $9, $5}')"
rm -f /tmp/dx8.zip
