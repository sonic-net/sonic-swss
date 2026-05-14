use std::fmt;
use std::str::FromStr;

/// SAI queue statistics enum
/// This enum represents all the queue statistics defined in sai_queue_stat_t
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum SaiQueueStat {
    /// Get/set tx packets count [uint64_t]
    Packets = 0x00000000,

    /// Get/set tx bytes count [uint64_t]
    Bytes = 0x00000001,

    /// Get/set dropped packets count [uint64_t]
    DroppedPackets = 0x00000002,

    /// Get/set dropped bytes count [uint64_t]
    DroppedBytes = 0x00000003,

    /// Get/set green color tx packets count [uint64_t]
    GreenPackets = 0x00000004,

    /// Get/set green color tx bytes count [uint64_t]
    GreenBytes = 0x00000005,

    /// Get/set green color dropped packets count [uint64_t]
    GreenDroppedPackets = 0x00000006,

    /// Get/set green color dropped bytes count [uint64_t]
    GreenDroppedBytes = 0x00000007,

    /// Get/set yellow color tx packets count [uint64_t]
    YellowPackets = 0x00000008,

    /// Get/set yellow color tx bytes count [uint64_t]
    YellowBytes = 0x00000009,

    /// Get/set yellow color dropped packets count [uint64_t]
    YellowDroppedPackets = 0x0000000a,

    /// Get/set yellow color dropped bytes count [uint64_t]
    YellowDroppedBytes = 0x0000000b,

    /// Get/set red color tx packets count [uint64_t]
    RedPackets = 0x0000000c,

    /// Get/set red color tx bytes count [uint64_t]
    RedBytes = 0x0000000d,

    /// Get/set red color dropped packets count [uint64_t]
    RedDroppedPackets = 0x0000000e,

    /// Get/set red color dropped bytes count [uint64_t]
    RedDroppedBytes = 0x0000000f,

    /// Get/set WRED green color dropped packets count [uint64_t]
    GreenWredDroppedPackets = 0x00000010,

    /// Get/set WRED green color dropped bytes count [uint64_t]
    GreenWredDroppedBytes = 0x00000011,

    /// Get/set WRED yellow color dropped packets count [uint64_t]
    YellowWredDroppedPackets = 0x00000012,

    /// Get/set WRED yellow color dropped bytes count [uint64_t]
    YellowWredDroppedBytes = 0x00000013,

    /// Get/set WRED red color dropped packets count [uint64_t]
    RedWredDroppedPackets = 0x00000014,

    /// Get/set WRED red color dropped bytes count [uint64_t]
    RedWredDroppedBytes = 0x00000015,

    /// Get/set WRED dropped packets count [uint64_t]
    WredDroppedPackets = 0x00000016,

    /// Get/set WRED dropped bytes count [uint64_t]
    WredDroppedBytes = 0x00000017,

    /// Get current queue occupancy in bytes [uint64_t]
    CurrOccupancyBytes = 0x00000018,

    /// Get watermark queue occupancy in bytes [uint64_t]
    WatermarkBytes = 0x00000019,

    /// Get current queue shared occupancy in bytes [uint64_t]
    SharedCurrOccupancyBytes = 0x0000001a,

    /// Get watermark queue shared occupancy in bytes [uint64_t]
    SharedWatermarkBytes = 0x0000001b,

    /// Get/set WRED green color marked packets count [uint64_t]
    GreenWredEcnMarkedPackets = 0x0000001c,

    /// Get/set WRED green color marked bytes count [uint64_t]
    GreenWredEcnMarkedBytes = 0x0000001d,

    /// Get/set WRED yellow color marked packets count [uint64_t]
    YellowWredEcnMarkedPackets = 0x0000001e,

    /// Get/set WRED yellow color marked bytes count [uint64_t]
    YellowWredEcnMarkedBytes = 0x0000001f,

    /// Get/set WRED red color marked packets count [uint64_t]
    RedWredEcnMarkedPackets = 0x00000020,

    /// Get/set WRED red color marked bytes count [uint64_t]
    RedWredEcnMarkedBytes = 0x00000021,

    /// Get/set WRED marked packets count [uint64_t]
    WredEcnMarkedPackets = 0x00000022,

    /// Get/set WRED marked bytes count [uint64_t]
    WredEcnMarkedBytes = 0x00000023,

    /// Get current queue occupancy percentage [uint64_t]
    CurrOccupancyLevel = 0x00000024,

    /// Get watermark queue occupancy percentage [uint64_t]
    WatermarkLevel = 0x00000025,

    /// Get packets deleted when the credit watch dog expires for VOQ System [uint64_t]
    CreditWdDeletedPackets = 0x00000026,

    /// Queue delay watermark in nanoseconds [uint64_t]
    DelayWatermarkNs = 0x00000027,

    /// Packets trimmed due to failed admission [uint64_t]
    TrimPackets = 0x00000028,

    /// Get current queue occupancy in cells [uint64_t]
    CurrOccupancyCells = 0x00000029,

    /// Get watermark queue occupancy in cells [uint64_t]
    WatermarkCells = 0x0000002a,

    /// Get current queue shared occupancy in cells [uint64_t]
    SharedCurrOccupancyCells = 0x0000002b,

    /// Get watermark queue shared occupancy in cells [uint64_t]
    SharedWatermarkCells = 0x0000002c,

    /// Packets trimmed but failed to be admitted on a trim queue due to congestion [uint64_t]
    DroppedTrimPackets = 0x0000002d,

    /// Packets trimmed and successfully transmitted on a trim queue [uint64_t]
    TxTrimPackets = 0x0000002e,

    /// Custom range base value
    CustomRangeBase = 0x10000000,
}

