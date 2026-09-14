# Canonical SAI names and refreshed performance matrix

## Export schema change

Known `(type_id, stat_id)` pairs now export the canonical SAI C names supplied
by countersyncd's existing SAI tables. For example `(1,0)`:

```
Metric.name = SAI_PORT_STAT_IF_IN_OCTETS
NumberDataPoint.attributes:
  object_name = Ethernet0
  sai_type = SAI_OBJECT_TYPE_PORT
  sai_stat = SAI_PORT_STAT_IF_IN_OCTETS
```

`sai_type_id` and `sai_stat_id` attributes are replaced, not supplemented. Metric
descriptions use names too. This changes measurement names and labels: dashboards,
queries and schema mappings referring to old names must be updated. Numeric IDs
remain internal for stable routing/cache keys; worker assignment, timestamp/value
encoding, retries and per-series ordering are unchanged.

Object names use `SaiObjectType`; stat-name resolution currently covers the
existing Port, Queue, BufferPool and IngressPriorityGroup tables (same coverage as
CounterDB). Unknown object IDs export `UNKNOWN_SAI_OBJECT_TYPE_<id>`; unsupported
or unknown stat pairs export `UNKNOWN_SAI_STAT_TYPE_<type_id>_ID_<stat_id>`. Both
IDs are retained in the fallback metric name to prevent collisions. This does
not invent names for unsupported/custom SAI IDs or drop their samples. Adding
new stat families requires their authoritative enum mapping.

The fast encoder resolves names on series-cache insertion and reuses encoded
metadata/attributes for subsequent samples. Message/debug conversion shares the
same resolver. Unit tests check canonical names across all four supported stat
families, unknown IDs/extensions, exact decoder equivalence and absence of old
ID attributes. Real RPC pool tests still validate full attributes, order,
duplicates and transient retries.

## Workload and measurement

Host: zegan-dev-vm, Ubuntu, Xeon Platinum 8370C, Rust 1.90, release native CPU
codegen with benchmark mimalloc. Production ordered pool, no benchmark feature,
10K points/request; router CPU 0, sender CPUs 2 / 2,4 / 2,4,6, receiver CPUs
8,10,12,14 fixed across all combinations. Thus sender core count is threads +
one routing core; receivers have four separate physical cores. No TLS or
compression; loopback transport, not a physical 100G measurement. Timing includes
live routing, hashing, cloning, channel operations, encoding and all ACKs.

Pool workload now uses **100 known PORT stat IDs (0..99) across 500 distinct
object/stat series**, by mapping stat_id=i%100. This avoids measuring unknown
fallbacks for nonexistent PORT stats (the previous synthetic workload used
0..499). It is NOT 500 distinct stat types. There are still 500 total time series,
and every record changes timestamp/value. Historical pre-sharded and single-actor
tools retain their synthetic 0..499 IDs and now exercise fallbacks too; use
otel-pool-bench for the all-known-name matrix reported here.

## Raw receive-only throughput

No server protobuf decode. Full request counts verified; no semantic point
validation at the raw server. Two 40M-point trials per combination, Mpoints/s:

| Sender threads | In-flight lanes per worker | Total lanes | Trial 1 | Trial 2 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 1 | 4.728 | 4.868 |
| 1 | 2 | 2 | 5.756 | 5.892 |
| 1 | 4 | 4 | 4.298 | 4.394 |
| 2 | 1 | 2 | 7.976 | 8.318 |
| 2 | 2 | 4 | 9.219 | 7.736 |
| 2 | 4 | 8 | 7.241 | 7.726 |
| 3 | 1 | 3 | 10.178 | 10.109 |
| 3 | 2 | 6 | 10.458 | 11.032 |
| 3 | 4 | 12 | 12.969 | 12.543 |

Longer confirmation, two 200M-point trials, three sender threads:

| Names/schema | Lanes per worker | Trial 1 | Trial 2 | Bytes / 400M points |
| --- | ---: | ---: | ---: | ---: |
| Old numeric export | 2 | 17.492 | 16.468 | 36,568,065,360 |
| New canonical names | 2 | 11.294 | 10.861 | 56,642,959,680 |
| Old numeric export | 4 | 17.139 | 16.710 | 36,424,929,280 |
| New canonical names | 4 | 13.445 | 13.026 | 56,385,125,120 |

The old-schema baseline is commit 582be6d with ONLY the new pool_bench input
generator copied into an isolated worktree, same dependency lock/compiler. This
controls for changed stat IDs/routing distribution. `--named-schema` on run.py
selects the input ID layout for independent request-count calculation; it does
not select the server's encoding or affect the client. Both sides of this
comparison use it. Every pair delivered 40,000 complete requests.

With four lanes/worker, the median-of-two throughput is ~13.24M/s versus ~16.92M/s
before (~22% lower); payload rises ~55%, from ~91 to ~141 bytes/point. With two
lanes, throughput drops ~35%. This is consistent with increased buffer copying/
transport work and scheduling sensitivity, not proof of an expensive per-sample
name lookup (the lookup is cached). No function profile was collected in this
round. Do not extrapolate these noisy two-trial results to a universal limit.

## Actual Go Collector (full decode)

Collector core v0.123.0, GOMAXPROCS=4, GOGC=500, nop exporter. Two 4M-point
short trials per combination; Mpoints/s:

| Threads | Lanes/worker | Trial 1 | Trial 2 |
| ---: | ---: | ---: | ---: |
| 1 | 1 | 1.267 | 1.266 |
| 1 | 2 | 2.404 | 2.413 |
| 1 | 4 | 3.914 | 3.357 |
| 2 | 1 | 2.360 | 2.372 |
| 2 | 2 | 4.132 | 4.072 |
| 2 | 4 | 4.051 | 3.780 |
| 3 | 1 | 3.143 | 3.264 |
| 3 | 2 | 3.871 | 4.665 |
| 3 | 4 | 4.404 | 4.509 |

Confirmation with 40M points/trial: 3 threads x 2 lanes **4.755 / 4.765M/s**;
3 x 4 lanes **4.673 / 4.728M/s**. Each pair accepted 80M points, refused zero.
This checks decoded-point counts, not every value. Separate RPC tests verify
semantics. None of these tests measure InfluxDB writes or durability.

## Reproduction

Build otel-pool-bench with the instructions in WORKER_CONFIGURATION.md. For raw
tests add `--named-schema` (input ID layout) to the existing raw runner:

```sh
python3 raw-grpc/run.py --server /path/raw-grpc-sink --client /path/otel-pool-bench \
 --output /tmp/named-results --pool --named-schema --router-cpu 0 \
 --client-cpus 2,4,6 --server-cpus 8,10,12,14 --procs 4 \
 --inflight 1 2 4 --points 40000000 --repeats 2
```

For full Collector use collector_bench.py with --pool; it reads decoded point
counters so it requires no input-layout switch. Existing historical raw byte-size
constants apply only to the old schema; pass --named-schema for named payloads.
