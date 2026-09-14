//! Real IpfixActor including parsing, output iteration, checksums and destruction.
//! Reuses a 16-payload input pool (not a live network/generator benchmark).
use countersyncd_otel_bench::{
    actor::{
        ipfix::IpfixActor,
        otel::{OtelActorConfig, OtelWorkerConfig, OtelWorkerPool},
    },
    message::{ipfix::IPFixTemplatesMessage, saistats::SAIStatsBatchMessage},
};
use std::{
    sync::Arc,
    time::{Duration, Instant},
};
use tokio::{
    runtime::Builder,
    sync::{mpsc, oneshot},
};
#[path = "../../../tests/ipfix_test_helpers.rs"]
#[allow(dead_code)]
mod helpers;
#[cfg(feature = "mimalloc")]
#[global_allocator]
static ALLOCATOR: mimalloc::MiMalloc = mimalloc::MiMalloc;
fn main() {
    let args: Vec<_> = std::env::args().collect();
    let fields: usize = args.get(1).map(|s| s.parse().unwrap()).unwrap_or(500);
    let points: usize = args.get(2).map(|s| s.parse().unwrap()).unwrap_or(40000000);
    let repeats: usize = args.get(3).map(|s| s.parse().unwrap()).unwrap_or(3);
    assert!(fields > 0 && points % fields == 0);
    let mut templates = helpers::generate_ipfix_templates(fields, 300);
    // Canonical PORT stats instead of synthetic object types, preserving field IDs.
    for (i, field) in templates[28..].chunks_exact_mut(8).enumerate() {
        field[4..8].copy_from_slice(&(0x10000u32 + (i % 100) as u32).to_be_bytes());
    }
    let (names, ids) = helpers::generate_object_metadata(fields);
    let update = IPFixTemplatesMessage::new(
        "bench".into(),
        Arc::new(templates.clone()),
        Some(names),
        Some(ids),
    );
    let base = helpers::generate_ipfix_records(&templates);
    let payloads: Vec<_> = (0..16)
        .map(|sequence| {
            let mut bytes = base.clone();
            bytes[20..28]
                .copy_from_slice(&(1_700_000_000_000_000_000u64 + sequence as u64).to_be_bytes());
            for (i, field) in bytes[28..].chunks_exact_mut(8).enumerate() {
                field.copy_from_slice(&(sequence as u64 * 1000003 + i as u64).to_be_bytes());
            }
            Arc::new(bytes)
        })
        .collect();
    for trial in 0..repeats {
        let update = update.clone();
        let payloads = payloads.clone();
        let result = std::thread::spawn(move || {
            let allowed = core_affinity::get_core_ids().unwrap();
            assert!(core_affinity::set_for_current(core_affinity::CoreId {
                id: 0
            }));
            Builder::new_current_thread()
                .enable_all()
                .build()
                .unwrap()
                .block_on(async move {
                    let (tt, tr) = mpsc::channel(1);
                    let (bt, br) = mpsc::channel(64);
                    let (st, mut sr) = mpsc::channel::<SAIStatsBatchMessage>(64);
                    let mut actor = IpfixActor::new(tr, br);
                    actor.add_recipient(st);
                    let ipfix = tokio::spawn(IpfixActor::run(actor));
                    tt.send(update).await.unwrap();
                    let permit = tt.reserve().await.unwrap();
                    bt.send(payloads[0].clone()).await.unwrap();
                    let probe = sr.recv().await.unwrap();
                    assert_eq!(probe.counter_count(), fields);
                    drop(probe);
                    drop(permit);
                    let records = points / fields;
                    let expected_sum = (0..records).fold(0u64, |sum, r| {
                        sum.wrapping_add((fields as u64).wrapping_mul((r % 16) as u64 * 1000003))
                            .wrapping_add((fields * (fields - 1) / 2) as u64)
                    });
                    let endpoint = std::env::var("OTEL_EXTERNAL_ENDPOINT").ok();
                    let start = Instant::now();
                    let producer = async move {
                        for r in 0..records {
                            bt.send(payloads[r % 16].clone()).await.unwrap();
                        }
                        drop(bt);
                    };
                    let consumer = async move {
                        if let Some(endpoint) = endpoint {
                            let (done, _) = oneshot::channel();
                            // Construct on a helper thread with the original CPU
                            // mask; the timed IPFIX/router thread remains CPU 0.
                            let _mask = allowed.clone();
                            let pool = std::thread::spawn(move || {
                                #[cfg(target_os = "linux")]
                                unsafe {
                                    let mut set: libc::cpu_set_t = std::mem::zeroed();
                                    libc::CPU_ZERO(&mut set);
                                    for cpu in _mask {
                                        libc::CPU_SET(cpu.id, &mut set);
                                    }
                                    assert_eq!(
                                        libc::sched_setaffinity(
                                            0,
                                            std::mem::size_of::<libc::cpu_set_t>(),
                                            &set
                                        ),
                                        0
                                    );
                                }
                                let pool = OtelWorkerPool::new(
                                    sr,
                                    OtelActorConfig {
                                        collector_endpoint: endpoint,
                                        ..Default::default()
                                    },
                                    OtelWorkerConfig {
                                        threads: 3,
                                        in_flight_per_worker: 2,
                                        cpu_ids: vec![2, 4, 6],
                                        ..Default::default()
                                    },
                                    done,
                                )
                                .unwrap();
                                pool
                            })
                            .join()
                            .unwrap();
                            pool.run().await.unwrap();
                            return;
                        }
                        let mut count = 0usize;
                        let mut sum = 0u64;
                        let mut record_count = 0usize;
                        while let Some(batch) = sr.recv().await {
                            #[cfg(feature = "baseline-legacy")]
                            let views = batch.iter();
                            #[cfg(not(feature = "baseline-legacy"))]
                            let views = batch.iter();
                            for record in views {
                                record_count += 1;
                                for stat in record.stats {
                                    count += 1;
                                    sum = sum.wrapping_add(stat.counter);
                                }
                            }
                        }
                        assert_eq!(count, points);
                        assert_eq!(record_count, records);
                        assert_eq!(sum, expected_sum);
                    };
                    tokio::time::timeout(Duration::from_secs(180), async {
                        tokio::join!(producer, consumer);
                        ipfix.await.unwrap();
                    })
                    .await
                    .unwrap();
                    drop(tt);
                    start.elapsed().as_secs_f64()
                })
        })
        .join()
        .unwrap();
        println!("trial={trial} fields={fields} points={points} elapsed_s={result:.6} Mpoints_s={:.3} checksum_iteration={}",points as f64/result/1e6,std::env::var_os("OTEL_EXTERNAL_ENDPOINT").is_none());
    }
}
