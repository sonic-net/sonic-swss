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
        heatmap_schema, AggregatedStatsMessage, AggregatorConfig, AggregatorConfigMessage,
        CounterSelector, Heatmap, HeatmapLayout, HeatmapValueKind,
    },
    saistats::{SAIStat, SAIStats, SAIStatsMessage},
};

const NANOS_PER_MICROSECOND: u64 = 1_000;

#[derive(Debug)]
struct ReportingWindow {
    window: u64,
    observation_time: u64,
    stats: Vec<SAIStat>,
    heatmap_values: Option<Vec<u64>>,
    stat_times: Vec<u64>,
    index: HashMap<String, HashMap<(u32, u32), usize>>,
    watermark_counters: Arc<HashSet<CounterSelector>>,
}

impl ReportingWindow {
    fn new(
        window: u64,
        sample: &SAIStats,
        watermark_counters: Arc<HashSet<CounterSelector>>,
    ) -> Self {
        let mut state = Self {
            window,
            observation_time: sample.observation_time,
            stats: Vec::with_capacity(sample.stats.len()),
            heatmap_values: None,
            stat_times: Vec::with_capacity(sample.stats.len()),
            index: HashMap::with_capacity(sample.stats.len()),
            watermark_counters,
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
                let selector = CounterSelector::new(stat.type_id, stat.stat_id);
                if self.watermark_counters.contains(&selector) {
                    let heatmap_values = self.heatmap_values.get_or_insert_with(|| {
                        self.stats.iter().map(|stat| stat.counter).collect()
                    });
                    heatmap_values[position] = heatmap_values[position].max(stat.counter);
                } else if let Some(heatmap_values) = self.heatmap_values.as_mut() {
                    heatmap_values[position] = stat.counter;
                }
                self.stats[position] = stat.clone();
                self.stat_times[position] = sample.observation_time;
            } else {
                let position = self.stats.len();
                let is_watermark = self
                    .watermark_counters
                    .contains(&CounterSelector::new(stat.type_id, stat.stat_id));
                if is_watermark && self.heatmap_values.is_none() {
                    self.heatmap_values = Some(
                        self.stats.iter().map(|existing| existing.counter).collect(),
                    );
                }
                if let Some(heatmap_values) = self.heatmap_values.as_mut() {
                    heatmap_values.push(stat.counter);
                }
                self.stats.push(stat.clone());
                self.stat_times.push(sample.observation_time);
                self.index
                    .entry(stat.object_name.clone())
                    .or_default()
                    .insert((stat.type_id, stat.stat_id), position);
            }
        }
    }

    fn into_sample(self, interval_ns: u64) -> ReportedSample {
        let heatmap_time = self.window.saturating_add(1).saturating_mul(interval_ns);
        let stats = Arc::new(SAIStats {
            observation_time: self.observation_time,
            stats: self.stats,
        });
        ReportedSample {
            stats,
            heatmap_values: self.heatmap_values,
            heatmap_time,
        }
    }
}

struct ReportedSample {
    stats: SAIStatsMessage,
    heatmap_values: Option<Vec<u64>>,
    heatmap_time: u64,
}

#[derive(Debug)]
struct HeatmapAccumulator {
    layout: Arc<HeatmapLayout>,
    value_kind: HeatmapValueKind,
    schema: Arc<str>,
    bucket_counts: Vec<u64>,
    count: u64,
    sum: u128,
    min: u64,
    max: u64,
}

impl HeatmapAccumulator {
    fn new(
        layout: Arc<HeatmapLayout>,
        value_kind: HeatmapValueKind,
        schema: Arc<str>,
        value: u64,
    ) -> Self {
        let mut accumulator = Self {
            bucket_counts: vec![0; layout.bucket_count()],
            layout,
            value_kind,
            schema,
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
        let bucket = self
            .layout
            .explicit_bounds_u64()
            .partition_point(|bound| *bound < value);
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
            explicit_bounds: self.layout.explicit_bounds(),
            bucket_counts: self.bucket_counts,
            value_kind: self.value_kind,
            schema: self.schema,
        }
    }
}

#[derive(Debug)]
struct ReportingState {
    interval_ns: u64,
    watermark_counters: Arc<HashSet<CounterSelector>>,
    current: Option<ReportingWindow>,
}

