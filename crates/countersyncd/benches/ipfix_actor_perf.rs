use criterion::SamplingMode;
use criterion::{criterion_group, criterion_main, BenchmarkId, Criterion, Throughput};
use std::sync::Arc;
use tokio::runtime::Builder;
use tokio::sync::mpsc;
use tokio::time::{timeout, Duration, Instant};

use countersyncd::actor::ipfix::IpfixActor;
use countersyncd::message::{
    buffer::SocketBufferMessage, ipfix::IPFixTemplatesMessage, saistats::SAIStatsBatchMessage,
};
use log::warn;

mod ipfix_bench_data;
use ipfix_bench_data::{datasets, PreparedDataset};

const STATS_RECV_TIMEOUT: Duration = Duration::from_secs(5);

fn counters_per_second(elapsed: Duration, counters: usize) -> f64 {
    if elapsed.as_secs_f64() > 0.0 {
        counters as f64 / elapsed.as_secs_f64()
    } else {
        0.0
    }
}

async fn run_prepared_dataset(prepared: PreparedDataset) -> (Duration, usize, usize, usize, usize) {
    let (template_tx, template_rx) =
        mpsc::channel::<IPFixTemplatesMessage>(prepared.template_messages.len() + 4);
    let (buffer_tx, buffer_rx) = mpsc::channel::<SocketBufferMessage>(1024);
    let (stats_tx, mut stats_rx) = mpsc::channel::<SAIStatsBatchMessage>(1024);

    let mut actor = IpfixActor::new(template_rx, buffer_rx);
    actor.add_recipient(stats_tx);

    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    for message in &prepared.template_messages {
        template_tx
            .send(message.clone())
            .await
            .expect("template send should succeed");
    }

    buffer_tx
        .send(Arc::clone(&prepared.readiness_record))
        .await
        .expect("readiness probe send should succeed");
    let probe_batch = timeout(STATS_RECV_TIMEOUT, stats_rx.recv())
        .await
        .expect("readiness probe timed out")
        .expect("stats channel closed before readiness probe");
    assert_eq!(probe_batch.record_count(), 1);
    assert_eq!(probe_batch.counter_count(), 1);

    let expected_messages = prepared.expected_messages;
    let expected_counters = prepared.expected_counters;
    let records: Vec<_> = prepared
        .templates
        .into_iter()
        .map(|template| (template.base_record, template.records))
        .collect();

    let sender_tasks: Vec<_> = records
        .into_iter()
        .map(|(record, record_count)| {
            let tx = buffer_tx.clone();
            tokio::spawn(async move {
                for _ in 0..record_count {
                    if tx.send(Arc::clone(&record)).await.is_err() {
                        break;
                    }
                }
            })
        })
        .collect();

    let start = Instant::now();

    let mut received_messages = 0usize;
    let mut received_counters = 0usize;

    while received_messages < expected_messages {
        match timeout(STATS_RECV_TIMEOUT, stats_rx.recv()).await {
            Ok(Some(stats_msg)) => {
                let batch_counters = stats_msg.counter_count();
                let counters_before = received_counters;
                for record in stats_msg.iter() {
                    received_messages += 1;
                    received_counters += record.stats.len();
                }
                assert_eq!(received_counters - counters_before, batch_counters);
                assert!(
                    received_messages <= expected_messages,
                    "dataset {} produced more records than expected",
                    prepared.spec.name
                );
            }
            Ok(None) => {
                warn!(
                    "Stats channel closed early for dataset {} after {} messages",
                    prepared.spec.name, received_messages
                );
                break;
            }
            Err(_) => {
                panic!(
                    "Stats recv timeout for dataset {} after {} messages (expected {})",
                    prepared.spec.name, received_messages, expected_messages
                );
            }
        }
    }

    let elapsed = start.elapsed();

    for task in sender_tasks {
        task.await.expect("record sender should join");
    }

    drop(buffer_tx);

    drop(template_tx);
    drop(stats_rx);
    actor_handle
        .await
        .expect("IPFIX actor task should join")
        .expect_err("IPFIX actor should report closed input channels");

    if received_messages != expected_messages || received_counters != expected_counters {
        panic!(
            "Dataset {} incomplete: msgs {}/{}, counters {}/{}",
            prepared.spec.name,
            received_messages,
            expected_messages,
            received_counters,
            expected_counters
        );
    }

    (
        elapsed,
        received_messages,
        received_counters,
        expected_messages,
        expected_counters,
    )
}

fn bench_ipfix_actor_datasets(c: &mut Criterion) {
    let mut group = c.benchmark_group("ipfix_actor_dataset_perf");
    group.measurement_time(Duration::from_secs(60));
    group.sample_size(10);
    group.sampling_mode(SamplingMode::Flat);

    for spec in datasets() {
        let bench_id = BenchmarkId::from_parameter(spec.name);
        group.throughput(Throughput::Elements(
            spec.total_counters_per_iteration() as u64
        ));
        let bench_spec = Arc::new(spec.clone());
        group.bench_function(bench_id, move |b| {
            let rt = Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("tokio current-thread runtime");
            let spec = bench_spec.clone();
            b.to_async(&rt).iter_custom(move |iterations| {
                let spec = Arc::clone(&spec);
                async move {
                    let mut measured = Duration::ZERO;
                    for _ in 0..iterations {
                        let prepared = PreparedDataset::new((*spec).clone());
                        let (
                            elapsed,
                            received_messages,
                            received_counters,
                            expected_messages,
                            expected_counters,
                        ) = run_prepared_dataset(prepared).await;
                        measured += elapsed;

                        let cps = counters_per_second(elapsed, received_counters);

                        println!(
                            "Dataset {} -> elapsed {:?}, records {}/{}, counters {}/{}, cps {:.2}, readiness probe 1 record/1 counter (excluded)",
                            spec.name,
                            elapsed,
                            received_messages,
                            expected_messages,
                            received_counters,
                            expected_counters,
                            cps,
                        );
                    }
                    measured
                }
            })
        });
    }

    group.finish();
}

criterion_group!(benches, bench_ipfix_actor_datasets);
criterion_main!(benches);
