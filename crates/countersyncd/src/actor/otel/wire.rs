//! Encode the fixed SAI Gauge schema without constructing a protobuf object tree.
//! All field numbers/types are from opentelemetry-proto 0.25. Decode-equivalence
//! tests below guard this wire-compatible optimization. No sample is aggregated.
use crate::message::otel::sai_metric_names;
use crate::message::saistats::{SAIStat, SAIStatMetadata, SAIStatRef};
use ahash::AHashMap;
use opentelemetry_proto::tonic::collector::metrics::v1::{
    ExportMetricsServiceRequest, ExportMetricsServiceResponse,
};
use prost::bytes::BufMut;
#[cfg(test)]
use prost::bytes::{Bytes, BytesMut};
use std::sync::Arc;
use tonic::codec::{BufferSettings, Codec, EncodeBuf, Encoder, ProstCodec};

// Bound retained series metadata; active requests are bounded by the actor's
// counter threshold. Clear the cache after a flush when this limit is exceeded.
const MAX_CACHED_SERIES: usize = 4096;
const MAX_RETAINED_BYTES: usize = 32 * 1024 * 1024;

#[derive(Clone, Copy)]
pub(super) struct Sample {
    pub(super) timestamp: u64,
    pub(super) value: i64,
}
#[derive(Hash, PartialEq, Eq)]
struct SeriesKey {
    object: Arc<str>,
    type_id: u32,
    stat_id: u32,
}
struct Series {
    metadata: Vec<u8>,
    attributes: Vec<u8>,
    // Shared immutable metadata plus a compact contiguous timestamp/value list.
    // Point attributes are expanded once, only at the OTLP encoding boundary.
    points: Vec<Sample>,
}
pub(super) struct GaugeBuffer {
    index: AHashMap<SeriesKey, usize>,
    series: Vec<Series>,
    active: Vec<usize>,
    plans: Vec<(Arc<[SAIStatMetadata]>, Arc<[usize]>)>,
}
fn varint_len(n: usize) -> usize {
    ((usize::BITS - n.leading_zeros()).max(1) as usize + 6) / 7
}
fn field_len(n: usize) -> usize {
    1 + varint_len(n) + n
}
fn varint(out: &mut impl BufMut, mut n: usize) {
    while n >= 128 {
        out.put_u8((n as u8 & 127) | 128);
        n >>= 7;
    }
    out.put_u8(n as u8);
}
fn header(out: &mut impl BufMut, tag: u8, n: usize) {
    out.put_u8(tag);
    varint(out, n);
}
fn string(out: &mut Vec<u8>, tag: u8, s: &str) {
    header(out, tag, s.len());
    out.extend_from_slice(s.as_bytes());
}

