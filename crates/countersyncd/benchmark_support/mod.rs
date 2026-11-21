//! Benchmark-only helpers shared with independent regression tests, not a shipped API.

use std::{
    path::{Path, PathBuf},
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

use clap::ValueEnum;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>;
pub(super) const BPS_ASSUMPTION: u64 = 200_000_000_000;
pub(super) const ENVELOPE_NS: u64 = 100_000_000;

pub(super) fn max_bytes(step_ns: u64) -> u128 {
    u128::from(BPS_ASSUMPTION) * u128::from(step_ns) / (8 * 1_000_000_000u128)
}

pub(super) fn audit_bytes(records: usize, hashes: bool) -> Option<usize> {
    // Conservatively charge 8 flag bytes/record for packet, decoded and raw
    // bitmaps (including spare capacity), plus an optional 8-byte value hash.
    records.checked_mul(if hashes { 16 } else { 8 })
}

#[derive(Clone, Copy, Debug, ValueEnum)]
pub(super) enum Pattern {
    Idle,
    Burst,
    Sustained,
    #[value(alias = "staggered")]
    Mixed,
    Constant,
    Large100,
    Large33,
    Large66,
}
impl Pattern {
    pub(super) fn name(self) -> &'static str {
        match self {
            Self::Idle => "idle",
            Self::Burst => "burst",
            Self::Sustained => "sustained",
            Self::Mixed => "mixed",
            Self::Constant => "constant",
            Self::Large100 => "large100",
            Self::Large33 => "large33",
            Self::Large66 => "large66",
        }
    }
    pub(super) fn large(self, index: usize) -> bool {
        match self {
            Self::Idle | Self::Burst | Self::Sustained | Self::Mixed | Self::Constant => false,
            Self::Large100 => true,
            Self::Large33 => index % 3 == 0,
            Self::Large66 => index % 3 < 2,
        }
    }
    pub(super) fn value(self, sequence: usize, index: usize, step_ns: u64) -> u64 {
        if matches!(
            self,
            Self::Idle | Self::Burst | Self::Sustained | Self::Mixed
        ) {
            let time = (sequence as u64 + 1) * step_ns;
            let series = index as u64;
            let epoch = time / ENVELOPE_NS;
            let phase = (time % ENVELOPE_NS + series * 104_729) % ENVELOPE_NS;
            let ticks = time / 10_000 + series * 17;
            let low_phase = (time / 1_000_000 + series * 37) % 101;
            let low = if low_phase < 80 {
                0
            } else {
                (ticks / 31 + epoch) % 7
            };
            let width = if matches!(self, Self::Mixed) {
                (5 + series % 16) * 1_000_000
            } else {
                10_000_000
            };
            let value = if matches!(self, Self::Idle) {
                low
            } else if matches!(self, Self::Sustained) || phase < width {
                let half_period = 31 + series % 17 + epoch % 13;
                let wave = ticks % (2 * half_period);
                let triangle = wave.min(2 * half_period - wave);
                let jitter = (ticks ^ (ticks >> 4) ^ series) & 3;
                let high = 250_000 - triangle * (1 + series % 5) - jitter;
                if matches!(self, Self::Sustained) {
                    high
                } else {
                    high * phase.min(width - phase).min(2_000_000) / 2_000_000
                }
            } else if phase < width + 2_000_000 {
                let plateau = match (phase - width) / 400_000 {
                    0 => 10_000,
                    1 | 2 => 101_000,
                    3 => 10_200,
                    _ => 10_201,
                };
                plateau
                    + if index % 8 == 0 {
                        0
                    } else {
                        (ticks / 23 + series + epoch) % 7
                    }
            } else {
                low
            };
            return (u128::from(value) * u128::from(step_ns) / 10_000) as u64;
        }
        if self.large(index) {
            mix((sequence as u64).wrapping_mul(0x9e3779b97f4a7c15) ^ mix(index as u64 + 0x12345678))
        } else {
            (index as u64 + 1) * 1_000_000
                + if matches!(self, Self::Constant) {
                    0
                } else {
                    sequence as u64
                }
        }
    }
}

