//! OpenTelemetry Message Types
//!
//! This module defines data structures for converting SAI statistics
//! to OpenTelemetry gauge format for export to observability systems.

use std::sync::Arc;
use crate::message::saistats::{SAIStat, SAIStats};
use opentelemetry_proto::tonic::{
    common::v1::{KeyValue as ProtoKeyValue, AnyValue, any_value::Value},
    metrics::v1::{NumberDataPoint, number_data_point},
};

/// OpenTelemetry Gauge representation for SAI statistics
///
/// This struct represents an OpenTelemetry gauge metric following the OTLP protocol.
/// Each gauge contains data points with attributes, timestamps, and values derived
/// from SAI statistics.
#[derive(Debug, Clone, PartialEq)]
pub struct OtelGauge {
    /// Metric name (e.g., "sai_counter_type_100_stat_200")
    pub name: String,
    /// Description of the metric
    pub description: String,
    /// Unit of measurement (typically "1" for counters)
    pub unit: String,
    /// Data points for this gauge
    pub data_points: Vec<OtelDataPoint>,
}

/// OpenTelemetry Data Point for a single measurement
///
/// Represents a single measurement point in time for a gauge metric,
/// converted from a SAI statistic entry.
#[derive(Debug, Clone, PartialEq)]
pub struct OtelDataPoint {
    /// Attributes (labels) for this data point
    pub attributes: Vec<OtelAttribute>,
    /// Timestamp in nanoseconds since Unix epoch
    pub time_unix_nano: u64,
    /// The gauge value (converted from SAI counter)
    pub value: i64,
}

/// OpenTelemetry Attribute (Key-Value Pair)
///
/// Represents a single attribute/label attached to a metric data point.
#[derive(Debug, Clone, PartialEq)]
pub struct OtelAttribute {
    /// Attribute key
    pub key: String,
    /// Attribute value
    pub value: String,
}

impl OtelAttribute {
    /// Creates a new OtelAttribute
    pub fn new(key: impl Into<String>, value: impl Into<String>) -> Self {
        Self {
            key: key.into(),
            value: value.into(),
        }
    }

    /// Converts to OpenTelemetry protobuf KeyValue
    pub fn to_proto(&self) -> ProtoKeyValue {
        ProtoKeyValue {
            key: self.key.clone(),
            value: Some(AnyValue {
                value: Some(Value::StringValue(self.value.clone())),
            }),
        }
    }
}

impl OtelDataPoint {
    /// Creates a new OtelDataPoint from SAI statistic
    pub fn from_sai_stat(sai_stat: &SAIStat, observation_time_nano: u64) -> Self {
        let attributes = vec![
            OtelAttribute::new("object_name", &sai_stat.object_name),
            OtelAttribute::new("sai_type_id", sai_stat.type_id.to_string()),
            OtelAttribute::new("sai_stat_id", sai_stat.stat_id.to_string()),
        ];

        Self {
            attributes,
            time_unix_nano: observation_time_nano,
            value: sai_stat.counter as i64,
        }
    }

    /// Converts to OpenTelemetry protobuf NumberDataPoint
    pub fn to_proto(&self) -> NumberDataPoint {
        NumberDataPoint {
            time_unix_nano: self.time_unix_nano,
            value: Some(number_data_point::Value::AsInt(self.value)),
            attributes: self.attributes.iter().map(|attr| attr.to_proto()).collect(),
            ..Default::default()
        }
    }
}

impl OtelGauge {
    /// Creates a new OtelGauge from SAI statistic
    pub fn from_sai_stat(sai_stat: &SAIStat, observation_time_nano: u64) -> Self {
        let name = format!("sai_counter_type_{}_stat_{}", sai_stat.type_id, sai_stat.stat_id);
        let description = format!(
            "SAI counter for object {} (type:{}, stat:{})",
            sai_stat.object_name, sai_stat.type_id, sai_stat.stat_id
        );

        let data_point = OtelDataPoint::from_sai_stat(sai_stat, observation_time_nano);

        Self {
            name,
            description,
            unit: "1".to_string(),
            data_points: vec![data_point],
        }
    }

