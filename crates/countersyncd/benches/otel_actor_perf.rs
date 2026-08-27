use std::{
    net::SocketAddr,
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
    thread,
    time::Duration,
};

use criterion::{
    black_box, criterion_group, criterion_main, BatchSize, BenchmarkId, Criterion, Throughput,
};
use prost::Message;
use tokio::runtime::Builder;
use tokio::sync::oneshot;
use tokio_stream::wrappers::TcpListenerStream;
use tonic::{transport::Server, Request, Response, Status};

use countersyncd::{
    actor::otel::{
        default_instrumentation_scope, default_resource, split_export_requests,
        DEFAULT_MAX_EXPORT_BYTES,
    },
    message::aggregator::{
        default_heatmap_layout, heatmap_schema, Heatmap, HeatmapLayout, HeatmapQuantity,
        HeatmapValueKind,
    },
};
use opentelemetry_proto::tonic::{
    collector::metrics::v1::{
        metrics_service_client::MetricsServiceClient,
        metrics_service_server::{MetricsService, MetricsServiceServer},
        ExportMetricsServiceRequest, ExportMetricsServiceResponse,
    },
    metrics::v1::{metric::Data, AggregationTemporality, Histogram, Metric},
};

const NOMINAL_INTERVAL_US: u32 = 1_000;
const SESSION_KEY: &str = "profile_with_representative_name|PORT";

struct MockMetricsService {
    requests: Arc<AtomicU64>,
    data_points: Arc<AtomicU64>,
}

#[tonic::async_trait]
impl MetricsService for MockMetricsService {
    async fn export(
        &self,
        request: Request<ExportMetricsServiceRequest>,
    ) -> Result<Response<ExportMetricsServiceResponse>, Status> {
        let request = request.into_inner();
        assert!(request.encoded_len() <= DEFAULT_MAX_EXPORT_BYTES);
        let data_points = request
            .resource_metrics
            .into_iter()
            .flat_map(|resource| resource.scope_metrics)
            .flat_map(|scope| scope.metrics)
            .filter_map(|metric| match metric.data {
                Some(Data::Histogram(histogram)) => {
                    for point in &histogram.data_points {
                        assert_eq!(point.bucket_counts.iter().sum::<u64>(), point.count);
                    }
                    Some(histogram.data_points.len() as u64)
                }
                _ => None,
            })
            .sum::<u64>();
        self.requests.fetch_add(1, Ordering::Relaxed);
        self.data_points.fetch_add(data_points, Ordering::Relaxed);
        Ok(Response::new(ExportMetricsServiceResponse::default()))
    }
}

struct MockCollector {
    endpoint: String,
    shutdown: oneshot::Sender<()>,
    handle: thread::JoinHandle<()>,
    requests: Arc<AtomicU64>,
    data_points: Arc<AtomicU64>,
}

fn start_mock_collector() -> MockCollector {
    let (addr_sender, addr_receiver) = std::sync::mpsc::channel();
    let (shutdown, shutdown_receiver) = oneshot::channel();
    let requests = Arc::new(AtomicU64::new(0));
    let data_points = Arc::new(AtomicU64::new(0));
    let service_requests = requests.clone();
    let service_data_points = data_points.clone();

    let handle = thread::spawn(move || {
        let runtime = tokio::runtime::Runtime::new().expect("mock collector runtime");
        runtime.block_on(async move {
            let listener = tokio::net::TcpListener::bind("127.0.0.1:0")
                .await
                .expect("bind mock collector");
            addr_sender
                .send(listener.local_addr().expect("collector address"))
                .expect("send collector address");
            let incoming = TcpListenerStream::new(listener);
            let service = MockMetricsService {
                requests: service_requests,
                data_points: service_data_points,
            };
            Server::builder()
                .add_service(MetricsServiceServer::new(service))
                .serve_with_incoming_shutdown(incoming, async {
                    let _ = shutdown_receiver.await;
                })
                .await
                .expect("serve mock collector");
        });
    });

    let address: SocketAddr = addr_receiver.recv().expect("receive collector address");
    MockCollector {
        endpoint: format!("http://{address}"),
        shutdown,
        handle,
        requests,
        data_points,
    }
}

fn compact_layouts() -> Vec<(HeatmapQuantity, Arc<HeatmapLayout>)> {
    [
        HeatmapQuantity::DeltaBytes,
        HeatmapQuantity::AbsoluteBytes,
        HeatmapQuantity::AbsoluteCells,
        HeatmapQuantity::DeltaCount,
        HeatmapQuantity::Native,
    ]
    .into_iter()
    .map(|quantity| {
        (
            quantity,
            default_heatmap_layout(quantity, Some(NOMINAL_INTERVAL_US))
                .expect("compact default layout"),
        )
    })
    .collect()
}

