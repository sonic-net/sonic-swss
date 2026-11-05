#ifndef PFC_WD_COUNTERS_H
#define PFC_WD_COUNTERS_H

#include <cstdint>

// COUNTERS_DB schema shared by the software (PfcWdActionHandler) and hardware
// (PfcWdHwOrch) PFC watchdogs.
#define PFC_WD_QUEUE_STATUS             "PFC_WD_STATUS"
#define PFC_WD_QUEUE_STATUS_OPERATIONAL "operational"
#define PFC_WD_QUEUE_STATUS_STORMED     "stormed"

#define PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED "PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED"
#define PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED "PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED"

#define PFC_WD_QUEUE_STATS_TX_PACKETS "PFC_WD_QUEUE_STATS_TX_PACKETS"
#define PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS "PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS"
#define PFC_WD_QUEUE_STATS_RX_PACKETS "PFC_WD_QUEUE_STATS_RX_PACKETS"
#define PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS "PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS"

#define PFC_WD_QUEUE_STATS_TX_PACKETS_LAST "PFC_WD_QUEUE_STATS_TX_PACKETS_LAST"
#define PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST "PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST"
#define PFC_WD_QUEUE_STATS_RX_PACKETS_LAST "PFC_WD_QUEUE_STATS_RX_PACKETS_LAST"
#define PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST "PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST"

// STATE_DB, keyed <port_alias>|<queue_index>; times in microseconds since the epoch.
// PFC_WD_STATE: current storm state. PFC_WD_STORM_HISTORY: last_storm and storm_<N>_*.
#define PFC_WD_QUEUE_STATE_TABLE                 "PFC_WD_STATE"
#define PFC_WD_QUEUE_STATE_STATUS                "status"
#define PFC_WD_QUEUE_STATE_STATUS_DETECTED       "storm_detected"
#define PFC_WD_QUEUE_STATE_STATUS_RESTORED       "storm_restored"
#define PFC_WD_QUEUE_STATE_DETECTION_COUNT       "detection_count"
#define PFC_WD_QUEUE_STATE_RESTORATION_COUNT     "restoration_count"
#define PFC_WD_QUEUE_STATE_LAST_DETECTION_TIME   "last_detection_time"
#define PFC_WD_QUEUE_STATE_LAST_RESTORATION_TIME "last_restoration_time"
#define PFC_WD_QUEUE_STATE_STORM_DURATION_MS     "storm_duration_ms"

#define PFC_WD_STORM_HISTORY_TABLE                 "PFC_WD_STORM_HISTORY"
#define PFC_WD_STORM_HISTORY_DEPTH                 10
#define PFC_WD_STORM_HISTORY_LAST_STORM            "last_storm"
// storm_<N>_ suffixes
#define PFC_WD_STORM_HISTORY_DETECTED              "detected"
#define PFC_WD_STORM_HISTORY_RESTORED              "restored"
#define PFC_WD_STORM_HISTORY_DURATION_MS           "duration_ms"
#define PFC_WD_STORM_HISTORY_TX_PACKETS            "tx_packets"
#define PFC_WD_STORM_HISTORY_TX_DROPPED            "tx_dropped"
#define PFC_WD_STORM_HISTORY_RX_PACKETS            "rx_packets"
#define PFC_WD_STORM_HISTORY_RX_DROPPED            "rx_dropped"
// Set instead of restored when the storm's handling ended without a restoration
#define PFC_WD_STORM_HISTORY_INTERRUPTED           "interrupted"

// One sample of a monitored queue's raw counters: from SAI for the software
// watchdog, from COUNTERS_DB for the hardware one.
struct PfcWdHwStats
{
    uint64_t txPkt;
    uint64_t txDropPkt;
    uint64_t rxPkt;
    uint64_t rxDropPkt;
};

// The watchdog's own per-queue counters as persisted in COUNTERS_DB.
struct PfcWdQueueStats
{
    uint64_t detectCount;
    uint64_t restoreCount;
    uint64_t txPkt;
    uint64_t txDropPkt;
    uint64_t rxPkt;
    uint64_t rxDropPkt;
    uint64_t txPktLast;
    uint64_t txDropPktLast;
    uint64_t rxPktLast;
    uint64_t rxDropPktLast;
    bool     operational;
};

#endif
