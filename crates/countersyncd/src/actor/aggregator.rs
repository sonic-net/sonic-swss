use ahash::{HashMap, HashMapExt};
use hdrhistogram::Histogram;
use log::debug;
use std::collections::{HashSet, LinkedList};
use std::sync::Arc;
use tokio::{
    select,
    sync::mpsc::{Receiver, Sender},
};

use crate::message::{
    aggregator::{
        AggregatedStatsMessage, AggregatorConfig, AggregatorConfigMessage, AggregatorStatsMessage,
        CounterSelector, Heatmap,
    },
    saistats::{SAIStat, SAIStats, SAIStatsMessage},
};

const NANOS_PER_MICROSECOND: u64 = 1_000;

#[derive(Debug)]
struct ReportingWindow {
    window: u64,
    observation_time: u64,
    stats: Vec<SAIStat>,
    index: HashMap<String, HashMap<(u32, u32), usize>>,
    heatmap_counters: Arc<HashSet<CounterSelector>>,
    heatmaps: Vec<Option<HeatmapAccumulator>>,
}

impl ReportingWindow {
    fn new(
        window: u64,
        heatmap_counters: Arc<HashSet<CounterSelector>>,
        sample: &SAIStats,
    ) -> Self {
        let mut state = Self {
            window,
            observation_time: sample.observation_time,
            stats: Vec::with_capacity(sample.stats.len()),
            index: HashMap::with_capacity(sample.stats.len()),
            heatmap_counters,
            heatmaps: Vec::with_capacity(sample.stats.len()),
        };
        state.merge(sample);
        state
    }

    fn merge(&mut self, sample: &SAIStats) {
        self.observation_time = self.observation_time.max(sample.observation_time);

        for stat in &sample.stats {
            let stat_index = self.index.get_mut(stat.object_name.as_str());
            if let Some(position) = stat_index
                .and_then(|index| index.get(&(stat.type_id, stat.stat_id)))
                .copied()
            {
                self.stats[position] = stat.clone();
                if let Some(heatmap) = self.heatmaps[position].as_mut() {
                    heatmap.record(stat.counter);
                }
            } else {
                let position = self.stats.len();
                self.stats.push(stat.clone());
                self.heatmaps.push(
                    self.heatmap_counters
                        .contains(&CounterSelector::new(stat.type_id, stat.stat_id))
                        .then(|| HeatmapAccumulator::new(stat.counter)),
                );
                self.index
                    .entry(stat.object_name.clone())
                    .or_default()
                    .insert((stat.type_id, stat.stat_id), position);
            }
        }
    }

    fn into_message(self, interval_ns: u64) -> AggregatedStatsMessage {
        let start_time_unix_nano = self.window.saturating_mul(interval_ns);
        let time_unix_nano = start_time_unix_nano.saturating_add(interval_ns);
        let heatmaps = self
            .stats
            .iter()
            .zip(self.heatmaps)
            .filter_map(|(stat, heatmap)| {
                heatmap
                    .map(|heatmap| heatmap.into_message(stat, start_time_unix_nano, time_unix_nano))
            })
            .collect();
        let stats = Arc::new(SAIStats {
            observation_time: self.observation_time,
            stats: self.stats,
        });
        AggregatedStatsMessage::with_heatmaps(None, stats, heatmaps)
    }
}

#[derive(Debug)]
struct HeatmapAccumulator {
    histogram: Histogram<u64>,
    sum: u128,
    min: u64,
    max: u64,
}

impl HeatmapAccumulator {
    fn new(value: u64) -> Self {
        let mut accumulator = Self {
            histogram: Histogram::new_with_bounds(1, u64::MAX, 0)
                .expect("valid HDR histogram bounds"),
            sum: 0,
            min: value,
            max: value,
        };
        accumulator.record(value);
        accumulator
    }

    fn record(&mut self, value: u64) {
        self.histogram
            .record(value)
            .expect("u64 value must fit configured HDR histogram");
        self.sum += u128::from(value);
        self.min = self.min.min(value);
        self.max = self.max.max(value);
    }

