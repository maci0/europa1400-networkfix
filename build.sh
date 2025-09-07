zig build-lib -target x86-windows-gnu -dynamic -O ReleaseSmall -Ivendor/minhook/include -lc -lws2_32 -lshlwapi \
main.c \
vendor/minhook/src/buffer.c \
vendor/minhook/src/hde/hde32.c \
vendor/minhook/src/hde/hde64.c \
vendor/minhook/src/hook.c \
vendor/minhook/src/trampoline.c \

mv main.dll networkfix.asi
#cp networkfix.asi ~/.wine/drive_c/Guild
