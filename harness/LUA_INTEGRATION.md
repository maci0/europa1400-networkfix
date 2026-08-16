# Lua harness integration (sister project europa1400-lua)

The `europa1400-lua` console (`luaapi.asi`) can run alongside `networkfix.asi`. Both
are loaded from `C:\Guild` by dxwrapper (`[Plugins] LoadPlugins=1` + `d3d8=n,b`).
Wine does not auto-load `.asi` files, and Miles only scans ASI providers when audio
init succeeds (it does not, headless).

## Current wiring

`harness/entrypoint.sh` checks `LUA_CONSOLE=1` + `/harness/luaapi.asi`. Use the lua
overlay so the sister ASI and `harness/lua/` scripts are mounted:

```bash
docker compose -f harness/docker-compose.yml -f harness/docker-compose.lua.yml \
  up --abort-on-container-exit
```

`LUA_CONSOLE=1` alone on the base compose file is not enough: `luaapi.asi` must be
present at `/harness/luaapi.asi` (the lua overlay does that).

## Input model (do not rediscover)

xdotool / XTest clicks reach the main menu but **not** submenu buttons: the game
tracks its own DirectInput cursor. Blind Escape/Return on the main menu quits the
game.

Working path: inject from inside the process. `harness/lua/init.lua` (loaded by
`luaapi.asi`) replaces the REPL with a command loop. Drivers write lua to
`/tmp/lua_cmd.lua` via `lua_do` in `drivers/common.sh`; it runs
`SetCursorPos` + `mouse_event` / `keybd_event`, and the result lands in
`/tmp/lua_out.txt`. The AllocConsole window is unmapped by `hide_lua_console`.

Requirements:
- `game.ini mouse_speed=256` (entrypoint enforces this; any other value breaks
  synthetic coordinates)
- click targets = rendered position with **y + 43** (Wine client-area offset)
- 1152×864 (`cur_res=2`; Xwayland `-geometry 1152x864`)

Host flow: Network → Start New Game → Start Game As Server → players Continue →
London → Continue → lobby → Ready. Client: Network → Join → LAN-discovered
server → Connect → Ready.

`lua_probe want` still polls `/tmp/lua_${want}.ok` when `LUA_CONSOLE=1`.
`wait_lua_ready` waits for `/tmp/lua_Ready.ok` from the command loop.
