# HFT Local Storage: Matrix Performance

> **HISTORICAL v3/v4 evidence only, not current v5 measurements.** These
> measurements used the earlier 128 MiB raw / 5-second rotation policy and v4's
> three-list matrix with `record_seq`. Current `sonic-hft-arrow-v5` uses only
> `timestamps_ns` and `values`, eight IPC buffer slots instead of twelve, and
> generic 32-batch Tokio-channel backpressure rather than a nonblocking tap.
> It retains the 16 MiB block target, 4,096-record cap, and synchronization, with
> raw record bytes now `8 * (C + 1)`. Rotation defaults remain 100,000,000 encoded
> bytes / 30 minutes; raw-size rotation is no longer supported. All measured
> numbers and the report body below remain unchanged; no benchmarks were rerun.
> Old columns/buffers, sequence/drop accounting, hashes, reproduction commands,
> RSS, throughput, compression ratios, and test counts do not describe or validate
> v5. References below to "current" code/harness mean the historical measured
> version, not today's source. Rebuilding today's tree is not a v4 reproduction.
> v5 rejects v4 streams; use the old reader, with no automatic migration and
> archived files untouched. See the [current storage contract](hft-local-storage.md).

Measured `sonic-hft-arrow-v4` versus the copied v3 numeric baseline, using the
same `interval-bytes-v1` generator. **50,000,000 lossless raw metrics/s was not
achieved**, including at 500 counters. Short standalone median speedups are
1.110-1.164x at 500 counters and 3.574-4.966x at 8,000 counters. These are
blocking, unpaced standalone measurements, not production UDP capacity claims.

## Evidence

All evidence below is external to the repository, under
`/home/zegan/opencode-swss-verify/`; this report does not rerun benchmarks.
The older [v3 report](hft-local-storage-ipc-performance.md) uses a different
generator and must not supply comparison numbers for these tables.

| Log | Trials | Records / metrics per pattern per repeat | Source span |
| --- | ---: | ---: | ---: |
| `numeric-v3-500.jsonl` | 12 | 300,000 / 150,000,000 | 3 s |
| `numeric-v4-500.jsonl` | 12 | 300,000 / 150,000,000 | 3 s |
| `numeric-v3-8000.jsonl` | 12 | 20,000 / 160,000,000 | 0.2 s |
| `numeric-v4-8000.jsonl` | 12 | 20,000 / 160,000,000 | 0.2 s |
| `numeric-v3-minute-500.jsonl` | 4 | 6,000,000 / 3,000,000,000 | 60 s |
| `numeric-v4-minute-500.jsonl` | 4 | 6,000,000 / 3,000,000,000 | 60 s |
| `ipc-v4-reader-pyarrow20-final.jsonl` | 2 fixtures + provenance/summary | 12,000 / 96,000 per fixture | 0.12 s |

Environment is the existing CI-like Bookworm setup documented in the v3 report:
Xeon Platinum 8370C @ 2.80 GHz, 16 logical CPUs / 8 cores with SMT; Microsoft
VM, Linux `6.8.0-138-generic`; `/dev/sdc1` ext4 (`0xef53`, `rw,relatime`).
Container `ipfix-local-storage-verify` uses
`sonicdev-microsoft.azurecr.io:443/sonic-slave-bookworm:master-amd64` and mounts
`/home/zegan/sonic-swss-ipfix-local-storage` at `/workspace/repos/sonic-swss`.
Rust/cargo 1.86.0, optimized benchmark, Rust Arrow 55.2.0 with IPC ZSTD;
two Tokio workers, one storage writer, 32 input batches. Dependencies are reused
swss-common artifact **1212995** and common-lib **1213281**, not newly selected
CI artifacts. Installed inputs include libswsscommon/dev 1.0.0, libyang3
3.12.2-1 and SONiC libnl 3.7.0-0.2+b1sonic1. CI reference:
`.azure-pipelines/build-template.yml`. No clean rebuild, package/DVS validation,
isolated-core, cold-cache, bare-metal or physical-device bandwidth claim follows.

Source provenance is content SHA-256, **not Git HEAD**: the sources are
uncommitted. Current hashes were checked against the reader provenance log.
Paths below are relative to the mounted repository; the copied baseline binary
is the reproducible v3 artifact, not a v3 build from the current source tree.

