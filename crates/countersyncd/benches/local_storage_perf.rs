//! Disk-backed, non-Criterion benchmark. Run with --help for the CLI.
//!
//! Each trial sums independently drained segments of at most 6000 records and
//! 120 MB of prebuilt input (including conservative allocation overhead). Larger
//! --records does NOT replay a payload pool: absolute sequence numbers continue.
//! Generation, template installation and Parquet verification are untimed; each
//! segment times enqueue/send through reducer/writer join and final publish fsync.
//! Segment boundaries add finalization cost, not an uninterrupted streaming run.
//! Range/shard intervals are 10 ms/5 s. Pacing uses wall time, independently of
//! --step-ns source time. Default pacing is 1M metrics/s; 0 measures overload.
//!
//! large33 selects index % 3 == 0; large66 selects index % 3 < 2. At 500
//! counters these are 167/500 (33.4%) and 334/500 (66.8%), not rounded percentages.
//! Selected series use independent SplitMix64-derived full-width values; others
//! increment by one. constant keeps every series exactly unchanged, conservatively
//! exercising maximum compression. No cache eviction or cold-disk claim is made.
//!
//! UDP includes a bounded audit recipient (64 batches), hashing every decoded
//! value and checking unique timestamps. Its scheduling/backpressure is measured.
//! There is no readiness probe: a capacity-one template channel reserve barrier
//! precedes data. Its sender stays alive until the actor drains the closed input.
//! UDP loss completion includes the 250 ms receiver idle drain timeout in seconds;
//! it is not subtracted from end-to-end timing. Only paced lossless results are
//! headline measurements; overload observations make no capacity claim.
//! Only published _READY shards count as writes. Readback checks identities,
//! sample counts and per-row first/last/min/max against deterministic input.
//! In a storage-loss row, missing interior samples cannot be reconstructed from
//! the best-effort tap: report partial verification, never claim losslessness.
//!
//! An independent watchdog bounds each segment, including blocking joins. On a
//! hard timeout/quota violation it emits JSON and exits; the reported private run
//! directory may remain for inspection/removal. Normal/error unwinding cleans it.
//! Disk peaks are sampled every 10 ms (plus final usage), not exact high-water
//! marks. Production quota 2.4 GB plus its 512 MB staging reserve is below 3 GB.
//! Segments are deleted before preparing the next one. Finalized allocated bytes
//! sum published shard allocations across segments, not physical write amplification.

#![recursion_limit = "256"]

use std::{
    collections::HashSet,
    ffi::CString,
    fs::{self, File},
    io::{self, Write},
    os::unix::{ffi::OsStrExt, fs::MetadataExt},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc, Arc,
    },
    thread,
    time::{Duration, Instant},
};

use arrow_array::{RecordBatch, StringArray, UInt32Array, UInt64Array};
use byteorder::{ByteOrder, NetworkEndian};
use clap::{Parser, ValueEnum};
use countersyncd::{
    actor::{
        ipfix::IpfixActor,
        local_storage::{LocalStorageActor, LocalStorageConfig},
    },
    message::{
        ipfix::IPFixTemplatesMessage,
        local_storage::LocalStorageStatus,
        saistats::{SAIStat, SAIStatsBatch, SAIStatsBatchMessage},
    },
};
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;
use serde_json::json;
use tokio::{net::UdpSocket, runtime::Builder, sync::mpsc as channel};

#[path = "../tests/ipfix_test_helpers.rs"]
mod helpers;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>;
const RANGE_NS: u64 = 10_000_000;
const QUOTA: u64 = 2_400_000_000;
const INPUT_BUDGET: usize = 120_000_000;
const ID: u16 = 300;
const STORAGE_QUEUE_BATCHES: usize = 32;
const UDP_IDLE_TIMEOUT_MS: u64 = 250;

#[derive(Clone, Copy, Debug, ValueEnum)]
enum Mode {
    Standalone,
    Udp,
    All,
}

