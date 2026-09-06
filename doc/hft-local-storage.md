# HFT Local Storage

`countersyncd --enable-local-storage` writes bounded, best-effort raw HFT counter
summaries to the dedicated `/mnt/hft` filesystem. It is disabled by default.
This ports the gauge reducer and Parquet writer from Pterosaur PR7
(`origin/feat/hft-local-parquet-storage`, commit `2af702c3`) to the flat-batch IPFIX
pipeline. There is no aggregator, heatmap path, rollover correction, or added
decoder/session lifecycle.

## Integration API

The public `message::local_storage::LocalStorageMessage` is a type alias for
`SAIStatsBatchMessage`, namely `Arc<SAIStatsBatch>`. Create a bounded
`std::sync::mpsc::sync_channel`, then connect it using
`IpfixActor::set_local_storage_recipient(sender, status)`.
`LocalStorageActor::new(receiver, config, status) -> Result<Self, String>` prepares
storage; `actor.run()` is blocking and belongs on an isolated thread.
`LocalStorageConfig` retains the public `root`, `range_interval`, `shard_interval`,
`max_bytes`, and `require_dedicated_filesystem` fields. The cloneable
`LocalStorageStatus` retains input-drop accounting, failed state, and shutdown
request APIs. These APIs are also exposed through the crate library for external
benchmarks; no special benchmark-only message conversion is necessary.

The IPFIX `send_batch` path taps each outgoing chunk using `try_send`, sharing its
Arc with other sinks. It does not use `add_recipient` for storage, wait for a
storage slot, or copy records into individual messages. The reducer iterates
borrowed record slices. Healthy sink delivery and its existing backpressure
semantics remain unchanged, including when storage is full or disconnected.
Storage also works when no other sink is enabled.

## Fixed Production Policy

| Setting | Value |
| --- | ---: |
| Gauge range interval | 10 ms |
| Immutable shard interval | 5 seconds |
| Application quota | 4,000,000,000 bytes |
| Rotation target | 120,000,000 estimated uncompressed bytes |
| Reducer/shard estimate limit | 128,000,000 bytes |
| Per-shard disk reservation | 512,000,000 bytes |
| Filesystem free-space reserve | 512,000,000 bytes |
| Input queue capacity | 32 flat batches |
| Writer queue capacity | 1 shard |

The input queue is smaller than PR7's 1,024 individual-sample queue because each
flat batch contains many records. Its memory footprint also depends on decoder
record sizes; 8,192 counters is a chunk target, not a hard cap on a single record.
Reducer accounting includes identity state and open ranges as well as completed
rows. These are conservative estimates, not a process RSS guarantee. Arrow
conversion, compression, queued batches, and the active writer add memory.

`/mnt/hft` must already exist as a dedicated mount with a different device from
its parent. The root filesystem, ordinary directories, same-device bind mounts,
and symlink roots are rejected by the production policy. The application never
creates a fallback under `/var/log` or on the system filesystem. Setup errors are
logged and disable only local output, not the critical pipeline. Filesystem setup
runs inside the isolated blocking task, after critical actors are spawned; it
does not delay their startup or signal supervision. While setup is pending the
bounded input tap drops on full, and setup failure marks its shared status failed.
Tests and benchmarks may explicitly use a temporary directory with
`require_dedicated_filesystem: false` and a smaller `max_bytes` above the shard
reservation. The production quota remains 4 GB, not the benchmark's limit.

## Directory Layout

```text
/mnt/hft/
  .writer.lock
  .staging/
  shards/
    <unix-ns>-<pid>-<sequence>/
      gauge_ranges.parquet  # absent for a loss-only shard
      loss.json             # present only when losses are recorded
      _READY
```

Only one writer holds the nonblocking exclusive `.writer.lock`. Setup rejects
symlink staging/shard directories and opens the lock with `O_NOFOLLOW`. After
locking, startup deletes incomplete staging entries without following symlinks;
it never deletes committed shards. The mount and its directory tree must be
owned and controlled by the service/operator, not writable by untrusted users.

Publication closes and fsyncs Parquet files, fsyncs the optional loss sidecar and
`_READY`, fsyncs staging, atomically renames the shard, and fsyncs both parent
directories. Consumers read only `shards` directories containing `_READY`.
Application quota accounting uses allocated file blocks, including sidecars and
pre-existing contents, not just logical Parquet lengths. Before writing, the
writer reserves a shard's quota and checks free space with `statvfs`; after
writing, it checks actual allocated bytes before publication. A dedicated mount
and free-space reserve remain necessary because this is not a filesystem quota
against other writers or filesystem metadata allocation.

## Gauge Semantics

Values and timestamps remain `UINT64`, including values above `2^53`, above
`i64::MAX`, and `u64::MAX`. No floating-point conversion occurs. Epoch-aligned
half-open `[window_start_unix_nano, window_end_unix_nano)` summaries store:

- Object name, SAI object type, and SAI statistic ID.
- First, last, minimum, and maximum values and their observation times.
- Value immediately preceding the window, when retained.
- Total monotonic increase, maximum absolute change, and its observation time.
- Sample/change counts and quality flags.

