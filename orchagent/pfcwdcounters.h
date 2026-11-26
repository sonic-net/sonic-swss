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
