# HFT Local Storage: v4 Arrow IPC Matrix

`countersyncd --enable-local-storage` enables best-effort capture of **exact raw
decoded observations** on `/mnt/hft`. It is disabled by default. The current
format is `sonic-hft-arrow-v4`: self-contained, ZSTD-compressed standard Arrow IPC
streams with one series-major matrix block per record batch. This is not the
old range-summary implementation, a custom delta codec, Parquet, or Feather.

This document describes the source contract, not measured throughput. See the
[v4 matrix performance report](hft-local-storage-matrix-performance.md) for
separately maintained measurement evidence. Its existing 5-second/128 MiB raw
rotation measurements are historical, unchanged results, not measurements of the
current 100 MB compressed-file/30-minute policy. The [v3 IPC report](hft-local-storage-ipc-performance.md)
and [v1 summary report](hft-local-storage-performance.md) are historical; their
rates, compression ratios, and test results do not establish v4 performance.

## CLI

Local capture remains opt-in. Defaults use an existing dedicated filesystem:

```sh
countersyncd --enable-local-storage
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--enable-local-storage` | `false` | Enable best-effort local capture |
| `--local-storage-root PATH` | `/mnt/hft` | Existing capture directory |
| `--local-storage-max-bytes BYTES` | `4000000000` | Application quota in decimal bytes |
| `--local-storage-file-bytes BYTES` | `100000000` | Compressed IPC logical file-size target (decimal 100 MB), checked after a complete batch flush |
| `--local-storage-file-seconds SECONDS` | `1800` | Maximum wall-clock file age (30 minutes), independent of source timestamps |
| `--local-storage-allow-shared-filesystem` | `false` | Disable only the dedicated-filesystem requirement; requires `--enable-local-storage` |

For a real-DUT capture in an existing operator-controlled directory on a shared
filesystem, with a decimal 2.4 GB quota:

```sh
countersyncd --enable-local-storage \
  --local-storage-root /mnt/dut-capture \
  --local-storage-max-bytes 2400000000 \
  --local-storage-allow-shared-filesystem
```

Quota parsing accepts `u64` values from `67108865` through `18446744073709551615`.
Zero and values at or below the 64 MiB batch reserve are rejected before actor
startup. File bytes and seconds must be positive `u64` values; zero, negative,
invalid, and overflowing values are rejected by CLI parsing. Root, quota, and
rotation options alone do not enable capture. Root existence,
symlink, locking, entry, and filesystem-space checks remain in the storage actor;
the shared-filesystem flag does not bypass them or change permissions. Setup
failure still disables only local output. Shared capture is an explicit exception
to the production dedicated-filesystem policy below: unrelated writers can
consume space, and the application quota does not constrain them. Startup logs
report the selected root, quota, file-size target, maximum age,
dedicated-filesystem requirement, and v4 format.

## Capture Contract

Storage retains every input record timestamp and every counter value in its
original field position within complete persisted blocks. Values and timestamps
are non-null Arrow `UInt64`, including values above `2^53`, above `i64::MAX`, and
`u64::MAX`. No floating-point conversion, timestamp precision reduction,
summarization, deduplication, application-level delta encoding, or inferred
rollover is applied. Repeated values, decreases, peaks, valleys, duplicate
identities, equal timestamps, and backward timestamps remain in arrival order.

For example, gauge observations `10000, 101000, 101000, 10200, 10201` are stored
as exactly those five values, not cumulative counts or deltas. Units belong to
the decoded statistic; storage does not convert a bandwidth gauge into bytes.
For scale only, 200 Gb/s over 10 microseconds is
`200_000_000_000 * 0.000010 / 8 = 250_000` bytes per interval, not 250 bytes.

This is exactness for decoded records that reach complete persisted blocks,
**not lossless end-to-end ingestion**. The nonblocking storage tap can drop
messages. Channel acceptance is not a durability acknowledgement: queued input
and an unfinished in-memory block are outside the persistence boundary. UDP
loss, undecoded input, and RAM-only data cannot be recovered from the archive.

Storage copies `SAIStatsRef.observation_time` unchanged into `timestamps_ns`
(plural). The decoder may supply a fallback when a record has no decoded
timestamp: an available message timestamp, the previously observed source
timestamp, or current system time if neither exists. Storage cannot restore an
absent original timestamp or wire headers the decoder did not retain. Flat
records do not carry source session/template metadata; storage does not invent
it or distinguish independent sources with identical field identities.

