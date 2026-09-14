use crate::message::{otel::OtelMetrics, saistats::SAIStatsBatchMessage};
use crate::utilities::{record_comm_stats, ChannelLabel};
use log::{debug, error, info, warn};
use opentelemetry::ExportError;
use opentelemetry_proto::tonic::{
    common::v1::{any_value::Value, AnyValue, InstrumentationScope, KeyValue as ProtoKeyValue},
    resource::v1::Resource as ProtoResource,
};
use prost::Message;
use std::{
    fmt::{Display, Formatter},
    pin::Pin,
    time::Duration,
};
use tokio::{
    select,
    sync::{mpsc::Receiver, oneshot},
    time::{sleep_until, Instant as TokioInstant, Sleep},
};
use tonic::transport::{Channel, Endpoint};
#[path = "otel/wire.rs"]
mod wire;
use wire::{EncodedMetricsCodec, GaugeBuffer, WireRequest};
#[path = "otel/pool.rs"]
pub mod pool;
pub use pool::{OtelWorkerConfig, OtelWorkerPool};

const INITIAL_BACKOFF_DELAY_SECS: u64 = 1;
const MAX_BACKOFF_DELAY_SECS: u64 = 10;
const MAX_EXPORT_RETRIES: u64 = 30;

/// Configuration for the OtelActor
#[derive(Debug, Clone)]
pub struct OtelActorConfig {
    /// OpenTelemetry collector endpoint
    pub collector_endpoint: String,
    /// Max counters to accumulate before forcing an export
    pub max_counters_per_export: usize,
    /// Max time to wait before flushing buffered metrics
    pub flush_timeout: Duration,
}

