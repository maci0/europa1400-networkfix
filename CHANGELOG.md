# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- `dist/networkfix-<version>.sha256` recorded build-tree paths (`dist/…zip`,
  `bin/networkfix.asi`), so `sha256sum -c` failed for anyone who downloaded the
  release assets. It now lists bare filenames and verifies where the files land.
- The SBOM fell back to the literal version `unknown` whenever the MinHook
  submodule had no tag, which is what a shallow CI checkout gives. It now falls
  back to the submodule commit and `make sbom` fails outright if it can read
  neither.
- CI dropped the harness image smoke build. It could never pass: the image
  `COPY`s `setup_the_guild_gold_2.0.0.5.exe`, which is not redistributable and
  is gitignored, so every checkout failed at that layer.
- CI runs the integration tests under `xvfb-run` instead of backgrounding
  `Xvfb :99 &` before `make test`. The old form raced the server startup and
  left an orphaned Xvfb holding the step's stdout.

### Changed

- The SBOM declares the project's own licence (`GPL-3.0-only`) and lists zig
  under `metadata.tools` instead of as a shipped component.
- `make install` takes a `GAME_DIR` override and fails with a usable message
  when the target directory is missing, instead of hardcoding
  `~/.wine/drive_c/Guild`.
- `harness/.dockerignore` limits the build context to the installer,
  `entrypoint.sh` and `drivers/`. `logs/`, `artifacts/` and the fetched
  dxwrapper DLLs used to add about 1 GB to every `docker build`.
- dependabot tracks the harness base image (`harness/Dockerfile`) monthly.

## [0.4.1] - 2026-08-26

### Changed

- cppcheck is pinned to 2.17.1, closing the last unpinned gate. It publishes
  no binary release, so CI and the documented local override both run it from
  the `cppcheck-wheel` PyPI package (1.5.1 ships 2.17.1). `make analyze` now
  means the same thing on every machine.
- CI: bumped actions/checkout to v7.0.1, actions/cache to v6.1.0,
  actions/upload-artifact to v7.0.1, softprops/action-gh-release to v3.0.2 and
  actions/attest-build-provenance to v4.2.2. Rewriting history made dependabot
  close its five open bump PRs, so they are applied directly here.
- CI: raised the job timeout from 15 to 30 minutes. A normal run takes about
  3, but the wine+wine32+xvfb apt install passed 12 on a slow mirror and
  cancelled the v0.4.0 tag build.

## [0.4.0] - 2026-08-26

### Fixed

- `hooks.c`: the `hook_send` retry wait now pumps pending window messages
  (`PeekMessage`/`TranslateMessage`/`DispatchMessage`). Previously a full send
  buffer stalled the UI thread for up to ~5 s per call with no message pump,
  freezing the multiplayer loading screen for minutes during save/map sync
  (verified via `harness/`). The progress dialog now stays responsive while
  the buffer drains; retry semantics are unchanged and pinned by tests.
- `hooks.c`: honor the documented `game.ini` `ServerPath` fallback: a missing,
  unsafe, or unloadable configured path now retries `Server\server.dll` instead
  of aborting initialization (docs/configuration.md "Default behavior" and
  Example 3). An empty value (`ServerPath=""` / bare) counts as absent and no
  longer shadows the legacy `Server` key. End-to-end Wine test pins the
  fallback against a real server.dll fixture.
- `docs/configuration.md`: `NETWORKFIX_DISABLE` row claimed it keeps
  "TCP_NODELAY injection setup" alive; the code skips NODELAY when the fix is
  disabled (as its own table row already said). Row now matches behavior.
- `hooks.c`: keep `TCP_NODELAY` behind `g_fix_active`. Without it
  `NETWORKFIX_DISABLE=1` still disabled Nagle, so the A/B baseline was a
  half-patched build and every comparison against it was invalid. A test now
  drives `maybe_set_nodelay` against a real socket and reads the option back
  with `getsockopt`.

### Changed