## Integration

`message::local_storage::LocalStorageMessage` aliases `SAIStatsBatchMessage`, an
`Arc<SAIStatsBatch>`. A bounded `std::sync::mpsc::sync_channel` connects through
`IpfixActor::set_local_storage_recipient(sender, status)`.
`LocalStorageActor::new(receiver, config, status) -> Result<Self, String>`
validates and locks the root, checks directory entries, and accounts for existing
files. **It does not repair or publish abandoned streams.** `actor.run()` is one
blocking storage loop, run on an isolated thread/blocking task, without a
secondary writer queue or whole-shard queue-drop policy.

The IPFIX tap uses `try_send` and shares the flat batch's `Arc` with other sinks.
It never waits for a storage slot. Existing healthy-sink delivery/backpressure
is unchanged when storage is full, failed, or disconnected. Storage can also
run without another sink.

`LocalStorageConfig` contains `root`, `shard_interval`, `file_target_bytes`,
`max_bytes`, and `require_dedicated_filesystem`. `shard_interval` is the maximum
wall-clock file age; both it and `file_target_bytes` must be positive.
`LocalStorageStatus` supplies input-drop
accounting, failed state, and shutdown requests. Main starts filesystem setup
in `spawn_blocking` after spawning critical actors, so setup does not delay
their startup or signal supervision. The tap can fill and drop during setup;
setup failure disables only local output.

## Schema And Ordering

Each file embeds one schema for one stable ordered layout. Layout matching
compares every positional `(object_name, type_id, stat_id)` tuple. A change in
count, name, type, stat ID, or order closes the current stream. Returning to an
earlier layout starts a new file; layouts are not coalesced. Time/size rotation
can also produce multiple files with the same schema.

The writer emits these schema metadata keys:

| Key | Value |
| --- | --- |
| `format_version` | `sonic-hft-arrow-v4` |
| `timestamp_unit` | `ns` |
| `matrix_order` | `series-major` |
| `series` | Ordered JSON array of name objects, one per counter position |

Each `series` entry contains exactly `object_name`, `type_name`, and `stat_name`,
all strings. Names occur **only in this schema metadata**, once per file, not in
field names or repeated in each block. Array position is the stat index; there
is no `stat_index` property or separate numeric ID column. Duplicate name tuples
remain separate positions. Do not collapse them into an identity-keyed dictionary.

`series_names(type_id, stat_id)` uses full checked-in SAI C names for known
objects and mapped port, queue, buffer-pool, and ingress-priority-group stats,
such as `SAI_OBJECT_TYPE_PORT` and `SAI_PORT_STAT_IF_IN_OCTETS`. Unknown object
IDs use `SAI_OBJECT_TYPE_UNKNOWN_{type_id}`; unknown/unmapped stats use
`{type_name}_STAT_UNKNOWN_{stat_id}`. Readback needs no external SAI dictionary
or schema sidecar. JSON escaping preserves special characters in names.

There are exactly three fields, in this order. Each is a non-null
`List<item: UInt64 not null>` with no field/child metadata:

| Field | Contents Of Its Single List |
| --- | --- |
| `timestamps_ns` | `T` unmodified decoded observation timestamps |
| `record_seq` | `T` storage-assigned record sequences |
| `values` | `C * T` raw values, concatenated series-major |

Every record batch has **one Arrow row representing one block**, not one row
per input record. Let `C = len(series)` and `T = len(timestamps_ns[0])`.
After extracting the lists from row zero:

```text
timestamp for input record t = timestamps_ns[t]
sequence for input record t  = record_seq[t]
value for series c, record t = values[c * T + t]
```

`1 <= T <= 4096`, `len(record_seq) == T`, and `len(values) == C * T`.
The lists use offsets `[0, list_length]` and have no null elements. The standard
IPC batch has six array nodes and twelve buffer descriptors, independent of
counter count. This changes the layout, not the stored integers.

**Zero counters is valid:** `series` and `values[0]` are empty lists, while
`timestamps_ns[0]` and `record_seq[0]` still hold the real input records. A block
with an empty timestamp list (`T == 0`) is not valid.

