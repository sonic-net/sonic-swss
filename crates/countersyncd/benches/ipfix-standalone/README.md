# IPFIX shared counter metadata: isolated review and benchmark

This PR is based directly on upstream master bb6c96de. It extracts the shared
IPFIX output representation from the larger OTel experiment, without OTel
encoding, naming, routing or worker-pool changes. Shared code overlaps PR #4883;
merge/rebase coordination is needed rather than independently applying both.

## Where the speedup comes from

The IPFIX framing/validation, field offsets, endian reads and timestamp handling
are unchanged. Previously every sampled counter produced a SAIStat containing
Arc<str> object_name, type_id, stat_id and counter. On a typical x86-64 layout
that is 32 bytes per counter, with an Arc increment on production and decrement
on destruction for every sample (not a fresh allocation of the string itself).

Now the compiled template owns Arc<[SAIStatMetadata]> once. Each sampled record
stores its timestamp and appends only u64 counter values, with metadata shared
at record/batch granularity. This reduces repeated output writes, reference-count
operations and downstream read/destruction traffic. E.g. 500 counters write
roughly 4KB of values rather than 16KB of SAIStat storage, plus record metadata.
This is not a claim that endian parsing itself became three times faster.

`records()` returns a unified borrowed iterator over owned/shared records and
stat fields without per-point allocation. CounterDB/StatsReporter adopt it with
minimal consumer-loop changes. Existing `iter()` remains a lazy OnceLock slice
adapter for old callers. It materializes owned SAIStat values once and costs
memory/Arc operations. Held template Arcs preserve old identities after config
updates; mixed representations, duplicates and shared-preserving split are tested.

## Important regression boundary (keep this PR draft)

The old slice adapter is **not performance-neutral**. Unmodified upstream OTel
still calls `iter()` and therefore materializes shared batches. This PR does not
claim whole-daemon or OTel performance gains. Migrate remaining hot consumers or
choose a rollout policy before merge. Keeping this explicit boundary makes the
IPFIX/data-structure change independently reviewable; it is not a hidden gain
obtained by excluding a still-required production consumer cost.

The output representation stores both metadata and values. If a consumer asks
for the legacy projection, both may remain resident until the batch is dropped.
Metadata tables are searched by Arc identity; workload with many distinct template
generations may differ from this benchmark's stable template. Template registration
retains an additional descriptor array; setup/steady-state tradeoffs need review.

## Reproduce

```sh
cargo +1.90.0 test --locked --manifest-path crates/countersyncd/benches/ipfix-standalone/Cargo.toml
CARGO_TARGET_DIR=/tmp/ipfix-new-target RUSTFLAGS='-C target-cpu=native' \
 cargo +1.90.0 build --release --locked --features mimalloc \
 --manifest-path crates/countersyncd/benches/ipfix-standalone/Cargo.toml
/tmp/ipfix-new-target/release/countersyncd-ipfix-bench 500 400000000 3 borrowed
/tmp/ipfix-new-target/release/countersyncd-ipfix-bench 500 400000000 3 legacy
```

For baseline, copy only this standalone harness (including lockfile) to a clean
upstream master worktree, build with `--features mimalloc,baseline-legacy`, and
run the same arguments. `baseline-legacy` avoids references to new records() API.
No production Cargo dependency change is required. Benchmark-only mimalloc does
not change the daemon allocator.

## Independent branch measurements

zegan-dev-vm, Ubuntu, Xeon Platinum 8370C, Rust 1.90.0, native release/mimalloc.
One pinned CPU0 runs the actual IpfixActor, producer and checksum consumer on a
current-thread Tokio runtime. Template setup/readiness excluded. Timed work:
parsing, output construction, channel delivery, complete counter iteration,
checksum, destruction and actor drain. Input is a 16-message prebuilt pool,
not live packet generation/NIC ingestion. All count/record/checksum assertions
passed. Each row below shows three independent runs in old/new/new-legacy order.

| Counters/record | Points/trial | Old Mpoints/s | Shared + borrowed | Shared + legacy slice |
| ---: | ---: | --- | --- | --- |
| 2 | 40M | 5.891 / 5.886 / 5.870 | 5.938 / 5.951 / 5.955 | 5.331 / 5.354 / 5.352 |
| 500 | 400M | 65.355 / 65.148 / 65.294 | 211.857 / 212.642 / 211.742 | 44.641 / 44.641 / 44.631 |
| 8000 | 400M | 64.789 / 65.229 / 65.750 | 252.621 / 252.585 / 251.454 | 46.234 / 45.977 / 46.074 |

The new borrowed path is ~3.25x / ~3.86x on wide records; tiny records are roughly
unchanged. Legacy projection regresses ~32% / ~29% on wide records and ~9% on
tiny records. These are combined parser/output-consumer results, not pure parser
CPU profiles or network throughput. No OTel, Collector or InfluxDB measurement
is included. No universal speedup or formal no-regression guarantee is claimed.

## Validation

90 standalone tests pass on Linux and Windows: real IPFIX template generation,
cutover, deletion, malformed input, mixed widths, timestamp inference, batching,
fan-out, shared/owned iterator equivalence, lazy projection invalidation, split
and retained-generation tests; real StatsReporter and existing SAI table tests.
CounterDB uses records() and its native test adds shared-input assertions, but
native SONiC/Redis CounterDB integration and the full daemon build were not run.
The production IPFIX Criterion benchmark also switches to records(); use this
standalone tool's legacy mode to assess unmigrated consumer cost explicitly.
