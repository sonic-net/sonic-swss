use std::{
    collections::BTreeMap,
    fmt::{Display, Formatter},
    pin::Pin,
    sync::Arc,
    time::Duration,
};
use tokio::{
    select,
    sync::{mpsc::Receiver, oneshot},
    time::{sleep_until, Instant as TokioInstant, Sleep},
};
use log::{debug, error, info, warn};
use tonic::transport::{Channel, Endpoint};
use opentelemetry::ExportError;
use opentelemetry_proto::tonic::{
    collector::metrics::v1::{
        metrics_service_client::MetricsServiceClient,
        ExportMetricsServiceRequest,
    },
    common::v1::{
        any_value::Value,
        AnyValue,
        InstrumentationScope,
        KeyValue as ProtoKeyValue,
    },
    metrics::v1::{
        metric::Data,
        AggregationTemporality,
        Gauge as ProtoGauge,
        Histogram as ProtoHistogram,
        Metric,
        ResourceMetrics,
        ScopeMetrics,
    },
    resource::v1::Resource as ProtoResource,
};
use prost::Message;
use crate::message::{
    aggregator::{AggregatedStatsMessage, Heatmap},
    otel::OtelMetrics,
};
use crate::utilities::{record_comm_stats, ChannelLabel};

const INITIAL_BACKOFF_DELAY_SECS: u64 = 1;
const MAX_BACKOFF_DELAY_SECS: u64 = 10;
const MAX_EXPORT_RETRIES: u64 = 30;
pub const DEFAULT_MAX_EXPORT_BYTES: usize = 3 * 1024 * 1024;

fn heatmap_payload_units(heatmap: &Heatmap) -> usize {
    1usize
        .saturating_add(heatmap.explicit_bounds.len())
        .saturating_add(heatmap.bucket_counts.len())
}

/// Configuration for the OtelActor
#[derive(Debug, Clone)]
pub struct OtelActorConfig {
    /// OpenTelemetry collector endpoint
    pub collector_endpoint: String,
    /// Max counters to accumulate before forcing an export
    pub max_counters_per_export: usize,
    /// Maximum encoded OTLP request payload in bytes.
    pub max_export_bytes: usize,
    /// Max time to wait before flushing buffered metrics
    pub flush_timeout: Duration,
}

impl Default for OtelActorConfig {
    fn default() -> Self {
        Self {
            collector_endpoint: "http://localhost:4317".to_string(),
            max_counters_per_export: 10_000,
            max_export_bytes: DEFAULT_MAX_EXPORT_BYTES,
            flush_timeout: Duration::from_secs(1),
        }
    }
}

#[derive(Debug)]
pub struct OtelActorExportError(String);

impl std::error::Error for OtelActorExportError {}

impl ExportError for OtelActorExportError {
    fn exporter_name(&self) -> &'static str {
        "Otel client exporter"
    }
}