`record_seq` starts at zero per storage actor and continues across file
rotations in processed-record order, not timestamp order. It resets on restart,
is not a global/source sequence, and does not prove loss-free ingestion.
Dropped input messages never receive sequence numbers. Sequence exhaustion
stops storage rather than wrapping. Within a file, sequences are contiguous.

## Compression

The writer uses Arrow 55.2.0 `StreamWriter` with
`IpcWriteOptions::default().try_with_compression(Some(CompressionType::ZSTD))`.
Standard Arrow IPC compresses individual buffers, including the concatenated
values buffer; this is not a whole-file `.zst` wrapper. Arrow's encoder uses
ZSTD level `0`, selecting its default (level 3 in the locked dependency), not
explicit level 1. Standard IPC may store a buffer uncompressed when compression
would enlarge it. There is no application delta transform or entropy-dependent
lossy mode. Compression ratio and throughput are workload-dependent.

An Arrow **stream** contains its schema, record-batch messages, and an
end-of-stream (EOS) marker, without the random-access footer of the IPC **file**
format. Despite the `.arrow` suffix, use `pyarrow.ipc.open_stream`, not
`ipc.open_file` or `feather.read_table`.

## Production Limits

| Setting | Current Value |
| --- | --- |
| Root | `/mnt/hft`, existing dedicated filesystem |
| Application quota | 4,000,000,000 bytes (decimal 4 GB) |
| Input queue | 32 flat batches |
| Maximum counters per record/layout | 65,536 |
| `series` metadata budget | 16 MiB of actual serialized JSON bytes |
| Raw block target | 16 MiB |
| Maximum input records per block | 4,096 (one Arrow row) |
| Block flush due | 100 ms after its first buffered record |
| File rotation | 100,000,000 compressed IPC logical bytes, 1,800 seconds maximum wall-clock age, or layout change |
| Raw record/reader block safety limit | 128 MiB |
| Minimum schema/block disk reservation | 64 MiB |
| Filesystem emergency reserve | 512 MiB |

For `C` counters, raw record bytes are `8 * (C + 2)`, including timestamp and
sequence. The block's record capacity is
`clamp(floor(16 MiB / raw_record_bytes), 1, 4096)`. With the explicit counter
limit, accepted writer blocks fit the 16 MiB target: capacities include 4,096
records at 500 counters, 262 at 8,000, and 31 at 65,536. File-size rotation counts
the exact logical bytes accepted by successful writes, including schema and IPC
framing, rather than raw input bytes or allocated filesystem blocks. It adds no
per-record filesystem stats. The target is checked only after a complete batch
has been written, flushed, and fsynced. It is approximate: a file can overshoot
by one encoded batch plus the 8-byte EOS marker. Even when the schema alone
exceeds a tiny configured target, at least one batch is written before size
publication. Highly compressible input can exceed 128 MiB raw without rotating.
No whole-file buffering is introduced. Maximum-age rotation uses elapsed wall
time since opening the stream, ignores source timestamps, and flushes any final
buffered records before publication; compression, scheduling, and I/O can delay
the age check. Layout changes and graceful shutdown can publish smaller files.

Metadata admission first checks aggregate source object-name bytes, then counts
the **actual serialized `series` JSON**, including all names, punctuation, and
escaping, before allocating the JSON output buffer. There is no coarse fixed
per-name allowance. The independent 65,536-counter limit still applies even to
short names. Metadata construction/serialization and the retained layout consume
additional memory; the 16 MiB JSON limit is not a total metadata-memory bound.

The writer keeps up to 16 MiB of reusable original column buffers **plus up to
16 MiB of reusable series-major concatenation storage**. Arrow conversion and
compression working buffers, metadata, vector/allocator overhead, and queued
input add memory. The 32-message queue is not a fixed byte/RSS bound: flat batches
vary in size, and the decoder's 8,192-counter chunk target does not split a
single wide record. These are operational limits, not a total process-memory
guarantee.

Deadlines are checked between records and while waiting for input. **100 ms is
a flush trigger, not a hard durability deadline.** Compression, scheduling,
writes, and fsync can extend it. Each completed block write flushes the buffered
writer and calls file `sync_all` (fsync); it does not wait for the 100 MB target
or 30-minute file publication. Durability depends on successful I/O and the
filesystem/device honoring fsync.

## Files And Quota

