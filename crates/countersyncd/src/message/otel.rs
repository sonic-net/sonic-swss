//! OpenTelemetry Message Types
//!
//! This module defines data structures for converting SAI statistics
//! to OpenTelemetry gauge format for export to observability systems.

use std::borrow::Cow;
use std::collections::HashMap;

use crate::message::saistats::{SAIStat, SAIStats};
use crate::sai::{
    SaiBufferPoolStat, SaiIngressPriorityGroupStat, SaiObjectType, SaiPortStat, SaiQueueStat,
};
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
    /// Metric name (e.g., "SAI_PORT_STAT_IF_IN_UCAST_PKTS")
    pub name: Cow<'static, str>,
    /// Description of the metric
    pub description: Cow<'static, str>,
    /// Unit of measurement (typically "1" for counters)
    pub unit: Cow<'static, str>,
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
    pub key: Cow<'static, str>,
    /// Attribute value
    pub value: Cow<'static, str>,
}

impl OtelAttribute {
    /// Creates a new OtelAttribute
    pub fn new(key: impl Into<Cow<'static, str>>, value: impl Into<Cow<'static, str>>) -> Self {
        Self {
            key: key.into(),
            value: value.into(),
        }
    }

    /// Converts to OpenTelemetry protobuf KeyValue
    pub fn to_proto(&self) -> ProtoKeyValue {
        ProtoKeyValue {
            key: self.key.to_string(),
            value: Some(AnyValue {
                value: Some(Value::StringValue(self.value.to_string())),
            }),
        }
    }
}

/// Returns the readable SAI object-type name for a `type_id`
/// (e.g. `1` -> `"SAI_OBJECT_TYPE_PORT"`). Unknown ids fall back to a
/// synthetic name so no information is lost.
fn sai_type_name(type_id: u32) -> Cow<'static, str> {
    match SaiObjectType::from_u32(type_id) {
        Some(object_type) => Cow::Borrowed(object_type.to_c_name()),
        None => Cow::Owned(format!("SAI_OBJECT_TYPE_UNKNOWN_{}", type_id)),
    }
}

/// Returns the readable SAI stat name for a `(type_id, stat_id)` pair
/// (e.g. `(1, 1)` -> `"SAI_PORT_STAT_IF_IN_UCAST_PKTS"`), dispatching on the
/// object type. Unknown ids fall back to a synthetic name.
fn sai_stat_name(type_id: u32, stat_id: u32) -> Cow<'static, str> {
    let name = SaiObjectType::from_u32(type_id).and_then(|object_type| match object_type {
        SaiObjectType::Port => SaiPortStat::from_u32(stat_id).map(|s| s.to_c_name()),
        SaiObjectType::Queue => SaiQueueStat::from_u32(stat_id).map(|s| s.to_c_name()),
        SaiObjectType::BufferPool => SaiBufferPoolStat::from_u32(stat_id).map(|s| s.to_c_name()),
        SaiObjectType::IngressPriorityGroup => {
            SaiIngressPriorityGroupStat::from_u32(stat_id).map(|s| s.to_c_name())
        }
        _ => None,
    });

    match name {
        Some(c_name) => Cow::Borrowed(c_name),
        None => Cow::Owned(format!("SAI_STAT_UNKNOWN_TYPE_{}_STAT_{}", type_id, stat_id)),
    }
}

