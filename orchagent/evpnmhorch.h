#ifndef SWSS_EVPNMHORCH_H
#define SWSS_EVPNMHORCH_H

#include <memory>
#include <vector>

#include "orch.h"
#include "observer.h"

struct EsCacheEntry
{
    /*
     * Lifecycle of the DF role attribute (SAI_BRIDGE_PORT_ATTR_NON_DF):
     * - Port Creation/deletion: Handled by portsorch querying evpnMhOrch
     * - EVPN_DF_TABLE Updates: Handled by evpnMhOrch querying portsorch
     */
    bool is_df;

    EsCacheEntry()
    {
    }

    EsCacheEntry(bool is_df) : is_df(is_df)
    {
    }
};

class EvpnMhOrch : public Orch
{
public:
    EvpnMhOrch(vector<TableConnector> &connectors);
    ~EvpnMhOrch();

    bool isPortInterfaceAssociatedToEs(const std::string &port_name);
    bool isPortAndVlanAssociatedToEs(const std::string &port_name, sai_vlan_id_t vlan_id);
    bool isInterfaceDF(const std::string &port_name, sai_vlan_id_t vlan_id);
    /* Value for SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP (true = drop): true only on a
     * port configured in an ES whose EVPN_DF_TABLE entry for the VLAN says non-DF. Without
     * an entry the port does not drop: zebra sends no non-DF entry for an ES it considers
     * not ready, and no entry for a VLAN given its VNI after the port joined the ES. */
    bool isTunnelTermBumDropped(const std::string &port_name, sai_vlan_id_t vlan_id);

private:
    std::map<std::string, std::unique_ptr<EsCacheEntry>> m_esDataMap;
    std::map<std::string, bool> m_esIntfMap;

    EsCacheEntry *getEsCache(const std::string &key);
    bool updateEsCache(string &key, KeyOpFieldsValuesTuple &t);
    bool deleteEsCache(string &key);
    void doEvpnEsDfTask(Consumer &consumer);
    void doEvpnEsIntfTask(Consumer &consumer);
    bool vlanMembersApplyNonDF(string port_name);
    std::string stripVlanFromInterfaceName(const std::string interfaceName);

    void doTask(Consumer &consumer);
};

#endif /* SWSS_EVPNMHORCH_H */
