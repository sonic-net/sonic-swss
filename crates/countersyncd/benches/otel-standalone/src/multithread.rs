//! Thread-per-shard POC. Inputs are already partitioned before timing; this
//! measures sending/ACK scaling, not the cost of a production central router.
use countersyncd_otel_bench::{
    actor::otel::{OtelActor, OtelActorConfig},
    message::saistats::{SAIStat, SAIStatsBatch},
};
use std::{
    sync::{Arc, Barrier},
    thread,
    time::{Duration, Instant},
};
use tokio::{
    runtime::Builder,
    sync::{mpsc, oneshot},
};

#[cfg(feature = "mimalloc")]
#[global_allocator]
static ALLOCATOR: mimalloc::MiMalloc = mimalloc::MiMalloc;

// All samples of an exact series map to the same worker. Explicit deterministic
// byte hashing (no randomized per-process hasher) makes the POC repeatable.
fn shard(object: &str, type_id: u32, stat_id: u32, workers: usize) -> usize {
    let mut hash = 0xcbf29ce484222325u64;
    for byte in object
        .bytes()
        .chain(type_id.to_le_bytes())
        .chain(stat_id.to_le_bytes())
    {
        hash = (hash ^ u64::from(byte)).wrapping_mul(0x100000001b3);
    }
    hash as usize % workers
}
#[cfg(target_os = "linux")]
fn thread_cpu() -> f64 {
    let mut t = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    assert_eq!(
        unsafe { libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &mut t) },
        0
    );
    t.tv_sec as f64 + t.tv_nsec as f64 / 1e9
}
#[cfg(not(target_os = "linux"))]
fn thread_cpu() -> f64 {
    f64::NAN
}