impl SaiQueueStat {
    /// Convert from u32 value to enum variant
    pub fn from_u32(value: u32) -> Option<Self> {
        match value {
            0x00000000 => Some(Self::Packets),
            0x00000001 => Some(Self::Bytes),
            0x00000002 => Some(Self::DroppedPackets),
            0x00000003 => Some(Self::DroppedBytes),
            0x00000004 => Some(Self::GreenPackets),
            0x00000005 => Some(Self::GreenBytes),
            0x00000006 => Some(Self::GreenDroppedPackets),
            0x00000007 => Some(Self::GreenDroppedBytes),
            0x00000008 => Some(Self::YellowPackets),
            0x00000009 => Some(Self::YellowBytes),
            0x0000000a => Some(Self::YellowDroppedPackets),
            0x0000000b => Some(Self::YellowDroppedBytes),
            0x0000000c => Some(Self::RedPackets),
            0x0000000d => Some(Self::RedBytes),
            0x0000000e => Some(Self::RedDroppedPackets),
            0x0000000f => Some(Self::RedDroppedBytes),
            0x00000010 => Some(Self::GreenWredDroppedPackets),
            0x00000011 => Some(Self::GreenWredDroppedBytes),
            0x00000012 => Some(Self::YellowWredDroppedPackets),
            0x00000013 => Some(Self::YellowWredDroppedBytes),
            0x00000014 => Some(Self::RedWredDroppedPackets),
            0x00000015 => Some(Self::RedWredDroppedBytes),
            0x00000016 => Some(Self::WredDroppedPackets),
            0x00000017 => Some(Self::WredDroppedBytes),
            0x00000018 => Some(Self::CurrOccupancyBytes),
            0x00000019 => Some(Self::WatermarkBytes),
            0x0000001a => Some(Self::SharedCurrOccupancyBytes),
            0x0000001b => Some(Self::SharedWatermarkBytes),
            0x0000001c => Some(Self::GreenWredEcnMarkedPackets),
            0x0000001d => Some(Self::GreenWredEcnMarkedBytes),
            0x0000001e => Some(Self::YellowWredEcnMarkedPackets),
            0x0000001f => Some(Self::YellowWredEcnMarkedBytes),
            0x00000020 => Some(Self::RedWredEcnMarkedPackets),
            0x00000021 => Some(Self::RedWredEcnMarkedBytes),
            0x00000022 => Some(Self::WredEcnMarkedPackets),
            0x00000023 => Some(Self::WredEcnMarkedBytes),
            0x00000024 => Some(Self::CurrOccupancyLevel),
            0x00000025 => Some(Self::WatermarkLevel),
            0x00000026 => Some(Self::CreditWdDeletedPackets),
            0x00000027 => Some(Self::DelayWatermarkNs),
            0x00000028 => Some(Self::TrimPackets),
            0x00000029 => Some(Self::CurrOccupancyCells),
            0x0000002a => Some(Self::WatermarkCells),
            0x0000002b => Some(Self::SharedCurrOccupancyCells),
            0x0000002c => Some(Self::SharedWatermarkCells),
            0x0000002d => Some(Self::DroppedTrimPackets),
            0x0000002e => Some(Self::TxTrimPackets),
            0x10000000 => Some(Self::CustomRangeBase),
            _ => None,
        }
    }

