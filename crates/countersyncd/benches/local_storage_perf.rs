//! Disk-backed, non-Criterion benchmark. Run with --help for the CLI.
//!
//! Each trial sums independently drained segments of at most 6000 records and
//! 120 MB of prebuilt input (including conservative allocation overhead). Larger
//! --records does NOT replay a payload pool: absolute sequence numbers continue.
//! By default generation, template installation and verification are untimed.
//! --streaming uses one actor/storage instance for the whole trial, generating
//! UDP packets or standalone batches inside timing without replaying a pool. All modes
//! time enqueue/send through reducer/writer join and final publish fsync.
//! Default segments add finalization cost, not uninterrupted streaming throughput.
//! Standard Arrow IPC v5 shards retain exactly two raw UInt64 lists,
//! timestamps_ns and series-major values; defaults rotate at
//! 100 MB of compressed IPC bytes or 1800 s maximum age, after complete batches.
//! Each physical IPC row is one matrix block; reported rows remain logical source
//! records, with ipc_block_rows reported separately. Generator sequences are
//! internal, derived on readback from the exact timestamp base and step.
//! Pacing uses wall time, independently of
//! --step-ns source time. Default pacing is 1M metrics/s; 0 measures overload.
//!
//! large33 selects index % 3 == 0; large66 selects index % 3 < 2. At 500
//! counters these are 167/500 (33.4%) and 334/500 (66.8%), not rounded percentages.
//! These optional stress patterns use independent SplitMix64 full-width values; others
//! increment by one. constant keeps every series exactly unchanged, conservatively
//! exercising maximum compression. No cache eviction or cold-disk claim is made.
//!
//! Default values are bytes per sample interval, NOT cumulative counters or bps.
//! Cap = floor(200_000_000_000 * step_ns / (8 * 1_000_000_000)), using u128:
//! exactly 250000 bytes at 10 us. Shapes below are specified at 10 us and scaled
//! by step_ns/10000 with integer truncation (never above the cap).
//! Phase = (source_time + index*104729) % 100ms. Burst has a 10% active duty:
//! 2% ramp up, 6% near-cap triangular peaks/valleys, 2% ramp down. Mixed uses
//! 5-20% duty by index with the same 2% ramps. The following 2ms transition,
//! for index%8==0, holds 10000,101000,101000,10200,10201 for 400us each.
//! Other indexes perturb these plateaus by small integers. The remaining long
//! low interval is 0-6 bytes (0-4.8Mbps at 10us), mostly zero; idle is all low.
//! Sustained stays near cap. Triangle periods drift each envelope by index and
//! epoch; jitter is only 0-3 bytes. No stateful replay or full-width default RNG.
//!
//! UDP includes a bounded audit recipient (64 batches), checking record counts
//! and unique timestamps; --audit-values also hashes values and checks identities.
//! Its scheduling/backpressure is measured. Persisted values are always verified.
//! There is no readiness probe: a capacity-one template channel reserve barrier
//! precedes data. Its sender stays alive until the actor drains the closed input.
//! UDP completion drains the loopback socket after synchronous sends finish,
//! without an idle timeout. Stage times are overlapping elapsed milestones from
//! the common start, not additive CPU times. Generation is timed only in streaming.
//! Overload observations with losses make no lossless-capacity claim.
//! Only published *.arrow files count as writes. Independent StreamReader raw
//! columns (mandatory) and optional --reader-api-check streamed read_shard check identities,
//! exact timestamps, values, full series names, record/stat order and uniqueness
//! against deterministic input. Audit memory scales with records, not metrics.
//!
//! A dedicated deadline thread bounds each segment, including readback, cleanup
//! and sampler joins. Hard timeout exits immediately with status 124, without
//! logging or cleanup. Quota/accounting diagnostics are best-effort JSON from a
//! separate sampler. Private run directories may remain after either abort.
//! Trial-summary output and between-segment setup are outside the deadline.
//! Disk peaks are sampled every 10 ms (plus final usage), not exact high-water
//! marks. RSS is sampled from /proc/self/statm every 10ms during writing until
//! storage worker join; a separate overall sample peak includes readback. Write
//! RSS includes generation/UDP/audits and allocator history, NOT an isolated writer.
//! marks. Production quota is 2.4 GB; the independent watchdog fails at 2.95 GB.
//! Segments are deleted unless --output-root retains them for third-party readers.
//! Retained trials share the quota/watchdog budget. Finalized allocated bytes
//! include the storage root, not physical write amplification. Ratios divide
//! actual persisted value bytes or canonical record payload (values*8+records*8)
//! by all Arrow file bytes, with no codec-ratio claim. Fresh v5 trials must not
//! produce loss.json. Bounded storage queues backpressure rather than discard.
//! No compression numerator includes dropped input.
//!
//! CLI examples (append to `cargo bench -p countersyncd --features local-storage-benchmark --bench local_storage_perf --`):
//! - Smoke: --root <disk> --mode udp --streaming --records 16 --counters 8
//!   --repeats 1 --rate 0 --audit-values --require-lossless
//! - Four-pattern throughput: --root <disk> --mode udp --streaming --records 60000
//!   --counters 8000 --repeats 1 --rate 0
//! - Full queue: --root <disk> --mode standalone --full-queue --records 32
//!   --counters 8 --repeats 1 --rate 0 --require-lossless
//! Lossless sample storage describes surviving samples, not zero packet/storage
//! loss: check all_input_persisted separately or require --require-lossless.

#![recursion_limit = "512"]

use std::{
    collections::HashSet,
    ffi::CString,
    fs::{self, File},
    io::{self, Write},
    os::unix::{
        ffi::OsStrExt,
        fs::{MetadataExt, PermissionsExt},
    },
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc, Arc,
    },
    thread,
    time::{Duration, Instant},
};

use arrow_array::{Array, ListArray, UInt64Array};
use arrow_ipc::reader::StreamReader;
use arrow_schema::DataType;
use byteorder::{ByteOrder, NetworkEndian};
use clap::{Parser, ValueEnum};
use countersyncd::{
    actor::{
        ipfix::IpfixActor,
        local_storage::{
            read_shard, series_names, DecodedSample, LocalStorageActor, LocalStorageConfig,
        },
    },
    message::{
        ipfix::IPFixTemplatesMessage,
        local_storage::LocalStorageStatus,
        saistats::{SAIStat, SAIStatsBatch, SAIStatsBatchMessage},
    },
};
use serde_json::json;
use tokio::{
    net::UdpSocket,
    runtime::Builder,
    sync::{mpsc as channel, Notify},
};

#[path = "../tests/ipfix_test_helpers.rs"]
mod helpers;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>;
const QUOTA: u64 = 2_400_000_000;
const INPUT_BUDGET: usize = 120_000_000;
const ID: u16 = 300;
const STORAGE_QUEUE_BATCHES: usize = 32;
const FORMAT_VERSION: &str = "sonic-hft-arrow-v5";
const GENERATOR_VERSION: &str = "interval-bytes-v1";
const BPS_ASSUMPTION: u64 = 200_000_000_000;
const ENVELOPE_NS: u64 = 100_000_000;
const STREAMING_RECORD_CAP: usize = 6_000_000;
const AUDIT_BUDGET: usize = 96_000_000;

fn max_bytes(step_ns: u64) -> u128 {
    u128::from(BPS_ASSUMPTION) * u128::from(step_ns) / (8 * 1_000_000_000u128)
}

