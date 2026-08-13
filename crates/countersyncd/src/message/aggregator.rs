use std::{collections::HashSet, sync::Arc};

use super::saistats::SAIStatsMessage;
use crate::sai::{
    SaiBufferPoolStat, SaiIngressPriorityGroupStat, SaiObjectType, SaiPortStat, SaiQueueStat,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct CounterSelector {
    pub type_id: u32,
    pub stat_id: u32,
}

impl CounterSelector {
    pub fn new(type_id: u32, stat_id: u32) -> Self {
        Self { type_id, stat_id }
    }

    pub fn parse_list(serialized: &str) -> Result<HashSet<Self>, String> {
        serialized
            .split(',')
            .map(str::trim)
            .filter(|item| !item.is_empty())
            .map(Self::parse)
            .collect()
    }

    fn parse(item: &str) -> Result<Self, String> {
        let (group, counter) = item
            .split_once('|')
            .ok_or_else(|| format!("Invalid heatmap counter '{}'; expected GROUP|COUNTER", item))?;
        if counter.contains('|') {
            return Err(format!(
                "Invalid heatmap counter '{}'; expected GROUP|COUNTER",
                item
            ));
        }

        let selector = match group {
            "PORT" => Self::new(
                SaiObjectType::Port.to_u32(),
                format!("SAI_PORT_STAT_{}", counter)
                    .parse::<SaiPortStat>()
                    .map_err(|_| format!("Invalid PORT heatmap counter '{}'", counter))?
                    .to_u32(),
            ),
            "QUEUE" => Self::new(
                SaiObjectType::Queue.to_u32(),
                format!("SAI_QUEUE_STAT_{}", counter)
                    .parse::<SaiQueueStat>()
                    .map_err(|_| format!("Invalid QUEUE heatmap counter '{}'", counter))?
                    .to_u32(),
            ),
            "BUFFER_POOL" => Self::new(
                SaiObjectType::BufferPool.to_u32(),
                format!("SAI_BUFFER_POOL_STAT_{}", counter)
                    .parse::<SaiBufferPoolStat>()
                    .map_err(|e| format!("Invalid BUFFER_POOL heatmap counter: {}", e))?
                    .to_u32(),
            ),
            "INGRESS_PRIORITY_GROUP" => Self::new(
                SaiObjectType::IngressPriorityGroup.to_u32(),
                format!("SAI_INGRESS_PRIORITY_GROUP_STAT_{}", counter)
                    .parse::<SaiIngressPriorityGroupStat>()
                    .map_err(|_| {
                        format!(
                            "Invalid INGRESS_PRIORITY_GROUP heatmap counter '{}'",
                            counter
                        )
                    })?
                    .to_u32(),
            ),
            _ => return Err(format!("Invalid heatmap counter group '{}'", group)),
        };

        let is_marker = match group {
            "PORT" => counter == "START" || selector.stat_id == SaiPortStat::End.to_u32(),
            "QUEUE" => selector.stat_id == SaiQueueStat::CustomRangeBase.to_u32(),
            "BUFFER_POOL" => selector.stat_id == SaiBufferPoolStat::CustomRangeBase.to_u32(),
            "INGRESS_PRIORITY_GROUP" => {
                selector.stat_id == SaiIngressPriorityGroupStat::CustomRangeBase.to_u32()
            }
            _ => false,
        };
        if is_marker {
            return Err(format!("Invalid {} heatmap counter '{}'", group, counter));
        }

        Ok(selector)
    }
}

/// CounterSyncd-side subset of HIGH_FREQUENCY_TELEMETRY_AGGREGATOR.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct AggregatorConfig {
    /// Reporting interval in microseconds.
    pub reporting_rate: Option<u32>,
    /// Counters summarized as heatmaps within each reporting interval.
    pub heatmap_counters: HashSet<CounterSelector>,
}

#[derive(Debug, Clone)]
pub struct AggregatorConfigMessage {
    pub key: String,
    pub config: Option<AggregatorConfig>,
    pub is_delete: bool,
}

impl AggregatorConfigMessage {
    pub fn new(key: String, config: Option<AggregatorConfig>) -> Self {
        Self {
            key,
            config,
            is_delete: false,
        }
    }

    pub fn delete(key: String) -> Self {
        Self {
            key,
            config: None,
            is_delete: true,
        }
    }
}

#[derive(Debug, Clone)]
pub struct StatsMessage {
    pub key: Option<Arc<str>>,
    pub stats: SAIStatsMessage,
    pub heatmaps: Arc<[Heatmap]>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Heatmap {
    pub object_name: String,
    pub type_id: u32,
    pub stat_id: u32,
    pub start_time_unix_nano: u64,
    pub time_unix_nano: u64,
    pub count: u64,
    pub sum: f64,
    pub min: u64,
    pub max: u64,
    pub explicit_bounds: Vec<f64>,
    pub bucket_counts: Vec<u64>,
}

/// Common message format for every actor downstream of the aggregator.
pub type AggregatorStatsMessage = StatsMessage;
pub type AggregatedStatsMessage = StatsMessage;

impl StatsMessage {
    pub fn new(key: Option<Arc<str>>, stats: SAIStatsMessage) -> Self {
        Self {
            key,
            stats,
            heatmaps: Arc::from([]),
        }
    }