- Static analysis tightened: three more compiler warnings joined the
  warnings-as-errors set (`-Wnull-dereference`, `-Wfloat-equal`,
  `-Wdouble-promotion`, all clean across release and test targets), and
  `make analyze-cppcheck` now also enables the `unusedFunction` checker.
  New inline suppressions carry justifications: `DllMain` is loader-called,
  and the `test_sleep`/`test_pump_messages` hooks are only called under
  `NETWORKFIX_TEST`, which analysis intentionally does not define.
- `hooks.c`, `logging.c`: the recv WSAEWOULDBLOCK diagnostic no longer issues
  an `ioctlsocket` probe per poll tick just to feed a rate-limited log line.
  A new `log_msg_rate_gate` lets callers ask the limiter whether a message is
  due before gathering it, so the probe runs at most once per 5 s (and never
  when logging is down) instead of on every would-block event of the game's
  recv poll loop.
- `hooks.c`: reuse the version-detection SHA256 of the loaded server.dll as
  the TOCTOU post-load hash, dropping one redundant full-file hash per start.
- `sha256.c`: hash files in 16 KiB chunks instead of 4 KiB, cutting read/hash
  syscalls per file 4x on the startup path.
- `hooks.c`: require a directory separator in the game-dir containment check
  so `C:\Guild` no longer matches `C:\GuildExtra\…`; restore the
  `server.dll` Sleep IAT on unload and on `MH_EnableHook` failure; bound the
  IAT walk to `SizeOfImage`; treat truncated `GetModuleFileName` results as
  failure.
- `logging.c`: initialize `log_file` to `INVALID_HANDLE_VALUE` and tear down
  the critical section on every `init_logging` failure path.
- `main.c`: skip join/cleanup on process-exit `DLL_PROCESS_DETACH`
  (`lpReserved != NULL`) to avoid loader deadlocks.
- Docs now match the running code: env-var table, relative-only `ServerPath`,
  `NETWORKFIX_DISABLE` as pass-through (not "hooks off"), fastsync independent
  of the disable flag, ASI loading via dxwrapper under Wine, and harness
  geometry `1152x864` (`cur_res=2`).
- `hooks.c`: `env_once()` replaces three hand-rolled copies of the interlocked
  lazy-init used by the `TCP_NODELAY`, tiny-buffer and net-trace gates.
- `pattern_matcher.c`: `_Static_assert` the pattern/mask lengths and the two
  rel32 branch offsets, so reordering the pattern cannot silently desync the
  indices; the prologue size gate widens to 64-bit before the addition, where
  a near-4G `rva_offset` used to wrap `size_t` and pass.
- Named constants replace the last magic numbers: `SHA256_HEX_SIZE` (was a
  literal 65 in eleven places), `NET_TRACE_MAX_BYTES`, `SERVER_PUMP_SLEEP_MS`,
  `FASTSYNC_SLEEP_MS`, `INIT_THREAD_JOIN_TIMEOUT_MS`, `PROLOGUE_MIN_BYTES`.
- `make lint` now enforces a trailing newline (`InsertNewlineAtEOF`); nine
  tracked files were missing one.
- clang-format is pinned to 22.1.8 and checked like Zig already was. `make
  lint` previously ran whatever the runner image happened to ship, so a
  version bump under CI could fail `main` with no change on our side; it did,
  on this release. CI installs the pinned version via uvx.
- shellcheck is pinned to 0.11.0 the same way, installed in CI from the
  upstream release with a checksum. Check IDs move between releases, so the
  apt build reported SC2317/SC2015 on scripts that pass here. cppcheck stays
  on apt in this release; the Makefile and development guide say so rather
  than implying the whole gate is reproducible.
- `harness/drivers/common.sh`: `hide_lua_console` used `A && B || C`, which
  runs C when B fails too. Rewritten as an `if`.
- `make analyze` was never documented; the development guide now covers it,
  along with the corrected 100-column limit.
