ZIG ?= zig
ZIG_VERSION_EXPECTED := 0.16.0
# Pinned: clang-format reflows differently between major versions, so an
# unpinned tool silently turns `make lint` into "whatever the runner shipped
# this month". CI installs this exact version (.github/workflows/build.yml).
CLANG_FORMAT ?= clang-format
CLANG_FORMAT_VERSION_EXPECTED := 22.1.8
WINE ?= wine
TARGET := bin/networkfix.asi
DEBUG_TARGET := bin/networkfix-debug.asi
TEST_TARGET := bin/test_hooks.exe
VERSION_FILE := VERSION
VERSION := $(shell cat $(VERSION_FILE) 2>/dev/null || echo 0.0.0-dev)
DIST_DIR := dist
DIST_NAME := networkfix-$(VERSION)
# Warnings are errors for all first-party sources; vendored MinHook is
# compiled without them (upstream code, off-limits to our lint policy).
# The extra -W flags pass cleanly across release and test targets; do not
# add -Wcast-align/-Wcast-qual: PE parsing intentionally casts unaligned
# mapped bytes to struct pointers. -Wformat=2 -Wvla -Wconversion
# -Wsign-conversion -Wswitch-enum -Wcovered-switch-default do not pass yet;
# enable each once the tree is clean under it.
WARNFLAGS := -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
             -Wpointer-arith -Wredundant-decls -Wundef -Wwrite-strings \
             -Wnull-dereference -Wfloat-equal -Wdouble-promotion
MINHOOK_DIR := vendor/minhook
# Target is 32-bit only; hde64.c excluded to save ~400 LOC of compile
MINHOOK_TAG := $(shell git -C $(MINHOOK_DIR) describe --tags --exact-match 2>/dev/null)
MINHOOK_VERSION := $(if $(MINHOOK_TAG),$(patsubst v%,%,$(MINHOOK_TAG)),unknown)
# SHA256 tool for dist/verify: coreutils on Linux, Perl shasum on stock macOS.
ifneq ($(shell command -v sha256sum 2>/dev/null),)
SHA256SUM := sha256sum
else
SHA256SUM := $(shell command -v shasum >/dev/null 2>&1 && echo shasum -a 256)
endif
require_sha256 = $(if $(SHA256SUM),,$(error no sha256sum or shasum in PATH; needed by dist/verify))
MINHOOK_SRCS := $(MINHOOK_DIR)/src/buffer.c \
$(MINHOOK_DIR)/src/hde/hde32.c \
$(MINHOOK_DIR)/src/hook.c \
$(MINHOOK_DIR)/src/trampoline.c
SHELL_SCRIPTS := harness/entrypoint.sh harness/netem.sh harness/scripts/*.sh harness/drivers/*.sh harness/dxwrapper/fetch.sh
HDRS := $(wildcard src/*.h) Makefile .clang-format
CORE_SRCS := src/main.c src/hooks.c src/logging.c src/sha256.c src/pattern_matcher.c src/versions.c
SRCS := $(CORE_SRCS) $(MINHOOK_SRCS)
TEST_CORE_SRCS := test/test_hooks.c src/hooks.c src/logging.c src/sha256.c src/pattern_matcher.c src/versions.c
TEST_SRCS := $(TEST_CORE_SRCS) $(MINHOOK_SRCS)
CFLAGS := -I$(MINHOOK_DIR)/include -Isrc
LDFLAGS := -lc -lws2_32 -lshlwapi -ladvapi32 -luser32

.PHONY: all debug clean install test build-test check-zig check-clang-format format lint analyze analyze-cppcheck analyze-shellcheck verify dist sbom

# README documents bare `make` as building the release target.
.DEFAULT_GOAL := all

check-zig:
	@$(ZIG) version | grep -q "$(ZIG_VERSION_EXPECTED)" || \
		(echo "ERROR: Zig $(ZIG_VERSION_EXPECTED) required, got $$( $(ZIG) version)" && exit 1)

check-clang-format:
	@$(CLANG_FORMAT) --version | grep -q "$(CLANG_FORMAT_VERSION_EXPECTED)" || \
		(echo "ERROR: clang-format $(CLANG_FORMAT_VERSION_EXPECTED) required, got $$( $(CLANG_FORMAT) --version)" && exit 1)

all: check-zig $(TARGET)

debug: check-zig $(DEBUG_TARGET)

test: build-test
	$(WINE) $(TEST_TARGET)

build-test: check-zig $(TEST_TARGET)

$(TARGET): $(SRCS) $(HDRS)
	mkdir -p $(dir $@)
	$(ZIG) build-lib --name networkfix -femit-bin=$@ -target x86-windows-gnu -dynamic -O ReleaseSmall \
$(CFLAGS) $(LDFLAGS) \
$(MINHOOK_SRCS) -cflags $(WARNFLAGS) -- \
$(CORE_SRCS)

$(DEBUG_TARGET): $(SRCS) $(HDRS)
	mkdir -p $(dir $@)
	$(ZIG) build-lib --name networkfix-debug -femit-bin=$@ -target x86-windows-gnu -dynamic -O Debug \
$(CFLAGS) $(LDFLAGS) \
$(MINHOOK_SRCS) -cflags $(WARNFLAGS) -- \
$(CORE_SRCS)

$(TEST_TARGET): $(TEST_SRCS) $(HDRS)
	mkdir -p $(dir $@)
	$(ZIG) build-exe --name test_hooks -femit-bin=$@ -target x86-windows-gnu -O Debug \
-DNETWORKFIX_TEST=1 \
$(CFLAGS) $(LDFLAGS) \
$(MINHOOK_SRCS) -cflags $(WARNFLAGS) -- \
$(TEST_CORE_SRCS)

# Reproducible verify: build twice with fixed SOURCE_DATE_EPOCH
verify: check-zig
	$(require_sha256)
	rm -rf bin/verify1 bin/verify2; mkdir -p bin/verify1 bin/verify2
	SOURCE_DATE_EPOCH=0 $(MAKE) $(TARGET)
	cp $(TARGET) bin/verify1/networkfix.asi
	mkdir -p bin/verify2
	SOURCE_DATE_EPOCH=0 $(MAKE) $(TARGET)
	cp $(TARGET) bin/verify2/networkfix.asi
	$(SHA256SUM) bin/verify1/networkfix.asi bin/verify2/networkfix.asi
	cmp bin/verify1/networkfix.asi bin/verify2/networkfix.asi && echo "reproducible: OK"

clean:
	rm -f bin/*.asi bin/*.exe bin/*.pdb bin/*.lib 2>/dev/null; rm -f bin/verify*/*.asi 2>/dev/null || true

