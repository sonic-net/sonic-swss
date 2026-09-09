//! Bounded capture of exact raw observations in Arrow IPC streams.
//!
//! # File format and interpretation
//!
//! Each file is a self-contained **Arrow IPC stream**, compressed with ZSTD, not
//! an Arrow IPC file/Feather file or Parquet file. Schema metadata contains:
//! - `format_version = sonic-hft-arrow-v5`
//! - `timestamp_unit = ns` (the source observation time, not writer wall time)
//! - `matrix_order = series-major`
//! - `series`: an ordered JSON array of `{object_name, type_name, stat_name}`.
//!
//! Known identities use full SAI names, e.g. `Ethernet0`, `SAI_OBJECT_TYPE_PORT`,
//! `SAI_PORT_STAT_IF_IN_OCTETS`; unknown IDs use explicit numeric-suffix names.
//! Names are stored once per file, not per sample. Array position identifies the
//! series, including duplicate identity tuples: do not sort or deduplicate it.
//! Object type is not a counter/gauge classification. Units, clock epoch and
//! counter/reset semantics are source-specific, not inferred by this format.
//!
//! Every record batch has **one row**, containing two non-null `List<UInt64>`
//! fields with non-null elements:
//! - `timestamps_ns[0]`: T observation timestamps, shared by all C series.
//! - `values[0]`: C*T raw values, with series c / observation t at `c*T + t`.
//!
//! C is the length of `series`; T can vary between batches. For example:
//! ```text
//! series = [Ethernet0/IF_IN_OCTETS, Ethernet8/IF_IN_OCTETS]
//! timestamps_ns[0] = [1000000, 1100000, 1200000]
//! values[0]        = [10, 12, 12, 40, 40, 45]
//!                     Ethernet0    Ethernet8
//! ```
//! Thus Ethernet8 has value 45 at 1200000 ns. Empty input records are represented
//! by timestamps with C=0 and an empty values list. No stored record sequence,
//! application delta encoding, rounding, aggregation or resampling is applied.
//! Duplicate/regressing timestamps and decreasing values remain in input order.
//!
//! # Reading
//!
//! Rust clients can use [`read_shard`] for ordered [`DecodedSample`] callbacks.
//! Standard PyArrow can stream completed files without a private decoder:
//! ```python
//! import json
//! import pyarrow as pa
//! import pyarrow.ipc as ipc
//!
//! with pa.OSFile("capture.arrow", "rb") as source:
//!     reader = ipc.open_stream(source)
//!     meta = reader.schema.metadata
//!     assert meta[b"format_version"] == b"sonic-hft-arrow-v5"
//!     assert meta[b"timestamp_unit"] == b"ns"
//!     assert meta[b"matrix_order"] == b"series-major"
//!     series = json.loads(meta[b"series"].decode("utf-8"))
//!     for batch in reader:
//!         assert batch.num_rows == 1
//!         times = batch.column(0)[0].values
//!         values = batch.column(1)[0].values
//!         assert len(values) == len(series) * len(times)
//!         for c, identity in enumerate(series):
//!             samples = values.slice(c * len(times), len(times))
//!             # Process identity, times and samples here, one block at a time.
//! ```
//! Keep timestamps and values as unsigned integers/Python ints, not floats or
//! JavaScript Numbers. Compute differences with signed/widened arithmetic to
//! handle timestamp regressions and counter resets without unsigned underflow.
//! Do not reconstruct timestamps from a nominal polling interval. Earlier format
//! versions require their corresponding readers; v5 rejects them explicitly.
//!
//! # Publication, limits and lifetime
//!
//! One blocking storage worker consumes a bounded generic IPFIX recipient with
//! backpressure. Blocks target 16 MiB of raw data and at most 4096 observations;
//! partial blocks are due for flush after 100 ms. Encoding and I/O can delay this.
//! Each block is flushed and fsynced. A layout change, shutdown, maximum age or
//! encoded-byte target closes the stream, writes EOS, fsyncs and atomically moves
//! `.staging/<time_ns>-<pid>-<file_sequence>.arrow.partial` to `shards/*.arrow`.
//! The default target is 100,000,000 bytes / 30 minutes, checked after full blocks.
//! Files can exceed the size target by a block; earlier closure yields smaller
//! files. Within one known writer run, order files by numeric file_sequence, not
//! wall time (which can regress). Filenames alone do not identify runs globally.
//!
//! Writers never repair abandoned partials. After the writer stops, `read_shard`
//! can return the completely decoded prefix of an interrupted stream; an invalid
//! suffix is not returned and operational I/O errors propagate. Standard Arrow
//! readers may instead report a truncated-tail error. Neither proves that the
//! source delivered every observation, and unflushed memory can be lost.
//!
//! Capture is opt-in. The default `/tmp/hft` is created privately (0700), with
//! service-owned files (0600), a single-writer lock, symlink/device checks and a
//! 128 MiB quota including directory/staging allocation. Schema and batch admission
//! require at least 64 MiB of headroom, so capture can stop before the 100 MB
//! rotation target. Storage
//! errors close this recipient; backpressure and upstream receive loss remain
//! possible. There is no automatic upload, eviction or retention management.
//!
//! On tmpfs, admission also checks host MemAvailable with a 512 MiB emergency
//! reserve; this does not cover cgroup limits or guarantee against concurrent OOM.
//! `/tmp` may disappear on reboot/cleanup and differs across host/container
//! namespaces. For longer captures, configure a suitable root and quota. Stop
//! capture and verify transfer before deleting the capture directory.

use crate::message::{
    local_storage::LocalStorageStatus,
    saistats::{SAIStatsBatchMessage, SAIStatsRef},
};
use arrow_array::{RecordBatch, UInt64Array};
use arrow_ipc::{
    writer::{IpcWriteOptions, StreamWriter},
    CompressionType,
};
use log::{error, info};
use std::{
    fs::{self, File, OpenOptions},
    io::{self, BufWriter, Write},
    os::unix::{
        ffi::OsStrExt,
        fs::{DirBuilderExt, MetadataExt, OpenOptionsExt},
        io::AsRawFd,
    },
    path::{Path, PathBuf},
    sync::Arc,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
use tokio::{sync::mpsc::Receiver, time::MissedTickBehavior};

#[path = "local_storage_codec.rs"]
mod codec;
#[allow(unused_imports)]
pub use codec::{read_shard, series_names, DecodedSample};

const FORMAT_VERSION: &str = "sonic-hft-arrow-v5";
const LOCK_FILE: &str = ".writer.lock";
const BATCH_TARGET_BYTES: usize = 16 * 1024 * 1024;
const MAX_ROWS: usize = 4096;
const FLUSH_INTERVAL: Duration = Duration::from_millis(100);
const MAX_RECORD_BYTES: usize = 128 * 1024 * 1024;
const BATCH_RESERVE_BYTES: u64 = 64 * 1024 * 1024;
const FILESYSTEM_RESERVE_BYTES: u64 = 512 * 1024 * 1024;

#[derive(Debug, Clone)]
pub struct LocalStorageConfig {
    pub root: PathBuf,
    /// Maximum wall-clock stream age; batch flushes are due after 100 ms.
    /// Compression and filesystem sync time can extend the durability window.
    pub shard_interval: Duration,
    /// Logical compressed IPC bytes, checked after each complete batch flush.
    pub file_target_bytes: u64,
    pub max_bytes: u64,
    pub require_dedicated_filesystem: bool,
}

impl LocalStorageConfig {
    pub fn validate(&self) -> Result<(), String> {
        if self.shard_interval.is_zero() {
            return Err("local storage shard interval must be greater than zero".into());
        }
        if self.file_target_bytes == 0 {
            return Err("local storage file target bytes must be greater than zero".into());
        }
        if self.max_bytes <= BATCH_RESERVE_BYTES {
            return Err(format!(
                "local storage max bytes must exceed the {BATCH_RESERVE_BYTES} byte batch reserve"
            ));
        }
        Ok(())
    }

    fn prepare_root(&self) -> Result<(), String> {
        self.validate()?;
        // Inspect every component before traversing it, including aliases ending
        // in `/`, `/.`, or `/..`. Only the final directory may be created.
        let mut root = PathBuf::new();
        let mut components = self.root.components().peekable();
        while let Some(component) = components.next() {
            root.push(component);
            let metadata = match fs::symlink_metadata(&root) {
                Ok(metadata) => metadata,
                Err(e) if e.kind() == io::ErrorKind::NotFound && components.peek().is_none() => {
                    fs::DirBuilder::new()
                        .mode(0o700)
                        .create(&root)
                        .map_err(|e| e.to_string())?;
                    fs::symlink_metadata(&root).map_err(|e| e.to_string())?
                }
                Err(e) => return Err(e.to_string()),
            };
            if metadata.file_type().is_symlink() || !metadata.is_dir() {
                return Err(format!(
                    "local storage path {} must be a directory, not a symbolic link",
                    root.display()
                ));
            }
        }
        let metadata = fs::symlink_metadata(&root).map_err(|e| e.to_string())?;
        validate_root_access(&root, metadata.uid(), metadata.mode())?;
        if self.require_dedicated_filesystem && !is_mount_point(&root)? {
            return Err(format!(
                "local storage root {} must be a dedicated mount point",
                self.root.display()
            ));
        }
        Ok(())
    }
}

// Cap each schema/batch write before it consumes unreserved disk space. The
// BufWriter sits inside the cap so its buffered bytes are charged immediately.
struct CappedFile {
    file: BufWriter<File>,
    remaining: u64,
    bytes_written: u64,
}

impl Write for CappedFile {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() as u64 > self.remaining {
            return Err(io::Error::other("IPC write exceeds batch reservation"));
        }
        let written = self.file.write(bytes)?;
        self.remaining -= written as u64;
        self.bytes_written += written as u64;
        Ok(written)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.file.flush()
    }
}

