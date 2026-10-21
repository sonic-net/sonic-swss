use chrono::DateTime;
use std::sync::Arc;
use std::time::Duration;

use ahash::{HashMap, HashMapExt};
use log::{debug, info};
use tokio::{
    select,
    sync::mpsc::Receiver,
    time::{interval, Interval},
};

use super::super::message::saistats::{
    SAIStatMetadata, SAIStatRef, SAIStatsBatchMessage, SAIStatsView,
};
use crate::sai::{
    SaiBufferPoolStat, SaiIngressPriorityGroupStat, SaiObjectType, SaiPortStat, SaiQueueStat,
};
use crate::utilities::{record_comm_stats, ChannelLabel};

/// Unique key for identifying a specific counter based on the triplet
/// (object_name, type_id, stat_id)
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct CounterKey {
    pub object_name: Arc<str>,
    pub type_id: u32,
    pub stat_id: u32,
}

impl CounterKey {
    pub fn new(object_name: Arc<str>, type_id: u32, stat_id: u32) -> Self {
        Self {
            object_name,
            type_id,
            stat_id,
        }
    }
}

/// Counter information including the latest value and associated metadata
#[derive(Debug, Clone)]
pub struct CounterInfo {
    pub counter: u64,
    pub last_observation_time: u64,
    pub type_name: Option<&'static str>,
    pub stat_name: Option<&'static str>,
}

/// Trait for output writing to enable testing
pub trait OutputWriter: Send + Sync {
    fn write_line(&mut self, line: &str);
}

/// Console writer implementation
pub struct ConsoleWriter;

impl OutputWriter for ConsoleWriter {
    fn write_line(&mut self, line: &str) {
        println!("{}", line);
    }
}

/// Test writer that captures output
#[cfg(test)]
pub struct TestWriter {
    pub lines: Vec<String>,
}

#[cfg(test)]
impl TestWriter {
    pub fn new() -> Self {
        Self { lines: Vec::new() }
    }

    #[allow(dead_code)]
    pub fn get_output(&self) -> &[String] {
        &self.lines
    }
}

#[cfg(test)]
impl OutputWriter for TestWriter {
    fn write_line(&mut self, line: &str) {
        self.lines.push(line.to_string());
    }
}

/// Configuration for the StatsReporterActor
#[derive(Debug, Clone)]
pub struct StatsReporterConfig {
    /// Reporting interval - how often to print the latest statistics
    pub interval: Duration,
    /// Whether to print detailed statistics or summary only
    pub detailed: bool,
    /// Maximum number of statistics to display per report
    pub max_stats_per_report: Option<usize>,
}

impl Default for StatsReporterConfig {
    fn default() -> Self {
        Self {
            interval: Duration::from_secs(10),
            detailed: true,
            max_stats_per_report: None,
        }
    }
}

/// Templates with at most this many fields keep their latest values in their
/// hot table entry.
const INLINE_VALUES: usize = 3;
const MIN_TABLE_CAPACITY: usize = 16;

/// Per-template state touched by every record: one cache line per template.
#[derive(Clone, Copy)]
#[repr(C, align(64))]
struct HotSlot {
    /// Metadata address of the template; 0 marks an empty table entry.
    address: usize,
    /// Sequence number of the latest record.
    last_seq: u64,
    /// Records received since the last fold.
    records: u64,
    observation_time: u64,
    /// Index of the template in `StatsReporterActor::slots`.
    slot: u32,
    /// Fields (values) per record.
    len: u32,
    /// Latest values of a template with at most INLINE_VALUES fields.
    inline: [u64; INLINE_VALUES],
}

const EMPTY_HOT_SLOT: HotSlot = HotSlot {
    address: 0,
    last_seq: 0,
    records: 0,
    observation_time: 0,
    slot: 0,
    len: 0,
    inline: [0; INLINE_VALUES],
};

/// Cold data of one IPFIX template.
///
/// IpfixActor shares one metadata allocation per template across all of that
/// template's records, so the allocation address identifies the template. The
/// slot keeps a clone of the metadata, so the address cannot be reused by
/// another allocation while the slot exists.
struct TemplateSlot {
    metadata: Arc<[SAIStatMetadata]>,
    /// Index in `StatsReporterActor::counters` of every field, in field order.
    counter_indices: Box<[u32]>,
    /// Latest values of a template with more than INLINE_VALUES fields.
    values: Box<[u64]>,
}

/// Reporting state of one (object_name, type_id, stat_id) counter.
struct CounterState {
    key: CounterKey,
    /// Latest value, its observation time and the resolved names.
    info: CounterInfo,
    /// Sequence number of the record that set `info`; 0 means no value yet.
    seq: u64,
    /// Occurrences in the current reporting period.
    period_messages: u64,
}

fn metadata_address(metadata: &Arc<[SAIStatMetadata]>) -> usize {
    Arc::as_ptr(metadata) as *const SAIStatMetadata as usize
}

/// Actor responsible for consuming SAI statistics messages and reporting them to the terminal.
///
/// The StatsReporterActor handles:
/// - Receiving SAI statistics messages from IPFIX processor
/// - Maintaining the latest statistics state per counter key (object_name, type_id, stat_id)
/// - Tracking message counts per counter key for each reporting period
/// - Periodic reporting based on configured interval
/// - Formatted output to terminal with optional detail levels
///
/// Records that share template metadata are tracked per template: a record
/// only stores its values and bumps the template's record count. Template
/// slots are folded into per-counter state when a report is generated, so
/// the per-record cost does not depend on hashing counter keys.
pub struct StatsReporterActor<W: OutputWriter> {
    /// Channel for receiving SAI statistics messages
    stats_receiver: Receiver<SAIStatsBatchMessage>,
    /// Configuration for reporting behavior
    config: StatsReporterConfig,
    /// Timer for periodic reporting
    report_timer: Interval,
    /// Cold data of every template seen in shared (template-backed) records.
    slots: Vec<TemplateSlot>,
    /// Open-addressing table of hot per-template state keyed by metadata
    /// address; its length is a power of two of at least twice `slots.len()`.
    table: Vec<HotSlot>,
    /// 64 - log2(table.len()), for Fibonacci hashing of addresses.
    table_shift: u32,
    /// Most recently used (address, table position); consecutive records
    /// usually share a template.
    last_slot: (usize, usize),
    /// Every counter seen, with its latest value and per-period occurrences.
    counters: Vec<CounterState>,
    /// (object_name, type_id, stat_id) -> index in `counters`.
    counter_index: HashMap<CounterKey, u32>,
    /// Total records received; also the sequence number of the last record.
    total_messages_received: u64,
    /// Counter for total reports generated
    reports_generated: u64,
    /// Output writer for dependency injection
    writer: W,
}