```text
/mnt/hft/
  .writer.lock
  loss.json                 # eventual input-drop diagnostic
  .loss.json.partial        # temporary diagnostic replacement
  .staging/
    <unix-ns>-<pid>-<sequence>.arrow.partial
  shards/
    <unix-ns>-<pid>-<sequence>.arrow
```

Filename components are zero-padded decimal (20, 10, and 20 digits). They
describe creation time, not the source timestamp range. Filename sorting is not
a global source-order guarantee across clock changes/restarts. There are no
per-shard directories, `_READY` markers, or required sidecars.

Only one writer holds the nonblocking exclusive `.writer.lock`. Production
rejects a symlink root, the root filesystem, ordinary directories, and
same-device bind mounts: the existing root must be on a different device from
its parent. Staging/shard directories reject symlinks; relevant writer opens
use `O_NOFOLLOW`. The whole root must be service/operator-controlled, not
writable by untrusted users. There is no fallback to the system filesystem.
Tests can explicitly disable the dedicated-mount requirement.

Opening a stream flushes/fsyncs its schema and fsyncs `.staging`. Normal
finalization flushes the last block, writes the standard EOS marker,
flushes/fsyncs the file, and renames it into `shards` with `RENAME_NOREPLACE`.
Both parent directories are fsynced. A collision never overwrites an existing
finalized file. Normal consumers select only `shards/*.arrow`.

Quota charges allocated file and directory blocks (`st_blocks * 512`), including
staging, abandoned files, diagnostics, and pre-existing contents, not just
logical lengths. A block write reserves the larger of 64 MiB and
`2 * raw_block_bytes + 256 * (C + 2) + 8 MiB`; `C + 2` counts the writer's
original in-memory columns, not the three Arrow fields. Admission checks both
the application quota and `statvfs` available space plus the 512 MiB emergency
reserve. Encoded writes are capped to the reservation; allocation accounting
is updated after writes. Capture can stop before the nominal quota is full.
This application policy is not a filesystem-enforced quota against unrelated
writers; the dedicated service-owned filesystem remains necessary.

Quota exhaustion, `ENOSPC`, `EIO`, `EROFS`, write/fsync/publication failure, or a
format/size-limit failure stops local output for the process. The policy is
**append-until-stop, not retention**: no automatic archive deletion, circular
overwrite, or in-process retry loop is provided.

## Interrupted Streams

**The writer must not repair abandoned files. Tail handling belongs to readers.**
Startup leaves existing regular files in `.staging` and `shards` untouched,
including incomplete headers, malformed tails, and older writer formats. It
does not decode, truncate, append EOS, rename, publish, or delete these files.
They remain charged to quota. Non-regular entries in those directories cause
setup to fail without removing them. Entry checks are not a content audit.

An interrupted stream may contain a valid schema and a prefix of complete
blocks, followed by incomplete bytes. Readers can consume the surviving complete
prefix without modifying the original. Missing EOS does not necessarily discard
earlier blocks; an incomplete header may expose no records. Neither readers nor
restart can restore queued input or an unfinished in-memory block. Surviving
unsynced bytes are not evidence of an earlier durability acknowledgement.

Ordinary consumers must read finalized files only. For an abandoned
`.arrow.partial`, the operator must first stop the writer and prevent automatic
restart, then acquire and hold the same root's exclusive `.writer.lock` for the
entire read. Do not read a concurrently mutable staging file or copy one while
the writer is active and call that copy a consistent snapshot. `read_shard`
itself does not acquire this lock.

The Rust API `countersyncd::actor::local_storage::read_shard(path, visit)` is
read-only. It checks IPC message boundaries and bounds, the v4 schema, matrix
dimensions, nulls/offsets, and in-file sequence continuity before visiting a
block. It stops at an incomplete or certain invalid tails, returning samples
only from the fully decoded valid prefix. An invalid complete initial
header/schema/version is an error. Operational read/seek errors and visitor
errors propagate, even if earlier callbacks already ran. **Corrupt compressed
payloads can also produce a returned error** rather than a successful prefix
result. This is not arbitrary corruption repair or a promise that every damaged
file reads successfully. Do not equate successful prefix readback with complete
capture or a clean EOS.

`DecodedSample` contains `record_seq`, `stat_index`, `object_name`, `type_name`,
`stat_name`, `observation_time`, and `value`, delivered in record/stat order via
`values[c * T + t]`. This is a **samples API**: zero-counter records emit no
callbacks. Use standard Arrow lists when their timestamps/sequences matter.

