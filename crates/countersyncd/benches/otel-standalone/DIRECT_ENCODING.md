# Direct Tonic encoding with exact destination allocation

Selected implementation only: the actor passes an immutable Arc-owned request
of cached series metadata plus timestamp/value samples to the Tonic codec. The
codec writes standard OTLP protobuf directly into Tonic EncodeBuf and sets
BufferSettings to the exact protobuf length plus the five-byte gRPC envelope.
There is no intermediate expanded request buffer followed by a whole-request
copy, and no incremental growth from Tonic's default 8KiB initial capacity.

Samples are retained until ACK/retry completion. Retry attempts re-encode the
same immutable samples. Arc::try_unwrap returns sample/cache storage for reuse
only after all transport references are gone; retained clones are never mutated.
Cached series identity lookup remains the original SeriesKey HashMap. No template
offset tables, alternate codec modes, request-Bytes reuse pool, or speculative
sample preallocation policy are included. Direct encoding uses a compact
timestamp/value sample representation so attributes can be written directly to
the destination, rather than materializing an intermediate protobuf byte stream.

Export schema, canonical SAI names, point attributes, timestamps, values, ordered
worker lanes, input messages and routing are unchanged. This is not zero-copy
network I/O: per-point attributes and gRPC/kernel transport still incur writes.

## Final selected-code validation

Standalone suite: 56 tests pass, one manual calibration ignored. Linux delivery
and pool tests pass, including identical retry payload, partial failure, tail
flush, exact fields, duplicates, per-series order and concurrency ceilings.
Additional unit tests verify immutable direct requests, exact framed size,
sample capacity recovery, changed shapes/resources, and protobuf decoding.

zegan-dev-vm, Ubuntu Xeon 8370C, Rust 1.90 release/native/mimalloc. Three sending
cores + one live routing core, two ordered lanes per worker, 10K batch, 100 real
port stats across 500 series. No TLS/compression; loopback, not physical NIC.

Raw grpc-go receiver (no decoding, four other cores), two 200M-point trials:

```
elapsed_s=12.116195 acked_Mpoints_s=16.507
elapsed_s=12.057294 acked_Mpoints_s=16.587
received: 40000 complete requests, 56642959680 protobuf bytes
```

Actual Go Collector core 0.123.0, four physical cores/GOGC=500/nop exporter,
two 40M-point trials:

```
elapsed_s=7.691708 acked_Mpoints_s=5.200
elapsed_s=7.845093 acked_Mpoints_s=5.099
accepted delta=80000000, refused delta=0
```

Raw receiver checks request/byte counts, not decoded samples; correctness is
covered separately. Go runs verify accepted point counts. Neither result is
InfluxDB persistence throughput or a 50M/s claim. Earlier ~17M/s experiments
also changed identity lookup; that unrelated optimization was excluded and these
measurements rerun on the narrowed code selected for submission.

Reproduction uses the commands from NAMED_METRICS.md with the new binary, the
same --pool --named-schema workload, three workers/two lanes. No encoding mode
environment variable or benchmark feature is needed for this implementation.
