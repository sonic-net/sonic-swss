//! Bounded capture of exact raw observations in Arrow IPC streams.
//!
//! Supported SONiC targets are amd64 and arm64. Local storage is unavailable on
//! armhf (32-bit ARM): upstream Arrow IPC compression uses a platform-sized
//! length prefix that does not produce valid compressed IPC on 32-bit targets.
//!
//! # File format and interpretation
//!
//! Each file is a self-contained **Arrow IPC stream**, compressed with ZSTD, not
//! an Arrow IPC file/Feather file or Parquet file. Schema metadata contains:
//! - `timestamp_unit = ns` (the source observation time, not writer wall time)
//! - `matrix_order = series-major`
//! - `layouts`: a JSON array of layouts. A layout is the ordered JSON array of
//!   `{object_name, type_name, stat_name}` series of one record shape, normally
//!   one IPFIX template: a MIXED-mode session has one template for all of its
//!   groups, otherwise each group of each session has its own.
//!
//! Known identities use full SAI names, e.g. `Ethernet0`, `SAI_OBJECT_TYPE_PORT`,
//! `SAI_PORT_STAT_IF_IN_OCTETS`; unknown IDs use explicit numeric-suffix names.
//! Names are stored once per file, not per sample. Array position identifies the
//! series, including duplicate identity tuples: do not sort or deduplicate it.
//! Object type is not a counter/gauge classification. Units, clock epoch and
//! counter/reset semantics are source-specific, not inferred by this format.
//!
//! Each record batch row is one **block** of consecutive records of one layout,
//! in three non-null fields:
//! - `layout`: UInt32 index into `layouts`.
//! - `timestamps_ns`: `List<UInt64>` of T observation timestamps, shared by the
//!   layout's C series.
//! - `values`: `List<UInt64>` of C*T raw values, series c / observation t at
//!   `c*T + t`.
//!
//! A batch has one block per layout that received records since the previous
//! batch, and T varies between blocks. For example, with two templates:
//! ```text
//! layouts = [[Ethernet0/IF_IN_OCTETS, Ethernet8/IF_IN_OCTETS],
//!            [Ethernet0|3/SAI_QUEUE_STAT_PACKETS]]
//! row 0: layout = 0, timestamps_ns = [1000000, 1100000], values = [10, 12, 40, 45]
//!                                                                   Ethernet0  Ethernet8
//! row 1: layout = 1, timestamps_ns = [1050000],          values = [7]
//! ```
//! Thus Ethernet8 has value 45 at 1100000 ns. Empty input records form blocks of
//! a layout with C=0 and an empty values list. Blocks of one layout keep input
//! order across batches and files; blocks of different layouts are not ordered
//! by arrival, so correlate layouts by timestamp. No stored record sequence,
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
//!     assert meta[b"timestamp_unit"] == b"ns"
//!     assert meta[b"matrix_order"] == b"series-major"
//!     layouts = json.loads(meta[b"layouts"].decode("utf-8"))
//!     for batch in reader:
//!         for row in range(batch.num_rows):
//!             series = layouts[batch.column("layout")[row].as_py()]
//!             times = batch.column("timestamps_ns")[row].values
//!             values = batch.column("values")[row].values
//!             assert len(values) == len(series) * len(times)
//!             for c, identity in enumerate(series):
//!                 samples = values.slice(c * len(times), len(times))
//!                 # Process identity, times and samples here, one block at a time.
//! ```
//! Keep timestamps and values as unsigned integers/Python ints, not floats or
//! JavaScript Numbers. Compute differences with signed/widened arithmetic to
//! handle timestamp regressions and counter resets without unsigned underflow.
//! Do not reconstruct timestamps from a nominal polling interval.
//!
//! # Publication, limits and lifetime
//!
//! One blocking storage worker consumes a bounded generic IPFIX recipient with
//! backpressure. Records are buffered per layout. A batch is written when the
//! buffered records reach 16 MiB of raw data or a layout reaches 4096 records,
//! and at most 100 ms after the oldest buffered record arrived; encoding and I/O
//! can delay this. Each batch is flushed and fsynced. Shutdown, maximum age, the
//! encoded-byte target, or a layout missing from the stream's schema closes the
//! stream, writes EOS, fsyncs and atomically moves
//! `.staging/<time_ns>-<pid>-<file_sequence>.arrow.partial` to `shards/*.arrow`.
//! A new stream lists the layouts with buffered records and those that wrote to
//! the previous stream, so a layout that stops receiving records is listed by at
//! most one more file. The default target is 100,000,000 bytes / 30 minutes,
//! checked after complete batches. Files can exceed the size target by a batch;
//! earlier closure yields smaller files. Within one known writer run, order
//! files by numeric file_sequence, not wall time (which can regress). Filenames
//! alone do not identify runs globally.
//!
//! Writers never repair abandoned partials. After the writer stops, `read_shard`
//! can return the completely decoded prefix of an interrupted stream; an invalid
//! suffix is not returned and operational I/O errors propagate. Standard Arrow
//! readers may instead report a truncated-tail error. Neither proves that the
//! source delivered every observation, and unflushed memory can be lost.
//!
//! Capture is opt-in. The default `/tmp/hft` is created privately (0700), with
//! service-owned files (0600), a single-writer lock, symlink/device checks and a
//! 128 MiB quota including directory/staging allocation. A schema or batch is
//! written only if its worst-case encoding (its raw bytes plus IPC framing) and
//! room for the end-of-stream marker fit the quota. When one does not fit,
//! capture stops: the stream is published and the records that did not fit are
//! discarded. Storage errors also close this recipient; backpressure and
//! upstream receive loss remain possible. There is no automatic upload,
//! eviction or retention management.
//!
//! On tmpfs, admission also checks host MemAvailable with a 512 MiB emergency
//! reserve; this does not cover cgroup limits or guarantee against concurrent OOM.
//! `/tmp` may disappear on reboot/cleanup and differs across host/container
//! namespaces. For longer captures, configure a suitable root and quota. Stop
//! capture and verify transfer before deleting the capture directory.

use crate::{
    message::{
        local_storage::LocalStorageStatus,
        saistats::{SAIStatMetadata, SAIStatsBatchMessage, SAIStatsRef, SAIStatsView},
    },
    utilities::{ChannelLabel, record_comm_stats, set_comm_capacity},
};
use ahash::{HashMap, HashMapExt};
use arrow_array::{RecordBatch, UInt32Array, UInt64Array};
use arrow_ipc::{
    CompressionType,
    writer::{IpcWriteOptions, StreamWriter},
};
use arrow_schema::{ArrowError, SchemaRef};
use log::{error, info, warn};
use std::{
    ffi::CString,
    fmt::Display,
    fs::{self, File, OpenOptions},
    io::{self, BufWriter, Write},
    mem::MaybeUninit,
    os::unix::{
        ffi::OsStrExt,
        fs::{DirBuilderExt, MetadataExt, OpenOptionsExt},
        io::AsRawFd,
    },
    path::{Path, PathBuf},
    sync::Arc,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
use tokio::{
    sync::mpsc::{Receiver, Sender},
    task::JoinHandle,
};

mod codec;
#[allow(unused_imports)]
pub use codec::{DecodedSample, read_shard};

const LOCK_FILE: &str = ".writer.lock";
/// Buffered raw bytes (timestamps and values) that trigger a batch.
const BATCH_TARGET_BYTES: usize = 16 * 1024 * 1024;
/// Records per block.
const MAX_ROWS: usize = 4096;
/// Longest time a buffered record waits for its batch.
const FLUSH_INTERVAL: Duration = Duration::from_millis(100);
const MAX_RECORD_BYTES: usize = 128 * 1024 * 1024;
// One record fits a batch. Keep reader limits independent of writer bounds.
const _: () = assert!((codec::MAX_COUNTERS + 1) * 8 <= BATCH_TARGET_BYTES);
/// Smallest quota: one full batch and the largest schema.
pub const MIN_MAX_BYTES: u64 = 2 * BATCH_TARGET_BYTES as u64;
const _: () = assert!(codec::MAX_SCHEMA_BYTES as u64 <= BATCH_TARGET_BYTES as u64);
/// IPC message metadata and length prefixes beyond the raw buffer bytes.
const IPC_FRAMING_BYTES: u64 = 64 * 1024;
/// Kept free with every write, so the end-of-stream marker always fits.
const EOS_RESERVE_BYTES: u64 = 64 * 1024;
const FILESYSTEM_RESERVE_BYTES: u64 = 512 * 1024 * 1024;

/// Name the failed operation and path in errors that are only logged.
trait Context<T> {
    fn context(self, operation: &str, path: &Path) -> Result<T, String>;
}

impl<T, E: Display> Context<T> for Result<T, E> {
    fn context(self, operation: &str, path: &Path) -> Result<T, String> {
        self.map_err(|e| format!("{operation} {}: {e}", path.display()))
    }
}

/// Command-line options of the local storage sink.
#[derive(Debug, Clone, clap::Args)]
pub struct LocalStorageArgs {
    /// Enable bounded raw UInt64 Arrow IPC streams with backpressure (100 ms batch flush)
    #[arg(long, default_value = "false")]
    pub enable_local_storage: bool,

    /// Private capture directory; create the final directory when capture is enabled
    #[arg(long, default_value = "/tmp/hft")]
    pub local_storage_root: PathBuf,

    /// Local storage quota in bytes, including directories; at least 32 MiB
    #[arg(
        long,
        default_value = "134217728",
        value_parser = clap::value_parser!(u64).range(MIN_MAX_BYTES..)
    )]
    pub local_storage_max_bytes: u64,

    /// Compressed IPC file byte target; rotate after a complete batch flush
    #[arg(long, default_value = "100000000", value_parser = clap::value_parser!(u64).range(1..))]
    pub local_storage_file_bytes: u64,

    /// Maximum local storage file age in wall-clock seconds
    #[arg(long, default_value = "1800", value_parser = clap::value_parser!(u64).range(1..))]
    pub local_storage_file_seconds: u64,

    /// SAI stats batches IPFIX can queue for local storage; values above 64 are
    /// clamped to 64
    #[arg(
        long,
        default_value = "32",
        value_parser = clap::builder::RangedU64ValueParser::<usize>::new().range(1..)
    )]
    pub local_storage_capacity: usize,
}

