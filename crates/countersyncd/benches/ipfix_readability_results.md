# Readability Refactor: Before/After Measurement

Measured 2026-09-05. This compares the six readability refactors with their
immediate parent, not with upstream and not with an earlier lifecycle design.

- Before: `d614a46cdd23a8eb4b9ffc4bb26c38670618fcf5`.
- After: the code introduced alongside this report. Measured Git blob IDs:
  `src/actor/ipfix.rs` = `32e73420c4c330c1fd6fd22c6019546a5998cfc9`,
  `src/message/ipfix.rs` = `aeb5ff861f38fb55b8226de26b1f2e617dceee26`,
  `benches/ipfix_bench_data.rs` = `cddb881a656805605c8fac560fb62e048199ef5c`.
- One metric is one unsigned counter sample; all benchmark counters are 8 bytes.

## Full Five-Workload Comparison

Absolute Criterion throughput point estimates, with 95% confidence intervals.
M means 1,000,000 metrics/s. Change = after/before - 1; stored Criterion change
sections were not used because they may compare unrelated previous runs.

| Workload | Before, M metrics/s [CI] | After, M metrics/s [CI] | Change |
|---|---:|---:|---:|
| One template, 2 counters/record | 6.0918 [6.0557, 6.1253] | 6.0467 [6.0086, 6.0795] | -0.74% |
| One template, 8,000 counters/record | 62.302 [61.753, 62.723] | 62.308 [62.026, 62.569] | +0.01% |
| Five sessions, one large template each | 60.152 [59.520, 60.590] | 60.678 [60.518, 60.849] | +0.87% |
| Five sessions, four large templates each | 58.819 [58.618, 59.021] | 58.431 [57.878, 58.968] | -0.66% |
| Five sessions, mixed templates | 31.165 [31.063, 31.240] | 30.803 [30.651, 30.920] | -1.16% |

The first four pairs have overlapping intervals. Mixed throughput has disjoint
intervals and a small decrease; it is not described as unchanged.

## Reverse-Order Mixed Follow-up

After observing the mixed decrease, reran that workload with after first, before
second. No code changes between the original run and follow-up:

| Revision | M metrics/s [CI] |
|---|---:|
| After (run first) | 30.620 [30.503, 30.727] |
| Before (run second) | 30.989 [30.839, 31.121] |

The paired point difference remains -1.19%. This is evidence of a repeatable
small decrease on this host, not proof of a particular compiler/layout cause.
No width special cases or tuning were added to hide this readability tradeoff.

## Correctness and Scope

Every logged iteration matched expected logical records and counters. Output
batch counts were identical before/after:

| Workload | Records/iteration | Counters/iteration | Batches/iteration |
|---|---:|---:|---:|
| 2 counters | 2,000,000 | 4,000,000 | 31,250 |
| 8,000 counters | 500 | 4,000,000 | 500 |
| Five sessions, one template | 500 | 4,000,000 | 500 |
| Five sessions, four templates | 500 | 4,000,000 | 500 |
| Mixed templates | 200,265 | 4,040,100 | 3,385 |

Timing covers input channel, IPFIX actor decoding/output construction, output
channel, and sink counting. Template setup, deterministic input preparation,
readiness probe and teardown are excluded. It does not measure Redis/OTel output,
netlink reception, reconciliation performance, or multi-worker scaling.

Full validation: 350 test executions plus two compile-fail doctests per run,
parallel and serial; all-target check, bench compilation, release build, and all
five end-to-end consumption/DB-smoke scenarios passed. The doctests enforce the
nonrecursive reconciliation envelope; duplicate owner validation remains runtime.

## Reproduction

- CPU 2 (`docker --cpuset-cpus=2`), current-thread Tokio, separate sequential containers.
- Image `sonicdev-microsoft.azurecr.io:443/sonic-slave-bookworm:master-amd64`.
- Same cached CI common-lib/swss-common artifacts installed before both runs.
- Rust `1.86.0 (05f9846f8 2025-03-31)`, Cargo `1.86.0 (adf9b6ad1 2025-02-28)`.
- Channel capacities: template 1, raw input 1,024, stats batches 64.
- Up to 16 deterministic payloads/template; same metadata, record counts and values.
- Criterion 0.5.1, 3-second warmup, 60-second measurement target, 10 samples.
- `RUSTFLAGS=-Dwarnings cargo bench --locked -p countersyncd --bench ipfix_actor_perf -- --noplot`
- Follow-up adds filter `five_keys_six_templates_mixed` before `--noplot`.
- In-container wall-clock timeout: 35 minutes full run, 12 minutes filtered run.
- Virtualized/shared host: timing drift and scheduling remain limitations; CI
  overlap is not a substitute for a controlled hypothesis test.

Raw logs are in `/home/zegan/opencode-swss-verify/`:

```text
089d4b385c6eb501b3491c3d107bad61a318fddf2285e1b2519c4d9d04320195  pr4860-readability-before-d614a46c.log
0d85f991e12fd9b94667d3e6cd2e2e906f9123ad73c980683ee4a570b6280f30  pr4860-readability-after.log
cc40719c3f99d68a03fcc3dbbee7fdc76fe4ddcedd855bac4c80567256343f18  pr4860-readability-mixed-after-repeat.log
6a465c52231bcbd2a9ae4a1e20c1cb1fac9a45e00aad5c89b19eb3c7f35058c8  pr4860-readability-mixed-before-repeat.log
```

Installed swss-common package SHA-256:

```text
e51723531ea0b43e82bc549d62c3e6093b28c3b9b67f750495f76296c440b57c  libswsscommon_1.0.0_amd64.deb
41e1d1c2a641cf5607c401de1f7a2e679f94fa8d0978258428d157509f18ccbe  libswsscommon-dev_1.0.0_amd64.deb
```