struct ActiveStream {
    writer: StreamWriter<CappedFile>,
    path: PathBuf,
    started: Instant,
}

struct Store {
    config: LocalStorageConfig,
    layout: Option<codec::Layout>,
    columns: Vec<Vec<u64>>,
    // At most 16 MiB of original columns plus 16 MiB of reusable series-major
    // concatenation, excluding Arrow's bounded IPC/compression working buffers.
    matrix: Vec<u64>,
    active: Option<ActiveStream>,
    file_sequence: u64,
    batch_started: Option<Instant>,
    // Includes all files and directory blocks, including active staging data.
    used_bytes: u64,
    directory_blocks: u64,
    _storage_lock: File,
}

impl Store {
    fn new(config: LocalStorageConfig, lock: File) -> Result<Self, String> {
        let used_bytes = directory_bytes(&config.root).map_err(|e| e.to_string())?;
        let directory_blocks = directory_overhead(&config.root)?;
        Ok(Self {
            config,
            layout: None,
            columns: Vec::new(),
            matrix: Vec::new(),
            active: None,
            file_sequence: 0,
            batch_started: None,
            used_bytes,
            directory_blocks,
            _storage_lock: lock,
        })
    }

    fn reserve(&mut self, bytes: u64) -> Result<(), String> {
        // Only directory blocks can change independently of our writes under the
        // service-owned root. Refresh them without rescanning finalized files.
        self.refresh_directory_blocks()?;
        if self.used_bytes.saturating_add(bytes) > self.config.max_bytes {
            return Err(format!(
                "local storage quota cannot reserve batch: {} allocated + {bytes} reserve > {}",
                self.used_bytes, self.config.max_bytes
            ));
        }
        if available_bytes(&self.config.root)? < bytes.saturating_add(FILESYSTEM_RESERVE_BYTES) {
            return Err(
                "local storage filesystem or tmpfs host-memory emergency reserve reached".into(),
            );
        }
        Ok(())
    }

    fn open_stream(&mut self) -> Result<(), String> {
        self.reserve(BATCH_RESERVE_BYTES)?;
        let layout = self.layout.as_ref().ok_or("missing layout")?;
        let options = IpcWriteOptions::default()
            .try_with_compression(Some(CompressionType::ZSTD))
            .map_err(|e| e.to_string())?;
        let (path, file) = loop {
            let now = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map_err(|e| e.to_string())?
                .as_nanos();
            let name = format!(
                "{now:020}-{:010}-{:020}.arrow",
                std::process::id(),
                self.file_sequence
            );
            self.file_sequence = self
                .file_sequence
                .checked_add(1)
                .ok_or("file sequence exhausted")?;
            if self
                .config
                .root
                .join("shards")
                .join(&name)
                .try_exists()
                .map_err(|e| e.to_string())?
            {
                continue;
            }
            let path = self
                .config
                .root
                .join(".staging")
                .join(format!("{name}.partial"));
            match OpenOptions::new()
                .create_new(true)
                .read(true)
                .write(true)
                .mode(0o600)
                .custom_flags(libc::O_NOFOLLOW)
                .open(&path)
            {
                Ok(file) => break (path, file),
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(e.to_string()),
            }
        };
        let writer = StreamWriter::try_new_with_options(
            CappedFile {
                file: BufWriter::with_capacity(256 * 1024, file),
                remaining: BATCH_RESERVE_BYTES,
                bytes_written: 0,
            },
            &layout.schema,
            options,
        )
        .map_err(|e| e.to_string())?;
        self.active = Some(ActiveStream {
            writer,
            path,
            started: Instant::now(),
        });
        self.sync_active()?;
        sync_directory(&self.config.root.join(".staging")).map_err(|e| e.to_string())?;
        Ok(())
    }

    fn sync_active(&mut self) -> Result<(), String> {
        let active = self.active.as_mut().ok_or("no active stream")?;
        active.writer.get_mut().flush().map_err(|e| e.to_string())?;
        active
            .writer
            .get_ref()
            .file
            .get_ref()
            .sync_all()
            .map_err(|e| e.to_string())?;
        self.used_bytes = self.used_bytes.saturating_add(
            active
                .writer
                .get_ref()
                .file
                .get_ref()
                .metadata()
                .map_err(|e| e.to_string())?
                .blocks()
                * 512,
        );
        self.refresh_directory_blocks()?;
        Ok(())
    }

    fn refresh_directory_blocks(&mut self) -> Result<(), String> {
        let overhead = directory_overhead(&self.config.root)?;
        self.used_bytes = self
            .used_bytes
            .saturating_sub(self.directory_blocks)
            .saturating_add(overhead);
        self.directory_blocks = overhead;
        Ok(())
    }

    fn add_record(&mut self, record: SAIStatsRef<'_>) -> Result<(), String> {
        self.tick()?;
        if record.stats.len().saturating_add(1).saturating_mul(8) > MAX_RECORD_BYTES {
            self.finish_stream()?;
            return Err("record exceeds 128 MiB raw limit; previous records flushed".into());
        }
        if !self.layout.as_ref().is_some_and(|l| l.matches(record)) {
            self.finish_stream()?;
            self.layout = None;
            self.columns.clear();
            self.matrix = Vec::new();
            let layout = codec::Layout::new(record.stats)?;
            self.columns = (0..record.stats.len() + 1)
                .map(|_| Vec::with_capacity(layout.batch_rows))
                .collect();
            self.matrix = Vec::with_capacity(record.stats.len() * layout.batch_rows);
            self.layout = Some(layout);
        }
        if self.active.is_none() {
            self.open_stream()?;
        }
        self.batch_started.get_or_insert_with(Instant::now);
        self.columns[0].push(record.observation_time);
        for (column, stat) in self.columns[1..].iter_mut().zip(record.stats) {
            column.push(stat.counter);
        }
        let layout = self.layout.as_ref().unwrap();
        if self.columns[0].len() >= layout.batch_rows {
            self.flush_batch()?;
        }
        Ok(())
    }

    fn flush_batch(&mut self) -> Result<(), String> {
        if self.batch_started.is_none() {
            return Ok(());
        }
        let layout = self.layout.as_ref().ok_or("missing batch layout")?;
        let raw_bytes = (self.columns[0].len() * layout.row_bytes) as u64;
        let reserve = BATCH_RESERVE_BYTES.max(
            raw_bytes
                .saturating_mul(2)
                .saturating_add((self.columns.len() as u64) * 256 + 8 * 1024 * 1024),
        );
        let schema = Arc::clone(&layout.schema);
        self.reserve(reserve)?;
        for column in &mut self.columns[1..] {
            self.matrix.extend_from_slice(column);
            column.clear();
        }
        let arrays = [
            UInt64Array::from(std::mem::take(&mut self.columns[0])),
            UInt64Array::from(std::mem::take(&mut self.matrix)),
        ];
        let batch = RecordBatch::try_new(
            schema,
            arrays.iter().map(|a| codec::list(a.clone())).collect(),
        )
        .map_err(|e| e.to_string())?;
        let active = self.active.as_mut().ok_or("missing batch stream")?;
        let before = active
            .writer
            .get_ref()
            .file
            .get_ref()
            .metadata()
            .map_err(|e| e.to_string())?
            .blocks()
            * 512;
        active.writer.get_mut().remaining = reserve;
        active.writer.write(&batch).map_err(|e| e.to_string())?;
        active.writer.get_mut().flush().map_err(|e| e.to_string())?;
        active
            .writer
            .get_ref()
            .file
            .get_ref()
            .sync_all()
            .map_err(|e| e.to_string())?;
        let after = active
            .writer
            .get_ref()
            .file
            .get_ref()
            .metadata()
            .map_err(|e| e.to_string())?
            .blocks()
            * 512;
        self.used_bytes = self.used_bytes.saturating_add(after.saturating_sub(before));
        drop(batch);
        let rows = self.layout.as_ref().unwrap().batch_rows;
        // Reclaim the original Vec allocations after Arrow releases its references.
        let matrix_capacity = (self.columns.len() - 1) * rows;
        for (i, array) in arrays.into_iter().enumerate() {
            let c = if i == 0 {
                &mut self.columns[i]
            } else {
                &mut self.matrix
            };
            *c = array
                .into_parts()
                .1
                .into_inner()
                .into_vec::<u64>()
                .unwrap_or_else(|_| {
                    Vec::with_capacity(if i == 0 { rows } else { matrix_capacity })
                });
            c.clear();
        }
        self.batch_started = None;
        // Include schema/framing, but always persist a batch before size rotation.
        if self
            .active
            .as_ref()
            .is_some_and(|a| a.writer.get_ref().bytes_written >= self.config.file_target_bytes)
        {
            self.publish_stream()?;
        }
        Ok(())
    }

