//! OpenTelemetry Message Types
//!
//! This module defines data structures for converting SAI statistics
//! to OpenTelemetry gauge format for export to observability systems.

use super::aggregator::Heatmap;
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
    pub value: u64,
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

impl Heatmap {
    pub fn to_proto(
        &self,
        session_key: Option<&str>,
    ) -> opentelemetry_proto::tonic::metrics::v1::HistogramDataPoint {
        let mut attributes = vec![
            OtelAttribute::new("object_name", self.object_name.as_ref()).to_proto(),
            OtelAttribute::new("sai_type_id", self.type_id.to_string()).to_proto(),
            OtelAttribute::new("sai_stat_id", self.stat_id.to_string()).to_proto(),
            OtelAttribute::new("heatmap_value_kind", self.value_kind.as_str()).to_proto(),
            OtelAttribute::new("heatmap_quantity", self.quantity.as_str()).to_proto(),
            OtelAttribute::new("heatmap_schema", self.schema.as_ref()).to_proto(),
        ];
        if let Some(session_key) = session_key {
            attributes.push(OtelAttribute::new("hft_session", session_key).to_proto());
        }

        opentelemetry_proto::tonic::metrics::v1::HistogramDataPoint {
            attributes,
            start_time_unix_nano: self.start_time_unix_nano,
            time_unix_nano: self.time_unix_nano,
            count: self.count,
            sum: Some(self.sum),
            bucket_counts: self.bucket_counts.clone(),
            explicit_bounds: self.explicit_bounds.to_vec(),
            min: Some(self.min as f64),
            max: Some(self.max as f64),
            ..Default::default()
        }
    }
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
            value: sai_stat.counter,
        }
    }

    /// Converts to OpenTelemetry protobuf NumberDataPoint
    pub fn to_proto(&self) -> NumberDataPoint {
        // Keep one numeric representation for a metric across its lifetime.
        // OTLP doubles cannot exactly represent every u64 above 2^53, but they
        // avoid signed wraparound and backend type changes at i64::MAX.
        let value = number_data_point::Value::AsDouble(self.value as f64);
        NumberDataPoint {
            time_unix_nano: self.time_unix_nano,
            value: Some(value),
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
        // Use the observation_time from the SAI statistics
        let observation_time_nano = sai_stats.observation_time;

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

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use crate::message::aggregator::{
        default_heatmap_layout, heatmap_schema, HeatmapLayout, HeatmapQuantity, HeatmapValueKind,
    };
    use crate::message::saistats::{SAIStat, SAIStats};
    use log::{info, debug};
    use opentelemetry_proto::tonic::{
        collector::metrics::v1::ExportMetricsServiceRequest,
        common::v1::InstrumentationScope,
        metrics::v1::{
            metric::Data, AggregationTemporality, Histogram, Metric, ResourceMetrics, ScopeMetrics,
        },
        resource::v1::Resource,
    };
    use prost::Message;

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

    fn encoded_heatmap_point(
        object_index: usize,
        quantity: HeatmapQuantity,
        layout: Arc<HeatmapLayout>,
    ) -> opentelemetry_proto::tonic::metrics::v1::HistogramDataPoint {
        let value_kind = match quantity {
            HeatmapQuantity::DeltaBytes | HeatmapQuantity::DeltaCount => HeatmapValueKind::Delta,
            HeatmapQuantity::AbsoluteBytes
            | HeatmapQuantity::AbsoluteCells
            | HeatmapQuantity::Native => HeatmapValueKind::CurrentOccupancy,
        };
        Heatmap {
            object_name: Arc::from(format!("Ethernet{object_index}")),
            type_id: u32::MAX,
            stat_id: u32::MAX,
            start_time_unix_nano: u64::MAX - 1,
            time_unix_nano: u64::MAX,
            count: 64,
            sum: 4_096.0,
            min: 64,
            max: 64,
            explicit_bounds: layout.explicit_bounds(),
            bucket_counts: {
                let mut counts = vec![0; layout.bucket_count()];
                let bucket = layout
                    .explicit_bounds_u64()
                    .partition_point(|bound| *bound < 64);
                counts[bucket] = 64;
                counts
            },
            value_kind,
            quantity,
            unit: quantity.unit(),
            schema: heatmap_schema(value_kind, quantity, layout.explicit_bounds_u64()),
        }
        .to_proto(Some("profile_with_representative_name|PORT"))
    }

    fn encoded_request_size(
        series_count: usize,
        layouts: Vec<(HeatmapQuantity, Arc<HeatmapLayout>)>,
    ) -> usize {
        let mut points = vec![Vec::new(); layouts.len()];
        for index in 0..series_count {
            let layout_index = index % layouts.len();
            let (quantity, layout) = &layouts[layout_index];
            points[layout_index].push(encoded_heatmap_point(index, *quantity, layout.clone()));
        }
        let metrics = layouts
            .into_iter()
            .zip(points)
            .map(|((quantity, _), data_points)| Metric {
                name: format!("sai_counter_{}_heatmap", quantity.as_str()),
                description: format!("SAI {} heatmap", quantity.as_str()),
                unit: quantity.unit().to_string(),
                data: Some(Data::Histogram(Histogram {
                    data_points,
                    aggregation_temporality: AggregationTemporality::Delta as i32,
                })),
                ..Default::default()
            })
            .collect();
        ExportMetricsServiceRequest {
            resource_metrics: vec![ResourceMetrics {
                resource: Some(Resource {
                    attributes: vec![OtelAttribute::new("service.name", "countersyncd").to_proto()],
                    dropped_attributes_count: 0,
                }),
                scope_metrics: vec![ScopeMetrics {
                    scope: Some(InstrumentationScope {
                        name: "countersyncd".to_string(),
                        version: "1.0".to_string(),
                        attributes: Vec::new(),
                        dropped_attributes_count: 0,
                    }),
                    metrics,
                    schema_url: String::new(),
                }],
                schema_url: String::new(),
            }],
        }
        .encoded_len()
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
            .find(|g| g.name == "sai_counter_type_1_stat_1").unwrap();
        assert_eq!(port_gauge.data_points[0].value, 12345);

        let buffer_gauge = otel_metrics.gauges.iter()
            .find(|g| g.name == "sai_counter_type_24_stat_2").unwrap();
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
            number_data_point::Value::AsDouble(val) => assert_eq!(val, 777.0),
            _ => panic!("Expected double value"),
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
    let expected_time = 1672531200u64; 
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

    #[test]
    fn converts_heatmap_to_otel_histogram() {
        let heatmap = Heatmap {
            object_name: Arc::from("Ethernet0"),
            type_id: 1,
            stat_id: 2,
            start_time_unix_nano: 1_000,
            time_unix_nano: 2_000,
            count: 3,
            sum: 11.0,
            min: 1,
            max: 8,
            explicit_bounds: Arc::from([1.0, 2.0, 8.0]),
            bucket_counts: vec![1, 1, 1, 0],
            value_kind: crate::message::aggregator::HeatmapValueKind::Delta,
            quantity: crate::message::aggregator::HeatmapQuantity::DeltaCount,
            unit: "1",
            schema: crate::message::aggregator::heatmap_schema(
                crate::message::aggregator::HeatmapValueKind::Delta,
                crate::message::aggregator::HeatmapQuantity::DeltaCount,
                &[1, 2, 8],
            ),
        };

        let point = heatmap.to_proto(Some("profile|PORT"));

        assert_eq!(point.start_time_unix_nano, 1_000);
        assert_eq!(point.time_unix_nano, 2_000);
        assert_eq!(point.count, 3);
        assert_eq!(point.sum, Some(11.0));
        assert_eq!(point.min, Some(1.0));
        assert_eq!(point.max, Some(8.0));
        assert_eq!(point.explicit_bounds, vec![1.0, 2.0, 8.0]);
        assert_eq!(point.bucket_counts, vec![1, 1, 1, 0]);
        assert_eq!(point.attributes.len(), 7);
        assert!(point.attributes.iter().any(|attribute| {
            attribute.key == "hft_session"
                && attribute
                    .value
                    .as_ref()
                    .and_then(|value| value.value.as_ref())
                    == Some(&Value::StringValue("profile|PORT".to_string()))
        }));
        for (key, expected) in [
            ("heatmap_value_kind", "delta"),
            ("heatmap_quantity", "delta_count"),
            ("heatmap_schema", heatmap.schema.as_ref()),
        ] {
            assert!(point.attributes.iter().any(|attribute| {
                attribute.key == key
                    && attribute
                        .value
                        .as_ref()
                        .and_then(|value| value.value.as_ref())
                        == Some(&Value::StringValue(expected.to_string()))
            }));
        }
    }

    #[test]
    fn compact_default_histogram_points_have_stable_encoded_sizes() {
        let quantities = [
            HeatmapQuantity::DeltaBytes,
            HeatmapQuantity::AbsoluteBytes,
            HeatmapQuantity::AbsoluteCells,
            HeatmapQuantity::DeltaCount,
            HeatmapQuantity::Native,
        ];
        let sizes = quantities.map(|quantity| {
            let layout = default_heatmap_layout(quantity, Some(1_000)).unwrap();
            encoded_heatmap_point(0, quantity, layout).encoded_len()
        });

        assert_eq!(sizes, [595, 528, 802, 804, 1_250]);
        let old = encoded_heatmap_point(
            0,
            HeatmapQuantity::DeltaCount,
            HeatmapLayout::from_explicit_bounds((0..255).collect()).unwrap(),
        )
        .encoded_len();
        assert_eq!(old, 4_436);
        assert!(old > 4 * 1_024, "old point encoded to {old} bytes");
    }

    #[test]
    fn compact_default_export_requests_stay_under_grpc_budget() {
        let compact_layouts = || {
            [
                HeatmapQuantity::DeltaBytes,
                HeatmapQuantity::AbsoluteBytes,
                HeatmapQuantity::AbsoluteCells,
                HeatmapQuantity::DeltaCount,
                HeatmapQuantity::Native,
            ]
            .into_iter()
            .map(|quantity| {
                (
                    quantity,
                    default_heatmap_layout(quantity, Some(1_000)).unwrap(),
                )
            })
            .collect()
        };
        let compact = [64, 512, 4_096].map(|count| encoded_request_size(count, compact_layouts()));
        assert_eq!(compact, [51_140, 409_858, 3_283_291]);
        assert!(compact[2] > 3 * 1_024 * 1_024);
        assert!(compact.windows(2).all(|pair| pair[0] < pair[1]));

        let old_layout = || {
            vec![(
                HeatmapQuantity::DeltaCount,
                HeatmapLayout::from_explicit_bounds((0..255).collect()).unwrap(),
            )]
        };
        let old = encoded_request_size(4_096, old_layout());
        assert_eq!(old, 18_193_460);
        assert!(
            old > 16 * 1_024 * 1_024,
            "4096 old points encoded to {old} bytes"
        );

        let per_quantity = [
            HeatmapQuantity::DeltaBytes,
            HeatmapQuantity::AbsoluteBytes,
            HeatmapQuantity::AbsoluteCells,
            HeatmapQuantity::DeltaCount,
            HeatmapQuantity::Native,
        ]
        .map(|quantity| {
            encoded_request_size(
                4_096,
                vec![(
                    quantity,
                    default_heatmap_layout(quantity, Some(1_000)).unwrap(),
                )],
            )
        });
        assert_eq!(
            per_quantity,
            [2_460_725, 2_186_299, 3_308_607, 3_316_788, 5_143_594]
        );
        assert!(per_quantity[..2]
            .iter()
            .all(|size| *size < 3 * 1_024 * 1_024));
        assert!(per_quantity[2..]
            .iter()
            .all(|size| *size > 3 * 1_024 * 1_024));
    }

    #[test]
    fn encodes_gauges_consistently_as_double() {
        for counter in [i64::MAX as u64, i64::MAX as u64 + 1, u64::MAX] {
            let stat = SAIStat {
                object_name: "Ethernet0".to_string(),
                type_id: 1,
                stat_id: 2,
                counter,
            };
            let point = OtelDataPoint::from_sai_stat(&stat, 1).to_proto();
            assert_eq!(point.value, Some(number_data_point::Value::AsDouble(counter as f64)));
        }
    }
}
