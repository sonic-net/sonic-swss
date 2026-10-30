#ifndef __MACSYNC__
#define __MACSYNC__

#include <linux/rtnetlink.h>
#include <linux/neighbour.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dbconnector.h"
#include "producerstatetable.h"
#include "selectabletimer.h"
#include "subscriberstatetable.h"
#include "table.h"
#include "fpmsyncd/fpminterface.h"

#ifndef CFG_FDB_SYNC_TABLE_NAME
// TODO: drop once sonic-net/sonic-buildimage#30365 is merged.
#define CFG_FDB_SYNC_TABLE_NAME "FDB_SYNC"
#endif

/* The mode fpmsyncd runs, which fdbsyncd follows. */
#ifndef STATE_FDB_SYNC_TABLE_NAME
#define STATE_FDB_SYNC_TABLE_NAME "FDB_SYNC"
#endif

namespace swss {

/*
 * MAC (bridge FDB) synchronization with zebra over the FPM channel.
 *
 * Local direction : STATE_DB FDB_TABLE -> AF_BRIDGE RTM_NEWNEIGH/RTM_DELNEIGH -> zebra.
 * Remote direction: AF_BRIDGE RTM_NEWNEIGH/RTM_DELNEIGH from zebra -> APP_VXLAN_FDB_TABLE.
 *
 * Active only when CONFIG_DB FDB_SYNC|global mac_sync_mode is "fpm" at startup. The
 * mode is read once, as zebra reads --kernel-mac-ext-learn, so a change applies on
 * the next bgp restart, and is published in STATE_DB FDB_SYNC|global for fdbsyncd to
 * follow. In "kernel" mode every entry point is a no-op so behaviour is identical to
 * fdbsyncd's netlink path.
 */
class MacSync
{
public:
    MacSync(RedisPipeline *pipeline, DBConnector *stateDb, DBConnector *cfgDb);

    SubscriberStateTable *getStateFdbTable() { return &m_stateFdbTable; }

    /* STATE_DB FDB_TABLE updates: local MACs toward zebra. */
    void processStateFdb();

    /* Inbound AF_BRIDGE neighbour message from zebra: remote MACs toward APPL_DB. */
    void onMacMsg(struct nlmsghdr *h, int len);

    void onFpmConnected(FpmInterface& fpm);
    void onFpmDisconnected();

    /* A VxLAN netdev was deleted: withdraw the remote MACs that came from it. */
    void onVxlanLinkDel(const std::string& ifname);

    /* Reads the link messages already queued. A netdev's delete is queued
     * before zebra can see a new netdev under its name, so reading them before
     * each FPM MAC message withdraws the old netdev's rows before the new one's
     * are written. */
    void setLinkReader(std::function<void()> reader) { m_readLinks = std::move(reader); }

