use std::fmt;
use std::str::FromStr;

/// SAI buffer pool statistics enum
/// This enum represents all the buffer pool statistics defined in sai_buffer_pool_stat_t
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum SaiBufferPoolStat {
    /// Get current pool occupancy in bytes [uint64_t]
    CurrOccupancyBytes = 0x00000000,

    /// Get watermark pool occupancy in bytes [uint64_t]
    WatermarkBytes = 0x00000001,

    /// Get count of packets dropped in this pool [uint64_t]
    DroppedPackets = 0x00000002,

    /// Get/set WRED green dropped packet count [uint64_t]
    GreenWredDroppedPackets = 0x00000003,

    /// Get/set WRED green dropped byte count [uint64_t]
    GreenWredDroppedBytes = 0x00000004,

    /// Get/set WRED yellow dropped packet count [uint64_t]
    YellowWredDroppedPackets = 0x00000005,

    /// Get/set WRED yellow dropped byte count [uint64_t]
    YellowWredDroppedBytes = 0x00000006,

    /// Get/set WRED red dropped packet count [uint64_t]
    RedWredDroppedPackets = 0x00000007,

    /// Get/set WRED red dropped byte count [uint64_t]
    RedWredDroppedBytes = 0x00000008,

    /// Get/set WRED dropped packets count [uint64_t]
    WredDroppedPackets = 0x00000009,

    /// Get/set WRED dropped bytes count [uint64_t]
    WredDroppedBytes = 0x0000000a,

    /// Get/set WRED green marked packet count [uint64_t]
    GreenWredEcnMarkedPackets = 0x0000000b,

    /// Get/set WRED green marked byte count [uint64_t]
    GreenWredEcnMarkedBytes = 0x0000000c,

    /// Get/set WRED yellow marked packet count [uint64_t]
    YellowWredEcnMarkedPackets = 0x0000000d,

    /// Get/set WRED yellow marked byte count [uint64_t]
    YellowWredEcnMarkedBytes = 0x0000000e,

    /// Get/set WRED red marked packet count [uint64_t]
    RedWredEcnMarkedPackets = 0x0000000f,

    /// Get/set WRED red marked byte count [uint64_t]
    RedWredEcnMarkedBytes = 0x00000010,

    /// Get/set WRED marked packets count [uint64_t]
    WredEcnMarkedPackets = 0x00000011,

    /// Get/set WRED marked bytes count [uint64_t]
    WredEcnMarkedBytes = 0x00000012,

    /// Get current headroom pool occupancy in bytes [uint64_t]
    XoffRoomCurrOccupancyBytes = 0x00000013,

    /// Get headroom pool occupancy in bytes [uint64_t]
    XoffRoomWatermarkBytes = 0x00000014,

    /// Get current headroom pool occupancy in cells [uint64_t]
    XoffRoomCurrOccupancyCells = 0x00000015,

    /// Get headroom pool occupancy in cells [uint64_t]
    XoffRoomWatermarkCells = 0x00000016,

    /// Get current pool occupancy in cells [uint64_t]
    CurrOccupancyCells = 0x00000017,

    /// Get watermark pool occupancy in cells [uint64_t]
    WatermarkCells = 0x00000018,

    /// Custom range base value
    CustomRangeBase = 0x10000000,
}

impl SaiBufferPoolStat {
    /// Convert from u32 value to enum variant
    pub fn from_u32(value: u32) -> Option<Self> {
        match value {
            0x00000000 => Some(Self::CurrOccupancyBytes),
            0x00000001 => Some(Self::WatermarkBytes),
            0x00000002 => Some(Self::DroppedPackets),
            0x00000003 => Some(Self::GreenWredDroppedPackets),
            0x00000004 => Some(Self::GreenWredDroppedBytes),
            0x00000005 => Some(Self::YellowWredDroppedPackets),
            0x00000006 => Some(Self::YellowWredDroppedBytes),
            0x00000007 => Some(Self::RedWredDroppedPackets),
            0x00000008 => Some(Self::RedWredDroppedBytes),
            0x00000009 => Some(Self::WredDroppedPackets),
            0x0000000a => Some(Self::WredDroppedBytes),
            0x0000000b => Some(Self::GreenWredEcnMarkedPackets),
            0x0000000c => Some(Self::GreenWredEcnMarkedBytes),
            0x0000000d => Some(Self::YellowWredEcnMarkedPackets),
            0x0000000e => Some(Self::YellowWredEcnMarkedBytes),
            0x0000000f => Some(Self::RedWredEcnMarkedPackets),
            0x00000010 => Some(Self::RedWredEcnMarkedBytes),
            0x00000011 => Some(Self::WredEcnMarkedPackets),
            0x00000012 => Some(Self::WredEcnMarkedBytes),
            0x00000013 => Some(Self::XoffRoomCurrOccupancyBytes),
            0x00000014 => Some(Self::XoffRoomWatermarkBytes),
            0x00000015 => Some(Self::XoffRoomCurrOccupancyCells),
            0x00000016 => Some(Self::XoffRoomWatermarkCells),
            0x00000017 => Some(Self::CurrOccupancyCells),
            0x00000018 => Some(Self::WatermarkCells),
            0x10000000 => Some(Self::CustomRangeBase),
            _ => None,
        }
    }

