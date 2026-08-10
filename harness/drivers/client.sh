#!/bin/bash
# Client driver: waits for window, joins lobby at host IP.
set -euo pipefail
# Optional common helpers (lua probe + screenshot hashing)
if [ -f "$(dirname "$0")/common.sh" ]; then . "$(dirname "$0")/common.sh"; fi
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
xdotool windowmove "$WID" 0 0 2>/dev/null || true; sleep 0.5
for _ in $(seq 1 5); do
  DBG=$(xdotool search --name "Program Error|Wine Debugger" 2>/dev/null | head -n1 || true)
  if [[ -n "$DBG" ]]; then
    echo "[driver:client] Dismissing crash dialog $DBG" | tee -a "$LOG"
    xdotool windowactivate --sync "$DBG" 2>/dev/null || true; sleep 0.5
    xdotool key --window "$DBG" Escape 2>/dev/null || true; sleep 0.5
    xdotool key --window "$DBG" Tab Tab Tab 2>/dev/null || true; sleep 0.2; xdotool key --window "$DBG" Return 2>/dev/null || true; sleep 0.5
  fi
done
read -r WX WY WW WH < <(xdotool getwindowgeometry --shell "$WID" 2>/dev/null | awk -F= '/X=/{x=$2} /Y=/{y=$2} /WIDTH/{w=$2} /HEIGHT/{h=$2} END{print x, y, w, h}')
WW="${WW:-1024}"; WH="${WH:-768}"
echo "[driver:client] geom $WX $WY ${WW}x${WH} HOST_IP=$HOST_IP" | tee -a "$LOG"
# Give host time to start lobby
sleep 8
xdotool key --window "$WID" Escape 2>/dev/null || true; sleep 1
xdotool key --window "$WID" Escape 2>/dev/null || true; sleep 1
# Try lua flag first (host writes lobby IP or marker), else coordinate click
if lua_probe "Joined"; then echo "[driver:client] lua Joined flag — skipping click" | tee -a "$LOG"
else
  for attempt in 1 2 3; do
    echo "[driver:client] join attempt $attempt: Network→Join at $WW,$WH HOST_IP=$HOST_IP" | tee -a "$LOG"
    # Center-ish click (lobby list) + keyboard fallback (Down×2 → Return for Join)
    xdotool mousemove --window "$WID" $((WW/2)) $((WH/2 + 40)) 2>/dev/null || true; sleep 0.3
    xdotool click --window "$WID" 1 2>/dev/null || true; sleep 3
    for _ in 1 2; do xdotool key --clearmodifiers --window "$WID" Down 2>/dev/null || true; sleep 0.6; done
    xdotool key --clearmodifiers --window "$WID" Return 2>/dev/null || true; sleep 4
    # If host IP field needs typing, try it
    if [ -n "${HOST_IP:-}" ] && [ "$HOST_IP" != "gilde-host" ]; then
      xdotool key --window "$WID" Tab 2>/dev/null || true; sleep 0.3
      xdotool type --window "$WID" "$HOST_IP" 2>/dev/null || true; sleep 0.3
      xdotool key --window "$WID" Return 2>/dev/null || true; sleep 2
    fi
    import -window root "${LOG_DIR:-/tmp}/screenshot_client_join${attempt}.png" 2>/dev/null || true
    sleep 2
    break
  done
  shot client_joined
fi
echo "[driver:client] join clicks done (waiting for game start)." | tee -a "$LOG"
while kill -0 "${GAME_PID:-1}" 2>/dev/null; do
  echo "[driver:client] heartbeat WID=$WID $(date -Iseconds) HOST_IP=$HOST_IP" >>"$LOG" 2>/dev/null || true
  sleep 15
done