impl GaugeBuffer {
    pub(super) fn shared_slots(&mut self, metadata: &Arc<[SAIStatMetadata]>) -> Arc<[usize]> {
        if let Some((_, slots)) = self.plans.iter().find(|(m, _)| Arc::ptr_eq(m, metadata)) {
            return slots.clone();
        }
        if self.plans.len() >= 64 {
            self.plans.clear();
        }
        let slots: Arc<[usize]> = metadata
            .iter()
            .map(|m| {
                let key = SeriesKey {
                    object: m.object_name.clone(),
                    type_id: m.type_id,
                    stat_id: m.stat_id,
                };
                if let Some(&slot) = self.index.get(&key) {
                    return slot;
                }
                self.push_ref((m, 0).into(), 0);
                let slot = *self.index.get(&key).unwrap();
                self.series[slot].points.pop();
                self.active.pop();
                slot
            })
            .collect::<Vec<_>>()
            .into();
        self.plans.push((metadata.clone(), slots.clone()));
        slots
    }
    pub(super) fn push_slot(&mut self, slot: usize, timestamp: u64, value: u64) {
        let series = &mut self.series[slot];
        if series.points.is_empty() {
            self.active.push(slot);
        }
        series.points.push(Sample {
            timestamp,
            value: value as i64,
        });
    }
    pub(super) fn new() -> Self {
        Self {
            index: AHashMap::new(),
            series: Vec::new(),
            active: Vec::new(),
            plans: Vec::new(),
        }
    }
    #[allow(dead_code)]
    pub(super) fn push(&mut self, stat: &SAIStat, timestamp: u64) {
        self.push_ref(stat.into(), timestamp);
    }
    pub(super) fn push_ref(&mut self, stat: SAIStatRef<'_>, timestamp: u64) {
        let key = SeriesKey {
            object: stat.object_name.clone(),
            type_id: stat.type_id,
            stat_id: stat.stat_id,
        };
        let slot = *self.index.entry(key).or_insert_with(|| {
            let (type_name, stat_name) = match (stat.type_name(), stat.stat_name()) {
                (Some(type_name), Some(stat_name)) => (
                    std::borrow::Cow::Borrowed(type_name),
                    std::borrow::Cow::Borrowed(stat_name),
                ),
                _ => sai_metric_names(stat.type_id, stat.stat_id),
            };
            let mut metadata = Vec::new();
            string(&mut metadata, 10, &stat_name);
            string(
                &mut metadata,
                18,
                &format!(
                    "SAI counter for object {} (type:{}, stat:{})",
                    stat.object_name, type_name, stat_name
                ),
            );
            let mut attributes = Vec::new();
            for (key, value) in [
                ("object_name", stat.object_name.to_string()),
                ("sai_type", type_name.into_owned()),
                ("sai_stat", stat_name.into_owned()),
            ] {
                let mut kv = Vec::new();
                string(&mut kv, 10, key);
                header(&mut kv, 18, field_len(value.len()));
                string(&mut kv, 10, &value);
                header(&mut attributes, 58, kv.len());
                attributes.extend_from_slice(&kv);
            }
            self.series.push(Series {
                metadata,
                attributes,
                points: Vec::new(),
            });
            self.series.len() - 1
        });
        let series = &mut self.series[slot];
        if series.points.is_empty() {
            self.active.push(slot);
        }
        series.points.push(Sample {
            timestamp,
            value: stat.counter as i64,
        });
    }
    #[cfg(test)]
    pub(super) fn encode(&self, resource: &[u8], scope: &[u8]) -> Bytes {
        let mut out = BytesMut::with_capacity(field_len(self.encoded_lengths(resource, scope).1));
        self.encode_into(resource, scope, &mut out);
        out.freeze()
    }
    pub(super) fn clear(&mut self) {
        for i in self.active.drain(..) {
            self.series[i].points.clear();
        }
        if self.series.len() > MAX_CACHED_SERIES
            || self
                .series
                .iter()
                .map(|s| {
                    s.points.capacity() * std::mem::size_of::<Sample>()
                        + s.metadata.capacity()
                        + s.attributes.capacity()
                })
                .sum::<usize>()
                > MAX_RETAINED_BYTES
        {
            self.index.clear();
            self.series.clear();
            self.plans.clear();
        }
    }

    fn encoded_lengths(&self, resource: &[u8], scope: &[u8]) -> (usize, usize) {
        let scope_len = field_len(scope.len())
            + self
                .active
                .iter()
                .map(|&i| {
                    let s = &self.series[i];
                    field_len(
                        s.metadata.len()
                            + field_len(field_len(18 + s.attributes.len()) * s.points.len()),
                    )
                })
                .sum::<usize>();
        (scope_len, field_len(resource.len()) + field_len(scope_len))
    }

    fn encode_into(&self, resource: &[u8], scope: &[u8], out: &mut impl BufMut) {
        let (scope_len, resource_len) = self.encoded_lengths(resource, scope);
        header(out, 10, resource_len);
        header(out, 10, resource.len());
        out.put_slice(resource);
        header(out, 18, scope_len);
        header(out, 10, scope.len());
        out.put_slice(scope);
        for &i in &self.active {
            let s = &self.series[i];
            let point_len = 18 + s.attributes.len();
            let gauge_len = field_len(point_len) * s.points.len();
            header(out, 18, s.metadata.len() + field_len(gauge_len));
            out.put_slice(&s.metadata);
            header(out, 42, gauge_len);
            for sample in &s.points {
                header(out, 10, point_len);
                out.put_u8(25);
                out.put_u64_le(sample.timestamp);
                out.put_u8(49);
                out.put_i64_le(sample.value);
                out.put_slice(&s.attributes);
            }
        }
    }

    pub(super) fn prepare(&mut self, resource: &[u8], scope: &[u8]) -> WireRequest {
        let buffer = std::mem::replace(self, Self::new());
        WireRequest(Arc::new(DirectRequest {
            buffer,
            resource: resource.to_vec(),
            scope: scope.to_vec(),
        }))
    }

    pub(super) fn reclaim(&mut self, request: WireRequest) {
        if let Ok(request) = Arc::try_unwrap(request.0) {
            *self = request.buffer;
        }
        // A transport may retain a clone after returning. In that case keep
        // the fresh buffer; never mutate outstanding sample data.
    }
}

