> **Historical v3 evidence only.** This report describes `sonic-hft-arrow-v3`,
> not the current v4 matrix format. Its measurements, test counts, and writer-side
> recovery descriptions are preserved below as historical data, not current
> performance or behavior claims. v4 leaves abandoned files untouched and assigns
> tail handling to readers. See the [current storage contract](hft-local-storage.md)
> and [v4 matrix performance report](hft-local-storage-matrix-performance.md).

# HFT Local Storage: Arrow IPC Performance

Report prepared 2026-09-07 from existing measurements for `sonic-hft-arrow-v3`.
**The 50,000,000 raw metrics/s lossless ingestion target was not achieved.**
Production capture is best effort: a nonblocking tap feeds a 32-flat-batch input
queue, and one synchronous storage loop serially converts, compresses, writes,
and fsyncs batches. Exact storage of surviving observations is not lossless
end-to-end ingestion.

At 500 counters/record, standalone lossless medians are 27,262,050-30,648,997
metrics/s. Unpaced UDP end-to-end medians are 24,762,206-26,039,463 metrics/s,
**with substantial loss**. At 8,000 counters/record, UDP medians are approximately
5,000,000 metrics/s, but these are **extremely lossy, short overload trials**,
not sustained-capacity measurements. Even the separate 10,000,000 metrics/s
paced UDP observations lose storage input.

This report does not reuse the retired range-summary measurements as raw-storage
results. See [storage behavior and reader API](hft-local-storage.md) and the
[historical v1 performance report](hft-local-storage-performance.md).

## Contents

