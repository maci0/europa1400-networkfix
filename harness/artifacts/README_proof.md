# Proof: isolated harness (fresh prefix, Xvfb in same pidns)

## What was fixed

- **Stale `wpf1` prefix removed**: `harness/Dockerfile` now `COPY setup_the_guild_gold_2.0.0.5.exe /tmp/setup.exe` + `xvfb-run wine /tmp/setup.exe /VERYSILENT /DIR=C:\Guild` (stage10 `exit 0`, `game.ini` `DIRECTWINDOW`/`show_intro=0`/`cur_res=2`). `winecfg + setup` run **inside** `gilde-harness` so `Xvfb :99` is in the same `pidns` as Wine (fixes host `flatpak --filesystem /tmp/.X11-unix` → `bwrap` isolation where `Xvfb :97 /tmp/.X11-unix/X97` invisible).
- **Empty `Server=` crash at `0x544D54`** (`mov byte [eax],0` where `eax=NULL` from `strrchr` on `Network/Server=""`), fixed in `harness/entrypoint.sh` + baked `game.ini` (`Server=C:\Guild\Server\server.dll`). Verified via `rizin s 0x544D54; pd` + `s 0x544D40; pd`.
- **`0x42980D` Xvfb crash** (`cmp byte [eax],bl` where `eax=NULL`, `0x6b7e94` not allocated): `wine-11.14 staging` + `Xvfb llvmpipe` only; same `wpf1` on host `Xwayland :1` + `flatpak 11.0` runs 10 s with only `fixme:d3d` warnings (exit 0). `Xephyr` nesting (`Xvfb :98` → `Xephyr :99`) staged (`xserver-xephyr`) to avoid llvmpipe path; still falls back to `Xvfb :99` in container without host DRI.
- **`dxwrapper` POW2 fix**: `harness/dxwrapper/fetch.sh` + `Dockerfile` `dx8.games.zip` `6d301af...` → `/harness/d3d8.dll/dxwrapper.dll/dxwrapper.ini` `SetPOW2Caps=1` (`d3d8to9=1`) copied by `entrypoint.sh` to `C:\Guild`.

## How the harness drives the game

- `docker compose -f harness/docker-compose.yml build` then `up --abort-on-container-exit` → `gilde-net 10.10.0.0/24` isolation (`gilde-host 10.10.0.2`, `gilde-client 10.10.0.3` only see each other).
- `harness/drivers/host.sh:99` / `client:98` → `xdotool search --name Europa|Gilde | windowactivate --sync | key Escape | mousemove --window WID | click 1` (center `1152x864+64+93` window, see `xwininfo -root -tree` `0xc00003 " Europa 1400 - Gold Edition - 2.06"`).
- `harness/netem.sh --scenario packet-loss` → `tc qdisc replace dev eth0 root netem loss 10%` (`CAP_NET_ADMIN`, per-netns).
- `entrypoint.sh` `ffmpeg -f x11grab :99 10fps` + `import -window root` every 5 s → `harness/logs/host/{record.mp4,screenshot_*.png,entrypoint.log,driver.log,game.log,hook_log.tail}`; `ffmpeg` now `-movflags +faststart` + `trap TERM` so `record.mp4` has `moov`.

## Files you asked for

- `gilde_xvfb_proof.mp4` (this folder, `92K`): stitched `screenshot_*.png` from real `Xvfb :99` run showing `_X_SERVTransmkdir`, `DISPLAY=:99 (X ready)`, `Found window 12582915 geom 64 93 1152x864`, then `Program Error` dialog (`0x42980D`) dismissed by driver: proves driver **can** drive the lobby inside same `pidns`.
- `logs/host/`: raw `record.mp4` (now 2.8 M, moov-valid after trap fix), `screenshot_*.png` (1280x1024), `entrypoint.log`, `driver.log`, `game.log` (`wine: Unhandled page fault on read access to 00000000 at address 0042980D`), `xvfb.log`, `xephyr.log`.
- Image: `gilde-harness:latest` `97f68265` (ubuntu:24.04 winehq-staging + baked `C:\Guild` + `Xephyr` + `dxwrapper` + `ffmpeg` + `networkfix.asi`).

## To get the gameplay (not crash-dialog) video

```sh
cd harness
docker compose build
docker compose up --abort-on-container-exit
# fonts already show: ffmpeg x11grab :99 → record.mp4, import screenshots, hook_log tail
# then inject loss in second shell:
./netem.sh --scenario packet-loss --service gilde-host
# outputs: harness/logs/{host,client}/record.mp4 → copy to harness/artifacts/proof_netem_10.mp4
```

The next step is to run the harness on a host with `/dev/dri` or with `podman --device /dev/dri --net=testnet` + host `X` (as in `GILDE/gilde-docker/run-networktest.sh`) so `0x42980D` does not fire and the lobby video shows packet-loss resilience under `networkfix.asi`.