- Docs: removed a README diagnostic step that told readers to uncomment debug
  lines that do not exist, corrected the test count, collapsed two duplicate
  documentation indexes, and replaced the "manual testing TODO" and its
  contradicting "proven over VPN" feature claim with one section stating what
  is verified and that performance impact is unmeasured.
- `harness/artifacts/`: the reproduce instructions named a script and a
  compose file that no longer exist and a WINEPREFIX under a personal home
  directory; three screenshots filed as Network-screen stills actually show
  the Tutorial screen and the Graphics Options dialog; the
  `mp_harness_loss{0,10,25}.csv` files are the same 111-byte pass/fail
  summary rather than per-loss measurements. All relabelled to match.

### Added

- Tests for `ServerPath` preference, `is_safe_server_path`, and
  `path_is_within_dir` prefix-sibling rejection.
- Harness A/B runner (`harness/scripts/ab.sh`) + `docker-compose.ab.yml`.
  Freezes client game+wineserver on the first host `server.dll` send
  `len>28`. A 25 s freeze on the 128 B payload did not desync either arm.

### Removed

- `hooks.c`: the `GetTickCount` hook. It forwarded to the original
  unconditionally (no timing behavior changed, per docs/architecture.md) and
  its NULL-fallback branch was unreachable: a failed hook creation aborts
  initialization before any hook is enabled. Dropping it removes a pointless
  trampoline hop on every `GetTickCount` call in the process; the two init log
  lines that mentioned it are gone with it.
- `handoff.md`: agent handover note. Everything in it is covered by
  `harness/README.md`, `harness/LUA_INTEGRATION.md` or the drivers.
- `harness/artifacts/`: `mp_harness.exe` (a committed build output) and seven
  byte-identical screenshot copies.

## [0.3.0] - 2026-08-10

### Fixed

- `harness/entrypoint.sh`: add Xwayland `:92` accelerated path (`USE_XWAYLAND=1`, host socket `/tmp/.X11-unix/X92`, `radeonsi`), fix trap cleanup for host-X passthrough, keep `gilde-net` isolated; verified via `Xwayland :92 -geometry 1280x1024` (host `wayland-0`) vs `Xvfb` `llvmpipe`: `0x42980D` (`fcn.00429800` poll on `6b7e94` evt table) crashes ~10 s on plain Xvfb headless but survives 30 s on accelerated Xwayland (see `harness/README.md`, `harness/artifacts/VIDEO_INDEX.md`).
- `harness/drivers/host.sh`: restore `Network` keyboard navigation (`Down×3→Return` from main menu) with `LOG_DIR` fix (`${LOG_DIR:-/tmp}`), 8 s warm-up for evt table, stay-on-Network (no `Escape` back to avoid `evt:console` teardown fault); `client.sh` split to `Xvfb :99` to avoid X contention.

### Added

- `harness/docker-compose.xwayland.yml`: overlay for dedicated `Xwayland :92` (host GPU) with `privileged: true` + `/dev/dri` for container `radeonsi` (was `llvmpipe` `amdgpu_get_auth` fail).
- `harness/scripts/start-xwayland.sh`: helper to start `Xwayland :92` (own root, `host GPU`).
- `harness/artifacts/`: `proof_xwayland_*.mp4`, `proof_host_xwayland_final.mp4`, screenshots, `VIDEO_INDEX.md`.

### Changed

- `harness/README.md`: document Xvfb vs Xwayland `:92` paths and `0x42980D` headless note.
- `.github/workflows/build.yml`: add harness image smoke build (`docker build -t gilde-harness:ci harness/`).
- `Makefile`: `clean` no longer deletes `bin/verify*/`, `verify` recreates dirs after clean (fix `bin/verify2` missing), reproducible `OK` (`43418974`).

### Verification

- `rizin` validates `0x42980D` is `fcn.00429800` (`6b7e94` evt table, `00429920` allocator), `server.dll` `GOG 0x3960` / `Steam 0x3720`, `make test` all passed, `make verify` reproducible.



## [0.2.0] - 2026-08-09

### Fixed