    pub fn with_heatmaps(
        key: Option<Arc<str>>,
        stats: SAIStatsMessage,
        heatmaps: Vec<Heatmap>,
    ) -> Self {
        Self {
            key,
            stats,
            heatmaps: heatmaps.into(),
        }
    }
}

impl From<SAIStatsMessage> for StatsMessage {
    fn from(stats: SAIStatsMessage) -> Self {
        Self::new(None, stats)
    }
}

impl AggregatorConfig {
    pub fn parse(serialized: &str) -> Option<Self> {
        let trimmed = serialized.trim();
        if trimmed.is_empty() {
            return None;
        }

        let reporting_rate = if let Ok(value) = trimmed.parse::<u32>() {
            Some(value)
        } else {
            parse_named_u32(trimmed, "reporting_rate")
        };

        match reporting_rate {
            Some(0) => Some(Self {
                reporting_rate: None,
                ..Default::default()
            }),
            Some(value) => Some(Self {
                reporting_rate: Some(value),
                ..Default::default()
            }),
            None => Some(Self {
                reporting_rate: None,
                ..Default::default()
            }),
        }
    }
}

fn parse_named_u32(input: &str, name: &str) -> Option<u32> {
    let start = input.find(name)? + name.len();
    let value_start = input[start..].char_indices().find_map(|(offset, ch)| {
        if ch.is_ascii_digit() {
            Some(start + offset)
        } else if ch.is_ascii_whitespace()
            || matches!(ch, '=' | ':' | ',' | ';' | '{' | '}' | '"' | '\'')
        {
            None
        } else {
            Some(input.len())
        }
    })?;

    if value_start >= input.len() {
        return None;
    }

    let value_end = input[value_start..]
        .char_indices()
        .find_map(|(offset, ch)| (!ch.is_ascii_digit()).then_some(value_start + offset))
        .unwrap_or(input.len());

    input[value_start..value_end].parse::<u32>().ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_reporting_rate_from_supported_formats() {
        assert_eq!(
            AggregatorConfig::parse("100").unwrap().reporting_rate,
            Some(100)
        );
        assert_eq!(
            AggregatorConfig::parse("reporting_rate=200")
                .unwrap()
                .reporting_rate,
            Some(200)
        );
        assert_eq!(
            AggregatorConfig::parse("{\"reporting_rate\":300}")
                .unwrap()
                .reporting_rate,
            Some(300)
        );
        assert_eq!(
            AggregatorConfig::parse("rollover_counters=PORT|A;reporting_rate:400")
                .unwrap()
                .reporting_rate,
            Some(400)
        );
    }

    #[test]
    fn parse_preserves_missing_or_zero_reporting_rate() {
        assert_eq!(AggregatorConfig::parse(""), None);
        assert_eq!(
            AggregatorConfig::parse("rollover_counters=PORT|A")
                .unwrap()
                .reporting_rate,
            None
        );
        assert_eq!(
            AggregatorConfig::parse("reporting_rate=0")
                .unwrap()
                .reporting_rate,
            None
        );
    }

    #[test]
    fn parses_heatmap_counter_selectors() {
        let selectors = CounterSelector::parse_list(
            "PORT|IF_IN_UCAST_PKTS,QUEUE|WATERMARK_BYTES,BUFFER_POOL|CURR_OCCUPANCY_BYTES,INGRESS_PRIORITY_GROUP|PACKETS",
        )
        .unwrap();

        assert!(selectors.contains(&CounterSelector::new(1, 1)));
        assert!(selectors.contains(&CounterSelector::new(21, 25)));
        assert!(selectors.contains(&CounterSelector::new(24, 0)));
        assert!(selectors.contains(&CounterSelector::new(26, 0)));
    }

    #[test]
    fn rejects_invalid_heatmap_counter_selectors() {
        assert!(CounterSelector::parse_list("QUEUE|IF_IN_UCAST_PKTS").is_err());
        assert!(CounterSelector::parse_list("PORT").is_err());
        assert!(CounterSelector::parse_list("UNKNOWN|PACKETS").is_err());
        assert!(CounterSelector::parse_list("PORT|END").is_err());
        assert!(CounterSelector::parse_list("PORT|START").is_err());
        assert!(CounterSelector::parse_list("QUEUE|CUSTOM_RANGE_BASE").is_err());
        assert!(CounterSelector::parse_list("BUFFER_POOL|CUSTOM_RANGE_BASE").is_err());
        assert!(CounterSelector::parse_list("INGRESS_PRIORITY_GROUP|CUSTOM_RANGE_BASE").is_err());
    }
}
