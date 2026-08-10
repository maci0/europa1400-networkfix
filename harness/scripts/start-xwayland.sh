#!/bin/bash
set -euo pipefail
# Start a dedicated Xwayland :92 with own 1024x768 root, accelerated via host DRI.
# Must be run on the host (not inside container), before docker compose up with xwayland overlay.
DISP=":92"
PIDFILE="/tmp/xwayland${DISP}.pid"
LOG="/tmp/xw${DISP}.log"

stop() {
  if [[ -f "$PIDFILE" ]]; then
    pid=$(cat "$PIDFILE" 2>/dev/null || true)
    if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
      echo "Stopping Xwayland $DISP pid $pid" >&2
      kill "$pid" 2>/dev/null || true
      sleep 1
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$PIDFILE" "/tmp/.X11-unix/X${DISP#:}" 2>/dev/null || true
  fi
  # Also kill any stray Xwayland :92 not in pidfile
  pkill -f "Xwayland ${DISP} -geometry" 2>/dev/null || true
}

if [[ "${1:-}" == "--stop" ]] || [[ "${1:-}" == "stop" ]]; then
  stop
  exit 0
fi

if [ -e /tmp/.X11-unix/X92 ] && xdpyinfo -display :92 >/dev/null 2>&1; then
  echo "Xwayland $DISP already running" >&2
  xdpyinfo -display $DISP 2>&1 | head -n 3
  # Update pidfile if we can find the pid
  pid=$(pgrep -f "Xwayland :92 -geometry" 2>/dev/null | head -n1 || true)
  if [[ -n "$pid" ]]; then echo "$pid" > "$PIDFILE"; fi
  exit 0
fi

# Clean stale socket/pidfile if Xwayland died
rm -f "/tmp/.X11-unix/X${DISP#:}" "$PIDFILE" 2>/dev/null || true
echo "Starting Xwayland $DISP -geometry 1024x768 (host GPU)..." >&2
WAYLAND_DISPLAY=wayland-0 Xwayland $DISP -geometry 1024x768 -retro >"$LOG" 2>&1 &
pid=$!
echo "$pid" > "$PIDFILE"
# Ensure we clean up on script exit if Xwayland failed to start is not needed; actual daemon lives
# Caller should call --stop or docker compose down will kill via harness (see entrypoint trap below)
sleep 2
if ! xdpyinfo -display $DISP >/dev/null 2>&1; then
  echo "failed" >&2
  cat "$LOG" 2>&1 | tail -n 20
  rm -f "$PIDFILE"
  exit 1
fi
echo "Xwayland $DISP ready (pid $pid):" >&2
DISPLAY=$DISP glxinfo -B 2>&1 | grep -E "renderer|Accelerated" || true
echo "Use: DISPLAY=$DISP <game>  or  docker compose -f harness/docker-compose.yml -f harness/docker-compose.xwayland.yml up" >&2
echo "Stop: harness/scripts/start-xwayland.sh --stop"