impl<W: OutputWriter> StatsReporterActor<W> {
    /// Creates a new StatsReporterActor instance.
    ///
    /// # Arguments
    ///
    /// * `stats_receiver` - Channel for receiving SAI statistics messages
    /// * `config` - Configuration for reporting behavior
    /// * `writer` - Output writer for dependency injection
    ///
    /// # Returns
    ///
    /// A new StatsReporterActor instance
    pub fn new(
        stats_receiver: Receiver<SAIStatsBatchMessage>,
        config: StatsReporterConfig,
        writer: W,
    ) -> Self {
        let report_timer = interval(config.interval);

        info!(
            "StatsReporter initialized with interval: {:?}, detailed: {}",
            config.interval, config.detailed
        );

        Self {
            stats_receiver,
            config,
            report_timer,
            slots: Vec::new(),
            table: vec![EMPTY_HOT_SLOT; MIN_TABLE_CAPACITY],
            table_shift: 64 - MIN_TABLE_CAPACITY.trailing_zeros(),
            last_slot: (0, 0),
            counters: Vec::new(),
            counter_index: HashMap::new(),
            total_messages_received: 0,
            reports_generated: 0,
            writer,
        }
    }

    /// Creates a new StatsReporterActor with default configuration and console writer.
    ///
    /// # Arguments
    ///
    /// * `stats_receiver` - Channel for receiving SAI statistics messages
    ///
    /// # Returns
    ///
    /// A new StatsReporterActor instance with default settings
    #[allow(dead_code)]
    pub fn new_with_defaults(
        stats_receiver: Receiver<SAIStatsBatchMessage>,
    ) -> StatsReporterActor<ConsoleWriter> {
        StatsReporterActor::new(
            stats_receiver,
            StatsReporterConfig::default(),
            ConsoleWriter,
        )
    }

    /// Index of the counter for `stat`, registering it on first sight.
    fn counter_for(&mut self, stat: SAIStatRef<'_>) -> u32 {
        let key = CounterKey::new(Arc::clone(stat.object_name), stat.type_id, stat.stat_id);
        if let Some(&index) = self.counter_index.get(&key) {
            return index;
        }
        let index = u32::try_from(self.counters.len()).expect("counter count fits u32");
        self.counters.push(CounterState {
            key: key.clone(),
            info: CounterInfo {
                counter: 0,
                last_observation_time: 0,
                type_name: stat.type_name(),
                stat_name: stat.stat_name(),
            },
            seq: 0,
            period_messages: 0,
        });
        self.counter_index.insert(key, index);
        index
    }

    #[inline]
    fn home_position(&self, address: usize) -> usize {
        ((address as u64).wrapping_mul(0x9e37_79b9_7f4a_7c15) >> self.table_shift) as usize
    }

    /// Table position of the template that owns `metadata`.
    #[inline]
    fn position_for(&mut self, metadata: &Arc<[SAIStatMetadata]>, view: SAIStatsView<'_>) -> usize {
        let address = metadata_address(metadata);
        if address == self.last_slot.0 {
            return self.last_slot.1;
        }
        let mask = self.table.len() - 1;
        let mut position = self.home_position(address);
        let position = loop {
            match self.table[position].address {
                found if found == address => break position,
                0 => break self.register_slot(metadata, view),
                _ => position = (position + 1) & mask,
            }
        };
        self.last_slot = (address, position);
        position
    }

    #[cold]
    fn register_slot(&mut self, metadata: &Arc<[SAIStatMetadata]>, view: SAIStatsView<'_>) -> usize {
        let counter_indices = view.iter().map(|stat| self.counter_for(stat)).collect();
        let len = metadata.len();
        let slot = u32::try_from(self.slots.len()).expect("slot count fits u32");
        self.slots.push(TemplateSlot {
            metadata: Arc::clone(metadata),
            counter_indices,
            values: if len > INLINE_VALUES {
                vec![0; len].into_boxed_slice()
            } else {
                Box::default()
            },
        });
        if 2 * self.slots.len() > self.table.len() {
            let capacity = 2 * self.table.len();
            self.rebuild_table(capacity, Some);
        }
        self.insert_hot(HotSlot {
            address: metadata_address(metadata),
            slot,
            len: u32::try_from(len).expect("template length fits u32"),
            ..EMPTY_HOT_SLOT
        })
    }

    fn insert_hot(&mut self, entry: HotSlot) -> usize {
        let mask = self.table.len() - 1;
        let mut position = self.home_position(entry.address);
        while self.table[position].address != 0 {
            position = (position + 1) & mask;
        }
        self.table[position] = entry;
        position
    }

    /// Rehash into `capacity` entries, renumbering or dropping slots by `remap`.
    fn rebuild_table(&mut self, capacity: usize, remap: impl Fn(u32) -> Option<u32>) {
        let old = std::mem::replace(&mut self.table, vec![EMPTY_HOT_SLOT; capacity]);
        self.table_shift = 64 - capacity.trailing_zeros();
        for mut entry in old.into_iter().filter(|entry| entry.address != 0) {
            if let Some(slot) = remap(entry.slot) {
                entry.slot = slot;
                self.insert_hot(entry);
            }
        }
        self.last_slot = (0, 0);
    }

    /// Updates the internal state with new statistics data.
    ///
    /// For each statistic in the message, updates:
    /// - The latest counter value for the (object_name, type_id, stat_id) key
    /// - The message count for that key in the current reporting period
    ///
    /// Template-backed records update their template slot; the per-counter
    /// view is produced by `fold()`.
    ///
    /// # Arguments
    ///
    /// * `stats_batch` - New batch of SAI statistics records to process
    fn update_stats(&mut self, stats_batch: SAIStatsBatchMessage) {
        debug!(
            "Received SAI stats batch with {} records and {} counters",
            stats_batch.record_count(),
            stats_batch.counter_count()
        );
        for stats in stats_batch.iter() {
            self.total_messages_received += 1;
            let seq = self.total_messages_received;
            match stats.stats {
                SAIStatsView::Shared { metadata, values } => {
                    let position = self.position_for(metadata, stats.stats);
                    let entry = &mut self.table[position];
                    entry.observation_time = stats.observation_time;
                    entry.last_seq = seq;
                    entry.records += 1;
                    if values.len() <= INLINE_VALUES {
                        for (target, value) in entry.inline.iter_mut().zip(values) {
                            *target = *value;
                        }
                    } else {
                        let slot = entry.slot as usize;
                        self.slots[slot].values.copy_from_slice(values);
                    }
                }
                SAIStatsView::Owned(_) => {
                    for stat in stats.stats {
                        let index = self.counter_for(stat) as usize;
                        let state = &mut self.counters[index];
                        state.info.counter = stat.counter;
                        state.info.last_observation_time = stats.observation_time;
                        state.seq = seq;
                        state.period_messages += 1;
                    }
                }
            }
        }
    }

    /// Applies template slots to per-counter state: the latest value in
    /// record order wins, and every record counts once per field. Slots of
    /// templates no longer referenced outside this actor are then dropped;
    /// their counters keep their latest values.
    fn fold(&mut self) {
        for entry in self.table.iter_mut() {
            if entry.address == 0 || entry.records == 0 {
                continue;
            }
            let slot = &self.slots[entry.slot as usize];
            let values: &[u64] = if entry.len as usize <= INLINE_VALUES {
                &entry.inline[..entry.len as usize]
            } else {
                &slot.values
            };
            for (field, &index) in slot.counter_indices.iter().enumerate() {
                let state = &mut self.counters[index as usize];
                // `>=`: a later field of the same record wins, as in record order.
                if entry.last_seq >= state.seq {
                    state.info.counter = values[field];
                    state.info.last_observation_time = entry.observation_time;
                    state.seq = entry.last_seq;
                }
                state.period_messages += entry.records;
            }
            entry.records = 0;
        }
        if self
            .slots
            .iter()
            .any(|slot| Arc::strong_count(&slot.metadata) == 1)
        {
            let mut remap = vec![u32::MAX; self.slots.len()];
            let mut kept = Vec::with_capacity(self.slots.len());
            for (old, slot) in std::mem::take(&mut self.slots).into_iter().enumerate() {
                if Arc::strong_count(&slot.metadata) > 1 {
                    remap[old] = kept.len() as u32;
                    kept.push(slot);
                }
            }
            self.slots = kept;
            let capacity = self.table.len();
            self.rebuild_table(capacity, |slot| {
                Some(remap[slot as usize]).filter(|&kept| kept != u32::MAX)
            });
        }
    }

