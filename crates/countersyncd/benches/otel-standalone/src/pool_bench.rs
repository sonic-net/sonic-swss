//! Actual worker pool including live routing/hash/stat-cloning cost. Only raw
//! input generation is outside timing. External receiver counters are required.
use clap::Parser;
use countersyncd_otel_bench::{
    actor::otel::{OtelActorConfig, OtelWorkerConfig, OtelWorkerPool},
    message::saistats::{SAIStat, SAIStatsBatch},
};
use std::{
    sync::Arc,
    time::{Duration, Instant},
};
use tokio::{
    runtime::Builder,
    sync::{mpsc, oneshot},
};
#[cfg(feature = "mimalloc")]
#[global_allocator]
static ALLOCATOR: mimalloc::MiMalloc = mimalloc::MiMalloc;

#[derive(Parser)]
struct Args {
    #[arg(long, default_value = "http://127.0.0.1:24317")]
    endpoint: String,
    #[arg(long, default_value_t = 1)]
    threads: usize,
    #[arg(long, default_value_t = 1)]
    in_flight_per_worker: usize,
    #[arg(long, value_delimiter = ',')]
    cpus: Vec<usize>,
    #[arg(long)]
    router_cpu: Option<usize>,
    #[arg(long, default_value_t = 8)]
    queue_capacity: usize,
    #[arg(long, default_value_t = 10000)]
    batch: usize,
    #[arg(long, default_value_t = 40000000)]
    points: usize,
    #[arg(long, default_value_t = 3)]
    repeats: usize,
}
fn main() {
    let args = Args::parse();
    assert!(args.points > 0 && args.points % 500 == 0 && args.repeats > 0 && args.batch > 0);
    let workers = OtelWorkerConfig {
        threads: args.threads,
        in_flight_per_worker: args.in_flight_per_worker,
        cpu_ids: args.cpus.clone(),
        queue_capacity: args.queue_capacity,
    };
    workers.validate().expect("invalid worker configuration");
    for trial in 0..args.repeats {
        let objects: Vec<Arc<str>> = (0..500)
            .map(|i| Arc::from(format!("Ethernet{i}")))
            .collect();
        let input: Vec<_> = (0..args.points / 500)
            .step_by(20)
            .map(|first| {
                let mut batch = SAIStatsBatch::with_capacity(20, 10000);
                for record in first..(first + 20).min(args.points / 500) {
                    batch.push_record(
                        1_700_000_000_000_000_000 + record as u64 * 10000,
                        (0..500).map(|i| {
                            SAIStat::new(
                                objects[i].clone(),
                                1,
                                (i % 100) as u32,
                                record as u64 * 1_000_003 + i as u64,
                            )
                        }),
                    );
                }
                Arc::new(batch)
            })
            .collect();
        let runtime = Builder::new_current_thread().enable_all().build().unwrap();
        std::thread::scope(|scope| {
            scope.spawn(|| runtime.block_on(async {
            let (tx,rx)=mpsc::channel(8);let (done,done_rx)=oneshot::channel();
            let pool=OtelWorkerPool::new(rx,OtelActorConfig {collector_endpoint:args.endpoint.clone(),max_counters_per_export:args.batch,flush_timeout:Duration::from_secs(1)},workers.clone(),done).expect("create pool");
            if let Some(cpu)=args.router_cpu {
                assert!(core_affinity::get_core_ids().unwrap().iter().any(|c|c.id==cpu));
                assert!(core_affinity::set_for_current(core_affinity::CoreId{id:cpu}));
            }
            let producer=async move {for batch in input {tx.send(batch).await.map_err(|e|e.to_string())?;}drop(tx);Ok::<_,String>(())};
            let start=Instant::now();
            tokio::time::timeout(Duration::from_secs(180),async {
                tokio::try_join!(producer,async {pool.run().await.map_err(|e|e.to_string())})
            }).await.expect("pool timed out").expect("all routed samples must ACK");
            done_rx.await.expect("pool shutdown notified");
            let elapsed=start.elapsed().as_secs_f64();
            println!("trial={trial} threads={} in_flight_per_worker={} max_total_inflight={} batch={} points={} elapsed_s={elapsed:.6} acked_Mpoints_s={:.3} live_routing=true",args.threads,args.in_flight_per_worker,workers.lanes(),args.batch,args.points,args.points as f64/elapsed/1e6);
        })).join().expect("router completed");
        });
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn pool_workload_has_500_series_and_100_known_stat_names() {
        use countersyncd_otel_bench::message::otel::sai_metric_names;
        use std::collections::HashSet;
        let series: HashSet<_> = (0..500)
            .map(|i| (format!("Ethernet{i}"), 1u32, (i % 100) as u32))
            .collect();
        assert_eq!(series.len(), 500);
        let names: HashSet<_> = series
            .iter()
            .map(|(_, t, s)| {
                let (object, name) = sai_metric_names(*t, *s);
                assert_eq!(object, "SAI_OBJECT_TYPE_PORT");
                assert!(name.starts_with("SAI_PORT_STAT_"));
                name.into_owned()
            })
            .collect();
        assert_eq!(names.len(), 100);
    }
}
