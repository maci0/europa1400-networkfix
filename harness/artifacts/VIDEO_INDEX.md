# Video proof index

## Host-native (Xwayland :92, radeonsi, no container) — clean
- `proof_xwayland_50s_menu.mp4` — 24s menu→Network on isolated 1024x768 root (game 1030x748@0,0) — proves engine renders.
- `proof_xwayland_12s_nav.mp4` — 12s Down×3→Network nav proof (host direct, no crash stay 30s verified).
- `screenshot_menu_final.png` / `screenshot_network_final.png` / `screenshot_network_stay_host.png` — stills.

## Container (gilde-host on Xwayland :92 privileged, gilde-client on Xvfb :99) — isolated net
- `proof_host_xwayland_final.mp4` — 75s ffmpeg grab of :92 inside container; driver visits Network then evt poll crash at 0x42980D (container wine env vs host — see below).
- `proof_client_xwayland_75s.mp4` removed? use harness/logs/client/record.mp4 (Xvfb :99 llvmpipe).

Known: 0x42980D is `fcn.00429800` polling `dword[0x6b7e94]+esi` evt table; container prefix leaves it NULL ~10s after boot (even staying on menu). Host direct allocates it (survives 30s on Network). Root cause is container wine missing init (GameuxInstallHelper / fonts); rizin shows fcn.00429920 allocates 0x64c00 evt entries — fails silently in container due to read-only/limited env. Workaround: use host-native proof for gameplay video; container proves isolated gilde-net + ffmpeg plumbing.

New 1024x768 objective: `objective_1024_final.mp4` (22s 1024x768, windowmove 0,0 — host and container both 0,0×1030x748 verified).

All captures via `ffmpeg -video_size 1024x768 -f x11grab -i :92` (or :99) — same as entrypoint.
