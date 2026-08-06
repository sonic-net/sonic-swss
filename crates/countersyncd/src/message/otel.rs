//! OpenTelemetry Message Types
//!
//! This module defines data structures for converting SAI statistics
//! to OpenTelemetry gauge format for export to observability systems.

use std::borrow::Cow;
use std::collections::HashMap;
use std::fmt;

use crate::message::saistats::SAIStats;
use crate::sai::{
    SaiBufferPoolStat, SaiIngressPriorityGroupStat, SaiObjectType, SaiPortStat, SaiQueueStat,
};
use opentelemetry_proto::tonic::{
    common::v1::{KeyValue as ProtoKeyValue, AnyValue, any_value::Value},
    metrics::v1::{NumberDataPoint, number_data_point, Gauge as ProtoGauge, Metric, metric},
};

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

/// Builds an OTLP protobuf `KeyValue` attribute with a string value.
fn proto_string_attr(key: &'static str, value: String) -> ProtoKeyValue {
    ProtoKeyValue {
        key: key.to_string(),
        value: Some(AnyValue {
            value: Some(Value::StringValue(value)),
        }),
    }
}

/// Converts a SAI statistics collection directly into OTLP protobuf `Metric`s,
/// grouping stats that share the same `(type_id, stat_id)` into a single gauge
/// with one data point per object.
pub fn sai_stats_to_proto_metrics(sai_stats: &SAIStats) -> Vec<Metric> {
    let observation_time_nano = sai_stats.observation_time;

    let mut index: HashMap<(u32, u32), usize> = HashMap::new();
    let mut metrics: Vec<Metric> = Vec::new();

    for stat in &sai_stats.stats {
        let key = (stat.type_id, stat.stat_id);

        let data_point = NumberDataPoint {
            time_unix_nano: observation_time_nano,
            value: Some(number_data_point::Value::AsInt(stat.counter as i64)),
            attributes: vec![
                proto_string_attr("object_name", stat.object_name.clone()),
                proto_string_attr("sai_type_name", sai_type_name(stat.type_id).into_owned()),
                proto_string_attr(
                    "sai_stat_name",
                    sai_stat_name(stat.type_id, stat.stat_id).into_owned(),
                ),
            ],
            ..Default::default()
        };

        let metric_index = match index.get(&key).copied() {
            Some(i) => i,
            None => {
                let i = metrics.len();
                index.insert(key, i);

                let type_name = sai_type_name(stat.type_id);
                let stat_name = sai_stat_name(stat.type_id, stat.stat_id);
                metrics.push(Metric {
                    name: stat_name.clone().into_owned(),
                    description: format!("{} / {}", type_name, stat_name),
                    metadata: vec![],
                    data: Some(metric::Data::Gauge(ProtoGauge {
                        data_points: Vec::new(),
                    })),
                    ..Default::default()
                });

                i
            }
        };

        if let Some(metric::Data::Gauge(gauge)) = metrics[metric_index].data.as_mut() {
            gauge.data_points.push(data_point);
        }
    }

    metrics
}

/// Human-readable rendering of a [`SAIStats`] batch for debug logging.
pub struct DisplaySaiStats<'a>(pub &'a SAIStats);

