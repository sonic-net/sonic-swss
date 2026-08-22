use ahash::{HashMap, HashMapExt};
use log::{debug, error};
use std::collections::{HashSet, LinkedList};
use std::sync::Arc;
use tokio::{
    select,
    sync::mpsc::{Receiver, Sender},
};

use crate::message::{
    aggregator::{
        AggregatedStatsMessage, AggregatorConfig, AggregatorConfigMessage, CounterSelector, Heatmap,
    },
    saistats::{SAIStat, SAIStats, SAIStatsMessage},
};

const NANOS_PER_MICROSECOND: u64 = 1_000;

#[derive(Debug)]
struct ReportingWindow {
    window: u64,
    observation_time: u64,
    stats: Vec<SAIStat>,
    stat_times: Vec<u64>,
    index: HashMap<String, HashMap<(u32, u32), usize>>,
}

impl ReportingWindow {
    fn new(window: u64, sample: &SAIStats) -> Self {
        let mut state = Self {
            window,
            observation_time: sample.observation_time,
            stats: Vec::with_capacity(sample.stats.len()),
            stat_times: Vec::with_capacity(sample.stats.len()),
            index: HashMap::with_capacity(sample.stats.len()),
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
                if sample.observation_time < self.stat_times[position] {
                    debug!(
                        "Ignoring late reporting-window sample for {} type {} stat {} at {}",
                        stat.object_name, stat.type_id, stat.stat_id, sample.observation_time
                    );
                    continue;
                }
                self.stats[position] = stat.clone();
                self.stat_times[position] = sample.observation_time;
            } else {
                let position = self.stats.len();
                self.stats.push(stat.clone());
                self.stat_times.push(sample.observation_time);
                self.index
                    .entry(stat.object_name.clone())
                    .or_default()
                    .insert((stat.type_id, stat.stat_id), position);
            }
        }
    }

    fn into_sample(self) -> SAIStatsMessage {
        Arc::new(SAIStats {
            observation_time: self.observation_time,
            stats: self.stats,
        })
    }
}

#[derive(Debug)]
struct HeatmapAccumulator {
    bounds: Arc<[u64]>,
    explicit_bounds: Arc<[f64]>,
    bucket_counts: Vec<u64>,
    count: u64,
    sum: u128,
    min: u64,
    max: u64,
}

impl HeatmapAccumulator {
    fn new(bounds: Arc<[u64]>, explicit_bounds: Arc<[f64]>, value: u64) -> Self {
        let mut accumulator = Self {
            bucket_counts: vec![0; bounds.len() + 1],
            bounds,
            explicit_bounds,
            count: 0,
            sum: 0,
            min: value,
            max: value,
        };
        accumulator.record(value);
        accumulator
    }

    fn record(&mut self, value: u64) {
        // Count bounds strictly below the value, making each configured bound
        // an inclusive upper bound and preserving the OTLP bounds + 1 invariant.
        let bucket = self.bounds.partition_point(|bound| *bound < value);
        self.bucket_counts[bucket] += 1;
        self.count += 1;
        self.sum += u128::from(value);
        self.min = self.min.min(value);
        self.max = self.max.max(value);
    }

    fn into_message(
        self,
        object_name: Arc<str>,
        type_id: u32,
        stat_id: u32,
        start_time_unix_nano: u64,
        time_unix_nano: u64,
    ) -> Heatmap {
        Heatmap {
            object_name,
            type_id,
            stat_id,
            start_time_unix_nano,
            time_unix_nano,
            count: self.count,
            sum: self.sum as f64,
            min: self.min,
            max: self.max,
            explicit_bounds: self.explicit_bounds,
            bucket_counts: self.bucket_counts,
        }
    }
}

#[derive(Debug)]
struct ReportingState {
    interval_ns: u64,
    current: Option<ReportingWindow>,
}

impl ReportingState {
    fn new(reporting_rate_us: u32) -> Self {
        Self {
            interval_ns: u64::from(reporting_rate_us) * NANOS_PER_MICROSECOND,
            current: None,
        }
    }

