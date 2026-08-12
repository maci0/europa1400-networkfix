#!/bin/bash
# Cross-peer desync + failure watch for an ENDURANCE run.
#
# The per-container drivers can only see their own screen, so a *silent* desync
# (both windows alive, game states diverged) is invisible to them. This host-side
# watcher compares the two peers' in-game date strip and reports divergence, plus
# year advances and driver-declared failures. Run it after launching an
# endurance session:
#
#   ENDURANCE=1 docker compose -f harness/docker-compose.yml \
#     -f harness/docker-compose.lua.yml up -d
#   harness/scripts/syncwatch.sh          # emits one line per event, exits on failure
#
# Env: LOGDIR (default harness/logs relative to repo), OUTDIR (screenshots on
# desync; default /tmp), CHECK_S (poll seconds, default 60).
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"           # harness/
LOGDIR="${LOGDIR:-$HERE/logs}"
OUTDIR="${OUTDIR:-/tmp}"
CHECK_S="${CHECK_S:-60}"
HOST_DRV="$LOGDIR/host/driver.log"
CLIENT_DRV="$LOGDIR/client/driver.log"

last_years=-1
divergent=0
# season+year strip, centre of the top bar (independent of per-player name/AP)
grab() { docker exec -e DISPLAY=:99 "gilde-$1" sh -c 'import -window root -crop 300x28+430+14 miff:-' 2>/dev/null | md5sum 2>/dev/null | cut -d' ' -f1; }

while true; do
  if grep -qa '\[endur\] FAIL' "$HOST_DRV" "$CLIENT_DRV" 2>/dev/null; then
    echo "DRIVER-FAILURE:"; grep -ah '\[endur\] FAIL' "$HOST_DRV" "$CLIENT_DRV" 2>/dev/null | sort -u | tail -4
    break
  fi
  docker ps --format '{{.Names}}' | grep -q gilde-host || { echo "CONTAINERS-GONE"; break; }

  y=$(grep -aho 'years~=[0-9]*' "$HOST_DRV" 2>/dev/null | tail -1 | grep -o '[0-9]*')
  if [ -n "$y" ] && [ "$y" != "$last_years" ]; then
    echo "year passed: years~=$y ($(grep -aho 'alive [0-9]*s' "$HOST_DRV" | tail -1))"; last_years=$y
  fi

  # only compare when neither peer is in a modal (both status bars bright)
  hb=$(grep -aho 'bar=[0-9]*' "$HOST_DRV" | tail -1 | grep -o '[0-9]*')
  cb=$(grep -aho 'bar=[0-9]*' "$CLIENT_DRV" | tail -1 | grep -o '[0-9]*')
  if [ "${hb:-0}" -gt 28000 ] 2>/dev/null && [ "${cb:-0}" -gt 28000 ] 2>/dev/null; then
    h=$(grab host); c=$(grab client)
    if [ -n "$h" ] && [ -n "$c" ] && [ "$h" != "$c" ]; then
      divergent=$((divergent + 1))
      if [ "$divergent" -ge 3 ]; then
        echo "DESYNC: host/client date diverged for $divergent checks (years~=$last_years)"
        docker exec -e DISPLAY=:99 gilde-host   sh -c 'import -window root /tmp/dsync.png' 2>/dev/null; docker cp gilde-host:/tmp/dsync.png   "$OUTDIR/desync_host.png"   2>/dev/null
        docker exec -e DISPLAY=:99 gilde-client sh -c 'import -window root /tmp/dsync.png' 2>/dev/null; docker cp gilde-client:/tmp/dsync.png "$OUTDIR/desync_client.png" 2>/dev/null
        break
      fi
    else
      divergent=0
    fi
  fi
  sleep "$CHECK_S"
done