    /// Generates and prints a statistics report to the terminal.
    ///
    /// Reports all current counter values and their triplets, as well as
    /// message counts for the current reporting period. After reporting,
    /// clears the per-period message counters.
    fn generate_report(&mut self) {
        self.fold();
        self.reports_generated += 1;

        if !self.counters.iter().any(|state| state.seq != 0) {
            self.writer.write_line(&format!(
                "[Report #{}] No statistics data available yet",
                self.reports_generated
            ));
            self.writer.write_line(&format!(
                "   Total Messages Received: {}",
                self.total_messages_received
            ));
        } else {
            self.print_counters_report();
        }

        // Clear per-period message counters for next reporting period
        for state in &mut self.counters {
            state.period_messages = 0;
        }

        self.writer.write_line(""); // Add blank line for readability
    }

    /// Prints formatted counters report to terminal.
    ///
    /// Shows all current counters with their triplet keys and the number of
    /// messages received for each counter in the current reporting period.
    fn print_counters_report(&mut self) {
        let unique_counters = self.counters.iter().filter(|state| state.seq != 0).count();
        self.writer.write_line(&format!(
            "[Report #{}] SAI Counters Report",
            self.reports_generated
        ));
        self.writer.write_line(&format!(
            "   Total Unique Counters: {}",
            unique_counters
        ));
        self.writer.write_line(&format!(
            "   Total Messages Received: {}",
            self.total_messages_received
        ));

        if self.config.detailed && unique_counters > 0 {
            // Group by SAI object type for better organization
            use std::collections::BTreeMap;
            let mut grouped_counters: BTreeMap<u32, Vec<&CounterState>> = BTreeMap::new();

            for state in self.counters.iter().filter(|state| state.seq != 0) {
                grouped_counters
                    .entry(state.key.type_id)
                    .or_insert_with(Vec::new)
                    .push(state);
            }

            self.writer.write_line("   Detailed Counters:");

            let mut total_shown = 0;
            for (type_id, mut counters) in grouped_counters {
                // Sort counters within each type by object name and stat id
                counters.sort_by(|a, b| {
                    a.key
                        .object_name
                        .cmp(&b.key.object_name)
                        .then_with(|| a.key.stat_id.cmp(&b.key.stat_id))
                });

                let type_name = counters
                    .first()
                    .and_then(|state| state.info.type_name)
                    .map(std::borrow::Cow::Borrowed)
                    .unwrap_or_else(|| std::borrow::Cow::Owned(type_id_to_string(type_id)));
                self.writer
                    .write_line(&format!("      Type: {} ({})", type_name, type_id));

                let counters_to_show = if let Some(max) = self.config.max_stats_per_report {
                    let remaining = max.saturating_sub(total_shown);
                    &counters[..std::cmp::min(remaining, counters.len())]
                } else {
                    &counters
                };

                for (index, state) in counters_to_show.iter().enumerate() {
                    let messages_per_second =
                        state.period_messages as f64 / self.config.interval.as_secs_f64();
                    let stat_name = state
                        .info
                        .stat_name
                        .map(remove_sai_prefix)
                        .unwrap_or_else(|| stat_id_to_string(state.key.type_id, state.key.stat_id));
                    let formatted_time = format_timestamp(state.info.last_observation_time);

                    self.writer.write_line(&format!(
                        "         [{:3}] Object: {:15}, Stat: {:25}, Counter: {:15}, Msg/s: {:6.1}, LastTime: {}",
                        index + 1,
                        state.key.object_name,
                        stat_name,
                        state.info.counter,
                        messages_per_second,
                        formatted_time
                    ));
                }

                total_shown += counters_to_show.len();
                if let Some(max) = self.config.max_stats_per_report {
                    if total_shown >= max && unique_counters > max {
                        self.writer.write_line(&format!(
                            "         ... and {} more counters (use max_stats_per_report: None to show all)",
                            unique_counters - max
                        ));
                        break;
                    }
                }
            }
        } else if !self.config.detailed && unique_counters > 0 {
            // Summary mode - show aggregate information
            let live = self.counters.iter().filter(|state| state.seq != 0);
            let total_counter_value: u128 =
                live.clone().map(|state| state.info.counter as u128).sum();
            let unique_types = live
                .clone()
                .map(|state| state.key.type_id)
                .collect::<std::collections::HashSet<_>>()
                .len();
            let unique_objects = live
                .map(|state| &state.key.object_name)
                .collect::<std::collections::HashSet<_>>()
                .len();
            let total_messages_in_period: u64 =
                self.counters.iter().map(|state| state.period_messages).sum();
            let messages_per_second =
                total_messages_in_period as f64 / self.config.interval.as_secs_f64();

            self.writer.write_line("   Summary:");
            self.writer.write_line(&format!(
                "      Total Counter Value: {}",
                total_counter_value
            ));
            self.writer
                .write_line(&format!("      Unique Types: {}", unique_types));
            self.writer
                .write_line(&format!("      Unique Objects: {}", unique_objects));
            self.writer.write_line(&format!(
                "      Messages per Second: {:.1}",
                messages_per_second
            ));
        }
    }

    /// Latest value of every counter, as the per-counter implementation kept it.
    #[cfg(test)]
    fn latest_counters(&mut self) -> HashMap<CounterKey, CounterInfo> {
        self.fold();
        self.counters
            .iter()
            .filter(|state| state.seq != 0)
            .map(|state| (state.key.clone(), state.info.clone()))
            .collect()
    }

    /// Occurrences per counter in the current period (counters seen in it only).
    #[cfg(test)]
    fn messages_per_counter(&mut self) -> HashMap<CounterKey, u64> {
        self.fold();
        self.counters
            .iter()
            .filter(|state| state.period_messages != 0)
            .map(|state| (state.key.clone(), state.period_messages))
            .collect()
    }

    /// Main event loop for the StatsReporterActor.
    ///
    /// Continuously processes incoming statistics messages and generates periodic reports.
    /// The loop will exit when the statistics channel is closed.
    ///
    /// # Arguments
    ///
    /// * `actor` - The StatsReporterActor instance to run
    pub async fn run(mut actor: StatsReporterActor<W>) {
        info!("StatsReporter actor started");

        loop {
            select! {
                // Handle incoming statistics messages
                stats_msg = actor.stats_receiver.recv() => {
                    match stats_msg {
                        Some(stats) => {
                            record_comm_stats(
                                ChannelLabel::IpfixToStatsReporter,
                                actor.stats_receiver.len(),
                            );
                            actor.update_stats(stats);
                        }
                        None => {
                            info!("Stats receiver channel closed, shutting down reporter");
                            break;
                        }
                    }
                }

                // Handle periodic reporting
                _ = actor.report_timer.tick() => {
                    actor.generate_report();
                }
            }
        }

        // Generate final report before shutdown
        info!("Generating final report before shutdown...");
        actor.generate_report();
        info!(
            "StatsReporter actor terminated. Total reports generated: {}",
            actor.reports_generated
        );
    }
}

