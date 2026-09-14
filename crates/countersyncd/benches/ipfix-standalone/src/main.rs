//! Actual IPFIX actor/output-consumption benchmark. No OTel or database code.
//! Args: counters/record, total counters, repetitions, borrowed|legacy.
use countersyncd_ipfix_bench::{
    actor::ipfix::IpfixActor,
    message::{ipfix::IPFixTemplatesMessage, saistats::SAIStatsBatchMessage},
};
use std::{
    sync::Arc,
    time::{Duration, Instant},
};
use tokio::{runtime::Builder, sync::mpsc};

#[path = "../../../tests/ipfix_test_helpers.rs"]
#[allow(dead_code)]
mod helpers;

#[cfg(feature = "mimalloc")]
#[global_allocator]
static ALLOCATOR: mimalloc::MiMalloc = mimalloc::MiMalloc;

fn pin() {
    #[cfg(target_os = "linux")]
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        libc::CPU_ZERO(&mut set);
        libc::CPU_SET(0, &mut set);
        assert_eq!(
            libc::sched_setaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &set),
            0
        );
    }
}

fn main() {
    let args: Vec<_> = std::env::args().collect();
    let fields: usize = args.get(1).map(|s| s.parse().unwrap()).unwrap_or(500);
    let points: usize = args
        .get(2)
        .map(|s| s.parse().unwrap())
        .unwrap_or(400_000_000);
    let repeats: usize = args.get(3).map(|s| s.parse().unwrap()).unwrap_or(3);
    let legacy = args.get(4).is_some_and(|s| s == "legacy");
    assert!(fields > 0 && points > 0 && points % fields == 0 && repeats > 0);
    let mut templates = helpers::generate_ipfix_templates(fields, 300);
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
                field.copy_from_slice(&(sequence as u64 * 1_000_003 + i as u64).to_be_bytes());
            }
            Arc::new(bytes)
        })
        .collect();
    for trial in 0..repeats {
        let update = update.clone();
        let payloads = payloads.clone();
        let elapsed = std::thread::spawn(move || {
            pin();
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
                    let task = tokio::spawn(IpfixActor::run(actor));
                    tt.send(update).await.unwrap();
                    let permit = tt.reserve().await.unwrap();
                    bt.send(payloads[0].clone()).await.unwrap();
                    let probe = sr.recv().await.unwrap();
                    assert_eq!(probe.counter_count(), fields);
                    drop(probe);
                    drop(permit);
                    let records = points / fields;
                    let expected_sum = (0..records).fold(0u64, |sum, r| {
                        sum.wrapping_add((fields as u64).wrapping_mul((r % 16) as u64 * 1_000_003))
                            .wrapping_add((fields * (fields - 1) / 2) as u64)
                    });
                    let start = Instant::now();
                    let producer = async move {
                        for r in 0..records {
                            bt.send(payloads[r % 16].clone()).await.unwrap();
                        }
                        drop(bt);
                    };
                    let consumer = async move {
                        let (mut count, mut sum, mut record_count) = (0usize, 0u64, 0usize);
                        while let Some(batch) = sr.recv().await {
                            // This branch is also the old-source baseline; do not call
                            // new-only APIs when compiling baseline-legacy.
                            if legacy || cfg!(feature = "baseline-legacy") {
                                for record in batch.iter() {
                                    record_count += 1;
                                    for stat in record.stats {
                                        count += 1;
                                        sum = sum.wrapping_add(stat.counter);
                                    }
                                }
                            } else {
                                #[cfg(not(feature = "baseline-legacy"))]
                                for record in batch.records() {
                                    record_count += 1;
                                    for stat in record.stats {
                                        count += 1;
                                        sum = sum.wrapping_add(stat.counter);
                                    }
                                }
                            }
                        }
                        assert_eq!(count, points);
                        assert_eq!(record_count, records);
                        assert_eq!(sum, expected_sum);
                    };
                    tokio::time::timeout(Duration::from_secs(180), async {
                        tokio::join!(producer, consumer);
                        task.await.unwrap();
                    })
                    .await
                    .unwrap();
                    // Closing the template channel sooner can race with draining
                    // data in the upstream actor. Keep it open until completion.
                    drop(tt);
                    start.elapsed().as_secs_f64()
                })
        })
        .join()
        .unwrap();
        println!("trial={trial} fields={fields} points={points} legacy_adapter={} elapsed_s={elapsed:.6} Mpoints_s={:.3} checksum=true",
                 legacy || cfg!(feature = "baseline-legacy"), points as f64 / elapsed / 1e6);
    }
}
