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
| one template, 2 counters | 1.4703 M/s [1.4565, 1.4818] | 5.1588 M/s [5.1151, 5.2012] | 3.51x | 2,000,000 | 31,250 |
| one template, 8,000 counters | 4.1600 M/s [4.0929, 4.2208] | 56.280 M/s [56.155, 56.414] | 13.53x | 500 | 500 |
| five keys, one large template each | 4.0437 M/s [4.0225, 4.0618] | 54.695 M/s [54.552, 54.855] | 13.53x | 500 | 500 |
| five keys, four large templates each | 3.7859 M/s [3.7244, 3.8343] | 53.918 M/s [53.806, 54.017] | 14.24x | 500 | 500 |
| five keys, mixed templates | 3.7214 M/s [3.6574, 3.7809] | 27.627 M/s [27.531, 27.715] | 7.42x | 200,265 | 3,385 |

Every sample produced the expected logical records and counters. These are
end-to-end actor/API measurements. The output-count columns make the batching
component explicit; they are not parser-only measurements.

## Unified-width decoder cost

The candidate uses one unsigned big-endian decoder for every template-defined
counter width from one through eight bytes. Compared with the preceding
candidate revision that special-cased 8-byte counters, the same five workloads
measured the following throughput changes:

| Dataset | 8-byte-specialized candidate | Unified 1-8-byte candidate | Change |
|---|---:|---:|---:|
| one template, 2 counters | 5.3266 M/s | 5.1588 M/s | -3.15% |
| one template, 8,000 counters | 63.144 M/s | 56.280 M/s | -10.87% |
| five keys, one large template each | 61.346 M/s | 54.695 M/s | -10.84% |
| five keys, four large templates each | 59.551 M/s | 53.918 M/s | -9.46% |
| five keys, mixed templates | 28.851 M/s | 27.627 M/s | -4.24% |

This is the measured cost of keeping one implementation for all widths. The
unified implementation remains 3.51x to 14.24x faster than upstream across the
same end-to-end workloads.