| Artifact | SHA-256 |
| --- | --- |
| `crates/countersyncd/benches/local_storage_perf.rs` | `db0ef2503ebf9a9d17d425af73795eabfc86079fbd245e6b77ffce2100f37af2` |
| `crates/countersyncd/src/actor/local_storage.rs` | `4af5cec4db76eb351fa91d402d88de61d7d1da83674975cd431fc5bb85b1fea2` |
| `crates/countersyncd/src/actor/local_storage_codec.rs` | `9673c89fbf37e1b28b311ee75894a4020429dff18c9ccaf6fad2e8e6edf719a6` |
| `target/release/deps/local_storage_perf-f491f1a1df8fa23c` (v4) | `9f864cc7e188cc03382b567457c00f4e15dd569c846b77a92386e65ad4288045` |
| `target/release/local-storage-perf-v3-numeric` (copied baseline) | `8be757d6f945687a6d622319f046cafad5f26db988afa803d64614731e0dd544` |

The numeric JSONL files do not embed baseline source hashes; the binary hash
must not be presented as a source hash. Reader script and fixture hashes are
also recorded in `ipc-v4-reader-sha256.txt` and the interoperability JSONL.

## Measurement Contract

One metric is one raw counter observation; one reported row/record is one source
timestamp with C observations, **not** one physical IPC matrix block.
All 56 numeric trials use one continuous streaming segment, generation included,
no prebuilt/replayed payload pool, `--rate 0`, and standalone blocking enqueue.
Timing covers generation/enqueue through drain, write, fsync, publication and
worker join. Setup, full raw readback and cleanup are outside elapsed write time.
This is not a pure compression microbenchmark or a nonblocking production tap.

The current harness performs **one mandatory generic Arrow StreamReader pass**,
checking every raw value, timestamp, sequence/stat order, uniqueness and full
series names. `reader_api_check=false`: the older duplicate `read_shard` pass is
not included. Readback is untimed, but its allocations can affect later trials.
PyArrow is a separate small-fixture interoperability check, not a second scan
of all performance outputs. `audit_values=false` does not disable raw readback.

```text
metrics/s = verified persisted metrics / measured write elapsed seconds
canonical bytes = persisted metrics * 8 + persisted records * 8
file bytes = Arrow IPC logical bytes + root loss.json logical bytes
payload/file ratio = canonical bytes / file bytes
speedup = median(v4 metrics/s) / median(v3 metrics/s)
```

Canonical bytes count each timestamp once and exclude generated `record_seq`.
File bytes include schema/names, framing, sequence buffers, compressed or
uncompressed payloads, EOS and loss diagnostics. No dropped input enters the
numerator; this is not a codec-only ratio. All numeric trials have zero loss
file bytes, zero dropped input/shards, no storage failure, no readback errors,
zero verification errors and `all_input_persisted=true`. There are no partial
failures hidden from the tables. Every trial reports the 50M target unmet.

## Integer Workloads

Default values are **bytes per interval**, not bps or cumulative counters:
`floor(200000000000 * 10000 / (8 * 1000000000)) = 250000` bytes at 10 us.
All arithmetic is integer; changing the step scales values with truncation.
The 100 ms envelope uses `(source_time + index * 104729) % 100000000`.
Thus **even the `burst` label is index-phased/staggered**, not synchronized.

| Pattern | Actual construction at 10 us |
| --- | --- |
| idle | Long mostly-zero low signal, integer values 0-6 bytes (up to 4.8 Mbps equivalent) |
| burst | Per-series 10 ms active window: 2 ms ramp up, 6 ms near-cap triangular peaks/valleys, 2 ms ramp down; then transition and low interval |
| sustained | Near 250,000 with triangular decreases/increases; index/epoch-dependent periods and 0-3 byte jitter |
| mixed | Same staggered burst construction, active window 5-20 ms by index, including 2 ms ramps |

After burst/mixed active windows, five 400 us transition plateaus are exactly
`10000, 101000, 101000, 10200, 10201` for `index % 8 == 0`; other indices add
small integer perturbations. Triangle periods drift by index and epoch, so the
100 ms envelope is not an exactly replayed payload. Generator self-checks cover
transitions, caps, low/nonzero values, falls and later epochs. Synthetic names
use `type_id == stat_id == index + 1`, `Ethernet{index % 64}` and full fallback
SAI names. This is not a captured production database or guaranteed entropy.
Full-width `large100/33/66` and constant inputs are separate stress cases, not
the four default throughput patterns. Partial/masked-input and input-flood
tests are correctness stress, not evidence for long-run throughput.