impl Display for OtelActorExportError {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

fn encoded_len_varint(mut value: usize) -> usize {
    let mut len = 1;
    while value >= 0x80 {
        value >>= 7;
        len += 1;
    }
    len
}

fn length_delimited_field_len(payload_len: usize) -> usize {
    1 + encoded_len_varint(payload_len) + payload_len
}

fn request_len(resource_len: usize, scope_len: usize, metric_fields_len: usize) -> usize {
    let scope_metrics_len = length_delimited_field_len(scope_len) + metric_fields_len;
    let resource_metrics_len =
        length_delimited_field_len(resource_len) + length_delimited_field_len(scope_metrics_len);
    length_delimited_field_len(resource_metrics_len)
}

fn export_request(
    resource: &ProtoResource,
    instrumentation_scope: &InstrumentationScope,
    metrics: Vec<Metric>,
) -> ExportMetricsServiceRequest {
    ExportMetricsServiceRequest {
        resource_metrics: vec![ResourceMetrics {
            resource: Some(resource.clone()),
            scope_metrics: vec![ScopeMetrics {
                scope: Some(instrumentation_scope.clone()),
                schema_url: String::new(),
                metrics,
            }],
            schema_url: String::new(),
        }],
    }
}

pub fn default_resource() -> ProtoResource {
    ProtoResource {
        attributes: vec![ProtoKeyValue {
            key: "service.name".to_string(),
            value: Some(AnyValue {
                value: Some(Value::StringValue("countersyncd".to_string())),
            }),
        }],
        dropped_attributes_count: 0,
    }
}

pub fn default_instrumentation_scope() -> InstrumentationScope {
    InstrumentationScope {
        name: "countersyncd".to_string(),
        version: "1.0".to_string(),
        attributes: Vec::new(),
        dropped_attributes_count: 0,
    }
}

fn finish_request(
    requests: &mut Vec<ExportMetricsServiceRequest>,
    resource: &ProtoResource,
    instrumentation_scope: &InstrumentationScope,
    metrics: &mut Vec<Metric>,
    max_export_bytes: usize,
) -> Result<(), OtelActorExportError> {
    if metrics.is_empty() {
        return Ok(());
    }
    let request = export_request(resource, instrumentation_scope, std::mem::take(metrics));
    let encoded_len = request.encoded_len();
    if encoded_len > max_export_bytes {
        return Err(OtelActorExportError(format!(
            "internal OTLP batching error: encoded request is {} bytes, exceeding max_export_bytes {}",
            encoded_len, max_export_bytes
        )));
    }
    requests.push(request);
    Ok(())
}

/// Splits metrics into exact-size OTLP requests in one pass over their encoded data.
pub fn split_export_requests(
    resource: &ProtoResource,
    instrumentation_scope: &InstrumentationScope,
    metrics: Vec<Metric>,
    max_export_bytes: usize,
) -> Result<Vec<ExportMetricsServiceRequest>, OtelActorExportError> {
    if max_export_bytes == 0 {
        return Err(OtelActorExportError(
            "max_export_bytes must be greater than zero".to_string(),
        ));
    }

    let resource_len = resource.encoded_len();
    let scope_len = instrumentation_scope.encoded_len();
    let mut requests = Vec::new();
    let mut request_metrics = Vec::new();
    let mut request_metric_fields_len = 0usize;

    for mut metric in metrics {
        let histogram = match metric.data.take() {
            Some(Data::Histogram(histogram)) if !histogram.data_points.is_empty() => histogram,
            data => {
                metric.data = data;
                let metric_field_len = length_delimited_field_len(metric.encoded_len());
                if request_len(
                    resource_len,
                    scope_len,
                    request_metric_fields_len + metric_field_len,
                ) > max_export_bytes
                {
                    finish_request(
                        &mut requests,
                        resource,
                        instrumentation_scope,
                        &mut request_metrics,
                        max_export_bytes,
                    )?;
                    request_metric_fields_len = 0;
                }
                if request_len(resource_len, scope_len, metric_field_len) > max_export_bytes {
                    return Err(OtelActorExportError(format!(
                        "OTLP metric '{}' requires {} encoded bytes and exceeds max_export_bytes {}",
                        metric.name,
                        request_len(resource_len, scope_len, metric_field_len),
                        max_export_bytes
                    )));
                }
                request_metric_fields_len += metric_field_len;
                request_metrics.push(metric);
                continue;
            }
        };

        let mut histogram_template = histogram;
        let data_points = std::mem::take(&mut histogram_template.data_points);
        let histogram_base_len = histogram_template.encoded_len();
        let mut metric_without_data = metric.clone();
        metric_without_data.data = None;
        let metric_without_data_len = metric_without_data.encoded_len();
        metric.data = Some(Data::Histogram(histogram_template.clone()));
        let empty_metric_len = metric.encoded_len();
        let histogram_key_len = empty_metric_len
            .checked_sub(
                metric_without_data_len
                    + histogram_base_len
                    + encoded_len_varint(histogram_base_len),
            )
            .ok_or_else(|| {
                OtelActorExportError("failed to calculate OTLP histogram envelope size".to_string())
            })?;
        let mut segment_points = Vec::new();
        let mut segment_point_fields_len = 0usize;

        for point in data_points {
            let point_field_len = length_delimited_field_len(point.encoded_len());
            loop {
                let histogram_len = histogram_base_len + segment_point_fields_len + point_field_len;
                let metric_len = metric_without_data_len
                    + histogram_key_len
                    + encoded_len_varint(histogram_len)
                    + histogram_len;
                let metric_field_len = length_delimited_field_len(metric_len);
                if request_len(
                    resource_len,
                    scope_len,
                    request_metric_fields_len + metric_field_len,
                ) <= max_export_bytes
                {
                    segment_point_fields_len += point_field_len;
                    segment_points.push(point);
                    break;
                }

                if !segment_points.is_empty() {
                    let mut segment = metric.clone();
                    let Some(Data::Histogram(segment_histogram)) = segment.data.as_mut() else {
                        unreachable!("histogram metric template")
                    };
                    segment_histogram.data_points = std::mem::take(&mut segment_points);
                    request_metrics.push(segment);
                    finish_request(
                        &mut requests,
                        resource,
                        instrumentation_scope,
                        &mut request_metrics,
                        max_export_bytes,
                    )?;
                    request_metric_fields_len = 0;
                    segment_point_fields_len = 0;
                    continue;
                }

                if !request_metrics.is_empty() {
                    finish_request(
                        &mut requests,
                        resource,
                        instrumentation_scope,
                        &mut request_metrics,
                        max_export_bytes,
                    )?;
                    request_metric_fields_len = 0;
                    continue;
                }

                return Err(OtelActorExportError(format!(
                    "single histogram data point for metric '{}' requires {} encoded bytes and exceeds max_export_bytes {}",
                    metric.name,
                    request_len(resource_len, scope_len, metric_field_len),
                    max_export_bytes
                )));
            }
        }

        if !segment_points.is_empty() {
            let mut segment = metric;
            let Some(Data::Histogram(segment_histogram)) = segment.data.as_mut() else {
                unreachable!("histogram metric template")
            };
            segment_histogram.data_points = segment_points;
            request_metric_fields_len += length_delimited_field_len(segment.encoded_len());
            request_metrics.push(segment);
        }
    }

    finish_request(
        &mut requests,
        resource,
        instrumentation_scope,
        &mut request_metrics,
        max_export_bytes,
    )?;
    Ok(requests)
}

fn build_proto_metrics(
    buffer: &[OtelMetrics],
    heatmaps: &[(Option<Arc<str>>, Arc<[Heatmap]>)],
) -> Vec<Metric> {
    let gauge_count = buffer.iter().map(|metrics| metrics.gauges.len()).sum();
    let mut proto_metrics = Vec::with_capacity(gauge_count);
    for otel_metrics in buffer {
        for gauge in &otel_metrics.gauges {
            proto_metrics.push(Metric {
                name: gauge.name.clone(),
                description: gauge.description.clone(),
                unit: gauge.unit.clone(),
                metadata: Vec::new(),
                data: Some(Data::Gauge(ProtoGauge {
                    data_points: gauge
                        .data_points
                        .iter()
                        .map(|point| point.to_proto())
                        .collect(),
                })),
            });
        }
    }

    let mut histograms = BTreeMap::<(u32, u32), (ProtoHistogram, &'static str)>::new();
    for (key, heatmaps) in heatmaps {
        for heatmap in heatmaps.iter() {
            histograms
                .entry((heatmap.type_id, heatmap.stat_id))
                .or_insert_with(|| {
                    (
                        ProtoHistogram {
                            data_points: Vec::new(),
                            aggregation_temporality: AggregationTemporality::Delta as i32,
                        },
                        heatmap.unit,
                    )
                })
                .0
                .data_points
                .push(heatmap.to_proto(key.as_deref()));
        }
    }
    proto_metrics.extend(
        histograms
            .into_iter()
            .map(|((type_id, stat_id), (histogram, unit))| Metric {
                name: format!("sai_counter_type_{}_stat_{}_heatmap", type_id, stat_id),
                description: format!("SAI counter heatmap (type:{}, stat:{})", type_id, stat_id),
                unit: unit.to_string(),
                metadata: Vec::new(),
                data: Some(Data::Histogram(histogram)),
            }),
    );
    proto_metrics
}

/// Actor that receives SAI statistics and exports to OpenTelemetry
pub struct OtelActor {
    stats_receiver: Receiver<AggregatedStatsMessage>,
    config: OtelActorConfig,
    shutdown_notifier: Option<oneshot::Sender<()>>,
    client: Option<MetricsServiceClient<Channel>>,

    // Pre-allocated reusable structures
    resource: ProtoResource,
    instrumentation_scope: InstrumentationScope,

    // Batching
    buffer: Vec<OtelMetrics>,
    heatmaps: Vec<(Option<Arc<str>>, Arc<[Heatmap]>)>,
    buffered_counters: usize,
    flush_deadline: TokioInstant,

    // Statistics tracking
    messages_received: u64,
    exports_performed: u64,
    export_failures: u64,
    console_reports: u64,

    // Reconnecting tracking
    consecutive_failures: u64,

    // Shutdown flag
    should_shutdown: bool,
}

impl OtelActor {
    /// Creates a new OtelActor instance
    pub async fn new(
        stats_receiver: Receiver<AggregatedStatsMessage>,
        config: OtelActorConfig,
        shutdown_notifier: oneshot::Sender<()>,
    ) -> Result<OtelActor, Box<dyn std::error::Error>> {
        if config.max_export_bytes == 0 {
            return Err(Box::new(OtelActorExportError(
                "max_export_bytes must be greater than zero".to_string(),
            )));
        }
        let client = None;

        // Pre-create reusable resource
        let resource = default_resource();

        // Pre-create reusable instrumentation scope
        let instrumentation_scope = default_instrumentation_scope();

        info!(
            "OtelActor initialized - endpoint: {}",
            config.collector_endpoint
        );

        let flush_deadline = TokioInstant::now() + config.flush_timeout;

        Ok(OtelActor {
            stats_receiver,
            config,
            shutdown_notifier: Some(shutdown_notifier),
            client,
            resource,
            instrumentation_scope,
            buffer: Vec::new(),
            heatmaps: Vec::new(),
            buffered_counters: 0,
            flush_deadline,
            messages_received: 0,
            exports_performed: 0,
            export_failures: 0,
            console_reports: 0,
            consecutive_failures: 0,
            should_shutdown: false,
        })
    }

    /// Main run loop
    pub async fn run(mut self) -> Result<(), Box<dyn ExportError>> {
        info!("OtelActor started");

        let mut flush_timer = Box::pin(sleep_until(self.flush_deadline));
        let mut run_error: Option<Box<dyn ExportError>> = None;

        loop {
            select! {
                stats_msg = self.stats_receiver.recv() => {
                    match stats_msg {
                        Some(stats) => {
                            record_comm_stats(
                                ChannelLabel::IpfixToOtel,
                                self.stats_receiver.len(),
                            );
                            if let Err(e) = self.handle_stats_message(stats).await {
                                run_error = Some(e);
                                break;
                            }
                            self.reset_flush_timer(&mut flush_timer);
                        }
                        _none => {
                            info!("Stats receiver channel closed, shutting down OtelActor");
                            break;
                        }
                    }
                }
                _ = &mut flush_timer => {
                    if let Err(e) = self.flush_buffer().await {
                        run_error = Some(e);
                        break;
                    }
                    self.reset_flush_timer(&mut flush_timer);
                }
            }

            // Check for shutdown flag
            if self.should_shutdown {
                info!("Shutdown flag set, exiting Otel run loop");
                break;
            }
        }

        // Flush any remaining buffered metrics before shutdown
        if run_error.is_none() {
            if let Err(e) = self.flush_buffer().await {
                run_error = Some(e);
            }
        }
        self.shutdown().await;
        match run_error {
            Some(e) => Err(e),
            None => Ok(()),
        }
    }

    /// Handle incoming SAI statistics message
    async fn handle_stats_message(
        &mut self,
        message: AggregatedStatsMessage,
    ) -> Result<(), Box<dyn ExportError>> {
        self.messages_received += 1;

        let stats = message.stats;

        debug!(
            "Received SAI stats with {} entries, observation_time: {}",
            stats.stats.len(),
            stats.observation_time
        );

        let was_empty = self.buffer.is_empty() && self.heatmaps.is_empty();

        // Convert to OTel format using message types and buffer
        let otel_metrics = OtelMetrics::from_sai_stats(&stats);
        let counters_in_message = stats.stats.len();

        if log::log_enabled!(log::Level::Debug) {
            self.print_otel_metrics(&otel_metrics).await;
        }

        self.buffer.push(otel_metrics);
        let heatmap_payload = message
            .heatmaps
            .iter()
            .map(heatmap_payload_units)
            .fold(0usize, usize::saturating_add);
        if heatmap_payload != 0 {
            self.heatmaps.push((message.key, message.heatmaps));
        }
        self.buffered_counters = self
            .buffered_counters
            .saturating_add(counters_in_message)
            .saturating_add(heatmap_payload);

        // Start timeout when buffer transitions from empty to non-empty
        if was_empty {
            self.flush_deadline = TokioInstant::now() + self.config.flush_timeout;
        }

        // This inexpensive counter threshold controls flush cadence. Exact
        // protobuf byte limits are enforced when the buffer is serialized.
        if self.buffered_counters >= self.config.max_counters_per_export {
            self.flush_buffer().await?;
            self.flush_deadline = TokioInstant::now() + self.config.flush_timeout;
        }

        Ok(())
    }

    async fn print_otel_metrics(&mut self, otel_metrics: &OtelMetrics) {
        self.console_reports += 1;

        debug!(
            "[OTel Report #{}] Service: {}, Scope: {} v{}, Total Gauges: {}, Messages Received: {}, Exports: {} (Failures: {})",
            self.console_reports,
            otel_metrics.service_name,
            otel_metrics.scope_name,
            otel_metrics.scope_version,
            otel_metrics.len(),
            self.messages_received,
            self.exports_performed,
            self.export_failures
        );

        if !otel_metrics.is_empty() {
            debug!("Gauge Metrics:");
            for (index, gauge) in otel_metrics.gauges.iter().enumerate() {
                let data_point = &gauge.data_points[0];

                debug!("[{:3}] Gauge: {}", index + 1, gauge.name);
                debug!("Value: {}", data_point.value);
                debug!("Unit: {}", gauge.unit);
                debug!("Time: {}ns", data_point.time_unix_nano);
                debug!("Description: {}", gauge.description);

                if !data_point.attributes.is_empty() {
                    debug!("Attributes:");
                    for attr in &data_point.attributes {
                        debug!("  - {}={}", attr.key, attr.value);
                    }
                }

                debug!("Raw Gauge: {:#?}", gauge);
            }
        }
    }

    // Exponential backoff
    async fn backoff(&self, attempt: u64) {
        let delay_secs = std::cmp::min(INITIAL_BACKOFF_DELAY_SECS * 2u64.pow(attempt as u32 - 1), MAX_BACKOFF_DELAY_SECS);
        tokio::time::sleep(Duration::from_secs(delay_secs)).await;
    }

    // Get or create the Otel MetricsServiceClient
    fn get_client(&mut self) -> Option<&mut MetricsServiceClient<Channel>> {
        if self.client.is_none() {
            let endpoint = match self.config.collector_endpoint.parse::<Endpoint>() {
                Ok(e) => e,
                Err(e) => {
                    warn!("Invalid Otel endpoint: {}", e);
                    return None;
                }
            };

            let channel = endpoint.connect_lazy();
            self.client = Some(MetricsServiceClient::new(channel));
        }

        self.client.as_mut()
    }

    async fn send_request(
        &mut self,
        request: ExportMetricsServiceRequest,
    ) -> Result<(), Box<dyn ExportError>> {
        for attempt in 1..=MAX_EXPORT_RETRIES {
            // Ensure we have a client
            let client = match self.get_client() {
                Some(c) => c, // Use existing or newly created client
                _none => { // Failed to create client
                    self.client = None;
                    self.backoff(attempt).await; // Wait before retrying
                    continue;
                }
            };

            // Attempt to send the request
            match client.export(request.clone()).await {
                Ok(_) => { // Successful export
                    self.exports_performed += 1;
                    self.consecutive_failures = 0;
                    return Ok(());
                }
                Err(e) => {
                    warn!("Export attempt {} failed: {}", attempt, e);
                    self.client = None; // Drop broken client
                    self.consecutive_failures += 1;
                    self.backoff(attempt).await; // Wait before retrying
                }
            }
        }

        // All retries exhausted
        Err(Box::new(OtelActorExportError("Max export retries exceeded".to_string())))
    }

    // Export buffered metrics to OpenTelemetry collector 
    async fn flush_buffer(&mut self) -> Result<(), Box<dyn ExportError>> {
        if self.buffer.is_empty() && self.heatmaps.is_empty() {
            return Ok(());
        }

        let proto_metrics = build_proto_metrics(&self.buffer, &self.heatmaps);

        if proto_metrics.is_empty() {
            self.buffer.clear();
            self.heatmaps.clear();
            self.buffered_counters = 0;
            return Ok(());
        }

        let requests = match split_export_requests(
            &self.resource,
            &self.instrumentation_scope,
            proto_metrics,
            self.config.max_export_bytes,
        ) {
            Ok(requests) => requests,
            Err(error) => {
                self.export_failures += 1;
                error!("Failed to construct bounded OTLP export: {}", error);
                return Err(Box::new(error));
            }
        };

        let mut first_error = None;
        for request in requests {
            if let Err(error) = self.send_request(request).await {
                self.export_failures += 1;
                error!(
                    "Failed to export buffered metrics (consecutive failures {}): {:?}",
                    self.consecutive_failures, error
                );
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
        }

        // Keep the logical batch intact until every size-bounded request has
        // completed its retry path, then retire the batch as one unit.
        self.buffer.clear();
        self.heatmaps.clear();
        self.buffered_counters = 0;
        match first_error {
            Some(error) => Err(error),
            None => Ok(()),
        }
    }

    fn reset_flush_timer(&self, timer: &mut Pin<Box<Sleep>>) {
        // Ensure the deadline is in the future to avoid immediate wakeups
        let now = TokioInstant::now();
        let deadline = if self.flush_deadline <= now {
            now + self.config.flush_timeout
        } else {
            self.flush_deadline
        };

        timer.as_mut().reset(deadline);
    }

    /// Shutdown the actor
    async fn shutdown(self) {
        info!("Shutting down OtelActor...");

        tokio::time::sleep(Duration::from_secs(1)).await;

        if let Some(notifier) = self.shutdown_notifier {
            let _ = notifier.send(());
        }

        info!(
            "OtelActor shutdown complete. {} messages, {} exports, {} failures",
            self.messages_received, self.exports_performed, self.export_failures
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::aggregator::{
        default_heatmap_layout, heatmap_schema, HeatmapLayout, HeatmapQuantity, HeatmapValueKind,
    };
    use opentelemetry_proto::tonic::metrics::v1::{HistogramDataPoint, NumberDataPoint};

    fn histogram_point(
        object_name: &str,
        quantity: HeatmapQuantity,
        layout: Arc<HeatmapLayout>,
    ) -> HistogramDataPoint {
        let mut bucket_counts = vec![0; layout.bucket_count()];
        let bucket = layout
            .explicit_bounds_u64()
            .partition_point(|bound| *bound < 64);
        bucket_counts[bucket] = 64;
        let value_kind = match quantity {
            HeatmapQuantity::DeltaBytes | HeatmapQuantity::DeltaCount => HeatmapValueKind::Delta,
            _ => HeatmapValueKind::CurrentOccupancy,
        };
        Heatmap {
            object_name: Arc::from(object_name),
            type_id: u32::MAX,
            stat_id: u32::MAX,
            start_time_unix_nano: u64::MAX - 1,
            time_unix_nano: u64::MAX,
            count: 64,
            sum: 4_096.0,
            min: 64,
            max: 64,
            explicit_bounds: layout.explicit_bounds(),
            bucket_counts,
            value_kind,
            quantity,
            unit: quantity.unit(),
            schema: heatmap_schema(value_kind, quantity, layout.explicit_bounds_u64()),
        }
        .to_proto(Some("profile_with_representative_name|PORT"))
    }

    fn histogram_metric(
        name: &str,
        quantity: HeatmapQuantity,
        layout: Arc<HeatmapLayout>,
        point_count: usize,
        object_name_len: usize,
    ) -> Metric {
        let object_name = "x".repeat(object_name_len);
        Metric {
            name: name.to_string(),
            description: format!("description for {name}"),
            unit: quantity.unit().to_string(),
            metadata: Vec::new(),
            data: Some(Data::Histogram(ProtoHistogram {
                data_points: (0..point_count)
                    .map(|_| histogram_point(&object_name, quantity, layout.clone()))
                    .collect(),
                aggregation_temporality: AggregationTemporality::Delta as i32,
            })),
        }
    }

    fn request_stats(requests: &[ExportMetricsServiceRequest]) -> (usize, usize) {
        let mut points = 0;
        let mut gauges = 0;
        for request in requests {
            assert!(request.encoded_len() <= DEFAULT_MAX_EXPORT_BYTES);
            let resource_metrics = &request.resource_metrics[0];
            assert_eq!(
                resource_metrics.resource.as_ref(),
                Some(&default_resource())
            );
            let scope_metrics = &resource_metrics.scope_metrics[0];
            assert_eq!(
                scope_metrics.scope.as_ref(),
                Some(&default_instrumentation_scope())
            );
            for metric in &scope_metrics.metrics {
                match metric.data.as_ref() {
                    Some(Data::Histogram(histogram)) => {
                        assert_eq!(
                            histogram.aggregation_temporality,
                            AggregationTemporality::Delta as i32
                        );
                        for point in &histogram.data_points {
                            assert_eq!(point.bucket_counts.iter().sum::<u64>(), point.count);
                        }
                        points += histogram.data_points.len();
                    }
                    Some(Data::Gauge(gauge)) => gauges += gauge.data_points.len(),
                    _ => {}
                }
            }
        }
        (points, gauges)
    }

    #[test]
    fn heatmap_flush_accounting_tracks_bounds_and_counts() {
        let heatmap = Heatmap {
            object_name: Arc::from("Ethernet0"),
            type_id: 1,
            stat_id: 2,
            start_time_unix_nano: 0,
            time_unix_nano: 1,
            count: 1,
            sum: 1.0,
            min: 1,
            max: 1,
            explicit_bounds: Arc::from([1.0, 2.0, 8.0]),
            bucket_counts: vec![1, 0, 0, 0],
            value_kind: HeatmapValueKind::Delta,
            quantity: HeatmapQuantity::DeltaCount,
            unit: "1",
            schema: heatmap_schema(
                HeatmapValueKind::Delta,
                HeatmapQuantity::DeltaCount,
                &[1, 2, 8],
            ),
        };

        assert_eq!(heatmap_payload_units(&heatmap), 8);
    }

    #[test]
    fn splits_4096_native_points_and_preserves_histogram_metadata() {
        let metric = histogram_metric(
            "native_level",
            HeatmapQuantity::Native,
            default_heatmap_layout(HeatmapQuantity::Native, None).unwrap(),
            4_096,
            32,
        );
        let requests = split_export_requests(
            &default_resource(),
            &default_instrumentation_scope(),
            vec![metric],
            DEFAULT_MAX_EXPORT_BYTES,
        )
        .unwrap();

        assert_eq!(requests.len(), 2);
        assert_eq!(
            requests
                .iter()
                .map(Message::encoded_len)
                .collect::<Vec<_>>(),
            vec![3_145_464, 2_081_276]
        );
        assert_eq!(request_stats(&requests), (4_096, 0));
        for request in requests {
            for metric in &request.resource_metrics[0].scope_metrics[0].metrics {
                assert_eq!(metric.name, "native_level");
                assert_eq!(metric.description, "description for native_level");
                assert_eq!(metric.unit, "1");
            }
        }
    }

    #[test]
    fn splits_4096_points_for_every_default_quantity() {
        for quantity in [
            HeatmapQuantity::DeltaBytes,
            HeatmapQuantity::AbsoluteBytes,
            HeatmapQuantity::AbsoluteCells,
            HeatmapQuantity::DeltaCount,
            HeatmapQuantity::Native,
        ] {
            let metric = histogram_metric(
                quantity.as_str(),
                quantity,
                default_heatmap_layout(quantity, Some(1_000)).unwrap(),
                4_096,
                32,
            );
            let requests = split_export_requests(
                &default_resource(),
                &default_instrumentation_scope(),
                vec![metric],
                DEFAULT_MAX_EXPORT_BYTES,
            )
            .unwrap();

            assert!(!requests.is_empty(), "{}", quantity.as_str());
            assert!(
                requests
                    .iter()
                    .all(|request| request.encoded_len() <= DEFAULT_MAX_EXPORT_BYTES),
                "{}",
                quantity.as_str()
            );
            assert_eq!(
                request_stats(&requests),
                (4_096, 0),
                "{}",
                quantity.as_str()
            );
        }
    }

    #[test]
    fn splits_512_max_custom_points_with_long_attributes() {
        let layout = HeatmapLayout::from_explicit_bounds_for(
            HeatmapQuantity::DeltaCount,
            (0..511).collect(),
        )
        .unwrap();
        let metric_name = "m".repeat(4_096);
        let metric = histogram_metric(
            &metric_name,
            HeatmapQuantity::DeltaCount,
            layout,
            512,
            4_096,
        );
        let requests = split_export_requests(
            &default_resource(),
            &default_instrumentation_scope(),
            vec![metric],
            DEFAULT_MAX_EXPORT_BYTES,
        )
        .unwrap();

        assert_eq!(
            requests
                .iter()
                .map(Message::encoded_len)
                .collect::<Vec<_>>(),
            vec![3_139_294, 3_139_294, 210_290]
        );
        assert_eq!(request_stats(&requests), (512, 0));
    }

    #[test]
    fn splits_mixed_gauges_and_histograms_without_losing_points() {
        let gauge = Metric {
            name: "gauge".to_string(),
            description: "gauge description".to_string(),
            unit: "1".to_string(),
            metadata: Vec::new(),
            data: Some(Data::Gauge(ProtoGauge {
                data_points: vec![NumberDataPoint::default()],
            })),
        };
        let histogram = histogram_metric(
            "native",
            HeatmapQuantity::Native,
            default_heatmap_layout(HeatmapQuantity::Native, None).unwrap(),
            4_096,
            128,
        );
        let requests = split_export_requests(
            &default_resource(),
            &default_instrumentation_scope(),
            vec![gauge, histogram],
            DEFAULT_MAX_EXPORT_BYTES,
        )
        .unwrap();

        assert_eq!(
            requests
                .iter()
                .map(Message::encoded_len)
                .collect::<Vec<_>>(),
            vec![3_144_772, 2_487_487]
        );
        assert_eq!(request_stats(&requests), (4_096, 1));
    }

    #[test]
    fn rejects_a_single_point_that_exceeds_the_cap() {
        let metric = histogram_metric(
            "too_large",
            HeatmapQuantity::Native,
            default_heatmap_layout(HeatmapQuantity::Native, None).unwrap(),
            1,
            4_096,
        );
        let error = split_export_requests(
            &default_resource(),
            &default_instrumentation_scope(),
            vec![metric],
            64,
        )
        .unwrap_err();
        assert!(error.to_string().contains("single histogram data point"));
        assert!(error.to_string().contains("max_export_bytes 64"));
    }

    #[tokio::test]
    async fn rejects_zero_export_byte_configuration() {
        let (_sender, receiver) = tokio::sync::mpsc::channel(1);
        let (shutdown, _shutdown_receiver) = oneshot::channel();
        let error = OtelActor::new(
            receiver,
            OtelActorConfig {
                max_export_bytes: 0,
                ..Default::default()
            },
            shutdown,
        )
        .await
        .err()
        .expect("zero max_export_bytes must fail");
        assert!(error.to_string().contains("greater than zero"));
    }

    #[tokio::test]
    async fn failed_size_split_retains_buffered_metrics() {
        let (_sender, receiver) = tokio::sync::mpsc::channel(1);
        let (shutdown, _shutdown_receiver) = oneshot::channel();
        let mut actor = OtelActor::new(
            receiver,
            OtelActorConfig {
                max_export_bytes: 64,
                ..Default::default()
            },
            shutdown,
        )
        .await
        .unwrap();
        actor.buffer.push(OtelMetrics::from_sai_stats(
            &crate::message::saistats::SAIStats::new(
                1,
                vec![crate::message::saistats::SAIStat {
                    object_name: "x".repeat(4_096),
                    type_id: 1,
                    stat_id: 1,
                    counter: 1,
                }],
            ),
        ));
        actor.buffered_counters = 1;

        assert!(actor.flush_buffer().await.is_err());
        assert_eq!(actor.buffer.len(), 1);
        assert_eq!(actor.buffered_counters, 1);
    }
}