fn main() {
    let args: Vec<_> = std::env::args().collect();
    let batch: usize = args.get(1).map(|s| s.parse().unwrap()).unwrap_or(10000);
    let points: usize = args.get(2).map(|s| s.parse().unwrap()).unwrap_or(40000000);
    let repeats: usize = args.get(3).map(|s| s.parse().unwrap()).unwrap_or(3);
    let worker_cpus: Vec<usize> = std::env::var("OTEL_CLIENT_CPUS")
        .unwrap_or("0".into())
        .split(',')
        .map(|s| s.parse().unwrap())
        .collect();
    assert!(!worker_cpus.is_empty() && batch > 0 && points > 0 && points % 500 == 0);
    let available = core_affinity::get_core_ids().unwrap();
    for (i, cpu) in worker_cpus.iter().enumerate() {
        assert!(available.iter().any(|c| c.id == *cpu));
        assert!(!worker_cpus[..i].contains(cpu));
    }
    let endpoint = std::env::var("OTEL_EXTERNAL_ENDPOINT")
        .expect("requires external Collector and independent count verification");
    let workers = worker_cpus.len();
    let lanes: usize = std::env::var("OTEL_LANES_PER_WORKER")
        .ok()
        .map(|s| s.parse().unwrap())
        .unwrap_or(1);
    assert!(lanes > 0 && workers * lanes <= 500);
    if lanes > 1 {
        assert_eq!(
            std::env::var("OTEL_MAX_IN_FLIGHT").unwrap_or("1".into()),
            "1",
            "ordered lanes require single-flight actors"
        );
    }
    let objects: Vec<Arc<str>> = (0..500)
        .map(|i| Arc::from(format!("Ethernet{i}")))
        .collect();
    let mut ids = vec![Vec::new(); workers * lanes];
    for (i, object) in objects.iter().enumerate() {
        ids[shard(object, 1, i as u32, workers * lanes)].push(i);
    }
    assert!(ids.iter().all(|s| !s.is_empty()));
    for trial in 0..repeats {
        let mut inputs = Vec::new();
        let mut expected = Vec::new();
        for series in &ids {
            let mut input = Vec::new();
            let records_per_message = (10000 / series.len()).max(1);
            for first in (0..points / 500).step_by(records_per_message) {
                let end = (first + records_per_message).min(points / 500);
                let mut message =
                    SAIStatsBatch::with_capacity(end - first, (end - first) * series.len());
                for record in first..end {
                    message.push_record(
                        1_700_000_000_000_000_000 + record as u64 * 10000,
                        series.iter().map(|&i| {
                            SAIStat::new(
                                objects[i].clone(),
                                1,
                                i as u32,
                                (record as u64)
                                    .wrapping_mul(1_000_003)
                                    .wrapping_add(i as u64),
                            )
                        }),
                    );
                }
                input.push(Arc::new(message));
            }
            expected.push(series.len() * points / 500);
            inputs.push(input);
        }
        assert_eq!(expected.iter().sum::<usize>(), points);
        // Runtime and actor setup complete before all threads start together.
        let ready = Arc::new(Barrier::new(workers + 1));
        let start_gate = Arc::new(Barrier::new(workers + 1));
        let mut inputs = inputs.into_iter();
        let grouped: Vec<_> = (0..workers)
            .map(|_| inputs.by_ref().take(lanes).collect::<Vec<_>>())
            .collect();
        let handles: Vec<_> = grouped
            .into_iter()
            .zip(&worker_cpus)
            .enumerate()
            .map(|(worker, (input, &cpu))| {
                let endpoint = endpoint.clone();
                let ready = ready.clone();
                let start_gate = start_gate.clone();
                let worker_points = expected[worker * lanes..(worker + 1) * lanes]
                    .iter()
                    .sum::<usize>();
                thread::spawn(move || {
                    assert!(core_affinity::set_for_current(core_affinity::CoreId {
                        id: cpu
                    }));
                    Builder::new_current_thread()
                        .enable_all()
                        .build()
                        .unwrap()
                        .block_on(async {
                            let mut prepared = Vec::new();
                            for lane_input in input {
                                let (tx, rx) = mpsc::channel(8);
                                let (shutdown_tx, _) = oneshot::channel();
                                let actor = OtelActor::new(
                                    rx,
                                    OtelActorConfig {
                                        collector_endpoint: endpoint.clone(),
                                        max_counters_per_export: batch,
                                        flush_timeout: Duration::from_secs(1),
                                    },
                                    shutdown_tx,
                                )
                                .await
                                .unwrap();
                                prepared.push((lane_input, tx, actor));
                            }
                            ready.wait();
                            start_gate.wait();
                            let cpu_start = thread_cpu();
                            let start = Instant::now();
                            let mut tasks = Vec::new();
                            for (input, tx, actor) in prepared {
                                tasks.push(async move {
                                    let producer = async move {
                                        for message in input {
                                            tx.send(message).await.map_err(|e| e.to_string())?;
                                        }
                                        Ok::<_, String>(())
                                    };
                                    tokio::try_join!(producer, async {
                                        actor.run().await.map_err(|e| e.to_string())
                                    })?;
                                    Ok::<_, String>(())
                                });
                            }
                            tokio::time::timeout(Duration::from_secs(180), async {
                                futures_util::future::try_join_all(tasks).await?;
                                Ok::<_, String>(())
                            })
                            .await
                            .expect("worker timeout")
                            .expect("every worker export must ACK");
                            (
                                worker,
                                worker_points,
                                start.elapsed().as_secs_f64(),
                                thread_cpu() - cpu_start,
                            )
                        })
                })
            })
            .collect();
        ready.wait();
        let start = Instant::now();
        start_gate.wait();
        let results: Vec<_> = handles
            .into_iter()
            .map(|h| h.join().expect("worker must finish"))
            .collect();
        let elapsed = start.elapsed().as_secs_f64();
        let cpu = results.iter().map(|r| r.3).sum::<f64>();
        println!("trial={trial} workers={workers} lanes_per_worker={lanes} batch={batch} points={points} elapsed_s={elapsed:.6} acked_Mpoints_s={:.3} client_cpu_s={cpu:.6} client_cpu_cores={:.3} worker_details={results:?}",points as f64/elapsed/1e6,cpu/elapsed);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn partitions_all_series_once_and_keeps_time_independent_routing() {
        for workers in [1, 2, 4, 8] {
            let mut counts = vec![0; workers];
            for id in 0..500 {
                let object = format!("Ethernet{id}");
                let owner = shard(&object, 1, id, workers);
                assert!(owner < workers);
                counts[owner] += 1;
                assert_eq!(owner, shard(&object, 1, id, workers));
            }
            assert_eq!(counts.iter().sum::<usize>(), 500);
            assert!(counts.iter().all(|&n| n > 0));
        }
    }
}
