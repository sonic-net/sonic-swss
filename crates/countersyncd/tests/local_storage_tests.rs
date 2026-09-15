//! Correctness tests for the shared local-storage benchmark helpers, not performance runs.

#![cfg(not(target_arch = "arm"))]

#[path = "../benchmark_support/mod.rs"]
mod support;

use std::{
    io::{self, Write},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

use clap::ValueEnum;
use support::*;
use wait_timeout::ChildExt;

#[test]
fn generator_properties() {
    assert_eq!(ENVELOPE_NS, 100_000_000);
    generator_self_check();
}

#[test]
fn pattern_names_and_stress_values() {
    assert_eq!(
        Pattern::value_variants()
            .iter()
            .map(|p| p.name())
            .collect::<Vec<_>>(),
        [
            "idle",
            "burst",
            "sustained",
            "mixed",
            "constant",
            "large100",
            "large33",
            "large66"
        ]
    );
    for &pattern in Pattern::value_variants() {
        assert_eq!(
            Pattern::from_str(pattern.name(), false).unwrap().name(),
            pattern.name()
        );
        assert_eq!(
            pattern.to_possible_value().unwrap().get_name(),
            pattern.name()
        );
    }
    assert_eq!(
        Pattern::from_str("staggered", false).unwrap().name(),
        "mixed"
    );
    assert!(Pattern::from_str("unknown", false).is_err());
    for (pattern, count) in [
        (Pattern::Large100, 500),
        (Pattern::Large33, 167),
        (Pattern::Large66, 334),
    ] {
        assert_eq!(
            (0..500).filter(|&index| pattern.large(index)).count(),
            count
        );
        for index in [0, 1, 2, 499, 7999] {
            let first = pattern.value(0, index, 10_000);
            let later = pattern.value(60_000, index, 10_000);
            if pattern.large(index) {
                assert_ne!(first, later);
                assert!((0..32).any(|seq| pattern.value(seq, index, 10_000) > i64::MAX as u64));
            } else {
                assert_eq!(later, first + 60_000);
            }
            assert_eq!(later, pattern.value(60_000, index, 1));
        }
    }
    for index in [0, 1, 499, 7999] {
        assert!(!Pattern::Constant.large(index));
        assert_eq!(
            Pattern::Constant.value(0, index, 10_000),
            (index as u64 + 1) * 1_000_000
        );
        assert_eq!(
            Pattern::Constant.value(0, index, 10_000),
            Pattern::Constant.value(60_000, index, 1)
        );
    }
    assert_eq!(hash_value(1, 0), 128);
    assert_eq!(hash_value(0, 1), 0x5692161d100b05e5);
    assert_ne!(
        hash_value(hash_value(0, 1), 2),
        hash_value(hash_value(0, 2), 1)
    );
}

#[test]
fn arithmetic_preserves_full_width_and_pacing() {
    assert_eq!(BPS_ASSUMPTION, 200_000_000_000);
    assert_eq!(max_bytes(0), 0);
    assert_eq!(max_bytes(u64::MAX), u128::from(u64::MAX) * 25);
    assert_eq!(audit_bytes(6_000_000, true), Some(96_000_000));
    assert_eq!(audit_bytes(6_000_000, false), Some(48_000_000));
    assert_eq!(audit_bytes(0, true), Some(0));
    for hashes in [false, true] {
        let bytes = if hashes { 16 } else { 8 };
        assert_eq!(
            audit_bytes(usize::MAX / bytes, hashes),
            Some(usize::MAX / bytes * bytes)
        );
        assert_eq!(audit_bytes(usize::MAX / bytes + 1, hashes), None);
    }
    assert_eq!(metric_count(600_000, 8000), 4_800_000_000);
    assert_eq!(metric_count(6_000_000, 8000), 48_000_000_000);
    assert_eq!(metric_count(u64::MAX, 1), u64::MAX);
    assert_eq!(metric_count(u64::MAX, 0), 0);
    let before = pacing_target(metric_count(536_870, 8000), 1_000_000);
    let after = pacing_target(metric_count(536_871, 8000), 1_000_000);
    assert!(after > before);
    assert_eq!(after, Duration::from_millis(4_294_968));
    assert_eq!(pacing_target(0, 1_000_000), Duration::ZERO);
}

#[test]
#[should_panic(expected = "validated metric count fits u64")]
fn metric_count_rejects_overflow() {
    metric_count(u64::MAX, 2);
}

#[test]
fn shards_sort_by_sequence_not_wall_clock_or_text() {
    let names = [
        "00000000000000000030-0000000042-00000000000000000000.arrow",
        "00000000000000000020-0000000042-00000000000000000001.arrow",
        "00000000000000000010-0000000042-00000000000000000002.arrow",
    ]
    .map(PathBuf::from);
    assert_eq!(
        sort_shards(vec![names[2].clone(), names[0].clone(), names[1].clone()]).unwrap(),
        names
    );
    assert_eq!(shard_sequence(Path::new("1-42-10.arrow")).unwrap(), 10);
    assert_eq!(
        shard_sequence(Path::new("1-42-18446744073709551615.arrow")).unwrap(),
        u64::MAX
    );
    assert_eq!(
        sort_shards(vec![
            PathBuf::from("1-42-10.arrow"),
            PathBuf::from("1-42-2.arrow")
        ])
        .unwrap(),
        [
            PathBuf::from("1-42-2.arrow"),
            PathBuf::from("1-42-10.arrow")
        ]
    );
    assert!(sort_shards(Vec::new()).unwrap().is_empty());
    for name in [
        "unknown.arrow",
        "1-42-bad.arrow",
        "1-42-0.partial",
        "1-42-18446744073709551616.arrow",
        "1-42-.arrow",
        "1-42-+1.arrow",
        "1-42-1-2.arrow",
        "bad-42-0.arrow",
    ] {
        assert!(shard_sequence(Path::new(name)).is_err());
        assert!(sort_shards(vec![names[0].clone(), PathBuf::from(name)]).is_err());
    }
}

#[test]
fn filesystem_magics_preserve_signed_32_bit_patterns() {
    for magic in [0xef53u32, 0x58465342, 0x9123683e, 0x2fc12fc1, 0xf2f52010] {
        assert!(is_disk_filesystem(magic));
        assert!(is_disk_filesystem((magic as i32) as u32));
        assert!(is_disk_filesystem(i64::from(magic as i32) as u32));
        assert!(is_disk_filesystem(i64::from(magic) as u32));
        let mut stat: libc::statfs = unsafe { std::mem::zeroed() };
        stat.f_type = magic as _;
        assert!(is_disk_filesystem(stat.f_type as u32));
    }
    for magic in [0x9123683eu32, 0xf2f52010] {
        assert!((magic as i32) < 0, "btrfs/f2fs exercise the sign bit");
    }
    // tmpfs, overlay, ramfs and unknown filesystems must remain rejected.
    for magic in [0x01021994u32, 0x794c7630, 0x858458f6, 0, u32::MAX] {
        assert!(!is_disk_filesystem(magic));
        assert!(!is_disk_filesystem(i64::from(magic as i32) as u32));
    }
}

#[test]
fn sampler_drop_stops_and_joins_before_cancelling_deadline() {
    let deadline = HardDeadline::new(Duration::from_secs(60));
    let (stop, rx) = mpsc::channel();
    let (finished, done) = mpsc::channel();
    let handle = thread::spawn(move || {
        rx.recv().unwrap();
        finished.send(()).unwrap();
    });
    drop(SamplerGuard::new(deadline, stop, handle));
    // Drop must have joined, not merely sent the stop signal.
    assert_eq!(done.try_recv(), Ok(()));
    assert_eq!(done.try_recv(), Err(mpsc::TryRecvError::Disconnected));
}

#[test]
fn watchdog_blocked_sampler_and_stdout() {
    const CHILD: &str = "LOCAL_STORAGE_BENCH_WATCHDOG_CHILD";
    if std::env::var_os(CHILD).is_some() {
        let deadline = HardDeadline::new(Duration::from_secs(1));
        let (ready, rx) = mpsc::channel();
        let handle = thread::spawn(move || {
            let mut stdout = io::stdout().lock();
            ready.send(()).unwrap();
            // The parent deliberately never drains this pipe. Simulate the
            // accounting sampler blocking while holding the stdout lock.
            for _ in 0..64 {
                stdout.write_all(&[b'x'; 64 * 1024]).unwrap();
            }
            thread::sleep(Duration::from_secs(60));
        });
        rx.recv().unwrap();
        let (stop, _rx) = mpsc::channel();
        let guard = SamplerGuard::new(deadline, stop, handle);
        drop(guard); // The actual watchdog lifecycle must bound the blocked join.
        panic!("blocked sampler unexpectedly returned");
    }
    let start = Instant::now();
    let mut child = Command::new(std::env::current_exe().unwrap())
        .args([
            "--exact",
            "watchdog_blocked_sampler_and_stdout",
            "--nocapture",
        ])
        .env(CHILD, "1")
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let status = child.wait_timeout(Duration::from_secs(5)).unwrap();
    if status.is_none() {
        child.kill().unwrap();
        child.wait().unwrap();
        panic!("hard deadline failed with blocked sampler/stdout");
    }
    assert_eq!(status.unwrap().code(), Some(124));
    assert!(start.elapsed() >= Duration::from_secs(1));
}