    /// Convert to u32 value
    #[allow(dead_code)] // May be used by external code or future features
    pub fn to_u32(self) -> u32 {
        self as u32
    }

    /// Get the C name of this stat
    pub fn to_c_name(self) -> &'static str {
        match self {
            Self::CurrOccupancyBytes => "SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_BYTES",
            Self::WatermarkBytes => "SAI_BUFFER_POOL_STAT_WATERMARK_BYTES",
            Self::DroppedPackets => "SAI_BUFFER_POOL_STAT_DROPPED_PACKETS",
            Self::GreenWredDroppedPackets => "SAI_BUFFER_POOL_STAT_GREEN_WRED_DROPPED_PACKETS",
            Self::GreenWredDroppedBytes => "SAI_BUFFER_POOL_STAT_GREEN_WRED_DROPPED_BYTES",
            Self::YellowWredDroppedPackets => "SAI_BUFFER_POOL_STAT_YELLOW_WRED_DROPPED_PACKETS",
            Self::YellowWredDroppedBytes => "SAI_BUFFER_POOL_STAT_YELLOW_WRED_DROPPED_BYTES",
            Self::RedWredDroppedPackets => "SAI_BUFFER_POOL_STAT_RED_WRED_DROPPED_PACKETS",
            Self::RedWredDroppedBytes => "SAI_BUFFER_POOL_STAT_RED_WRED_DROPPED_BYTES",
            Self::WredDroppedPackets => "SAI_BUFFER_POOL_STAT_WRED_DROPPED_PACKETS",
            Self::WredDroppedBytes => "SAI_BUFFER_POOL_STAT_WRED_DROPPED_BYTES",
            Self::GreenWredEcnMarkedPackets => "SAI_BUFFER_POOL_STAT_GREEN_WRED_ECN_MARKED_PACKETS",
            Self::GreenWredEcnMarkedBytes => "SAI_BUFFER_POOL_STAT_GREEN_WRED_ECN_MARKED_BYTES",
            Self::YellowWredEcnMarkedPackets => {
                "SAI_BUFFER_POOL_STAT_YELLOW_WRED_ECN_MARKED_PACKETS"
            }
            Self::YellowWredEcnMarkedBytes => "SAI_BUFFER_POOL_STAT_YELLOW_WRED_ECN_MARKED_BYTES",
            Self::RedWredEcnMarkedPackets => "SAI_BUFFER_POOL_STAT_RED_WRED_ECN_MARKED_PACKETS",
            Self::RedWredEcnMarkedBytes => "SAI_BUFFER_POOL_STAT_RED_WRED_ECN_MARKED_BYTES",
            Self::WredEcnMarkedPackets => "SAI_BUFFER_POOL_STAT_WRED_ECN_MARKED_PACKETS",
            Self::WredEcnMarkedBytes => "SAI_BUFFER_POOL_STAT_WRED_ECN_MARKED_BYTES",
            Self::XoffRoomCurrOccupancyBytes => {
                "SAI_BUFFER_POOL_STAT_XOFF_ROOM_CURR_OCCUPANCY_BYTES"
            }
            Self::XoffRoomWatermarkBytes => "SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES",
            Self::XoffRoomCurrOccupancyCells => {
                "SAI_BUFFER_POOL_STAT_XOFF_ROOM_CURR_OCCUPANCY_CELLS"
            }
            Self::XoffRoomWatermarkCells => "SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_CELLS",
            Self::CurrOccupancyCells => "SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_CELLS",
            Self::WatermarkCells => "SAI_BUFFER_POOL_STAT_WATERMARK_CELLS",
            Self::CustomRangeBase => "SAI_BUFFER_POOL_STAT_CUSTOM_RANGE_BASE",
        }
    }
}

