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

# Click coords are rendered position + 43 (wine client-area Y offset), 1152x864.
# dismiss_year_scroll — click Continue until the year-start scroll clears to the
# town view (top status bar appears). Returns 0 once in-game.
dismiss_year_scroll() {
  for _ in $(seq 1 10); do
    # town view differs from the scroll in the top-bar strip (x300 y0 500x60)
    local before; before=$(crop_md5 300 0 500 60)
    lua_do "click(575,706)"; sleep 3
    [ "$(crop_md5 300 0 500 60)" != "$before" ] && return 0
  done
  return 1
}

# desync_alive WID — true while the game session is healthy: process window
# still present AND title still the in-game title (a drop reverts to a menu/
# dialog or the window vanishes). Also fails on known fatal log strings.
desync_alive() {
  local wid="$1"
  xdotool getwindowname "$wid" 2>/dev/null | grep -qi "Europa 1400" || return 1
  # game.log fatal markers (connection lost / terminating / desync)
  if grep -qaiE "connection (lost|closed)|terminating|desync|disconnect" \
      "${LOG_DIR:-/tmp}/game.log" 2>/dev/null; then
    return 1
  fi
  return 0
}

# play_town — light in-game activity that generates synced command traffic:
# left-click ground points (character move), interspersed with waits so the
# game clock advances and both peers exchange state. $1 = iterations.
play_town() {
  local n="${1:-20}" wid="$2" i
  local xs="380 560 300 640 460 520 360 600"
  local ys="520 480 600 560 440 620 500 540"
  set -- $xs; local -a X=("$@")
  set -- $ys; local -a Y=("$@")
  for i in $(seq 1 "$n"); do
    local k=$(( i % 8 ))
    lua_do "click(${X[$k]},${Y[$k]})"
    sleep 3
    if [ $(( i % 5 )) -eq 0 ]; then
      shot "play_${i}"
      if [ -n "${wid:-}" ] && ! desync_alive "$wid"; then
        echo "[play] SESSION LOST at iteration $i" | tee -a "$LOG"
        shot "desync_${i}"
        return 1
      fi
      echo "[play] iteration $i ok" | tee -a "$LOG"
    fi
  done
  return 0
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