Session and source-template columns are **omitted** because flat source batches
do not carry that metadata. Identity is `(object_name, type_id, stat_id)`, not
record position, batch, or layout. A positional cache checks all identities before
reuse. Switching, interleaving, shortening, or reordering layouts does not clear
the prior series state or merge different objects. Equal timestamps count every
observation, including unchanged values, exactly once: IPFIX timestamp fallback
can legitimately repeat timestamps. Only positive intervals seed cadence.
Backward timestamps close the previous range and start a new range in the
sample's actual window, with both ranges flagged for time regression and no
previous-value bridge. Cadence is reset for that series; no sample is discarded.
If independent sources use exactly the same object/type/stat tuple, this input
API cannot distinguish them. Session restarts are not inferred or tracked.

| Flag | Meaning |
| ---: | --- |
| 1 | Counter decreased in this range |
| 2 | Inferred source sample gap or skipped range window |
| 4 | Local input message or complete shard was dropped |
| 8 | Source time regressed; range boundary is a clock discontinuity |

Expected cadence is inferred independently per series from the smallest positive
observed interval; gaps exceed 1.5 times that interval. A decrease may be a reset,
rollover, or source transition; storage does not decide which. Counts and total
increase saturate rather than overflow. Source-time shard rotation retains open
range state and never resets the independent wall-clock flush timer.
Wall-clock/size flushing closes open ranges and clears state to bound
idle/stalled-source retention; a subsequent row can cover the same window and
has no preceding value. Readers must not assume exactly one row per window.
Large records are processed incrementally as borrowed slices. Conservative growth
preflight flushes valid accumulated data before accepting a slice that could
cross the rotation target; it does not disable storage and lose the prior shard
just because the next record is large. An individually oversized identity is
rejected only after accumulated valid data is queued for publication.

This is lossy range summarization, not a raw sample archive. Parquet integer
encoding and ZSTD level 1 are lossless relative to the summarized rows.

## Failure And Shutdown

A full input queue drops the outgoing chunk and counts one dropped input message
(a batch, not a counter or sample). A full writer queue drops a complete shard;
its loss counts carry into the next shard. To avoid log flooding, writer-queue
drops emit one warning per process and are then reported through loss sidecars
rather than per-drop error logs.
After any detected input or shard drop, all buffered and subsequent ranges remain
storage-drop flagged for the rest of the process, including across state flushes.
Attribution is intentionally conservative: the atomic drop counter is consumed
out of order relative to queued batches. Sidecar counts report observed loss, not
the precise source-time interval or batch position where it occurred. Already
published shards cannot be retroactively flagged. Losses are
best-effort metadata: a final failure or forced exit can prevent their publication.
`ENOSPC`, `EIO`, `EROFS`, quota/estimate exhaustion, or writer failure stops local
output for the process. The quota is append-until-stop, not circular retention.
No committed shard is removed automatically and no retry/recovery loop is added.

Closing all input senders drains accepted batches and flushes open ranges and
final drop counts. SIGINT/SIGTERM stops critical actors, closing IPFIX's sole
storage sender. Main waits up to 10 seconds for storage, requests a stop on
timeout, allows one further second, then exits without waiting indefinitely on
filesystem I/O. Forced shutdown can lose queued or staged data; staging cleanup
runs on the next startup. The stop request is checked between records even if
the input queue remains busy. This drain covers already accepted storage batches,
not all pending Netlink inputs or critical sink exports.

## Reading

After selecting committed directories, DuckDB can query their gauge files:

```sql
SELECT window_start_unix_nano, object_name, first_value, last_value,
       min_value, max_value, max_change, flags
FROM read_parquet('/mnt/hft/shards/*/gauge_ranges.parquet')
WHERE object_name = 'Ethernet0'
ORDER BY window_start_unix_nano, first_time_unix_nano;
```

Inspect `loss.json` and flags before treating a time range as complete. Local
storage safety does not establish an end-to-end throughput claim.

## Port Verification

Production changes are in the actor/message local-storage modules, IPFIX tap,
main setup/shutdown, module exports, and workspace/crate Cargo manifests. Source
tests cover interleaved flat layouts and identity separation, unchanged healthy
delivery under full/disconnected storage, shared batch allocation, exact unsigned
Parquet values, close/shutdown flush, drop propagation, quota/estimate bounds,
exclusive writer locking, symlink rejection, and staging cleanup.

Arrow/Parquet 55.2.0 dependencies are pinned in Cargo.lock. Parallel and serial
container test runs pass 424 test executions (198 library, 209 binary, and 17
integration/helper executions; shared tests run in multiple targets). All-target
checking, benchmark compilation, and release build also pass. See
[performance measurements](hft-local-storage-performance.md) for the UDP and
standalone benchmark, reproduction commands, and measurement limits.
Real dedicated-mount integration, SIGINT/SIGTERM deadline behavior under blocked
I/O, and power-loss/fsync fault injection still need system-level verification.
