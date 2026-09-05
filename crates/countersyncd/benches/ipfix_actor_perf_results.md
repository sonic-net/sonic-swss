# Historical IPFIX Actor Benchmark Results

These historical measurements compare candidate commit
`bff4dc878ac4ce6a135b1edef6e1fa4edf980bf1` (`bff4dc87`) against upstream baseline
`0b4057cde6ccdea51c21b626b3d91699fd0cf0bc` (`0b4057cd`). Later revisions changed
executed hot paths, including live domain lookups, identity checks, and repeated
ready drains. These numbers do not establish the final head's performance or
show a regression; the final head has not been remeasured for this report.

The baseline uses the zero-context
patch in `ipfix_actor_perf_upstream.patch` (apply with `git apply
--unidiff-zero`); it adapts only the benchmark harness and test data to the
candidate's workload and timing contract.

Recorded run configuration:

- `sonicdev-microsoft.azurecr.io:443/sonic-slave-bookworm:master-amd64`
- image ID `sha256:18da454f7f24b47bbe59c52ed7af713ae037e3ed8a8bfa3b8a0b99f31af2c2fb`
- common-lib and swss-common CI artifacts described at measurement time as
  latest-from-`master`; exact artifact versions/build IDs were not recorded in
  this report or the available benchmark logs and cannot be pinned from them
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

The image ID above is the recorded environment pin. The unpinned CI artifacts
limit exact reproduction; fetching today's latest artifacts would not recreate
the historical environment.

Historical Criterion point estimates and 95% confidence intervals:

| Dataset | Upstream `0b4057cd` | Candidate `bff4dc87` | Ratio | Upstream items | Candidate batches |
|---|---:|---:|---:|---:|---:|
| one template, 2 counters | 1.4703 M/s [1.4565, 1.4818] | 5.1085 M/s [5.0746, 5.1417] | 3.47x | 2,000,000 | 31,250 |
| one template, 8,000 counters | 4.1600 M/s [4.0929, 4.2208] | 62.809 M/s [62.510, 63.086] | 15.10x | 500 | 500 |
| five keys, one large template each | 4.0437 M/s [4.0225, 4.0618] | 60.919 M/s [60.673, 61.158] | 15.07x | 500 | 500 |
| five keys, four large templates each | 3.7859 M/s [3.7244, 3.8343] | 58.091 M/s [57.652, 58.523] | 15.34x | 500 | 500 |
| five keys, mixed templates | 3.7214 M/s [3.6574, 3.7809] | 28.396 M/s [28.310, 28.467] | 7.63x | 200,265 | 3,385 |

The recorded samples produced the expected logical records and counters. These are
end-to-end actor/API measurements. The output-count columns make the batching
component explicit; they are not parser-only measurements.

The internal 8,192-counter batching target is not an input or record limit.
Records larger than the target are sent intact in their own batch; channel
capacity limits the number of items, not a fixed maximum number of counters.

## Historical Helper Revision Comparison

The measured candidate used one unsigned big-endian decoder for every template-defined
counter width from one through eight bytes. The helper has one local branch for
the common 8-byte case, using `NetworkEndian::read_u64`; widths 1-7 retain the
same implementation. The table compares recorded runs of revisions with the
fully unified and eight-byte-specialized helpers, not an isolated helper-only
A/B experiment. Git history identifies the unified-helper revision as
`2aa5c7aec425c75de9229e0a34395eb18985e7ac` (`2aa5c7ae`). The same five workloads measured:

| Dataset | Unified-helper revision `2aa5c7ae` | Specialized-helper revision `bff4dc87` | Change |
|---|---:|---:|---:|
| one template, 2 counters | 5.1588 M/s | 5.1085 M/s | -0.98% (overlapping CIs) |
| one template, 8,000 counters | 56.280 M/s | 62.809 M/s | +11.60% |
| five keys, one large template each | 54.695 M/s | 60.919 M/s | +11.38% |
| five keys, four large templates each | 53.918 M/s | 58.091 M/s | +7.74% |
| five keys, mixed templates | 27.627 M/s | 28.396 M/s | +2.78% |

The specialized-helper revision measured 7.74-11.60% higher throughput on the
large all-8-byte workloads. This revision comparison does not isolate the
branch's contribution. The historical `bff4dc87` candidate measured 3.47x to
15.34x faster than the patched `0b4057cd` baseline across these actor/API
workloads; neither table is a final-head performance claim.
