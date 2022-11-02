#pragma once

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <set>

namespace swss {

struct TunnelInfo
{
    std::string type;
    std::string dst_ip;
    std::string remote_ip;
};

class TunnelMgr : public Orch
{
public:
    TunnelMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    void doTask(Consumer &consumer);

    bool doTunnelTask(const KeyOpFieldsValuesTuple & t);
    bool doTunnelRouteTask(const KeyOpFieldsValuesTuple & t);
    bool doLpbkIntfTask(const KeyOpFieldsValuesTuple & t);

    bool configIpTunnel(const TunnelInfo& info);

    void finalizeWarmReboot();

    ProducerStateTable m_appIpInIpTunnelTable;
    Table m_cfgPeerTable;
    Table m_cfgTunnelTable;

    std::map<std::string, TunnelInfo > m_tunnelCache;
    std::map<std::string, IpPrefix> m_intfCache;
    std::string m_peerIp;

    std::set<std::string> m_tunnelReplay;
    bool replayDone = false;
};

}
