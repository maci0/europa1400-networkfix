#!/bin/bash
# Client driver: joins the host's multiplayer game via in-process lua input
# injection (see host.sh header for why xdotool clicks don't work).
# Server discovery is LAN broadcast inside gilde-net — no IP entry needed.
set -uo pipefail
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

# Host needs ~60s from boot to lobby; browse after that
echo "[driver:client] lua ready — waiting for host lobby" | tee -a "$LOG"
sleep 55

lua_do "click(585,516)"; sleep 4                              # Network
lua_do "click(575,468)"; sleep 5; shot client_browser         # Join An Existing Game
for attempt in 1 2 3; do
  echo "[driver:client] connect attempt $attempt" | tee -a "$LOG"
  lua_do "click(560,390)"; sleep 1                            # select server entry
  lua_do "click(405,730)"; sleep 8                            # Connect
  shot "client_connect${attempt}"
  # Joined when the browser is gone; crude check: Refresh button area no longer present
  # (cheap heuristic: just try Ready; a miss on the browser screen is harmless)
  lua_do "click(578,730)"; sleep 3                            # Refresh (no-op if already in lobby)
done
sleep 5
echo "[driver:client] clicking Ready" | tee -a "$LOG"
lua_do "click(705,544)"                                       # Ready
sleep 3
shot client_ready

while kill -0 "${GAME_PID:-1}" 2>/dev/null; do
  echo "[driver:client] heartbeat $(date -Iseconds)" >>"$LOG" 2>/dev/null || true
  sleep 15
done