fn audit_bytes(records: usize, hashes: bool) -> Option<usize> {
    // Conservatively charge 8 flag bytes/record for packet, decoded and raw
    // bitmaps (including spare capacity), plus an optional 8-byte value hash.
    records.checked_mul(if hashes { 16 } else { 8 })
}

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
    #[arg(long, required_unless_present = "self_check")]
    root: Option<PathBuf>,
    /// Run pure benchmark regression checks and exit without disk/network work.
    #[arg(long)]
    self_check: bool,
    /// Retain trials under this initially empty directory for independent readers.
    /// Must be on the same disk filesystem as --root; all trials share the quota.
    #[arg(long)]
    output_root: Option<PathBuf>,
    /// Records per pattern/trial (streaming limit: 6000000; 96MB record-audit budget).
    #[arg(long, default_value_t = 60000)]
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
    /// One continuous trial; generate UDP packets or standalone batches inside timing.
    #[arg(long)]
    streaming: bool,
    /// Standalone full-queue benchmark: one-record batches, capacity one, delayed
    /// worker start until a send encounters the full queue; no input is discarded.
    #[arg(long)]
    full_queue: bool,
    /// Hash every decoded value in the timed audit (persisted values always checked).
    #[arg(long)]
    audit_values: bool,
    /// Also validate every sample through read_shard (three Arc clones per metric).
    /// Independent standard Arrow full-array readback is always mandatory.
    #[arg(long)]
    reader_api_check: bool,
    /// Print exact first 20 values for index zero in each trial's JSON.
    #[arg(long)]
    pattern_example: bool,
    /// Run bounded deterministic generator assertions before any storage work.
    #[arg(long)]
    generator_self_check: bool,
    /// Bytes per interval; constant/large100/large33/large66 are raw_u64 stress.
    #[arg(
        long,
        value_enum,
        value_delimiter = ',',
        default_value = "idle,burst,sustained,mixed"
    )]
    patterns: Vec<Pattern>,
    /// Hard per-segment deadline (whole streaming trial), including readback/joins.
    #[arg(long, default_value_t = 120)]
    timeout_secs: u64,
    /// Compressed IPC file target, checked after each complete batch flush.
    #[arg(long, default_value = "100000000", value_parser = clap::value_parser!(u64).range(1..))]
    local_storage_file_bytes: u64,
    /// Maximum file age in wall-clock seconds, independent of source timestamps.
    #[arg(long, default_value = "1800", value_parser = clap::value_parser!(u64).range(1..))]
    local_storage_file_seconds: u64,
    /// Print all trial accounting, then fail if any input was lost.
    #[arg(long)]
    require_lossless: bool,
    // cargo bench passes this even though the executable is not a libtest harness.
    #[arg(long, hide = true)]
    bench: bool,
}

