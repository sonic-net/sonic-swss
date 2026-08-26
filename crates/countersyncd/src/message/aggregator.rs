use std::{
    collections::{BTreeMap, HashSet},
    sync::{Arc, OnceLock},
};

use super::saistats::SAIStatsMessage;
use crate::sai::{
    SaiBufferPoolStat, SaiIngressPriorityGroupStat, SaiObjectType, SaiPortStat, SaiQueueStat,
};

pub const DEFAULT_HEATMAP_BUCKET_COUNT: u16 = 256;
pub const MIN_HEATMAP_BUCKET_COUNT: u16 = 4;
pub const MAX_HEATMAP_BUCKET_COUNT: u16 = 512;
pub const MAX_EXACT_OTLP_BOUNDARY: u64 = 1 << 53;
static EMPTY_HEATMAPS: OnceLock<Arc<[Heatmap]>> = OnceLock::new();
static DEFAULT_HEATMAP_LAYOUTS: OnceLock<Vec<OnceLock<Arc<HeatmapLayout>>>> = OnceLock::new();

fn empty_heatmaps() -> Arc<[Heatmap]> {
    EMPTY_HEATMAPS.get_or_init(|| Arc::from([])).clone()
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CounterSelector {
    pub type_id: u32,
    pub stat_id: u32,
}

impl CounterSelector {
    pub fn new(type_id: u32, stat_id: u32) -> Self {
        Self { type_id, stat_id }
    }

    pub fn parse_list(serialized: &str) -> Result<HashSet<Self>, String> {
        let serialized = serialized.trim();
        if serialized.is_empty() {
            return Ok(HashSet::new());
        }

        serialized.split(',').map(Self::parse).collect()
    }

    pub fn parse(item: &str) -> Result<Self, String> {
        let item = item.trim();
        let (group, counter) = item.split_once('|').ok_or_else(|| {
            format!(
                "Invalid counter selector '{}'; expected GROUP|COUNTER",
                item
            )
        })?;
        let group = group.trim();
        let counter = counter.trim();
        if group.is_empty() || counter.is_empty() {
            return Err(format!(
                "Invalid counter selector '{}'; expected GROUP|COUNTER",
                item
            ));
        }
        if counter.contains('|') {
            return Err(format!(
                "Invalid counter selector '{}'; expected GROUP|COUNTER",
                item
            ));
        }

        let selector = match group {
            "PORT" => Self::new(
                SaiObjectType::Port.to_u32(),
                format!("SAI_PORT_STAT_{}", counter)
                    .parse::<SaiPortStat>()
                    .map_err(|_| format!("Invalid PORT counter '{}'", counter))?
                    .to_u32(),
            ),
            "QUEUE" => Self::new(
                SaiObjectType::Queue.to_u32(),
                format!("SAI_QUEUE_STAT_{}", counter)
                    .parse::<SaiQueueStat>()
                    .map_err(|_| format!("Invalid QUEUE counter '{}'", counter))?
                    .to_u32(),
            ),
            "BUFFER_POOL" => Self::new(
                SaiObjectType::BufferPool.to_u32(),
                format!("SAI_BUFFER_POOL_STAT_{}", counter)
                    .parse::<SaiBufferPoolStat>()
                    .map_err(|e| format!("Invalid BUFFER_POOL counter: {}", e))?
                    .to_u32(),
            ),
            "INGRESS_PRIORITY_GROUP" => Self::new(
                SaiObjectType::IngressPriorityGroup.to_u32(),
                format!("SAI_INGRESS_PRIORITY_GROUP_STAT_{}", counter)
                    .parse::<SaiIngressPriorityGroupStat>()
                    .map_err(|_| format!("Invalid INGRESS_PRIORITY_GROUP counter '{}'", counter))?
                    .to_u32(),
            ),
            _ => return Err(format!("Invalid counter selector group '{}'", group)),
        };

        let is_marker = match group {
            "PORT" => counter == "START" || selector.stat_id == SaiPortStat::End.to_u32(),
            "QUEUE" => selector.stat_id == SaiQueueStat::CustomRangeBase.to_u32(),
            "BUFFER_POOL" => selector.stat_id == SaiBufferPoolStat::CustomRangeBase.to_u32(),
            "INGRESS_PRIORITY_GROUP" => {
                selector.stat_id == SaiIngressPriorityGroupStat::CustomRangeBase.to_u32()
            }
            _ => false,
        };
        if is_marker {
            return Err(format!("Invalid {} counter '{}'", group, counter));
        }

        Ok(selector)
    }

    pub fn heatmap_value_kind(self) -> HeatmapValueKind {
        match SaiObjectType::from_u32(self.type_id) {
            Some(SaiObjectType::Port) => match SaiPortStat::from_u32(self.stat_id) {
                Some(
                    SaiPortStat::InWatermarkBytes
                    | SaiPortStat::InSharedWatermarkBytes
                    | SaiPortStat::OutWatermarkBytes
                    | SaiPortStat::OutSharedWatermarkBytes,
                ) => HeatmapValueKind::Watermark,
                Some(
                    SaiPortStat::InCurrOccupancyBytes
                    | SaiPortStat::InSharedCurrOccupancyBytes
                    | SaiPortStat::OutCurrOccupancyBytes
                    | SaiPortStat::OutSharedCurrOccupancyBytes,
                ) => HeatmapValueKind::CurrentOccupancy,
                _ => HeatmapValueKind::Delta,
            },
            Some(SaiObjectType::Queue) => match SaiQueueStat::from_u32(self.stat_id) {
                Some(
                    SaiQueueStat::WatermarkBytes
                    | SaiQueueStat::SharedWatermarkBytes
                    | SaiQueueStat::WatermarkLevel
                    | SaiQueueStat::DelayWatermarkNs
                    | SaiQueueStat::WatermarkCells
                    | SaiQueueStat::SharedWatermarkCells,
                ) => HeatmapValueKind::Watermark,
                Some(
                    SaiQueueStat::CurrOccupancyBytes
                    | SaiQueueStat::SharedCurrOccupancyBytes
                    | SaiQueueStat::CurrOccupancyLevel
                    | SaiQueueStat::CurrOccupancyCells
                    | SaiQueueStat::SharedCurrOccupancyCells,
                ) => HeatmapValueKind::CurrentOccupancy,
                _ => HeatmapValueKind::Delta,
            },
            Some(SaiObjectType::BufferPool) => match SaiBufferPoolStat::from_u32(self.stat_id) {
                Some(
                    SaiBufferPoolStat::WatermarkBytes
                    | SaiBufferPoolStat::XoffRoomWatermarkBytes
                    | SaiBufferPoolStat::XoffRoomWatermarkCells
                    | SaiBufferPoolStat::WatermarkCells,
                ) => HeatmapValueKind::Watermark,
                Some(
                    SaiBufferPoolStat::CurrOccupancyBytes
                    | SaiBufferPoolStat::XoffRoomCurrOccupancyBytes
                    | SaiBufferPoolStat::XoffRoomCurrOccupancyCells
                    | SaiBufferPoolStat::CurrOccupancyCells,
                ) => HeatmapValueKind::CurrentOccupancy,
                _ => HeatmapValueKind::Delta,
            },
            Some(SaiObjectType::IngressPriorityGroup) => {
                match SaiIngressPriorityGroupStat::from_u32(self.stat_id) {
                    Some(
                        SaiIngressPriorityGroupStat::WatermarkBytes
                        | SaiIngressPriorityGroupStat::SharedWatermarkBytes
                        | SaiIngressPriorityGroupStat::XoffRoomWatermarkBytes
                        | SaiIngressPriorityGroupStat::WatermarkCells
                        | SaiIngressPriorityGroupStat::SharedWatermarkCells
                        | SaiIngressPriorityGroupStat::XoffRoomWatermarkCells,
                    ) => HeatmapValueKind::Watermark,
                    Some(
                        SaiIngressPriorityGroupStat::CurrOccupancyBytes
                        | SaiIngressPriorityGroupStat::SharedCurrOccupancyBytes
                        | SaiIngressPriorityGroupStat::XoffRoomCurrOccupancyBytes
                        | SaiIngressPriorityGroupStat::CurrOccupancyCells
                        | SaiIngressPriorityGroupStat::SharedCurrOccupancyCells
                        | SaiIngressPriorityGroupStat::XoffRoomCurrOccupancyCells,
                    ) => HeatmapValueKind::CurrentOccupancy,
                    _ => HeatmapValueKind::Delta,
                }
            }
            _ => HeatmapValueKind::Delta,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HeatmapValueKind {
    Delta,
    Watermark,
    CurrentOccupancy,
}

impl HeatmapValueKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Delta => "delta",
            Self::Watermark => "watermark",
            Self::CurrentOccupancy => "current_occupancy",
        }
    }
}

#[derive(Debug)]
pub struct HeatmapLayout {
    explicit_bounds_u64: Arc<[u64]>,
    explicit_bounds: Arc<[f64]>,
}

impl HeatmapLayout {
    fn from_bounds_unchecked(bounds: Vec<u64>) -> Arc<Self> {
        let explicit_bounds = bounds
            .iter()
            .map(|bound| *bound as f64)
            .collect::<Arc<[f64]>>();
        Arc::new(Self {
            explicit_bounds_u64: bounds.into(),
            explicit_bounds,
        })
    }

    pub fn from_explicit_bounds(bounds: Vec<u64>) -> Result<Arc<Self>, String> {
        validate_explicit_bounds(&bounds)?;
        Ok(Self::from_bounds_unchecked(bounds))
    }

    pub fn explicit_bounds_u64(&self) -> &[u64] {
        &self.explicit_bounds_u64
    }

    pub fn explicit_bounds(&self) -> Arc<[f64]> {
        self.explicit_bounds.clone()
    }

    pub fn bucket_count(&self) -> usize {
        self.explicit_bounds_u64.len() + 1
    }
}

pub fn default_heatmap_layout(bucket_count: u16) -> Result<Arc<HeatmapLayout>, String> {
    validate_default_bucket_count(bucket_count)?;
    let layouts = DEFAULT_HEATMAP_LAYOUTS.get_or_init(|| {
        (0..=MAX_HEATMAP_BUCKET_COUNT)
            .map(|_| OnceLock::new())
            .collect()
    });
    Ok(layouts[usize::from(bucket_count)]
        .get_or_init(|| HeatmapLayout::from_bounds_unchecked(generate_default_bounds(bucket_count)))
        .clone())
}

fn generate_default_bounds(bucket_count: u16) -> Vec<u64> {
    let bucket_count = usize::from(bucket_count);
    let exact_max = 15usize.min(bucket_count - 3);
    let logarithmic_count = bucket_count - exact_max - 2;
    let mut bounds = Vec::with_capacity(bucket_count - 1);
    bounds.extend(0..=exact_max as u64);

    if exact_max < 15 {
        bounds.push(MAX_EXACT_OTLP_BOUNDARY);
        return bounds;
    }

    for index in 1..=logarithmic_count {
        let remaining = logarithmic_count - index;
        let lower = bounds.last().copied().unwrap_or_default() + 1;
        let upper = MAX_EXACT_OTLP_BOUNDARY - remaining as u64;
        let candidate = if index == logarithmic_count {
            MAX_EXACT_OTLP_BOUNDARY
        } else {
            let scaled = index * 49;
            let octave = scaled / logarithmic_count;
            let remainder = scaled % logarithmic_count;
            let base = 1u128 << (4 + octave);
            let interpolation = (base * remainder as u128 + logarithmic_count as u128 - 1)
                / logarithmic_count as u128;
            (base + interpolation) as u64
        };
        bounds.push(candidate.clamp(lower, upper));
    }

    bounds
}

fn validate_default_bucket_count(bucket_count: u16) -> Result<(), String> {
    if !(MIN_HEATMAP_BUCKET_COUNT..=MAX_HEATMAP_BUCKET_COUNT).contains(&bucket_count) {
        return Err(format!(
            "heatmap_default_bucket_count must be in range {}..={}",
            MIN_HEATMAP_BUCKET_COUNT, MAX_HEATMAP_BUCKET_COUNT
        ));
    }
    Ok(())
}

fn validate_explicit_bounds(bounds: &[u64]) -> Result<(), String> {
    if !(1..=usize::from(MAX_HEATMAP_BUCKET_COUNT - 1)).contains(&bounds.len()) {
        return Err(format!(
            "explicit_bounds must contain between 1 and {} values",
            MAX_HEATMAP_BUCKET_COUNT - 1
        ));
    }
    if bounds.windows(2).any(|pair| pair[0] >= pair[1]) {
        return Err("explicit_bounds must be strictly increasing".to_string());
    }
    if bounds
        .iter()
        .any(|boundary| *boundary > MAX_EXACT_OTLP_BOUNDARY)
    {
        return Err(format!(
            "explicit_bounds must not exceed {}",
            MAX_EXACT_OTLP_BOUNDARY
        ));
    }
    Ok(())
}

/// Returns `hft-explicit-v1:<kind>:fnv1a64-<16 lowercase hex digits>`.
/// The FNV-1a input is the value kind, the bound count, and each u64 bound in
/// little-endian order, making schema identity stable across processes.
pub fn heatmap_schema(value_kind: HeatmapValueKind, bounds: &[u64]) -> Arc<str> {
    const FNV_OFFSET_BASIS: u64 = 0xcbf29ce484222325;
    const FNV_PRIME: u64 = 0x00000100000001b3;

    let mut hash = FNV_OFFSET_BASIS;
    let mut update = |bytes: &[u8]| {
        for byte in bytes {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(FNV_PRIME);
        }
    };
    update(value_kind.as_str().as_bytes());
    update(&(bounds.len() as u64).to_le_bytes());
    for bound in bounds {
        update(&bound.to_le_bytes());
    }

    Arc::from(format!(
        "hft-explicit-v1:{}:fnv1a64-{hash:016x}",
        value_kind.as_str()
    ))
}

/// CounterSyncd-side subset of HIGH_FREQUENCY_TELEMETRY_AGGREGATOR.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AggregatorConfig {
    /// Reporting interval in microseconds.
    pub reporting_rate: Option<u32>,
    /// Counters corrected when a newly reported raw value is lower than the previous value.
    pub rollover_counters: HashSet<CounterSelector>,
    /// Heatmap aggregation interval in microseconds.
    pub heatmap_interval: Option<u32>,
    /// Counters summarized as heatmaps after optional reporting-rate aggregation.
    pub heatmap_counters: HashSet<CounterSelector>,
    /// Bucket count used when a selected counter has no custom layout.
    pub heatmap_default_bucket_count: u16,
    /// Inclusive upper bounds keyed by selected heatmap counter.
    pub heatmap_explicit_bounds: BTreeMap<CounterSelector, Vec<u64>>,
}