    fn process(&mut self, sample: &SAIStats) -> Option<SAIStatsMessage> {
        debug_assert_ne!(self.interval_ns, 0);

        let window = sample.observation_time.saturating_sub(1) / self.interval_ns;
        // Reporting-rate aggregation is sample-driven: a later sample closes
        // the previous window. For continuous stream telemetry this avoids a
        // per-session timer, accepting that the final partial window may remain
        // buffered when a stream becomes idle or ends.
        match self.current.as_mut() {
            None => {
                self.current = Some(ReportingWindow::new(window, sample));
                None
            }
            Some(current) if current.window == window => {
                current.merge(sample);
                None
            }
            Some(current) if window < current.window => {
                debug!(
                    "Ignoring late aggregator sample at {} in window {} (current window {})",
                    sample.observation_time, window, current.window
                );
                None
            }
            Some(_) => {
                let flushed = self.current.take().map(ReportingWindow::into_sample);
                self.current = Some(ReportingWindow::new(window, sample));
                flushed
            }
        }
    }
}

#[derive(Debug)]
struct HeatmapWindow {
    window: u64,
    heatmaps: HashMap<Arc<str>, HashMap<(u32, u32), HeatmapSeries>>,
}

#[derive(Debug)]
struct HeatmapSeries {
    last_observation_time: u64,
    accumulator: HeatmapAccumulator,
}

impl HeatmapWindow {
    fn new(window: u64) -> Self {
        Self {
            window,
            heatmaps: HashMap::new(),
        }
    }

    fn merge(
        &mut self,
        sample: &SAIStats,
        counters: &HashSet<CounterSelector>,
        bounds: &Arc<[u64]>,
        explicit_bounds: &Arc<[f64]>,
    ) {
        for stat in &sample.stats {
            if !counters.contains(&CounterSelector::new(stat.type_id, stat.stat_id)) {
                continue;
            }

            let key = (stat.type_id, stat.stat_id);
            if let Some(series) = self
                .heatmaps
                .get_mut(stat.object_name.as_str())
                .and_then(|series| series.get_mut(&key))
            {
                if sample.observation_time < series.last_observation_time {
                    debug!(
                        "Ignoring late heatmap sample for {} type {} stat {} at {}",
                        stat.object_name, stat.type_id, stat.stat_id, sample.observation_time
                    );
                    continue;
                }
                series.last_observation_time = sample.observation_time;
                series.accumulator.record(stat.counter);
                continue;
            }

            let series = HeatmapSeries {
                last_observation_time: sample.observation_time,
                accumulator: HeatmapAccumulator::new(
                    bounds.clone(),
                    explicit_bounds.clone(),
                    stat.counter,
                ),
            };
            if let Some(series_by_counter) = self.heatmaps.get_mut(stat.object_name.as_str()) {
                series_by_counter.insert(key, series);
            } else {
                self.heatmaps.insert(
                    Arc::from(stat.object_name.as_str()),
                    HashMap::from_iter([(key, series)]),
                );
            }
        }
    }

    fn into_heatmaps(self, interval_ns: u64) -> Vec<Heatmap> {
        let start_time_unix_nano = self.window.saturating_mul(interval_ns);
        let time_unix_nano = start_time_unix_nano.saturating_add(interval_ns);
        self.heatmaps
            .into_iter()
            .flat_map(|(object_name, heatmaps)| {
                heatmaps
                    .into_iter()
                    .map(move |((type_id, stat_id), series)| {
                        series.accumulator.into_message(
                            object_name.clone(),
                            type_id,
                            stat_id,
                            start_time_unix_nano,
                            time_unix_nano,
                        )
                    })
            })
            .collect()
    }
}

#[derive(Debug)]
struct HeatmapState {
    interval_ns: u64,
    counters: Arc<HashSet<CounterSelector>>,
    bucket_boundaries: Arc<[u64]>,
    explicit_bounds: Arc<[f64]>,
    current: Option<HeatmapWindow>,
}

impl HeatmapState {
    fn new(
        interval_us: u32,
        counters: Arc<HashSet<CounterSelector>>,
        bucket_boundaries: Arc<[u64]>,
    ) -> Self {
        let explicit_bounds = bucket_boundaries
            .iter()
            .map(|bound| *bound as f64)
            .collect::<Arc<[f64]>>();
        Self {
            interval_ns: u64::from(interval_us) * NANOS_PER_MICROSECOND,
            counters,
            bucket_boundaries,
            explicit_bounds,
            current: None,
        }
    }

