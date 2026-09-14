//! OpenTelemetry Message Types
//!
//! This module defines data structures for converting SAI statistics
//! to OpenTelemetry gauge format for export to observability systems.

use crate::message::saistats::{SAIStatRef, SAIStatsRef};
#[cfg(test)]
use opentelemetry_proto::tonic::{
    common::v1::{any_value::Value, AnyValue, KeyValue as ProtoKeyValue},
    metrics::v1::{number_data_point, NumberDataPoint},
};
use std::borrow::Cow;

/// Canonical SAI C names for the object/stat tables shipped with countersyncd.
/// Unknown pairs retain both numeric IDs to remain unique and lossless. Resolve
/// only when populating a series cache, never for each sample in the fast path.
pub fn sai_metric_names(type_id: u32, stat_id: u32) -> (Cow<'static, str>, Cow<'static, str>) {
    let (type_name, stat_name) = crate::message::saistats::resolve_names(type_id, stat_id);
    let type_name = type_name
        .map(Cow::Borrowed)
        .unwrap_or_else(|| Cow::Owned(format!("UNKNOWN_SAI_OBJECT_TYPE_{type_id}")));
    let stat_name = stat_name
        .map(Cow::Borrowed)
        .unwrap_or_else(|| Cow::Owned(format!("UNKNOWN_SAI_STAT_TYPE_{type_id}_ID_{stat_id}")));
    (type_name, stat_name)
}