## Short Comparisons

Three repeats per pattern/version. Medians are selected from full-precision log
values, not averaged or chosen as the best run; display rounds rates to six
decimal places and ratios to six. RSS and size medians are computed separately,
not combined into a fictitious median trial.

| Counters | Pattern | v3 median metrics/s | v4 median metrics/s | v4/v3 speedup |
| ---: | --- | ---: | ---: | ---: |
| 500 | idle | 26519311.980000 | 29423991.736029 | 1.109531 |
| 500 | burst | 23233823.028326 | 27042851.870505 | 1.163943 |
| 500 | sustained | 16871425.490518 | 19359077.740702 | 1.147448 |
| 500 | mixed | 22978065.123561 | 25968645.621399 | 1.130149 |
| 8000 | idle | 6232095.040745 | 30949082.859221 | 4.966080 |
| 8000 | burst | 5978785.162721 | 29556778.820113 | 4.943609 |
| 8000 | sustained | 5189590.738682 | 18546281.307779 | 3.573746 |
| 8000 | mixed | 6079769.788836 | 28232969.165852 | 4.643756 |

At 500 counters each repeat spans 3 source seconds / 30 envelopes; at 8,000,
only **0.2 source seconds / two envelopes**, despite 5-31 seconds of elapsed
writing. The latter is a short wide-layout diagnostic, not a minute/soak run.
Canonical payloads are respectively 1,202,400,000 and 1,280,160,000 bytes.

| Counters | Pattern | v3 median file bytes | v4 median file bytes | v3 payload/file | v4 payload/file | v3/v4 file size |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 500 | idle | 13,419,720 | 11,762,248 | 89.599485 | 102.225357 | 1.140915 |
| 500 | burst | 40,325,960 | 32,185,288 | 29.817021 | 37.358684 | 1.252931 |
| 500 | sustained | 104,360,520 | 103,266,056 | 11.521598 | 11.643710 | 1.010598 |
| 500 | mixed | 42,886,920 | 35,287,176 | 28.036520 | 34.074702 | 1.215368 |
| 8000 | idle | 155,824,272 | 22,028,944 | 8.215408 | 58.112636 | 7.073615 |
| 8000 | burst | 182,229,456 | 39,958,352 | 7.024989 | 32.037357 | 4.560485 |
| 8000 | sustained | 306,311,952 | 156,023,568 | 4.179269 | 8.204914 | 1.963242 |
| 8000 | mixed | 185,969,808 | 44,946,320 | 6.883698 | 28.481976 | 4.137598 |

Only v3/500 sustained file size varies: repeat 1 is 104,381,896 bytes;
repeats 2/3 are 104,360,520. Rotation/flush boundaries are wall-time-sensitive.
Maximum sampled short-run allocation is 306,372,608 bytes (v3/8000 sustained).

## Full Minute: 500

These are **actual completed writes**, one repeat per pattern/version, each
6,000,000 records / 3,000,000,000 metrics: 24,000,000,000 value bytes plus
48,000,000 timestamp bytes = 24,048,000,000 canonical bytes. The source covers
60 seconds / 600 envelopes; measured writing takes 101.5-179.3 seconds, not 60.

| Pattern | v3 write seconds | v4 write seconds | v3 metrics/s | v4 metrics/s | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| idle | 109.549737949 | 101.543793908 | 27384821.325603 | 29543903.024916 | 1.078842 |
| burst | 128.655085609 | 112.629404468 | 23318161.002336 | 26636028.257189 | 1.142287 |
| sustained | 179.264103728 | 157.599031346 | 16735084.925602 | 19035649.993392 | 1.137470 |
| mixed | 131.008141998 | 113.885815553 | 22899340.103959 | 26342174.268435 | 1.150346 |