    fn process(&mut self, sample: &SAIStats) -> Vec<Heatmap> {
        debug_assert_ne!(self.interval_ns, 0);
        let window = sample.observation_time.saturating_sub(1) / self.interval_ns;
        match self.current.as_mut() {
            None => {
                let mut current = HeatmapWindow::new(window);
                current.merge(
                    sample,
                    &self.counters,
                    &self.bucket_boundaries,
                    &self.explicit_bounds,
                );
                self.current = Some(current);
                Vec::new()
            }
            Some(current) if current.window == window => {
                current.merge(
                    sample,
                    &self.counters,
                    &self.bucket_boundaries,
                    &self.explicit_bounds,
                );
                Vec::new()
            }
            Some(current) if window < current.window => {
                debug!(
                    "Ignoring late heatmap sample at {} in window {} (current window {})",
                    sample.observation_time, window, current.window
                );
                Vec::new()
            }
            Some(_) => {
                let heatmaps = self
                    .current
                    .take()
                    .map(|current| current.into_heatmaps(self.interval_ns))
                    .unwrap_or_default();
                let mut current = HeatmapWindow::new(window);
                current.merge(
                    sample,
                    &self.counters,
                    &self.bucket_boundaries,
                    &self.explicit_bounds,
                );
                self.current = Some(current);
                heatmaps
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
    config: AggregatorConfig,
    rollover: Option<Box<RolloverState>>,
    reporting: Option<ReportingState>,
    heatmap: Option<HeatmapState>,
}

#[derive(Debug)]
struct RolloverState {
    counters: HashSet<CounterSelector>,
    series: HashMap<String, HashMap<(u32, u32), RolloverSeries>>,
}

#[derive(Debug)]
struct RolloverSeries {
    last_raw: u64,
    offset: u64,
    last_corrected: u64,
    last_observation_time: u64,
}

impl RolloverSeries {
    fn new(value: u64, observation_time: u64) -> Self {
        Self {
            last_raw: value,
            offset: 0,
            last_corrected: value,
            last_observation_time: observation_time,
        }
    }

    fn correct(&mut self, raw: u64, observation_time: u64) -> Option<u64> {
        if observation_time < self.last_observation_time {
            return None;
        }
        if raw < self.last_raw {
            self.offset = self.last_corrected;
        }

        let corrected = match self.offset.checked_add(raw) {
            Some(corrected) => corrected,
            None => {
                error!(
                    "Rollover correction overflow: offset {} + raw {}; re-baselining series",
                    self.offset, raw
                );
                *self = Self::new(raw, observation_time);
                return Some(raw);
            }
        };
        self.last_raw = raw;
        self.last_corrected = corrected;
        self.last_observation_time = observation_time;
        Some(corrected)
    }
}

impl RolloverState {
    fn new(counters: HashSet<CounterSelector>) -> Self {
        Self {
            counters,
            series: HashMap::new(),
        }
    }

    fn process(&mut self, sample: SAIStatsMessage) -> SAIStatsMessage {
        if !sample.stats.iter().any(|stat| {
            self.counters
                .contains(&CounterSelector::new(stat.type_id, stat.stat_id))
        }) {
            return sample;
        }

        let observation_time = sample.observation_time;
        let mut corrected = sample;
        {
            let corrected = Arc::make_mut(&mut corrected);
            corrected.stats.retain_mut(|stat| {
                if !self
                    .counters
                    .contains(&CounterSelector::new(stat.type_id, stat.stat_id))
                {
                    return true;
                }

                let key = (stat.type_id, stat.stat_id);
                if let Some(state) = self
                    .series
                    .get_mut(stat.object_name.as_str())
                    .and_then(|states| states.get_mut(&key))
                {
                    let Some(value) = state.correct(stat.counter, observation_time) else {
                        debug!(
                            "Ignoring late rollover sample for {} type {} stat {} at {}",
                            stat.object_name, stat.type_id, stat.stat_id, observation_time
                        );
                        return false;
                    };
                    stat.counter = value;
                    return true;
                }

                let series = RolloverSeries::new(stat.counter, observation_time);
                if let Some(states) = self.series.get_mut(stat.object_name.as_str()) {
                    states.insert(key, series);
                } else {
                    self.series.insert(
                        stat.object_name.clone(),
                        HashMap::from_iter([(key, series)]),
                    );
                }
                true
            });
        }

        corrected
    }
}

impl AggregatorState {
    fn new(config: AggregatorConfig) -> Self {
        let rollover = (!config.rollover_counters.is_empty())
            .then(|| Box::new(RolloverState::new(config.rollover_counters.clone())));
        let reporting = config.reporting_rate.map(ReportingState::new);
        let heatmap = config.heatmap_interval.map(|interval| {
            HeatmapState::new(
                interval,
                Arc::new(config.heatmap_counters.clone()),
                Arc::from(config.heatmap_bucket_boundaries.clone()),
            )
        });

        Self {
            config,
            rollover,
            reporting,
            heatmap,
        }
    }

    fn process(&mut self, sample: SAIStatsMessage) -> Option<AggregatedStatsMessage> {
        let mut sample = sample;

        if let Some(rollover) = self.rollover.as_mut() {
            sample = rollover.process(sample);
        }

        if let Some(reporting) = self.reporting.as_mut() {
            let Some(reported) = reporting.process(sample.as_ref()) else {
                return None;
            };
            sample = reported;
        }

        let mut heatmaps = Vec::new();
        if let Some(heatmap) = self.heatmap.as_mut() {
            heatmaps = heatmap.process(sample.as_ref());
        }

        Some(AggregatedStatsMessage::with_heatmaps(
            None, sample, heatmaps,
        ))
    }
}

impl Aggregator {
    pub fn set_config(&mut self, key: String, config: Option<AggregatorConfig>) {
        match config {
            Some(config) => {
                if let Err(reason) = config.validate() {
                    error!(
                        "Rejecting aggregator config for session {}: {}",
                        key, reason
                    );
                    return;
                }
                if let Some(state) = self.sessions.get_mut(&key) {
                    if state.config == config {
                        return;
                    }

                    let preserve_rollover =
                        state.config.rollover_counters == config.rollover_counters;
                    let mut replacement = AggregatorState::new(config);
                    if preserve_rollover {
                        replacement.rollover = state.rollover.take();
                    }
                    // Partial reporting and heatmap windows are intentionally
                    // discarded when their configuration changes.
                    *state = replacement;
                } else {
                    self.sessions.insert(key, AggregatorState::new(config));
                }
            }
            None => {
                // Removing a session discards any partial reporting or heatmap window.
                self.sessions.remove(&key);
            }
        }
    }

    pub fn remove_config(&mut self, key: &str) {
        // Session teardown discards any partial reporting or heatmap window.
        self.sessions.remove(key);
    }

    pub fn process(
        &mut self,
        key: Option<Arc<str>>,
        sample: SAIStatsMessage,
    ) -> Option<AggregatedStatsMessage> {
        let Some(key) = key else {
            return Some(sample.into());
        };

        let Some(state) = self.sessions.get_mut(key.as_ref()) else {
            return Some(AggregatedStatsMessage::new(Some(key), sample));
        };

        state.process(sample).map(|mut message| {
            message.key = Some(key);
            message
        })
    }
}

pub struct AggregatorActor {
    config_recipient: Receiver<AggregatorConfigMessage>,
    stats_recipient: Receiver<AggregatedStatsMessage>,
    recipients: LinkedList<Sender<AggregatedStatsMessage>>,
    aggregator: Aggregator,
}

impl AggregatorActor {
    pub fn new(
        config_recipient: Receiver<AggregatorConfigMessage>,
        stats_recipient: Receiver<AggregatedStatsMessage>,
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

        if message
            .config
            .as_ref()
            .is_some_and(|config| config.reporting_rate.is_none())
        {
            debug!(
                "Aggregator config for session {} uses the lower-layer reporting interval",
                message.key
            );
        }
        self.aggregator.set_config(message.key, message.config);
    }

    fn handle_stats(&mut self, message: AggregatedStatsMessage) -> Option<AggregatedStatsMessage> {
        // A heatmap-bearing envelope is already aggregator output. Preserve it
        // unchanged if a downstream message is ever recirculated through here.
        if !message.heatmaps.is_empty() {
            return Some(message);
        }

        self.aggregator.process(message.key, message.stats)
    }

    pub async fn run(mut actor: AggregatorActor) {
        loop {
            select! {
                biased;
                config = actor.config_recipient.recv() => {
                    match config {
                        Some(config) => actor.handle_config(config),
                        // The SWSS config producer is critical; closure
                        // intentionally terminates this actor and lets the
                        // supervisor shut down the daemon.
                        None => break,
                    }
                },
                stats = actor.stats_recipient.recv() => {
                    match stats {
                        Some(stats) => {
                            if let Some(message) = actor.handle_stats(stats) {
                                // Await bounded sinks intentionally: enabled
                                // consumers share end-to-end backpressure and
                                // the daemon's critical failure domain.
                                for recipient in &actor.recipients {
                                    if recipient.send(message.clone()).await.is_err() {
                                        error!("Aggregator output channel closed");
                                        return;
                                    }
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

    fn process(
        aggregator: &mut Aggregator,
        key: Option<Arc<str>>,
        sample: SAIStatsMessage,
    ) -> Vec<AggregatedStatsMessage> {
        aggregator.process(key, sample).into_iter().collect()
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
        let output = process(&mut aggregator, Some(Arc::from("session")), input.clone());

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
            .process(
                Some(Arc::from("session")),
                sample(1_000, vec![stat("Ethernet0", 1)])
            )
            .is_none());
        assert!(aggregator
            .process(
                Some(Arc::from("session")),
                sample(9_000, vec![stat("Ethernet0", 9)])
            )
            .is_none());

        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![stat("Ethernet0", 10)]),
        );

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

        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_000, vec![stat("Ethernet0", 1)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(2_000, vec![stat("Ethernet0", 2), stat("Ethernet4", 3)]),
        );

        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(11_000, vec![stat("Ethernet0", 11)]),
        );

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
            .process(
                Some(Arc::from("session")),
                sample(1_000, vec![stat("Ethernet0", 1)])
            )
            .is_none());

        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(20),
                ..Default::default()
            }),
        );

        assert!(aggregator
            .process(
                Some(Arc::from("session")),
                sample(11_000, vec![stat("Ethernet0", 11)])
            )
            .is_none());
    }

    #[test]
    fn preserves_rollover_state_when_config_is_unchanged() {
        let mut aggregator = Aggregator::default();
        let config = AggregatorConfig {
            rollover_counters: HashSet::from([CounterSelector::new(1, 2)]),
            ..Default::default()
        };
        aggregator.set_config("session".to_string(), Some(config.clone()));
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_000, vec![stat("Ethernet0", 200)]),
        );

        aggregator.set_config("session".to_string(), Some(config));
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(2_000, vec![stat("Ethernet0", 10)]),
        );

        assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", 210)]);
    }

