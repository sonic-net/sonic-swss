use std::fmt;
use std::str::FromStr;

/// SAI port statistics enum
/// This enum represents all the port statistics defined in sai_port_stat_t
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u32)]
pub enum SaiPortStat {
    // SAI port stat range start / SAI port stat if in octets (same value in C)
    IfInOctets = 0,

    // Following the exact C enum order
    IfInUcastPkts = 1,
    IfInNonUcastPkts = 2,
    IfInDiscards = 3,
    IfInErrors = 4,
    IfInUnknownProtos = 5,
    IfInBroadcastPkts = 6,
    IfInMulticastPkts = 7,
    IfInVlanDiscards = 8,
    IfOutOctets = 9,
    IfOutUcastPkts = 10,
    IfOutNonUcastPkts = 11,
    IfOutDiscards = 12,
    IfOutErrors = 13,
    IfOutQlen = 14,
    IfOutBroadcastPkts = 15,
    IfOutMulticastPkts = 16,
    EtherStatsDropEvents = 17,
    EtherStatsMulticastPkts = 18,
    EtherStatsBroadcastPkts = 19,
    EtherStatsUndersizePkts = 20,
    EtherStatsFragments = 21,
    EtherStatsPkts64Octets = 22,
    EtherStatsPkts65To127Octets = 23,
    EtherStatsPkts128To255Octets = 24,
    EtherStatsPkts256To511Octets = 25,
    EtherStatsPkts512To1023Octets = 26,
    EtherStatsPkts1024To1518Octets = 27,
    EtherStatsPkts1519To2047Octets = 28,
    EtherStatsPkts2048To4095Octets = 29,
    EtherStatsPkts4096To9216Octets = 30,
    EtherStatsPkts9217To16383Octets = 31,
    EtherStatsOversizePkts = 32,
    EtherRxOversizePkts = 33,
    EtherTxOversizePkts = 34,
    EtherStatsJabbers = 35,
    EtherStatsOctets = 36,
    EtherStatsPkts = 37,
    EtherStatsCollisions = 38,
    EtherStatsCrcAlignErrors = 39,
    EtherStatsTxNoErrors = 40,
    EtherStatsRxNoErrors = 41,
    IpInReceives = 42,
    IpInOctets = 43,
    IpInUcastPkts = 44,
    IpInNonUcastPkts = 45,
    IpInDiscards = 46,
    IpOutOctets = 47,
    IpOutUcastPkts = 48,
    IpOutNonUcastPkts = 49,
    IpOutDiscards = 50,
    Ipv6InReceives = 51,
    Ipv6InOctets = 52,
    Ipv6InUcastPkts = 53,
    Ipv6InNonUcastPkts = 54,
    Ipv6InMcastPkts = 55,
    Ipv6InDiscards = 56,
    Ipv6OutOctets = 57,
    Ipv6OutUcastPkts = 58,
    Ipv6OutNonUcastPkts = 59,
    Ipv6OutMcastPkts = 60,
    Ipv6OutDiscards = 61,
    GreenWredDroppedPackets = 62,
    GreenWredDroppedBytes = 63,
    YellowWredDroppedPackets = 64,
    YellowWredDroppedBytes = 65,
    RedWredDroppedPackets = 66,
    RedWredDroppedBytes = 67,
    WredDroppedPackets = 68,
    WredDroppedBytes = 69,
    EcnMarkedPackets = 70,

    // Packet size based packets count (continuing exact C enum order)
    EtherInPkts64Octets = 71,
    EtherInPkts65To127Octets = 72,
    EtherInPkts128To255Octets = 73,
    EtherInPkts256To511Octets = 74,
    EtherInPkts512To1023Octets = 75,
    EtherInPkts1024To1518Octets = 76,
    EtherInPkts1519To2047Octets = 77,
    EtherInPkts2048To4095Octets = 78,
    EtherInPkts4096To9216Octets = 79,
    EtherInPkts9217To16383Octets = 80,
    EtherOutPkts64Octets = 81,
    EtherOutPkts65To127Octets = 82,
    EtherOutPkts128To255Octets = 83,
    EtherOutPkts256To511Octets = 84,
    EtherOutPkts512To1023Octets = 85,
    EtherOutPkts1024To1518Octets = 86,
    EtherOutPkts1519To2047Octets = 87,
    EtherOutPkts2048To4095Octets = 88,
    EtherOutPkts4096To9216Octets = 89,
    EtherOutPkts9217To16383Octets = 90,

    // Port occupancy statistics
    InCurrOccupancyBytes = 91,
    InWatermarkBytes = 92,
    InSharedCurrOccupancyBytes = 93,
    InSharedWatermarkBytes = 94,
    OutCurrOccupancyBytes = 95,
    OutWatermarkBytes = 96,
    OutSharedCurrOccupancyBytes = 97,
    OutSharedWatermarkBytes = 98,
    InDroppedPkts = 99,
    OutDroppedPkts = 100,

    // Pause frame statistics
    PauseRxPkts = 101,
    PauseTxPkts = 102,

    // PFC Packet Counters for RX and TX per PFC priority
    Pfc0RxPkts = 103,
    Pfc0TxPkts = 104,
    Pfc1RxPkts = 105,
    Pfc1TxPkts = 106,
    Pfc2RxPkts = 107,
    Pfc2TxPkts = 108,
    Pfc3RxPkts = 109,
    Pfc3TxPkts = 110,
    Pfc4RxPkts = 111,
    Pfc4TxPkts = 112,
    Pfc5RxPkts = 113,
    Pfc5TxPkts = 114,
    Pfc6RxPkts = 115,
    Pfc6TxPkts = 116,
    Pfc7RxPkts = 117,
    Pfc7TxPkts = 118,

    // PFC pause duration for RX and TX per PFC priority
    Pfc0RxPauseDuration = 119,
    Pfc0TxPauseDuration = 120,
    Pfc1RxPauseDuration = 121,
    Pfc1TxPauseDuration = 122,
    Pfc2RxPauseDuration = 123,
    Pfc2TxPauseDuration = 124,
    Pfc3RxPauseDuration = 125,
    Pfc3TxPauseDuration = 126,
    Pfc4RxPauseDuration = 127,
    Pfc4TxPauseDuration = 128,
    Pfc5RxPauseDuration = 129,
    Pfc5TxPauseDuration = 130,
    Pfc6RxPauseDuration = 131,
    Pfc6TxPauseDuration = 132,
    Pfc7RxPauseDuration = 133,
    Pfc7TxPauseDuration = 134,

    // PFC pause duration in micro seconds
    Pfc0RxPauseDurationUs = 135,
    Pfc0TxPauseDurationUs = 136,
    Pfc1RxPauseDurationUs = 137,
    Pfc1TxPauseDurationUs = 138,
    Pfc2RxPauseDurationUs = 139,
    Pfc2TxPauseDurationUs = 140,
    Pfc3RxPauseDurationUs = 141,
    Pfc3TxPauseDurationUs = 142,
    Pfc4RxPauseDurationUs = 143,
    Pfc4TxPauseDurationUs = 144,
    Pfc5RxPauseDurationUs = 145,
    Pfc5TxPauseDurationUs = 146,
    Pfc6RxPauseDurationUs = 147,
    Pfc6TxPauseDurationUs = 148,
    Pfc7RxPauseDurationUs = 149,
    Pfc7TxPauseDurationUs = 150,

    // PFC ON to OFF pause transitions counter per PFC priority
    Pfc0On2OffRxPkts = 151,
    Pfc1On2OffRxPkts = 152,
    Pfc2On2OffRxPkts = 153,
    Pfc3On2OffRxPkts = 154,
    Pfc4On2OffRxPkts = 155,
    Pfc5On2OffRxPkts = 156,
    Pfc6On2OffRxPkts = 157,
    Pfc7On2OffRxPkts = 158,

    // DOT3 statistics
    Dot3StatsAlignmentErrors = 159,
    Dot3StatsFcsErrors = 160,
    Dot3StatsSingleCollisionFrames = 161,
    Dot3StatsMultipleCollisionFrames = 162,
    Dot3StatsSqeTestErrors = 163,
    Dot3StatsDeferredTransmissions = 164,
    Dot3StatsLateCollisions = 165,
    Dot3StatsExcessiveCollisions = 166,
    Dot3StatsInternalMacTransmitErrors = 167,
    Dot3StatsCarrierSenseErrors = 168,
    Dot3StatsFrameTooLongs = 169,
    Dot3StatsInternalMacReceiveErrors = 170,
    Dot3StatsSymbolErrors = 171,
    Dot3ControlInUnknownOpcodes = 172,

    // EEE statistics
    EeeTxEventCount = 173,
    EeeRxEventCount = 174,
    EeeTxDuration = 175,
    EeeRxDuration = 176,

    // PRBS and FEC statistics
    PrbsErrorCount = 177,
    IfInFecCorrectableFrames = 178,
    IfInFecNotCorrectableFrames = 179,
    IfInFecSymbolErrors = 180,

    // Fabric data units
    IfInFabricDataUnits = 181,
    IfOutFabricDataUnits = 182,

    // FEC codeword symbol error counters
    IfInFecCodewordErrorsS0 = 183,
    IfInFecCodewordErrorsS1 = 184,
    IfInFecCodewordErrorsS2 = 185,
    IfInFecCodewordErrorsS3 = 186,
    IfInFecCodewordErrorsS4 = 187,
    IfInFecCodewordErrorsS5 = 188,
    IfInFecCodewordErrorsS6 = 189,
    IfInFecCodewordErrorsS7 = 190,
    IfInFecCodewordErrorsS8 = 191,
    IfInFecCodewordErrorsS9 = 192,
    IfInFecCodewordErrorsS10 = 193,
    IfInFecCodewordErrorsS11 = 194,
    IfInFecCodewordErrorsS12 = 195,
    IfInFecCodewordErrorsS13 = 196,
    IfInFecCodewordErrorsS14 = 197,
    IfInFecCodewordErrorsS15 = 198,
    IfInFecCodewordErrorsS16 = 199,
    IfInFecCorrectedBits = 200,

    // Trimmed packet statistics
    TrimPackets = 201,
    DroppedTrimPackets = 202,
    TxTrimPackets = 203,

    // Drop reason ranges (0x00001000 base)
    InConfiguredDropReasons0DroppedPkts = 0x00001000,
    InConfiguredDropReasons1DroppedPkts = 0x00001001,
    InConfiguredDropReasons2DroppedPkts = 0x00001002,
    InConfiguredDropReasons3DroppedPkts = 0x00001003,
    InConfiguredDropReasons4DroppedPkts = 0x00001004,
    InConfiguredDropReasons5DroppedPkts = 0x00001005,
    InConfiguredDropReasons6DroppedPkts = 0x00001006,
    InConfiguredDropReasons7DroppedPkts = 0x00001007,
    InConfiguredDropReasons8DroppedPkts = 0x00001008,
    InConfiguredDropReasons9DroppedPkts = 0x00001009,
    InConfiguredDropReasons10DroppedPkts = 0x0000100a,
    InConfiguredDropReasons11DroppedPkts = 0x0000100b,
    InConfiguredDropReasons12DroppedPkts = 0x0000100c,
    InConfiguredDropReasons13DroppedPkts = 0x0000100d,
    InConfiguredDropReasons14DroppedPkts = 0x0000100e,
    InConfiguredDropReasons15DroppedPkts = 0x0000100f,

