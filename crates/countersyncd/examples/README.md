# psample Netlink State Test

`psample_netlink_state_test` is a privileged local end-to-end stress test for the
`ControlNetlinkActor` and `DataNetlinkActor` Generic Netlink state machine.

It repeatedly unloads and reloads the host's `psample` and `act_sample` modules,
creates a veth and `tc action sample` path, and verifies that countersyncd receives
a fresh kernel-generated payload after every reload.

## Requirements

- An exclusive disposable Linux host or VM. Do not run it on a shared kernel.
- glibc 2.33 or newer for allocator leak measurements.
- Root privileges.
- `CONFIG_PSAMPLE=m` and `CONFIG_NET_ACT_SAMPLE=m`.
- `iproute2` (`ip` and `tc`) and `kmod` (`modprobe`, `modinfo`, and `rmmod`).
- `psample` and `act_sample` initially unloaded. The test refuses to disrupt
  pre-existing users.

## Run

```bash
sudo env \
  "HOME=$HOME" \
  "PATH=$PATH:/usr/sbin:/sbin" \
  CARGO_TARGET_DIR=/tmp/countersyncd-psample-target \
  "$(command -v cargo)" run \
  -p countersyncd \
  --example psample_netlink_state_test \
  --release
```

The default performs 137 real module reloads: one warm-up cycle for each outage
duration plus 128 measured cycles rotating through `100ms`, `1s`, `10s`, `30s`,
`1h`, `1d`, `1w`, `1mo`, and `1y`. Durations use Tokio virtual time; module and
packet operations remain real.

For local debugging only, a shorter run can bypass the minimum of 101 measured
reloads:

```bash
sudo env \
  "HOME=$HOME" \
  "PATH=$PATH:/usr/sbin:/sbin" \
  CARGO_TARGET_DIR=/tmp/countersyncd-psample-target \
  PSAMPLE_TEST_SMOKE=1 \
  PSAMPLE_TEST_RELOADS=1 \
  "$(command -v cargo)" run \
  -p countersyncd \
  --example psample_netlink_state_test \
  --release
```

Normal `cargo test` does not execute this test. `cargo check --all-targets` still
compiles it so source regressions are caught in CI.
