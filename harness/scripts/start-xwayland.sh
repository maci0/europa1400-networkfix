#!/bin/bash
set -euo pipefail
# Start a dedicated Xwayland :92 with own 1280x1024 root, accelerated via host DRI.
# Must be run on the host (not inside container), before docker compose up with xwayland overlay.
DISP=":92"
if [ -e /tmp/.X11-unix/X92 ]; then echo "Xwayland $DISP already running"; xdpyinfo -display $DISP 2>&1 | head -n 3; exit 0; fi
echo "Starting Xwayland $DISP -geometry 1280x1024 (host GPU)..."
WAYLAND_DISPLAY=wayland-0 Xwayland $DISP -geometry 1280x1024 -retro 2>/tmp/xw92.log &
sleep 2
xdpyinfo -display $DISP >/dev/null 2>&1 || { echo "failed"; cat /tmp/xw92.log | tail -n 20; exit 1; }
echo "Xwayland $DISP ready:"
DISPLAY=$DISP glxinfo -B 2>&1 | grep -E "renderer|Accelerated"
echo "Use: DISPLAY=$DISP <game>  or  docker compose -f harness/docker-compose.yml -f harness/docker-compose.xwayland.yml up"
