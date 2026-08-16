# Handoff — europa1400-networkfix

## What this does
Auto-discovers `server.dll` (pattern `cmp byte [0xE]` + SHA256 in `src/pattern_matcher.c` / `src/versions.c`)
and patches the two desync vectors (`recv`/`send` WSAEWOULDBLOCK, `srv_gameStreamReader` persistent `ctx[0xE]`).
Verified against Ghidra/rizin and online sources (Ghidra 0x3720 Steam / 0x3960 GOG, rizin `0x42980D` evt poll).
Builds reproducible `43418974` (`SOURCE_DATE_EPOCH=0`, Zig 0.16.0).

## Build / test / CI
- `make` / `make debug` — Zig 0.16.0 → `bin/networkfix.asi`.
- `make verify` — reproducible `bin/verify*/`.
- `make test` — `bin/test_hooks.exe` under Wine (40+ mocks + real `server*.dll` fixtures).
- CI `.github/workflows/build.yml` → `latest` on `main`, `v*` tags.

## Harness (how you actually run it)
- **Fully headless, zero host deps:** default `X_BACKEND=weston`: in-container `weston --backend=headless --renderer=gl` + rootful `Xwayland :99 -geometry 1152x864` (`cur_res=2`). Nothing on the desktop, no host X, works on llvmpipe (CI-safe); optional GPU via `harness/docker-compose.gpu.yml` (`/dev/dri` + `RENDER_GID`). Verified 120s+ stable on both llvmpipe and radeonsi; menu renders, hooks active. Xvfb kept as debug mode only; game init NEEDS Xwayland's RandR mode list (see below).
- **ASI loading truth:** Wine does NOT auto-load `.asi`; Miles (mss32) only loads ASI providers when audio init succeeds (never headless). The harness loads `networkfix.asi` via dxwrapper `[Plugins] LoadPlugins=1` + `WINEDLLOVERRIDES="…;d3d8=n,b"` (without the override the native dxwrapper `d3d8.dll` is never used and everything downstream is inert). Check `logs/*/hook_log.txt` for "All hooks enabled successfully".
- **Required config fixes** (entrypoint does both): `game.ini` `ServerPath=Server\server.dll` relative (installer writes absolute → networkfix path-safety rejects it → init aborted), i386 GL libs in image (`libgl1:i386` etc; 32-bit wine had NO GL before; `Failed to find a suitable pixel format`).
- **A/B testing:** `NETWORKFIX_DISABLE=1 docker compose up` = baseline (same ASI, hooks stay installed, fix behaviour off — no WSAEWOULDBLOCK conversion, no send retry, no stream-reader clamp). Fastsync still patches unless `NETWORKFIX_FASTSYNC=0`. `HARNESS_EVT_GUARD=1` (default on in compose) = env-gated NULL guard on evt poll `0x429800` in networkfix.asi.
- **Fresh prefix inside the image:** `Dockerfile` `wineboot --init` + silent installer → baked `/home/gilde/pfx` (`WINEARCH=win32`, `game.ini cur_res=2` → 1152x864).
- **Hybrid Lua:** `LUA_CONSOLE=1` + mount `europa1400-lua/bin/luaapi.asi` → co-loaded from `C:\Guild` (same dxwrapper plugin loader); `drivers/common.sh` `lua_probe()` flags, `harness/LUA_INTEGRATION.md`.
- **Drivers (lua-driven, FULL LOBBY WORKS):** `docker compose -f docker-compose.yml -f docker-compose.lua.yml up` — `harness/lua/init.lua` runs a command loop inside the game (luaapi.asi); drivers `lua_do "click(x,y)"` → in-process `SetCursorPos`+`mouse_event` (XTest never reaches submenu buttons; blind Escape/Return on main menu quits game). Host: Network(585,516)→StartNew(575,431)→AsServer(575,431)→players Continue(578,558)→London(605,283)→Continue(487,821)→lobby→Ready(705,544). Client: Network→Join(575,468)→server auto-discovered (LAN broadcast, no IP entry)→Connect(405,730)→Ready. Both peers verified in one lobby + "Receiving town data" game start. REQUIREMENTS: `mouse_speed=256` (game scales mouse deltas by mouse_speed/256; entrypoint enforces), coords = rendered y+43 (wine client offset), 1152x864 (`cur_res=2`, Xwayland `-geometry 1152x864`).
- **Video:** ffmpeg x11grab `record.mp4` + 5s screenshots per container.
- **Isolated net + netem:** `gilde-net 10.10.0.0/24` (host `.2`, client `.3`); `harness/netem.sh --scenario packet-loss` or `GC_LOSS=10%` env (`CAP_NET_ADMIN`). Host kernel needs `sch_netem` (see `NETEM_NOTE.md`).

## Lua sister (europa1400-lua)
`luaapi.asi` in-process console (4463 catalog, `ui.lua` `ui.find`, 92 state wrappers, 2219 cheats). Future: `lua_probe` → real `game.call("ShowMessage")`/`system.window_info` flag.

## Headless crash chain: SOLVED
Root cause chain (each masked the next): (1) image lacked i386 GL → 32-bit wined3d had no GL (`Failed to find a suitable pixel format`); (2) Xvfb has no RandR mode list → game init `0x538850` fails at adapter/mode check (`0x41a970`, `dd:AdapterInfo` via `IDirect3D8::GetAdapterCount`) → returns 0 → `WinMain` calls shutdown `0x538220` → cleanup iterates never-allocated `d2:fileobj` table (`[0x1015ea0]`) → crash `0x46B2CC`; the old `0x42980D` was the same class (evt tick on unallocated `evt:` tables, `[0x6B7E94]`, alloc `0x429920` publishes it last, free `0x4298C8` clears it). Fixes: i386 GL libs + weston/Xwayland backend (RandR modes) + `HARNESS_EVT_GUARD` NULL guard hook (signature-checked, env-gated) in `src/hooks.c`. Result: 120s+ stable headless on llvmpipe, menu rendered, all hooks active.

## Next
Automate save-slot screen (server) + join dialog (client) via `luaapi ui.find` or template matcher (blind keys/warped clicks exhausted there), wire `lua_probe` to real `luaapi` flag, packet-drop A/B proof (`NETWORKFIX_DISABLE=1` vs default) after `sch_netem` reboot.