impl FromStr for SaiBufferPoolStat {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_BYTES" => Ok(Self::CurrOccupancyBytes),
            "SAI_BUFFER_POOL_STAT_WATERMARK_BYTES" => Ok(Self::WatermarkBytes),
            "SAI_BUFFER_POOL_STAT_DROPPED_PACKETS" => Ok(Self::DroppedPackets),
            "SAI_BUFFER_POOL_STAT_GREEN_WRED_DROPPED_PACKETS" => Ok(Self::GreenWredDroppedPackets),
            "SAI_BUFFER_POOL_STAT_GREEN_WRED_DROPPED_BYTES" => Ok(Self::GreenWredDroppedBytes),
            "SAI_BUFFER_POOL_STAT_YELLOW_WRED_DROPPED_PACKETS" => {
                Ok(Self::YellowWredDroppedPackets)
            }
            "SAI_BUFFER_POOL_STAT_YELLOW_WRED_DROPPED_BYTES" => Ok(Self::YellowWredDroppedBytes),
            "SAI_BUFFER_POOL_STAT_RED_WRED_DROPPED_PACKETS" => Ok(Self::RedWredDroppedPackets),
            "SAI_BUFFER_POOL_STAT_RED_WRED_DROPPED_BYTES" => Ok(Self::RedWredDroppedBytes),
            "SAI_BUFFER_POOL_STAT_WRED_DROPPED_PACKETS" => Ok(Self::WredDroppedPackets),
            "SAI_BUFFER_POOL_STAT_WRED_DROPPED_BYTES" => Ok(Self::WredDroppedBytes),
            "SAI_BUFFER_POOL_STAT_GREEN_WRED_ECN_MARKED_PACKETS" => {
                Ok(Self::GreenWredEcnMarkedPackets)
            }
            "SAI_BUFFER_POOL_STAT_GREEN_WRED_ECN_MARKED_BYTES" => Ok(Self::GreenWredEcnMarkedBytes),
            "SAI_BUFFER_POOL_STAT_YELLOW_WRED_ECN_MARKED_PACKETS" => {
                Ok(Self::YellowWredEcnMarkedPackets)
            }
            "SAI_BUFFER_POOL_STAT_YELLOW_WRED_ECN_MARKED_BYTES" => {
                Ok(Self::YellowWredEcnMarkedBytes)
            }
            "SAI_BUFFER_POOL_STAT_RED_WRED_ECN_MARKED_PACKETS" => Ok(Self::RedWredEcnMarkedPackets),
            "SAI_BUFFER_POOL_STAT_RED_WRED_ECN_MARKED_BYTES" => Ok(Self::RedWredEcnMarkedBytes),
            "SAI_BUFFER_POOL_STAT_WRED_ECN_MARKED_PACKETS" => Ok(Self::WredEcnMarkedPackets),
            "SAI_BUFFER_POOL_STAT_WRED_ECN_MARKED_BYTES" => Ok(Self::WredEcnMarkedBytes),
            "SAI_BUFFER_POOL_STAT_XOFF_ROOM_CURR_OCCUPANCY_BYTES" => {
                Ok(Self::XoffRoomCurrOccupancyBytes)
            }
            "SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES" => Ok(Self::XoffRoomWatermarkBytes),
            "SAI_BUFFER_POOL_STAT_XOFF_ROOM_CURR_OCCUPANCY_CELLS" => {
                Ok(Self::XoffRoomCurrOccupancyCells)
            }
            "SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_CELLS" => Ok(Self::XoffRoomWatermarkCells),
            "SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_CELLS" => Ok(Self::CurrOccupancyCells),
            "SAI_BUFFER_POOL_STAT_WATERMARK_CELLS" => Ok(Self::WatermarkCells),
            "SAI_BUFFER_POOL_STAT_CUSTOM_RANGE_BASE" => Ok(Self::CustomRangeBase),
            _ => Err(format!("Unknown buffer pool stat: {}", s)),
        }
    }
}