Readers are intended for the service's own files, including crash-damaged
ones, not hostile input. Envelope/raw-size checks do not make arbitrary
FlatBuffers or malicious ZSTD expansion bombs safe to parse. Keep files and
directories under trusted control. v4 readers do not migrate or decode v1/v2/v3
data; select readers by embedded format version rather than renaming old files.

## Reading With PyArrow

Use PyArrow with IPC ZSTD support. This example makes a finite pass over the
finalized files selected at startup, reads one block at a time, validates the
whole block before yielding any records from it, and keeps Python integers
exact. It does not poll or tail a growing file.

```python
from pathlib import Path
import json
import warnings
import pyarrow as pa
import pyarrow.ipc as ipc


def iter_records(path):
    completed_blocks = 0
    previous_seq = None
    try:
        with pa.OSFile(str(path), "rb") as source:
            reader = ipc.open_stream(source)
            schema = reader.schema
            meta = schema.metadata or {}
            if (meta.get(b"format_version") != b"sonic-hft-arrow-v4"
                    or meta.get(b"timestamp_unit") != b"ns"
                    or meta.get(b"matrix_order") != b"series-major"
                    or schema.names != ["timestamps_ns", "record_seq", "values"]):
                raise ValueError("unsupported storage schema")
            for field in schema:
                if (field.nullable or field.metadata
                        or not pa.types.is_list(field.type)
                        or field.type.value_type != pa.uint64()
                        or field.type.value_field.nullable
                        or field.type.value_field.metadata):
                    raise ValueError("expected non-null List<UInt64> fields")
            raw_series = meta.get(b"series", b"")
            if len(raw_series) > 16 * 1024 * 1024:
                raise ValueError("series JSON exceeds 16 MiB")
            series = json.loads(raw_series)
            if not isinstance(series, list) or len(series) > 65536:
                raise ValueError("invalid series array/count")
            for names in series:
                if (not isinstance(names, dict)
                        or set(names) != {"object_name", "type_name", "stat_name"}
                        or not all(isinstance(v, str) for v in names.values())):
                    raise ValueError("invalid series names")
            C = len(series)
            while True:
                try:
                    batch = reader.read_next_batch()
                except StopIteration:
                    return
                batch.validate(full=True)
                if batch.num_rows != 1 or batch.num_columns != 3:
                    raise ValueError("expected one matrix block per batch")
                lists = []
                for column in batch.columns:
                    if (column.null_count or column.values.null_count
                            or column.offsets.to_pylist() != [0, len(column.values)]):
                        raise ValueError("invalid list offsets/nulls")
                    lists.append(column[0].as_py())
                timestamps_ns, record_seq, values = lists
                T = len(timestamps_ns)
                if (not 1 <= T <= 4096 or len(record_seq) != T
                        or len(values) != C * T
                        or 8 * (C + 2) * T > 128 * 1024 * 1024):
                    raise ValueError("invalid matrix dimensions/raw size")
                last = previous_seq
                for seq in record_seq:
                    if last is not None and seq != last + 1:
                        raise ValueError("non-contiguous record sequence")
                    last = seq
                previous_seq = last
                completed_blocks += 1
                for t in range(T):
                    stats = [(series[c], values[c * T + t]) for c in range(C)]
                    yield timestamps_ns[t], record_seq[t], stats
    except (pa.ArrowInvalid, OSError, EOFError, ValueError) as error:
        warnings.warn(
            f"{path}: stopped after {completed_blocks} complete blocks: "
            f"{type(error).__name__}: {error}. The stream may be incomplete "
            "or invalid, or reading may have failed; this is not a successful "
            "full-file read.",
            RuntimeWarning,
        )
        raise


files = sorted(Path("/mnt/hft/shards").glob("*.arrow"))
for path in files:
    for timestamp_ns, record_seq, stats in iter_records(path):
        print(timestamp_ns, record_seq, stats)
```

`stats` remains an ordered list, preserving duplicate identities; with `C == 0`
it is empty but each timestamp/sequence is still yielded. Do not convert the
integers to floats or signed-only timestamps. Python list/int allocations add
memory beyond Arrow's buffers; the example is not a bounded-RSS parser.

