#ifndef SWSS_PORTSORCH_H
#define SWSS_PORTSORCH_H

#include "orch.h"
#include "port.h"

#include "macaddress.h"

#include <map>

class PortsOrch : public Orch
{
public:
    PortsOrch(DBConnector *db, string tableName);

    bool getPort(string alias, Port &p);

    bool setPortAdminStatus(sai_object_id_t id, bool up);
private:
    void doTask(Consumer &consumer);

    sai_object_id_t m_cpuPort;

    sai_uint32_t m_portCount;
    map<set<int>, sai_object_id_t> m_portListLaneMap;
    map<string, Port> m_portList;

    bool initializePort(Port &p);
    bool setupVlan(sai_vlan_id_t vlan_id, sai_object_id_t port_id, sai_object_id_t &vlan_member_id);
    bool setupRouterIntfs(sai_object_id_t virtual_router_id, MacAddress mac_address,
            sai_vlan_id_t vlan_id, sai_object_id_t &router_intfs_id);
    bool setupHostIntfs(sai_object_id_t router_intfs_id, string alias, sai_object_id_t &host_intfs_id);
};
#endif /* SWSS_PORTSORCH_H */

