use std::process::{Command, Stdio};
use std::sync::{Arc, atomic::{AtomicU64, Ordering}};
use std::time::Duration;
use std::{net::SocketAddr, thread};

use criterion::{criterion_group, criterion_main, BatchSize, BenchmarkId, Criterion, Throughput};
use tokio::runtime::Builder;
use tokio::sync::{mpsc, oneshot};
use tokio::time::Instant;
use tokio::task::spawn_blocking;
use tokio_stream::wrappers::TcpListenerStream;
use tonic::{Request, Response, Status};
use tonic::transport::Server;

use countersyncd::actor::counter_db::{CounterDBActor, CounterDBConfig};
use countersyncd::actor::ipfix::IpfixActor;
use countersyncd::actor::otel::{OtelActor, OtelActorConfig};
use countersyncd::actor::stats_reporter::{OutputWriter, StatsReporterActor, StatsReporterConfig};
use countersyncd::message::{buffer::SocketBufferMessage, ipfix::IPFixTemplatesMessage, saistats::SAIStatsMessage};

mod ipfix_bench_data;
use ipfix_bench_data::{PreparedDataset, datasets, randomize_record, rng_for_template};

const COUNTERS_DB_ID: i32 = 2;
const SOCK_PATH: &str = "/var/run/redis/redis.sock";

struct BenchNullWriter;

impl OutputWriter for BenchNullWriter {
    fn write_line(&mut self, _: &str) {}
}

/// Simple mock collector service that just counts exports.
struct MockMetricsService {
    exports: Arc<AtomicU64>,
}

#[tonic::async_trait]
impl opentelemetry_proto::tonic::collector::metrics::v1::metrics_service_server::MetricsService for MockMetricsService {
    async fn export(
        &self,
        _request: Request<opentelemetry_proto::tonic::collector::metrics::v1::ExportMetricsServiceRequest>,
    ) -> Result<Response<opentelemetry_proto::tonic::collector::metrics::v1::ExportMetricsServiceResponse>, Status> {
        self.exports.fetch_add(1, Ordering::Relaxed);
        Ok(Response::new(Default::default()))
    }
}

/// Start a mock OTLP collector on an ephemeral port, returning its endpoint and a shutdown handle.
fn start_mock_collector() -> (String, oneshot::Sender<()>, thread::JoinHandle<()>, Arc<AtomicU64>) {
    let (addr_tx, addr_rx) = std::sync::mpsc::channel();
    let (shutdown_tx, shutdown_rx) = oneshot::channel();
    let exports = Arc::new(AtomicU64::new(0));
    let exports_clone = exports.clone();

    let handle = thread::spawn(move || {
        let rt = tokio::runtime::Runtime::new().expect("mock collector runtime");
        rt.block_on(async move {
            let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
                .await
                .expect("bind mock collector");
            let addr = listener.local_addr().expect("collector addr");
            addr_tx.send(addr).expect("send collector addr");

            let svc = MockMetricsService { exports: exports_clone };
            let incoming = TcpListenerStream::new(listener);

            Server::builder()
                .add_service(
                    opentelemetry_proto::tonic::collector::metrics::v1::metrics_service_server::MetricsServiceServer::new(
                        svc,
                    ),
                )
                .serve_with_incoming_shutdown(incoming, async {
                    let _ = shutdown_rx.await;
                })
                .await
                .ok();
        });
    });

    let addr: SocketAddr = addr_rx.recv().expect("collector addr recv");
    (format!("http://{}", addr), shutdown_tx, handle, exports)
}

fn flush_counters_db() {
    let output = Command::new("redis-cli")
        .args([
            "-s",
            SOCK_PATH,
            "-n",
            &COUNTERS_DB_ID.to_string(),
            "FLUSHDB",
        ])
        .stdout(Stdio::null())
        .output()
        .expect("spawn redis-cli for flush");

    if !output.status.success() {
        panic!("redis-cli FLUSHDB failed with status {}", output.status);
    }
}

fn seed_port_name_map(port_count: usize) {
    let db = swss_common::DbConnector::new_unix(COUNTERS_DB_ID, SOCK_PATH, 0)
        .expect("connect counter db for seed");

    let table = "COUNTERS_PORT_NAME_MAP";
    for idx in 0..port_count {
        let name = format!("Ethernet{}", idx);
        let value = swss_common::CxxString::from(format!("oid:0x100000000{:04x}", idx));
        let _ = db.hset(table, name.as_str(), &*value);
    }
}

