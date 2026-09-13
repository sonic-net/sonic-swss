# Go Collector bottleneck investigation

Follow-up to the single-flight draft and bounded-concurrency experiment. Same
zegan-dev-vm, Xeon 8370C, Rust 1.90.0, official Go Collector core v0.123.0.
One pinned Rust runtime thread, standard OTLP/gRPC, all three production point
attributes, 500 series, changing timestamps/values, nop exporter. No TLS,
compression or InfluxDB. Every measured run waits for all ACKs and matches the
Go accepted-point counter; all refused-point deltas were zero.

## Profile evidence

Go pprof CPU profile: 20.18 seconds at eight in-flight requests / 10K batch,
GOMAXPROCS=12, affinity 2-15, GOGC=100. Throughput during the 200M-point trial was
5.670M/s. Samples summed to 161.39 CPU seconds (~8 CPU equivalents).

| CPU call path | Cumulative share |
| --- | ---: |
| protobuf request Unmarshal | 68.78% |
| NumberDataPoint.Unmarshal | 58.67% |
| runtime.mallocgc | 45.13% |
| KeyValue.Unmarshal | 31.85% |
| runtime.growslice | 20.37% |
| runtime.gcDrain | 19.30% |

These paths overlap: **do not add the percentages**. Flat scanobject cost was
4.62%; gcDrain's cumulative cost includes other marking work. This is evidence
of substantial receiver decoding/allocation/GC overhead, not just network wait.
An allocation-space profile taken during that run attributed ~90.4% of allocated
bytes to the ResourceMetrics decode tree. The profile's ~59 GB is cumulative
allocation, NOT retained memory/RSS.

## Controlled experiments

Unless noted, 40M points/trial, two trials, client built with mimalloc and native
CPU. Go's allocator is unaffected by the Rust allocator choice.

| Collector / batch / in-flight | ACKed Mpoints/s | Timed client CPU equivalents |
| --- | --- | --- |
| P=12, CPUs 2-15, GOGC=500, 10K, N=8 | 6.776 / 6.798 | .883 / .885 |
| same, N=16 | 6.636 / 6.998 | .937 / .943 |
| same, N=32 | 6.030 / 6.434 | .945 / .934 |
| same, 5K, N=8 | 6.536 / 6.555 | .890 / .887 |
| same, 20K, N=8 | 6.207 / 6.643 | .874 / .889 |
| P=4, CPUs 2,4,6,8, GOGC=500, 10K, N=8 | 5.259 / 5.242 | .720 / .705 |
| P=7, CPUs 2,4,6,8,10,12,14, GOGC=500, 10K, N=8 | 6.858 / 6.876 | .926 / .928 |
| same P=7, GOGC=100 | 5.952 / 5.908 | .776 / .770 |
| same P=7, GOGC=1000 | 7.121 / 7.199 | .926 / .942 |

The P=7 mask chooses one logical CPU from each of seven separate VM-visible
physical cores, leaving core 0 to the sender. This avoids adding SMT siblings
as if they were additional physical cores. Host activity/frequency are not
controlled, so small differences require more repetitions.

### Longer confirmation: P=7, GOGC=500, 10K, N=8

```
trial=0 points=200000000 elapsed_s=28.953105 acked_Mpoints_s=6.908 client_cpu_s=26.520382 client_cpu_cores=0.916
trial=1 points=200000000 elapsed_s=28.599311 acked_Mpoints_s=6.993 client_cpu_s=26.084117 client_cpu_cores=0.912
collector accepted delta=400000000 refused delta=0 cpu delta=291.8 seconds
```

One 20-second CPU profile during this confirmation showed ~5 CPU equivalents
on the Collector. NumberDataPoint.Unmarshal remained prominent (66.69%
cumulative), as did mallocgc (52.45% cumulative). Their *percentage* increased
after reducing GC overhead; this does not mean absolute cost increased.

Final Collector metrics snapshot: RSS 368,574,464 bytes (~351.5 MiB), heap alloc
87,785,200 bytes, lifetime allocated bytes ~196.6 GB (~492 allocated bytes per
accepted point, including small startup overhead). These are final snapshots,
not peaks. At ~7M/s that allocation rate is roughly 3.4 GB/s of allocation churn.
The sender's ~6.5 GiB RSS in this run is mostly 200M prebuilt input points, NOT
in-flight queue memory. Higher GOGC trades memory headroom for less GC work and
needs a production memory budget; GOGC=1000 was only a shorter experiment.

## Have we reached a limit?

Not an OTLP theoretical limit. The current optimized *single-client-core*
implementation is approaching CPU saturation (91-94% within timed ACK intervals).
Measured client CPU cost is ~130-135ns/point. Keeping that cost unchanged would
give a conditional CPU budget around 7.4-7.7M/s, not a promised achievable ceiling:
cost changes with load and profiles have not yet identified the client's exact
hot functions. Linux perf_event_paranoid=4 blocked nonprivileged perf sampling;
no machine security settings were modified. Client CPU is instead measured using
CLOCK_PROCESS_CPUTIME_ID inside the timed interval, excluding input preparation.

The 10K schema is ~96 bytes/point: ~7M/s represents ~672MB/s (~5.38Gb/s) protobuf
payload. A physical 25Gb/s link's payload-only arithmetic is ~32.5M/s at this size,
before overhead. This is a localhost experiment, so neither number measures NIC
capacity. The older ~20-byte no-attribute case is not this workload.

## Prioritized next work

1. Retain N=8 / 10K as a starting point; avoid blindly raising N to 32 or batch
   to 100K. Add production concurrency configuration, retry/error-drain policy
   and per-series ordering semantics before enabling outside benchmark builds.
2. Obtain a client CPU profile on a permitted profiling host. Inspect channel
   consumption/input disposal, Arc/hash/cache lookups, buffer assembly/copies,
   Tonic EncodeBuf, HTTP2 frame handling, and socket syscalls. These are candidates,
   not measured per-function percentages. Compare one shared channel versus a
   small connection pool only after the profile supports transport contention.
3. Reduce Collector per-point allocation: evaluate a newer Collector with the
   same test first; then consider decoder slice preallocation/reuse or an upstream
   pdata optimization. Profile shows GC tuning alone cannot remove decoding cost.
4. Preserve schema unless explicitly redesigning it. Moving true resource labels,
   dictionary/columnar OTAP transport, or shared series metadata may reduce repeated
   strings, but changes compatibility/mapping and needs end-to-end validation.
5. For 50M/s, distribute senders and collectors by series and measure multi-core
   scaling, network and backend writes independently. Do not multiply 7M by core
   count and assume linear scaling or InfluxDB capacity.

The receiver evidence, scripts and improved CPU instrumentation are reproducible;
there is no claim that single-flight now reaches 4-5M/s or that persistence does.