impl fmt::Display for SaiBufferPoolStat {
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
            SaiBufferPoolStat::from_u32(0x00000000),
            Some(SaiBufferPoolStat::CurrOccupancyBytes)
        );
        assert_eq!(
            SaiBufferPoolStat::from_u32(0x00000001),
            Some(SaiBufferPoolStat::WatermarkBytes)
        );
        assert_eq!(
            SaiBufferPoolStat::from_u32(0x00000018),
            Some(SaiBufferPoolStat::WatermarkCells)
        );
        assert_eq!(
            SaiBufferPoolStat::from_u32(0x10000000),
            Some(SaiBufferPoolStat::CustomRangeBase)
        );
        assert_eq!(SaiBufferPoolStat::from_u32(0xFFFFFFFF), None);
    }

    #[test]
    fn test_to_u32() {
        assert_eq!(SaiBufferPoolStat::CurrOccupancyBytes.to_u32(), 0x00000000);
        assert_eq!(SaiBufferPoolStat::WatermarkBytes.to_u32(), 0x00000001);
        assert_eq!(SaiBufferPoolStat::WatermarkCells.to_u32(), 0x00000018);
        assert_eq!(SaiBufferPoolStat::CustomRangeBase.to_u32(), 0x10000000);
    }

    #[test]
    fn test_string_conversion() {
        let stat = SaiBufferPoolStat::CurrOccupancyBytes;
        let c_name = stat.to_c_name();
        assert_eq!(c_name, "SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_BYTES");

        let parsed: SaiBufferPoolStat = c_name.parse().unwrap();
        assert_eq!(parsed, stat);

        assert_eq!(format!("{}", stat), c_name);
    }

    #[test]
    fn test_wred_stats() {
        // Test WRED drop stats
        assert_eq!(
            SaiBufferPoolStat::GreenWredDroppedPackets.to_u32(),
            0x00000003
        );
        assert_eq!(
            SaiBufferPoolStat::YellowWredDroppedBytes.to_u32(),
            0x00000006
        );
        assert_eq!(
            SaiBufferPoolStat::RedWredDroppedPackets.to_u32(),
            0x00000007
        );

        // Test WRED ECN mark stats
        assert_eq!(
            SaiBufferPoolStat::GreenWredEcnMarkedPackets.to_u32(),
            0x0000000b
        );
        assert_eq!(SaiBufferPoolStat::WredEcnMarkedBytes.to_u32(), 0x00000012);
    }

    #[test]
    fn test_xoff_room_stats() {
        assert_eq!(
            SaiBufferPoolStat::XoffRoomCurrOccupancyBytes.to_u32(),
            0x00000013
        );
        assert_eq!(
            SaiBufferPoolStat::XoffRoomWatermarkCells.to_u32(),
            0x00000016
        );
    }
}

/// SAI ingress priority group statistics enum
/// This enum represents all the ingress priority group statistics defined in sai_ingress_priority_group_stat_t
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum SaiIngressPriorityGroupStat {
    /// Get rx packets count [uint64_t]
    Packets = 0x00000000,

    /// Get rx bytes count [uint64_t]
    Bytes = 0x00000001,

    /// Get current pg occupancy in bytes [uint64_t]
    CurrOccupancyBytes = 0x00000002,

    /// Get watermark pg occupancy in bytes [uint64_t]
    WatermarkBytes = 0x00000003,

    /// Get current pg shared occupancy in bytes [uint64_t]
    SharedCurrOccupancyBytes = 0x00000004,

    /// Get watermark pg shared occupancy in bytes [uint64_t]
    SharedWatermarkBytes = 0x00000005,

    /// Get current pg XOFF room occupancy in bytes [uint64_t]
    XoffRoomCurrOccupancyBytes = 0x00000006,

    /// Get watermark pg XOFF room occupancy in bytes [uint64_t]
    XoffRoomWatermarkBytes = 0x00000007,

    /// Get dropped packets count [uint64_t]
    DroppedPackets = 0x00000008,

    /// Get current pg occupancy in cells [uint64_t]
    CurrOccupancyCells = 0x00000009,

    /// Get watermark pg occupancy in cells [uint64_t]
    WatermarkCells = 0x0000000a,

    /// Get current pg shared occupancy in cells [uint64_t]
    SharedCurrOccupancyCells = 0x0000000b,

    /// Get watermark pg shared occupancy in cells [uint64_t]
    SharedWatermarkCells = 0x0000000c,

    /// Get current pg XOFF room occupancy in cells [uint64_t]
    XoffRoomCurrOccupancyCells = 0x0000000d,

    /// Get watermark pg XOFF room occupancy in cells [uint64_t]
    XoffRoomWatermarkCells = 0x0000000e,

    /// Custom range base value
    CustomRangeBase = 0x10000000,
}