async fn run_end_to_end(prepared: PreparedDataset, endpoint: String, exports_counter: Arc<AtomicU64>) -> (Duration, usize, u64) {
    let (template_tx, template_rx) = mpsc::channel::<IPFixTemplatesMessage>(prepared.template_messages.len() + 4);
    let (buffer_tx, buffer_rx) = mpsc::channel::<SocketBufferMessage>(1024);
    let (counter_tx, counter_rx) = mpsc::channel::<SAIStatsMessage>(1024);
    let (otel_tx, otel_rx) = mpsc::channel::<SAIStatsMessage>(1024);
    let (stats_tx, stats_rx) = mpsc::channel::<SAIStatsMessage>(1024);
    let (otel_done_tx, otel_done_rx) = oneshot::channel();

    let mut ipfix = IpfixActor::new(template_rx, buffer_rx);
    ipfix.add_recipient(counter_tx.clone());
    ipfix.add_recipient(otel_tx.clone());
    ipfix.add_recipient(stats_tx.clone());

    // Run IpfixActor on a dedicated thread with its own runtime to satisfy thread-local requirements
    let ipfix_handle = spawn_blocking(move || {
        let rt = tokio::runtime::Runtime::new().expect("ipfix runtime");
        rt.block_on(async move {
            IpfixActor::run(ipfix).await;
        });
    });

    let counter_cfg = CounterDBConfig {
        interval: Duration::from_millis(100),
    };
    let counter_interval = counter_cfg.interval;
    let counter_actor = CounterDBActor::new(counter_rx, counter_cfg).expect("create counter db actor");
    let counter_handle = tokio::spawn(async move { counter_actor.run().await });

    let otel_cfg = OtelActorConfig {
        collector_endpoint: endpoint.clone(),
        max_counters_per_export: 10_000,
        flush_timeout: Duration::from_secs(1),
    };
    let otel_actor = OtelActor::new(otel_rx, otel_cfg, otel_done_tx)
        .await
        .expect("create otel actor");
    let otel_handle = tokio::spawn(async move { otel_actor.run().await });

    let reporter_cfg = StatsReporterConfig {
        interval: Duration::from_secs(60),
        detailed: false,
        max_stats_per_report: None,
    };
    let reporter = StatsReporterActor::new(stats_rx, reporter_cfg, BenchNullWriter);
    let reporter_handle = tokio::spawn(async move { StatsReporterActor::run(reporter).await });

    for message in &prepared.template_messages {
        template_tx
            .send(message.clone())
            .await
            .expect("template send should succeed");
    }

    tokio::time::sleep(Duration::from_millis(50)).await;

    let expected_messages = prepared.expected_messages;
    let expected_counters = prepared.expected_counters;

    let start = Instant::now();

    let sender_tasks: Vec<_> = prepared
        .templates
        .iter()
        .cloned()
        .map(|tmpl| {
            let tx = buffer_tx.clone();
            let base_record = tmpl.base_record.clone();
            let mut rng = rng_for_template(&tmpl.spec);
            tokio::spawn(async move {
                for seq in 0..tmpl.records {
                    let msg = randomize_record(&base_record, seq as u64, &mut rng);
                    if tx.send(msg).await.is_err() {
                        break;
                    }
                }
            })
        })
        .collect();

    for task in sender_tasks {
        let _ = task.await;
    }

    drop(buffer_tx);
    drop(template_tx);

    let _ = ipfix_handle.await.expect("ipfix join");

    // Allow at least one counter DB write tick before closing the channel
    tokio::time::sleep(counter_interval * 2).await;
    drop(counter_tx);

    drop(otel_tx);
    drop(stats_tx);

    let _ = counter_handle.await;

    // Wait for otel to finish and notify
    let _ = otel_handle.await;
    let _ = otel_done_rx.await;

    let _ = reporter_handle.await;

    let elapsed = start.elapsed();
    let exports = exports_counter.load(Ordering::Relaxed);

    if expected_messages == 0 || expected_counters == 0 {
        panic!("Invalid expectations for dataset {}", prepared.spec.name);
    }

    (elapsed, expected_counters, exports)
}

fn bench_end_to_end(c: &mut Criterion) {
    let (endpoint, collector_shutdown, collector_handle, exports_counter) = start_mock_collector();

    let mut group = c.benchmark_group("end_to_end_pipeline");
    group.sample_size(10);
    group.measurement_time(Duration::from_secs(60));

    for spec in datasets()
    {
        let bench_id = BenchmarkId::from_parameter(spec.name);
        group.throughput(Throughput::Elements(
            spec.total_counters_per_iteration() as u64,
        ));

        let bench_spec = Arc::new(spec.clone());
        let endpoint = endpoint.clone();
        let exports_counter = exports_counter.clone();

        group.bench_function(bench_id, move |b| {
            let rt = Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("tokio current-thread runtime");

            let spec = bench_spec.clone();
            let endpoint = endpoint.clone();
            let exports_counter = exports_counter.clone();

            b.to_async(&rt).iter_batched(
                {
                    let spec = spec.clone();
                    move || PreparedDataset::new((*spec).clone())
                },
                move |prepared| {
                    let spec = spec.clone();
                    let endpoint = endpoint.clone();
                    let exports_counter = exports_counter.clone();
                    async move {
                        flush_counters_db();
                        seed_port_name_map(64);

                        let exports_before = exports_counter.load(Ordering::Relaxed);
                        let (elapsed, counters, exports_after) =
                            run_end_to_end(prepared, endpoint.clone(), exports_counter.clone())
                                .await;

                        let cps = counters as f64 / elapsed.as_secs_f64();
                        let exported = exports_after.saturating_sub(exports_before);

                        println!(
                            "Dataset {} -> elapsed {:?}, counters {}, cps {:.2}, exports {}",
                            spec.name, elapsed, counters, cps, exported
                        );

                        flush_counters_db();
                    }
                },
                BatchSize::SmallInput,
            )
        });
    }

    group.finish();

    let _ = collector_shutdown.send(());
    let _ = collector_handle.join();

    println!(
        "Total mock exports: {}",
        exports_counter.load(Ordering::Relaxed)
    );
}

criterion_group!(benches, bench_end_to_end);
criterion_main!(benches);
