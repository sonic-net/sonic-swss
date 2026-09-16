use crate::message::{otel::OtelMetrics, saistats::SAIStatsBatchMessage};
use crate::utilities::{record_comm_stats, ChannelLabel};
use log::{debug, error, info, warn};
use opentelemetry::ExportError;
use opentelemetry_proto::tonic::{
    collector::metrics::v1::{
        metrics_service_client::MetricsServiceClient, ExportMetricsServiceRequest,
    },
    common::v1::{any_value::Value, AnyValue, InstrumentationScope, KeyValue as ProtoKeyValue},
    metrics::v1::{Gauge as ProtoGauge, Metric, ResourceMetrics, ScopeMetrics},
    resource::v1::Resource as ProtoResource,
};
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

const INITIAL_BACKOFF_DELAY_SECS: u64 = 1;
const MAX_BACKOFF_DELAY_SECS: u64 = 10;
const MAX_EXPORT_RETRIES: u64 = 30;

/// Configuration for the OtelActor
#[derive(Debug, Clone)]
pub struct OtelActorConfig {
    /// OpenTelemetry collector endpoint
    pub collector_endpoint: String,
    /// Maximum counters per export; must be positive.
    /// This bounds counter count, not encoded bytes or collector message size.
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
        stats_receiver: Receiver<SAIStatsBatchMessage>,
        config: OtelActorConfig,
        shutdown_notifier: oneshot::Sender<()>,
    ) -> Result<OtelActor, Box<dyn std::error::Error>> {
        if config.max_counters_per_export == 0 {
            return Err(Box::new(OtelActorExportError(
                "max_counters_per_export must be positive".to_string(),
            )));
        }
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

            // Convert to OTel format using message types and buffer
            let otel_metrics = OtelMetrics::from_sai_stats(stats);

            if log::log_enabled!(log::Level::Debug) {
                self.print_otel_metrics(&otel_metrics).await;
            }

            let OtelMetrics {
                service_name,
                scope_name,
                scope_version,
                gauges,
            } = otel_metrics;
            let mut gauges = gauges.into_iter();
            while gauges.len() > 0 {
                // Each SAI counter produces one gauge. Split only at the exporter,
                // moving whole gauges so their timestamps and attributes survive.
                let remaining = self.config.max_counters_per_export - self.buffered_counters;
                let chunk: Vec<_> = gauges.by_ref().take(remaining).collect();
                if self.buffer.is_empty() {
                    self.flush_deadline = TokioInstant::now() + self.config.flush_timeout;
                }
                self.buffered_counters += chunk.len();
                self.buffer.push(OtelMetrics {
                    service_name: service_name.clone(),
                    scope_name: scope_name.clone(),
                    scope_version: scope_version.clone(),
                    gauges: chunk,
                });

                if self.buffered_counters == self.config.max_counters_per_export {
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
                debug!("Value (exact u64 before OTLP conversion): {}", data_point.value);
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
                _none => {
                    // Failed to create client
                    self.client = None;
                    self.backoff(attempt).await; // Wait before retrying
                    continue;
                }
            };

            // Attempt to send the request
            match client.export(request.clone()).await {
                Ok(_) => {
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
        if self.buffer.is_empty() {
            return Ok(());
        }

        let mut proto_metrics: Vec<Metric> = Vec::new();

        for otel_metrics in &self.buffer {
            for gauge in &otel_metrics.gauges {
                let proto_data_points = gauge.data_points.iter().map(|dp| dp.to_proto()).collect();

                let proto_gauge = ProtoGauge {
                    data_points: proto_data_points,
                };

                proto_metrics.push(Metric {
                    name: gauge.name.clone(),
                    description: gauge.description.clone(),
                    metadata: vec![],
                    data: Some(
                        opentelemetry_proto::tonic::metrics::v1::metric::Data::Gauge(proto_gauge),
                    ),
                    ..Default::default()
                });
            }
        }

        if proto_metrics.is_empty() {
            self.buffer.clear();
            self.buffered_counters = 0;
            return Ok(());
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

        if let Err(e) = &result {
            self.export_failures += 1;
            error!(
                "Failed to export buffered metrics (consecutive failures {}): {:?}",
                self.consecutive_failures, e
            );
        }

        self.buffer.clear();
        self.buffered_counters = 0;

        result
    }

    fn reset_flush_timer(&self, timer: &mut Pin<Box<Sleep>>) {
        // Avoid idle wakeups without postponing an overdue non-empty buffer.
        let now = TokioInstant::now();
        let deadline = if self.buffer.is_empty() && self.flush_deadline <= now {
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
    use crate::message::saistats::{SAIStat, SAIStatsBatch};
    use opentelemetry_proto::tonic::{
        collector::metrics::v1::{
            metrics_service_server::{MetricsService, MetricsServiceServer},
            ExportMetricsServiceResponse,
        },
        metrics::v1::{metric, number_data_point},
    };
    use std::sync::Arc;
    use tokio::sync::mpsc;
    use tokio_stream::wrappers::TcpListenerStream;

    struct Collector(mpsc::UnboundedSender<ExportMetricsServiceRequest>);

    #[tonic::async_trait]
    impl MetricsService for Collector {
        async fn export(
            &self,
            request: tonic::Request<ExportMetricsServiceRequest>,
        ) -> Result<tonic::Response<ExportMetricsServiceResponse>, tonic::Status> {
            self.0.send(request.into_inner()).unwrap();
            Ok(tonic::Response::new(Default::default()))
        }
    }

    async fn start_collector() -> (
        String,
        mpsc::UnboundedReceiver<ExportMetricsServiceRequest>,
        tokio::task::JoinHandle<()>,
    ) {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let endpoint = format!("http://{}", listener.local_addr().unwrap());
        let (sender, receiver) = mpsc::unbounded_channel();
        let server = tokio::spawn(async move {
            tonic::transport::Server::builder()
                .add_service(MetricsServiceServer::new(Collector(sender)))
                .serve_with_incoming(TcpListenerStream::new(listener))
                .await
                .unwrap();
        });
        (endpoint, receiver, server)
    }

    #[tokio::test]
    async fn caps_exports_across_records_and_within_wide_records() {
        let (endpoint, mut requests, server) = start_collector().await;
        for (cap, record_sizes) in [
            (3, vec![2, 2, 3]),
            (3, vec![8]),
            (3, vec![3, 3]),
            (1, vec![0, 2, 1]),
        ] {
            let (_sender, receiver) = mpsc::channel(1);
            let (shutdown_sender, _) = oneshot::channel();
            let mut actor = OtelActor::new(
                receiver,
                OtelActorConfig {
                    collector_endpoint: endpoint.clone(),
                    max_counters_per_export: cap,
                    ..OtelActorConfig::default()
                },
                shutdown_sender,
            )
            .await
            .unwrap();

            let mut batch = SAIStatsBatch::default();
            let mut counter = 0;
            for (record, count) in record_sizes.iter().enumerate() {
                batch.push_record(
                    100 + record as u64,
                    (0..*count).map(|_| {
                        counter += 1;
                        SAIStat::new(format!("Ethernet{counter}"), 1, 0, counter)
                    }),
                );
            }
            let expected: Vec<_> = batch
                .iter()
                .flat_map(|stats| OtelMetrics::from_sai_stats(stats).gauges)
                .flat_map(|gauge| gauge.data_points)
                .map(|point| point.to_proto())
                .collect();
            let total = expected.len();

            tokio::time::timeout(
                Duration::from_secs(5),
                actor.handle_stats_batch(Arc::new(batch)),
            )
            .await
            .unwrap()
            .unwrap();
            assert_eq!(actor.messages_received, record_sizes.len() as u64);
            assert_eq!(actor.exports_performed, (total / cap) as u64);
            assert_eq!(actor.buffered_counters, total % cap);
            tokio::time::timeout(Duration::from_secs(5), actor.flush_buffer())
                .await
                .unwrap()
                .unwrap();

            let mut actual = Vec::new();
            let mut counts = Vec::new();
            while let Ok(request) = requests.try_recv() {
                let mut count = 0;
                for resource in request.resource_metrics {
                    for scope in resource.scope_metrics {
                        for metric in scope.metrics {
                            let Some(metric::Data::Gauge(gauge)) = metric.data else {
                                panic!("Expected gauge");
                            };
                            count += gauge.data_points.len();
                            actual.extend(gauge.data_points);
                        }
                    }
                }
                assert!(count > 0 && count <= cap);
                counts.push(count);
            }
            let mut expected_counts = vec![cap; total / cap];
            if total % cap != 0 {
                expected_counts.push(total % cap);
            }
            assert_eq!(counts, expected_counts);
            assert_eq!(actual, expected); // Includes every timestamp and attribute.
            for (index, point) in actual.iter().enumerate() {
                assert_eq!(
                    point.value,
                    Some(number_data_point::Value::AsInt(index as i64 + 1))
                );
            }
            assert!(actor.buffer.is_empty());
            assert_eq!(actor.buffered_counters, 0);
        }
        server.abort();
    }

    #[tokio::test]
    async fn timeout_flushes_partial_export_while_input_stays_open() {
        let (endpoint, mut requests, server) = start_collector().await;
        let (sender, receiver) = mpsc::channel(1);
        let (shutdown_sender, _) = oneshot::channel();
        let actor = OtelActor::new(
            receiver,
            OtelActorConfig {
                collector_endpoint: endpoint,
                max_counters_per_export: 3,
                flush_timeout: Duration::from_millis(25),
            },
            shutdown_sender,
        )
        .await
        .unwrap();
        let mut batch = SAIStatsBatch::default();
        batch.push_record(99, [SAIStat::new("Ethernet0", 1, 0, u64::MAX)]);
        sender.send(Arc::new(batch)).await.unwrap();
        let task = tokio::spawn(actor.run());
        let request = tokio::time::timeout(Duration::from_secs(5), requests.recv())
            .await
            .unwrap()
            .unwrap();
        let metrics = &request.resource_metrics[0].scope_metrics[0].metrics;
        assert_eq!(metrics.len(), 1);
        let Some(metric::Data::Gauge(gauge)) = &metrics[0].data else {
            panic!("Expected gauge");
        };
        assert_eq!(gauge.data_points.len(), 1);
        assert_eq!(gauge.data_points[0].time_unix_nano, 99);
        assert_eq!(
            gauge.data_points[0].value,
            Some(number_data_point::Value::AsDouble(u64::MAX as f64))
        );
        drop(sender);
        tokio::time::timeout(Duration::from_secs(5), task)
            .await
            .unwrap()
            .unwrap()
            .unwrap();
        assert!(requests.try_recv().is_err());
        server.abort();
    }

    #[tokio::test(start_paused = true)]
    async fn later_records_do_not_postpone_partial_export_deadline() {
        let (_sender, receiver) = mpsc::channel(1);
        let (shutdown_sender, _) = oneshot::channel();
        let mut actor = OtelActor::new(receiver, OtelActorConfig::default(), shutdown_sender)
            .await
            .unwrap();
        let mut batch = SAIStatsBatch::default();
        batch.push_record(99, [SAIStat::new("Ethernet0", 1, 0, 1)]);
        let batch = Arc::new(batch);
        actor.handle_stats_batch(batch.clone()).await.unwrap();
        let deadline = actor.flush_deadline;
        let mut timer = Box::pin(sleep_until(deadline));
        tokio::time::advance(Duration::from_millis(500)).await;
        actor.handle_stats_batch(batch.clone()).await.unwrap();
        actor.reset_flush_timer(&mut timer);
        assert_eq!(timer.deadline(), deadline);
        tokio::time::advance(Duration::from_secs(1)).await;
        actor.handle_stats_batch(batch).await.unwrap();
        actor.reset_flush_timer(&mut timer);
        assert_eq!(timer.deadline(), deadline);
        assert!(timer.deadline() <= TokioInstant::now());
    }

    #[tokio::test]
    async fn rejects_zero_counter_cap() {
        let (_sender, receiver) = mpsc::channel(1);
        let (shutdown_sender, _) = oneshot::channel();
        let result = OtelActor::new(
            receiver,
            OtelActorConfig {
                max_counters_per_export: 0,
                ..OtelActorConfig::default()
            },
            shutdown_sender,
        )
        .await;
        assert!(matches!(
            result,
            Err(error) if error.to_string() == "max_counters_per_export must be positive"
        ));
    }

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
        assert_eq!(actor.buffer.len(), 2);
        assert_eq!(actor.buffer[0].gauges[0].data_points[0].value, 3);
        assert_eq!(actor.buffer[0].gauges[1].data_points[0].value, 4);
        assert_eq!(actor.buffer[1].gauges[0].data_points[0].value, 5);
        assert_eq!(actor.buffer[1].gauges[0].data_points[0].time_unix_nano, 20);
    }
}