impl LocalStorageArgs {
    pub fn config(&self) -> LocalStorageConfig {
        LocalStorageConfig {
            root: self.local_storage_root.clone(),
            shard_interval: Duration::from_secs(self.local_storage_file_seconds),
            file_target_bytes: self.local_storage_file_bytes,
            max_bytes: self.local_storage_max_bytes,
        }
    }
}

/// The local storage sink before its worker starts. Its queue exists first,
/// so IPFIX can hold the only sender from startup.
pub struct LocalStorage {
    receiver: Receiver<SAIStatsBatchMessage>,
    config: LocalStorageConfig,
    status: LocalStorageStatus,
}

impl LocalStorage {
    /// The sink and the sender for its IPFIX recipient queue of `capacity` batches.
    pub fn new(
        config: LocalStorageConfig,
        capacity: usize,
    ) -> (Self, Sender<SAIStatsBatchMessage>) {
        let (sender, receiver) = tokio::sync::mpsc::channel(capacity);
        set_comm_capacity(ChannelLabel::IpfixToLocalStorage, capacity);
        info!(
            "Local storage requested: path={}, compression=zstd, max_bytes={}, file_target_bytes={}, shard_interval_secs={}, flush_interval_ms={}, queue_batches={capacity}; capture applies backpressure",
            config.root.display(),
            config.max_bytes,
            config.file_target_bytes,
            config.shard_interval.as_secs(),
            FLUSH_INTERVAL.as_millis(),
        );
        let status = LocalStorageStatus::default();
        (
            Self {
                receiver,
                config,
                status,
            },
            sender,
        )
    }

    /// Set up storage and run the worker on a blocking thread. Until setup
    /// finishes, the bounded queue applies backpressure; a setup error closes
    /// it, and IPFIX continues without this sink.
    pub fn spawn(self) -> LocalStorageHandle {
        let status = self.status.clone();
        let task = tokio::task::spawn_blocking(move || {
            match LocalStorageActor::new(self.receiver, self.config, self.status) {
                Ok(actor) => actor.run(),
                Err(reason) => {
                    error!("Local storage setup failed; continuing without local output: {reason}")
                }
            }
        });
        LocalStorageHandle { status, task }
    }
}

pub struct LocalStorageHandle {
    status: LocalStorageStatus,
    task: JoinHandle<()>,
}

impl LocalStorageHandle {
    /// Drain the accepted batches and publish, waiting at most `timeout`. Stop
    /// the sender's owner first, so that the queue closes.
    pub async fn shutdown(self, timeout: Duration) {
        self.status.request_shutdown();
        match tokio::time::timeout(timeout, self.task).await {
            Ok(Ok(())) if self.status.failed() => {
                error!("Local storage stopped with a failure; drain is incomplete")
            }
            Ok(Ok(())) => info!("Local storage flushed before shutdown"),
            Ok(Err(reason)) => error!("Local storage task failed: {reason}"),
            Err(_) => error!("Timed out draining local storage; final data may be incomplete"),
        }
    }
}

#[derive(Debug, Clone)]
pub struct LocalStorageConfig {
    pub root: PathBuf,
    /// Maximum wall-clock stream age; batch flushes are due after 100 ms.
    /// Compression and filesystem sync time can extend the durability window.
    pub shard_interval: Duration,
    /// Logical compressed IPC bytes, checked after each complete batch flush.
    pub file_target_bytes: u64,
    pub max_bytes: u64,
}

impl LocalStorageConfig {
    pub fn validate(&self) -> Result<(), String> {
        if self.shard_interval.is_zero() {
            return Err("local storage shard interval must be greater than zero".into());
        }
        if self.file_target_bytes == 0 {
            return Err("local storage file target bytes must be greater than zero".into());
        }
        if self.max_bytes < MIN_MAX_BYTES {
            return Err(format!(
                "local storage max bytes must be at least {MIN_MAX_BYTES}"
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
                        .context("create", &root)?;
                    fs::symlink_metadata(&root).context("inspect", &root)?
                }
                Err(e) => return Err(e).context("inspect", &root),
            };
            if metadata.file_type().is_symlink() || !metadata.is_dir() {
                return Err(format!(
                    "local storage path {} must be a directory, not a symbolic link",
                    root.display()
                ));
            }
        }
        let metadata = fs::symlink_metadata(&root).context("inspect", &root)?;
        validate_root_access(&root, metadata.uid(), metadata.mode())?;
        Ok(())
    }
}

// Cap each schema/batch write at its admitted size before it uses more disk. The
// BufWriter sits inside the cap so its buffered bytes are charged immediately.
struct CappedFile {
    file: BufWriter<File>,
    remaining: u64,
    bytes_written: u64,
}

impl Write for CappedFile {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        if bytes.len() as u64 > self.remaining {
            return Err(io::Error::other("IPC write exceeds its admitted size"));
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

impl CappedFile {
    /// Hand buffered bytes to the kernel and fsync the file.
    fn sync(&mut self) -> io::Result<()> {
        self.file.flush()?;
        self.file.get_ref().sync_all()
    }

    fn allocated_bytes(&self) -> io::Result<u64> {
        Ok(self.file.get_ref().metadata()?.blocks().saturating_mul(512))
    }
}

struct ActiveStream {
    writer: StreamWriter<CappedFile>,
    path: PathBuf,
    started: Instant,
    schema: SchemaRef,
}

impl ActiveStream {
    /// Write under a byte cap, then fsync. Returns the newly allocated bytes.
    fn write_synced(
        &mut self,
        cap: u64,
        write: impl FnOnce(&mut StreamWriter<CappedFile>) -> Result<(), ArrowError>,
    ) -> Result<u64, String> {
        let before = self
            .writer
            .get_ref()
            .allocated_bytes()
            .context("inspect", &self.path)?;
        self.writer.get_mut().remaining = cap;
        write(&mut self.writer).context("write", &self.path)?;
        self.writer.get_mut().sync().context("sync", &self.path)?;
        let after = self
            .writer
            .get_ref()
            .allocated_bytes()
            .context("inspect", &self.path)?;
        Ok(after.saturating_sub(before))
    }
}

/// Buffered records of one layout, normally one IPFIX template.
struct Lane {
    layout: codec::Layout,
    /// Template metadata of the latest shared record, matched by address.
    /// Holding it keeps another template from reusing that address.
    template: Option<Arc<[SAIStatMetadata]>>,
    /// Pending records, row-major: the timestamp, then one value per series.
    rows: Vec<u64>,
    /// Position in the active stream's `layouts`.
    index: Option<u32>,
    /// Stream generation that last received this lane's records.
    written: Option<u64>,
}

impl Lane {
    fn stride(&self) -> usize {
        self.layout.width() + 1
    }

    fn holds(&self, stats: SAIStatsView<'_>) -> bool {
        match stats {
            SAIStatsView::Shared { metadata, .. } => self
                .template
                .as_ref()
                .is_some_and(|template| Arc::ptr_eq(template, metadata)),
            SAIStatsView::Owned(_) => self.layout.matches(stats),
        }
    }
}

fn template_key(metadata: &Arc<[SAIStatMetadata]>) -> usize {
    Arc::as_ptr(metadata).cast::<u8>() as usize
}

struct Store {
    config: LocalStorageConfig,
    lanes: Vec<Lane>,
    /// Lane index by template address, for records of many interleaved templates.
    templates: HashMap<usize, usize>,
    /// Lane of the previous record; consecutive records usually share it.
    last_lane: usize,
    /// Raw bytes of the pending records of all lanes.
    pending_bytes: usize,
    /// Arrival of the oldest pending record.
    pending_since: Option<Instant>,
    active: Option<ActiveStream>,
    /// A lane created after the active stream opened is missing from its schema.
    schema_stale: bool,
    /// Streams opened so far.
    generation: u64,
    /// Capture stopped at the quota or the filesystem reserve.
    stopped: bool,
    // Reusable batch columns, at most one batch of pending records.
    layout_ids: Vec<u32>,
    timestamps: Vec<u64>,
    values: Vec<u64>,
    file_sequence: u64,
    // Includes all files and directory blocks, including active staging data.
    used_bytes: u64,
    directory_blocks: u64,
    _storage_lock: File,
}

impl Store {
    fn new(config: LocalStorageConfig, lock: File) -> Result<Self, String> {
        let used_bytes = directory_bytes(&config.root).context("measure", &config.root)?;
        let directory_blocks = directory_overhead(&config.root)?;
        Ok(Self {
            config,
            lanes: Vec::new(),
            templates: HashMap::new(),
            last_lane: 0,
            pending_bytes: 0,
            pending_since: None,
            active: None,
            schema_stale: false,
            generation: 0,
            stopped: false,
            layout_ids: Vec::new(),
            timestamps: Vec::new(),
            values: Vec::new(),
            file_sequence: 0,
            used_bytes,
            directory_blocks,
            _storage_lock: lock,
        })
    }

