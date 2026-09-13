# Ordered per-series sender scaling (raw gRPC receiver)

Same actor and 500-series tagged data as MULTITHREAD_POC.md. Each worker owns a
stable series shard and has **one** in-flight RPC; different workers use separate
connections. On successful runs input order is preserved within a series.
This is not an end-to-end ordering guarantee across crashes, ambiguous retries,
resizing, downstream queues or overlapping producers. Input is already ordered
and partitioned before timing. No service-side aggregation or persistence occurs.

Ubuntu zegan-dev-vm, Xeon 8370C, eight physical cores. Rust 1.90/native/mimalloc,
raw grpc-go 1.71 / Go 1.24.1, no protobuf decode, TLS or compression. 10K points
per request. Raw receiver validates complete request counts but cannot verify
point contents/order. Multiworker payload sizes differ because metadata is
grouped by shard; the harness does not claim exact point integrity from bytes.

| Sender physical cores | Raw receiver physical cores | Workload / repeats | Mpoints/s |
| ---: | ---: | --- | --- |
| 1 | 4 | 40M / 2 | 6.270 / 6.509 |
| 2 | 4 | 40M / 2 | 11.905 / 12.141 |
| 4 | 4 | 200M / 3 | 20.091 / 20.257 / 20.251 |
| 6 | 2 | 200M / 3 | 18.881 / 19.008 / 18.670 |

Four-sender confirmation: 600M generated samples, 60,000 received complete
requests, 55,609,995,600 protobuf bytes (~92.68 bytes/sample). Sender timed CPU
2.81-2.83 equivalents across four cores; aggregate wall time ends at last ACK.
Six senders use 2.84-2.92 equivalents across six cores. That run changes receiver
resources and has uneven static shard loads (28M-38.8M samples per 200M run), so
it is not a controlled proof that six sender cores are intrinsically slower.

CPU-only extrapolation, not a benchmark: sustaining the observed ~5M/s per
allocated sending core would suggest ~10 sending cores for 50M/s; begin tests
with 12-16 physical sender cores on a larger/separate host to allow headroom.
Nonlinear scaling, sharding imbalance and receiver/transport limitations remain
unmeasured. No 50M/s result has been obtained.

At the observed 92.68 bytes/sample, 50M/s needs ~37.07 Gbit/s payload. A 25G link
allows at most ~33.72M/s before network overhead (62.5 bytes/sample at 50M/s).
To reach 50M/s across 25G requires lossless compression or a more compact
schema/encoding; ~1.48x reduction just to fit line rate, ~1.85x if allocating 80%
of nominal bandwidth to payload. Compression ratio/CPU costs have not been
measured. Every original timestamp/value must remain represented.

Example:

```sh
python3 raw-grpc/run.py --server /tmp/otel-pr4883-linux-bench/raw-grpc-sink \
 --client /tmp/otel-pr4883-linux-bench/target-linux-concurrent/release/otel-multithread \
 --output /tmp/otel-pr4883-linux-bench/raw-multicore --points 200000000 \
 --repeats 3 --inflight 1 --multithread --client-cpus 0,2,4,6 \
 --server-cpus 8,10,12,14 --procs 4
```