impl Default for OtelActorConfig {
    fn default() -> Self {
        Self {
            collector_endpoint: "http://localhost:4317".to_string(),
            max_counters_per_export: 10_000,
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

/// Actor that receives SAI statistics and exports to OpenTelemetry
pub struct OtelActor {
    stats_receiver: Receiver<SAIStatsBatchMessage>,
    config: OtelActorConfig,
    shutdown_notifier: Option<oneshot::Sender<()>>,
    client: Option<tonic::client::Grpc<Channel>>,

    // Pre-allocated reusable structures
    resource: Vec<u8>,
    instrumentation_scope: Vec<u8>,

    // Batching
    buffer: GaugeBuffer,
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
    // Benchmark-only bounded concurrency experiment. Production stays single-flight.
    #[cfg(feature = "benchmark")]
    in_flight: tokio::task::JoinSet<Result<(), tonic::Status>>,
    #[cfg(feature = "benchmark")]
    max_in_flight: usize,
}

impl OtelActor {
    /// Creates a new OtelActor instance
    pub async fn new(
        stats_receiver: Receiver<SAIStatsBatchMessage>,
        config: OtelActorConfig,
        shutdown_notifier: oneshot::Sender<()>,
    ) -> Result<OtelActor, Box<dyn std::error::Error>> {
        let client = None;

        // Pre-create reusable resource
        let resource = ProtoResource {
            attributes: vec![ProtoKeyValue {
                key: "service.name".to_string(),
                value: Some(AnyValue {
                    value: Some(Value::StringValue("countersyncd".to_string())),
                }),
            }],
            dropped_attributes_count: 0,
        };

        // Pre-create reusable instrumentation scope
        let instrumentation_scope = InstrumentationScope {
            name: "countersyncd".to_string(),
            version: "1.0".to_string(),
            attributes: vec![],
            dropped_attributes_count: 0,
        };

        info!(
            "OtelActor initialized - endpoint: {}",
            config.collector_endpoint
        );

        if config.max_counters_per_export == 0 || config.flush_timeout.is_zero() {
            return Err("OTel batch size and flush timeout must be positive".into());
        }

        let flush_deadline = TokioInstant::now() + config.flush_timeout;

        Ok(OtelActor {
            stats_receiver,
            config,
            shutdown_notifier: Some(shutdown_notifier),
            client,
            resource: resource.encode_to_vec(),
            instrumentation_scope: instrumentation_scope.encode_to_vec(),
            buffer: GaugeBuffer::new(),
            buffered_counters: 0,
            flush_deadline,
            messages_received: 0,
            exports_performed: 0,
            export_failures: 0,
            console_reports: 0,
            consecutive_failures: 0,
            should_shutdown: false,
            #[cfg(feature = "benchmark")]
            in_flight: tokio::task::JoinSet::new(),
            #[cfg(feature = "benchmark")]
            max_in_flight: std::env::var("OTEL_MAX_IN_FLIGHT")
                .ok()
                .map(|s| s.parse::<usize>().expect("positive request concurrency"))
                .unwrap_or(1)
                .max(1),
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
                            if let Err(e) = self.handle_stats_batch(stats).await {
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
        #[cfg(feature = "benchmark")]
        if run_error.is_none() {
            while !self.in_flight.is_empty() {
                if let Err(error) = self.finish_export().await {
                    run_error = Some(error);
                    break;
                }
            }
        }
        self.shutdown().await;
        match run_error {
            Some(e) => Err(e),
            None => Ok(()),
        }
    }

    /// Handle an incoming batch in record order.
    async fn handle_stats_batch(
        &mut self,
        batch: SAIStatsBatchMessage,
    ) -> Result<(), Box<dyn ExportError>> {
        for stats in batch.iter() {
            self.messages_received += 1;

            debug!(
                "Received SAI stats with {} entries, observation_time: {}",
                stats.stats.len(),
                stats.observation_time
            );

            if log::log_enabled!(log::Level::Debug) {
                let otel_metrics = OtelMetrics::from_sai_stats(stats);
                self.print_otel_metrics(&otel_metrics).await;
            }
            if let crate::message::saistats::SAIStatsView::Shared { metadata, values } = stats.stats
            {
                // Resolve one template-generation plan per segment, not per
                // sample. Re-resolve after flush, which can evict the slot cache.
                let mut offset = 0;
                while offset < values.len() {
                    let slots = self.buffer.shared_slots(metadata);
                    let take = (self.config.max_counters_per_export - self.buffered_counters)
                        .min(values.len() - offset);
                    if self.buffered_counters == 0 {
                        self.flush_deadline = TokioInstant::now() + self.config.flush_timeout;
                    }
                    for i in offset..offset + take {
                        self.buffer
                            .push_slot(slots[i], stats.observation_time, values[i]);
                    }
                    offset += take;
                    self.buffered_counters += take;
                    if self.buffered_counters >= self.config.max_counters_per_export {
                        self.flush_buffer().await?;
                    }
                }
                continue;
            }
            for stat in stats.stats {
                if self.buffered_counters == 0 {
                    self.flush_deadline = TokioInstant::now() + self.config.flush_timeout;
                }
                self.buffer.push_ref(stat, stats.observation_time);
                self.buffered_counters += 1;
                if self.buffered_counters >= self.config.max_counters_per_export {
                    self.flush_buffer().await?;
                }
            }
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
        let delay_secs = std::cmp::min(
            INITIAL_BACKOFF_DELAY_SECS * 2u64.pow(attempt as u32 - 1),
            MAX_BACKOFF_DELAY_SECS,
        );
        tokio::time::sleep(Duration::from_secs(delay_secs)).await;
    }

    // Get or create the Otel MetricsServiceClient
    fn get_client(&mut self) -> Option<&mut tonic::client::Grpc<Channel>> {
        if self.client.is_none() {
            let endpoint = match self.config.collector_endpoint.parse::<Endpoint>() {
                Ok(e) => e,
                Err(e) => {
                    warn!("Invalid Otel endpoint: {}", e);
                    return None;
                }
            };

            let channel = endpoint.connect_lazy();
            self.client = Some(tonic::client::Grpc::new(channel));
        }

        self.client.as_mut()
    }

    async fn send_request(&mut self, request: WireRequest) -> Result<(), Box<dyn ExportError>> {
        for attempt in 1..=MAX_EXPORT_RETRIES {
            // Ensure we have a client
            let client = match self.get_client() {
                Some(c) => c, // Use existing or newly created client
                _none => {
                    // Failed to create client
                    self.client = None;
                    self.backoff(attempt).await; // Wait before retrying
                    continue;
                }
            };

            // Attempt to send the request
            let result = async {
                client
                    .ready()
                    .await
                    .map_err(|e| tonic::Status::unavailable(e.to_string()))?;
                let path = tonic::codegen::http::uri::PathAndQuery::from_static(
                    "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export",
                );
                client
                    .unary(
                        tonic::Request::new(request.clone()),
                        path,
                        EncodedMetricsCodec::for_request(&request),
                    )
                    .await
            }
            .await;
            match result {
                Ok(response) => {
                    if let Some(partial) = response.into_inner().partial_success {
                        if partial.rejected_data_points > 0 {
                            return Err(Box::new(OtelActorExportError(format!(
                                "Collector rejected {} data points: {}",
                                partial.rejected_data_points, partial.error_message
                            ))));
                        }
                    }
                    // Successful export
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
        Err(Box::new(OtelActorExportError(
            "Max export retries exceeded".to_string(),
        )))
    }

    // Export buffered metrics to OpenTelemetry collector
    async fn flush_buffer(&mut self) -> Result<(), Box<dyn ExportError>> {
        if self.buffered_counters == 0 {
            return Ok(());
        }
        let request = self
            .buffer
            .prepare(&self.resource, &self.instrumentation_scope);

        // Send the export request
        #[cfg(feature = "benchmark")]
        let result = if self.max_in_flight > 1 {
            self.queue_export(request.clone()).await
        } else {
            self.send_request(request.clone()).await
        };
        #[cfg(not(feature = "benchmark"))]
        let result = self.send_request(request.clone()).await;

        if let Err(e) = &result {
            self.export_failures += 1;
            error!(
                "Failed to export buffered metrics (consecutive failures {}): {:?}",
                self.consecutive_failures, e
            );
        }

        self.buffer.reclaim(request);
        self.buffer.clear();
        self.buffered_counters = 0;

        result
    }

    #[cfg(feature = "benchmark")]
    async fn finish_export(&mut self) -> Result<(), Box<dyn ExportError>> {
        match self
            .in_flight
            .join_next()
            .await
            .expect("an export is in flight")
        {
            Ok(Ok(())) => {
                self.exports_performed += 1;
                Ok(())
            }
            other => Err(Box::new(OtelActorExportError(format!(
                "concurrent benchmark export failed: {other:?}"
            )))),
        }
    }

    #[cfg(feature = "benchmark")]
    async fn queue_export(&mut self, request: WireRequest) -> Result<(), Box<dyn ExportError>> {
        if self.in_flight.len() >= self.max_in_flight {
            self.finish_export().await?;
        }
        let mut client = self
            .get_client()
            .ok_or_else(|| {
                Box::new(OtelActorExportError("invalid endpoint".into())) as Box<dyn ExportError>
            })?
            .clone();
        // On a current-thread runtime all these tasks and the channel driver
        // execute on the same CPU. The queue is bounded and every ACK is joined.
        // Fail fast instead of hiding failures with retries in this experiment.
        self.in_flight.spawn(async move {
            client
                .ready()
                .await
                .map_err(|e| tonic::Status::unavailable(e.to_string()))?;
            let path = tonic::codegen::http::uri::PathAndQuery::from_static(
                "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export",
            );
            let codec = EncodedMetricsCodec::for_request(&request);
            let response = client
                .unary(tonic::Request::new(request), path, codec)
                .await?;
            if let Some(partial) = response.into_inner().partial_success {
                if partial.rejected_data_points > 0 {
                    return Err(tonic::Status::data_loss(format!(
                        "{} rejected points",
                        partial.rejected_data_points
                    )));
                }
            }
            Ok(())
        });
        Ok(())
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

        // run() has already awaited the final export response. No grace sleep
        // is necessary: there are no background exports to drain.
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
    use crate::message::saistats::{SAIStat, SAIStatsBatch};
    use std::sync::Arc;
    use tokio::sync::mpsc;

    #[tokio::test]
    async fn handles_batch_records_in_order_and_counts_records() {
        let (_stats_sender, stats_receiver) = mpsc::channel(1);
        let (shutdown_sender, _shutdown_receiver) = oneshot::channel();
        let mut actor = OtelActor::new(
            stats_receiver,
            OtelActorConfig {
                max_counters_per_export: usize::MAX,
                ..OtelActorConfig::default()
            },
            shutdown_sender,
        )
        .await
        .unwrap();

        let mut batch = SAIStatsBatch::with_capacity(2, 3);
        batch.push_record(
            10,
            [
                SAIStat::new("Ethernet0", 1, 2, 3),
                SAIStat::new("Ethernet4", 1, 2, 4),
            ],
        );
        batch.push_record(20, [SAIStat::new("Ethernet8", 1, 2, 5)]);

        actor.handle_stats_batch(Arc::new(batch)).await.unwrap();

        assert_eq!(actor.messages_received, 2);
        assert_eq!(actor.buffered_counters, 3);
        use opentelemetry_proto::tonic::{
            collector::metrics::v1::ExportMetricsServiceRequest,
            metrics::v1::{metric::Data, number_data_point::Value},
        };
        let request = ExportMetricsServiceRequest::decode(
            actor
                .buffer
                .encode(&actor.resource, &actor.instrumentation_scope),
        )
        .unwrap();
        let metrics = &request.resource_metrics[0].scope_metrics[0].metrics;
        assert_eq!(metrics.len(), 3);
        for (metric, (value, time)) in metrics.iter().zip([(3, 10), (4, 10), (5, 20)]) {
            let Some(Data::Gauge(g)) = &metric.data else {
                panic!("expected Gauge")
            };
            assert_eq!(g.data_points[0].value, Some(Value::AsInt(value)));
            assert_eq!(g.data_points[0].time_unix_nano, time);
        }
    }
}