#[derive(Parser, Clone)]
#[command(about = "Verified local-storage metrics/s; JSONL on stdout", long_about = None)]
struct Args {
    /// Existing real-disk parent for private temporary trial directories (no tmpfs).
    #[arg(long)]
    root: PathBuf,
    #[arg(long, default_value_t = 6000)]
    records: usize,
    #[arg(long, default_value_t = 500)]
    counters: usize,
    #[arg(long, default_value_t = 3)]
    repeats: usize,
    #[arg(long, value_enum, default_value_t = Mode::All)]
    mode: Mode,
    /// Offered metrics/s; zero is unpaced. Standalone paces batches, UDP datagrams.
    #[arg(long, default_value_t = 1_000_000)]
    rate: u64,
    #[arg(long, default_value_t = 10_000)]
    step_ns: u64,
    /// Hard per-segment deadline, including preparation/readback and blocking joins.
    #[arg(long, default_value_t = 120)]
    timeout_secs: u64,
    /// Print all trial accounting, then fail if any input was lost.
    #[arg(long)]
    require_lossless: bool,
    // cargo bench passes this even though the executable is not a libtest harness.
    #[arg(long, hide = true)]
    bench: bool,
}

#[derive(Clone, Copy, Debug)]
enum Pattern {
    Constant,
    Large100,
    Large33,
    Large66,
}
impl Pattern {
    fn name(self) -> &'static str {
        match self {
            Self::Constant => "constant",
            Self::Large100 => "large100",
            Self::Large33 => "large33",
            Self::Large66 => "large66",
        }
    }
    fn large(self, index: usize) -> bool {
        match self {
            Self::Constant => false,
            Self::Large100 => true,
            Self::Large33 => index % 3 == 0,
            Self::Large66 => index % 3 < 2,
        }
    }
    fn value(self, sequence: usize, index: usize) -> u64 {
        if self.large(index) {
            mix((sequence as u64).wrapping_mul(0x9e3779b97f4a7c15) ^ mix(index as u64 + 0x12345678))
        } else {
            (index as u64 + 1) * 1_000_000
                + if matches!(self, Self::Constant) {
                    0
                } else {
                    sequence as u64
                }
        }
    }
}

fn mix(mut x: u64) -> u64 {
    x = (x ^ (x >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)).wrapping_mul(0x94d049bb133111eb);
    x ^ (x >> 31)
}

fn hash_value(hash: u64, value: u64) -> u64 {
    hash.rotate_left(7) ^ mix(value)
}

#[derive(Default)]
struct Counts {
    sent: AtomicU64,
    received: AtomicU64,
    decoded: AtomicU64,
}

// Include directory blocks and staging; tolerate concurrent rename/removal only.
fn allocated(path: &Path, seen: &mut HashSet<(u64, u64)>) -> io::Result<u64> {
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(0),
        Err(error) => return Err(error),
    };
    // A staging inode can appear again under shards after a concurrent publish.
    if !seen.insert((metadata.dev(), metadata.ino())) {
        return Ok(0);
    }
    let mut bytes = metadata.blocks() * 512;
    if metadata.is_dir() {
        let entries = match fs::read_dir(path) {
            Ok(entries) => entries,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(0),
            Err(error) => return Err(error),
        };
        for entry in entries {
            bytes += allocated(&entry?.path(), seen)?;
        }
    }
    Ok(bytes)
}

