use std::{sync::Arc, time::Duration};
use tokio::{sync::mpsc::Receiver, sync::oneshot, select};
use opentelemetry::metrics::MetricsError;
use opentelemetry_proto::tonic::{
    common::v1::{KeyValue as ProtoKeyValue, AnyValue, any_value::Value, InstrumentationScope},
    metrics::v1::{Metric, Gauge as ProtoGauge, ResourceMetrics, ScopeMetrics},
    resource::v1::Resource as ProtoResource,
};
use crate::message::{
    saistats::{SAIStats, SAIStatsMessage},
    otel::{OtelMetrics, OtelMetricsMessageExt},
};
use log::{info, error, debug, warn};
use opentelemetry_proto::tonic::collector::metrics::v1::metrics_service_client::MetricsServiceClient;
use opentelemetry_proto::tonic::collector::metrics::v1::ExportMetricsServiceRequest;
use tonic::transport::Endpoint;

/// Configuration for the OtelActor
#[derive(Debug, Clone)]
pub struct OtelActorConfig {
    /// Whether to print statistics to console
    pub print_to_console: bool,
    /// OpenTelemetry collector endpoint
    pub collector_endpoint: String,
}

impl Default for OtelActorConfig {
    fn default() -> Self {
        Self {
            print_to_console: true,
            collector_endpoint: "http://localhost:4317".to_string(),
        }
    }
}

/// Actor that receives SAI statistics and exports to OpenTelemetry
pub struct OtelActor {
    stats_receiver: Receiver<SAIStatsMessage>,
    config: OtelActorConfig,
    shutdown_notifier: Option<oneshot::Sender<()>>,
    client: MetricsServiceClient<tonic::transport::Channel>,

    // Statistics tracking
    messages_received: u64,
    exports_performed: u64,
    export_failures: u64,
    console_reports: u64,
}

impl OtelActor {
    /// Creates a new OtelActor instance
    pub async fn new(
        stats_receiver: Receiver<SAIStatsMessage>,
        config: OtelActorConfig,
        shutdown_notifier: oneshot::Sender<()>
    ) -> Result<OtelActor, Box<dyn std::error::Error>> {
        let endpoint = config.collector_endpoint.parse::<Endpoint>()?;
        let client = MetricsServiceClient::connect(endpoint).await?;

        info!(
            "OtelActor initialized - console: {}, endpoint: {}",
            config.print_to_console,
            config.collector_endpoint
        );

        Ok(OtelActor {
            stats_receiver,
            config,
            shutdown_notifier: Some(shutdown_notifier),
            client,
            messages_received: 0,
            exports_performed: 0,
            export_failures: 0,
            console_reports: 0,
        })
    }

    /// Main run loop
    pub async fn run(mut self) {
        info!("OtelActor started");

        loop {
            select! {
                stats_msg = self.stats_receiver.recv() => {
                    match stats_msg {
                        Some(stats) => {
                            self.handle_stats_message(stats).await;
                        }
                        None => {
                            info!("Stats receiver channel closed, shutting down OtelActor");
                            break;
                        }
                    }
                }
            }
        }

        self.shutdown().await;
    }

    /// Handle incoming SAI statistics message
    async fn handle_stats_message(&mut self, stats: SAIStatsMessage) {
        self.messages_received += 1;

        debug!("Received SAI stats with {} entries, observation_time: {}",
               stats.stats.len(), stats.observation_time);

        // Convert to OTel format using message types
        let otel_metrics = OtelMetrics::from_sai_stats(&stats);

        // Print to console if enabled
        if self.config.print_to_console {
            self.print_otel_metrics(&otel_metrics).await;
        }

        // Export to OpenTelemetry collector
        self.export_otel_metrics(&otel_metrics).await;
    }

    /// Print metrics to console
    async fn print_otel_metrics(&mut self, otel_metrics: &OtelMetrics) {
        self.console_reports += 1;

        println!("   [OTel Report #{}] OpenTelemetry Metrics Export", self.console_reports);
        println!("   Service: {}", otel_metrics.service_name);
        println!("   Scope: {} v{}", otel_metrics.scope_name, otel_metrics.scope_version);
        println!("   Total Gauges: {}", otel_metrics.len());
        println!("   Messages Received: {}", self.messages_received);
        println!("   Exports: {} (Failures: {})", self.exports_performed, self.export_failures);

        if !otel_metrics.is_empty() {
            println!("Gauge Metrics:");
            for (index, gauge) in otel_metrics.gauges.iter().enumerate() {
                let data_point = &gauge.data_points[0]; // Each gauge has one data point

                // Print the gauge with full details
                println!("      [{:3}] Gauge: {}", index + 1, gauge.name);
                println!("           Value: {}", data_point.value);
                println!("           Unit: {}", gauge.unit);
                println!("           Time: {}ns", data_point.time_unix_nano);
                println!("           Description: {}", gauge.description);

                // Print attributes
                if !data_point.attributes.is_empty() {
                    println!("           Attributes:");
                    for attr in &data_point.attributes {
                        println!("             - {}={}", attr.key, attr.value);
                    }
                }

                // Print the raw OtelGauge struct for debugging
                println!("           Raw Gauge: {:#?}", gauge);
                println!();
            }
        }
        println!(); // Blank line
    }