    // Out drop reason ranges (0x00002000 base)
    OutConfiguredDropReasons0DroppedPkts = 0x00002000,
    OutConfiguredDropReasons1DroppedPkts = 0x00002001,
    OutConfiguredDropReasons2DroppedPkts = 0x00002002,
    OutConfiguredDropReasons3DroppedPkts = 0x00002003,
    OutConfiguredDropReasons4DroppedPkts = 0x00002004,
    OutConfiguredDropReasons5DroppedPkts = 0x00002005,
    OutConfiguredDropReasons6DroppedPkts = 0x00002006,
    OutConfiguredDropReasons7DroppedPkts = 0x00002007,

    // HW protection switchover events
    IfInHwProtectionSwitchoverEvents = 0x00002008,
    IfInHwProtectionSwitchoverDropPkts = 0x00002009,

    // Additional packet size statistics
    EtherInPkts1519To2500Octets = 0x0000200a,
    EtherInPkts2501To9000Octets = 0x0000200b,
    EtherInPkts9001To16383Octets = 0x0000200c,
    EtherOutPkts1519To2500Octets = 0x0000200d,
    EtherOutPkts2501To9000Octets = 0x0000200e,
    EtherOutPkts9001To16383Octets = 0x0000200f,

    // Port stat range end
    End = 0x00002010,
}

impl SaiPortStat {
    /// Convert from u32 value to enum variant
    pub fn from_u32(value: u32) -> Option<Self> {
        match value {
            0 => Some(Self::IfInOctets),
            1 => Some(Self::IfInUcastPkts),
            2 => Some(Self::IfInNonUcastPkts),
            3 => Some(Self::IfInDiscards),
            4 => Some(Self::IfInErrors),
            5 => Some(Self::IfInUnknownProtos),
            6 => Some(Self::IfInBroadcastPkts),
            7 => Some(Self::IfInMulticastPkts),
            8 => Some(Self::IfInVlanDiscards),
            9 => Some(Self::IfOutOctets),
            10 => Some(Self::IfOutUcastPkts),
            11 => Some(Self::IfOutNonUcastPkts),
            12 => Some(Self::IfOutDiscards),
            13 => Some(Self::IfOutErrors),
            14 => Some(Self::IfOutQlen),
            15 => Some(Self::IfOutBroadcastPkts),
            16 => Some(Self::IfOutMulticastPkts),
            17 => Some(Self::EtherStatsDropEvents),
            18 => Some(Self::EtherStatsMulticastPkts),
            19 => Some(Self::EtherStatsBroadcastPkts),
            20 => Some(Self::EtherStatsUndersizePkts),
            21 => Some(Self::EtherStatsFragments),
            22 => Some(Self::EtherStatsPkts64Octets),
            23 => Some(Self::EtherStatsPkts65To127Octets),
            24 => Some(Self::EtherStatsPkts128To255Octets),
            25 => Some(Self::EtherStatsPkts256To511Octets),
            26 => Some(Self::EtherStatsPkts512To1023Octets),
            27 => Some(Self::EtherStatsPkts1024To1518Octets),
            28 => Some(Self::EtherStatsPkts1519To2047Octets),
            29 => Some(Self::EtherStatsPkts2048To4095Octets),
            30 => Some(Self::EtherStatsPkts4096To9216Octets),
            31 => Some(Self::EtherStatsPkts9217To16383Octets),
            32 => Some(Self::EtherStatsOversizePkts),
            33 => Some(Self::EtherRxOversizePkts),
            34 => Some(Self::EtherTxOversizePkts),
            35 => Some(Self::EtherStatsJabbers),
            36 => Some(Self::EtherStatsOctets),
            37 => Some(Self::EtherStatsPkts),
            38 => Some(Self::EtherStatsCollisions),
            39 => Some(Self::EtherStatsCrcAlignErrors),
            40 => Some(Self::EtherStatsTxNoErrors),
            41 => Some(Self::EtherStatsRxNoErrors),
            42 => Some(Self::IpInReceives),
            43 => Some(Self::IpInOctets),
            44 => Some(Self::IpInUcastPkts),
            45 => Some(Self::IpInNonUcastPkts),
            46 => Some(Self::IpInDiscards),
            47 => Some(Self::IpOutOctets),
            48 => Some(Self::IpOutUcastPkts),
            49 => Some(Self::IpOutNonUcastPkts),
            50 => Some(Self::IpOutDiscards),
            51 => Some(Self::Ipv6InReceives),
            52 => Some(Self::Ipv6InOctets),
            53 => Some(Self::Ipv6InUcastPkts),
            54 => Some(Self::Ipv6InNonUcastPkts),
            55 => Some(Self::Ipv6InMcastPkts),
            56 => Some(Self::Ipv6InDiscards),
            57 => Some(Self::Ipv6OutOctets),
            58 => Some(Self::Ipv6OutUcastPkts),
            59 => Some(Self::Ipv6OutNonUcastPkts),
            60 => Some(Self::Ipv6OutMcastPkts),
            61 => Some(Self::Ipv6OutDiscards),
            62 => Some(Self::GreenWredDroppedPackets),
            63 => Some(Self::GreenWredDroppedBytes),
            64 => Some(Self::YellowWredDroppedPackets),
            65 => Some(Self::YellowWredDroppedBytes),
            66 => Some(Self::RedWredDroppedPackets),
            67 => Some(Self::RedWredDroppedBytes),
            68 => Some(Self::WredDroppedPackets),
            69 => Some(Self::WredDroppedBytes),
            70 => Some(Self::EcnMarkedPackets),
            71 => Some(Self::EtherInPkts64Octets),
            72 => Some(Self::EtherInPkts65To127Octets),
            73 => Some(Self::EtherInPkts128To255Octets),
            74 => Some(Self::EtherInPkts256To511Octets),
            75 => Some(Self::EtherInPkts512To1023Octets),
            76 => Some(Self::EtherInPkts1024To1518Octets),
            77 => Some(Self::EtherInPkts1519To2047Octets),
            78 => Some(Self::EtherInPkts2048To4095Octets),
            79 => Some(Self::EtherInPkts4096To9216Octets),
            80 => Some(Self::EtherInPkts9217To16383Octets),
            81 => Some(Self::EtherOutPkts64Octets),
            82 => Some(Self::EtherOutPkts65To127Octets),
            83 => Some(Self::EtherOutPkts128To255Octets),
            84 => Some(Self::EtherOutPkts256To511Octets),
            85 => Some(Self::EtherOutPkts512To1023Octets),
            86 => Some(Self::EtherOutPkts1024To1518Octets),
            87 => Some(Self::EtherOutPkts1519To2047Octets),
            88 => Some(Self::EtherOutPkts2048To4095Octets),
            89 => Some(Self::EtherOutPkts4096To9216Octets),
            90 => Some(Self::EtherOutPkts9217To16383Octets),
            91 => Some(Self::InCurrOccupancyBytes),
            92 => Some(Self::InWatermarkBytes),
            93 => Some(Self::InSharedCurrOccupancyBytes),
            94 => Some(Self::InSharedWatermarkBytes),
            95 => Some(Self::OutCurrOccupancyBytes),
            96 => Some(Self::OutWatermarkBytes),
            97 => Some(Self::OutSharedCurrOccupancyBytes),
            98 => Some(Self::OutSharedWatermarkBytes),
            99 => Some(Self::InDroppedPkts),
            100 => Some(Self::OutDroppedPkts),
            101 => Some(Self::PauseRxPkts),
            102 => Some(Self::PauseTxPkts),
            103 => Some(Self::Pfc0RxPkts),
            104 => Some(Self::Pfc0TxPkts),
            105 => Some(Self::Pfc1RxPkts),
            106 => Some(Self::Pfc1TxPkts),
            107 => Some(Self::Pfc2RxPkts),
            108 => Some(Self::Pfc2TxPkts),
            109 => Some(Self::Pfc3RxPkts),
            110 => Some(Self::Pfc3TxPkts),
            111 => Some(Self::Pfc4RxPkts),
            112 => Some(Self::Pfc4TxPkts),
            113 => Some(Self::Pfc5RxPkts),
            114 => Some(Self::Pfc5TxPkts),
            115 => Some(Self::Pfc6RxPkts),
            116 => Some(Self::Pfc6TxPkts),
            117 => Some(Self::Pfc7RxPkts),
            118 => Some(Self::Pfc7TxPkts),
            119 => Some(Self::Pfc0RxPauseDuration),
            120 => Some(Self::Pfc0TxPauseDuration),
            121 => Some(Self::Pfc1RxPauseDuration),
            122 => Some(Self::Pfc1TxPauseDuration),
            123 => Some(Self::Pfc2RxPauseDuration),
            124 => Some(Self::Pfc2TxPauseDuration),
            125 => Some(Self::Pfc3RxPauseDuration),
            126 => Some(Self::Pfc3TxPauseDuration),
            127 => Some(Self::Pfc4RxPauseDuration),
            128 => Some(Self::Pfc4TxPauseDuration),
            129 => Some(Self::Pfc5RxPauseDuration),
            130 => Some(Self::Pfc5TxPauseDuration),
            131 => Some(Self::Pfc6RxPauseDuration),
            132 => Some(Self::Pfc6TxPauseDuration),
            133 => Some(Self::Pfc7RxPauseDuration),
            134 => Some(Self::Pfc7TxPauseDuration),
            135 => Some(Self::Pfc0RxPauseDurationUs),
            136 => Some(Self::Pfc0TxPauseDurationUs),
            137 => Some(Self::Pfc1RxPauseDurationUs),
            138 => Some(Self::Pfc1TxPauseDurationUs),
            139 => Some(Self::Pfc2RxPauseDurationUs),
            140 => Some(Self::Pfc2TxPauseDurationUs),
            141 => Some(Self::Pfc3RxPauseDurationUs),
            142 => Some(Self::Pfc3TxPauseDurationUs),
            143 => Some(Self::Pfc4RxPauseDurationUs),
            144 => Some(Self::Pfc4TxPauseDurationUs),
            145 => Some(Self::Pfc5RxPauseDurationUs),
            146 => Some(Self::Pfc5TxPauseDurationUs),
            147 => Some(Self::Pfc6RxPauseDurationUs),
            148 => Some(Self::Pfc6TxPauseDurationUs),
            149 => Some(Self::Pfc7RxPauseDurationUs),
            150 => Some(Self::Pfc7TxPauseDurationUs),
            151 => Some(Self::Pfc0On2OffRxPkts),
            152 => Some(Self::Pfc1On2OffRxPkts),
            153 => Some(Self::Pfc2On2OffRxPkts),
            154 => Some(Self::Pfc3On2OffRxPkts),
            155 => Some(Self::Pfc4On2OffRxPkts),
            156 => Some(Self::Pfc5On2OffRxPkts),
            157 => Some(Self::Pfc6On2OffRxPkts),
            158 => Some(Self::Pfc7On2OffRxPkts),
            159 => Some(Self::Dot3StatsAlignmentErrors),
            160 => Some(Self::Dot3StatsFcsErrors),
            161 => Some(Self::Dot3StatsSingleCollisionFrames),
            162 => Some(Self::Dot3StatsMultipleCollisionFrames),
            163 => Some(Self::Dot3StatsSqeTestErrors),
            164 => Some(Self::Dot3StatsDeferredTransmissions),
            165 => Some(Self::Dot3StatsLateCollisions),
            166 => Some(Self::Dot3StatsExcessiveCollisions),
            167 => Some(Self::Dot3StatsInternalMacTransmitErrors),
            168 => Some(Self::Dot3StatsCarrierSenseErrors),
            169 => Some(Self::Dot3StatsFrameTooLongs),
            170 => Some(Self::Dot3StatsInternalMacReceiveErrors),
            171 => Some(Self::Dot3StatsSymbolErrors),
            172 => Some(Self::Dot3ControlInUnknownOpcodes),
            173 => Some(Self::EeeTxEventCount),
            174 => Some(Self::EeeRxEventCount),
            175 => Some(Self::EeeTxDuration),
            176 => Some(Self::EeeRxDuration),
            177 => Some(Self::PrbsErrorCount),
            178 => Some(Self::IfInFecCorrectableFrames),
            179 => Some(Self::IfInFecNotCorrectableFrames),
            180 => Some(Self::IfInFecSymbolErrors),
            181 => Some(Self::IfInFabricDataUnits),
            182 => Some(Self::IfOutFabricDataUnits),
            183 => Some(Self::IfInFecCodewordErrorsS0),
            184 => Some(Self::IfInFecCodewordErrorsS1),
            185 => Some(Self::IfInFecCodewordErrorsS2),
            186 => Some(Self::IfInFecCodewordErrorsS3),
            187 => Some(Self::IfInFecCodewordErrorsS4),
            188 => Some(Self::IfInFecCodewordErrorsS5),
            189 => Some(Self::IfInFecCodewordErrorsS6),
            190 => Some(Self::IfInFecCodewordErrorsS7),
            191 => Some(Self::IfInFecCodewordErrorsS8),
            192 => Some(Self::IfInFecCodewordErrorsS9),
            193 => Some(Self::IfInFecCodewordErrorsS10),
            194 => Some(Self::IfInFecCodewordErrorsS11),
            195 => Some(Self::IfInFecCodewordErrorsS12),
            196 => Some(Self::IfInFecCodewordErrorsS13),
            197 => Some(Self::IfInFecCodewordErrorsS14),
            198 => Some(Self::IfInFecCodewordErrorsS15),
            199 => Some(Self::IfInFecCodewordErrorsS16),
            200 => Some(Self::IfInFecCorrectedBits),
            201 => Some(Self::TrimPackets),
            202 => Some(Self::DroppedTrimPackets),
            203 => Some(Self::TxTrimPackets),

            // Drop reason ranges
            0x00001000 => Some(Self::InConfiguredDropReasons0DroppedPkts),
            0x00001001 => Some(Self::InConfiguredDropReasons1DroppedPkts),
            0x00001002 => Some(Self::InConfiguredDropReasons2DroppedPkts),
            0x00001003 => Some(Self::InConfiguredDropReasons3DroppedPkts),
            0x00001004 => Some(Self::InConfiguredDropReasons4DroppedPkts),
            0x00001005 => Some(Self::InConfiguredDropReasons5DroppedPkts),
            0x00001006 => Some(Self::InConfiguredDropReasons6DroppedPkts),
            0x00001007 => Some(Self::InConfiguredDropReasons7DroppedPkts),
            0x00001008 => Some(Self::InConfiguredDropReasons8DroppedPkts),
            0x00001009 => Some(Self::InConfiguredDropReasons9DroppedPkts),
            0x0000100a => Some(Self::InConfiguredDropReasons10DroppedPkts),
            0x0000100b => Some(Self::InConfiguredDropReasons11DroppedPkts),
            0x0000100c => Some(Self::InConfiguredDropReasons12DroppedPkts),
            0x0000100d => Some(Self::InConfiguredDropReasons13DroppedPkts),
            0x0000100e => Some(Self::InConfiguredDropReasons14DroppedPkts),
            0x0000100f => Some(Self::InConfiguredDropReasons15DroppedPkts),

            0x00002000 => Some(Self::OutConfiguredDropReasons0DroppedPkts),
            0x00002001 => Some(Self::OutConfiguredDropReasons1DroppedPkts),
            0x00002002 => Some(Self::OutConfiguredDropReasons2DroppedPkts),
            0x00002003 => Some(Self::OutConfiguredDropReasons3DroppedPkts),
            0x00002004 => Some(Self::OutConfiguredDropReasons4DroppedPkts),
            0x00002005 => Some(Self::OutConfiguredDropReasons5DroppedPkts),
            0x00002006 => Some(Self::OutConfiguredDropReasons6DroppedPkts),
            0x00002007 => Some(Self::OutConfiguredDropReasons7DroppedPkts),

            0x00002008 => Some(Self::IfInHwProtectionSwitchoverEvents),
            0x00002009 => Some(Self::IfInHwProtectionSwitchoverDropPkts),
            0x0000200a => Some(Self::EtherInPkts1519To2500Octets),
            0x0000200b => Some(Self::EtherInPkts2501To9000Octets),
            0x0000200c => Some(Self::EtherInPkts9001To16383Octets),
            0x0000200d => Some(Self::EtherOutPkts1519To2500Octets),
            0x0000200e => Some(Self::EtherOutPkts2501To9000Octets),
            0x0000200f => Some(Self::EtherOutPkts9001To16383Octets),
            0x00002010 => Some(Self::End),
            _ => None,
        }
    }

