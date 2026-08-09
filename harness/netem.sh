#!/bin/bash
set -euo pipefail
# Wrapper around guild-network-test/netem_inject.sh semantics but for compose network.
# Usage: ./netem.sh --scenario packet-loss [--service gilde-host]
# Also supports --clear

SCENARIO=""
SERVICE=""
CLEAR=false
STATUS=false
NETWORK="gilde-net"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scenario) SCENARIO="$2"; shift 2;;
    --service) SERVICE="$2"; shift 2;;
    --network) NETWORK="$2"; shift 2;;
    --clear) CLEAR=true; shift;;
    --status) STATUS=true; shift;;
    *) echo "Unknown $1"; exit 1;;
  esac
done

to_args() {
  case "$1" in
    high-latency) echo "delay 200ms" ;;
    variable-latency) echo "delay 100ms 50ms distribution normal" ;;
    packet-loss) echo "loss 10%" ;;
    reorder) echo "delay 20ms reorder 25% 50%" ;;
    duplicate) echo "duplicate 5%" ;;
    corrupt) echo "corrupt 0.5%" ;;
    *) echo "unknown $1" >&2; exit 1;;
  esac
}

services=()
if [[ -n "$SERVICE" ]]; then services=("$SERVICE"); else services=(gilde-host gilde-client); fi

for svc in "${services[@]}"; do
  CID=$(docker compose ps -q "$svc" 2>/dev/null || docker ps -q --filter "name=$svc" | head -n1)
  if [[ -z "$CID" ]]; then echo "No container for $svc"; continue; fi
  PID=$(docker inspect --format '{{.State.Pid}}' "$CID")
  echo ">>> $svc PID $PID"
  if $STATUS; then
    sudo nsenter -n -t "$PID" tc qdisc show dev eth0 || true
    continue
  fi
  if $CLEAR; then
    echo "Clearing netem on $svc"
    sudo nsenter -n -t "$PID" tc qdisc del dev eth0 root 2>/dev/null || true
    continue
  fi
  if [[ -z "$SCENARIO" ]]; then echo "Need --scenario"; exit 1; fi
  ARGS=$(to_args "$SCENARIO")
  echo "Applying $SCENARIO ($ARGS) to $svc eth0"
  sudo nsenter -n -t "$PID" tc qdisc replace dev eth0 root netem $ARGS
  sudo nsenter -n -t "$PID" tc qdisc show dev eth0
done
