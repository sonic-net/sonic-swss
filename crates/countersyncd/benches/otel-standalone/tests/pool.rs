use countersyncd_otel_bench::{
    actor::otel::{pool::series_shard, OtelActorConfig, OtelWorkerConfig, OtelWorkerPool},
    message::saistats::{SAIStat, SAIStatsBatch},
};
use opentelemetry_proto::tonic::{
    collector::metrics::v1::{
        metrics_service_server::{MetricsService, MetricsServiceServer},
        ExportMetricsPartialSuccess, ExportMetricsServiceRequest, ExportMetricsServiceResponse,
    },
    common::v1::any_value,
    metrics::v1::{metric::Data, number_data_point::Value},
};
use std::{
    collections::{HashMap, HashSet},
    sync::{Arc, Mutex},
    time::Duration,
};
use tokio::sync::{mpsc, oneshot};
use tokio_stream::wrappers::TcpListenerStream;
use tonic::{transport::Server, Request, Response, Status};

#[derive(Default)]
struct State {
    active: HashSet<usize>,
    peak: usize,
    observed: HashMap<u32, Vec<(u64, u64)>>,
    retried: bool,
}
struct Sink {
    state: Arc<Mutex<State>>,
    lanes: usize,
    retry: bool,
    reject: bool,
}
struct Active {
    state: Arc<Mutex<State>>,
    lane: usize,
}
impl Drop for Active {
    fn drop(&mut self) {
        self.state.lock().unwrap().active.remove(&self.lane);
    }
}
#[tonic::async_trait]
impl MetricsService for Sink {
    async fn export(
        &self,
        request: Request<ExportMetricsServiceRequest>,
    ) -> Result<Response<ExportMetricsServiceResponse>, Status> {
        let mut samples = Vec::new();
        let mut lane = None;
        for resource in &request.get_ref().resource_metrics {
            for scope in &resource.scope_metrics {
                for metric in &scope.metrics {
                    let Some(Data::Gauge(g)) = &metric.data else {
                        panic!("expected gauge")
                    };
                    for dp in &g.data_points {
                        assert_eq!(dp.attributes.len(), 3);
                        let attr = |name: &str| {
                            let a = dp.attributes.iter().find(|a| a.key == name).unwrap();
                            let Some(any_value::Value::StringValue(v)) =
                                &a.value.as_ref().unwrap().value
                            else {
                                panic!()
                            };
                            v.clone()
                        };
                        let id = attr("object_name")
                            .strip_prefix("Ethernet")
                            .unwrap()
                            .parse::<u32>()
                            .unwrap();
                        assert_eq!(attr("sai_type"), "SAI_OBJECT_TYPE_PORT");
                        assert_eq!(
                            attr("sai_stat"),
                            countersyncd_otel_bench::message::otel::sai_metric_names(1, id).1
                        );
                        assert_eq!(attr("object_name"), format!("Ethernet{id}"));
                        let Some(Value::AsInt(value)) = dp.value else {
                            panic!()
                        };
                        let stat = SAIStat::new(format!("Ethernet{id}"), 1, id, value as u64);
                        let owner = series_shard(&stat, self.lanes);
                        assert_eq!(
                            *lane.get_or_insert(owner),
                            owner,
                            "each export belongs to one lane"
                        );
                        assert_eq!(
                            metric.name,
                            countersyncd_otel_bench::message::otel::sai_metric_names(1, id).1
                        );
                        samples.push((id, dp.time_unix_nano, value as u64));
                    }
                }
            }
        }
        let lane = lane.unwrap();
        {
            let mut state = self.state.lock().unwrap();
            assert!(
                state.active.insert(lane),
                "two simultaneous requests for one ordered lane"
            );
            state.peak = state.peak.max(state.active.len());
            assert!(state.peak <= self.lanes);
        }
        let _active = Active {
            state: self.state.clone(),
            lane,
        };
        tokio::time::sleep(Duration::from_millis(10)).await;
        let mut state = self.state.lock().unwrap();
        if self.retry && !state.retried {
            state.retried = true;
            return Err(Status::unavailable("retry before accepting"));
        }
        if self.reject {
            return Ok(Response::new(ExportMetricsServiceResponse {
                partial_success: Some(ExportMetricsPartialSuccess {
                    rejected_data_points: samples.len() as i64,
                    error_message: "rejected".into(),
                }),
            }));
        }
        for (id, time, value) in samples {
            state.observed.entry(id).or_default().push((time, value));
        }
        Ok(Response::new(ExportMetricsServiceResponse::default()))
    }
}

#[test]
fn validates_bounds_and_affinity() {
    let default = OtelWorkerConfig::default();
    default.validate().unwrap();
    assert_eq!(default.lanes(), 1);
    for invalid in [
        OtelWorkerConfig {
            threads: 0,
            ..default.clone()
        },
        OtelWorkerConfig {
            in_flight_per_worker: 0,
            ..default.clone()
        },
        OtelWorkerConfig {
            threads: 64,
            in_flight_per_worker: 64,
            ..default.clone()
        },
        OtelWorkerConfig {
            queue_capacity: 0,
            ..default.clone()
        },
        OtelWorkerConfig {
            threads: 2,
            cpu_ids: vec![0],
            ..default.clone()
        },
        OtelWorkerConfig {
            threads: 2,
            cpu_ids: vec![0, 0],
            ..default.clone()
        },
        OtelWorkerConfig {
            cpu_ids: vec![usize::MAX],
            ..default.clone()
        },
    ] {
        assert!(invalid.validate().is_err(), "{invalid:?}");
    }
}

