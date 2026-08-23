#!/bin/bash
# A/B harness runner: same stress, NETWORKFIX_DISABLE=0 vs 1.
# Stress = SIGSTOP the client game after Ready so the host's first in-game
# send fills (tiny 4 KB buffers), then SIGCONT after AB_FREEZE_S seconds.
# Usage:
#   harness/scripts/ab.sh            # both arms
#   harness/scripts/ab.sh on         # fix ON only
#   harness/scripts/ab.sh off        # baseline only
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
COMPOSE=(docker compose -f "$HERE/docker-compose.yml" -f "$HERE/docker-compose.lua.yml" -f "$HERE/docker-compose.ab.yml")
ARM="${1:-both}"
TIMEOUT_S="${AB_TIMEOUT_S:-360}"
FREEZE_S="${AB_FREEZE_S:-20}"

# Freeze the in-container Wine game (not the whole container) so the TCP
# peer stops draining while the host is sending the town snapshot / first
# 145 B command. SIGCONT after FREEZE_S so the session can recover or die.
freeze_client_game() {
  local freeze_log="$HERE/logs/client/freeze.log"
  echo "[freeze] waiting for host server.dll send (town snapshot / first cmd)" | tee -a "$freeze_log"
  local waited=0
  # Lobby handshake is a 28-byte send. The next send (128 B) is the first
  # real payload — freeze the client as soon as that line appears so the
  # host's subsequent send()s hit a stopped wineserver.
  while [[ $waited -lt 360 ]]; do
    if grep -qE 'send: called from server.dll:.*len=(1[2-9][0-9]|[2-9][0-9]{2,})' \
        "$HERE/logs/host/hook_log.tail" 2>/dev/null; then
      break
    fi
    sleep 1
    waited=$((waited + 1))
  done
  if [[ $waited -ge 360 ]]; then
    echo "[freeze] timed out waiting for host send (${waited}s) — no SIGSTOP" | tee -a "$freeze_log"
    return 0
  fi
  echo "[freeze] saw host send >28 B at t=${waited}s" | tee -a "$freeze_log"
  local game_pid
  game_pid=$(tr -d '[:space:]' < "$HERE/logs/client/wine.pid" 2>/dev/null || true)
  if [[ -z "${game_pid:-}" ]]; then
    echo "[freeze] no wine.pid from entrypoint — aborting freeze" | tee -a "$freeze_log"
    return 0
  fi
  # wineserver owns the TCP sockets; SIGSTOP of the PE process alone
  # still lets the kernel + wineserver ACK and drain.
  local pids="$game_pid"
  local ws
  ws=$(docker exec gilde-client sh -c 'pgrep -x wineserver || true' 2>/dev/null || true)
  [[ -n "${ws:-}" ]] && pids="$pids $ws"
  echo "[freeze] SIGSTOP pids=[$pids] for ${FREEZE_S}s" | tee -a "$freeze_log"
  # SC2086: $pids/$game_pid interpolate a pid list into the remote sh command
  # shellcheck disable=SC2086
  docker exec gilde-client sh -c "kill -STOP $pids; ps -o pid,stat,comm -p $(echo $pids | tr ' ' ',')" \
    2>&1 | tee -a "$freeze_log" || true
  if ! docker exec gilde-client sh -c "ps -o stat= -p $game_pid" 2>/dev/null | grep -q T; then
    echo "[freeze] game pid $game_pid did not enter T state — aborting" | tee -a "$freeze_log"
    docker exec gilde-client sh -c "kill -CONT $pids" 2>/dev/null || true
    return 0
  fi
  sleep "$FREEZE_S"
  echo "[freeze] SIGCONT pids=[$pids] after ${FREEZE_S}s" | tee -a "$freeze_log"
  # shellcheck disable=SC2086
  docker exec gilde-client sh -c "kill -CONT $pids; ps -o pid,stat,comm -p $(echo $pids | tr ' ' ',')" \
    2>&1 | tee -a "$freeze_log" || true
}

run_arm() {
  local name="$1" disable="$2"
  local out="$HERE/logs/ab-$name"
  echo "===== ARM $name (NETWORKFIX_DISABLE=$disable) ====="
  "${COMPOSE[@]}" down --timeout 5 >/dev/null 2>&1 || true
  rm -rf "$HERE/logs/host" "$HERE/logs/client"
  mkdir -p "$HERE/logs/host" "$HERE/logs/client" "$out"
  (
    cd "$REPO"
    NETWORKFIX_DISABLE="$disable" "${COMPOSE[@]}" up --abort-on-container-exit
  ) &
  local up_pid=$!
  freeze_client_game &
  local freeze_pid=$!
  local i=0
  while kill -0 "$up_pid" 2>/dev/null && [[ $i -lt $TIMEOUT_S ]]; do
    sleep 5
    i=$((i + 5))
    if grep -qE 'gameplay completed|SESSION LOST|gameplay ended early|ENDURANCE ended' \
        "$HERE/logs/host/driver.log" "$HERE/logs/client/driver.log" 2>/dev/null; then
      # give the other peer a moment to log the same
      sleep 8
      break
    fi
  done
  wait "$freeze_pid" 2>/dev/null || true
  if kill -0 "$up_pid" 2>/dev/null; then
    echo "ARM $name: stopping after ${i}s"
    "${COMPOSE[@]}" down --timeout 8 >/dev/null 2>&1 || true
    wait "$up_pid" 2>/dev/null || true
  else
    wait "$up_pid" 2>/dev/null || true
  fi
  mkdir -p "$out/host" "$out/client"
  cp -a "$HERE/logs/host/." "$out/host/" 2>/dev/null || true
  cp -a "$HERE/logs/client/." "$out/client/" 2>/dev/null || true
  echo "----- ARM $name summary -----"
  for side in host client; do
    echo "-- $side --"
    grep -E 'Hook OK|Hook init|NETWORKFIX_DISABLE|TINY BUF|WSAEWOULDBLOCK|Max retries|gameplay completed|SESSION LOST|gameplay ended|in-game|lua ready|SIGSTOP|SIGCONT|freeze' \
      "$out/$side/entrypoint.log" "$out/$side/driver.log" "$out/$side/hook_log.txt" "$out/$side/hook_log.tail" "$out/$side/freeze.log" 2>/dev/null \
      | tail -n 40 || true
  done
}

case "$ARM" in
  on)  run_arm on 0 ;;
  off) run_arm off 1 ;;
  both)
    run_arm on 0
    run_arm off 1
    ;;
  *) echo "usage: $0 [on|off|both]"; exit 1 ;;
esac
