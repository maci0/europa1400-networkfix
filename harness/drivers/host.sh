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
sleep 3   # let the menu settle

lua_do "click(585,516)"; sleep 3;  shot host_multiplayer      # Network
lua_do "click(575,431)"; sleep 3                              # Start New Game
lua_do "click(575,431)"; sleep 3                              # Start Game As Server
lua_do "click(578,558)"; sleep 6                              # Number of players: Continue (default 2)
lua_do "click(605,283)"; sleep 2                              # Town: London
lua_do "click(487,821)"; sleep 8; shot host_lobby             # Map: Continue -> lobby
echo "[driver:host] lobby up — waiting for client to join (watching player list)" | tee -a "$LOG"

# Event-driven: the second row of "Registered players" (x200 y205 350x30) changes
# the moment the client registers — no fixed timer
if wait_crop_change 200 205 350 30 120; then
  echo "[driver:host] client joined — waiting for its Ready marker" | tee -a "$LOG"
  # Same row changes again when the client's "** Ready **" marker appears
  if wait_crop_change 200 205 350 30 40; then
    echo "[driver:host] client is Ready" | tee -a "$LOG"
  else
    echo "[driver:host] no client Ready marker in 40s — proceeding" | tee -a "$LOG"
  fi
else
  echo "[driver:host] no join detected in 120s — clicking Ready anyway" | tee -a "$LOG"
fi
shot host_lobby_prestart
echo "[driver:host] clicking Ready" | tee -a "$LOG"
lua_do "click(705,544)"                                       # Ready -> game starts when all ready
sleep 3
shot host_ready

# --- in-game: wait for town data, then play and watch for desync ---
# Town view appears after the ~20s (fastsync) transfer + year-start scroll
echo "[driver:host] waiting for in-game (town data)" | tee -a "$LOG"
sleep 25
if dismiss_year_scroll; then echo "[driver:host] in-game (town view)" | tee -a "$LOG"; shot host_ingame
else echo "[driver:host] year scroll not dismissed (maybe not in-game yet)" | tee -a "$LOG"; fi
if play_town "${PLAY_ITERS:-30}" "$WID"; then
  echo "[driver:host] gameplay completed, session healthy" | tee -a "$LOG"
else
  echo "[driver:host] gameplay ended early (session lost/desync)" | tee -a "$LOG"
fi
shot host_final

# Keep alive while game runs; heartbeat for harness logs
while kill -0 "${GAME_PID:-1}" 2>/dev/null; do
  echo "[driver:host] heartbeat $(date -Iseconds)" >>"$LOG" 2>/dev/null || true
  sleep 15
done
