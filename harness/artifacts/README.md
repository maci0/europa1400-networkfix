# Proof artifacts

Captures kept from harness runs. Most `.mp4` recordings are gitignored and
stay on the machine that produced them; only `proof_netem_0_10_25.mp4` and
`host_client_pair/{host,client}.mp4` are committed. A doc entry naming an
`.mp4` that is not one of those describes a local file, not a repo file.

- `proof_netem_0_10_25.mp4`: the same session replayed at 0%, 10% and 25%
  netem packet loss.
- `host_client_pair/host.mp4`, `host_client_pair/client.mp4`: both peers of
  one lobby, recorded side by side.
- `mp_harness_loss{0,10,25}.csv`: per-run counters. All three files are
  byte-identical, so they record no difference between the loss levels.
- `screenshot_*.png`: `import -window root` stills. Despite the names, the
  committed ones show the Tutorial screen (`screenshot_network_final.png`,
  `gameplay_network.png`), the Graphics Options dialog
  (`screenshot_network_stay_host.png`) and the main menu
  (`screenshot_main_menu.png`). None of them shows the Network screen.
- `crash_dialog_dxwrapper.png`, `screenshot_host_xwayland_network.png`: the
  Wine "Program Error" dialog from the crashes described in
  `README_proof.md`.

The `Xwayland :92` host-passthrough setup these captures came from is gone;
`harness/scripts/start-xwayland.sh` and `harness/docker-compose.xwayland.yml`
no longer exist. To reproduce anything here, use the current fully headless
harness (`harness/README.md`), optionally with
`harness/docker-compose.gpu.yml` for GPU rendering.