    /// Admit a write of at most `bytes`. Returns why a write does not fit.
    fn admit(&mut self, bytes: u64) -> Result<Option<String>, String> {
        // Only directory blocks can change independently of our writes under the
        // service-owned root. Refresh them without rescanning finalized files.
        self.refresh_directory_blocks()?;
        let bytes = bytes.saturating_add(EOS_RESERVE_BYTES);
        if self.used_bytes.saturating_add(bytes) > self.config.max_bytes {
            return Ok(Some(format!(
                "quota reached: {} allocated + {bytes} for the next write > {}",
                self.used_bytes, self.config.max_bytes
            )));
        }
        if available_bytes(&self.config.root)? < bytes.saturating_add(FILESYSTEM_RESERVE_BYTES) {
            return Ok(Some(
                "filesystem or tmpfs host-memory emergency reserve reached".into(),
            ));
        }
        Ok(None)
    }

    /// Stop capturing: publish what was written and discard what does not fit.
    fn stop(&mut self, reason: String) -> Result<(), String> {
        let discarded: usize = self
            .lanes
            .iter()
            .map(|lane| lane.rows.len() / lane.stride())
            .sum();
        for lane in &mut self.lanes {
            lane.rows = Vec::new();
        }
        self.pending_bytes = 0;
        self.pending_since = None;
        self.stopped = true;
        self.publish_stream()?;
        warn!("Local storage stopped, {reason}; discarded {discarded} pending records");
        Ok(())
    }

    /// Open a stream that lists every lane with pending records or records in
    /// the previous stream. Other lanes are retired.
    fn open_stream(&mut self) -> Result<(), String> {
        let previous = self.generation;
        self.lanes
            .retain(|lane| !lane.rows.is_empty() || lane.written == Some(previous));
        self.templates.clear();
        self.last_lane = 0;
        let mut layouts = String::from("[");
        for (index, lane) in self.lanes.iter_mut().enumerate() {
            if index > 0 {
                layouts.push(',');
            }
            layouts.push_str(&lane.layout.json);
            lane.index = Some(index as u32);
            if let Some(template) = &lane.template {
                self.templates.insert(template_key(template), index);
            }
        }
        layouts.push(']');
        let bound = layouts.len() as u64 + IPC_FRAMING_BYTES;
        if let Some(reason) = self.admit(bound)? {
            return self.stop(reason);
        }
        let schema = codec::schema(layouts);
        let options = IpcWriteOptions::default()
            .try_with_compression(Some(CompressionType::ZSTD))
            .map_err(|e| e.to_string())?;
        let shards = self.config.root.join("shards");
        let staging = self.config.root.join(".staging");
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
            let published = shards.join(&name);
            if published.try_exists().context("inspect", &published)? {
                continue;
            }
            let path = staging.join(format!("{name}.partial"));
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
                Err(e) => return Err(e).context("create", &path),
            }
        };
        let mut writer = StreamWriter::try_new_with_options(
            CappedFile {
                file: BufWriter::with_capacity(256 * 1024, file),
                remaining: bound,
                bytes_written: 0,
            },
            &schema,
            options,
        )
        .context("write schema to", &path)?;
        writer.get_mut().sync().context("sync", &path)?;
        let allocated = writer
            .get_ref()
            .allocated_bytes()
            .context("inspect", &path)?;
        self.used_bytes = self.used_bytes.saturating_add(allocated);
        self.generation += 1;
        self.schema_stale = false;
        self.active = Some(ActiveStream {
            writer,
            path,
            started: Instant::now(),
            schema,
        });
        self.refresh_directory_blocks()?;
        sync_directory(&staging).context("sync", &staging)?;
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
        if self.stopped {
            return Ok(());
        }
        let row_bytes = record.stats.len().saturating_add(1).saturating_mul(8);
        if self.pending_bytes.saturating_add(row_bytes) > BATCH_TARGET_BYTES {
            self.flush()?;
            if self.stopped {
                return Ok(());
            }
        }
        let index = match self.lane_for(record.stats) {
            Ok(index) => index,
            Err(reason) => {
                // Persist the accepted records before rejecting this layout.
                self.finish_stream()?;
                return Err(reason);
            }
        };
        let lane = &mut self.lanes[index];
        let stride = lane.stride();
        if lane.rows.capacity() - lane.rows.len() < stride {
            // Grow geometrically, but never past one batch of this layout.
            let batch = (BATCH_TARGET_BYTES / (stride * 8)).clamp(1, MAX_ROWS) * stride;
            let wanted = (lane.rows.capacity() * 2)
                .min(batch)
                .max(lane.rows.len() + stride);
            lane.rows.reserve_exact(wanted - lane.rows.len());
        }
        lane.rows.push(record.observation_time);
        match record.stats {
            SAIStatsView::Shared { values, .. } => lane.rows.extend_from_slice(values),
            SAIStatsView::Owned(stats) => lane.rows.extend(stats.iter().map(|stat| stat.counter)),
        }
        let full = lane.rows.len() / stride >= MAX_ROWS;
        self.pending_bytes += row_bytes;
        self.pending_since.get_or_insert_with(Instant::now);
        // Write a full batch now rather than when the next record arrives.
        if full || self.pending_bytes + row_bytes > BATCH_TARGET_BYTES {
            self.flush()?;
        }
        Ok(())
    }

    /// The lane for a record's layout: by template address for shared IPFIX
    /// records, else by identities (also for a reinstalled template), else new.
    fn lane_for(&mut self, stats: SAIStatsView<'_>) -> Result<usize, String> {
        if self
            .lanes
            .get(self.last_lane)
            .is_some_and(|lane| lane.holds(stats))
        {
            return Ok(self.last_lane);
        }
        let template = match stats {
            SAIStatsView::Shared { metadata, .. } => Some(metadata),
            SAIStatsView::Owned(_) => None,
        };
        let found = template
            .and_then(|template| self.templates.get(&template_key(template)).copied())
            .or_else(|| {
                self.lanes
                    .iter()
                    .position(|lane| lane.layout.matches(stats))
            });
        let index = match found {
            Some(index) => index,
            None => self.add_lane(stats)?,
        };
        if let Some(template) = template {
            let lane = &mut self.lanes[index];
            if !lane.holds(stats) {
                if let Some(previous) = lane.template.replace(Arc::clone(template)) {
                    self.templates.remove(&template_key(&previous));
                }
                self.templates.insert(template_key(template), index);
            }
        }
        self.last_lane = index;
        Ok(index)
    }

    fn add_lane(&mut self, stats: SAIStatsView<'_>) -> Result<usize, String> {
        let layout = codec::Layout::new(stats)?;
        // The next stream lists the new lane and every lane its rotation keeps.
        let (count, bytes) = self
            .lanes
            .iter()
            .filter(|lane| !lane.rows.is_empty() || lane.written == Some(self.generation))
            .fold((1, layout.json.len() + 2), |(count, bytes), lane| {
                (count + 1, bytes + lane.layout.json.len() + 1)
            });
        if count > codec::MAX_LAYOUTS || bytes > codec::MAX_SCHEMA_BYTES {
            return Err(format!(
                "{count} concurrent layouts exceed the stream schema limits"
            ));
        }
        self.schema_stale |= self.active.is_some();
        self.lanes.push(Lane {
            layout,
            template: None,
            rows: Vec::new(),
            index: None,
            written: None,
        });
        Ok(self.lanes.len() - 1)
    }

