use std::sync::{Arc, atomic::{AtomicU64, Ordering}};
use std::time::Duration;
use std::{net::SocketAddr, thread};

use criterion::{criterion_group, criterion_main, BatchSize, BenchmarkId, Criterion, Throughput};
use tokio::runtime::Builder;
use tokio::sync::{mpsc, oneshot};
use tokio_stream::wrappers::TcpListenerStream;
use tonic::{Request, Response, Status};
use tonic::transport::Server;

use countersyncd::actor::otel::{OtelActor, OtelActorConfig};
use countersyncd::message::saistats::{SAIStat, SAIStats, SAIStatsMessage};

mod ipfix_bench_data;
use ipfix_bench_data::{PreparedDataset, datasets};

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

fn build_stats_message(counters: usize, seed: u64) -> SAIStatsMessage {
    let stats = (0..counters)
        .map(|idx| SAIStat {
            object_name: format!("obj_{idx}"),
            type_id: 1 + (idx as u32 % 4),
            stat_id: 100 + idx as u32,
            counter: seed.wrapping_add(idx as u64),
        })
        .collect::<Vec<_>>();

    Arc::new(SAIStats::new(seed, stats))
}

async fn run_stream(prepared: PreparedDataset, endpoint: String) -> (std::time::Duration, usize) {
    let (tx, rx) = mpsc::channel(1024);
    let (shutdown_tx, _shutdown_rx) = oneshot::channel();

    let cfg = OtelActorConfig {
        print_to_console: false,
        collector_endpoint: endpoint,
        max_counters_per_export: 10_000,
        flush_timeout: Duration::from_secs(1),
    };

    let actor = OtelActor::new(rx, cfg, shutdown_tx)
        .await
        .expect("create otel actor");

    let handle = tokio::spawn(async move { actor.run().await });

    let total_counters = prepared.expected_counters;
    let start = std::time::Instant::now();

    for tmpl in prepared.templates.iter() {
        for msg_idx in 0..tmpl.records {
            let msg = build_stats_message(tmpl.spec.counters, msg_idx as u64);
            tx.send(msg).await.expect("send stats");
        }
    }

    drop(tx); // close channel so actor exits after processing

    let _ = handle.await;
    let elapsed = start.elapsed();

    (elapsed, total_counters)
}

fn counters_per_second(elapsed: std::time::Duration, counters: usize) -> f64 {
    if elapsed.as_secs_f64() > 0.0 {
        counters as f64 / elapsed.as_secs_f64()
    } else {
        0.0
    }
}

fn bench_otel_actor(c: &mut Criterion) {
    let (endpoint, collector_shutdown, collector_handle, exports_counter) = start_mock_collector();
    let mut group = c.benchmark_group("otel_actor_perf");
    group.sample_size(10);
    group.measurement_time(Duration::from_secs(30));

    let endpoint_clone = endpoint.clone();
    let exports_counter_total = exports_counter.clone();

    for spec in datasets() {
        let bench_id = BenchmarkId::from_parameter(spec.name);
        group.throughput(Throughput::Elements(
            spec.total_counters_per_iteration() as u64,
        ));

        let endpoint = endpoint_clone.clone();
        let exports_counter = exports_counter.clone();

        group.bench_function(bench_id, move |b| {
            let rt = Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("tokio current-thread runtime");

            let spec = spec.clone();
            let endpoint = endpoint.clone();
            let exports_counter = exports_counter.clone();

            b.to_async(&rt).iter_batched(
                {
                    let spec = spec.clone();
                    move || PreparedDataset::new(spec.clone())
                },
                move |prepared| {
                    let endpoint = endpoint.clone();
                    let exports_counter = exports_counter.clone();
                    let spec = spec.clone();
                    async move {
                        let exports_before = exports_counter.load(Ordering::Relaxed);

                        let (elapsed, counters) = run_stream(prepared, endpoint.clone()).await;

                        let exports_after = exports_counter.load(Ordering::Relaxed);
                        let exported = exports_after.saturating_sub(exports_before);
                        let cps = counters_per_second(elapsed, counters);
                        println!(
                            "Dataset {} -> elapsed {:?}, counters {}, cps {:.2}, exports {}",
                            spec.name, elapsed, counters, cps, exported
                        );
                    }
                },
                BatchSize::SmallInput,
            )
        });
    }

    group.finish();

    // Shut down mock collector
    let _ = collector_shutdown.send(());
    let _ = collector_handle.join();

    println!("Total mock exports: {}", exports_counter_total.load(Ordering::Relaxed));
}

criterion_group!(benches, bench_otel_actor);
criterion_main!(benches);
