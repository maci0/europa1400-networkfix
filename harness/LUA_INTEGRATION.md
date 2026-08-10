# Lua harness integration (sister project europa1400-lua)

The `europa1400-lua` console (`luaapi.asi`) can run alongside `networkfix.asi` — both are ASI
plugins auto-loaded from `C:\\Guild`.

## Current wiring

`harness/entrypoint.sh` checks `LUA_CONSOLE=1` + `/harness/luaapi.asi` (mount host `europa1400-lua/bin/luaapi.asi`
and `scripts/lua/`):

    LUA_CONSOLE=1 docker compose -f harness/docker-compose.yml up --abort-on-container-exit

`harness/Dockerfile` now installs `wmctrl`/`scrot` + keeps `python3-pip` for optional `check_frame.py`
(OpenCV not required for basic run; `Pillow` is fallback).

## Planned: hybrid driver

`harness/drivers/host.sh` keeps `xdotool` (windowactivate → windowmove 0,0 → warm-up 8s → Down×3→Return)
as the primary path — it already handles `1024×768` (`1030×748` game@0,0, `geom 0 0` verified both peers).
Lua is reserved for *state checks*: `ui.find("Network")`, `system.window_info`, `watch.poll` on menu
selection index — once `luaapi` exposes a named pipe/flag, `lua_probe()` in the drivers will call
`game.call("ui_probe")` instead of blind key repeats.

See `europa1400-lua/scripts/lua/ui.lua` — `ui.windows()/ui.find()` sugar.
