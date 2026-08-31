use std::{
    collections::{BTreeMap, HashSet},
    sync::{Arc, Mutex, OnceLock},
};

use super::saistats::SAIStatsMessage;
use crate::sai::{
    SaiBufferPoolStat, SaiIngressPriorityGroupStat, SaiObjectType, SaiPortStat, SaiQueueStat,
};

pub const MAX_EXACT_OTLP_BOUNDARY: u64 = 1 << 53;
pub const MAX_HEATMAP_EXPLICIT_BOUNDS: usize = 511;
pub const DEFAULT_ROLLOVER_BIT_WIDTH: u8 = 32;
pub const MIN_ROLLOVER_BIT_WIDTH: u8 = 1;
pub const MAX_ROLLOVER_BIT_WIDTH: u8 = 63;

static EMPTY_HEATMAPS: OnceLock<Arc<[Heatmap]>> = OnceLock::new();
static DELTA_BYTES_LAYOUTS: OnceLock<Mutex<BTreeMap<u32, Arc<HeatmapLayout>>>> = OnceLock::new();
static ABSOLUTE_BYTES_LAYOUT: OnceLock<Arc<HeatmapLayout>> = OnceLock::new();
static ABSOLUTE_CELLS_LAYOUT: OnceLock<Arc<HeatmapLayout>> = OnceLock::new();
static DELTA_COUNT_LAYOUT: OnceLock<Arc<HeatmapLayout>> = OnceLock::new();
static NATIVE_LAYOUT: OnceLock<Arc<HeatmapLayout>> = OnceLock::new();

// Build raw byte-delta bounds from utilization bands for common link speeds.
// This is config-time layout generation, not per-observation rate conversion.
const DELTA_BYTES_LINK_SPEEDS_GBPS: [u64; 5] = [100, 200, 400, 800, 1_600];
const DELTA_BYTES_UTILIZATION_BASIS_POINTS: [u64; 9] = [
    5_000, 7_500, 9_000, 9_500, 9_800, 9_900, 9_950, 9_980, 10_000,
];
const ABSOLUTE_BYTES_BOUNDS: [u64; 9] = [
    0,
    512,
    1_024,
    512 * 1_024,
    1_024 * 1_024,
    5 * 1_024 * 1_024,
    10 * 1_024 * 1_024,
    50 * 1_024 * 1_024,
    100 * 1_024 * 1_024,
];
const DELTA_COUNT_BOUNDS: [u64; 28] = [
    0,
    1,
    2,
    5,
    10,
    20,
    50,
    100,
    200,
    500,
    1_000,
    2_000,
    5_000,
    10_000,
    20_000,
    50_000,
    100_000,
    200_000,
    500_000,
    1_000_000,
    2_000_000,
    5_000_000,
    10_000_000,
    20_000_000,
    50_000_000,
    100_000_000,
    200_000_000,
    500_000_000,
];

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
        if group.is_empty() || counter.is_empty() || counter.contains('|') {
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

    /// Classifies a numeric SAI selector once while its effective config is built.
    pub fn heatmap_quantity(self) -> HeatmapQuantity {
        match SaiObjectType::from_u32(self.type_id) {
            Some(SaiObjectType::Port) => match SaiPortStat::from_u32(self.stat_id) {
                Some(
                    SaiPortStat::InCurrOccupancyBytes
                    | SaiPortStat::InWatermarkBytes
                    | SaiPortStat::InSharedCurrOccupancyBytes
                    | SaiPortStat::InSharedWatermarkBytes
                    | SaiPortStat::OutCurrOccupancyBytes
                    | SaiPortStat::OutWatermarkBytes
                    | SaiPortStat::OutSharedCurrOccupancyBytes
                    | SaiPortStat::OutSharedWatermarkBytes,
                ) => HeatmapQuantity::AbsoluteBytes,
                Some(
                    SaiPortStat::IfInOctets
                    | SaiPortStat::IfOutOctets
                    | SaiPortStat::EtherStatsOctets
                    | SaiPortStat::IpInOctets
                    | SaiPortStat::IpOutOctets
                    | SaiPortStat::Ipv6InOctets
                    | SaiPortStat::Ipv6OutOctets
                    | SaiPortStat::GreenWredDroppedBytes
                    | SaiPortStat::YellowWredDroppedBytes
                    | SaiPortStat::RedWredDroppedBytes
                    | SaiPortStat::WredDroppedBytes,
                ) => HeatmapQuantity::DeltaBytes,
                _ => HeatmapQuantity::DeltaCount,
            },
            Some(SaiObjectType::Queue) => match SaiQueueStat::from_u32(self.stat_id) {
                Some(
                    SaiQueueStat::CurrOccupancyBytes
                    | SaiQueueStat::WatermarkBytes
                    | SaiQueueStat::SharedCurrOccupancyBytes
                    | SaiQueueStat::SharedWatermarkBytes,
                ) => HeatmapQuantity::AbsoluteBytes,
                Some(
                    SaiQueueStat::CurrOccupancyCells
                    | SaiQueueStat::WatermarkCells
                    | SaiQueueStat::SharedCurrOccupancyCells
                    | SaiQueueStat::SharedWatermarkCells,
                ) => HeatmapQuantity::AbsoluteCells,
                Some(
                    SaiQueueStat::Bytes
                    | SaiQueueStat::DroppedBytes
                    | SaiQueueStat::GreenBytes
                    | SaiQueueStat::GreenDroppedBytes
                    | SaiQueueStat::YellowBytes
                    | SaiQueueStat::YellowDroppedBytes
                    | SaiQueueStat::RedBytes
                    | SaiQueueStat::RedDroppedBytes
                    | SaiQueueStat::GreenWredDroppedBytes
                    | SaiQueueStat::YellowWredDroppedBytes
                    | SaiQueueStat::RedWredDroppedBytes
                    | SaiQueueStat::WredDroppedBytes
                    | SaiQueueStat::GreenWredEcnMarkedBytes
                    | SaiQueueStat::YellowWredEcnMarkedBytes
                    | SaiQueueStat::RedWredEcnMarkedBytes
                    | SaiQueueStat::WredEcnMarkedBytes,
                ) => HeatmapQuantity::DeltaBytes,
                Some(
                    SaiQueueStat::CurrOccupancyLevel
                    | SaiQueueStat::WatermarkLevel
                    | SaiQueueStat::DelayWatermarkNs,
                ) => HeatmapQuantity::Native,
                _ => HeatmapQuantity::DeltaCount,
            },
            Some(SaiObjectType::BufferPool) => match SaiBufferPoolStat::from_u32(self.stat_id) {
                Some(
                    SaiBufferPoolStat::CurrOccupancyBytes
                    | SaiBufferPoolStat::WatermarkBytes
                    | SaiBufferPoolStat::XoffRoomCurrOccupancyBytes
                    | SaiBufferPoolStat::XoffRoomWatermarkBytes,
                ) => HeatmapQuantity::AbsoluteBytes,
                Some(
                    SaiBufferPoolStat::XoffRoomCurrOccupancyCells
                    | SaiBufferPoolStat::XoffRoomWatermarkCells
                    | SaiBufferPoolStat::CurrOccupancyCells
                    | SaiBufferPoolStat::WatermarkCells,
                ) => HeatmapQuantity::AbsoluteCells,
                Some(
                    SaiBufferPoolStat::GreenWredDroppedBytes
                    | SaiBufferPoolStat::YellowWredDroppedBytes
                    | SaiBufferPoolStat::RedWredDroppedBytes
                    | SaiBufferPoolStat::WredDroppedBytes
                    | SaiBufferPoolStat::GreenWredEcnMarkedBytes
                    | SaiBufferPoolStat::YellowWredEcnMarkedBytes
                    | SaiBufferPoolStat::RedWredEcnMarkedBytes
                    | SaiBufferPoolStat::WredEcnMarkedBytes,
                ) => HeatmapQuantity::DeltaBytes,
                _ => HeatmapQuantity::DeltaCount,
            },
            Some(SaiObjectType::IngressPriorityGroup) => {
                match SaiIngressPriorityGroupStat::from_u32(self.stat_id) {
                    Some(
                        SaiIngressPriorityGroupStat::CurrOccupancyBytes
                        | SaiIngressPriorityGroupStat::WatermarkBytes
                        | SaiIngressPriorityGroupStat::SharedCurrOccupancyBytes
                        | SaiIngressPriorityGroupStat::SharedWatermarkBytes
                        | SaiIngressPriorityGroupStat::XoffRoomCurrOccupancyBytes
                        | SaiIngressPriorityGroupStat::XoffRoomWatermarkBytes,
                    ) => HeatmapQuantity::AbsoluteBytes,
                    Some(
                        SaiIngressPriorityGroupStat::CurrOccupancyCells
                        | SaiIngressPriorityGroupStat::WatermarkCells
                        | SaiIngressPriorityGroupStat::SharedCurrOccupancyCells
                        | SaiIngressPriorityGroupStat::SharedWatermarkCells
                        | SaiIngressPriorityGroupStat::XoffRoomCurrOccupancyCells
                        | SaiIngressPriorityGroupStat::XoffRoomWatermarkCells,
                    ) => HeatmapQuantity::AbsoluteCells,
                    Some(SaiIngressPriorityGroupStat::Bytes) => HeatmapQuantity::DeltaBytes,
                    _ => HeatmapQuantity::DeltaCount,
                }
            }
            _ => HeatmapQuantity::DeltaCount,
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

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum HeatmapQuantity {
    DeltaBytes,
    AbsoluteBytes,
    AbsoluteCells,
    DeltaCount,
    Native,
}

impl HeatmapQuantity {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::DeltaBytes => "delta_bytes",
            Self::AbsoluteBytes => "absolute_bytes",
            Self::AbsoluteCells => "absolute_cells",
            Self::DeltaCount => "delta_count",
            Self::Native => "native",
        }
    }

    pub fn unit(self) -> &'static str {
        match self {
            Self::DeltaBytes | Self::AbsoluteBytes => "By",
            Self::AbsoluteCells => "{cell}",
            Self::DeltaCount | Self::Native => "1",
        }
    }
}

