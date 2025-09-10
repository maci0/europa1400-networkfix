ZIG ?= zig
TARGET := bin/networkfix.asi
DEBUG_TARGET := bin/networkfix-debug.asi
MINHOOK_DIR := vendor/minhook
SRCS := src/main.c src/hooks.c src/logging.c src/sha256.c \
$(MINHOOK_DIR)/src/buffer.c \
$(MINHOOK_DIR)/src/hde/hde32.c \
$(MINHOOK_DIR)/src/hde/hde64.c \
$(MINHOOK_DIR)/src/hook.c \
$(MINHOOK_DIR)/src/trampoline.c
CFLAGS := -I$(MINHOOK_DIR)/include -Isrc
LDFLAGS := -lc -lws2_32 -lshlwapi -ladvapi32

.PHONY: all clean install

all: format $(TARGET)

debug: format $(DEBUG_TARGET)

$(TARGET): $(SRCS)
	mkdir -p $(dir $@)
	$(ZIG) build-lib --name networkfix -femit-bin=$@ -target x86-windows-gnu -dynamic -O ReleaseSmall \
$(CFLAGS) $(LDFLAGS) \
$(SRCS)

$(DEBUG_TARGET): $(SRCS)
	mkdir -p $(dir $@)
	$(ZIG) build-lib --name networkfix-debug -femit-bin=$@ -target x86-windows-gnu -dynamic -O Debug \
$(CFLAGS) $(LDFLAGS) \
$(SRCS)

clean:
	rm -f bin/*

format:
	clang-format -i src/*

install: $(TARGET)
	cp $(TARGET) ~/.wine/drive_c/Guild