impl OtelDataPoint {
    /// Creates a new OtelDataPoint from SAI statistic
    pub fn from_sai_stat(sai_stat: &SAIStat, observation_time_nano: u64) -> Self {
        let attributes = vec![
            OtelAttribute::new("object_name", sai_stat.object_name.clone()),
            OtelAttribute::new("sai_type_name", sai_type_name(sai_stat.type_id)),
            OtelAttribute::new("sai_stat_name", sai_stat_name(sai_stat.type_id, sai_stat.stat_id)),
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
    /// Creates an empty gauge (metadata only, no data points yet) for a (type_id, stat_id) key.
    fn empty_for(type_id: u32, stat_id: u32) -> Self {
        let type_name = sai_type_name(type_id);
        let stat_name = sai_stat_name(type_id, stat_id);
        Self {
            description: Cow::Owned(format!("{} / {}", type_name, stat_name)),
            name: stat_name,
            unit: Cow::Borrowed("1"),
            data_points: Vec::new(),
        }
    }

    /// Creates OtelGauges from a SAI statistics collection, merging all stats
    /// that share the same (type_id, stat_id) into a single gauge with one data
    /// point per object. 
    pub fn from_sai_stats(sai_stats: &SAIStats) -> Vec<Self> {
        let observation_time_nano = sai_stats.observation_time;

        let mut index: HashMap<(u32, u32), usize> = HashMap::new();
        let mut gauges: Vec<OtelGauge> = Vec::new();

        for stat in &sai_stats.stats {
            let key = (stat.type_id, stat.stat_id);
            let data_point = OtelDataPoint::from_sai_stat(stat, observation_time_nano);

            let gauge_index = match index.get(&key).copied() {
                Some(i) => i,
                None => {
                    let i = gauges.len();
                    index.insert(key, i);
                    gauges.push(OtelGauge::empty_for(stat.type_id, stat.stat_id));
                    i
                }
            };
            gauges[gauge_index].data_points.push(data_point);
        }

        gauges
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use crate::message::saistats::{SAIStat, SAIStats};
    use log::{info, debug};

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
            type_id: 1,
            stat_id: 1,
            counter: 1500,
        };

        let observation_time_nano = 0u64; // 1970-01-01 00:00:00 UTC
        let data_point = OtelDataPoint::from_sai_stat(&sai_stat, observation_time_nano);

        assert_eq!(data_point.time_unix_nano, observation_time_nano);
        assert_eq!(data_point.value, 1500);
        assert_eq!(data_point.attributes.len(), 3);

        let object_name_attr = data_point.attributes.iter()
            .find(|attr| attr.key == "object_name").unwrap();
        assert_eq!(object_name_attr.value, "Ethernet0");

        let type_name_attr = data_point.attributes.iter()
            .find(|attr| attr.key == "sai_type_name").unwrap();
        assert_eq!(type_name_attr.value, "SAI_OBJECT_TYPE_PORT");

        let stat_name_attr = data_point.attributes.iter()
            .find(|attr| attr.key == "sai_stat_name").unwrap();
        assert_eq!(stat_name_attr.value, "SAI_PORT_STAT_IF_IN_UCAST_PKTS");
    }

    #[test]
    fn test_otel_gauge_from_single_sai_stat() {
        let observation_time_nano = 0u64; // 1970-01-01 00:00:00 UTC
        let sai_stats = SAIStats::new(
            observation_time_nano,
            vec![SAIStat {
                object_name: "BufferPool1".to_string(),
                type_id: 24,
                stat_id: 2,
                counter: 5000,
            }],
        );

        let gauges = OtelGauge::from_sai_stats(&sai_stats);
        assert_eq!(gauges.len(), 1);
        let gauge = &gauges[0];

        assert_eq!(gauge.name, "SAI_BUFFER_POOL_STAT_DROPPED_PACKETS");
        assert_eq!(
            gauge.description,
            "SAI_OBJECT_TYPE_BUFFER_POOL / SAI_BUFFER_POOL_STAT_DROPPED_PACKETS"
        );
        assert_eq!(gauge.unit, "1");
        assert_eq!(gauge.data_points.len(), 1);

        let data_point = &gauge.data_points[0];
        assert_eq!(data_point.value, 5000);
        assert_eq!(data_point.time_unix_nano, observation_time_nano);
        // object_name is preserved as a data-point attribute.
        assert!(data_point
            .attributes
            .iter()
            .any(|a| a.key == "object_name" && a.value == "BufferPool1"));
    }

    #[test]
    fn test_otel_gauge_from_sai_stats_collection() {
        let sai_stats = create_test_sai_stats(1672531200, 3);
        let gauges = OtelGauge::from_sai_stats(&sai_stats);

        assert_eq!(gauges.len(), 3);

        // Check first gauge
        let first_gauge = &gauges[0];
        assert_eq!(first_gauge.name, "SAI_PORT_STAT_IF_IN_UCAST_PKTS");
        assert_eq!(
            first_gauge.description,
            "SAI_OBJECT_TYPE_PORT / SAI_PORT_STAT_IF_IN_UCAST_PKTS"
        );
        assert!(first_gauge.data_points[0]
            .attributes
            .iter()
            .any(|a| a.key == "object_name" && a.value == "Ethernet0"));
        assert_eq!(first_gauge.data_points[0].value, 500);

        let expected_time_nano = 1672531200u64; 
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
            .find(|g| g.name == "SAI_PORT_STAT_IF_IN_UCAST_PKTS").unwrap();
        assert_eq!(port_gauge.data_points[0].value, 12345);

        let buffer_gauge = otel_metrics.gauges.iter()
            .find(|g| g.name == "SAI_BUFFER_POOL_STAT_DROPPED_PACKETS").unwrap();
        assert_eq!(buffer_gauge.data_points[0].value, 67890);
    }

    #[test]
    fn test_otel_metrics_message_creation() {
        let sai_stats = create_test_sai_stats(555555, 2);

        // Wrap metrics in Arc manually for sharing scenarios
        let message1 = Arc::new(OtelMetrics::from_sai_stats(&sai_stats));
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
fn test_sai_to_otel_gauge_conversion() {
    let test_stats = vec![
        SAIStat { object_name: "Ethernet0".to_string(), type_id: 1, stat_id: 1, counter: 1000000 },
        SAIStat { object_name: "Ethernet0".to_string(), type_id: 1, stat_id: 2, counter: 2000000 },
        SAIStat { object_name: "Ethernet1".to_string(), type_id: 1, stat_id: 1, counter: 1500000 },
        SAIStat { object_name: "BufferPool_ingress_lossless_pool".to_string(), type_id: 24, stat_id: 1, counter: 500000 },
    ];

    let sai_stats = SAIStats::new(1672531200, test_stats);
    let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);

    for (index, gauge) in otel_metrics.gauges.iter().enumerate() {
        let data_point = &gauge.data_points[0];
        info!("[{}] Gauge: {}", index + 1, gauge.name);
        info!("Value: {}, Unit: {}, Timestamp: {}ns", data_point.value, gauge.unit, data_point.time_unix_nano);
        info!("Description: {}", gauge.description);

        if !data_point.attributes.is_empty() {
            for attr in &data_point.attributes {
                debug!("  - {}={}", attr.key, attr.value);
            }
        }
        info!("Raw gauge: {:#?}", gauge);
    }

    assert_eq!(otel_metrics.len(), 3);

    // (type 1, stat 1) merges Ethernet0 + Ethernet1 into one gauge with 2 data points.
    let type1_stat1 = otel_metrics.gauges.iter()
        .find(|g| g.name == "SAI_PORT_STAT_IF_IN_UCAST_PKTS").unwrap();
    assert_eq!(type1_stat1.description, "SAI_OBJECT_TYPE_PORT / SAI_PORT_STAT_IF_IN_UCAST_PKTS");
    assert_eq!(type1_stat1.data_points.len(), 2);

    // Each object keeps its own value, timestamp, and object_name attribute.
    let e0 = type1_stat1.data_points.iter()
        .find(|dp| dp.attributes.iter().any(|a| a.key == "object_name" && a.value == "Ethernet0"))
        .unwrap();
    assert_eq!(e0.value, 1000000);
    assert_eq!(e0.time_unix_nano, 1672531200);
    let e1 = type1_stat1.data_points.iter()
        .find(|dp| dp.attributes.iter().any(|a| a.key == "object_name" && a.value == "Ethernet1"))
        .unwrap();
    assert_eq!(e1.value, 1500000);
    assert_eq!(e1.time_unix_nano, 1672531200);

    // (type 1, stat 2) has a single data point (Ethernet0).
    let type1_stat2 = otel_metrics.gauges.iter()
        .find(|g| g.name == "SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS").unwrap();
    assert_eq!(type1_stat2.data_points.len(), 1);
    assert_eq!(type1_stat2.data_points[0].value, 2000000);

    // Buffer pool stat (type 24, stat 1) has a single data point.
    let type24_stat1 = otel_metrics.gauges.iter()
        .find(|g| g.name == "SAI_BUFFER_POOL_STAT_WATERMARK_BYTES").unwrap();
    assert_eq!(type24_stat1.data_points.len(), 1);
    assert!(type24_stat1.data_points[0]
        .attributes.iter()
        .any(|a| a.key == "object_name" && a.value == "BufferPool_ingress_lossless_pool"));
    assert_eq!(type24_stat1.data_points[0].value, 500000);

    // All data points retain the message observation time.
    let expected_time = 1672531200u64;
    for gauge in &otel_metrics.gauges {
        for dp in &gauge.data_points {
            assert_eq!(dp.time_unix_nano, expected_time);
        }
    }
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