impl SaiIngressPriorityGroupStat {
    /// Convert from u32 value to enum variant
    pub fn from_u32(value: u32) -> Option<Self> {
        match value {
            0x00000000 => Some(Self::Packets),
            0x00000001 => Some(Self::Bytes),
            0x00000002 => Some(Self::CurrOccupancyBytes),
            0x00000003 => Some(Self::WatermarkBytes),
            0x00000004 => Some(Self::SharedCurrOccupancyBytes),
            0x00000005 => Some(Self::SharedWatermarkBytes),
            0x00000006 => Some(Self::XoffRoomCurrOccupancyBytes),
            0x00000007 => Some(Self::XoffRoomWatermarkBytes),
            0x00000008 => Some(Self::DroppedPackets),
            0x00000009 => Some(Self::CurrOccupancyCells),
            0x0000000a => Some(Self::WatermarkCells),
            0x0000000b => Some(Self::SharedCurrOccupancyCells),
            0x0000000c => Some(Self::SharedWatermarkCells),
            0x0000000d => Some(Self::XoffRoomCurrOccupancyCells),
            0x0000000e => Some(Self::XoffRoomWatermarkCells),
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
            Self::Packets => "SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS",
            Self::Bytes => "SAI_INGRESS_PRIORITY_GROUP_STAT_BYTES",
            Self::CurrOccupancyBytes => "SAI_INGRESS_PRIORITY_GROUP_STAT_CURR_OCCUPANCY_BYTES",
            Self::WatermarkBytes => "SAI_INGRESS_PRIORITY_GROUP_STAT_WATERMARK_BYTES",
            Self::SharedCurrOccupancyBytes => {
                "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_CURR_OCCUPANCY_BYTES"
            }
            Self::SharedWatermarkBytes => "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES",
            Self::XoffRoomCurrOccupancyBytes => {
                "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_CURR_OCCUPANCY_BYTES"
            }
            Self::XoffRoomWatermarkBytes => {
                "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES"
            }
            Self::DroppedPackets => "SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS",
            Self::CurrOccupancyCells => "SAI_INGRESS_PRIORITY_GROUP_STAT_CURR_OCCUPANCY_CELLS",
            Self::WatermarkCells => "SAI_INGRESS_PRIORITY_GROUP_STAT_WATERMARK_CELLS",
            Self::SharedCurrOccupancyCells => {
                "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_CURR_OCCUPANCY_CELLS"
            }
            Self::SharedWatermarkCells => "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_CELLS",
            Self::XoffRoomCurrOccupancyCells => {
                "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_CURR_OCCUPANCY_CELLS"
            }
            Self::XoffRoomWatermarkCells => {
                "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_CELLS"
            }
            Self::CustomRangeBase => "SAI_INGRESS_PRIORITY_GROUP_STAT_CUSTOM_RANGE_BASE",
        }
    }
}

impl FromStr for SaiIngressPriorityGroupStat {
    type Err = ();

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS" => Ok(Self::Packets),
            "SAI_INGRESS_PRIORITY_GROUP_STAT_BYTES" => Ok(Self::Bytes),
            "SAI_INGRESS_PRIORITY_GROUP_STAT_CURR_OCCUPANCY_BYTES" => Ok(Self::CurrOccupancyBytes),
            "SAI_INGRESS_PRIORITY_GROUP_STAT_WATERMARK_BYTES" => Ok(Self::WatermarkBytes),
            "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_CURR_OCCUPANCY_BYTES" => {
                Ok(Self::SharedCurrOccupancyBytes)
            }
            "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES" => {
                Ok(Self::SharedWatermarkBytes)
            }
            "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_CURR_OCCUPANCY_BYTES" => {
                Ok(Self::XoffRoomCurrOccupancyBytes)
            }
            "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES" => {
                Ok(Self::XoffRoomWatermarkBytes)
            }
            "SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS" => Ok(Self::DroppedPackets),
            "SAI_INGRESS_PRIORITY_GROUP_STAT_CURR_OCCUPANCY_CELLS" => Ok(Self::CurrOccupancyCells),
            "SAI_INGRESS_PRIORITY_GROUP_STAT_WATERMARK_CELLS" => Ok(Self::WatermarkCells),
            "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_CURR_OCCUPANCY_CELLS" => {
                Ok(Self::SharedCurrOccupancyCells)
            }
            "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_CELLS" => {
                Ok(Self::SharedWatermarkCells)
            }
            "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_CURR_OCCUPANCY_CELLS" => {
                Ok(Self::XoffRoomCurrOccupancyCells)
            }
            "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_CELLS" => {
                Ok(Self::XoffRoomWatermarkCells)
            }
            "SAI_INGRESS_PRIORITY_GROUP_STAT_CUSTOM_RANGE_BASE" => Ok(Self::CustomRangeBase),
            _ => Err(()),
        }
    }
}

