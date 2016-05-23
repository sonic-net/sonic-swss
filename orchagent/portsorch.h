#ifndef SWSS_PORTSORCH_H
#define SWSS_PORTSORCH_H

#include "orch.h"
#include "port.h"

#include "macaddress.h"

#include <map>

class PortsOrch : public Orch
{
public:
    PortsOrch(DBConnector *db, vector<string> tableNames);

    bool isInitDone();

    bool getPort(string alias, Port &port);
    void setPort(string alias, Port port);

private:
    bool m_initDone = false;
    sai_object_id_t m_cpuPort;

    sai_uint32_t m_portCount;
    map<set<int>, sai_object_id_t> m_portListLaneMap;
    map<string, Port> m_portList;

    void doTask(Consumer &consumer);
    void doPortTask(Consumer &consumer);
    void doVlanTask(Consumer &consumer);
    void doLagTask(Consumer &consumer);

    bool initializePort(Port &port);

    bool addHostIntfs(sai_object_id_t router_intfs_id, string alias, sai_object_id_t &host_intfs_id);

    bool addVlan(string vlan);
    bool removeVlan(Port vlan);
    bool addVlanMember(Port vlan, Port port);
    bool removeVlanMember(Port vlan, Port port);

    bool addLag(string lag);
    bool removeLag(Port lag);
    bool addLagMember(Port lag, Port port);
    bool removeLagMember(Port lag, Port port);

    bool setPortAdminStatus(sai_object_id_t id, bool up);
};
#endif /* SWSS_PORTSORCH_H */