fn build_heatmap(
    object_index: usize,
    quantity: HeatmapQuantity,
    layout: Arc<HeatmapLayout>,
) -> Heatmap {
    let mut bucket_counts = vec![0; layout.bucket_count()];
    let bucket = layout
        .explicit_bounds_u64()
        .partition_point(|bound| *bound < 64);
    bucket_counts[bucket] = 64;
    Heatmap {
        object_name: Arc::from(format!("Ethernet{object_index}")),
        type_id: quantity as u32 + 1,
        stat_id: quantity as u32 + 100,
        start_time_unix_nano: 1_000_000,
        time_unix_nano: 2_000_000,
        count: 64,
        sum: 4_096.0,
        min: 64,
        max: 64,
        explicit_bounds: layout.explicit_bounds(),
        bucket_counts,
        value_kind: match quantity {
            HeatmapQuantity::AbsoluteBytes
            | HeatmapQuantity::AbsoluteCells
            | HeatmapQuantity::Native => HeatmapValueKind::CurrentOccupancy,
            HeatmapQuantity::DeltaBytes | HeatmapQuantity::DeltaCount => HeatmapValueKind::Delta,
        },
        quantity,
        unit: quantity.unit(),
        schema: heatmap_schema(
            match quantity {
                HeatmapQuantity::AbsoluteBytes
                | HeatmapQuantity::AbsoluteCells
                | HeatmapQuantity::Native => HeatmapValueKind::CurrentOccupancy,
                HeatmapQuantity::DeltaBytes | HeatmapQuantity::DeltaCount => {
                    HeatmapValueKind::Delta
                }
            },
            quantity,
            layout.explicit_bounds_u64(),
        ),
    }
}

fn request_metrics(request: ExportMetricsServiceRequest) -> Vec<Metric> {
    request
        .resource_metrics
        .into_iter()
        .next()
        .unwrap()
        .scope_metrics
        .into_iter()
        .next()
        .unwrap()
        .metrics
}

fn native_split_requests(series_count: usize) -> Vec<ExportMetricsServiceRequest> {
    let layout = default_heatmap_layout(HeatmapQuantity::Native, None).unwrap();
    let points = (0..series_count)
        .map(|index| {
            build_heatmap(index, HeatmapQuantity::Native, layout.clone())
                .to_proto(Some(SESSION_KEY))
        })
        .collect();
    split_export_requests(
        &default_resource(),
        &default_instrumentation_scope(),
        vec![Metric {
            name: "benchmark_native_heatmap".to_string(),
            description: "Benchmark native heatmap".to_string(),
            unit: "1".to_string(),
            data: Some(Data::Histogram(Histogram {
                data_points: points,
                aggregation_temporality: AggregationTemporality::Delta as i32,
            })),
            ..Default::default()
        }],
        DEFAULT_MAX_EXPORT_BYTES,
    )
    .unwrap()
}

fn build_export_requests(series_count: usize) -> Vec<ExportMetricsServiceRequest> {
    let layouts = compact_layouts();
    let mut data_points = vec![Vec::new(); layouts.len()];
    for index in 0..series_count {
        let layout_index = index % layouts.len();
        let (quantity, layout) = &layouts[layout_index];
        data_points[layout_index]
            .push(build_heatmap(index, *quantity, layout.clone()).to_proto(Some(SESSION_KEY)));
    }

    let metrics = layouts
        .into_iter()
        .zip(data_points)
        .filter(|(_, points)| !points.is_empty())
        .map(|((quantity, _), points)| Metric {
            name: format!("benchmark_{}_heatmap", quantity.as_str()),
            description: format!("Benchmark {} heatmap", quantity.as_str()),
            unit: quantity.unit().to_string(),
            data: Some(Data::Histogram(Histogram {
                data_points: points,
                aggregation_temporality: AggregationTemporality::Delta as i32,
            })),
            ..Default::default()
        })
        .collect();

    split_export_requests(
        &default_resource(),
        &default_instrumentation_scope(),
        metrics,
        DEFAULT_MAX_EXPORT_BYTES,
    )
    .expect("split compact benchmark request")
}