    fn finish_stream(&mut self) -> Result<(), String> {
        self.flush_batch()?;
        self.publish_stream()
    }

    fn publish_stream(&mut self) -> Result<(), String> {
        if let Some(mut active) = self.active.take() {
            let before = active
                .writer
                .get_ref()
                .file
                .get_ref()
                .metadata()
                .map_err(|e| e.to_string())?
                .blocks()
                * 512;
            active.writer.get_mut().remaining = 8;
            active.writer.finish().map_err(|e| e.to_string())?;
            active.writer.get_mut().flush().map_err(|e| e.to_string())?;
            active
                .writer
                .get_ref()
                .file
                .get_ref()
                .sync_all()
                .map_err(|e| e.to_string())?;
            publish(&self.config.root, &active.path)?;
            let after = active
                .writer
                .get_ref()
                .file
                .get_ref()
                .metadata()
                .map_err(|e| e.to_string())?
                .blocks()
                * 512;
            self.used_bytes = self.used_bytes.saturating_add(after.saturating_sub(before));
            self.refresh_directory_blocks()?;
        }
        Ok(())
    }

    fn tick(&mut self) -> Result<(), String> {
        if self
            .active
            .as_ref()
            .is_some_and(|a| a.started.elapsed() >= self.config.shard_interval)
        {
            self.finish_stream()?;
        } else if self
            .batch_started
            .is_some_and(|t| t.elapsed() >= FLUSH_INTERVAL)
        {
            self.flush_batch()?;
        }
        Ok(())
    }
}

pub struct LocalStorageActor {
    receiver: Receiver<SAIStatsBatchMessage>,
    status: LocalStorageStatus,
    store: Store,
}

impl LocalStorageActor {
    pub fn new(
        receiver: Receiver<SAIStatsBatchMessage>,
        config: LocalStorageConfig,
        status: LocalStorageStatus,
    ) -> Result<Self, String> {
        let result = prepare_storage(&config).and_then(|lock| Store::new(config, lock));
        let store = result.inspect_err(|_| status.mark_failed())?;
        Ok(Self {
            receiver,
            status,
            store,
        })
    }

    /// One isolated blocking storage loop: no secondary queue or whole-shard drops.
    /// Run on a dedicated blocking worker, never on a shared async runtime thread.
    /// Shutdown closes the receiver to new sends, then drains accepted buffered
    /// messages and flushes them before returning (unless storage fails).
    pub fn run(mut self) {
        let result = tokio::runtime::Builder::new_current_thread()
            .enable_time()
            .build()
            .map_err(|e| e.to_string())
            .and_then(|runtime| {
                runtime.block_on(async {
                    let mut tick = tokio::time::interval(FLUSH_INTERVAL);
                    tick.set_missed_tick_behavior(MissedTickBehavior::Skip);
                    while !self.status.failed() {
                        if self.status.shutdown_requested() {
                            self.receiver.close();
                        }
                        tokio::select! {
                            biased;
                            _ = tick.tick() => {
                                self.store.tick()?;
                                self.store.flush_batch()?;
                            }
                            message = self.receiver.recv() => {
                                let Some(message) = message else { break; };
                                for record in message.iter() {
                                    self.store.add_record(record)?;
                                }
                            }
                        }
                    }
                    self.store.finish_stream()
                })
            });
        if let Err(reason) = result {
            // Do not retry a failed IPC write or modify its durable prefix.
            // Readers consume complete batches directly from abandoned partials.
            self.status.mark_failed();
            error!("Disabling local HFT storage: {reason}");
        }
    }
}

fn validate_root_access(path: &Path, owner: u32, mode: u32) -> Result<(), String> {
    if owner != unsafe { libc::geteuid() } || mode & 0o077 != 0 {
        return Err(format!(
            "local storage root {} must be owned by the effective service user with no group/other permissions",
            path.display()
        ));
    }
    Ok(())
}

fn is_mount_point(path: &Path) -> Result<bool, String> {
    let path = fs::canonicalize(path).map_err(|e| e.to_string())?;
    let metadata = fs::metadata(&path).map_err(|e| e.to_string())?;
    let parent = path.parent().ok_or("local storage root has no parent")?;
    Ok(metadata.dev() != fs::metadata(parent).map_err(|e| e.to_string())?.dev())
}

fn validate_storage_device(root_dev: u64, child_dev: u64, path: &Path) -> Result<(), String> {
    if child_dev != root_dev {
        return Err(format!(
            "{} must be on the same filesystem as the local storage root",
            path.display()
        ));
    }
    Ok(())
}

fn ensure_directory(path: &Path, root_dev: u64) -> Result<(), String> {
    match fs::symlink_metadata(path) {
        Ok(m) if m.file_type().is_symlink() || !m.is_dir() => Err(format!(
            "{} must be a directory, not a symbolic link",
            path.display()
        )),
        Ok(metadata) => validate_storage_device(root_dev, metadata.dev(), path),
        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            fs::DirBuilder::new()
                .mode(0o700)
                .create(path)
                .map_err(|e| e.to_string())?;
            let metadata = fs::symlink_metadata(path).map_err(|e| e.to_string())?;
            validate_storage_device(root_dev, metadata.dev(), path)
        }
        Err(e) => Err(e.to_string()),
    }
}

fn prepare_storage(config: &LocalStorageConfig) -> Result<File, String> {
    config.prepare_root()?;
    let lock = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW)
        .open(config.root.join(LOCK_FILE))
        .map_err(|e| e.to_string())?;
    if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        return Err(format!(
            "local storage root is already in use: {}",
            io::Error::last_os_error()
        ));
    }
    // Reservation uses root's statvfs, and publication renames between these
    // directories. Reject nested filesystems before writing any IPC data.
    let root_dev = fs::metadata(&config.root).map_err(|e| e.to_string())?.dev();
    ensure_directory(&config.root.join(".staging"), root_dev)?;
    ensure_directory(&config.root.join("shards"), root_dev)?;
    sync_directory(&config.root).map_err(|e| e.to_string())?;
    // Leave regular files from every writer generation untouched. Readers decide
    // format support; all abandoned files remain charged against the disk quota.
    for entry in fs::read_dir(config.root.join("shards")).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if !entry.file_type().map_err(|e| e.to_string())?.is_file() {
            return Err(format!(
                "unsupported archive entry {}; use a separate empty storage root",
                entry.path().display()
            ));
        }
    }
    for entry in fs::read_dir(config.root.join(".staging")).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if !entry.file_type().map_err(|e| e.to_string())?.is_file() {
            return Err(format!(
                "unknown staging entry {}; left untouched",
                entry.path().display()
            ));
        }
    }
    Ok(lock)
}

fn publish(root: &Path, path: &Path) -> Result<(), String> {
    let name = path
        .file_name()
        .and_then(|n| n.to_str())
        .and_then(|n| n.strip_suffix(".partial"))
        .ok_or("invalid partial filename")?;
    let destination = root.join("shards").join(name);
    // RENAME_NOREPLACE preserves finalized data even if a name unexpectedly collides.
    let from = std::ffi::CString::new(path.as_os_str().as_bytes()).map_err(|e| e.to_string())?;
    let to =
        std::ffi::CString::new(destination.as_os_str().as_bytes()).map_err(|e| e.to_string())?;
    if unsafe {
        libc::renameat2(
            libc::AT_FDCWD,
            from.as_ptr(),
            libc::AT_FDCWD,
            to.as_ptr(),
            libc::RENAME_NOREPLACE,
        )
    } != 0
    {
        return Err(io::Error::last_os_error().to_string());
    }
    sync_directory(&root.join("shards")).map_err(|e| e.to_string())?;
    sync_directory(&root.join(".staging")).map_err(|e| e.to_string())?;
    info!("Published local HFT stream {}", destination.display());
    Ok(())
}

fn directory_bytes(path: &Path) -> io::Result<u64> {
    let mut total = fs::symlink_metadata(path)?.blocks().saturating_mul(512);
    for entry in fs::read_dir(path)? {
        let entry = entry?;
        let m = fs::symlink_metadata(entry.path())?;
        total = total.saturating_add(if m.is_dir() && !m.file_type().is_symlink() {
            directory_bytes(&entry.path())?
        } else {
            m.blocks().saturating_mul(512)
        });
    }
    Ok(total)
}

fn directory_overhead(root: &Path) -> Result<u64, String> {
    [
        root.to_path_buf(),
        root.join(".staging"),
        root.join("shards"),
    ]
    .iter()
    .try_fold(0u64, |sum, path| {
        Ok(sum.saturating_add(
            fs::metadata(path)
                .map_err(|e| e.to_string())?
                .blocks()
                .saturating_mul(512),
        ))
    })
}

fn sync_directory(path: &Path) -> io::Result<()> {
    File::open(path)?.sync_all()
}

