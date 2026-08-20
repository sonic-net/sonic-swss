#ifndef __MACSYNC__
#define __MACSYNC__

#include <linux/rtnetlink.h>
#include <linux/neighbour.h>
#include <linux/nexthop.h>

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

namespace swss {

/*
 * MAC (bridge FDB) synchronization with zebra over the FPM channel.
 *
 * Local direction : STATE_DB FDB_TABLE -> AF_BRIDGE RTM_NEWNEIGH/RTM_DELNEIGH -> zebra.
 * Remote direction: AF_BRIDGE RTM_NEWNEIGH/RTM_DELNEIGH from zebra -> APP_VXLAN_FDB_TABLE.
 *
 * Active only while CONFIG_DB FDB_SYNC|global mac_sync_mode is "fpm"; in "kernel"
 * mode every entry point is a no-op so behaviour is identical to fdbsyncd's
 * netlink path.
 */
class MacSync
{
public:
    MacSync(RedisPipeline *pipeline, DBConnector *stateDb, DBConnector *cfgDb);

    SubscriberStateTable *getStateFdbTable() { return &m_stateFdbTable; }
    SubscriberStateTable *getCfgFdbSyncTable() { return &m_cfgFdbSyncTable; }

    /* CONFIG_DB FDB_SYNC|global updates. */
    void processCfgFdbSync();

    /* STATE_DB FDB_TABLE updates: local MACs toward zebra. */
    void processStateFdb();

    /* Inbound AF_BRIDGE neighbour message from zebra: remote MACs toward APPL_DB. */
    void onMacMsg(struct nlmsghdr *h, int len);

    /* Inbound nexthop message from zebra. An FDB nexthop is an EVPN Ethernet
     * Segment destination rather than an L3 one, so it is consumed here and
     * must not reach the route path. Returns true when it was an FDB nexthop. */
    bool onFdbNhgMsg(struct nlmsghdr *h, int len);

    /* zebra finished replaying its remote MACs. */
    void onRemoteReplayEnd();

    void onFpmConnected(FpmInterface& fpm);
    void onFpmDisconnected();

    bool isFpmMode() const { return m_fpmMode; }

private:
    /* Local MACs already advertised to zebra, keyed "Vlan<id>:<mac>". */
    struct LocalMac
    {
        std::string port;
        bool isStatic;
    };

    /* An FDB nexthop is either a single remote VTEP or a group of other FDB
     * nexthop ids. Kept so a group can be validated against its members and
     * re-derived when one of them is withdrawn. */
    enum L2NhgType
    {
        L2_NHG_TYPE_VTEP,
        L2_NHG_TYPE_GROUP,
    };

    struct L2NhgInfo
    {
        L2NhgType type;
        std::string vtepIp;
        std::vector<uint32_t> memberIds;
    };

    void readCfgFdbSyncMode();
    void setMacSyncMode(const std::string& mode);
    bool isEthernetSegmentInterface(const std::string& ifname);
    void sendLocalMac(const std::string& vlanName, const std::string& mac,
                      const std::string& port, bool isStatic, bool add);
    void loadLocalMacs();
    void loadRemoteMacs();
    uint32_t nextGeneration();
    void replayLocalMacs();
    void sendReplayEnd();

    ProducerStateTable m_vxlanFdbTable;
    Table m_vxlanFdbTableRead;
    ProducerStateTable m_l2NhgTable;
    SubscriberStateTable m_stateFdbTable;
    SubscriberStateTable m_cfgFdbSyncTable;
    Table m_cfgFdbSyncTableRead;
    Table m_cfgEvpnEsTable;
    Table m_stateFdbTableRead;
    Table m_stateFdbSyncTable;

    FpmInterface *m_fpmInterface {nullptr};
    bool m_fpmMode {false};

    /* Carried in nlmsg_seq so zebra can drop MACs left over from a previous
     * session once the replay ends. Kept in STATE_DB because it has to differ
     * from the one the previous fpmsyncd process sent. */
    uint32_t m_generation {0};

    std::map<std::string, LocalMac> m_localMacs;
    std::set<std::string> m_remoteMacs;

    /* Remote MACs zebra mentioned since it connected. Entries in m_remoteMacs
     * missing from this set at end of replay no longer exist. */
    std::set<std::string> m_remoteMacsSeen;
    bool m_remoteReplayPending {false};

    std::map<uint32_t, L2NhgInfo> m_l2Nhgs;
};

}

#endif
