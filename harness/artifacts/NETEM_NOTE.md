# Netem on CachyOS 7.1.4 → 7.1.6

`CONFIG_NET_SCH_NETEM=m` is built as `sch_netem.ko.zst`, but running kernel is `7.1.4-1-cachyos` while installed `linux-cachyos` is `7.1.6-1`, so `/usr/lib/modules/7.1.4-1-cachyos` is missing (only `7.1.6-1-cachyos` + `6.18-lts` present). Hence `tc qdisc add ... netem` → `qdisc kind is unknown` on host and via `sudo nsenter` for `gilde-host`/`gilde-client` (`CAP_NET_ADMIN` present).

**Fix:** Reboot into `7.1.6-1-cachyos` (or `6.18 LTS`), then `modprobe sch_netem` and `harness/netem.sh --scenario packet-loss` / `GC_LOSS=10% docker compose up` will log `Applying netem to eth0: delay=0 loss=10%` and `tc qdisc show` → `netem loss 10%`.

Harness wiring already correct: `harness/docker-compose.yml` passes `GC_LOSS=${GC_LOSS:-}` (both services), `harness/entrypoint.sh:46` `tc qdisc replace dev eth0 root netem loss $GC_LOSS` (with `CAP_NET_ADMIN`), `harness/netem.sh` does `sudo nsenter -n -t $PID tc qdisc replace ...`.
