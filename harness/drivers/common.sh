#!/bin/bash
# Common helpers for host/client drivers (sourced).

SHOT_DIR="${LOG_DIR:-/tmp}"
LUA_FLAG_DIR="/tmp"
# lua_probe: if LUA_CONSOLE=1 and flag file present, succeed (lets driver skip xdotool retry)
lua_probe() {
  local want="$1"
  if [[ "${LUA_CONSOLE:-0}" != "1" ]]; then return 1; fi
  local flag="$LUA_FLAG_DIR/lua_${want}.ok"
  if [[ -f "$flag" ]]; then return 0; fi
  return 1
}
shot() { import -window root "$SHOT_DIR/screenshot_$(date +%s)_$1.png" 2>/dev/null || true; }
wait_window() {
  for i in $(seq 1 60); do
    WID=$(xdotool search --onlyvisible --name "Europa|Gilde|Guild" 2>/dev/null | head -n1 || true)
    if [ -n "${WID:-}" ]; then echo "$WID"; return 0; fi
    WID=$(xdotool search --onlyvisible --class "wine" 2>/dev/null | head -n1 || true)
    if [ -n "${WID:-}" ]; then echo "$WID"; return 0; fi
    sleep 1
  done
  return 1
}
has_pil() { python3 -c "import PIL" 2>/dev/null; }
# Exponential backoff sleep that is interruptible
backoff_sleep() { sleep "${1:-1}"; }
