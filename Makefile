ZIG ?= zig
WINE ?= wine
TARGET := bin/networkfix.asi
DEBUG_TARGET := bin/networkfix-debug.asi
TEST_TARGET := bin/test_hooks.exe
MINHOOK_DIR := vendor/minhook
MINHOOK_SRCS := $(MINHOOK_DIR)/src/buffer.c \
$(MINHOOK_DIR)/src/hde/hde32.c \
$(MINHOOK_DIR)/src/hde/hde64.c \
$(MINHOOK_DIR)/src/hook.c \
$(MINHOOK_DIR)/src/trampoline.c
SRCS := src/main.c src/hooks.c src/logging.c src/sha256.c src/pattern_matcher.c $(MINHOOK_SRCS)
TEST_SRCS := test/test_hooks.c src/hooks.c src/logging.c src/sha256.c src/pattern_matcher.c $(MINHOOK_SRCS)
CFLAGS := -I$(MINHOOK_DIR)/include -Isrc
LDFLAGS := -lc -lws2_32 -lshlwapi -ladvapi32

.PHONY: all clean install test build-test

all: format $(TARGET)

debug: format $(DEBUG_TARGET)

test: build-test
	$(WINE) $(TEST_TARGET)

build-test: $(TEST_TARGET)

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

$(TEST_TARGET): $(TEST_SRCS)
	mkdir -p $(dir $@)
	$(ZIG) build-exe --name test_hooks -femit-bin=$@ -target x86-windows-gnu -O Debug \
-DNETWORKFIX_TEST=1 \
$(CFLAGS) $(LDFLAGS) \
$(TEST_SRCS)

clean:
	rm -f bin/*

format:
	clang-format -i src/*

install: $(TARGET)
	cp $(TARGET) ~/.wine/drive_c/Guild
