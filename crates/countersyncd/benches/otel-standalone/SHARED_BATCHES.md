# Shared template metadata with unified consumer adapters

Rebase update: the current implementation uses the base branch's borrowed
`batch.iter()` / `SAIStatsRef` API throughout. The lazy owned projection and
`records()` alias described in the original measurements below are removed.
Metadata names are resolved at template construction, and consecutive metadata
reuse requires at most one pointer comparison per record. OTel routing and slot
plans use these borrowed views and cached names. Performance tables below are
historical pre-rebase measurements and have not been rerun on the rebased code.

## Representation and compatibility

IPFIX CompiledTemplate now owns an immutable Arc<[SAIStatMetadata]> of
object/type/stat identities. Each output SAIStatsBatch stores shared metadata
tables, a flat Vec<u64> of sampled values, record timestamps/ranges and optional
owned stats. Repeated records in a batch reuse one metadata entry. No string or
object Arc is cloned for each decoded counter. Existing record order, counter
order, duplicate samples, u64 values and original timestamps are preserved.

- `batch.records()` returns borrowed SAIRecordView / SAIStatsView. Its iterator
  yields SAIStatRef with the same identity/value fields without allocation.
- Existing `batch.iter()` and SAIStatsRef slice API remain available. For a shared
  batch they lazily materialize owned SAIStat values ONCE with OnceLock. This is
  a compatibility path, not a free iterator: using it on hot consumers forfeits
  the memory/Arc benefit and retains the projection for the batch lifetime.
- `push_record()` keeps supporting owned input. Mixed representations work and
  mutation invalidates the cached projection. Splitting into record batches
  preserves shared metadata and never requires the compatibility projection.
- CounterDB and StatsReporter now iterate records(), otherwise retaining their
  existing cache/update semantics. OTel consumes shared views directly. Debug
  logging may explicitly materialize data; production performance assumes debug
  is disabled. Existing IPFIX/end-to-end benchmarks use the new borrowed iterator.

The metadata Arc belongs to a compiled template generation, so queued old data
keep the correct old names/IDs after update, withdrawal or session replacement.
No numeric pointer-as-handle is exposed outside its owning lifetime.

## OTel routing and local series registration

The worker-pool router caches up to 64 source metadata generations by Arc identity,
holding the Arc to prevent allocator-address reuse from aliasing an old plan.
For each generation it precomputes lane field indexes and projected lane metadata.
Each sampled record then gathers numeric values using the cached indexes. Projected
metadata is shared and queued numeric batches move by batch ownership. Existing
owned input continues to use the old hash/copy path. Large lane records are split
into bounded metadata slices; this slower fallback preserves the point limit.

The OTel encoder caches per-metadata field-to-series-slot mappings (64 plans).
Registration occurs once per plan; sampled shared records append directly by slot,
with no per-point hash/Arc clone. It re-resolves the plan after export, because
the pre-existing series cache may evict. Cache eviction clears slot mappings.
Tests verify mixed pending owned values, repeated plans, and eviction rebuilding.

Numeric values are still copied once during lane gathering and once into the
encoder sample vector. This deliberately does NOT claim C's pure Vec transfer or
zero-copy; it preserves record adapters and multi-consumer fan-out. Thus actual
integration differs from the isolated A/B/C prototype. Adding another consumer
shares the batch Arc, not exclusive ownership of the same Vec.

Plan caches are bounded by entry count (64), not total metadata bytes. Large
templates and many generations can consume substantial retained memory. This is
an integration review point; benchmark warm/low-churn results do not characterize
worst-case dynamic configuration. No live pool resizing is implemented.

## Real IPFIX actor A/B

Baseline: 7f876ae production sources in a separate worktree. Both builds use the
same standalone manifest/lock and ipfix_shared_bench.rs; baseline-legacy selects
the baseline's old iterator only. New build uses records(). CPU 0 hosts the
actual IpfixActor, input producer and output consumer. Output is fully iterated
and summed, records/point counts checked, buffers dropped, actor drained. Template
installation and readiness probe excluded. Keep template sender open until data
drains: early experimental runs that closed it prematurely failed assertions and
were discarded, on BOTH versions. No production shutdown behavior was changed.

