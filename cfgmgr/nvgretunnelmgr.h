#ifndef __NVGRETUNNELMGR__
#define __NVGRETUNNELMGR__

#include "dbconnector.h"
#include "orch.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef CFG_NVGRE_TUNNEL_TABLE_NAME
#define CFG_NVGRE_TUNNEL_TABLE_NAME     "NVGRE_TUNNEL"
#endif
#ifndef CFG_NVGRE_TUNNEL_MAP_TABLE_NAME
#define CFG_NVGRE_TUNNEL_MAP_TABLE_NAME "NVGRE_TUNNEL_MAP"
#endif
#ifndef STATE_NVGRE_TUNNEL_TABLE_NAME
#define STATE_NVGRE_TUNNEL_TABLE_NAME   "NVGRE_TUNNEL_TABLE"
#endif

#define NVGRE_FIELD_SRC_IP              "src_ip"
#define NVGRE_MAP_FIELD_TUNNEL_NAME     "tunnel_name"
#define NVGRE_MAP_FIELD_VSID            "vsid"
#define NVGRE_MAP_FIELD_VNI             "vni"

namespace swss {

/*
 * NvgreTunnelMgr — switchdev NVGRE tunnel manager.
 *
 * Reads CONFIG_DB NVGRE_TUNNEL (src_ip) + NVGRE_TUNNEL_MAP (vsid/tunnel_name),
 * programs the kernel via `ip link add ... type gre ... key <vsid>` and writes
 * status to STATE_DB NVGRE_TUNNEL_TABLE. No APP_DB/orchagent hand-off.
 */
class NvgreTunnelMgr : public Orch
{
public:
    NvgreTunnelMgr(DBConnector *cfgDb, DBConnector *stateDb,
                   const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    Table m_stateNvgreTunnelTable;
    std::map<std::string, std::string> m_tunnelSrcIp;   // tunnel_name -> src_ip
    std::set<std::string> m_programmedTunnels;

    void doTask(Consumer &consumer);
    void doNvgreTunnelTask(Consumer &consumer);
    void doNvgreTunnelMapTask(Consumer &consumer);

    bool addGreTunnel(const std::string &name, const std::string &srcIp,
                      const std::string &vsid);
    bool removeGreTunnel(const std::string &name);
};

} // namespace swss

#endif /* __NVGRETUNNELMGR__ */
