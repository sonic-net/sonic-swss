use ahash::{HashMap, HashMapExt};
use log::debug;
use std::collections::LinkedList;
use std::sync::Arc;
use tokio::{
    select,
    sync::mpsc::{Receiver, Sender},
};

use crate::message::{
    aggregator::{AggregatorConfig, AggregatorConfigMessage, AggregatorStatsMessage},
    saistats::{SAIStat, SAIStats, SAIStatsMessage},
};

const NANOS_PER_MICROSECOND: u64 = 1_000;

#[derive(Debug)]
struct ReportingWindow {
    window: u64,
    observation_time: u64,
    stats: Vec<SAIStat>,
    index: HashMap<String, HashMap<(u32, u32), usize>>,
}

impl ReportingWindow {
    fn new(window: u64, sample: &SAIStats) -> Self {
        let mut state = Self {
            window,
            observation_time: sample.observation_time,
            stats: Vec::with_capacity(sample.stats.len()),
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
                self.stats[position] = stat.clone();
            } else {
                let position = self.stats.len();
                self.stats.push(stat.clone());
                self.index
                    .entry(stat.object_name.clone())
                    .or_default()
                    .insert((stat.type_id, stat.stat_id), position);
            }
        }
    }

    fn into_message(self) -> SAIStatsMessage {
        Arc::new(SAIStats {
            observation_time: self.observation_time,
            stats: self.stats,
        })
    }
}

#[derive(Debug)]
struct ReportingRateState {
    interval_ns: u64,
    current: Option<ReportingWindow>,
}

impl ReportingRateState {
    fn new(reporting_rate_us: u32) -> Self {
        Self {
            interval_ns: u64::from(reporting_rate_us) * NANOS_PER_MICROSECOND,
            current: None,
        }
    }

    fn push(&mut self, sample: &SAIStats) -> Option<SAIStatsMessage> {
        if self.interval_ns == 0 {
            return None;
        }

        let window = sample.observation_time / self.interval_ns;
        // Reporting-rate harmonization is sample-driven: a later sample closes
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
            Some(_) => {
                let flushed = self.current.take().map(ReportingWindow::into_message);
                self.current = Some(ReportingWindow::new(window, sample));
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
        let reporting_rate = config.reporting_rate.map(ReportingRateState::new);

        Self {
            _config: config,
            reporting_rate,
        }
    }

    fn process(&mut self, sample: SAIStatsMessage) -> Vec<SAIStatsMessage> {
        let Some(reporting_rate) = self.reporting_rate.as_mut() else {
            return vec![sample];
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

    pub fn process(&mut self, key: Option<&str>, sample: SAIStatsMessage) -> Vec<SAIStatsMessage> {
        let Some(key) = key else {
            return vec![sample];
        };

        let Some(state) = self.sessions.get_mut(key) else {
            return vec![sample];
        };

        state.process(sample)
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
    saistats_recipients: LinkedList<Sender<SAIStatsMessage>>,
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
            saistats_recipients: LinkedList::new(),
            aggregator: Aggregator::default(),
        }
    }

    pub fn add_recipient(&mut self, recipient: Sender<SAIStatsMessage>) {
        self.saistats_recipients.push_back(recipient);
    }

    fn handle_config(&mut self, message: AggregatorConfigMessage) {
        if message.is_delete {
            self.aggregator.remove_config(&message.key);
            return;
        }

        validate_reporting_rate(&message.config, &message.key);
        self.aggregator.set_config(message.key, message.config);
    }

    fn handle_stats(&mut self, message: AggregatorStatsMessage) -> Vec<SAIStatsMessage> {
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
                            for recipient in &actor.saistats_recipients {
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
            }),
        );

        let input = sample(10, vec![stat("Ethernet0", 1)]);
        let output = aggregator.process(Some("session"), input.clone());

        assert_eq!(output, vec![input]);
    }

    #[test]
    fn aggregates_samples_until_next_reporting_window() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(10),
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
        assert_eq!(output[0].observation_time, 9_000);
        assert_eq!(output[0].stats, vec![stat("Ethernet0", 9)]);
    }

    #[test]
    fn keeps_latest_stat_per_object_type_and_counter() {
        let mut aggregator = Aggregator::default();
        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(10),
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
            output[0].stats,
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
            }),
        );
        assert!(aggregator
            .process(Some("session"), sample(1_000, vec![stat("Ethernet0", 1)]))
            .is_empty());

        aggregator.set_config(
            "session".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(20),
            }),
        );

        assert!(aggregator
            .process(Some("session"), sample(11_000, vec![stat("Ethernet0", 11)]))
            .is_empty());
    }
}
