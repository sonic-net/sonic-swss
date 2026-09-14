use countersyncd_otel_bench::{
    actor::{
        ipfix::IpfixActor,
        otel::{OtelActorConfig, OtelWorkerConfig, OtelWorkerPool},
    },
    message::{ipfix::IPFixTemplatesMessage, saistats::SAIStatsView},
};
use opentelemetry_proto::tonic::{
    collector::metrics::v1::{
        metrics_service_server::{MetricsService, MetricsServiceServer},
        ExportMetricsServiceRequest, ExportMetricsServiceResponse,
    },
    metrics::v1::{metric::Data, number_data_point::Value},
};
use std::{
    sync::{Arc, Mutex},
    time::Duration,
};
use tokio::sync::{mpsc, oneshot};
use tokio_stream::wrappers::TcpListenerStream;
use tonic::{transport::Server, Request, Response, Status};
#[path = "../../../tests/ipfix_test_helpers.rs"]
#[allow(dead_code)]
mod helpers;

struct Sink(Arc<Mutex<Vec<(String, u64, i64)>>>);
#[tonic::async_trait]
impl MetricsService for Sink {
    async fn export(
        &self,
        r: Request<ExportMetricsServiceRequest>,
    ) -> Result<Response<ExportMetricsServiceResponse>, Status> {
        let mut received = self.0.lock().unwrap();
        for resource in &r.get_ref().resource_metrics {
            for scope in &resource.scope_metrics {
                for metric in &scope.metrics {
                    let Some(Data::Gauge(g)) = &metric.data else {
                        panic!()
                    };
                    for p in &g.data_points {
                        assert_eq!(p.attributes.len(), 3);
                        let Some(Value::AsInt(v)) = p.value else {
                            panic!()
                        };
                        assert!(metric.name.starts_with("SAI_PORT_STAT_"));
                        received.push((metric.name.clone(), p.time_unix_nano, v));
                    }
                }
            }
        }
        Ok(Response::new(ExportMetricsServiceResponse::default()))
    }
}

#[tokio::test]
async fn actual_ipfix_shared_fanout_and_otel_delivery() {
    let mut templates = helpers::generate_ipfix_templates(2, 300);
    for (i, f) in templates[28..].chunks_exact_mut(8).enumerate() {
        f[4..8].copy_from_slice(&(0x10000u32 + i as u32).to_be_bytes());
    }
    let (names, ids) = helpers::generate_object_metadata(2);
    let update = IPFixTemplatesMessage::new(
        "test".into(),
        Arc::new(templates.clone()),
        Some(names),
        Some(ids),
    );
    let (tt, tr) = mpsc::channel(1);
    let (bt, br) = mpsc::channel(4);
    let (st, sr) = mpsc::channel(4);
    let (ft, mut fr) = mpsc::channel(4);
    let mut ipfix = IpfixActor::new(tr, br);
    ipfix.add_recipient(st);
    ipfix.add_recipient(ft);
    let ipfix = tokio::spawn(IpfixActor::run(ipfix));
    tt.send(update).await.unwrap();
    let ready = tt.reserve().await.unwrap();
    let mut bytes = helpers::generate_ipfix_records(&templates);
    bytes[20..28].copy_from_slice(&123u64.to_be_bytes());
    bt.send(Arc::new(bytes.clone())).await.unwrap();
    let batch = fr.recv().await.unwrap();
    assert!(matches!(
        batch.records().next().unwrap().stats,
        SAIStatsView::Shared { .. }
    ));
    // A legacy sibling may materialize its slice view without invalidating the
    // immutable shared representation used by OTel.
    assert_eq!(batch.iter().next().unwrap().stats[0].counter, 1);
    assert!(matches!(
        batch.records().next().unwrap().stats,
        SAIStatsView::Shared { .. }
    ));
    drop(ready);
    bytes[20..28].copy_from_slice(&124u64.to_be_bytes());
    bytes[28..36].copy_from_slice(&9u64.to_be_bytes());
    bt.send(Arc::new(bytes)).await.unwrap();
    drop(bt);
    let state = Arc::new(Mutex::new(Vec::new()));
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    let (stop, stopped) = oneshot::channel();
    let server = tokio::spawn(
        Server::builder()
            .add_service(MetricsServiceServer::new(Sink(state.clone())))
            .serve_with_incoming_shutdown(TcpListenerStream::new(listener), async {
                let _ = stopped.await;
            }),
    );
    let (done, done_rx) = oneshot::channel();
    let pool = OtelWorkerPool::new(
        sr,
        OtelActorConfig {
            collector_endpoint: format!("http://{addr}"),
            max_counters_per_export: 2,
            flush_timeout: Duration::from_millis(10),
        },
        OtelWorkerConfig {
            threads: 2,
            in_flight_per_worker: 2,
            ..Default::default()
        },
        done,
    )
    .unwrap();
    tokio::time::timeout(Duration::from_secs(5), pool.run())
        .await
        .unwrap()
        .unwrap();
    ipfix.await.unwrap();
    drop(tt);
    done_rx.await.unwrap();
    let _ = stop.send(());
    server.await.unwrap().unwrap();
    let data = state.lock().unwrap();
    assert_eq!(data.len(), 4);
    assert_eq!(
        data.iter()
            .filter(|(name, _, _)| name == "SAI_PORT_STAT_IF_IN_OCTETS")
            .map(|(_, t, v)| (*t, *v))
            .collect::<Vec<_>>(),
        vec![(123, 1), (124, 9)]
    );
    assert_eq!(
        data.iter()
            .filter(|(name, _, _)| name == "SAI_PORT_STAT_IF_IN_UCAST_PKTS")
            .map(|(_, t, v)| (*t, *v))
            .collect::<Vec<_>>(),
        vec![(123, 2), (124, 2)]
    );
}