fn bench_histogram_conversion(c: &mut Criterion) {
    let mut group = c.benchmark_group("histogram_data_point_conversion");
    group.throughput(Throughput::Elements(1));

    let mut cases = compact_layouts()
        .into_iter()
        .map(|(quantity, layout)| (quantity.as_str(), quantity, layout))
        .collect::<Vec<_>>();
    cases.push((
        "synthetic_old_256_buckets",
        HeatmapQuantity::DeltaCount,
        HeatmapLayout::from_explicit_bounds((0..255).collect()).expect("synthetic old layout"),
    ));

    for (name, quantity, layout) in cases {
        let heatmap = build_heatmap(0, quantity, layout);
        let encoded_bytes = heatmap.to_proto(Some(SESSION_KEY)).encoded_len();
        group.bench_with_input(
            BenchmarkId::new(name, format!("{encoded_bytes}B_per_point")),
            &heatmap,
            |bencher, heatmap| {
                bencher.iter(|| black_box(heatmap).to_proto(black_box(Some(SESSION_KEY))));
            },
        );
    }
    group.finish();
}

#[derive(Clone)]
struct SendCase {
    name: &'static str,
    series_count: usize,
    requests: Vec<ExportMetricsServiceRequest>,
    encoded_bytes: usize,
    max_request_bytes: usize,
}

fn send_cases() -> Vec<SendCase> {
    [
        ("mixed", 64usize, build_export_requests(64)),
        ("mixed", 512, build_export_requests(512)),
        ("mixed", 4_096, build_export_requests(4_096)),
        ("native_split", 4_096, native_split_requests(4_096)),
    ]
    .into_iter()
    .map(|(name, series_count, requests)| {
        assert!(requests
            .iter()
            .all(|request| request.encoded_len() <= DEFAULT_MAX_EXPORT_BYTES));
        let encoded_bytes = requests.iter().map(Message::encoded_len).sum();
        let max_request_bytes = requests.iter().map(Message::encoded_len).max().unwrap();
        let decoded_points = requests
            .iter()
            .cloned()
            .flat_map(request_metrics)
            .filter_map(|metric| match metric.data {
                Some(Data::Histogram(histogram)) => Some(histogram.data_points.len()),
                _ => None,
            })
            .sum::<usize>();
        assert_eq!(decoded_points, series_count);
        SendCase {
            name,
            series_count,
            requests,
            encoded_bytes,
            max_request_bytes,
        }
    })
    .collect()
}

async fn export_requests(
    mut client: MetricsServiceClient<tonic::transport::Channel>,
    requests: Vec<ExportMetricsServiceRequest>,
) {
    for request in requests {
        black_box(
            client
                .export(request)
                .await
                .expect("export benchmark request"),
        );
    }
}

fn bench_send_group(
    c: &mut Criterion,
    runtime: &tokio::runtime::Runtime,
    collector: &MockCollector,
    bytes: bool,
) {
    let suffix = if bytes { "bytes" } else { "points" };
    let mut group = c.benchmark_group(format!("direct_tonic_histogram_export_{suffix}"));
    group.sample_size(10);
    group.warm_up_time(Duration::from_millis(500));
    group.measurement_time(Duration::from_secs(2));

    for case in send_cases() {
        let request_count = case.requests.len();
        let requests_before = collector.requests.load(Ordering::Relaxed);
        let points_before = collector.data_points.load(Ordering::Relaxed);
        let client = runtime
            .block_on(MetricsServiceClient::connect(collector.endpoint.clone()))
            .expect("connect benchmark client");
        runtime.block_on(export_requests(client.clone(), case.requests.clone()));
        assert_eq!(
            collector.requests.load(Ordering::Relaxed) - requests_before,
            request_count as u64
        );
        assert_eq!(
            collector.data_points.load(Ordering::Relaxed) - points_before,
            case.series_count as u64
        );

        group.throughput(if bytes {
            Throughput::Bytes(case.encoded_bytes as u64)
        } else {
            Throughput::Elements(case.series_count as u64)
        });
        group.bench_function(
            BenchmarkId::new(
                format!("{}_{}", case.name, suffix),
                format!(
                    "{}_points_{}_requests_{}B_max_{}B_total",
                    case.series_count, request_count, case.max_request_bytes, case.encoded_bytes
                ),
            ),
            |bencher| {
                bencher.to_async(runtime).iter_batched(
                    || (client.clone(), case.requests.clone()),
                    |(mut client, requests)| async move {
                        for request in requests {
                            black_box(
                                client
                                    .export(request)
                                    .await
                                    .expect("export benchmark request"),
                            );
                        }
                    },
                    BatchSize::SmallInput,
                );
            },
        );
    }
    group.finish();
}

fn bench_grpc_export(c: &mut Criterion) {
    let collector = start_mock_collector();
    let runtime = Builder::new_current_thread()
        .enable_all()
        .build()
        .expect("benchmark runtime");
    bench_send_group(c, &runtime, &collector, false);
    bench_send_group(c, &runtime, &collector, true);
    drop(runtime);
    let _ = collector.shutdown.send(());
    collector.handle.join().expect("join mock collector");
}

criterion_group!(benches, bench_histogram_conversion, bench_grpc_export);
criterion_main!(benches);
