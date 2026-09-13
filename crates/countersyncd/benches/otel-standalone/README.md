# Single-flight OtelActor experiment (draft: throughput target not met)

This standalone package imports the **actual** actor, SAI message, OTel message,
and communication-statistics source files. It avoids the unrelated netlink and
SONiC native-library dependencies of the daemon. It is not a replacement actor.

## Reproduce

From the repository root (Rust 1.90.0 was used locally):

```sh
cargo test --locked --manifest-path crates/countersyncd/benches/otel-standalone/Cargo.toml
cargo run --release --locked --manifest-path crates/countersyncd/benches/otel-standalone/Cargo.toml -- 10000 4000000 3
cargo run --release --locked --manifest-path crates/countersyncd/benches/otel-standalone/Cargo.toml -- 100000 4000000 3
```

Arguments: points per export, total input points, independent trials. Total input
must be divisible by 500. The workload has 500 metric/series identities, three
unchanged production point attributes (`object_name`, `sai_type_id`, `sai_stat_id`),
and 20 records per input channel batch. Every record has a distinct 64-bit
nanosecond timestamp; values also change for every record/metric. Inputs and
expected checksums are constructed before timing. Memory consumption scales
with total input size; do not use an unbounded point count.

The producer, actor, and Tonic client run on one current-thread Tokio runtime
pinned to one logical CPU. A fully decoding Tonic/Prost receiver runs on a
different pinned logical CPU. No HTTP handler that only drains bytes, no
pre-encoded request replay, no SDK aggregation, no compression, no TLS, no
concurrent exports. The actor waits for the unary gRPC response before preparing
and exporting the next request. The server verifies point count and checksums
of timestamps, values, and metric/timestamp/value tuples. Exact attribute/value
equivalence, duplicate preservation, large varint lengths, retry replay, tail
flush, timer flush, and partial-success failure have separate tests. Checksums
are a throughput sanity check, not a collision-free proof of every sample.

`acked_Mpoints_s` includes actor processing, channel consumption/input disposal,
request assembly, gRPC encode/send, complete server decode, checksum verification,
response, final flush and actor shutdown. It includes connection establishment
and cold metadata caches. It excludes input generation, server setup/join and
runtime creation. `received_elapsed_s` stops when the last handler has counted
the points; it is diagnostic only, not the acceptance metric.

The actor's old one-second shutdown sleep was removed because every export is
already awaited; the final elapsed measurement now includes actual response
completion. Older baseline measurements below stopped at final receipt to avoid
that artificial one-second delay, which slightly favors the old implementation.

The receiver's 128-MiB decode limit permits batch-size experiments. Production
receivers must be sized separately: these **tagged** 100K-point exports are about
9.2 MB, exceeding Tonic's default 4-MiB limit. Increasing only the actor's counter
threshold is insufficient. Server HTTP/2 windows are 16/32 MiB; client transport
defaults remain unchanged. The actor still defaults to 10K points per export.

### Diagnostic variants (not default production settings)

```sh
# Change both client and test receiver's allocator; label results accordingly.
RUSTFLAGS='-C target-cpu=native' cargo run --release --locked --features mimalloc --manifest-path crates/countersyncd/benches/otel-standalone/Cargo.toml -- 10000 4000000 3

# Fully decode/drop an already encoded production-schema request, without RPC.
OTEL_DECODE_CALIBRATION=1 RUSTFLAGS='-C target-cpu=native' cargo run --release --locked --features mimalloc --manifest-path crates/countersyncd/benches/otel-standalone/Cargo.toml -- 10000

# Actual actor buffer encoding only; DOES NOT measure delivery.
RUSTFLAGS='-C target-cpu=native' cargo test --release --locked --features mimalloc --manifest-path crates/countersyncd/benches/otel-standalone/Cargo.toml encoding_throughput -- --ignored --nocapture

# More receiver workers, still only one in-flight RPC from the client.
OTEL_SERVER_THREADS=8 RUSTFLAGS='-C target-cpu=native' cargo run --release --locked --features mimalloc --manifest-path crates/countersyncd/benches/otel-standalone/Cargo.toml -- 100000 4000000 2
```

On PowerShell set environment variables with `$env:NAME='value'` and clear them
after the run. The optional allocator affects this benchmark executable only.

## Local results

Windows VM, Intel Xeon Platinum 8370C @ 2.80 GHz, 8 reported cores / 16 logical
CPUs. Localhost TCP, one sender runtime thread and one receiver runtime thread,
no physical NIC test or host/VM isolation. Rust 1.90.0; Tonic 0.12.3, Prost 0.13.5;
other dependencies pinned in this package's Cargo.lock. No production Linux
daemon build, real Go Collector or InfluxDB was used. These are local experimental
results, not vendor benchmarks or hardware-independent limits.

All runs below use four million points. Full output excerpts are in RESULTS.txt.

| Implementation / configuration | Batch | Mpoints/s |
| --- | ---: | ---: |
| Original actor, system allocator (final-receipt timing) | 10K | 0.149 |
| Original actor, system allocator (final-receipt timing) | 100K | 0.130 / 0.132 / 0.130 |
| Optimized actor, system allocator, full ACK timing | 10K | 0.622 / 0.698 / 0.719 |
| Optimized actor, system allocator, full ACK timing | 100K | 0.544 / 0.557 / 0.549 |
| Optimized, mimalloc + native CPU, full ACK timing | 10K | 1.009 / 1.018 / 1.041 |
| Optimized, mimalloc + native CPU, full ACK timing | 100K | 0.915 / 0.913 / 0.887 |
| Optimized, mimalloc + native CPU, full ACK timing | 1M | 0.818 / 0.814 |
| Optimized, mimalloc + native CPU, 8 receiver workers | 100K | 0.816 / 0.762 |

**The requested 5M/s target and 4M/s floor were NOT achieved.** Keep this PR draft.
Production-schema sender-only buffer assembly/encoding measured 10.789M/s.
Receiver-only Prost decode+drop measured 2.207M/s at 100K (mimalloc), and 2.307M/s
at 10K (mimalloc + native CPU). Thus this particular local fully decoding receiver
already consumes more than 4M/s's entire 250ns/point budget, before network or actor
costs. This is evidence of a receiver/allocation bottleneck in this setup, not a
universal bound on OTLP or a prediction for a different Collector implementation.

Raising batch size, enlarging HTTP/2 windows, using mimalloc, native CPU codegen,
and adding receiver runtime workers did not reach the target. More sender workers
would violate the single-in-flight condition; stripping attributes or timing only
encoding would change the workload. Next work should profile the receiver's
decode/allocate/drop path on the target Linux machine and repeat against the real
Collector. The present changes improve actor costs but cannot prove that target.

## Wire compatibility and review points

The actor now groups samples by `(object_name, type_id, stat_id)` to keep the
existing object-specific descriptions and all three per-point attributes. This
preserves per-series sample order, timestamps, values (including the existing
u64-to-i64 bit-preserving cast), duplicates and retry payloads. Global interleaving
of different series within one request changes, as permitted by the metric model.
No type/stat labels are dropped or relocated and the existing empty proto unit is
unchanged. Input records may now split at the exact configured counter threshold.

The specialized protobuf encoder requires careful review: it retains the standard
OTLP wire schema and Tonic unary gRPC framing, with Prost decode-equivalence tests.
Static metadata is cached, point buffers are reused, and retries clone immutable
Bytes instead of an entire request tree. Cache retention is limited to 4096 series
and 32 MiB of retained per-series buffers after a flush. Input batch size and
individual object-name lengths still determine peak request memory. No unlimited
background send queue is introduced.
