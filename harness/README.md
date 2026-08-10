# Harness — isolated Wine containers with virtual framebuffer + xdotool

Uses Docker Compose isolated bridge `gilde-net` (10.10.0.0/24) so containers only see each other — mimics `guild-network-test/run_netns.sh` without Flatpak/nsenter. Silent installer via `setup_the_guild_gold_2.0.0.5.exe /VERYSILENT /DIR=C:\Guild` (reference: `GILDE/gilde-docker/Dockerfile:43`, `GILDE/guild-network-test/setup.sh:27`).

## Build

```bash
# Place installer next to Dockerfile if you want baked game (optional; otherwise bind-mount a prebuilt pfx)
cp ~/Desktop/Projects/Europa1400/GILDE/setup_the_guild_gold_2.0.0.5.exe harness/
docker compose -f harness/docker-compose.yml build
# or
docker build -t gilde-harness harness/
```

Base is `ubuntu:24.04` + `winehq-staging` (latest) + `xvfb` + `xdotool` + `imagemagick` + `iproute2` — upgrades reference Alpine `wine-staging` to latest Wine.

## Run

```bash
mkdir -p harness/logs/host harness/logs/client
make -C ../  # builds bin/networkfix.asi
docker compose -f harness/docker-compose.yml up --abort-on-container-exit
# Logs:
#   harness/logs/{host,client}/game.log  hook_log.tail  hook_log.txt  xvfb.log  driver.log  final.png
```

* X server per container:
  * default: Xvfb `:99 1280x1024x24` per container (isolated, no host X) — up to `llvmpipe` on 0x42980D evt table (`6b7e94` NULL) headless path.
  * accelerated: dedicated `Xwayland :92 -geometry 1280x1024` on host (one root, `radeonsi` via `/dev/dri`), shared read-only into containers via `/tmp/.X11-unix:ro` — `entrypoint.sh` detects `USE_XWAYLAND=1 + /tmp/.X11-unix/X92` and uses `DISPLAY=:92` with `privileged: true` for DRI auth. Requires `harness/scripts/start-xwayland.sh` before `compose -f docker-compose.yml -f docker-compose.xwayland.yml up`. Keeps `gilde-net` isolation (network stays `10.10.0.0/24`, only display socket crosses).
* Silent install tweaks: `game.ini` `Bildmodus=DIRECTWINDOW`, `show_intro=0`, `cur_res=2` (1280x768-ish) applied at runtime if present.
* ASI is bind-mounted to `/harness/networkfix.asi` and copied to `C:\Guild\networkfix.asi` before launch (Wine auto-loads ASI).
* Drivers: `drivers/host.sh` waits for `Europa|Gilde|Guild` window (`xdotool search --name`/`--class wine`) then best-effort clicks to reach Multiplayer — calibrate with `final.png` + `xwininfo` dump.

## Isolated network

`docker-compose.yml` declares `gilde-net` bridge; DNS names `gilde-host`/`gilde-client` resolve inside net. No `network_mode: host`.

Verify:

```bash
docker compose exec gilde-host ping -c2 gilde-client
docker compose exec gilde-client ping -c2 10.10.0.2
```

## Netem (packet loss / latency)

Per-container via `tc netem` on `eth0` inside netns (needs `cap_add: NET_ADMIN`):

```bash
# Apply 10% loss to both
./harness/netem.sh --scenario packet-loss
# Only host, high latency
./harness/netem.sh --scenario high-latency --service gilde-host
# Or via env at start:
GC_LOSS=10% docker compose up
GC_DELAY="100ms 50ms distribution normal" docker compose up
./harness/netem.sh --clear
./harness/netem.sh --status --service gilde-host
```

Scenarios: `high-latency`, `variable-latency`, `packet-loss`, `reorder`, `duplicate`, `corrupt` (same as `guild-network-test/netem_inject.sh`).

## xdotool

Drivers use `xdotool search/ windowactivate/ getwindowgeometry/ mousemove/ click/ key`. First run will need calibration — grab `harness/logs/*/final.png` and `xwininfo -root -tree` from driver logs. Reference has no xdotool driver (only `start.sh` injector); this harness adds it as requested.

## Reference material

- `GILDE/gilde-docker/Dockerfile:43` `/VERYSILENT /DIR=C:\Guild`, `game.ini` sed tweaks.
- `GILDE/guild-network-test/run_netns.sh` (bridge + veth + netns + `tc qdisc netem`).
- `GILDE/guild-network-test/setup.sh` (flatpak Wine silent install).

## Video fix (POW2 bug)

Europa1400 Gold has a D3D8 bug on modern Windows: `GetDeviceCaps` no longer
reports `D3DPTEXTURECAPS_POW2` → `FN_allocate_image_surface 0x04770A0` mis-sizes
surfaces → truncated UI. Fixed via dxwrapper (`elishacloud/dxwrapper`
v1.7.8400.25 `dx8.games.zip` `6d301af…`, `D3d8to9=1` + `SetPOW2Caps=1`).

This harness auto-installs it: `Dockerfile` downloads and verifies the zip,
`entrypoint.sh` copies `d3d8.dll`/`dxwrapper.dll`/`dxwrapper.ini` to
`C:\Guild`. Without it the game may crash at `0x42980D` on `wine 11.14`.

Local native run: `./harness/dxwrapper/fetch.sh` fetches the DLLs, or
copy to `GILDE/guild-network-test/wpf*/drive_c/Guild/` manually.

## Known: 0x42980D headless crash

`Europa1400Gold_TL.exe+0x42980D` (`fcn.00429800`) is a polling loop over `0x6b7e94+esi` (`0x64c00` evt table, init by `fcn.00429920`). On plain Xvfb headless the table stays NULL and the `cmp byte [eax],bl` faults ~10 s after boot even staying on main menu (verified container, while host `Xwayland :1/:92` allocates it and survives 30 s on Network). `dxwrapper v1.7.8400.25 SetPOW2Caps=1` fixes the earlier D3D8 POW2 UI crash; this `0x42980D` is a separate evt-init race — workaround is the accelerated Xwayland path above (see `harness/artifacts/VIDEO_INDEX.md`).
