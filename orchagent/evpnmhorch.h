#ifndef SWSS_EVPNMHORCH_H
#define SWSS_EVPNMHORCH_H

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "orch.h"
#include "observer.h"
#include "port.h"
#include "timer.h"

struct EsCacheEntry
{
    /*
     * Lifecycle of the DF role, programmed as SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP
     * (true on the non-DF):
     * - VLAN member creation: Handled by portsorch querying evpnMhOrch
     * - EVPN_DF_TABLE and EVPN_ETHERNET_SEGMENT Updates: Handled by evpnMhOrch querying portsorch
     */
    bool is_df;

    EsCacheEntry()
    {
    }

    EsCacheEntry(bool is_df) : is_df(is_df)
    {
    }
};

/*
 * EVPN_ES_BACKUP_NHG_TABLE entry of a local ES port: zebra's backup L2 next hop
 * group (the ES's peer VTEPs), set as the port bridge port's protection group
 * (SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID). While the port
 * is oper down, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE (or the
 * deprecated SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER) sends traffic for
 * the port's MACs to that group.
 */
struct EsBackupEntry
{
    std::string nhg_id;
    sai_object_id_t bridge_port_id = SAI_NULL_OBJECT_ID; /* bridge port the attributes are on */
    sai_object_id_t nhg_oid = SAI_NULL_OBJECT_ID;        /* protection group set on it */
    bool switchover = false;
    /* The table entry is deleted but the group still carries the port's traffic
     * (isEsBackupInUse): attached until that ends, a SET, or the group or ES goes. */
    bool deleted = false;
};

class EvpnMhOrch : public Orch, public Observer
{
public:
    EvpnMhOrch(vector<TableConnector> &connectors);
    ~EvpnMhOrch();

    void update(SubjectType type, void *cntx) override;
    using Orch::doTask;
    void doTask() override;
    void doTask(swss::SelectableTimer &timer) override;

    /* True while the port's bridge port has a protection group: its MACs stay
     * reachable through the group when the port goes down, so they are kept. */
    bool isEsBackupAttached(const std::string &port_name) const;
    /* True while the port's traffic is switched over to its protection group. */
    bool isEsSwitchedOver(const std::string &port_name) const;
    /* L2NhgOrch created the group's bridge port: attach ES ports waiting for it. */
    void onL2NhgActive(const std::string &nhg_id);

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

    std::map<std::string, EsBackupEntry> m_esBackup;
    std::set<std::string> m_esBackupPending;
    /* ES ports back up and still switched over, each until its switchback hold passes. */
    std::map<std::string, std::chrono::steady_clock::time_point> m_esBackupSwitchback;
    swss::SelectableTimer *m_esBackupSwitchbackTimer = nullptr;
    bool m_esBackupCapChecked = false;
    bool m_esBackupSupported = false;
    /* Switch over with PROTECTION_ADMIN_MODE, else with the deprecated SET_SWITCHOVER. */
    bool m_esBackupAdminMode = false;

    bool isEsBackupSupported();
    void doEvpnEsBackupNhgTask(Consumer &consumer);
    bool applyEsBackup(const std::string &port_name);
    bool setEsBackupNhg(EsBackupEntry &entry, const std::string &port_name, sai_object_id_t oid);
    bool setEsBackupSwitchover(EsBackupEntry &entry, const std::string &port_name, bool on);
    bool detachEsBackup(EsBackupEntry &entry, const std::string &port_name);
    bool isEsBackupInUse(const EsBackupEntry &entry, const std::string &port_name,
                         const Port &port) const;
    bool holdEsBackupDelete(EsBackupEntry &entry, const std::string &port_name);
    bool finishEsBackupDelete(const std::string &port_name);
    /* LAG members whose distribution PortsOrch has disabled. */
    std::set<std::string> m_lagMembersNotDistributing;
    bool isEsPortDown(const Port &port, const Port *changed_member = nullptr) const;
    void updateEsBackupSwitchover(const std::string &port_name, bool down);
    void onEsBackupPortOperChange(const Port &port);
    void onEsBackupLagMemberChange(const std::string &member_alias, const Port *changed_member);
    void onEsBackupSwitchbackTimer();
    void armEsBackupSwitchbackTimer();
    void onEsBackupBridgePortChange(const Port &port, bool add);
};

#endif /* SWSS_EVPNMHORCH_H */
