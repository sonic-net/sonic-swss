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
    use tokio::sync::mpsc::{channel, unbounded_channel, UnboundedReceiver, UnboundedSender};
    use tokio::time::timeout;

    use crate::message::saistats::{SAIStat, SAIStatsBatch};

    fn reporter(config: StatsReporterConfig) -> StatsReporterActor<TestWriter> {
        let (_, receiver) = channel(1);
        StatsReporterActor::new(receiver, config, TestWriter::new())
    }

    #[tokio::test]
    async fn owned_and_shared_batches_preserve_record_order() {
        for shared in [false, true] {
            let mut actor = reporter(StatsReporterConfig::default());
            let metadata: Arc<[SAIStatMetadata]> =
                vec![SAIStatMetadata::new("Ethernet0", 1, 2)].into();
            let mut batch = SAIStatsBatch::default();
            for (time, counter) in [(10, 100), (20, 200)] {
                if shared {
                    batch.push_shared_record(time, Arc::clone(&metadata), [counter]);
                } else {
                    batch.push_record(time, [SAIStat::new("Ethernet0", 1, 2, counter)]);
                }
            }
            actor.update_stats(Arc::new(batch));

            let key = CounterKey::new("Ethernet0".into(), 1, 2);
            assert_eq!(actor.total_messages_received, 2);
            assert_eq!(actor.messages_per_counter().get(&key), Some(&2));
            let latest = actor.latest_counters();
            assert_eq!(
                (latest[&key].counter, latest[&key].last_observation_time),
                (200, 20)
            );
        }
    }

    #[tokio::test]
    async fn latest_record_wins_when_observation_time_decreases() {
        let mut actor = reporter(StatsReporterConfig::default());
        let metadata: Arc<[SAIStatMetadata]> = vec![SAIStatMetadata::new("Ethernet0", 1, 0)].into();
        let mut batch = SAIStatsBatch::default();
        batch.push_shared_record(300, Arc::clone(&metadata), [10]);
        batch.push_record(200, [SAIStat::new("Ethernet0", 1, 0, 20)]);
        batch.push_shared_record(100, metadata, [30]);
        actor.update_stats(Arc::new(batch));

        let key = CounterKey::new("Ethernet0".into(), 1, 0);
        let latest = actor.latest_counters();
        assert_eq!(
            (latest[&key].counter, latest[&key].last_observation_time),
            (30, 100)
        );
        assert_eq!(actor.messages_per_counter().get(&key), Some(&3));
    }

    struct ReportWriter(UnboundedSender<String>);

    impl OutputWriter for ReportWriter {
        fn write_line(&mut self, line: &str) {
            self.0.send(line.to_owned()).unwrap();
        }
    }

    async fn read_report(lines: &mut UnboundedReceiver<String>) -> Vec<String> {
        timeout(Duration::from_secs(20), async {
            let mut report = Vec::new();
            loop {
                let line = lines
                    .recv()
                    .await
                    .expect("writer closed before report ended");
                if line.is_empty() {
                    return report;
                }
                report.push(line);
            }
        })
        .await
        .expect("report timed out")
    }

    #[tokio::test(start_paused = true)]
    async fn run_reports_periodically_and_on_shutdown() {
        let (sender, receiver) = channel(1);
        let (output, mut lines) = unbounded_channel();
        let config = StatsReporterConfig {
            detailed: false,
            ..StatsReporterConfig::default()
        };
        let mut actor = StatsReporterActor::new(receiver, config, ReportWriter(output));
        // Consume the immediate tick so the first report follows the first batch.
        actor.report_timer.tick().await;
        let handle = tokio::spawn(StatsReporterActor::run(actor));
        let batch = |value| {
            let mut batch = SAIStatsBatch::default();
            batch.push_record(value, [SAIStat::new("Ethernet0", 1, 0, value)]);
            Arc::new(batch)
        };
        sender.send(batch(10)).await.unwrap();
        let periodic = read_report(&mut lines).await;
        assert_eq!(periodic[0], "[Report #1] SAI Counters Report");
        assert!(periodic
            .iter()
            .any(|line| line == "   Total Messages Received: 1"));
        assert!(periodic
            .iter()
            .any(|line| line == "      Total Counter Value: 10"));

        sender.send(batch(20)).await.unwrap();
        drop(sender);
        timeout(Duration::from_secs(20), handle)
            .await
            .unwrap()
            .unwrap();
        let final_report = read_report(&mut lines).await;
        assert_eq!(final_report[0], "[Report #2] SAI Counters Report");
        assert!(final_report
            .iter()
            .any(|line| line == "   Total Messages Received: 2"));
        assert!(final_report
            .iter()
            .any(|line| line == "      Total Counter Value: 20"));
        assert!(lines.recv().await.is_none());
    }

    #[tokio::test]
    async fn detailed_reports_preserve_order_names_rates_and_limits() {
        let metadata: Arc<[SAIStatMetadata]> = vec![
            SAIStatMetadata::new("Ethernet16", 1, 0),
            SAIStatMetadata::new("Ethernet0", 1, 1),
            SAIStatMetadata::new("Ethernet0", 1, 0),
            SAIStatMetadata::new("Queue0", 21, 0),
            SAIStatMetadata::new("mystery", 999, 9999),
            SAIStatMetadata::new("Ethernet0", 1, 9999),
        ]
        .into();
        let mut batch = SAIStatsBatch::default();
        batch.push_shared_record(1_000_000_123, Arc::clone(&metadata), [1, 2, 3, 4, 5, 6]);
        batch.push_shared_record(2_000_000_456, metadata, [10, 20, 30, 40, 50, 60]);
        let batch = Arc::new(batch);
        let expected = [
            "[Report #1] SAI Counters Report",
            "   Total Unique Counters: 6",
            "   Total Messages Received: 2",
            "   Detailed Counters:",
            "      Type: SAI_OBJECT_TYPE_PORT (1)",
            "         [  1] Object: Ethernet0      , Stat: IF_IN_OCTETS             , Counter:              30, Msg/s:    0.2, LastTime: 1970-01-01 00:00:02.000000456 UTC",
            "         [  2] Object: Ethernet0      , Stat: IF_IN_UCAST_PKTS         , Counter:              20, Msg/s:    0.2, LastTime: 1970-01-01 00:00:02.000000456 UTC",
            "         [  3] Object: Ethernet0      , Stat: UNKNOWN_PORT_STAT_9999   , Counter:              60, Msg/s:    0.2, LastTime: 1970-01-01 00:00:02.000000456 UTC",
            "         [  4] Object: Ethernet16     , Stat: IF_IN_OCTETS             , Counter:              10, Msg/s:    0.2, LastTime: 1970-01-01 00:00:02.000000456 UTC",
            "      Type: SAI_OBJECT_TYPE_QUEUE (21)",
            "         [  1] Object: Queue0         , Stat: PACKETS                  , Counter:              40, Msg/s:    0.2, LastTime: 1970-01-01 00:00:02.000000456 UTC",
            "      Type: UNKNOWN(999) (999)",
            "         [  1] Object: mystery        , Stat: INVALID_TYPE_999_STAT_9999, Counter:              50, Msg/s:    0.2, LastTime: 1970-01-01 00:00:02.000000456 UTC",
            "",
        ];
        // Truncate within a type and after crossing a type boundary.
        for (limit, expected) in [
            (None, expected.to_vec()),
            (Some(2), [&expected[..7], &[
                "         ... and 4 more counters (use max_stats_per_report: None to show all)", "",
            ]].concat()),
            (Some(5), [&expected[..11], &[
                "         ... and 1 more counters (use max_stats_per_report: None to show all)", "",
            ]].concat()),
        ] {
            let mut actor = reporter(StatsReporterConfig {
                max_stats_per_report: limit,
                ..StatsReporterConfig::default()
            });
            actor.update_stats(Arc::clone(&batch));
            actor.generate_report();
            assert_eq!(actor.writer.lines, expected, "limit={limit:?}");
        }
    }

    #[tokio::test]
    async fn summary_report_uses_wide_totals_and_counts_all_fields() {
        let mut actor = reporter(StatsReporterConfig {
            interval: Duration::from_secs(4),
            detailed: false,
            max_stats_per_report: None,
        });
        let metadata = vec![
            SAIStatMetadata::new("shared", 1, 0),
            SAIStatMetadata::new("shared", 21, 0),
        ]
        .into();
        let mut batch = SAIStatsBatch::default();
        batch.push_shared_record(1, metadata, [u64::MAX, u64::MAX]);
        batch.push_record(2, [SAIStat::new("other", 1, 1, 1)]);
        actor.update_stats(Arc::new(batch));
        actor.generate_report();
        assert_eq!(
            actor.writer.lines,
            [
                "[Report #1] SAI Counters Report",
                "   Total Unique Counters: 3",
                "   Total Messages Received: 2",
                "   Summary:",
                "      Total Counter Value: 36893488147419103231",
                "      Unique Types: 2",
                "      Unique Objects: 2",
                "      Messages per Second: 0.8",
                "",
            ]
        );
    }

    #[tokio::test]
    async fn report_without_data() {
        let mut actor = reporter(StatsReporterConfig::default());
        actor.generate_report();
        assert_eq!(
            actor.writer.lines,
            [
                "[Report #1] No statistics data available yet",
                "   Total Messages Received: 0",
                "",
            ]
        );
    }

    /// Independent per-counter reference model for the previous behavior.
    #[derive(Default)]
    struct PerCounterModel {
        latest_counters: HashMap<CounterKey, CounterInfo>,
        messages_per_counter: HashMap<CounterKey, u64>,
        total_messages_received: u64,
    }

    impl PerCounterModel {
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

    fn random_template(rng: &mut Rng) -> Arc<[SAIStatMetadata]> {
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

    fn assert_same_state(actor: &mut StatsReporterActor<TestWriter>, model: &PerCounterModel) {
        let latest = actor.latest_counters();
        assert_eq!(latest.len(), model.latest_counters.len());
        for (key, info) in &model.latest_counters {
            let mine = &latest[key];
            assert_eq!(
                (
                    mine.counter,
                    mine.last_observation_time,
                    mine.type_name,
                    mine.stat_name
                ),
                (
                    info.counter,
                    info.last_observation_time,
                    info.type_name,
                    info.stat_name
                ),
                "{key:?}"
            );
        }
        assert_eq!(actor.messages_per_counter(), model.messages_per_counter);
        assert_eq!(actor.total_messages_received, model.total_messages_received);
    }

    fn check_report(actor: &mut StatsReporterActor<TestWriter>, model: &mut PerCounterModel) {
        // Compare period counts before the report clears them, then verify the reset.
        assert_same_state(actor, model);
        actor.generate_report();
        model.messages_per_counter.clear();
        assert_same_state(actor, model);
        actor.writer.lines.clear();
    }

    #[tokio::test]
    async fn template_slots_match_per_counter_updates() {
        let mut rng = Rng(0x9e37_79b9_7f4a_7c15 ^ 1);
        let mut actor = reporter(StatsReporterConfig::default());
        let mut model = PerCounterModel::default();
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
            model.update_stats(&batch);
            actor.update_stats(Arc::new(batch));
            if rng.next(8) == 0 {
                check_report(&mut actor, &mut model);
                reports += 1;
                // Replaced templates are referenced by nothing but their slot.
                assert!(actor.slots.len() <= templates.len());
            }
            if step % 40 == 0 {
                assert_same_state(&mut actor, &model);
            }
        }
        check_report(&mut actor, &mut model);
        assert!(reports > 50);
    }

    #[tokio::test]
    async fn retired_template_slot_is_dropped_but_its_counters_are_kept() {
        let mut actor = reporter(StatsReporterConfig::default());
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
        assert_eq!(
            latest[&CounterKey::new("Ethernet0".into(), 1, 1)].counter,
            2
        );
    }

    #[tokio::test]
    async fn hot_table_growth_and_retirement_keep_every_template() {
        const TEMPLATE_COUNT: usize = 64;
        let mut actor = reporter(StatsReporterConfig::default());
        let mut model = PerCounterModel::default();
        // Exercise inline and boxed values, with overlap between neighbouring templates.
        let mut templates: Vec<Option<Arc<[SAIStatMetadata]>>> = (0..TEMPLATE_COUNT)
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
        for round in 0..3 {
            if round == 1 {
                // Retire every other template.
                for template in templates.iter_mut().step_by(2) {
                    *template = None;
                }
            }
            let mut batch = SAIStatsBatch::default();
            // Earlier templates keep pending counts and values across later expansions.
            for template in templates.iter().flatten().rev() {
                for _ in 0..2 {
                    time += 1;
                    let values = (0..template.len()).map(|field| time + field as u64);
                    batch.push_shared_record(time, Arc::clone(template), values);
                }
            }
            model.update_stats(&batch);
            actor.update_stats(Arc::new(batch));
            if round == 0 {
                assert!(
                    actor.table.len() >= 2 * TEMPLATE_COUNT,
                    "multiple table expansions"
                );
            }
            check_report(&mut actor, &mut model);
            assert_eq!(actor.slots.len(), templates.iter().flatten().count());
            assert!(2 * actor.slots.len() <= actor.table.len());
            assert_eq!(
                actor
                    .table
                    .iter()
                    .filter(|entry| entry.address != 0)
                    .count(),
                actor.slots.len()
            );
        }
    }
}