Ubuntu zegan-dev-vm, Xeon 8370C, Rust 1.90 release/native/mimalloc. A pool of 16
prebuilt valid IPFIX byte messages is replayed, with varying timestamps/values.
This isolates actor parsing/output cost; it is not physical packet I/O, a new
payload generator, or a claim of persistent storage throughput. Replaying the
pool repeats timestamps every 16 records, so this benchmark is not proof of
globally monotonically increasing live timestamps. Tests use distinct timestamps
to validate per-series order.

| Counters/record | Points/trial | Old Mpoints/s (3 trials) | Shared Mpoints/s (3 trials) |
| ---: | ---: | --- | --- |
| 2 | 40M | 5.522 / 5.435 / 5.443 | 5.429 / 5.489 / 5.510 |
| 500 | 400M | 64.690 / 64.932 / 65.012 | 210.240 / 210.354 / 210.186 |
| 8000 | 400M | 65.877 / 63.930 / 64.577 | 251.126 / 250.685 / 250.965 |

Wide-record results improve ~3.24x and ~3.87x respectively. Small two-counter
records are essentially unchanged within the observed variation; there is no
claim of a statistically proven no-regression bound. Wide-record peak RSS:
500 counters old ~16MiB/new ~12MiB, 8000 old ~32MiB/new ~24MiB. These working-set
sizes depend on channel/input pool sizes and cannot be extrapolated to production.

## Real IPFIX -> OTel -> raw gRPC integration A/B

Actual parser and router share CPU 0; OTel workers CPUs 2,4,6, two ordered lanes
each, 10K exports; raw receiver CPUs 8,10,12,14. Same 500-counter IPFIX inputs as
above and no pre-sharding. All exports ACKed before ending timing. Three rotated
old/new trials of 100M counters:

| Round | Old Mpoints/s | Shared Mpoints/s |
| --- | ---: | ---: |
| 1 | 16.316 | 20.660 |
| 2 | 15.678 | 20.393 |
| 3 | 15.853 | 21.155 |

Total-points/total-time: old ~15.94M/s, new ~20.73M/s (~30% improvement). Every run
received 10,000 full RPCs / 14,062,851,000 protobuf bytes. This sink does not
decode/check each value. A separate real IPFIX->shared fan-out->OTel->Tonic test
does full protobuf decode and asserts names, values, timestamps and per-series
order. No Go Collector or InfluxDB throughput is inferred from this raw-sink test.

## Tests and scope

Standalone now compiles the actual IPFIX and StatsReporter modules, so their
existing template generation/cutover/delete/truncation/width/time/fan-out and
reporter tests execute without native SONiC libraries. New tests cover mixed
shared/owned records, duplicate semantics, lazy compatibility projections,
mutation invalidation, shared-preserving splitting, held old generations,
reporter behavior, projected plans/slot eviction, and real decoded delivery with
a sibling legacy consumer. CounterDB adapter change and shared-input assertion
are included; native Redis/SONiC CounterDB tests were NOT executed here.

## Reproduction

```sh
cargo test --locked --manifest-path crates/countersyncd/benches/otel-standalone/Cargo.toml
CARGO_TARGET_DIR=/tmp/shared-target RUSTFLAGS='-C target-cpu=native' cargo build \
 --release --locked --features mimalloc --bin ipfix-shared-bench \
 --manifest-path crates/countersyncd/benches/otel-standalone/Cargo.toml
/tmp/shared-target/release/ipfix-shared-bench 500 400000000 3
```

Build old production sources with the SAME added standalone harness/lock and
`--features mimalloc,baseline-legacy`. Then use ipfix_shared_ab.py --old PATH
--new PATH --output DIR --fields 2 500 8000 --points COUNT --rounds 3. For
integration add --raw-server PATH and use --fields 500. Standalone integration
affinity restoration is Linux-only and requires CPUs 0,2,4,6; adapt on other hosts.

The 21M/s integrated result is encouraging, but still less than half the 50M/s
goal. Multi-template churn, fan-out consumer work, more cores and physical NIC
tests remain necessary before a production capacity claim.