    /// Creates multiple OtelGauges from SAI statistics collection
    pub fn from_sai_stats(sai_stats: &SAIStats) -> Vec<Self> {
        // 1970-01-01 00:00:00 UTC 
        let observation_time_nano = 0u64;

        sai_stats.stats
            .iter()
            .map(|stat| Self::from_sai_stat(stat, observation_time_nano))
            .collect()
    }
}

/// Collection of OpenTelemetry gauges with metadata
///
/// This structure represents a collection of OpenTelemetry gauges
/// derived from SAI statistics, ready for export to collectors.
#[derive(Debug, Clone)]
pub struct OtelMetrics {
    /// Service name for resource attribution
    pub service_name: String,
    /// Instrumentation scope name
    pub scope_name: String,
    /// Instrumentation scope version
    pub scope_version: String,
    /// Collection of gauge metrics
    pub gauges: Vec<OtelGauge>,
}

impl OtelMetrics {
    /// Creates OtelMetrics from SAI statistics
    pub fn from_sai_stats(sai_stats: &SAIStats) -> Self {
        let gauges = OtelGauge::from_sai_stats(sai_stats);

        Self {
            service_name: "countersyncd".to_string(),
            scope_name: "countersyncd".to_string(),
            scope_version: "1.0".to_string(),
            gauges,
        }
    }

    /// Returns the number of gauges in this collection
    pub fn len(&self) -> usize {
        self.gauges.len()
    }

    /// Returns true if this collection is empty
    pub fn is_empty(&self) -> bool {
        self.gauges.is_empty()
    }
}

/// Type alias for Arc-wrapped OtelMetrics for efficient sharing
pub type OtelMetricsMessage = Arc<OtelMetrics>;

/// Extension trait for creating OtelMetricsMessage instances
pub trait OtelMetricsMessageExt {
    /// Converts OtelMetrics to Arc-wrapped message
    fn into_message(self) -> OtelMetricsMessage;

    /// Creates OtelMetricsMessage from SAI statistics
    fn from_sai_stats(sai_stats: &SAIStats) -> OtelMetricsMessage;
}

impl OtelMetricsMessageExt for OtelMetrics {
    fn into_message(self) -> OtelMetricsMessage {
        Arc::new(self)
    }