#[tokio::test]
async fn empty_pool_is_spawnable_and_drains_every_worker() {
    let (tx, rx) = mpsc::channel(1);
    drop(tx);
    let (done, done_rx) = oneshot::channel();
    let pool = OtelWorkerPool::new(
        rx,
        OtelActorConfig::default(),
        OtelWorkerConfig {
            threads: 2,
            in_flight_per_worker: 3,
            ..Default::default()
        },
        done,
    )
    .unwrap();
    let handle = tokio::spawn(async move { pool.run().await.map_err(|e| e.to_string()) });
    tokio::time::timeout(Duration::from_secs(3), handle)
        .await
        .unwrap()
        .unwrap()
        .unwrap();
    done_rx.await.unwrap();
}

#[tokio::test]
async fn ordered_pool_preserves_samples_limits_concurrency_and_retries() {
    let workers = OtelWorkerConfig {
        threads: 2,
        in_flight_per_worker: 2,
        queue_capacity: 1,
        cpu_ids: vec![],
    };
    let state = Arc::new(Mutex::new(State::default()));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let address = listener.local_addr().unwrap();
    let (stop, stopped) = oneshot::channel();
    let server = tokio::spawn(
        Server::builder()
            .add_service(MetricsServiceServer::new(Sink {
                state: state.clone(),
                lanes: workers.lanes(),
                retry: true,
                reject: false,
            }))
            .serve_with_incoming_shutdown(TcpListenerStream::new(listener), async {
                let _ = stopped.await;
            }),
    );
    let (tx, rx) = mpsc::channel(1);
    let (done, done_rx) = oneshot::channel();
    let pool = OtelWorkerPool::new(
        rx,
        OtelActorConfig {
            collector_endpoint: format!("http://{address}"),
            max_counters_per_export: 7,
            flush_timeout: Duration::from_millis(20),
        },
        workers,
        done,
    )
    .unwrap();
    let mut expected = HashMap::<u32, Vec<(u64, u64)>>::new();
    let mut inputs = Vec::new();
    for record in 0..10 {
        let mut batch = SAIStatsBatch::default();
        let time = 1_700_000_000_000_000_000 + record;
        let mut stats = Vec::new();
        for id in 0..32u32 {
            let value = u64::MAX - record * 100 - id as u64;
            expected.entry(id).or_default().push((time, value));
            stats.push(SAIStat::new(format!("Ethernet{id}"), 1, id, value));
            if id == 0 {
                expected.entry(id).or_default().push((time, value));
                stats.push(SAIStat::new("Ethernet0", 1, id, value));
            }
        }
        batch.push_record(time, stats);
        inputs.push(Arc::new(batch));
    }
    let producer = async move {
        for input in inputs {
            tx.send(input).await.unwrap();
        }
        drop(tx);
    };
    let (_, result) = tokio::time::timeout(Duration::from_secs(10), async {
        tokio::join!(producer, pool.run())
    })
    .await
    .unwrap();
    result.unwrap();
    done_rx.await.unwrap();
    let _ = stop.send(());
    server.await.unwrap().unwrap();
    let observed = state.lock().unwrap();
    assert_eq!(observed.observed, expected);
    assert!(observed.peak > 1 && observed.peak <= 4);
    assert!(observed.retried);
    assert!(observed.active.is_empty());
}

#[tokio::test]
async fn idle_router_detects_worker_failure_without_waiting_for_input_close() {
    let workers = OtelWorkerConfig::default();
    let state = Arc::new(Mutex::new(State::default()));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let (stop, stopped) = oneshot::channel();
    let server = tokio::spawn(
        Server::builder()
            .add_service(MetricsServiceServer::new(Sink {
                state,
                lanes: 1,
                retry: false,
                reject: true,
            }))
            .serve_with_incoming_shutdown(TcpListenerStream::new(listener), async {
                let _ = stopped.await;
            }),
    );
    let (tx, rx) = mpsc::channel(1);
    let (done, done_rx) = oneshot::channel();
    let pool = OtelWorkerPool::new(
        rx,
        OtelActorConfig {
            collector_endpoint: format!("http://{addr}"),
            max_counters_per_export: 100,
            flush_timeout: Duration::from_millis(10),
        },
        workers,
        done,
    )
    .unwrap();
    let mut batch = SAIStatsBatch::default();
    batch.push_record(10, [SAIStat::new("Ethernet0", 1, 0, 1)]);
    tx.send(Arc::new(batch)).await.unwrap();
    let result = tokio::time::timeout(Duration::from_secs(3), pool.run())
        .await
        .unwrap();
    assert!(result.is_err());
    done_rx.await.unwrap();
    drop(tx);
    let _ = stop.send(());
    server.await.unwrap().unwrap();
}
