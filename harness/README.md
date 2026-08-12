# Harness: fully headless multiplayer testbed (Docker + Wine + weston)

Two isolated containers (`gilde-host` / `gilde-client`) on a private bridge
(`gilde-net` 10.10.0.0/24) run the game under Wine with `networkfix.asi`
loaded, drive it via xdotool, and record video/screenshots. Everything renders
inside the containers (headless weston + Xwayland): no host X server, no
windows on your desktop, no GPU required.

## The stack (and why each part exists)

| Layer | Why |
|-------|-----|
| weston `--backend=headless` + rootful `Xwayland :99 -geometry 1024x768` | The game's init enumerates D3D adapters/modes; it needs the RandR mode list Xwayland provides. Plain Xvfb has no usable mode list → init fails → shutdown-path crash at `0x46B2CC`. |
| i386 GL libraries (`libgl1:i386`, `libgl1-mesa-dri:i386`, …) | Wine here is 32-bit; without them wined3d finds no GL at all (`Failed to find a suitable pixel format`). 64-bit `glxinfo` working proves nothing. |
| `WINEDLLOVERRIDES="…;d3d8=n,b"` (set by `entrypoint.sh`) | Loads the native dxwrapper `d3d8.dll`. Without the override Wine silently uses builtin d3d8 and dxwrapper is inert. |
| dxwrapper `D3d8to9=1` + `SetPOW2Caps=1` | POW2 caps fix for the D3D8 surface-sizing UI bug. |
| dxwrapper `[Plugins] LoadPlugins=1` | Actually loads `*.asi` from `C:\Guild`. Miles (mss32) only scans ASI providers when audio init succeeds, which it does not headless: without this the harness never loaded `networkfix.asi` at all. Verify via `logs/*/hook_log.txt`. |
| `game.ini` `ServerPath=Server\server.dll` (relative, set by `entrypoint.sh`) | networkfix rejects absolute server paths; the installer writes an absolute one. |
| `HARNESS_EVT_GUARD=1` | Env-gated NULL guard in networkfix.asi for the game's evt poll (`0x429800`): skips the tick while the `evt:` tables (`[0x6B7E94]`) are unallocated/freed. Belt-and-braces against the headless init race (`0x42980D`). |
| `NETWORKFIX_DISABLE=1` | Baseline mode for A/B runs: same ASI, network-fix hooks off, evt guard still available. |

## Build

```bash
cp <path>/setup_the_guild_gold_2.0.0.5.exe harness/   # once, baked into image
docker compose -f harness/docker-compose.yml build
```

## Run

```bash
make            # bin/networkfix.asi
mkdir -p harness/logs/host harness/logs/client
docker compose -f harness/docker-compose.yml up --abort-on-container-exit
# A/B baseline (patch off, still crash-free):
NETWORKFIX_DISABLE=1 docker compose -f harness/docker-compose.yml up
```

Logs per container in `harness/logs/{host,client}/`: `game.log`,
`hook_log.txt` (must contain "All hooks enabled successfully"),
`entrypoint.log`, `weston.log`, `xvfb.log` (Xwayland), `driver.log`,
`record.mp4`, `screenshot_*.png`, `final.png`.

### Optional GPU rendering

Default is llvmpipe (software, portable, CI-safe, stable). For radeonsi:

```bash
RENDER_GID=$(stat -c %g /dev/dri/renderD128) \
  docker compose -f harness/docker-compose.yml -f harness/docker-compose.gpu.yml up
```

Still fully headless: the render node is used offscreen.

### X_BACKEND

`X_BACKEND=weston` (default) is the only mode where the game survives.
`X_BACKEND=xvfb` is kept for debugging the X stack itself; expect the game to
fail init (no RandR modes) and die within ~15 s.

## Isolated network + netem

`gilde-net` bridge, DNS names `gilde-host`/`gilde-client`, no host networking.

```bash
./harness/netem.sh --scenario packet-loss          # 10% loss both peers
GC_LOSS=10% docker compose -f harness/docker-compose.yml up
./harness/netem.sh --clear
```

Scenarios: `high-latency`, `variable-latency`, `packet-loss`, `reorder`,
`duplicate`, `corrupt` (needs `sch_netem` on the host kernel: see
`NETEM_NOTE.md` if `qdisc kind is unknown`).

## Drivers: lua-driven multiplayer lobby (full flow automated)

With the lua overlay the drivers build a REAL multiplayer session end-to-end:

```bash
docker compose -f harness/docker-compose.yml -f harness/docker-compose.lua.yml \
  up --abort-on-container-exit
```

Host: Network → Start New Game → Start Game As Server → players=2 Continue →
London → Continue → **lobby** → waits for client → Ready. Client: Network →
Join An Existing Game → server appears via LAN broadcast (no IP entry) →
Connect → Ready. Both peers then load into the game ("Receiving town data...")
over `gilde-net` with `networkfix.asi` + evt guard active. Step screenshots in
`logs/*/screenshot_*_{multiplayer,lobby,browser,connect*,ready}.png`.

