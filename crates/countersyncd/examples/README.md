# psample Netlink State Test

`psample_netlink_state_test` is a privileged local end-to-end stress test for the
`ControlNetlinkActor` and `DataNetlinkActor` Generic Netlink state machine.

It repeatedly unloads and reloads the host's `psample` and `act_sample` modules,
creates a veth and `tc action sample` path, and verifies that countersyncd receives
a fresh kernel-generated payload after every reload.

Before the reload stress phase, it also verifies both startup orderings without
injecting a reconnect command from the test: `psample` registered before the actors
start, and the actors started while `psample` is absent followed by its first
registration. Both scenarios must receive their own fresh marker payload. The second
scenario keeps Tokio time paused, so the one-second reconciliation timer cannot make
it pass instead of the `NEWFAMILY` notification path.

## Requirements

- An exclusive disposable Linux host or VM. Do not run it on a shared kernel.
- glibc 2.33 or newer in both build and runtime environments. The example links
  the `mallinfo2` symbol; this is a binary symbol requirement, not just a preferred
  measurement backend.
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

After the two startup scenarios, the default stress phase performs 137 real module
reloads: one warm-up cycle for each outage duration plus 128 measured cycles rotating
through `100ms`, `1s`, `10s`, `30s`, `1h`, `1d`, `1w`, `1mo`, and `1y`. Durations use
Tokio virtual time; module and packet operations remain real.

Wall-clock runtime is host-dependent because module, link, and packet operations
remain synchronous and real. Successful runs report `elapsed_wall`; measure a full
run on the target host rather than inferring runtime from the virtual outages. The
latest Linux 6.8 validation completed both startup scenarios and the 137-reload
stress phase in 24.62 seconds, excluding the release build and package installation.

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

## Coverage Boundaries

The current-thread Tokio virtual-time harness serializes synchronous host operations
with actor execution. It therefore does not cover teardown races possible in the
production multi-threaded runtime.

FD snapshots prove endpoint cardinality and detect unbounded FD growth at observed
checkpoints. They do not prove socket identity stability or absence of churn. Data
sockets intentionally change on each reload, and family/group helper resolution
creates expected socket churn, so raw socket inode identities are not tracked.

## Cleanup After an Abnormal Exit

Use these steps only on the exclusive disposable host or VM dedicated to this test.
The veth names are `pst<hex-pid>` and `psr<hex-pid>`: `pst` is the transmit endpoint,
`psr` is the receive endpoint, and the suffix is the test process PID rendered in
lowercase hexadecimal. Inspect `ip -brief link show` if the old PID is unknown.

Delete either stale veth endpoint first; deleting one removes the pair. Then unload
`act_sample` before its `psample` dependency. For example, for a known decimal PID:

```bash
old_pid=12345
sudo ip link delete "pst$(printf '%x' "$old_pid")"
sudo rmmod act_sample
sudo rmmod psample
```

If only the `psr` name remains visible, use that endpoint in the `ip link delete`
command instead. The test may also have introduced `cls_matchall`, `sch_ingress`,
and `veth`; after removing the pair and sampling modules, unload those helpers in
that order only if this disposable host had none of them loaded before the test.
