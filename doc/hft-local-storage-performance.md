# Local Storage Performance

> **HISTORICAL / OBSOLETE: v1 lossy range-summary report, not current v4 raw storage.**
> The measurements, validation counts, formats, and reproduction commands below
> describe the retired 10 ms reducer/Parquet implementation. They are retained as
> historical evidence, not results for the current `sonic-hft-arrow-v4` Arrow IPC
> raw-gauge writer. Do not use these summary rates or compression sizes to claim
> lossless raw-capture performance or interrupt recovery. The 50 million raw
> metrics/s goal has not been achieved. See [current storage documentation](hft-local-storage.md)
> for the v4 schema, reader API, durability conditions, and limitations, and
> [matrix measurements](hft-local-storage-matrix-performance.md) for the subsequent
> raw-storage benchmarks (before the 100 MB / 30-minute rotation change).

Measured 2026-09-06 on the local-storage port based on IPFIX PR4860 commit
`2e9ede3e40ea85a326e37e33d60d1940a8956b56`. Source storage is Pterosaur PR7 commit
`2af702c39226f61e3d2c3d845e3e28aa38bcdfa3`.

## Metric And Scope

One metric is one input counter observation. **metrics/s = sum(sample_count) in
published, read-back Parquet / measured elapsed seconds**. This measures input
observations represented by 10 ms range summaries, not a lossless raw-sample
archive. Summarization discards intermediate values; ZSTD compresses stored rows
losslessly.

- UDP: real IPv4 loopback send/receive, actual IPFIX actor, nonblocking local tap,
  reducer, Parquet writer, final close/fsync, atomic publication and writer join.
- Standalone: prebuilt flat batches, blocking bounded input enqueue, the same
  reducer/writer/publication/join. No IPFIX or socket processing.
- Generation, template setup, readback and directory cleanup are outside timing.
  Data uses deterministic unique continuing sequence numbers, not a repeating
  small payload pool. Prebuilt input is capped at 120 MB.
- Each segment has 6,000 records (3 million metrics), is finalized, verified and
  deleted before the next. Time sums segment times. These are **segmented tests,
  not uninterrupted sustained-load or maximum-capacity claims**.
- Each record has 500 counters; source time advances 10 us. UDP payload is 4,028
  bytes. Loopback does not establish physical-network MTU/fragmentation, Netlink,
  switch hardware, Redis or OTLP performance. Other production sinks are disabled;
  UDP has a 64-batch audit sink hashing every decoded value.
- Storage capacity is production's 32 flat batches; writer capacity one shard;
  UDP-to-IPFIX capacity 256 buffers. Two Tokio workers plus sender, reducer, writer
  and monitoring threads. No CPU pinning or cache eviction.
- Readback verifies identities, timestamps, nonoverlapping sample spans, sample
  counts, first/last/min/max and clean staging. Only `_READY` shards count. Sent,
  received, decoded and persisted counts are separately reported.

## Workloads

| Pattern | Definition |
| --- | --- |
| constant | Every series exactly unchanged, the strongest unchanged case |
| large100 | All observations use deterministic full-width u64 changes |
| large33 | 167/500 (33.4%) series change greatly; remaining 333 increment by one |
| large66 | 334/500 (66.8%) series change greatly; remaining 166 increment by one |

Mixed groups use index modulo three. Large values use SplitMix64-derived values,
including decreases, not monotonic-only counters.

## Results

All throughput values are **metrics/s**, rounded to integers. Three repeats per
case; headline is median. All 24 final trials had zero UDP/decode/storage losses,
readback errors and partially verified rows.

| Pattern | UDP median | UDP min-max | Standalone median | Standalone min-max |
| --- | ---: | ---: | ---: | ---: |
| constant | 993,136 | 992,900-993,608 | 34,798,858 | 34,695,005-34,904,575 |
| large100 | 992,419 | 992,134-992,999 | 28,099,224 | 27,312,636-28,747,861 |
| large33 | 992,914 | 992,538-993,171 | 30,596,192 | 29,863,613-30,879,071 |
| large66 | 992,633 | 992,586-993,087 | 29,907,572 | 29,559,331-30,083,207 |

UDP was **paced at 1,000,000 offered metrics/s**, not saturation. Each trial sent
and persisted 30 million metrics over 10 segments, taking 30.19-30.24 measured
seconds. It establishes completion at that offered rate for these trials, not a
maximum of 1 million. Standalone was unpaced with blocking producer backpressure:
120 million persisted metrics over 40 segments, 3.44-4.39 measured seconds.
Finalization and fsync are included. UDP trials produced 35,000 summary rows each;
standalone trials produced 140,000 each.

| Pattern | UDP repeats (metrics/s) | Standalone repeats (metrics/s) |
| --- | --- | --- |
| constant | 992900, 993136, 993608 | 34695005, 34798858, 34904575 |
| large100 | 992134, 992419, 992999 | 28747861, 28099224, 27312636 |
| large33 | 993171, 992538, 992914 | 30879071, 29863613, 30596192 |
| large66 | 992633, 993087, 992586 | 29559331, 29907572, 30083207 |

