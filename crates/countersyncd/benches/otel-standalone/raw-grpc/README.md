# Raw gRPC sink: client transport limit experiment

This is **not an OpenTelemetry Collector**. It uses grpc-go to receive complete
unary Export RPC messages and a custom codec that only records byte length. It
does not parse Protobuf or validate any metric. It returns an empty valid
ExportMetricsServiceResponse after the complete request is received, then discards
the data. Production use would silently lose all telemetry.

Standard Collector OTLP receivers decode before passing data to an exporter; a
nop exporter does not skip decoding. This separate sink removes that variable
when investigating client build/encode/gRPC transport performance.

```sh
go build -o /tmp/raw-grpc-sink .
python3 run.py --server /tmp/raw-grpc-sink \
  --client /tmp/otel-pr4883-linux-bench/target-linux-concurrent/release/otel-throughput \
  --output /tmp/raw-results --points 200000000 --inflight 1 2
```

Use the actual Rust actor benchmark compiled with `benchmark,mimalloc` and native
CPU codegen. Same 500 series, same three production attributes, changing values,
and complete 64-bit timestamps. Source data construction is outside ACK timing.
`run.py` checks the number of complete RPCs and exact protobuf byte count against
the same payload's previous FULLY DECODING Collector run. It cannot verify
decoded point counts or value integrity because this server deliberately does
not decode. Reported Mpoints/s is client-generated points / complete-ACK time.

## Local results on zegan-dev-vm

Ubuntu / Xeon 8370C, Rust 1.90.0, grpc-go 1.71.0 built with Go 1.24.1. Sender
runtime pinned to CPU 0, raw server pinned to CPU 14 / GOMAXPROCS=1. Localhost,
no compression/TLS, no downstream processing. Server buffers the full gRPC
message, not just its headers. No pre-encoded payload replay by the client.

40M points/trial, two trials, 10K batch:

| In-flight | Mpoints/s | Client CPU equivalents |
| ---: | --- | --- |
| 1 | 6.688 / 6.854 | .853 / .847 |
| 2 | 7.720 / 7.540 | .999 / .999 |
| 4 | 7.478 / 7.539 | .996 / .999 |
| 8 | 6.965 / 7.398 | .944 / .949 |
| 16 | 7.187 / 7.240 | .968 / .974 |

200M points/trial, two-trial confirmation:

```
N=1: elapsed=29.725664 / 28.697626s; Mpoints/s=6.728 / 6.969; CPU=.852 / .851
N=2: elapsed=25.659788 / 24.582834s; Mpoints/s=7.794 / 8.136; CPU=.987 / 1.000
```

Each pair delivered 40,000 complete requests and 38,413,320,000 protobuf bytes,
exactly matching the encoded 400M-point workload. Giving the raw server four
physical cores yielded 7.835 / 7.948M/s at N=2 in shorter trials: no clear gain.
100K batches were slower: N=1 3.994/4.016; N=2 5.011/4.554; N=4 4.515/4.285M/s.
1M batches: N=1 3.575/3.677; N=2 3.664/3.720M/s. Larger-buffer memory/transport
cost is a candidate, not a proven per-function explanation.

Interpretation: on this implementation and machine the single-core tagged client
achieves ~6.7-7M/s with one in-flight request when protobuf decode is removed from
the receiving side; ~7.8-8.1M/s with two saturates its CPU. This does NOT meet the
old 5M/s **single-flight fully-decoding Collector** goal. Neither it nor the
earlier untagged specialized-encoder experiment establishes a universal OTLP
limit or InfluxDB throughput. The raw sink itself still has transport/buffering
cost, and physical 25G NIC throughput was not measured.
