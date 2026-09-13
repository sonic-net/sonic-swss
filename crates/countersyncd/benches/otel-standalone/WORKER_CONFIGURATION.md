# Configurable ordered worker pool

The daemon now exposes explicit startup parameters (no automatic resizing):

| Parameter | Default | Meaning |
| --- | ---: | --- |
| `--otel-worker-threads` | 1 | Dedicated current-thread Tokio workers, 1..64 |
| `--otel-in-flight-per-worker` | 1 | Independent single-flight series lanes on each worker, 1..64 |
| `--otel-worker-cpus` | unset | Optional comma-separated logical CPU IDs, exactly one per thread |
| `--otel-worker-queue-capacity` | 8 | Queued input batches per lane, 1..64 |
| `--otel-max-counters-per-export` | 10000 | Existing point threshold per lane |
| `--otel-flush-timeout-ms` | 1000 | Existing flush timeout per lane |

Maximum total lanes/concurrent requests = threads * in-flight-per-worker (at most
1024). A lane is NOT one time series: each owns a stable hash partition of series
identities `(object_name, type_id, stat_id)`. Every series stays in exactly one
lane. Each lane uses its own actor/connection, awaits the prior response (including
its existing retry loop), then sends the next batch. Multiple lanes on one CPU
overlap waits without issuing concurrent requests for a single series. This pool
is compiled without the experimental `benchmark` feature; even with that feature
it overrides `OTEL_MAX_IN_FLIGHT` to keep each lane single-flight.

Example on a machine where these CPU IDs are available:

```sh
countersyncd --enable-otel --otel-endpoint http://collector:4317 \
  --otel-worker-threads 3 --otel-in-flight-per-worker 2 \
  --otel-worker-cpus 2,4,6 --otel-worker-queue-capacity 8 \
  --otel-max-counters-per-export 10000 --otel-flush-timeout-ms 1000
```

Omit the CPU list for OS scheduling. CPU IDs are logical processors; select
different physical cores deliberately if avoiding SMT contention. The router
executes on the daemon's existing Tokio runtime, in addition to worker threads.

## Ordering, backpressure and shutdown

Normal input close drains each bounded channel, flushes tails, awaits all ACKs,
and joins worker OS threads. A worker error/panic is reported to the pool even
when upstream is idle; remaining workers are cancelled and the caller receives an
error, never false success. Dropping the pool run future signals cancellation so
the daemon's supervisor can abort it. Completed sibling exports cannot be undone.
Retry on ambiguous network failures may still duplicate data; this is not an
exactly-once guarantee. Downstream queues/reordering are outside the client scope.

Ordering preserves *input order*, not automatic timestamp sorting. Already
out-of-order inputs remain so. No samples are aggregated, deduplicated or dropped
on a successful run. All three attributes, original timestamps/values and duplicate
samples are retained. Router buffers are limited to one export-sized batch per
lane plus one export-sized chunk, and queued messages contain at most the configured
point threshold. Original input ownership remains live while routing. Bounds are
in points/batches, not bytes; large names and many lanes require memory budgeting.
Resource metadata caches and buffers are per-lane. A hot/stalled lane can apply
backpressure to the central router and other lanes.

Worker count/lane count are fixed during a run. Live autoscaling would change the
hash ownership and needs an explicit drain/migration protocol; it is intentionally
not implemented. A larger configured concurrency than active series provides no
benefit. Tune based on CPU, queue/backpressure and ACK throughput.

## Benchmarks

`otel-pool-bench` uses the actual pool and includes live routing, hashing, stats
cloning, channels, worker startup and shutdown, and final ACKs. Only source data
generation is outside timing. This is stricter than `otel-multithread`, whose
input was pre-sharded. Do not compare the two as identical timing boundaries.

```sh
CARGO_TARGET_DIR=/tmp/otel-pool-target RUSTFLAGS='-C target-cpu=native' \
 cargo build --release --locked --features mimalloc --bin otel-pool-bench \
 --manifest-path crates/countersyncd/benches/otel-standalone/Cargo.toml

# Use the real Go Collector, validating its accepted/refused point counters.
python3 crates/countersyncd/benches/otel-standalone/collector_bench.py \
 --collector /path/to/otelcol --client /tmp/otel-pool-target/release/otel-pool-bench \
 --output /tmp/pool-results --pool --router-cpu 0 --client-cpus 2,4,6 \
 --cpus 8,10,12,14 --gomaxprocs 4 --inflight 1 2 4 --gogc 500 \
 --points 40000000 --repeats 2

# Raw grpc-go sink: transport only; validates complete requests, not point contents.
python3 crates/countersyncd/benches/otel-standalone/raw-grpc/run.py \
 --server /path/to/raw-grpc-sink --client /tmp/otel-pool-target/release/otel-pool-bench \
 --output /tmp/raw-pool-results --pool --router-cpu 0 --client-cpus 2,4,6 \
 --server-cpus 8,10,12,14 --procs 4 --inflight 1 2 4 --points 40000000 --repeats 2
```

Tests (`tests/pool.rs`) verify bounds, unavailable/duplicate CPUs, default values,
empty shutdown, exact attribute/value/timestamp/duplicate delivery, strict
per-series input order, one active request per lane, overlapping distinct lanes,
global concurrency ceiling, transient retry and idle-input error propagation.
These run against a decoding Tonic server with deliberately delayed responses.
Daemon CLI parser tests were added to main.rs; complete daemon build/CLI tests
still require the Linux SONiC native build environment.

## Results on zegan-dev-vm

Ubuntu, Xeon 8370C, Rust 1.90, 500 total series with all production point labels,
10K batch, router CPU 0, three sender worker CPUs 2,4,6, raw receiver CPUs
8,10,12,14. Production pool built with mimalloc/native CPU, **without benchmark
feature**. Total includes the separate routing CPU.

| In-flight lanes per worker | Total max requests | 40M-point trial Mpoints/s |
| ---: | ---: | --- |
| 1 | 3 | 15.335 / 14.884 |
| 2 | 6 | 17.665 / 17.305 |
| 4 | 12 | 18.123 / 16.638 |

Longer transport confirmation, two lanes per worker:

```
trial=0 threads=3 in_flight_per_worker=2 max_total_inflight=6 batch=10000 points=200000000 elapsed_s=11.885448 acked_Mpoints_s=16.827 live_routing=true
trial=1 threads=3 in_flight_per_worker=2 max_total_inflight=6 batch=10000 points=200000000 elapsed_s=11.735728 acked_Mpoints_s=17.042 live_routing=true
Raw receiver: 40000 complete requests, 36928008480 payload bytes.
```

Same pool against fully decoding Go Collector core v0.123.0 (four physical cores,
GOGC=500), two 40M-point runs: **5.423 / 5.406M/s**, accepted delta 80M, refused 0.
These are local receive/ACK rates, not InfluxDB writes, and not a 50M/s claim.
Raw receiver counts only request boundaries/bytes; correctness is verified in
the decoding tests separately. Final whole-system scaling still requires a larger
or separate sender/receiver host.