impl<W: OutputWriter> Drop for StatsReporterActor<W> {
    fn drop(&mut self) {
        info!(
            "StatsReporter dropped after {} reports and {} messages",
            self.reports_generated, self.total_messages_received
        );
    }
}

/// Helper function to convert type_id to string representation
fn type_id_to_string(type_id: u32) -> String {
    match SaiObjectType::try_from(type_id) {
        Ok(sai_type) => format!("{:?}", sai_type),
        Err(_) => format!("UNKNOWN({})", type_id),
    }
}

/// Helper function to remove SAI prefixes from stat names
fn remove_sai_prefix(stat_name: &str) -> String {
    // Remove common SAI stat prefixes using regex pattern
    // Pattern: SAI_<TYPE>_STAT_<ACTUAL_NAME>
    if stat_name.starts_with("SAI_") && stat_name.contains("_STAT_") {
        // Find the position of "_STAT_" and return everything after it
        if let Some(stat_pos) = stat_name.find("_STAT_") {
            let start_pos = stat_pos + "_STAT_".len();
            stat_name[start_pos..].to_string()
        } else {
            stat_name.to_string()
        }
    } else {
        // If no SAI pattern found, return as-is
        stat_name.to_string()
    }
}

/// Helper function to convert stat_id to string representation
fn stat_id_to_string(type_id: u32, stat_id: u32) -> String {
    // Convert type_id to SaiObjectType first
    match SaiObjectType::try_from(type_id) {
        Ok(object_type) => {
            match object_type {
                SaiObjectType::Port => {
                    // Convert stat_id to SaiPortStat and get its C name
                    if let Some(port_stat) = SaiPortStat::from_u32(stat_id) {
                        remove_sai_prefix(port_stat.to_c_name())
                    } else {
                        format!("UNKNOWN_PORT_STAT_{}", stat_id)
                    }
                }
                SaiObjectType::Queue => {
                    // Convert stat_id to SaiQueueStat and get its C name
                    if let Some(queue_stat) = SaiQueueStat::from_u32(stat_id) {
                        remove_sai_prefix(queue_stat.to_c_name())
                    } else {
                        format!("UNKNOWN_QUEUE_STAT_{}", stat_id)
                    }
                }
                SaiObjectType::BufferPool => {
                    // Convert stat_id to SaiBufferPoolStat and get its C name
                    if let Some(buffer_stat) = SaiBufferPoolStat::from_u32(stat_id) {
                        remove_sai_prefix(buffer_stat.to_c_name())
                    } else {
                        format!("UNKNOWN_BUFFER_POOL_STAT_{}", stat_id)
                    }
                }
                SaiObjectType::IngressPriorityGroup => {
                    // Convert stat_id to SaiIngressPriorityGroupStat and get its C name
                    if let Some(ipg_stat) = SaiIngressPriorityGroupStat::from_u32(stat_id) {
                        remove_sai_prefix(ipg_stat.to_c_name())
                    } else {
                        format!("UNKNOWN_IPG_STAT_{}", stat_id)
                    }
                }
                _ => {
                    format!("UNSUPPORTED_TYPE_{}_STAT_{}", type_id, stat_id)
                }
            }
        }
        Err(_) => {
            format!("INVALID_TYPE_{}_STAT_{}", type_id, stat_id)
        }
    }
}

