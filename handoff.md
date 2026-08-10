# Handoff — europa1400-networkfix

## What this does
Auto-discovers `server.dll` (pattern `cmp byte [0xE]` + SHA256 in `src/pattern_matcher.c` / `src/versions.c`)
and patches the two desync vectors (`recv`/`send` WSAEWOULDBLOCK, `srv_gameStreamReader` persistent `ctx[0xE]`).
Verified against Ghidra/rizin and online sources (Ghidra 0x3720 Steam / 0x3960 GOG, rizin `0x42980D` evt poll).
Builds reproducible `43418974` (`SOURCE_DATE_EPOCH=0`, Zig 0.16.0).

## Build / test / CI
- `make` / `make debug` — Zig 0.16.0 → `bin/networkfix.asi`.
- `make verify` — reproducible `bin/verify*/`.
- `make test` — `bin/test_hooks.exe` under Wine (37+ mocks + real `server*.dll` fixtures).
- CI `.github/workflows/build.yml` → `latest` on `main`, `v*` tags.

## Harness (how you actually run it)
- **Fresh prefix inside the image** (not stale wpf1): `harness/Dockerfile:37` `wineboot --init` + `wine /tmp/setup.exe /VERYSILENT /DIR=C:\\Guild` → `harness/setup_the_guild_gold_2.0.0.5.exe` cached + `WINEARCH=win32` (`PE32`). `harness/docker-compose.yml` has no `wpf1` bind — uses baked `/home/gilde/pfx` (`#arch=win32`, `game.ini cur_res=2 → 1024x768`).
- **Xvfb in same pidns as Wine:** `harness/entrypoint.sh:96` `Xvfb :99 1024x768x24 & XVFB_PID` + `xdpyinfo` wait → `DISPLAY=:99`. Accelerated: `USE_XWAYLAND=1` + host `Xwayland :92 -geometry 1024x768` (`radeonsi`, `privileged + /dev/dri`), keeps `gilde-net`. Host helper `harness/scripts/start-xwayland.sh` (`--stop` + pidfile `/tmp/xwayland:92.pid` — **stop it after use**, no orphan `:92`).
- **Hybrid Lua:** `harness/entrypoint.sh` **before game launch** copies `luaapi.asi` + `lua/` if `LUA_CONSOLE=1` + `/harness/luaapi.asi` (mount `europa1400-lua/bin/luaapi.asi` + `scripts/lua/`). Co-loads `networkfix.asi` + `luaapi.asi` from `C:\\Guild`. `harness/drivers/common.sh` `lua_probe("Network"/"HostCreated"/"Joined")` checks `/tmp/lua_*.ok` written by `harness/lua/harness_probe.lua` (`system.window_info` → `win_contains("Network")`). `harness/LUA_INTEGRATION.md` + `harness/scripts/check_frame.py` (`wmctrl`/`scrot` deps in Dockerfile).
- **Drivers:** `harness/drivers/{host,client,common}.sh` — `host: windowmove 0,0` (`geom 0,0 1030x748` both peers verified, `1024x768` root clip 6px) → `warm-up 8s` (evt `6b7e94`) → `Down×3→Return` to Network (+ blank-screenshot retry + `shot()` + lua flag) → 3× Host `512,340` clicks → `City`/`Start` → `host_lobby` heartbeat `15s`. Client waits for Network then center-click + `Down×2→Return` Join (+ `HOST_IP=gilde-host` type-in fallback) `shot client_joined`. All `common.sh` sourced, `bash -n` clean.
- **Video:** `entrypoint.sh` `ffmpeg -video_size 1024x768 -framerate 10 -f x11grab -i :99 → record.mp4` + `import -window root screenshot_*.png` + `xvfb.log` + periodic 5s shots. Artifacts in `harness/artifacts/` (`objective_1024_*.mp4`, `proof_host_xwayland_*.mp4`) + `harness/logs/{host,client}/`.
- **Isolated net:** `gilde-net 10.10.0.0/24` (host `.2`, client `.3`, ping verified).
- **Netem:** `harness/netem.sh --scenario packet-loss` → `loss 10%` (`nsenter -n -t $PID tc qdisc replace dev eth0 root netem`, also `GC_LOSS=10%` env via entrypoint `tc ... netem loss $GC_LOSS`). `gilde-net` isolated, `CAP_NET_ADMIN` on both containers. **Host quirk:** kernel `7.1.4` lacks `sch_netem.ko.zst` → `qdisc kind is unknown` → `NETEM_NOTE.md` (reboot to `7.1.6` + `modprobe sch_netem` fixes; verified config `CONFIG_NET_SCH_NETEM=m`).

## Lua sister (europa1400-lua)
`luaapi.asi` in-process console (4463 catalog, `ui.lua` `ui.find`, 92 state wrappers, 2219 cheats). Future: `lua_probe` → real `game.call("ShowMessage")`/`system.window_info` flag.

## Known quirk
`0x42980D` `fcn.00429800 cmp byte [eax],bl` on `NULL 0x6b7e94` evt table (`fcn.00429920` allocator `0x64c00`). Headless Xvfb polls ~10s on menu if evt not init; `Xwayland :92` host allocates and survives 30s on Network. `dxwrapper v1.7.8400.25 SetPOW2Caps=1` fixes earlier POW2 UI crash; this `0x42980D` is separate evt-init race — workaround is accelerated Xwayland (see `VIDEO_INDEX.md`) or longer warm-up.

## Next
Tighten Create→City→Start coord calibration against German/English menu variants, wire `lua_probe` to real `luaapi` named-pipe flag, add OpenCV template matcher for lobby vs Network, finish paired packet-drop proof after kernel reboot.
