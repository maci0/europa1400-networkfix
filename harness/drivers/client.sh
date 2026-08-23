#!/bin/bash
# Client driver: joins the host's multiplayer game via in-process lua input
# injection (see host.sh header for why xdotool clicks don't work).
# Server discovery is LAN broadcast inside gilde-net — no IP entry needed.
set -uo pipefail
# shellcheck source-path=SCRIPTDIR
if [ -f "$(dirname "$0")/common.sh" ]; then . "$(dirname "$0")/common.sh"; fi
LOG_DIR="${LOG_DIR:-/tmp}"
LOG="${LOG_DIR}/driver.log"
DISPLAY="${DISPLAY:-:99}"
export DISPLAY
HOST_IP="${HOST_IP:-gilde-host}"
echo "[driver:client] DISPLAY=$DISPLAY HOST_IP=$HOST_IP waiting for game window..." | tee -a "$LOG"

WID=$(wait_window || true)
if [[ -z "${WID:-}" ]]; then
  echo "[driver:client] No window found after 60s" | tee -a "$LOG"
  exit 0
fi
echo "[driver:client] Found window $WID" | tee -a "$LOG"
hide_lua_console
xdotool windowmove "$WID" 0 0 2>/dev/null || true
sleep 2

if ! wait_lua_ready; then
  echo "[driver:client] lua command loop not ready — cannot drive join" | tee -a "$LOG"
  shot client_no_lua
  exit 0
fi

# Host needs ~50s from boot to lobby; open the browser slightly before that
echo "[driver:client] lua ready — heading to server browser" | tee -a "$LOG"
sleep 30

lua_do "click(585,516)"; sleep 3                              # Network
lua_do "click(575,468)"; sleep 4; shot client_browser         # Join An Existing Game
# Event-driven connect: server list entry area (x460 y380 250x25) fills once the
# host lobby broadcasts; then select + Connect, and the Connect button region
# vanishing tells us we entered the lobby.
joined=0
for attempt in 1 2 3 4 5 6; do
  echo "[driver:client] connect attempt $attempt" | tee -a "$LOG"
  lua_do "click(560,390)"; sleep 1                            # select server entry
  base=$(crop_md5 340 675 130 30)                             # Connect button area, pre-click
  lua_do "click(405,730)"                                     # Connect
  for _ in 1 2 3 4 5 6; do
    sleep 2
    if [ "$(crop_md5 340 675 130 30)" != "$base" ]; then joined=1; break; fi
  done
  [[ "$joined" == "1" ]] && break
  lua_do "click(578,730)"; sleep 4                            # Refresh, retry
done
shot client_lobby
if [[ "$joined" == "1" ]]; then echo "[driver:client] joined lobby" | tee -a "$LOG"; else echo "[driver:client] join not confirmed — trying Ready anyway" | tee -a "$LOG"; fi
sleep 4   # lobby settle before Ready registers
# Ready with confirmation: OWN row (second row, y205) gains "** Ready **".
# Row 1 is the host's row — watching it false-confirms on the host's marker.
for r in 1 2 3 4 5; do
  echo "[driver:client] clicking Ready (attempt $r)" | tee -a "$LOG"
  base=$(crop_md5 200 205 350 28)
  lua_do "click(705,544)"
  ok=0
  for _ in 1 2 3; do
    sleep 2
    if [ "$(crop_md5 200 205 350 28)" != "$base" ]; then ok=1; break; fi
  done
  [[ "$ok" == "1" ]] && { echo "[driver:client] Ready confirmed (own row)" | tee -a "$LOG"; break; }
done
shot client_ready

# --- in-game: wait for town data, then play and watch for desync ---
echo "[driver:client] waiting for in-game (town data)" | tee -a "$LOG"
sleep 25
if dismiss_year_scroll; then echo "[driver:client] in-game (town view)" | tee -a "$LOG"; shot client_ingame
else echo "[driver:client] year scroll not dismissed" | tee -a "$LOG"; fi
if [[ "${ENDURANCE:-0}" == "1" ]]; then
  endurance_run "$WID"
  echo "[driver:client] ENDURANCE ended (see endur_fail_* shots + log)" | tee -a "$LOG"
elif play_town "${PLAY_ITERS:-30}" "$WID"; then
  echo "[driver:client] gameplay completed, session healthy" | tee -a "$LOG"
else
  echo "[driver:client] gameplay ended early (session lost/desync)" | tee -a "$LOG"
fi
shot client_final

while kill -0 "${GAME_PID:-1}" 2>/dev/null; do
  echo "[driver:client] heartbeat $(date -Iseconds)" >>"$LOG" 2>/dev/null || true
  sleep 15
done