#[derive(Clone, Copy, Debug, ValueEnum)]
enum Pattern {
    Idle,
    Burst,
    Sustained,
    #[value(alias = "staggered")]
    Mixed,
    Constant,
    Large100,
    Large33,
    Large66,
}
impl Pattern {
    fn name(self) -> &'static str {
        match self {
            Self::Idle => "idle",
            Self::Burst => "burst",
            Self::Sustained => "sustained",
            Self::Mixed => "mixed",
            Self::Constant => "constant",
            Self::Large100 => "large100",
            Self::Large33 => "large33",
            Self::Large66 => "large66",
        }
    }
    fn large(self, index: usize) -> bool {
        match self {
            Self::Idle | Self::Burst | Self::Sustained | Self::Mixed | Self::Constant => false,
            Self::Large100 => true,
            Self::Large33 => index % 3 == 0,
            Self::Large66 => index % 3 < 2,
        }
    }
    fn value(self, sequence: usize, index: usize, step_ns: u64) -> u64 {
        if matches!(
            self,
            Self::Idle | Self::Burst | Self::Sustained | Self::Mixed
        ) {
            let time = (sequence as u64 + 1) * step_ns;
            let series = index as u64;
            let epoch = time / ENVELOPE_NS;
            let phase = (time % ENVELOPE_NS + series * 104_729) % ENVELOPE_NS;
            let ticks = time / 10_000 + series * 17;
            let low_phase = (time / 1_000_000 + series * 37) % 101;
            let low = if low_phase < 80 {
                0
            } else {
                (ticks / 31 + epoch) % 7
            };
            let width = if matches!(self, Self::Mixed) {
                (5 + series % 16) * 1_000_000
            } else {
                10_000_000
            };
            let value = if matches!(self, Self::Idle) {
                low
            } else if matches!(self, Self::Sustained) || phase < width {
                let half_period = 31 + series % 17 + epoch % 13;
                let wave = ticks % (2 * half_period);
                let triangle = wave.min(2 * half_period - wave);
                let jitter = (ticks ^ (ticks >> 4) ^ series) & 3;
                let high = 250_000 - triangle * (1 + series % 5) - jitter;
                if matches!(self, Self::Sustained) {
                    high
                } else {
                    high * phase.min(width - phase).min(2_000_000) / 2_000_000
                }
            } else if phase < width + 2_000_000 {
                let plateau = match (phase - width) / 400_000 {
                    0 => 10_000,
                    1 | 2 => 101_000,
                    3 => 10_200,
                    _ => 10_201,
                };
                plateau
                    + if index % 8 == 0 {
                        0
                    } else {
                        (ticks / 23 + series + epoch) % 7
                    }
            } else {
                low
            };
            return (u128::from(value) * u128::from(step_ns) / 10_000) as u64;
        }
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

// This bench has harness=false; opt-in assertions run in the actual smoke binary.
fn generator_self_check() {
    assert_eq!(max_bytes(10_000), 250_000);
    assert_eq!(max_bytes(1), 25);
    assert_eq!(audit_bytes(STREAMING_RECORD_CAP, true), Some(AUDIT_BUDGET));
    assert!(audit_bytes(usize::MAX, true).is_none());
    assert_eq!(
        [999, 1039, 1079, 1119, 1159].map(|seq| Pattern::Burst.value(seq, 0, 10_000)),
        [10_000, 101_000, 101_000, 10_200, 10_201]
    );
    for pattern in [
        Pattern::Idle,
        Pattern::Burst,
        Pattern::Sustained,
        Pattern::Mixed,
    ] {
        for index in [0, 1, 7, 8, 499, 7999] {
            for epoch in [0, 1, 59, 599] {
                let mut minimum = u64::MAX;
                let mut maximum = 0;
                let mut falls = 0;
                let mut previous = 0;
                for sequence in epoch * 10_000..(epoch + 1) * 10_000 {
                    let value = pattern.value(sequence, index, 10_000);
                    assert!(value <= 250_000);
                    minimum = minimum.min(value);
                    maximum = maximum.max(value);
                    falls += usize::from(value < previous);
                    previous = value;
                }
                match pattern {
                    Pattern::Idle => assert!(minimum == 0 && maximum <= 6),
                    Pattern::Sustained => assert!(minimum >= 249_000 && maximum > minimum),
                    _ => assert!(minimum == 0 && maximum >= 249_000 && falls > 0),
                }
            }
        }
        for step in [1, 9999, 10_000, 10_001, 1_000_000] {
            for sequence in 0..1000 {
                assert!(u128::from(pattern.value(sequence, 7, step)) <= max_bytes(step));
            }
        }
    }
    for pattern in [Pattern::Burst, Pattern::Mixed, Pattern::Sustained] {
        assert!(
            (0..10_000).any(|seq| pattern.value(seq, 0, 10_000) != pattern.value(seq, 1, 10_000))
        );
        assert!((0..10_000)
            .any(|seq| pattern.value(seq, 0, 10_000) != pattern.value(seq + 10_000, 0, 10_000)));
    }
    println!(
        "{}",
        json!({"event":"generator_self_check", "status":"passed", "generator_version":GENERATOR_VERSION})
    );
}

fn self_check() {
    generator_self_check();
    assert!(Args::try_parse_from(["bench"]).is_err());
    assert!(Args::try_parse_from(["bench", "--self-check"]).is_ok());
    assert_eq!(metric_count(600_000, 8000), 4_800_000_000);
    assert_eq!(metric_count(6_000_000, 8000), 48_000_000_000);
    let before = pacing_target(metric_count(536_870, 8000), 1_000_000);
    let after = pacing_target(metric_count(536_871, 8000), 1_000_000);
    assert!(after > before);
    assert_eq!(after, Duration::from_millis(4_294_968));
    let names = [
        "00000000000000000030-0000000042-00000000000000000000.arrow",
        "00000000000000000020-0000000042-00000000000000000001.arrow",
        "00000000000000000010-0000000042-00000000000000000002.arrow",
    ]
    .map(PathBuf::from);
    assert_eq!(
        sort_shards(vec![names[2].clone(), names[0].clone(), names[1].clone()]).unwrap(),
        names
    );
    assert_eq!(shard_sequence(Path::new("1-42-10.arrow")).unwrap(), 10);
    assert_eq!(
        sort_shards(vec![
            PathBuf::from("1-42-10.arrow"),
            PathBuf::from("1-42-2.arrow")
        ])
        .unwrap(),
        [
            PathBuf::from("1-42-2.arrow"),
            PathBuf::from("1-42-10.arrow")
        ]
    );
    for name in [
        "unknown.arrow",
        "1-42-bad.arrow",
        "1-42-0.partial",
        "1-42-18446744073709551616.arrow",
    ] {
        assert!(shard_sequence(Path::new(name)).is_err());
    }
    println!(
        "{}",
        json!({"event":"benchmark_self_check", "status":"passed"})
    );
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

fn rss_bytes() -> io::Result<u64> {
    let statm = fs::read_to_string("/proc/self/statm")?;
    let pages = statm
        .split_whitespace()
        .nth(1)
        .and_then(|value| value.parse::<u64>().ok())
        .ok_or_else(|| io::Error::other("invalid /proc/self/statm RSS"))?;
    let page_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    if page_size <= 0 {
        return Err(io::Error::other("invalid system page size"));
    }
    pages
        .checked_mul(page_size as u64)
        .ok_or_else(|| io::Error::other("RSS byte count overflow"))
}

struct HardDeadline {
    cancel: mpsc::Sender<()>,
    handle: Option<thread::JoinHandle<()>>,
}

impl HardDeadline {
    fn new(timeout: Duration) -> Self {
        let start = Instant::now();
        let (cancel, rx) = mpsc::channel();
        let handle = thread::spawn(move || {
            if matches!(
                rx.recv_timeout(timeout.saturating_sub(start.elapsed())),
                Err(mpsc::RecvTimeoutError::Timeout)
            ) {
                // No I/O, allocator work, exit handlers or destructors: another
                // thread may hold any of their locks or be stuck in filesystem I/O.
                unsafe { libc::_exit(124) }
            }
        });
        Self {
            cancel,
            handle: Some(handle),
        }
    }
}

impl Drop for HardDeadline {
    fn drop(&mut self) {
        let _ = self.cancel.send(());
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

struct Watchdog {
    // Dropped only after Watchdog::drop has joined the accounting sampler.
    _deadline: HardDeadline,
    stop: mpsc::Sender<()>,
    handle: Option<thread::JoinHandle<()>>,
    peak: Arc<AtomicU64>,
    rss_peak: Arc<AtomicU64>,
    writing_rss_peak: Arc<AtomicU64>,
    writing: Arc<AtomicBool>,
}
impl Watchdog {
    fn new(root: &Path, args: &Args, counts: Arc<Counts>) -> Result<Self> {
        let deadline = HardDeadline::new(Duration::from_secs(args.timeout_secs));
        let (stop, rx) = mpsc::channel();
        let peak = Arc::new(AtomicU64::new(0));
        let high = peak.clone();
        let rss_peak = Arc::new(AtomicU64::new(rss_bytes()?));
        let writing_rss_peak = Arc::new(AtomicU64::new(0));
        let writing = Arc::new(AtomicBool::new(false));
        let rss_high = rss_peak.clone();
        let writing_high = writing_rss_peak.clone();
        let writing_active = writing.clone();
        let root = root.to_path_buf();
        let counters = args.counters;
        let handle = thread::spawn(move || {
            let start = Instant::now();
            while matches!(
                rx.recv_timeout(Duration::from_millis(10)),
                Err(mpsc::RecvTimeoutError::Timeout)
            ) {
                let rss = rss_bytes();
                if let Ok(bytes) = rss.as_ref() {
                    rss_high.fetch_max(*bytes, Ordering::Relaxed);
                    if writing_active.load(Ordering::Acquire) {
                        writing_high.fetch_max(*bytes, Ordering::Relaxed);
                    }
                }
                let disk = allocated(&root, &mut HashSet::new());
                if let Ok(bytes) = &disk {
                    high.fetch_max(*bytes, Ordering::Relaxed);
                }
                let reason = if disk.is_err() {
                    Some("disk_accounting_error")
                } else if rss.is_err() {
                    Some("rss_accounting_error")
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
                        "sent_metrics":metric_count(counts.sent.load(Ordering::Relaxed), counters),
                        "received_metrics":metric_count(counts.received.load(Ordering::Relaxed), counters),
                        "decoded_metrics":counts.decoded.load(Ordering::Relaxed),
                        "persisted_metrics":null, "disk_allocated_peak_sampled":high.load(Ordering::Relaxed)})
                    );
                    let _ = io::stdout().flush();
                    unsafe { libc::_exit(124) }
                }
            }
        });
        Ok(Self {
            _deadline: deadline,
            stop,
            handle: Some(handle),
            peak,
            rss_peak,
            writing_rss_peak,
            writing,
        })
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

fn metric_count(records: u64, counters: usize) -> u64 {
    records
        .checked_mul(counters as u64)
        .expect("validated metric count fits u64")
}

fn pacing_target(metrics: u64, rate: u64) -> Duration {
    Duration::from_secs_f64(metrics as f64 / rate as f64)
}

fn pace(start: Instant, metrics: u64, rate: u64) {
    if rate == 0 {
        return;
    }
    let target = pacing_target(metrics, rate);
    if let Some(delay) = target.checked_sub(start.elapsed()) {
        thread::sleep(delay);
    }
}

#[derive(Default)]
struct Audit {
    seen: Vec<bool>,
    hashes: Vec<u64>,
    errors: u64,
    packets: u64,
    duplicate_packets: u64,
    bad_packets: u64,
    truncated_packets: u64,
    send_errors: u64,
    sender_seconds: f64,
    receiver_seconds: f64,
    decoder_seconds: f64,
}

async fn udp(
    args: &Args,
    pattern: Pattern,
    offset: usize,
    count: usize,
    storage: channel::Sender<SAIStatsBatchMessage>,
    counts: Arc<Counts>,
) -> Result<(Instant, Audit)> {
    let template = helpers::generate_ipfix_templates(args.counters, ID);
    let mut base = helpers::generate_ipfix_records(&template);
    let step = args.step_ns;
    let generate = move |bytes: &mut [u8], sequence: usize| {
        NetworkEndian::write_u32(&mut bytes[8..12], sequence as u32);
        NetworkEndian::write_u64(&mut bytes[20..28], (sequence as u64 + 1) * step);
        for (index, field) in bytes[28..].chunks_exact_mut(8).enumerate() {
            NetworkEndian::write_u64(field, pattern.value(sequence, index, step));
        }
    };
    let payloads: Vec<_> = if args.streaming {
        Vec::new()
    } else {
        (offset..offset + count)
            .map(|sequence| {
                let mut bytes = base.clone();
                generate(&mut bytes, sequence);
                bytes
            })
            .collect()
    };
    let (names, ids) = helpers::generate_object_metadata(args.counters);
    let (template_tx, template_rx) = channel::channel(1);
    let (input_tx, input_rx) = channel::channel(256);
    let (audit_tx, mut audit_rx) = channel::channel::<SAIStatsBatchMessage>(64);
    let mut actor = IpfixActor::new(template_rx, input_rx);
    actor.add_recipient(storage);
    actor.add_recipient(audit_tx);
    let actor = tokio::spawn(async move {
        IpfixActor::run(actor).await;
        Instant::now()
    });
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

    let receiver_socket = std::net::UdpSocket::bind("127.0.0.1:0")?;
    receiver_socket.set_nonblocking(true)?;
    let receiver = UdpSocket::from_std(receiver_socket.try_clone()?)?;
    let sender = std::net::UdpSocket::bind("127.0.0.1:0")?;
    sender.connect(receiver.local_addr()?)?;
    sender.set_write_timeout(Some(Duration::from_secs(1)))?;
    let complete = Arc::new(AtomicBool::new(false));
    let completion = Arc::new(Notify::new());
    let sender_notification = completion.clone();
    let sender_done = complete.clone();
    let send_counts = counts.clone();
    let rate = args.rate;
    let counters = args.counters;
    let streaming = args.streaming;
    let audit_values = args.audit_values;
    let decode_counts = counts.clone();
    let collector = tokio::spawn(async move {
        let mut audit = Audit {
            seen: vec![false; count],
            hashes: if audit_values {
                vec![0; count]
            } else {
                Vec::new()
            },
            ..Audit::default()
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
                if audit_values {
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
                }
                if audit_values {
                    audit.hashes[index] = hash;
                }
                if std::mem::replace(&mut audit.seen[index], true) {
                    audit.errors += 1;
                }
            }
        }
        audit
    });
    let mut packet_ids = vec![false; count];
    let mut packet_audit = Audit::default();
    let mut buffer = vec![0u8; 65536];
    let start = Instant::now();
    let send = thread::spawn(move || {
        let result = (|| {
            for i in 0..count {
                pace(start, metric_count(i as u64, counters), rate);
                let payload = if streaming {
                    generate(&mut base, offset + i);
                    &base
                } else {
                    &payloads[i]
                };
                if sender.send(payload)? != payload.len() {
                    return Err(io::Error::other("short UDP send"));
                }
                send_counts.sent.fetch_add(1, Ordering::Relaxed);
            }
            Ok(())
        })();
        let elapsed = start.elapsed().as_secs_f64();
        // Publish only after all synchronous sends, including error returns.
        sender_done.store(true, Ordering::Release);
        sender_notification.notify_one();
        (result, elapsed)
    });
    loop {
        let notified = completion.notified();
        tokio::pin!(notified);
        notified.as_mut().enable();
        let received = match receiver.try_recv(&mut buffer) {
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                if complete.load(Ordering::Acquire) {
                    // Recheck after Acquire: the first poll can race the last send.
                    match receiver.try_recv(&mut buffer) {
                        Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                            // Tokio readiness can lag the kernel. Prove the actual
                            // loopback socket is empty, not merely its cached readiness.
                            match receiver_socket.recv(&mut buffer) {
                                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                                result => result,
                            }
                        }
                        result => result,
                    }
                } else {
                    tokio::select! {
                        ready = receiver.readable() => { ready?; }
                        _ = &mut notified => {}
                    }
                    continue;
                }
            }
            result => result,
        };
        match received {
            Ok(length) => {
                packet_audit.packets += 1;
                // One fixed-width record in one Set, from one installed template.
                if length != 28 + counters * 8
                    || NetworkEndian::read_u16(&buffer[..2]) != 10
                    || NetworkEndian::read_u16(&buffer[2..4]) as usize != length
                    || NetworkEndian::read_u32(&buffer[12..16]) != 0
                    || NetworkEndian::read_u16(&buffer[16..18]) != ID
                    || NetworkEndian::read_u16(&buffer[18..20]) as usize != length - 16
                {
                    packet_audit.bad_packets += 1;
                    packet_audit.truncated_packets += u64::from(length < 28 + counters * 8);
                    continue;
                }
                let sequence = NetworkEndian::read_u32(&buffer[8..12]) as usize;
                let Some(index) = sequence.checked_sub(offset).filter(|i| *i < count) else {
                    packet_audit.bad_packets += 1;
                    continue;
                };
                if NetworkEndian::read_u64(&buffer[20..28]) != (sequence as u64 + 1) * step {
                    packet_audit.bad_packets += 1;
                    continue;
                }
                if std::mem::replace(&mut packet_ids[index], true) {
                    packet_audit.duplicate_packets += 1;
                    continue;
                }
                counts.received.fetch_add(1, Ordering::Relaxed);
                if input_tx
                    .send(Arc::new(buffer[..length].to_vec()))
                    .await
                    .is_err()
                {
                    break;
                }
            }
            Err(error) => return Err(error.into()),
        }
    }
    let receiver_seconds = start.elapsed().as_secs_f64();
    drop(input_tx);
    let (send_result, sender_seconds) = send.join().map_err(|_| "UDP sender panicked")?;
    let decoder_seconds = actor.await?.duration_since(start).as_secs_f64();
    drop(template_tx);
    let mut audit = collector.await?;
    audit.packets = packet_audit.packets;
    audit.duplicate_packets = packet_audit.duplicate_packets;
    audit.bad_packets = packet_audit.bad_packets;
    audit.truncated_packets = packet_audit.truncated_packets;
    audit.errors += audit.duplicate_packets + audit.bad_packets;
    audit.sender_seconds = sender_seconds;
    audit.receiver_seconds = receiver_seconds;
    audit.decoder_seconds = decoder_seconds;
    audit.errors += packet_ids
        .iter()
        .zip(&audit.seen)
        .filter(|(received, decoded)| !**received && **decoded)
        .count() as u64;
    // Preserve counts/readback and print accounting even on a socket send error.
    if let Err(error) = send_result {
        audit.send_errors += 1;
        eprintln!("UDP send stopped: {error}");
    }
    Ok((start, audit))
}