    /*
     * On every FPM connect the remote MACs in APPL_DB are stale until zebra
     * sends them again: zebra replays what it holds, and a MAC it withdrew
     * while fpmsyncd or the connection was down is never withdrawn otherwise.
     * warmRestartTimer is the bgp warm restart timer when warm restart is
     * enabled, 0 otherwise. In fpmsyncd's warm start onStaleSweepTimer()
     * withdraws the rest at that timer, otherwise once zebra's replay and
     * BGP's convergence are over. markRemoteMacsStale() returns the seconds
     * to the first onStaleSweepTimer(), 0 when nothing is stale.
     */
    static constexpr time_t STALE_SWEEP_HOLD = 120;
    static constexpr time_t STALE_SWEEP_QUIET = 10;
    static constexpr time_t STALE_SWEEP_MAX = 600;
    time_t markRemoteMacsStale(time_t warmRestartTimer,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    time_t onStaleSweepTimer(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void noteFpmMessage(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
    {
        m_lastFpmMessage = now;
    }

    /* zebra now holds a port as a local Ethernet Segment port in a VLAN, or
     * no longer does; dfKey is that EVPN_DF_TABLE key, "Vlan<id>:<port>". */
    void onEsPortVlan(const std::string& dfKey, bool present);

    /* The warm restart window is over: the EVPN_DF_TABLE keys read at start
     * that zebra has not sent again are taken as gone. */
    void onWarmStartEnd();

    /* Sends the local MACs whose re-add onEsPortVlan() delayed. */
    SelectableTimer *getEsReAddTimer() { return &m_esReAddTimer; }
    void processEsReAdd(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    static constexpr long ES_READD_DELAY_MS = 1000;

    /*
     * A kernel nexthop message. zebra programs an Ethernet Segment's L2 nexthop
     * group into the kernel, not over FPM, and deleting a group flushes the
     * kernel FDB entries that use it; zebra can free a MAC on the group without
     * a delete of its own. So NHG_FLUSH_DELAY_MS after a group's delete, the
     * rows the delete took are withdrawn: all the rows on it once the kernel
     * confirms the group is gone, or the ones sent before a group under the
     * same id was created.
     */
    void onKernelNexthop(struct nlmsghdr *h);
    SelectableTimer *getNhgFlushTimer() { return &m_nhgFlushTimer; }
    void processNhgFlush(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    static constexpr long NHG_FLUSH_DELAY_MS = 1000;
    /* Deletes due this much later are flushed together. */
    static constexpr long NHG_FLUSH_BATCH_MS = 100;
    /* Kernel lookups with no clear answer before the rows are kept. */
    static constexpr int NHG_FLUSH_TRIES = 3;

    /* Reads the kernel nexthop messages already queued. zebra creates a group
     * in the kernel before it sends MACs on it, so reading them before each
     * FPM MAC message puts a group's delete and creation in order with them. */
    void setKernelNexthopReader(std::function<void()> reader) { m_readKernelNexthops = std::move(reader); }

    /* What the kernel holds on a nexthop id: an FDB nexthop, none (no such id,
     * or a routing nexthop), or no clear answer. */
    enum class NexthopState { Fdb, Absent, Unknown };
    /* Parses an RTM_GETNEXTHOP answer; false when buf holds none for seq. */
    static bool parseNexthopReply(char *buf, size_t len, uint32_t seq, NexthopState& state);

    bool isFpmMode() const { return m_fpmMode; }

    /* MacSync has written VXLAN_FDB_TABLE since the pipeline was last empty:
     * the caller flushes it, which it otherwise holds during a warm restart. */
    bool hasPendingWrites() const { return m_pendingWrites; }
    void clearPendingWrites() { m_pendingWrites = false; }

private:
    /* Local MACs already advertised to zebra, keyed "Vlan<id>:<mac>". */
    struct LocalMac
    {
        std::string port;
        bool isStatic;
        /* The ifindex zebra last heard for this MAC, 0 until it is sent. The
         * delete goes there, whatever port the entry names by then. */
        unsigned int ifindex;
    };

    void readCfgFdbSyncMode();
    bool isEthernetSegmentInterface(const std::string& ifname);
    void processStateFdbEntry(const KeyOpFieldsValuesTuple& entry);
    /* False when nothing was sent. */
    bool sendLocalMac(const std::string& vlanName, const std::string& mac,
                      LocalMac& local, bool add);
    void writeRemoteMac(const std::string& key, const std::vector<FieldValueTuple>& row, uint64_t seq);
    void removeRemoteMac(const std::string& key);
    void withdrawStaleRemoteMacs();
    struct HeldRow;
    void releaseEsReAddDeferred(const std::string& key);
    void armEsReAddTimer(std::chrono::steady_clock::time_point now);
    void armNhgFlushTimer(std::chrono::steady_clock::time_point now);

    ProducerStateTable m_vxlanFdbTable;
    Table m_vxlanFdbTableRead;
    SubscriberStateTable m_stateFdbTable;
    Table m_stateFdbSyncTable;
    Table m_cfgFdbSyncTableRead;
    Table m_cfgEvpnEsTable;

    FpmInterface *m_fpmInterface {nullptr};
    bool m_fpmMode {false};

    /* An L3EvpnMH device always syncs MACs over FPM, whatever FDB_SYNC says. */
    bool m_l3EvpnMh {false};

    /* if_nametoindex(), replaceable so a test can take a port away. */
    std::function<unsigned int(const std::string&)> m_ifNameToIndex;

    /* if_indextoname(), empty when the index does not resolve; replaceable so a
     * test can name a VxLAN netdev. */
    std::function<std::string(unsigned int)> m_ifIndexToName;

    std::function<void()> m_readLinks;

    std::map<std::string, LocalMac> m_localMacs;

    /* The remote MAC rows MacSync has queued, keyed like VXLAN_FDB_TABLE, as
     * zebra sent them. APPL_DB shows a row once orchagent has read it, so the
     * rows are looked up here before APPL_DB. */
    struct RemoteRow
    {
        std::vector<FieldValueTuple> row;
        /* m_remoteMacSeq when zebra sent it, which orders it against a nexthop
         * group's delete. */
        uint64_t seq {0};
    };
    std::map<std::string, RemoteRow> m_remoteRows;
    /* The remote rows at the last connect, in APPL_DB or queued, that zebra
     * has not sent or withdrawn since. zebra's replay rewrites a row that
     * differs; an empty one always does. */
    std::map<std::string, std::vector<FieldValueTuple>> m_staleRemoteMacs;
    std::chrono::steady_clock::time_point m_staleSince;
    std::chrono::steady_clock::time_point m_lastFpmMessage;
    /* zebra has sent a remote MAC since the rows were marked stale. */
    bool m_remoteMacSinceStale {false};
    bool m_markedBefore {false};
    bool m_pendingWrites {false};
    /* The stale rows go at this many seconds after the connect, whatever the
     * channel does; 0 when the settle rule applies. */
    time_t m_staleWarmTimer {0};
    /* fpmsyncd's warm start has not withdrawn its stale rows yet, so a
     * reconnect before then is part of the warm start. */
    bool m_warmSweepPending {false};

    /* EVPN_DF_TABLE keys zebra holds: the VLANs of each local ES port. */
    std::set<std::string> m_esDfKeys;
    /* The keys found in APPL_DB at start that zebra has not sent since. */
    std::set<std::string> m_esDfUnconfirmed;

    /* Local MACs withdrawn from zebra by onEsPortVlan(), and when to add them back. */
    std::map<std::string, std::chrono::steady_clock::time_point> m_esReAddPending;
    SelectableTimer m_esReAddTimer;

    /* A remote MAC zebra sent for a MAC with a re-add pending, written only if
     * the hardware loses the MAC first. */
    struct HeldRow
    {
        /* The netdev zebra sent it on. */
        unsigned int ifindex;
        std::vector<FieldValueTuple> row;
        /* m_remoteMacSeq when zebra sent it. */
        uint64_t seq {0};
    };
    std::map<std::string, HeldRow> m_esReAddDeferred;
    /* Re-added MACs, and until when zebra's earlier answer is dropped. */
    std::map<std::string, std::chrono::steady_clock::time_point> m_esReAddGrace;

    /* Asks the kernel about a nexthop id; replaceable so a test can answer. */
    std::function<NexthopState(uint32_t)> m_kernelNexthopState;
    std::function<void()> m_readKernelNexthops;
    /* Remote MACs zebra has sent, counted so rows can be told from a group's delete. */
    uint64_t m_remoteMacSeq {0};

    /* FDB nexthop groups the kernel has deleted and not created again. */
    std::set<uint32_t> m_deletedNhgs;

    struct NhgFlush
    {
        std::chrono::steady_clock::time_point due;
        /* m_remoteMacSeq at the delete, or at the group's creation after it. */
        uint64_t seq;
        /* An RTM_NEWNEXTHOP followed the delete. */
        bool recreated;
        int tries;
    };
    /* Deleted nexthop groups whose rows are still to withdraw. */
    std::map<uint32_t, NhgFlush> m_nhgFlushPending;
    SelectableTimer m_nhgFlushTimer;
};

}

#endif
