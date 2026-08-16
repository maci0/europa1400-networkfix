ZIG ?= zig
ZIG_VERSION_EXPECTED := 0.16.0
WINE ?= wine
TARGET := bin/networkfix.asi
DEBUG_TARGET := bin/networkfix-debug.asi
TEST_TARGET := bin/test_hooks.exe
VERSION_FILE := VERSION
VERSION := $(shell cat $(VERSION_FILE) 2>/dev/null || echo 0.0.0-dev)
DIST_DIR := dist
DIST_NAME := networkfix-$(VERSION)
MINHOOK_DIR := vendor/minhook
# hde64.c is 32-bit only target - excluded to save ~400LOC compile
MINHOOK_SRCS := $(MINHOOK_DIR)/src/buffer.c \
$(MINHOOK_DIR)/src/hde/hde32.c \
$(MINHOOK_DIR)/src/hook.c \
$(MINHOOK_DIR)/src/trampoline.c
HDRS := $(wildcard src/*.h) Makefile .clang-format
SRCS := src/main.c src/hooks.c src/logging.c src/sha256.c src/pattern_matcher.c src/versions.c $(MINHOOK_SRCS)
TEST_SRCS := test/test_hooks.c src/hooks.c src/logging.c src/sha256.c src/pattern_matcher.c src/versions.c $(MINHOOK_SRCS)
CFLAGS := -I$(MINHOOK_DIR)/include -Isrc
LDFLAGS := -lc -lws2_32 -lshlwapi -ladvapi32

.PHONY: all clean install test build-test check-zig format lint verify dist sbom

check-zig:
	@$(ZIG) version | grep -q "$(ZIG_VERSION_EXPECTED)" || \
		(echo "ERROR: Zig $(ZIG_VERSION_EXPECTED) required, got $$( $(ZIG) version)" && exit 1)

all: check-zig $(TARGET)

debug: check-zig $(DEBUG_TARGET)

test: build-test
	$(WINE) $(TEST_TARGET)

build-test: check-zig $(TEST_TARGET)

$(TARGET): $(SRCS) $(HDRS)
	mkdir -p $(dir $@)
	$(ZIG) build-lib --name networkfix -femit-bin=$@ -target x86-windows-gnu -dynamic -O ReleaseSmall \
$(CFLAGS) $(LDFLAGS) \
$(SRCS)

$(DEBUG_TARGET): $(SRCS) $(HDRS)
	mkdir -p $(dir $@)
	$(ZIG) build-lib --name networkfix-debug -femit-bin=$@ -target x86-windows-gnu -dynamic -O Debug \
$(CFLAGS) $(LDFLAGS) \
$(SRCS)

$(TEST_TARGET): $(TEST_SRCS) $(HDRS)
	mkdir -p $(dir $@)
	$(ZIG) build-exe --name test_hooks -femit-bin=$@ -target x86-windows-gnu -O Debug \
-DNETWORKFIX_TEST=1 \
$(CFLAGS) $(LDFLAGS) \
$(TEST_SRCS)

# Reproducible verify: build twice with fixed SOURCE_DATE_EPOCH
verify: check-zig
	rm -rf bin/verify1 bin/verify2; mkdir -p bin/verify1 bin/verify2
	SOURCE_DATE_EPOCH=0 $(MAKE) $(TARGET)
	cp $(TARGET) bin/verify1/networkfix.asi
	mkdir -p bin/verify2
	SOURCE_DATE_EPOCH=0 $(MAKE) $(TARGET)
	cp $(TARGET) bin/verify2/networkfix.asi
	sha256sum bin/verify1/networkfix.asi bin/verify2/networkfix.asi
	cmp bin/verify1/networkfix.asi bin/verify2/networkfix.asi && echo "reproducible: OK"

clean:
	rm -f bin/*.asi bin/*.exe bin/*.pdb bin/*.lib 2>/dev/null; rm -f bin/verify*/*.asi 2>/dev/null || true

format:
	clang-format -i src/*.c src/*.h test/*.c

lint: format
	git diff --exit-code src/ test/

install: $(TARGET)
	cp $(TARGET) ~/.wine/drive_c/Guild

sbom:
	@mkdir -p $(DIST_DIR)
	@echo '{"bomFormat":"CycloneDX","specVersion":"1.5","version":1,"metadata":{"component":{"name":"europa1400-networkfix","version":"$(VERSION)","type":"application"}},"components":[{"name":"minhook","version":"1.3.4","type":"library","purl":"pkg:github/TsudaKageyu/minhook@v1.3.4"},{"name":"zig","version":"$(ZIG_VERSION_EXPECTED)","type":"application"}]}' > $(DIST_DIR)/sbom.json
	@echo "sbom: $(DIST_DIR)/sbom.json"

dist: check-zig $(TARGET) sbom
	@mkdir -p $(DIST_DIR)
	@rm -f $(DIST_DIR)/$(DIST_NAME).zip $(DIST_DIR)/$(DIST_NAME).sha256
	@zip -j $(DIST_DIR)/$(DIST_NAME).zip $(TARGET) LICENSE README.md CHANGELOG.md $(DIST_DIR)/sbom.json >/dev/null
	@sha256sum $(DIST_DIR)/$(DIST_NAME).zip > $(DIST_DIR)/$(DIST_NAME).sha256
	@sha256sum $(TARGET) >> $(DIST_DIR)/$(DIST_NAME).sha256
	@ls -lh $(DIST_DIR)/$(DIST_NAME).zip $(DIST_DIR)/$(DIST_NAME).sha256
	@cat $(DIST_DIR)/$(DIST_NAME).sha256
