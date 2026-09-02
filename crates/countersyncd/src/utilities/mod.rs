/// Utility helpers shared across countersyncd modules.
use std::fmt::Write;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use log::info;
use once_cell::sync::Lazy;

/// Formats a binary buffer into a hex string with 4 bytes per line.
///
/// Each line contains up to 4 bytes, formatted as two-digit lowercase hex
/// separated by a single space.
pub fn format_hex_lines(buffer: &[u8]) -> String {
    const BYTES_PER_LINE: usize = 4;
    let mut output = String::with_capacity(buffer.len().saturating_mul(3));
    for (index, byte) in buffer.iter().enumerate() {
        if index > 0 {
            output.push(if index % BYTES_PER_LINE == 0 {
                '\n'
            } else {
                ' '
            });
        }
        write!(output, "{byte:02x}").expect("writing to String cannot fail");
    }
    output
}

/// Configurable log interval for communication stats (seconds).
/// Set via set_comm_log_interval_secs() at startup (e.g. from CLI); default 600.
static COMM_LOG_INTERVAL_SECS: AtomicU64 = AtomicU64::new(600);

/// Sets the interval (in seconds) between periodic comm stats log lines.
/// Call once at startup (e.g. from CLI). Shorter intervals (e.g. 60) help when
/// verifying HFT processing slowness.
pub fn set_comm_log_interval_secs(secs: u64) {
    COMM_LOG_INTERVAL_SECS.store(secs, Ordering::Relaxed);
}

/// Channel labels for actor-to-actor communication.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ChannelLabel {
    ControlNetlinkToDataNetlink,
    DataNetlinkToIpfixRecords,
    SwssToIpfixTemplates,
    IpfixToStatsReporter,
    IpfixToCounterDb,
    IpfixToOtel,
}

const CHANNEL_LABEL_COUNT: usize = 6;

impl ChannelLabel {
    const fn index(self) -> usize {
        self as usize
    }

    fn as_str(self) -> &'static str {
        match self {
            ChannelLabel::ControlNetlinkToDataNetlink => "control_netlink.data_netlink_cmd",
            ChannelLabel::DataNetlinkToIpfixRecords => "data_netlink.ipfix_records",
            ChannelLabel::SwssToIpfixTemplates => "swss.ipfix_templates",
            ChannelLabel::IpfixToStatsReporter => "ipfix.stats_reporter",
            ChannelLabel::IpfixToCounterDb => "ipfix.counter_db",
            ChannelLabel::IpfixToOtel => "ipfix.otel",
        }
    }
}

#[derive(Debug)]
struct CommStats {
    /// Total number of samples recorded in the current reporting window.
    /// Use to normalize sums and compare workload across windows.
    count: AtomicU64,
    /// Sum of sampled channel lengths (used to compute average).
    /// Higher sum with same count means consistently higher queue occupancy.
    sum: AtomicU64,
    /// Peak channel length observed in the current window.
    /// Spikes here indicate bursty producers or downstream backpressure.
    max: AtomicUsize,
    /// Minimum channel length observed in the current window.
    /// Useful to confirm idle periods (min == 0) or steady load (min > 0).
    min: AtomicUsize,
    /// Most recent sampled channel length.
    /// Helps correlate with immediate behavior when reading logs.
    last: AtomicUsize,
    /// Sum of squared channel lengths (used to compute RMS).
    /// RMS > AVG implies variability/peaks; RMS ~= AVG implies stable load.
    sum_sq: AtomicU64,
    /// Number of samples where channel length was non-zero.
    /// Non-zero ratio hints at sustained pressure vs. intermittent bursts.
    nonzero_count: AtomicU64,
    /// Configured channel capacity (0 means unknown/not set).
    /// Enables utilization analysis: avg/capacity and peak/capacity.
    capacity: AtomicUsize,
    /// Last time we emitted a log for this label.
    last_log_ns: AtomicU64,
}

impl Default for CommStats {
    fn default() -> Self {
        Self {
            count: AtomicU64::new(0),
            sum: AtomicU64::new(0),
            max: AtomicUsize::new(0),
            min: AtomicUsize::new(usize::MAX),
            last: AtomicUsize::new(0),
            sum_sq: AtomicU64::new(0),
            nonzero_count: AtomicU64::new(0),
            capacity: AtomicUsize::new(0),
            last_log_ns: AtomicU64::new(0),
        }
    }
}