#[derive(Default)]
struct Verification {
    persisted: u64,
    records: u64,
    ipc_block_rows: u64,
    errors: u64,
    arrow_file_bytes: u64,
    loss_file_bytes: u64,
}

fn shard_sequence(path: &Path) -> Result<u64> {
    let invalid = || format!("unexpected shard name: {}", path.display());
    let stem = path
        .file_stem()
        .and_then(|name| name.to_str())
        .ok_or_else(invalid)?;
    let parts: Vec<_> = stem.split('-').collect();
    if path
        .extension()
        .is_none_or(|extension| extension != "arrow")
        || parts.len() != 3
        || parts
            .iter()
            .any(|part| part.is_empty() || !part.bytes().all(|byte| byte.is_ascii_digit()))
    {
        return Err(invalid().into());
    }
    Ok(parts[2].parse()?)
}

fn sort_shards(paths: Vec<PathBuf>) -> Result<Vec<PathBuf>> {
    // Private segments have one writer; its sequence survives wall-clock rollback.
    let mut ordered = paths
        .into_iter()
        .map(|path| Ok((shard_sequence(&path)?, path)))
        .collect::<Result<Vec<_>>>()?;
    ordered.sort_by_key(|(sequence, _)| *sequence);
    Ok(ordered.into_iter().map(|(_, path)| path).collect())
}

