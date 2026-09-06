//! Bounded, best-effort Parquet range storage, ported from Pterosaur PR7.
use std::{
    collections::HashMap,
    fs::{self, File, OpenOptions},
    io,
    os::unix::{
        ffi::OsStrExt,
        fs::{MetadataExt, OpenOptionsExt},
        io::AsRawFd,
    },
    path::{Path, PathBuf},
    sync::{mpsc, Arc},
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use crate::message::{
    local_storage::{LocalStorageMessage, LocalStorageStatus},
    saistats::SAIStatsRef,
};
use arrow_array::{ArrayRef, RecordBatch, StringArray, UInt32Array, UInt64Array};
use arrow_schema::{DataType, Field, Schema, SchemaRef};
use log::{error, info, warn};
use parquet::{
    arrow::ArrowWriter,
    basic::{Compression, Encoding, ZstdLevel},
    file::properties::{EnabledStatistics, WriterProperties, WriterVersion},
    format::KeyValue,
    schema::types::ColumnPath,
};

const FORMAT_VERSION: &str = "sonic-hft-parquet-v1";
const GAUGE_FILE: &str = "gauge_ranges.parquet";
const READY_FILE: &str = "_READY";
const LOSS_FILE: &str = "loss.json";
const LOCK_FILE: &str = ".writer.lock";
const MAX_ROW_GROUP_ROWS: usize = 100_000;
const DATA_PAGE_SIZE: usize = 256 * 1024;
const WRITER_QUEUE_CAPACITY: usize = 1;
const SHARD_ROTATE_UNCOMPRESSED_BYTES: u64 = 120_000_000;
const MAX_SHARD_UNCOMPRESSED_BYTES: u64 = 128_000_000;
const SHARD_RESERVE_BYTES: u64 = 512_000_000;
const FILESYSTEM_RESERVE_BYTES: u64 = 512_000_000;

pub const RANGE_FLAG_DECREASED: u32 = 1;
pub const RANGE_FLAG_GAP: u32 = 1 << 1;
pub const RANGE_FLAG_STORAGE_DROP: u32 = 1 << 2;
pub const RANGE_FLAG_TIME_REGRESSED: u32 = 1 << 3;

#[derive(Debug, Clone)]
pub struct LocalStorageConfig {
    pub root: PathBuf,
    pub range_interval: Duration,
    pub shard_interval: Duration,
    pub max_bytes: u64,
    pub require_dedicated_filesystem: bool,
}

impl LocalStorageConfig {
    pub fn validate(&self) -> Result<(), String> {
        if self.range_interval.is_zero() {
            return Err("local storage range interval must be greater than zero".to_string());
        }
        if self.shard_interval.is_zero() {
            return Err("local storage shard interval must be greater than zero".to_string());
        }
        if self.max_bytes <= SHARD_RESERVE_BYTES {
            return Err(format!(
                "local storage max bytes must exceed the {} byte shard reserve",
                SHARD_RESERVE_BYTES
            ));
        }
        if self.shard_interval < self.range_interval {
            return Err(
                "local storage shard interval must not be shorter than range interval".to_string(),
            );
        }
        Ok(())
    }

    pub fn validate_root(&self) -> Result<(), String> {
        self.validate()?;
        let metadata = fs::symlink_metadata(&self.root).map_err(|error| error.to_string())?;
        if metadata.file_type().is_symlink() || !metadata.is_dir() {
            return Err(format!(
                "local storage root {} must be an existing directory, not a symbolic link",
                self.root.display()
            ));
        }
        if self.require_dedicated_filesystem && !is_mount_point(&self.root)? {
            return Err(format!(
                "local storage root {} must be a dedicated mount point",
                self.root.display()
            ));
        }
        Ok(())
    }

    fn range_interval_ns(&self) -> u64 {
        self.range_interval.as_nanos().min(u128::from(u64::MAX)) as u64
    }

    fn shard_interval_ns(&self) -> u64 {
        self.shard_interval.as_nanos().min(u128::from(u64::MAX)) as u64
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
struct SeriesKey {
    object_name: Arc<str>,
    type_id: u32,
    stat_id: u32,
}

#[derive(Debug)]
struct SeriesState {
    range: RangeState,
    expected_interval_ns: Option<u64>,
}

#[derive(Debug)]
struct RangeState {
    window: u64,
    first_time_unix_nano: u64,
    last_time_unix_nano: u64,
    first_value: u64,
    previous_value: Option<u64>,
    last_value: u64,
    min_value: u64,
    max_value: u64,
    min_time_unix_nano: u64,
    max_time_unix_nano: u64,
    max_change: u64,
    max_change_time_unix_nano: u64,
    total_increase: u64,
    sample_count: u32,
    change_count: u32,
    flags: u32,
}

impl RangeState {
    fn new(window: u64, time_unix_nano: u64, value: u64, previous_value: Option<u64>) -> Self {
        let mut state = Self {
            window,
            first_time_unix_nano: time_unix_nano,
            last_time_unix_nano: time_unix_nano,
            first_value: value,
            previous_value,
            last_value: value,
            min_value: value,
            max_value: value,
            min_time_unix_nano: time_unix_nano,
            max_time_unix_nano: time_unix_nano,
            max_change: 0,
            max_change_time_unix_nano: time_unix_nano,
            total_increase: 0,
            sample_count: 1,
            change_count: 0,
            flags: 0,
        };
        if let Some(previous_value) = previous_value {
            state.record_change(time_unix_nano, previous_value, value);
        }
        state
    }

    fn update(&mut self, time_unix_nano: u64, value: u64) {
        let previous_value = self.last_value;
        self.record_change(time_unix_nano, previous_value, value);
        if value < self.min_value {
            self.min_value = value;
            self.min_time_unix_nano = time_unix_nano;
        }
        if value > self.max_value {
            self.max_value = value;
            self.max_time_unix_nano = time_unix_nano;
        }
        self.last_time_unix_nano = time_unix_nano;
        self.last_value = value;
        self.sample_count = self.sample_count.saturating_add(1);
    }

    fn record_change(&mut self, time_unix_nano: u64, previous_value: u64, value: u64) {
        if value != previous_value {
            self.change_count = self.change_count.saturating_add(1);
        }
        let change = value.abs_diff(previous_value);
        if change > self.max_change {
            self.max_change = change;
            self.max_change_time_unix_nano = time_unix_nano;
        }
        if value < previous_value {
            self.flags |= RANGE_FLAG_DECREASED;
        } else {
            self.total_increase = self.total_increase.saturating_add(change);
        }
    }
}

#[derive(Debug)]
struct GaugeRangeRow {
    key: SeriesKey,
    state: RangeState,
    window_start_unix_nano: u64,
    window_end_unix_nano: u64,
}

#[derive(Debug, Default)]
struct ShardRows {
    gauges: Vec<GaugeRangeRow>,
    dropped_input_messages: u64,
    dropped_shards: u64,
    estimated_uncompressed_bytes: u64,
}

impl ShardRows {
    fn push_gauge(&mut self, key: SeriesKey, state: RangeState, interval_ns: u64) {
        self.estimated_uncompressed_bytes = self
            .estimated_uncompressed_bytes
            .saturating_add(176)
            .saturating_add(key.object_name.len() as u64);
        let window_start_unix_nano = state.window.saturating_mul(interval_ns);
        self.gauges.push(GaugeRangeRow {
            key,
            state,
            window_start_unix_nano,
            window_end_unix_nano: window_start_unix_nano.saturating_add(interval_ns),
        });
    }

    fn is_empty(&self) -> bool {
        self.gauges.is_empty() && self.dropped_input_messages == 0 && self.dropped_shards == 0
    }
}

struct LocalReducer {
    range_interval_ns: u64,
    shard_interval_ns: u64,
    series: HashMap<SeriesKey, usize>,
    states: Vec<(SeriesKey, SeriesState)>,
    // Only a positional lookup cache, never the identity or lifetime of a series.
    layout: Vec<usize>,
    estimated_state_bytes: u64,
    shard_start: Option<u64>,
    rows: ShardRows,
    storage_drop_pending: bool,
    writer_drop_warned: bool,
}

impl LocalReducer {
    fn new(config: &LocalStorageConfig) -> Self {
        Self {
            range_interval_ns: config.range_interval_ns(),
            shard_interval_ns: config.shard_interval_ns(),
            series: HashMap::new(),
            states: Vec::new(),
            layout: Vec::new(),
            estimated_state_bytes: 0,
            shard_start: None,
            rows: ShardRows::default(),
            storage_drop_pending: false,
            writer_drop_warned: false,
        }
    }

    fn add_gauges(&mut self, stats: SAIStatsRef<'_>) -> Result<(), String> {
        // Preflight before mutating any state. Each input can create a new identity
        // or close one existing range; 320 + name bytes covers either operation.
        let growth = stats.stats.iter().fold(0u64, |bytes, stat| {
            bytes
                .saturating_add(320)
                .saturating_add(stat.object_name.len() as u64)
        });
        if self.estimated_bytes().saturating_add(growth) > MAX_SHARD_UNCOMPRESSED_BYTES {
            return Err("local storage reducer size limit exceeded".to_string());
        }
        let time = stats.observation_time;
        let window = time / self.range_interval_ns;
        self.shard_start.get_or_insert(time);
        let same_layout = self.layout.len() == stats.stats.len()
            && self.layout.iter().zip(stats.stats).all(|(&index, stat)| {
                let key = &self.states[index].0;
                key.object_name == stat.object_name
                    && key.type_id == stat.type_id
                    && key.stat_id == stat.stat_id
            });
        if !same_layout {
            self.layout.clear();
            for stat in stats.stats {
                let key = SeriesKey {
                    object_name: Arc::clone(&stat.object_name),
                    type_id: stat.type_id,
                    stat_id: stat.stat_id,
                };
                let index = match self.series.get(&key) {
                    Some(&index) => index,
                    None => {
                        // Include open ranges and identity/cache overhead in the bound.
                        let next_bytes = self
                            .estimated_state_bytes
                            .saturating_add(320)
                            .saturating_add(key.object_name.len() as u64);
                        if next_bytes.saturating_add(self.rows.estimated_uncompressed_bytes)
                            > MAX_SHARD_UNCOMPRESSED_BYTES
                        {
                            return Err("local storage reducer state limit exceeded".to_string());
                        }
                        self.estimated_state_bytes = next_bytes;
                        let index = self.states.len();
                        let mut range = RangeState::new(window, time, stat.counter, None);
                        // The update pass counts this first observation exactly once.
                        range.sample_count = 0;
                        if self.storage_drop_pending {
                            range.flags |= RANGE_FLAG_STORAGE_DROP;
                        }
                        self.states.push((
                            key.clone(),
                            SeriesState {
                                range,
                                expected_interval_ns: None,
                            },
                        ));
                        self.series.insert(key, index);
                        index
                    }
                };
                self.layout.push(index);
            }
        }
        for (&index, stat) in self.layout.iter().zip(stats.stats) {
            let (key, state) = &mut self.states[index];
            let previous_time = state.range.last_time_unix_nano;
            if state.range.sample_count == 0 {
                state.range.sample_count = 1;
                continue;
            }
            let regressed = time < previous_time;
            let observed_interval = time.saturating_sub(previous_time);
            if observed_interval > 0 {
                state.expected_interval_ns = Some(
                    state
                        .expected_interval_ns
                        .map_or(observed_interval, |expected| {
                            expected.min(observed_interval)
                        }),
                );
            }
            let gap = state
                .expected_interval_ns
                .is_some_and(|expected| observed_interval > expected.saturating_add(expected / 2))
                || window > state.range.window.saturating_add(1);
            if regressed {
                // Preserve both sides of a source-clock discontinuity, without
                // assigning a backwards sample to the current window or bridging it.
                state.range.flags |= RANGE_FLAG_TIME_REGRESSED;
                let mut next = RangeState::new(window, time, stat.counter, None);
                next.flags |= RANGE_FLAG_TIME_REGRESSED;
                let previous = std::mem::replace(&mut state.range, next);
                self.rows
                    .push_gauge(key.clone(), previous, self.range_interval_ns);
                state.expected_interval_ns = None;
            } else if window > state.range.window {
                let next =
                    RangeState::new(window, time, stat.counter, Some(state.range.last_value));
                let previous = std::mem::replace(&mut state.range, next);
                self.rows
                    .push_gauge(key.clone(), previous, self.range_interval_ns);
            } else {
                state.range.update(time, stat.counter);
            }
            if gap {
                state.range.flags |= RANGE_FLAG_GAP;
            }
            if self.storage_drop_pending {
                state.range.flags |= RANGE_FLAG_STORAGE_DROP;
            }
        }
        if self.estimated_bytes() > MAX_SHARD_UNCOMPRESSED_BYTES {
            return Err("local storage reducer size limit exceeded".to_string());
        }
        Ok(())
    }

    fn estimated_bytes(&self) -> u64 {
        self.estimated_state_bytes
            .saturating_add(self.rows.estimated_uncompressed_bytes)
    }

    fn should_rotate(&self, time_unix_nano: u64) -> bool {
        self.estimated_bytes() >= SHARD_ROTATE_UNCOMPRESSED_BYTES
            || self
                .shard_start
                .is_some_and(|start| time_unix_nano.saturating_sub(start) >= self.shard_interval_ns)
    }

    fn take_shard(&mut self, next_start_unix_nano: Option<u64>) -> ShardRows {
        self.shard_start = next_start_unix_nano;
        // Drop counters are out-of-band: keep attribution conservative forever.
        std::mem::take(&mut self.rows)
    }

    fn add_record(
        &mut self,
        record: SAIStatsRef<'_>,
        sender: &mpsc::SyncSender<ShardRows>,
    ) -> Result<(), String> {
        let mut remaining = record.stats;
        while !remaining.is_empty() {
            let mut bytes = self.estimated_bytes();
            let count = remaining
                .iter()
                .take_while(|stat| {
                    bytes = bytes
                        .saturating_add(320)
                        .saturating_add(stat.object_name.len() as u64);
                    bytes <= SHARD_ROTATE_UNCOMPRESSED_BYTES
                })
                .count();
            if count == 0 {
                // Publish valid accumulated data before rejecting even an oversized
                // identity. A large valid record is handled in bounded slices.
                let rows = self.finish();
                queue_shard(sender, rows, self).map_err(|_| "local writer stopped".to_string())?;
                if 320u64.saturating_add(remaining[0].object_name.len() as u64)
                    > SHARD_ROTATE_UNCOMPRESSED_BYTES
                {
                    return Err("local storage identity exceeds reducer capacity".to_string());
                }
                continue;
            }
            self.add_gauges(SAIStatsRef {
                observation_time: record.observation_time,
                stats: &remaining[..count],
            })?;
            remaining = &remaining[count..];
        }
        Ok(())
    }

    fn rotate(
        &mut self,
        time: Option<u64>,
        last_wall_flush: &mut Instant,
        interval: Duration,
        sender: &mpsc::SyncSender<ShardRows>,
    ) -> Result<(), ()> {
        let wall_due = last_wall_flush.elapsed() >= interval;
        if wall_due || time.is_some_and(|time| self.should_rotate(time)) {
            if wall_due {
                self.flush_open_ranges();
                *last_wall_flush = Instant::now();
            }
            let rows = self.take_shard(time);
            queue_shard(sender, rows, self)?;
        }
        Ok(())
    }

    fn flush_open_ranges(&mut self) {
        for (key, state) in self.states.drain(..) {
            self.rows
                .push_gauge(key, state.range, self.range_interval_ns);
        }
        self.series.clear();
        self.layout.clear();
        self.estimated_state_bytes = 0;
    }

    fn finish(&mut self) -> ShardRows {
        self.flush_open_ranges();
        self.take_shard(None)
    }

    fn mark_input_drop(&mut self, count: u64) {
        if count == 0 {
            return;
        }
        self.rows.dropped_input_messages = self.rows.dropped_input_messages.saturating_add(count);
        self.storage_drop_pending = true;
        self.mark_ranges_dropped();
    }

    fn mark_shard_drop(&mut self, count: u64) {
        self.rows.dropped_shards = self.rows.dropped_shards.saturating_add(count);
        self.storage_drop_pending = true;
        self.mark_ranges_dropped();
    }

    fn mark_ranges_dropped(&mut self) {
        for row in &mut self.rows.gauges {
            row.state.flags |= RANGE_FLAG_STORAGE_DROP;
        }
        for (_, state) in &mut self.states {
            state.range.flags |= RANGE_FLAG_STORAGE_DROP;
        }
    }
}

pub struct LocalStorageActor {
    receiver: mpsc::Receiver<LocalStorageMessage>,
    config: LocalStorageConfig,
    status: LocalStorageStatus,
    storage_lock: File,
}

impl LocalStorageActor {
    pub fn new(
        receiver: mpsc::Receiver<LocalStorageMessage>,
        config: LocalStorageConfig,
        status: LocalStorageStatus,
    ) -> Result<Self, String> {
        let storage_lock = prepare_storage(&config).inspect_err(|_| status.mark_failed())?;
        Ok(Self {
            receiver,
            config,
            status,
            storage_lock,
        })
    }

    /// Blocking reducer loop. Closing all senders drains queued batches and flushes.
    pub fn run(self) {
        let mut reducer = LocalReducer::new(&self.config);
        let writer_config = self.config.clone();
        let writer_status = self.status.clone();
        let storage_lock = self.storage_lock;
        let (writer_sender, writer_receiver) = mpsc::sync_channel(WRITER_QUEUE_CAPACITY);
        let writer = thread::Builder::new()
            .name("hft-parquet-writer".to_string())
            .spawn(move || {
                writer_loop(writer_config, writer_receiver, writer_status, storage_lock)
            });
        let writer = match writer {
            Ok(writer) => writer,
            Err(reason) => {
                self.status.mark_failed();
                error!("Failed to start local HFT writer: {}", reason);
                return;
            }
        };
        let mut last_wall_flush = Instant::now();
        'receive: loop {
            if self.status.failed() || self.status.shutdown_requested() {
                break;
            }
            let wait = Duration::from_millis(250).min(
                self.config
                    .shard_interval
                    .saturating_sub(last_wall_flush.elapsed()),
            );
            let message = match self.receiver.recv_timeout(wait) {
                Ok(message) => message,
                Err(mpsc::RecvTimeoutError::Timeout) => {
                    reducer.mark_input_drop(self.status.take_input_drops());
                    if reducer
                        .rotate(
                            None,
                            &mut last_wall_flush,
                            self.config.shard_interval,
                            &writer_sender,
                        )
                        .is_err()
                    {
                        self.status.mark_failed();
                        break;
                    }
                    continue;
                }
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            };
            reducer.mark_input_drop(self.status.take_input_drops());
            // Borrow slices directly; never rebuild a per-record SAIStats or Vec.
            for record in message.iter() {
                if self.status.failed() || self.status.shutdown_requested() {
                    break 'receive;
                }
                if let Err(reason) = reducer.add_record(record, &writer_sender) {
                    error!("Disabling local HFT storage: {}", reason);
                    self.status.mark_failed();
                    break 'receive;
                }
                if reducer
                    .rotate(
                        Some(record.observation_time),
                        &mut last_wall_flush,
                        self.config.shard_interval,
                        &writer_sender,
                    )
                    .is_err()
                {
                    self.status.mark_failed();
                    break 'receive;
                }
            }
        }

        reducer.mark_input_drop(self.status.take_input_drops());
        if !self.status.failed() {
            let rows = reducer.finish();
            // Only this isolated thread waits for the final writer slot. Main bounds
            // shutdown even if the filesystem or writer cannot make progress.
            if writer_sender.send(rows).is_err() {
                self.status.mark_failed();
                error!("Unable to queue final local HFT shard because the writer stopped");
            }
        }
        drop(writer_sender);
        if writer.join().is_err() {
            self.status.mark_failed();
            error!("Local HFT writer thread panicked");
        }
    }
}

fn queue_shard(
    sender: &mpsc::SyncSender<ShardRows>,
    rows: ShardRows,
    reducer: &mut LocalReducer,
) -> Result<(), ()> {
    if rows.is_empty() {
        return Ok(());
    }
    match sender.try_send(rows) {
        Ok(()) => Ok(()),
        Err(mpsc::TrySendError::Full(rows)) => {
            // Loss sidecars carry cumulative counts. Logging every dropped shard
            // would flood logs when source timestamps jump faster than the writer.
            if !reducer.writer_drop_warned {
                warn!("Local HFT writer queue is full; dropping shards. Further losses are reported in loss.json");
                reducer.writer_drop_warned = true;
            }
            reducer.mark_input_drop(rows.dropped_input_messages);
            reducer.mark_shard_drop(rows.dropped_shards.saturating_add(1));
            Ok(())
        }
        Err(mpsc::TrySendError::Disconnected(_)) => {
            error!("Disabling local HFT storage because the writer stopped");
            Err(())
        }
    }
}

fn is_mount_point(path: &Path) -> Result<bool, String> {
    let path = fs::canonicalize(path).map_err(|error| error.to_string())?;
    let metadata = fs::metadata(&path).map_err(|error| error.to_string())?;
    let parent = path
        .parent()
        .ok_or_else(|| format!("local storage root {} has no parent", path.display()))?;
    let parent_metadata = fs::metadata(parent).map_err(|error| error.to_string())?;
    Ok(metadata.dev() != parent_metadata.dev())
}

fn writer_loop(
    config: LocalStorageConfig,
    receiver: mpsc::Receiver<ShardRows>,
    status: LocalStorageStatus,
    _storage_lock: File,
) {
    let mut committed_bytes = match directory_bytes(&config.root) {
        Ok(bytes) => bytes,
        Err(reason) => {
            error!("Local HFT writer could not measure storage: {}", reason);
            status.mark_failed();
            return;
        }
    };
    for (sequence, rows) in (0u64..).zip(receiver) {
        if let Err(reason) = write_shard(&config, sequence, rows, &mut committed_bytes) {
            error!("Local HFT writer stopped after write failure: {}", reason);
            status.mark_failed();
            return;
        }
    }
}

fn ensure_directory(path: &Path) -> Result<(), String> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_symlink() => {
            Err(format!("{} must not be a symbolic link", path.display()))
        }
        Ok(metadata) if !metadata.is_dir() => {
            Err(format!("{} must be a directory", path.display()))
        }
        Ok(_) => Ok(()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            fs::create_dir(path).map_err(|error| error.to_string())
        }
        Err(error) => Err(error.to_string()),
    }
}

