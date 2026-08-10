#!/bin/bash
# Common helpers for host/client drivers (sourced).

SHOT_DIR="${LOG_DIR:-/tmp}"
lua_probe() { return 1; }  # placeholder: returns failure so xdotool path runs; real lua would echo ui.find hit and return 0
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
# Frame hash helper availability check
has_pil() { python3 -c "import PIL" 2>/dev/null; }