fn verify(
    root: &Path,
    args: &Args,
    pattern: Pattern,
    offset: usize,
    audit: &Audit,
) -> Result<Verification> {
    let mut result = Verification::default();
    let (objects, _) = helpers::generate_object_metadata(args.counters);
    let names: Vec<_> = (0..args.counters)
        .map(|i| series_names(i as u32 + 1, i as u32 + 1))
        .collect();
    // Always scan all raw arrays; the optional API pass never collects samples.
    let mut raw_records = vec![false; audit.seen.len()];
    let mut last_raw_record: Option<usize> = None;
    let mut last_sequence = vec![None; args.counters];
    let mut last_record: Option<(usize, u32)> = None;
    let mut api_samples = 0;
    let mut api_records = 0;
    let loss_path = root.join("loss.json");
    if loss_path.exists() {
        result.loss_file_bytes = fs::metadata(&loss_path)?.len();
        // These are private, fresh trial roots, not an existing data migration.
        result.errors += 1;
    }
    let paths = fs::read_dir(root.join("shards"))?
        .map(|entry| entry.map(|entry| entry.path()))
        .collect::<io::Result<Vec<_>>>()?;
    for path in sort_shards(paths)? {
        if path
            .extension()
            .is_none_or(|extension| extension != "arrow")
            || !path.is_file()
        {
            return Err(format!("unexpected published entry: {}", path.display()).into());
        }
        let file = File::open(&path)?;
        result.arrow_file_bytes += file.metadata()?.len();
        // Proof of self-contained IPC: no application decoder or sidecar schema.
        let reader = StreamReader::try_new(file, None)?;
        let schema = reader.schema();
        if schema.fields().len() != 2
            || schema.metadata().get("format_version").map(String::as_str) != Some(FORMAT_VERSION)
            || schema.metadata().get("timestamp_unit").map(String::as_str) != Some("ns")
            || schema.metadata().get("matrix_order").map(String::as_str) != Some("series-major")
        {
            return Err("unexpected independent Arrow schema".into());
        }
        for (field, name) in schema.fields().iter().zip(["timestamps_ns", "values"]) {
            if field.name() != name
                || field.is_nullable()
                || !field.metadata().is_empty()
                || !matches!(field.data_type(), DataType::List(child)
                    if child.data_type() == &DataType::UInt64 && !child.is_nullable()
                        && child.name() == "item" && child.metadata().is_empty())
            {
                return Err("expected two non-null raw List<UInt64> fields".into());
            }
        }
        let series_json = schema
            .metadata()
            .get("series")
            .ok_or("missing series metadata")?;
        if series_json.len() > 16 * 1024 * 1024 {
            return Err("series metadata exceeds 16 MiB".into());
        }
        let series: serde_json::Value = serde_json::from_str(series_json)?;
        let series = series.as_array().ok_or("series metadata is not an array")?;
        if series.len() != args.counters {
            return Err("incorrect independent Arrow series count".into());
        }
        for (index, entry) in series.iter().enumerate() {
            if entry.as_object().is_none_or(|entry| entry.len() != 3)
                || entry["object_name"].as_str() != Some(objects[index].as_str())
                || entry["type_name"].as_str() != Some(names[index].0.as_str())
                || entry["stat_name"].as_str() != Some(names[index].1.as_str())
            {
                return Err("incorrect independent Arrow ordered series names/metadata".into());
            }
        }
        for batch in reader {
            let batch = batch?;
            if batch.num_rows() != 1 || batch.num_columns() != 2 {
                return Err("expected one physical IPC row per matrix block".into());
            }
            let columns = batch
                .columns()
                .iter()
                .map(|column| {
                    let list = column
                        .as_any()
                        .downcast_ref::<ListArray>()
                        .ok_or("non-List raw column")?;
                    // Borrow the underlying arrays: no flattened matrix copy or
                    // per-metric Arc clones on this mandatory independent pass.
                    let values = list
                        .values()
                        .as_any()
                        .downcast_ref::<UInt64Array>()
                        .ok_or("non-UInt64 raw list child")?;
                    if list.null_count() != 0
                        || values.null_count() != 0
                        || i32::try_from(values.len())
                            .ok()
                            .is_none_or(|len| list.value_offsets() != [0, len])
                    {
                        return Err("invalid raw list offsets or nulls");
                    }
                    Ok(values)
                })
                .collect::<std::result::Result<Vec<_>, _>>()?;
            let rows = columns[0].len();
            if rows == 0
                || rows > audit.seen.len()
                || rows.checked_mul(args.counters) != Some(columns[1].len())
                || args
                    .counters
                    .checked_add(1)
                    .and_then(|n| n.checked_mul(rows))
                    .and_then(|n| n.checked_mul(8))
                    .is_none_or(|bytes| bytes > 128 * 1024 * 1024)
            {
                return Err("invalid raw matrix dimensions or byte limit".into());
            }
            result.ipc_block_rows += batch.num_rows() as u64;
            for row in 0..rows {
                result.records += 1;
                let timestamp = columns[0].value(row);
                let sequence = (timestamp / args.step_ns)
                    .checked_sub(1)
                    .and_then(|sequence| usize::try_from(sequence).ok());
                let Some(sequence) =
                    sequence.filter(|seq| *seq >= offset && *seq - offset < audit.seen.len())
                else {
                    return Err("raw timestamp outside generated input".into());
                };
                if timestamp != (sequence as u64 + 1) * args.step_ns
                    || !audit.seen[sequence - offset]
                    || std::mem::replace(&mut raw_records[sequence - offset], true)
                    || last_raw_record.is_some_and(|last| sequence <= last)
                {
                    result.errors += 1;
                }
                last_raw_record = Some(sequence);
                for index in 0..args.counters {
                    // Count actual non-null samples per row, not records*counters.
                    result.persisted += 1;
                    if columns[1].value(index * rows + row)
                        != pattern.value(sequence, index, args.step_ns)
                    {
                        result.errors += 1;
                    }
                }
            }
        }
        if !args.reader_api_check {
            continue;
        }
        read_shard(&path, |sample: DecodedSample| {
            api_samples += 1;
            let index = sample.stat_index as usize;
            let sequence = (sample.observation_time / args.step_ns)
                .checked_sub(1)
                .and_then(|seq| usize::try_from(seq).ok());
            let Some(sequence) =
                sequence.filter(|seq| *seq >= offset && *seq - offset < audit.seen.len())
            else {
                result.errors += 1;
                return Ok(());
            };
            if index >= args.counters {
                result.errors += 1;
                return Ok(());
            }
            if sample.observation_time != (sequence as u64 + 1) * args.step_ns
                || !raw_records[sequence - offset]
                || sample.object_name.as_ref() != objects[index]
                || sample.type_name.as_ref() != names[index].0
                || sample.stat_name.as_ref() != names[index].1
                || sample.value != pattern.value(sequence, index, args.step_ns)
                || last_sequence[index].is_some_and(|last| sequence <= last)
            {
                result.errors += 1;
            }
            if let Some((seq, stat)) = last_record {
                if (sequence, sample.stat_index) <= (seq, stat)
                    || (sequence == seq && sample.stat_index != stat + 1)
                    || (sequence > seq && (index != 0 || stat as usize + 1 != args.counters))
                {
                    result.errors += 1;
                }
            }
            if last_record.is_none_or(|(seq, _)| seq != sequence) {
                api_records += 1;
                if index != 0 {
                    result.errors += 1;
                }
            }
            last_sequence[index] = Some(sequence);
            last_record = Some((sequence, sample.stat_index));
            Ok(())
        })
        .map_err(io::Error::other)?;
    }
    if result.records != raw_records.iter().filter(|seen| **seen).count() as u64
        || result.persisted != metric_count(result.records, args.counters)
        || (args.reader_api_check
            && (api_samples != result.persisted
                || api_records != result.records
                || last_record.is_some_and(|(_, stat)| stat as usize + 1 != args.counters)))
    {
        result.errors += 1;
    }
    if fs::read_dir(root.join(".staging"))?.next().is_some() {
        result.errors += 1;
    }
    Ok(result)
}

fn is_disk_filesystem(fs_type: u32) -> bool {
    const DISK_FILESYSTEMS: [u32; 5] = [0xef53, 0x58465342, 0x9123683e, 0x2fc12fc1, 0xf2f52010];
    // Overlay cannot prove a real-disk backing filesystem: require a bind mount.
    DISK_FILESYSTEMS.contains(&fs_type)
}

