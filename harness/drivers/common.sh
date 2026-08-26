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
# lua_do "click(1,2)": run lua source in-process via the harness init.lua command loop
lua_do() {
  rm -f /tmp/lua_out.txt
  printf '%s\nreturn 1' "$1" > /tmp/lua_cmd.lua
  for _ in $(seq 1 40); do [ -f /tmp/lua_out.txt ] && return 0; sleep 0.5; done
  echo "[lua_do] timeout waiting for lua_out: $1" >&2
  return 1
}
# wait_lua_ready: true once the in-process command loop wrote its Ready flag
wait_lua_ready() {
  for _ in $(seq 1 60); do [ -f /tmp/lua_Ready.ok ] && return 0; sleep 1; done
  return 1
}
# crop_md5 x y w h: md5 of a root-window region (cursor-safe change detection)
crop_md5() {
  import -window root -crop "${3}x${4}+${1}+${2}" /tmp/crop_wait.png 2>/dev/null
  md5sum /tmp/crop_wait.png 2>/dev/null | cut -d' ' -f1
}
# wait_crop_change x y w h timeout_s: true once the region's content changes
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
# hide_lua_console: the AllocConsole window covers the game; unmap it
hide_lua_console() {
  local c
  c=$(xdotool search --onlyvisible --name "Lua Console" 2>/dev/null | head -n1 || true)
  [ -n "$c" ] && xdotool windowunmap "$c" 2>/dev/null || true
}

# Click coords are rendered position + 43 (wine client-area Y offset), 1152x864.
# dismiss_year_scroll: click Continue until the year-start scroll clears to the
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

# desync_alive WID: true while the game session is healthy: process window
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

# topbar_mean: mean brightness (0..65535) of the in-game status bar strip.
# High (~34k) when the parchment bar is visible; low (~21k) when a fullscreen
# scroll/dialog (year-end, error) covers it. Used to detect blocking modals.
topbar_mean() {
  import -window root -crop 500x55+330+8 miff:- 2>/dev/null \
    | convert miff:- -colorspace Gray -format '%[fx:mean*65535]' info: 2>/dev/null
}

# max_speed: click the clock a few times to raise game speed to max. The clock
# face is at rendered (1085,45) -> click (1085,88); repeated clicks step the
# speed up (measured ~3x advance after 5 clicks). Year-start resets it, so the
# endurance loop re-calls this after each dismissed year modal.
max_speed() {
  for _ in 1 2 3 4 5; do lua_do "click(1085,88)"; sleep 0.3; done
}

# endurance_run WID: max speed, then run indefinitely: pass years at max speed,
# dismiss the year-end modal(s) whenever the status bar is covered, and watch
# for failure (window/title gone, fatal log line, or a modal that will not
# clear). On failure, screenshot + dump log tail and return 1.
endurance_run() {
  local wid="$1" start now el iter=0 years=0 stuck=0 mean
  start=$(date +%s)
  echo "[endur] setting max speed" | tee -a "$LOG"
  max_speed
  echo "[endur] running until failure" | tee -a "$LOG"
  while true; do
    sleep 12
    iter=$((iter + 1))
    now=$(date +%s); el=$((now - start))

    # --- failure: window/title gone or fatal log line ---
    if ! xdotool getwindowname "$wid" 2>/dev/null | grep -qi "Europa 1400"; then
      echo "[endur] FAIL: game window/title gone at ${el}s (iter $iter, ~$years years)" | tee -a "$LOG"
      shot "endur_fail_window_${el}"; tail -n 40 "${LOG_DIR}/game.log" >>"$LOG" 2>/dev/null || true
      return 1
    fi
    if grep -qaiE "connection (lost|closed)|terminating|desync|disconnect|synchron" "${LOG_DIR}/game.log" 2>/dev/null; then
      echo "[endur] FAIL: fatal log line at ${el}s (~$years years)" | tee -a "$LOG"
      shot "endur_fail_log_${el}"; grep -aiE "connection|terminat|desync|disconn|synchron" "${LOG_DIR}/game.log" | tail -n 20 >>"$LOG" 2>/dev/null || true
      return 1
    fi

    # --- year-end / blocking modal: status bar covered ---
    mean=$(topbar_mean); mean=${mean%.*}
    if [ -n "$mean" ] && [ "$mean" -lt 28000 ] 2>/dev/null; then
      stuck=$((stuck + 1))
      echo "[endur] modal at ${el}s (bar mean=$mean, streak=$stuck), dismissing" | tee -a "$LOG"
      shot "endur_modal_${el}"
      # year-end can stack several panels; click both button positions a few times
      for _ in 1 2 3; do lua_do "click(575,706)"; sleep 1; lua_do "click(662,706)"; sleep 1; done
      if [ "$stuck" -ge 6 ]; then
        echo "[endur] FAIL: modal would not clear after ${stuck} tries at ${el}s (~$years years), likely error dialog" | tee -a "$LOG"
        shot "endur_fail_stuck_${el}"; tail -n 40 "${LOG_DIR}/game.log" >>"$LOG" 2>/dev/null || true
        return 1
      fi
      years=$((years + 1))
      sleep 2; max_speed   # year start resets speed
    else
      stuck=0
    fi

    [ $((iter % 5)) -eq 0 ] && echo "[endur] alive ${el}s iter=$iter years~=$years bar=$mean" | tee -a "$LOG"
  done
}

# play_town: light in-game activity that generates synced command traffic:
# left-click ground points (character move), interspersed with waits so the
# game clock advances and both peers exchange state. $1 = iterations.
play_town() {
  local n="${1:-20}" wid="$2" i
  local xs="380 560 300 640 460 520 360 600"
  local ys="520 480 600 560 440 620 500 540"
  # intentional whitespace-split of coordinate lists into arrays
  # shellcheck disable=SC2086
  set -- $xs; local -a X=("$@")
  # shellcheck disable=SC2086
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