For truncated files, standard PyArrow may raise `ArrowInvalid`, `EOFError`, or
an I/O exception (`ArrowIOError` is exposed as `OSError`); the exact exception
depends on where reading fails. The example warns and **re-raises**, preserving
the exception for the caller rather than swallowing all I/O errors as crash
tails. Earlier yields contain only fully decoded, validated blocks. An exception
identifies unsuccessful readback, not proof that truncation rather than
corruption or operational I/O was the cause. Some missing-EOS cases end with
normal `StopIteration`, which by itself does not prove normal finalization.

To inspect one abandoned partial, use the same function only after stopping the
writer and preventing restart, holding the lock throughout iteration:

```python
import fcntl

root = Path("/mnt/hft")
partial = root / ".staging" / "<existing-filename>.arrow.partial"
with (root / ".writer.lock").open("rb") as lock:
    # Failure to acquire means do not read the partial.
    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    for timestamp_ns, record_seq, stats in iter_records(partial):
        print(timestamp_ns, record_seq, stats)
```

This leaves the partial unchanged. An error remains an error even after some
records have been printed; the example neither appends EOS nor publishes a
repaired file. It is an interoperability illustration for trusted service files,
not a substitute for the Rust reader's envelope checks or a hostile-file boundary.

## Diagnostics And Shutdown

Root-level `loss.json` is an eventual diagnostic, replaced through
`.loss.json.partial`, file fsync, rename, and root-directory fsync. Its fields
are `format_version`, `dropped_input_messages`, and `dropped_shards`.
`dropped_input_messages` counts dropped channel messages (flat batches), **not
exact lost records or samples**. The count accumulates from shared status and
the previous diagnostic across restarts. `dropped_shards` is zero because there
is no secondary shard queue, not because capture is lossless.

A full input queue records one message drop. A disconnected receiver records a
drop and marks storage failed. Later attempts after failure/shutdown are skipped
and are not all counted. Drops are observed asynchronously and do not identify
exact sequence positions or source-time loss intervals. UDP/decode losses and
interrupted RAM-only data are not accounted for here. Failure/kill can prevent
the latest count reaching disk. Missing diagnostics, zero drops, and contiguous
sequences are not evidence of complete capture. Diagnostic write failure also
stops local storage; it is not retried indefinitely.

With all senders closed and healthy I/O, the actor drains accepted input,
flushes its final block, finalizes the stream, and writes diagnostics.
SIGINT/SIGTERM supervision stops critical actors, closing IPFIX's sender. Main
waits up to 10 seconds, requests storage shutdown on timeout, and allows one
additional second before exiting without waiting indefinitely on filesystem
I/O. A stop request is checked between records and can leave queued records
unprocessed. This is not a drain of all pending Netlink input or other sinks.
Forced exit leaves surviving partial files for **reader-side** inspection, not
writer startup repair.

## Verification Scope

Source references are `crates/countersyncd/src/actor/local_storage.rs`,
`crates/countersyncd/src/actor/local_storage_codec.rs`,
`crates/countersyncd/src/actor/ipfix.rs`,
`crates/countersyncd/src/message/local_storage.rs`, and
`crates/countersyncd/src/main.rs`. Current source tests cover exact unsigned
values and timestamps, matrix ordering and zero-counter records, metadata and
counter limits, buffer reuse, rotation, read-only complete-prefix handling,
operational I/O errors, abandoned-file preservation, quota, locking, process
interruption, shutdown, and the nonblocking tap.

Rotation regressions cover tiny targets publishing only after complete batches,
sequence continuity across files, more than 128 MiB of real compressible raw
input staying in one below-target stream, and backdated maximum-age expiry.
The benchmark accepts the same file bytes/seconds options and defaults as main;
trial JSON reports `file_target_bytes`, `file_max_age_seconds`, and
`file_rotation_basis`. Its existing `shard_ns` reports maximum age in nanoseconds
(null only if that conversion exceeds `u64`). Existing performance reports are
not rewritten or reinterpreted as results for the new rotation policy.

This documentation edit does not claim those tests or the PyArrow example were
executed. Current verification results belong with the integration work and
the [v4 report](hft-local-storage-matrix-performance.md), not copied historical
test counts. Neither source coverage nor a process SIGKILL test establishes
physical power-loss behavior, arbitrary corruption repair, a hard durability/RSS
bound, or any particular lossless ingestion rate.
