#ifndef __MACSYNC__
#define __MACSYNC__

#include <linux/rtnetlink.h>
#include <linux/neighbour.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "dbconnector.h"
#include "producerstatetable.h"
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

    bool isFpmMode() const { return m_fpmMode; }

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
    void sendLocalMac(const std::string& vlanName, const std::string& mac,
                      LocalMac& local, bool add);
    void writeRemoteMac(const std::string& key, const std::vector<FieldValueTuple>& row);
    void removeRemoteMac(const std::string& key);

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
    };
    std::map<std::string, RemoteRow> m_remoteRows;
};

}

#endif