- [Evidence and environment](#evidence-and-environment)
- [Measurement contract](#measurement-contract)
- [Workloads](#workloads)
- [Results](#results)
- [Compression and disk](#compression-and-disk)
- [Correctness and recovery](#correctness-and-recovery)
- [Interpretation and next work](#interpretation-and-next-work)
- [Reproduction commands](#reproduction-commands)
- [Per-trial evidence](#per-trial-evidence)

## Evidence And Environment

The evidence files are under `/home/zegan/opencode-swss-verify/`. Their basenames
and observed modification times are recorded below as provenance, not as exact
trial start times. JSONL does not record a per-trial wall-clock date. The system
clock check, `date --iso-8601=seconds`, returned
`2026-09-07T00:07:30+00:00`; the runs cross the September 6/7 UTC boundary.
No tests or benchmarks were rerun for this documentation edit.

| Evidence File | Records | Last Modified (UTC) | Scope |
| --- | ---: | --- | --- |
| `ipc-v3-standalone-500.jsonl` | 12 trials | 2026-09-06 23:58:29 | Four patterns, three repeats, standalone |
| `ipc-v3-udp-500.jsonl` | 12 trials | 2026-09-07 00:00:00 | Four patterns, three repeats, unpaced UDP |
| `ipc-v3-udp-8000.jsonl` | 12 trials | 2026-09-07 00:00:33 | Four patterns, three repeats, wide unpaced UDP |
| `ipc-v3-udp-500-rate10m.jsonl` | 4 trials | 2026-09-07 00:04:13 | One paced UDP trial per pattern |
| `ipc-v3-cargo-test-final.log` | 7 result summaries | 2026-09-07 00:07:00 | Final Cargo test output |
| `ipc-reader-pyarrow20.jsonl` | 2 fixtures + summary | 2026-09-07 00:05:55 | Independent Arrow C++ readback |

The portable tables below retain the relevant trial measurements; readers do not
need access to these machine-local paths to reconstruct rates, losses, and byte
ratios. A subsequent standalone 8,000-counter diagnostic is recorded separately
below; it is not included in the 40-trial aggregates.

### Additional Wide Diagnostic

After the main runs, `ipc-v3-standalone-8000.jsonl` recorded one continuous,
unpaced standalone trial per pattern: 20,000 records, 160 million observations,
generation included, blocking enqueue, final fsync included. All four trials
persisted every input with zero readback errors. These single repeats diagnose
the wide-layout cost; they are not a proven hardware maximum.

| Pattern | Persisted Metrics/s | Canonical Payload / File Bytes | File Bytes |
| --- | ---: | ---: | ---: |
| idle | 6,260,916 | 8.400560 | 152,389,840 |
| burst | 6,176,113 | 7.773724 | 164,677,840 |
| sustained | 6,361,560 | 8.400560 | 152,389,840 |
| mixed | 6,047,707 | 7.401701 | 172,954,832 |

The maximum sampled allocation in these additional trials was 172,998,656 bytes;
trials were removed between runs. The wide layout performs substantially worse
even without UDP. IPC compresses individual column buffers, and the fixed batch
byte budget gives 8,000 columns far fewer time samples per buffer than 500
columns. These structural differences are confirmed in the code, but their
individual timing contributions have not been profiled.

| Item | Environment |
| --- | --- |
| CPU | Intel Xeon Platinum 8370C @ 2.80 GHz; 16 logical CPUs, 8 cores, SMT |
| Host | Microsoft virtual machine, x86-64, Linux `6.8.0-138-generic` |
| Filesystem | `/dev/sdc1`, ext4, `rw,relatime`; JSONL filesystem type `0xef53` |
| Container | Existing `ipfix-local-storage-verify`, `sonicdev-microsoft.azurecr.io:443/sonic-slave-bookworm:master-amd64` |
| Container OS | Debian Bookworm; installed `base-files` `12.4+deb12u15` |
| Repository mount | `/home/zegan/sonic-swss-ipfix-local-storage` to `/workspace/repos/sonic-swss` |
| Toolchain | rustc 1.86.0 (`05f9846f8`), cargo 1.86.0 (`adf9b6ad1`); optimized benchmark |
| IPC implementation | Rust Arrow 55.2.0; standard IPC ZSTD buffer compression |
| Independent reader | PyArrow 20.0.0 / Arrow C++ 20.0.0 |
| Existing CI dependencies | swss-common artifact 1212995 and common-lib artifact 1213281, reused, not newly selected/downloaded for this report |
| Installed libraries | libswsscommon/dev 1.0.0, libyang3 3.12.2-1, SONiC libnl 3.7.0-0.2+b1sonic1 |
| CI reference | `.azure-pipelines/build-template.yml`; prepared Redis Unix socket with AKE notifications |
| Runtime | Two Tokio workers; separate sender/storage/watchdog work; one storage writer |

CPU, mount, container, installed-package, and toolchain details were checked
read-only in the current environment. The existing dependency artifacts are
build inputs, not fresh September 7 CI validation. There is no isolated-core,
bare-metal, cold-cache, physical-device bandwidth, full Debian package build,
full SONiC build, or DVS validation claim.

## Measurement Contract

One metric is one counter observation, not a row, packet, byte, or summary.

```text
persisted metrics/s = verified persisted counter values / end-to-end seconds
canonical record bytes = persisted metrics * 8 + persisted records * 8
storage file bytes = Arrow IPC logical file bytes + root loss.json logical bytes
compression ratio = canonical record bytes / storage file bytes
```

Canonical bytes count each original record timestamp once. They exclude the
storage-generated `record_seq`, in-memory structures, wire headers, and schema
metadata. The denominator includes schema, sequence storage, IPC framing,
compressed/uncompressed buffers, EOS, and loss diagnostics. It is not allocated
filesystem blocks and is not an isolated ZSTD codec compression ratio. Dropped
observations never enter the numerator.

- All 40 performance trials use `--streaming`: one continuous actor/storage
  instance and one segment per trial, no replayed payload pool or prebuilt input
  budget. Generation is included in the measured time.
- Standalone generates flat batches and uses blocking enqueue/backpressure. It
  bypasses UDP and IPFIX, so its zero loss does not establish production-tap
  behavior at that rate.
- UDP uses real IPv4 loopback send/receive, the actual IPFIX actor, its
  nonblocking storage tap, and the same storage loop. One record is sent per
  datagram: 4,028 bytes at width 500 and 64,028 bytes at width 8,000.
- UDP-to-IPFIX capacity is 256 buffers. A separate 64-batch audit recipient checks
  counts, timestamps, and uniqueness. Its scheduling/backpressure is included;
  production Redis/OTLP sinks are not included. `--audit-values` is **off** in all
  performance trials: timed auditing does not hash every decoded value.
- Templates, metadata setup, storage setup/recovery, raw readback, and cleanup
  are outside timing. Timed work extends from generation/send/enqueue through
  storage drain, last-batch write, fsync, final publication, and storage join.
- Sender, receiver, and decoder durations are **overlapping elapsed milestones
  from the same start**, not additive stage CPU times. Do not sum them, subtract
  stage rates, or treat a fast decoder milestone as durable ingestion capacity.
- UDP completion waits for sender completion and drains the actual kernel socket
  to `WouldBlock`; these runs do not include an artificial idle-completion delay.
- Readback is untimed and checks every persisted raw value, exact timestamp,
  record/stat order, uniqueness, full names, and schema metadata against the
  deterministic source. It uses both direct Rust Arrow `StreamReader` columns
  and application `read_shard`. These are separate validation paths but use the
  **same Rust Arrow implementation**, not two independent codec implementations.
- Independent PyArrow/Arrow C++ validation is a separate small smoke fixture,
  not a second-language scan of all performance-trial output.

`lossless_sample_storage=true` means surviving persisted samples were exact.
Only `all_input_persisted=true` means all generated input survived. Across these
logs every trial has zero verification/readback errors and no storage failure,
but only the 12 standalone trials have all input persisted. All 40 report
`meets_50m_lossless_target=false`.

## Workloads

All main patterns are deterministic unsigned integer bandwidth gauges in bits/s,
not cumulative counters. Values can repeat, decrease, spike, and return to zero.
The source step is 10,000 ns per record. The burst envelope has a 100 ms
**source-time** period; wall-clock pacing is independent.

| Pattern | Synthetic Signal |
| --- | --- |
| idle | Zero except a 5% low-activity window around 1-5 Mbps, with slow triangular drift |
| burst | Synchronized 10% high window per 100 ms; sharp 100 Gb/s edges, slopes over 10% of that window, 200 Gb/s plateau; idle behavior otherwise |
| sustained | 200 Gb/s minus at most 200 kb/s triangular jitter |
| mixed | Per-series staggered phase and 5-20% burst duty, with the same high/low construction |

At width 500, each trial generates 300,000 records / 150,000,000 metrics: a
3-second synthetic signal, covering 30 burst periods. **That is not a measured
3-second ingestion run.** Standalone takes 4.835-5.548 seconds, approximately
4.8-5.5 seconds; paced UDP takes approximately 15.04 seconds. At width 8,000,
20,000 records / 160,000,000 metrics span just 0.2 seconds of source time (two
periods), with 1.217-1.321 seconds measured end to end.

The generator assigns `type_id == stat_id == index + 1` and object names
`Ethernet{index % 64}`. It exercises known SAI names at a few positions but mostly
unknown/fallback identities, not a captured production 500-counter database
layout. Each column embeds full object/type/stat names in its field name and
metadata. Repeated long metadata and one-column-per-stat schema costs exaggerate
the 8,000-wide case; its observed result is not a general real-device width
limit. An actual 500-counter database workload should retain its actual raw
names and full metadata in a future comparison, not shorten names to improve
the result. This synthetic value distribution is not evidence of production
entropy or guaranteed compression.

## Results

Main entries are medians of three trials; all rates are explicitly **metrics/s**
and rounded to the nearest integer. Loss percentages are medians calculated
independently, not the loss of a fabricated median trial. Exact counts and
elapsed measurements appear in the appendix.

| Pattern | Standalone 500 Median (metrics/s) | UDP 500 Median (metrics/s), Lossy | UDP 500 Total Loss (%) | UDP 8,000 Median (metrics/s), Extremely Lossy/Short | UDP 8,000 Total Loss (%) |
| --- | ---: | ---: | ---: | ---: | ---: |
| idle | 30,648,997 | 26,039,463 | 66.405333 | 5,040,816 | 95.910000 |
| burst | 28,583,231 | 25,411,279 | 62.820333 | 5,130,781 | 95.910000 |
| sustained | 29,423,362 | 24,762,206 | 66.404333 | 5,272,372 | 95.910000 |
| mixed | 27,262,050 | 25,283,648 | 62.814667 | 5,089,616 | 95.910000 |

Standalone persisted all 150,000,000 metrics each time, with zero input drops.
UDP 500 persisted 49,379,000-58,584,000 of 150,000,000 metrics per trial; total
loss was 60.944000-67.080667%. UDP 8,000 persisted only 6,464,000-6,544,000 of
160,000,000 metrics, or 808-818 records out of 20,000: **95.910000-95.960000%
total loss**. All wide UDP trials are explicitly flagged short by the harness;
UDP 500 idle repeats 1 and 3 are also below its 2-second warning threshold.
The other unpaced UDP trials are still short observations, not a soak test.

UDP losses occur both before decoding (sent versus received) and at storage
input (decoded versus persisted). Received and decoded counts match in every
UDP trial: measured decode loss is zero, not overall loss. Sender/receiver/
decoder rates can exceed 50,000,000 metrics/s while durable output falls far
short; they have different numerators and overlapping elapsed milestones.

### Paced UDP At 10,000,000 Metrics/s

These are four additional trials, **one per pattern**, not three-repeat medians.
Each sends, receives, and decodes 150,000,000 metrics with no packet loss, but
the nonblocking storage input drops batches.

| Pattern | Persisted (metrics/s) | End-to-End (s) | Persisted Metrics | Storage-Lost Metrics | Dropped Input Batches | Total Loss (%) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| idle | 7,125,706 | 15.047140055 | 107,221,500 | 42,778,500 | 42,456 | 28.519000 |
| burst | 7,304,674 | 15.043930081 | 109,891,000 | 40,109,000 | 39,647 | 26.739333 |
| sustained | 7,278,554 | 15.044334642 | 109,501,000 | 40,499,000 | 40,232 | 26.999333 |
| mixed | 7,083,471 | 15.045307488 | 106,573,000 | 43,427,000 | 43,012 | 28.951333 |

Pacing changes scheduling and batch arrival conditions. This result is evidence
of loss in these runs, not proof of a ranked root cause for the lower output rate
relative to unpaced UDP. Batch-drop counts are messages, not records or metrics;
do not multiply them by a fixed nominal batch size to infer loss.

## Compression And Disk

### Stable 500-Counter Layout

Standalone's canonical record payload is 1,202,400,000 bytes per trial:
1,200,000,000 raw value bytes plus 2,400,000 timestamp bytes. Logical output
sizes and compression ratios are identical across all three repeats of each
pattern, despite elapsed-time differences. No standalone loss diagnostic file
was present. These are exact raw observations, not range summaries.

| Pattern | Canonical Raw Bytes | Persisted File Bytes | Canonical Raw / Persisted File Bytes | Values-Only / File Bytes |
| --- | ---: | ---: | ---: | ---: |
| idle | 1,202,400,000 | 9,713,480 | 123.786738 | 123.539658 |
| burst | 1,202,400,000 | 19,281,352 | 62.360772 | 62.236300 |
| sustained | 1,202,400,000 | 16,632,840 | 72.290721 | 72.146428 |
| mixed | 1,202,400,000 | 28,616,072 | 42.018345 | 41.934477 |

Standard Arrow IPC does **not** implement integer delta encoding here. Non-null
`UInt64` timestamps, record sequence, and values remain exact. ZSTD compresses
individual IPC buffers losslessly; there is no private delta codec or floating
point conversion. Arrow 55.2.0 uses ZSTD's default level (encoder level 0,
currently level 3 in the locked dependency), not an explicitly chosen level 1.
Standard IPC may retain a buffer uncompressed if compression would enlarge it.

| Pattern | UDP 500 Canonical Ratio, Median | UDP 8,000 Canonical Ratio, Median | Paced UDP 500 Canonical Ratio, Single Trial |
| --- | ---: | ---: | ---: |
| idle | 115.939208 | 5.979427 | 70.604886 |
| burst | 51.880950 | 5.353389 | 45.474725 |
| sustained | 67.326396 | 5.668766 | 46.460613 |
| mixed | 43.905691 | 5.415761 | 33.461579 |

UDP ratios describe only surviving samples plus their file overhead. They must
not be used to estimate bytes for all offered input, and are not controlled
same-sample compression comparisons with standalone.

### Test Disk Footprint

The benchmark uses a 2,400,000,000-byte application quota and a private-tree
watchdog with a 2,950,000,000-byte sampled cap, below the 3 GB testing budget.
The watchdog samples allocated file/directory blocks, including staging, every
10 ms and at completion; this is not an exact instantaneous high-water mark.
The benchmark rejects tmpfs/overlay roots and disables the production dedicated
filesystem requirement only for its private real-disk test directory. Trials
are verified and deleted before the next; generated input is not written as a
raw disk file.

| Scope | Trials | Logical Output Bytes, Sum | Finalized Allocated Bytes, Sum | Maximum Sampled Simultaneous Bytes |
| --- | ---: | ---: | ---: | ---: |
| Three unpaced datasets | 36 | 420,125,136 | 421,007,360 | 28,655,616 |
| Paced UDP supplement | 4 | 75,967,112 | 76,099,584 | 25,567,232 |
| All performance datasets | 40 | 496,092,248 | 497,106,944 | 28,655,616 |

All reported trial footprints are well below 3 GB. The 420,125,136-byte figure is
the cumulative logical output for **36**, not 40, trials; it is not a concurrent
footprint or physical write-amplification measurement. The independent reader
retains 4,852,240 logical bytes / 4,894,720 allocated bytes, approximately 4.9 MB.
These figures concern test data, not the repository's build cache, dependency
artifacts, or a global filesystem usage audit.

### Hypothetical 10 GB At Full Target Ingest

This is a **synthetic extrapolation, not measured or guaranteed retention**.
Assume all 50,000,000 metrics/s at width 500 are ingested without loss, the
standalone signal and compression repeat unchanged, and all decimal
10,000,000,000 bytes are usable for logical output. Each standalone dataset
represents 150,000,000 metrics, so the target-rate output is its file bytes / 3 s,
**not file bytes / measured standalone elapsed time**.

```text
logical bytes/s at full target = standalone logical output bytes / 3
seconds for 10 GB = 10,000,000,000 / logical bytes/s at full target
```

| Pattern | Extrapolated Logical Bytes/s At 50,000,000 Metrics/s | 10 GB Seconds | 10 GB Minutes |
| --- | ---: | ---: | ---: |
| idle | 3,237,826.666667 | 3,088.49 | 51.47 |
| burst | 6,427,117.333333 | 1,555.91 | 25.93 |
| sustained | 5,544,280.000000 | 1,803.66 | 30.06 |
| mixed | 9,538,690.666667 | 1,048.36 | 17.47 |

Rounding is from the exact byte arithmetic; idle is 51.4748576 minutes (51.47
to two decimal places). Actual names, entropy, layout changes, flush sizes,
filesystem allocation, and reservations alter the result. Most importantly,
**50,000,000 lossless metrics/s was not achieved**, and production's quota is
**4,000,000,000 bytes**, not 10 GB. Production stops appending on capacity or I/O
failure, possibly before the nominal quota because of reservations. It has no
rolling retention, automatic deletion, or circular overwrite policy.

## Correctness And Recovery

All performance-trial persisted values were reconstructed and compared, not
sampled or inferred from byte counts. This includes all values of every surviving
record in the lossy UDP trials; it cannot validate or recover dropped records.

### Independent Arrow C++ Smoke

`ipc-reader-pyarrow20.jsonl` reports `PASS` using
`pyarrow.ipc.open_stream`, with no Rust/application decoder. Each fixture has
1,000 records, 500 metrics per record, 502 non-null `UInt64` columns, one record
batch, timestamps 10,000-10,000,000 ns, and record sequences 0-999. Both have zero
duplicate records, nulls, and mismatches. Full field names/metadata and all
1,000,000 total raw values were checked with Python integer arithmetic.

| Pattern | File Bytes | Values Checked | Minimum | Maximum | Values > 2^53 | Values > 2^63 | Odd Values > 2^53 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| burst | 570,120 | 500,000 | 0 | 200,000,000,000 | 0 | 0 | 0 |
| large100 | 4,282,120 | 500,000 | 1,172,673,148,423 | 18,446,636,308,625,890,033 | 499,770 | 250,005 | 249,685 |

| Fixture | SHA-256 |
| --- | --- |
| burst | `2103724d4a621dbadcec46400c24c2cac5fa706f4b7854420cdd653642b38b68` |
| large100 | `c37aef2ffdd2b78e9b030899ec58f89ab5a42ba444a61b9e11d58782c4554d3b` |

The smoke data covers only 10 ms of source time, less than a full burst period;
it is interoperability/value-exactness evidence, not a throughput/compression
benchmark. `large100` is benchmark-generated full-width unsigned stress. Literal
`u64::MAX` was **not injected** into this PyArrow fixture; exact endpoint vectors
are covered separately by Rust tests. Use `open_stream`, not Feather or IPC
`open_file`, despite the `.arrow` suffix.

### Final Cargo Log

Parsing the final log's result summaries gives the following counts. Do not
reuse the historical v1 total of 424.

| Test Target/Group | Passed Executions | Reported Time (s) |
| --- | ---: | ---: |
| Library | 197 | 18.90 |
| Binary | 208 | 18.95 |
| End-to-end integration | 2 | 0.00 |
| IPFIX/helper integration | 11 | 0.02 |
| Helper target | 4 | 0.00 |
| Empty target | 0 | 0.00 |
| Empty target | 0 | 0.00 |
| Total | 422 | Not a separately measured total |

All summaries report zero failures, ignored, measured, and filtered tests. There
are 225 distinct printed test-name strings, not 422 independent test cases:
shared tests run in multiple targets. The SIGKILL test invokes a child test
process using `storage_child_sigkill_marker`; that helper and repeated
library/binary coverage must not be counted as additional independent recovery
scenarios or an extra completed full suite.

There are **22 distinct tests under `actor::local_storage`**, including two codec
tests and the child marker, each appearing in the library and binary output.
They are not all power-failure tests. The complete targeted set is:

| Test Name (Local Storage Prefix Omitted) | Coverage |
| --- | --- |
| `codec::tests::recovery_classifies_io_sources_not_error_messages` | Classify operational I/O errors by source, not message text |
| `codec::tests::recovery_io_errors_abort_scan_without_modifying_partial` | Propagate read I/O errors; preserve partial data |
| `tests::actor_drains_input_reports_drops_and_restart_accumulates_diagnostics` | Drain and drop diagnostics across restart |
| `tests::actor_quota_failure_sets_status_and_disconnects_without_touching_archive` | Fail local output while preserving archive |
| `tests::exact_raw_random_gauges_peaks_valleys_and_unsigned_boundaries` | Exact random, peak/valley, repeated/decreasing and unsigned boundary values |
| `tests::idle_wall_flush_then_shutdown_with_sender_still_connected` | Idle deadline and shutdown behavior |
| `tests::interrupted_write_preserves_earlier_batches_without_a_writer_queue` | Earlier complete batches survive interrupted write |
| `tests::layout_changes_stat_order_names_ids_duplicates_and_empty_rows` | Positional schema and raw record preservation |
| `tests::lock_unknown_staging_legacy_archives_and_symlinks_are_preserved` | Locking and fail-closed preservation |
| `tests::names_limits_and_setup_status` | Name mapping, limits, setup status |
| `tests::oversized_schema_and_sequence_exhaustion_flush_previous_valid_rows` | Preserve prior valid rows at limits |
| `tests::quota_stops_append_keeps_durable_prefix_and_never_overwrites_published` | Stop at capacity, retain prefix, no overwrite |
| `tests::real_child_sigkill_recovers_synced_batches_and_restarts_idempotently` | Actual process SIGKILL and repeated restart |
| `tests::recover_every_byte_truncation_preserves_exact_complete_batch_prefix` | Every byte truncation of a constructed small stream |
| `tests::recovery_admission_requires_incremental_filesystem_and_quota_reserves` | Recovery capacity admission |
| `tests::recovery_missing_eos_malformed_tail_completed_staging_and_repeat_restart` | Tail recovery, staged complete stream, idempotence |
| `tests::recovery_quota_admission_preserves_partial_before_truncation_or_removal` | Preserve partial before capacity admission succeeds |
| `tests::rejects_invalid_header_and_nullable_schema_without_removing_state` | Invalid schema/header preservation |
| `tests::standard_reader_accepts_embedded_schema_without_any_sidecars` | Self-contained standard IPC |
| `tests::storage_child_sigkill_marker` | Child-process test helper, not another crash scenario |
| `tests::wall_and_raw_rotation_are_independent_of_source_timestamps` | Wall/raw rotation logic |
| `tests::wide_layout_batch_budget_and_buffer_reuse` | Batch-size logic and reuse, not measured RSS bound |

Recovery keeps only a surviving valid schema and complete valid batch prefix.
Every-byte truncation exercises a small constructed stream, not every possible
filesystem failure. Read/seek I/O failures propagate rather than authorizing
truncation; capacity admission precedes truncation/removal and preserves the
partial on admission failure. SIGKILL checks process interruption, not removal
of machine power.

Completed batch writes flush and fsync; final publication fsyncs the file and
directories. Durability remains conditional on successful I/O and the filesystem
and device honoring fsync. **Actual power failure was not tested.** Queued input
and unfinished in-memory batches are not durable. A 100 ms flush being due is
not a hard 100 ms persistence guarantee under compression, scheduling, or
blocked I/O. A 32-message queue is not a fixed byte/RSS bound for variable-size
records. No hard end-to-end memory, queue-latency, blocked-I/O shutdown, or
power-loss durability bound is established by these measurements. This is
targeted Rust/IPC validation, not a full package build or DVS/full-build result.

## Interpretation And Next Work

The demonstrated facts are exact surviving-value readback, stable standalone
file ratios for this workload, and a substantial gap from the lossless ingest
target. Source inspection shows synchronous serial conversion/compression/write/
fsync, a best-effort input queue, per-column IPC buffer compression, and repeated
schema metadata. The logs do not contain CPU profiles, allocator profiles, or
isolated fsync/compression timings, so they do **not prove or rank** these as the
causes of the throughput gap or the paced/unpaced difference.

Per-buffer compressor context creation and allocation in the pinned Arrow/ZSTD
path are candidate performance limitations, especially with many columns, not
a measured dominant cause. Long repeated fallback names, batch fill, diagnostic
fsync activity, generator cost, and actor scheduling are also variables to
control, not reasons to invent a causal ranking from overlapping milestones.

Future work should measure longer runs with actual 500-counter database names,
full metadata, realistic raw gauges and transport, explicit loss accounting,
and controlled pacing. Profile before attributing costs. Evaluate denser
messages, timestamp/time-vector handling that preserves every original record
timestamp, and multi-batch compression scheduling with bounded, measured resource
use. Preserve standard IPC and independently readable exact values; prefer
supported upstream compressor reuse/parallelism and Arrow APIs rather than
introducing a private integer-delta codec. Validate ordering, recovery, memory,
and both Rust/PyArrow readers after any such change.

A larger input buffer may absorb transient bursts but **cannot fix a sustained
service-rate deficit**. It can also increase RAM use and the amount of accepted
but not durable data. Do not present buffering as evidence that 50,000,000
lossless metrics/s or continuous capture at the production quota is supported.

## Reproduction Commands

These are explicit reproduction invocations for the recorded configurations,
not recovered shell-history evidence of the original timeout wrapper. They are
shown for execution **inside the already prepared CI-like container**, at
`/workspace/repos/sonic-swss`, with its existing real-disk mount and dependencies.
No commands in this section were executed for this documentation change. Do not
run these tests/builds on the host. `RUSTFLAGS=-Dwarnings` is explicit rather than
assumed to be a persistent container environment variable.

```sh
# ipc-v3-cargo-test-final.log configuration; no package/DVS/full-build implication.
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 900s \
  cargo test --locked -p countersyncd

# Targeted storage coverage, including codec tests and the SIGKILL parent/helper.
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 900s \
  cargo test --locked -p countersyncd --lib actor::local_storage

# ipc-v3-standalone-500.jsonl: 12 trials, blocking lossless producer.
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 1200s \
  cargo bench --locked -p countersyncd --bench local_storage_perf -- \
  --root /workspace/repos/sonic-swss --mode standalone --streaming \
  --records 300000 --counters 500 --repeats 3 --rate 0 --step-ns 10000 \
  --patterns idle,burst,sustained,mixed --timeout-secs 120 --require-lossless

# ipc-v3-udp-500.jsonl: 12 trials; intentionally allow loss accounting.
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 1200s \
  cargo bench --locked -p countersyncd --bench local_storage_perf -- \
  --root /workspace/repos/sonic-swss --mode udp --streaming \
  --records 300000 --counters 500 --repeats 3 --rate 0 --step-ns 10000 \
  --patterns idle,burst,sustained,mixed --timeout-secs 120

# ipc-v3-udp-8000.jsonl: 12 short, extremely lossy overload trials.
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 1200s \
  cargo bench --locked -p countersyncd --bench local_storage_perf -- \
  --root /workspace/repos/sonic-swss --mode udp --streaming \
  --records 20000 --counters 8000 --repeats 3 --rate 0 --step-ns 10000 \
  --patterns idle,burst,sustained,mixed --timeout-secs 120

# ipc-v3-udp-500-rate10m.jsonl: one trial per pattern, still lossy.
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 1200s \
  cargo bench --locked -p countersyncd --bench local_storage_perf -- \
  --root /workspace/repos/sonic-swss --mode udp --streaming \
  --records 300000 --counters 500 --repeats 1 --rate 10000000 --step-ns 10000 \
  --patterns idle,burst,sustained,mixed --timeout-secs 120

# Regenerate only the independent smoke inputs under a NEW/EMPTY fixture root.
# Existing ipc-reader-fixture is retained evidence; do not overwrite it.
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 900s \
  cargo bench --locked -p countersyncd --bench local_storage_perf -- \
  --root /workspace/repos/sonic-swss \
  --output-root /workspace/repos/sonic-swss/ipc-reader-fixture-new \
  --mode standalone --streaming --records 1000 --counters 500 --repeats 1 \
  --rate 0 --step-ns 10000 --patterns burst,large100 --timeout-secs 120 \
  --require-lossless
```

The benchmark prints the generated trial paths when retaining output. The
existing external verifier is
`/home/zegan/opencode-swss-verify/verify-ipc-pyarrow.py`; it is not a checked-in
portable dependency of this report. With that script exposed in the prepared
container and a Python interpreter containing PyArrow 20.0.0, the exact argument
form for the existing retained fixture is below. `PYARROW20_PYTHON` and
`IPC_VERIFY_SCRIPT` must name those prepared container paths; new fixture runs
must instead use their newly emitted paths.

```sh
timeout --signal=TERM --kill-after=30s 120s \
  "${PYARROW20_PYTHON:?set to the prepared PyArrow 20.0.0 Python}" \
  "${IPC_VERIFY_SCRIPT:?set to the exposed verify-ipc-pyarrow.py}" \
  burst=/workspace/repos/sonic-swss/ipc-reader-fixture/local-storage-perf-DYrFLM \
  large100=/workspace/repos/sonic-swss/ipc-reader-fixture/local-storage-perf-4vsusp
```

Omitting `--audit-values` matches the performance logs. Adding
`--require-lossless` to the UDP cases would turn their recorded loss into a
nonzero benchmark exit; it would not make those measurements lossless.

## Per-Trial Evidence

### Reading The Tables

`S` = standalone 500, `U` = unpaced UDP 500, `W` = unpaced UDP 8,000, and `P` =
paced UDP 500 at 10,000,000 metrics/s. Trials are keyed by group, pattern, and
repeat. Rates are rounded to integer metrics/s; times are shown to nanosecond
decimal precision (insignificant binary-float rendering tails are omitted).
Byte and count columns are exact integers. Use counts/time for unrounded rates.

The following shared values and formulas reconstruct the remaining relevant
JSONL accounting without pretending that dropped-batch counts are sample counts:

```text
N = 300000 records for S/U/P; 20000 for W
C = 500 counters for S/U/P; 8000 for W
R = received records; D = decoded records = R; K = persisted records
B = dropped input batches; F = storage logical file bytes; A = finalized allocation
expected metrics = sent metrics = N*C; sent records = N; unsent metrics = 0
received metrics = decoded metrics = R*C; persisted metrics = K*C
lost records = N-K; UDP lost packets = N-R
UDP received packets = unique received packets = R; UDP sent packets = N
UDP-lost metrics = (N-R)*C; decode-lost metrics = 0
storage-lost metrics = (R-K)*C
packet-loss fraction = (N-R)/N; storage-loss fraction = (R-K)/R
total-loss fraction = (N-K)/N; decode-loss fraction = 0
rows = K; rows/s = K/seconds; persisted metrics/s = K*C/seconds
persisted value bytes = K*C*8; persisted timestamp bytes = K*8
canonical record bytes = K*(C+1)*8; canonical ratio = K*(C+1)*8/F
values-only ratio = K*C*8/F
loss.json bytes = 0 for S; 89 for U/W; 90 for P
Arrow logical file bytes = F - loss.json bytes
current allocated bytes before cleanup = A + 4096
```

For S, `R = D = K = N`, all losses and input drops are zero, and UDP packet/stage
fields are null, not measured zero. For all groups: `storage_failed=false`,
`verification_errors=0`, `readback_errors=[]`, `dropped_shards=0`,
`lossless_sample_storage=true`, `meets_50m_lossless_target=false`. UDP send errors,
bad framing, duplicates, and truncated packets are all zero. S status is
`lossless` / `unpaced_lossless_streaming`; U/W/P status is `loss` /
`overload_no_capacity_claim`, with `all_input_persisted=false`.

Common configuration: `streaming=true`, `generation_included=true`,
`audit_values=false`, `segments=1`, `segment_record_limit=N`,
`prebuilt_input_budget_bytes=0`, `runtime_workers=2`, `storage_queue_batches=32`,
`storage_writers=1`, `udp_audit_queue_batches=64`, `step_ns=10000`,
`shard_ns=5000000000`, `pattern_period_source_ns=100000000`, `source_span_ns=N*10000`,
`value_unit=bps`, `large_counters=0`, `large_fraction=0`,
`quota_bytes=2400000000`, `disk_allocated_sampled_cap_bytes=2950000000`,
`target_metrics_per_second=50000000`, `filesystem_type=0xef53`,
`retained_trial=null`. `source_pattern_warning` is null throughout.
`seconds` equals `total_fsync_end_to_end_seconds`. The readback mode is the two
Rust passes described above; UDP completion is `sender_done_then_kernel_would_block`.
The short-trial warning applies exactly to U idle 1/3 and all W trials.

### End-to-End Counts And Times

| Group | Pattern | Repeat | Seconds | Persisted (metrics/s) | Received = Decoded Records | Persisted Records | Dropped Input Batches |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| S | idle | 1 | 4.835362551 | 31,021,459 | 300000 | 300000 | 0 |
| S | idle | 2 | 4.922912287 | 30,469,769 | 300000 | 300000 | 0 |
| S | idle | 3 | 4.894124231 | 30,648,997 | 300000 | 300000 | 0 |
| S | burst | 1 | 5.337621136 | 28,102,407 | 300000 | 300000 | 0 |
| S | burst | 2 | 5.218970252 | 28,741,302 | 300000 | 300000 | 0 |
| S | burst | 3 | 5.247832147 | 28,583,231 | 300000 | 300000 | 0 |
| S | sustained | 1 | 5.060984559 | 29,638,502 | 300000 | 300000 | 0 |
| S | sustained | 2 | 5.097989875 | 29,423,362 | 300000 | 300000 | 0 |
| S | sustained | 3 | 5.133266627 | 29,221,159 | 300000 | 300000 | 0 |
| S | mixed | 1 | 5.547599639 | 27,038,721 | 300000 | 300000 | 0 |
| S | mixed | 2 | 5.502154192 | 27,262,050 | 300000 | 300000 | 0 |
| S | mixed | 3 | 5.491730534 | 27,313,795 | 300000 | 300000 | 0 |
| U | idle | 1 | 1.976154196 | 25,500,034 | 205071 | 100784 | 6518 |
| U | idle | 2 | 2.050006013 | 26,315,776 | 230511 | 107895 | 7704 |
| U | idle | 3 | 1.896314073 | 26,039,463 | 202027 | 98758 | 6471 |
| U | burst | 1 | 2.184941672 | 25,873,917 | 246090 | 113066 | 8319 |
| U | burst | 2 | 2.174021161 | 25,064,613 | 240173 | 108982 | 8231 |
| U | burst | 3 | 2.194675043 | 25,411,279 | 245116 | 111539 | 8367 |
| U | sustained | 1 | 2.100138029 | 24,967,883 | 232637 | 104872 | 7998 |
| U | sustained | 2 | 2.034855033 | 24,762,206 | 215601 | 100775 | 7181 |
| U | sustained | 3 | 2.040039251 | 24,702,221 | 213468 | 100787 | 7043 |
| U | mixed | 1 | 2.297499206 | 25,499,029 | 252677 | 117168 | 8510 |
| U | mixed | 2 | 2.206089856 | 25,283,648 | 233108 | 111556 | 7692 |
| U | mixed | 3 | 2.245584876 | 24,265,171 | 241985 | 108979 | 9188 |
| W | idle | 1 | 1.298202542 | 5,040,816 | 7357 | 818 | 6539 |
| W | idle | 2 | 1.305539804 | 4,951,209 | 7178 | 808 | 6370 |
| W | idle | 3 | 1.235602605 | 5,296,201 | 7132 | 818 | 6314 |
| W | burst | 1 | 1.274942620 | 5,132,780 | 8619 | 818 | 7801 |
| W | burst | 2 | 1.321090516 | 4,953,483 | 8723 | 818 | 7905 |
| W | burst | 3 | 1.275439427 | 5,130,781 | 8850 | 818 | 8032 |
| W | sustained | 1 | 1.241187142 | 5,272,372 | 7248 | 818 | 6430 |
| W | sustained | 2 | 1.320374123 | 4,956,171 | 7521 | 818 | 6703 |
| W | sustained | 3 | 1.217479690 | 5,375,038 | 7780 | 818 | 6962 |
| W | mixed | 1 | 1.319268104 | 4,960,326 | 9471 | 818 | 8653 |
| W | mixed | 2 | 1.285755092 | 5,089,616 | 9065 | 818 | 8247 |
| W | mixed | 3 | 1.266856191 | 5,165,543 | 8824 | 818 | 8006 |
| P | idle | 1 | 15.047140055 | 7,125,706 | 300000 | 214443 | 42456 |
| P | burst | 1 | 15.043930081 | 7,304,674 | 300000 | 219782 | 39647 |
| P | sustained | 1 | 15.044334642 | 7,278,554 | 300000 | 219002 | 40232 |
| P | mixed | 1 | 15.045307488 | 7,083,471 | 300000 | 213146 | 43012 |

### Per-Trial Disk Bytes

For S, each pattern's row applies identically to repeats 1, 2, and 3. Other rows
are individual trials. F includes the loss diagnostic as defined above; A is the
sum of finalized allocated bytes, not logical output or physical write traffic.

| Group | Pattern | Repeat | F: Logical File Bytes | A: Finalized Allocated Bytes | Peak Sampled Allocated Bytes |
| --- | --- | --- | ---: | ---: | ---: |
| S | idle | 1/2/3 | 9713480 | 9732096 | 9736192 |
| S | burst | 1/2/3 | 19281352 | 19316736 | 19320832 |
| S | sustained | 1/2/3 | 16632840 | 16666624 | 16670720 |
| S | mixed | 1/2/3 | 28616072 | 28651520 | 28655616 |
| U | idle | 1 | 3614137 | 3637248 | 3641344 |
| U | idle | 2 | 3729913 | 3760128 | 3768320 |
| U | idle | 3 | 3234481 | 3256320 | 3260416 |
| U | burst | 1 | 8734777 | 8761344 | 8769536 |
| U | burst | 2 | 7430393 | 7454720 | 7462912 |
| U | burst | 3 | 9069945 | 9097216 | 9101312 |
| U | sustained | 1 | 6215545 | 6238208 | 6242304 |
| U | sustained | 2 | 5999225 | 6025216 | 6029312 |
| U | sustained | 3 | 6069113 | 6090752 | 6094848 |
| U | mixed | 1 | 10695865 | 10719232 | 10723328 |
| U | mixed | 2 | 10436089 | 10461184 | 10465280 |
| U | mixed | 3 | 9692089 | 9719808 | 9723904 |
| W | idle | 1 | 8756449 | 8773632 | 8781824 |
| W | idle | 2 | 8760417 | 8777728 | 8781824 |
| W | idle | 3 | 8756449 | 8773632 | 8781824 |
| W | burst | 1 | 9780449 | 9797632 | 9805824 |
| W | burst | 2 | 9780449 | 9797632 | 9805824 |
| W | burst | 3 | 9780449 | 9797632 | 9805824 |
| W | sustained | 1 | 9236321 | 9252864 | 9256960 |
| W | sustained | 2 | 9680609 | 9699328 | 9707520 |
| W | sustained | 3 | 8946977 | 8966144 | 8970240 |
| W | mixed | 1 | 9680865 | 9699328 | 9703424 |
| W | mixed | 2 | 9645089 | 9662464 | 9670656 |
| W | mixed | 3 | 9667809 | 9687040 | 9695232 |
| P | idle | 1 | 12173202 | 12197888 | 12201984 |
| P | burst | 1 | 19370898 | 19410944 | 19419136 |
| P | sustained | 1 | 18892562 | 18931712 | 18939904 |
| P | mixed | 1 | 25530450 | 25559040 | 25567232 |

### UDP Milestone Times

All entries are seconds from the common start, **not additive stage durations**.
Reconstruct sender metrics/s as `N*C / sender_seconds`, receiver metrics/s as
`R*C / receiver_seconds`, and decoder metrics/s as `R*C / decoder_seconds`.
The durable-output denominator remains the end-to-end seconds in the first
table, not any of these milestones. Standalone has no UDP milestones.

| Group | Pattern | Repeat | Sender (s) | Receiver (s) | Decoder (s) |
| --- | --- | ---: | ---: | ---: | ---: |
| U | idle | 1 | 1.878189067 | 1.904286549 | 1.908654962 |
| U | idle | 2 | 1.980849631 | 2.004051545 | 2.005475434 |
| U | idle | 3 | 1.824696147 | 1.852007192 | 1.853815181 |
| U | burst | 1 | 2.106092062 | 2.131048293 | 2.132862722 |
| U | burst | 2 | 2.096714093 | 2.119999272 | 2.121927863 |
| U | burst | 3 | 2.127566974 | 2.147271685 | 2.149716993 |
| U | sustained | 1 | 2.019967828 | 2.046637714 | 2.048710839 |
| U | sustained | 2 | 1.919580330 | 1.947320625 | 1.948611356 |
| U | sustained | 3 | 1.950883858 | 1.973621302 | 1.975741404 |
| U | mixed | 1 | 2.192648375 | 2.214016364 | 2.215305398 |
| U | mixed | 2 | 2.144315698 | 2.155506515 | 2.157686612 |
| U | mixed | 3 | 2.169298758 | 2.194720871 | 2.196117641 |
| W | idle | 1 | 0.730138838 | 0.771095268 | 0.796688332 |
| W | idle | 2 | 0.703897542 | 0.743543909 | 0.767914566 |
| W | idle | 3 | 0.719088150 | 0.760569240 | 0.786230662 |
| W | burst | 1 | 0.850044128 | 0.893763478 | 0.919224720 |
| W | burst | 2 | 0.856831820 | 0.900413369 | 0.925166207 |
| W | burst | 3 | 0.880281166 | 0.923337994 | 0.950692238 |
| W | sustained | 1 | 0.735336415 | 0.777242247 | 0.801768642 |
| W | sustained | 2 | 0.762396548 | 0.802162555 | 0.826557101 |
| W | sustained | 3 | 0.743854030 | 0.788709727 | 0.813085135 |
| W | mixed | 1 | 0.939018785 | 0.981009727 | 1.005604200 |
| W | mixed | 2 | 0.915192310 | 0.959767221 | 0.986177886 |
| W | mixed | 3 | 0.902404886 | 0.946837118 | 0.974210915 |
| P | idle | 1 | 15.000015533 | 15.000062450 | 15.000090512 |
| P | burst | 1 | 14.999972709 | 14.999993423 | 15.000018163 |
| P | sustained | 1 | 15.000016081 | 15.000040792 | 15.000069483 |
| P | mixed | 1 | 14.999972245 | 14.999992424 | 15.000017739 |
