/// Utility helpers shared across countersyncd modules.

use std::collections::HashMap;
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
    if buffer.is_empty() {
        return String::new();
    }

    let mut lines = Vec::new();
    for chunk in buffer.chunks(BYTES_PER_LINE) {
        let line = chunk
            .iter()
            .map(|byte| format!("{:02x}", byte))
            .collect::<Vec<String>>()
            .join(" ");
        lines.push(line);
    }

    lines.join("\n")
}

/// Log interval for communication stats.
const COMM_LOG_INTERVAL: Duration = Duration::from_secs(600);

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

static COMM_STATS: Lazy<Mutex<HashMap<ChannelLabel, CommStats>>> =
    Lazy::new(|| Mutex::new(HashMap::new()));

/// Records a communication channel length sample and logs periodically.
pub fn record_comm_stats(label: ChannelLabel, channel_len: usize) {
    let mut stats_map = COMM_STATS
        .lock()
        .expect("COMM_STATS mutex poisoned");

    let stats = stats_map.entry(label).or_insert_with(CommStats::default);

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
    if channel_len > stats.max {
        stats.max = channel_len;
    }

    let now = Instant::now();
    if now.duration_since(stats.last_log) >= COMM_LOG_INTERVAL {
        let avg = stats.sum as f64 / stats.count as f64;
        let rms = (stats.sum_sq as f64 / stats.count as f64).sqrt();
        if stats.capacity > 0 {
            let avg_util = avg / stats.capacity as f64;
            let peak_util = stats.max as f64 / stats.capacity as f64;
            info!(
                "Comm stats [{}]: count={}, avg_len={:.2}, peak_len={}, min_len={}, last_len={}, rms_len={:.2}, nonzero_count={}, capacity={}, avg_util={:.2}, peak_util={:.2}",
                label.as_str(),
                stats.count,
                avg,
                stats.max,
                stats.min,
                stats.last,
                rms,
                stats.nonzero_count,
                stats.capacity,
                avg_util,
                peak_util
            );
        } else {
            info!(
                "Comm stats [{}]: count={}, avg_len={:.2}, peak_len={}, min_len={}, last_len={}, rms_len={:.2}, nonzero_count={}",
                label.as_str(),
                stats.count,
                avg,
                stats.max,
                stats.min,
                stats.last,
                rms,
                stats.nonzero_count
            );
        }
        let capacity = stats.capacity;
        *stats = CommStats {
            capacity,
            last_log: now,
            ..CommStats::default()
        };
    }
}

/// Sets channel capacity for utilization analysis (optional).
/// Call this once during initialization if capacity is known.
pub fn set_comm_capacity(label: ChannelLabel, capacity: usize) {
    let mut stats_map = COMM_STATS
        .lock()
        .expect("COMM_STATS mutex poisoned");
    let stats = stats_map.entry(label).or_insert_with(CommStats::default);
    stats.capacity = capacity;
}
