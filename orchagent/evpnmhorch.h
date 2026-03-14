#ifndef SWSS_EVPNMHORCH_H
#define SWSS_EVPNMHORCH_H

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

    bool isPortInterfaceAssociatedToEs(const std::string port_name);
    bool isPortAndVlanAssociatedToEs(const std::string port_name, const sai_vlan_id_t vlan_id);
    bool isInterfaceDF(const std::string port_name, const sai_vlan_id_t vlan_id);

private:
    vector<Table *> m_appTables;

    std::map<std::string, struct EsCacheEntry *> m_esDataMap;
    std::map<std::string, bool> m_esIntfMap;

    struct EsCacheEntry *getEsCache(const std::string &key);
    struct EsCacheEntry *getEsCacheForPort(const std::string &key);
    void updateEsCache(string &key, KeyOpFieldsValuesTuple &t);
    void deleteEsCache(string &key);
    void doEvpnEsDfTask(Consumer &consumer);
    void doEvpnEsIntfTask(Consumer &consumer);
    void vlanMembersApplyNonDF(string port_name);
    std::string stripVlanFromInterfaceName(const std::string interfaceName);

    void doTask(Consumer &consumer);
};

#endif /* SWSS_EVPNMHORCH_H */
