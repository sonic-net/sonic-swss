use std::{collections::HashSet, sync::Arc};

use super::saistats::SAIStatsMessage;
use crate::sai::{
    SaiBufferPoolStat, SaiIngressPriorityGroupStat, SaiObjectType, SaiPortStat, SaiQueueStat,
};

pub const MAX_EXACT_OTLP_BOUNDARY: u64 = 1 << 53;

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
        let serialized = serialized.trim();
        if serialized.is_empty() {
            return Ok(HashSet::new());
        }

        serialized.split(',').map(Self::parse).collect()
    }

    fn parse(item: &str) -> Result<Self, String> {
        let item = item.trim();
        let (group, counter) = item.split_once('|').ok_or_else(|| {
            format!(
                "Invalid counter selector '{}'; expected GROUP|COUNTER",
                item
            )
        })?;
        let group = group.trim();
        let counter = counter.trim();
        if group.is_empty() || counter.is_empty() {
            return Err(format!(
                "Invalid counter selector '{}'; expected GROUP|COUNTER",
                item
            ));
        }
        if counter.contains('|') {
            return Err(format!(
                "Invalid counter selector '{}'; expected GROUP|COUNTER",
                item
            ));
        }

        let selector = match group {
            "PORT" => Self::new(
                SaiObjectType::Port.to_u32(),
                format!("SAI_PORT_STAT_{}", counter)
                    .parse::<SaiPortStat>()
                    .map_err(|_| format!("Invalid PORT counter '{}'", counter))?
                    .to_u32(),
            ),
            "QUEUE" => Self::new(
                SaiObjectType::Queue.to_u32(),
                format!("SAI_QUEUE_STAT_{}", counter)
                    .parse::<SaiQueueStat>()
                    .map_err(|_| format!("Invalid QUEUE counter '{}'", counter))?
                    .to_u32(),
            ),
            "BUFFER_POOL" => Self::new(
                SaiObjectType::BufferPool.to_u32(),
                format!("SAI_BUFFER_POOL_STAT_{}", counter)
                    .parse::<SaiBufferPoolStat>()
                    .map_err(|e| format!("Invalid BUFFER_POOL counter: {}", e))?
                    .to_u32(),
            ),
            "INGRESS_PRIORITY_GROUP" => Self::new(
                SaiObjectType::IngressPriorityGroup.to_u32(),
                format!("SAI_INGRESS_PRIORITY_GROUP_STAT_{}", counter)
                    .parse::<SaiIngressPriorityGroupStat>()
                    .map_err(|_| format!("Invalid INGRESS_PRIORITY_GROUP counter '{}'", counter))?
                    .to_u32(),
            ),
            _ => return Err(format!("Invalid counter selector group '{}'", group)),
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
            return Err(format!("Invalid {} counter '{}'", group, counter));
        }

        Ok(selector)
    }
}

/// CounterSyncd-side subset of HIGH_FREQUENCY_TELEMETRY_AGGREGATOR.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct AggregatorConfig {
    /// Reporting interval in microseconds.
    pub reporting_rate: Option<u32>,
    /// Counters corrected when a newly reported raw value is lower than the previous value.
    pub rollover_counters: HashSet<CounterSelector>,
    /// Heatmap aggregation interval in microseconds.
    pub heatmap_interval: Option<u32>,
    /// Counters summarized as heatmaps after optional reporting-rate aggregation.
    pub heatmap_counters: HashSet<CounterSelector>,
    /// Inclusive upper bounds shared by all heatmap counters.
    pub heatmap_bucket_boundaries: Vec<u64>,
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
    pub fn validate(&self) -> Result<(), String> {
        if self.reporting_rate == Some(0) {
            return Err("reporting_rate must be greater than zero".to_string());
        }
        if self.heatmap_interval == Some(0) {
            return Err("heatmap_interval must be greater than zero".to_string());
        }

        let heatmap_configured = self.heatmap_interval.is_some();
        if heatmap_configured != !self.heatmap_counters.is_empty()
            || heatmap_configured != !self.heatmap_bucket_boundaries.is_empty()
        {
            return Err(
                "heatmap_interval, heatmap_counters, and heatmap_bucket_boundaries must be configured together"
                    .to_string(),
            );
        }

        for boundaries in self.heatmap_bucket_boundaries.windows(2) {
            if boundaries[0] >= boundaries[1] {
                return Err("heatmap_bucket_boundaries must be strictly increasing".to_string());
            }
        }
        if self
            .heatmap_bucket_boundaries
            .iter()
            .any(|boundary| *boundary > MAX_EXACT_OTLP_BOUNDARY)
        {
            return Err(format!(
                "heatmap_bucket_boundaries must not exceed {}",
                MAX_EXACT_OTLP_BOUNDARY
            ));
        }

        Ok(())
    }

    pub fn parse_bucket_boundaries(serialized: &str) -> Result<Vec<u64>, String> {
        let serialized = serialized.trim();
        if serialized.is_empty() {
            return Ok(Vec::new());
        }

        serialized
            .split(',')
            .map(|item| {
                let item = item.trim();
                if item.is_empty() {
                    return Err(
                        "heatmap bucket boundaries must not contain empty entries".to_string()
                    );
                }
                item.parse::<u64>()
                    .map_err(|_| format!("Invalid heatmap bucket boundary '{}'", item))
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
        assert!(CounterSelector::parse_list("PORT|IF_IN_OCTETS,,QUEUE|PACKETS").is_err());
    }

    #[test]
    fn parses_and_validates_heatmap_bucket_boundaries() {
        assert_eq!(
            AggregatorConfig::parse_bucket_boundaries("0, 1024,4096").unwrap(),
            vec![0, 1024, 4096]
        );
        assert!(AggregatorConfig::parse_bucket_boundaries("0,invalid").is_err());
        assert!(AggregatorConfig::parse_bucket_boundaries("0,,1024").is_err());

        let valid = AggregatorConfig {
            heatmap_interval: Some(1_000),
            heatmap_counters: HashSet::from([CounterSelector::new(1, 2)]),
            heatmap_bucket_boundaries: vec![0, 1024, 4096],
            ..Default::default()
        };
        assert!(valid.validate().is_ok());

        let mut missing_boundaries = valid.clone();
        missing_boundaries.heatmap_bucket_boundaries.clear();
        assert!(missing_boundaries.validate().is_err());

        let mut missing_interval = valid.clone();
        missing_interval.heatmap_interval = None;
        assert!(missing_interval.validate().is_err());

        let mut zero_interval = valid.clone();
        zero_interval.heatmap_interval = Some(0);
        assert!(zero_interval.validate().is_err());

        let mut unordered = valid;
        unordered.heatmap_bucket_boundaries = vec![0, 4096, 1024];
        assert!(unordered.validate().is_err());

        let too_large = AggregatorConfig {
            heatmap_interval: Some(1_000),
            heatmap_counters: HashSet::from([CounterSelector::new(1, 2)]),
            heatmap_bucket_boundaries: vec![MAX_EXACT_OTLP_BOUNDARY + 1],
            ..Default::default()
        };
        assert!(too_large.validate().is_err());
    }
}
