ZIG ?= zig
TARGET := bin/networkfix.asi
MINHOOK_DIR := vendor/minhook
SRCS := src/main.c \
$(MINHOOK_DIR)/src/buffer.c \
$(MINHOOK_DIR)/src/hde/hde32.c \
$(MINHOOK_DIR)/src/hde/hde64.c \
$(MINHOOK_DIR)/src/hook.c \
$(MINHOOK_DIR)/src/trampoline.c
CFLAGS := -I$(MINHOOK_DIR)/include
LDFLAGS := -lc -lws2_32 -lshlwapi

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	mkdir -p $(dir $@)
	$(ZIG) build-lib -target x86-windows-gnu -dynamic -O ReleaseSmall \
$(CFLAGS) $(LDFLAGS) \
$(SRCS)
	mv main.dll $@

clean:
	rm -f main.dll main.lib $(TARGET)

