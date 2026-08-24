#ifndef __NVGRETUNNELMGR__
#define __NVGRETUNNELMGR__

#include "dbconnector.h"
#include "orch.h"

#include <map>
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
#ifndef STATE_NVGRE_TUNNEL_MAP_TABLE_NAME
#define STATE_NVGRE_TUNNEL_MAP_TABLE_NAME "NVGRE_TUNNEL_MAP_TABLE"
#endif

/* NVGRE_TUNNEL field */
#define NVGRE_FIELD_SRC_IP              "src_ip"

/* NVGRE_TUNNEL_MAP fields (composite key <tunnel>|<map>) */
#define NVGRE_MAP_FIELD_VSID            "vsid"
#define NVGRE_MAP_FIELD_VLAN_ID         "vlan_id"

/* 24-bit VSID range (matches NVGRE_VSID_MAX_VALUE in nvgreorch.cpp) */
#define NVGRE_VSID_MAX_VALUE            16777214

namespace swss {

/* Teardown state for one programmed decap map entry. */
struct NvgreMapState
{
    std::string dev;     // kernel gretap device name
    std::string vsid;    // GRE key
    std::string vlanId;  // bridge access VLAN
};

/*
 * NvgreTunnelMgr — switchdev NVGRE tunnel manager (replaces NvgreTunnelOrch).
 *
 * Reads CONFIG_DB NVGRE_TUNNEL (src_ip) + NVGRE_TUNNEL_MAP (vsid/vlan_id, composite
 * key <tunnel>|<map>), programs the kernel with one `gretap` device per VSID (the
 * VSID is the GRE key) enslaved to the bridge as an untagged access port on the
 * mapped VLAN, and writes status to STATE_DB NVGRE_TUNNEL_TABLE /
 * NVGRE_TUNNEL_MAP_TABLE. No APP_DB/orchagent hand-off.
 */
class NvgreTunnelMgr : public Orch
{
public:
    NvgreTunnelMgr(DBConnector *cfgDb, DBConnector *stateDb,
                   const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    Table m_stateNvgreTunnelTable;
    Table m_stateNvgreTunnelMapTable;
    Table m_cfgVlanTable;

    std::map<std::string, std::string> m_tunnelSrcIp;   // tunnel_name -> src_ip
    std::map<std::string, NvgreMapState> m_mapDev;      // composite key "tunnel|map" -> state

    void doTask(Consumer &consumer);
    void doNvgreTunnelTask(Consumer &consumer);
    void doNvgreTunnelMapTask(Consumer &consumer);

    bool programMap(const std::string &key, const std::string &vsid,
                    const std::string &vlanId, const std::string &srcIp);
    void removeMap(const std::string &key);
    void removeTunnelCascade(const std::string &tunnel);

    bool interfaceExists(const std::string &dev);
    bool vlanExists(const std::string &vlanId);
};

} // namespace swss

#endif /* __NVGRETUNNELMGR__ */
