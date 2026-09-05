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
mod ipfix_bench_data;
use ipfix_bench_data::{datasets, PreparedDataset, PAYLOAD_POOL_RECORDS};

const READINESS_TIMEOUT: Duration = Duration::from_secs(5);
const ITERATION_TIMEOUT: Duration = Duration::from_secs(120);
const SAI_STATS_CHANNEL_CAPACITY: usize = 64;

fn counters_per_second(elapsed: Duration, counters: usize) -> f64 {
    if elapsed.as_secs_f64() > 0.0 {
        counters as f64 / elapsed.as_secs_f64()
    } else {
        0.0
    }
}

async fn run_prepared_dataset(
    prepared: PreparedDataset,
) -> (Duration, usize, usize, usize, usize, usize) {
    let (template_tx, template_rx) = mpsc::channel::<IPFixTemplatesMessage>(1);
    let (buffer_tx, buffer_rx) = mpsc::channel::<SocketBufferMessage>(1024);
    let (stats_tx, mut stats_rx) =
        mpsc::channel::<SAIStatsBatchMessage>(SAI_STATS_CHANNEL_CAPACITY);

    let mut actor = IpfixActor::new(template_rx, buffer_rx);
    actor.add_recipient(stats_tx);

    let mut actor_handle = tokio::spawn(IpfixActor::run(actor));

    for message in &prepared.template_messages {
        template_tx
            .send(message.clone())
            .await
            .expect("template send should succeed");
    }
    let template_barrier = template_tx
        .reserve()
        .await
        .expect("template barrier reserve should succeed");

    buffer_tx
        .send(Arc::clone(&prepared.readiness_record))
        .await
        .expect("readiness probe send should succeed");
    let probe_batch = timeout(READINESS_TIMEOUT, stats_rx.recv())
        .await
        .expect("readiness probe timed out")
        .expect("stats channel closed before readiness probe");
    assert_eq!(probe_batch.record_count(), 1);
    assert_eq!(probe_batch.counter_count(), 1);
    drop(template_barrier);

    let expected_messages = prepared.expected_messages;
    let expected_counters = prepared.expected_counters;
    let records: Vec<_> = prepared
        .templates
        .into_iter()
        .map(|template| (template.payload_pool, template.records))
        .collect();

    let sender_tasks: Vec<_> = records
        .into_iter()
        .map(|(payload_pool, record_count)| {
            let tx = buffer_tx.clone();
            tokio::spawn(async move {
                for index in 0..record_count {
                    let payload = &payload_pool[index % payload_pool.len()];
                    if tx.send(Arc::clone(payload)).await.is_err() {
                        break;
                    }
                }
            })
        })
        .collect();
    drop(buffer_tx);

    let mut received_batches = 0usize;
    let mut received_messages = 0usize;
    let mut received_counters = 0usize;

    let mut sender_tasks = sender_tasks;
    let completion = timeout(
        ITERATION_TIMEOUT,
        std::pin::pin!(async {
            let start = Instant::now();
            let mut measured_elapsed = None;
            while let Some(stats_msg) = stats_rx.recv().await {
                received_batches += 1;
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
                assert!(
                    received_counters <= expected_counters,
                    "dataset {} produced more counters than expected",
                    prepared.spec.name
                );
                if received_messages == expected_messages
                    && received_counters == expected_counters
                    && measured_elapsed.is_none()
                {
                    measured_elapsed = Some(start.elapsed());
                }
            }

            for task in &mut sender_tasks {
                task.await.expect("record sender should join");
            }
            let actor_error = (&mut actor_handle)
                .await
                .expect("IPFIX actor task should join")
                .expect_err("IPFIX actor should report a closed input channel");
            assert!(
                actor_error
                    .to_string()
                    .contains("IPFIX record input channel closed"),
                "unexpected actor termination: {actor_error}"
            );

            measured_elapsed.expect("stats channel closed before complete output")
        }),
    )
    .await;
    let elapsed = match completion {
        Ok(elapsed) => elapsed,
        Err(_) => {
            for task in &sender_tasks {
                task.abort();
            }
            actor_handle.abort();
            panic!(
                "Dataset {} timed out after {:?}: batches {}, records {}/{}, counters {}/{}",
                prepared.spec.name,
                ITERATION_TIMEOUT,
                received_batches,
                received_messages,
                expected_messages,
                received_counters,
                expected_counters
            );
        }
    };

    drop(template_tx);

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
        received_batches,
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
            let prepared = Arc::new(PreparedDataset::new((*spec).clone()));
            b.to_async(&rt).iter_custom(move |iterations| {
                let spec = Arc::clone(&spec);
                let prepared = Arc::clone(&prepared);
                async move {
                    let mut measured = Duration::ZERO;
                    for _ in 0..iterations {
                        let (
                            elapsed,
                            received_batches,
                            received_messages,
                            received_counters,
                            expected_messages,
                            expected_counters,
                        ) = run_prepared_dataset((*prepared).clone()).await;
                        measured += elapsed;

                        let cps = counters_per_second(elapsed, received_counters);

                        println!(
                            "Dataset {} -> elapsed {:?}, output batches {}, records {}/{}, counters {}/{}, cps {:.2}, up to {} pre-generated payloads/template, readiness probe 1 record/1 counter (excluded)",
                            spec.name,
                            elapsed,
                            received_batches,
                            received_messages,
                            expected_messages,
                            received_counters,
                            expected_counters,
                            cps,
                            PAYLOAD_POOL_RECORDS,
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
