zig build-lib -target x86-windows-gnu -dynamic -O ReleaseSmall -Iminhook-1.3.4/include -lc -lws2_32 -lshlwapi \
main.c \
minhook-1.3.4/src/buffer.c \
minhook-1.3.4/src/hde/hde32.c \
minhook-1.3.4/src/hde/hde64.c \
minhook-1.3.4/src/hook.c \
minhook-1.3.4/src/trampoline.c \



mv main.dll networkfix.asi
cp networkfix.asi ~/.wine/drive_c/Guild