    fn into_message(
        self,
        stat: &SAIStat,
        start_time_unix_nano: u64,
        time_unix_nano: u64,
    ) -> Heatmap {
        let mut explicit_bounds = Vec::new();
        let mut bucket_counts = Vec::new();
        for bucket in self.histogram.iter_recorded() {
            explicit_bounds.push(bucket.value_iterated_to() as f64);
            bucket_counts.push(bucket.count_at_value());
        }
        bucket_counts.push(0);

        Heatmap {
            object_name: stat.object_name.clone(),
            type_id: stat.type_id,
            stat_id: stat.stat_id,
            start_time_unix_nano,
            time_unix_nano,
            count: self.histogram.len(),
            sum: self.sum as f64,
            min: self.min,
            max: self.max,
            explicit_bounds,
            bucket_counts,
        }
    }
}

#[derive(Debug)]
struct ReportingRateState {
    interval_ns: u64,
    heatmap_counters: Arc<HashSet<CounterSelector>>,
    current: Option<ReportingWindow>,
}

impl ReportingRateState {
    fn new(reporting_rate_us: u32, heatmap_counters: Arc<HashSet<CounterSelector>>) -> Self {
        Self {
            interval_ns: u64::from(reporting_rate_us) * NANOS_PER_MICROSECOND,
            heatmap_counters,
            current: None,
        }
    }

    fn push(&mut self, sample: &SAIStats) -> Option<AggregatedStatsMessage> {
        if self.interval_ns == 0 {
            return None;
        }

        let window = sample.observation_time / self.interval_ns;
        // Reporting-rate aggregation is sample-driven: a later sample closes
        // the previous window. For continuous stream telemetry this avoids a
        // per-session timer, accepting that the final partial window may remain
        // buffered when a stream becomes idle or ends.
        match self.current.as_mut() {
            None => {
                self.current = Some(ReportingWindow::new(
                    window,
                    self.heatmap_counters.clone(),
                    sample,
                ));
                None
            }
            Some(current) if current.window == window => {
                current.merge(sample);
                None
            }
            Some(_) => {
                let flushed = self
                    .current
                    .take()
                    .map(|window| window.into_message(self.interval_ns));
                self.current = Some(ReportingWindow::new(
                    window,
                    self.heatmap_counters.clone(),
                    sample,
                ));
                flushed
            }
        }
    }
}

#[derive(Debug, Default)]
pub struct Aggregator {
    sessions: HashMap<String, AggregatorState>,
}

#[derive(Debug)]
struct AggregatorState {
    _config: AggregatorConfig,
    reporting_rate: Option<ReportingRateState>,
}

impl AggregatorState {
    fn new(config: AggregatorConfig) -> Self {
        let heatmap_counters = Arc::new(config.heatmap_counters.clone());
        let reporting_rate = config
            .reporting_rate
            .map(|rate| ReportingRateState::new(rate, heatmap_counters));

        Self {
            _config: config,
            reporting_rate,
        }
    }

    fn process(&mut self, sample: SAIStatsMessage) -> Vec<AggregatedStatsMessage> {
        let Some(reporting_rate) = self.reporting_rate.as_mut() else {
            return vec![sample.into()];
        };

        match reporting_rate.push(sample.as_ref()) {
            Some(message) => vec![message],
            None => Vec::new(),
        }
    }
}

impl Aggregator {
    pub fn set_config(&mut self, key: String, config: Option<AggregatorConfig>) {
        match config {
            Some(config) => {
                self.sessions.insert(key, AggregatorState::new(config));
            }
            None => {
                self.sessions.remove(&key);
            }
        }
    }

    pub fn remove_config(&mut self, key: &str) {
        self.sessions.remove(key);
    }