    /// Convert enum variant to u32 value
    #[allow(dead_code)] // May be used by external code or future features
    pub fn to_u32(self) -> u32 {
        self as u32
    }

    /// Convert enum variant to C constant name
    pub fn to_c_name(self) -> &'static str {
        match self {
            Self::IfInOctets => "SAI_PORT_STAT_IF_IN_OCTETS",
            Self::IfInUcastPkts => "SAI_PORT_STAT_IF_IN_UCAST_PKTS",
            Self::IfInNonUcastPkts => "SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS",
            Self::IfInDiscards => "SAI_PORT_STAT_IF_IN_DISCARDS",
            Self::IfInErrors => "SAI_PORT_STAT_IF_IN_ERRORS",
            Self::IfInUnknownProtos => "SAI_PORT_STAT_IF_IN_UNKNOWN_PROTOS",
            Self::IfInBroadcastPkts => "SAI_PORT_STAT_IF_IN_BROADCAST_PKTS",
            Self::IfInMulticastPkts => "SAI_PORT_STAT_IF_IN_MULTICAST_PKTS",
            Self::IfInVlanDiscards => "SAI_PORT_STAT_IF_IN_VLAN_DISCARDS",
            Self::IfOutOctets => "SAI_PORT_STAT_IF_OUT_OCTETS",
            Self::IfOutUcastPkts => "SAI_PORT_STAT_IF_OUT_UCAST_PKTS",
            Self::IfOutNonUcastPkts => "SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS",
            Self::IfOutDiscards => "SAI_PORT_STAT_IF_OUT_DISCARDS",
            Self::IfOutErrors => "SAI_PORT_STAT_IF_OUT_ERRORS",
            Self::IfOutQlen => "SAI_PORT_STAT_IF_OUT_QLEN",
            Self::IfOutBroadcastPkts => "SAI_PORT_STAT_IF_OUT_BROADCAST_PKTS",
            Self::IfOutMulticastPkts => "SAI_PORT_STAT_IF_OUT_MULTICAST_PKTS",
            Self::EtherStatsDropEvents => "SAI_PORT_STAT_ETHER_STATS_DROP_EVENTS",
            Self::EtherStatsMulticastPkts => "SAI_PORT_STAT_ETHER_STATS_MULTICAST_PKTS",
            Self::EtherStatsBroadcastPkts => "SAI_PORT_STAT_ETHER_STATS_BROADCAST_PKTS",
            Self::EtherStatsUndersizePkts => "SAI_PORT_STAT_ETHER_STATS_UNDERSIZE_PKTS",
            Self::EtherStatsFragments => "SAI_PORT_STAT_ETHER_STATS_FRAGMENTS",
            Self::EtherStatsPkts64Octets => "SAI_PORT_STAT_ETHER_STATS_PKTS_64_OCTETS",
            Self::EtherStatsPkts65To127Octets => "SAI_PORT_STAT_ETHER_STATS_PKTS_65_TO_127_OCTETS",
            Self::EtherStatsPkts128To255Octets => {
                "SAI_PORT_STAT_ETHER_STATS_PKTS_128_TO_255_OCTETS"
            }
            Self::EtherStatsPkts256To511Octets => {
                "SAI_PORT_STAT_ETHER_STATS_PKTS_256_TO_511_OCTETS"
            }
            Self::EtherStatsPkts512To1023Octets => {
                "SAI_PORT_STAT_ETHER_STATS_PKTS_512_TO_1023_OCTETS"
            }
            Self::EtherStatsPkts1024To1518Octets => {
                "SAI_PORT_STAT_ETHER_STATS_PKTS_1024_TO_1518_OCTETS"
            }
            Self::EtherStatsPkts1519To2047Octets => {
                "SAI_PORT_STAT_ETHER_STATS_PKTS_1519_TO_2047_OCTETS"
            }
            Self::EtherStatsPkts2048To4095Octets => {
                "SAI_PORT_STAT_ETHER_STATS_PKTS_2048_TO_4095_OCTETS"
            }
            Self::EtherStatsPkts4096To9216Octets => {
                "SAI_PORT_STAT_ETHER_STATS_PKTS_4096_TO_9216_OCTETS"
            }
            Self::EtherStatsPkts9217To16383Octets => {
                "SAI_PORT_STAT_ETHER_STATS_PKTS_9217_TO_16383_OCTETS"
            }
            Self::EtherStatsOversizePkts => "SAI_PORT_STAT_ETHER_STATS_OVERSIZE_PKTS",
            Self::EtherRxOversizePkts => "SAI_PORT_STAT_ETHER_RX_OVERSIZE_PKTS",
            Self::EtherTxOversizePkts => "SAI_PORT_STAT_ETHER_TX_OVERSIZE_PKTS",
            Self::EtherStatsJabbers => "SAI_PORT_STAT_ETHER_STATS_JABBERS",
            Self::EtherStatsOctets => "SAI_PORT_STAT_ETHER_STATS_OCTETS",
            Self::EtherStatsPkts => "SAI_PORT_STAT_ETHER_STATS_PKTS",
            Self::EtherStatsCollisions => "SAI_PORT_STAT_ETHER_STATS_COLLISIONS",
            Self::EtherStatsCrcAlignErrors => "SAI_PORT_STAT_ETHER_STATS_CRC_ALIGN_ERRORS",
            Self::EtherStatsTxNoErrors => "SAI_PORT_STAT_ETHER_STATS_TX_NO_ERRORS",
            Self::EtherStatsRxNoErrors => "SAI_PORT_STAT_ETHER_STATS_RX_NO_ERRORS",
            Self::IpInReceives => "SAI_PORT_STAT_IP_IN_RECEIVES",
            Self::IpInOctets => "SAI_PORT_STAT_IP_IN_OCTETS",
            Self::IpInUcastPkts => "SAI_PORT_STAT_IP_IN_UCAST_PKTS",
            Self::IpInNonUcastPkts => "SAI_PORT_STAT_IP_IN_NON_UCAST_PKTS",
            Self::IpInDiscards => "SAI_PORT_STAT_IP_IN_DISCARDS",
            Self::IpOutOctets => "SAI_PORT_STAT_IP_OUT_OCTETS",
            Self::IpOutUcastPkts => "SAI_PORT_STAT_IP_OUT_UCAST_PKTS",
            Self::IpOutNonUcastPkts => "SAI_PORT_STAT_IP_OUT_NON_UCAST_PKTS",
            Self::IpOutDiscards => "SAI_PORT_STAT_IP_OUT_DISCARDS",
            Self::Ipv6InReceives => "SAI_PORT_STAT_IPV6_IN_RECEIVES",
            Self::Ipv6InOctets => "SAI_PORT_STAT_IPV6_IN_OCTETS",
            Self::Ipv6InUcastPkts => "SAI_PORT_STAT_IPV6_IN_UCAST_PKTS",
            Self::Ipv6InNonUcastPkts => "SAI_PORT_STAT_IPV6_IN_NON_UCAST_PKTS",
            Self::Ipv6InMcastPkts => "SAI_PORT_STAT_IPV6_IN_MCAST_PKTS",
            Self::Ipv6InDiscards => "SAI_PORT_STAT_IPV6_IN_DISCARDS",
            Self::Ipv6OutOctets => "SAI_PORT_STAT_IPV6_OUT_OCTETS",
            Self::Ipv6OutUcastPkts => "SAI_PORT_STAT_IPV6_OUT_UCAST_PKTS",
            Self::Ipv6OutNonUcastPkts => "SAI_PORT_STAT_IPV6_OUT_NON_UCAST_PKTS",
            Self::Ipv6OutMcastPkts => "SAI_PORT_STAT_IPV6_OUT_MCAST_PKTS",
            Self::Ipv6OutDiscards => "SAI_PORT_STAT_IPV6_OUT_DISCARDS",
            Self::GreenWredDroppedPackets => "SAI_PORT_STAT_GREEN_WRED_DROPPED_PACKETS",
            Self::GreenWredDroppedBytes => "SAI_PORT_STAT_GREEN_WRED_DROPPED_BYTES",
            Self::YellowWredDroppedPackets => "SAI_PORT_STAT_YELLOW_WRED_DROPPED_PACKETS",
            Self::YellowWredDroppedBytes => "SAI_PORT_STAT_YELLOW_WRED_DROPPED_BYTES",
            Self::RedWredDroppedPackets => "SAI_PORT_STAT_RED_WRED_DROPPED_PACKETS",
            Self::RedWredDroppedBytes => "SAI_PORT_STAT_RED_WRED_DROPPED_BYTES",
            Self::WredDroppedPackets => "SAI_PORT_STAT_WRED_DROPPED_PACKETS",
            Self::WredDroppedBytes => "SAI_PORT_STAT_WRED_DROPPED_BYTES",
            Self::EcnMarkedPackets => "SAI_PORT_STAT_ECN_MARKED_PACKETS",
            Self::EtherInPkts64Octets => "SAI_PORT_STAT_ETHER_IN_PKTS_64_OCTETS",
            Self::EtherInPkts65To127Octets => "SAI_PORT_STAT_ETHER_IN_PKTS_65_TO_127_OCTETS",
            Self::EtherInPkts128To255Octets => "SAI_PORT_STAT_ETHER_IN_PKTS_128_TO_255_OCTETS",
            Self::EtherInPkts256To511Octets => "SAI_PORT_STAT_ETHER_IN_PKTS_256_TO_511_OCTETS",
            Self::EtherInPkts512To1023Octets => "SAI_PORT_STAT_ETHER_IN_PKTS_512_TO_1023_OCTETS",
            Self::EtherInPkts1024To1518Octets => "SAI_PORT_STAT_ETHER_IN_PKTS_1024_TO_1518_OCTETS",
            Self::EtherInPkts1519To2047Octets => "SAI_PORT_STAT_ETHER_IN_PKTS_1519_TO_2047_OCTETS",
            Self::EtherInPkts2048To4095Octets => "SAI_PORT_STAT_ETHER_IN_PKTS_2048_TO_4095_OCTETS",
            Self::EtherInPkts4096To9216Octets => "SAI_PORT_STAT_ETHER_IN_PKTS_4096_TO_9216_OCTETS",
            Self::EtherInPkts9217To16383Octets => {
                "SAI_PORT_STAT_ETHER_IN_PKTS_9217_TO_16383_OCTETS"
            }
            Self::EtherOutPkts64Octets => "SAI_PORT_STAT_ETHER_OUT_PKTS_64_OCTETS",
            Self::EtherOutPkts65To127Octets => "SAI_PORT_STAT_ETHER_OUT_PKTS_65_TO_127_OCTETS",
            Self::EtherOutPkts128To255Octets => "SAI_PORT_STAT_ETHER_OUT_PKTS_128_TO_255_OCTETS",
            Self::EtherOutPkts256To511Octets => "SAI_PORT_STAT_ETHER_OUT_PKTS_256_TO_511_OCTETS",
            Self::EtherOutPkts512To1023Octets => "SAI_PORT_STAT_ETHER_OUT_PKTS_512_TO_1023_OCTETS",
            Self::EtherOutPkts1024To1518Octets => {
                "SAI_PORT_STAT_ETHER_OUT_PKTS_1024_TO_1518_OCTETS"
            }
            Self::EtherOutPkts1519To2047Octets => {
                "SAI_PORT_STAT_ETHER_OUT_PKTS_1519_TO_2047_OCTETS"
            }
            Self::EtherOutPkts2048To4095Octets => {
                "SAI_PORT_STAT_ETHER_OUT_PKTS_2048_TO_4095_OCTETS"
            }
            Self::EtherOutPkts4096To9216Octets => {
                "SAI_PORT_STAT_ETHER_OUT_PKTS_4096_TO_9216_OCTETS"
            }
            Self::EtherOutPkts9217To16383Octets => {
                "SAI_PORT_STAT_ETHER_OUT_PKTS_9217_TO_16383_OCTETS"
            }
            Self::InCurrOccupancyBytes => "SAI_PORT_STAT_IN_CURR_OCCUPANCY_BYTES",
            Self::InWatermarkBytes => "SAI_PORT_STAT_IN_WATERMARK_BYTES",
            Self::InSharedCurrOccupancyBytes => "SAI_PORT_STAT_IN_SHARED_CURR_OCCUPANCY_BYTES",
            Self::InSharedWatermarkBytes => "SAI_PORT_STAT_IN_SHARED_WATERMARK_BYTES",
            Self::OutCurrOccupancyBytes => "SAI_PORT_STAT_OUT_CURR_OCCUPANCY_BYTES",
            Self::OutWatermarkBytes => "SAI_PORT_STAT_OUT_WATERMARK_BYTES",
            Self::OutSharedCurrOccupancyBytes => "SAI_PORT_STAT_OUT_SHARED_CURR_OCCUPANCY_BYTES",
            Self::OutSharedWatermarkBytes => "SAI_PORT_STAT_OUT_SHARED_WATERMARK_BYTES",
            Self::InDroppedPkts => "SAI_PORT_STAT_IN_DROPPED_PKTS",
            Self::OutDroppedPkts => "SAI_PORT_STAT_OUT_DROPPED_PKTS",
            Self::PauseRxPkts => "SAI_PORT_STAT_PAUSE_RX_PKTS",
            Self::PauseTxPkts => "SAI_PORT_STAT_PAUSE_TX_PKTS",
            Self::Pfc0RxPkts => "SAI_PORT_STAT_PFC_0_RX_PKTS",
            Self::Pfc0TxPkts => "SAI_PORT_STAT_PFC_0_TX_PKTS",
            Self::Pfc1RxPkts => "SAI_PORT_STAT_PFC_1_RX_PKTS",
            Self::Pfc1TxPkts => "SAI_PORT_STAT_PFC_1_TX_PKTS",
            Self::Pfc2RxPkts => "SAI_PORT_STAT_PFC_2_RX_PKTS",
            Self::Pfc2TxPkts => "SAI_PORT_STAT_PFC_2_TX_PKTS",
            Self::Pfc3RxPkts => "SAI_PORT_STAT_PFC_3_RX_PKTS",
            Self::Pfc3TxPkts => "SAI_PORT_STAT_PFC_3_TX_PKTS",
            Self::Pfc4RxPkts => "SAI_PORT_STAT_PFC_4_RX_PKTS",
            Self::Pfc4TxPkts => "SAI_PORT_STAT_PFC_4_TX_PKTS",
            Self::Pfc5RxPkts => "SAI_PORT_STAT_PFC_5_RX_PKTS",
            Self::Pfc5TxPkts => "SAI_PORT_STAT_PFC_5_TX_PKTS",
            Self::Pfc6RxPkts => "SAI_PORT_STAT_PFC_6_RX_PKTS",
            Self::Pfc6TxPkts => "SAI_PORT_STAT_PFC_6_TX_PKTS",
            Self::Pfc7RxPkts => "SAI_PORT_STAT_PFC_7_RX_PKTS",
            Self::Pfc7TxPkts => "SAI_PORT_STAT_PFC_7_TX_PKTS",
            Self::Pfc0RxPauseDuration => "SAI_PORT_STAT_PFC_0_RX_PAUSE_DURATION",
            Self::Pfc0TxPauseDuration => "SAI_PORT_STAT_PFC_0_TX_PAUSE_DURATION",
            Self::Pfc1RxPauseDuration => "SAI_PORT_STAT_PFC_1_RX_PAUSE_DURATION",
            Self::Pfc1TxPauseDuration => "SAI_PORT_STAT_PFC_1_TX_PAUSE_DURATION",
            Self::Pfc2RxPauseDuration => "SAI_PORT_STAT_PFC_2_RX_PAUSE_DURATION",
            Self::Pfc2TxPauseDuration => "SAI_PORT_STAT_PFC_2_TX_PAUSE_DURATION",
            Self::Pfc3RxPauseDuration => "SAI_PORT_STAT_PFC_3_RX_PAUSE_DURATION",
            Self::Pfc3TxPauseDuration => "SAI_PORT_STAT_PFC_3_TX_PAUSE_DURATION",
            Self::Pfc4RxPauseDuration => "SAI_PORT_STAT_PFC_4_RX_PAUSE_DURATION",
            Self::Pfc4TxPauseDuration => "SAI_PORT_STAT_PFC_4_TX_PAUSE_DURATION",
            Self::Pfc5RxPauseDuration => "SAI_PORT_STAT_PFC_5_RX_PAUSE_DURATION",
            Self::Pfc5TxPauseDuration => "SAI_PORT_STAT_PFC_5_TX_PAUSE_DURATION",
            Self::Pfc6RxPauseDuration => "SAI_PORT_STAT_PFC_6_RX_PAUSE_DURATION",
            Self::Pfc6TxPauseDuration => "SAI_PORT_STAT_PFC_6_TX_PAUSE_DURATION",
            Self::Pfc7RxPauseDuration => "SAI_PORT_STAT_PFC_7_RX_PAUSE_DURATION",
            Self::Pfc7TxPauseDuration => "SAI_PORT_STAT_PFC_7_TX_PAUSE_DURATION",
            Self::Pfc0RxPauseDurationUs => "SAI_PORT_STAT_PFC_0_RX_PAUSE_DURATION_US",
            Self::Pfc0TxPauseDurationUs => "SAI_PORT_STAT_PFC_0_TX_PAUSE_DURATION_US",
            Self::Pfc1RxPauseDurationUs => "SAI_PORT_STAT_PFC_1_RX_PAUSE_DURATION_US",
            Self::Pfc1TxPauseDurationUs => "SAI_PORT_STAT_PFC_1_TX_PAUSE_DURATION_US",
            Self::Pfc2RxPauseDurationUs => "SAI_PORT_STAT_PFC_2_RX_PAUSE_DURATION_US",
            Self::Pfc2TxPauseDurationUs => "SAI_PORT_STAT_PFC_2_TX_PAUSE_DURATION_US",
            Self::Pfc3RxPauseDurationUs => "SAI_PORT_STAT_PFC_3_RX_PAUSE_DURATION_US",
            Self::Pfc3TxPauseDurationUs => "SAI_PORT_STAT_PFC_3_TX_PAUSE_DURATION_US",
            Self::Pfc4RxPauseDurationUs => "SAI_PORT_STAT_PFC_4_RX_PAUSE_DURATION_US",
            Self::Pfc4TxPauseDurationUs => "SAI_PORT_STAT_PFC_4_TX_PAUSE_DURATION_US",
            Self::Pfc5RxPauseDurationUs => "SAI_PORT_STAT_PFC_5_RX_PAUSE_DURATION_US",
            Self::Pfc5TxPauseDurationUs => "SAI_PORT_STAT_PFC_5_TX_PAUSE_DURATION_US",
            Self::Pfc6RxPauseDurationUs => "SAI_PORT_STAT_PFC_6_RX_PAUSE_DURATION_US",
            Self::Pfc6TxPauseDurationUs => "SAI_PORT_STAT_PFC_6_TX_PAUSE_DURATION_US",
            Self::Pfc7RxPauseDurationUs => "SAI_PORT_STAT_PFC_7_RX_PAUSE_DURATION_US",
            Self::Pfc7TxPauseDurationUs => "SAI_PORT_STAT_PFC_7_TX_PAUSE_DURATION_US",
            Self::Pfc0On2OffRxPkts => "SAI_PORT_STAT_PFC_0_ON2OFF_RX_PKTS",
            Self::Pfc1On2OffRxPkts => "SAI_PORT_STAT_PFC_1_ON2OFF_RX_PKTS",
            Self::Pfc2On2OffRxPkts => "SAI_PORT_STAT_PFC_2_ON2OFF_RX_PKTS",
            Self::Pfc3On2OffRxPkts => "SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS",
            Self::Pfc4On2OffRxPkts => "SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS",
            Self::Pfc5On2OffRxPkts => "SAI_PORT_STAT_PFC_5_ON2OFF_RX_PKTS",
            Self::Pfc6On2OffRxPkts => "SAI_PORT_STAT_PFC_6_ON2OFF_RX_PKTS",
            Self::Pfc7On2OffRxPkts => "SAI_PORT_STAT_PFC_7_ON2OFF_RX_PKTS",
            Self::Dot3StatsAlignmentErrors => "SAI_PORT_STAT_DOT3_STATS_ALIGNMENT_ERRORS",
            Self::Dot3StatsFcsErrors => "SAI_PORT_STAT_DOT3_STATS_FCS_ERRORS",
            Self::Dot3StatsSingleCollisionFrames => {
                "SAI_PORT_STAT_DOT3_STATS_SINGLE_COLLISION_FRAMES"
            }
            Self::Dot3StatsMultipleCollisionFrames => {
                "SAI_PORT_STAT_DOT3_STATS_MULTIPLE_COLLISION_FRAMES"
            }
            Self::Dot3StatsSqeTestErrors => "SAI_PORT_STAT_DOT3_STATS_SQE_TEST_ERRORS",
            Self::Dot3StatsDeferredTransmissions => {
                "SAI_PORT_STAT_DOT3_STATS_DEFERRED_TRANSMISSIONS"
            }
            Self::Dot3StatsLateCollisions => "SAI_PORT_STAT_DOT3_STATS_LATE_COLLISIONS",
            Self::Dot3StatsExcessiveCollisions => "SAI_PORT_STAT_DOT3_STATS_EXCESSIVE_COLLISIONS",
            Self::Dot3StatsInternalMacTransmitErrors => {
                "SAI_PORT_STAT_DOT3_STATS_INTERNAL_MAC_TRANSMIT_ERRORS"
            }
            Self::Dot3StatsCarrierSenseErrors => "SAI_PORT_STAT_DOT3_STATS_CARRIER_SENSE_ERRORS",
            Self::Dot3StatsFrameTooLongs => "SAI_PORT_STAT_DOT3_STATS_FRAME_TOO_LONGS",
            Self::Dot3StatsInternalMacReceiveErrors => {
                "SAI_PORT_STAT_DOT3_STATS_INTERNAL_MAC_RECEIVE_ERRORS"
            }
            Self::Dot3StatsSymbolErrors => "SAI_PORT_STAT_DOT3_STATS_SYMBOL_ERRORS",
            Self::Dot3ControlInUnknownOpcodes => "SAI_PORT_STAT_DOT3_CONTROL_IN_UNKNOWN_OPCODES",
            Self::EeeTxEventCount => "SAI_PORT_STAT_EEE_TX_EVENT_COUNT",
            Self::EeeRxEventCount => "SAI_PORT_STAT_EEE_RX_EVENT_COUNT",
            Self::EeeTxDuration => "SAI_PORT_STAT_EEE_TX_DURATION",
            Self::EeeRxDuration => "SAI_PORT_STAT_EEE_RX_DURATION",
            Self::PrbsErrorCount => "SAI_PORT_STAT_PRBS_ERROR_COUNT",
            Self::IfInFecCorrectableFrames => "SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES",
            Self::IfInFecNotCorrectableFrames => "SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES",
            Self::IfInFecSymbolErrors => "SAI_PORT_STAT_IF_IN_FEC_SYMBOL_ERRORS",
            Self::IfInFabricDataUnits => "SAI_PORT_STAT_IF_IN_FABRIC_DATA_UNITS",
            Self::IfOutFabricDataUnits => "SAI_PORT_STAT_IF_OUT_FABRIC_DATA_UNITS",
            Self::IfInFecCodewordErrorsS0 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S0",
            Self::IfInFecCodewordErrorsS1 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S1",
            Self::IfInFecCodewordErrorsS2 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S2",
            Self::IfInFecCodewordErrorsS3 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S3",
            Self::IfInFecCodewordErrorsS4 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S4",
            Self::IfInFecCodewordErrorsS5 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S5",
            Self::IfInFecCodewordErrorsS6 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S6",
            Self::IfInFecCodewordErrorsS7 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S7",
            Self::IfInFecCodewordErrorsS8 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S8",
            Self::IfInFecCodewordErrorsS9 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S9",
            Self::IfInFecCodewordErrorsS10 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S10",
            Self::IfInFecCodewordErrorsS11 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S11",
            Self::IfInFecCodewordErrorsS12 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S12",
            Self::IfInFecCodewordErrorsS13 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S13",
            Self::IfInFecCodewordErrorsS14 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S14",
            Self::IfInFecCodewordErrorsS15 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S15",
            Self::IfInFecCodewordErrorsS16 => "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S16",
            Self::IfInFecCorrectedBits => "SAI_PORT_STAT_IF_IN_FEC_CORRECTED_BITS",
            Self::TrimPackets => "SAI_PORT_STAT_TRIM_PACKETS",
            Self::DroppedTrimPackets => "SAI_PORT_STAT_DROPPED_TRIM_PACKETS",
            Self::TxTrimPackets => "SAI_PORT_STAT_TX_TRIM_PACKETS",
            Self::InConfiguredDropReasons0DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_0_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons1DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_1_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons2DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_2_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons3DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_3_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons4DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_4_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons5DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_5_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons6DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_6_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons7DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_7_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons8DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_8_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons9DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_9_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons10DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_10_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons11DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_11_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons12DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_12_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons13DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_13_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons14DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_14_DROPPED_PKTS"
            }
            Self::InConfiguredDropReasons15DroppedPkts => {
                "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_15_DROPPED_PKTS"
            }
            Self::OutConfiguredDropReasons0DroppedPkts => {
                "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_0_DROPPED_PKTS"
            }
            Self::OutConfiguredDropReasons1DroppedPkts => {
                "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_1_DROPPED_PKTS"
            }
            Self::OutConfiguredDropReasons2DroppedPkts => {
                "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_2_DROPPED_PKTS"
            }
            Self::OutConfiguredDropReasons3DroppedPkts => {
                "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_3_DROPPED_PKTS"
            }
            Self::OutConfiguredDropReasons4DroppedPkts => {
                "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_4_DROPPED_PKTS"
            }
            Self::OutConfiguredDropReasons5DroppedPkts => {
                "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_5_DROPPED_PKTS"
            }
            Self::OutConfiguredDropReasons6DroppedPkts => {
                "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_6_DROPPED_PKTS"
            }
            Self::OutConfiguredDropReasons7DroppedPkts => {
                "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_7_DROPPED_PKTS"
            }
            Self::IfInHwProtectionSwitchoverEvents => {
                "SAI_PORT_STAT_IF_IN_HW_PROTECTION_SWITCHOVER_EVENTS"
            }
            Self::IfInHwProtectionSwitchoverDropPkts => {
                "SAI_PORT_STAT_IF_IN_HW_PROTECTION_SWITCHOVER_DROP_PKTS"
            }
            Self::EtherInPkts1519To2500Octets => "SAI_PORT_STAT_ETHER_IN_PKTS_1519_TO_2500_OCTETS",
            Self::EtherInPkts2501To9000Octets => "SAI_PORT_STAT_ETHER_IN_PKTS_2501_TO_9000_OCTETS",
            Self::EtherInPkts9001To16383Octets => {
                "SAI_PORT_STAT_ETHER_IN_PKTS_9001_TO_16383_OCTETS"
            }
            Self::EtherOutPkts1519To2500Octets => {
                "SAI_PORT_STAT_ETHER_OUT_PKTS_1519_TO_2500_OCTETS"
            }
            Self::EtherOutPkts2501To9000Octets => {
                "SAI_PORT_STAT_ETHER_OUT_PKTS_2501_TO_9000_OCTETS"
            }
            Self::EtherOutPkts9001To16383Octets => {
                "SAI_PORT_STAT_ETHER_OUT_PKTS_9001_TO_16383_OCTETS"
            }
            Self::End => "SAI_PORT_STAT_END",
        }
    }
}