struct Watchdog {
    stop: mpsc::Sender<()>,
    handle: Option<thread::JoinHandle<()>>,
    peak: Arc<AtomicU64>,
}
impl Watchdog {
    fn new(root: &Path, args: &Args, counts: Arc<Counts>) -> Self {
        let (stop, rx) = mpsc::channel();
        let peak = Arc::new(AtomicU64::new(0));
        let high = peak.clone();
        let root = root.to_path_buf();
        let deadline = Duration::from_secs(args.timeout_secs);
        let counters = args.counters;
        let handle = thread::spawn(move || {
            let start = Instant::now();
            while rx.recv_timeout(Duration::from_millis(10)).is_err() {
                let disk = allocated(&root, &mut HashSet::new());
                if let Ok(bytes) = &disk {
                    high.fetch_max(*bytes, Ordering::Relaxed);
                }
                let reason = if start.elapsed() >= deadline {
                    Some("timeout")
                } else if disk.is_err() {
                    Some("disk_accounting_error")
                } else if high.load(Ordering::Relaxed) >= 2_950_000_000 {
                    Some("disk_limit")
                } else {
                    None
                };
                if let Some(reason) = reason {
                    println!(
                        "{}",
                        json!({"event":"aborted_segment", "status":reason,
                        "root":root, "seconds":start.elapsed().as_secs_f64(),
                        "sent_metrics":counts.sent.load(Ordering::Relaxed) * counters as u64,
                        "received_metrics":counts.received.load(Ordering::Relaxed) * counters as u64,
                        "decoded_metrics":counts.decoded.load(Ordering::Relaxed),
                        "persisted_metrics":null, "disk_allocated_peak_sampled":high.load(Ordering::Relaxed)})
                    );
                    let _ = io::stdout().flush();
                    std::process::exit(124);
                }
            }
        });
        Self {
            stop,
            handle: Some(handle),
            peak,
        }
    }
}
impl Drop for Watchdog {
    fn drop(&mut self) {
        let _ = self.stop.send(());
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

fn pace(start: Instant, metrics: usize, rate: u64) {
    if rate == 0 {
        return;
    }
    let target = Duration::from_secs_f64(metrics as f64 / rate as f64);
    if let Some(delay) = target.checked_sub(start.elapsed()) {
        thread::sleep(delay);
    }
}

#[derive(Default)]
struct Audit {
    hashes: Vec<Option<u64>>,
    errors: u64,
}

async fn udp(
    args: &Args,
    pattern: Pattern,
    offset: usize,
    count: usize,
    storage: mpsc::SyncSender<SAIStatsBatchMessage>,
    status: LocalStorageStatus,
    counts: Arc<Counts>,
) -> Result<(Instant, Audit)> {
    let template = helpers::generate_ipfix_templates(args.counters, ID);
    let base = helpers::generate_ipfix_records(&template);
    let payloads: Vec<_> = (offset..offset + count)
        .map(|sequence| {
            let mut bytes = base.clone();
            NetworkEndian::write_u32(&mut bytes[8..12], sequence as u32);
            NetworkEndian::write_u64(&mut bytes[20..28], (sequence as u64 + 1) * args.step_ns);
            for (index, field) in bytes[28..].chunks_exact_mut(8).enumerate() {
                NetworkEndian::write_u64(field, pattern.value(sequence, index));
            }
            bytes
        })
        .collect();
    let (names, ids) = helpers::generate_object_metadata(args.counters);
    let (template_tx, template_rx) = channel::channel(1);
    let (input_tx, input_rx) = channel::channel(256);
    let (audit_tx, mut audit_rx) = channel::channel::<SAIStatsBatchMessage>(64);
    let mut actor = IpfixActor::new(template_rx, input_rx);
    actor.set_local_storage_recipient(storage, status);
    actor.add_recipient(audit_tx);
    let actor = tokio::spawn(IpfixActor::run(actor));
    template_tx
        .send(IPFixTemplatesMessage::new(
            "local-storage-bench".into(),
            Arc::new(template),
            Some(names.clone()),
            Some(ids),
        ))
        .await?;
    // The actor handles the received template synchronously before selecting data.
    drop(template_tx.reserve().await?);

    let receiver = UdpSocket::bind("127.0.0.1:0").await?;
    let sender = std::net::UdpSocket::bind("127.0.0.1:0")?;
    sender.connect(receiver.local_addr()?)?;
    sender.set_write_timeout(Some(Duration::from_secs(1)))?;
    let complete = Arc::new(AtomicBool::new(false));
    let sender_done = complete.clone();
    let send_counts = counts.clone();
    let rate = args.rate;
    let counters = args.counters;
    let step = args.step_ns;
    let decode_counts = counts.clone();
    let collector = tokio::spawn(async move {
        let mut audit = Audit {
            hashes: vec![None; count],
            errors: 0,
        };
        while let Some(batch) = audit_rx.recv().await {
            decode_counts
                .decoded
                .fetch_add(batch.counter_count() as u64, Ordering::Relaxed);
            for record in batch.iter() {
                let index = (record.observation_time / step)
                    .checked_sub(1)
                    .and_then(|seq| (seq as usize).checked_sub(offset));
                let Some(index) = index.filter(|i| *i < count) else {
                    audit.errors += 1;
                    continue;
                };
                if record.observation_time % step != 0 || record.stats.len() != counters {
                    audit.errors += 1;
                }
                let mut hash = record.observation_time;
                for (i, stat) in record.stats.iter().enumerate() {
                    if i >= counters
                        || stat.type_id != i as u32 + 1
                        || stat.stat_id != i as u32 + 1
                        || stat.object_name.as_ref() != names[i]
                    {
                        audit.errors += 1;
                    }
                    hash = hash_value(hash, stat.counter);
                }
                if audit.hashes[index].replace(hash).is_some() {
                    audit.errors += 1;
                }
            }
        }
        audit
    });
    let start = Instant::now();
    let send = thread::spawn(move || -> io::Result<()> {
        let result = (|| {
            for (i, payload) in payloads.iter().enumerate() {
                pace(start, i * counters, rate);
                if sender.send(payload)? != payload.len() {
                    return Err(io::Error::other("short UDP send"));
                }
                send_counts.sent.fetch_add(1, Ordering::Relaxed);
            }
            Ok(())
        })();
        sender_done.store(true, Ordering::Release);
        result
    });
    let mut buffer = vec![0u8; 65536];
    loop {
        // Stop immediately on complete counts; on loss, drain until sender-complete
        // AND an idle receive. The independent watchdog is the absolute deadline.
        if counts.received.load(Ordering::Relaxed) >= count as u64
            || (complete.load(Ordering::Acquire)
                && counts.received.load(Ordering::Relaxed) >= counts.sent.load(Ordering::Relaxed))
        {
            break;
        }
        match tokio::time::timeout(
            Duration::from_millis(UDP_IDLE_TIMEOUT_MS),
            receiver.recv(&mut buffer),
        )
        .await
        {
            Ok(Ok(length)) => {
                counts.received.fetch_add(1, Ordering::Relaxed);
                if input_tx
                    .send(Arc::new(buffer[..length].to_vec()))
                    .await
                    .is_err()
                {
                    break;
                }
            }
            Ok(Err(error)) => return Err(error.into()),
            Err(_) if complete.load(Ordering::Acquire) => break,
            Err(_) => {}
        }
    }
    drop(input_tx);
    let send_result = send.join().map_err(|_| "UDP sender panicked")?;
    actor.await?;
    drop(template_tx);
    let audit = collector.await?;
    // Preserve counts/readback and print accounting even on a socket send error.
    if let Err(error) = send_result {
        eprintln!("UDP send stopped: {error}");
    }
    Ok((start, audit))
}

#[derive(Default)]
struct Verification {
    persisted: u64,
    rows: u64,
    errors: u64,
    partial_rows: u64,
    input_drops: u64,
    shard_drops: u64,
}

fn u64_at(batch: &RecordBatch, name: &str, row: usize) -> u64 {
    batch
        .column_by_name(name)
        .unwrap()
        .as_any()
        .downcast_ref::<UInt64Array>()
        .unwrap()
        .value(row)
}
fn u32_at(batch: &RecordBatch, name: &str, row: usize) -> u32 {
    batch
        .column_by_name(name)
        .unwrap()
        .as_any()
        .downcast_ref::<UInt32Array>()
        .unwrap()
        .value(row)
}

fn verify(
    root: &Path,
    args: &Args,
    pattern: Pattern,
    offset: usize,
    audit: &Audit,
) -> Result<Verification> {
    let mut result = Verification::default();
    // Per-series row intervals must not overlap, including across shard files.
    let mut intervals = vec![Vec::new(); args.counters];
    for entry in fs::read_dir(root.join("shards"))? {
        let path = entry?.path();
        if !path.join("_READY").is_file() {
            result.errors += 1;
            continue;
        }
        if fs::read_to_string(path.join("_READY"))? != "sonic-hft-parquet-v1" {
            result.errors += 1;
        }
        if path.join("loss.json").exists() {
            let loss: serde_json::Value =
                serde_json::from_reader(File::open(path.join("loss.json"))?)?;
            result.input_drops += loss["dropped_input_messages"]
                .as_u64()
                .ok_or("bad loss metadata")?;
            result.shard_drops += loss["dropped_shards"].as_u64().ok_or("bad loss metadata")?;
        }
        if !path.join("gauge_ranges.parquet").exists() {
            continue;
        }
        let reader = ParquetRecordBatchReaderBuilder::try_new(File::open(
            path.join("gauge_ranges.parquet"),
        )?)?
        .build()?;
        for batch in reader {
            let batch = batch?;
            for row in 0..batch.num_rows() {
                result.rows += 1;
                let samples = u32_at(&batch, "sample_count", row) as u64;
                result.persisted += samples;
                let index = u32_at(&batch, "sai_stat_id", row).wrapping_sub(1) as usize;
                let first = u64_at(&batch, "first_time_unix_nano", row);
                let last = u64_at(&batch, "last_time_unix_nano", row);
                let window = u64_at(&batch, "window_start_unix_nano", row);
                let name = batch
                    .column_by_name("object_name")
                    .unwrap()
                    .as_any()
                    .downcast_ref::<StringArray>()
                    .unwrap()
                    .value(row);
                if index >= args.counters
                    || first == 0
                    || first > last
                    || first % args.step_ns != 0
                    || last % args.step_ns != 0
                    || first / RANGE_NS * RANGE_NS != window
                    || last / RANGE_NS * RANGE_NS != window
                    || u64_at(&batch, "window_end_unix_nano", row) != window + RANGE_NS
                    || u32_at(&batch, "sai_type_id", row) != index as u32 + 1
                    || name != format!("Ethernet{}", index % 64)
                {
                    result.errors += 1;
                    continue;
                }
                let begin = (first / args.step_ns - 1) as usize;
                let end = (last / args.step_ns - 1) as usize;
                if begin < offset || end >= offset + audit.hashes.len() {
                    result.errors += 1;
                    continue;
                }
                intervals[index].push((begin, end));
                let mut expected_count = 0;
                let mut min = u64::MAX;
                let mut max = 0;
                for sequence in begin..=end {
                    if audit.hashes[sequence - offset].is_some() {
                        let value = pattern.value(sequence, index);
                        expected_count += 1;
                        min = min.min(value);
                        max = max.max(value);
                    }
                }
                if samples == 0
                    || samples > expected_count
                    || audit.hashes[begin - offset].is_none()
                    || audit.hashes[end - offset].is_none()
                    || u64_at(&batch, "first_value", row) != pattern.value(begin, index)
                    || u64_at(&batch, "last_value", row) != pattern.value(end, index)
                {
                    result.errors += 1;
                }
                if samples == expected_count {
                    if u64_at(&batch, "min_value", row) != min
                        || u64_at(&batch, "max_value", row) != max
                    {
                        result.errors += 1;
                    }
                } else {
                    result.partial_rows += 1;
                }
            }
        }
    }
    for series in &mut intervals {
        series.sort_unstable();
        result.errors += series
            .windows(2)
            .filter(|pair| pair[0].1 >= pair[1].0)
            .count() as u64;
    }
    if fs::read_dir(root.join(".staging"))?.next().is_some() {
        result.errors += 1;
    }
    Ok(result)
}

fn run(args: Args) -> Result<()> {
    if args.records == 0
        || args.counters == 0
        || args.counters > (65507 - 28) / 8
        || args.repeats == 0
        || args.step_ns == 0
        || args.timeout_secs == 0
        || args.records > u32::MAX as usize
        || (args.records as u64 + 1)
            .checked_mul(args.step_ns)
            .is_none_or(|time| time > u64::MAX - RANGE_NS)
    {
        return Err("invalid counts, UDP datagram size, timestamp span, or deadline".into());
    }
    let root = fs::canonicalize(&args.root)?;
    if !root.is_dir() {
        return Err("--root must be an existing directory".into());
    }
    let cpath = CString::new(root.as_os_str().as_bytes())?;
    let mut stats = std::mem::MaybeUninit::<libc::statfs>::uninit();
    if unsafe { libc::statfs(cpath.as_ptr(), stats.as_mut_ptr()) } != 0 {
        return Err(io::Error::last_os_error().into());
    }
    let fs_type = unsafe { stats.assume_init() }.f_type;
    // Overlay cannot prove a real-disk backing filesystem: require a bind mount.
    if ![0xef53, 0x58465342, 0x9123683e, 0x2fc12fc1, 0xf2f52010].contains(&fs_type) {
        return Err(format!("--root filesystem {fs_type:#x} is not a recognized disk filesystem (ext4/xfs/btrfs/zfs/f2fs); bind-mount real disk, not tmpfs/overlay").into());
    }
    let runtime = Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()?;
    let per_record = args.counters * std::mem::size_of::<SAIStat>() + 256;
    let segment_records = (INPUT_BUDGET / per_record).clamp(1, 6000);
    let modes: &[Mode] = match args.mode {
        Mode::All => &[Mode::Standalone, Mode::Udp],
        Mode::Standalone => &[Mode::Standalone],
        Mode::Udp => &[Mode::Udp],
    };
    let mut any_loss = false;
    let mut any_error = false;
    for &mode in modes {
        for pattern in [
            Pattern::Constant,
            Pattern::Large100,
            Pattern::Large33,
            Pattern::Large66,
        ] {
            for repeat in 1..=args.repeats {
                let trial = tempfile::Builder::new()
                    .prefix("local-storage-perf-")
                    .tempdir_in(&root)?;
                let mut totals = Verification::default();
                let (mut sent, mut received, mut decoded) = (0, 0, 0);
                let (mut seconds, mut peak, mut current, mut segments) = (0.0, 0, 0, 0);
                let mut finalized_allocated_bytes = 0;
                let mut failed = false;
                let mut readback_errors = Vec::new();
                for offset in (0..args.records).step_by(segment_records) {
                    let count = segment_records.min(args.records - offset);
                    let counts = Arc::new(Counts::default());
                    let guard = Watchdog::new(trial.path(), &args, counts.clone());
                    let segment = tempfile::Builder::new()
                        .prefix("segment-")
                        .tempdir_in(trial.path())?;
                    let status = LocalStorageStatus::default();
                    let (tx, rx) = mpsc::sync_channel(STORAGE_QUEUE_BATCHES);
                    let storage = LocalStorageActor::new(
                        rx,
                        LocalStorageConfig {
                            root: segment.path().to_path_buf(),
                            range_interval: Duration::from_nanos(RANGE_NS),
                            shard_interval: Duration::from_secs(5),
                            max_bytes: QUOTA,
                            require_dedicated_filesystem: false,
                        },
                        status.clone(),
                    )
                    .map_err(io::Error::other)?;
                    // Preparation occurs before starting the storage wall-clock timer.
                    let mut batches = Vec::new();
                    if matches!(mode, Mode::Standalone) {
                        let (names, _) = helpers::generate_object_metadata(args.counters);
                        let names: Vec<Arc<str>> = names.into_iter().map(Arc::from).collect();
                        let batch_records = (8192 / args.counters).max(1);
                        for begin in (offset..offset + count).step_by(batch_records) {
                            let end = (begin + batch_records).min(offset + count);
                            let mut batch = SAIStatsBatch::with_capacity(
                                end - begin,
                                (end - begin) * args.counters,
                            );
                            for sequence in begin..end {
                                batch.push_record(
                                    (sequence as u64 + 1) * args.step_ns,
                                    names.iter().enumerate().map(|(index, name)| {
                                        SAIStat::new(
                                            name.clone(),
                                            index as u32 + 1,
                                            index as u32 + 1,
                                            pattern.value(sequence, index),
                                        )
                                    }),
                                );
                            }
                            batches.push(Arc::new(batch));
                        }
                    }
                    let worker = thread::spawn(move || storage.run());
                    let (start, mut audit) = if matches!(mode, Mode::Udp) {
                        runtime.block_on(udp(
                            &args,
                            pattern,
                            offset,
                            count,
                            tx,
                            status.clone(),
                            counts.clone(),
                        ))?
                    } else {
                        let start = Instant::now();
                        for batch in batches {
                            let before = counts.sent.load(Ordering::Relaxed) as usize;
                            pace(start, before * args.counters, args.rate);
                            let records = batch.record_count();
                            if tx.send(batch).is_err() {
                                break;
                            }
                            counts.sent.fetch_add(records as u64, Ordering::Relaxed);
                        }
                        drop(tx);
                        let sent = counts.sent.load(Ordering::Relaxed);
                        counts.received.store(sent, Ordering::Relaxed);
                        counts
                            .decoded
                            .store(sent * args.counters as u64, Ordering::Relaxed);
                        (
                            start,
                            Audit {
                                hashes: (0..count)
                                    .map(|i| (i < sent as usize).then_some(0))
                                    .collect(),
                                errors: 0,
                            },
                        )
                    };
                    if worker.join().is_err() {
                        failed = true;
                    }
                    seconds += start.elapsed().as_secs_f64();
                    // Everything below, including deterministic expectations, is untimed.
                    for (i, hash) in audit.hashes.iter_mut().enumerate() {
                        if let Some(actual) = hash {
                            let sequence = offset + i;
                            let mut expected = (sequence as u64 + 1) * args.step_ns;
                            for index in 0..args.counters {
                                expected = hash_value(expected, pattern.value(sequence, index));
                            }
                            if matches!(mode, Mode::Udp) && *actual != expected {
                                audit.errors += 1;
                            }
                            *actual = expected;
                        }
                    }
                    let verified = match verify(segment.path(), &args, pattern, offset, &audit) {
                        Ok(verified) => verified,
                        Err(error) => {
                            // Do not skip overload accounting or invent persisted counts
                            // when a published file cannot be completely verified.
                            readback_errors.push(error.to_string());
                            Verification {
                                errors: 1,
                                ..Verification::default()
                            }
                        }
                    };
                    totals.persisted += verified.persisted;
                    totals.rows += verified.rows;
                    totals.errors += verified.errors + audit.errors;
                    totals.partial_rows += verified.partial_rows;
                    totals.input_drops += verified.input_drops + status.take_input_drops();
                    totals.shard_drops += verified.shard_drops;
                    failed |= status.failed();
                    sent += counts.sent.load(Ordering::Relaxed) * args.counters as u64;
                    received += counts.received.load(Ordering::Relaxed) * args.counters as u64;
                    decoded += counts.decoded.load(Ordering::Relaxed);
                    current = allocated(trial.path(), &mut HashSet::new())?;
                    finalized_allocated_bytes +=
                        allocated(&segment.path().join("shards"), &mut HashSet::new())?;
                    peak = peak.max(current).max(guard.peak.load(Ordering::Relaxed));
                    segments += 1;
                    segment.close()?;
                    drop(guard);
                }
                let expected = (args.records * args.counters) as u64;
                let lossless = expected == sent
                    && sent == received
                    && received == decoded
                    && decoded == totals.persisted
                    && totals.input_drops == 0
                    && totals.shard_drops == 0;
                let correct = !failed && totals.errors == 0 && totals.partial_rows == 0;
                any_loss |= !lossless || !correct;
                any_error |= failed || totals.errors != 0;
                let large = (0..args.counters).filter(|&i| pattern.large(i)).count();
                println!(
                    "{}",
                    json!({"event":"trial", "mode":format!("{mode:?}").to_lowercase(),
                    "pattern":pattern.name(), "repeat":repeat,
                    "status":if failed || totals.errors != 0 { "error" } else if !lossless || !correct { "loss" } else { "lossless" },
                    "measurement_class":if !correct { "invalid_or_partial" } else if args.rate != 0 && lossless { "paced_lossless" } else if matches!(mode, Mode::Standalone) && lossless { "unpaced_lossless_segmented" } else { "overload_no_capacity_claim" },
                    "records":args.records, "counters":args.counters, "large_counters":large,
                    "large_fraction":large as f64 / args.counters as f64,
                    "rate_metrics_per_second":args.rate, "step_ns":args.step_ns,
                    "range_ns":RANGE_NS, "shard_ns":5_000_000_000u64,
                    "segments":segments, "segment_record_limit":segment_records,
                    "prebuilt_input_budget_bytes":INPUT_BUDGET, "seconds":seconds,
                    "metrics_per_second":readback_errors.is_empty().then_some(totals.persisted as f64 / seconds),
                    "expected_metrics":expected, "sent_metrics":sent, "received_metrics":received,
                    "decoded_metrics":decoded, "persisted_metrics":readback_errors.is_empty().then_some(totals.persisted),
                    "sent_records":sent / args.counters as u64,
                    "received_records":received / args.counters as u64,
                    "decoded_records":decoded / args.counters as u64,
                    "unsent_metrics":expected.saturating_sub(sent),
                    "udp_lost_metrics":sent.saturating_sub(received),
                    "decode_lost_metrics":received.saturating_sub(decoded),
                    "storage_lost_metrics":readback_errors.is_empty().then_some(decoded.saturating_sub(totals.persisted)),
                    "input_dropped_batches":totals.input_drops, "dropped_shards":totals.shard_drops,
                    "rows":totals.rows, "rows_per_second":totals.rows as f64 / seconds,
                    "verification_errors":totals.errors, "partially_verified_rows":totals.partial_rows,
                    "readback_errors":readback_errors,
                    "measurement_warning":if seconds < 2.0 { Some("short trial; increase --records or use pacing") } else { None },
                    "runtime_workers":2, "storage_queue_batches":STORAGE_QUEUE_BATCHES, "udp_audit_queue_batches":64,
                    "udp_idle_timeout_ms":UDP_IDLE_TIMEOUT_MS,
                    "storage_failed":failed, "disk_allocated_peak_sampled":peak,
                    "disk_allocated_finalized_bytes_sum":finalized_allocated_bytes,
                    "disk_allocated_current_before_cleanup":current, "quota_bytes":QUOTA,
                    "filesystem_type":format!("{fs_type:#x}"), "root":root})
                );
                io::stdout().flush()?;
                trial.close()?;
            }
        }
    }
    if any_error || (args.require_lossless && any_loss) {
        return Err("trial errors or required lossless check failed; see JSONL accounting".into());
    }
    Ok(())
}

fn main() {
    if let Err(error) = run(Args::parse()) {
        println!("{}", json!({"event":"error", "error":error.to_string()}));
        std::process::exit(1);
    }
}
