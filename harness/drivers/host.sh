#!/bin/bash
# Host driver: waits for window, creates lobby, waits for client, starts mission.
# Coordinates are 1024x768 reference; uses xdotool with --sync and window activation.
set -euo pipefail
LOG_DIR="${LOG_DIR:-/tmp}"
LOG="${LOG_DIR}/driver.log"
DISPLAY="${DISPLAY:-:99}"
export DISPLAY
echo "[driver:host] DISPLAY=$DISPLAY waiting for Europa window..." | tee -a "$LOG"

# Wait for Wine window (class or title contains Europa/Gilde/Guild)
for i in $(seq 1 60); do
  WID=$(xdotool search --onlyvisible --name "Europa|Gilde|Guild" 2>/dev/null | head -n1 || true)
  if [[ -n "${WID:-}" ]]; then echo "[driver:host] Found window $WID" | tee -a "$LOG"; break; fi
  WID=$(xdotool search --onlyvisible --class "wine" 2>/dev/null | head -n1 || true)
  if [[ -n "${WID:-}" ]]; then echo "[driver:host] Found wine window $WID" | tee -a "$LOG"; break; fi
  sleep 1
done
WID="${WID:-}"
if [[ -z "$WID" ]]; then
  echo "[driver:host] No window found after 60s, dumping windows:" | tee -a "$LOG"
  xdotool search --name "" 2>&1 | head -n 20 | tee -a "$LOG" || true
  xwininfo -root -tree 2>&1 | head -n 60 | tee -a "$LOG" || true
  exit 0
fi
xdotool windowactivate --sync "$WID" 2>/dev/null || true
sleep 1
# Move game window to 0,0 to match Xwayland/Xvfb 1024x768 root exactly (no gray border, easier automation)
xdotool windowmove "$WID" 0 0 2>/dev/null || true; sleep 0.5
# If winedbg crash dialog appears, dismiss it
for _ in $(seq 1 5); do
  DBG=$(xdotool search --name "Program Error|Wine Debugger" 2>/dev/null | head -n1 || true)
  if [[ -n "$DBG" ]]; then
    echo "[driver:host] Dismissing crash dialog $DBG" | tee -a "$LOG"
    xdotool windowactivate --sync "$DBG" 2>/dev/null || true; sleep 0.5
    xdotool key --window "$DBG" Escape 2>/dev/null || true; sleep 0.5
    xdotool key --window "$DBG" Tab Tab Tab 2>/dev/null || true; sleep 0.2; xdotool key --window "$DBG" Return 2>/dev/null || true; sleep 0.5
  fi
done

# Skip intro
xdotool key --window "$WID" Escape 2>/dev/null || true; sleep 1
xdotool key --window "$WID" Escape 2>/dev/null || true; sleep 1

# Window geometry
read -r WX WY WW WH < <(xdotool getwindowgeometry --shell "$WID" 2>/dev/null | awk -F= '/X=/{x=$2} /Y=/{y=$2} /WIDTH/{w=$2} /HEIGHT/{h=$2} END{print x, y, w, h}')
WW="${WW:-1024}"; WH="${WH:-768}"
echo "[driver:host] geom $WX $WY ${WW}x${WH}" | tee -a "$LOG"
# Longer warm-up: container wine initializes evt tables slower (poll loop at 0x42980D needs 0x6b7e94 allocated)
sleep 8
echo "[driver:host] warm-up done, proceeding to Network" | tee -a "$LOG"

# Navigate to Network via keyboard (main menu: 0 New Game, 1 Load, 2 Tutorial, 3 Network)
echo "[driver:host] navigating to Network via keyboard" | tee -a "$LOG"
xdotool windowactivate --sync "$WID" 2>/dev/null || true; sleep 0.5
# Ensure we're at top of menu: Escape back to main, then send Down 3 to reach Network
xdotool key --clearmodifiers --window "$WID" Escape 2>/dev/null || true; sleep 1
xdotool key --clearmodifiers --window "$WID" Escape 2>/dev/null || true; sleep 1
for _ in 1 2 3; do xdotool key --clearmodifiers --window "$WID" Down 2>/dev/null || true; sleep 0.8; done
sleep 0.5
xdotool key --clearmodifiers --window "$WID" Return 2>/dev/null || true; sleep 4
import -window root "${LOG_DIR:-/tmp}/screenshot_network.png" 2>/dev/null || true
echo "[driver:host] on Network screen; holding for recording." | tee -a "$LOG"
sleep 10
import -window root "${LOG_DIR:-/tmp}/screenshot_network_stay.png" 2>/dev/null || true
echo "[driver:host] Driver done (stayed on Network). Leave game running." | tee -a "$LOG"
while kill -0 "${GAME_PID:-1}" 2>/dev/null; do sleep 5; done