fn prepare_storage(config: &LocalStorageConfig) -> Result<File, String> {
    config.validate_root()?;
    let staging_root = config.root.join(".staging");
    let shards_root = config.root.join("shards");
    let lock = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .custom_flags(libc::O_NOFOLLOW)
        .open(config.root.join(LOCK_FILE))
        .map_err(|error| error.to_string())?;
    let result = unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) };
    if result != 0 {
        return Err(format!(
            "local storage root is already in use: {}",
            io::Error::last_os_error()
        ));
    }
    ensure_directory(&staging_root)?;
    ensure_directory(&shards_root)?;
    sync_directory(&config.root).map_err(|error| error.to_string())?;
    for entry in fs::read_dir(&staging_root).map_err(|error| error.to_string())? {
        let path = entry.map_err(|error| error.to_string())?.path();
        let metadata = fs::symlink_metadata(&path).map_err(|error| error.to_string())?;
        if metadata.is_dir() && !metadata.file_type().is_symlink() {
            fs::remove_dir_all(path).map_err(|error| error.to_string())?;
        } else {
            fs::remove_file(path).map_err(|error| error.to_string())?;
        }
    }
    sync_directory(&staging_root).map_err(|error| error.to_string())?;
    Ok(lock)
}

fn gauge_schema() -> SchemaRef {
    Arc::new(Schema::new_with_metadata(
        vec![
            Field::new("object_name", DataType::Utf8, false),
            Field::new("sai_type_id", DataType::UInt32, false),
            Field::new("sai_stat_id", DataType::UInt32, false),
            Field::new("window_start_unix_nano", DataType::UInt64, false),
            Field::new("window_end_unix_nano", DataType::UInt64, false),
            Field::new("first_time_unix_nano", DataType::UInt64, false),
            Field::new("last_time_unix_nano", DataType::UInt64, false),
            Field::new("first_value", DataType::UInt64, false),
            Field::new("previous_value", DataType::UInt64, true),
            Field::new("last_value", DataType::UInt64, false),
            Field::new("min_value", DataType::UInt64, false),
            Field::new("max_value", DataType::UInt64, false),
            Field::new("min_time_unix_nano", DataType::UInt64, false),
            Field::new("max_time_unix_nano", DataType::UInt64, false),
            Field::new("max_change", DataType::UInt64, false),
            Field::new("max_change_time_unix_nano", DataType::UInt64, false),
            Field::new("total_increase", DataType::UInt64, false),
            Field::new("sample_count", DataType::UInt32, false),
            Field::new("change_count", DataType::UInt32, false),
            Field::new("flags", DataType::UInt32, false),
        ],
        HashMap::from([
            ("format_version".to_string(), FORMAT_VERSION.to_string()),
            (
                "window_semantics".to_string(),
                "[window_start_unix_nano,window_end_unix_nano)".to_string(),
            ),
            (
                "flags".to_string(),
                "1=decreased,2=source_gap,4=storage_drop,8=time_regressed".to_string(),
            ),
        ]),
    ))
}

