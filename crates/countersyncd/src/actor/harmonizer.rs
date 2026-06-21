use ahash::{HashMap, HashMapExt};
use log::warn;
use std::sync::Arc;

use crate::message::{
    harmonizer::HarmonizerConfig,
    saistats::{SAIStat, SAIStats, SAIStatsMessage},
};

const NANOS_PER_MICROSECOND: u64 = 1_000;

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
struct StatKey {
    object_name: String,
    type_id: u32,
    stat_id: u32,
}

impl From<&SAIStat> for StatKey {
    fn from(stat: &SAIStat) -> Self {
        Self {
            object_name: stat.object_name.clone(),
            type_id: stat.type_id,
            stat_id: stat.stat_id,
        }
    }
}

#[derive(Debug)]
struct ReportingWindow {
    window: u64,
    observation_time: u64,
    stats: Vec<SAIStat>,
    index: HashMap<StatKey, usize>,
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
            let key = StatKey::from(stat);
            if let Some(position) = self.index.get(&key).copied() {
                self.stats[position] = stat.clone();
            } else {
                let position = self.stats.len();
                self.stats.push(stat.clone());
                self.index.insert(key, position);
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
pub struct Harmonizer {
    reporting_rate: HashMap<String, ReportingRateState>,
}

impl Harmonizer {
    pub fn set_config(&mut self, key: String, config: Option<HarmonizerConfig>) {
        self.reporting_rate.remove(&key);

        match config {
            Some(config) => {
                if let Some(reporting_rate) = config.reporting_rate {
                    self.reporting_rate
                        .insert(key.clone(), ReportingRateState::new(reporting_rate));
                }
            }
            None => {}
        }
    }

    pub fn remove_config(&mut self, key: &str) {
        self.reporting_rate.remove(key);
    }

    pub fn process(&mut self, key: Option<&str>, sample: SAIStatsMessage) -> Vec<SAIStatsMessage> {
        let Some(key) = key else {
            return vec![sample];
        };

        let Some(state) = self.reporting_rate.get_mut(key) else {
            return vec![sample];
        };

        match state.push(sample.as_ref()) {
            Some(message) => vec![message],
            None => Vec::new(),
        }
    }
}

pub fn validate_reporting_rate(config: &Option<HarmonizerConfig>, key: &str) {
    if let Some(config) = config {
        if config.reporting_rate.is_none() {
            warn!(
                "Harmonizer config for session {} has no reporting_rate; forwarding samples unchanged",
                key
            );
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
        let mut harmonizer = Harmonizer::default();
        harmonizer.set_config("session".to_string(), None);

        let input = sample(10, vec![stat("Ethernet0", 1)]);
        let output = harmonizer.process(Some("session"), input.clone());

        assert_eq!(output, vec![input]);
    }

    #[test]
    fn aggregates_samples_until_next_reporting_window() {
        let mut harmonizer = Harmonizer::default();
        harmonizer.set_config(
            "session".to_string(),
            Some(HarmonizerConfig {
                reporting_rate: Some(10),
            }),
        );

        assert!(harmonizer
            .process(Some("session"), sample(1_000, vec![stat("Ethernet0", 1)]))
            .is_empty());
        assert!(harmonizer
            .process(Some("session"), sample(9_000, vec![stat("Ethernet0", 9)]))
            .is_empty());

        let output =
            harmonizer.process(Some("session"), sample(10_000, vec![stat("Ethernet0", 10)]));

        assert_eq!(output.len(), 1);
        assert_eq!(output[0].observation_time, 9_000);
        assert_eq!(output[0].stats, vec![stat("Ethernet0", 9)]);
    }

    #[test]
    fn keeps_latest_stat_per_object_type_and_counter() {
        let mut harmonizer = Harmonizer::default();
        harmonizer.set_config(
            "session".to_string(),
            Some(HarmonizerConfig {
                reporting_rate: Some(10),
            }),
        );

        harmonizer.process(Some("session"), sample(1_000, vec![stat("Ethernet0", 1)]));
        harmonizer.process(
            Some("session"),
            sample(2_000, vec![stat("Ethernet0", 2), stat("Ethernet4", 3)]),
        );

        let output =
            harmonizer.process(Some("session"), sample(11_000, vec![stat("Ethernet0", 11)]));

        assert_eq!(output.len(), 1);
        assert_eq!(
            output[0].stats,
            vec![stat("Ethernet0", 2), stat("Ethernet4", 3)]
        );
    }

    #[test]
    fn resets_state_when_config_changes() {
        let mut harmonizer = Harmonizer::default();
        harmonizer.set_config(
            "session".to_string(),
            Some(HarmonizerConfig {
                reporting_rate: Some(10),
            }),
        );
        assert!(harmonizer
            .process(Some("session"), sample(1_000, vec![stat("Ethernet0", 1)]))
            .is_empty());

        harmonizer.set_config(
            "session".to_string(),
            Some(HarmonizerConfig {
                reporting_rate: Some(20),
            }),
        );

        assert!(harmonizer
            .process(Some("session"), sample(11_000, vec![stat("Ethernet0", 11)]))
            .is_empty());
    }
}
