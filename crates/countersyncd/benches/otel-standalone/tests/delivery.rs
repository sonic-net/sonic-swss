use countersyncd_otel_bench::{
    actor::otel::{OtelActor, OtelActorConfig},
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
use prost::Message;
use std::{
    sync::{Arc, Mutex},
    time::Duration,
};
use tokio::sync::{mpsc, oneshot};
use tokio_stream::wrappers::TcpListenerStream;
use tonic::{transport::Server, Request, Response, Status};

struct Sink {
    requests: Arc<Mutex<Vec<ExportMetricsServiceRequest>>>,
    fail_first: bool,
    partial: bool,
    arrived: Arc<tokio::sync::Notify>,
}
#[tonic::async_trait]
impl MetricsService for Sink {
    async fn export(
        &self,
        r: Request<ExportMetricsServiceRequest>,
    ) -> Result<Response<ExportMetricsServiceResponse>, Status> {
        let mut requests = self.requests.lock().unwrap();
        requests.push(r.into_inner());
        self.arrived.notify_one();
        if self.fail_first && requests.len() == 1 {
            return Err(Status::unavailable("retry test"));
        }
        Ok(Response::new(ExportMetricsServiceResponse {
            partial_success: self.partial.then(|| ExportMetricsPartialSuccess {
                rejected_data_points: 1,
                error_message: "rejected test".into(),
            }),
        }))
    }
}
async fn scenario(fail_first: bool, partial: bool, timeout_flush: bool) {
    let requests = Arc::new(Mutex::new(Vec::new()));
    let arrived = Arc::new(tokio::sync::Notify::new());
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let (stop_tx, stop_rx) = oneshot::channel();
    let server = tokio::spawn(
        Server::builder()
            .add_service(MetricsServiceServer::new(Sink {
                requests: requests.clone(),
                fail_first,
                partial,
                arrived: arrived.clone(),
            }))
            .serve_with_incoming_shutdown(TcpListenerStream::new(listener), async {
                let _ = stop_rx.await;
            }),
    );
    let (tx, rx) = mpsc::channel(1);
    let (shutdown_tx, shutdown_rx) = oneshot::channel();
    let actor = OtelActor::new(
        rx,
        OtelActorConfig {
            collector_endpoint: format!("http://{addr}"),
            max_counters_per_export: if timeout_flush { 100 } else { 2 },
            flush_timeout: Duration::from_millis(20),
        },
        shutdown_tx,
    )
    .await
    .unwrap();
    let mut batch = SAIStatsBatch::with_capacity(2, 3);
    batch.push_record(
        123,
        [
            SAIStat::new("Ethernet0", 1, 2, u64::MAX),
            SAIStat::new("Ethernet4", 1, 2, 7),
        ],
    );
    batch.push_record(124, [SAIStat::new("Ethernet0", 1, 2, 9)]);
    tx.send(Arc::new(batch)).await.unwrap();
    let producer = async {
        if timeout_flush {
            arrived.notified().await;
        }
        drop(tx);
    };
    let (_, result) = tokio::time::timeout(Duration::from_secs(10), async {
        tokio::join!(producer, actor.run())
    })
    .await
    .expect("delivery must finish");
    assert_eq!(result.is_err(), partial);
    shutdown_rx.await.unwrap();
    let _ = stop_tx.send(());
    server.await.unwrap().unwrap();
    let requests = requests.lock().unwrap();
    if fail_first {
        assert_eq!(requests[0].encode_to_vec(), requests[1].encode_to_vec());
    }
    if partial {
        let concurrent = cfg!(feature = "benchmark")
            && std::env::var("OTEL_MAX_IN_FLIGHT")
                .ok()
                .and_then(|s| s.parse::<usize>().ok())
                .unwrap_or(1)
                > 1;
        if concurrent {
            // The tail may already be sent when the first rejection arrives.
            // Distinct already-in-flight requests are not retries.
            assert!(requests.len() <= 2);
            if requests.len() == 2 {
                assert_ne!(requests[0].encode_to_vec(), requests[1].encode_to_vec());
            }
        } else {
            assert_eq!(requests.len(), 1, "partial success must not be retried");
        }
        return;
    }
    let mut points = Vec::new();
    for request in requests.iter().skip(usize::from(fail_first)) {
        let resource = &request.resource_metrics[0];
        assert_eq!(
            resource.resource.as_ref().unwrap().attributes[0].key,
            "service.name"
        );
        assert_eq!(
            resource.scope_metrics[0].scope.as_ref().unwrap().name,
            "countersyncd"
        );
        let mut batch_points = 0;
        for metric in &resource.scope_metrics[0].metrics {
            assert_eq!(metric.name, "SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS");
            let Some(Data::Gauge(gauge)) = &metric.data else {
                panic!()
            };
            for dp in &gauge.data_points {
                assert_eq!(dp.attributes.len(), 3);
                let attr = |key: &str| {
                    let a = dp.attributes.iter().find(|a| a.key == key).unwrap();
                    let Some(any_value::Value::StringValue(s)) = &a.value.as_ref().unwrap().value
                    else {
                        panic!()
                    };
                    s.clone()
                };
                assert_eq!(attr("sai_type"), "SAI_OBJECT_TYPE_PORT");
                assert_eq!(attr("sai_stat"), "SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS");
                let Some(Value::AsInt(v)) = dp.value else {
                    panic!()
                };
                points.push((attr("object_name"), dp.time_unix_nano, v as u64));
                batch_points += 1;
            }
        }
        if !timeout_flush {
            assert!(batch_points <= 2);
        }
    }
    points.sort();
    let mut expected = vec![
        ("Ethernet0".into(), 123, u64::MAX),
        ("Ethernet4".into(), 123, 7),
        ("Ethernet0".into(), 124, 9),
    ];
    expected.sort();
    assert_eq!(points, expected);
}
#[tokio::test]
async fn drains_tail_and_preserves_all_fields() {
    scenario(false, false, false).await;
}
#[tokio::test]
async fn retries_identical_payload_before_next_batch() {
    scenario(true, false, false).await;
}
#[tokio::test]
async fn partial_success_fails_without_retry() {
    scenario(false, true, false).await;
}
#[tokio::test]
async fn flushes_on_timeout_while_input_stays_open() {
    scenario(false, false, true).await;
}
