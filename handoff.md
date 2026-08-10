# Handoff — europa1400-networkfix

## What this does
Auto-discovers `server.dll` (pattern `cmp byte [0xE]` + SHA256 in `src/pattern_matcher.c` / `src/versions.c`)
and patches the two desync vectors (`recv`/`send` WSAEWOULDBLOCK, `srv_gameStreamReader` persistent `ctx[0xE]`).
Verified against Ghidra/rizin and online sources (Ghidra 0x3720 Steam / 0x3960 GOG, rizin `0x42980D` evt poll).

## Build / test / CI
- `make` / `make debug` — Zig 0.16.0 → `bin/networkfix.asi` / `bin/networkfix-debug.asi` (x86-windows-gnu).
- `make verify` — reproducible `43418974` (`SOURCE_DATE_EPOCH=0`).
- `make test` — `bin/test_hooks.exe` under Wine (37+ mocks + real `server*.dll` fixtures; `GE` local).
- CI `.github/workflows/build.yml` builds release + runs verify+test, ships `latest` on `main` and `v*` tags.

## Harness (how you actually run it)
- **Fresh prefix inside the image** (not stale wpf1): `harness/Dockerfile:37` `wineboot --init` + `wine /tmp/setup.exe /VERYSILENT /DIR=C:\\Guild` → `harness/setup_the_guild_gold_2.0.0.5.exe` cached + `WINEARCH=win32` (`PE32`).
  `harness/docker-compose.yml` has **no** `wpf1` bind — uses baked `/home/gilde/pfx` (`#arch=win32`, `game.ini cur_res=2 → 1024x768`).
- **Xvfb in same pidns as Wine:** `harness/entrypoint.sh:96` `Xvfb :99 1024x768x24 & XVFB_PID` + `xdpyinfo` wait → `DISPLAY=:99`.
  Accelerated fallback: `USE_XWAYLAND=1` + host `Xwayland :92 -geometry 1024x768` (host `radeonsi`, `privileged + /dev/dri`), keeps `gilde-net`.
- **Drivers:** `harness/drivers/host.sh` / `client.sh` `windowactivate → windowmove 0,0` (`geom 0,0 1030x748` both peers verified, `1024x768` root clip 6px) → `warm-up 8s` (evt `6b7e94`) → host `Down×3→Return` to Network (+ blank-screenshot retry + `shot()` + lua-probe stub), client center-click join. Shared helpers in `harness/drivers/common.sh` (`lua_probe`/`shot`/`wait_window`/hash).
- **Video:** `entrypoint.sh` `ffmpeg -video_size 1024x768 -f x11grab -i :99 → record.mp4` + `import -window root screenshot_*.png` + `xvfb.log`. Artifacts in `harness/artifacts/` + `harness/logs/{host,client}/`.
- **Netem:** `harness/netem.sh --scenario packet-loss` → `loss 10%` (`nsenter -n -t $PID tc qdisc replace dev eth0 root netem`) with `GC_LOSS=10%` env; `gilde-net 10.10.0.0/24` isolated. Current host kernel `7.1.4` lacks `sch_netem.ko.zst` → `NETEM_NOTE.md` (reboot to `7.1.6` fixes).
- **Hybrid Lua:** `harness/entrypoint.sh` before game: if `LUA_CONSOLE=1` + `/harness/luaapi.asi` (from sister `europa1400-lua`), copy `luaapi.asi` + `lua/` into `C:\\Guild`; drivers `common.sh` `lua_probe("Network"/"HostCreated"/"Joined")` checks `/tmp/lua_*.ok` flags. `harness/LUA_INTEGRATION.md` details co-load (`networkfix.asi` + `luaapi.asi`). `harness/Dockerfile` now `wmctrl`/`scrot`, `harness/scripts/check_frame.py` helper.

## Lua sister (europa1400-lua)
`luaapi.asi` in-process console (4463 catalog entries). `harness/LUA_INTEGRATION.md` explains `game.call` + `ui.*` (`ui.windows()/ui.find()` via `system.window_info`) for future state-aware driver.

## Known quirk
`0x42980D` `fcn.00429800 cmp byte [eax],bl` on `NULL 0x6b7e94` evt table (`fcn.00429920` allocator). Headless Xvfb headless poll faults ~10 s on menu if evt not init; `Xwayland :92` host path allocates and survives 30 s on Network. Documented in `harness/README.md` 0x42980D section + `VIDEO_INDEX.md`.

## Next
Wire `lua_probe` to real `luaapi` flag (ui state), add OpenCV template matcher for host lobby vs Network, tighten packet-drop paired proof.