impl Default for AggregatorConfig {
    fn default() -> Self {
        Self {
            reporting_rate: None,
            rollover_counters: HashSet::new(),
            heatmap_interval: None,
            heatmap_counters: HashSet::new(),
            heatmap_default_bucket_count: DEFAULT_HEATMAP_BUCKET_COUNT,
            heatmap_explicit_bounds: BTreeMap::new(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct AggregatorConfigMessage {
    pub key: String,
    pub config: Option<AggregatorConfig>,
    pub is_delete: bool,
    pub reset: bool,
}

impl AggregatorConfigMessage {
    pub fn new(key: String, config: Option<AggregatorConfig>) -> Self {
        Self {
            key,
            config,
            is_delete: false,
            reset: false,
        }
    }

    pub fn replacement(key: String, config: Option<AggregatorConfig>) -> Self {
        Self {
            key,
            config,
            is_delete: false,
            reset: true,
        }
    }

    pub fn delete(key: String) -> Self {
        Self {
            key,
            config: None,
            is_delete: true,
            reset: false,
        }
    }
}

#[derive(Debug, Clone)]
pub struct StatsMessage {
    pub key: Option<Arc<str>>,
    pub stats: SAIStatsMessage,
    pub heatmaps: Arc<[Heatmap]>,
    pub config: Option<AggregatorConfigMessage>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Heatmap {
    pub object_name: Arc<str>,
    pub type_id: u32,
    pub stat_id: u32,
    pub start_time_unix_nano: u64,
    pub time_unix_nano: u64,
    pub count: u64,
    pub sum: f64,
    pub min: u64,
    pub max: u64,
    pub explicit_bounds: Arc<[f64]>,
    pub bucket_counts: Vec<u64>,
    pub value_kind: HeatmapValueKind,
    pub schema: Arc<str>,
}

/// Common message format for every actor downstream of the aggregator.
pub type AggregatedStatsMessage = StatsMessage;

impl StatsMessage {
    pub fn new(key: Option<Arc<str>>, stats: SAIStatsMessage) -> Self {
        Self {
            key,
            stats,
            heatmaps: empty_heatmaps(),
            config: None,
        }
    }

    pub fn with_heatmaps(
        key: Option<Arc<str>>,
        stats: SAIStatsMessage,
        heatmaps: Vec<Heatmap>,
    ) -> Self {
        let heatmaps = if heatmaps.is_empty() {
            empty_heatmaps()
        } else {
            heatmaps.into()
        };
        Self {
            key,
            stats,
            heatmaps,
            config: None,
        }
    }

    pub fn config(message: AggregatorConfigMessage) -> Self {
        Self {
            key: None,
            stats: Arc::new(super::saistats::SAIStats::new(0, Vec::new())),
            heatmaps: empty_heatmaps(),
            config: Some(message),
        }
    }
}

impl From<SAIStatsMessage> for StatsMessage {
    fn from(stats: SAIStatsMessage) -> Self {
        Self::new(None, stats)
    }
}

impl AggregatorConfig {
    pub fn validate(&self) -> Result<(), String> {
        if self.reporting_rate == Some(0) {
            return Err("reporting_rate must be greater than zero".to_string());
        }
        if self.heatmap_interval == Some(0) {
            return Err("heatmap_interval must be greater than zero".to_string());
        }

        validate_default_bucket_count(self.heatmap_default_bucket_count)?;
        if self.heatmap_interval.is_none()
            && self.heatmap_default_bucket_count != DEFAULT_HEATMAP_BUCKET_COUNT
        {
            return Err(
                "heatmap_default_bucket_count requires heatmap_interval and heatmap_counters"
                    .to_string(),
            );
        }

        let heatmap_fields = [
            ("heatmap_interval", self.heatmap_interval.is_some()),
            ("heatmap_counters", !self.heatmap_counters.is_empty()),
        ];
        if heatmap_fields.iter().any(|(_, configured)| *configured)
            && !heatmap_fields.iter().all(|(_, configured)| *configured)
        {
            let missing = heatmap_fields
                .iter()
                .filter_map(|(name, configured)| (!configured).then_some(*name))
                .collect::<Vec<_>>()
                .join(", ");
            return Err(format!(
                "incomplete heatmap configuration; missing {}",
                missing
            ));
        }

        for (selector, bounds) in &self.heatmap_explicit_bounds {
            if !self.heatmap_counters.contains(selector) {
                return Err(format!(
                    "explicit_bounds selector type {} stat {} is not selected by heatmap_counters",
                    selector.type_id, selector.stat_id
                ));
            }
            validate_explicit_bounds(bounds)?;
        }

        for selector in &self.rollover_counters {
            let kind = selector.heatmap_value_kind();
            if kind != HeatmapValueKind::Delta {
                return Err(format!(
                    "rollover_counters cannot contain {} selector type {} stat {}",
                    kind.as_str(),
                    selector.type_id,
                    selector.stat_id
                ));
            }
        }

        Ok(())
    }

    pub fn layout_for(&self, selector: CounterSelector) -> Result<Arc<HeatmapLayout>, String> {
        match self.heatmap_explicit_bounds.get(&selector) {
            Some(bounds) => HeatmapLayout::from_explicit_bounds(bounds.clone()),
            None => default_heatmap_layout(self.heatmap_default_bucket_count),
        }
    }

    pub fn parse_explicit_bounds(serialized: &str) -> Result<Vec<u64>, String> {
        let serialized = serialized.trim();
        if serialized.is_empty() {
            return Ok(Vec::new());
        }

        serialized
            .split(',')
            .map(|item| {
                let item = item.trim();
                if item.is_empty() {
                    return Err("explicit_bounds must not contain empty entries".to_string());
                }
                item.parse::<u64>()
                    .map_err(|_| format!("Invalid explicit bound '{}'", item))
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const GOLDEN_DEFAULT_4: &[u64] = &[0, 1, MAX_EXACT_OTLP_BOUNDARY];
    const GOLDEN_DEFAULT_19: &[u64] = &[
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        402_653_184,
        MAX_EXACT_OTLP_BOUNDARY,
    ];
    #[rustfmt::skip]
    const GOLDEN_DEFAULT_256: &[u64] = &[
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 20, 23, 26, 30, 33, 40, 46, 53,
        60, 68, 81, 94, 107, 120, 138, 164, 191, 217, 243, 282, 335, 387, 440, 492, 577,
        682, 787, 892, 997, 1_179, 1_389, 1_599, 1_809, 2_019, 2_408, 2_828, 3_248, 3_668,
        4_088, 4_919, 5_759, 6_599, 7_438, 8_364, 10_043, 11_723, 13_402, 15_082, 17_139,
        20_498, 23_857, 27_216, 30_575, 35_099, 41_817, 48_536, 55_254, 61_972, 71_843,
        85_280, 98_716, 112_152, 125_588, 146_977, 173_849, 200_722, 227_594, 254_467,
        300_534, 354_279, 408_024, 461_769, 515_514, 614_229, 721_719, 829_209, 936_699,
        1_044_189, 1_254_782, 1_469_762, 1_684_742, 1_899_722, 2_132_251, 2_562_211,
        2_992_171, 3_422_131, 3_852_091, 4_369_798, 5_229_718, 6_089_639, 6_949_559,
        7_809_479, 8_950_189, 10_670_029, 12_389_869, 14_109_709, 15_829_549, 18_321_563,
        21_761_243, 25_200_923, 28_640_604, 32_080_284, 37_485_496, 44_364_856, 51_244_217,
        58_123_577, 65_002_938, 76_655_732, 90_414_453, 104_173_174, 117_931_895,
        131_690_616, 156_680_947, 184_198_389, 211_715_831, 239_233_273, 266_750_715,
        320_100_858, 375_135_742, 430_170_627, 485_205_511, 543_609_878, 653_679_646,
        763_749_415, 873_819_184, 983_888_952, 1_114_175_617, 1_334_315_154, 1_554_454_691,
        1_774_594_228, 1_994_733_766, 2_282_262_957, 2_722_542_031, 3_162_821_106,
        3_603_100_180, 4_043_379_254, 4_672_349_360, 5_552_907_509, 6_433_465_657,
        7_314_023_806, 8_194_581_954, 9_560_345_613, 11_321_461_910, 13_082_578_208,
        14_843_694_505, 16_604_810_802, 19_551_985_013, 23_074_217_607, 26_596_450_202,
        30_118_682_796, 33_640_915_390, 39_966_557_600, 47_011_022_789, 54_055_487_977,
        61_099_953_165, 68_144_418_354, 81_658_290_348, 95_747_220_725, 109_836_151_102,
        123_925_081_478, 138_589_070_238, 166_766_930_992, 194_944_791_745,
        223_122_652_499, 251_300_513_253, 284_078_841_068, 340_434_562_575,
        396_790_284_083, 453_146_005_590, 509_501_727_098, 581_959_083_321,
        694_670_526_336, 807_381_969_351, 920_093_412_365, 1_032_804_855_380,
        1_191_520_969_013, 1_416_943_855_042, 1_642_366_741_072, 1_867_789_627_101,
        2_093_212_513_131, 2_438_247_542_767, 2_889_093_314_826, 3_339_939_086_885,
        3_790_784_858_944, 4_241_630_631_002, 4_986_906_295_018, 5_888_597_839_136,
        6_790_289_383_253, 7_691_980_927_371, 8_593_672_471_488, 10_194_635_009_003,
        11_998_018_097_238, 13_801_401_185_473, 15_604_784_273_708, 17_408_167_361_944,
        20_830_914_855_941, 24_437_681_032_411, 28_044_447_208_881, 31_651_213_385_351,
        35_331_587_034_811, 42_545_119_387_751, 49_758_651_740_692, 56_972_184_093_632,
        64_185_716_446_573, 72_429_753_421_362, 86_856_818_127_243, 101_283_882_833_124,
        115_710_947_539_004, 130_138_012_244_885, 148_392_665_546_204,
        177_246_794_957_966, 206_100_924_369_728, 234_955_053_781_490,
        263_809_183_193_251, 303_851_648_499_370, 361_559_907_322_893,
        419_268_166_146_417, 476_976_424_969_941, 534_684_683_793_464,
        621_835_931_812_663, 737_252_449_459_710, 852_668_967_106_758,
        968_085_484_753_805, 1_083_502_002_400_852, 1_271_937_133_253_174,
        1_502_770_168_547_269, 1_733_603_203_841_363, 1_964_436_239_135_457,
        2_195_269_274_429_552, 2_600_404_805_762_044, 3_062_070_876_350_233,
        3_523_736_946_938_422, 3_985_403_017_526_611, 4_447_069_088_114_800,
        5_313_870_690_035_481, 6_237_202_831_211_859, 7_160_534_972_388_237,
        8_083_867_113_564_615, 9_007_199_254_740_992,
    ];
    #[rustfmt::skip]
    const GOLDEN_DEFAULT_512: &[u64] = &[
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 18, 20, 21, 23, 24, 26, 28, 29,
        31, 32, 35, 39, 42, 45, 48, 51, 54, 58, 61, 64, 70, 76, 82, 89, 95, 101, 108, 114,
        120, 127, 137, 150, 163, 175, 188, 201, 213, 226, 239, 251, 271, 297, 322, 348, 373,
        398, 424, 449, 474, 500, 537, 588, 639, 689, 740, 791, 841, 892, 943, 993, 1_064,
        1_165, 1_267, 1_368, 1_469, 1_571, 1_672, 1_773, 1_875, 1_976, 2_106, 2_309,
        2_512, 2_715, 2_917, 3_120, 3_323, 3_526, 3_728, 3_931, 4_171, 4_576, 4_982,
        5_387, 5_793, 6_198, 6_604, 7_009, 7_415, 7_820, 8_259, 9_070, 9_881, 10_691,
        11_502, 12_313, 13_124, 13_935, 14_746, 15_557, 16_368, 17_973, 19_595, 21_217,
        22_839, 24_461, 26_083, 27_704, 29_326, 30_948, 32_570, 35_615, 38_859, 42_102,
        45_346, 48_590, 51_834, 55_077, 58_321, 61_565, 64_808, 70_568, 77_055, 83_542,
        90_030, 96_517, 103_005, 109_492, 115_979, 122_467, 128_954, 139_811, 152_785,
        165_760, 178_735, 191_710, 204_685, 217_659, 230_634, 243_609, 256_584, 276_973,
        302_922, 328_872, 354_822, 380_771, 406_721, 432_670, 458_620, 484_570, 510_519,
        548_649, 600_549, 652_448, 704_347, 756_246, 808_145, 860_045, 911_944, 963_843,
        1_015_742, 1_086_707, 1_190_505, 1_294_303, 1_398_102, 1_501_900, 1_605_699,
        1_709_497, 1_813_296, 1_917_094, 2_020_892, 2_152_229, 2_359_826, 2_567_423,
        2_775_020, 2_982_617, 3_190_214, 3_397_810, 3_605_407, 3_813_004, 4_020_601,
        4_262_091, 4_677_285, 5_092_479, 5_507_672, 5_922_866, 6_338_060, 6_753_254,
        7_168_447, 7_583_641, 7_998_835, 8_439_449, 9_269_836, 10_100_223, 10_930_611,
        11_760_998, 12_591_386, 13_421_773, 14_252_161, 15_082_548, 15_912_936,
        16_743_323, 18_370_205, 20_030_980, 21_691_755, 23_352_529, 25_013_304,
        26_674_079, 28_334_854, 29_995_629, 31_656_404, 33_317_179, 36_401_475,
        39_723_025, 43_044_575, 46_366_125, 49_687_675, 53_009_224, 56_330_774,
        59_652_324, 62_973_874, 66_295_424, 72_125_083, 78_768_182, 85_411_282,
        92_054_382, 98_697_481, 105_340_581, 111_983_681, 118_626_780, 125_269_880,
        131_912_980, 142_894_430, 156_180_629, 169_466_829, 182_753_028, 196_039_227,
        209_325_427, 222_611_626, 235_897_825, 249_184_025, 262_470_224, 283_077_390,
        309_649_789, 336_222_188, 362_794_586, 389_366_985, 415_939_384, 442_511_783,
        469_084_181, 495_656_580, 522_228_979, 560_731_842, 613_876_639, 667_021_437,
        720_166_234, 773_311_031, 826_455_829, 879_600_626, 932_745_423, 985_890_221,
        1_039_035_018, 1_110_617_806, 1_216_907_401, 1_323_196_996, 1_429_486_590,
        1_535_776_185, 1_642_065_780, 1_748_355_375, 1_854_644_969, 1_960_934_564,
        2_067_224_159, 2_199_543_858, 2_412_123_048, 2_624_702_237, 2_837_281_426,
        3_049_860_616, 3_262_439_805, 3_475_018_995, 3_687_598_184, 3_900_177_373,
        4_112_756_563, 4_355_704_208, 4_780_862_587, 5_206_020_965, 5_631_179_344,
        6_056_337_723, 6_481_496_102, 6_906_654_481, 7_331_812_859, 7_756_971_238,
        8_182_129_617, 8_624_641_399, 9_474_958_157, 10_325_274_914, 11_175_591_672,
        12_025_908_429, 12_876_225_187, 13_726_541_944, 14_576_858_702,
        15_427_175_460, 16_277_492_217, 17_127_808_975, 18_776_382_280,
        20_477_015_796, 22_177_649_311, 23_878_282_826, 25_578_916_341,
        27_279_549_856, 28_980_183_371, 30_680_816_887, 32_381_450_402,
        34_082_083_917, 37_205_696_496, 40_606_963_526, 44_008_230_557,
        47_409_497_587, 50_810_764_617, 54_212_031_648, 57_613_298_678,
        61_014_565_709, 64_415_832_739, 67_817_099_769, 73_717_256_863,
        80_519_790_923, 87_322_324_984, 94_124_859_045, 100_927_393_106,
        107_729_927_166, 114_532_461_227, 121_334_995_288, 128_137_529_349,
        134_940_063_409, 146_046_241_468, 159_651_309_589, 173_256_377_711,
        186_861_445_832, 200_466_513_954, 214_071_582_075, 227_676_650_197,
        241_281_718_318, 254_886_786_439, 268_491_854_561, 289_315_938_420,
        316_526_074_663, 343_736_210_906, 370_946_347_149, 398_156_483_392,
        425_366_619_635, 452_576_755_878, 479_786_892_121, 506_997_028_364,
        534_207_164_607, 573_078_787_811, 627_499_060_297, 681_919_332_783,
        736_339_605_269, 790_759_877_755, 845_180_150_240, 899_600_422_726,
        954_020_695_212, 1_008_440_967_698, 1_062_861_240_184, 1_135_051_397_563,
        1_243_891_942_535, 1_352_732_487_507, 1_461_573_032_478,
        1_570_413_577_450, 1_679_254_122_422, 1_788_094_667_394,
        1_896_935_212_366, 2_005_775_757_337, 2_114_616_302_309,
        2_247_890_439_009, 2_465_571_528_953, 2_683_252_618_896,
        2_900_933_708_840, 3_118_614_798_783, 3_336_295_888_727,
        3_553_976_978_670, 3_771_658_068_614, 3_989_339_158_557,
        4_207_020_248_501, 4_451_356_165_785, 4_886_718_345_672,
        5_322_080_525_559, 5_757_442_705_446, 6_192_804_885_333,
        6_628_167_065_220, 7_063_529_245_107, 7_498_891_424_994,
        7_934_253_604_881, 8_369_615_784_768, 8_813_862_907_102,
        9_684_587_266_876, 10_555_311_626_650, 11_426_035_986_424,
        12_296_760_346_198, 13_167_484_705_972, 14_038_209_065_747,
        14_908_933_425_521, 15_779_657_785_295, 16_650_382_145_069,
        17_521_106_504_843, 19_191_475_684_818, 20_932_924_404_366,
        22_674_373_123_914, 24_415_821_843_463, 26_157_270_563_011,
        27_898_719_282_559, 29_640_168_002_107, 31_381_616_721_656,
        33_123_065_441_204, 34_864_514_160_752, 38_027_553_671_768,
        41_510_451_110_865, 44_993_348_549_961, 48_476_245_989_058,
        51_959_143_428_154, 55_442_040_867_251, 58_924_938_306_347,
        62_407_835_745_444, 65_890_733_184_540, 69_373_630_623_637,
        75_344_311_947_802, 82_310_106_825_995, 89_275_901_704_188,
        96_241_696_582_381, 103_207_491_460_574, 110_173_286_338_767,
        117_139_081_216_960, 124_104_876_095_153, 131_070_670_973_346,
        138_036_465_851_539, 149_267_033_104_136, 163_198_622_860_522,
        177_130_212_616_908, 191_061_802_373_294, 204_993_392_129_680,
        218_924_981_886_066, 232_856_571_642_452, 246_788_161_398_838,
        260_719_751_155_224, 274_651_340_911_610, 295_690_884_625_336,
        323_554_064_138_108, 351_417_243_650_880, 379_280_423_163_652,
        407_143_602_676_424, 435_006_782_189_196, 462_869_961_701_968,
        490_733_141_214_740, 518_596_320_727_512, 546_459_500_240_284,
        585_695_406_084_800, 641_421_765_110_344, 697_148_124_135_888,
        752_874_483_161_432, 808_600_842_186_976, 864_327_201_212_520,
        920_053_560_238_064, 975_779_919_263_608, 1_031_506_278_289_152,
        1_087_232_637_314_696, 1_160_018_085_837_856, 1_271_470_803_888_944,
        1_382_923_521_940_032, 1_494_376_239_991_120, 1_605_828_958_042_208,
        1_717_281_676_093_296, 1_828_734_394_144_384, 1_940_187_112_195_472,
        2_051_639_830_246_560, 2_163_092_548_297_648, 2_297_290_719_012_223,
        2_520_196_155_114_399, 2_743_101_591_216_575, 2_966_007_027_318_751,
        3_188_912_463_420_927, 3_411_817_899_523_104, 3_634_723_335_625_280,
        3_857_628_771_727_456, 4_080_534_207_829_632, 4_303_439_643_931_808,
        4_549_090_532_697_471, 4_994_901_404_901_823, 5_440_712_277_106_175,
        5_886_523_149_310_528, 6_332_334_021_514_880, 6_778_144_893_719_232,
        7_223_955_765_923_584, 7_669_766_638_127_936, 8_115_577_510_332_288,
        8_561_388_382_536_640, 9_007_199_254_740_992,
    ];

    #[test]
    fn parses_heatmap_counter_selectors() {
        let selectors = CounterSelector::parse_list(
            "PORT|IF_IN_UCAST_PKTS,QUEUE|WATERMARK_BYTES,BUFFER_POOL|CURR_OCCUPANCY_BYTES,INGRESS_PRIORITY_GROUP|PACKETS",
        )
        .unwrap();

        assert!(selectors.contains(&CounterSelector::new(1, 1)));
        assert!(selectors.contains(&CounterSelector::new(21, 25)));
        assert!(selectors.contains(&CounterSelector::new(24, 0)));
        assert!(selectors.contains(&CounterSelector::new(26, 0)));
    }

    #[test]
    fn rejects_invalid_heatmap_counter_selectors() {
        assert!(CounterSelector::parse_list("QUEUE|IF_IN_UCAST_PKTS").is_err());
        assert!(CounterSelector::parse_list("PORT").is_err());
        assert!(CounterSelector::parse_list("UNKNOWN|PACKETS").is_err());
        assert!(CounterSelector::parse_list("PORT|END").is_err());
        assert!(CounterSelector::parse_list("PORT|START").is_err());
        assert!(CounterSelector::parse_list("QUEUE|CUSTOM_RANGE_BASE").is_err());
        assert!(CounterSelector::parse_list("BUFFER_POOL|CUSTOM_RANGE_BASE").is_err());
        assert!(CounterSelector::parse_list("INGRESS_PRIORITY_GROUP|CUSTOM_RANGE_BASE").is_err());
        assert!(CounterSelector::parse_list("PORT|IF_IN_OCTETS,,QUEUE|PACKETS").is_err());
    }

    #[test]
    fn default_layouts_have_the_requested_shape_and_are_cached() {
        for bucket_count in [4, 19, 256, 512] {
            let first = default_heatmap_layout(bucket_count).unwrap();
            let second = default_heatmap_layout(bucket_count).unwrap();
            let bounds = first.explicit_bounds_u64();

            assert!(Arc::ptr_eq(&first, &second));
            assert_eq!(first.bucket_count(), usize::from(bucket_count));
            assert_eq!(bounds.len(), usize::from(bucket_count) - 1);
            assert_eq!(bounds.last(), Some(&MAX_EXACT_OTLP_BOUNDARY));
            assert!(bounds.windows(2).all(|pair| pair[0] < pair[1]));
            assert!(bounds.iter().all(|bound| *bound <= MAX_EXACT_OTLP_BOUNDARY));
            let exact_max = 15usize.min(usize::from(bucket_count) - 3);
            assert_eq!(
                &bounds[..=exact_max],
                &(0..=exact_max as u64).collect::<Vec<_>>()
            );
            assert_eq!(
                first.explicit_bounds().len(),
                first.explicit_bounds_u64().len()
            );
        }

        for (bucket_count, golden) in [
            (4, GOLDEN_DEFAULT_4),
            (19, GOLDEN_DEFAULT_19),
            (256, GOLDEN_DEFAULT_256),
            (512, GOLDEN_DEFAULT_512),
        ] {
            assert_eq!(
                default_heatmap_layout(bucket_count)
                    .unwrap()
                    .explicit_bounds_u64(),
                golden
            );
        }
        assert!(default_heatmap_layout(3).is_err());
        assert!(default_heatmap_layout(513).is_err());
    }

    #[test]
    fn default_layout_schema_hashes_are_golden() {
        for (bucket_count, expected) in [
            (4, "hft-explicit-v1:delta:fnv1a64-5f918b1c2ff1ff23"),
            (19, "hft-explicit-v1:delta:fnv1a64-5dd027bc6d741cbb"),
            (256, "hft-explicit-v1:delta:fnv1a64-091b3b9b0211778d"),
            (512, "hft-explicit-v1:delta:fnv1a64-0b2ec1edb3aa702a"),
        ] {
            let layout = default_heatmap_layout(bucket_count).unwrap();
            assert_eq!(
                heatmap_schema(HeatmapValueKind::Delta, layout.explicit_bounds_u64()).as_ref(),
                expected
            );
        }
    }

    #[test]
    fn parses_and_validates_explicit_bounds_and_fallbacks() {
        assert_eq!(
            AggregatorConfig::parse_explicit_bounds("0, 1024,4096").unwrap(),
            vec![0, 1024, 4096]
        );
        assert!(AggregatorConfig::parse_explicit_bounds("0,invalid").is_err());
        assert!(AggregatorConfig::parse_explicit_bounds("0,,1024").is_err());

        let custom = CounterSelector::new(1, 2);
        let fallback = CounterSelector::new(1, 3);
        let valid = AggregatorConfig {
            heatmap_interval: Some(1_000),
            heatmap_counters: HashSet::from([custom, fallback]),
            heatmap_default_bucket_count: 4,
            heatmap_explicit_bounds: BTreeMap::from([(custom, vec![0, 1024, 4096])]),
            ..Default::default()
        };
        assert!(valid.validate().is_ok());
        assert_eq!(
            valid.layout_for(custom).unwrap().explicit_bounds_u64(),
            &[0, 1024, 4096]
        );
        let fallback_layout = valid.layout_for(fallback).unwrap();
        assert!(Arc::ptr_eq(
            &fallback_layout,
            &default_heatmap_layout(4).unwrap()
        ));

        let mut missing_interval = valid.clone();
        missing_interval.heatmap_interval = None;
        assert!(missing_interval.validate().is_err());

        let mut zero_interval = valid.clone();
        zero_interval.heatmap_interval = Some(0);
        assert!(zero_interval.validate().is_err());

        let zero_reporting = AggregatorConfig {
            reporting_rate: Some(0),
            ..Default::default()
        };
        assert!(zero_reporting.validate().is_err());

        let mut unordered = valid.clone();
        unordered
            .heatmap_explicit_bounds
            .insert(custom, vec![0, 4096, 1024]);
        assert!(unordered.validate().is_err());

        let mut too_large = valid.clone();
        too_large
            .heatmap_explicit_bounds
            .insert(custom, vec![MAX_EXACT_OTLP_BOUNDARY + 1]);
        assert!(too_large.validate().is_err());

        let mut empty = valid.clone();
        empty.heatmap_explicit_bounds.insert(custom, Vec::new());
        assert!(empty.validate().is_err());

        let mut too_few_default_buckets = valid.clone();
        too_few_default_buckets.heatmap_default_bucket_count = 3;
        assert!(too_few_default_buckets.validate().is_err());

        let mut too_many_default_buckets = valid.clone();
        too_many_default_buckets.heatmap_default_bucket_count = 513;
        assert!(too_many_default_buckets.validate().is_err());

        let bucket_count_without_heatmap = AggregatorConfig {
            heatmap_default_bucket_count: 64,
            ..Default::default()
        };
        assert!(bucket_count_without_heatmap.validate().is_err());

        let mut too_many = valid.clone();
        too_many
            .heatmap_explicit_bounds
            .insert(custom, (0..512).collect());
        assert!(too_many.validate().is_err());
        assert!(HeatmapLayout::from_explicit_bounds((0..511).collect()).is_ok());

        let mut orphan = valid;
        orphan
            .heatmap_explicit_bounds
            .insert(CounterSelector::new(1, 4), vec![1]);
        assert!(orphan.validate().is_err());
    }

    #[test]
    fn config_default_and_equality_are_deterministic() {
        let default = AggregatorConfig::default();
        assert_eq!(
            default.heatmap_default_bucket_count,
            DEFAULT_HEATMAP_BUCKET_COUNT
        );
        assert!(default.validate().is_ok());

        let selector_a = CounterSelector::new(1, 1);
        let selector_b = CounterSelector::new(21, 1);
        let first = AggregatorConfig {
            heatmap_interval: Some(10),
            heatmap_counters: HashSet::from([selector_a, selector_b]),
            heatmap_explicit_bounds: BTreeMap::from([
                (selector_a, vec![1, 2]),
                (selector_b, vec![3, 4]),
            ]),
            ..Default::default()
        };
        let second = AggregatorConfig {
            heatmap_interval: Some(10),
            heatmap_counters: HashSet::from([selector_b, selector_a]),
            heatmap_explicit_bounds: BTreeMap::from([
                (selector_b, vec![3, 4]),
                (selector_a, vec![1, 2]),
            ]),
            ..Default::default()
        };
        assert_eq!(first, second);
    }

    #[test]
    fn classifies_all_raw_occupancy_value_variants_by_numeric_id() {
        let watermark = [
            CounterSelector::new(1, SaiPortStat::InWatermarkBytes.to_u32()),
            CounterSelector::new(1, SaiPortStat::InSharedWatermarkBytes.to_u32()),
            CounterSelector::new(1, SaiPortStat::OutWatermarkBytes.to_u32()),
            CounterSelector::new(1, SaiPortStat::OutSharedWatermarkBytes.to_u32()),
            CounterSelector::new(21, SaiQueueStat::WatermarkBytes.to_u32()),
            CounterSelector::new(21, SaiQueueStat::SharedWatermarkBytes.to_u32()),
            CounterSelector::new(21, SaiQueueStat::WatermarkLevel.to_u32()),
            CounterSelector::new(21, SaiQueueStat::DelayWatermarkNs.to_u32()),
            CounterSelector::new(21, SaiQueueStat::WatermarkCells.to_u32()),
            CounterSelector::new(21, SaiQueueStat::SharedWatermarkCells.to_u32()),
            CounterSelector::new(24, SaiBufferPoolStat::WatermarkBytes.to_u32()),
            CounterSelector::new(24, SaiBufferPoolStat::XoffRoomWatermarkBytes.to_u32()),
            CounterSelector::new(24, SaiBufferPoolStat::XoffRoomWatermarkCells.to_u32()),
            CounterSelector::new(24, SaiBufferPoolStat::WatermarkCells.to_u32()),
            CounterSelector::new(26, SaiIngressPriorityGroupStat::WatermarkBytes.to_u32()),
            CounterSelector::new(
                26,
                SaiIngressPriorityGroupStat::SharedWatermarkBytes.to_u32(),
            ),
            CounterSelector::new(
                26,
                SaiIngressPriorityGroupStat::XoffRoomWatermarkBytes.to_u32(),
            ),
            CounterSelector::new(26, SaiIngressPriorityGroupStat::WatermarkCells.to_u32()),
            CounterSelector::new(
                26,
                SaiIngressPriorityGroupStat::SharedWatermarkCells.to_u32(),
            ),
            CounterSelector::new(
                26,
                SaiIngressPriorityGroupStat::XoffRoomWatermarkCells.to_u32(),
            ),
        ];
        let current = [
            CounterSelector::new(1, SaiPortStat::InCurrOccupancyBytes.to_u32()),
            CounterSelector::new(1, SaiPortStat::InSharedCurrOccupancyBytes.to_u32()),
            CounterSelector::new(1, SaiPortStat::OutCurrOccupancyBytes.to_u32()),
            CounterSelector::new(1, SaiPortStat::OutSharedCurrOccupancyBytes.to_u32()),
            CounterSelector::new(21, SaiQueueStat::CurrOccupancyBytes.to_u32()),
            CounterSelector::new(21, SaiQueueStat::SharedCurrOccupancyBytes.to_u32()),
            CounterSelector::new(21, SaiQueueStat::CurrOccupancyLevel.to_u32()),
            CounterSelector::new(21, SaiQueueStat::CurrOccupancyCells.to_u32()),
            CounterSelector::new(21, SaiQueueStat::SharedCurrOccupancyCells.to_u32()),
            CounterSelector::new(24, SaiBufferPoolStat::CurrOccupancyBytes.to_u32()),
            CounterSelector::new(24, SaiBufferPoolStat::XoffRoomCurrOccupancyBytes.to_u32()),
            CounterSelector::new(24, SaiBufferPoolStat::XoffRoomCurrOccupancyCells.to_u32()),
            CounterSelector::new(24, SaiBufferPoolStat::CurrOccupancyCells.to_u32()),
            CounterSelector::new(26, SaiIngressPriorityGroupStat::CurrOccupancyBytes.to_u32()),
            CounterSelector::new(
                26,
                SaiIngressPriorityGroupStat::SharedCurrOccupancyBytes.to_u32(),
            ),
            CounterSelector::new(
                26,
                SaiIngressPriorityGroupStat::XoffRoomCurrOccupancyBytes.to_u32(),
            ),
            CounterSelector::new(26, SaiIngressPriorityGroupStat::CurrOccupancyCells.to_u32()),
            CounterSelector::new(
                26,
                SaiIngressPriorityGroupStat::SharedCurrOccupancyCells.to_u32(),
            ),
            CounterSelector::new(
                26,
                SaiIngressPriorityGroupStat::XoffRoomCurrOccupancyCells.to_u32(),
            ),
        ];

        assert!(watermark
            .iter()
            .all(|selector| selector.heatmap_value_kind() == HeatmapValueKind::Watermark));
        assert!(current.iter().all(|selector| {
            selector.heatmap_value_kind() == HeatmapValueKind::CurrentOccupancy
        }));
        for selector in [
            CounterSelector::new(1, SaiPortStat::IfInOctets.to_u32()),
            CounterSelector::new(21, SaiQueueStat::Packets.to_u32()),
            CounterSelector::new(24, SaiBufferPoolStat::DroppedPackets.to_u32()),
            CounterSelector::new(26, SaiIngressPriorityGroupStat::Packets.to_u32()),
            CounterSelector::new(u32::MAX, u32::MAX),
        ] {
            assert_eq!(selector.heatmap_value_kind(), HeatmapValueKind::Delta);
        }
    }

    #[test]
    fn rejects_raw_value_counters_from_rollover() {
        // Runtime is deliberately stricter than schemas that only reject
        // watermark counters: current occupancy is also a raw gauge.
        for selector in [
            CounterSelector::new(21, SaiQueueStat::WatermarkBytes.to_u32()),
            CounterSelector::new(21, SaiQueueStat::CurrOccupancyBytes.to_u32()),
        ] {
            let config = AggregatorConfig {
                rollover_counters: HashSet::from([selector]),
                ..Default::default()
            };
            assert!(config.validate().is_err());
        }
    }

    #[test]
    fn schema_is_stable_and_separates_kind_and_layout() {
        let delta = heatmap_schema(HeatmapValueKind::Delta, &[1, 2, 8]);
        assert_eq!(
            delta.as_ref(),
            "hft-explicit-v1:delta:fnv1a64-90a86b480b3a8ca9"
        );
        assert_eq!(delta, heatmap_schema(HeatmapValueKind::Delta, &[1, 2, 8]));
        assert!(delta.starts_with("hft-explicit-v1:delta:fnv1a64-"));
        assert_ne!(
            delta,
            heatmap_schema(HeatmapValueKind::Watermark, &[1, 2, 8])
        );
        assert_ne!(delta, heatmap_schema(HeatmapValueKind::Delta, &[1, 2, 9]));
    }

    #[test]
    fn reuses_empty_heatmap_storage() {
        let stats = Arc::new(super::super::saistats::SAIStats::new(1, Vec::new()));
        let first = StatsMessage::new(None, stats.clone());
        let second = StatsMessage::with_heatmaps(None, stats, Vec::new());

        assert!(Arc::ptr_eq(&first.heatmaps, &second.heatmaps));
    }

    #[test]
    fn reports_missing_heatmap_fields() {
        let selector = CounterSelector::new(1, 2);
        let cases = [
            (
                AggregatorConfig {
                    heatmap_counters: HashSet::from([selector]),
                    ..Default::default()
                },
                "heatmap_interval",
            ),
            (
                AggregatorConfig {
                    heatmap_interval: Some(10),
                    ..Default::default()
                },
                "heatmap_counters",
            ),
        ];

        for (config, missing) in cases {
            assert!(config.validate().unwrap_err().contains(missing));
        }
    }
}
