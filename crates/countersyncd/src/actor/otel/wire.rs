//! Encode the fixed SAI Gauge schema without constructing a protobuf object tree.
//! All field numbers/types are from opentelemetry-proto 0.25. Decode-equivalence
//! tests below guard this wire-compatible optimization. No sample is aggregated.
use crate::message::saistats::SAIStat;
use ahash::AHashMap;
use opentelemetry_proto::tonic::collector::metrics::v1::{
    ExportMetricsServiceRequest, ExportMetricsServiceResponse,
};
use prost::bytes::{BufMut, Bytes, BytesMut};
use std::sync::Arc;
use tonic::codec::{Codec, EncodeBuf, Encoder, ProstCodec};

// Bound retained series metadata; active requests are bounded by the actor's
// counter threshold. Clear the cache after a flush when this limit is exceeded.
const MAX_CACHED_SERIES: usize = 4096;
const MAX_RETAINED_BYTES: usize = 32 * 1024 * 1024;

#[derive(Hash, PartialEq, Eq)]
struct SeriesKey {
    object: Arc<str>,
    type_id: u32,
    stat_id: u32,
}
struct Series {
    metadata: Vec<u8>,
    attributes: Vec<u8>,
    points: Vec<u8>,
}
pub(super) struct GaugeBuffer {
    index: AHashMap<SeriesKey, usize>,
    series: Vec<Series>,
    active: Vec<usize>,
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
    pub(super) fn new() -> Self {
        Self {
            index: AHashMap::new(),
            series: Vec::new(),
            active: Vec::new(),
        }
    }
    pub(super) fn push(&mut self, stat: &SAIStat, timestamp: u64) {
        let key = SeriesKey {
            object: stat.object_name.clone(),
            type_id: stat.type_id,
            stat_id: stat.stat_id,
        };
        let slot = *self.index.entry(key).or_insert_with(|| {
            let mut metadata = Vec::new();
            string(
                &mut metadata,
                10,
                &format!("sai_counter_type_{}_stat_{}", stat.type_id, stat.stat_id),
            );
            string(
                &mut metadata,
                18,
                &format!(
                    "SAI counter for object {} (type:{}, stat:{})",
                    stat.object_name, stat.type_id, stat.stat_id
                ),
            );
            let mut attributes = Vec::new();
            for (key, value) in [
                ("object_name", stat.object_name.to_string()),
                ("sai_type_id", stat.type_id.to_string()),
                ("sai_stat_id", stat.stat_id.to_string()),
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
        header(&mut series.points, 10, 18 + series.attributes.len());
        // NumberDataPoint.time_unix_nano (fixed64), value.as_int (sfixed64).
        series.points.put_u8(25);
        series.points.put_u64_le(timestamp);
        series.points.put_u8(49);
        series.points.put_i64_le(stat.counter as i64);
        series.points.extend_from_slice(&series.attributes);
    }
    pub(super) fn encode(&self, resource: &[u8], scope: &[u8]) -> Bytes {
        let scope_len = field_len(scope.len())
            + self
                .active
                .iter()
                .map(|&i| {
                    let s = &self.series[i];
                    field_len(s.metadata.len() + field_len(s.points.len()))
                })
                .sum::<usize>();
        let resource_len = field_len(resource.len()) + field_len(scope_len);
        let mut out = BytesMut::with_capacity(field_len(resource_len));
        header(&mut out, 10, resource_len);
        header(&mut out, 10, resource.len());
        out.extend_from_slice(resource);
        header(&mut out, 18, scope_len);
        header(&mut out, 10, scope.len());
        out.extend_from_slice(scope);
        for &i in &self.active {
            let s = &self.series[i];
            header(&mut out, 18, s.metadata.len() + field_len(s.points.len()));
            out.extend_from_slice(&s.metadata);
            header(&mut out, 42, s.points.len());
            out.extend_from_slice(&s.points);
        }
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
                .map(|s| s.points.capacity() + s.metadata.capacity() + s.attributes.capacity())
                .sum::<usize>()
                > MAX_RETAINED_BYTES
        {
            self.index.clear();
            self.series.clear();
        }
    }
}

// Tonic still supplies standard unary gRPC framing, transport and flow control.
// Bytes clones used for retries share one immutable, already encoded request.
pub(super) struct EncodedMetricsCodec;
pub(super) struct EncodedMetricsEncoder;
impl Encoder for EncodedMetricsEncoder {
    type Item = Bytes;
    type Error = tonic::Status;
    fn encode(&mut self, item: Bytes, dst: &mut EncodeBuf<'_>) -> Result<(), Self::Error> {
        dst.put_slice(&item);
        Ok(())
    }
}
impl Codec for EncodedMetricsCodec {
    type Encode = Bytes;
    type Decode = ExportMetricsServiceResponse;
    type Encoder = EncodedMetricsEncoder;
    type Decoder =
        <ProstCodec<ExportMetricsServiceRequest, ExportMetricsServiceResponse> as Codec>::Decoder;
    fn encoder(&mut self) -> Self::Encoder {
        EncodedMetricsEncoder
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
            assert_eq!(
                metric.name,
                format!("sai_counter_type_{}_stat_{}", stat.type_id, stat.stat_id)
            );
            assert_eq!(
                metric.description,
                format!(
                    "SAI counter for object {} (type:{}, stat:{})",
                    stat.object_name, stat.type_id, stat.stat_id
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
