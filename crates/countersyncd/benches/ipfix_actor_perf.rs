use criterion::{criterion_group, criterion_main, BatchSize, BenchmarkId, Criterion, Throughput};
use criterion::SamplingMode;
use std::sync::Arc;
use tokio::runtime::Builder;
use tokio::sync::mpsc;
use tokio::time::{timeout, Duration, Instant};

use countersyncd::actor::ipfix::IpfixActor;
use countersyncd::message::{
    buffer::SocketBufferMessage,
    ipfix::IPFixTemplatesMessage,
    saistats::SAIStatsMessage,
};
use log::warn;

mod ipfix_bench_data;
use ipfix_bench_data::{datasets, randomize_record, rng_for_template, PreparedDataset};

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
    let (stats_tx, mut stats_rx) = mpsc::channel::<SAIStatsMessage>(1024);

    let mut actor = IpfixActor::new(template_rx, buffer_rx);
    actor.add_recipient(stats_tx);

    let actor_handle = tokio::spawn(async move {
        IpfixActor::run(actor).await;
    });

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

    let mut received_messages = 0usize;
    let mut received_counters = 0usize;

    while received_messages < expected_messages {
        match timeout(STATS_RECV_TIMEOUT, stats_rx.recv()).await {
            Ok(Some(stats_msg)) => {
                received_messages += 1;
                received_counters += stats_msg.stats.len();
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

    for task in sender_tasks {
        let _ = task.await;
    }

    drop(buffer_tx);

    let elapsed = start.elapsed();

    drop(template_tx);
    drop(stats_rx);
    let _ = actor_handle.await;

    if received_messages != expected_messages {
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
            spec.total_counters_per_iteration() as u64,
        ));
        let bench_spec = Arc::new(spec.clone());
        group.bench_function(bench_id, move |b| {
            let rt = Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("tokio current-thread runtime");
            let spec = bench_spec.clone();
            b.to_async(&rt).iter_batched(
                {
                    let spec = Arc::clone(&spec);
                    move || PreparedDataset::new((*spec).clone())
                },
                {
                    let spec = Arc::clone(&spec);
                    move |prepared| {
                        let spec = Arc::clone(&spec);
                        async move {
                    let (
                        elapsed,
                        received_messages,
                        received_counters,
                        expected_messages,
                        expected_counters,
                    ) = run_prepared_dataset(prepared).await;

                    let cps = counters_per_second(elapsed, received_counters);

                    println!(
                        "Dataset {} -> elapsed {:?}, msgs {}/{}, counters {}/{}, cps {:.2}",
                        spec.name,
                        elapsed,
                        received_messages,
                        expected_messages,
                        received_counters,
                        expected_counters,
                        cps,
                    );
                        }
                    }
                },
                BatchSize::SmallInput,
            )
        });
    }

    group.finish();
}

criterion_group!(benches, bench_ipfix_actor_datasets);
criterion_main!(benches);
