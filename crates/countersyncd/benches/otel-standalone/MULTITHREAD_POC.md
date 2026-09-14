# Thread-per-series-shard POC

`otel-multithread` creates one actual OtelActor, current-thread Tokio runtime,
independent gRPC channel, and bounded input channel per pinned OS thread. A stable
hash of `(object_name, type_id, stat_id)` assigns all samples of one series to one
worker. The workload remains 500 total series, NOT 500 series per worker. Each
sample retains all three production attributes, timestamp and value. Threads
start together; elapsed time ends after the slowest worker has received all ACKs.
Go receiver accepted-count deltas must equal the **total** input across workers.

This POC prepares and shards input before timing. Production router/hash/copy
cost and live sampling are NOT measured. It is a finite-input experiment, not
a production sharding actor. Input allocation is bounded by the requested point
count; 200M prebuilt samples consume several GiB. Worker-specific counts are
reported, and their sum is checked. Go tests verify point counts, not every value;
the existing wire/delivery tests cover exact field preservation. A routing test
checks all 500 series belong to exactly one worker.

## Reproduction on zegan-dev-vm

```sh
CARGO_TARGET_DIR=/tmp/otel-pr4883-linux-bench/target-linux-concurrent \
RUSTFLAGS='-C target-cpu=native' cargo +1.90.0 build --release --locked \
  --features benchmark,mimalloc --bin otel-multithread \
  --manifest-path crates/countersyncd/benches/otel-standalone/Cargo.toml

python3 crates/countersyncd/benches/otel-standalone/collector_bench.py \
  --collector /tmp/otel-pr4883-linux-bench/otelcol \
  --client /tmp/otel-pr4883-linux-bench/target-linux-concurrent/release/otel-multithread \
  --output /tmp/otel-pr4883-linux-bench/multithread \
  --client-cpus 0,2 --cpus 4,6,8,10,12,14 --gomaxprocs 6 \
  --inflight 8 --batch 10000 --points 200000000 --repeats 2 --gogc 500
```

`--client-cpus` selects one worker per listed CPU. `--inflight` is **per worker**:
two workers with eight each have at most 16 aggregate in-flight requests and two
connections. Thread count experiments therefore also change connection count,
per-shard metadata locality, and aggregate concurrency; they do not isolate only
CPU parallelism. N=1 preserves order within a series in the successful path;
N>1 can reorder requests within a worker and needs production ordering policy.
Concurrent benchmark exports fail fast without retries, as documented in README.

## Results

Ubuntu, Xeon Platinum 8370C, 8 physical / 16 logical CPUs. Explicit masks below
use distinct physical cores and no overlapping client/Collector CPUs. Actual Go
Collector core v0.123.0, nop exporter, GOGC=500, no TLS/compression. Same 10K batch.

### Fixed four Collector physical cores (8,10,12,14)

40M total points/trial, two trials each:

| Client workers | Client CPUs | In-flight per worker | ACKed total Mpoints/s |
| --- | --- | ---: | --- |
| 1 | 0 | 1 | 1.496 / 1.476 |
| 2 | 0,2 | 1 | 2.840 / 2.815 |
| 4 | 0,2,4,6 | 1 | 4.892 / 4.868 |
| 1 | 0 | 8 | 5.212 / 5.228 |
| 2 | 0,2 | 8 | 5.876 / 5.938 |
| 4 | 0,2,4,6 | 8 | 5.864 / 6.035 |

At four workers / N=8 the clients together consume ~0.95-0.96 CPU equivalents,
while Collector CPU is ~3.86 equivalents over the timed intervals. Extra sending
threads are waiting for the receiver; they are not four saturated sending cores.

### Fixed six Collector physical cores (4,6,8,10,12,14)

40M total points/trial, three trials each:

| Client workers | In-flight per worker | ACKed total Mpoints/s |
| --- | ---: | --- |
| 1 (CPU 0) | 8 | 6.746 / 6.758 / 6.792 |
| 2 (CPUs 0,2) | 8 | 8.084 / 8.103 / 8.151 |

### Longer confirmation, 200M total points/trial

Two senders, N=8 each, six Collector physical cores:

```
trial=0 elapsed_s=24.724463 acked_Mpoints_s=8.089 client_cpu_s=27.948596 client_cpu_cores=1.130
trial=1 elapsed_s=24.011682 acked_Mpoints_s=8.329 client_cpu_s=27.368946 client_cpu_cores=1.140
Collector accepted delta=400000000 refused delta=0 cpu delta=277.13 seconds
```

Collector uses ~5.69 CPU equivalents over the ~48.74s timed total, close to its
six allocated physical cores. Final RSS ~518MiB (snapshot, not peak). The client
has two available cores but uses ~1.14, so the receive path is the limiting side.

Four senders, **N=1 each**, four Collector physical cores:

```
trial=0 elapsed_s=40.475232 acked_Mpoints_s=4.941 client_cpu_cores=0.893
trial=1 elapsed_s=41.026432 acked_Mpoints_s=4.875 client_cpu_cores=0.845
Collector accepted delta=400000000 refused delta=0
```

All matrix and confirmation runs matched accepted counts and had zero refused
points. These are receive/ACK rates, not InfluxDB durability or 25G NIC results.

## Interpretation

Multiple sender workers help, but cannot double receiver capacity. With six
Collector cores, adding a second worker raises ~6.77 to ~8.11M/s (~20%, not 2x).
With four Collector cores, raising sender workers from two to four at N=8 yields
little benefit. On this eight-physical-core VM, the best tested split is **two
sender cores + six Collector cores**, N=8 each. Four sender cores leave too few
receiver cores. More host cores or separate hosts are needed to measure further
scaling without taking cores away from the receiver. Existing pprof findings
(point/attribute decode, allocation and GC) explain the receiving CPU demand.