static COMM_STATS: Lazy<[CommStats; CHANNEL_LABEL_COUNT]> =
    Lazy::new(|| std::array::from_fn(|_| CommStats::default()));
static COMM_STATS_STARTED: Lazy<Instant> = Lazy::new(Instant::now);

/// Records a communication channel length sample and logs periodically.
pub fn record_comm_stats(label: ChannelLabel, channel_len: usize) {
    let stats = &COMM_STATS[label.index()];
    let count = stats
        .count
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| {
            Some(value.saturating_add(1))
        })
        .unwrap_or(u64::MAX)
        .saturating_add(1);
    atomic_saturating_add(&stats.sum, channel_len as u64);
    let squared = (channel_len as u64).saturating_mul(channel_len as u64);
    atomic_saturating_add(&stats.sum_sq, squared);
    stats.last.store(channel_len, Ordering::Relaxed);
    if channel_len > 0 {
        atomic_saturating_add(&stats.nonzero_count, 1);
    }
    stats.min.fetch_min(channel_len, Ordering::Relaxed);
    stats.max.fetch_max(channel_len, Ordering::Relaxed);

    // Avoid a clock read on every hot-path message. Low-rate channels still
    // check every sample until the first report window has elapsed.
    let last_log_ns = stats.last_log_ns.load(Ordering::Relaxed);
    if count >= 256 && count & 0xff != 0 {
        return;
    }

    let now_ns = COMM_STATS_STARTED
        .elapsed()
        .as_nanos()
        .min(u64::MAX as u128) as u64;
    let interval_ns = Duration::from_secs(COMM_LOG_INTERVAL_SECS.load(Ordering::Relaxed))
        .as_nanos()
        .min(u64::MAX as u128) as u64;
    if now_ns.saturating_sub(last_log_ns) >= interval_ns
        && stats
            .last_log_ns
            .compare_exchange(last_log_ns, now_ns, Ordering::Relaxed, Ordering::Relaxed)
            .is_ok()
    {
        let count = stats.count.swap(0, Ordering::Relaxed);
        let sum = stats.sum.swap(0, Ordering::Relaxed);
        let sum_sq = stats.sum_sq.swap(0, Ordering::Relaxed);
        let max = stats.max.swap(0, Ordering::Relaxed);
        let min = stats.min.swap(usize::MAX, Ordering::Relaxed);
        let last = stats.last.load(Ordering::Relaxed);
        let nonzero_count = stats.nonzero_count.swap(0, Ordering::Relaxed);
        let capacity = stats.capacity.load(Ordering::Relaxed);
        let avg = sum as f64 / count as f64;
        let rms = (sum_sq as f64 / count as f64).sqrt();
        if capacity > 0 {
            let avg_util = avg / capacity as f64;
            let peak_util = max as f64 / capacity as f64;
            info!(
                "Comm stats [{}]: count={}, avg_len={:.2}, peak_len={}, min_len={}, last_len={}, rms_len={:.2}, nonzero_count={}, capacity={}, avg_util={:.2}, peak_util={:.2}",
                label.as_str(),
                count,
                avg,
                max,
                min,
                last,
                rms,
                nonzero_count,
                capacity,
                avg_util,
                peak_util
            );
        } else {
            info!(
                "Comm stats [{}]: count={}, avg_len={:.2}, peak_len={}, min_len={}, last_len={}, rms_len={:.2}, nonzero_count={}",
                label.as_str(),
                count,
                avg,
                max,
                min,
                last,
                rms,
                nonzero_count
            );
        }
    }
}

fn atomic_saturating_add(value: &AtomicU64, amount: u64) {
    let _ = value.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |current| {
        Some(current.saturating_add(amount))
    });
}

/// Sets channel capacity for utilization analysis (optional).
/// Call this once during initialization if capacity is known.
pub fn set_comm_capacity(label: ChannelLabel, capacity: usize) {
    COMM_STATS[label.index()]
        .capacity
        .store(capacity, Ordering::Relaxed);
}