#[derive(Debug, Clone)]
pub struct HeatmapLayout {
    quantity: HeatmapQuantity,
    explicit_bounds_u64: Arc<[u64]>,
    explicit_bounds: Arc<[f64]>,
}

impl PartialEq for HeatmapLayout {
    fn eq(&self, other: &Self) -> bool {
        self.quantity == other.quantity && self.explicit_bounds_u64 == other.explicit_bounds_u64
    }
}

impl Eq for HeatmapLayout {}

impl HeatmapLayout {
    fn from_bounds_unchecked(quantity: HeatmapQuantity, bounds: Vec<u64>) -> Arc<Self> {
        let explicit_bounds = bounds
            .iter()
            .map(|bound| *bound as f64)
            .collect::<Arc<[f64]>>();
        Arc::new(Self {
            quantity,
            explicit_bounds_u64: bounds.into(),
            explicit_bounds,
        })
    }

    pub fn from_explicit_bounds(bounds: Vec<u64>) -> Result<Arc<Self>, String> {
        Self::from_explicit_bounds_for(HeatmapQuantity::DeltaCount, bounds)
    }

    pub fn from_explicit_bounds_for(
        quantity: HeatmapQuantity,
        bounds: Vec<u64>,
    ) -> Result<Arc<Self>, String> {
        validate_explicit_bounds(&bounds)?;
        Ok(Self::from_bounds_unchecked(quantity, bounds))
    }