    /// Write every lane's pending records as one batch, a block per layout.
    fn flush(&mut self) -> Result<(), String> {
        if self.pending_since.is_none() {
            return Ok(());
        }
        if self.schema_stale {
            self.publish_stream()?;
        }
        if self.active.is_none() {
            self.open_stream()?;
            if self.stopped {
                return Ok(());
            }
        }
        let (blocks, rows, values) = self.lanes.iter().filter(|lane| !lane.rows.is_empty()).fold(
            (0, 0, 0),
            |(blocks, rows, values), lane| {
                let records = lane.rows.len() / lane.stride();
                let series = records * lane.layout.width();
                (blocks + 1, rows + records, values + series)
            },
        );
        let bound = batch_bound(blocks, rows, values);
        if let Some(reason) = self.admit(bound)? {
            return self.stop(reason);
        }
        // Size the reused columns exactly; doubling could exceed one batch.
        // Blocks overwrite values in place, so only growth is zero-filled.
        self.layout_ids.clear();
        self.layout_ids.reserve_exact(blocks);
        self.timestamps.clear();
        self.timestamps.reserve_exact(rows);
        self.values
            .reserve_exact(values.saturating_sub(self.values.len()));
        self.values.resize(values, 0);
        let mut offset = 0;
        let mut time_offsets = Vec::with_capacity(blocks + 1);
        let mut value_offsets = Vec::with_capacity(blocks + 1);
        time_offsets.push(0);
        value_offsets.push(0);
        for lane in &mut self.lanes {
            if lane.rows.is_empty() {
                continue;
            }
            let stride = lane.stride();
            let index = lane.index.ok_or("layout missing from the stream schema")?;
            self.layout_ids.push(index);
            self.timestamps.extend(lane.rows.iter().step_by(stride));
            let block = lane.rows.len() / stride * (stride - 1);
            transpose(&lane.rows, stride, &mut self.values[offset..offset + block]);
            offset += block;
            time_offsets.push(list_offset(self.timestamps.len())?);
            value_offsets.push(list_offset(offset)?);
            // Keep capacity for similar batches, but release a past burst.
            let used = lane.rows.len();
            lane.rows.clear();
            if lane.rows.capacity() > 4 * used {
                lane.rows.shrink_to(2 * used);
            }
            lane.written = Some(self.generation);
        }
        let layout_ids = UInt32Array::from(std::mem::take(&mut self.layout_ids));
        let timestamps = UInt64Array::from(std::mem::take(&mut self.timestamps));
        let values = UInt64Array::from(std::mem::take(&mut self.values));
        let active = self.active.as_mut().ok_or("missing batch stream")?;
        let batch = RecordBatch::try_new(
            Arc::clone(&active.schema),
            vec![
                Arc::new(layout_ids.clone()),
                codec::list(timestamps.clone(), time_offsets),
                codec::list(values.clone(), value_offsets),
            ],
        )
        .map_err(|e| e.to_string())?;
        let allocated = active.write_synced(bound, |writer| writer.write(&batch))?;
        let rotate = active.writer.get_ref().bytes_written >= self.config.file_target_bytes;
        self.used_bytes = self.used_bytes.saturating_add(allocated);
        drop(batch);
        self.layout_ids = codec::reclaim(layout_ids);
        self.timestamps = codec::reclaim(timestamps);
        self.values = codec::reclaim(values);
        self.pending_bytes = 0;
        self.pending_since = None;
        // Include schema/framing, but always persist a batch before size rotation.
        if rotate {
            self.publish_stream()?;
        }
        Ok(())
    }

    fn finish_stream(&mut self) -> Result<(), String> {
        self.flush()?;
        self.publish_stream()
    }

    fn publish_stream(&mut self) -> Result<(), String> {
        if let Some(mut active) = self.active.take() {
            // The end-of-stream marker is 8 bytes.
            let allocated = active.write_synced(8, StreamWriter::finish)?;
            publish(&self.config.root, &active.path)?;
            self.used_bytes = self.used_bytes.saturating_add(allocated);
            self.refresh_directory_blocks()?;
        }
        Ok(())
    }

    /// When `poll` next has work: a due batch or the stream's maximum age.
    fn next_deadline(&self) -> Option<Instant> {
        let flush = self
            .pending_since
            .and_then(|since| since.checked_add(FLUSH_INTERVAL));
        let rotate = self
            .active
            .as_ref()
            .and_then(|active| active.started.checked_add(self.config.shard_interval));
        match (flush, rotate) {
            (Some(flush), Some(rotate)) => Some(flush.min(rotate)),
            (flush, rotate) => flush.or(rotate),
        }
    }

    fn poll(&mut self, now: Instant) -> Result<(), String> {
        let interval = self.config.shard_interval;
        if self
            .active
            .as_ref()
            .is_some_and(|active| now.saturating_duration_since(active.started) >= interval)
        {
            if self.schema_stale && self.pending_since.is_some() {
                // Adding the missing layout starts a new stream anyway.
                return self.flush();
            }
            return self.finish_stream();
        }
        if self
            .pending_since
            .is_some_and(|since| now.saturating_duration_since(since) >= FLUSH_INTERVAL)
        {
            self.flush()?;
        }
        Ok(())
    }
}

/// Copy row-major records (timestamp, then one value per series) into a
/// series-major block. Wide records are copied in tiles of 8 records, whose
/// rows stay in L1 while every series is visited; narrow ones in one tile.
fn transpose(rows: &[u64], stride: usize, block: &mut [u64]) {
    let records = rows.len() / stride;
    let tile = if stride > 16 { 8 } else { records.max(1) };
    for first in (0..records).step_by(tile) {
        let last = (first + tile).min(records);
        for series in 1..stride {
            let values = &mut block[(series - 1) * records..][first..last];
            for (record, value) in (first..last).zip(values) {
                *value = rows[record * stride + series];
            }
        }
    }
}

/// Upper bound of a batch's IPC encoding. Arrow keeps a buffer uncompressed
/// when compression would not shrink it, adding an 8-byte length prefix and up
/// to 7 bytes of padding to each of the 10 buffers (layout ids, list offsets,
/// values and their validity bitmaps).
fn batch_bound(blocks: usize, rows: usize, values: usize) -> u64 {
    let validity = |len: usize| len.div_ceil(8);
    let raw = 3 * validity(blocks)
        + 4 * blocks
        + 8 * (blocks + 1)
        + validity(rows)
        + 8 * rows
        + validity(values)
        + 8 * values;
    raw as u64 + 10 * 15 + IPC_FRAMING_BYTES
}

fn list_offset(len: usize) -> Result<i32, String> {
    i32::try_from(len).map_err(|_| "batch exceeds list offset range".into())
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
                    loop {
                        if self.status.shutdown_requested() {
                            self.receiver.close();
                        }
                        // Wake for the next batch or rotation, and at least every
                        // FLUSH_INTERVAL to observe a shutdown request.
                        let now = Instant::now();
                        let wake = self
                            .store
                            .next_deadline()
                            .map_or(now + FLUSH_INTERVAL, |deadline| {
                                deadline.min(now + FLUSH_INTERVAL)
                            });
                        tokio::select! {
                            biased;
                            _ = tokio::time::sleep_until(wake.into()) => {
                                self.store.poll(Instant::now())?;
                            }
                            message = self.receiver.recv() => {
                                let Some(message) = message else { break; };
                                record_comm_stats(
                                    ChannelLabel::IpfixToLocalStorage,
                                    self.receiver.len(),
                                );
                                for record in message.iter() {
                                    self.store.add_record(record)?;
                                }
                            }
                        }
                        if self.store.stopped {
                            break;
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
    // SAFETY: geteuid has no preconditions and cannot fail.
    if owner != unsafe { libc::geteuid() } || mode & 0o077 != 0 {
        return Err(format!(
            "local storage root {} must be owned by the effective service user with no group/other permissions",
            path.display()
        ));
    }
    Ok(())
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
                .context("create", path)?;
            let metadata = fs::symlink_metadata(path).context("inspect", path)?;
            validate_storage_device(root_dev, metadata.dev(), path)
        }
        Err(e) => Err(e).context("inspect", path),
    }
}

