#!/bin/bash
set -euo pipefail

# Isolated Xvfb + optional xdotool driver launcher.
# Contracts:
#   ROLE=host|client  (default host)
#   GC_NETWORK_DELAY / LOSS / REORDER / CORRUPT / RATE  (netem, applied to eth0 inside container)
#   GAME_ARGS extra args for Europa1400Gold_TL.exe
#   DRIVER=auto|none|file.sh  (default auto - runs drivers/<ROLE>.sh via xdotool if present)

ROLE="${ROLE:-host}"
DISPLAY_NUM="${DISPLAY:-:99}"
WINEPREFIX="${WINEPREFIX:-/home/gilde/pfx}"
GAMEDIR="${GAMEDIR:-C:\\Guild}"
GAME_EXE="${GAME_EXE:-Europa1400Gold_TL.exe}"
DRIVER="${DRIVER:-auto}"
LOG_DIR="/home/gilde/logs"

mkdir -p "$LOG_DIR"

# --- netem (requires NET_ADMIN, apply to eth0 inside netns) ---
if [[ -n "${GC_DELAY:-}" || -n "${GC_LOSS:-}" || -n "${GC_REORDER:-}" || -n "${GC_CORRUPT:-}" || -n "${GC_RATE:-}" ]]; then
  echo "[entrypoint] Applying netem to eth0: delay=${GC_DELAY:-0} loss=${GC_LOSS:-0} ..."
  NETEM_ARGS=()
  [[ -n "${GC_DELAY:-}" ]] && NETEM_ARGS+=(delay "$GC_DELAY")
  [[ -n "${GC_LOSS:-}" ]] && NETEM_ARGS+=(loss "$GC_LOSS")
  [[ -n "${GC_REORDER:-}" ]] && NETEM_ARGS+=(reorder "$GC_REORDER")
  [[ -n "${GC_CORRUPT:-}" ]] && NETEM_ARGS+=(corrupt "$GC_CORRUPT")
  [[ -n "${GC_RATE:-}" ]] && NETEM_ARGS+=(rate "$GC_RATE")
  if [[ ${#NETEM_ARGS[@]} -gt 0 ]]; then
    tc qdisc replace dev eth0 root netem "${NETEM_ARGS[@]}" 2>&1 | tee -a "$LOG_DIR/netem.log" || echo "[entrypoint] tc netem failed (need --cap-add=NET_ADMIN)" | tee -a "$LOG_DIR/netem.log"
    tc qdisc show dev eth0 | tee -a "$LOG_DIR/netem.log" || true
  fi
fi

# --- game.ini tweaks (idempotent) ---
GAME_INI="$WINEPREFIX/drive_c/Guild/game.ini"
if [[ -f "$GAME_INI" ]]; then
  # Use CRLF-aware sed; keep reference tweaks: DIRECTWINDOW, no intro, 1024x768-ish via cur_res=2
  sed -i 's/^\([Bb]ildmodus=\).*$/\1DIRECTWINDOW\r/' "$GAME_INI" 2>/dev/null || true
  sed -i 's/^\([Ss]how_intro=\).*$/\1 0\r/' "$GAME_INI" 2>/dev/null || true
  sed -i 's/^\([Cc]ur_res=\).*$/\12\r/' "$GAME_INI" 2>/dev/null || true
  echo "[entrypoint] Patched $GAME_INI" | tee -a "$LOG_DIR/entrypoint.log"
fi

# --- copy harness ASI into game dir (volume-mounted or baked) ---
HARNESS_ASI_SRC="/harness/networkfix.asi"
if [[ -f "$HARNESS_ASI_SRC" ]]; then
  cp -f "$HARNESS_ASI_SRC" "$WINEPREFIX/drive_c/Guild/networkfix.asi" 2>/dev/null || cp -f "$HARNESS_ASI_SRC" ./networkfix.asi || true
  echo "[entrypoint] Installed $HARNESS_ASI_SRC" | tee -a "$LOG_DIR/entrypoint.log"
fi

# --- start Xvfb ---
XVFB_LOG="$LOG_DIR/xvfb.log"
echo "[entrypoint] Starting Xvfb $DISPLAY_NUM (1280x1024x24)" | tee -a "$LOG_DIR/entrypoint.log"
Xvfb "$DISPLAY_NUM" -screen 0 1280x1024x24 -ac +extension GLX +render -noreset &
XVFB_PID=$!
# Wait for socket
for i in $(seq 1 30); do
  if xdpyinfo -display "$DISPLAY_NUM" >/dev/null 2>&1; then break; fi
  sleep 0.2
done
export DISPLAY="$DISPLAY_NUM"
echo "[entrypoint] DISPLAY=$DISPLAY" | tee -a "$LOG_DIR/entrypoint.log"

# --- launch game (Wine handles ASI autoload via game dir) ---
GAME_LOG="$LOG_DIR/game.log"
HOOK_LOG="$WINEPREFIX/drive_c/Guild/hook_log.txt"
echo "[entrypoint] Launching $GAME_EXE ROLE=$ROLE (WINEPREFIX=$WINEPREFIX) ..." | tee -a "$LOG_DIR/entrypoint.log"
# wine needs forward slashes? Use wine start /unix if needed; direct wine exe works when cwd is game dir
cd "$WINEPREFIX/drive_c/Guild" 2>/dev/null || true
# Unset SDL video driver that breaks Wine
unset SDL_VIDEODRIVER

# Use wineserver -w at end; capture PID via winedbg or just wine
set +e
wine "$GAME_EXE" ${GAME_ARGS:-} >"$GAME_LOG" 2>&1 &
GAME_PID=$!
echo "[entrypoint] Wine PID $GAME_PID" | tee -a "$LOG_DIR/entrypoint.log"

# --- optional xdotool driver ---
DRIVER_PID=""
if [[ "$DRIVER" != "none" ]]; then
  DRIVER_FILE=""
  if [[ "$DRIVER" == "auto" ]]; then
    for cand in "/home/gilde/drivers/${ROLE}.sh" "/harness/drivers/${ROLE}.sh" "/home/gilde/drivers/auto.sh"; do
      if [[ -x "$cand" || -f "$cand" ]]; then DRIVER_FILE="$cand"; break; fi
    done
  elif [[ -f "$DRIVER" ]]; then
    DRIVER_FILE="$DRIVER"
  fi
  if [[ -n "${DRIVER_FILE:-}" && -f "$DRIVER_FILE" ]]; then
    echo "[entrypoint] Starting driver $DRIVER_FILE (role=$ROLE)" | tee -a "$LOG_DIR/entrypoint.log"
    bash "$DRIVER_FILE" >>"$LOG_DIR/driver.log" 2>&1 &
    DRIVER_PID=$!
  else
    echo "[entrypoint] No driver for role=$ROLE (checked $DRIVER_FILE)" | tee -a "$LOG_DIR/entrypoint.log"
  fi
fi

# --- wait for game to exit (with hook_log tail) ---
echo "[entrypoint] Waiting for game (tail hook_log)..." | tee -a "$LOG_DIR/entrypoint.log"
# Stream hook_log if it appears
( while kill -0 "$GAME_PID" 2>/dev/null; do
    if [[ -f "$HOOK_LOG" ]]; then tail -n 200 "$HOOK_LOG" > "$LOG_DIR/hook_log.tail" 2>/dev/null || true; fi
    sleep 2
  done ) &
TAIL_PID=$!

wait "$GAME_PID" || true
GAME_EXIT=$?
echo "[entrypoint] Game exited code=$GAME_EXIT" | tee -a "$LOG_DIR/entrypoint.log"
kill "$TAIL_PID" 2>/dev/null || true
if [[ -n "${DRIVER_PID:-}" ]]; then kill "$DRIVER_PID" 2>/dev/null || true; fi

# Final logs
if [[ -f "$HOOK_LOG" ]]; then
  cp -f "$HOOK_LOG" "$LOG_DIR/hook_log.txt" 2>/dev/null || true
  echo "[entrypoint] hook_log.txt lines: $(wc -l < "$HOOK_LOG" 2>/dev/null || echo ?)" | tee -a "$LOG_DIR/entrypoint.log"
  # Check for expected hook success marker
  if grep -q "Hook initialization complete\|All hooks enabled" "$HOOK_LOG" 2>/dev/null; then
    echo "[entrypoint] Hook OK" | tee -a "$LOG_DIR/entrypoint.log"
  else
    echo "[entrypoint] Hook init not confirmed (see hook_log.txt)" | tee -a "$LOG_DIR/entrypoint.log"
  fi
fi

# Keep Xvfb until exit
kill "$XVFB_PID" 2>/dev/null || true
wait "$XVFB_PID" 2>/dev/null || true

# Screenshot on exit (if game left framebuffer)
if command -v import >/dev/null 2>&1; then
  import -window root "$LOG_DIR/final.png" 2>/dev/null || true
fi

exit "$GAME_EXIT"