impl fmt::Display for SaiIngressPriorityGroupStat {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.to_c_name())
    }
}

#[cfg(test)]
mod ingress_priority_group_tests {
    use super::*;

    #[test]
    fn test_ipg_from_u32() {
        assert_eq!(
            SaiIngressPriorityGroupStat::from_u32(0x00000000),
            Some(SaiIngressPriorityGroupStat::Packets)
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::from_u32(0x00000001),
            Some(SaiIngressPriorityGroupStat::Bytes)
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::from_u32(0x00000008),
            Some(SaiIngressPriorityGroupStat::DroppedPackets)
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::from_u32(0x0000000e),
            Some(SaiIngressPriorityGroupStat::XoffRoomWatermarkCells)
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::from_u32(0x10000000),
            Some(SaiIngressPriorityGroupStat::CustomRangeBase)
        );
        assert_eq!(SaiIngressPriorityGroupStat::from_u32(0xFFFFFFFF), None);
    }

    #[test]
    fn test_ipg_to_u32() {
        assert_eq!(SaiIngressPriorityGroupStat::Packets.to_u32(), 0x00000000);
        assert_eq!(SaiIngressPriorityGroupStat::Bytes.to_u32(), 0x00000001);
        assert_eq!(
            SaiIngressPriorityGroupStat::DroppedPackets.to_u32(),
            0x00000008
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::XoffRoomWatermarkCells.to_u32(),
            0x0000000e
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::CustomRangeBase.to_u32(),
            0x10000000
        );
    }

    #[test]
    fn test_ipg_string_conversion() {
        let stat = SaiIngressPriorityGroupStat::CurrOccupancyBytes;
        let c_name = stat.to_c_name();
        assert_eq!(
            c_name,
            "SAI_INGRESS_PRIORITY_GROUP_STAT_CURR_OCCUPANCY_BYTES"
        );

        let parsed: SaiIngressPriorityGroupStat = c_name.parse().unwrap();
        assert_eq!(parsed, stat);

        assert_eq!(format!("{}", stat), c_name);
    }

    #[test]
    fn test_ipg_occupancy_stats() {
        // Test byte-based occupancy stats
        assert_eq!(
            SaiIngressPriorityGroupStat::CurrOccupancyBytes.to_u32(),
            0x00000002
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::WatermarkBytes.to_u32(),
            0x00000003
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::SharedCurrOccupancyBytes.to_u32(),
            0x00000004
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::SharedWatermarkBytes.to_u32(),
            0x00000005
        );

        // Test cell-based occupancy stats
        assert_eq!(
            SaiIngressPriorityGroupStat::CurrOccupancyCells.to_u32(),
            0x00000009
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::WatermarkCells.to_u32(),
            0x0000000a
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::SharedCurrOccupancyCells.to_u32(),
            0x0000000b
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::SharedWatermarkCells.to_u32(),
            0x0000000c
        );
    }

    #[test]
    fn test_ipg_xoff_room_stats() {
        // Test XOFF room byte stats
        assert_eq!(
            SaiIngressPriorityGroupStat::XoffRoomCurrOccupancyBytes.to_u32(),
            0x00000006
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::XoffRoomWatermarkBytes.to_u32(),
            0x00000007
        );

        // Test XOFF room cell stats
        assert_eq!(
            SaiIngressPriorityGroupStat::XoffRoomCurrOccupancyCells.to_u32(),
            0x0000000d
        );
        assert_eq!(
            SaiIngressPriorityGroupStat::XoffRoomWatermarkCells.to_u32(),
            0x0000000e
        );
    }
}
