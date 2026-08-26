#!/bin/bash
set -euo pipefail

# Isolated Xvfb + optional xdotool driver launcher.
# Contracts:
#   ROLE=host|client  (default host)
#   GC_NETWORK_DELAY / LOSS / REORDER / CORRUPT / RATE  (netem, applied to eth0 inside container)
#   GAME_ARGS extra args for Europa1400Gold_TL.exe
#   DRIVER=auto|none|file.sh  (default auto - runs drivers/<ROLE>.sh via xdotool if present)

ROLE="${ROLE:-host}"
WINEARCH="${WINEARCH:-win32}"
export WINEARCH
DISPLAY_NUM="${DISPLAY:-:99}"
WINEPREFIX="${WINEPREFIX:-/home/gilde/pfx}"
GAMEDIR="${GAMEDIR:-C:\\Guild}"
GAME_EXE="${GAME_EXE:-Europa1400Gold_TL.exe}"
DRIVER="${DRIVER:-auto}"
LOG_DIR="/home/gilde/logs"
export LOG_DIR

mkdir -p "$LOG_DIR"
if [[ "${CLEAN_PREFIX:-0}" != "0" ]]; then
  echo "[entrypoint] CLEAN_PREFIX=1: fresh prefix" | tee -a "$LOG_DIR/entrypoint.log" 2>&1 || true
  rm -rf "$WINEPREFIX"; mkdir -p "$WINEPREFIX"
  xvfb-run --auto-servernum wine wineboot --init 2>&1 | tail -n 20 | tee -a "$LOG_DIR/entrypoint.log" 2>&1 || true
  for cand in "/harness/setup_the_guild_gold_2.0.0.5.exe" "/tmp/setup.exe"; do if [[ -f "$cand" ]]; then xvfb-run --auto-servernum wine "$cand" /VERYSILENT '/DIR=C:\Guild' 2>&1 | tail -n 40 | tee -a "$LOG_DIR/entrypoint.log" 2>&1 || true; break; fi; done
fi

# --- traffic control (requires NET_ADMIN, apply to eth0 inside netns) ---
# Prefer a tbf token-bucket (no sch_netem needed) when GC_TBF_RATE is set.
# Fall back to netem when GC_DELAY/LOSS/REORDER/CORRUPT/RATE are set.
if [[ -n "${GC_TBF_RATE:-}" ]]; then
  echo "[entrypoint] Applying tbf to eth0: rate=${GC_TBF_RATE} burst=${GC_TBF_BURST:-4kb} latency=${GC_TBF_LAT:-400ms}" | tee -a "$LOG_DIR/netem.log"
  if ! tc qdisc replace dev eth0 root tbf rate "$GC_TBF_RATE" burst "${GC_TBF_BURST:-4kb}" latency "${GC_TBF_LAT:-400ms}" 2>&1 | tee -a "$LOG_DIR/netem.log"; then
    echo "[entrypoint] tc tbf failed (need --cap-add=NET_ADMIN)" | tee -a "$LOG_DIR/netem.log"
  fi
  tc qdisc show dev eth0 | tee -a "$LOG_DIR/netem.log" || true