| Version | Pattern | Logical file bytes | Finalized allocated bytes | Sampled allocated peak bytes | Payload/file |
| --- | --- | ---: | ---: | ---: | ---: |
| v3 | idle | 269,814,496 | 270,229,504 | 270,233,600 | 89.127902 |
| v3 | burst | 808,332,640 | 808,722,432 | 808,726,528 | 29.750129 |
| v3 | sustained | 2,083,444,704 | 2,083,938,304 | 2,083,942,400 | 11.542423 |
| v3 | mixed | 859,223,008 | 859,639,808 | 859,643,904 | 27.988077 |
| **v3 TOTAL** | **sum of four outputs** | **4,020,814,848** | **4,022,530,048** | **2,083,942,400 (max)** | - |
| v4 | idle | 235,213,536 | 235,569,152 | 235,573,248 | 102.239014 |
| v4 | burst | 647,253,728 | 647,675,904 | 647,680,000 | 37.153900 |
| v4 | sustained | 2,063,183,136 | 2,063,659,008 | 2,063,663,104 | 11.655776 |
| v4 | mixed | 705,421,344 | 705,851,392 | 705,855,488 | 34.090264 |
| **v4 TOTAL** | **sum of four outputs** | **3,651,071,744** | **3,652,755,456** | **2,063,663,104 (max)** | - |

Totals are cumulative output from separate trials, **not simultaneously retained
disk occupancy**: trials are cleaned between patterns. Each version totals
24,000,000 records / 12,000,000,000 metrics. Allocated bytes are filesystem
blocks (`st_blocks * 512`), including directory overhead, not write amplification.
The 10 ms watchdog samples allocation with a 2,950,000,000-byte cap; production
test quota is 2,400,000,000 bytes with append reserves (the daemon default remains
4,000,000,000 bytes). Neither sustained run failed:
v3's approximately 2.08 GB output fit with reserves, as did v4. Sampled peaks
are not exact high-water marks. No quota-error or partial trial appears in either
minute log. A **full minute at 8,000 counters would be 48,000,000,000 metrics**
per pattern and was not run because of disk/time budget. Scaling 0.2 s by 300
would be an extrapolation, not measured output, and is excluded from these totals.

## Memory And Layout

RSS is whole-process `/proc/self/statm`, sampled every 10 ms during writing
through worker join. Entries are baseline / peak / delta in **bytes**, with
independent medians for short trials and actual values for minute trials.

| Scope | Pattern | v3 baseline / peak / delta | v4 baseline / peak / delta |
| --- | --- | ---: | ---: |
| short 500 | idle | 21,647,360 / 42,606,592 / 20,959,232 | 39,555,072 / 75,395,072 / 36,933,632 |
| short 500 | burst | 23,306,240 / 44,126,208 / 20,819,968 | 39,555,072 / 77,545,472 / 37,990,400 |
| short 500 | sustained | 23,818,240 / 46,542,848 / 22,904,832 | 39,555,072 / 78,213,120 / 38,658,048 |
| short 500 | mixed | 23,818,240 / 44,769,280 / 20,951,040 | 39,559,168 / 77,721,600 / 38,162,432 |
| short 8000 | idle | 32,788,480 / 68,386,816 / 35,598,336 | 57,778,176 / 96,993,280 / 41,578,496 |
| short 8000 | burst | 32,378,880 / 70,615,040 / 37,392,384 | 58,916,864 / 97,497,088 / 38,580,224 |
| short 8000 | sustained | 33,419,264 / 75,907,072 / 42,053,632 | 57,782,272 / 100,159,488 / 42,377,216 |
| short 8000 | mixed | 48,394,240 / 84,000,768 / 35,606,528 | 58,920,960 / 97,591,296 / 38,670,336 |
| minute 500 | idle | 11,161,600 / 41,541,632 / 30,380,032 | 11,309,056 / 65,830,912 / 54,521,856 |
| minute 500 | burst | 19,968,000 / 53,334,016 / 33,366,016 | 74,002,432 / 90,841,088 / 16,838,656 |
| minute 500 | sustained | 31,825,920 / 57,790,464 / 25,964,544 | 52,760,576 / 91,521,024 / 38,760,448 |
| minute 500 | mixed | 31,666,176 / 55,578,624 / 23,912,448 | 52,760,576 / 91,017,216 / 38,256,640 |

Process-start RSS (v3/v4 bytes) was 10,756,096/10,805,248 for short 500,
10,969,088/10,833,920 for short 8000, and 10,702,848/10,723,328 for minute 500.
The writing-phase baseline is not process start: repeats/patterns share a
process, retaining allocator history from generation, setup and earlier readback.
Overall/readback peaks are separate log fields. These are neither isolated
writer RSS nor exact high-water marks; median peak minus median baseline need
not equal median per-trial delta.

