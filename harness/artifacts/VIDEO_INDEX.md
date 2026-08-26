# Video index

These recordings are gitignored local captures, not repo files; see
`README.md` in this directory for what is actually committed.

## Host-native (Xwayland :92, radeonsi, no container)
- `proof_xwayland_50s_menu.mp4`: 24s, game boots to the main menu on an
  isolated 1024x768 root (game window 1030x748@0,0). Proves the engine
  renders.
- `proof_xwayland_12s_nav.mp4`: 12s, `Down×3` then `Network`, driven by
  xdotool. No crash over a 30s stay.

## Container (gilde-host on Xwayland :92 privileged, gilde-client on Xvfb :99)
- `proof_host_xwayland_final.mp4`: 75s ffmpeg grab of `:92` inside the
  container. The driver reaches Network, then the evt poll faults at
  `0x42980D` (see below).
- `objective_1024_final.mp4`: 22s at 1024x768 after `windowmove 0,0`; host
  and container both land at 0,0 with a 1030x748 client area.

`0x42980D` is `fcn.00429800` polling the `dword[0x6b7e94]+esi` evt table. The
container prefix leaves that pointer NULL about 10s after boot, even sitting
on the menu; a host-direct run allocates it and survives 30s on the Network
screen. rizin shows `fcn.00429920` allocating 0x64c00 evt entries, which
fails silently in the container. `HARNESS_EVT_GUARD=1` (see
`harness/README.md`) guards the poll instead.

All captures used `ffmpeg -video_size 1024x768 -f x11grab -i :92` (or `:99`),
the same invocation as `entrypoint.sh`.