    fn from_sai_stats(sai_stats: &SAIStats) -> OtelMetricsMessage {
        Arc::new(OtelMetrics::from_sai_stats(sai_stats))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::saistats::{SAIStat, SAIStats, SAIStatsMessageExt};

    /// Helper function to create test SAI statistics (similar to saistats.rs pattern)
    fn create_test_sai_stats(observation_time: u64, stat_count: usize) -> SAIStats {
        let stats = (0..stat_count)
            .map(|i| SAIStat {
                object_name: format!("Ethernet{}", i),
                type_id: (i * 100 + 1) as u32,
                stat_id: (i * 10 + 1) as u32,
                counter: (i * 1000 + 500) as u64,
            })
            .collect();

        SAIStats::new(observation_time, stats)
    }

    #[test]
    fn test_otel_attribute_creation() {
        let attr = OtelAttribute::new("object_name", "Ethernet0");
        assert_eq!(attr.key, "object_name");
        assert_eq!(attr.value, "Ethernet0");

        let attr2 = OtelAttribute::new("sai_type_id", "100");
        assert_eq!(attr2.key, "sai_type_id");
        assert_eq!(attr2.value, "100");
    }

    #[test]
    fn test_otel_data_point_from_sai_stat() {
        let sai_stat = SAIStat {
            object_name: "Ethernet0".to_string(),
            type_id: 100,
            stat_id: 200,
            counter: 1500,
        };

        let observation_time_nano = 0u64; // 1970-01-01 00:00:00 UTC
        let data_point = OtelDataPoint::from_sai_stat(&sai_stat, observation_time_nano);

        assert_eq!(data_point.time_unix_nano, observation_time_nano);
        assert_eq!(data_point.value, 1500);
        assert_eq!(data_point.attributes.len(), 3);

        // Check attributes
        let object_name_attr = data_point.attributes.iter()
            .find(|attr| attr.key == "object_name").unwrap();
        assert_eq!(object_name_attr.value, "Ethernet0");

        let type_id_attr = data_point.attributes.iter()
            .find(|attr| attr.key == "sai_type_id").unwrap();
        assert_eq!(type_id_attr.value, "100");

        let stat_id_attr = data_point.attributes.iter()
            .find(|attr| attr.key == "sai_stat_id").unwrap();
        assert_eq!(stat_id_attr.value, "200");
    }

    #[test]
    fn test_otel_gauge_from_sai_stat() {
        let sai_stat = SAIStat {
            object_name: "BufferPool1".to_string(),
            type_id: 24,
            stat_id: 2,
            counter: 5000,
        };

        let observation_time_nano = 0u64; // 1970-01-01 00:00:00 UTC
        let gauge = OtelGauge::from_sai_stat(&sai_stat, observation_time_nano);

        assert_eq!(gauge.name, "sai_counter_type_24_stat_2");
        assert_eq!(gauge.description, "SAI counter for object BufferPool1 (type:24, stat:2)");
        assert_eq!(gauge.unit, "1");
        assert_eq!(gauge.data_points.len(), 1);

        let data_point = &gauge.data_points[0];
        assert_eq!(data_point.value, 5000);
        assert_eq!(data_point.time_unix_nano, observation_time_nano);
    }

    #[test]
    fn test_otel_gauge_from_sai_stats_collection() {
        let sai_stats = create_test_sai_stats(1672531200, 3);
        let gauges = OtelGauge::from_sai_stats(&sai_stats);

        assert_eq!(gauges.len(), 3);

        // Check first gauge
        let first_gauge = &gauges[0];
        assert_eq!(first_gauge.name, "sai_counter_type_1_stat_1");
        assert!(first_gauge.description.contains("Ethernet0"));
        assert_eq!(first_gauge.data_points[0].value, 500);

        // All should have same timestamp 
        let expected_time_nano = 0u64; 
        for gauge in &gauges {
            assert_eq!(gauge.data_points[0].time_unix_nano, expected_time_nano);
        }
    }

    #[test]
    fn test_otel_metrics_from_sai_stats() {
        let sai_stats = SAIStats::new(
            1234567890,
            vec![
                SAIStat {
                    object_name: "Ethernet0".to_string(),
                    type_id: 1,
                    stat_id: 1,
                    counter: 12345,
                },
                SAIStat {
                    object_name: "BufferPool1".to_string(),
                    type_id: 24,
                    stat_id: 2,
                    counter: 67890,
                },
            ],
        );

        let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);

        assert_eq!(otel_metrics.service_name, "countersyncd");
        assert_eq!(otel_metrics.scope_name, "countersyncd");
        assert_eq!(otel_metrics.scope_version, "1.0");
        assert_eq!(otel_metrics.len(), 2);
        assert!(!otel_metrics.is_empty());

        // Check individual gauges
        let port_gauge = otel_metrics.gauges.iter()
            .find(|g| g.name == "sai_counter_type_1_stat_1").unwrap();
        assert_eq!(port_gauge.data_points[0].value, 12345);

        let buffer_gauge = otel_metrics.gauges.iter()
            .find(|g| g.name == "sai_counter_type_24_stat_2").unwrap();
        assert_eq!(buffer_gauge.data_points[0].value, 67890);
    }

    #[test]
    fn test_otel_metrics_message_creation() {
        let sai_stats = create_test_sai_stats(555555, 2);

        // Test using into_message()
        let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);
        let message1 = otel_metrics.into_message();

        // Test using from_sai_stats()
        let message2 = OtelMetrics::from_sai_stats(&sai_stats);