pub(super) struct DirectRequest {
    buffer: GaugeBuffer,
    resource: Vec<u8>,
    scope: Vec<u8>,
}
#[derive(Clone)]
pub(super) struct WireRequest(Arc<DirectRequest>);

// Tonic still supplies standard unary gRPC framing, transport and flow control.
// Retries share immutable samples and re-encode exactly the same payload.
pub(super) struct EncodedMetricsCodec {
    size: usize,
}
impl EncodedMetricsCodec {
    pub(super) fn for_request(request: &WireRequest) -> Self {
        let r = &request.0;
        let size = field_len(r.buffer.encoded_lengths(&r.resource, &r.scope).1) + 5;
        Self { size }
    }
}
pub(super) struct EncodedMetricsEncoder {
    size: usize,
}
impl Encoder for EncodedMetricsEncoder {
    type Item = WireRequest;
    type Error = tonic::Status;
    fn buffer_settings(&self) -> BufferSettings {
        BufferSettings::new(self.size, 32 * 1024)
    }
    fn encode(&mut self, item: WireRequest, dst: &mut EncodeBuf<'_>) -> Result<(), Self::Error> {
        let request = item.0;
        request
            .buffer
            .encode_into(&request.resource, &request.scope, dst);
        Ok(())
    }
}
impl Codec for EncodedMetricsCodec {
    type Encode = WireRequest;
    type Decode = ExportMetricsServiceResponse;
    type Encoder = EncodedMetricsEncoder;
    type Decoder =
        <ProstCodec<ExportMetricsServiceRequest, ExportMetricsServiceResponse> as Codec>::Decoder;
    fn encoder(&mut self) -> Self::Encoder {
        EncodedMetricsEncoder { size: self.size }
    }
    fn decoder(&mut self) -> Self::Decoder {
        ProstCodec::<ExportMetricsServiceRequest, ExportMetricsServiceResponse>::default().decoder()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::otel::OtelDataPoint;
    use opentelemetry_proto::tonic::metrics::v1::metric::Data;
    use prost::Message;

    #[test]
    fn direct_request_retains_immutable_samples_and_exact_encoding_size() {
        let mut buffer = GaugeBuffer::new();
        let mut stat = SAIStat::new("Ethernet0", 1, 0, u64::MAX);
        buffer.push(&stat, 10);
        let request = buffer.prepare(&[], &[]);
        let held = request.clone();
        buffer.reclaim(request);
        buffer.clear();
        stat.counter = 42;
        buffer.push(&stat, 20);
        let old = &held.0;
        let mut bytes = BytesMut::new();
        old.buffer
            .encode_into(&old.resource, &old.scope, &mut bytes);
        assert_eq!(
            EncodedMetricsCodec::for_request(&held).size,
            bytes.len() + 5
        );
        let decoded = ExportMetricsServiceRequest::decode(bytes.freeze()).unwrap();
        let Some(Data::Gauge(g)) = &decoded.resource_metrics[0].scope_metrics[0].metrics[0].data
        else {
            panic!()
        };
        assert_eq!(g.data_points[0].time_unix_nano, 10);
        assert_eq!(
            g.data_points[0].value,
            Some(opentelemetry_proto::tonic::metrics::v1::number_data_point::Value::AsInt(-1))
        );
    }

    #[test]
    fn direct_encoding_matches_reference_across_shape_and_metadata_changes() {
        let mut buffer = GaugeBuffer::new();
        let stat = SAIStat::new("Ethernet0", 1, 0, u64::MAX);
        let other = SAIStat::new("Ethernet4", 21, 1, 7);
        for (count, reverse, resource) in [
            (3, false, vec![]),
            (3, false, vec![]),
            (3, true, vec![]),
            (1, false, vec![]),
            (3, false, vec![0x10, 0x01]),
        ] {
            let mut reference = GaugeBuffer::new();
            for round in 0..count {
                for s in if reverse {
                    [&other, &stat]
                } else {
                    [&stat, &other]
                } {
                    buffer.push(s, round + 100);
                    reference.push(s, round + 100);
                }
            }
            let expected = reference.encode(&resource, &[]);
            let request = buffer.prepare(&resource, &[]);
            let actual = {
                let r = &request.0;
                let mut out = BytesMut::new();
                r.buffer.encode_into(&r.resource, &r.scope, &mut out);
                out.freeze()
            };
            assert_eq!(actual, expected);
            ExportMetricsServiceRequest::decode(actual).unwrap();
            buffer.reclaim(request);
            buffer.clear();
        }
    }

    #[test]
    fn direct_request_returns_sample_capacity_after_ack() {
        assert_eq!(std::mem::size_of::<Sample>(), 16);
        let mut buffer = GaugeBuffer::new();
        let stat = SAIStat::new("Ethernet0", 1, 0, 123);
        buffer.push(&stat, 10);
        let pointer = buffer.series[0].points.as_ptr();
        let first = buffer.prepare(&[], &[]);
        buffer.reclaim(first);
        buffer.clear();
        buffer.push(&stat, 20);
        assert_eq!(buffer.series[0].points.as_ptr(), pointer);
        let decoded = ExportMetricsServiceRequest::decode(buffer.encode(&[], &[])).unwrap();
        let Some(Data::Gauge(g)) = &decoded.resource_metrics[0].scope_metrics[0].metrics[0].data
        else {
            panic!()
        };
        assert_eq!(g.data_points[0].time_unix_nano, 20);
    }

    #[test]
    fn metadata_changes_create_distinct_cached_identities() {
        let mut buffer = GaugeBuffer::new();
        for stat in [
            SAIStat::new("Ethernet0", 1, 0, 1),
            SAIStat::new("Ethernet4", 1, 0, 2),
            SAIStat::new("Ethernet0", 1, 1, 3),
        ] {
            buffer.push(&stat, 100);
        }
        let decoded = ExportMetricsServiceRequest::decode(buffer.encode(&[], &[])).unwrap();
        assert_eq!(
            decoded.resource_metrics[0].scope_metrics[0].metrics.len(),
            3
        );
        assert_eq!(buffer.index.len(), 3);
    }
    #[test]
    fn preserves_attributes_values_timestamps_duplicates_and_series_order() {
        let mut buffer = GaugeBuffer::new();
        let a = SAIStat::new("Ethernet0", u32::MAX, 0x20000001, u64::MAX);
        let b = SAIStat::new("a long object name / \u{2603}", 1, 3, 0);
        for (stat, time) in [(&a, 1), (&b, u64::MAX), (&a, 2), (&a, 2)] {
            buffer.push(stat, time);
        }
        let decoded = ExportMetricsServiceRequest::decode(buffer.encode(&[], &[])).unwrap();
        let metrics = &decoded.resource_metrics[0].scope_metrics[0].metrics;
        assert_eq!(metrics.len(), 2);
        for (metric, stat, times) in [
            (&metrics[0], &a, vec![1, 2, 2]),
            (&metrics[1], &b, vec![u64::MAX]),
        ] {
            let Some(Data::Gauge(g)) = &metric.data else {
                panic!("expected gauge")
            };
            assert_eq!(g.data_points.len(), times.len());
            for (point, time) in g.data_points.iter().zip(times) {
                assert_eq!(point, &OtelDataPoint::from_sai_stat(stat, time).to_proto());
            }
            assert_eq!(metric.name, sai_metric_names(stat.type_id, stat.stat_id).1);
            assert_eq!(
                metric.description,
                format!(
                    "SAI counter for object {} (type:{}, stat:{})",
                    stat.object_name,
                    sai_metric_names(stat.type_id, stat.stat_id).0,
                    sai_metric_names(stat.type_id, stat.stat_id).1
                )
            );
        }
        buffer.clear();
        buffer.push(&a, 99);
        let decoded = ExportMetricsServiceRequest::decode(buffer.encode(&[], &[])).unwrap();
        let Some(Data::Gauge(g)) = &decoded.resource_metrics[0].scope_metrics[0].metrics[0].data
        else {
            panic!()
        };
        assert_eq!(g.data_points.len(), 1);
        assert_eq!(g.data_points[0].time_unix_nano, 99);
    }

    #[test]
    fn handles_large_fields_varint_boundaries_and_cache_eviction() {
        let mut buffer = GaugeBuffer::new();
        let stat = SAIStat::new("x".repeat(16384), u32::MAX, u32::MAX, 1 << 63);
        for time in 0..100 {
            buffer.push(&stat, time);
        }
        let decoded = ExportMetricsServiceRequest::decode(buffer.encode(&[], &[])).unwrap();
        let Some(Data::Gauge(g)) = &decoded.resource_metrics[0].scope_metrics[0].metrics[0].data
        else {
            panic!()
        };
        for (time, p) in g.data_points.iter().enumerate() {
            assert_eq!(
                p,
                &OtelDataPoint::from_sai_stat(&stat, time as u64).to_proto()
            );
        }
        buffer.clear();
        for id in 0..=MAX_CACHED_SERIES {
            buffer.push(&SAIStat::new("a", 1, id as u32, 0), 1);
        }
        buffer.clear();
        assert!(buffer.index.is_empty());
        assert!(buffer.series.is_empty());
    }

    #[test]
    fn canonical_stat_names_and_cached_attributes_match_message_conversion() {
        use crate::message::otel::OtelGauge;
        let mut buffer = GaugeBuffer::new();
        for type_id in [1, 21, 24, 26, u32::MAX] {
            let stat = SAIStat::new("object", type_id, 0, 123);
            for time in [10, 20] {
                buffer.push(&stat, time);
            }
        }
        let decoded = ExportMetricsServiceRequest::decode(buffer.encode(&[], &[])).unwrap();
        for (metric, type_id) in decoded.resource_metrics[0].scope_metrics[0]
            .metrics
            .iter()
            .zip([1, 21, 24, 26, u32::MAX])
        {
            let stat = SAIStat::new("object", type_id, 0, 123);
            let reference = OtelGauge::from_sai_stat(&stat, 10);
            assert_eq!(metric.name, reference.name);
            assert_eq!(metric.description, reference.description);
            let Some(Data::Gauge(gauge)) = &metric.data else {
                panic!()
            };
            assert_eq!(gauge.data_points.len(), 2);
            for (point, time) in gauge.data_points.iter().zip([10, 20]) {
                assert_eq!(*point, OtelDataPoint::from_sai_stat(&stat, time).to_proto());
            }
            assert!(gauge.data_points[0]
                .attributes
                .iter()
                .all(|a| a.key != "sai_type_id" && a.key != "sai_stat_id"));
        }
    }

    #[test]
    fn shared_plan_slots_preserve_pending_values_and_invalidate_on_eviction() {
        let mut buffer = GaugeBuffer::new();
        let stat = SAIStat::new("Ethernet0", 1, 0, 11);
        buffer.push(&stat, 1);
        let metadata: Arc<[SAIStatMetadata]> = vec![
            SAIStatMetadata::new("Ethernet0", 1, 0),
            SAIStatMetadata::new("Ethernet4", 1, 1),
        ]
        .into();
        let slots = buffer.shared_slots(&metadata);
        assert!(Arc::ptr_eq(&slots, &buffer.shared_slots(&metadata)));
        buffer.push_slot(slots[0], 2, 22);
        buffer.push_slot(slots[1], 3, 33);
        let decoded = ExportMetricsServiceRequest::decode(buffer.encode(&[], &[])).unwrap();
        let Some(Data::Gauge(g)) = &decoded.resource_metrics[0].scope_metrics[0].metrics[0].data
        else {
            panic!()
        };
        assert_eq!(
            g.data_points
                .iter()
                .map(|p| p.time_unix_nano)
                .collect::<Vec<_>>(),
            vec![1, 2]
        );
        buffer.clear();
        for id in 0..=MAX_CACHED_SERIES {
            buffer.push(&SAIStat::new("many", 1, id as u32, 0), 1);
        }
        buffer.clear();
        assert!(buffer.plans.is_empty());
        let rebuilt = buffer.shared_slots(&metadata);
        buffer.push_slot(rebuilt[0], 99, 44);
        let decoded = ExportMetricsServiceRequest::decode(buffer.encode(&[], &[])).unwrap();
        let Some(Data::Gauge(g)) = &decoded.resource_metrics[0].scope_metrics[0].metrics[0].data
        else {
            panic!()
        };
        assert_eq!(
            g.data_points[0],
            OtelDataPoint::from_sai_stat(&SAIStat::new("Ethernet0", 1, 0, 44), 99).to_proto()
        );
    }

    #[test]
    #[ignore = "manual sender-only calibration; not an RPC throughput result"]
    fn encoding_throughput() {
        let stats: Vec<_> = (0..500)
            .map(|i| SAIStat::new(format!("Ethernet{i}"), 1, i, i as u64))
            .collect();
        let mut buffer = GaugeBuffer::new();
        let start = std::time::Instant::now();
        let mut points = 0u64;
        while start.elapsed() < std::time::Duration::from_secs(5) {
            for record in 0..200 {
                for stat in &stats {
                    buffer.push(stat, 1_700_000_000_000_000_000 + points + record);
                }
            }
            std::hint::black_box(buffer.encode(&[], &[]));
            buffer.clear();
            points += 100_000;
        }
        println!(
            "actual_actor_encode_Mpoints_s={:.3}",
            points as f64 / start.elapsed().as_secs_f64() / 1e6
        );
    }
}
