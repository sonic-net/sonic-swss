use std::process::{Command, Stdio};
use std::time::Duration;

use criterion::{criterion_group, criterion_main, BatchSize, BenchmarkId, Criterion, Throughput};
use tokio::runtime::Builder;
use tokio::sync::mpsc;

use countersyncd::actor::counter_db::{CounterDBActor, CounterDBConfig};
use countersyncd::message::saistats::{SAIStat, SAIStats, SAIStatsMessage};
use countersyncd::sai::saitypes::SaiObjectType;
use swss_common::{CxxString, DbConnector};

mod ipfix_bench_data;
use ipfix_bench_data::{PreparedDataset, datasets};

const COUNTERS_DB_ID: i32 = 2;
const SOCK_PATH: &str = "/var/run/redis/redis.sock";

fn build_stats_message(count: usize, seq: u64) -> SAIStatsMessage {
    let type_id = SaiObjectType::Port.to_u32();

    let stats = (0..count)
        .map(|idx| SAIStat {
            object_name: format!("Ethernet{}", idx % 16),
            type_id,
            stat_id: (idx % 4) as u32, // Small, valid port stat IDs
            counter: seq.wrapping_add(idx as u64),
        })
        .collect();

    std::sync::Arc::new(SAIStats::new(seq, stats))
}

fn seed_port_name_map(port_count: usize) {
    let db = DbConnector::new_unix(COUNTERS_DB_ID, SOCK_PATH, 0)
        .expect("connect counter db for seed");

    let table = "COUNTERS_PORT_NAME_MAP";
    for idx in 0..port_count {
        let name = format!("Ethernet{}", idx);
        let value = CxxString::from(format!("oid:0x100000000{:04x}", idx));
        // Ignore individual hset errors; bench will reveal connectivity issues anyway
        let _ = db.hset(table, name.as_str(), &*value);
    }
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
        panic!(
            "redis-cli FLUSHDB failed with status {}",
            output.status
        );
    }
}

async fn run_stream(prepared: PreparedDataset) -> (Duration, usize) {
    let (tx, rx) = mpsc::channel(1024);

    let cfg = CounterDBConfig {
        interval: Duration::from_millis(100),
    };

    let actor = CounterDBActor::new(rx, cfg).expect("create counter db actor (requires redis)");
    let handle = tokio::spawn(async move { actor.run().await });

    let total_counters = prepared.expected_counters;
    let start = std::time::Instant::now();

    for tmpl in prepared.templates.iter() {
        for msg_idx in 0..tmpl.records {
            let msg = build_stats_message(tmpl.spec.counters, msg_idx as u64);
            let _ = tx.send(msg).await;
        }
    }

    drop(tx);
    let _ = handle.await;

    (start.elapsed(), total_counters)
}

fn counters_per_second(elapsed: Duration, counters: usize) -> f64 {
    if elapsed.as_secs_f64() > 0.0 {
        counters as f64 / elapsed.as_secs_f64()
    } else {
        0.0
    }
}

fn bench_counter_db_actor(c: &mut Criterion) {
    let mut group = c.benchmark_group("counter_db_actor_perf");
    group.sample_size(10);
    group.measurement_time(Duration::from_secs(20));

    for spec in datasets() {
        group.throughput(Throughput::Elements(
            spec.total_counters_per_iteration() as u64,
        ));

        let bench_id = BenchmarkId::from_parameter(spec.name);

        group.bench_function(bench_id, move |b| {
            let rt = Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("tokio runtime");

            let spec = spec.clone();

            b.to_async(&rt).iter_batched(
                {
                    let spec = spec.clone();
                    move || PreparedDataset::new(spec.clone())
                },
                move |prepared| async move {
                    flush_counters_db();
                    // Ensure required name map entries exist so lookups succeed
                    seed_port_name_map(32);
                    let (elapsed, counters) = run_stream(prepared).await;
                    let cps = counters_per_second(elapsed, counters);
                    println!(
                        "Dataset {} -> elapsed {:?}, counters {}, cps {:.2}",
                        spec.name, elapsed, counters, cps
                    );
                    flush_counters_db();
                },
                BatchSize::SmallInput,
            )
        });
    }

    group.finish();
}

criterion_group!(benches, bench_counter_db_actor);
criterion_main!(benches);