fn prepare_storage(config: &LocalStorageConfig) -> Result<File, String> {
    config.prepare_root()?;
    let lock_path = config.root.join(LOCK_FILE);
    let lock = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW)
        .open(&lock_path)
        .context("open", &lock_path)?;
    // SAFETY: `lock` owns an open descriptor for the duration of the call.
    if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        return Err(format!(
            "local storage root is already in use: {}",
            io::Error::last_os_error()
        ));
    }
    // Admission uses root's statvfs, and publication renames between these
    // directories. Reject nested filesystems before writing any IPC data.
    let root_dev = fs::metadata(&config.root)
        .context("inspect", &config.root)?
        .dev();
    let staging = config.root.join(".staging");
    let shards = config.root.join("shards");
    ensure_directory(&staging, root_dev)?;
    ensure_directory(&shards, root_dev)?;
    sync_directory(&config.root).context("sync", &config.root)?;
    // Leave regular files from every writer generation untouched. Readers decide
    // format support; all abandoned files remain charged against the disk quota.
    for entry in fs::read_dir(&shards).context("read", &shards)? {
        let entry = entry.context("read", &shards)?;
        if !entry
            .file_type()
            .context("inspect", &entry.path())?
            .is_file()
        {
            return Err(format!(
                "unsupported archive entry {}; use a separate empty storage root",
                entry.path().display()
            ));
        }
    }
    for entry in fs::read_dir(&staging).context("read", &staging)? {
        let entry = entry.context("read", &staging)?;
        if !entry
            .file_type()
            .context("inspect", &entry.path())?
            .is_file()
        {
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
    let shards = root.join("shards");
    let destination = shards.join(name);
    // Never replace finalized data, even if a name unexpectedly collides.
    rename_noreplace(path, &destination).context("publish", path)?;
    sync_directory(&shards).context("sync", &shards)?;
    let staging = root.join(".staging");
    sync_directory(&staging).context("sync", &staging)?;
    info!("Published local HFT stream {}", destination.display());
    Ok(())
}

fn rename_noreplace(from: &Path, to: &Path) -> io::Result<()> {
    let c_from = CString::new(from.as_os_str().as_bytes())?;
    let c_to = CString::new(to.as_os_str().as_bytes())?;
    // SAFETY: both arguments are NUL-terminated paths that outlive the call.
    let result = unsafe {
        libc::renameat2(
            libc::AT_FDCWD,
            c_from.as_ptr(),
            libc::AT_FDCWD,
            c_to.as_ptr(),
            libc::RENAME_NOREPLACE,
        )
    };
    if result == 0 {
        return Ok(());
    }
    let error = io::Error::last_os_error();
    match error.raw_os_error() {
        // NFS and some FUSE filesystems reject renameat2 flags.
        Some(libc::EINVAL | libc::ENOSYS | libc::EOPNOTSUPP) => link_noreplace(from, to),
        _ => Err(error),
    }
}

/// Publish by hard link, which also refuses to replace an existing file.
fn link_noreplace(from: &Path, to: &Path) -> io::Result<()> {
    fs::hard_link(from, to)?;
    fs::remove_file(from)
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
        let metadata = fs::metadata(path).context("inspect", path)?;
        Ok(sum.saturating_add(metadata.blocks().saturating_mul(512)))
    })
}

fn sync_directory(path: &Path) -> io::Result<()> {
    File::open(path)?.sync_all()
}