fn gauge_batch(rows: &[GaugeRangeRow]) -> Result<RecordBatch, String> {
    let arrays: Vec<ArrayRef> = vec![
        Arc::new(StringArray::from_iter_values(
            rows.iter().map(|row| row.key.object_name.as_ref()),
        )),
        Arc::new(UInt32Array::from_iter_values(
            rows.iter().map(|row| row.key.type_id),
        )),
        Arc::new(UInt32Array::from_iter_values(
            rows.iter().map(|row| row.key.stat_id),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.window_start_unix_nano),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.window_end_unix_nano),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.state.first_time_unix_nano),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.state.last_time_unix_nano),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.state.first_value),
        )),
        Arc::new(UInt64Array::from(
            rows.iter()
                .map(|row| row.state.previous_value)
                .collect::<Vec<_>>(),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.state.last_value),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.state.min_value),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.state.max_value),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.state.min_time_unix_nano),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.state.max_time_unix_nano),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.state.max_change),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.state.max_change_time_unix_nano),
        )),
        Arc::new(UInt64Array::from_iter_values(
            rows.iter().map(|row| row.state.total_increase),
        )),
        Arc::new(UInt32Array::from_iter_values(
            rows.iter().map(|row| row.state.sample_count),
        )),
        Arc::new(UInt32Array::from_iter_values(
            rows.iter().map(|row| row.state.change_count),
        )),
        Arc::new(UInt32Array::from_iter_values(
            rows.iter().map(|row| row.state.flags),
        )),
    ];
    RecordBatch::try_new(gauge_schema(), arrays).map_err(|error| error.to_string())
}

