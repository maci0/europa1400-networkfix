#!/usr/bin/env bash
set -e

# Run from repository root
cd "$(dirname "$0")/.."

zig build-lib -target x86-windows-gnu -dynamic -O ReleaseSmall \
  -Ivendor/minhook/include -lc -lws2_32 -lshlwapi \
  src/main.c \
  vendor/minhook/src/buffer.c \
  vendor/minhook/src/hde/hde32.c \
  vendor/minhook/src/hde/hde64.c \
  vendor/minhook/src/hook.c \
  vendor/minhook/src/trampoline.c

mkdir -p bin
mv main.dll bin/networkfix.asi
#cp bin/networkfix.asi ~/.wine/drive_c/Guild