fn available_bytes(path: &Path) -> Result<u64, String> {
    let c_path = CString::new(path.as_os_str().as_bytes()).context("inspect", path)?;
    let mut stats = MaybeUninit::<libc::statvfs>::uninit();
    // SAFETY: `c_path` is NUL-terminated and `stats` is valid for writes.
    if unsafe { libc::statvfs(c_path.as_ptr(), stats.as_mut_ptr()) } != 0 {
        return Err(io::Error::last_os_error()).context("statvfs", path);
    }
    // SAFETY: statvfs succeeded, so it initialized `stats`.
    let stats = unsafe { stats.assume_init() };
    // Field widths differ between targets.
    #[allow(clippy::unnecessary_cast)]
    let free = (stats.f_bavail as u64).saturating_mul(stats.f_frsize as u64);
    let mut filesystem = MaybeUninit::<libc::statfs>::uninit();
    // SAFETY: `c_path` is NUL-terminated and `filesystem` is valid for writes.
    if unsafe { libc::statfs(c_path.as_ptr(), filesystem.as_mut_ptr()) } != 0 {
        return Err(io::Error::last_os_error()).context("statfs", path);
    }
    // SAFETY: statfs succeeded, so it initialized `filesystem`.
    if unsafe { filesystem.assume_init() }.f_type == libc::TMPFS_MAGIC {
        let meminfo = Path::new("/proc/meminfo");
        let meminfo = fs::read_to_string(meminfo).context("read", meminfo)?;
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
    use crate::message::saistats::{SAIStat, SAIStatRef, SAIStatsBatch};
    use arrow_array::{Array, ListArray};
    use arrow_ipc::reader::StreamReader;
    use arrow_schema::{DataType, Field};
    use std::{
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

    fn layout_ids(batch: &RecordBatch) -> Vec<u32> {
        let ids = batch.column(0).as_any().downcast_ref::<UInt32Array>();
        ids.unwrap().values().to_vec()
    }

    /// Values of every block of a list column: 1 is timestamps_ns, 2 is values.
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

    /// Samples in reader order: the blocks of each layout in order of first
    /// appearance, keeping input order within a layout.
    fn expected(batch: &SAIStatsBatch) -> Vec<DecodedSample> {
        type Identities = Vec<(Arc<str>, u32, u32)>;
        let mut layouts: Vec<(Identities, Vec<SAIStatsRef<'_>>)> = Vec::new();
        for record in batch.iter() {
            let identities: Vec<_> = record
                .stats
                .iter()
                .map(|stat| (Arc::clone(stat.object_name), stat.type_id, stat.stat_id))
                .collect();
            match layouts.iter_mut().find(|(known, _)| *known == identities) {
                Some((_, records)) => records.push(record),
                None => layouts.push((identities, vec![record])),
            }
        }
        layouts
            .iter()
            .enumerate()
            .flat_map(|(layout, (_, records))| {
                records.iter().flat_map(move |record| {
                    record
                        .stats
                        .iter()
                        .enumerate()
                        .map(move |(i, stat)| DecodedSample {
                            layout: layout as u32,
                            stat_index: i as u32,
                            object_name: Arc::clone(stat.object_name),
                            type_name: Arc::from(stat.type_name_or_id()),
                            stat_name: Arc::from(stat.stat_name_or_id()),
                            observation_time: record.observation_time,
                            value: stat.counter,
                        })
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

    fn template(prefix: &str, width: usize) -> Arc<[SAIStatMetadata]> {
        (0..width)
            .map(|i| SAIStatMetadata::new(format!("{prefix}{i}"), 1, i as u32))
            .collect()
    }

    fn layout_widths(path: &Path) -> Vec<usize> {
        let reader = StreamReader::try_new(File::open(path).unwrap(), None).unwrap();
        let layouts: serde_json::Value =
            serde_json::from_str(&reader.schema().metadata()["layouts"]).unwrap();
        let layouts = layouts.as_array().unwrap();
        layouts
            .iter()
            .map(|l| l.as_array().unwrap().len())
            .collect()
    }

    fn representations(owned: SAIStatsBatch) -> [SAIStatsBatch; 3] {
        let mut shared = SAIStatsBatch::default();
        let mut mixed = SAIStatsBatch::default();
        let mut metadata: Arc<[SAIStatMetadata]> = Arc::from([]);
        for (index, record) in owned.iter().enumerate() {
            let SAIStatsView::Owned(stats) = record.stats else {
                panic!("fixture must start with owned stats");
            };
            if metadata.len() != stats.len()
                || !metadata.iter().zip(stats).all(|(m, s)| {
                    m.object_name == s.object_name
                        && m.type_id == s.type_id
                        && m.stat_id == s.stat_id
                })
            {
                metadata = stats
                    .iter()
                    .map(|s| SAIStatMetadata::new(Arc::clone(&s.object_name), s.type_id, s.stat_id))
                    .collect();
            }
            shared.push_shared_record(
                record.observation_time,
                Arc::clone(&metadata),
                stats.iter().map(|s| s.counter),
            );
            if index % 2 == 0 {
                mixed.push_record(record.observation_time, stats.iter().cloned());
            } else {
                mixed.push_shared_record(
                    record.observation_time,
                    Arc::clone(&metadata),
                    stats.iter().map(|s| s.counter),
                );
            }
        }
        assert_eq!(owned, shared);
        assert_eq!(owned, mixed);
        [owned, shared, mixed]
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
        for batch in representations(batch) {
            let (_temp, files) = encode(&batch);
            assert_eq!(files.len(), 1);
            assert_eq!(decoded(&files[0]), expected(&batch));
            let reader = StreamReader::try_new(File::open(&files[0]).unwrap(), None).unwrap();
            let schema = reader.schema();
            let names: Vec<_> = schema.fields().iter().map(|f| f.name().as_str()).collect();
            assert_eq!(names, ["layout", "timestamps_ns", "values"]);
            assert_eq!(schema.field(0).data_type(), &DataType::UInt32);
            for field in schema.fields() {
                assert!(!field.is_nullable());
            }
            for field in &schema.fields()[1..] {
                assert_eq!(
                    field.data_type(),
                    &DataType::List(Arc::new(Field::new("item", DataType::UInt64, false)))
                );
            }
            let batches: Vec<_> = reader.map(Result::unwrap).collect();
            assert_eq!(
                batches.iter().map(|b| child(b, 1).len()).sum::<usize>(),
                9000
            );
            for b in batches {
                assert_eq!(layout_ids(&b), [0]);
                for c in b.columns() {
                    assert_eq!(c.null_count(), 0);
                }
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
        batch.push_record(0, [SAIStat::new("a", 21, 2, 42), stat("a", 2)]);
        batch.push_record(0, [SAIStat::new("a", 21, 1, 42), stat("a", 2)]);
        batch.push_record(0, [SAIStat::new("renamed", 21, 1, 42), stat("a", 2)]);
        batch.push_record(0, []);
        for batch in representations(batch) {
            let (_temp, files) = encode(&batch);
            // Layout changes share one stream, with a block per layout.
            assert_eq!(files.len(), 1);
            assert_eq!(decoded(&files[0]), expected(&batch));
            assert_eq!(layout_widths(&files[0]), [0, 2, 2, 2, 2, 2, 2]);
            let reader = StreamReader::try_new(File::open(&files[0]).unwrap(), None).unwrap();
            let batches: Vec<_> = reader.map(Result::unwrap).collect();
            assert_eq!(batches.len(), 1);
            assert_eq!(layout_ids(&batches[0]), [0, 1, 2, 3, 4, 5, 6]);
            // Timestamps keep input order within each layout.
            assert_eq!(
                child(&batches[0], 1).values().to_vec(),
                [u64::MAX, 0, 0, 2, 2, 1, 1, 0, 0, 0]
            );
            assert_eq!(
                read_shard(&files[0], |_| Err("visitor stopped".into())),
                Err("visitor stopped".into())
            );
        }
    }

    #[test]
    fn interleaved_templates_share_one_stream() {
        let templates = [
            template("Ethernet", 2),
            template("Queue", 3),
            template("", 0),
        ];
        let mut batch = SAIStatsBatch::default();
        for time in 0..300u64 {
            let template = &templates[time as usize % 3];
            let values = (0..template.len() as u64).map(|i| time * 10 + i);
            batch.push_shared_record(time, Arc::clone(template), values);
        }
        // An owned record with a template's identities joins that template's layout.
        batch.push_record(
            300,
            [
                SAIStat::new("Ethernet0", 1, 0, 7),
                SAIStat::new("Ethernet1", 1, 1, 8),
            ],
        );
        let (_temp, files) = encode(&batch);
        assert_eq!(files.len(), 1);
        assert_eq!(decoded(&files[0]), expected(&batch));
        assert_eq!(layout_widths(&files[0]), [2, 3, 0]);
        let reader = StreamReader::try_new(File::open(&files[0]).unwrap(), None).unwrap();
        let batches: Vec<_> = reader.map(Result::unwrap).collect();
        assert_eq!(batches.len(), 1);
        assert_eq!(layout_ids(&batches[0]), [0, 1, 2]);
        assert_eq!(child(&batches[0], 1).len(), 301);
        assert_eq!(child(&batches[0], 2).len(), 101 * 2 + 100 * 3);
    }

    #[test]
    fn reinstalled_template_keeps_its_layout() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        let [first, second] = [template("Ethernet", 2), template("Ethernet", 2)];
        for (time, template) in [&first, &second, &first, &second].into_iter().enumerate() {
            let record = SAIStatsRef {
                observation_time: time as u64,
                stats: SAIStatsView::Shared {
                    metadata: template,
                    values: &[1, 2],
                },
            };
            store.add_record(record).unwrap();
            assert_eq!(store.lanes.len(), 1);
            let lane = store.lanes[0].template.as_ref().unwrap();
            assert!(Arc::ptr_eq(lane, template));
            assert_eq!(store.templates.len(), 1);
        }
        store.finish_stream().unwrap();
        let files = paths(temp.path(), "shards");
        assert_eq!(files.len(), 1);
        assert_eq!(layout_widths(&files[0]), [2]);
        assert_eq!(decoded(&files[0]).len(), 8);
    }

    #[test]
    fn new_template_rotates_once_and_idle_templates_retire() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        let [a, b, c] = [template("a", 1), template("b", 2), template("c", 3)];
        let add = |store: &mut Store, template: &Arc<[SAIStatMetadata]>, time| {
            let values: Vec<_> = (0..template.len() as u64).collect();
            let stats = SAIStatsView::Shared {
                metadata: template,
                values: &values,
            };
            store
                .add_record(SAIStatsRef {
                    observation_time: time,
                    stats,
                })
                .unwrap();
        };
        add(&mut store, &a, 0);
        add(&mut store, &b, 0);
        store.flush().unwrap();
        add(&mut store, &a, 1);
        store.flush().unwrap();
        // Known templates keep the stream.
        assert_eq!(store.generation, 1);
        add(&mut store, &c, 2);
        store.flush().unwrap();
        // A new template rotates once, keeping templates of the previous stream.
        assert_eq!(store.generation, 2);
        add(&mut store, &c, 3);
        store.finish_stream().unwrap();
        add(&mut store, &c, 4);
        store.finish_stream().unwrap();
        let files = paths(temp.path(), "shards");
        let widths: Vec<_> = files.iter().map(|path| layout_widths(path)).collect();
        assert_eq!(widths, [vec![1, 2], vec![1, 2, 3], vec![3]]);
        let samples: Vec<_> = files.iter().map(|path| decoded(path).len()).collect();
        assert_eq!(samples, [4, 6, 3]);
        assert_eq!(store.lanes.len(), 1);
        assert_eq!(store.templates.len(), 1);
    }

    #[test]
    fn thousands_of_single_counter_templates_share_one_batch() {
        let templates: Vec<_> = (0..4096).map(|i| template(&format!("t{i}-"), 1)).collect();
        let mut batch = SAIStatsBatch::default();
        for round in 0..2u64 {
            for (i, template) in templates.iter().enumerate() {
                batch.push_shared_record(round, Arc::clone(template), [round + i as u64]);
            }
        }
        let (_temp, files) = encode(&batch);
        assert_eq!(files.len(), 1);
        assert_eq!(decoded(&files[0]), expected(&batch));
        let reader = StreamReader::try_new(File::open(&files[0]).unwrap(), None).unwrap();
        let batches: Vec<_> = reader.map(Result::unwrap).collect();
        assert_eq!(batches.len(), 1);
        assert_eq!(batches[0].num_rows(), 4096);
    }

    #[test]
    fn transpose_matches_naive_series_major_order() {
        for stride in [1, 2, 9, 16, 17, 18, 501] {
            for records in [0, 1, 7, 8, 9, 17] {
                let rows: Vec<u64> = (0..(stride * records) as u64).collect();
                let mut block = vec![u64::MAX; records * (stride - 1)];
                transpose(&rows, stride, &mut block);
                let naive: Vec<u64> = (1..stride)
                    .flat_map(|series| {
                        (0..records).map(move |record| (record * stride + series) as u64)
                    })
                    .collect();
                assert_eq!(block, naive, "stride={stride}, records={records}");
            }
        }
    }

    #[test]
    fn wide_layout_batch_budget_and_buffer_reuse() {
        for (width, rows) in [
            (0, 4096),
            (500, 4096),
            (8000, 262),
            (codec::MAX_COUNTERS, 31),
        ] {
            assert_eq!(rows, (BATCH_TARGET_BYTES / ((width + 1) * 8)).min(MAX_ROWS));
            let temp = private_tempdir();
            let mut store = store(temp.path());
            let stats: Vec<_> = (0..width).map(|i| stat("port", i as u64)).collect();
            let record = SAIStatsRef {
                observation_time: u64::MAX,
                stats: SAIStatsView::Owned(&stats),
            };
            let mut buffers = None;
            for _ in 0..2 {
                for row in 0..rows {
                    assert_eq!(store.pending_since.is_some(), row > 0);
                    store.add_record(record).unwrap();
                }
                // A full batch is written without waiting for its deadline.
                assert!(store.pending_since.is_none());
                let lane = &store.lanes[0];
                assert!(lane.rows.capacity() * 8 <= BATCH_TARGET_BYTES);
                assert!(store.values.capacity() * 8 <= BATCH_TARGET_BYTES);
                // The next batch reuses both buffers.
                let current = (lane.rows.as_ptr(), store.values.as_ptr());
                assert_eq!(*buffers.get_or_insert(current), current);
            }
            assert_eq!(store.used_bytes, directory_bytes(temp.path()).unwrap());
            store.finish_stream().unwrap();
            let samples = decoded(&paths(temp.path(), "shards")[0]).len();
            assert_eq!(samples, width * rows * 2);
        }
    }

    fn small_stream() -> (Vec<u8>, Vec<(usize, u64)>, Vec<DecodedSample>) {
        let stats = [stat("a", 0)];
        let schema = codec::test_schema(&[&stats]);
        let options = IpcWriteOptions::default()
            .try_with_compression(Some(CompressionType::ZSTD))
            .unwrap();
        let mut writer = StreamWriter::try_new_with_options(Vec::new(), &schema, options).unwrap();
        let mut boundaries = vec![(writer.get_ref().len(), 0)];
        let mut expected = Vec::new();
        let source = SAIStatRef::from(&stats[0]);
        for i in 0..3u64 {
            let value = [u64::MAX, 0, 1 << 63][i as usize];
            let batch = codec::test_batch(&schema, &[(0, &[u64::MAX - i], &[value])]);
            writer.write(&batch).unwrap();
            boundaries.push((writer.get_ref().len(), i + 1));
            expected.push(DecodedSample {
                layout: 0,
                stat_index: 0,
                object_name: "a".into(),
                type_name: Arc::from(source.type_name_or_id()),
                stat_name: Arc::from(source.stat_name_or_id()),
                observation_time: u64::MAX - i,
                value,
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
                .next_back()
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
                        stats: SAIStatsView::Owned(&[]),
                    })
                    .unwrap();
                writer.flush().unwrap();
                assert_ne!(writer.active.as_ref().unwrap().path, partial);
                writer.finish_stream().unwrap();
                assert_eq!(decoded(&partial), expected);
                assert_eq!(fs::read(&partial).unwrap(), damaged);
                assert_eq!(paths(temp.path(), ".staging"), vec![partial.clone()]);
            }
        }
    }

    #[test]
    fn rejects_invalid_header_and_schema_without_removing_state() {
        let temp = private_tempdir();
        drop(prepare_storage(&config(temp.path())).unwrap());
        let partial = temp.path().join(".staging/bad.arrow.partial");
        for bytes in [b"not an Arrow stream".to_vec(), {
            let valid = codec::test_schema(&[&[stat("a", 0)]]);
            let mut metadata = valid.metadata().clone();
            metadata.insert("matrix_order".into(), "time-major".into());
            let schema = arrow_schema::Schema::new_with_metadata(valid.fields().clone(), metadata);
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
        writer.config.max_bytes = writer.used_bytes;
        writer
            .add_record(SAIStatsRef {
                observation_time: 0,
                stats: SAIStatsView::Owned(&[]),
            })
            .unwrap();
        writer.flush().unwrap();
        assert!(writer.stopped);
        assert_eq!(paths(temp.path(), ".staging").len(), 3);
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
        config.max_bytes = MIN_MAX_BYTES - 1;
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
                stats: SAIStatsView::Owned(&[stat("a", 7)]),
            })
            .unwrap();
        actor.store.flush().unwrap();
        let partial = actor.store.active.as_ref().unwrap().path.clone();
        assert_eq!(fs::metadata(partial).unwrap().mode() & 0o777, 0o600);
        actor.store.finish_stream().unwrap();
        for directory in [root.clone(), root.join(".staging"), root.join("shards")] {
            let metadata = fs::metadata(directory).unwrap();
            assert_eq!(metadata.mode() & 0o777, 0o700);
            assert_eq!(metadata.uid(), unsafe { libc::geteuid() });
            assert_eq!(metadata.dev(), fs::metadata(parent.path()).unwrap().dev());
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
            assert!(
                prepare_storage(&config(root.path()))
                    .unwrap_err()
                    .contains("effective service user")
            );
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
        let needed = FILESYSTEM_RESERVE_BYTES + BATCH_TARGET_BYTES as u64;
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
            assert!(
                prepare_storage(&config(&path))
                    .unwrap_err()
                    .contains("not a symbolic link")
            );
            assert_eq!(fs::read_dir(&target).unwrap().count(), 0);
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
            assert!(
                ensure_directory(&path, root_dev ^ 1)
                    .unwrap_err()
                    .contains("same filesystem")
            );
            assert_eq!(fs::read_dir(&path).unwrap().count(), 0);
        }
    }

    #[test]
    fn quota_stop_publishes_written_batches_and_never_overwrites_published() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        let stats = [stat("a", u64::MAX)];
        let record = SAIStatsRef {
            observation_time: 42,
            stats: SAIStatsView::Owned(&stats),
        };
        store.add_record(record).unwrap();
        store.finish_stream().unwrap();
        let first = paths(temp.path(), "shards")[0].clone();
        let bytes = fs::read(&first).unwrap();
        store.add_record(record).unwrap();
        store.flush().unwrap();
        store.add_record(record).unwrap();
        store.config.max_bytes = store.used_bytes + EOS_RESERVE_BYTES;
        // The next batch does not fit: capture stops and publishes the stream.
        store.flush().unwrap();
        assert!(store.stopped);
        assert!(store.active.is_none() && store.pending_since.is_none());
        store.add_record(record).unwrap();
        assert!(store.pending_since.is_none());
        assert_eq!(fs::read(&first).unwrap(), bytes);
        assert!(store.used_bytes <= store.config.max_bytes);
        assert_eq!(store.used_bytes, directory_bytes(temp.path()).unwrap());
        let files = paths(temp.path(), "shards");
        assert_eq!(files.len(), 2);
        assert_eq!(decoded(&files[1]).len(), 1);
        assert!(paths(temp.path(), ".staging").is_empty());
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
    fn record_whose_flush_stops_capture_is_not_stored() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        let (narrow, wide) = (template("narrow", 1023), template("wide", 1024));
        let values = [7u64; 1024];
        let add = |store: &mut Store, template: &Arc<[SAIStatMetadata]>, time| {
            store.add_record(SAIStatsRef {
                observation_time: time,
                stats: SAIStatsView::Shared {
                    metadata: template,
                    values: &values[..template.len()],
                },
            })
        };
        // Fill the batch to one narrow record short of its target, so that a
        // wider record first flushes it.
        let rows = BATCH_TARGET_BYTES / ((narrow.len() + 1) * 8) - 1;
        for time in 0..rows as u64 {
            add(&mut store, &narrow, time).unwrap();
        }
        assert!(store.active.is_none() && store.pending_since.is_some());
        // The stream's schema fits the quota; its batch does not.
        store.config.max_bytes = store.used_bytes + 4 * 1024 * 1024;
        add(&mut store, &wide, rows as u64).unwrap();
        assert!(store.stopped && store.pending_since.is_none());
        store.finish_stream().unwrap();
        // Only the stopped stream is published, without the wider record.
        let files = paths(temp.path(), "shards");
        assert_eq!(files.len(), 1);
        assert!(decoded(&files[0]).is_empty());
        assert!(paths(temp.path(), ".staging").is_empty());
    }

    #[test]
    fn quota_admits_by_encoded_size_and_fills_most_of_it() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        let quota = 8 * 1024 * 1024;
        store.config.max_bytes = store.used_bytes + quota;
        let mut random = 0x1234_5678_9abc_def0u64;
        let mut written = 0u64;
        while !store.stopped {
            // Incompressible values make batches as large as their raw bytes.
            let stats: Vec<_> = (0..64)
                .map(|_| {
                    random ^= random << 13;
                    random ^= random >> 7;
                    random ^= random << 17;
                    stat("a", random)
                })
                .collect();
            store
                .add_record(SAIStatsRef {
                    observation_time: written,
                    stats: SAIStatsView::Owned(&stats),
                })
                .unwrap();
            written += 1;
        }
        assert!(store.used_bytes <= store.config.max_bytes);
        assert_eq!(store.used_bytes, directory_bytes(temp.path()).unwrap());
        // Less than one more full batch stays unused.
        let unused = store.config.max_bytes - store.used_bytes;
        let batch = batch_bound(1, MAX_ROWS, 64 * MAX_ROWS) + EOS_RESERVE_BYTES;
        assert!(unused < batch, "unused={unused}");
        let persisted: usize = paths(temp.path(), "shards")
            .iter()
            .map(|p| decoded(p).len())
            .sum();
        assert!(persisted > 0 && persisted % 64 == 0);
        assert!(paths(temp.path(), ".staging").is_empty());
    }

    #[test]
    fn link_fallback_publishes_without_replacing() {
        let temp = private_tempdir();
        let from = temp.path().join("stream.arrow.partial");
        let to = temp.path().join("stream.arrow");
        fs::write(&from, b"first").unwrap();
        link_noreplace(&from, &to).unwrap();
        assert!(!from.exists());
        assert_eq!(fs::read(&to).unwrap(), b"first");
        fs::write(&from, b"second").unwrap();
        assert_eq!(
            link_noreplace(&from, &to).unwrap_err().kind(),
            io::ErrorKind::AlreadyExists
        );
        assert_eq!(fs::read(&to).unwrap(), b"first");
        assert_eq!(fs::read(&from).unwrap(), b"second");
    }

    #[test]
    fn oversized_schema_flushes_previous_valid_rows() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        for value in [u64::MAX, 0, 1 << 63] {
            store
                .add_record(SAIStatsRef {
                    observation_time: value,
                    stats: SAIStatsView::Owned(&[stat("a", value)]),
                })
                .unwrap();
        }
        assert!(
            store
                .add_record(SAIStatsRef {
                    observation_time: 0,
                    stats: SAIStatsView::Owned(&[stat(&"x".repeat(16 * 1024 * 1024), 0)])
                })
                .is_err()
        );
        let file = &paths(temp.path(), "shards")[0];
        assert_eq!(
            decoded(file)
                .iter()
                .map(|s| (s.observation_time, s.value))
                .collect::<Vec<_>>(),
            vec![(u64::MAX, u64::MAX), (0, 0), (1 << 63, 1 << 63)]
        );
        assert!(store.pending_since.is_none());
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
    fn size_rotation_waits_for_complete_batches_and_preserves_records() {
        for schema_exceeds_target in [true, false] {
            let target = if schema_exceeds_target { 1 } else { u64::MAX };
            let temp = private_tempdir();
            let mut store = store(temp.path());
            store.config.file_target_bytes = target;
            let stats = [stat(&"a".repeat(4096), u64::MAX)];
            let record = SAIStatsRef {
                observation_time: 0,
                stats: SAIStatsView::Owned(&stats),
            };
            for seq in 0..3 {
                store.add_record(record).unwrap();
                store.flush().unwrap();
                if !schema_exceeds_target {
                    // Below the target, the stream stays open; the next complete
                    // batch crosses it.
                    let active = store.active.as_ref().unwrap();
                    let bytes = active.writer.get_ref().bytes_written;
                    assert_eq!(bytes, fs::metadata(&active.path).unwrap().len());
                    assert_eq!(paths(temp.path(), "shards").len(), seq);
                    store.config.file_target_bytes = bytes + 1;
                    store.add_record(record).unwrap();
                    store.flush().unwrap();
                    store.config.file_target_bytes = target;
                }
                assert!(store.active.is_none());
                assert!(store.pending_since.is_none());
                let files = paths(temp.path(), "shards");
                assert_eq!(files.len(), seq + 1);
                let samples = decoded(&files[seq]);
                assert_eq!(samples.len(), if schema_exceeds_target { 1 } else { 2 });
                assert!(
                    samples
                        .iter()
                        .all(|s| s.observation_time == 0 && s.value == u64::MAX)
                );
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
                    stats: SAIStatsView::Owned(&stats),
                })
                .unwrap();
        }
        store.flush().unwrap();
        assert!(paths(temp.path(), "shards").is_empty());
        let active = store.active.as_ref().unwrap();
        let bytes = active.writer.get_ref().bytes_written;
        assert_eq!(bytes, fs::metadata(&active.path).unwrap().len());
        assert!(bytes < store.config.file_target_bytes);
        assert_eq!(store.file_sequence, 1);
        assert!(store.lanes[0].rows.capacity() * 8 <= BATCH_TARGET_BYTES);
        assert!(store.values.capacity() * 8 <= BATCH_TARGET_BYTES);
        store.finish_stream().unwrap();
        let files = paths(temp.path(), "shards");
        assert_eq!(files.len(), 1);
        assert_eq!(fs::metadata(&files[0]).unwrap().len(), bytes + 8);
        let reader = StreamReader::try_new(File::open(&files[0]).unwrap(), None).unwrap();
        let mut read_records = 0;
        for batch in reader {
            let batch = batch.unwrap();
            let timestamps = child(&batch, 1);
            assert!(timestamps.values().iter().all(|&value| value == 0));
            read_records += timestamps.len();
            assert_eq!(child(&batch, 2).len(), stats.len() * timestamps.len());
            assert!(child(&batch, 2).values().iter().all(|&value| value == 0));
        }
        assert_eq!(read_records, records);
    }

    #[test]
    fn max_age_rotation_is_independent_of_source_timestamps() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        let empty = |time| SAIStatsRef {
            observation_time: time,
            stats: SAIStatsView::Owned(&[]),
        };
        for time in [0, u64::MAX, 0] {
            store.add_record(empty(time)).unwrap();
        }
        store.flush().unwrap();
        assert!(paths(temp.path(), "shards").is_empty());
        store.active.as_mut().unwrap().started = Instant::now() - store.config.shard_interval;
        store.poll(Instant::now()).unwrap();
        assert_eq!(paths(temp.path(), "shards").len(), 1);
        assert!(store.active.is_none());
        assert_eq!(
            codec::scan(&paths(temp.path(), "shards")[0])
                .unwrap()
                .records,
            3
        );
        store.add_record(empty(0)).unwrap();
        // Streams open with their first batch.
        assert!(store.active.is_none());
        assert_eq!(paths(temp.path(), "shards").len(), 1);
        store.add_record(empty(0)).unwrap();
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
    fn deadlines_follow_the_oldest_record_and_stream_age() {
        let temp = private_tempdir();
        let mut store = store(temp.path());
        assert_eq!(store.next_deadline(), None);
        let record = SAIStatsRef {
            observation_time: 0,
            stats: SAIStatsView::Owned(&[]),
        };
        store.add_record(record).unwrap();
        let since = store.pending_since.unwrap();
        store.add_record(record).unwrap();
        assert_eq!(store.next_deadline(), Some(since + FLUSH_INTERVAL));
        store.poll(since + FLUSH_INTERVAL / 2).unwrap();
        assert_eq!(store.pending_since, Some(since));
        store.poll(since + FLUSH_INTERVAL).unwrap();
        assert!(store.pending_since.is_none());
        let started = store.active.as_ref().unwrap().started;
        assert_eq!(
            store.next_deadline(),
            Some(started + store.config.shard_interval)
        );
        // An unrepresentable maximum age never rotates, rather than overflowing.
        store.config.shard_interval = Duration::from_secs(u64::MAX);
        assert_eq!(store.next_deadline(), None);
        store.poll(Instant::now()).unwrap();
        assert!(store.active.is_some());
    }

    #[test]
    fn names_limits_and_setup_status() {
        // Series names match the other sinks, including unknown numeric IDs.
        let stats = [
            SAIStat::new("Ethernet0", 1, 0, 0),
            SAIStat::new("Ethernet0|0", 21, 0, 0),
            SAIStat::new("pool", 24, 0, 0),
            SAIStat::new("Ethernet0|0", 26, 0, 0),
            SAIStat::new("x", u32::MAX, 7, 0),
        ];
        let layout = codec::Layout::new(SAIStatsView::Owned(&stats)).unwrap();
        let series: serde_json::Value = serde_json::from_str(&layout.json).unwrap();
        let names: Vec<_> = series
            .as_array()
            .unwrap()
            .iter()
            .map(|s| {
                (
                    s["type_name"].as_str().unwrap(),
                    s["stat_name"].as_str().unwrap(),
                )
            })
            .collect();
        assert_eq!(
            names[0],
            ("SAI_OBJECT_TYPE_PORT", "SAI_PORT_STAT_IF_IN_OCTETS")
        );
        for (index, prefix) in [
            (1, "SAI_QUEUE_STAT_"),
            (2, "SAI_BUFFER_POOL_STAT_"),
            (3, "SAI_INGRESS_PRIORITY_GROUP_STAT_"),
        ] {
            assert!(names[index].1.starts_with(prefix));
        }
        assert_eq!(
            names[4],
            (
                "UNKNOWN_SAI_OBJECT_TYPE_4294967295",
                "UNKNOWN_SAI_STAT_TYPE_4294967295_ID_7"
            )
        );
        let temp = private_tempdir();
        let mut config = config(temp.path());
        config.prepare_root().unwrap();
        config.shard_interval = Duration::ZERO;
        assert!(config.validate().is_err());
        config.shard_interval = Duration::from_secs(1);
        config.file_target_bytes = 0;
        assert!(config.validate().is_err());
        config.file_target_bytes = 1;
        assert!(config.validate().is_ok());
        config.max_bytes = MIN_MAX_BYTES - 1;
        assert!(config.validate().is_err());
        let (sender, receiver) = channel(32);
        let status = LocalStorageStatus::default();
        assert!(LocalStorageActor::new(receiver, config, status.clone()).is_err());
        assert!(status.failed());
        assert!(
            sender
                .blocking_send(Arc::new(SAIStatsBatch::default()))
                .is_err()
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
                        stats: SAIStatsView::Owned(&stats),
                    })
                    .unwrap();
                time += 1;
            }
            store.flush().unwrap();
        }
        store.finish_stream().unwrap();
        let path = &paths(temp.path(), "shards")[0];
        let reader = StreamReader::try_new(File::open(path).unwrap(), None).unwrap();
        let schema = reader.schema();
        assert_eq!(schema.fields().len(), 3);
        assert_eq!(schema.metadata()["matrix_order"], "series-major");
        let layouts: serde_json::Value =
            serde_json::from_str(&schema.metadata()["layouts"]).unwrap();
        assert_eq!(layouts.as_array().unwrap().len(), 1);
        let series = &layouts[0];
        assert_eq!(series.as_array().unwrap().len(), 2);
        assert_eq!(series[0], series[1]);
        assert_eq!(series[0].as_object().unwrap().len(), 3);
        assert_eq!(series[0]["object_name"], "same|\"\n");
        let mut record_index = 0;
        for (batch, rows) in reader.zip([3, 1, 5]) {
            let batch = batch.unwrap();
            assert_eq!(layout_ids(&batch), [0]);
            let timestamps = child(&batch, 1);
            let values = child(&batch, 2);
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
                stats: SAIStatsView::Owned(&[stat("a", u64::MAX)]),
            })
            .unwrap();
        store.flush().unwrap();
        let schema = Arc::clone(&store.active.as_ref().unwrap().schema);
        let batch = codec::test_batch(&schema, &[(0, &[0], &[0])]);
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
    fn actor_quota_stop_disconnects_without_failure_or_touching_archive() {
        let temp = private_tempdir();
        let (sender, receiver) = channel(32);
        let status = LocalStorageStatus::default();
        let mut actor =
            LocalStorageActor::new(receiver, config(temp.path()), status.clone()).unwrap();
        actor.store.config.max_bytes = actor.store.used_bytes;
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
        assert!(!status.failed());
        assert!(
            sender
                .blocking_send(Arc::new(SAIStatsBatch::default()))
                .is_err()
        );
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
                    stats: SAIStatsView::Owned(&[stat("a", value)]),
                })
                .unwrap();
            store.flush().unwrap();
        }
        // An accepted in-memory row is deliberately not promised durable.
        store
            .add_record(SAIStatsRef {
                observation_time: 1,
                stats: SAIStatsView::Owned(&[stat("a", 99)]),
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
