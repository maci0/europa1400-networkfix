ZIG ?= zig
ZIG_VERSION_EXPECTED := 0.16.0
# Pinned: clang-format reflows differently between major versions, so an
# unpinned tool silently turns `make lint` into "whatever the runner shipped
# this month". CI installs this exact version (.github/workflows/build.yml).
CLANG_FORMAT ?= clang-format
CLANG_FORMAT_VERSION_EXPECTED := 22.1.8
# Pinned for the same reason: shellcheck renumbers and retunes checks between
# releases (0.9 reports SC2317 where 0.11 reports SC2329), so an unpinned
# binary makes `make analyze` mean different things on different machines.
SHELLCHECK ?= shellcheck
SHELLCHECK_VERSION_EXPECTED := 0.11.0
# Pinned likewise. cppcheck has no upstream binary release, so both CI and the
# documented local override run it from the cppcheck-wheel PyPI package
# (`uvx --from cppcheck==1.5.1 cppcheck`), which ships 2.17.1.
CPPCHECK ?= cppcheck
CPPCHECK_VERSION_EXPECTED := 2.17.1
WINE ?= wine
TARGET := bin/networkfix.asi
DEBUG_TARGET := bin/networkfix-debug.asi
TEST_TARGET := bin/test_hooks.exe
VERSION_FILE := VERSION
VERSION := $(shell cat $(VERSION_FILE) 2>/dev/null || echo 0.0.0-dev)
DIST_DIR := dist
DIST_NAME := networkfix-$(VERSION)
# Where `make install` drops the ASI: the folder holding
# Europa1400Gold_TL.exe. Override for a non-default Wine prefix, e.g.
# make install GAME_DIR="$$HOME/Games/guild/drive_c/Guild"
GAME_DIR ?= $(HOME)/.wine/drive_c/Guild
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
# Vendored MinHook revision, as recorded in the SBOM. A tagged checkout
# reports the tag; a shallow one (GitHub Actions clones submodules without
# tags) falls back to the commit, so the purl always names a resolvable
# revision instead of the string "unknown".
MINHOOK_TAG := $(shell git -C $(MINHOOK_DIR) describe --tags --exact-match 2>/dev/null)
MINHOOK_REV := $(shell git -C $(MINHOOK_DIR) rev-parse HEAD 2>/dev/null)
MINHOOK_VERSION := $(if $(MINHOOK_TAG),$(patsubst v%,%,$(MINHOOK_TAG)),$(MINHOOK_REV))
MINHOOK_PURL := pkg:github/TsudaKageyu/minhook@$(if $(MINHOOK_TAG),$(MINHOOK_TAG),$(MINHOOK_REV))
require_minhook_rev = $(if $(MINHOOK_VERSION),,$(error cannot read vendor/minhook revision; run git submodule update --init))
# SPDX id of this project's own licence (LICENSE is GPL v3 with no "or later").
LICENSE_ID := GPL-3.0-only
# SHA256 tool for dist/verify: coreutils on Linux, Perl shasum on stock macOS.
ifneq ($(shell command -v sha256sum 2>/dev/null),)
SHA256SUM := sha256sum
else
SHA256SUM := $(shell command -v shasum >/dev/null 2>&1 && echo shasum -a 256)
endif
require_sha256 = $(if $(SHA256SUM),,$(error no sha256sum or shasum in PATH; needed by dist/verify))
# Target is 32-bit only; hde64.c excluded to save ~400 LOC of compile
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

.PHONY: all debug clean install test build-test check-zig check-clang-format check-shellcheck check-cppcheck format lint analyze analyze-cppcheck analyze-shellcheck verify dist sbom

# README documents bare `make` as building the release target.
.DEFAULT_GOAL := all

check-zig:
	@$(ZIG) version | grep -q "$(ZIG_VERSION_EXPECTED)" || \
		(echo "ERROR: Zig $(ZIG_VERSION_EXPECTED) required, got $$( $(ZIG) version)" && exit 1)