### Higher Offered Rates

- At 5,000,000 offered metrics/s with the final 32-batch queue, persisted throughput
  was 4,798,567-4,837,948 metrics/s. Ten of twelve trials (60 million metrics each)
  were lossless; large33 repeat 1 dropped 5,500 storage-input metrics and large66
  repeat 2 dropped 1,500. No UDP losses. Strict lossless validation correctly
  failed. This is not an all-pattern lossless guarantee.
- Earlier 64-batch exploration at 10,000,000 offered metrics/s achieved
  9,234,200-9,408,082 persisted metrics/s, with storage loss in two trials. That
  queue differs from production and is not the headline comparison.
- Earlier unpaced 64-batch UDP tests lost 44,449,000 UDP-carried metrics and
  3,565,500 storage-input metrics out of 720 million sent. Loss-affected segments
  wait up to 250 ms idle before finalization, included in elapsed time. The
  approximately 8,365,360-10,499,420 metrics/s is timeout-inclusive overload
  behavior, **not decoder/storage capacity**. Partial-row verification is reported.

## Disk Budget

Benchmark quota is 2,400,000,000 bytes, not production's 4 GB. The writer reserves
space before staging. A watchdog samples the private tree, including staging and
directory blocks, every 10 ms and aborts at 2,950,000,000 bytes. Tmpfs/overlay roots
are rejected. Each segment is removed before the next; no raw input file is made.

Final tests' maximum sampled simultaneous footprint was **229,376 bytes**, far
below 3 GB. Sampling is not an exact peak guarantee. Maximum per-trial finalized
shard allocations summed to 8,622,080 bytes (standalone large100). Across the 24
final trials the sum was 85,561,344 bytes. These are allocated output bytes, not
physical write amplification or bytes for every raw metric. Small output follows
from 10 ms reduction and segment cleanup.

## Environment And Validation

- Azure VM: Xeon Platinum 8370C @ 2.80 GHz, 16 logical CPUs (8 cores, SMT), Linux
  `6.8.0-138-generic`; shared VM, not isolated bare metal.
- Disk: host `/dev/sdc1`, ext4 (`rw,relatime`), bind-mounted into the container.
- Fresh container `ipfix-local-storage-verify`, image
  `sonicdev-microsoft.azurecr.io:443/sonic-slave-bookworm:master-amd64`.
- rustc/cargo 1.86.0, `RUSTFLAGS=-Dwarnings`, optimized benchmark build.
- CI reference: `.azure-pipelines/build-template.yml`. Artifacts selected with
  latestFromBranch master: swss-common 1212995 (partial success eligible), common-lib
  1213281 (succeeded). Installed libswsscommon/dev 1.0.0, libyang3 3.12.2-1 and
  SONiC libnl 3.7.0-0.2+b1sonic1. Redis uses AKE notifications and Unix socket.
- Sairedis 1213229 downloaded but not needed/installed for targeted Rust tests.
  No full Debian package build or DVS validation claim.
- Parallel and serial suites each pass 424 executions: 198 library, 209 binary,
  2 integration, 11 IPFIX-helper integration and 4 helper executions. Shared tests
  run in multiple targets. All-target check, bench compilation and release pass.
- Initial wall-flush test accidentally overloaded its best-effort writer queue;
  replaced with deterministic deadline/queue-drain tests before full reruns.
  Setup libnl install ordering and libyang3 extraction were corrected before tests.
- Dedicated mount deployment, blocked-I/O signal deadlines, physical hardware
  performance and power-loss durability were not tested.

## Reproduce

Inside the prepared CI container at the repository root, with real disk mounted:

```sh
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 900s \
  cargo check --locked -p countersyncd --all-targets
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 900s \
  cargo test --locked -p countersyncd
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 900s \
  cargo test --locked -p countersyncd -- --test-threads=1
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 900s \
  cargo bench --locked -p countersyncd --no-run
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 900s \
  cargo build --release --locked -p countersyncd

RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 1200s \
  cargo bench --locked -p countersyncd --bench local_storage_perf -- \
  --root /workspace/repos/sonic-swss --records 60000 --counters 500 \
  --repeats 3 --mode udp --rate 1000000 --require-lossless
RUSTFLAGS=-Dwarnings timeout --signal=TERM --kill-after=30s 900s \
  cargo bench --locked -p countersyncd --bench local_storage_perf -- \
  --root /workspace/repos/sonic-swss --records 240000 --counters 500 \
  --repeats 3 --mode standalone --rate 0 --require-lossless
```

Output is JSONL with counts, loss, readback errors, metrics/s, allocation totals and
timing limits. `--help` describes smoke runs and alternative source timestamp
spacing. Changing source spacing changes reduction ratio and comparison scope.