### Input model (hard-won, do not rediscover)

- xdotool/XTest clicks reach the main menu but NOT submenu buttons: the game
  tracks its own DirectInput cursor. Blind Escape/Return on the main menu
  quits the game.
- Working method: inject from INSIDE the process. `harness/lua/init.lua`
  (loaded by `luaapi.asi`) replaces the REPL with a command loop: drivers write
  lua to `/tmp/lua_cmd.lua` (`lua_do` helper in `common.sh`), it runs
  `SetCursorPos` + `mouse_event`/`keybd_event`, result lands in
  `/tmp/lua_out.txt`. The AllocConsole window is unmapped by the driver
  (`hide_lua_console`).
- `game.ini mouse_speed` MUST be 256 (entrypoint enforces): the game scales
  relative mouse deltas by mouse_speed/256, so any other value breaks
  synthetic coordinates.
- Click targets: rendered position with **y + 43** (wine client-area offset)
  at 1152x864 (`cur_res=2`; Xwayland runs `-geometry 1152x864` to match).

## Session timing (measured)

From `compose up`: game menu ~30s, host lobby ~45s, client connected ~50s,
both Ready ~55s, town data ~22s, **in-game ~87s**.

The town-data transfer used to take ~170s: server.dll's network pump thread
is throttled by a hardcoded Sleep(30) and dequeues exactly one queued message
per connection per tick (~2x145B per 30ms, ~10KB/s), while the whole snapshot
sits pre-queued and TCP is idle (strace: the only wait between sends is the
Sleep itself). networkfix.asi now patches server.dll's Sleep IAT entry and
clamps that 30ms to 1ms (FASTSYNC). Scope is server.dll only, so game-exe
timing is untouched; accelerating the global winmm tick instead desyncs the
start handshake (tried, rejected). `NETWORKFIX_FASTSYNC=0` restores original
pacing; baseline mode (`NETWORKFIX_DISABLE=1`) never patches.

`HARNESS_NET_TRACE=1` hex-dumps server.dll send/recv payloads to hook_log for
protocol debugging.

## Gameplay + desync testing

The drivers no longer stop at the lobby: after both peers reach the town view
they run `play_town` (character-move clicks that generate synced command
traffic) while `desync_alive` watches for a dropped session (window title
revert, process death, fatal log strings). `PLAY_ITERS` sets the length.

Fault-injection knobs in networkfix.asi (all env-gated, applied independently
of the fix so the A/B stays fair):

- `NETWORKFIX_DISABLE=1` — faithful baseline: hooks stay installed but pass
  through with the original game semantics (single no-retry send, no
  WSAEWOULDBLOCK conversion, no stream-reader clamp). Only the fix behaviour is
  toggled, so both arms see identical machinery/stress.
- `HARNESS_TINY_BUFFERS=N` — shrink each server.dll socket's SO_SNDBUF/SO_RCVBUF
  to N bytes so the sender fills quickly under any congestion.

Local bridge caveat: on a clean docker bridge, normal gameplay does NOT desync
with or without the fix, because TCP is reliable and drains instantly. To
exercise the fix's send-retry path you must create real back-pressure by
freezing a peer so its receive window closes and the sender's (tiny) send
buffer fills: `HARNESS_TINY_BUFFERS=4096` then `kill -STOP` the client's game
process during a transfer.

Confirmed with a sustained freeze, fix ON: the host logs
`send: WSAEWOULDBLOCK, send buffer likely full (retry 1/5000)` …
`retry 4595/5000` … and, once the freeze outlasts the ~5s retry budget, a
graceful `Max retries exceeded, sent 0/145 bytes` (WSAETIMEDOUT) instead of a
silent partial send. This is the exact path the baseline drops with no retry.

Timing note: a *brief* freeze is unreliable to A/B because `fastsync` makes the
transfer so fast that the send-buffer-full window is tiny — a randomly-timed
freeze usually misses it. For a clean pass/fail comparison, freeze on the first
observed 145B send (`HARNESS_NET_TRACE=1`), or use netem for a steady
degradation (`GC_LOSS`, needs `sch_netem` — see `NETEM_NOTE.md`). The SIGSTOP
method is what forces the send-buffer-full path that netem alone does not
produce on a fast link.

## Debugging crashes

The game's own shutdown path crashes (`0x46B2CC`) when init fails, masking the
real error. To find the actual failure: run the game under `winedbg` with
breakpoints on the init-failure returns in `0x538850` (see git history of this
harness for the exact recipe), or set `WINEDEBUG=fixme-all,err+all` and read
`game.log`. Init-failure history so far: missing i386 GL (pixel format), then
missing RandR modes (Xvfb): both fixed by the stack above.