    pub fn process(
        &mut self,
        key: Option<&str>,
        sample: SAIStatsMessage,
    ) -> Vec<AggregatedStatsMessage> {
        let Some(key) = key else {
            return vec![sample.into()];
        };

        let Some(state) = self.sessions.get_mut(key) else {
            return vec![AggregatedStatsMessage::new(Some(Arc::from(key)), sample)];
        };

        state
            .process(sample)
            .into_iter()
            .map(|mut message| {
                message.key = Some(Arc::from(key));
                message
            })
            .collect()
    }
}

fn validate_reporting_rate(config: &Option<AggregatorConfig>, key: &str) {
    if let Some(config) = config {
        if config.reporting_rate.is_none() {
            debug!(
                "Aggregator config for session {} has no valid reporting_rate; forwarding samples unchanged",
                key
            );
        }
    }
}

pub struct AggregatorActor {
    config_recipient: Receiver<AggregatorConfigMessage>,
    stats_recipient: Receiver<AggregatorStatsMessage>,
    recipients: LinkedList<Sender<AggregatedStatsMessage>>,
    aggregator: Aggregator,
}

impl AggregatorActor {
    pub fn new(
        config_recipient: Receiver<AggregatorConfigMessage>,
        stats_recipient: Receiver<AggregatorStatsMessage>,
    ) -> Self {
        Self {
            config_recipient,
            stats_recipient,
            recipients: LinkedList::new(),
            aggregator: Aggregator::default(),
        }
    }

    pub fn add_recipient(&mut self, recipient: Sender<AggregatedStatsMessage>) {
        self.recipients.push_back(recipient);
    }

    fn handle_config(&mut self, message: AggregatorConfigMessage) {
        if message.is_delete {
            self.aggregator.remove_config(&message.key);
            return;
        }

        validate_reporting_rate(&message.config, &message.key);
        self.aggregator.set_config(message.key, message.config);
    }

    fn handle_stats(&mut self, message: AggregatorStatsMessage) -> Vec<AggregatedStatsMessage> {
        if !message.heatmaps.is_empty() {
            return vec![message];
        }

        self.aggregator
            .process(message.key.as_deref(), message.stats)
    }

