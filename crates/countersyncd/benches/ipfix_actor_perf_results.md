# IPFIX Actor Benchmark Reproduction

The PR benchmark compares the candidate against upstream commit
`0b4057cde6ccdea51c21b626b3d91699fd0cf0bc`. The baseline uses the patch in
`ipfix_actor_perf_upstream.patch`; it adapts only the benchmark harness and test
data to the candidate's workload and timing contract.

Both runs used:

- `sonicdev-microsoft.azurecr.io:443/sonic-slave-bookworm:master-amd64`
- image ID `sha256:18da454f7f24b47bbe59c52ed7af713ae037e3ed8a8bfa3b8a0b99f31af2c2fb`
- latest-from-`master` common-lib and swss-common CI artifacts
- CPU 2 via `taskset -c 2` and Docker `--cpuset-cpus=2`
- `RUSTFLAGS=-Dwarnings cargo bench --locked -p countersyncd --bench ipfix_actor_perf -- --noplot`
- up to 16 deterministic, distinct, pre-generated inputs per template
- one readiness probe outside the measured interval
- one iteration-wide watchdog, with no per-output timeout
- a manually measured interval from sender-task execution through receipt of
  the expected records and counters
- full output drain and actor/task teardown after the measured interval

Criterion point estimates and 95% confidence intervals:

| Dataset | Upstream | Candidate | Ratio | Upstream items | Candidate batches |
|---|---:|---:|---:|---:|---:|
| one template, 2 counters | 1.4357 M/s [1.4296, 1.4416] | 5.9147 M/s [5.8865, 5.9374] | 4.12x | 2,000,000 | 31,250 |
| one template, 8,000 counters | 3.5500 M/s [3.5378, 3.5628] | 37.546 M/s [37.418, 37.650] | 10.58x | 500 | 500 |
| five keys, one large template each | 3.5704 M/s [3.5446, 3.5923] | 37.067 M/s [36.799, 37.301] | 10.38x | 500 | 500 |
| five keys, four large templates each | 3.5917 M/s [3.5794, 3.6033] | 37.102 M/s [37.026, 37.174] | 10.33x | 500 | 500 |
| five keys, mixed templates | 3.5845 M/s [3.5650, 3.6060] | 28.387 M/s [28.316, 28.451] | 7.92x | 200,265 | 3,385 |

Every sample produced the expected logical records and counters. These are
end-to-end actor/API measurements. The output-count columns make the batching
component explicit; they are not parser-only measurements.