    /// Convert enum variant to u32 value
    #[allow(dead_code)] // May be used by external code or future features
    pub fn to_u32(self) -> u32 {
        self as u32
    }

    /// Get the C enum name as a string
    pub fn to_c_name(self) -> &'static str {
        match self {
            Self::Packets => "SAI_QUEUE_STAT_PACKETS",
            Self::Bytes => "SAI_QUEUE_STAT_BYTES",
            Self::DroppedPackets => "SAI_QUEUE_STAT_DROPPED_PACKETS",
            Self::DroppedBytes => "SAI_QUEUE_STAT_DROPPED_BYTES",
            Self::GreenPackets => "SAI_QUEUE_STAT_GREEN_PACKETS",
            Self::GreenBytes => "SAI_QUEUE_STAT_GREEN_BYTES",
            Self::GreenDroppedPackets => "SAI_QUEUE_STAT_GREEN_DROPPED_PACKETS",
            Self::GreenDroppedBytes => "SAI_QUEUE_STAT_GREEN_DROPPED_BYTES",
            Self::YellowPackets => "SAI_QUEUE_STAT_YELLOW_PACKETS",
            Self::YellowBytes => "SAI_QUEUE_STAT_YELLOW_BYTES",
            Self::YellowDroppedPackets => "SAI_QUEUE_STAT_YELLOW_DROPPED_PACKETS",
            Self::YellowDroppedBytes => "SAI_QUEUE_STAT_YELLOW_DROPPED_BYTES",
            Self::RedPackets => "SAI_QUEUE_STAT_RED_PACKETS",
            Self::RedBytes => "SAI_QUEUE_STAT_RED_BYTES",
            Self::RedDroppedPackets => "SAI_QUEUE_STAT_RED_DROPPED_PACKETS",
            Self::RedDroppedBytes => "SAI_QUEUE_STAT_RED_DROPPED_BYTES",
            Self::GreenWredDroppedPackets => "SAI_QUEUE_STAT_GREEN_WRED_DROPPED_PACKETS",
            Self::GreenWredDroppedBytes => "SAI_QUEUE_STAT_GREEN_WRED_DROPPED_BYTES",
            Self::YellowWredDroppedPackets => "SAI_QUEUE_STAT_YELLOW_WRED_DROPPED_PACKETS",
            Self::YellowWredDroppedBytes => "SAI_QUEUE_STAT_YELLOW_WRED_DROPPED_BYTES",
            Self::RedWredDroppedPackets => "SAI_QUEUE_STAT_RED_WRED_DROPPED_PACKETS",
            Self::RedWredDroppedBytes => "SAI_QUEUE_STAT_RED_WRED_DROPPED_BYTES",
            Self::WredDroppedPackets => "SAI_QUEUE_STAT_WRED_DROPPED_PACKETS",
            Self::WredDroppedBytes => "SAI_QUEUE_STAT_WRED_DROPPED_BYTES",
            Self::CurrOccupancyBytes => "SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES",
            Self::WatermarkBytes => "SAI_QUEUE_STAT_WATERMARK_BYTES",
            Self::SharedCurrOccupancyBytes => "SAI_QUEUE_STAT_SHARED_CURR_OCCUPANCY_BYTES",
            Self::SharedWatermarkBytes => "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES",
            Self::GreenWredEcnMarkedPackets => "SAI_QUEUE_STAT_GREEN_WRED_ECN_MARKED_PACKETS",
            Self::GreenWredEcnMarkedBytes => "SAI_QUEUE_STAT_GREEN_WRED_ECN_MARKED_BYTES",
            Self::YellowWredEcnMarkedPackets => "SAI_QUEUE_STAT_YELLOW_WRED_ECN_MARKED_PACKETS",
            Self::YellowWredEcnMarkedBytes => "SAI_QUEUE_STAT_YELLOW_WRED_ECN_MARKED_BYTES",
            Self::RedWredEcnMarkedPackets => "SAI_QUEUE_STAT_RED_WRED_ECN_MARKED_PACKETS",
            Self::RedWredEcnMarkedBytes => "SAI_QUEUE_STAT_RED_WRED_ECN_MARKED_BYTES",
            Self::WredEcnMarkedPackets => "SAI_QUEUE_STAT_WRED_ECN_MARKED_PACKETS",
            Self::WredEcnMarkedBytes => "SAI_QUEUE_STAT_WRED_ECN_MARKED_BYTES",
            Self::CurrOccupancyLevel => "SAI_QUEUE_STAT_CURR_OCCUPANCY_LEVEL",
            Self::WatermarkLevel => "SAI_QUEUE_STAT_WATERMARK_LEVEL",
            Self::CreditWdDeletedPackets => "SAI_QUEUE_STAT_CREDIT_WD_DELETED_PACKETS",
            Self::DelayWatermarkNs => "SAI_QUEUE_STAT_DELAY_WATERMARK_NS",
            Self::TrimPackets => "SAI_QUEUE_STAT_TRIM_PACKETS",
            Self::CurrOccupancyCells => "SAI_QUEUE_STAT_CURR_OCCUPANCY_CELLS",
            Self::WatermarkCells => "SAI_QUEUE_STAT_WATERMARK_CELLS",
            Self::SharedCurrOccupancyCells => "SAI_QUEUE_STAT_SHARED_CURR_OCCUPANCY_CELLS",
            Self::SharedWatermarkCells => "SAI_QUEUE_STAT_SHARED_WATERMARK_CELLS",
            Self::DroppedTrimPackets => "SAI_QUEUE_STAT_DROPPED_TRIM_PACKETS",
            Self::TxTrimPackets => "SAI_QUEUE_STAT_TX_TRIM_PACKETS",
            Self::CustomRangeBase => "SAI_QUEUE_STAT_CUSTOM_RANGE_BASE",
        }
    }
}