fn available_bytes(path: &Path) -> Result<u64, String> {
    let path = std::ffi::CString::new(path.as_os_str().as_bytes()).map_err(|e| e.to_string())?;
    let mut stats = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    if unsafe { libc::statvfs(path.as_ptr(), stats.as_mut_ptr()) } != 0 {
        return Err(io::Error::last_os_error().to_string());
    }
    let stats = unsafe { stats.assume_init() };
    let free = (stats.f_bavail as u64).saturating_mul(stats.f_frsize as u64);
    let mut filesystem = std::mem::MaybeUninit::<libc::statfs>::uninit();
    if unsafe { libc::statfs(path.as_ptr(), filesystem.as_mut_ptr()) } != 0 {
        return Err(io::Error::last_os_error().to_string());
    }
    if unsafe { filesystem.assume_init() }.f_type == 0x0102_1994 {
        let meminfo = fs::read_to_string("/proc/meminfo").map_err(|e| e.to_string())?;
        return tmpfs_available_bytes(free, &meminfo);
    }
    Ok(free)
}

fn tmpfs_available_bytes(free: u64, meminfo: &str) -> Result<u64, String> {
    for line in meminfo.lines() {
        let mut fields = line.split_whitespace();
        if fields.next() != Some("MemAvailable:") {
            continue;
        }
        let bytes = fields
            .next()
            .and_then(|value| value.parse::<u64>().ok())
            .and_then(|kb| kb.checked_mul(1024));
        if let Some(bytes) = bytes {
            if fields.next() == Some("kB") && fields.next().is_none() {
                return Ok(free.min(bytes));
            }
        }
        break;
    }
    Err("tmpfs capture requires valid MemAvailable in /proc/meminfo".into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::saistats::{SAIStat, SAIStatsBatch};
    use arrow_array::{Array, ListArray};
    use arrow_ipc::reader::StreamReader;
    use std::{
        io::Cursor,
        process::{Command, Stdio},
        sync::mpsc,
        thread,
    };
    use tokio::sync::mpsc::channel;
    use wait_timeout::ChildExt;

    fn private_tempdir() -> tempfile::TempDir {
        use std::os::unix::fs::PermissionsExt;
        tempfile::Builder::new()
            .permissions(fs::Permissions::from_mode(0o700))
            .tempdir()
            .unwrap()
    }

    fn config(root: &Path) -> LocalStorageConfig {
        LocalStorageConfig {
            root: root.into(),
            shard_interval: Duration::from_secs(1800),
            file_target_bytes: 100_000_000,
            max_bytes: 1_000_000_000,
            require_dedicated_filesystem: false,
        }
    }

    fn store(root: &Path) -> Store {
        let config = config(root);
        let lock = prepare_storage(&config).unwrap();
        Store::new(config, lock).unwrap()
    }

    fn stat(name: &str, value: u64) -> SAIStat {
        SAIStat::new(name, 1, 2, value)
    }

    fn paths(root: &Path, directory: &str) -> Vec<PathBuf> {
        let mut paths: Vec<_> = fs::read_dir(root.join(directory))
            .unwrap()
            .map(|e| e.unwrap().path())
            .collect();
        paths.sort();
        paths
    }

    fn decoded(path: &Path) -> Vec<DecodedSample> {
        let mut samples = Vec::new();
        read_shard(path, |sample| {
            samples.push(sample);
            Ok(())
        })
        .unwrap();
        samples
    }

    fn child(batch: &RecordBatch, column: usize) -> &UInt64Array {
        batch
            .column(column)
            .as_any()
            .downcast_ref::<ListArray>()
            .unwrap()
            .values()
            .as_any()
            .downcast_ref::<UInt64Array>()
            .unwrap()
    }

    fn expected(batch: &SAIStatsBatch) -> Vec<DecodedSample> {
        batch
            .iter()
            .flat_map(|record| {
                record.stats.iter().enumerate().map(move |(i, stat)| {
                    let (type_name, stat_name) = series_names(stat.type_id, stat.stat_id);
                    DecodedSample {
                        stat_index: i as u32,
                        object_name: Arc::clone(&stat.object_name),
                        type_name: type_name.into(),
                        stat_name: stat_name.into(),
                        observation_time: record.observation_time,
                        value: stat.counter,
                    }
                })
            })
            .collect()
    }

    fn encode(batch: &SAIStatsBatch) -> (tempfile::TempDir, Vec<PathBuf>) {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        for record in batch.iter() {
            store.add_record(record).unwrap();
        }
        store.finish_stream().unwrap();
        assert_eq!(store.used_bytes, directory_bytes(temp.path()).unwrap());
        let paths = paths(temp.path(), "shards");
        (temp, paths)
    }

    #[test]
    fn exact_raw_random_gauges_peaks_valleys_and_unsigned_boundaries() {
        let mut batch = SAIStatsBatch::default();
        let mut random = 0x1234_5678_9abc_def0u64;
        let extremes = [0, u64::MAX, 1 << 63, (1 << 63) - 1, (1 << 53) + 1, 1];
        for i in 0..9000usize {
            random ^= random << 13;
            random ^= random >> 7;
            random ^= random << 17;
            let time = [0, u64::MAX, 42, 42, 1, random][i % 6];
            batch.push_record(
                time,
                [
                    stat("random|\"\n", random),
                    stat("extremes", extremes[i % 6]),
                    stat("constant", 42),
                    stat("sawtooth", i as u64 % 7),
                    stat("duplicate", 0),
                    stat("duplicate", u64::MAX),
                    SAIStat::new("unknown", u32::MAX, u32::MAX, random),
                ],
            );
        }
        let (_temp, files) = encode(&batch);
        assert_eq!(files.len(), 1);
        assert_eq!(decoded(&files[0]), expected(&batch));
        let reader = StreamReader::try_new(File::open(&files[0]).unwrap(), None).unwrap();
        assert_eq!(
            reader.schema().metadata()["format_version"],
            "sonic-hft-arrow-v5"
        );
        assert_eq!(reader.schema().fields().len(), 2);
        assert_eq!(reader.schema().field(0).name(), "timestamps_ns");
        assert_eq!(reader.schema().field(1).name(), "values");
        for field in reader.schema().fields() {
            assert!(!field.is_nullable());
            assert_eq!(
                field.data_type(),
                &arrow_schema::DataType::List(Arc::new(arrow_schema::Field::new(
                    "item",
                    arrow_schema::DataType::UInt64,
                    false
                )))
            );
        }
        let batches: Vec<_> = reader.map(Result::unwrap).collect();
        assert_eq!(
            batches.iter().map(|b| child(b, 0).len()).sum::<usize>(),
            9000
        );
        for b in batches {
            assert_eq!(b.num_rows(), 1);
            for c in b.columns() {
                assert!(matches!(c.data_type(), arrow_schema::DataType::List(_)));
                assert_eq!(c.null_count(), 0);
            }
        }
    }

    #[test]
    fn layout_changes_stat_order_names_ids_duplicates_and_empty_rows() {
        let mut batch = SAIStatsBatch::default();
        batch.push_record(u64::MAX, []);
        batch.push_record(0, []);
        batch.push_record(2, [stat("a", 1), stat("b", 2)]);
        batch.push_record(2, [stat("b", 3), stat("a", 4)]);
        batch.push_record(1, [stat("b", 5), stat("a", 6)]);
        batch.push_record(1, [stat("a", 0), stat("a", u64::MAX)]);
        batch.push_record(0, [SAIStat::new("a", 21, 0, 42), stat("a", 2)]);
        batch.push_record(0, [SAIStat::new("a", 21, 1, 42), stat("a", 2)]);
        batch.push_record(0, [SAIStat::new("renamed", 21, 1, 42), stat("a", 2)]);
        batch.push_record(0, []);
        let (_temp, files) = encode(&batch);
        assert_eq!(files.len(), 8);
        assert_eq!(
            files.iter().flat_map(|p| decoded(p)).collect::<Vec<_>>(),
            expected(&batch)
        );
        let rows: Vec<u64> = files
            .iter()
            .flat_map(|p| {
                StreamReader::try_new(File::open(p).unwrap(), None)
                    .unwrap()
                    .flat_map(|b| {
                        let b = b.unwrap();
                        child(&b, 0).values().to_vec()
                    })
                    .collect::<Vec<_>>()
            })
            .collect();
        assert_eq!(
            rows,
            batch.iter().map(|r| r.observation_time).collect::<Vec<_>>()
        );
        assert_eq!(
            read_shard(&files[1], |_| Err("visitor stopped".into())),
            Err("visitor stopped".into())
        );
    }

    #[test]
    fn wide_layout_batch_budget_and_buffer_reuse() {
        for (width, expected_rows) in [
            (0, 4096),
            (500, 4096),
            (8000, 262),
            (codec::MAX_COUNTERS, 31),
        ] {
            let temp = private_tempdir();
            let mut store = store(temp.path());
            let stats: Vec<_> = (0..width).map(|i| stat("port", i as u64)).collect();
            let record = SAIStatsRef {
                observation_time: u64::MAX,
                stats: &stats,
            };
            store.add_record(record).unwrap();
            let layout = store.layout.as_ref().unwrap();
            assert_eq!(
                layout.batch_rows,
                (BATCH_TARGET_BYTES / ((width + 1) * 8)).min(MAX_ROWS)
            );
            assert_eq!(layout.batch_rows, expected_rows);
            assert!(layout.batch_rows * layout.row_bytes <= BATCH_TARGET_BYTES);
            let pointers: Vec<_> = store.columns.iter().map(|c| c.as_ptr()).collect();
            let matrix_pointer = store.matrix.as_ptr();
            store.flush_batch().unwrap();
            assert_eq!(
                pointers,
                store.columns.iter().map(|c| c.as_ptr()).collect::<Vec<_>>()
            );
            assert_eq!(matrix_pointer, store.matrix.as_ptr());
            assert!(
                store
                    .columns
                    .iter()
                    .map(|c| c.capacity() * 8)
                    .sum::<usize>()
                    <= BATCH_TARGET_BYTES
            );
            assert!(store.matrix.capacity() * 8 <= BATCH_TARGET_BYTES);
            store.add_record(record).unwrap();
            store.flush_batch().unwrap();
            assert_eq!(matrix_pointer, store.matrix.as_ptr());
            assert_eq!(store.used_bytes, directory_bytes(temp.path()).unwrap());
            store.finish_stream().unwrap();
            assert_eq!(decoded(&paths(temp.path(), "shards")[0]).len(), width * 2);
        }
    }

    fn small_stream() -> (Vec<u8>, Vec<(usize, u64)>, Vec<DecodedSample>) {
        let layout = codec::Layout::new(&[stat("a", 0)]).unwrap();
        let options = IpcWriteOptions::default()
            .try_with_compression(Some(CompressionType::ZSTD))
            .unwrap();
        let mut writer =
            StreamWriter::try_new_with_options(Vec::new(), &layout.schema, options).unwrap();
        let mut boundaries = vec![(writer.get_ref().len(), 0)];
        let mut expected = Vec::new();
        for i in 0..3u64 {
            let batch = RecordBatch::try_new(
                Arc::clone(&layout.schema),
                vec![
                    codec::list(UInt64Array::from(vec![u64::MAX - i])),
                    codec::list(UInt64Array::from(vec![[u64::MAX, 0, 1 << 63][i as usize]])),
                ],
            )
            .unwrap();
            writer.write(&batch).unwrap();
            boundaries.push((writer.get_ref().len(), i + 1));
            let (ty, stat) = series_names(1, 2);
            expected.push(DecodedSample {
                stat_index: 0,
                object_name: "a".into(),
                type_name: ty.into(),
                stat_name: stat.into(),
                observation_time: u64::MAX - i,
                value: [u64::MAX, 0, 1 << 63][i as usize],
            });
        }
        writer.finish().unwrap();
        (writer.into_inner().unwrap(), boundaries, expected)
    }

    #[test]
    fn read_every_byte_truncation_preserves_exact_complete_batch_prefix() {
        let (bytes, boundaries, expected) = small_stream();
        let temp = private_tempdir();
        let _lock = prepare_storage(&config(temp.path())).unwrap();
        let partial = temp.path().join(".staging/truncated.arrow.partial");
        for cut in 0..=bytes.len() {
            fs::write(&partial, &bytes[..cut]).unwrap();
            let count = boundaries
                .iter()
                .filter(|(end, _)| *end <= cut)
                .map(|(_, count)| *count)
                .last()
                .unwrap_or(0);
            let scan = codec::scan(&partial).unwrap();
            assert_eq!(scan.records, count, "cut={cut}");
            assert!(scan.boundary <= cut as u64);
            assert_eq!(decoded(&partial), expected[..count as usize], "cut={cut}");
            assert_eq!(fs::read(&partial).unwrap(), bytes[..cut]);
            assert!(paths(temp.path(), "shards").is_empty());
        }
    }

    #[test]
    fn writer_does_not_modify_abandoned_streams_and_reader_handles_tails() {
        let (bytes, boundaries, expected) = small_stream();
        for suffix in [
            vec![],
            vec![255, 255, 255, 255, 255, 255, 255, 127],
            vec![255, 255, 255, 255, 0, 0, 0, 0],
        ] {
            let temp = private_tempdir();
            drop(prepare_storage(&config(temp.path())).unwrap());
            let partial = temp.path().join(".staging/recovered.arrow.partial");
            let end = boundaries.last().unwrap().0;
            let mut damaged = bytes[..end].to_vec();
            damaged.extend(suffix);
            fs::write(&partial, &damaged).unwrap();
            for _ in 0..2 {
                let mut writer = store(temp.path());
                assert_eq!(writer.used_bytes, directory_bytes(temp.path()).unwrap());
                writer
                    .add_record(SAIStatsRef {
                        observation_time: 0,
                        stats: &[],
                    })
                    .unwrap();
                assert_ne!(writer.active.as_ref().unwrap().path, partial);
                writer.finish_stream().unwrap();
                assert_eq!(decoded(&partial), expected);
                assert_eq!(fs::read(&partial).unwrap(), damaged);
                assert_eq!(paths(temp.path(), ".staging"), vec![partial.clone()]);
            }
        }
    }

    #[test]
    fn rejects_invalid_header_and_nullable_schema_without_removing_state() {
        let temp = private_tempdir();
        drop(prepare_storage(&config(temp.path())).unwrap());
        let partial = temp.path().join(".staging/bad.arrow.partial");
        for bytes in [b"not an Arrow stream".to_vec(), {
            let schema = arrow_schema::Schema::new(vec![arrow_schema::Field::new(
                "timestamp_ns",
                arrow_schema::DataType::UInt64,
                true,
            )]);
            let mut writer = StreamWriter::try_new(Vec::new(), &schema).unwrap();
            writer.finish().unwrap();
            writer.into_inner().unwrap()
        }] {
            fs::write(&partial, &bytes).unwrap();
            assert!(read_shard(&partial, |_| Ok(())).is_err());
            drop(prepare_storage(&config(temp.path())).unwrap());
            assert_eq!(fs::read(&partial).unwrap(), bytes);
        }
    }

    #[test]
    fn startup_preserves_empty_unknown_and_old_files_and_charges_quota() {
        let (bytes, _, _) = small_stream();
        let temp = private_tempdir();
        drop(prepare_storage(&config(temp.path())).unwrap());
        let files = [
            (".staging/empty.arrow.partial", &b""[..]),
            (".staging/torn.arrow.partial", &bytes[..3]),
            (".staging/old.partial", &b"old-v3"[..]),
            ("shards/old.arrow", &b"old-v3"[..]),
            ("loss.json", &b"obsolete diagnostics, not parsed"[..]),
            (".loss.json.partial", &b"obsolete partial diagnostics"[..]),
        ];
        for (path, data) in files {
            fs::write(temp.path().join(path), data).unwrap();
        }
        let mut writer = store(temp.path());
        assert_eq!(writer.used_bytes, directory_bytes(temp.path()).unwrap());
        writer.config.max_bytes = writer.used_bytes + BATCH_RESERVE_BYTES - 1;
        assert!(writer
            .add_record(SAIStatsRef {
                observation_time: 0,
                stats: &[]
            })
            .is_err());
        for (path, data) in files {
            assert_eq!(fs::read(temp.path().join(path)).unwrap(), data);
        }
    }

    #[test]
    fn lock_unknown_staging_legacy_archives_and_symlinks_are_preserved() {
        let temp = private_tempdir();
        let lock = prepare_storage(&config(temp.path())).unwrap();
        let unknown = temp.path().join(".staging/old-v2");
        fs::create_dir(&unknown).unwrap();
        fs::write(unknown.join("keep"), b"prior data").unwrap();
        assert!(prepare_storage(&config(temp.path())).is_err());
        drop(lock);
        assert!(prepare_storage(&config(temp.path())).is_err());
        assert_eq!(fs::read(unknown.join("keep")).unwrap(), b"prior data");
        fs::remove_dir_all(unknown).unwrap();
        fs::create_dir(temp.path().join("shards/v2")).unwrap();
        assert!(prepare_storage(&config(temp.path())).is_err());
        fs::remove_dir(temp.path().join("shards/v2")).unwrap();
        let outside = private_tempdir();
        let link = temp.path().join(".staging/link.arrow.partial");
        std::os::unix::fs::symlink(outside.path(), &link).unwrap();
        assert!(prepare_storage(&config(temp.path())).is_err());
        assert!(link.is_symlink());
        for entry in ["root", ".staging", "shards", LOCK_FILE] {
            let root = private_tempdir();
            let path = root.path().join(entry);
            std::os::unix::fs::symlink(outside.path(), &path).unwrap();
            assert!(
                prepare_storage(&config(if entry == "root" { &path } else { root.path() }))
                    .is_err()
            );
        }
    }

    #[test]
    fn actor_creates_only_final_private_root_and_private_output() {
        let parent = private_tempdir();
        let root = parent.path().join("capture");
        let mut config = config(&root);
        config.max_bytes = BATCH_RESERVE_BYTES;
        assert!(prepare_storage(&config).is_err());
        assert!(!root.exists());
        config.max_bytes = 128 * 1024 * 1024;
        config.validate().unwrap();
        assert!(!root.exists());
        let (_, receiver) = channel(32);
        let mut actor =
            LocalStorageActor::new(receiver, config, LocalStorageStatus::default()).unwrap();
        actor
            .store
            .add_record(SAIStatsRef {
                observation_time: 42,
                stats: &[stat("a", 7)],
            })
            .unwrap();
        let partial = actor.store.active.as_ref().unwrap().path.clone();
        assert_eq!(fs::metadata(partial).unwrap().mode() & 0o777, 0o600);
        actor.store.finish_stream().unwrap();
        for directory in [root.clone(), root.join(".staging"), root.join("shards")] {
            let metadata = fs::metadata(directory).unwrap();
            assert_eq!(metadata.mode() & 0o777, 0o700);
            assert_eq!(metadata.uid(), unsafe { libc::geteuid() });
        }
        for file in [root.join(LOCK_FILE), paths(&root, "shards")[0].clone()] {
            assert_eq!(fs::metadata(file).unwrap().mode() & 0o777, 0o600);
        }
        let missing_parent = parent.path().join("missing");
        assert!(prepare_storage(&self::config(&missing_parent.join("capture"))).is_err());
        assert!(!missing_parent.exists());
    }

    #[test]
    fn root_access_rejects_foreign_owner_and_all_group_other_permissions_without_chmod() {
        use std::os::unix::fs::PermissionsExt;
        let root = private_tempdir();
        let uid = unsafe { libc::geteuid() };
        assert!(validate_root_access(root.path(), uid ^ 1, 0o700).is_err());
        assert!(validate_root_access(root.path(), uid, 0o700).is_ok());
        fs::write(root.path().join("keep"), b"unchanged").unwrap();
        for mode in [
            0o755, 0o770, 0o707, 0o740, 0o720, 0o710, 0o704, 0o702, 0o701,
        ] {
            fs::set_permissions(root.path(), fs::Permissions::from_mode(mode)).unwrap();
            assert!(prepare_storage(&config(root.path()))
                .unwrap_err()
                .contains("effective service user"));
            assert_eq!(fs::metadata(root.path()).unwrap().mode() & 0o777, mode);
            assert_eq!(fs::read(root.path().join("keep")).unwrap(), b"unchanged");
            assert!(!root.path().join(LOCK_FILE).exists());
        }
        fs::set_permissions(root.path(), fs::Permissions::from_mode(0o700)).unwrap();
        drop(prepare_storage(&config(root.path())).unwrap());
    }

    #[test]
    fn tmpfs_memory_budget_is_fail_closed_and_preserves_emergency_reserve() {
        for invalid in [
            "",
            "MemFree: 999999 kB",
            "MemAvailable:",
            "MemAvailable: nope kB",
            "MemAvailable: -1 kB",
            "MemAvailable: 123 MB",
            "MemAvailable: 123 kB extra",
            "MemAvailable: 18446744073709551615 kB",
        ] {
            assert!(
                tmpfs_available_bytes(u64::MAX, invalid).is_err(),
                "{invalid}"
            );
        }
        let needed = FILESYSTEM_RESERVE_BYTES + BATCH_RESERVE_BYTES;
        for memory in [
            0,
            FILESYSTEM_RESERVE_BYTES,
            needed - 1024,
            needed,
            needed + 1024,
        ] {
            let meminfo = format!("MemTotal: 9999999 kB\nMemAvailable: {} kB\n", memory / 1024);
            let available = tmpfs_available_bytes(u64::MAX, &meminfo).unwrap();
            assert_eq!(available, memory);
            assert_eq!(available >= needed, memory >= needed);
            assert_eq!(
                tmpfs_available_bytes(1024, &meminfo).unwrap(),
                memory.min(1024)
            );
        }
    }

    #[test]
    fn root_symlink_spelling_cannot_bypass_validation() {
        let temp = private_tempdir();
        let target = temp.path().join("target");
        fs::DirBuilder::new().mode(0o700).create(&target).unwrap();
        let link = temp.path().join("link");
        std::os::unix::fs::symlink(&target, &link).unwrap();
        for suffix in ["", "/", "/.", "//./", "/../target", "/missing"] {
            let path = PathBuf::from(format!("{}{suffix}", link.display()));
            for dedicated in [false, true] {
                let mut config = config(&path);
                config.require_dedicated_filesystem = dedicated;
                assert!(prepare_storage(&config)
                    .unwrap_err()
                    .contains("not a symbolic link"));
                assert_eq!(fs::read_dir(&target).unwrap().count(), 0);
            }
        }
        // A symlink in an ancestor is rejected before creating the final root.
        let alias = temp.path().join("alias");
        std::os::unix::fs::symlink(temp.path(), &alias).unwrap();
        assert!(prepare_storage(&config(&alias.join("new-root"))).is_err());
        assert!(!temp.path().join("new-root").exists());
        for suffix in ["", "/", "/."] {
            let path = PathBuf::from(format!("{}{suffix}", target.display()));
            drop(prepare_storage(&config(&path)).unwrap());
            fs::remove_file(target.join(LOCK_FILE)).unwrap();
            fs::remove_dir(target.join(".staging")).unwrap();
            fs::remove_dir(target.join("shards")).unwrap();
        }
    }

    #[test]
    fn storage_children_must_share_root_device() {
        let temp = private_tempdir();
        let root_dev = fs::metadata(temp.path()).unwrap().dev();
        for child in [".staging", "shards"] {
            let path = temp.path().join(child);
            ensure_directory(&path, root_dev).unwrap();
            ensure_directory(&path, root_dev).unwrap();
            assert!(validate_storage_device(root_dev, root_dev, &path).is_ok());
            let error = validate_storage_device(root_dev, root_dev ^ 1, &path).unwrap_err();
            assert!(error.contains("same filesystem"));
            assert!(error.contains(child));
            // Inject a mismatched root device to exercise the metadata check,
            // without requiring privileged mounts or a second host filesystem.
            assert!(ensure_directory(&path, root_dev ^ 1)
                .unwrap_err()
                .contains("same filesystem"));
            assert_eq!(fs::read_dir(&path).unwrap().count(), 0);
        }
    }

    #[test]
    fn quota_stops_append_keeps_durable_prefix_and_never_overwrites_published() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        let stats = [stat("a", u64::MAX)];
        let record = SAIStatsRef {
            observation_time: 42,
            stats: &stats,
        };
        store.add_record(record).unwrap();
        store.finish_stream().unwrap();
        let first = paths(temp.path(), "shards")[0].clone();
        let bytes = fs::read(&first).unwrap();
        store.add_record(record).unwrap();
        store.flush_batch().unwrap();
        store.add_record(record).unwrap();
        store.config.max_bytes = store.used_bytes + BATCH_RESERVE_BYTES - 1;
        assert!(store.flush_batch().is_err());
        assert_eq!(fs::read(&first).unwrap(), bytes);
        drop(store);
        drop(prepare_storage(&config(temp.path())).unwrap());
        let files = paths(temp.path(), "shards");
        assert_eq!(files.len(), 1);
        let partials = paths(temp.path(), ".staging");
        assert_eq!(partials.len(), 1);
        assert_eq!(decoded(&partials[0]).len(), 1);
        let collision = temp.path().join(".staging").join(format!(
            "{}.partial",
            first.file_name().unwrap().to_str().unwrap()
        ));
        fs::write(&collision, &bytes).unwrap();
        assert!(publish(temp.path(), &collision).is_err());
        assert_eq!(fs::read(&first).unwrap(), bytes);
        assert!(collision.exists());
    }

    #[test]
    fn oversized_schema_flushes_previous_valid_rows() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        for value in [u64::MAX, 0, 1 << 63] {
            store
                .add_record(SAIStatsRef {
                    observation_time: value,
                    stats: &[stat("a", value)],
                })
                .unwrap();
        }
        assert!(store
            .add_record(SAIStatsRef {
                observation_time: 0,
                stats: &[stat(&"x".repeat(16 * 1024 * 1024), 0)]
            })
            .is_err());
        let file = &paths(temp.path(), "shards")[0];
        assert_eq!(
            decoded(file)
                .iter()
                .map(|s| (s.observation_time, s.value))
                .collect::<Vec<_>>(),
            vec![(u64::MAX, u64::MAX), (0, 0), (1 << 63, 1 << 63)]
        );
        assert!(store.batch_started.is_none());
    }

    #[test]
    fn actor_drains_disconnected_input_and_preserves_obsolete_sidecars() {
        let temp = private_tempdir();
        let loss = temp.path().join("loss.json");
        fs::write(&loss, b"obsolete diagnostics").unwrap();
        for _ in 0..2 {
            let (sender, receiver) = channel(32);
            let status = LocalStorageStatus::default();
            let actor =
                LocalStorageActor::new(receiver, config(temp.path()), status.clone()).unwrap();
            let mut batch = SAIStatsBatch::default();
            for time in [1, 2, 2, 1] {
                batch.push_record(time, [stat("Ethernet0", u64::MAX)]);
            }
            sender.blocking_send(Arc::new(batch)).unwrap();
            drop(sender);
            actor.run();
            assert!(!status.failed());
            assert_eq!(fs::read(&loss).unwrap(), b"obsolete diagnostics");
            assert!(!temp.path().join(".loss.json.partial").exists());
        }
        assert_eq!(
            paths(temp.path(), "shards")
                .iter()
                .map(|p| decoded(p).len())
                .sum::<usize>(),
            8
        );
    }

    #[test]
    fn shutdown_closes_full_queue_wakes_producer_and_drains_every_accepted_batch() {
        for shutdown_before_run in [true, false] {
            let temp = private_tempdir();
            let (sender, receiver) = channel(32);
            let status = LocalStorageStatus::default();
            let actor =
                LocalStorageActor::new(receiver, config(temp.path()), status.clone()).unwrap();
            let mut batch = SAIStatsBatch::default();
            for time in [u64::MAX, 42, 42, 0] {
                batch.push_record(time, [stat("a", time)]);
            }
            let expected = expected(&batch);
            let batch = Arc::new(batch);
            for _ in 0..32 {
                sender.blocking_send(Arc::clone(&batch)).unwrap();
            }
            let (progress_tx, progress_rx) = mpsc::channel();
            let (producer_tx, producer_rx) = mpsc::channel();
            let producer = thread::spawn(move || {
                let mut accepted = 32;
                while sender.blocking_send(Arc::clone(&batch)).is_ok() {
                    accepted += 1;
                    if accepted == 33 {
                        progress_tx.send(()).unwrap();
                    }
                }
                producer_tx.send(accepted).unwrap();
            });
            assert!(producer_rx.recv_timeout(Duration::from_millis(20)).is_err());
            if shutdown_before_run {
                status.request_shutdown();
            }
            let (done_tx, done_rx) = mpsc::channel();
            let worker = thread::spawn(move || {
                actor.run();
                done_tx.send(()).unwrap();
            });
            if !shutdown_before_run {
                progress_rx.recv_timeout(Duration::from_secs(3)).unwrap();
                status.request_shutdown();
            }
            let accepted = producer_rx.recv_timeout(Duration::from_secs(3)).unwrap();
            done_rx.recv_timeout(Duration::from_secs(3)).unwrap();
            producer.join().unwrap();
            worker.join().unwrap();
            assert!(!status.failed());
            if shutdown_before_run {
                assert_eq!(accepted, 32);
            }
            let samples: Vec<_> = paths(temp.path(), "shards")
                .iter()
                .flat_map(|p| decoded(p))
                .collect();
            assert_eq!(samples.len(), accepted * expected.len());
            for records in samples.chunks_exact(expected.len()) {
                assert_eq!(records, expected);
            }
            assert!(paths(temp.path(), ".staging").is_empty());
            assert!(!temp.path().join("loss.json").exists());
        }
    }

    #[test]
    fn idle_wall_flush_then_shutdown_with_sender_still_connected() {
        let temp = private_tempdir();
        let (sender, receiver) = channel(32);
        let status = LocalStorageStatus::default();
        let actor = LocalStorageActor::new(receiver, config(temp.path()), status.clone()).unwrap();
        let mut batch = SAIStatsBatch::default();
        batch.push_record(0, [stat("a", 42)]);
        sender.blocking_send(Arc::new(batch)).unwrap();
        let (done_tx, done_rx) = mpsc::channel();
        let handle = thread::spawn(move || {
            actor.run();
            done_tx.send(()).unwrap();
        });
        let start = Instant::now();
        loop {
            if paths(temp.path(), ".staging")
                .iter()
                .any(|p| codec::scan(p).is_ok_and(|r| r.records == 1))
            {
                break;
            }
            assert!(start.elapsed() < Duration::from_secs(3));
            thread::sleep(Duration::from_millis(10));
        }
        assert!(paths(temp.path(), "shards").is_empty());
        status.request_shutdown();
        done_rx.recv_timeout(Duration::from_secs(3)).unwrap();
        handle.join().unwrap();
        assert!(!status.failed());
        assert_eq!(decoded(&paths(temp.path(), "shards")[0]).len(), 1);
        drop(sender);
    }

    #[test]
    fn size_rotation_waits_for_complete_batch_and_preserves_records() {
        for schema_exceeds_target in [true, false] {
            let temp = private_tempdir();
            let mut store = store(temp.path());
            store.config.file_target_bytes = if schema_exceeds_target { 1 } else { u64::MAX };
            let stats = [stat(&"a".repeat(4096), u64::MAX)];
            let record = SAIStatsRef {
                observation_time: 0,
                stats: &stats,
            };
            for seq in 0..3 {
                store.add_record(record).unwrap();
                let active = store.active.as_ref().unwrap();
                let schema_bytes = active.writer.get_ref().bytes_written;
                if schema_exceeds_target {
                    assert!(schema_bytes >= store.config.file_target_bytes);
                } else {
                    // Schema is below target; the complete batch must cross it.
                    store.config.file_target_bytes = schema_bytes + 1;
                }
                assert_eq!(schema_bytes, fs::metadata(&active.path).unwrap().len());
                assert_eq!(paths(temp.path(), "shards").len(), seq);
                store.flush_batch().unwrap();
                assert!(store.active.is_none());
                assert!(store.batch_started.is_none());
                let files = paths(temp.path(), "shards");
                assert_eq!(files.len(), seq + 1);
                assert!(fs::metadata(&files[seq]).unwrap().len() > schema_bytes + 8);
                let samples = decoded(&files[seq]);
                assert_eq!(samples.len(), 1);
                assert_eq!(samples[0].observation_time, 0);
                assert_eq!(samples[0].value, u64::MAX);
            }
            store.finish_stream().unwrap();
            assert_eq!(paths(temp.path(), "shards").len(), 3);
            assert!(paths(temp.path(), ".staging").is_empty());
            assert_eq!(store.used_bytes, directory_bytes(temp.path()).unwrap());
        }
    }

    #[test]
    fn size_rotation_uses_compressed_bytes_not_128_mib_raw_input() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        let stats = vec![stat("a", 0); 8000];
        let records = 2100;
        assert!(records * (stats.len() + 1) * 8 > 128 * 1024 * 1024);
        // Reuse one input record; the writer must encode all >128 MiB, not a fake counter.
        for _ in 0..records {
            store
                .add_record(SAIStatsRef {
                    observation_time: 0,
                    stats: &stats,
                })
                .unwrap();
        }
        store.flush_batch().unwrap();
        assert!(paths(temp.path(), "shards").is_empty());
        let active = store.active.as_ref().unwrap();
        let bytes = active.writer.get_ref().bytes_written;
        assert_eq!(bytes, fs::metadata(&active.path).unwrap().len());
        assert!(bytes < store.config.file_target_bytes);
        assert_eq!(store.file_sequence, 1);
        assert!(
            store
                .columns
                .iter()
                .map(|c| c.capacity() * 8)
                .sum::<usize>()
                <= BATCH_TARGET_BYTES
        );
        assert!(store.matrix.capacity() * 8 <= BATCH_TARGET_BYTES);
        store.finish_stream().unwrap();
        let files = paths(temp.path(), "shards");
        assert_eq!(files.len(), 1);
        assert_eq!(fs::metadata(&files[0]).unwrap().len(), bytes + 8);
        let reader = StreamReader::try_new(File::open(&files[0]).unwrap(), None).unwrap();
        let mut read_records = 0;
        for batch in reader {
            let batch = batch.unwrap();
            let timestamps = child(&batch, 0);
            assert!(timestamps.values().iter().all(|&value| value == 0));
            read_records += timestamps.len();
            assert_eq!(child(&batch, 1).len(), stats.len() * timestamps.len());
            assert!(child(&batch, 1).values().iter().all(|&value| value == 0));
        }
        assert_eq!(read_records, records);
    }

    #[test]
    fn max_age_rotation_is_independent_of_source_timestamps() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        for time in [0, u64::MAX, 0] {
            store
                .add_record(SAIStatsRef {
                    observation_time: time,
                    stats: &[],
                })
                .unwrap();
        }
        assert!(paths(temp.path(), "shards").is_empty());
        store.active.as_mut().unwrap().started = Instant::now() - store.config.shard_interval;
        store.tick().unwrap();
        assert_eq!(paths(temp.path(), "shards").len(), 1);
        assert!(store.active.is_none());
        assert_eq!(
            codec::scan(&paths(temp.path(), "shards")[0])
                .unwrap()
                .records,
            3
        );
        store
            .add_record(SAIStatsRef {
                observation_time: 0,
                stats: &[],
            })
            .unwrap();
        assert!(store.active.is_some());
        assert_eq!(paths(temp.path(), "shards").len(), 1);
        store
            .add_record(SAIStatsRef {
                observation_time: 0,
                stats: &[],
            })
            .unwrap();
        store.finish_stream().unwrap();
        assert_eq!(paths(temp.path(), "shards").len(), 2);
        assert_eq!(
            paths(temp.path(), "shards")
                .iter()
                .map(|p| codec::scan(p).unwrap().records)
                .sum::<u64>(),
            5
        );
    }

    #[test]
    fn names_limits_and_setup_status() {
        assert_eq!(
            series_names(1, 0),
            (
                "SAI_OBJECT_TYPE_PORT".into(),
                "SAI_PORT_STAT_IF_IN_OCTETS".into()
            )
        );
        for (id, prefix) in [
            (21, "SAI_QUEUE_STAT_"),
            (24, "SAI_BUFFER_POOL_STAT_"),
            (26, "SAI_INGRESS_PRIORITY_GROUP_STAT_"),
        ] {
            assert!(series_names(id, 0).1.starts_with(prefix));
        }
        assert_eq!(
            series_names(u32::MAX, 7).1,
            "SAI_OBJECT_TYPE_UNKNOWN_4294967295_STAT_UNKNOWN_7"
        );
        let temp = private_tempdir();
        let mut config = config(temp.path());
        config.require_dedicated_filesystem = true;
        assert!(config.prepare_root().is_err());
        config.shard_interval = Duration::ZERO;
        assert!(config.validate().is_err());
        config.shard_interval = Duration::from_secs(1);
        config.file_target_bytes = 0;
        assert!(config.validate().is_err());
        config.file_target_bytes = 1;
        assert!(config.validate().is_ok());
        config.max_bytes = BATCH_RESERVE_BYTES;
        assert!(config.validate().is_err());
        let (sender, receiver) = channel(32);
        let status = LocalStorageStatus::default();
        assert!(LocalStorageActor::new(receiver, config, status.clone()).is_err());
        assert!(status.failed());
        assert!(sender
            .blocking_send(Arc::new(SAIStatsBatch::default()))
            .is_err());
    }

    #[test]
    fn standard_reader_accepts_embedded_schema_without_any_sidecars() {
        let (bytes, _, expected) = small_stream();
        let reader = StreamReader::try_new(Cursor::new(bytes), None).unwrap();
        assert_eq!(reader.schema().metadata()["format_version"], FORMAT_VERSION);
        assert_eq!(
            reader.map(|b| b.unwrap().num_rows()).sum::<usize>(),
            expected.len()
        );
    }

    #[test]
    fn independent_typed_reader_uses_ordered_metadata_and_dynamic_block_lengths() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        let mut expected = SAIStatsBatch::default();
        let mut time = 0u64;
        for rows in [3, 1, 5] {
            for _ in 0..rows {
                let stats = [stat("same|\"\n", time), stat("same|\"\n", u64::MAX - time)];
                expected.push_record(u64::MAX - time, stats.clone());
                store
                    .add_record(SAIStatsRef {
                        observation_time: u64::MAX - time,
                        stats: &stats,
                    })
                    .unwrap();
                time += 1;
            }
            store.flush_batch().unwrap();
        }
        store.finish_stream().unwrap();
        let path = &paths(temp.path(), "shards")[0];
        let reader = StreamReader::try_new(File::open(path).unwrap(), None).unwrap();
        let schema = reader.schema();
        assert_eq!(schema.fields().len(), 2);
        assert_eq!(schema.metadata()["matrix_order"], "series-major");
        let series: serde_json::Value = serde_json::from_str(&schema.metadata()["series"]).unwrap();
        assert_eq!(series.as_array().unwrap().len(), 2);
        assert_eq!(series[0], series[1]);
        assert_eq!(series[0].as_object().unwrap().len(), 3);
        assert_eq!(series[0]["object_name"], "same|\"\n");
        let mut record_index = 0;
        for (batch, rows) in reader.zip([3, 1, 5]) {
            let batch = batch.unwrap();
            assert_eq!(batch.num_rows(), 1);
            let timestamps = child(&batch, 0);
            let values = child(&batch, 1);
            let t = timestamps.len();
            assert_eq!(t, rows);
            assert_eq!(values.len(), series.as_array().unwrap().len() * t);
            for row in 0..t {
                assert_eq!(timestamps.value(row), u64::MAX - record_index);
                assert_eq!(values.value(row), record_index);
                assert_eq!(values.value(t + row), timestamps.value(row));
                record_index += 1;
            }
        }
        assert_eq!(decoded(path), self::expected(&expected));
    }

    #[test]
    fn interrupted_write_preserves_earlier_batches_without_a_writer_queue() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        store
            .add_record(SAIStatsRef {
                observation_time: 42,
                stats: &[stat("a", u64::MAX)],
            })
            .unwrap();
        store.flush_batch().unwrap();
        let layout = &store.layout.as_ref().unwrap().schema;
        let batch = RecordBatch::try_new(
            Arc::clone(layout),
            vec![
                codec::list(UInt64Array::from(vec![0])),
                codec::list(UInt64Array::from(vec![0])),
            ],
        )
        .unwrap();
        let active = store.active.as_mut().unwrap();
        active.writer.get_mut().remaining = 8;
        assert!(active.writer.write(&batch).is_err());
        active.writer.get_mut().flush().unwrap();
        let partial = active.path.clone();
        assert_eq!(codec::scan(&partial).unwrap().records, 1);
        drop(store);
        drop(prepare_storage(&config(temp.path())).unwrap());
        assert!(partial.exists());
        assert!(paths(temp.path(), "shards").is_empty());
        let samples = decoded(&partial);
        assert_eq!(samples.len(), 1);
        assert_eq!(samples[0].value, u64::MAX);
    }

    #[test]
    fn actor_quota_failure_sets_status_and_disconnects_without_touching_archive() {
        let temp = private_tempdir();
        let (sender, receiver) = channel(32);
        let status = LocalStorageStatus::default();
        let mut actor =
            LocalStorageActor::new(receiver, config(temp.path()), status.clone()).unwrap();
        actor.store.config.max_bytes = actor.store.used_bytes + BATCH_RESERVE_BYTES - 1;
        let mut batch = SAIStatsBatch::default();
        batch.push_record(42, [stat("a", 42)]);
        let batch = Arc::new(batch);
        for _ in 0..32 {
            sender.blocking_send(Arc::clone(&batch)).unwrap();
        }
        let (done_tx, done_rx) = mpsc::channel();
        let blocked_sender = sender.clone();
        let producer = thread::spawn(move || {
            while blocked_sender.blocking_send(Arc::clone(&batch)).is_ok() {}
            done_tx.send(()).unwrap();
        });
        assert!(done_rx.recv_timeout(Duration::from_millis(20)).is_err());
        actor.run();
        done_rx.recv_timeout(Duration::from_secs(3)).unwrap();
        producer.join().unwrap();
        assert!(status.failed());
        assert!(sender
            .blocking_send(Arc::new(SAIStatsBatch::default()))
            .is_err());
        assert!(paths(temp.path(), "shards").is_empty());
        assert!(paths(temp.path(), ".staging").is_empty());
    }

    #[test]
    fn storage_child_sigkill_marker() {
        let Some(root) = std::env::var_os("HFT_STORAGE_SIGKILL_CHILD_ROOT") else {
            return;
        };
        let root = PathBuf::from(root);
        let mut store = store(&root);
        for value in [u64::MAX, 0, 1 << 63] {
            store
                .add_record(SAIStatsRef {
                    observation_time: value,
                    stats: &[stat("a", value)],
                })
                .unwrap();
            store.flush_batch().unwrap();
        }
        // An accepted in-memory row is deliberately not promised durable.
        store
            .add_record(SAIStatsRef {
                observation_time: 1,
                stats: &[stat("a", 99)],
            })
            .unwrap();
        fs::write(root.join("child.ready"), b"three synced batches").unwrap();
        loop {
            thread::park();
        }
    }

    #[test]
    fn real_child_sigkill_reader_consumes_synced_prefix_without_repair() {
        let temp = private_tempdir();
        let mut child = Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "actor::local_storage::tests::storage_child_sigkill_marker",
                "--nocapture",
            ])
            .env("HFT_STORAGE_SIGKILL_CHILD_ROOT", temp.path())
            .stdout(Stdio::null())
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(15);
        while !temp.path().join("child.ready").exists() && Instant::now() < deadline {
            if child.try_wait().unwrap().is_some() {
                panic!("storage child exited before marker");
            }
            thread::sleep(Duration::from_millis(10));
        }
        let ready = temp.path().join("child.ready").exists();
        child.kill().unwrap();
        let exit = child
            .wait_timeout(Duration::from_secs(5))
            .unwrap()
            .expect("child did not exit");
        assert!(ready, "child marker timed out");
        use std::os::unix::process::ExitStatusExt;
        assert_eq!(exit.signal(), Some(libc::SIGKILL));
        assert!(paths(temp.path(), "shards").is_empty());
        let files = paths(temp.path(), ".staging");
        assert_eq!(files.len(), 1);
        let bytes = fs::read(&files[0]).unwrap();
        assert_eq!(
            decoded(&files[0])
                .iter()
                .map(|s| s.value)
                .collect::<Vec<_>>(),
            vec![u64::MAX, 0, 1 << 63]
        );
        assert_eq!(fs::read(&files[0]).unwrap(), bytes);
        drop(prepare_storage(&config(temp.path())).unwrap());
        assert_eq!(fs::read(&files[0]).unwrap(), bytes);
        assert_eq!(paths(temp.path(), ".staging"), files);
        assert!(paths(temp.path(), "shards").is_empty());
    }
}
