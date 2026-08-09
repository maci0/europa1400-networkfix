#!/bin/bash
# Client driver: waits for window, joins lobby at host IP.
set -euo pipefail
LOG="${LOG_DIR:-/tmp}/driver.log"
DISPLAY="${DISPLAY:-:99}"
export DISPLAY
HOST_IP="${HOST_IP:-gilde-host}"
echo "[driver:client] DISPLAY=$DISPLAY HOST_IP=$HOST_IP waiting for window..." | tee -a "$LOG"

for i in $(seq 1 60); do
  WID=$(xdotool search --onlyvisible --name "Europa|Gilde|Guild" 2>/dev/null | head -n1 || true)
  if [[ -n "${WID:-}" ]]; then echo "[driver:client] Found window $WID" | tee -a "$LOG"; break; fi
  WID=$(xdotool search --onlyvisible --class "wine" 2>/dev/null | head -n1 || true)
  if [[ -n "${WID:-}" ]]; then echo "[driver:client] Found wine window $WID" | tee -a "$LOG"; break; fi
  sleep 1
done
WID="${WID:-}"
if [[ -z "$WID" ]]; then
  echo "[driver:client] No window, dumping tree" | tee -a "$LOG"
  xdotool search --name "" 2>&1 | head -n 20 | tee -a "$LOG" || true
  xwininfo -root -tree 2>&1 | head -n 60 | tee -a "$LOG" || true
  exit 0
fi
xdotool windowactivate --sync "$WID" 2>/dev/null || true; sleep 1
read -r WX WY WW WH < <(xdotool getwindowgeometry --shell "$WID" 2>/dev/null | awk -F= '/X=/{x=$2} /Y=/{y=$2} /WIDTH/{w=$2} /HEIGHT/{h=$2} END{print x, y, w, h}')
WW="${WW:-1280}"; WH="${WH:-1024}"
echo "[driver:client] geom $WX $WY ${WW}x${WH} HOST_IP=$HOST_IP" | tee -a "$LOG"
# Give host time to start lobby
sleep 8
xdotool key --window "$WID" Escape 2>/dev/null || true; sleep 1
xdotool key --window "$WID" Escape 2>/dev/null || true; sleep 1
xdotool mousemove --window "$WID" $((WW/2)) $((WH/2)) 2>/dev/null || true; sleep 0.3; xdotool click --window "$WID" 1 2>/dev/null || true; sleep 2
echo "[driver:client] Best-effort join clicks done." | tee -a "$LOG"
while kill -0 "${GAME_PID:-1}" 2>/dev/null; do sleep 5; done
