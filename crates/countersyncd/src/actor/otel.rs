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
use log::{debug, error, info};
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
        Gauge as ProtoGauge,
        Metric,
        ResourceMetrics,
        ScopeMetrics,
    },
    resource::v1::Resource as ProtoResource,
};
use crate::message::{
    otel::OtelMetrics,
    saistats::SAIStatsMessage,
};

const INITIAL_BACKOFF_DELAY_SECS: u64 = 1;
const MAX_BACKOFF_DELAY_SECS: u64 = 10;
const MAX_EXPORT_RETRIES: u64 = 30;

/// Configuration for the OtelActor
#[derive(Debug, Clone)]
pub struct OtelActorConfig {
    /// Whether to print statistics to console
    pub print_to_console: bool,
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
            print_to_console: true,
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
    stats_receiver: Receiver<SAIStatsMessage>,
    config: OtelActorConfig,
    shutdown_notifier: Option<oneshot::Sender<()>>,
    client: Option<MetricsServiceClient<Channel>>,

    // Pre-allocated reusable structures
    resource: ProtoResource,
    instrumentation_scope: InstrumentationScope,

    // Batching
    buffer: Vec<OtelMetrics>,
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
        stats_receiver: Receiver<SAIStatsMessage>,
        config: OtelActorConfig,
        shutdown_notifier: oneshot::Sender<()>
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
            "OtelActor initialized - console: {}, endpoint: {}",
            config.print_to_console,
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
    pub async fn run(mut self) {
        info!("OtelActor started");

        let mut flush_timer = Box::pin(sleep_until(self.flush_deadline));

        loop {
            select! {
                stats_msg = self.stats_receiver.recv() => {
                    match stats_msg {
                        Some(stats) => {
                            self.handle_stats_message(stats).await;
                            self.reset_flush_timer(&mut flush_timer);
                        }
                        _none => {
                            info!("Stats receiver channel closed, shutting down OtelActor");
                            break;
                        }
                    }
                }
                _ = &mut flush_timer => {
                    self.flush_buffer().await;
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
        self.flush_buffer().await;
        self.shutdown().await;
    }

    /// Handle incoming SAI statistics message
    async fn handle_stats_message(&mut self, stats: SAIStatsMessage) {
        self.messages_received += 1;

        debug!("Received SAI stats with {} entries, observation_time: {}",
               stats.stats.len(), stats.observation_time);

        let was_empty = self.buffer.is_empty();

        // Convert to OTel format using message types and buffer
        let otel_metrics = OtelMetrics::from_sai_stats(&stats);
        let counters_in_message = stats.stats.len();

        if self.config.print_to_console {
            self.print_otel_metrics(&otel_metrics).await;
        }

        self.buffer.push(otel_metrics);
        self.buffered_counters += counters_in_message;

        // Start timeout when buffer transitions from empty to non-empty
        if was_empty {
            self.flush_deadline = TokioInstant::now() + self.config.flush_timeout;
        }

        // Force flush when counter threshold is reached
        if self.buffered_counters >= self.config.max_counters_per_export {
            self.flush_buffer().await;
            self.flush_deadline = TokioInstant::now() + self.config.flush_timeout;
        }
    }

    async fn print_otel_metrics(&mut self, otel_metrics: &OtelMetrics) {
        self.console_reports += 1;

        info!(
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
            info!("Gauge Metrics:");
            for (index, gauge) in otel_metrics.gauges.iter().enumerate() {
                let data_point = &gauge.data_points[0];

                info!("[{:3}] Gauge: {}", index + 1, gauge.name);
                info!("Value: {}", data_point.value);
                info!("Unit: {}", gauge.unit);
                info!("Time: {}ns", data_point.time_unix_nano);
                info!("Description: {}", gauge.description);

                if !data_point.attributes.is_empty() {
                    info!("Attributes:");
                    for attr in &data_point.attributes {
                        info!("  - {}={}", attr.key, attr.value);
                    }
                }

                info!("Raw Gauge: {:#?}", gauge);
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
                    error!("Invalid Otel endpoint: {}", e);
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
                    error!("Export attempt {} failed: {}", attempt, e);
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
    async fn flush_buffer(&mut self) {
        if self.buffer.is_empty() {
            return;
        }

        let mut proto_metrics: Vec<Metric> = Vec::new();

        for otel_metrics in &self.buffer {
            for gauge in &otel_metrics.gauges {
                let proto_data_points = gauge.data_points.iter()
                    .map(|dp| dp.to_proto())
                    .collect();

                let proto_gauge = ProtoGauge {
                    data_points: proto_data_points,
                };

                proto_metrics.push(Metric {
                    name: gauge.name.clone(),
                    description: gauge.description.clone(),
                    metadata: vec![],
                    data: Some(opentelemetry_proto::tonic::metrics::v1::metric::Data::Gauge(proto_gauge)),
                    ..Default::default()
                });
            }
        }

        if proto_metrics.is_empty() {
            self.buffer.clear();
            self.buffered_counters = 0;
            return;
        }

        let resource_metrics = ResourceMetrics {
            resource: Some(self.resource.clone()),
            scope_metrics: vec![ScopeMetrics {
                scope: Some(self.instrumentation_scope.clone()),
                schema_url: String::new(),
                metrics: proto_metrics,
            }],
            schema_url: String::new(),
        };

        let request = ExportMetricsServiceRequest {
            resource_metrics: vec![resource_metrics],
        };

        // Send the export request
        let result = self.send_request(request).await;

        match result {
            Ok(_) => {
                debug!("Successfully exported buffered metrics");
            }
            Err(e) => {
                // Handle export failure
                self.export_failures += 1;
                error!(
                    "Failed to export buffered metrics (consecutive failures {}): {:?}",
                    self.consecutive_failures, e
                );
            }
        }

        // Clear the buffer after export attempt
        self.buffer.clear();
        self.buffered_counters = 0;
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
