//! Single-flight actor throughput, with a fully decoding local Tonic server.
//! Run via otel-standalone/Cargo.toml; no SONiC or netlink libraries are required.
use countersyncd_otel_bench::{
    actor::otel::{OtelActor, OtelActorConfig},
    message::saistats::{SAIStat, SAIStatsBatch},
};
use opentelemetry_proto::tonic::{
    collector::metrics::v1::{
        metrics_service_server::{MetricsService, MetricsServiceServer},
        ExportMetricsServiceRequest, ExportMetricsServiceResponse,
    },
    metrics::v1::{metric::Data, number_data_point::Value},
};
use prost::Message;
use std::{
    sync::{Arc, Mutex},
    thread,
    time::{Duration, Instant},
};
use tokio::{
    runtime::Builder,
    sync::{mpsc, oneshot},
};
use tokio_stream::wrappers::TcpListenerStream;
use tonic::{transport::Server, Request, Response, Status};

#[global_allocator]
#[cfg(feature = "mimalloc")]
static ALLOCATOR: mimalloc::MiMalloc = mimalloc::MiMalloc;

#[derive(Default, Debug)]
struct Received {
    points: u64,
    requests: u64,
    timestamp_sum: u64,
    value_sum: u64,
    tuple_sum: u64,
    bytes: u64,
}
struct Sink {
    state: Arc<Mutex<Received>>,
    target: u64,
    done: Mutex<Option<oneshot::Sender<Instant>>>,
}
#[tonic::async_trait]
impl MetricsService for Sink {
    async fn export(
        &self,
        request: Request<ExportMetricsServiceRequest>,
    ) -> Result<Response<ExportMetricsServiceResponse>, Status> {
        let mut received = Received::default();
        received.bytes = request.get_ref().encoded_len() as u64;
        for resource in request.get_ref().resource_metrics.iter() {
            for scope in &resource.scope_metrics {
                for metric in &scope.metrics {
                    let id: u64 = metric
                        .name
                        .strip_prefix("sai_counter_type_1_stat_")
                        .expect("metric name preserved")
                        .parse()
                        .unwrap();
                    let Some(Data::Gauge(gauge)) = &metric.data else {
                        return Err(Status::invalid_argument("expected Gauge"));
                    };
                    for point in &gauge.data_points {
                        let Some(Value::AsInt(value)) = point.value else {
                            return Err(Status::invalid_argument("expected integer"));
                        };
                        received.points += 1;
                        received.timestamp_sum =
                            received.timestamp_sum.wrapping_add(point.time_unix_nano);
                        received.value_sum = received.value_sum.wrapping_add(value as u64);
                        received.tuple_sum = received.tuple_sum.wrapping_add(
                            point.time_unix_nano.rotate_left(17)
                                ^ value as u64
                                ^ id.rotate_left(33),
                        );
                    }
                }
            }
        }
        let mut state = self.state.lock().unwrap();
        state.points += received.points;
        state.requests += 1;
        state.timestamp_sum = state.timestamp_sum.wrapping_add(received.timestamp_sum);
        state.value_sum = state.value_sum.wrapping_add(received.value_sum);
        state.tuple_sum = state.tuple_sum.wrapping_add(received.tuple_sum);
        state.bytes += received.bytes;
        if state.points == self.target {
            if let Some(done) = self.done.lock().unwrap().take() {
                let _ = done.send(Instant::now());
            }
        }
        Ok(Response::new(ExportMetricsServiceResponse::default()))
    }
}
fn main() {
    let args: Vec<_> = std::env::args().collect();
    let batch: usize = args.get(1).map(|x| x.parse().unwrap()).unwrap_or(100_000);
    let points: usize = args.get(2).map(|x| x.parse().unwrap()).unwrap_or(4_000_000);
    let repeats: usize = args.get(3).map(|x| x.parse().unwrap()).unwrap_or(3);
    assert_eq!(points % 500, 0);
    let cores = core_affinity::get_core_ids().expect("CPU affinity available");
    assert!(cores.len() >= 2);
    if std::env::var_os("OTEL_DECODE_CALIBRATION").is_some() {
        use countersyncd_otel_bench::message::otel::OtelDataPoint;
        use opentelemetry_proto::tonic::metrics::v1::{
            Gauge, Metric, ResourceMetrics, ScopeMetrics,
        };
        assert!(core_affinity::set_for_current(cores[cores.len() - 1]));
        let metrics = (0..500)
            .map(|i| {
                let stat = SAIStat::new(format!("Ethernet{i}"), 1, i, 7);
                Metric {
                    name: format!("sai_counter_type_1_stat_{i}"),
                    description: format!("SAI counter for object Ethernet{i} (type:1, stat:{i})"),
                    data: Some(Data::Gauge(Gauge {
                        data_points: (0..batch / 500)
                            .map(|j| {
                                OtelDataPoint::from_sai_stat(
                                    &stat,
                                    1_700_000_000_000_000_000 + j as u64,
                                )
                                .to_proto()
                            })
                            .collect(),
                    })),
                    ..Default::default()
                }
            })
            .collect();
        let request = ExportMetricsServiceRequest {
            resource_metrics: vec![ResourceMetrics {
                scope_metrics: vec![ScopeMetrics {
                    metrics,
                    ..Default::default()
                }],
                ..Default::default()
            }],
        };
        let bytes = request.encode_to_vec();
        drop(request);
        let start = Instant::now();
        let mut n = 0;
        while start.elapsed() < Duration::from_secs(5) {
            let decoded = ExportMetricsServiceRequest::decode(bytes.as_slice()).unwrap();
            std::hint::black_box(&decoded);
            drop(decoded);
            n += batch;
        }
        println!(
            "decode_drop_only batch={batch} bytes={} Mpoints_s={:.3}",
            bytes.len(),
            n as f64 / start.elapsed().as_secs_f64() / 1e6
        );
        return;
    }
    for trial in 0..repeats {
        // Input construction is outside the measured interval. Every record gets
        // a distinct timestamp; all three production attributes are preserved.
        let objects: Vec<Arc<str>> = (0..500)
            .map(|i| Arc::from(format!("Ethernet{i}")))
            .collect();
        let mut input = Vec::new();
        let mut expected = Received::default();
        for first in (0..points / 500).step_by(20) {
            let mut msg = SAIStatsBatch::with_capacity(20, 10_000);
            for record in first..(first + 20).min(points / 500) {
                let timestamp = 1_700_000_000_000_000_000u64 + record as u64 * 10_000;
                msg.push_record(
                    timestamp,
                    (0..500).map(|i| {
                        let value = (record as u64)
                            .wrapping_mul(1_000_003)
                            .wrapping_add(i as u64);
                        expected.points += 1;
                        expected.timestamp_sum = expected.timestamp_sum.wrapping_add(timestamp);
                        expected.value_sum = expected.value_sum.wrapping_add(value);
                        expected.tuple_sum = expected.tuple_sum.wrapping_add(
                            timestamp.rotate_left(17) ^ value ^ (i as u64).rotate_left(33),
                        );
                        SAIStat::new(objects[i].clone(), 1, i as u32, value)
                    }),
                );
            }
            input.push(Arc::new(msg));
        }
        let state = Arc::new(Mutex::new(Received::default()));
        let server_state = state.clone();
        let (ready_tx, ready_rx) = std::sync::mpsc::channel();
        let (stop_tx, stop_rx) = oneshot::channel();
        let (done_tx, done_rx) = oneshot::channel();
        let server_core = cores[cores.len() - 1];
        let server_cores = cores[2.min(cores.len() - 1)..].to_vec();
        let server_threads: usize = std::env::var("OTEL_SERVER_THREADS")
            .ok()
            .map(|s| s.parse().unwrap())
            .unwrap_or(1);
        let server = thread::spawn(move || {
            assert!(core_affinity::set_for_current(server_core));
            let mut builder = if server_threads == 1 {
                Builder::new_current_thread()
            } else {
                Builder::new_multi_thread()
            };
            if server_threads > 1 {
                let next = Arc::new(std::sync::atomic::AtomicUsize::new(0));
                builder
                    .worker_threads(server_threads)
                    .on_thread_start(move || {
                        let index = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
                            % server_cores.len();
                        assert!(core_affinity::set_for_current(server_cores[index]));
                    });
            }
            builder.enable_all().build().unwrap().block_on(async move {
                let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
                ready_tx.send(listener.local_addr().unwrap()).unwrap();
                Server::builder()
                    .initial_stream_window_size(16 * 1024 * 1024)
                    .initial_connection_window_size(32 * 1024 * 1024)
                    .add_service(
                        MetricsServiceServer::new(Sink {
                            state: server_state,
                            target: points as u64,
                            done: Mutex::new(Some(done_tx)),
                        })
                        .max_decoding_message_size(128 * 1024 * 1024),
                    )
                    .serve_with_incoming_shutdown(TcpListenerStream::new(listener), async {
                        let _ = stop_rx.await;
                    })
                    .await
                    .unwrap();
            });
        });
        let address = ready_rx.recv().unwrap();
        assert!(core_affinity::set_for_current(cores[0]));
        Builder::new_current_thread().enable_all().build().unwrap().block_on(async {
            let (tx,rx)=mpsc::channel(8);let (shutdown_tx,_)=oneshot::channel();
            let actor=OtelActor::new(rx,OtelActorConfig {collector_endpoint:format!("http://{address}"),max_counters_per_export:batch,flush_timeout:Duration::from_secs(1)},shutdown_tx).await.unwrap();
            // join! keeps actor, producer, and Tonic client on this one runtime thread.
            let producer=async move {for message in input {tx.send(message).await.map_err(|e|e.to_string())?;}drop(tx);Ok::<_,String>(())};
            let start=Instant::now();
            let (_,(),end)=tokio::time::timeout(Duration::from_secs(120),async {
                tokio::try_join!(producer,async {actor.run().await.map_err(|e|e.to_string())},async {done_rx.await.map_err(|e|e.to_string())})
            }).await.expect("benchmark timed out").expect("all exports must succeed");
            let elapsed=start.elapsed();let received_elapsed=end.duration_since(start);
            let observed=state.lock().unwrap();assert_eq!(observed.points,expected.points);assert_eq!(observed.timestamp_sum,expected.timestamp_sum);assert_eq!(observed.value_sum,expected.value_sum);
            assert_eq!(observed.tuple_sum,expected.tuple_sum);
            assert_eq!(observed.requests,(points as u64+batch as u64-1)/batch as u64);
            println!("trial={trial} batch={batch} points={} requests={} bytes={} elapsed_s={:.6} acked_Mpoints_s={:.3} received_elapsed_s={:.6}",observed.points,observed.requests,observed.bytes,elapsed.as_secs_f64(),observed.points as f64/elapsed.as_secs_f64()/1e6,received_elapsed.as_secs_f64());
        });
        let _ = stop_tx.send(());
        server.join().unwrap();
    }
}
