/// Utility helpers shared across countersyncd modules.
use std::fmt::Write;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;
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

impl ChannelLabel {
    fn stats(self) -> &'static Mutex<CommStats> {
        match self {
            ChannelLabel::ControlNetlinkToDataNetlink => &CONTROL_NETLINK_TO_DATA_NETLINK_STATS,
            ChannelLabel::DataNetlinkToIpfixRecords => &DATA_NETLINK_TO_IPFIX_RECORDS_STATS,
            ChannelLabel::SwssToIpfixTemplates => &SWSS_TO_IPFIX_TEMPLATES_STATS,
            ChannelLabel::IpfixToStatsReporter => &IPFIX_TO_STATS_REPORTER_STATS,
            ChannelLabel::IpfixToCounterDb => &IPFIX_TO_COUNTER_DB_STATS,
            ChannelLabel::IpfixToOtel => &IPFIX_TO_OTEL_STATS,
        }
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

#[derive(Debug, Clone)]
struct CommStats {
    /// Total number of samples recorded in the current reporting window.
    /// Use to normalize sums and compare workload across windows.
    count: u64,
    /// Sum of sampled channel lengths (used to compute average).
    /// Higher sum with same count means consistently higher queue occupancy.
    sum: u64,
    /// Peak channel length observed in the current window.
    /// Spikes here indicate bursty producers or downstream backpressure.
    max: usize,
    /// Minimum channel length observed in the current window.
    /// Useful to confirm idle periods (min == 0) or steady load (min > 0).
    min: usize,
    /// Most recent sampled channel length.
    /// Helps correlate with immediate behavior when reading logs.
    last: usize,
    /// Sum of squared channel lengths (used to compute RMS).
    /// RMS > AVG implies variability/peaks; RMS ~= AVG implies stable load.
    sum_sq: u128,
    /// Number of samples where channel length was non-zero.
    /// Non-zero ratio hints at sustained pressure vs. intermittent bursts.
    nonzero_count: u64,
    /// Configured channel capacity (0 means unknown/not set).
    /// Enables utilization analysis: avg/capacity and peak/capacity.
    capacity: usize,
    /// Last time we emitted a log for this label.
    last_log: Instant,
}

impl Default for CommStats {
    fn default() -> Self {
        Self {
            count: 0,
            sum: 0,
            max: 0,
            min: 0,
            last: 0,
            sum_sq: 0,
            nonzero_count: 0,
            capacity: 0,
            last_log: Instant::now(),
        }
    }
}

static CONTROL_NETLINK_TO_DATA_NETLINK_STATS: Lazy<Mutex<CommStats>> =
    Lazy::new(|| Mutex::new(CommStats::default()));
static DATA_NETLINK_TO_IPFIX_RECORDS_STATS: Lazy<Mutex<CommStats>> =
    Lazy::new(|| Mutex::new(CommStats::default()));
static SWSS_TO_IPFIX_TEMPLATES_STATS: Lazy<Mutex<CommStats>> =
    Lazy::new(|| Mutex::new(CommStats::default()));
static IPFIX_TO_STATS_REPORTER_STATS: Lazy<Mutex<CommStats>> =
    Lazy::new(|| Mutex::new(CommStats::default()));
static IPFIX_TO_COUNTER_DB_STATS: Lazy<Mutex<CommStats>> =
    Lazy::new(|| Mutex::new(CommStats::default()));
static IPFIX_TO_OTEL_STATS: Lazy<Mutex<CommStats>> = Lazy::new(|| Mutex::new(CommStats::default()));

/// Records a communication channel length sample and logs periodically.
pub fn record_comm_stats(label: ChannelLabel, channel_len: usize) {
    let mut stats = label.stats().lock().expect("comm stats mutex poisoned");
    stats.count = stats.count.saturating_add(1);
    stats.sum = stats.sum.saturating_add(channel_len as u64);
    stats.sum_sq = stats
        .sum_sq
        .saturating_add((channel_len as u128).saturating_mul(channel_len as u128));
    stats.last = channel_len;
    if channel_len > 0 {
        stats.nonzero_count = stats.nonzero_count.saturating_add(1);
    }
    if stats.count == 1 || channel_len < stats.min {
        stats.min = channel_len;
    }
    stats.max = stats.max.max(channel_len);

    let now = Instant::now();
    let interval = Duration::from_secs(COMM_LOG_INTERVAL_SECS.load(Ordering::Relaxed));
    if now.duration_since(stats.last_log) < interval {
        return;
    }

    let capacity = stats.capacity;
    let snapshot = std::mem::replace(
        &mut *stats,
        CommStats {
            capacity,
            last_log: now,
            ..CommStats::default()
        },
    );
    drop(stats);

    let avg = snapshot.sum as f64 / snapshot.count as f64;
    let rms = (snapshot.sum_sq as f64 / snapshot.count as f64).sqrt();
    if snapshot.capacity > 0 {
        let avg_util = avg / snapshot.capacity as f64;
        let peak_util = snapshot.max as f64 / snapshot.capacity as f64;
        info!(
            "Comm stats [{}]: count={}, avg_len={:.2}, peak_len={}, min_len={}, last_len={}, rms_len={:.2}, nonzero_count={}, capacity={}, avg_util={:.2}, peak_util={:.2}",
            label.as_str(),
            snapshot.count,
            avg,
            snapshot.max,
            snapshot.min,
            snapshot.last,
            rms,
            snapshot.nonzero_count,
            snapshot.capacity,
            avg_util,
            peak_util
        );
    } else {
        info!(
            "Comm stats [{}]: count={}, avg_len={:.2}, peak_len={}, min_len={}, last_len={}, rms_len={:.2}, nonzero_count={}",
            label.as_str(),
            snapshot.count,
            avg,
            snapshot.max,
            snapshot.min,
            snapshot.last,
            rms,
            snapshot.nonzero_count
        );
    }
}

/// Sets channel capacity for utilization analysis (optional).
/// Call this once during initialization if capacity is known.
pub fn set_comm_capacity(label: ChannelLabel, capacity: usize) {
    label
        .stats()
        .lock()
        .expect("comm stats mutex poisoned")
        .capacity = capacity;
}