- `pattern_matcher.c`: fix `SRV_GAMESTREAMREADER_MASK` JZ entry (`FF FF 00 00 FF 00` → `FF FF 00 00 00 00`) and
  `validate_function_prologue` offsets (`18/28` → `17/27`) with signed `int32_t`/`int64_t` to avoid overflow on large
  relative jumps; validated via `rizin` decomp of `server.dll` (`GOG 3cc2ce9` @ `0x3960`, `Steam b341730` @ `0x3720`,
  `JE/JNE 0x10003B84`, `SizeOfImage 0x1779000`, `WSAEWOULDBLOCK 0x2733`).
- `hooks.c`: cap `SEND_MAX_RETRIES` to `5000` (`~5 s` at `1 ms`) to avoid `INT_MAX` signed overflow and infinite hang;
  fix `PathCombineA` overlapping-buffer UB via temp buffer; add `NULL` guard in `create_hook_api`; support both
  `ServerPath` and `Server` INI keys; add `is_safe_server_path` + canonical-path-within-game-dir check and remove
  legacy `Sleep(100)` after `LoadLibraryA`; add `g_test_force_caller_server` test hook and cleanup on `MH_Initialize`/
  `create_hooks`/`MH_EnableHook` failure (`MH_Uninitialize` + `reset_server_globals`).
- `logging.c`: fix missing `LeaveCriticalSection` on early returns in `logf`; guard `logf_rate_limited` and
  `log_socket_buffer_info` against races (CS protection for `rate_limit_cache`/`last_logged_socket`); validate
  `GetModuleFileNameW`/`PathRemoveFileSpecW`/`wcscat_s` and `NULL` `hModule` in `init_logging`.
- `sha256.c`: add `NULL`/size checks, fallback `PROV_RSA_AES` → `PROV_RSA_FULL` on `NTE_BAD_PROV_TYPE`, strict
  `ReadFile` error handling.
- `test/test_hooks.c`: fix stale mask in uniqueness test, correct `build_valid_prologue` offsets to `17/27`,
  isolate SHA tests via temp-file helpers, add `WSAGetLastError` propagation check, known empty-file SHA vector,
  and `is_caller` passthrough coverage.
- `src/versions.h`/`src/versions.c`: move `known_versions[]` out of header (`extern`).
- `Makefile`: remove `format` from `all`/`debug`, add `check-zig` (expect `0.16.0`), header deps, `format`/`lint`/`verify`
  targets, drop `hde64.c` (32-bit only).

### Changed

- `docs/configuration.md`: `SEND_MAX_RETRIES` now `5000`, `LOG_RATE_LIMIT_MS` `5000`, Zig `0.16.0`.
- `docs/development-guide.md`, `README.md`: Zig `0.11.0` → `0.16.0` (pinned).
- `docs/architecture.md`: correct `MH_CreateHookApi` flow and `pattern→SHA256→fail-closed` detection, update
  `is_caller_from_server` to cached range.
- `Makefile`: `VERSION` `0.2.0` source of truth for packaging.

### Added

- `.github/workflows/build.yml`: `concurrency`, `timeout`, Zig cache, `Verify ASI`, rolling `latest` + versioned `v*`
  release splits, pinned `actions/*@v4`/`softprops@v2`, artifact `retention-days`/`if-no-files-found`.
- Packaging via `make dist` → `dist/networkfix-<version>.zip` + `.sha256` + `sbom.json`.

## [0.1.0] - 2026-01-06

- Initial ASI plugin with MinHook hooks for `recv`/`send`/`GetTickCount`/`srv_gameStreamReader`, pattern matcher,
  SHA256 version detection, and Wine integration tests (`make test`).

[Unreleased]: https://github.com/maci0/europa1400-networkfix/compare/v0.4.1...HEAD
[0.4.1]: https://github.com/maci0/europa1400-networkfix/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/maci0/europa1400-networkfix/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/maci0/europa1400-networkfix/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/maci0/europa1400-networkfix/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/maci0/europa1400-networkfix/releases/tag/v0.1.0