    /// Export metrics to OpenTelemetry collector
    async fn export_otel_metrics(&mut self, otel_metrics: &OtelMetrics) {
        if otel_metrics.is_empty() {
            return;
        }

        // Convert gauges to protobuf metrics
        let proto_metrics: Vec<Metric> = otel_metrics.gauges.iter().map(|gauge| {
            let proto_data_points = gauge.data_points.iter()
                .map(|dp| dp.to_proto())
                .collect();

            let proto_gauge = ProtoGauge {
                data_points: proto_data_points,
            };

            Metric {
                name: gauge.name.clone(),
                description: gauge.description.clone(),
                metadata: vec![],
                data: Some(opentelemetry_proto::tonic::metrics::v1::metric::Data::Gauge(proto_gauge)),
                ..Default::default()
            }
        }).collect();

        // Create resource metrics
        let resource_metrics = ResourceMetrics {
            resource: Some(ProtoResource {
                attributes: vec![ProtoKeyValue {
                    key: "service.name".to_string(),
                    value: Some(AnyValue {
                        value: Some(Value::StringValue(otel_metrics.service_name.clone())),
                    }),
                }],
                dropped_attributes_count: 0,
            }),
            scope_metrics: vec![ScopeMetrics {
                scope: Some(InstrumentationScope {
                    name: otel_metrics.scope_name.clone(),
                    version: otel_metrics.scope_version.clone(),
                    attributes: vec![],
                    dropped_attributes_count: 0,
                }),
                schema_url: "".to_string(),
                metrics: proto_metrics,
            }],
            schema_url: "".to_string(),
        };

        // Create export request
        let request = ExportMetricsServiceRequest {
            resource_metrics: vec![resource_metrics],
        };

        // Export to collector
        match self.client.export(request).await {
            Ok(_) => {
                self.exports_performed += 1;
                info!("Exported {} metrics to collector", otel_metrics.len());
            }
            Err(e) => {
                self.export_failures += 1;
                error!("Failed to export metrics: {}", e);
            }
        }
    }

    pub fn print_conversion_report(sai_stats: &SAIStats, otel_metrics: &OtelMetrics) {
        println!(" [Conversion Report] SAI Stats → OpenTelemetry Gauges");
        println!("   Conversion timestamp: {}", sai_stats.observation_time);
        println!("   Input: {} SAI statistics", sai_stats.stats.len());
        println!("   Output: {} OpenTelemetry gauges", otel_metrics.len());
        println!();

        println!("BEFORE - Original SAI Statistics:");
        for (index, sai_stat) in sai_stats.stats.iter().enumerate().take(10) {
            println!(
                "   [{:2}] Object: {:20} | Type: {:3} | Stat: {:3} | Counter: {:>12}",
                index + 1,
                sai_stat.object_name,
                sai_stat.type_id,
                sai_stat.stat_id,
                sai_stat.counter
            );
        }
        if sai_stats.stats.len() > 10 {
            println!("   ... and {} more SAI statistics", sai_stats.stats.len() - 10);
        }
        println!();

        println!("AFTER - Converted OpenTelemetry Gauges:");
        for (index, gauge) in otel_metrics.gauges.iter().enumerate().take(10) {
            let data_point = &gauge.data_points[0];
            println!(
                "   [{:2}] Metric: {:35} | Value: {:>12} | Time: {}ns",
                index + 1,
                gauge.name,
                data_point.value,
                data_point.time_unix_nano
            );

            // Show key attributes on the same line
            let attrs: Vec<String> = data_point.attributes.iter()
                .map(|attr| format!("{}={}", attr.key, attr.value))
                .collect();
            if !attrs.is_empty() {
                println!("       Attributes: [{}]", attrs.join(", "));
            }
            println!("       Description: {}", gauge.description);
            println!();
        }
        if otel_metrics.gauges.len() > 10 {
            println!("   ... and {} more OpenTelemetry gauges", otel_metrics.gauges.len() - 10);
        }

        println!("Conversion completed successfully!");
        println!("═══════════════════════════════════════════════════════════════════");
        println!();
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