impl FromStr for SaiQueueStat {
    type Err = ();

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "SAI_QUEUE_STAT_PACKETS" => Ok(Self::Packets),
            "SAI_QUEUE_STAT_BYTES" => Ok(Self::Bytes),
            "SAI_QUEUE_STAT_DROPPED_PACKETS" => Ok(Self::DroppedPackets),
            "SAI_QUEUE_STAT_DROPPED_BYTES" => Ok(Self::DroppedBytes),
            "SAI_QUEUE_STAT_GREEN_PACKETS" => Ok(Self::GreenPackets),
            "SAI_QUEUE_STAT_GREEN_BYTES" => Ok(Self::GreenBytes),
            "SAI_QUEUE_STAT_GREEN_DROPPED_PACKETS" => Ok(Self::GreenDroppedPackets),
            "SAI_QUEUE_STAT_GREEN_DROPPED_BYTES" => Ok(Self::GreenDroppedBytes),
            "SAI_QUEUE_STAT_YELLOW_PACKETS" => Ok(Self::YellowPackets),
            "SAI_QUEUE_STAT_YELLOW_BYTES" => Ok(Self::YellowBytes),
            "SAI_QUEUE_STAT_YELLOW_DROPPED_PACKETS" => Ok(Self::YellowDroppedPackets),
            "SAI_QUEUE_STAT_YELLOW_DROPPED_BYTES" => Ok(Self::YellowDroppedBytes),
            "SAI_QUEUE_STAT_RED_PACKETS" => Ok(Self::RedPackets),
            "SAI_QUEUE_STAT_RED_BYTES" => Ok(Self::RedBytes),
            "SAI_QUEUE_STAT_RED_DROPPED_PACKETS" => Ok(Self::RedDroppedPackets),
            "SAI_QUEUE_STAT_RED_DROPPED_BYTES" => Ok(Self::RedDroppedBytes),
            "SAI_QUEUE_STAT_GREEN_WRED_DROPPED_PACKETS" => Ok(Self::GreenWredDroppedPackets),
            "SAI_QUEUE_STAT_GREEN_WRED_DROPPED_BYTES" => Ok(Self::GreenWredDroppedBytes),
            "SAI_QUEUE_STAT_YELLOW_WRED_DROPPED_PACKETS" => Ok(Self::YellowWredDroppedPackets),
            "SAI_QUEUE_STAT_YELLOW_WRED_DROPPED_BYTES" => Ok(Self::YellowWredDroppedBytes),
            "SAI_QUEUE_STAT_RED_WRED_DROPPED_PACKETS" => Ok(Self::RedWredDroppedPackets),
            "SAI_QUEUE_STAT_RED_WRED_DROPPED_BYTES" => Ok(Self::RedWredDroppedBytes),
            "SAI_QUEUE_STAT_WRED_DROPPED_PACKETS" => Ok(Self::WredDroppedPackets),
            "SAI_QUEUE_STAT_WRED_DROPPED_BYTES" => Ok(Self::WredDroppedBytes),
            "SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES" => Ok(Self::CurrOccupancyBytes),
            "SAI_QUEUE_STAT_WATERMARK_BYTES" => Ok(Self::WatermarkBytes),
            "SAI_QUEUE_STAT_SHARED_CURR_OCCUPANCY_BYTES" => Ok(Self::SharedCurrOccupancyBytes),
            "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES" => Ok(Self::SharedWatermarkBytes),
            "SAI_QUEUE_STAT_GREEN_WRED_ECN_MARKED_PACKETS" => Ok(Self::GreenWredEcnMarkedPackets),
            "SAI_QUEUE_STAT_GREEN_WRED_ECN_MARKED_BYTES" => Ok(Self::GreenWredEcnMarkedBytes),
            "SAI_QUEUE_STAT_YELLOW_WRED_ECN_MARKED_PACKETS" => Ok(Self::YellowWredEcnMarkedPackets),
            "SAI_QUEUE_STAT_YELLOW_WRED_ECN_MARKED_BYTES" => Ok(Self::YellowWredEcnMarkedBytes),
            "SAI_QUEUE_STAT_RED_WRED_ECN_MARKED_PACKETS" => Ok(Self::RedWredEcnMarkedPackets),
            "SAI_QUEUE_STAT_RED_WRED_ECN_MARKED_BYTES" => Ok(Self::RedWredEcnMarkedBytes),
            "SAI_QUEUE_STAT_WRED_ECN_MARKED_PACKETS" => Ok(Self::WredEcnMarkedPackets),
            "SAI_QUEUE_STAT_WRED_ECN_MARKED_BYTES" => Ok(Self::WredEcnMarkedBytes),
            "SAI_QUEUE_STAT_CURR_OCCUPANCY_LEVEL" => Ok(Self::CurrOccupancyLevel),
            "SAI_QUEUE_STAT_WATERMARK_LEVEL" => Ok(Self::WatermarkLevel),
            "SAI_QUEUE_STAT_CREDIT_WD_DELETED_PACKETS" => Ok(Self::CreditWdDeletedPackets),
            "SAI_QUEUE_STAT_DELAY_WATERMARK_NS" => Ok(Self::DelayWatermarkNs),
            "SAI_QUEUE_STAT_TRIM_PACKETS" => Ok(Self::TrimPackets),
            "SAI_QUEUE_STAT_CURR_OCCUPANCY_CELLS" => Ok(Self::CurrOccupancyCells),
            "SAI_QUEUE_STAT_WATERMARK_CELLS" => Ok(Self::WatermarkCells),
            "SAI_QUEUE_STAT_SHARED_CURR_OCCUPANCY_CELLS" => Ok(Self::SharedCurrOccupancyCells),
            "SAI_QUEUE_STAT_SHARED_WATERMARK_CELLS" => Ok(Self::SharedWatermarkCells),
            "SAI_QUEUE_STAT_DROPPED_TRIM_PACKETS" => Ok(Self::DroppedTrimPackets),
            "SAI_QUEUE_STAT_TX_TRIM_PACKETS" => Ok(Self::TxTrimPackets),
            "SAI_QUEUE_STAT_CUSTOM_RANGE_BASE" => Ok(Self::CustomRangeBase),
            _ => Err(()),
        }
    }
}

