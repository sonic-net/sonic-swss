#ifndef SWSS_PORTSORCH_H
#define SWSS_PORTSORCH_H

#include "orch.h"
#include "port.h"
#include "observer.h"

#include "macaddress.h"

#include <map>

#define FCS_LEN 4
#define VLAN_TAG_LEN 4

typedef std::vector<sai_uint32_t> PortSupportedSpeeds;

static const map<sai_port_oper_status_t, string> oper_status_strings =
{
    { SAI_PORT_OPER_STATUS_UNKNOWN,     "unknown" },
    { SAI_PORT_OPER_STATUS_UP,          "up" },
    { SAI_PORT_OPER_STATUS_DOWN,        "down" },
    { SAI_PORT_OPER_STATUS_TESTING,     "testing" },
    { SAI_PORT_OPER_STATUS_NOT_PRESENT, "not present" }
};

struct LagMemberUpdate
{
    Port lag;
    Port member;
    bool add;
};

struct VlanMemberUpdate
{
    Port vlan;
    Port member;
    bool add;
};

struct PortSpeedUpdate
{
    Port port;
    sai_uint32_t speed;
};

class PortsOrch : public Orch, public Subject
{
public:
    PortsOrch(DBConnector *db, vector<string> tableNames);

    bool isInitDone();

    bool getPort(string alias, Port &port);
    bool getPort(sai_object_id_t id, Port &port);
    void setPort(string alias, Port port);
    sai_object_id_t getCpuPort();

    bool setHostIntfsOperStatus(sai_object_id_t id, bool up);
    void updateDbPortOperStatus(sai_object_id_t id, sai_port_oper_status_t status);
private:
    unique_ptr<Table> m_counterTable;
    unique_ptr<Table> m_portTable;

    std::map<sai_object_id_t, PortSupportedSpeeds> m_portSupportedSpeeds;

    bool m_initDone = false;
    sai_object_id_t m_cpuPort;

    sai_uint32_t m_portCount;
    map<set<int>, sai_object_id_t> m_portListLaneMap;
    map<string, Port> m_portList;

    void doTask(Consumer &consumer);
    void doPortTask(Consumer &consumer);
    void doVlanTask(Consumer &consumer);
    void doVlanMemberTask(Consumer &consumer);
    void doLagTask(Consumer &consumer);
    void doLagMemberTask(Consumer &consumer);

    bool initializePort(Port &port);
    void initializePriorityGroups(Port &port);
    void initializeQueues(Port &port);

    bool addHostIntfs(sai_object_id_t router_intfs_id, string alias, sai_object_id_t &host_intfs_id);

    bool addVlan(string vlan);
    bool removeVlan(Port vlan);
    bool addVlanMember(Port vlan, Port port, string& tagging_mode);
    bool removeVlanMember(Port vlan, Port port);

    bool addLag(string lag);
    bool removeLag(Port lag);
    bool addLagMember(Port lag, Port port);
    bool removeLagMember(Port lag, Port port);

    bool setPortAdminStatus(sai_object_id_t id, bool up);
    bool setPortMtu(sai_object_id_t id, sai_uint32_t mtu);
    bool validatePortSpeed(sai_object_id_t port_id, sai_uint32_t speed);
    bool setPortSpeed(Port &port, sai_uint32_t speed);
    bool getPortSpeed(sai_object_id_t port_id, sai_uint32_t &speed);
};
#endif /* SWSS_PORTSORCH_H */