check-clang-format:
	@$(CLANG_FORMAT) --version | grep -q "$(CLANG_FORMAT_VERSION_EXPECTED)" || \
		(echo "ERROR: clang-format $(CLANG_FORMAT_VERSION_EXPECTED) required, got $$( $(CLANG_FORMAT) --version)" && exit 1)

check-shellcheck:
	@$(SHELLCHECK) --version | grep -q "version: $(SHELLCHECK_VERSION_EXPECTED)" || \
		(echo "ERROR: shellcheck $(SHELLCHECK_VERSION_EXPECTED) required, got $$( $(SHELLCHECK) --version | grep version:)" && exit 1)

check-cppcheck:
	@$(CPPCHECK) --version | grep -q "Cppcheck $(CPPCHECK_VERSION_EXPECTED)" || \
		(echo "ERROR: cppcheck $(CPPCHECK_VERSION_EXPECTED) required, got $$( $(CPPCHECK) --version)" && exit 1)

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

analyze-cppcheck: check-cppcheck
	$(CPPCHECK) --error-exitcode=2 --inline-suppr --check-level=exhaustive \
	--enable=warning,style,portability,performance,unusedFunction \
	--std=c11 --platform=win32A -D_M_IX86=600 -D__GNUC__=4 \
	-I$(MINHOOK_DIR)/include -Isrc $(CORE_SRCS) test/test_hooks.c

analyze-shellcheck: check-shellcheck
	$(SHELLCHECK) -x $(SHELL_SCRIPTS)

install: $(TARGET)
	@test -d "$(GAME_DIR)" || \
		(echo "ERROR: $(GAME_DIR) does not exist; set GAME_DIR to the folder holding Europa1400Gold_TL.exe" && exit 1)
	cp $(TARGET) "$(GAME_DIR)"

# zig is the compiler, not a shipped component, so it goes under
# metadata.tools; components lists only what is linked into the ASI.
sbom:
	$(require_minhook_rev)
	@mkdir -p $(DIST_DIR)
	@echo '{"bomFormat":"CycloneDX","specVersion":"1.5","version":1,"metadata":{"tools":[{"vendor":"Zig Software Foundation","name":"zig","version":"$(ZIG_VERSION_EXPECTED)"}],"component":{"name":"europa1400-networkfix","version":"$(VERSION)","type":"application","licenses":[{"license":{"id":"$(LICENSE_ID)"}}]}},"components":[{"name":"minhook","version":"$(MINHOOK_VERSION)","type":"library","purl":"$(MINHOOK_PURL)","licenses":[{"license":{"id":"BSD-2-Clause"}}]}]}' > $(DIST_DIR)/sbom.json
	@echo "sbom: $(DIST_DIR)/sbom.json"

dist: check-zig $(TARGET) sbom
	$(require_sha256)
	@mkdir -p $(DIST_DIR)
	@rm -f $(DIST_DIR)/$(DIST_NAME).zip $(DIST_DIR)/$(DIST_NAME).sha256
	@zip -j $(DIST_DIR)/$(DIST_NAME).zip $(TARGET) LICENSE README.md CHANGELOG.md $(DIST_DIR)/sbom.json >/dev/null
	@# Bare filenames, not build-tree paths: the release ships zip, .asi and
	@# .sha256 side by side, so `sha256sum -c` has to resolve both entries in
	@# whatever directory the user downloaded (or unzipped) them into.
	@(cd $(DIST_DIR) && $(SHA256SUM) $(DIST_NAME).zip) > $(DIST_DIR)/$(DIST_NAME).sha256
	@(cd $(dir $(TARGET)) && $(SHA256SUM) $(notdir $(TARGET))) >> $(DIST_DIR)/$(DIST_NAME).sha256
	@ls -lh $(DIST_DIR)/$(DIST_NAME).zip $(DIST_DIR)/$(DIST_NAME).sha256
	@cat $(DIST_DIR)/$(DIST_NAME).sha256
