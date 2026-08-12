#!/bin/bash
# Host driver: creates a multiplayer game (server) via in-process lua input
# injection and waits in-game. Requires LUA_CONSOLE=1 + harness lua/init.lua
# (command loop) — synthetic X clicks (xdotool/XTest) do NOT reach the game's
# DirectInput cursor on submenu screens, in-process SetCursorPos+mouse_event do.
# Coordinates: 1152x864 fullscreen, mouse_speed=256 (1:1), rendered y + 43
# (wine client-area offset) baked in.
set -uo pipefail
if [ -f "$(dirname "$0")/common.sh" ]; then . "$(dirname "$0")/common.sh"; fi
LOG_DIR="${LOG_DIR:-/tmp}"
LOG="${LOG_DIR}/driver.log"
DISPLAY="${DISPLAY:-:99}"
export DISPLAY
echo "[driver:host] DISPLAY=$DISPLAY waiting for game window..." | tee -a "$LOG"

WID=$(wait_window || true)
if [[ -z "${WID:-}" ]]; then
  echo "[driver:host] No window found after 60s" | tee -a "$LOG"
  xwininfo -root -tree 2>&1 | head -n 40 | tee -a "$LOG" || true
  exit 0
fi
echo "[driver:host] Found window $WID" | tee -a "$LOG"
hide_lua_console
xdotool windowmove "$WID" 0 0 2>/dev/null || true
sleep 2

if ! wait_lua_ready; then
  echo "[driver:host] lua command loop not ready (LUA_CONSOLE=1 + luaapi.asi mounted?) — cannot drive lobby" | tee -a "$LOG"
  shot host_no_lua
  exit 0
fi
echo "[driver:host] lua ready — creating server lobby" | tee -a "$LOG"
sleep 5   # let the menu settle

lua_do "click(585,516)"; sleep 4;  shot host_multiplayer      # Network
lua_do "click(575,431)"; sleep 4                              # Start New Game
lua_do "click(575,431)"; sleep 4                              # Start Game As Server
lua_do "click(578,558)"; sleep 8                              # Number of players: Continue (default 2)
lua_do "click(605,283)"; sleep 3                              # Town: London
lua_do "click(487,821)"; sleep 10; shot host_lobby            # Map: Continue -> lobby
echo "[driver:host] lobby up — waiting for client to join" | tee -a "$LOG"

# Give the client time to boot, browse, connect (it readies itself first)
sleep 60
shot host_lobby_prestart
echo "[driver:host] clicking Ready" | tee -a "$LOG"
lua_do "click(705,544)"                                       # Ready -> game starts when all ready
sleep 5
shot host_ready

# Keep alive while game runs; heartbeat for harness logs
while kill -0 "${GAME_PID:-1}" 2>/dev/null; do
  echo "[driver:host] heartbeat $(date -Iseconds)" >>"$LOG" 2>/dev/null || true
  sleep 15
done