impl fmt::Display for SaiQueueStat {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.to_c_name())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_from_u32() {
        assert_eq!(
            SaiQueueStat::from_u32(0x00000000),
            Some(SaiQueueStat::Packets)
        );
        assert_eq!(
            SaiQueueStat::from_u32(0x00000001),
            Some(SaiQueueStat::Bytes)
        );
        assert_eq!(
            SaiQueueStat::from_u32(0x00000002),
            Some(SaiQueueStat::DroppedPackets)
        );
        assert_eq!(
            SaiQueueStat::from_u32(0x0000002e),
            Some(SaiQueueStat::TxTrimPackets)
        );
        assert_eq!(
            SaiQueueStat::from_u32(0x10000000),
            Some(SaiQueueStat::CustomRangeBase)
        );
        assert_eq!(SaiQueueStat::from_u32(0xFFFFFFFF), None);
    }

    #[test]
    fn test_to_u32() {
        assert_eq!(SaiQueueStat::Packets.to_u32(), 0x00000000);
        assert_eq!(SaiQueueStat::Bytes.to_u32(), 0x00000001);
        assert_eq!(SaiQueueStat::DroppedPackets.to_u32(), 0x00000002);
        assert_eq!(SaiQueueStat::TxTrimPackets.to_u32(), 0x0000002e);
        assert_eq!(SaiQueueStat::CustomRangeBase.to_u32(), 0x10000000);
    }

    #[test]
    fn test_string_conversion() {
        let stat = SaiQueueStat::CurrOccupancyBytes;
        let c_name = stat.to_c_name();
        assert_eq!(c_name, "SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES");

        let parsed: SaiQueueStat = c_name.parse().unwrap();
        assert_eq!(parsed, stat);

        assert_eq!(format!("{}", stat), c_name);
    }

    #[test]
    fn test_color_based_stats() {
        // Test green color stats
        assert_eq!(SaiQueueStat::GreenPackets.to_u32(), 0x00000004);
        assert_eq!(SaiQueueStat::GreenBytes.to_u32(), 0x00000005);
        assert_eq!(SaiQueueStat::GreenDroppedPackets.to_u32(), 0x00000006);

        // Test yellow color stats
        assert_eq!(SaiQueueStat::YellowPackets.to_u32(), 0x00000008);
        assert_eq!(SaiQueueStat::YellowDroppedBytes.to_u32(), 0x0000000b);

        // Test red color stats
        assert_eq!(SaiQueueStat::RedPackets.to_u32(), 0x0000000c);
        assert_eq!(SaiQueueStat::RedDroppedBytes.to_u32(), 0x0000000f);
    }

    #[test]
    fn test_wred_stats() {
        // Test WRED drop stats
        assert_eq!(SaiQueueStat::GreenWredDroppedPackets.to_u32(), 0x00000010);
        assert_eq!(SaiQueueStat::YellowWredDroppedBytes.to_u32(), 0x00000013);
        assert_eq!(SaiQueueStat::RedWredDroppedPackets.to_u32(), 0x00000014);
        assert_eq!(SaiQueueStat::WredDroppedBytes.to_u32(), 0x00000017);

        // Test WRED ECN mark stats
        assert_eq!(SaiQueueStat::GreenWredEcnMarkedPackets.to_u32(), 0x0000001c);
        assert_eq!(SaiQueueStat::WredEcnMarkedBytes.to_u32(), 0x00000023);
    }

    #[test]
    fn test_occupancy_stats() {
        // Test byte-based occupancy stats
        assert_eq!(SaiQueueStat::CurrOccupancyBytes.to_u32(), 0x00000018);
        assert_eq!(SaiQueueStat::WatermarkBytes.to_u32(), 0x00000019);
        assert_eq!(SaiQueueStat::SharedCurrOccupancyBytes.to_u32(), 0x0000001a);
        assert_eq!(SaiQueueStat::SharedWatermarkBytes.to_u32(), 0x0000001b);

        // Test cell-based occupancy stats
        assert_eq!(SaiQueueStat::CurrOccupancyCells.to_u32(), 0x00000029);
        assert_eq!(SaiQueueStat::WatermarkCells.to_u32(), 0x0000002a);
        assert_eq!(SaiQueueStat::SharedCurrOccupancyCells.to_u32(), 0x0000002b);
        assert_eq!(SaiQueueStat::SharedWatermarkCells.to_u32(), 0x0000002c);

        // Test occupancy level stats
        assert_eq!(SaiQueueStat::CurrOccupancyLevel.to_u32(), 0x00000024);
        assert_eq!(SaiQueueStat::WatermarkLevel.to_u32(), 0x00000025);
    }

    #[test]
    fn test_special_stats() {
        // Test specialized queue statistics
        assert_eq!(SaiQueueStat::CreditWdDeletedPackets.to_u32(), 0x00000026);
        assert_eq!(SaiQueueStat::DelayWatermarkNs.to_u32(), 0x00000027);
        assert_eq!(SaiQueueStat::TrimPackets.to_u32(), 0x00000028);
        assert_eq!(SaiQueueStat::DroppedTrimPackets.to_u32(), 0x0000002d);
        assert_eq!(SaiQueueStat::TxTrimPackets.to_u32(), 0x0000002e);
    }
}