fn writer_properties() -> Result<WriterProperties, String> {
    let zstd = ZstdLevel::try_new(1).map_err(|error| error.to_string())?;
    Ok(WriterProperties::builder()
        .set_writer_version(WriterVersion::PARQUET_1_0)
        .set_key_value_metadata(Some(vec![KeyValue::new(
            "sonic_hft_format".to_string(),
            Some(FORMAT_VERSION.to_string()),
        )]))
        .set_compression(Compression::ZSTD(zstd))
        .set_dictionary_enabled(false)
        .set_statistics_enabled(EnabledStatistics::Chunk)
        .set_data_page_size_limit(DATA_PAGE_SIZE)
        .set_max_row_group_size(MAX_ROW_GROUP_ROWS)
        .set_column_dictionary_enabled(ColumnPath::from("object_name"), true)
        .set_column_encoding(
            ColumnPath::from("window_start_unix_nano"),
            Encoding::DELTA_BINARY_PACKED,
        )
        .set_column_encoding(
            ColumnPath::from("window_end_unix_nano"),
            Encoding::DELTA_BINARY_PACKED,
        )
        .build())
}

fn write_parquet(path: &Path, batch: RecordBatch) -> Result<(), String> {
    let file = File::create(path).map_err(|error| error.to_string())?;
    let sync_file = file.try_clone().map_err(|error| error.to_string())?;
    let mut writer = ArrowWriter::try_new(file, batch.schema(), Some(writer_properties()?))
        .map_err(|error| error.to_string())?;
    writer.write(&batch).map_err(|error| error.to_string())?;
    writer.close().map_err(|error| error.to_string())?;
    sync_file.sync_all().map_err(|error| error.to_string())
}

fn directory_bytes(path: &Path) -> io::Result<u64> {
    let mut total = 0u64;
    for entry in fs::read_dir(path)? {
        let entry = entry?;
        let metadata = fs::symlink_metadata(entry.path())?;
        total = total.saturating_add(if metadata.is_dir() && !metadata.file_type().is_symlink() {
            directory_bytes(&entry.path())?
        } else {
            metadata.blocks().saturating_mul(512)
        });
    }
    Ok(total)
}

fn sync_directory(path: &Path) -> io::Result<()> {
    File::open(path)?.sync_all()
}

fn available_bytes(path: &Path) -> Result<u64, String> {
    let path = std::ffi::CString::new(path.as_os_str().as_bytes())
        .map_err(|_| format!("path contains a NUL byte: {}", path.display()))?;
    let mut stats = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    let result = unsafe { libc::statvfs(path.as_ptr(), stats.as_mut_ptr()) };
    if result != 0 {
        return Err(io::Error::last_os_error().to_string());
    }
    let stats = unsafe { stats.assume_init() };
    Ok((stats.f_bavail as u64).saturating_mul(stats.f_frsize as u64))
}

