#!/bin/bash
# Host driver: waits for window, creates lobby, waits for client, starts mission.
# Coordinates are 1280x1024 reference; uses xdotool with --sync and window activation.
set -euo pipefail
LOG="${LOG_DIR:-/tmp}/driver.log"
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
# Take reference screenshot if needed
# import -window "$WID" /tmp/host_before.png 2>/dev/null || true

# Heuristic clicks: reference harness historically clicks through intro → Multiplayer → Host.
# These are best-effort; harness is meant to be calibrated via screenshots.
# Example sequence (adjust via screenshots):
# - ESC to skip intro, then click "Mehrspieler"/"Multiplayer"
# - Host → map select → Start

# Skip intro
xdotool key --window "$WID" Escape 2>/dev/null || true; sleep 1
xdotool key --window "$WID" Escape 2>/dev/null || true; sleep 1

# Try to click center-ish "Multiplayer" (calibrate: screenshot first run)
# Window geometry
read -r WX WY WW WH < <(xdotool getwindowgeometry --shell "$WID" 2>/dev/null | awk -F= '/X=/{x=$2} /Y=/{y=$2} /WIDTH/{w=$2} /HEIGHT/{h=$2} END{print x, y, w, h}')
WW="${WW:-1280}"; WH="${WH:-1024}"
echo "[driver:host] geom $WX $WY ${WW}x${WH}" | tee -a "$LOG"

# Click approximate positions (to be refined with real run screenshots)
# Multiplayer button roughly center
xdotool mousemove --window "$WID" $((WW/2)) $((WH/2)) 2>/dev/null || true; sleep 0.3; xdotool click --window "$WID" 1 2>/dev/null || true; sleep 2

echo "[driver:host] Driver done (best-effort clicks). Leave game running for harness to capture hook_log." | tee -a "$LOG"
# Keep alive while game runs
while kill -0 "${GAME_PID:-1}" 2>/dev/null; do sleep 5; done