impl ReportingState {
    fn new(reporting_rate_us: u32, watermark_counters: Arc<HashSet<CounterSelector>>) -> Self {
        Self {
            interval_ns: u64::from(reporting_rate_us) * NANOS_PER_MICROSECOND,
            watermark_counters,
            current: None,
        }
    }

    fn process(&mut self, sample: &SAIStats) -> Option<ReportedSample> {
        debug_assert_ne!(self.interval_ns, 0);

        let window = sample.observation_time.saturating_sub(1) / self.interval_ns;
        // Reporting-rate aggregation is sample-driven: a later sample closes
        // the previous window. For continuous stream telemetry this avoids a
        // per-session timer, accepting that the final partial window may remain
        // buffered when a stream becomes idle or ends.
        match self.current.as_mut() {
            None => {
                self.current = Some(ReportingWindow::new(
                    window,
                    sample,
                    self.watermark_counters.clone(),
                ));
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
                let flushed = self
                    .current
                    .take()
                    .map(|window| window.into_sample(self.interval_ns));
                self.current = Some(ReportingWindow::new(
                    window,
                    sample,
                    self.watermark_counters.clone(),
                ));
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
    accumulator: HeatmapAccumulator,
}

#[derive(Debug)]
struct HeatmapSelector {
    value_kind: HeatmapValueKind,
    layout: Arc<HeatmapLayout>,
    schema: Arc<str>,
}

impl HeatmapWindow {
    fn new(window: u64) -> Self {
        Self {
            window,
            heatmaps: HashMap::new(),
        }
    }

    fn record(&mut self, stat: &SAIStat, value: u64, selector: &HeatmapSelector) {
        let key = (stat.type_id, stat.stat_id);
        if let Some(series) = self
            .heatmaps
            .get_mut(stat.object_name.as_str())
            .and_then(|series| series.get_mut(&key))
        {
            series.accumulator.record(value);
            return;
        }

        let series = HeatmapSeries {
            accumulator: HeatmapAccumulator::new(
                selector.layout.clone(),
                selector.value_kind,
                selector.schema.clone(),
                value,
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
    selectors: HashMap<CounterSelector, HeatmapSelector>,
    series: HashMap<String, HashMap<(u32, u32), HeatmapValueSeries>>,
    current: Option<HeatmapWindow>,
}

#[derive(Debug)]
struct HeatmapValueSeries {
    last_observation_time: u64,
    baseline: Option<u64>,
}

impl HeatmapValueSeries {
    fn new() -> Self {
        Self {
            last_observation_time: 0,
            baseline: None,
        }
    }

    fn transform(
        &mut self,
        value_kind: HeatmapValueKind,
        value: u64,
        observation_time: u64,
    ) -> Option<u64> {
        if self.baseline.is_some() && observation_time < self.last_observation_time {
            return None;
        }
        self.last_observation_time = observation_time;

        if value_kind != HeatmapValueKind::Delta {
            self.baseline = Some(value);
            return Some(value);
        }

        let Some(previous) = self.baseline.replace(value) else {
            return None;
        };
        value.checked_sub(previous)
    }
}

impl HeatmapState {
    fn new(interval_us: u32, config: &AggregatorConfig) -> Self {
        let selectors = config
            .heatmap_counters
            .iter()
            .map(|selector| {
                let value_kind = selector.heatmap_value_kind();
                let layout = config
                    .layout_for(*selector)
                    .expect("validated heatmap layout");
                let schema = heatmap_schema(value_kind, layout.explicit_bounds_u64());
                (
                    *selector,
                    HeatmapSelector {
                        value_kind,
                        layout,
                        schema,
                    },
                )
            })
            .collect();
        Self {
            interval_ns: u64::from(interval_us) * NANOS_PER_MICROSECOND,
            selectors,
            series: HashMap::new(),
            current: None,
        }
    }

    fn process(
        &mut self,
        sample: &SAIStats,
        input_values: Option<&[u64]>,
        accepted_time: u64,
    ) -> Vec<Heatmap> {
        debug_assert_ne!(self.interval_ns, 0);
        debug_assert!(input_values.map_or(true, |values| values.len() == sample.stats.len()));
        let window = accepted_time.saturating_sub(1) / self.interval_ns;
        if self
            .current
            .as_ref()
            .is_some_and(|current| window < current.window)
        {
            if let Some(current) = self.current.as_ref() {
                debug!(
                    "Ignoring late heatmap sample at {} in window {} (current window {})",
                    accepted_time, window, current.window
                );
            }
            return Vec::new();
        }

        let heatmaps = if self
            .current
            .as_ref()
            .is_some_and(|current| current.window < window)
        {
            self.current
                .take()
                .map(|current| current.into_heatmaps(self.interval_ns))
                .unwrap_or_default()
        } else {
            Vec::new()
        };
        let current = self
            .current
            .get_or_insert_with(|| HeatmapWindow::new(window));

        for (position, stat) in sample.stats.iter().enumerate() {
            let selector_key = CounterSelector::new(stat.type_id, stat.stat_id);
            let Some(selector) = self.selectors.get(&selector_key) else {
                continue;
            };
            let key = (stat.type_id, stat.stat_id);
            let series = if let Some(series) = self.series.get_mut(stat.object_name.as_str()) {
                series.entry(key).or_insert_with(HeatmapValueSeries::new)
            } else {
                self.series.insert(
                    stat.object_name.clone(),
                    HashMap::from_iter([(key, HeatmapValueSeries::new())]),
                );
                self.series
                    .get_mut(stat.object_name.as_str())
                    .and_then(|series| series.get_mut(&key))
                    .expect("inserted heatmap value series")
            };
            let Some(value) = series.transform(
                selector.value_kind,
                input_values.map_or(stat.counter, |values| values[position]),
                accepted_time,
            ) else {
                if accepted_time < series.last_observation_time {
                    debug!(
                        "Ignoring late heatmap sample for {} type {} stat {} at {}",
                        stat.object_name, stat.type_id, stat.stat_id, accepted_time
                    );
                }
                continue;
            };
            current.record(stat, value, selector);
        }

        heatmaps
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
        let watermark_counters = Arc::new(
            config
                .heatmap_counters
                .iter()
                .copied()
                .filter(|selector| selector.heatmap_value_kind() == HeatmapValueKind::Watermark)
                .collect(),
        );
        let reporting = config
            .reporting_rate
            .map(|rate| ReportingState::new(rate, watermark_counters));
        let heatmap = config
            .heatmap_interval
            .map(|interval| HeatmapState::new(interval, &config));

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

        let (heatmap_values, heatmap_time) = if let Some(reporting) = self.reporting.as_mut() {
            let Some(reported) = reporting.process(sample.as_ref()) else {
                return None;
            };
            sample = reported.stats;
            (reported.heatmap_values, reported.heatmap_time)
        } else {
            (None, sample.observation_time)
        };

        let mut heatmaps = Vec::new();
        if let Some(heatmap) = self.heatmap.as_mut() {
            heatmaps = heatmap.process(sample.as_ref(), heatmap_values.as_deref(), heatmap_time);
        }

        Some(AggregatedStatsMessage::with_heatmaps(
            None, sample, heatmaps,
        ))
    }
}

impl Aggregator {
    pub fn set_config(&mut self, key: String, config: Option<AggregatorConfig>) {
        self.update_config(key, config, false);
    }

    pub fn replace_config(&mut self, key: String, config: Option<AggregatorConfig>) {
        self.update_config(key, config, true);
    }

    fn update_config(&mut self, key: String, config: Option<AggregatorConfig>, reset: bool) {
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
                    if !reset && state.config == config {
                        return;
                    }

                    let preserve_rollover =
                        !reset && state.config.rollover_counters == config.rollover_counters;
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

    pub fn new_without_config(stats_recipient: Receiver<AggregatedStatsMessage>) -> Self {
        let (_sender, config_recipient) = tokio::sync::mpsc::channel(1);
        Self::new(config_recipient, stats_recipient)
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
        if message.reset {
            self.aggregator.replace_config(message.key, message.config);
        } else {
            self.aggregator.set_config(message.key, message.config);
        }
    }

    fn handle_stats(
        &mut self,
        mut message: AggregatedStatsMessage,
    ) -> Option<AggregatedStatsMessage> {
        if let Some(config) = message.config.take() {
            self.handle_config(config);
            return None;
        }

        // A heatmap-bearing envelope is already aggregator output. Preserve it
        // unchanged if a downstream message is ever recirculated through here.
        if !message.heatmaps.is_empty() {
            return Some(message);
        }

        self.aggregator.process(message.key, message.stats)
    }

    pub async fn run(mut actor: AggregatorActor) {
        let mut config_open = true;
        loop {
            select! {
                biased;
                config = actor.config_recipient.recv(), if config_open => {
                    match config {
                        Some(config) => actor.handle_config(config),
                        // Production configuration is sequenced through the
                        // IPFIX data channel. This direct channel remains for
                        // tests and embedders that configure AggregatorActor.
                        None => config_open = false,
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
    use std::collections::BTreeMap;

    fn stat(object_name: &str, counter: u64) -> SAIStat {
        SAIStat {
            object_name: object_name.to_string(),
            type_id: 1,
            stat_id: 2,
            counter,
        }
    }

    fn selected_stat(object_name: &str, selector: CounterSelector, counter: u64) -> SAIStat {
        SAIStat {
            object_name: object_name.to_string(),
            type_id: selector.type_id,
            stat_id: selector.stat_id,
            counter,
        }
    }

    fn heatmap_config(
        selectors: impl IntoIterator<Item = CounterSelector>,
        custom_bounds: impl IntoIterator<Item = (CounterSelector, Vec<u64>)>,
    ) -> AggregatorConfig {
        AggregatorConfig {
            heatmap_interval: Some(10),
            heatmap_counters: selectors.into_iter().collect(),
            heatmap_explicit_bounds: custom_bounds.into_iter().collect(),
            ..Default::default()
        }
    }

    fn accumulator(
        bounds: Vec<u64>,
        value_kind: HeatmapValueKind,
        value: u64,
    ) -> HeatmapAccumulator {
        let layout = HeatmapLayout::from_explicit_bounds(bounds).unwrap();
        let schema = heatmap_schema(value_kind, layout.explicit_bounds_u64());
        HeatmapAccumulator::new(layout, value_kind, schema, value)
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
    fn reporting_handoff_keeps_aligned_values_and_window_end_time() {
        let watermark = CounterSelector::new(21, crate::sai::SaiQueueStat::WatermarkBytes.to_u32());
        let mut reporting = ReportingState::new(10, Arc::new(HashSet::from([watermark])));
        let generic = CounterSelector::new(1, 2);

        assert!(reporting
            .process(
                sample(
                    10_001,
                    vec![
                        selected_stat("Queue0", watermark, 90),
                        selected_stat("Ethernet0", generic, 7),
                    ],
                )
                .as_ref(),
            )
            .is_none());
        assert!(reporting
            .process(sample(11_000, vec![selected_stat("Queue0", watermark, 20)]).as_ref())
            .is_none());
        let reported = reporting
            .process(sample(20_001, Vec::new()).as_ref())
            .expect("completed reporting window");

        assert_eq!(reported.stats.observation_time, 11_000);
        assert_eq!(reported.stats.stats[0].counter, 20);
        assert_eq!(reported.stats.stats[1].counter, 7);
        assert_eq!(reported.heatmap_values, Some(vec![90, 7]));
        assert_eq!(reported.heatmap_time, 20_000);
    }

    #[test]
    fn reporting_without_watermarks_does_not_allocate_heatmap_values() {
        let mut reporting = ReportingState::new(10, Arc::new(HashSet::new()));
        assert!(reporting
            .process(sample(1_000, vec![stat("Ethernet0", 1)]).as_ref())
            .is_none());
        let reported = reporting
            .process(sample(10_001, Vec::new()).as_ref())
            .expect("completed reporting window");

        assert!(reported.heatmap_values.is_none());
    }

    #[test]
    fn sparse_series_uses_reporting_window_end_for_heatmap_placement() {
        let selector =
            CounterSelector::new(21, crate::sai::SaiQueueStat::CurrOccupancyBytes.to_u32());
        let mut config = heatmap_config([selector], [(selector, vec![10, 100])]);
        config.reporting_rate = Some(10);
        config.heatmap_interval = Some(15);
        let mut aggregator = Aggregator::default();
        aggregator.set_config("session".to_string(), Some(config));

        assert!(aggregator
            .process(
                Some(Arc::from("session")),
                sample(10_001, vec![selected_stat("Queue0", selector, 42)]),
            )
            .is_none());
        let reported = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(20_001, Vec::new()),
        );
        assert_eq!(reported[0].stats.observation_time, 10_001);
        assert_eq!(reported[0].stats.stats[0].counter, 42);
        assert_eq!(reported[0].stats.stats.len(), 1);
        assert!(reported[0].heatmaps.is_empty());

        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(
                30_001,
                vec![SAIStat {
                    stat_id: 99,
                    ..stat("Ethernet0", 1)
                }],
            ),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(40_001, Vec::new()),
        );
        assert_eq!(output[0].heatmaps[0].start_time_unix_nano, 15_000);
        assert_eq!(output[0].heatmaps[0].time_unix_nano, 30_000);
        assert_eq!(output[0].heatmaps[0].count, 1);
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

    fn stateful_config() -> AggregatorConfig {
        let selector = CounterSelector::new(1, 2);
        AggregatorConfig {
            reporting_rate: Some(10),
            rollover_counters: HashSet::from([selector]),
            heatmap_interval: Some(20),
            heatmap_counters: HashSet::from([selector]),
            heatmap_explicit_bounds: BTreeMap::from([(selector, vec![5, 10])]),
            ..Default::default()
        }
    }

    fn actor_stats(time: u64, value: u64) -> AggregatedStatsMessage {
        AggregatedStatsMessage::new(
            Some(Arc::from("session")),
            sample(time, vec![stat("Ethernet0", value)]),
        )
    }

    #[test]
    fn ordinary_equal_config_preserves_all_state() {
        let (config_sender, config_receiver) = tokio::sync::mpsc::channel(1);
        let (_stats_sender, stats_receiver) = tokio::sync::mpsc::channel(1);
        let mut actor = AggregatorActor::new(config_receiver, stats_receiver);
        let config = stateful_config();
        actor.handle_config(AggregatorConfigMessage::new(
            "session".to_string(),
            Some(config.clone()),
        ));
        assert!(actor.handle_stats(actor_stats(1_000, 200)).is_none());
        assert!(actor.handle_stats(actor_stats(10_001, 10)).is_some());
        assert!(actor.handle_stats(actor_stats(20_001, 20)).is_some());

        actor.handle_config(AggregatorConfigMessage::new(
            "session".to_string(),
            Some(config),
        ));
        let output = actor
            .handle_stats(actor_stats(30_001, 30))
            .expect("preserved reporting window");
        drop(config_sender);

        assert_eq!(output.stats.stats[0].counter, 220);
        assert_eq!(output.heatmaps.len(), 1);
        assert_eq!(output.heatmaps[0].count, 1);
        assert_eq!(output.heatmaps[0].sum, 10.0);
    }

    #[test]
    fn equal_config_session_replacement_resets_all_state() {
        let (config_sender, config_receiver) = tokio::sync::mpsc::channel(1);
        let (_stats_sender, stats_receiver) = tokio::sync::mpsc::channel(1);
        let mut actor = AggregatorActor::new(config_receiver, stats_receiver);
        let config = stateful_config();
        actor.handle_config(AggregatorConfigMessage::new(
            "session".to_string(),
            Some(config.clone()),
        ));
        assert!(actor.handle_stats(actor_stats(1_000, 200)).is_none());
        assert!(actor.handle_stats(actor_stats(10_001, 10)).is_some());
        assert!(actor.handle_stats(actor_stats(20_001, 20)).is_some());

        actor.handle_config(AggregatorConfigMessage::replacement(
            "session".to_string(),
            Some(config),
        ));
        assert!(actor.handle_stats(actor_stats(30_001, 5)).is_none());
        let output = actor
            .handle_stats(actor_stats(40_001, 10))
            .expect("new reporting window");
        drop(config_sender);

        assert_eq!(output.stats.stats[0].counter, 5);
        assert!(output.heatmaps.is_empty());
    }

    #[test]
    fn ordered_control_envelope_resets_before_following_stats() {
        let (_config_sender, config_receiver) = tokio::sync::mpsc::channel(1);
        let (_stats_sender, stats_receiver) = tokio::sync::mpsc::channel(1);
        let mut actor = AggregatorActor::new(config_receiver, stats_receiver);
        let config = stateful_config();
        actor.handle_config(AggregatorConfigMessage::new(
            "session".to_string(),
            Some(config.clone()),
        ));
        assert!(actor.handle_stats(actor_stats(1_000, 200)).is_none());
        assert!(actor.handle_stats(actor_stats(10_001, 10)).is_some());

        assert!(actor
            .handle_stats(AggregatedStatsMessage::config(
                AggregatorConfigMessage::replacement("session".to_string(), Some(config)),
            ))
            .is_none());
        assert!(actor.handle_stats(actor_stats(20_001, 5)).is_none());
        let output = actor
            .handle_stats(actor_stats(30_001, 10))
            .expect("new reporting window");

        assert_eq!(output.stats.stats[0].counter, 5);
        assert!(output.heatmaps.is_empty());
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
                heatmap_explicit_bounds: BTreeMap::from([(selector, vec![100, 300])]),
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
    fn generic_heatmap_uses_deltas_and_inclusive_custom_bounds() {
        let selector = CounterSelector::new(1, 2);
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(heatmap_config([selector], [(selector, vec![1, 2, 8])])),
        );

        for (time, value) in [(1_000, 10), (2_000, 11), (3_000, 13), (4_000, 21)] {
            let output = process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![stat("Ethernet0", value)]),
            );
            assert_eq!(output[0].stats.stats, vec![stat("Ethernet0", value)]);
            assert!(output[0].heatmaps.is_empty());
        }
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![stat("Ethernet0", 22)]),
        );
        let heatmap = &output[0].heatmaps[0];
        assert_eq!(heatmap.count, 3);
        assert_eq!(heatmap.sum, 11.0);
        assert_eq!(heatmap.min, 1);
        assert_eq!(heatmap.max, 8);
        assert_eq!(heatmap.bucket_counts, vec![1, 1, 1, 0]);
        assert_eq!(heatmap.value_kind, HeatmapValueKind::Delta);
        assert_eq!(heatmap.start_time_unix_nano, 0);
        assert_eq!(heatmap.time_unix_nano, 10_000);
    }

    #[test]
    fn generic_reset_rebaselines_without_observation() {
        let selector = CounterSelector::new(1, 2);
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(heatmap_config([selector], [(selector, vec![5, 10])])),
        );
        for (time, value) in [(1_000, 100), (2_000, 110), (3_000, 7), (4_000, 12)] {
            process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![stat("Ethernet0", value)]),
            );
        }
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, Vec::new()),
        );
        assert_eq!(output[0].heatmaps[0].count, 2);
        assert_eq!(output[0].heatmaps[0].sum, 15.0);
        assert_eq!(output[0].heatmaps[0].bucket_counts, vec![1, 1, 0]);
    }

    #[test]
    fn generic_delta_continues_across_consecutive_heatmap_windows() {
        let selector = CounterSelector::new(1, 2);
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(heatmap_config([selector], [(selector, vec![5, 10])])),
        );

        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_000, vec![stat("Ethernet0", 100)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(2_000, vec![stat("Ethernet0", 105)]),
        );
        let first = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![stat("Ethernet0", 110)]),
        );
        let second = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(20_001, vec![stat("Ethernet0", 120)]),
        );