// Shared property validation; callers own test registration and CLI diagnostics.
pub(super) fn generator_self_check() {
    assert_eq!(max_bytes(10_000), 250_000);
    assert_eq!(max_bytes(1), 25);
    assert_eq!(
        [999, 1039, 1079, 1119, 1159].map(|seq| Pattern::Burst.value(seq, 0, 10_000)),
        [10_000, 101_000, 101_000, 10_200, 10_201]
    );
    for pattern in [
        Pattern::Idle,
        Pattern::Burst,
        Pattern::Sustained,
        Pattern::Mixed,
    ] {
        for index in [0, 1, 7, 8, 499, 7999] {
            for epoch in [0, 1, 59, 599] {
                let mut minimum = u64::MAX;
                let mut maximum = 0;
                let mut falls = 0;
                let mut previous = 0;
                for sequence in epoch * 10_000..(epoch + 1) * 10_000 {
                    let value = pattern.value(sequence, index, 10_000);
                    assert!(value <= 250_000);
                    minimum = minimum.min(value);
                    maximum = maximum.max(value);
                    falls += usize::from(value < previous);
                    previous = value;
                }
                match pattern {
                    Pattern::Idle => assert!(minimum == 0 && maximum <= 6),
                    Pattern::Sustained => assert!(minimum >= 249_000 && maximum > minimum),
                    _ => assert!(minimum == 0 && maximum >= 249_000 && falls > 0),
                }
            }
        }
        for step in [1, 9999, 10_000, 10_001, 1_000_000] {
            for sequence in 0..1000 {
                assert!(u128::from(pattern.value(sequence, 7, step)) <= max_bytes(step));
            }
        }
    }
    for pattern in [Pattern::Burst, Pattern::Mixed, Pattern::Sustained] {
        assert!(
            (0..10_000).any(|seq| pattern.value(seq, 0, 10_000) != pattern.value(seq, 1, 10_000))
        );
        assert!((0..10_000)
            .any(|seq| pattern.value(seq, 0, 10_000) != pattern.value(seq + 10_000, 0, 10_000)));
    }
}

fn mix(mut x: u64) -> u64 {
    x = (x ^ (x >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)).wrapping_mul(0x94d049bb133111eb);
    x ^ (x >> 31)
}

pub(super) fn hash_value(hash: u64, value: u64) -> u64 {
    hash.rotate_left(7) ^ mix(value)
}

pub(super) struct HardDeadline {
    cancel: mpsc::Sender<()>,
    handle: Option<thread::JoinHandle<()>>,
}

impl HardDeadline {
    pub(super) fn new(timeout: Duration) -> Self {
        let start = Instant::now();
        let (cancel, rx) = mpsc::channel();
        let handle = thread::spawn(move || {
            if matches!(
                rx.recv_timeout(timeout.saturating_sub(start.elapsed())),
                Err(mpsc::RecvTimeoutError::Timeout)
            ) {
                // No I/O, allocator work, exit handlers or destructors: another
                // thread may hold any of their locks or be stuck in filesystem I/O.
                unsafe { libc::_exit(124) }
            }
        });
        Self {
            cancel,
            handle: Some(handle),
        }
    }
}

impl Drop for HardDeadline {
    fn drop(&mut self) {
        let _ = self.cancel.send(());
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

pub(super) struct SamplerGuard {
    // Field destruction follows Drop, so the deadline also bounds the sampler join.
    _deadline: HardDeadline,
    stop: mpsc::Sender<()>,
    handle: Option<thread::JoinHandle<()>>,
}

impl SamplerGuard {
    pub(super) fn new(
        deadline: HardDeadline,
        stop: mpsc::Sender<()>,
        handle: thread::JoinHandle<()>,
    ) -> Self {
        Self {
            _deadline: deadline,
            stop,
            handle: Some(handle),
        }
    }
}

impl Drop for SamplerGuard {
    fn drop(&mut self) {
        let _ = self.stop.send(());
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

pub(super) fn metric_count(records: u64, counters: usize) -> u64 {
    records
        .checked_mul(counters as u64)
        .expect("validated metric count fits u64")
}

pub(super) fn pacing_target(metrics: u64, rate: u64) -> Duration {
    Duration::from_secs_f64(metrics as f64 / rate as f64)
}

pub(super) fn shard_sequence(path: &Path) -> Result<u64> {
    let invalid = || format!("unexpected shard name: {}", path.display());
    let stem = path
        .file_stem()
        .and_then(|name| name.to_str())
        .ok_or_else(invalid)?;
    let parts: Vec<_> = stem.split('-').collect();
    if path
        .extension()
        .is_none_or(|extension| extension != "arrow")
        || parts.len() != 3
        || parts
            .iter()
            .any(|part| part.is_empty() || !part.bytes().all(|byte| byte.is_ascii_digit()))
    {
        return Err(invalid().into());
    }
    Ok(parts[2].parse()?)
}

pub(super) fn sort_shards(paths: Vec<PathBuf>) -> Result<Vec<PathBuf>> {
    // Private segments have one writer; its sequence survives wall-clock rollback.
    let mut ordered = paths
        .into_iter()
        .map(|path| Ok((shard_sequence(&path)?, path)))
        .collect::<Result<Vec<_>>>()?;
    ordered.sort_by_key(|(sequence, _)| *sequence);
    Ok(ordered.into_iter().map(|(_, path)| path).collect())
}

pub(super) fn is_disk_filesystem(fs_type: u32) -> bool {
    const DISK_FILESYSTEMS: [u32; 5] = [0xef53, 0x58465342, 0x9123683e, 0x2fc12fc1, 0xf2f52010];
    // Overlay cannot prove a real-disk backing filesystem: require a bind mount.
    DISK_FILESYSTEMS.contains(&fs_type)
}