elif [[ -n "${GC_DELAY:-}" || -n "${GC_LOSS:-}" || -n "${GC_REORDER:-}" || -n "${GC_CORRUPT:-}" || -n "${GC_RATE:-}" ]]; then
  echo "[entrypoint] Applying netem to eth0: delay=${GC_DELAY:-0} loss=${GC_LOSS:-0} ..."
  NETEM_ARGS=()
  [[ -n "${GC_DELAY:-}" ]] && NETEM_ARGS+=(delay "$GC_DELAY")
  [[ -n "${GC_LOSS:-}" ]] && NETEM_ARGS+=(loss "$GC_LOSS")
  [[ -n "${GC_REORDER:-}" ]] && NETEM_ARGS+=(reorder "$GC_REORDER")
  [[ -n "${GC_CORRUPT:-}" ]] && NETEM_ARGS+=(corrupt "$GC_CORRUPT")
  [[ -n "${GC_RATE:-}" ]] && NETEM_ARGS+=(rate "$GC_RATE")
  if [[ ${#NETEM_ARGS[@]} -gt 0 ]]; then
    tc qdisc replace dev eth0 root netem "${NETEM_ARGS[@]}" 2>&1 | tee -a "$LOG_DIR/netem.log" || echo "[entrypoint] tc netem failed (need --cap-add=NET_ADMIN or sch_netem)" | tee -a "$LOG_DIR/netem.log"
    tc qdisc show dev eth0 | tee -a "$LOG_DIR/netem.log" || true
  fi
fi

# --- game.ini tweaks (idempotent) ---
GAME_INI="$WINEPREFIX/drive_c/Guild/game.ini"
if [[ -f "$GAME_INI" ]]; then
  # Use CRLF-aware sed; keep reference tweaks: DIRECTWINDOW, no intro, 1152x864 via cur_res=2
  sed -i 's/^\([Bb]ildmodus=\).*$/\1DIRECTWINDOW\r/' "$GAME_INI" 2>/dev/null || true
  sed -i 's/^\([Ss]how_intro=\).*$/\1 0\r/' "$GAME_INI" 2>/dev/null || true
  sed -i 's/^\([Cc]ur_res=\).*$/\12\r/' "$GAME_INI" 2>/dev/null || true
  # networkfix.asi rejects absolute server paths, so force the relative default
  sed -i 's/^\([Ss]erver\([Pp]ath\)\?=\).*$/\1Server\\server.dll\r/' "$GAME_INI" 2>/dev/null || true
  # mouse_speed=256 = 1:1 pointer scaling; anything else breaks synthetic input coords
  sed -i 's/^\(mouse_speed=\).*$/\1256\r/' "$GAME_INI" 2>/dev/null || true
  echo "[entrypoint] Patched $GAME_INI" | tee -a "$LOG_DIR/entrypoint.log"
fi

# --- copy harness ASI + dxwrapper POW2 fix into game dir ---
HARNESS_ASI_SRC="/harness/networkfix.asi"
if [[ -f "$HARNESS_ASI_SRC" ]]; then
  cp -f "$HARNESS_ASI_SRC" "$WINEPREFIX/drive_c/Guild/networkfix.asi" 2>/dev/null || cp -f "$HARNESS_ASI_SRC" ./networkfix.asi || true
  echo "[entrypoint] Installed $HARNESS_ASI_SRC" | tee -a "$LOG_DIR/entrypoint.log"
fi
# dxwrapper D3D8 POW2 fix (elishacloud/dxwrapper v1.7.8400.25, SetPOW2Caps=1)
# Fixes Gold-Guild-Patch UI bug: missing D3DCAPS8.TextureCaps POW2 on Win10+ → mis-sized surfaces
for f in d3d8.dll dxwrapper.dll dxwrapper.ini; do
  if [[ -f "/harness/$f" ]]; then
    cp -f "/harness/$f" "$WINEPREFIX/drive_c/Guild/$f" 2>/dev/null || cp -f "/harness/$f" "./$f" || true
    echo "[entrypoint] Installed dxwrapper $f" | tee -a "$LOG_DIR/entrypoint.log"
  fi
done
# Also support harness-provided dxwrapper.fixed.ini as override
if [[ -f "/harness/dxwrapper.fixed.ini" ]]; then
  cp -f "/harness/dxwrapper.fixed.ini" "$WINEPREFIX/drive_c/Guild/dxwrapper.ini" 2>/dev/null || true
  echo "[entrypoint] Installed dxwrapper.fixed.ini -> dxwrapper.ini" | tee -a "$LOG_DIR/entrypoint.log"
fi

# --- X server: in-container only. X_BACKEND=weston (default: headless weston + rootful Xwayland,
# needed for the RandR mode list the game's adapter enumeration requires; GPU via /dev/dri if present,
# llvmpipe otherwise) | xvfb (debug only; game init fails without RandR modes) ---
XVFB_LOG="$LOG_DIR/xvfb.log"
WESTON_PID=""
# 1777 keeps /tmp's sticky bit: X lock/socket files there must not be
# clobberable across users, and a plain 777 would strip that protection.
chmod 1777 /tmp 2>/dev/null || true; rm -f /tmp/.X99-lock /tmp/.X98-lock 2>/dev/null || true
if [[ "${X_BACKEND:-weston}" == "weston" ]]; then
  echo "[entrypoint] Starting weston headless + Xwayland $DISPLAY_NUM (1152x864)" | tee -a "$LOG_DIR/entrypoint.log"
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg}"
  mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
  weston --backend=headless-backend.so --renderer=gl --width=1160 --height=880 --socket=wayland-1 >"$LOG_DIR/weston.log" 2>&1 &
  WESTON_PID=$!
  for _attempt in $(seq 1 50); do [[ -S "$XDG_RUNTIME_DIR/wayland-1" ]] && break; sleep 0.2; done
  WAYLAND_DISPLAY=wayland-1 Xwayland "$DISPLAY_NUM" -geometry 1152x864 -ac -noreset >"$XVFB_LOG" 2>&1 &
  XVFB_PID=$!
else
  echo "[entrypoint] Starting Xvfb $DISPLAY_NUM (1024x768x24)" | tee -a "$LOG_DIR/entrypoint.log"
  Xvfb "$DISPLAY_NUM" -screen 0 1024x768x24 -ac +extension GLX +render -noreset >"$XVFB_LOG" 2>&1 &
  XVFB_PID=$!
fi
for _attempt in $(seq 1 30); do xdpyinfo -display "$DISPLAY_NUM" >/dev/null 2>&1 && break; sleep 0.2; done
export DISPLAY="$DISPLAY_NUM"
glxinfo -B 2>/dev/null | grep -E 'renderer string' | tee -a "$LOG_DIR/entrypoint.log" || true
echo "[entrypoint] DISPLAY=$DISPLAY (XVFB_PID=${XVFB_PID:-none} WESTON_PID=${WESTON_PID:-none})" | tee -a "$LOG_DIR/entrypoint.log"

# --- ffmpeg x11grab recording (if available) ---
FFMPEG_PID=""
if command -v ffmpeg >/dev/null 2>&1; then
  echo "[entrypoint] Starting ffmpeg x11grab $DISPLAY -> $LOG_DIR/record.mp4" | tee -a "$LOG_DIR/entrypoint.log"
  # -draw_mouse 0: Xwayland rootful fails xcb pointer queries, killing x11grab mid-run
  ffmpeg -y -video_size 1152x864 -framerate 10 -draw_mouse 0 -f x11grab -i "$DISPLAY" -vcodec libx264 -pix_fmt yuv420p -preset ultrafast -movflags +faststart "$LOG_DIR/record.mp4" >"$LOG_DIR/ffmpeg.log" 2>&1 &
  FFMPEG_PID=$!
fi
# periodic screenshots, pruned to the newest MAX_SCREENSHOTS so multi-day
# endurance runs cannot fill the disk with 5-second frames
MAX_SCREENSHOTS=240
( while true; do sleep 5; import -window root "$LOG_DIR/screenshot_$(date +%s).png" 2>/dev/null || true
    # filenames are machine-generated epoch stamps, no spaces/globs
    # shellcheck disable=SC2012
    ls -1t "$LOG_DIR"/screenshot_*.png 2>/dev/null | tail -n +$((MAX_SCREENSHOTS + 1)) | xargs -r rm -f || true
done ) &
SCREENSHOT_PID=$!

# invoked indirectly via trap below
# shellcheck disable=SC2329
cleanup() { for _pid in "${FFMPEG_PID:-}" "${SCREENSHOT_PID:-}"; do kill "$_pid" 2>/dev/null || true; done; sleep 1; exit 0; }
trap cleanup TERM INT

# --- optional lua console (sister project europa1400-lua): install BEFORE game so ASI autoloads ---
if [[ "${LUA_CONSOLE:-0}" == "1" && -f "/harness/luaapi.asi" ]]; then
  echo "[entrypoint] LUA_CONSOLE=1 installing luaapi.asi + lua scripts" | tee -a "$LOG_DIR/entrypoint.log"
  mkdir -p "$WINEPREFIX/drive_c/Guild/lua"
  cp -f /harness/luaapi.asi "$WINEPREFIX/drive_c/Guild/luaapi.asi" 2>/dev/null || true
  if [[ -d "/harness/lua" ]]; then cp -rf /harness/lua/* "$WINEPREFIX/drive_c/Guild/lua/" 2>/dev/null || true; fi
else
  if [[ "${LUA_CONSOLE:-0}" == "1" ]]; then echo "[entrypoint] LUA_CONSOLE requested but /harness/luaapi.asi missing" | tee -a "$LOG_DIR/entrypoint.log"; fi
fi

# --- launch game (dxwrapper LoadPlugins loads *.asi from game dir) ---
GAME_LOG="$LOG_DIR/game.log"
HOOK_LOG="$WINEPREFIX/drive_c/Guild/hook_log.txt"
echo "[entrypoint] Launching $GAME_EXE ROLE=$ROLE (WINEPREFIX=$WINEPREFIX) ..." | tee -a "$LOG_DIR/entrypoint.log"
# wine needs forward slashes? Use wine start /unix if needed; direct wine exe works when cwd is game dir
cd "$WINEPREFIX/drive_c/Guild" 2>/dev/null || true
# Unset SDL video driver that breaks Wine
unset SDL_VIDEODRIVER
# Load native dxwrapper d3d8.dll (POW2 fix + LoadPlugins ASI loader); builtin d3d8 would bypass it
export WINEDLLOVERRIDES="mscoree,mshtml=;d3d8=n,b"

# Use wineserver -w at end; capture PID via winedbg or just wine
set +e
# GAME_ARGS is an intentional multi-token argument list
# shellcheck disable=SC2086
wine "$GAME_EXE" ${GAME_ARGS:-} >"$GAME_LOG" 2>&1 &
GAME_PID=$!
echo "$GAME_PID" > "$LOG_DIR/wine.pid"
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

# set +e is active: capture the game's real exit status (a plain wait would
# trip errexit; || true here would clobber $? and always report success).
wait "$GAME_PID"
GAME_EXIT=$?
echo "[entrypoint] Game exited code=$GAME_EXIT" | tee -a "$LOG_DIR/entrypoint.log"
kill "$TAIL_PID" 2>/dev/null || true
if [[ -n "${DRIVER_PID:-}" ]]; then kill "$DRIVER_PID" 2>/dev/null || true; fi
# Stop the recording helpers the TERM/INT trap would otherwise miss on this path
for _pid in "${FFMPEG_PID:-}" "${SCREENSHOT_PID:-}"; do kill "$_pid" 2>/dev/null || true; done

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

# Tear down in-container X stack
if [[ -n "${XVFB_PID:-}" ]]; then kill "$XVFB_PID" 2>/dev/null || true; wait "$XVFB_PID" 2>/dev/null || true; fi
if [[ -n "${WESTON_PID:-}" ]]; then kill "$WESTON_PID" 2>/dev/null || true; wait "$WESTON_PID" 2>/dev/null || true; fi

# Screenshot on exit (if game left framebuffer)
if command -v import >/dev/null 2>&1; then
  import -window root "$LOG_DIR/final.png" 2>/dev/null || true
fi

exit "$GAME_EXIT"
