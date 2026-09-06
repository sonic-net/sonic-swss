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

/* NVGRE_TUNNEL fields */
#define NVGRE_FIELD_SRC_IP              "src_ip"
#define NVGRE_FIELD_DST_IP              "dst_ip"

/* NVGRE_TUNNEL_MAP fields (composite key <tunnel>|<map>) */
#define NVGRE_MAP_FIELD_VSID            "vsid"
#define NVGRE_MAP_FIELD_VLAN_ID         "vlan_id"

/* STATE_DB map-table field: the programmed kernel device name (observability) */
#define NVGRE_STATE_FIELD_DEV           "dev"

/* 24-bit VSID range (matches the sonic-nvgre-tunnel YANG: 0..16777214) */
#define NVGRE_VSID_MAX_VALUE            16777214

namespace swss {

/* Teardown state for one programmed decap map entry. */
struct NvgreMapState
{
    std::string dev;     // kernel gretap device name (hash-derived)
    std::string vsid;    // VSID (GRE key = vsid << 8, RFC 7637)
    std::string vlanId;  // bridge access VLAN
};

/*
 * NvgreTunnelMgr — switchdev NVGRE tunnel manager (replaces NvgreTunnelOrch).
 *
 * Reads CONFIG_DB NVGRE_TUNNEL (src_ip) + NVGRE_TUNNEL_MAP (vsid/vlan_id, composite
 * key <tunnel>|<map>), programs the kernel with one `gretap` (or `ip6gretap`) device
 * per map entry — the VSID is carried in the high 24 bits of the GRE key (RFC 7637),
 * FlowID is left zero — enslaved to the bridge as an untagged access port on the
 * mapped VLAN. Writes status/dev to STATE_DB. No APP_DB/orchagent hand-off.
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
    Table m_cfgMapTable;   // CONFIG_DB NVGRE_TUNNEL_MAP (for tunnel-delete deferral)

    std::map<std::string, std::string> m_tunnelSrcIp;   // tunnel_name -> src_ip
    std::map<std::string, std::string> m_tunnelDstIp;   // tunnel_name -> dst_ip (remote VTEP; empty = any)
    std::map<std::string, NvgreMapState> m_mapDev;      // composite key "tunnel|map" -> state

    void doTask(Consumer &consumer);
    void doNvgreTunnelTask(Consumer &consumer);
    void doNvgreTunnelMapTask(Consumer &consumer);

    bool programMap(const std::string &key, const std::string &vsid,
                    const std::string &vlanId, const std::string &srcIp,
                    const std::string &dstIp);
    void removeMap(const std::string &key);

    bool interfaceExists(const std::string &dev);
    bool vlanExists(const std::string &vlanId);
    bool bridgeExists();
    bool tunnelHasMaps(const std::string &tunnel);
};

} // namespace swss

#endif /* __NVGRETUNNELMGR__ */
