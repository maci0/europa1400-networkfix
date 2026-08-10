# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.3.0] - 2026-08-10

### Fixed

- `harness/entrypoint.sh`: add Xwayland `:92` accelerated path (`USE_XWAYLAND=1`, host socket `/tmp/.X11-unix/X92`, `radeonsi`), fix trap cleanup for host-X passthrough, keep `gilde-net` isolated; verified via `Xwayland :92 -geometry 1280x1024` (host `wayland-0`) vs `Xvfb` `llvmpipe` — `0x42980D` (`fcn.00429800` poll on `6b7e94` evt table) crashes ~10 s on plain Xvfb headless but survives 30 s on accelerated Xwayland (see `harness/README.md`, `harness/artifacts/VIDEO_INDEX.md`).
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

[Unreleased]: https://github.com/maci0/europa1400-networkfix/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/maci0/europa1400-networkfix/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/maci0/europa1400-networkfix/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/maci0/europa1400-networkfix/releases/tag/v0.1.0