impl fmt::Display for SaiPortStat {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.to_c_name())
    }
}

impl FromStr for SaiPortStat {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "SAI_PORT_STAT_START" | "SAI_PORT_STAT_IF_IN_OCTETS" => Ok(Self::IfInOctets),
            "SAI_PORT_STAT_IF_IN_UCAST_PKTS" => Ok(Self::IfInUcastPkts),
            "SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS" => Ok(Self::IfInNonUcastPkts),
            "SAI_PORT_STAT_IF_IN_DISCARDS" => Ok(Self::IfInDiscards),
            "SAI_PORT_STAT_IF_IN_ERRORS" => Ok(Self::IfInErrors),
            "SAI_PORT_STAT_IF_IN_UNKNOWN_PROTOS" => Ok(Self::IfInUnknownProtos),
            "SAI_PORT_STAT_IF_IN_BROADCAST_PKTS" => Ok(Self::IfInBroadcastPkts),
            "SAI_PORT_STAT_IF_IN_MULTICAST_PKTS" => Ok(Self::IfInMulticastPkts),
            "SAI_PORT_STAT_IF_IN_VLAN_DISCARDS" => Ok(Self::IfInVlanDiscards),
            "SAI_PORT_STAT_IF_OUT_OCTETS" => Ok(Self::IfOutOctets),
            "SAI_PORT_STAT_IF_OUT_UCAST_PKTS" => Ok(Self::IfOutUcastPkts),
            "SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS" => Ok(Self::IfOutNonUcastPkts),
            "SAI_PORT_STAT_IF_OUT_DISCARDS" => Ok(Self::IfOutDiscards),
            "SAI_PORT_STAT_IF_OUT_ERRORS" => Ok(Self::IfOutErrors),
            "SAI_PORT_STAT_IF_OUT_QLEN" => Ok(Self::IfOutQlen),
            "SAI_PORT_STAT_IF_OUT_BROADCAST_PKTS" => Ok(Self::IfOutBroadcastPkts),
            "SAI_PORT_STAT_IF_OUT_MULTICAST_PKTS" => Ok(Self::IfOutMulticastPkts),
            "SAI_PORT_STAT_ETHER_STATS_DROP_EVENTS" => Ok(Self::EtherStatsDropEvents),
            "SAI_PORT_STAT_ETHER_STATS_MULTICAST_PKTS" => Ok(Self::EtherStatsMulticastPkts),
            "SAI_PORT_STAT_ETHER_STATS_BROADCAST_PKTS" => Ok(Self::EtherStatsBroadcastPkts),
            "SAI_PORT_STAT_ETHER_STATS_UNDERSIZE_PKTS" => Ok(Self::EtherStatsUndersizePkts),
            "SAI_PORT_STAT_ETHER_STATS_FRAGMENTS" => Ok(Self::EtherStatsFragments),
            "SAI_PORT_STAT_ETHER_STATS_PKTS_64_OCTETS" => Ok(Self::EtherStatsPkts64Octets),
            "SAI_PORT_STAT_ETHER_STATS_PKTS_65_TO_127_OCTETS" => {
                Ok(Self::EtherStatsPkts65To127Octets)
            }
            "SAI_PORT_STAT_ETHER_STATS_PKTS_128_TO_255_OCTETS" => {
                Ok(Self::EtherStatsPkts128To255Octets)
            }
            "SAI_PORT_STAT_ETHER_STATS_PKTS_256_TO_511_OCTETS" => {
                Ok(Self::EtherStatsPkts256To511Octets)
            }
            "SAI_PORT_STAT_ETHER_STATS_PKTS_512_TO_1023_OCTETS" => {
                Ok(Self::EtherStatsPkts512To1023Octets)
            }
            "SAI_PORT_STAT_ETHER_STATS_PKTS_1024_TO_1518_OCTETS" => {
                Ok(Self::EtherStatsPkts1024To1518Octets)
            }
            "SAI_PORT_STAT_ETHER_STATS_PKTS_1519_TO_2047_OCTETS" => {
                Ok(Self::EtherStatsPkts1519To2047Octets)
            }
            "SAI_PORT_STAT_ETHER_STATS_PKTS_2048_TO_4095_OCTETS" => {
                Ok(Self::EtherStatsPkts2048To4095Octets)
            }
            "SAI_PORT_STAT_ETHER_STATS_PKTS_4096_TO_9216_OCTETS" => {
                Ok(Self::EtherStatsPkts4096To9216Octets)
            }
            "SAI_PORT_STAT_ETHER_STATS_PKTS_9217_TO_16383_OCTETS" => {
                Ok(Self::EtherStatsPkts9217To16383Octets)
            }
            "SAI_PORT_STAT_ETHER_STATS_OVERSIZE_PKTS" => Ok(Self::EtherStatsOversizePkts),
            "SAI_PORT_STAT_ETHER_RX_OVERSIZE_PKTS" => Ok(Self::EtherRxOversizePkts),
            "SAI_PORT_STAT_ETHER_TX_OVERSIZE_PKTS" => Ok(Self::EtherTxOversizePkts),
            "SAI_PORT_STAT_ETHER_STATS_JABBERS" => Ok(Self::EtherStatsJabbers),
            "SAI_PORT_STAT_ETHER_STATS_OCTETS" => Ok(Self::EtherStatsOctets),
            "SAI_PORT_STAT_ETHER_STATS_PKTS" => Ok(Self::EtherStatsPkts),
            "SAI_PORT_STAT_ETHER_STATS_COLLISIONS" => Ok(Self::EtherStatsCollisions),
            "SAI_PORT_STAT_ETHER_STATS_CRC_ALIGN_ERRORS" => Ok(Self::EtherStatsCrcAlignErrors),
            "SAI_PORT_STAT_ETHER_STATS_TX_NO_ERRORS" => Ok(Self::EtherStatsTxNoErrors),
            "SAI_PORT_STAT_ETHER_STATS_RX_NO_ERRORS" => Ok(Self::EtherStatsRxNoErrors),
            "SAI_PORT_STAT_IP_IN_RECEIVES" => Ok(Self::IpInReceives),
            "SAI_PORT_STAT_IP_IN_OCTETS" => Ok(Self::IpInOctets),
            "SAI_PORT_STAT_IP_IN_UCAST_PKTS" => Ok(Self::IpInUcastPkts),
            "SAI_PORT_STAT_IP_IN_NON_UCAST_PKTS" => Ok(Self::IpInNonUcastPkts),
            "SAI_PORT_STAT_IP_IN_DISCARDS" => Ok(Self::IpInDiscards),
            "SAI_PORT_STAT_IP_OUT_OCTETS" => Ok(Self::IpOutOctets),
            "SAI_PORT_STAT_IP_OUT_UCAST_PKTS" => Ok(Self::IpOutUcastPkts),
            "SAI_PORT_STAT_IP_OUT_NON_UCAST_PKTS" => Ok(Self::IpOutNonUcastPkts),
            "SAI_PORT_STAT_IP_OUT_DISCARDS" => Ok(Self::IpOutDiscards),
            "SAI_PORT_STAT_IPV6_IN_RECEIVES" => Ok(Self::Ipv6InReceives),
            "SAI_PORT_STAT_IPV6_IN_OCTETS" => Ok(Self::Ipv6InOctets),
            "SAI_PORT_STAT_IPV6_IN_UCAST_PKTS" => Ok(Self::Ipv6InUcastPkts),
            "SAI_PORT_STAT_IPV6_IN_NON_UCAST_PKTS" => Ok(Self::Ipv6InNonUcastPkts),
            "SAI_PORT_STAT_IPV6_IN_MCAST_PKTS" => Ok(Self::Ipv6InMcastPkts),
            "SAI_PORT_STAT_IPV6_IN_DISCARDS" => Ok(Self::Ipv6InDiscards),
            "SAI_PORT_STAT_IPV6_OUT_OCTETS" => Ok(Self::Ipv6OutOctets),
            "SAI_PORT_STAT_IPV6_OUT_UCAST_PKTS" => Ok(Self::Ipv6OutUcastPkts),
            "SAI_PORT_STAT_IPV6_OUT_NON_UCAST_PKTS" => Ok(Self::Ipv6OutNonUcastPkts),
            "SAI_PORT_STAT_IPV6_OUT_MCAST_PKTS" => Ok(Self::Ipv6OutMcastPkts),
            "SAI_PORT_STAT_IPV6_OUT_DISCARDS" => Ok(Self::Ipv6OutDiscards),
            "SAI_PORT_STAT_GREEN_WRED_DROPPED_PACKETS" => Ok(Self::GreenWredDroppedPackets),
            "SAI_PORT_STAT_GREEN_WRED_DROPPED_BYTES" => Ok(Self::GreenWredDroppedBytes),
            "SAI_PORT_STAT_YELLOW_WRED_DROPPED_PACKETS" => Ok(Self::YellowWredDroppedPackets),
            "SAI_PORT_STAT_YELLOW_WRED_DROPPED_BYTES" => Ok(Self::YellowWredDroppedBytes),
            "SAI_PORT_STAT_RED_WRED_DROPPED_PACKETS" => Ok(Self::RedWredDroppedPackets),
            "SAI_PORT_STAT_RED_WRED_DROPPED_BYTES" => Ok(Self::RedWredDroppedBytes),
            "SAI_PORT_STAT_WRED_DROPPED_PACKETS" => Ok(Self::WredDroppedPackets),
            "SAI_PORT_STAT_WRED_DROPPED_BYTES" => Ok(Self::WredDroppedBytes),
            "SAI_PORT_STAT_ECN_MARKED_PACKETS" => Ok(Self::EcnMarkedPackets),
            "SAI_PORT_STAT_ETHER_IN_PKTS_64_OCTETS" => Ok(Self::EtherInPkts64Octets),
            "SAI_PORT_STAT_ETHER_IN_PKTS_65_TO_127_OCTETS" => Ok(Self::EtherInPkts65To127Octets),
            "SAI_PORT_STAT_ETHER_IN_PKTS_128_TO_255_OCTETS" => Ok(Self::EtherInPkts128To255Octets),
            "SAI_PORT_STAT_ETHER_IN_PKTS_256_TO_511_OCTETS" => Ok(Self::EtherInPkts256To511Octets),
            "SAI_PORT_STAT_ETHER_IN_PKTS_512_TO_1023_OCTETS" => {
                Ok(Self::EtherInPkts512To1023Octets)
            }
            "SAI_PORT_STAT_ETHER_IN_PKTS_1024_TO_1518_OCTETS" => {
                Ok(Self::EtherInPkts1024To1518Octets)
            }
            "SAI_PORT_STAT_ETHER_IN_PKTS_1519_TO_2047_OCTETS" => {
                Ok(Self::EtherInPkts1519To2047Octets)
            }
            "SAI_PORT_STAT_ETHER_IN_PKTS_2048_TO_4095_OCTETS" => {
                Ok(Self::EtherInPkts2048To4095Octets)
            }
            "SAI_PORT_STAT_ETHER_IN_PKTS_4096_TO_9216_OCTETS" => {
                Ok(Self::EtherInPkts4096To9216Octets)
            }
            "SAI_PORT_STAT_ETHER_IN_PKTS_9217_TO_16383_OCTETS" => {
                Ok(Self::EtherInPkts9217To16383Octets)
            }
            "SAI_PORT_STAT_ETHER_OUT_PKTS_64_OCTETS" => Ok(Self::EtherOutPkts64Octets),
            "SAI_PORT_STAT_ETHER_OUT_PKTS_65_TO_127_OCTETS" => Ok(Self::EtherOutPkts65To127Octets),
            "SAI_PORT_STAT_ETHER_OUT_PKTS_128_TO_255_OCTETS" => {
                Ok(Self::EtherOutPkts128To255Octets)
            }
            "SAI_PORT_STAT_ETHER_OUT_PKTS_256_TO_511_OCTETS" => {
                Ok(Self::EtherOutPkts256To511Octets)
            }
            "SAI_PORT_STAT_ETHER_OUT_PKTS_512_TO_1023_OCTETS" => {
                Ok(Self::EtherOutPkts512To1023Octets)
            }
            "SAI_PORT_STAT_ETHER_OUT_PKTS_1024_TO_1518_OCTETS" => {
                Ok(Self::EtherOutPkts1024To1518Octets)
            }
            "SAI_PORT_STAT_ETHER_OUT_PKTS_1519_TO_2047_OCTETS" => {
                Ok(Self::EtherOutPkts1519To2047Octets)
            }
            "SAI_PORT_STAT_ETHER_OUT_PKTS_2048_TO_4095_OCTETS" => {
                Ok(Self::EtherOutPkts2048To4095Octets)
            }
            "SAI_PORT_STAT_ETHER_OUT_PKTS_4096_TO_9216_OCTETS" => {
                Ok(Self::EtherOutPkts4096To9216Octets)
            }
            "SAI_PORT_STAT_ETHER_OUT_PKTS_9217_TO_16383_OCTETS" => {
                Ok(Self::EtherOutPkts9217To16383Octets)
            }
            "SAI_PORT_STAT_IN_CURR_OCCUPANCY_BYTES" => Ok(Self::InCurrOccupancyBytes),
            "SAI_PORT_STAT_IN_WATERMARK_BYTES" => Ok(Self::InWatermarkBytes),
            "SAI_PORT_STAT_IN_SHARED_CURR_OCCUPANCY_BYTES" => Ok(Self::InSharedCurrOccupancyBytes),
            "SAI_PORT_STAT_IN_SHARED_WATERMARK_BYTES" => Ok(Self::InSharedWatermarkBytes),
            "SAI_PORT_STAT_OUT_CURR_OCCUPANCY_BYTES" => Ok(Self::OutCurrOccupancyBytes),
            "SAI_PORT_STAT_OUT_WATERMARK_BYTES" => Ok(Self::OutWatermarkBytes),
            "SAI_PORT_STAT_OUT_SHARED_CURR_OCCUPANCY_BYTES" => {
                Ok(Self::OutSharedCurrOccupancyBytes)
            }
            "SAI_PORT_STAT_OUT_SHARED_WATERMARK_BYTES" => Ok(Self::OutSharedWatermarkBytes),
            "SAI_PORT_STAT_IN_DROPPED_PKTS" => Ok(Self::InDroppedPkts),
            "SAI_PORT_STAT_OUT_DROPPED_PKTS" => Ok(Self::OutDroppedPkts),
            "SAI_PORT_STAT_PAUSE_RX_PKTS" => Ok(Self::PauseRxPkts),
            "SAI_PORT_STAT_PAUSE_TX_PKTS" => Ok(Self::PauseTxPkts),
            "SAI_PORT_STAT_PFC_0_RX_PKTS" => Ok(Self::Pfc0RxPkts),
            "SAI_PORT_STAT_PFC_0_TX_PKTS" => Ok(Self::Pfc0TxPkts),
            "SAI_PORT_STAT_PFC_1_RX_PKTS" => Ok(Self::Pfc1RxPkts),
            "SAI_PORT_STAT_PFC_1_TX_PKTS" => Ok(Self::Pfc1TxPkts),
            "SAI_PORT_STAT_PFC_2_RX_PKTS" => Ok(Self::Pfc2RxPkts),
            "SAI_PORT_STAT_PFC_2_TX_PKTS" => Ok(Self::Pfc2TxPkts),
            "SAI_PORT_STAT_PFC_3_RX_PKTS" => Ok(Self::Pfc3RxPkts),
            "SAI_PORT_STAT_PFC_3_TX_PKTS" => Ok(Self::Pfc3TxPkts),
            "SAI_PORT_STAT_PFC_4_RX_PKTS" => Ok(Self::Pfc4RxPkts),
            "SAI_PORT_STAT_PFC_4_TX_PKTS" => Ok(Self::Pfc4TxPkts),
            "SAI_PORT_STAT_PFC_5_RX_PKTS" => Ok(Self::Pfc5RxPkts),
            "SAI_PORT_STAT_PFC_5_TX_PKTS" => Ok(Self::Pfc5TxPkts),
            "SAI_PORT_STAT_PFC_6_RX_PKTS" => Ok(Self::Pfc6RxPkts),
            "SAI_PORT_STAT_PFC_6_TX_PKTS" => Ok(Self::Pfc6TxPkts),
            "SAI_PORT_STAT_PFC_7_RX_PKTS" => Ok(Self::Pfc7RxPkts),
            "SAI_PORT_STAT_PFC_7_TX_PKTS" => Ok(Self::Pfc7TxPkts),
            "SAI_PORT_STAT_PFC_0_RX_PAUSE_DURATION" => Ok(Self::Pfc0RxPauseDuration),
            "SAI_PORT_STAT_PFC_0_TX_PAUSE_DURATION" => Ok(Self::Pfc0TxPauseDuration),
            "SAI_PORT_STAT_PFC_1_RX_PAUSE_DURATION" => Ok(Self::Pfc1RxPauseDuration),
            "SAI_PORT_STAT_PFC_1_TX_PAUSE_DURATION" => Ok(Self::Pfc1TxPauseDuration),
            "SAI_PORT_STAT_PFC_2_RX_PAUSE_DURATION" => Ok(Self::Pfc2RxPauseDuration),
            "SAI_PORT_STAT_PFC_2_TX_PAUSE_DURATION" => Ok(Self::Pfc2TxPauseDuration),
            "SAI_PORT_STAT_PFC_3_RX_PAUSE_DURATION" => Ok(Self::Pfc3RxPauseDuration),
            "SAI_PORT_STAT_PFC_3_TX_PAUSE_DURATION" => Ok(Self::Pfc3TxPauseDuration),
            "SAI_PORT_STAT_PFC_4_RX_PAUSE_DURATION" => Ok(Self::Pfc4RxPauseDuration),
            "SAI_PORT_STAT_PFC_4_TX_PAUSE_DURATION" => Ok(Self::Pfc4TxPauseDuration),
            "SAI_PORT_STAT_PFC_5_RX_PAUSE_DURATION" => Ok(Self::Pfc5RxPauseDuration),
            "SAI_PORT_STAT_PFC_5_TX_PAUSE_DURATION" => Ok(Self::Pfc5TxPauseDuration),
            "SAI_PORT_STAT_PFC_6_RX_PAUSE_DURATION" => Ok(Self::Pfc6RxPauseDuration),
            "SAI_PORT_STAT_PFC_6_TX_PAUSE_DURATION" => Ok(Self::Pfc6TxPauseDuration),
            "SAI_PORT_STAT_PFC_7_RX_PAUSE_DURATION" => Ok(Self::Pfc7RxPauseDuration),
            "SAI_PORT_STAT_PFC_7_TX_PAUSE_DURATION" => Ok(Self::Pfc7TxPauseDuration),
            "SAI_PORT_STAT_PFC_0_RX_PAUSE_DURATION_US" => Ok(Self::Pfc0RxPauseDurationUs),
            "SAI_PORT_STAT_PFC_0_TX_PAUSE_DURATION_US" => Ok(Self::Pfc0TxPauseDurationUs),
            "SAI_PORT_STAT_PFC_1_RX_PAUSE_DURATION_US" => Ok(Self::Pfc1RxPauseDurationUs),
            "SAI_PORT_STAT_PFC_1_TX_PAUSE_DURATION_US" => Ok(Self::Pfc1TxPauseDurationUs),
            "SAI_PORT_STAT_PFC_2_RX_PAUSE_DURATION_US" => Ok(Self::Pfc2RxPauseDurationUs),
            "SAI_PORT_STAT_PFC_2_TX_PAUSE_DURATION_US" => Ok(Self::Pfc2TxPauseDurationUs),
            "SAI_PORT_STAT_PFC_3_RX_PAUSE_DURATION_US" => Ok(Self::Pfc3RxPauseDurationUs),
            "SAI_PORT_STAT_PFC_3_TX_PAUSE_DURATION_US" => Ok(Self::Pfc3TxPauseDurationUs),
            "SAI_PORT_STAT_PFC_4_RX_PAUSE_DURATION_US" => Ok(Self::Pfc4RxPauseDurationUs),
            "SAI_PORT_STAT_PFC_4_TX_PAUSE_DURATION_US" => Ok(Self::Pfc4TxPauseDurationUs),
            "SAI_PORT_STAT_PFC_5_RX_PAUSE_DURATION_US" => Ok(Self::Pfc5RxPauseDurationUs),
            "SAI_PORT_STAT_PFC_5_TX_PAUSE_DURATION_US" => Ok(Self::Pfc5TxPauseDurationUs),
            "SAI_PORT_STAT_PFC_6_RX_PAUSE_DURATION_US" => Ok(Self::Pfc6RxPauseDurationUs),
            "SAI_PORT_STAT_PFC_6_TX_PAUSE_DURATION_US" => Ok(Self::Pfc6TxPauseDurationUs),
            "SAI_PORT_STAT_PFC_7_RX_PAUSE_DURATION_US" => Ok(Self::Pfc7RxPauseDurationUs),
            "SAI_PORT_STAT_PFC_7_TX_PAUSE_DURATION_US" => Ok(Self::Pfc7TxPauseDurationUs),
            "SAI_PORT_STAT_PFC_0_ON2OFF_RX_PKTS" => Ok(Self::Pfc0On2OffRxPkts),
            "SAI_PORT_STAT_PFC_1_ON2OFF_RX_PKTS" => Ok(Self::Pfc1On2OffRxPkts),
            "SAI_PORT_STAT_PFC_2_ON2OFF_RX_PKTS" => Ok(Self::Pfc2On2OffRxPkts),
            "SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS" => Ok(Self::Pfc3On2OffRxPkts),
            "SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS" => Ok(Self::Pfc4On2OffRxPkts),
            "SAI_PORT_STAT_PFC_5_ON2OFF_RX_PKTS" => Ok(Self::Pfc5On2OffRxPkts),
            "SAI_PORT_STAT_PFC_6_ON2OFF_RX_PKTS" => Ok(Self::Pfc6On2OffRxPkts),
            "SAI_PORT_STAT_PFC_7_ON2OFF_RX_PKTS" => Ok(Self::Pfc7On2OffRxPkts),
            "SAI_PORT_STAT_DOT3_STATS_ALIGNMENT_ERRORS" => Ok(Self::Dot3StatsAlignmentErrors),
            "SAI_PORT_STAT_DOT3_STATS_FCS_ERRORS" => Ok(Self::Dot3StatsFcsErrors),
            "SAI_PORT_STAT_DOT3_STATS_SINGLE_COLLISION_FRAMES" => {
                Ok(Self::Dot3StatsSingleCollisionFrames)
            }
            "SAI_PORT_STAT_DOT3_STATS_MULTIPLE_COLLISION_FRAMES" => {
                Ok(Self::Dot3StatsMultipleCollisionFrames)
            }
            "SAI_PORT_STAT_DOT3_STATS_SQE_TEST_ERRORS" => Ok(Self::Dot3StatsSqeTestErrors),
            "SAI_PORT_STAT_DOT3_STATS_DEFERRED_TRANSMISSIONS" => {
                Ok(Self::Dot3StatsDeferredTransmissions)
            }
            "SAI_PORT_STAT_DOT3_STATS_LATE_COLLISIONS" => Ok(Self::Dot3StatsLateCollisions),
            "SAI_PORT_STAT_DOT3_STATS_EXCESSIVE_COLLISIONS" => {
                Ok(Self::Dot3StatsExcessiveCollisions)
            }
            "SAI_PORT_STAT_DOT3_STATS_INTERNAL_MAC_TRANSMIT_ERRORS" => {
                Ok(Self::Dot3StatsInternalMacTransmitErrors)
            }
            "SAI_PORT_STAT_DOT3_STATS_CARRIER_SENSE_ERRORS" => {
                Ok(Self::Dot3StatsCarrierSenseErrors)
            }
            "SAI_PORT_STAT_DOT3_STATS_FRAME_TOO_LONGS" => Ok(Self::Dot3StatsFrameTooLongs),
            "SAI_PORT_STAT_DOT3_STATS_INTERNAL_MAC_RECEIVE_ERRORS" => {
                Ok(Self::Dot3StatsInternalMacReceiveErrors)
            }
            "SAI_PORT_STAT_DOT3_STATS_SYMBOL_ERRORS" => Ok(Self::Dot3StatsSymbolErrors),
            "SAI_PORT_STAT_DOT3_CONTROL_IN_UNKNOWN_OPCODES" => {
                Ok(Self::Dot3ControlInUnknownOpcodes)
            }
            "SAI_PORT_STAT_EEE_TX_EVENT_COUNT" => Ok(Self::EeeTxEventCount),
            "SAI_PORT_STAT_EEE_RX_EVENT_COUNT" => Ok(Self::EeeRxEventCount),
            "SAI_PORT_STAT_EEE_TX_DURATION" => Ok(Self::EeeTxDuration),
            "SAI_PORT_STAT_EEE_RX_DURATION" => Ok(Self::EeeRxDuration),
            "SAI_PORT_STAT_PRBS_ERROR_COUNT" => Ok(Self::PrbsErrorCount),
            "SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES" => Ok(Self::IfInFecCorrectableFrames),
            "SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES" => {
                Ok(Self::IfInFecNotCorrectableFrames)
            }
            "SAI_PORT_STAT_IF_IN_FEC_SYMBOL_ERRORS" => Ok(Self::IfInFecSymbolErrors),
            "SAI_PORT_STAT_IF_IN_FABRIC_DATA_UNITS" => Ok(Self::IfInFabricDataUnits),
            "SAI_PORT_STAT_IF_OUT_FABRIC_DATA_UNITS" => Ok(Self::IfOutFabricDataUnits),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S0" => Ok(Self::IfInFecCodewordErrorsS0),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S1" => Ok(Self::IfInFecCodewordErrorsS1),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S2" => Ok(Self::IfInFecCodewordErrorsS2),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S3" => Ok(Self::IfInFecCodewordErrorsS3),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S4" => Ok(Self::IfInFecCodewordErrorsS4),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S5" => Ok(Self::IfInFecCodewordErrorsS5),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S6" => Ok(Self::IfInFecCodewordErrorsS6),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S7" => Ok(Self::IfInFecCodewordErrorsS7),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S8" => Ok(Self::IfInFecCodewordErrorsS8),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S9" => Ok(Self::IfInFecCodewordErrorsS9),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S10" => Ok(Self::IfInFecCodewordErrorsS10),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S11" => Ok(Self::IfInFecCodewordErrorsS11),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S12" => Ok(Self::IfInFecCodewordErrorsS12),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S13" => Ok(Self::IfInFecCodewordErrorsS13),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S14" => Ok(Self::IfInFecCodewordErrorsS14),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S15" => Ok(Self::IfInFecCodewordErrorsS15),
            "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S16" => Ok(Self::IfInFecCodewordErrorsS16),
            "SAI_PORT_STAT_IF_IN_FEC_CORRECTED_BITS" => Ok(Self::IfInFecCorrectedBits),
            "SAI_PORT_STAT_TRIM_PACKETS" => Ok(Self::TrimPackets),
            "SAI_PORT_STAT_DROPPED_TRIM_PACKETS" => Ok(Self::DroppedTrimPackets),
            "SAI_PORT_STAT_TX_TRIM_PACKETS" => Ok(Self::TxTrimPackets),
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_0_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons0DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_1_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons1DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_2_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons2DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_3_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons3DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_4_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons4DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_5_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons5DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_6_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons6DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_7_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons7DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_8_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons8DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_9_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons9DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_10_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons10DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_11_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons11DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_12_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons12DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_13_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons13DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_14_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons14DroppedPkts)
            }
            "SAI_PORT_STAT_IN_CONFIGURED_DROP_REASONS_15_DROPPED_PKTS" => {
                Ok(Self::InConfiguredDropReasons15DroppedPkts)
            }
            "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_0_DROPPED_PKTS" => {
                Ok(Self::OutConfiguredDropReasons0DroppedPkts)
            }
            "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_1_DROPPED_PKTS" => {
                Ok(Self::OutConfiguredDropReasons1DroppedPkts)
            }
            "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_2_DROPPED_PKTS" => {
                Ok(Self::OutConfiguredDropReasons2DroppedPkts)
            }
            "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_3_DROPPED_PKTS" => {
                Ok(Self::OutConfiguredDropReasons3DroppedPkts)
            }
            "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_4_DROPPED_PKTS" => {
                Ok(Self::OutConfiguredDropReasons4DroppedPkts)
            }
            "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_5_DROPPED_PKTS" => {
                Ok(Self::OutConfiguredDropReasons5DroppedPkts)
            }
            "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_6_DROPPED_PKTS" => {
                Ok(Self::OutConfiguredDropReasons6DroppedPkts)
            }
            "SAI_PORT_STAT_OUT_CONFIGURED_DROP_REASONS_7_DROPPED_PKTS" => {
                Ok(Self::OutConfiguredDropReasons7DroppedPkts)
            }
            "SAI_PORT_STAT_IF_IN_HW_PROTECTION_SWITCHOVER_EVENTS" => {
                Ok(Self::IfInHwProtectionSwitchoverEvents)
            }
            "SAI_PORT_STAT_IF_IN_HW_PROTECTION_SWITCHOVER_DROP_PKTS" => {
                Ok(Self::IfInHwProtectionSwitchoverDropPkts)
            }
            "SAI_PORT_STAT_ETHER_IN_PKTS_1519_TO_2500_OCTETS" => {
                Ok(Self::EtherInPkts1519To2500Octets)
            }
            "SAI_PORT_STAT_ETHER_IN_PKTS_2501_TO_9000_OCTETS" => {
                Ok(Self::EtherInPkts2501To9000Octets)
            }
            "SAI_PORT_STAT_ETHER_IN_PKTS_9001_TO_16383_OCTETS" => {
                Ok(Self::EtherInPkts9001To16383Octets)
            }
            "SAI_PORT_STAT_ETHER_OUT_PKTS_1519_TO_2500_OCTETS" => {
                Ok(Self::EtherOutPkts1519To2500Octets)
            }
            "SAI_PORT_STAT_ETHER_OUT_PKTS_2501_TO_9000_OCTETS" => {
                Ok(Self::EtherOutPkts2501To9000Octets)
            }
            "SAI_PORT_STAT_ETHER_OUT_PKTS_9001_TO_16383_OCTETS" => {
                Ok(Self::EtherOutPkts9001To16383Octets)
            }
            "SAI_PORT_STAT_END" => Ok(Self::End),
            _ => Err(format!("Unknown SAI port stat: {}", s)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_basic_conversion() {
        assert_eq!(SaiPortStat::IfInOctets.to_u32(), 0);
        assert_eq!(SaiPortStat::IfInUcastPkts.to_u32(), 1);
        assert_eq!(SaiPortStat::Pfc0RxPkts.to_u32(), 103);
        assert_eq!(SaiPortStat::End.to_u32(), 0x00002010);
    }

    #[test]
    fn test_from_u32() {
        assert_eq!(SaiPortStat::from_u32(0), Some(SaiPortStat::IfInOctets));
        assert_eq!(SaiPortStat::from_u32(1), Some(SaiPortStat::IfInUcastPkts));
        assert_eq!(SaiPortStat::from_u32(103), Some(SaiPortStat::Pfc0RxPkts));
        assert_eq!(
            SaiPortStat::from_u32(0x00001000),
            Some(SaiPortStat::InConfiguredDropReasons0DroppedPkts)
        );
        assert_eq!(SaiPortStat::from_u32(0x00002010), Some(SaiPortStat::End));
        assert_eq!(SaiPortStat::from_u32(999999), None);
    }

    #[test]
    fn test_string_conversion() {
        let stat = SaiPortStat::IfInOctets;
        assert_eq!(stat.to_string(), "SAI_PORT_STAT_IF_IN_OCTETS");
        assert_eq!(
            "SAI_PORT_STAT_IF_IN_OCTETS".parse::<SaiPortStat>().unwrap(),
            stat
        );

        let pfc_stat = SaiPortStat::Pfc0RxPkts;
        assert_eq!(pfc_stat.to_string(), "SAI_PORT_STAT_PFC_0_RX_PKTS");
        assert_eq!(
            "SAI_PORT_STAT_PFC_0_RX_PKTS"
                .parse::<SaiPortStat>()
                .unwrap(),
            pfc_stat
        );

        // Test that both START and IF_IN_OCTETS parse to the same enum value
        assert_eq!(
            "SAI_PORT_STAT_START".parse::<SaiPortStat>().unwrap(),
            SaiPortStat::IfInOctets
        );
    }
}