        assert_eq!(message1.service_name, message2.service_name);
        assert_eq!(message1.len(), message2.len());
        assert_eq!(message1.gauges.len(), 2);
    }

    #[test]
    fn test_otel_data_point_proto_conversion() {
        let sai_stat = SAIStat {
            object_name: "TestInterface".to_string(),
            type_id: 999,
            stat_id: 888,
            counter: 777,
        };

        let data_point = OtelDataPoint::from_sai_stat(&sai_stat, 123456789);
        let proto_point = data_point.to_proto();

        assert_eq!(proto_point.time_unix_nano, 123456789);
        match proto_point.value.unwrap() {
            number_data_point::Value::AsInt(val) => assert_eq!(val, 777),
            _ => panic!("Expected integer value"),
        }
        assert_eq!(proto_point.attributes.len(), 3);

        // Check one attribute conversion
        let object_attr = &proto_point.attributes[0];
        assert_eq!(object_attr.key, "object_name");
        if let Some(AnyValue { value: Some(Value::StringValue(val)) }) = &object_attr.value {
            assert_eq!(val, "TestInterface");
        } else {
            panic!("Expected string value");
        }
    }

#[test]
fn test_realistic_sai_to_otel_conversion() {
    // Create realistic SAI data similar to what would come from IPFIX
    let realistic_stats = vec![
        SAIStat { object_name: "Ethernet0".to_string(), type_id: 1, stat_id: 1, counter: 1000000 },
        SAIStat { object_name: "Ethernet0".to_string(), type_id: 1, stat_id: 2, counter: 2000000 },
        SAIStat { object_name: "Ethernet1".to_string(), type_id: 1, stat_id: 1, counter: 1500000 },
        SAIStat { object_name: "BufferPool_ingress_lossless_pool".to_string(), type_id: 24, stat_id: 1, counter: 500000 },
    ];

    let sai_stats = SAIStats::new(1672531200, realistic_stats);
    let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);

    // Print the conversion for debugging
    println!(" [Test] SAI to OpenTelemetry Conversion Debug Output");
    println!("   Input: {} SAI statistics", sai_stats.stats.len());
    println!("   Output: {} OpenTelemetry gauges", otel_metrics.len());
    println!();

    // Print all converted gauges
    for (index, gauge) in otel_metrics.gauges.iter().enumerate() {
        let data_point = &gauge.data_points[0];
        println!("   [{}] Gauge: {}", index + 1, gauge.name);
        println!("       Value: {}", data_point.value);
        println!("       Unit: {}", gauge.unit);
        println!("       Time: {}ns", data_point.time_unix_nano);
        println!("       Description: {}", gauge.description);

        if !data_point.attributes.is_empty() {
            println!("       Attributes:");
            for attr in &data_point.attributes {
                println!("         - {}={}", attr.key, attr.value);
            }
        }
        println!("       Raw Debug: {:#?}", gauge);
        println!();
    }

    assert_eq!(otel_metrics.len(), 4);

    // Verify port stats conversion
    let port_stats: Vec<_> = otel_metrics.gauges.iter()
        .filter(|g| g.description.contains("Ethernet"))
        .collect();
    assert_eq!(port_stats.len(), 3);

    // Verify buffer pool stats conversion
    let buffer_stats: Vec<_> = otel_metrics.gauges.iter()
        .filter(|g| g.description.contains("BufferPool"))
        .collect();
    assert_eq!(buffer_stats.len(), 1);

    // Check that all metrics have proper timestamps 
    let expected_time = 0u64; 
    for gauge in &otel_metrics.gauges {
        assert_eq!(gauge.data_points[0].time_unix_nano, expected_time);
    }

    // Verify metric naming
    let port_rx_metric = otel_metrics.gauges.iter()
        .find(|g| g.name == "sai_counter_type_1_stat_1").unwrap();
    assert!(port_rx_metric.description.contains("type:1, stat:1"));
}

    #[test]
    fn test_empty_sai_stats_to_otel() {
        let empty_stats = SAIStats::new(1111111111, vec![]);
        let otel_metrics = OtelMetrics::from_sai_stats(&empty_stats);

        assert_eq!(otel_metrics.len(), 0);
        assert!(otel_metrics.is_empty());
        assert_eq!(otel_metrics.service_name, "countersyncd");
    }
}