fn write_shard(
    config: &LocalStorageConfig,
    sequence: u64,
    rows: ShardRows,
    committed_bytes: &mut u64,
) -> Result<(), String> {
    if rows.is_empty() {
        return Ok(());
    }
    if rows.estimated_uncompressed_bytes > MAX_SHARD_UNCOMPRESSED_BYTES {
        return Err(format!(
            "local storage shard estimate {} exceeds {} byte limit",
            rows.estimated_uncompressed_bytes, MAX_SHARD_UNCOMPRESSED_BYTES
        ));
    }
    let staging_root = config.root.join(".staging");
    let shards_root = config.root.join("shards");
    if committed_bytes.saturating_add(SHARD_RESERVE_BYTES) > config.max_bytes {
        return Err(format!(
            "local storage quota cannot reserve next shard: {} committed + {} reserve > {} bytes",
            committed_bytes, SHARD_RESERVE_BYTES, config.max_bytes
        ));
    }
    let available = available_bytes(&config.root)?;
    let required = SHARD_RESERVE_BYTES.saturating_add(FILESYSTEM_RESERVE_BYTES);
    if available < required {
        return Err(format!(
            "local storage filesystem has {} bytes available; {} required",
            available, required
        ));
    }

    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|error| error.to_string())?
        .as_nanos();
    let name = format!("{now:020}-{:010}-{sequence:06}", std::process::id());
    let staging = staging_root.join(&name);
    let destination = shards_root.join(&name);
    fs::create_dir(&staging).map_err(|error| error.to_string())?;
    let result = (|| {
        if !rows.gauges.is_empty() {
            write_parquet(&staging.join(GAUGE_FILE), gauge_batch(&rows.gauges)?)?;
        }
        if rows.dropped_input_messages != 0 || rows.dropped_shards != 0 {
            let loss = staging.join(LOSS_FILE);
            fs::write(
                &loss,
                format!(
                    "{{\"dropped_input_messages\":{},\"dropped_shards\":{}}}\n",
                    rows.dropped_input_messages, rows.dropped_shards
                ),
            )
            .map_err(|error| error.to_string())?;
            File::open(&loss)
                .and_then(|file| file.sync_all())
                .map_err(|error| error.to_string())?;
        }
        let ready = staging.join(READY_FILE);
        fs::write(&ready, FORMAT_VERSION).map_err(|error| error.to_string())?;
        File::open(&ready)
            .and_then(|file| file.sync_all())
            .map_err(|error| error.to_string())?;
        sync_directory(&staging).map_err(|error| error.to_string())?;

        let staged_bytes = directory_bytes(&staging).map_err(|error| error.to_string())?;
        let next_bytes = committed_bytes.saturating_add(staged_bytes);
        if next_bytes > config.max_bytes {
            return Err(format!(
                "local storage shard would exceed quota: {} > {} bytes",
                next_bytes, config.max_bytes
            ));
        }
        fs::rename(&staging, &destination).map_err(|error| error.to_string())?;
        sync_directory(&shards_root).map_err(|error| error.to_string())?;
        sync_directory(&staging_root).map_err(|error| error.to_string())?;
        *committed_bytes = next_bytes;
        info!("Published local HFT shard {}", destination.display());
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_dir_all(&staging);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::saistats::{SAIStat, SAIStatsBatch};
    use arrow_array::Array;
    use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;

    fn config(root: &Path) -> LocalStorageConfig {
        LocalStorageConfig {
            root: root.to_path_buf(),
            range_interval: Duration::from_millis(10),
            shard_interval: Duration::from_secs(5),
            max_bytes: 1_000_000_000,
            require_dedicated_filesystem: false,
        }
    }

    fn stat(name: &str, value: u64) -> SAIStat {
        SAIStat::new(name, 1, 2, value)
    }

    fn add(reducer: &mut LocalReducer, time: u64, stats: &[SAIStat]) {
        reducer
            .add_gauges(SAIStatsRef {
                observation_time: time,
                stats,
            })
            .unwrap();
    }

    fn shards(root: &Path) -> Vec<PathBuf> {
        fs::read_dir(root.join("shards"))
            .unwrap()
            .map(|entry| entry.unwrap().path())
            .collect()
    }

    fn read_gauges(shard: &Path) -> Vec<RecordBatch> {
        assert_eq!(
            fs::read_to_string(shard.join(READY_FILE)).unwrap(),
            FORMAT_VERSION
        );
        ParquetRecordBatchReaderBuilder::try_new(File::open(shard.join(GAUGE_FILE)).unwrap())
            .unwrap()
            .build()
            .unwrap()
            .map(Result::unwrap)
            .collect()
    }

    #[test]
    fn summarizes_monotonic_range_and_decrease() {
        let mut reducer = LocalReducer::new(&config(Path::new("")));
        for (time, value) in [(1, 10), (101, 15), (201, 12), (10_000_000, 20)] {
            add(&mut reducer, time, &[stat("Ethernet0", value)]);
        }
        assert_eq!(reducer.rows.gauges.len(), 1);
        let row = &reducer.rows.gauges[0].state;
        assert_eq!(row.first_value, 10);
        assert_eq!(row.previous_value, None);
        assert_eq!(row.last_value, 12);
        assert_eq!(row.min_value, 10);
        assert_eq!(row.max_value, 15);
        assert_eq!(row.max_change, 5);
        assert_eq!(row.max_change_time_unix_nano, 101);
        assert_eq!(row.total_increase, 5);
        assert_eq!(row.sample_count, 3);
        assert_eq!(row.change_count, 2);
        assert_eq!(row.flags, RANGE_FLAG_DECREASED);
        let open = &reducer.states[0].1.range;
        assert_eq!(open.previous_value, Some(12));
        assert_eq!(open.total_increase, 8);
        assert_eq!(open.flags, RANGE_FLAG_GAP);
    }

    #[test]
    fn flat_batches_keep_identity_across_interleaved_and_reordered_layouts() {
        let mut reducer = LocalReducer::new(&config(Path::new("")));
        let mut first = SAIStatsBatch::default();
        first.push_record(1, [stat("Ethernet0", 10), stat("Ethernet4", 100)]);
        // Same length, different object and SAI IDs, at the same observation time.
        first.push_record(
            1,
            [
                stat("Ethernet8", 1000),
                SAIStat::new("Ethernet0", 2, 2, 500),
            ],
        );
        first.push_record(2, [stat("Ethernet4", 110), stat("Ethernet0", 20)]);
        let mut second = SAIStatsBatch::default();
        second.push_record(2, [SAIStat::new("Ethernet0", 1, 3, 900)]);
        second.push_record(3, [stat("Ethernet8", 1010)]);
        second.push_record(3, [stat("Ethernet0", 25)]);
        second.push_record(10_000_000, [stat("Ethernet0", 30), stat("Ethernet4", 120)]);
        for batch in [&first, &second] {
            for record in batch.iter() {
                reducer.add_gauges(record).unwrap();
            }
        }
        let rows = reducer.finish();
        assert_eq!(rows.gauges.len(), 7);
        let ethernet0: Vec<_> = rows
            .gauges
            .iter()
            .filter(|row| {
                row.key.object_name.as_ref() == "Ethernet0"
                    && row.key.type_id == 1
                    && row.key.stat_id == 2
            })
            .collect();
        assert_eq!(ethernet0.len(), 2);
        assert_eq!(ethernet0[0].state.sample_count, 3);
        assert_eq!(ethernet0[0].state.first_value, 10);
        assert_eq!(ethernet0[0].state.last_value, 25);
        assert_eq!(ethernet0[1].state.previous_value, Some(25));
        assert_eq!(ethernet0[1].state.total_increase, 5);
        let ethernet8 = rows
            .gauges
            .iter()
            .find(|row| row.key.object_name.as_ref() == "Ethernet8")
            .unwrap();
        assert_eq!(ethernet8.state.sample_count, 2);
        assert_eq!(ethernet8.state.first_value, 1000);
        assert_eq!(ethernet8.state.last_value, 1010);
        for (type_id, stat_id, value) in [(2, 2, 500), (1, 3, 900)] {
            let row = rows
                .gauges
                .iter()
                .find(|row| row.key.type_id == type_id && row.key.stat_id == stat_id)
                .unwrap();
            assert_eq!(row.state.first_value, value);
            assert_eq!(row.state.sample_count, 1);
        }
    }

    #[test]
    fn backwards_samples_split_flagged_ranges_without_losing_samples() {
        let mut reducer = LocalReducer::new(&config(Path::new("")));
        add(&mut reducer, 100, &[stat("Ethernet0", 10)]);
        add(&mut reducer, 50, &[stat("Ethernet4", 100)]);
        add(&mut reducer, 99, &[stat("Ethernet0", 99)]);
        add(&mut reducer, 100, &[stat("Ethernet0", 99)]);
        add(&mut reducer, 101, &[stat("Ethernet0", 20)]);
        let rows = reducer.finish();
        assert_eq!(rows.gauges.len(), 3);
        assert_eq!(rows.gauges[0].state.last_value, 10);
        assert_eq!(rows.gauges[0].state.sample_count, 1);
        assert_eq!(rows.gauges[0].state.flags, RANGE_FLAG_TIME_REGRESSED);
        assert_eq!(rows.gauges[1].state.first_value, 99);
        assert_eq!(rows.gauges[1].state.last_value, 20);
        assert_eq!(rows.gauges[1].state.sample_count, 3);
        assert_eq!(rows.gauges[1].state.previous_value, None);
        assert_eq!(
            rows.gauges[1].state.flags,
            RANGE_FLAG_TIME_REGRESSED | RANGE_FLAG_DECREASED
        );
        assert_eq!(rows.gauges[2].state.first_value, 100);
        assert_eq!(rows.gauges[2].state.flags, 0);
    }

    #[test]
    fn equal_timestamps_count_every_value_once_across_layout_changes() {
        let mut reducer = LocalReducer::new(&config(Path::new("")));
        add(&mut reducer, 42, &[stat("Ethernet0", 10)]);
        add(&mut reducer, 42, &[stat("Ethernet0", 20)]);
        add(
            &mut reducer,
            42,
            &[stat("Ethernet4", 100), stat("Ethernet0", 15)],
        );
        add(&mut reducer, 42, &[stat("Ethernet0", 15)]);
        assert_eq!(reducer.states[0].1.expected_interval_ns, None);
        add(&mut reducer, 52, &[stat("Ethernet0", 30)]);
        assert_eq!(reducer.states[0].1.expected_interval_ns, Some(10));
        let rows = reducer.finish();
        let range = &rows.gauges[0].state;
        assert_eq!(range.sample_count, 5);
        assert_eq!(range.change_count, 3);
        assert_eq!(range.first_value, 10);
        assert_eq!(range.last_value, 30);
        assert_eq!(range.min_value, 10);
        assert_eq!(range.max_value, 30);
        assert_eq!(range.total_increase, 25);
        assert_eq!(range.flags, RANGE_FLAG_DECREASED);
        assert_eq!(rows.gauges[1].state.sample_count, 1);
    }

    #[test]
    fn backwards_window_starts_without_a_previous_value_bridge() {
        let mut reducer = LocalReducer::new(&config(Path::new("")));
        add(&mut reducer, 20_000_000, &[stat("Ethernet0", 10)]);
        add(&mut reducer, 1, &[stat("Ethernet0", 100)]);
        add(&mut reducer, 1, &[stat("Ethernet0", 110)]);
        let rows = reducer.finish();
        assert_eq!(rows.gauges.len(), 2);
        assert_eq!(rows.gauges[0].window_start_unix_nano, 20_000_000);
        assert_eq!(rows.gauges[1].window_start_unix_nano, 0);
        assert_eq!(rows.gauges[1].state.previous_value, None);
        assert_eq!(rows.gauges[1].state.sample_count, 2);
        assert_eq!(rows.gauges[1].state.total_increase, 10);
        assert!(rows
            .gauges
            .iter()
            .all(|row| row.state.flags == RANGE_FLAG_TIME_REGRESSED));
    }

    #[test]
    fn drops_remain_flagged_after_source_size_and_wall_flushes() {
        let mut reducer = LocalReducer::new(&config(Path::new("")));
        add(&mut reducer, 1, &[stat("Ethernet0", 10)]);
        add(&mut reducer, 10_000_000, &[stat("Ethernet0", 20)]);
        // Atomic loss may be observed before processing the affected queued batch.
        reducer.mark_input_drop(1);
        let rows = reducer.take_shard(Some(10_000_000));
        assert_eq!(rows.dropped_input_messages, 1);
        assert_eq!(rows.gauges[0].state.flags, RANGE_FLAG_STORAGE_DROP);
        add(&mut reducer, 20_000_000, &[stat("Ethernet0", 30)]);
        let rows = reducer.finish();
        assert_eq!(rows.dropped_input_messages, 0);
        assert!(rows
            .gauges
            .iter()
            .all(|row| row.state.flags & RANGE_FLAG_STORAGE_DROP != 0));
        add(&mut reducer, 30_000_000, &[stat("Ethernet4", 100)]);
        let rows = reducer.finish();
        assert_eq!(rows.gauges[0].state.flags, RANGE_FLAG_STORAGE_DROP);
    }

    #[test]
    fn source_rotations_do_not_postpone_wall_flush_of_inactive_series() {
        let config = config(Path::new(""));
        let mut reducer = LocalReducer::new(&config);
        let (sender, receiver) = mpsc::sync_channel(1);
        // Keep the wall deadline explicitly pending, independent of test runtime.
        let started = Instant::now() + Duration::from_secs(3600);
        let mut wall = started;
        add(&mut reducer, 1, &[stat("inactive", 10), stat("active", 20)]);
        for time in [5_000_000_001, 10_000_000_001, 15_000_000_001] {
            add(&mut reducer, time, &[stat("active", 30)]);
            reducer
                .rotate(Some(time), &mut wall, config.shard_interval, &sender)
                .unwrap();
            assert_eq!(wall, started);
            assert!(receiver.try_recv().unwrap().gauges.iter().all(|row| row
                .key
                .object_name
                .as_ref()
                == "active"));
        }
        wall = Instant::now() - config.shard_interval;
        reducer
            .rotate(
                Some(20_000_000_001),
                &mut wall,
                config.shard_interval,
                &sender,
            )
            .unwrap();
        let rows = receiver.try_recv().unwrap();
        assert_eq!(rows.dropped_shards, 0);
        assert_eq!(rows.gauges.len(), 2);
        assert!(rows
            .gauges
            .iter()
            .any(|row| row.key.object_name.as_ref() == "inactive"));
        assert!(reducer.states.is_empty());
    }

    #[test]
    fn record_crossing_capacity_flushes_existing_data_and_continues() {
        let mut reducer = LocalReducer::new(&config(Path::new("")));
        let (sender, receiver) = mpsc::sync_channel(8);
        add(&mut reducer, 1, &[stat("existing", 10)]);
        // Simulate a nearly full shard without allocating 120 MB for this test.
        reducer.estimated_state_bytes = SHARD_ROTATE_UNCOMPRESSED_BYTES - 1;
        // The next valid record would cross both the 120 MB target and 128 MB
        // limit if appended atomically to the simulated current allocation.
        let name =
            "x".repeat((MAX_SHARD_UNCOMPRESSED_BYTES - SHARD_ROTATE_UNCOMPRESSED_BYTES) as usize);
        let stats = [stat(&name, 20), stat("Ethernet4", 30)];
        reducer
            .add_record(
                SAIStatsRef {
                    observation_time: 2,
                    stats: &stats,
                },
                &sender,
            )
            .unwrap();
        let flushed = receiver.try_recv().unwrap();
        assert_eq!(flushed.gauges[0].key.object_name.as_ref(), "existing");
        assert_eq!(flushed.gauges[0].state.sample_count, 1);
        assert!(flushed.estimated_uncompressed_bytes <= MAX_SHARD_UNCOMPRESSED_BYTES);
        let rows = reducer.finish();
        assert_eq!(rows.gauges.len(), 2);
        assert!(rows.gauges.iter().all(|row| row.state.sample_count == 1));
    }

    #[test]
    fn idle_wall_deadline_queues_open_ranges_without_new_source_data() {
        let config = config(Path::new(""));
        let mut reducer = LocalReducer::new(&config);
        let (sender, receiver) = mpsc::sync_channel(1);
        add(&mut reducer, 1, &[stat("inactive", 10)]);

        let mut wall = Instant::now() + Duration::from_secs(3600);
        reducer
            .rotate(None, &mut wall, config.shard_interval, &sender)
            .unwrap();
        assert!(matches!(
            receiver.try_recv(),
            Err(mpsc::TryRecvError::Empty)
        ));
        assert_eq!(reducer.states.len(), 1);

        // Exercise the receive-timeout path without sleeping or racing a writer.
        wall = Instant::now() - config.shard_interval;
        let before_flush = Instant::now();
        reducer
            .rotate(None, &mut wall, config.shard_interval, &sender)
            .unwrap();
        assert!(wall >= before_flush);
        let rows = receiver.try_recv().unwrap();
        assert_eq!(rows.dropped_shards, 0);
        assert_eq!(rows.gauges.len(), 1);
        assert_eq!(rows.gauges[0].key.object_name.as_ref(), "inactive");
        assert_eq!(rows.gauges[0].state.last_value, 10);
        assert_eq!(rows.gauges[0].state.sample_count, 1);
        assert!(reducer.states.is_empty());
        assert!(reducer.rows.is_empty());

        wall = Instant::now() - config.shard_interval;
        reducer
            .rotate(None, &mut wall, config.shard_interval, &sender)
            .unwrap();
        assert!(matches!(
            receiver.try_recv(),
            Err(mpsc::TryRecvError::Empty)
        ));
    }

    #[test]
    fn setup_failure_marks_status_and_disconnects_the_bounded_queue() {
        let temp = tempfile::tempdir().unwrap();
        let mut config = config(temp.path());
        config.root = temp.path().join("missing");
        let (sender, receiver) = mpsc::sync_channel(1);
        let status = LocalStorageStatus::default();
        assert!(LocalStorageActor::new(receiver, config, status.clone()).is_err());
        assert!(status.failed());
        assert!(matches!(
            sender.try_send(Arc::new(SAIStatsBatch::default())),
            Err(mpsc::TrySendError::Disconnected(_))
        ));
    }

    #[test]
    fn oversized_record_preflight_is_atomic() {
        let mut reducer = LocalReducer::new(&config(Path::new("")));
        add(&mut reducer, 1, &[stat("existing", 10)]);
        reducer.estimated_state_bytes = MAX_SHARD_UNCOMPRESSED_BYTES - 1;
        assert!(reducer
            .add_gauges(SAIStatsRef {
                observation_time: 20_000_000,
                stats: &[stat("existing", 20), stat("new", 30)],
            })
            .is_err());
        assert_eq!(reducer.states.len(), 1);
        assert_eq!(reducer.states[0].1.range.last_value, 10);
        assert!(reducer.rows.is_empty());
    }

    #[test]
    fn actor_drains_flat_batches_and_preserves_u64_in_published_parquet() {
        let temp = tempfile::tempdir().unwrap();
        let (sender, receiver) = mpsc::sync_channel(8);
        let status = LocalStorageStatus::default();
        let actor = LocalStorageActor::new(receiver, config(temp.path()), status.clone()).unwrap();
        let mut batch = SAIStatsBatch::default();
        batch.push_record(1, [stat("Ethernet0", (1u64 << 63) + 1)]);
        batch.push_record(2, [stat("Ethernet4", (1u64 << 53) + 1)]);
        batch.push_record(3, [stat("Ethernet0", u64::MAX)]);
        batch.push_record(3, [stat("Ethernet0", u64::MAX)]);
        sender.send(Arc::new(batch)).unwrap();
        let mut batch = SAIStatsBatch::default();
        batch.push_record(10_000_000, [stat("Ethernet0", 0)]);
        sender.send(Arc::new(batch)).unwrap();
        // Include drops recorded after the last successful enqueue.
        status.record_input_drop();
        drop(sender);
        actor.run();
        assert!(!status.failed());
        let shards = shards(temp.path());
        assert_eq!(shards.len(), 1);
        assert_eq!(
            fs::read_dir(temp.path().join(".staging")).unwrap().count(),
            0
        );
        let batches = read_gauges(&shards[0]);
        assert_eq!(batches.iter().map(RecordBatch::num_rows).sum::<usize>(), 3);
        let batch = &batches[0];
        let first = batch
            .column_by_name("first_value")
            .unwrap()
            .as_any()
            .downcast_ref::<UInt64Array>()
            .unwrap();
        let last = batch
            .column_by_name("last_value")
            .unwrap()
            .as_any()
            .downcast_ref::<UInt64Array>()
            .unwrap();
        let previous = batch
            .column_by_name("previous_value")
            .unwrap()
            .as_any()
            .downcast_ref::<UInt64Array>()
            .unwrap();
        let change = batch
            .column_by_name("max_change")
            .unwrap()
            .as_any()
            .downcast_ref::<UInt64Array>()
            .unwrap();
        assert_eq!(first.value(0), (1u64 << 63) + 1);
        assert_eq!(last.value(0), u64::MAX);
        assert!(previous.is_null(0));
        assert_eq!(previous.value(1), u64::MAX);
        assert_eq!(change.value(1), u64::MAX);
        assert_eq!(first.value(2), (1u64 << 53) + 1);
        let counts = batch
            .column_by_name("sample_count")
            .unwrap()
            .as_any()
            .downcast_ref::<UInt32Array>()
            .unwrap();
        assert_eq!(counts.values().as_ref(), &[3, 1, 1]);
        assert_eq!(
            fs::read_to_string(shards[0].join(LOSS_FILE)).unwrap(),
            "{\"dropped_input_messages\":1,\"dropped_shards\":0}\n"
        );
    }

    #[test]
    fn shutdown_request_flushes_without_waiting_for_sender_disconnect() {
        let temp = tempfile::tempdir().unwrap();
        let (sender, receiver) = mpsc::sync_channel(8);
        let status = LocalStorageStatus::default();
        let actor = LocalStorageActor::new(receiver, config(temp.path()), status.clone()).unwrap();
        let mut batch = SAIStatsBatch::default();
        batch.push_record(1, [stat("Ethernet0", 10)]);
        let batch = Arc::new(batch);
        let consumed = Arc::downgrade(&batch);
        sender.send(batch).unwrap();
        let (done_sender, done_receiver) = mpsc::channel();
        let handle = thread::spawn(move || {
            actor.run();
            done_sender.send(()).unwrap();
        });
        let deadline = Instant::now() + Duration::from_secs(5);
        while consumed.strong_count() != 0 {
            assert!(Instant::now() < deadline, "actor did not consume the batch");
            thread::sleep(Duration::from_millis(1));
        }
        status.record_input_drop();
        status.request_shutdown();
        done_receiver.recv_timeout(Duration::from_secs(5)).unwrap();
        handle.join().unwrap();
        assert!(!status.failed());
        let shards = shards(temp.path());
        assert_eq!(shards.len(), 1);
        assert_eq!(read_gauges(&shards[0])[0].num_rows(), 1);
        drop(sender);
    }

    #[test]
    fn drop_tracker_marks_existing_and_new_ranges() {
        let mut reducer = LocalReducer::new(&config(Path::new("")));
        add(&mut reducer, 1, &[stat("Ethernet0", 10)]);
        reducer.mark_input_drop(3);
        add(&mut reducer, 2, &[stat("Ethernet4", 20)]);
        reducer.mark_input_drop(2);
        let rows = reducer.finish();
        assert_eq!(rows.dropped_input_messages, 5);
        assert_eq!(rows.gauges.len(), 2);
        assert!(rows
            .gauges
            .iter()
            .all(|row| row.state.flags == RANGE_FLAG_STORAGE_DROP));
    }

    #[test]
    fn full_writer_queue_carries_loss_to_next_shard() {
        let (sender, receiver) = mpsc::sync_channel(1);
        let mut reducer = LocalReducer::new(&config(Path::new("")));
        sender.send(ShardRows::default()).unwrap();
        let lost = ShardRows {
            dropped_input_messages: 7,
            dropped_shards: 2,
            ..Default::default()
        };
        queue_shard(&sender, lost, &mut reducer).unwrap();
        assert!(reducer.writer_drop_warned);
        let next = reducer.finish();
        assert!(reducer.writer_drop_warned);
        assert_eq!(next.dropped_input_messages, 7);
        assert_eq!(next.dropped_shards, 3);
        drop(receiver);
        assert!(queue_shard(&sender, next, &mut reducer).is_err());
    }

    #[test]
    fn rejects_second_writer_and_cleans_only_staging_after_lock() {
        let temp = tempfile::tempdir().unwrap();
        let config = config(temp.path());
        let first = prepare_storage(&config).unwrap();
        fs::create_dir(temp.path().join(".staging/incomplete")).unwrap();
        fs::write(temp.path().join(".staging/incomplete/partial"), b"data").unwrap();
        fs::create_dir(temp.path().join("shards/keep")).unwrap();
        fs::write(temp.path().join("shards/keep/_READY"), FORMAT_VERSION).unwrap();
        assert!(prepare_storage(&config).is_err());
        assert!(temp.path().join(".staging/incomplete/partial").exists());
        drop(first);
        let _second = prepare_storage(&config).unwrap();
        assert_eq!(
            fs::read_dir(temp.path().join(".staging")).unwrap().count(),
            0
        );
        assert!(temp.path().join("shards/keep/_READY").exists());
    }

    #[test]
    fn rejects_symlink_roots_directories_and_locks() {
        use std::os::unix::fs::symlink;
        let temp = tempfile::tempdir().unwrap();
        let outside = tempfile::tempdir().unwrap();
        fs::write(outside.path().join("keep"), b"untouched").unwrap();
        for name in ["root", ".staging", "shards", LOCK_FILE] {
            if temp.path().join(LOCK_FILE).exists() {
                fs::remove_file(temp.path().join(LOCK_FILE)).unwrap();
            }
            let target = if name == LOCK_FILE {
                outside.path().join("keep")
            } else {
                outside.path().to_path_buf()
            };
            let link = temp.path().join(name);
            symlink(&target, &link).unwrap();
            let root = if name == "root" { &link } else { temp.path() };
            assert!(prepare_storage(&config(root)).is_err(), "{name}");
            fs::remove_file(&link).unwrap();
            // A failed shards check may already have created .staging.
            if temp.path().join(".staging").is_dir() {
                fs::remove_dir(temp.path().join(".staging")).unwrap();
            }
        }
        assert_eq!(fs::read(outside.path().join("keep")).unwrap(), b"untouched");
    }

    #[test]
    fn validates_limits_and_dedicated_mount_policy() {
        let temp = tempfile::tempdir().unwrap();
        let mut config = config(temp.path());
        config.require_dedicated_filesystem = true;
        assert!(config.validate_root().is_err());
        config.root = PathBuf::from("/");
        assert!(config.validate_root().is_err());
        config = self::config(temp.path());
        config.range_interval = Duration::ZERO;
        assert!(config.validate().is_err());
        config.range_interval = Duration::from_secs(10);
        assert!(config.validate().is_err());
        config.range_interval = Duration::from_millis(10);
        config.max_bytes = SHARD_RESERVE_BYTES;
        assert!(config.validate().is_err());
    }

    #[test]
    fn quota_and_estimate_limits_prevent_publication() {
        let temp = tempfile::tempdir().unwrap();
        let mut config = config(temp.path());
        config.max_bytes = SHARD_RESERVE_BYTES + 1;
        let _lock = prepare_storage(&config).unwrap();
        let mut committed_bytes = 2;
        assert!(write_shard(
            &config,
            0,
            ShardRows {
                dropped_input_messages: 1,
                ..Default::default()
            },
            &mut committed_bytes
        )
        .is_err());
        assert_eq!(committed_bytes, 2);
        assert!(write_shard(
            &config,
            1,
            ShardRows {
                dropped_input_messages: 1,
                estimated_uncompressed_bytes: MAX_SHARD_UNCOMPRESSED_BYTES + 1,
                ..Default::default()
            },
            &mut 0
        )
        .is_err());
        assert!(shards(temp.path()).is_empty());
        assert_eq!(
            fs::read_dir(temp.path().join(".staging")).unwrap().count(),
            0
        );
    }

    #[test]
    fn reducer_bounds_new_identity_state() {
        let mut reducer = LocalReducer::new(&config(Path::new("")));
        reducer.estimated_state_bytes = MAX_SHARD_UNCOMPRESSED_BYTES;
        assert!(reducer
            .add_gauges(SAIStatsRef {
                observation_time: 1,
                stats: &[stat("Ethernet0", 10)],
            })
            .is_err());
        assert!(reducer.states.is_empty());
    }

    #[test]
    fn writer_failure_sets_status_without_publication() {
        let temp = tempfile::tempdir().unwrap();
        let config = config(temp.path());
        let lock = prepare_storage(&config).unwrap();
        let (sender, receiver) = mpsc::sync_channel(1);
        let status = LocalStorageStatus::default();
        // Deterministic failure independent of host disk free space or uid.
        sender
            .send(ShardRows {
                dropped_input_messages: 1,
                estimated_uncompressed_bytes: MAX_SHARD_UNCOMPRESSED_BYTES + 1,
                ..Default::default()
            })
            .unwrap();
        writer_loop(config, receiver, status.clone(), lock);
        assert!(status.failed());
        assert!(shards(temp.path()).is_empty());
    }

    #[test]
    fn failed_publication_removes_staging_and_does_not_advance_quota() {
        let temp = tempfile::tempdir().unwrap();
        let config = config(temp.path());
        let _lock = prepare_storage(&config).unwrap();
        fs::remove_dir(temp.path().join("shards")).unwrap();
        fs::write(temp.path().join("shards"), b"not a directory").unwrap();
        let mut committed_bytes = 0;
        assert!(write_shard(
            &config,
            0,
            ShardRows {
                dropped_input_messages: 1,
                ..Default::default()
            },
            &mut committed_bytes
        )
        .is_err());
        assert_eq!(committed_bytes, 0);
        assert_eq!(
            fs::read_dir(temp.path().join(".staging")).unwrap().count(),
            0
        );
        assert_eq!(
            fs::read(temp.path().join("shards")).unwrap(),
            b"not a directory"
        );
    }

    #[test]
    fn writes_loss_only_sidecar_and_accounts_allocated_bytes() {
        let temp = tempfile::tempdir().unwrap();
        let config = config(temp.path());
        let _lock = prepare_storage(&config).unwrap();
        let mut committed_bytes = directory_bytes(temp.path()).unwrap();
        write_shard(
            &config,
            0,
            ShardRows {
                dropped_input_messages: 7,
                dropped_shards: 2,
                ..Default::default()
            },
            &mut committed_bytes,
        )
        .unwrap();
        let shards = shards(temp.path());
        assert_eq!(shards.len(), 1);
        assert!(shards[0].join(READY_FILE).is_file());
        assert!(!shards[0].join(GAUGE_FILE).exists());
        assert_eq!(
            fs::read_to_string(shards[0].join(LOSS_FILE)).unwrap(),
            "{\"dropped_input_messages\":7,\"dropped_shards\":2}\n"
        );
        assert_eq!(committed_bytes, directory_bytes(temp.path()).unwrap());
    }
}