/// Helper function to format timestamp with nanosecond precision
fn format_timestamp(timestamp_ns: u64) -> String {
    // Convert nanoseconds to seconds and nanoseconds
    let secs = (timestamp_ns / 1_000_000_000) as i64;
    let nanos = (timestamp_ns % 1_000_000_000) as u32;

    // Create DateTime from the timestamp using the new API
    match DateTime::from_timestamp(secs, nanos) {
        Some(utc_dt) => {
            // Format as "YYYY-MM-DD HH:MM:SS.nnnnnnnnn UTC"
            utc_dt.format("%Y-%m-%d %H:%M:%S.%f UTC").to_string()
        }
        None => {
            // Fallback to original format if conversion fails
            format!("{}.{:09}", secs, nanos)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use tokio::{spawn, sync::mpsc::channel, time::sleep};

    use crate::message::saistats::{SAIStat, SAIStats, SAIStatsBatch};

    /// Helper function to create test SAI statistics
    fn create_test_stats(observation_time: u64, stat_count: usize) -> SAIStats {
        let stats = (0..stat_count)
            .map(|i| {
                SAIStat::new(
                    format!("Ethernet{}", i),
                    (i * 100) as u32,
                    (i * 10) as u32,
                    (i * 1000) as u64,
                )
            })
            .collect();

        SAIStats {
            observation_time,
            stats,
        }
    }

    #[tokio::test]
    async fn test_update_stats_counts_and_processes_batch_records_in_order() {
        let (_sender, receiver) = channel(1);
        let mut actor =
            StatsReporterActor::new(receiver, StatsReporterConfig::default(), TestWriter::new());
        let mut batch = SAIStatsBatch::with_capacity(2, 2);
        batch.push_record(10, [SAIStat::new("Ethernet0", 1, 2, 100)]);
        batch.push_record(20, [SAIStat::new("Ethernet0", 1, 2, 200)]);

        actor.update_stats(Arc::new(batch));

        let key = CounterKey::new("Ethernet0".into(), 1, 2);
        assert_eq!(actor.total_messages_received, 2);
        assert_eq!(actor.messages_per_counter().get(&key), Some(&2));
        let latest_counters = actor.latest_counters();
        let latest = latest_counters.get(&key).unwrap();
        assert_eq!(latest.counter, 200);
        assert_eq!(latest.last_observation_time, 20);
    }

    #[tokio::test]
    async fn shared_metadata_batches_preserve_reporting_semantics() {
        use crate::message::saistats::SAIStatMetadata;
        let (_,rx)=channel(1);
        let mut actor=StatsReporterActor::new(rx,StatsReporterConfig::default(),TestWriter::new());
        let metadata=Arc::from(vec![SAIStatMetadata::new("Ethernet0",1,0)]);
        let mut batch=SAIStatsBatch::default();
        batch.push_shared_record(10,Arc::clone(&metadata),[123]);
        batch.push_shared_record(20,metadata,[456]);
        actor.update_stats(Arc::new(batch));
        let key=CounterKey::new(Arc::from("Ethernet0"),1,0);
        assert_eq!(actor.total_messages_received,2);
        assert_eq!(actor.messages_per_counter().get(&key),Some(&2));
        assert_eq!(actor.latest_counters()[&key].counter,456);
        assert_eq!(actor.latest_counters()[&key].last_observation_time,20);
    }

    #[tokio::test]
    async fn test_stats_reporter_basic_functionality() {
        let (sender, receiver) = channel(10);
        let test_writer = TestWriter::new();

        let config = StatsReporterConfig {
            interval: Duration::from_millis(200),
            detailed: true,
            max_stats_per_report: Some(3),
        };

        // Create actor with test writer
        let actor = StatsReporterActor::new(receiver, config, test_writer);
        let handle = spawn(StatsReporterActor::run(actor));

        // Send test statistics
        let test_stats = create_test_stats(12345, 5);
        sender
            .send(Arc::new(SAIStatsBatch::from_stats(test_stats)))
            .await
            .unwrap();

        // Wait for processing
        sleep(Duration::from_millis(50)).await;

        // Wait for at least one report
        sleep(Duration::from_millis(250)).await;

        // Send another set of statistics
        let test_stats2 = create_test_stats(67890, 2);
        sender
            .send(Arc::new(SAIStatsBatch::from_stats(test_stats2)))
            .await
            .unwrap();

        // Wait for processing
        sleep(Duration::from_millis(50)).await;

        // Close the channel to terminate the actor
        drop(sender);

        // Wait for actor to finish
        let _finished_actor = handle.await.expect("Actor should complete successfully");
    }

    #[tokio::test]
    async fn test_stats_reporter_with_shared_writer() {
        use std::sync::{Arc, Mutex};

        // Shared writer that can be accessed from multiple places
        #[derive(Clone)]
        struct SharedTestWriter {
            lines: Arc<Mutex<Vec<String>>>,
        }

        impl SharedTestWriter {
            fn new() -> Self {
                Self {
                    lines: Arc::new(Mutex::new(Vec::new())),
                }
            }

            fn get_lines(&self) -> Vec<String> {
                self.lines.lock().unwrap().clone()
            }
        }

        impl OutputWriter for SharedTestWriter {
            fn write_line(&mut self, line: &str) {
                self.lines.lock().unwrap().push(line.to_string());
            }
        }

        let (sender, receiver) = channel(10);
        let shared_writer = SharedTestWriter::new();
        let writer_clone = shared_writer.clone();

        let config = StatsReporterConfig {
            interval: Duration::from_millis(200),
            detailed: true,
            max_stats_per_report: Some(3),
        };

        // Create actor with shared writer
        let actor = StatsReporterActor::new(receiver, config, shared_writer);
        let handle = spawn(StatsReporterActor::run(actor));

        // Send test statistics
        let test_stats = create_test_stats(12345, 5);
        sender
            .send(Arc::new(SAIStatsBatch::from_stats(test_stats)))
            .await
            .unwrap();

        // Wait for processing
        sleep(Duration::from_millis(50)).await;

        // Wait for at least one report
        sleep(Duration::from_millis(250)).await;

        // Send another set of statistics
        let test_stats2 = create_test_stats(67890, 2);
        sender
            .send(Arc::new(SAIStatsBatch::from_stats(test_stats2)))
            .await
            .unwrap();

        // Wait for processing
        sleep(Duration::from_millis(50)).await;

        // Close the channel to terminate the actor
        drop(sender);

        // Wait for actor to finish
        handle.await.expect("Actor should complete successfully");

        // Now we can check the output
        let output = writer_clone.get_lines();

        // Verify we have some output
        assert!(!output.is_empty(), "Should have captured some output");

        // Verify report header is present (now "SAI Counters Report")
        let has_report_header = output
            .iter()
            .any(|line| line.contains("SAI Counters Report"));
        assert!(has_report_header, "Should contain counters report header");

        // Verify counter count for all unique counters (first 5 + 2 overlapping = 5 unique)
        let has_counter_count = output
            .iter()
            .any(|line| line.contains("Total Unique Counters: 5"));
        assert!(
            has_counter_count,
            "Should show correct unique counters count"
        );

        // Verify detailed output
        let has_detailed = output
            .iter()
            .any(|line| line.contains("Detailed Counters:"));
        assert!(has_detailed, "Should show detailed counters");

        // Verify individual counter entries with new format
        let has_counter_entry = output.iter().any(|line| {
            line.contains("Object:") && line.contains("Stat:") && line.contains("Msg/s:")
        });
        assert!(
            has_counter_entry,
            "Should show individual counter entries with message counts"
        );
    }

    #[tokio::test]
    async fn test_stats_reporter_summary_mode() {
        use std::sync::{Arc, Mutex};

        #[derive(Clone)]
        struct SharedTestWriter {
            lines: Arc<Mutex<Vec<String>>>,
        }

        impl SharedTestWriter {
            fn new() -> Self {
                Self {
                    lines: Arc::new(Mutex::new(Vec::new())),
                }
            }

            fn get_lines(&self) -> Vec<String> {
                self.lines.lock().unwrap().clone()
            }
        }

        impl OutputWriter for SharedTestWriter {
            fn write_line(&mut self, line: &str) {
                self.lines.lock().unwrap().push(line.to_string());
            }
        }

        let (sender, receiver) = channel(10);
        let shared_writer = SharedTestWriter::new();
        let writer_clone = shared_writer.clone();

        let config = StatsReporterConfig {
            interval: Duration::from_millis(100),
            detailed: false, // Summary mode
            max_stats_per_report: None,
        };

        let actor = StatsReporterActor::new(receiver, config, shared_writer);
        let handle = spawn(StatsReporterActor::run(actor));

        // Send test statistics with known values
        let test_stats = create_test_stats(99999, 3);
        sender
            .send(Arc::new(SAIStatsBatch::from_stats(test_stats)))
            .await
            .unwrap();

        // Wait for processing and one report
        sleep(Duration::from_millis(150)).await;

        // Close and finish
        drop(sender);
        handle.await.expect("Actor should complete successfully");

        // Verify captured output
        let output = writer_clone.get_lines();

        // Verify we have output
        assert!(!output.is_empty(), "Should have captured some output");

        // Verify summary mode elements
        let has_summary_header = output.iter().any(|line| line.contains("Summary:"));
        assert!(has_summary_header, "Should contain summary header");

        // Verify total counter calculation (0 + 1000 + 2000 = 3000)
        let has_total_counter = output
            .iter()
            .any(|line| line.contains("Total Counter Value: 3000"));
        assert!(has_total_counter, "Should show correct total counter value");

        // Verify unique counts
        let has_unique_types = output.iter().any(|line| line.contains("Unique Types: 3"));
        assert!(has_unique_types, "Should show correct unique types count");

        let has_unique_labels = output.iter().any(|line| line.contains("Unique Objects: 3"));
        assert!(
            has_unique_labels,
            "Should show correct unique objects count"
        );

        // Should NOT have detailed counters
        let has_detailed = output
            .iter()
            .any(|line| line.contains("Detailed Counters:"));
        assert!(
            !has_detailed,
            "Should NOT show detailed counters in summary mode"
        );

        // Should show messages per second
        let has_messages_per_second = output
            .iter()
            .any(|line| line.contains("Messages per Second:"));
        assert!(
            has_messages_per_second,
            "Should show messages per second in summary mode"
        );
    }

    #[tokio::test]
    async fn test_stats_reporter_no_data() {
        use std::sync::{Arc, Mutex};

        #[derive(Clone)]
        struct SharedTestWriter {
            lines: Arc<Mutex<Vec<String>>>,
        }

        impl SharedTestWriter {
            fn new() -> Self {
                Self {
                    lines: Arc::new(Mutex::new(Vec::new())),
                }
            }

            fn get_lines(&self) -> Vec<String> {
                self.lines.lock().unwrap().clone()
            }
        }

        impl OutputWriter for SharedTestWriter {
            fn write_line(&mut self, line: &str) {
                self.lines.lock().unwrap().push(line.to_string());
            }
        }

        let (sender, receiver) = channel(10);
        let shared_writer = SharedTestWriter::new();
        let writer_clone = shared_writer.clone();

        let config = StatsReporterConfig {
            interval: Duration::from_millis(50),
            detailed: true,
            max_stats_per_report: None,
        };

        let actor = StatsReporterActor::new(receiver, config, shared_writer);
        let handle = spawn(StatsReporterActor::run(actor));

        // Don't send any data, just wait for a report
        sleep(Duration::from_millis(100)).await;

        // Close the channel
        drop(sender);
        handle.await.expect("Actor should complete successfully");

        // Verify captured output
        let output = writer_clone.get_lines();

        // Verify we have output
        assert!(!output.is_empty(), "Should have captured some output");

        // Verify "no data" message
        let has_no_data_msg = output
            .iter()
            .any(|line| line.contains("No statistics data available yet"));
        assert!(has_no_data_msg, "Should show 'no data available' message");

        // Verify message count is 0
        let has_zero_messages = output
            .iter()
            .any(|line| line.contains("Total Messages Received: 0"));
        assert!(has_zero_messages, "Should show 0 total messages received");
    }

    #[tokio::test]
    async fn test_stats_reporter_max_stats_limit() {
        use std::sync::{Arc, Mutex};

        #[derive(Clone)]
        struct SharedTestWriter {
            lines: Arc<Mutex<Vec<String>>>,
        }

        impl SharedTestWriter {
            fn new() -> Self {
                Self {
                    lines: Arc::new(Mutex::new(Vec::new())),
                }
            }

            fn get_lines(&self) -> Vec<String> {
                self.lines.lock().unwrap().clone()
            }
        }

        impl OutputWriter for SharedTestWriter {
            fn write_line(&mut self, line: &str) {
                self.lines.lock().unwrap().push(line.to_string());
            }
        }

        let (sender, receiver) = channel(10);
        let shared_writer = SharedTestWriter::new();
        let writer_clone = shared_writer.clone();

        let config = StatsReporterConfig {
            interval: Duration::from_millis(500), // Longer interval to avoid multiple reports
            detailed: true,
            max_stats_per_report: Some(2), // Limit to 2 stats
        };

        let actor = StatsReporterActor::new(receiver, config, shared_writer);
        let handle = spawn(StatsReporterActor::run(actor));

        // Send stats with more entries than the limit
        let test_stats = create_test_stats(55555, 5);
        sender
            .send(Arc::new(SAIStatsBatch::from_stats(test_stats)))
            .await
            .unwrap();

        // Wait for processing but not long enough for multiple reports
        sleep(Duration::from_millis(50)).await;

        // Close and finish quickly to avoid multiple timer ticks
        drop(sender);
        handle.await.expect("Actor should complete successfully");

        // Verify captured output
        let output = writer_clone.get_lines();

        // Find the first detailed counters section
        let mut in_detailed_section = false;
        let mut counter_entries = Vec::new();

        for line in &output {
            if line.contains("Detailed Counters:") {
                in_detailed_section = true;
                continue;
            }

            if in_detailed_section {
                if line.contains("] Object:") && line.contains("Stat:") {
                    counter_entries.push(line);
                } else if line.contains("[Report") || line.trim().is_empty() {
                    // End of this detailed section
                    break;
                }
            }
        }

        // Should show exactly 2 counter entries in the first report
        assert_eq!(
            counter_entries.len(),
            2,
            "Should show exactly 2 counter entries due to limit"
        );

        // Verify "more counters" message
        let has_more_msg = output
            .iter()
            .any(|line| line.contains("and 3 more counters"));
        assert!(has_more_msg, "Should show 'more counters' message");

        // Verify total count is still correct
        let has_total_count = output
            .iter()
            .any(|line| line.contains("Total Unique Counters: 5"));
        assert!(
            has_total_count,
            "Should show correct total unique counters count"
        );
    }

    #[tokio::test]
    async fn test_stats_reporter_sai_stat_names() {
        use std::sync::{Arc, Mutex};

        #[derive(Clone)]
        struct SharedTestWriter {
            lines: Arc<Mutex<Vec<String>>>,
        }

        impl SharedTestWriter {
            fn new() -> Self {
                Self {
                    lines: Arc::new(Mutex::new(Vec::new())),
                }
            }

            fn get_lines(&self) -> Vec<String> {
                self.lines.lock().unwrap().clone()
            }
        }

        impl OutputWriter for SharedTestWriter {
            fn write_line(&mut self, line: &str) {
                self.lines.lock().unwrap().push(line.to_string());
            }
        }

        let (sender, receiver) = channel(10);
        let shared_writer = SharedTestWriter::new();
        let writer_clone = shared_writer.clone();

        let config = StatsReporterConfig {
            interval: Duration::from_millis(100),
            detailed: true,
            max_stats_per_report: None,
        };

        let actor = StatsReporterActor::new(receiver, config, shared_writer);
        let handle = spawn(StatsReporterActor::run(actor));

        // Create stats with known SAI types and stat IDs
        let stats = vec![
            SAIStat::new("Ethernet0", 1, 0, 832),   // Port, IfInOctets
            SAIStat::new("Ethernet16", 1, 1, 1664), // Port, IfInUcastPkts
        ];

        let test_stats = SAIStats {
            observation_time: 12345,
            stats,
        };

        sender
            .send(Arc::new(SAIStatsBatch::from_stats(test_stats)))
            .await
            .unwrap();

        // Wait for processing and one report
        sleep(Duration::from_millis(150)).await;

        // Close and finish
        drop(sender);
        handle.await.expect("Actor should complete successfully");

        // Verify captured output
        let output = writer_clone.get_lines();

        // Find the line that should contain IF_IN_OCTETS (without SAI_PORT_STAT_ prefix)
        let has_if_in_octets = output.iter().any(|line| line.contains("IF_IN_OCTETS"));
        assert!(
            has_if_in_octets,
            "Should show IF_IN_OCTETS without SAI_PORT_STAT_ prefix"
        );

        // Find the line that should contain IF_IN_UCAST_PKTS (without SAI_PORT_STAT_ prefix)
        let has_if_in_ucast_pkts = output.iter().any(|line| line.contains("IF_IN_UCAST_PKTS"));
        assert!(
            has_if_in_ucast_pkts,
            "Should show IF_IN_UCAST_PKTS without SAI_PORT_STAT_ prefix"
        );

        // Should NOT have full SAI prefixes
        let has_sai_prefix = output.iter().any(|line| {
            line.contains("SAI_PORT_STAT_IF_IN_OCTETS")
                || line.contains("SAI_PORT_STAT_IF_IN_UCAST_PKTS")
        });
        assert!(
            !has_sai_prefix,
            "Should NOT show full SAI_PORT_STAT_ prefixes"
        );

        // Should NOT have generic STAT_ prefixes
        let has_generic_stat = output
            .iter()
            .any(|line| line.contains("STAT_0") || line.contains("STAT_1"));
        assert!(!has_generic_stat, "Should NOT show generic STAT_ names");
    }

    /// The previous per-counter implementation, kept as the reference model.
    struct LegacyReporter<W: OutputWriter> {
        config: StatsReporterConfig,
        latest_counters: HashMap<CounterKey, CounterInfo>,
        messages_per_counter: HashMap<CounterKey, u64>,
        total_messages_received: u64,
        reports_generated: u64,
        writer: W,
    }

    impl<W: OutputWriter> LegacyReporter<W> {
        fn new(config: StatsReporterConfig, writer: W) -> Self {
            Self {
                config,
                latest_counters: HashMap::new(),
                messages_per_counter: HashMap::new(),
                total_messages_received: 0,
                reports_generated: 0,
                writer,
            }
        }

        fn update_stats(&mut self, stats_batch: &SAIStatsBatch) {
            for stats in stats_batch.iter() {
                self.total_messages_received += 1;
                for stat in stats.stats {
                    let key =
                        CounterKey::new(Arc::clone(stat.object_name), stat.type_id, stat.stat_id);
                    let counter_info = CounterInfo {
                        counter: stat.counter,
                        last_observation_time: stats.observation_time,
                        type_name: stat.type_name(),
                        stat_name: stat.stat_name(),
                    };
                    self.latest_counters.insert(key.clone(), counter_info);
                    *self.messages_per_counter.entry(key).or_insert(0) += 1;
                }
            }
        }

        fn generate_report(&mut self) {
            self.reports_generated += 1;
            if self.latest_counters.is_empty() {
                self.writer.write_line(&format!(
                    "[Report #{}] No statistics data available yet",
                    self.reports_generated
                ));
                self.writer.write_line(&format!(
                    "   Total Messages Received: {}",
                    self.total_messages_received
                ));
            } else {
                self.print_counters_report();
            }
            self.messages_per_counter.clear();
            self.writer.write_line("");
        }

        fn print_counters_report(&mut self) {
            self.writer.write_line(&format!(
                "[Report #{}] SAI Counters Report",
                self.reports_generated
            ));
            self.writer.write_line(&format!(
                "   Total Unique Counters: {}",
                self.latest_counters.len()
            ));
            self.writer.write_line(&format!(
                "   Total Messages Received: {}",
                self.total_messages_received
            ));

            if self.config.detailed && !self.latest_counters.is_empty() {
                use std::collections::BTreeMap;
                let mut grouped_counters: BTreeMap<u32, Vec<(&CounterKey, &CounterInfo)>> =
                    BTreeMap::new();
                for (key, counter_info) in &self.latest_counters {
                    grouped_counters
                        .entry(key.type_id)
                        .or_insert_with(Vec::new)
                        .push((key, counter_info));
                }
                self.writer.write_line("   Detailed Counters:");
                let mut total_shown = 0;
                for (type_id, mut counters) in grouped_counters {
                    counters.sort_by(|a, b| {
                        a.0.object_name
                            .cmp(&b.0.object_name)
                            .then_with(|| a.0.stat_id.cmp(&b.0.stat_id))
                    });
                    let type_name = counters
                        .first()
                        .and_then(|(_, info)| info.type_name)
                        .map(std::borrow::Cow::Borrowed)
                        .unwrap_or_else(|| std::borrow::Cow::Owned(type_id_to_string(type_id)));
                    self.writer
                        .write_line(&format!("      Type: {} ({})", type_name, type_id));
                    let counters_to_show = if let Some(max) = self.config.max_stats_per_report {
                        let remaining = max.saturating_sub(total_shown);
                        &counters[..std::cmp::min(remaining, counters.len())]
                    } else {
                        &counters
                    };
                    for (index, (key, counter_info)) in counters_to_show.iter().enumerate() {
                        let messages_in_period = self.messages_per_counter.get(key).unwrap_or(&0);
                        let messages_per_second =
                            *messages_in_period as f64 / self.config.interval.as_secs_f64();
                        let stat_name = counter_info
                            .stat_name
                            .map(remove_sai_prefix)
                            .unwrap_or_else(|| stat_id_to_string(key.type_id, key.stat_id));
                        let formatted_time = format_timestamp(counter_info.last_observation_time);
                        self.writer.write_line(&format!(
                            "         [{:3}] Object: {:15}, Stat: {:25}, Counter: {:15}, Msg/s: {:6.1}, LastTime: {}",
                            index + 1,
                            key.object_name,
                            stat_name,
                            counter_info.counter,
                            messages_per_second,
                            formatted_time
                        ));
                    }
                    total_shown += counters_to_show.len();
                    if let Some(max) = self.config.max_stats_per_report {
                        if total_shown >= max && self.latest_counters.len() > max {
                            self.writer.write_line(&format!(
                                "         ... and {} more counters (use max_stats_per_report: None to show all)",
                                self.latest_counters.len() - max
                            ));
                            break;
                        }
                    }
                }
            } else if !self.config.detailed && !self.latest_counters.is_empty() {
                let total_counter_value: u128 = self
                    .latest_counters
                    .values()
                    .map(|info| info.counter as u128)
                    .sum();
                let unique_types = self
                    .latest_counters
                    .keys()
                    .map(|k| k.type_id)
                    .collect::<std::collections::HashSet<_>>()
                    .len();
                let unique_objects = self
                    .latest_counters
                    .keys()
                    .map(|k| &k.object_name)
                    .collect::<std::collections::HashSet<_>>()
                    .len();
                let total_messages_in_period: u64 = self.messages_per_counter.values().sum();
                let messages_per_second =
                    total_messages_in_period as f64 / self.config.interval.as_secs_f64();
                self.writer.write_line("   Summary:");
                self.writer.write_line(&format!(
                    "      Total Counter Value: {}",
                    total_counter_value
                ));
                self.writer
                    .write_line(&format!("      Unique Types: {}", unique_types));
                self.writer
                    .write_line(&format!("      Unique Objects: {}", unique_objects));
                self.writer.write_line(&format!(
                    "      Messages per Second: {:.1}",
                    messages_per_second
                ));
            }
        }
    }

    struct Rng(u64);

    impl Rng {
        fn next(&mut self, bound: u64) -> u64 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 7;
            self.0 ^= self.0 << 17;
            self.0 % bound
        }
    }

    // Small key universe so templates and owned records overlap; includes
    // unknown object types and stat IDs.
    fn random_key(rng: &mut Rng) -> (String, u32, u32) {
        let name = format!("Ethernet{}", rng.next(6) * 4);
        let type_id = [1u32, 21, 24, 26, 999][rng.next(5) as usize];
        let stat_id = [0u32, 1, 2, 7, 9999][rng.next(5) as usize];
        (name, type_id, stat_id)
    }

    fn random_template(
        rng: &mut Rng,
    ) -> Arc<[crate::message::saistats::SAIStatMetadata]> {
        use crate::message::saistats::SAIStatMetadata;
        let fields = 1 + rng.next(6) as usize;
        let mut metadata: Vec<SAIStatMetadata> = (0..fields)
            .map(|_| {
                let (name, type_id, stat_id) = random_key(rng);
                SAIStatMetadata::new(name, type_id, stat_id)
            })
            .collect();
        if rng.next(4) == 0 {
            // The same counter twice in one template.
            let copy = metadata[rng.next(fields as u64) as usize].clone();
            metadata.push(copy);
        }
        metadata.into()
    }

    fn assert_same_state(
        actor: &mut StatsReporterActor<TestWriter>,
        legacy: &LegacyReporter<TestWriter>,
    ) {
        let latest = actor.latest_counters();
        assert_eq!(latest.len(), legacy.latest_counters.len());
        for (key, info) in &legacy.latest_counters {
            let mine = &latest[key];
            assert_eq!(
                (mine.counter, mine.last_observation_time, mine.type_name, mine.stat_name),
                (info.counter, info.last_observation_time, info.type_name, info.stat_name),
                "{key:?}"
            );
        }
        let messages = actor.messages_per_counter();
        assert_eq!(messages.len(), legacy.messages_per_counter.len());
        for (key, count) in &legacy.messages_per_counter {
            assert_eq!(messages.get(key), Some(count), "{key:?}");
        }
        assert_eq!(actor.total_messages_received, legacy.total_messages_received);
    }

    #[tokio::test]
    async fn template_slots_report_exactly_like_per_counter_updates() {
        let configs = [
            StatsReporterConfig {
                interval: Duration::from_secs(10),
                detailed: true,
                max_stats_per_report: None,
            },
            StatsReporterConfig {
                interval: Duration::from_secs(10),
                detailed: true,
                max_stats_per_report: Some(7),
            },
            StatsReporterConfig {
                interval: Duration::from_secs(4),
                detailed: false,
                max_stats_per_report: None,
            },
        ];
        for (round, config) in configs.into_iter().enumerate() {
            let mut rng = Rng(0x9e37_79b9_7f4a_7c15 ^ (round as u64 + 1));
            let (_sender, receiver) = channel(1);
            let mut actor = StatsReporterActor::new(receiver, config.clone(), TestWriter::new());
            let mut legacy = LegacyReporter::new(config, TestWriter::new());
            let mut templates: Vec<_> = (0..6).map(|_| random_template(&mut rng)).collect();
            let mut time = 1_700_000_000_000_000_000u64;
            let mut reports = 0;
            for step in 0..800 {
                if rng.next(20) == 0 {
                    let index = rng.next(templates.len() as u64) as usize;
                    templates[index] = if rng.next(2) == 0 {
                        // Same content in a new allocation, like a re-registered template.
                        Arc::from(templates[index].to_vec())
                    } else {
                        random_template(&mut rng)
                    };
                }
                let mut batch = SAIStatsBatch::default();
                for _ in 0..1 + rng.next(12) {
                    time += 1 + rng.next(1_000_000_000);
                    if rng.next(5) == 0 {
                        let count = 1 + rng.next(4);
                        let stats: Vec<SAIStat> = (0..count)
                            .map(|_| {
                                let (name, type_id, stat_id) = random_key(&mut rng);
                                SAIStat::new(name, type_id, stat_id, rng.next(u64::MAX))
                            })
                            .collect();
                        batch.push_record(time, stats);
                    } else {
                        let template =
                            Arc::clone(&templates[rng.next(templates.len() as u64) as usize]);
                        let values: Vec<u64> =
                            (0..template.len()).map(|_| rng.next(u64::MAX)).collect();
                        batch.push_shared_record(time, template, values);
                    }
                }
                legacy.update_stats(&batch);
                actor.update_stats(Arc::new(batch));
                if rng.next(8) == 0 {
                    legacy.generate_report();
                    actor.generate_report();
                    reports += 1;
                    // Replaced templates are referenced by nothing but their slot.
                    assert!(actor.slots.len() <= templates.len());
                }
                if step % 40 == 0 {
                    assert_same_state(&mut actor, &legacy);
                }
            }
            legacy.generate_report();
            actor.generate_report();
            assert!(reports > 50);
            assert!(actor.writer.lines.len() > 6 * reports);
            assert_eq!(actor.writer.lines, legacy.writer.lines);
        }
    }

    #[tokio::test]
    async fn retired_template_slot_is_dropped_but_its_counters_are_kept() {
        use crate::message::saistats::SAIStatMetadata;
        let (_sender, receiver) = channel(1);
        let mut actor =
            StatsReporterActor::new(receiver, StatsReporterConfig::default(), TestWriter::new());
        let metadata: Arc<[SAIStatMetadata]> = Arc::from(vec![
            SAIStatMetadata::new("Ethernet0", 1, 0),
            SAIStatMetadata::new("Ethernet0", 1, 1),
        ]);
        let mut batch = SAIStatsBatch::default();
        batch.push_shared_record(10, Arc::clone(&metadata), [1, 2]);
        actor.update_stats(Arc::new(batch));
        actor.generate_report();
        assert_eq!(actor.slots.len(), 1, "template still installed");
        drop(metadata);
        actor.generate_report();
        assert!(actor.slots.is_empty());
        assert!(actor.table.iter().all(|entry| entry.address == 0));
        let latest = actor.latest_counters();
        assert_eq!(latest.len(), 2);
        assert_eq!(latest[&CounterKey::new("Ethernet0".into(), 1, 1)].counter, 2);
    }

    #[tokio::test]
    async fn hot_table_growth_and_retirement_keep_every_template() {
        use crate::message::saistats::SAIStatMetadata;
        let config = StatsReporterConfig {
            interval: Duration::from_secs(10),
            detailed: true,
            max_stats_per_report: None,
        };
        let (_sender, receiver) = channel(1);
        let mut actor = StatsReporterActor::new(receiver, config.clone(), TestWriter::new());
        let mut legacy = LegacyReporter::new(config, TestWriter::new());
        let mut rng = Rng(0x2545_f491_4f6c_dd1d);
        // 600 templates of 1..5 fields with distinct counters; some fields are
        // shared between neighbouring templates.
        let mut templates: Vec<Option<Arc<[SAIStatMetadata]>>> = (0..600)
            .map(|template| {
                let fields = 1 + template % 5;
                let metadata: Vec<SAIStatMetadata> = (0..fields)
                    .map(|field| {
                        SAIStatMetadata::new(
                            format!("Ethernet{}", (template + field) / 2),
                            21,
                            field as u32,
                        )
                    })
                    .collect();
                Some(Arc::from(metadata))
            })
            .collect();
        let mut time = 1_700_000_000_000_000_000u64;
        for round in 0..6 {
            if round == 3 {
                // Retire every other template.
                for template in templates.iter_mut().step_by(2) {
                    *template = None;
                }
            }
            for _ in 0..40 {
                let mut batch = SAIStatsBatch::default();
                for _ in 0..200 {
                    let live: Vec<_> = templates.iter().flatten().collect();
                    let template = Arc::clone(live[rng.next(live.len() as u64) as usize]);
                    time += 1 + rng.next(1_000_000);
                    let values: Vec<u64> = (0..template.len()).map(|_| rng.next(u64::MAX)).collect();
                    batch.push_shared_record(time, template, values);
                }
                legacy.update_stats(&batch);
                actor.update_stats(Arc::new(batch));
            }
            legacy.generate_report();
            actor.generate_report();
            assert_same_state(&mut actor, &legacy);
            assert_eq!(actor.slots.len(), templates.iter().flatten().count());
            assert!(2 * actor.slots.len() <= actor.table.len());
            assert_eq!(
                actor.table.iter().filter(|entry| entry.address != 0).count(),
                actor.slots.len()
            );
        }
        assert!(actor.table.len() >= 1024, "the table grew past its initial capacity");
        assert_eq!(actor.writer.lines, legacy.writer.lines);
    }
}
