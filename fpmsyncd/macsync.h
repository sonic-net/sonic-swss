#ifndef __MACSYNC__
#define __MACSYNC__

#include <linux/rtnetlink.h>
#include <linux/neighbour.h>

#include <functional>
#include <string>

#include "dbconnector.h"
#include "subscriberstatetable.h"
#include "table.h"
#include "fpmsyncd/fpminterface.h"

namespace swss {

/*
 * MAC (bridge FDB) synchronization with zebra over the FPM channel.
 *
 * Local direction : STATE_DB FDB_TABLE -> AF_BRIDGE RTM_NEWNEIGH/RTM_DELNEIGH -> zebra.
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

    void onFpmConnected(FpmInterface& fpm);
    void onFpmDisconnected();

    bool isFpmMode() const { return m_fpmMode; }

private:
    /* Local MACs already advertised to zebra, keyed "Vlan<id>:<mac>". */
    struct LocalMac
    {
        std::string port;
        bool isStatic;
        /* Last ifindex of the port, so a delete still reaches zebra once the port is gone. */
        unsigned int ifindex;
    };

    void readCfgFdbSyncMode();
    void setMacSyncMode(const std::string& mode);
    void processStateFdbEntry(const KeyOpFieldsValuesTuple& entry);
    void sendLocalMac(const std::string& vlanName, const std::string& mac,
                      LocalMac& local, bool add);

    SubscriberStateTable m_stateFdbTable;
    SubscriberStateTable m_cfgFdbSyncTable;
    Table m_cfgFdbSyncTableRead;

    FpmInterface *m_fpmInterface {nullptr};
    bool m_fpmMode {false};

    /* An L3EvpnMH device always syncs MACs over FPM, whatever FDB_SYNC says. */
    bool m_l3EvpnMh {false};

    /* if_nametoindex(), replaceable so a test can take a port away. */
    std::function<unsigned int(const std::string&)> m_ifNameToIndex;

    std::map<std::string, LocalMac> m_localMacs;
};

}

#endif