# Rewrite formatting in place. lint checks; format fixes.
format: check-clang-format
	$(CLANG_FORMAT) -i src/*.c src/*.h test/*.c

# Verify formatting without mutating anything. Checks the working tree
# directly (not git state), so it behaves identically locally and in CI.
lint: check-clang-format
	$(CLANG_FORMAT) --dry-run --Werror src/*.c src/*.h test/*.c

# Static analysis over first-party code; both targets must pass with zero
# findings (inline cppcheck suppressions carry an in-code justification).
analyze: analyze-cppcheck analyze-shellcheck

analyze-cppcheck:
	cppcheck --error-exitcode=2 --inline-suppr --check-level=exhaustive \
	--enable=warning,style,portability,performance,unusedFunction \
	--std=c11 --platform=win32A -D_M_IX86=600 -D__GNUC__=4 \
	-I$(MINHOOK_DIR)/include -Isrc $(CORE_SRCS) test/test_hooks.c

analyze-shellcheck:
	shellcheck -x $(SHELL_SCRIPTS)

install: $(TARGET)
	cp $(TARGET) ~/.wine/drive_c/Guild

sbom:
	@mkdir -p $(DIST_DIR)
	@echo '{"bomFormat":"CycloneDX","specVersion":"1.5","version":1,"metadata":{"component":{"name":"europa1400-networkfix","version":"$(VERSION)","type":"application"}},"components":[{"name":"minhook","version":"$(MINHOOK_VERSION)","type":"library","purl":"pkg:github/TsudaKageyu/minhook@v$(MINHOOK_VERSION)","licenses":[{"license":{"id":"BSD-2-Clause"}}]},{"name":"zig","version":"$(ZIG_VERSION_EXPECTED)","type":"application"}]}' > $(DIST_DIR)/sbom.json
	@echo "sbom: $(DIST_DIR)/sbom.json"

dist: check-zig $(TARGET) sbom
	$(require_sha256)
	@mkdir -p $(DIST_DIR)
	@rm -f $(DIST_DIR)/$(DIST_NAME).zip $(DIST_DIR)/$(DIST_NAME).sha256
	@zip -j $(DIST_DIR)/$(DIST_NAME).zip $(TARGET) LICENSE README.md CHANGELOG.md $(DIST_DIR)/sbom.json >/dev/null
	@$(SHA256SUM) $(DIST_DIR)/$(DIST_NAME).zip > $(DIST_DIR)/$(DIST_NAME).sha256
	@$(SHA256SUM) $(TARGET) >> $(DIST_DIR)/$(DIST_NAME).sha256
	@ls -lh $(DIST_DIR)/$(DIST_NAME).zip $(DIST_DIR)/$(DIST_NAME).sha256
	@cat $(DIST_DIR)/$(DIST_NAME).sha256
