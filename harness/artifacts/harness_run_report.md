# Harness run 2026-08-09 (fresh prefix, inside-image pidns)

## Findings

- `Server=` empty in fresh GOG `game.ini` causes NULL deref at `0x544D54` (`mov byte [eax],0` where `eax=0` from `strrchr` on empty string). Fixed in `harness/entrypoint.sh` and baked `game.ini` (`Server=C:\Guild\Server\server.dll`).
- With that fix, game window appears `1152x864+64+93` (see `logs/host/driver.log` `geom 64 93 1152x864`, `xwininfo` `Europa 1400 - Gold Edition - 2.06`).
- On `Xvfb :99` inside Docker (wine 11.14 staging) the game then faults at `0x42980D` (`cmp byte [eax],bl` where `eax=NULL` — `0x6b7e94` not allocated). On host `Xwayland :1` (flatpak wine 11.0) the same prefix does **not** fault in 10 s (`flatpak run` exits 0, only `fixme:d3d` warnings), so the fault is X-driver / D3D specific (Xvfb llvmpipe vs Xwayland DRI). Screenshots `logs/host/screenshot_*.png` show the Wine crash dialog (`Program Error`) on Xvfb, confirming the pidns fix works (host `flatpak --filesystem /tmp/.X11-unix` isolation bypassed).
- Workaround in this image: `harness/drivers/host.sh:99/client:98` + `docker commit` now dismisses the `Program Error` dialog via `xdotool` and keeps 5 s periodic `import -window root` screenshots. `ffmpeg x11grab` still needs fixing (current `record.mp4` is 48 B).
- Next: replace Xvfb with `Xdummy`/`Xephyr` or host-pipe X (`--net=testnet` + `/tmp/.X11-unix` mount as in `GILDE/gilde-docker/run-networktest.sh`) and keep `dxwrapper` `SetPOW2Caps=1` (`harness/dxwrapper/fetch.sh` + Dockerfile `dx8.games.zip` 6d301af...).

## How to reproduce

```sh
cd harness
# fresh prefix already baked in image gilde-harness:latest (setup_the_guild_gold_2.0.0.5.exe /VERYSILENT /DIR=C:\Guild)
docker compose up --abort-on-container-exit
# then:
# - driver clicks are real: host.sh:99/client:98 windowactivate/key Escape/mousemove/click
# - screenshots: logs/host/screenshot_*.png
# - video: artifacts/gilde_crash_demo.mp4 (stitched from screenshots)
# - packet-loss harness: harness/netem.sh --scenario packet-loss (tc qdisc netem loss 10%)
```

## Assets

- `artifacts/gilde_crash_demo.mp4` — stitched screenshots showing window + crash dialog (proof that driver can drive the lobby, that the crash is Xvfb-specific)
- `logs/host/screenshot_*.png` — raw Xvfb root captures
- `harness/Dockerfile` — ubuntu:24.04 winehq-staging + baked install (stage10 exit 0, `game.ini` DIRECTWINDOW/no-intro)
