# IPFIX Actor Benchmark Reproduction

The PR benchmark compares the candidate against upstream commit
`0b4057cde6ccdea51c21b626b3d91699fd0cf0bc`. The baseline uses the zero-context
patch in `ipfix_actor_perf_upstream.patch` (apply with `git apply
--unidiff-zero`); it adapts only the benchmark harness and test data to the
candidate's workload and timing contract.

Both runs used:

- `sonicdev-microsoft.azurecr.io:443/sonic-slave-bookworm:master-amd64`
- image ID `sha256:18da454f7f24b47bbe59c52ed7af713ae037e3ed8a8bfa3b8a0b99f31af2c2fb`
- latest-from-`master` common-lib and swss-common CI artifacts
- CPU 2 via `taskset -c 2` and Docker `--cpuset-cpus=2`
- `RUSTFLAGS=-Dwarnings cargo bench --locked -p countersyncd --bench ipfix_actor_perf -- --noplot`
- up to 16 deterministic, distinct, pre-generated inputs per template
- one readiness probe outside the measured interval
- complete object metadata for every normal/readiness template update
- record-input channel capacity 1,024 and SAI-stats output capacity 64
- one iteration-wide watchdog, with no per-output timeout
- a manually measured interval from sender-task execution through receipt of
  the expected records and counters
- full output drain and actor/task teardown after the measured interval

Criterion point estimates and 95% confidence intervals:

| Dataset | Upstream | Candidate | Ratio | Upstream items | Candidate batches |
|---|---:|---:|---:|---:|---:|
| one template, 2 counters | 1.4892 M/s [1.4773, 1.5004] | 5.3266 M/s [5.3152, 5.3382] | 3.58x | 2,000,000 | 31,250 |
| one template, 8,000 counters | 3.9295 M/s [3.8452, 4.0076] | 63.144 M/s [62.938, 63.352] | 16.07x | 500 | 500 |
| five keys, one large template each | 3.9430 M/s [3.9296, 3.9557] | 61.346 M/s [61.170, 61.480] | 15.56x | 500 | 500 |
| five keys, four large templates each | 3.6682 M/s [3.6508, 3.6860] | 59.551 M/s [58.899, 60.126] | 16.23x | 500 | 500 |
| five keys, mixed templates | 3.7761 M/s [3.7564, 3.7961] | 28.851 M/s [28.672, 28.993] | 7.64x | 200,265 | 3,385 |

Every sample produced the expected logical records and counters. These are
end-to-end actor/API measurements. The output-count columns make the batching
component explicit; they are not parser-only measurements.
