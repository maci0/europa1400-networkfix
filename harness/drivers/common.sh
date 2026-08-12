#!/bin/bash
# Common helpers for host/client drivers (sourced).

SHOT_DIR="${LOG_DIR:-/tmp}"
LUA_FLAG_DIR="${LUA_FLAG_DIR:-/tmp}"
# lua_probe: if LUA_CONSOLE=1 and flag file present, succeed (lets driver skip xdotool retry)
lua_probe() {
  local want="$1"
  if [[ "${LUA_CONSOLE:-0}" != "1" ]]; then return 1; fi
  local flag="$LUA_FLAG_DIR/lua_${want}.ok"
  if [[ -f "$flag" ]]; then return 0; fi
  return 1
}
shot() { import -window root "$SHOT_DIR/screenshot_$(date +%s)_$1.png" 2>/dev/null || true; }
# lua_do "click(1,2)" — run lua source in-process via the harness init.lua command loop
lua_do() {
  rm -f /tmp/lua_out.txt
  printf '%s\nreturn 1' "$1" > /tmp/lua_cmd.lua
  for _ in $(seq 1 40); do [ -f /tmp/lua_out.txt ] && return 0; sleep 0.5; done
  echo "[lua_do] timeout waiting for lua_out: $1" >&2
  return 1
}
# wait_lua_ready — true once the in-process command loop wrote its Ready flag
wait_lua_ready() {
  for _ in $(seq 1 60); do [ -f /tmp/lua_Ready.ok ] && return 0; sleep 1; done
  return 1
}
# crop_md5 x y w h — md5 of a root-window region (cursor-safe change detection)
crop_md5() {
  import -window root -crop "${3}x${4}+${1}+${2}" /tmp/crop_wait.png 2>/dev/null
  md5sum /tmp/crop_wait.png 2>/dev/null | cut -d' ' -f1
}
# wait_crop_change x y w h timeout_s — true once the region's content changes
wait_crop_change() {
  local base
  base=$(crop_md5 "$1" "$2" "$3" "$4")
  local tries=$(( $5 / 2 ))
  for _ in $(seq 1 "$tries"); do
    sleep 2
    [ "$(crop_md5 "$1" "$2" "$3" "$4")" != "$base" ] && return 0
  done
  return 1
}
# hide_lua_console — the AllocConsole window covers the game; unmap it
hide_lua_console() {
  local c
  c=$(xdotool search --onlyvisible --name "Lua Console" 2>/dev/null | head -n1 || true)
  [ -n "$c" ] && xdotool windowunmap "$c" 2>/dev/null || true
}
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