/// OpenTelemetry Gauge representation for SAI statistics
///
/// This struct represents an OpenTelemetry gauge metric following the OTLP protocol.
/// Each gauge contains data points with attributes, timestamps, and values derived
/// from SAI statistics.
#[derive(Debug, Clone, PartialEq)]
pub struct OtelGauge {
    /// Canonical SAI stat name (e.g., "SAI_PORT_STAT_IF_IN_OCTETS").
    /// Unsupported IDs use "UNKNOWN_SAI_STAT_TYPE_<type_id>_ID_<stat_id>".
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
    #[cfg(test)]
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
    pub fn from_sai_stat<'a>(
        sai_stat: impl Into<SAIStatRef<'a>>,
        observation_time_nano: u64,
    ) -> Self {
        let sai_stat = sai_stat.into();
        let attributes = vec![
            OtelAttribute::new("object_name", sai_stat.object_name.as_ref()),
            OtelAttribute::new("sai_type", sai_stat.type_name_or_id().into_owned()),
            OtelAttribute::new("sai_stat", sai_stat.stat_name_or_id().into_owned()),
        ];

        Self {
            attributes,
            time_unix_nano: observation_time_nano,
            value: sai_stat.counter as i64,
        }
    }

    /// Converts to OpenTelemetry protobuf NumberDataPoint
    #[cfg(test)]
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
    pub fn from_sai_stat<'a>(
        sai_stat: impl Into<SAIStatRef<'a>>,
        observation_time_nano: u64,
    ) -> Self {
        let sai_stat = sai_stat.into();
        let name = sai_stat.stat_name_or_id().into_owned();
        let description = format!(
            "SAI counter for object {} (type:{}, stat:{})",
            sai_stat.object_name.as_ref(),
            sai_stat.type_name_or_id(),
            name
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
    pub fn from_sai_stats(sai_stats: SAIStatsRef<'_>) -> Vec<Self> {
        // Use the observation_time from the SAI statistics
        let observation_time_nano = sai_stats.observation_time;

        sai_stats
            .stats
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
    pub fn from_sai_stats(sai_stats: SAIStatsRef<'_>) -> Self {
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
    use crate::message::saistats::{SAIStat, SAIStats};
    use log::{debug, info};
    use std::sync::Arc;

    #[test]
    fn shared_input_uses_precomputed_names_without_owned_projection() {
        use crate::message::saistats::{SAIStatMetadata, SAIStatsBatch};
        let mut batch = SAIStatsBatch::default();
        batch.push_shared_record(
            99,
            Arc::from(vec![SAIStatMetadata::new("Ethernet0", 1, 0)]),
            [123],
        );
        let metrics = OtelMetrics::from_sai_stats(batch.iter().next().unwrap());
        assert_eq!(metrics.gauges[0].name, "SAI_PORT_STAT_IF_IN_OCTETS");
        let point = &metrics.gauges[0].data_points[0];
        assert_eq!(point.time_unix_nano, 99);
        assert_eq!(point.value, 123);
        assert_eq!(point.attributes[1].key, "sai_type");
        assert_eq!(point.attributes[1].value, "SAI_OBJECT_TYPE_PORT");
        assert_eq!(point.attributes[2].key, "sai_stat");
        assert_eq!(point.attributes[2].value, "SAI_PORT_STAT_IF_IN_OCTETS");
    }

    #[test]
    fn canonical_names_are_object_specific_and_unknown_pairs_stay_unique() {
        for (type_id, stat_name) in [
            (1, "SAI_PORT_STAT_IF_IN_OCTETS"),
            (21, "SAI_QUEUE_STAT_PACKETS"),
            (24, "SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_BYTES"),
            (26, "SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS"),
        ] {
            let (object, stat) = sai_metric_names(type_id, 0);
            assert!(object.starts_with("SAI_OBJECT_TYPE_"));
            assert_eq!(stat, stat_name);
            let point = OtelDataPoint::from_sai_stat(&SAIStat::new("object", type_id, 0, 42), 99);
            assert_eq!(point.attributes[1].key, "sai_type");
            assert_eq!(point.attributes[1].value, object);
            assert_eq!(point.attributes[2].key, "sai_stat");
            assert_eq!(point.attributes[2].value, stat);
        }
        assert_eq!(
            sai_metric_names(u32::MAX, u32::MAX).0,
            "UNKNOWN_SAI_OBJECT_TYPE_4294967295"
        );
        assert_ne!(
            sai_metric_names(1, u32::MAX).1,
            sai_metric_names(21, u32::MAX).1
        );
        assert!(sai_metric_names(0x20000001, 0x20000001)
            .1
            .starts_with("UNKNOWN_SAI_STAT_TYPE_"));
    }

    /// Helper function to create test SAI statistics (similar to saistats.rs pattern)
    fn create_test_sai_stats(observation_time: u64, stat_count: usize) -> SAIStats {
        let stats = (0..stat_count)
            .map(|i| SAIStat {
                object_name: format!("Ethernet{}", i).into(),
                type_id: (i * 100 + 1) as u32,
                stat_id: (i * 10 + 1) as u32,
                counter: (i * 1000 + 500) as u64,
            })
            .collect();

        SAIStats::new(observation_time, stats)
    }

    fn as_ref(sai_stats: &SAIStats) -> SAIStatsRef<'_> {
        SAIStatsRef {
            observation_time: sai_stats.observation_time,
            stats: crate::message::saistats::SAIStatsView::Owned(&sai_stats.stats),
        }
    }

    #[test]
    fn test_otel_attribute_creation() {
        let attr = OtelAttribute::new("object_name", "Ethernet0");
        assert_eq!(attr.key, "object_name");
        assert_eq!(attr.value, "Ethernet0");

        let attr2 = OtelAttribute::new("sai_type", "SAI_OBJECT_TYPE_PORT");
        assert_eq!(attr2.key, "sai_type");
        assert_eq!(attr2.value, "SAI_OBJECT_TYPE_PORT");
    }

    #[test]
    fn test_otel_data_point_from_sai_stat() {
        let sai_stat = SAIStat {
            object_name: "Ethernet0".into(),
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
        let object_name_attr = data_point
            .attributes
            .iter()
            .find(|attr| attr.key == "object_name")
            .unwrap();
        assert_eq!(object_name_attr.value, "Ethernet0");

        let type_id_attr = data_point
            .attributes
            .iter()
            .find(|attr| attr.key == "sai_type")
            .unwrap();
        assert_eq!(type_id_attr.value, "SAI_OBJECT_TYPE_IPSEC_PORT");

        let stat_id_attr = data_point
            .attributes
            .iter()
            .find(|attr| attr.key == "sai_stat")
            .unwrap();
        assert_eq!(stat_id_attr.value, "UNKNOWN_SAI_STAT_TYPE_100_ID_200");
    }

    #[test]
    fn test_otel_gauge_from_sai_stat() {
        let sai_stat = SAIStat {
            object_name: "BufferPool1".into(),
            type_id: 24,
            stat_id: 2,
            counter: 5000,
        };

        let observation_time_nano = 0u64; // 1970-01-01 00:00:00 UTC
        let gauge = OtelGauge::from_sai_stat(&sai_stat, observation_time_nano);

        assert_eq!(gauge.name, "SAI_BUFFER_POOL_STAT_DROPPED_PACKETS");
        assert_eq!(
            gauge.description,
            "SAI counter for object BufferPool1 (type:SAI_OBJECT_TYPE_BUFFER_POOL, stat:SAI_BUFFER_POOL_STAT_DROPPED_PACKETS)"
        );
        assert_eq!(gauge.unit, "1");
        assert_eq!(gauge.data_points.len(), 1);

        let data_point = &gauge.data_points[0];
        assert_eq!(data_point.value, 5000);
        assert_eq!(data_point.time_unix_nano, observation_time_nano);
    }

    #[test]
    fn test_otel_gauge_from_sai_stats_collection() {
        let sai_stats = create_test_sai_stats(1672531200, 3);
        let gauges = OtelGauge::from_sai_stats(as_ref(&sai_stats));

        assert_eq!(gauges.len(), 3);

        // Check first gauge
        let first_gauge = &gauges[0];
        assert_eq!(first_gauge.name, "SAI_PORT_STAT_IF_IN_UCAST_PKTS");
        assert!(first_gauge.description.contains("Ethernet0"));
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
                    object_name: "Ethernet0".into(),
                    type_id: 1,
                    stat_id: 1,
                    counter: 12345,
                },
                SAIStat {
                    object_name: "BufferPool1".into(),
                    type_id: 24,
                    stat_id: 2,
                    counter: 67890,
                },
            ],
        );

        let otel_metrics = OtelMetrics::from_sai_stats(as_ref(&sai_stats));

        assert_eq!(otel_metrics.service_name, "countersyncd");
        assert_eq!(otel_metrics.scope_name, "countersyncd");
        assert_eq!(otel_metrics.scope_version, "1.0");
        assert_eq!(otel_metrics.len(), 2);
        assert!(!otel_metrics.is_empty());

        // Check individual gauges
        let port_gauge = otel_metrics
            .gauges
            .iter()
            .find(|g| g.name == "SAI_PORT_STAT_IF_IN_UCAST_PKTS")
            .unwrap();
        assert_eq!(port_gauge.data_points[0].value, 12345);

        let buffer_gauge = otel_metrics
            .gauges
            .iter()
            .find(|g| g.name == "SAI_BUFFER_POOL_STAT_DROPPED_PACKETS")
            .unwrap();
        assert_eq!(buffer_gauge.data_points[0].value, 67890);
    }

    #[test]
    fn test_otel_metrics_message_creation() {
        let sai_stats = create_test_sai_stats(555555, 2);

        // Wrap metrics in Arc manually for sharing scenarios
        let message1 = Arc::new(OtelMetrics::from_sai_stats(as_ref(&sai_stats)));
        let message2 = OtelMetrics::from_sai_stats(as_ref(&sai_stats));

        assert_eq!(message1.service_name, message2.service_name);
        assert_eq!(message1.len(), message2.len());
        assert_eq!(message1.gauges.len(), 2);
    }

    #[test]
    fn test_otel_data_point_proto_conversion() {
        let sai_stat = SAIStat {
            object_name: "TestInterface".into(),
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
        if let Some(AnyValue {
            value: Some(Value::StringValue(val)),
        }) = &object_attr.value
        {
            assert_eq!(val, "TestInterface");
        } else {
            panic!("Expected string value");
        }
    }

    #[test]
    fn test_sai_to_otel_gauge_conversion() {
        let test_stats = vec![
            SAIStat {
                object_name: "Ethernet0".into(),
                type_id: 1,
                stat_id: 1,
                counter: 1000000,
            },
            SAIStat {
                object_name: "Ethernet0".into(),
                type_id: 1,
                stat_id: 2,
                counter: 2000000,
            },
            SAIStat {
                object_name: "Ethernet1".into(),
                type_id: 1,
                stat_id: 1,
                counter: 1500000,
            },
            SAIStat {
                object_name: "BufferPool_ingress_lossless_pool".into(),
                type_id: 24,
                stat_id: 1,
                counter: 500000,
            },
        ];

        let sai_stats = SAIStats::new(1672531200, test_stats);
        let otel_metrics = OtelMetrics::from_sai_stats(as_ref(&sai_stats));

        for (index, gauge) in otel_metrics.gauges.iter().enumerate() {
            let data_point = &gauge.data_points[0];
            info!("[{}] Gauge: {}", index + 1, gauge.name);
            info!(
                "Value: {}, Unit: {}, Timestamp: {}ns",
                data_point.value, gauge.unit, data_point.time_unix_nano
            );
            info!("Description: {}", gauge.description);

            if !data_point.attributes.is_empty() {
                for attr in &data_point.attributes {
                    debug!("  - {}={}", attr.key, attr.value);
                }
            }
            info!("Raw gauge: {:#?}", gauge);
        }

        assert_eq!(otel_metrics.len(), 4);

        // Verify port stats conversion
        let port_stats: Vec<_> = otel_metrics
            .gauges
            .iter()
            .filter(|g| g.description.contains("Ethernet"))
            .collect();
        assert_eq!(port_stats.len(), 3);

        // Verify buffer pool stats conversion
        let buffer_stats: Vec<_> = otel_metrics
            .gauges
            .iter()
            .filter(|g| g.description.contains("BufferPool"))
            .collect();
        assert_eq!(buffer_stats.len(), 1);

        // Check that all metrics have proper timestamps
        let expected_time = 1672531200u64;
        for gauge in &otel_metrics.gauges {
            assert_eq!(gauge.data_points[0].time_unix_nano, expected_time);
        }

        // Verify metric naming
        let port_rx_metric = otel_metrics
            .gauges
            .iter()
            .find(|g| g.name == "SAI_PORT_STAT_IF_IN_UCAST_PKTS")
            .unwrap();
        assert!(port_rx_metric
            .description
            .contains("type:SAI_OBJECT_TYPE_PORT, stat:SAI_PORT_STAT_IF_IN_UCAST_PKTS"));
    }

    #[test]
    fn test_empty_sai_stats_to_otel() {
        let empty_stats = SAIStats::new(1111111111, vec![]);
        let otel_metrics = OtelMetrics::from_sai_stats(as_ref(&empty_stats));

        assert_eq!(otel_metrics.len(), 0);
        assert!(otel_metrics.is_empty());
        assert_eq!(otel_metrics.service_name, "countersyncd");
    }
}