v4 stores three non-null `List<UInt64>` fields: timestamps, sequence and
series-major values (`values[c*T+t]`), one physical IPC row per block, with full
ordered series names once in schema metadata. Compared with v3's C+2 primitive
columns, IPC has 12 buffer slots rather than 1,004/16,004 at widths 500/8,000
(including empty validity slots, not 12 nonempty compression calls). Both use
the same 16 MiB batch target and 4,096 logical-record cap; width 8,000 allows
262 records per full block. Flattening into v4's reusable matrix adds up to
another 16 MiB buffer while column allocations remain. Actual RSS above is the
measurement; buffer arithmetic alone is not a memory-performance result.
Fewer independent buffers and less repeated schema are structural explanations,
but **no profiler was run**: exact CPU percentages or attribution of speedup to
compression, copying, allocation or fsync are not established.

## Correctness And Status

PyArrow/Arrow C++ 20.0.0 independently passed 24,000 records / 192,000 metrics
across mixed and large100 fixtures, each with blocks `[4096, 4096, 3808]`, zero
mismatches/nulls/duplicate-or-missing records. Mixed spans 0-250,000; large100
includes 95,949 values above 2^53 and 48,142 above 2^63, with 95,699 values that
would lose precision on a float round trip. No literal `u64::MAX` was injected
in this external fixture. Total retained logical/allocated bytes: 885,584/925,696.

For each fixture, body/metadata/length-prefix truncations preserved two complete
blocks / 8,192 records before the generic reader raised a tail error. Reading a
complete prefix with EOS passed. **The writer leaves abandoned files untouched;
readers handle tails, with no startup repair.** External interoperability did
not test writer recovery. See the [current storage contract](hft-local-storage.md).

The final full-suite run in `matrix-v4-cargo-test.log` passed and records
201 + 212 + 2 + 11 + 4 = 430 passing executions (library/binary overlap means
these are not 430 unique tests), plus two empty targets. Locked all-target checking
with the benchmark feature and the locked release daemon build also passed with
`RUSTFLAGS=-Dwarnings` in the same container. No full Debian package or DVS build
was performed.

## Reproduction

Run in the existing container, working directory `/workspace/repos/sonic-swss`,
with the pinned dependencies above. Preserve the copied v3 binary: rebuilding
the current tree produces v4, not the baseline. The parent-produced JSONL names
are listed above. Do not overwrite that evidence when rerunning; redirect stdout
to fresh logs. These commands intentionally omit `--output-root` so each trial
is cleaned rather than retaining four minute outputs beyond the disk budget.

```bash
RUSTFLAGS=-Dwarnings cargo bench --locked -p countersyncd --bench local_storage_perf --no-run
V3=target/release/local-storage-perf-v3-numeric
V4=target/release/deps/local_storage_perf-f491f1a1df8fa23c
# Execute each command once with BIN=$V3, then once with BIN=$V4.
BIN=$V3
"$BIN" --root /workspace/repos/sonic-swss --mode standalone --streaming \
  --records 300000 --counters 500 --repeats 3 --rate 0 --step-ns 10000 \
  --patterns idle,burst,sustained,mixed --timeout-secs 300 --require-lossless
"$BIN" --root /workspace/repos/sonic-swss --mode standalone --streaming \
  --records 20000 --counters 8000 --repeats 3 --rate 0 --step-ns 10000 \
  --patterns idle,burst,sustained,mixed --timeout-secs 300 --require-lossless
"$BIN" --root /workspace/repos/sonic-swss --mode standalone --streaming \
  --records 6000000 --counters 500 --repeats 1 --rate 0 --step-ns 10000 \
  --patterns idle,burst,sustained,mixed --timeout-secs 900 --require-lossless
```

Timeouts bound each entire trial including untimed verification; they are not
source durations. For exact unrounded medians from one full short log, on the
host (`jq -s` reads all 12 trials):

```bash
jq -s 'group_by(.pattern) | map({pattern: .[0].pattern,
  median_metrics_per_second: (map(.metrics_per_second) | sort | .[1]),
  seconds: map(.seconds), file_bytes: map(.storage_logical_file_bytes)})' \
  /home/zegan/opencode-swss-verify/numeric-v4-500.jsonl
```