    pub fn quantity(&self) -> HeatmapQuantity {
        self.quantity
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

pub fn default_heatmap_layout(
    quantity: HeatmapQuantity,
    nominal_interval_us: Option<u32>,
) -> Result<Arc<HeatmapLayout>, String> {
    match quantity {
        HeatmapQuantity::DeltaBytes => {
            let interval_us = nominal_interval_us
                .filter(|interval| *interval != 0)
                .ok_or_else(|| {
                    "DeltaBytes default heatmap layout requires a non-zero nominal interval"
                        .to_string()
                })?;
            let layouts = DELTA_BYTES_LAYOUTS.get_or_init(|| Mutex::new(BTreeMap::new()));
            let mut layouts = layouts
                .lock()
                .map_err(|_| "DeltaBytes heatmap layout cache lock is poisoned".to_string())?;
            if let Some(layout) = layouts.get(&interval_us) {
                return Ok(layout.clone());
            }

            let mut bounds = Vec::with_capacity(
                1 + DELTA_BYTES_LINK_SPEEDS_GBPS.len() * DELTA_BYTES_UTILIZATION_BASIS_POINTS.len(),
            );
            bounds.push(0);
            for speed_gbps in DELTA_BYTES_LINK_SPEEDS_GBPS {
                for utilization_basis_points in DELTA_BYTES_UTILIZATION_BASIS_POINTS {
                    // Gbit/s * basis points * us / 80 yields bytes. Flooring is
                    // exact for integer observations when the ideal threshold
                    // lies between two whole-byte values.
                    let bound = u128::from(speed_gbps)
                        .checked_mul(u128::from(utilization_basis_points))
                        .and_then(|value| value.checked_mul(u128::from(interval_us)))
                        .ok_or_else(|| {
                            format!(
                                "DeltaBytes default boundary overflow for nominal interval {}us",
                                interval_us
                            )
                        })?
                        / 80;
                    let bound = u64::try_from(bound).map_err(|_| {
                        format!(
                            "DeltaBytes default boundary exceeds u64 for nominal interval {}us",
                            interval_us
                        )
                    })?;
                    if bound > MAX_EXACT_OTLP_BOUNDARY {
                        return Err(format!(
                            "DeltaBytes default boundary {} exceeds exact OTLP limit {} for nominal interval {}us",
                            bound, MAX_EXACT_OTLP_BOUNDARY, interval_us
                        ));
                    }
                    bounds.push(bound);
                }
            }
            bounds.sort_unstable();
            bounds.dedup();
            let layout = HeatmapLayout::from_bounds_unchecked(quantity, bounds);
            layouts.insert(interval_us, layout.clone());
            Ok(layout)
        }
        HeatmapQuantity::AbsoluteBytes => Ok(ABSOLUTE_BYTES_LAYOUT
            .get_or_init(|| {
                HeatmapLayout::from_bounds_unchecked(quantity, ABSOLUTE_BYTES_BOUNDS.to_vec())
            })
            .clone()),
        HeatmapQuantity::AbsoluteCells => Ok(ABSOLUTE_CELLS_LAYOUT
            .get_or_init(|| {
                HeatmapLayout::from_bounds_unchecked(
                    quantity,
                    std::iter::once(0)
                        .chain((0..=24).map(|power| 1u64 << power))
                        .collect(),
                )
            })
            .clone()),
        HeatmapQuantity::DeltaCount => Ok(DELTA_COUNT_LAYOUT
            .get_or_init(|| {
                HeatmapLayout::from_bounds_unchecked(quantity, DELTA_COUNT_BOUNDS.to_vec())
            })
            .clone()),
        HeatmapQuantity::Native => Ok(NATIVE_LAYOUT
            .get_or_init(|| {
                HeatmapLayout::from_bounds_unchecked(
                    quantity,
                    std::iter::once(0)
                        .chain((0..=53).map(|power| 1u64 << power))
                        .collect(),
                )
            })
            .clone()),
    }
}

fn validate_explicit_bounds(bounds: &[u64]) -> Result<(), String> {
    if !(1..=MAX_HEATMAP_EXPLICIT_BOUNDS).contains(&bounds.len()) {
        return Err(format!(
            "explicit_bounds must contain between 1 and {} values",
            MAX_HEATMAP_EXPLICIT_BOUNDS
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

/// Returns `hft-explicit-v2:<kind>:<quantity>:fnv1a64-<16 lowercase hex digits>`.
/// The FNV-1a input is the value kind, quantity, bound count, and each u64
/// bound in little-endian order, making schema identity stable across processes.
pub fn heatmap_schema(
    value_kind: HeatmapValueKind,
    quantity: HeatmapQuantity,
    bounds: &[u64],
) -> Arc<str> {
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
    update(quantity.as_str().as_bytes());
    update(&(bounds.len() as u64).to_le_bytes());
    for bound in bounds {
        update(&bound.to_le_bytes());
    }

    Arc::from(format!(
        "hft-explicit-v2:{}:{}:fnv1a64-{hash:016x}",
        value_kind.as_str(),
        quantity.as_str()
    ))
}

/// CounterSyncd-side subset of HIGH_FREQUENCY_TELEMETRY_AGGREGATOR.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AggregatorConfig {
    /// Reporting interval in microseconds.
    pub reporting_rate: Option<u32>,
    /// Effective profile polling interval in microseconds; not an aggregator DB field.
    pub poll_interval_us: Option<u32>,
    /// Counters corrected when a newly reported raw value is lower than the previous value.
    pub rollover_counters: HashSet<CounterSelector>,
    /// Counter widths that differ from the default rollover width.
    pub rollover_bit_width_overrides: BTreeMap<CounterSelector, u8>,
    /// Heatmap aggregation interval in microseconds.
    pub heatmap_interval: Option<u32>,
    /// Counters summarized as heatmaps after optional reporting-rate aggregation.
    pub heatmap_counters: HashSet<CounterSelector>,
    /// Inclusive upper bounds keyed by selected heatmap counter.
    pub heatmap_explicit_bounds: BTreeMap<CounterSelector, Vec<u64>>,
    /// Resolved layouts carried by an effective session config.
    pub heatmap_layouts: BTreeMap<CounterSelector, Arc<HeatmapLayout>>,
}

impl Default for AggregatorConfig {
    fn default() -> Self {
        Self {
            reporting_rate: None,
            poll_interval_us: None,
            rollover_counters: HashSet::new(),
            rollover_bit_width_overrides: BTreeMap::new(),
            heatmap_interval: None,
            heatmap_counters: HashSet::new(),
            heatmap_explicit_bounds: BTreeMap::new(),
            heatmap_layouts: BTreeMap::new(),
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
    pub quantity: HeatmapQuantity,
    pub unit: &'static str,
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
    pub fn nominal_interval_us(&self) -> Option<u32> {
        match (self.reporting_rate, self.poll_interval_us) {
            (Some(reporting_rate), Some(poll_interval)) => Some(reporting_rate.max(poll_interval)),
            (Some(reporting_rate), None) => Some(reporting_rate),
            (None, Some(poll_interval)) => Some(poll_interval),
            (None, None) => None,
        }
    }

    pub(crate) fn validate_structure(&self) -> Result<(), String> {
        if self.reporting_rate == Some(0) {
            return Err("reporting_rate must be greater than zero".to_string());
        }
        if self.poll_interval_us == Some(0) {
            return Err("poll_interval_us must be greater than zero".to_string());
        }
        if self.heatmap_interval == Some(0) {
            return Err("heatmap_interval must be greater than zero".to_string());
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

        for (selector, bit_width) in &self.rollover_bit_width_overrides {
            if !self.rollover_counters.contains(selector) {
                return Err(format!(
                    "bit_width override selector type {} stat {} is not selected by rollover_counters",
                    selector.type_id, selector.stat_id
                ));
            }
            if !(MIN_ROLLOVER_BIT_WIDTH..=MAX_ROLLOVER_BIT_WIDTH).contains(bit_width) {
                return Err(format!(
                    "rollover bit_width must be in range {}..={}",
                    MIN_ROLLOVER_BIT_WIDTH, MAX_ROLLOVER_BIT_WIDTH
                ));
            }
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

    pub fn validate(&self) -> Result<(), String> {
        self.validate_structure()?;
        let nominal_interval_us = self.nominal_interval_us();
        for selector in &self.heatmap_counters {
            if selector.heatmap_quantity() == HeatmapQuantity::DeltaBytes
                && !self.heatmap_explicit_bounds.contains_key(selector)
                && nominal_interval_us.is_none_or(|interval| interval == 0)
            {
                return Err(format!(
                    "byte-delta heatmap selector type {} stat {} requires a non-zero reporting_rate or profile poll_interval when explicit_bounds are absent",
                    selector.type_id, selector.stat_id
                ));
            }
        }
        Ok(())
    }

    pub fn resolve_heatmap_layouts(&mut self) -> Result<(), String> {
        self.validate()?;
        let nominal_interval_us = self.nominal_interval_us();
        let mut layouts = BTreeMap::new();
        for selector in &self.heatmap_counters {
            let quantity = selector.heatmap_quantity();
            let layout = match self.heatmap_explicit_bounds.get(selector) {
                Some(bounds) => HeatmapLayout::from_explicit_bounds_for(quantity, bounds.clone())?,
                None => default_heatmap_layout(quantity, nominal_interval_us)?,
            };
            layouts.insert(*selector, layout);
        }
        self.heatmap_layouts = layouts;
        Ok(())
    }

    pub(crate) fn layouts_are_resolved(&self) -> bool {
        self.heatmap_layouts.len() == self.heatmap_counters.len()
            && self.heatmap_counters.iter().all(|selector| {
                let Some(layout) = self.heatmap_layouts.get(selector) else {
                    return false;
                };
                layout.quantity() == selector.heatmap_quantity()
                    && match self.heatmap_explicit_bounds.get(selector) {
                        Some(bounds) => layout.explicit_bounds_u64() == bounds,
                        None => default_heatmap_layout(
                            selector.heatmap_quantity(),
                            self.nominal_interval_us(),
                        )
                        .is_ok_and(|expected| Arc::ptr_eq(layout, &expected)),
                    }
            })
    }

    pub fn rollover_bit_width_for(&self, selector: CounterSelector) -> u8 {
        self.rollover_bit_width_overrides
            .get(&selector)
            .copied()
            .unwrap_or(DEFAULT_ROLLOVER_BIT_WIDTH)
    }

    pub fn layout_for(&self, selector: CounterSelector) -> Result<Arc<HeatmapLayout>, String> {
        self.heatmap_layouts.get(&selector).cloned().ok_or_else(|| {
            format!(
                "heatmap layout for selector type {} stat {} has not been resolved",
                selector.type_id, selector.stat_id
            )
        })
    }

    pub fn quantity_and_layout_for(
        &self,
        selector: CounterSelector,
    ) -> Result<(HeatmapQuantity, Arc<HeatmapLayout>), String> {
        let layout = self.layout_for(selector)?;
        Ok((layout.quantity(), layout))
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

    fn selector(group: &str, counter: &str) -> CounterSelector {
        CounterSelector::parse(&format!("{}|{}", group, counter)).unwrap()
    }

    #[test]
    fn parses_and_rejects_counter_selectors() {
        let selectors = CounterSelector::parse_list(
            "PORT|IF_IN_UCAST_PKTS,QUEUE|WATERMARK_BYTES,BUFFER_POOL|CURR_OCCUPANCY_BYTES,INGRESS_PRIORITY_GROUP|PACKETS",
        )
        .unwrap();
        assert_eq!(selectors.len(), 4);

        for invalid in [
            "QUEUE|IF_IN_UCAST_PKTS",
            "PORT",
            "UNKNOWN|PACKETS",
            "PORT|END",
            "PORT|START",
            "QUEUE|CUSTOM_RANGE_BASE",
            "BUFFER_POOL|CUSTOM_RANGE_BASE",
            "INGRESS_PRIORITY_GROUP|CUSTOM_RANGE_BASE",
            "PORT|IF_IN_OCTETS,,QUEUE|PACKETS",
        ] {
            assert!(CounterSelector::parse_list(invalid).is_err(), "{invalid}");
        }
    }

    #[test]
    fn default_templates_are_exact_and_cached() {
        let absolute_bytes = default_heatmap_layout(HeatmapQuantity::AbsoluteBytes, None).unwrap();
        assert_eq!(
            absolute_bytes.explicit_bounds_u64(),
            [
                0,
                512,
                1_024,
                524_288,
                1_048_576,
                5_242_880,
                10_485_760,
                52_428_800,
                104_857_600
            ]
        );
        assert_eq!(absolute_bytes.bucket_count(), 10);

        let delta_count = default_heatmap_layout(HeatmapQuantity::DeltaCount, None).unwrap();
        assert_eq!(
            delta_count.explicit_bounds_u64(),
            [
                0,
                1,
                2,
                5,
                10,
                20,
                50,
                100,
                200,
                500,
                1_000,
                2_000,
                5_000,
                10_000,
                20_000,
                50_000,
                100_000,
                200_000,
                500_000,
                1_000_000,
                2_000_000,
                5_000_000,
                10_000_000,
                20_000_000,
                50_000_000,
                100_000_000,
                200_000_000,
                500_000_000,
            ]
        );
        assert_eq!(delta_count.bucket_count(), 29);

        let absolute_cells = default_heatmap_layout(HeatmapQuantity::AbsoluteCells, None).unwrap();
        assert_eq!(
            absolute_cells.explicit_bounds_u64(),
            [
                0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1_024, 2_048, 4_096, 8_192, 16_384,
                32_768, 65_536, 131_072, 262_144, 524_288, 1_048_576, 2_097_152, 4_194_304,
                8_388_608, 16_777_216,
            ]
        );
        assert_eq!(absolute_cells.bucket_count(), 27);

        let native = default_heatmap_layout(HeatmapQuantity::Native, None).unwrap();
        assert_eq!(
            native.explicit_bounds_u64(),
            [
                0,
                1,
                2,
                4,
                8,
                16,
                32,
                64,
                128,
                256,
                512,
                1_024,
                2_048,
                4_096,
                8_192,
                16_384,
                32_768,
                65_536,
                131_072,
                262_144,
                524_288,
                1_048_576,
                2_097_152,
                4_194_304,
                8_388_608,
                16_777_216,
                33_554_432,
                67_108_864,
                134_217_728,
                268_435_456,
                536_870_912,
                1_073_741_824,
                2_147_483_648,
                4_294_967_296,
                8_589_934_592,
                17_179_869_184,
                34_359_738_368,
                68_719_476_736,
                137_438_953_472,
                274_877_906_944,
                549_755_813_888,
                1_099_511_627_776,
                2_199_023_255_552,
                4_398_046_511_104,
                8_796_093_022_208,
                17_592_186_044_416,
                35_184_372_088_832,
                70_368_744_177_664,
                140_737_488_355_328,
                281_474_976_710_656,
                562_949_953_421_312,
                1_125_899_906_842_624,
                2_251_799_813_685_248,
                4_503_599_627_370_496,
                9_007_199_254_740_992,
            ]
        );
        assert_eq!(native.bucket_count(), 56);

        for (quantity, value_kind, expected) in [
            (
                HeatmapQuantity::AbsoluteBytes,
                HeatmapValueKind::CurrentOccupancy,
                "hft-explicit-v2:current_occupancy:absolute_bytes:fnv1a64-d339f5de765cb0e5",
            ),
            (
                HeatmapQuantity::AbsoluteCells,
                HeatmapValueKind::CurrentOccupancy,
                "hft-explicit-v2:current_occupancy:absolute_cells:fnv1a64-9ba92b0792237ea7",
            ),
            (
                HeatmapQuantity::DeltaCount,
                HeatmapValueKind::Delta,
                "hft-explicit-v2:delta:delta_count:fnv1a64-bb2d0f64f0904c89",
            ),
            (
                HeatmapQuantity::Native,
                HeatmapValueKind::CurrentOccupancy,
                "hft-explicit-v2:current_occupancy:native:fnv1a64-e9dbd596a6a66b71",
            ),
        ] {
            let layout = default_heatmap_layout(quantity, None).unwrap();
            assert_eq!(
                heatmap_schema(value_kind, quantity, layout.explicit_bounds_u64()).as_ref(),
                expected
            );
        }

        for quantity in [
            HeatmapQuantity::AbsoluteBytes,
            HeatmapQuantity::AbsoluteCells,
            HeatmapQuantity::DeltaCount,
            HeatmapQuantity::Native,
        ] {
            assert!(Arc::ptr_eq(
                &default_heatmap_layout(quantity, None).unwrap(),
                &default_heatmap_layout(quantity, Some(123)).unwrap()
            ));
        }
    }

    #[test]
    fn delta_bytes_boundaries_use_exact_nominal_intervals() {
        let expected_1ms = [
            0,
            6_250_000,
            9_375_000,
            11_250_000,
            11_875_000,
            12_250_000,
            12_375_000,
            12_437_500,
            12_475_000,
            12_500_000,
            18_750_000,
            22_500_000,
            23_750_000,
            24_500_000,
            24_750_000,
            24_875_000,
            24_950_000,
            25_000_000,
            37_500_000,
            45_000_000,
            47_500_000,
            49_000_000,
            49_500_000,
            49_750_000,
            49_900_000,
            50_000_000,
            75_000_000,
            90_000_000,
            95_000_000,
            98_000_000,
            99_000_000,
            99_500_000,
            99_800_000,
            100_000_000,
            150_000_000,
            180_000_000,
            190_000_000,
            196_000_000,
            198_000_000,
            199_000_000,
            199_600_000,
            200_000_000,
        ];
        for (interval_us, scale, schema) in [
            (
                1_000,
                1,
                "hft-explicit-v2:delta:delta_bytes:fnv1a64-ab511b6c09d3288e",
            ),
            (
                10_000,
                10,
                "hft-explicit-v2:delta:delta_bytes:fnv1a64-2a8d2b3d7ca226cc",
            ),
            (
                100_000,
                100,
                "hft-explicit-v2:delta:delta_bytes:fnv1a64-a09523fcb7c0174d",
            ),
        ] {
            let layout =
                default_heatmap_layout(HeatmapQuantity::DeltaBytes, Some(interval_us)).unwrap();
            let expected = expected_1ms.map(|bound| bound * scale);
            assert_eq!(layout.explicit_bounds_u64(), expected);
            assert_eq!(layout.bucket_count(), 43);
            assert_eq!(
                layout.explicit_bounds_u64().last(),
                Some(&(1_600 * 125 * u64::from(interval_us)))
            );
            assert!(Arc::ptr_eq(
                &layout,
                &default_heatmap_layout(HeatmapQuantity::DeltaBytes, Some(interval_us)).unwrap()
            ));
            assert_eq!(
                heatmap_schema(
                    HeatmapValueKind::Delta,
                    HeatmapQuantity::DeltaBytes,
                    layout.explicit_bounds_u64(),
                )
                .as_ref(),
                schema
            );
        }
        assert!(!Arc::ptr_eq(
            &default_heatmap_layout(HeatmapQuantity::DeltaBytes, Some(1_000)).unwrap(),
            &default_heatmap_layout(HeatmapQuantity::DeltaBytes, Some(10_000)).unwrap()
        ));

        let one_microsecond = default_heatmap_layout(HeatmapQuantity::DeltaBytes, Some(1)).unwrap();
        assert_eq!(one_microsecond.bucket_count(), 43);
        assert!(one_microsecond.explicit_bounds_u64().contains(&12_437));
        assert!(!one_microsecond.explicit_bounds_u64().contains(&12_438));

        let maximum_interval =
            default_heatmap_layout(HeatmapQuantity::DeltaBytes, Some(u32::MAX)).unwrap();
        assert_eq!(
            maximum_interval.explicit_bounds_u64().last(),
            Some(&(1_600 * 125 * u64::from(u32::MAX)))
        );
        assert!(default_heatmap_layout(HeatmapQuantity::DeltaBytes, None).is_err());
        assert!(default_heatmap_layout(HeatmapQuantity::DeltaBytes, Some(0)).is_err());
    }

    #[test]
    fn delta_bytes_layout_distinguishes_common_link_saturation_bands() {
        let layout = default_heatmap_layout(HeatmapQuantity::DeltaBytes, Some(1_000)).unwrap();
        let bucket_for_mbps = |rate_mbps: u64| {
            let bytes = rate_mbps * 1_000 / 8;
            layout
                .explicit_bounds_u64()
                .partition_point(|bound| *bound < bytes)
        };

        assert!(bucket_for_mbps(50_010) < bucket_for_mbps(99_800));
        assert!(bucket_for_mbps(99_800) < bucket_for_mbps(100_000));
        for speed_gbps in DELTA_BYTES_LINK_SPEEDS_GBPS {
            let saturation_buckets =
                [9_500, 9_800, 9_900, 9_950, 9_980, 10_000].map(|utilization_basis_points| {
                    bucket_for_mbps(speed_gbps * utilization_basis_points / 10)
                });
            assert!(saturation_buckets.windows(2).all(|pair| pair[0] < pair[1]));
        }
    }

    #[test]
    fn classifies_every_yang_allowed_counter() {
        let cases = [
            ("PORT", "IF_IN_OCTETS", HeatmapQuantity::DeltaBytes),
            ("PORT", "IF_OUT_OCTETS", HeatmapQuantity::DeltaBytes),
            ("PORT", "IF_IN_UCAST_PKTS", HeatmapQuantity::DeltaCount),
            ("PORT", "IF_IN_DISCARDS", HeatmapQuantity::DeltaCount),
            ("PORT", "IF_IN_ERRORS", HeatmapQuantity::DeltaCount),
            (
                "PORT",
                "IN_CURR_OCCUPANCY_BYTES",
                HeatmapQuantity::AbsoluteBytes,
            ),
            ("PORT", "IF_OUT_DISCARDS", HeatmapQuantity::DeltaCount),
            ("PORT", "IF_OUT_ERRORS", HeatmapQuantity::DeltaCount),
            ("PORT", "IF_OUT_UCAST_PKTS", HeatmapQuantity::DeltaCount),
            (
                "PORT",
                "OUT_CURR_OCCUPANCY_BYTES",
                HeatmapQuantity::AbsoluteBytes,
            ),
            ("PORT", "TRIM_PACKETS", HeatmapQuantity::DeltaCount),
            ("PORT", "PAUSE_RX_PKTS", HeatmapQuantity::DeltaCount),
            ("PORT", "PAUSE_TX_PKTS", HeatmapQuantity::DeltaCount),
            (
                "BUFFER_POOL",
                "DROPPED_PACKETS",
                HeatmapQuantity::DeltaCount,
            ),
            (
                "BUFFER_POOL",
                "CURR_OCCUPANCY_BYTES",
                HeatmapQuantity::AbsoluteBytes,
            ),
            (
                "BUFFER_POOL",
                "WATERMARK_BYTES",
                HeatmapQuantity::AbsoluteBytes,
            ),
            (
                "BUFFER_POOL",
                "XOFF_ROOM_WATERMARK_BYTES",
                HeatmapQuantity::AbsoluteBytes,
            ),
            (
                "BUFFER_POOL",
                "CURR_OCCUPANCY_CELLS",
                HeatmapQuantity::AbsoluteCells,
            ),
            (
                "BUFFER_POOL",
                "WATERMARK_CELLS",
                HeatmapQuantity::AbsoluteCells,
            ),
            (
                "INGRESS_PRIORITY_GROUP",
                "PACKETS",
                HeatmapQuantity::DeltaCount,
            ),
            (
                "INGRESS_PRIORITY_GROUP",
                "BYTES",
                HeatmapQuantity::DeltaBytes,
            ),
            (
                "INGRESS_PRIORITY_GROUP",
                "CURR_OCCUPANCY_BYTES",
                HeatmapQuantity::AbsoluteBytes,
            ),
            (
                "INGRESS_PRIORITY_GROUP",
                "WATERMARK_BYTES",
                HeatmapQuantity::AbsoluteBytes,
            ),
            (
                "INGRESS_PRIORITY_GROUP",
                "XOFF_ROOM_CURR_OCCUPANCY_BYTES",
                HeatmapQuantity::AbsoluteBytes,
            ),
            (
                "INGRESS_PRIORITY_GROUP",
                "XOFF_ROOM_WATERMARK_BYTES",
                HeatmapQuantity::AbsoluteBytes,
            ),
            (
                "INGRESS_PRIORITY_GROUP",
                "DROPPED_PACKETS",
                HeatmapQuantity::DeltaCount,
            ),
            (
                "INGRESS_PRIORITY_GROUP",
                "CURR_OCCUPANCY_CELLS",
                HeatmapQuantity::AbsoluteCells,
            ),
            (
                "INGRESS_PRIORITY_GROUP",
                "WATERMARK_CELLS",
                HeatmapQuantity::AbsoluteCells,
            ),
            (
                "INGRESS_PRIORITY_GROUP",
                "XOFF_ROOM_CURR_OCCUPANCY_CELLS",
                HeatmapQuantity::AbsoluteCells,
            ),
            (
                "INGRESS_PRIORITY_GROUP",
                "XOFF_ROOM_WATERMARK_CELLS",
                HeatmapQuantity::AbsoluteCells,
            ),
            ("QUEUE", "PACKETS", HeatmapQuantity::DeltaCount),
            ("QUEUE", "BYTES", HeatmapQuantity::DeltaBytes),
            ("QUEUE", "DROPPED_PACKETS", HeatmapQuantity::DeltaCount),
            (
                "QUEUE",
                "CURR_OCCUPANCY_BYTES",
                HeatmapQuantity::AbsoluteBytes,
            ),
            ("QUEUE", "WATERMARK_BYTES", HeatmapQuantity::AbsoluteBytes),
            (
                "QUEUE",
                "WRED_ECN_MARKED_PACKETS",
                HeatmapQuantity::DeltaCount,
            ),
            ("QUEUE", "TRIM_PACKETS", HeatmapQuantity::DeltaCount),
            (
                "QUEUE",
                "CURR_OCCUPANCY_CELLS",
                HeatmapQuantity::AbsoluteCells,
            ),
            ("QUEUE", "WATERMARK_CELLS", HeatmapQuantity::AbsoluteCells),
        ];
        for (group, counter, expected) in cases {
            assert_eq!(
                selector(group, counter).heatmap_quantity(),
                expected,
                "{group}|{counter}"
            );
        }
        for priority in 0..=7 {
            for direction in ["RX", "TX"] {
                assert_eq!(
                    selector("PORT", &format!("PFC_{priority}_{direction}_PKTS"))
                        .heatmap_quantity(),
                    HeatmapQuantity::DeltaCount
                );
            }
        }
    }

    #[test]
    fn classifies_practical_byte_variants_native_and_unknown() {
        for selector in [
            CounterSelector::new(1, SaiPortStat::EtherStatsOctets.to_u32()),
            CounterSelector::new(1, SaiPortStat::Ipv6OutOctets.to_u32()),
            CounterSelector::new(21, SaiQueueStat::RedWredDroppedBytes.to_u32()),
            CounterSelector::new(24, SaiBufferPoolStat::WredEcnMarkedBytes.to_u32()),
        ] {
            assert_eq!(selector.heatmap_quantity(), HeatmapQuantity::DeltaBytes);
        }
        for stat in [
            SaiQueueStat::CurrOccupancyLevel,
            SaiQueueStat::WatermarkLevel,
            SaiQueueStat::DelayWatermarkNs,
        ] {
            assert_eq!(
                CounterSelector::new(21, stat.to_u32()).heatmap_quantity(),
                HeatmapQuantity::Native
            );
        }
        assert_eq!(
            CounterSelector::new(u32::MAX, u32::MAX).heatmap_quantity(),
            HeatmapQuantity::DeltaCount
        );
    }

    #[test]
    fn resolves_custom_override_without_interval_and_validates_defaults() {
        let bytes = selector("PORT", "IF_IN_OCTETS");
        let count = selector("PORT", "IF_IN_UCAST_PKTS");
        let mut custom = AggregatorConfig {
            heatmap_interval: Some(1_000),
            heatmap_counters: HashSet::from([bytes]),
            heatmap_explicit_bounds: BTreeMap::from([(bytes, vec![0, 1_024, 4_096])]),
            ..Default::default()
        };
        custom.resolve_heatmap_layouts().unwrap();
        assert_eq!(
            custom.layout_for(bytes).unwrap().explicit_bounds_u64(),
            &[0, 1_024, 4_096]
        );

        let mut count_default = AggregatorConfig {
            heatmap_interval: Some(1_000),
            heatmap_counters: HashSet::from([count]),
            ..Default::default()
        };
        count_default.resolve_heatmap_layouts().unwrap();
        assert_eq!(count_default.layout_for(count).unwrap().bucket_count(), 29);

        let missing = AggregatorConfig {
            heatmap_interval: Some(1_000),
            heatmap_counters: HashSet::from([bytes]),
            ..Default::default()
        };
        assert!(missing.validate().unwrap_err().contains("byte-delta"));
        let zero = AggregatorConfig {
            poll_interval_us: Some(0),
            ..missing
        };
        assert!(zero
            .validate()
            .unwrap_err()
            .contains("poll_interval_us must be greater than zero"));

        let zero_without_heatmaps = AggregatorConfig {
            poll_interval_us: Some(0),
            ..Default::default()
        };
        assert_eq!(
            zero_without_heatmaps.validate_structure().unwrap_err(),
            "poll_interval_us must be greater than zero"
        );
    }

    #[test]
    fn nominal_interval_uses_slower_reporting_or_poll_interval() {
        let bytes = selector("PORT", "IF_IN_OCTETS");
        let mut config = AggregatorConfig {
            reporting_rate: Some(1_000),
            poll_interval_us: Some(10_000),
            heatmap_interval: Some(100_000),
            heatmap_counters: HashSet::from([bytes]),
            ..Default::default()
        };
        config.resolve_heatmap_layouts().unwrap();
        assert_eq!(
            config.layout_for(bytes).unwrap().explicit_bounds_u64()[1],
            62_500_000
        );

        config.reporting_rate = Some(100_000);
        config.resolve_heatmap_layouts().unwrap();
        assert_eq!(
            config.layout_for(bytes).unwrap().explicit_bounds_u64()[1],
            625_000_000
        );

        for (reporting_rate, poll_interval_us, expected) in [
            (Some(1_000), None, Some(1_000)),
            (None, Some(10_000), Some(10_000)),
            (Some(1_000), Some(10_000), Some(10_000)),
            (Some(100_000), Some(10_000), Some(100_000)),
            (None, None, None),
        ] {
            let config = AggregatorConfig {
                reporting_rate,
                poll_interval_us,
                ..Default::default()
            };
            assert_eq!(config.nominal_interval_us(), expected);
        }
    }

    #[test]
    fn quantities_expose_raw_metric_units() {
        for (quantity, unit) in [
            (HeatmapQuantity::DeltaBytes, "By"),
            (HeatmapQuantity::AbsoluteBytes, "By"),
            (HeatmapQuantity::AbsoluteCells, "{cell}"),
            (HeatmapQuantity::DeltaCount, "1"),
            (HeatmapQuantity::Native, "1"),
        ] {
            assert_eq!(quantity.unit(), unit);
            assert!(!quantity.unit().contains("/s"));
        }
    }

    #[test]
    fn validates_explicit_bounds_and_rollover() {
        assert_eq!(
            AggregatorConfig::parse_explicit_bounds("0, 1024,4096").unwrap(),
            vec![0, 1_024, 4_096]
        );
        assert!(AggregatorConfig::parse_explicit_bounds("0,invalid").is_err());
        assert!(AggregatorConfig::parse_explicit_bounds("0,,1024").is_err());
        assert!(HeatmapLayout::from_explicit_bounds((0..511).collect()).is_ok());
        assert!(HeatmapLayout::from_explicit_bounds((0..512).collect()).is_err());
        assert!(HeatmapLayout::from_explicit_bounds(vec![0, 2, 1]).is_err());
        assert!(HeatmapLayout::from_explicit_bounds(vec![MAX_EXACT_OTLP_BOUNDARY + 1]).is_err());

        let selected = selector("PORT", "IF_IN_OCTETS");
        for bit_width in [1, 24, 32, 48, 63] {
            let config = AggregatorConfig {
                rollover_counters: HashSet::from([selected]),
                rollover_bit_width_overrides: BTreeMap::from([(selected, bit_width)]),
                ..Default::default()
            };
            assert!(config.validate().is_ok());
            assert_eq!(config.rollover_bit_width_for(selected), bit_width);
        }
        for bit_width in [0, 64] {
            let config = AggregatorConfig {
                rollover_counters: HashSet::from([selected]),
                rollover_bit_width_overrides: BTreeMap::from([(selected, bit_width)]),
                ..Default::default()
            };
            assert!(config.validate().is_err());
        }
    }

    #[test]
    fn schema_is_stable_and_separates_kind_quantity_and_layout() {
        let delta = heatmap_schema(
            HeatmapValueKind::Delta,
            HeatmapQuantity::DeltaCount,
            &[1, 2, 8],
        );
        assert_eq!(
            delta,
            heatmap_schema(
                HeatmapValueKind::Delta,
                HeatmapQuantity::DeltaCount,
                &[1, 2, 8]
            )
        );
        assert!(delta.starts_with("hft-explicit-v2:delta:delta_count:fnv1a64-"));
        assert_ne!(
            delta,
            heatmap_schema(
                HeatmapValueKind::Watermark,
                HeatmapQuantity::DeltaCount,
                &[1, 2, 8]
            )
        );
        assert_ne!(
            delta,
            heatmap_schema(
                HeatmapValueKind::Delta,
                HeatmapQuantity::DeltaBytes,
                &[1, 2, 8]
            )
        );
        assert_ne!(
            delta,
            heatmap_schema(
                HeatmapValueKind::Delta,
                HeatmapQuantity::DeltaCount,
                &[1, 2, 9]
            )
        );
    }

    #[test]
    fn classifies_raw_value_kinds_by_numeric_id() {
        for selector in [
            selector("PORT", "IN_WATERMARK_BYTES"),
            selector("QUEUE", "WATERMARK_CELLS"),
            selector("BUFFER_POOL", "XOFF_ROOM_WATERMARK_BYTES"),
            selector("INGRESS_PRIORITY_GROUP", "XOFF_ROOM_WATERMARK_CELLS"),
        ] {
            assert_eq!(selector.heatmap_value_kind(), HeatmapValueKind::Watermark);
        }
        for selector in [
            selector("PORT", "OUT_CURR_OCCUPANCY_BYTES"),
            selector("QUEUE", "CURR_OCCUPANCY_CELLS"),
            selector("BUFFER_POOL", "XOFF_ROOM_CURR_OCCUPANCY_CELLS"),
            selector("INGRESS_PRIORITY_GROUP", "CURR_OCCUPANCY_BYTES"),
        ] {
            assert_eq!(
                selector.heatmap_value_kind(),
                HeatmapValueKind::CurrentOccupancy
            );
        }
    }

    #[test]
    fn reuses_empty_heatmap_storage() {
        let stats = Arc::new(super::super::saistats::SAIStats::new(1, Vec::new()));
        let first = StatsMessage::new(None, stats.clone());
        let second = StatsMessage::with_heatmaps(None, stats, Vec::new());
        assert!(Arc::ptr_eq(&first.heatmaps, &second.heatmaps));
    }
}
