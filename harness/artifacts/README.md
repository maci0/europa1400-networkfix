# Proof artifacts — dedicated Xwayland :92 (own root 1280×1024, radeonsi)

Generated on host via `WAYLAND_DISPLAY=wayland-0 Xwayland :92 -geometry 1280x1024` (isolated root, host GPU accelerated — avoids Xvfb llvmpipe `0x42980D` crash).

- `proof_main_menu_50s.mp4` — 50s x11grab of `:92` while game boots to Europa 1400 Expansion Pack main menu (DirectX 8 via Wine, DRI3 radeonsi).
- `proof_network_nav_12s.mp4` — 12s x11grab showing xdotool driver: `Down×3` → `Network` → `Escape` back to menu (proves automated lobby driving works on the accelerated root).
- `screenshot_*.png` — `import -window root` of the same X session at menu / Network screen.

## How to reproduce (clean prefix, isolated network intact)

```bash
# 1. start isolated accelerated root
harness/scripts/start-xwayland.sh   # WAYLAND_DISPLAY=wayland-0 Xwayland :92 -geometry 1280x1024

# 2. build fresh prefix inside container (fixes stale wpf1)
docker compose -f harness/docker-compose.yml -f harness/docker-compose.xwayland.yml build
docker compose -f harness/docker-compose.yml -f harness/docker-compose.xwayland.yml up --abort-on-container-exit
# entrypoint will see USE_XWAYLAND=1 + /tmp/.X11-unix/X92 and use DISPLAY=:92 (same pidns as Wine)
# ffmpeg x11grab :92 → harness/logs/host/record.mp4 while drivers/host.sh does Down×3→Network

# Alternative without docker (host proof)
DISPLAY=:92 WINEPREFIX=~/Desktop/Projects/Europa1400/GILDE/guild-network-test/wpf1 wine ~/Desktop/Projects/Europa1400/GILDE/guild-network-test/wpf1/drive_c/Guild/Europa1400Gold_TL.exe
DISPLAY=:92 xdotool search --name "Europa 1400" … # see harness/drivers/host.sh
ffmpeg -video_size 1280x1024 -f x11grab -i :92 proof.mp4
```

Network isolation is preserved via `harness/docker-compose.yml` `gilde-net` bridge — X passthrough is only for the display socket; game traffic stays on `10.10.0.0/24` (see `harness/netem.sh` for `tc netem loss 10%` injection).