    #[test]
    fn preserves_rollover_state_when_unrelated_config_changes() {
        let mut aggregator = Aggregator::default();
        let selector = CounterSelector::new(1, 2);
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([selector]),
                ..Default::default()
            }),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_000, vec![stat("Ethernet0", 200)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(2_000, vec![stat("Ethernet0", 10)]),
        );

        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([selector]),
                heatmap_interval: Some(10),
                heatmap_counters: HashSet::from([selector]),
                heatmap_bucket_boundaries: vec![100, 300],
                ..Default::default()
            }),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(3_000, vec![stat("Ethernet0", 20)]),
        );

        assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", 220)]);
    }

    #[test]
    fn resets_rollover_state_when_selector_changes() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([CounterSelector::new(1, 2)]),
                ..Default::default()
            }),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_000, vec![stat("Ethernet0", 200)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(2_000, vec![stat("Ethernet0", 10)]),
        );

        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([CounterSelector::new(1, 3)]),
                ..Default::default()
            }),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(3_000, vec![stat("Ethernet0", 20)]),
        );

        assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", 20)]);
    }

    #[test]
    fn rollover_overflow_rebaselines_and_recovers() {
        let mut series = RolloverSeries::new(u64::MAX - 5, 1);

        assert_eq!(series.correct(10, 2), Some(10));
        assert_eq!(series.correct(20, 3), Some(20));
        assert_eq!(series.correct(5, 4), Some(25));
    }

    #[test]
    fn corrects_rollovers_without_reporting_rate() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([CounterSelector::new(1, 2)]),
                ..Default::default()
            }),
        );

        for (time, raw, expected) in [
            (1_000, 100, 100),
            (2_000, 200, 200),
            (3_000, 10, 210),
            (4_000, 20, 220),
        ] {
            let output = process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![stat("Ethernet0", raw)]),
            );
            assert_eq!(output.len(), 1);
            assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", expected)]);
        }
    }

    #[test]
    fn ignores_late_samples_without_corrupting_rollover_state() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([CounterSelector::new(1, 2)]),
                ..Default::default()
            }),
        );

        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(2_000, vec![stat("Ethernet0", 200)]),
        );
        let late = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_000, vec![stat("Ethernet0", 10)]),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(3_000, vec![stat("Ethernet0", 10)]),
        );

        assert!(late[0].stats.stats.is_empty());
        assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", 210)]);
    }

    #[test]
    fn leaves_unselected_counters_unchanged_during_rollover() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([CounterSelector::new(1, 2)]),
                ..Default::default()
            }),
        );

        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(
                1_000,
                vec![
                    stat("Ethernet0", 200),
                    SAIStat {
                        stat_id: 3,
                        ..stat("Ethernet0", 200)
                    },
                ],
            ),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(
                2_000,
                vec![
                    stat("Ethernet0", 10),
                    SAIStat {
                        stat_id: 3,
                        ..stat("Ethernet0", 10)
                    },
                ],
            ),
        );

        assert_eq!(output[0].stats.stats[0].counter, 210);
        assert_eq!(output[0].stats.stats[1].counter, 10);
    }

    #[test]
    fn composes_rollover_with_reporting_rate() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(10),
                rollover_counters: HashSet::from([CounterSelector::new(1, 2)]),
                ..Default::default()
            }),
        );

        for (time, raw) in [(1_000, 100), (2_000, 200), (3_000, 10)] {
            assert!(aggregator
                .process(
                    Some(Arc::from("session")),
                    sample(time, vec![stat("Ethernet0", raw)])
                )
                .is_none());
        }
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![stat("Ethernet0", 20)]),
        );

        assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", 210)]);
        assert!(output[0].heatmaps.is_empty());
    }

    #[test]
    fn aggregates_lower_layer_samples_into_independent_heatmap_window() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                heatmap_interval: Some(10),
                heatmap_counters: HashSet::from([CounterSelector::new(1, 2)]),
                heatmap_bucket_boundaries: vec![100, 200, 300],
                ..Default::default()
            }),
        );

        let first = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_000, vec![stat("Ethernet0", 100)]),
        );
        let second = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(2_000, vec![stat("Ethernet0", 200)]),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![stat("Ethernet0", 300)]),
        );

        assert!(first[0].heatmaps.is_empty());
        assert!(second[0].heatmaps.is_empty());
        assert_eq!(output.len(), 1);
        assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", 300)]);
        assert_eq!(output[0].heatmaps.len(), 1);
        assert_eq!(output[0].heatmaps[0].count, 2);
        assert_eq!(
            output[0].heatmaps[0].explicit_bounds.as_ref(),
            &[100.0, 200.0, 300.0]
        );
        assert_eq!(output[0].heatmaps[0].bucket_counts, vec![1, 1, 0, 0]);
        assert_eq!(output[0].heatmaps[0].start_time_unix_nano, 0);
        assert_eq!(output[0].heatmaps[0].time_unix_nano, 10_000);
    }

    #[test]
    fn heatmap_window_tracks_each_selected_series() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                heatmap_interval: Some(10),
                heatmap_counters: HashSet::from([CounterSelector::new(1, 2)]),
                heatmap_bucket_boundaries: vec![100],
                ..Default::default()
            }),
        );

        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_000, vec![stat("Ethernet0", 1)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(2_000, vec![stat("Ethernet4", 2)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(3_000, vec![stat("Ethernet0", 3)]),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![stat("Ethernet0", 4)]),
        );

        let ethernet0 = output[0]
            .heatmaps
            .iter()
            .find(|heatmap| heatmap.object_name.as_ref() == "Ethernet0")
            .expect("Ethernet0 heatmap");
        let ethernet4 = output[0]
            .heatmaps
            .iter()
            .find(|heatmap| heatmap.object_name.as_ref() == "Ethernet4")
            .expect("Ethernet4 heatmap");
        assert_eq!(ethernet0.count, 2);
        assert_eq!(ethernet4.count, 1);
        assert_eq!(ethernet0.start_time_unix_nano, 0);
        assert_eq!(ethernet0.time_unix_nano, 10_000);
        assert_eq!(ethernet4.start_time_unix_nano, 0);
        assert_eq!(ethernet4.time_unix_nano, 10_000);
    }

    #[test]
    fn applies_rollover_before_independent_heatmap() {
        let mut aggregator = Aggregator::default();
        let selector = CounterSelector::new(1, 2);
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([selector]),
                heatmap_interval: Some(10),
                heatmap_counters: HashSet::from([selector]),
                heatmap_bucket_boundaries: vec![100, 200, 300],
                ..Default::default()
            }),
        );

        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_000, vec![stat("Ethernet0", 200)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(2_000, vec![stat("Ethernet0", 10)]),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![stat("Ethernet0", 20)]),
        );

        assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", 220)]);
        assert_eq!(output[0].heatmaps[0].count, 2);
        assert_eq!(output[0].heatmaps[0].min, 200);
        assert_eq!(output[0].heatmaps[0].max, 210);
        assert_eq!(output[0].heatmaps[0].bucket_counts, vec![0, 1, 1, 0]);
    }

    #[test]
    fn collects_heatmap_samples_with_configured_buckets() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                heatmap_interval: Some(10),
                heatmap_counters: HashSet::from([CounterSelector::new(1, 2)]),
                heatmap_bucket_boundaries: vec![1, 2, 8],
                ..Default::default()
            }),
        );

        for (time, value) in [(1_000, 1), (2_000, 2), (9_000, 8)] {
            let output = process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![stat("Ethernet0", value)]),
            );
            assert_eq!(output.len(), 1);
            assert!(output[0].heatmaps.is_empty());
        }
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![stat("Ethernet0", 10)]),
        );

        assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", 10)]);
        assert_eq!(output[0].heatmaps.len(), 1);
        assert_eq!(output[0].heatmaps[0].count, 3);
        assert_eq!(output[0].heatmaps[0].sum, 11.0);
        assert_eq!(output[0].heatmaps[0].min, 1);
        assert_eq!(output[0].heatmaps[0].max, 8);
        assert_eq!(
            output[0].heatmaps[0].explicit_bounds.as_ref(),
            &[1.0, 2.0, 8.0]
        );
        assert_eq!(output[0].heatmaps[0].bucket_counts, vec![1, 1, 1, 0]);
    }

    #[test]
    fn includes_exact_timestamp_boundary_in_preceding_heatmap_window() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                heatmap_interval: Some(10),
                heatmap_counters: HashSet::from([CounterSelector::new(1, 2)]),
                heatmap_bucket_boundaries: vec![1, 2, 3],
                ..Default::default()
            }),
        );

        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1, vec![stat("Ethernet0", 1)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_000, vec![stat("Ethernet0", 2)]),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![stat("Ethernet0", 3)]),
        );

        assert_eq!(output[0].heatmaps[0].count, 2);
        assert_eq!(output[0].heatmaps[0].start_time_unix_nano, 0);
        assert_eq!(output[0].heatmaps[0].time_unix_nano, 10_000);
    }

    #[test]
    fn ignores_late_samples_without_corrupting_heatmap_window() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                heatmap_interval: Some(10),
                heatmap_counters: HashSet::from([CounterSelector::new(1, 2)]),
                heatmap_bucket_boundaries: vec![1, 2, 3],
                ..Default::default()
            }),
        );

        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(5_000, vec![stat("Ethernet0", 2)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(4_000, vec![stat("Ethernet0", 1), stat("Ethernet4", 3)]),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![stat("Ethernet0", 3)]),
        );

        let ethernet0 = output[0]
            .heatmaps
            .iter()
            .find(|heatmap| heatmap.object_name.as_ref() == "Ethernet0")
            .expect("Ethernet0 heatmap");
        let ethernet4 = output[0]
            .heatmaps
            .iter()
            .find(|heatmap| heatmap.object_name.as_ref() == "Ethernet4")
            .expect("Ethernet4 heatmap");
        assert_eq!(ethernet0.count, 1);
        assert_eq!(ethernet0.bucket_counts, vec![0, 1, 0, 0]);
        assert_eq!(ethernet4.count, 1);
        assert_eq!(ethernet4.bucket_counts, vec![0, 0, 1, 0]);
    }

    #[test]
    fn heatmap_consumes_only_accepted_reporting_points() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(10),
                heatmap_interval: Some(20),
                heatmap_counters: HashSet::from([CounterSelector::new(1, 2)]),
                heatmap_bucket_boundaries: vec![1, 2, 3],
                ..Default::default()
            }),
        );

        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(5_000, vec![stat("Ethernet0", 2)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(4_000, vec![stat("Ethernet0", 1)]),
        );
        let first = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![stat("Ethernet0", 3)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(20_001, vec![stat("Ethernet0", 4)]),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(30_001, vec![stat("Ethernet0", 5)]),
        );

        assert_eq!(first[0].stats.stats, vec![stat("Ethernet0", 2)]);
        assert!(first[0].heatmaps.is_empty());
        assert_eq!(output[0].heatmaps[0].count, 2);
        assert_eq!(output[0].heatmaps[0].bucket_counts, vec![0, 1, 1, 0]);
    }

    #[test]
    fn heatmap_supports_full_counter_range() {
        let mut heatmap = HeatmapAccumulator::new(Arc::from([0]), Arc::from([0.0]), 0);
        heatmap.record(u64::MAX);

        let heatmap = heatmap.into_message(Arc::from("Ethernet0"), 1, 2, 0, 10_000);

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
    fn composes_rollover_reporting_and_independent_heatmap_in_order() {
        let mut aggregator = Aggregator::default();
        let selector = CounterSelector::new(1, 2);
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(100),
                rollover_counters: HashSet::from([selector]),
                heatmap_interval: Some(1_000),
                heatmap_counters: HashSet::from([selector]),
                heatmap_bucket_boundaries: vec![100, 500, 1_000],
            }),
        );

        let mut messages = Vec::new();
        for sample_index in 1..=100u64 {
            let time = sample_index * 10_000;
            let raw = ((sample_index - 1) % 20 + 1) * 10;
            messages.extend(process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![stat("Ethernet0", raw)]),
            ));
        }
        messages.extend(process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_000_001, Vec::new()),
        ));
        messages.extend(process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_100_001, Vec::new()),
        ));

        let reporting_points = messages
            .iter()
            .filter_map(|message| message.stats.stats.first())
            .map(|stat| stat.counter)
            .collect::<Vec<_>>();
        assert_eq!(
            reporting_points,
            vec![100, 200, 300, 400, 500, 600, 700, 800, 900, 1_000]
        );
        let heatmap = messages
            .iter()
            .find_map(|message| message.heatmaps.first())
            .expect("completed heatmap window");
        assert_eq!(heatmap.count, 10);
        assert_eq!(heatmap.sum, 5_500.0);
        assert_eq!(heatmap.min, 100);
        assert_eq!(heatmap.max, 1_000);
        assert_eq!(heatmap.bucket_counts, vec![1, 4, 5, 0]);
        assert_eq!(heatmap.start_time_unix_nano, 0);
        assert_eq!(heatmap.time_unix_nano, 1_000_000);
    }

    #[test]
    fn preserves_unified_messages_that_already_contain_heatmaps() {
        let stats = sample(1_000, vec![stat("Ethernet0", 1)]);
        let heatmap = HeatmapAccumulator::new(Arc::from([1, 2, 8]), Arc::from([1.0, 2.0, 8.0]), 1)
            .into_message(Arc::from("Ethernet0"), 1, 2, 0, 10_000);
        let message = AggregatedStatsMessage::with_heatmaps(
            Some(Arc::from("session")),
            stats.clone(),
            vec![heatmap.clone()],
        );

        let (config_sender, config_receiver) = tokio::sync::mpsc::channel(1);
        let (_stats_sender, stats_receiver) = tokio::sync::mpsc::channel(1);
        let mut actor = AggregatorActor::new(config_receiver, stats_receiver);
        let output = actor.handle_stats(message).expect("passthrough message");
        drop(config_sender);

        assert_eq!(output.key.as_deref(), Some("session"));
        assert_eq!(output.stats, stats);
        assert_eq!(output.heatmaps.as_ref(), &[heatmap]);
    }
}