        assert_eq!(first[0].heatmaps[0].start_time_unix_nano, 0);
        assert_eq!(first[0].heatmaps[0].sum, 5.0);
        assert_eq!(second[0].heatmaps[0].start_time_unix_nano, 10_000);
        assert_eq!(second[0].heatmaps[0].sum, 5.0);
    }

    #[test]
    fn rollover_correction_precedes_generic_delta() {
        let selector = CounterSelector::new(1, 2);
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([selector]),
                ..heatmap_config([selector], [(selector, vec![10, 20])])
            }),
        );
        for (time, value) in [(1_000, 200), (2_000, 10), (3_000, 20)] {
            process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![stat("Ethernet0", value)]),
            );
        }
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, Vec::new()),
        );
        assert_eq!(output[0].heatmaps[0].count, 2);
        assert_eq!(output[0].heatmaps[0].bucket_counts, vec![2, 0, 0]);
    }

    #[test]
    fn watermark_uses_reporting_max_but_gauge_stays_latest() {
        let selector = CounterSelector::new(21, crate::sai::SaiQueueStat::WatermarkBytes.to_u32());
        let mut aggregator = Aggregator::default();
        let mut config = heatmap_config([selector], [(selector, vec![10, 100])]);
        config.reporting_rate = Some(10);
        config.heatmap_interval = Some(20);
        aggregator.set_config("session".to_string(), Some(config));

        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(1_000, vec![selected_stat("Queue0", selector, 90)]),
        );
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(9_000, vec![selected_stat("Queue0", selector, 20)]),
        );
        let first = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, vec![selected_stat("Queue0", selector, 5)]),
        );
        assert_eq!(first[0].stats.stats[0].counter, 20);
        process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(20_001, Vec::new()),
        );
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(30_001, Vec::new()),
        );
        let heatmap = &output[0].heatmaps[0];
        assert_eq!(heatmap.count, 2);
        assert_eq!(heatmap.min, 5);
        assert_eq!(heatmap.max, 90);
        assert_eq!(heatmap.value_kind, HeatmapValueKind::Watermark);
    }

    #[test]
    fn current_occupancy_uses_latest_accepted_value() {
        let selector = CounterSelector::new(
            24,
            crate::sai::SaiBufferPoolStat::CurrOccupancyBytes.to_u32(),
        );
        let mut aggregator = Aggregator::default();
        let mut config = heatmap_config([selector], [(selector, vec![10, 100])]);
        config.reporting_rate = Some(10);
        config.heatmap_interval = Some(20);
        aggregator.set_config("session".to_string(), Some(config));
        for (time, value) in [(1_000, 90), (9_000, 20), (10_001, 5), (20_001, 6)] {
            process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![selected_stat("pool", selector, value)]),
            );
        }
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(30_001, Vec::new()),
        );
        let heatmap = &output[0].heatmaps[0];
        assert_eq!(heatmap.count, 2);
        assert_eq!(heatmap.min, 5);
        assert_eq!(heatmap.max, 20);
        assert_eq!(heatmap.value_kind, HeatmapValueKind::CurrentOccupancy);
    }

    #[test]
    fn no_reporting_rate_accepts_every_lower_layer_point() {
        let selector =
            CounterSelector::new(21, crate::sai::SaiQueueStat::CurrOccupancyBytes.to_u32());
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(heatmap_config([selector], [(selector, vec![1, 2, 3])])),
        );
        for (time, value) in [(1_000, 1), (2_000, 2), (10_000, 3)] {
            process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![selected_stat("Queue0", selector, value)]),
            );
        }
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, Vec::new()),
        );
        assert_eq!(output[0].heatmaps[0].count, 3);
        assert_eq!(output[0].heatmaps[0].bucket_counts, vec![1, 1, 1, 0]);
    }

    #[test]
    fn late_generic_sample_does_not_change_baseline_and_equal_time_is_accepted() {
        let selector = CounterSelector::new(1, 2);
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(heatmap_config([selector], [(selector, vec![5, 10])])),
        );
        for (time, value) in [(2_000, 100), (1_000, 10), (2_000, 105), (3_000, 110)] {
            process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![stat("Ethernet0", value)]),
            );
        }
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, Vec::new()),
        );
        assert_eq!(output[0].heatmaps[0].count, 2);
        assert_eq!(output[0].heatmaps[0].sum, 10.0);
    }

    #[test]
    fn generic_baselines_are_independent_per_object() {
        let selector = CounterSelector::new(1, 2);
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(heatmap_config([selector], [(selector, vec![5, 10])])),
        );
        for (time, object, value) in [
            (1_000, "Ethernet0", 100),
            (2_000, "Ethernet4", 1_000),
            (3_000, "Ethernet0", 105),
            (4_000, "Ethernet4", 1_010),
        ] {
            process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![stat(object, value)]),
            );
        }
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, Vec::new()),
        );
        assert_eq!(output[0].heatmaps.len(), 2);
        assert!(output[0].heatmaps.iter().all(|heatmap| heatmap.count == 1));
        assert!(Arc::ptr_eq(
            &output[0].heatmaps[0].explicit_bounds,
            &output[0].heatmaps[1].explicit_bounds
        ));
        assert!(Arc::ptr_eq(
            &output[0].heatmaps[0].schema,
            &output[0].heatmaps[1].schema
        ));
    }

    #[test]
    fn custom_and_default_layouts_coexist_in_one_config() {
        let custom = CounterSelector::new(1, 2);
        let fallback = CounterSelector::new(1, 3);
        let mut config = heatmap_config([custom, fallback], [(custom, vec![1, 2, 8])]);
        config.heatmap_default_bucket_count = 4;
        let mut aggregator = Aggregator::default();
        aggregator.set_config("session".to_string(), Some(config));
        for (time, value) in [(1_000, 10), (2_000, 11)] {
            process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(
                    time,
                    vec![
                        selected_stat("Ethernet0", custom, value),
                        selected_stat("Ethernet0", fallback, value),
                    ],
                ),
            );
        }
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, Vec::new()),
        );
        let custom_heatmap = output[0]
            .heatmaps
            .iter()
            .find(|heatmap| heatmap.stat_id == custom.stat_id)
            .unwrap();
        let fallback_heatmap = output[0]
            .heatmaps
            .iter()
            .find(|heatmap| heatmap.stat_id == fallback.stat_id)
            .unwrap();
        assert_eq!(custom_heatmap.explicit_bounds.as_ref(), &[1.0, 2.0, 8.0]);
        assert_eq!(fallback_heatmap.explicit_bounds.len(), 3);
        assert_ne!(custom_heatmap.schema, fallback_heatmap.schema);
    }

    #[test]
    fn config_replacement_discards_window_and_resets_delta_baseline() {
        let selector = CounterSelector::new(1, 2);
        let first = heatmap_config([selector], [(selector, vec![5, 10])]);
        let second = heatmap_config([selector], [(selector, vec![5, 20])]);
        let mut aggregator = Aggregator::default();
        aggregator.set_config("session".to_string(), Some(first));
        for (time, value) in [(1_000, 100), (2_000, 105)] {
            process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![stat("Ethernet0", value)]),
            );
        }
        aggregator.set_config("session".to_string(), Some(second));
        for (time, value) in [(3_000, 200), (4_000, 207)] {
            process(
                &mut aggregator,
                Some(Arc::from("session")),
                sample(time, vec![stat("Ethernet0", value)]),
            );
        }
        let output = process(
            &mut aggregator,
            Some(Arc::from("session")),
            sample(10_001, Vec::new()),
        );
        assert_eq!(output[0].heatmaps[0].count, 1);
        assert_eq!(output[0].heatmaps[0].sum, 7.0);
        assert_eq!(output[0].heatmaps[0].explicit_bounds.as_ref(), &[5.0, 20.0]);
    }

    #[test]
    fn heatmap_supports_full_counter_range() {
        let mut heatmap = accumulator(vec![0], HeatmapValueKind::Delta, 0);
        heatmap.record(u64::MAX);
        let heatmap = heatmap.into_message(Arc::from("Ethernet0"), 1, 2, 0, 10_000);
        assert_eq!(heatmap.count, 2);
        assert_eq!(heatmap.min, 0);
        assert_eq!(heatmap.max, u64::MAX);
        assert_eq!(heatmap.bucket_counts.iter().sum::<u64>(), 2);
    }

    #[test]
    fn composes_rollover_reporting_and_heatmap_delta_in_order() {
        let mut aggregator = Aggregator::default();
        let selector = CounterSelector::new(1, 2);
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(100),
                rollover_counters: HashSet::from([selector]),
                heatmap_interval: Some(1_000),
                heatmap_counters: HashSet::from([selector]),
                heatmap_explicit_bounds: BTreeMap::from([(selector, vec![50, 100, 500])]),
                ..Default::default()
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
        assert_eq!(heatmap.count, 9);
        assert_eq!(heatmap.sum, 900.0);
        assert_eq!(heatmap.min, 100);
        assert_eq!(heatmap.max, 100);
        assert_eq!(heatmap.bucket_counts, vec![0, 9, 0, 0]);
        assert_eq!(heatmap.start_time_unix_nano, 0);
        assert_eq!(heatmap.time_unix_nano, 1_000_000);
    }

    #[test]
    fn preserves_unified_messages_that_already_contain_heatmaps() {
        let stats = sample(1_000, vec![stat("Ethernet0", 1)]);
        let heatmap = accumulator(vec![1, 2, 8], HeatmapValueKind::Delta, 1).into_message(
            Arc::from("Ethernet0"),
            1,
            2,
            0,
            10_000,
        );
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