impl fmt::Display for DisplaySaiStats<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let stats = self.0;
        writeln!(
            f,
            "SAIStats @ {}ns ({} counters)",
            stats.observation_time,
            stats.stats.len()
        )?;
        for stat in &stats.stats {
            writeln!(
                f,
                "  {} {} / {} = {}",
                stat.object_name,
                sai_type_name(stat.type_id),
                sai_stat_name(stat.type_id, stat.stat_id),
                stat.counter
            )?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::saistats::SAIStat;

    /// Returns the `Gauge` data points of a proto `Metric`
    fn gauge_points(metric: &Metric) -> &Vec<NumberDataPoint> {
        match metric.data.as_ref() {
            Some(metric::Data::Gauge(gauge)) => &gauge.data_points,
            _ => panic!("metric {} is not a gauge", metric.name),
        }
    }

    /// Reads a string attribute value by key from a proto data point.
    fn attr<'a>(dp: &'a NumberDataPoint, key: &str) -> &'a str {
        dp.attributes
            .iter()
            .find(|kv| kv.key == key)
            .and_then(|kv| kv.value.as_ref())
            .and_then(|v| match v.value.as_ref() {
                Some(Value::StringValue(s)) => Some(s.as_str()),
                _ => None,
            })
            .unwrap_or_else(|| panic!("string attribute `{key}` missing"))
    }

    /// Reads the integer value of a proto data point.
    fn as_int(dp: &NumberDataPoint) -> i64 {
        match dp.value {
            Some(number_data_point::Value::AsInt(v)) => v,
            _ => panic!("data point value is not AsInt"),
        }
    }

    #[test]
    fn test_sai_stats_to_proto_metrics_empty() {
        let sai_stats = SAIStats::new(0, vec![]);
        assert!(sai_stats_to_proto_metrics(&sai_stats).is_empty());
    }

    #[test]
    fn test_sai_stats_to_proto_metrics_single_stat_fields() {
        let observation_time = 1_700_000_000u64;
        let sai_stats = SAIStats::new(
            observation_time,
            vec![SAIStat {
                object_name: "Ethernet0".to_string(),
                type_id: 1, // SAI_OBJECT_TYPE_PORT
                stat_id: 1, // SAI_PORT_STAT_IF_IN_UCAST_PKTS
                counter: 12345,
            }],
        );

        let metrics = sai_stats_to_proto_metrics(&sai_stats);
        assert_eq!(metrics.len(), 1, "one (type_id, stat_id) key -> one gauge");

        let metric = &metrics[0];
        assert_eq!(metric.name, "SAI_PORT_STAT_IF_IN_UCAST_PKTS");
        assert_eq!(
            metric.description,
            "SAI_OBJECT_TYPE_PORT / SAI_PORT_STAT_IF_IN_UCAST_PKTS"
        );

        let points = gauge_points(metric);
        assert_eq!(points.len(), 1);
        let dp = &points[0];
        assert_eq!(as_int(dp), 12345, "value comes from the SAI counter");
        assert_eq!(dp.time_unix_nano, observation_time);

        // Exactly the three expected attributes.
        assert_eq!(dp.attributes.len(), 3);
        assert_eq!(attr(dp, "object_name"), "Ethernet0");
        assert_eq!(attr(dp, "sai_type_name"), "SAI_OBJECT_TYPE_PORT");
        assert_eq!(attr(dp, "sai_stat_name"), "SAI_PORT_STAT_IF_IN_UCAST_PKTS");
    }

    #[test]
    fn test_sai_stats_to_proto_metrics_groups_by_type_and_stat() {
        let observation_time = 42u64;
        let sai_stats = SAIStats::new(
            observation_time,
            vec![
                // Two objects sharing the same (type_id, stat_id) must merge
                // into a single gauge, one data point per object.
                SAIStat {
                    object_name: "Ethernet0".to_string(),
                    type_id: 1,
                    stat_id: 1,
                    counter: 10,
                },
                SAIStat {
                    object_name: "Ethernet1".to_string(),
                    type_id: 1,
                    stat_id: 1,
                    counter: 20,
                },
                // A different (type_id, stat_id) gets its own gauge.
                SAIStat {
                    object_name: "BufferPool1".to_string(),
                    type_id: 24,
                    stat_id: 2,
                    counter: 30,
                },
            ],
        );

        let metrics = sai_stats_to_proto_metrics(&sai_stats);
        assert_eq!(
            metrics.len(),
            2,
            "two distinct (type_id, stat_id) keys -> two gauges"
        );

        // First gauge: the shared (1, 1) key, one data point per object in
        // input order.
        let shared = &metrics[0];
        assert_eq!(shared.name, "SAI_PORT_STAT_IF_IN_UCAST_PKTS");
        let shared_points = gauge_points(shared);
        assert_eq!(shared_points.len(), 2, "both objects merged into one gauge");
        assert_eq!(attr(&shared_points[0], "object_name"), "Ethernet0");
        assert_eq!(as_int(&shared_points[0]), 10);
        assert_eq!(attr(&shared_points[1], "object_name"), "Ethernet1");
        assert_eq!(as_int(&shared_points[1]), 20);

        // Second gauge: the distinct (24, 2) key.
        let other = &metrics[1];
        assert_eq!(other.name, "SAI_BUFFER_POOL_STAT_DROPPED_PACKETS");
        let other_points = gauge_points(other);
        assert_eq!(other_points.len(), 1);
        assert_eq!(attr(&other_points[0], "object_name"), "BufferPool1");
        assert_eq!(as_int(&other_points[0]), 30);
    }

    #[test]
    fn test_sai_stats_to_proto_metrics_covers_all_supported_object_types() {
        // (object_name, type_id, stat_id, expected_type_name, expected_stat_name)
        // Spans every object type the converter dispatches on (port, queue,
        // buffer pool, ingress priority group) with several distinct stat ids
        // so both the type-name and stat-name lookups are exercised.
        let cases: &[(&str, u32, u32, &str, &str)] = &[
            (
                "Ethernet0",
                1,
                1,
                "SAI_OBJECT_TYPE_PORT",
                "SAI_PORT_STAT_IF_IN_UCAST_PKTS",
            ),
            (
                "Ethernet0:Queue0",
                21,
                0,
                "SAI_OBJECT_TYPE_QUEUE",
                "SAI_QUEUE_STAT_PACKETS",
            ),
            (
                "Ethernet0:Queue1",
                21,
                2,
                "SAI_OBJECT_TYPE_QUEUE",
                "SAI_QUEUE_STAT_DROPPED_PACKETS",
            ),
            (
                "BufferPool0",
                24,
                0,
                "SAI_OBJECT_TYPE_BUFFER_POOL",
                "SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_BYTES",
            ),
            (
                "BufferPool0",
                24,
                2,
                "SAI_OBJECT_TYPE_BUFFER_POOL",
                "SAI_BUFFER_POOL_STAT_DROPPED_PACKETS",
            ),
            (
                "Ethernet0:PG0",
                26,
                0,
                "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP",
                "SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS",
            ),
            (
                "Ethernet0:PG0",
                26,
                8,
                "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP",
                "SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS",
            ),
        ];

        let observation_time = 9_000u64;
        let stats: Vec<SAIStat> = cases
            .iter()
            .enumerate()
            .map(|(i, (obj, type_id, stat_id, _, _))| SAIStat {
                object_name: (*obj).to_string(),
                type_id: *type_id,
                stat_id: *stat_id,
                counter: (i as u64 + 1) * 100,
            })
            .collect();
        let sai_stats = SAIStats::new(observation_time, stats);

        let metrics = sai_stats_to_proto_metrics(&sai_stats);
        assert_eq!(
            metrics.len(),
            cases.len(),
            "each distinct (type_id, stat_id) becomes its own gauge"
        );

        for (i, (obj, _type_id, _stat_id, type_name, stat_name)) in cases.iter().enumerate() {
            let metric = &metrics[i];
            assert_eq!(metric.name, *stat_name, "gauge name for case {i}");
            assert_eq!(
                metric.description,
                format!("{type_name} / {stat_name}"),
                "description for case {i}"
            );

            let points = gauge_points(metric);
            assert_eq!(points.len(), 1, "case {i} has a single object");
            let dp = &points[0];
            assert_eq!(as_int(dp), (i as i64 + 1) * 100, "value for case {i}");
            assert_eq!(dp.time_unix_nano, observation_time);
            assert_eq!(attr(dp, "object_name"), *obj, "object_name for case {i}");
            assert_eq!(attr(dp, "sai_type_name"), *type_name, "type name for case {i}");
            assert_eq!(attr(dp, "sai_stat_name"), *stat_name, "stat name for case {i}");
        }
    }

    #[test]
    fn test_sai_stats_to_proto_metrics_unknown_ids_fallback() {
        let sai_stats = SAIStats::new(
            7u64,
            vec![
                // Unknown object type -> synthetic type and stat names.
                SAIStat {
                    object_name: "Mystery0".to_string(),
                    type_id: 99_999,
                    stat_id: 5,
                    counter: 1,
                },
                // Known type (Port) but unknown stat id -> real type name,
                // synthetic stat name.
                SAIStat {
                    object_name: "Ethernet0".to_string(),
                    type_id: 1,
                    stat_id: 888_888,
                    counter: 2,
                },
            ],
        );

        let metrics = sai_stats_to_proto_metrics(&sai_stats);
        assert_eq!(metrics.len(), 2);

        let unknown_type = &metrics[0];
        assert_eq!(unknown_type.name, "SAI_STAT_UNKNOWN_TYPE_99999_STAT_5");
        assert_eq!(
            unknown_type.description,
            "SAI_OBJECT_TYPE_UNKNOWN_99999 / SAI_STAT_UNKNOWN_TYPE_99999_STAT_5"
        );
        let p0 = &gauge_points(unknown_type)[0];
        assert_eq!(attr(p0, "sai_type_name"), "SAI_OBJECT_TYPE_UNKNOWN_99999");
        assert_eq!(attr(p0, "sai_stat_name"), "SAI_STAT_UNKNOWN_TYPE_99999_STAT_5");

        let unknown_stat = &metrics[1];
        assert_eq!(unknown_stat.name, "SAI_STAT_UNKNOWN_TYPE_1_STAT_888888");
        let p1 = &gauge_points(unknown_stat)[0];
        // The type resolves to a real name; only the stat id is unknown.
        assert_eq!(attr(p1, "sai_type_name"), "SAI_OBJECT_TYPE_PORT");
        assert_eq!(attr(p1, "sai_stat_name"), "SAI_STAT_UNKNOWN_TYPE_1_STAT_888888");
    }

    #[test]
    fn test_sai_type_name_known_and_unknown() {
        assert_eq!(sai_type_name(1).as_ref(), "SAI_OBJECT_TYPE_PORT");
        assert_eq!(sai_type_name(21).as_ref(), "SAI_OBJECT_TYPE_QUEUE");
        assert_eq!(sai_type_name(24).as_ref(), "SAI_OBJECT_TYPE_BUFFER_POOL");
        assert_eq!(
            sai_type_name(26).as_ref(),
            "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP"
        );
        assert_eq!(sai_type_name(99_999).as_ref(), "SAI_OBJECT_TYPE_UNKNOWN_99999");
    }

    #[test]
    fn test_sai_stat_name_known_and_unknown() {
        assert_eq!(sai_stat_name(1, 1).as_ref(), "SAI_PORT_STAT_IF_IN_UCAST_PKTS");
        assert_eq!(sai_stat_name(21, 0).as_ref(), "SAI_QUEUE_STAT_PACKETS");
        assert_eq!(sai_stat_name(21, 2).as_ref(), "SAI_QUEUE_STAT_DROPPED_PACKETS");
        assert_eq!(
            sai_stat_name(24, 0).as_ref(),
            "SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_BYTES"
        );
        assert_eq!(
            sai_stat_name(24, 2).as_ref(),
            "SAI_BUFFER_POOL_STAT_DROPPED_PACKETS"
        );
        assert_eq!(
            sai_stat_name(26, 0).as_ref(),
            "SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS"
        );
        assert_eq!(
            sai_stat_name(26, 8).as_ref(),
            "SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS"
        );
        // Unknown stat id on a known type, and an unknown type entirely.
        assert_eq!(
            sai_stat_name(1, 888_888).as_ref(),
            "SAI_STAT_UNKNOWN_TYPE_1_STAT_888888"
        );
        assert_eq!(
            sai_stat_name(99_999, 5).as_ref(),
            "SAI_STAT_UNKNOWN_TYPE_99999_STAT_5"
        );
    }

    #[test]
    fn test_display_sai_stats_debug_format() {
        let sai_stats = SAIStats::new(
            1000,
            vec![
                SAIStat {
                    object_name: "Ethernet0".to_string(),
                    type_id: 1,
                    stat_id: 1,
                    counter: 42,
                },
                // Unknown ids still render via the synthetic-name fallback.
                SAIStat {
                    object_name: "Mystery0".to_string(),
                    type_id: 99_999,
                    stat_id: 5,
                    counter: 7,
                },
            ],
        );

        let rendered = DisplaySaiStats(&sai_stats).to_string();
        assert!(rendered.contains("SAIStats @ 1000ns (2 counters)"));
        assert!(rendered
            .contains("Ethernet0 SAI_OBJECT_TYPE_PORT / SAI_PORT_STAT_IF_IN_UCAST_PKTS = 42"));
        assert!(rendered.contains(
            "Mystery0 SAI_OBJECT_TYPE_UNKNOWN_99999 / SAI_STAT_UNKNOWN_TYPE_99999_STAT_5 = 7"
        ));
    }
}