fn run(args: Args) -> Result<()> {
    if args.self_check {
        self_check();
        return Ok(());
    }
    if args.full_queue && (!matches!(args.mode, Mode::Standalone) || args.records < 2) {
        return Err("--full-queue requires --mode standalone and at least two records".into());
    }
    if args.records == 0
        || args.counters == 0
        || args.counters > (65507 - 28) / 8
        || args.repeats == 0
        || args.step_ns == 0
        || args.timeout_secs == 0
        || args.patterns.is_empty()
        || args.records > u32::MAX as usize
        || (args.streaming && args.records > STREAMING_RECORD_CAP)
        || (args.streaming
            && audit_bytes(args.records, args.audit_values)
                .is_none_or(|bytes| bytes > AUDIT_BUDGET))
        || max_bytes(args.step_ns) > u128::from(u64::MAX)
        || (args.records as u64 + 1)
            .checked_mul(args.step_ns)
            .is_none()
    {
        return Err("invalid counts, UDP datagram size, timestamp/value span, audit budget or deadline; streaming supports at most 6000000 records within 96MB record-audit budget".into());
    }
    if args.generator_self_check {
        generator_self_check();
    }
    let process_initial_rss = rss_bytes()?;
    let root = fs::canonicalize(
        args.root
            .as_ref()
            .ok_or("--root is required for performance runs")?,
    )?;
    if !root.is_dir() {
        return Err("--root must be an existing directory".into());
    }
    let cpath = CString::new(root.as_os_str().as_bytes())?;
    let mut stats = std::mem::MaybeUninit::<libc::statfs>::uninit();
    if unsafe { libc::statfs(cpath.as_ptr(), stats.as_mut_ptr()) } != 0 {
        return Err(io::Error::last_os_error().into());
    }
    // Linux filesystem magics are 32-bit bit patterns, even when f_type is signed.
    let fs_type = unsafe { stats.assume_init() }.f_type as u32;
    if !is_disk_filesystem(fs_type) {
        return Err(format!("--root filesystem {fs_type:#x} is not a recognized disk filesystem (ext4/xfs/btrfs/zfs/f2fs); bind-mount real disk, not tmpfs/overlay").into());
    }
    let output_root = if let Some(path) = &args.output_root {
        if !path.exists() {
            fs::create_dir(path)?;
        }
        let path = fs::canonicalize(path)?;
        if fs::metadata(&path)?.dev() != fs::metadata(&root)?.dev()
            || fs::read_dir(&path)?.next().is_some()
        {
            return Err("--output-root must be empty and on the same filesystem as --root".into());
        }
        Some(path)
    } else {
        None
    };
    let runtime = Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()?;
    let per_record = args.counters * std::mem::size_of::<SAIStat>() + 256;
    let segment_records = if args.streaming {
        args.records
    } else {
        (INPUT_BUDGET / per_record).clamp(1, 6000)
    };
    let modes: &[Mode] = match args.mode {
        Mode::All => &[Mode::Standalone, Mode::Udp],
        Mode::Standalone => &[Mode::Standalone],
        Mode::Udp => &[Mode::Udp],
    };
    let mut any_loss = false;
    let mut any_error = false;
    for &mode in modes {
        for &pattern in &args.patterns {
            for repeat in 1..=args.repeats {
                let trial = tempfile::Builder::new()
                    .prefix("local-storage-perf-")
                    .permissions(fs::Permissions::from_mode(0o700))
                    .tempdir_in(output_root.as_ref().unwrap_or(&root))?;
                let monitored_root = output_root.as_deref().unwrap_or(trial.path());
                let mut totals = Verification::default();
                let (mut sent, mut received, mut decoded) = (0, 0, 0);
                let (mut seconds, mut peak, mut current, mut segments) = (0.0, 0, 0, 0);
                let mut finalized_allocated_bytes = 0;
                let mut failed = false;
                let mut full_queue_events = 0u64;
                let mut readback_errors = Vec::new();
                let mut udp_totals = Audit::default();
                let trial_initial_rss = rss_bytes()?;
                let mut overall_rss_peak = trial_initial_rss;
                let mut writing_rss_peak = 0;
                let mut writing_rss_delta_peak = 0;
                let mut writing_rss_baselines = Vec::new();
                for offset in (0..args.records).step_by(segment_records) {
                    let count = segment_records.min(args.records - offset);
                    let counts = Arc::new(Counts::default());
                    let guard = Watchdog::new(monitored_root, &args, counts.clone())?;
                    let segment = tempfile::Builder::new()
                        .prefix("segment-")
                        .permissions(fs::Permissions::from_mode(0o700))
                        .tempdir_in(trial.path())?;
                    let status = LocalStorageStatus::default();
                    let queue_batches = if args.full_queue {
                        1
                    } else {
                        STORAGE_QUEUE_BATCHES
                    };
                    let (tx, rx) = channel::channel(queue_batches);
                    let retained_bytes = allocated(monitored_root, &mut HashSet::new())?
                        .saturating_sub(allocated(segment.path(), &mut HashSet::new())?);
                    let storage = LocalStorageActor::new(
                        rx,
                        LocalStorageConfig {
                            root: segment.path().to_path_buf(),
                            shard_interval: Duration::from_secs(args.local_storage_file_seconds),
                            file_target_bytes: args.local_storage_file_bytes,
                            max_bytes: QUOTA
                                .checked_sub(retained_bytes)
                                .ok_or("retained output exhausted the shared 2.4 GB quota")?,
                        },
                        status.clone(),
                    )
                    .map_err(io::Error::other)?;
                    // Preparation occurs before starting the storage wall-clock timer.
                    let mut batches = Vec::new();
                    let (names, _) = helpers::generate_object_metadata(args.counters);
                    let names: Vec<Arc<str>> = names.into_iter().map(Arc::from).collect();
                    let batch_records = if args.full_queue {
                        1
                    } else {
                        (8192 / args.counters).max(1)
                    };
                    let generate_batch = |begin: usize| {
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
                                        pattern.value(sequence, index, args.step_ns),
                                    )
                                }),
                            );
                        }
                        Arc::new(batch)
                    };
                    if matches!(mode, Mode::Standalone) && !args.streaming {
                        for begin in (offset..offset + count).step_by(batch_records) {
                            batches.push(generate_batch(begin));
                        }
                    }
                    let writing_baseline = rss_bytes()?;
                    writing_rss_baselines.push(writing_baseline);
                    guard
                        .writing_rss_peak
                        .store(writing_baseline, Ordering::Relaxed);
                    guard.writing.store(true, Ordering::Release);
                    let (start_worker, worker_ready) = mpsc::channel();
                    let worker = thread::spawn(move || {
                        if worker_ready.recv().is_ok() {
                            storage.run();
                        }
                    });
                    let mut start_worker = Some(start_worker);
                    if !args.full_queue {
                        start_worker.take().unwrap().send(())?;
                    }
                    let (start, mut audit) = if matches!(mode, Mode::Udp) {
                        runtime.block_on(udp(&args, pattern, offset, count, tx, counts.clone()))?
                    } else {
                        let start = Instant::now();
                        let mut prebuilt = batches.into_iter();
                        for begin in (offset..offset + count).step_by(batch_records) {
                            let before = counts.sent.load(Ordering::Relaxed);
                            pace(start, metric_count(before, args.counters), args.rate);
                            let batch = if args.streaming {
                                generate_batch(begin)
                            } else {
                                prebuilt.next().ok_or("missing prebuilt batch")?
                            };
                            let records = batch.record_count();
                            let batch = if start_worker.is_some() {
                                match tx.try_send(batch) {
                                    Ok(()) => {
                                        counts.sent.fetch_add(records as u64, Ordering::Relaxed);
                                        continue;
                                    }
                                    Err(channel::error::TrySendError::Full(batch)) => {
                                        full_queue_events += 1;
                                        start_worker.take().unwrap().send(())?;
                                        batch
                                    }
                                    Err(channel::error::TrySendError::Closed(_)) => {
                                        failed = true;
                                        break;
                                    }
                                }
                            } else {
                                batch
                            };
                            if tx.blocking_send(batch).is_err() {
                                failed = true;
                                break;
                            }
                            counts.sent.fetch_add(records as u64, Ordering::Relaxed);
                        }
                        drop(tx);
                        if let Some(start_worker) = start_worker.take() {
                            start_worker.send(())?;
                        }
                        let sent = counts.sent.load(Ordering::Relaxed);
                        counts.received.store(sent, Ordering::Relaxed);
                        counts
                            .decoded
                            .store(metric_count(sent, args.counters), Ordering::Relaxed);
                        (
                            start,
                            Audit {
                                seen: (0..count).map(|i| i < sent as usize).collect(),
                                ..Audit::default()
                            },
                        )
                    };
                    if worker.join().is_err() {
                        failed = true;
                    }
                    seconds += start.elapsed().as_secs_f64();
                    guard.writing.store(false, Ordering::Release);
                    let writing_end_rss = rss_bytes()?;
                    let segment_rss_peak =
                        writing_end_rss.max(guard.writing_rss_peak.load(Ordering::Relaxed));
                    writing_rss_peak = writing_rss_peak.max(segment_rss_peak);
                    writing_rss_delta_peak = writing_rss_delta_peak
                        .max(segment_rss_peak.saturating_sub(writing_baseline));
                    // Everything below, including deterministic expectations, is untimed.
                    if args.audit_values && matches!(mode, Mode::Udp) {
                        for (i, hash) in audit.hashes.iter().enumerate() {
                            if !audit.seen[i] {
                                continue;
                            }
                            let sequence = offset + i;
                            let mut expected = (sequence as u64 + 1) * args.step_ns;
                            for index in 0..args.counters {
                                expected = hash_value(
                                    expected,
                                    pattern.value(sequence, index, args.step_ns),
                                );
                            }
                            if *hash != expected {
                                audit.errors += 1;
                            }
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
                    overall_rss_peak = overall_rss_peak
                        .max(segment_rss_peak)
                        .max(guard.rss_peak.load(Ordering::Relaxed))
                        .max(rss_bytes()?);
                    totals.persisted += verified.persisted;
                    totals.records += verified.records;
                    totals.ipc_block_rows += verified.ipc_block_rows;
                    totals.errors += verified.errors + audit.errors;
                    totals.arrow_file_bytes += verified.arrow_file_bytes;
                    totals.loss_file_bytes += verified.loss_file_bytes;
                    udp_totals.packets += audit.packets;
                    udp_totals.duplicate_packets += audit.duplicate_packets;
                    udp_totals.bad_packets += audit.bad_packets;
                    udp_totals.truncated_packets += audit.truncated_packets;
                    udp_totals.send_errors += audit.send_errors;
                    udp_totals.sender_seconds += audit.sender_seconds;
                    udp_totals.receiver_seconds += audit.receiver_seconds;
                    udp_totals.decoder_seconds += audit.decoder_seconds;
                    failed |= status.failed();
                    sent += metric_count(counts.sent.load(Ordering::Relaxed), args.counters);
                    received +=
                        metric_count(counts.received.load(Ordering::Relaxed), args.counters);
                    decoded += counts.decoded.load(Ordering::Relaxed);
                    current = allocated(monitored_root, &mut HashSet::new())?;
                    finalized_allocated_bytes += allocated(segment.path(), &mut HashSet::new())?;
                    peak = peak.max(current).max(guard.peak.load(Ordering::Relaxed));
                    if peak >= 2_950_000_000 {
                        return Err("sampled disk allocation reached 2.95 GB".into());
                    }
                    segments += 1;
                    if output_root.is_some() {
                        let _ = segment.keep();
                    } else {
                        segment.close()?;
                    }
                    drop(guard);
                }
                let expected = metric_count(args.records as u64, args.counters);
                if (readback_errors.is_empty() && totals.persisted > decoded)
                    || (args.full_queue && full_queue_events == 0)
                {
                    totals.errors += 1;
                }
                let lossless = expected == sent
                    && sent == received
                    && received == decoded
                    && decoded == totals.persisted;
                let correct = !failed && totals.errors == 0;
                let is_udp = matches!(mode, Mode::Udp);
                let sent_packets = sent / args.counters as u64;
                let unique_packets = received / args.counters as u64;
                let lost_packets = sent_packets.saturating_sub(unique_packets);
                let persisted_known = readback_errors.is_empty();
                let persisted_value_bytes = totals.persisted * 8;
                let persisted_timestamp_bytes = totals.records * 8;
                let canonical_record_payload_bytes =
                    persisted_value_bytes + persisted_timestamp_bytes;
                let storage_file_bytes = totals.arrow_file_bytes + totals.loss_file_bytes;
                any_loss |= !lossless || !correct;
                any_error |= failed || totals.errors != 0;
                let large = (0..args.counters).filter(|&i| pattern.large(i)).count();
                println!(
                    "{}",
                    json!({"event":"trial", "mode":format!("{mode:?}").to_lowercase(),
                    "pattern":pattern.name(), "repeat":repeat,
                    "status":if failed || totals.errors != 0 { "error" } else if !lossless || !correct { "loss" } else { "lossless" },
                    "measurement_class":if !correct { "invalid" } else if args.rate != 0 && lossless { "paced_lossless" } else if args.streaming && lossless { "unpaced_lossless_streaming" } else if lossless { "unpaced_lossless_segmented" } else { "overload_no_capacity_claim" },
                    "records":args.records, "counters":args.counters, "large_counters":large,
                    "large_fraction":large as f64 / args.counters as f64,
                    "rate_metrics_per_second":args.rate, "step_ns":args.step_ns,
                    "format_version":FORMAT_VERSION, "shard_ns":args.local_storage_file_seconds.checked_mul(1_000_000_000),
                    "file_target_bytes":args.local_storage_file_bytes,
                    "file_max_age_seconds":args.local_storage_file_seconds,
                    "file_rotation_basis":"logical compressed IPC bytes including schema/framing; checked after complete batch flush; up to one batch plus 8-byte EOS overshoot",
                    "value_unit":if matches!(pattern, Pattern::Idle | Pattern::Burst | Pattern::Sustained | Pattern::Mixed) { "bytes_per_interval" } else { "raw_u64" },
                    "generator_version":GENERATOR_VERSION,
                    "bps_assumption":BPS_ASSUMPTION,
                    "max_bytes_per_interval":max_bytes(args.step_ns) as u64,
                    "max_bytes_scope":"default patterns only; floor(bps_assumption * step_ns / (8 * 1000000000)) using u128",
                    "pattern_example_index":args.pattern_example.then_some(0),
                    "pattern_example_first20":args.pattern_example.then(|| (0..20).map(|seq| pattern.value(seq, 0, args.step_ns)).collect::<Vec<_>>()),
                    "pattern_period_source_ns":ENVELOPE_NS,
                    "pattern_period_note":"100ms envelope; index-phased, triangle periods drift each epoch; not an exact repeated payload",
                    "source_span_ns":args.records as u64 * args.step_ns,
                    "source_pattern_warning":(matches!(pattern, Pattern::Idle | Pattern::Burst | Pattern::Mixed) && args.records as u64 * args.step_ns < 100_000_000).then_some("source span shorter than one 100 ms gauge period"),
                    "target_metrics_per_second":50_000_000u64,
                    "meets_50m_lossless_target":correct && lossless && totals.persisted as f64 / seconds >= 50_000_000.0,
                    "streaming":args.streaming, "generation_included":args.streaming,
                    "audit_values":args.audit_values && is_udp,
                    "reader_api_check":args.reader_api_check,
                    "record_audit_budget_bytes":AUDIT_BUDGET,
                    "record_audit_reserved_bound_bytes":audit_bytes(segment_records.min(args.records), args.audit_values),
                    "lossless_sample_storage":correct && totals.persisted > 0,
                    "all_input_persisted":lossless && correct,
                    "segments":segments, "segment_record_limit":segment_records,
                    "prebuilt_input_budget_bytes":if args.streaming { 0 } else { INPUT_BUDGET }, "seconds":seconds,
                    "total_fsync_end_to_end_seconds":seconds,
                    "sender_elapsed_seconds":is_udp.then_some(udp_totals.sender_seconds),
                    "receiver_elapsed_seconds":is_udp.then_some(udp_totals.receiver_seconds),
                    "decoder_elapsed_seconds":is_udp.then_some(udp_totals.decoder_seconds),
                    "stage_timing_basis":"overlapping elapsed milestones from send start; summed only in segmented mode",
                    "sender_metrics_per_second":(is_udp && udp_totals.sender_seconds > 0.0).then_some(sent as f64 / udp_totals.sender_seconds),
                    "receiver_metrics_per_second":(is_udp && udp_totals.receiver_seconds > 0.0).then_some(received as f64 / udp_totals.receiver_seconds),
                    "decoder_metrics_per_second":(is_udp && udp_totals.decoder_seconds > 0.0).then_some(decoded as f64 / udp_totals.decoder_seconds),
                    "metrics_per_second":readback_errors.is_empty().then_some(totals.persisted as f64 / seconds),
                    "expected_metrics":expected, "sent_metrics":sent, "received_metrics":received,
                    "decoded_metrics":decoded, "persisted_metrics":readback_errors.is_empty().then_some(totals.persisted),
                    "sent_records":sent / args.counters as u64,
                    "received_records":received / args.counters as u64,
                    "decoded_records":decoded / args.counters as u64,
                    "persisted_records":persisted_known.then_some(totals.records),
                    "lost_records":persisted_known.then_some((args.records as u64).saturating_sub(totals.records)),
                    "sent_packets":is_udp.then_some(sent_packets),
                    "received_packets":is_udp.then_some(udp_totals.packets),
                    "unique_received_packets":is_udp.then_some(unique_packets),
                    "lost_packets":is_udp.then_some(lost_packets),
                    "packet_loss_fraction":(is_udp && sent_packets > 0).then_some(lost_packets as f64 / sent_packets as f64),
                    "duplicate_packets":is_udp.then_some(udp_totals.duplicate_packets),
                    "bad_framing_packets":is_udp.then_some(udp_totals.bad_packets),
                    "truncated_packets":is_udp.then_some(udp_totals.truncated_packets),
                    "udp_send_errors":is_udp.then_some(udp_totals.send_errors),
                    "unsent_metrics":expected.saturating_sub(sent),
                    "udp_lost_metrics":sent.saturating_sub(received),
                    "decode_lost_metrics":received.saturating_sub(decoded),
                    "storage_lost_metrics":readback_errors.is_empty().then_some(decoded.saturating_sub(totals.persisted)),
                    "decode_loss_fraction":(received > 0).then_some(received.saturating_sub(decoded) as f64 / received as f64),
                    "storage_loss_fraction":(persisted_known && decoded > 0).then_some(decoded.saturating_sub(totals.persisted) as f64 / decoded as f64),
                    "total_loss_fraction":persisted_known.then_some(expected.saturating_sub(totals.persisted) as f64 / expected as f64),
                    "rows":persisted_known.then_some(totals.records),
                    "rows_basis":"logical source records (timestamps), not physical IPC matrix block rows",
                    "ipc_block_rows":persisted_known.then_some(totals.ipc_block_rows),
                    "rows_per_second":persisted_known.then_some(totals.records as f64 / seconds),
                    "verification_errors":totals.errors,
                    "readback_errors":readback_errors,
                    "arrow_logical_file_bytes":persisted_known.then_some(totals.arrow_file_bytes),
                    "loss_logical_file_bytes":persisted_known.then_some(totals.loss_file_bytes),
                    "storage_logical_file_bytes":persisted_known.then_some(storage_file_bytes),
                    "persisted_value_bytes":persisted_known.then_some(persisted_value_bytes),
                    "persisted_timestamp_bytes":persisted_known.then_some(persisted_timestamp_bytes),
                    "persisted_record_seq_bytes":0,
                    "canonical_record_payload_bytes":persisted_known.then_some(canonical_record_payload_bytes),
                    "persisted_values_to_storage_ratio":(persisted_known && storage_file_bytes > 0).then_some(persisted_value_bytes as f64 / storage_file_bytes as f64),
                    "canonical_record_payload_to_storage_ratio":(persisted_known && storage_file_bytes > 0).then_some(canonical_record_payload_bytes as f64 / storage_file_bytes as f64),
                    "compression_basis":"persisted values*8 + persisted records*8 timestamp bytes / (Arrow IPC logical bytes + root loss.json logical bytes, expected absent); no stored record_seq; no codec ratio",
                    "readback":if args.reader_api_check { "independent standard Arrow StreamReader full raw arrays plus streamed read_shard" } else { "independent standard Arrow StreamReader full raw arrays (every metric)" },
                    "measurement_warning":if seconds < 2.0 { Some("short trial; increase --records (use --streaming for continuous generation)") } else { None },
                    "runtime_workers":2, "storage_queue_batches":if args.full_queue { 1 } else { STORAGE_QUEUE_BATCHES }, "udp_audit_queue_batches":64,
                    "storage_queue_policy":"bounded backpressure", "full_queue":args.full_queue,
                    "full_queue_events":full_queue_events,
                    "storage_writers":1, "udp_completion":"sender_done_then_kernel_would_block",
                    "storage_failed":failed, "disk_allocated_peak_sampled":peak,
                    "rss_process_initial_bytes":process_initial_rss,
                    "rss_trial_initial_bytes":trial_initial_rss,
                    "rss_writing_phase_baselines_bytes":writing_rss_baselines,
                    "rss_writing_peak_sampled_bytes":writing_rss_peak,
                    "rss_writing_peak_delta_from_phase_baseline_bytes":writing_rss_delta_peak,
                    "rss_overall_including_readback_peak_sampled_bytes":overall_rss_peak,
                    "rss_sampling_interval_ms":10,
                    "rss_basis":"/proc/self/statm; writing from pre-worker baseline through worker join; includes generation, UDP/audit, runtime and allocator history, not pure writer isolated; overall includes setup/readback; sampled peaks not exact high-water marks",
                    "disk_allocated_finalized_bytes_sum":finalized_allocated_bytes,
                    "disk_allocated_current_before_cleanup":current, "quota_bytes":QUOTA,
                    "disk_allocated_sampled_cap_bytes":2_950_000_000u64,
                    "filesystem_type":format!("{fs_type:#x}"), "root":root,
                    "retained_trial":output_root.as_ref().map(|_| trial.path())})
                );
                io::stdout().flush()?;
                if output_root.is_some() {
                    let _ = trial.keep();
                } else {
                    trial.close()?;
                }
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

#[cfg(test)]
mod tests {
    #[test]
    fn filesystem_magics_preserve_signed_32_bit_patterns() {
        use super::*;

        for magic in [0xef53u32, 0x58465342, 0x9123683e, 0x2fc12fc1, 0xf2f52010] {
            assert!(is_disk_filesystem(magic));
            assert!(is_disk_filesystem((magic as i32) as u32));
            assert!(is_disk_filesystem(i64::from(magic as i32) as u32));
            assert!(is_disk_filesystem(i64::from(magic) as u32));
            let mut stat: libc::statfs = unsafe { std::mem::zeroed() };
            stat.f_type = magic as _;
            assert!(is_disk_filesystem(stat.f_type as u32));
        }
        for magic in [0x9123683eu32, 0xf2f52010] {
            assert!((magic as i32) < 0, "btrfs/f2fs exercise the sign bit");
        }
        // tmpfs, overlay, ramfs and unknown filesystems must remain rejected.
        for magic in [0x01021994u32, 0x794c7630, 0x858458f6, 0, u32::MAX] {
            assert!(!is_disk_filesystem(magic));
            assert!(!is_disk_filesystem(i64::from(magic as i32) as u32));
        }
    }

    #[test]
    fn pure_checks() {
        use super::*;

        run(Args::try_parse_from(["bench", "--self-check"]).unwrap()).unwrap();
    }

    #[test]
    fn watchdog_blocked_sampler_and_stdout() {
        use super::*;
        use std::process::{Command, Stdio};
        use wait_timeout::ChildExt;

        const CHILD: &str = "LOCAL_STORAGE_BENCH_WATCHDOG_CHILD";
        if std::env::var_os(CHILD).is_some() {
            let deadline = HardDeadline::new(Duration::from_secs(1));
            let (ready, rx) = mpsc::channel();
            let handle = thread::spawn(move || {
                let mut stdout = io::stdout().lock();
                ready.send(()).unwrap();
                // The parent deliberately never drains this pipe. Simulate the
                // accounting sampler blocking while holding the stdout lock.
                for _ in 0..64 {
                    stdout.write_all(&[b'x'; 64 * 1024]).unwrap();
                }
                thread::sleep(Duration::from_secs(60));
            });
            rx.recv().unwrap();
            let (stop, _rx) = mpsc::channel();
            let guard = Watchdog {
                _deadline: deadline,
                stop,
                handle: Some(handle),
                peak: Arc::new(AtomicU64::new(0)),
                rss_peak: Arc::new(AtomicU64::new(0)),
                writing_rss_peak: Arc::new(AtomicU64::new(0)),
                writing: Arc::new(AtomicBool::new(false)),
            };
            drop(guard); // Must not cancel the deadline before the blocked join.
            panic!("blocked sampler unexpectedly returned");
        }
        let start = Instant::now();
        let mut child = Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "benchmark::tests::watchdog_blocked_sampler_and_stdout",
                "--nocapture",
            ])
            .env(CHILD, "1")
            .stdout(Stdio::piped())
            .spawn()
            .unwrap();
        let status = child.wait_timeout(Duration::from_secs(5)).unwrap();
        if status.is_none() {
            child.kill().unwrap();
            child.wait().unwrap();
            panic!("hard deadline failed with blocked sampler/stdout");
        }
        assert_eq!(status.unwrap().code(), Some(124));
        assert!(start.elapsed() >= Duration::from_secs(1));
    }
}