    pub async fn run(mut actor: AggregatorActor) {
        loop {
            select! {
                config = actor.config_recipient.recv() => {
                    match config {
                        Some(config) => actor.handle_config(config),
                        None => break,
                    }
                },
                stats = actor.stats_recipient.recv() => {
                    match stats {
                        Some(stats) => {
                            let messages = actor.handle_stats(stats);
                            for recipient in &actor.recipients {
                                for message in &messages {
                                    let _ = recipient.send(message.clone()).await;
                                }
                            }
                        },
                        None => break,
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn stat(object_name: &str, counter: u64) -> SAIStat {
        SAIStat {
            object_name: object_name.to_string(),
            type_id: 1,
            stat_id: 2,
            counter,
        }
    }

    fn sample(observation_time: u64, stats: Vec<SAIStat>) -> SAIStatsMessage {
        Arc::new(SAIStats {
            observation_time,
            stats,
        })
    }

    #[test]
    fn forwards_samples_without_reporting_rate() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: None,
                ..Default::default()
            }),
        );

        let input = sample(10, vec![stat("Ethernet0", 1)]);
        let output = aggregator.process(Some("session"), input.clone());

        assert_eq!(output.len(), 1);
        assert_eq!(output[0].stats, input);
        assert_eq!(output[0].key.as_deref(), Some("session"));
    }

    #[test]
    fn aggregates_samples_until_next_reporting_window() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(10),
                ..Default::default()
            }),
        );

        assert!(aggregator
            .process(Some("session"), sample(1_000, vec![stat("Ethernet0", 1)]))
            .is_empty());
        assert!(aggregator
            .process(Some("session"), sample(9_000, vec![stat("Ethernet0", 9)]))
            .is_empty());

        let output =
            aggregator.process(Some("session"), sample(10_000, vec![stat("Ethernet0", 10)]));

        assert_eq!(output.len(), 1);
        assert_eq!(output[0].stats.observation_time, 9_000);
        assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", 9)]);
    }

    #[test]
    fn keeps_latest_stat_per_object_type_and_counter() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(10),
                ..Default::default()
            }),
        );

        aggregator.process(Some("session"), sample(1_000, vec![stat("Ethernet0", 1)]));
        aggregator.process(
            Some("session"),
            sample(2_000, vec![stat("Ethernet0", 2), stat("Ethernet4", 3)]),
        );

        let output =
            aggregator.process(Some("session"), sample(11_000, vec![stat("Ethernet0", 11)]));

        assert_eq!(output.len(), 1);
        assert_eq!(
            output[0].stats.stats,
            vec![stat("Ethernet0", 2), stat("Ethernet4", 3)]
        );
    }

    #[test]
    fn resets_state_when_config_changes() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(10),
                ..Default::default()
            }),
        );
        assert!(aggregator
            .process(Some("session"), sample(1_000, vec![stat("Ethernet0", 1)]))
            .is_empty());

        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(20),
                ..Default::default()
            }),
        );

        assert!(aggregator
            .process(Some("session"), sample(11_000, vec![stat("Ethernet0", 11)]))
            .is_empty());
    }

    #[test]
    fn collects_heatmap_samples_with_hdrhistogram() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(10),
                heatmap_counters: HashSet::from([CounterSelector::new(1, 2)]),
            }),
        );

        for (time, value) in [(1_000, 1), (2_000, 2), (9_000, 8)] {
            assert!(aggregator
                .process(
                    Some("session"),
                    sample(time, vec![stat("Ethernet0", value)])
                )
                .is_empty());
        }
        let output =
            aggregator.process(Some("session"), sample(10_000, vec![stat("Ethernet0", 10)]));

        assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", 8)]);
        assert_eq!(output[0].heatmaps.len(), 1);
        assert_eq!(output[0].heatmaps[0].count, 3);
        assert_eq!(output[0].heatmaps[0].sum, 11.0);
        assert_eq!(output[0].heatmaps[0].min, 1);
        assert_eq!(output[0].heatmaps[0].max, 8);
        assert_eq!(output[0].heatmaps[0].bucket_counts.iter().sum::<u64>(), 3);
    }

    #[test]
    fn heatmap_supports_full_counter_range() {
        let stat = stat("Ethernet0", 0);
        let mut heatmap = HeatmapAccumulator::new(0);
        heatmap.record(u64::MAX);

        let heatmap = heatmap.into_message(&stat, 0, 10_000);

        assert_eq!(heatmap.count, 2);
        assert_eq!(heatmap.min, 0);
        assert_eq!(heatmap.max, u64::MAX);
        assert_eq!(heatmap.bucket_counts.iter().sum::<u64>(), 2);
        assert_eq!(
            heatmap.bucket_counts.len(),
            heatmap.explicit_bounds.len() + 1
        );
    }

    #[test]
    fn preserves_unified_messages_that_already_contain_heatmaps() {
        let stats = sample(1_000, vec![stat("Ethernet0", 1)]);
        let heatmap = HeatmapAccumulator::new(1).into_message(&stats.stats[0], 0, 10_000);
        let message = AggregatorStatsMessage::with_heatmaps(
            Some(Arc::from("session")),
            stats.clone(),
            vec![heatmap.clone()],
        );

        let (config_sender, config_receiver) = tokio::sync::mpsc::channel(1);
        let (_stats_sender, stats_receiver) = tokio::sync::mpsc::channel(1);
        let mut actor = AggregatorActor::new(config_receiver, stats_receiver);
        let output = actor.handle_stats(message);
        drop(config_sender);

        assert_eq!(output.len(), 1);
        assert_eq!(output[0].key.as_deref(), Some("session"));
        assert_eq!(output[0].stats, stats);
        assert_eq!(output[0].heatmaps.as_ref(), &[heatmap]);
    }
}
