#ifndef SWSS_PORTSORCH_H
#define SWSS_PORTSORCH_H

#include <map>

#include "acltable.h"
#include "orch.h"
#include "port.h"
#include "observer.h"
#include "macaddress.h"
#include "producertable.h"

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

class PortsOrch : public Orch, public Subject
{
public:
    PortsOrch(DBConnector *db, vector<table_name_with_pri_t> &tableNames);

    bool isInitDone();

    map<string, Port>& getAllPorts();
    bool getBridgePort(sai_object_id_t id, Port &port);
    bool getPort(string alias, Port &port);
    bool getPort(sai_object_id_t id, Port &port);
    bool getPortByBridgePortId(sai_object_id_t bridge_port_id, Port &port);
    void setPort(string alias, Port port);
    void getCpuPort(Port &port);
    bool getVlanByVlanId(sai_vlan_id_t vlan_id, Port &vlan);

    bool setHostIntfsOperStatus(sai_object_id_t id, bool up);
    void updateDbPortOperStatus(sai_object_id_t id, sai_port_oper_status_t status);
    bool bindAclTable(sai_object_id_t id, sai_object_id_t table_oid, sai_object_id_t &group_member_oid, acl_stage_type_t acl_stage = ACL_STAGE_INGRESS);
private:
    unique_ptr<Table> m_counterTable;
    unique_ptr<Table> m_portTable;
    unique_ptr<Table> m_queueTable;
    unique_ptr<Table> m_queuePortTable;
    unique_ptr<Table> m_queueIndexTable;
    unique_ptr<Table> m_queueTypeTable;
    unique_ptr<ProducerTable> m_flexCounterTable;
    unique_ptr<ProducerTable> m_flexCounterGroupTable;

    std::string getQueueFlexCounterTableKey(std::string s);
    std::string getPortFlexCounterTableKey(std::string s);

    shared_ptr<DBConnector> m_counter_db;
    shared_ptr<DBConnector> m_flex_db;

    std::map<sai_object_id_t, PortSupportedSpeeds> m_portSupportedSpeeds;

    bool m_initDone = false;
    Port m_cpuPort;
    // TODO: Add Bridge/Vlan class
    sai_object_id_t m_default1QBridge;
    sai_object_id_t m_defaultVlan;

    bool m_portConfigDone = false;
    sai_uint32_t m_portCount;
    map<set<int>, sai_object_id_t> m_portListLaneMap;
    map<set<int>, tuple<string, uint32_t>> m_lanesAliasSpeedMap;
    map<string, Port> m_portList;

    NotificationConsumer* m_portStatusNotificationConsumer;

    void doTask(Consumer &consumer);
    void doPortTask(Consumer &consumer);
    void doVlanTask(Consumer &consumer);
    void doVlanMemberTask(Consumer &consumer);
    void doLagTask(Consumer &consumer);
    void doLagMemberTask(Consumer &consumer);

    void doTask(NotificationConsumer &consumer);

    void removeDefaultVlanMembers();
    void removeDefaultBridgePorts();

    bool initializePort(Port &port);
    void initializePriorityGroups(Port &port);
    void initializeQueues(Port &port);

    bool addHostIntfs(Port &port, string alias, sai_object_id_t &host_intfs_id);
    bool setHostIntfsStripTag(Port &port, sai_hostif_vlan_tag_t strip);

    bool addBridgePort(Port &port);
    bool removeBridgePort(Port &port);

    bool addVlan(string vlan);
    bool removeVlan(Port vlan);
    bool addVlanMember(Port &vlan, Port &port, string& tagging_mode);
    bool removeVlanMember(Port &vlan, Port &port);

    bool addLag(string lag);
    bool removeLag(Port lag);
    bool addLagMember(Port &lag, Port &port);
    bool removeLagMember(Port &lag, Port &port);
    void getLagMember(Port &lag, vector<Port> &portv);

    bool addPort(const set<int> &lane_set, uint32_t speed);
    bool removePort(sai_object_id_t port_id);
    bool initPort(const string &alias, const set<int> &lane_set);

    bool setPortAdminStatus(sai_object_id_t id, bool up);
    bool setPortMtu(sai_object_id_t id, sai_uint32_t mtu);
    bool setPortPvid (Port &port, sai_uint32_t pvid);
    bool getPortPvid(Port &port, sai_uint32_t &pvid);
    bool setPortFec(sai_object_id_t id, sai_port_fec_mode_t mode);

    bool setBridgePortAdminStatus(sai_object_id_t id, bool up);

    bool validatePortSpeed(const std::string& alias, sai_object_id_t port_id, sai_uint32_t speed);
    bool setPortSpeed(sai_object_id_t port_id, sai_uint32_t speed);
    bool getPortSpeed(sai_object_id_t port_id, sai_uint32_t &speed);

    bool getQueueType(sai_object_id_t queue_id, string &type);
};
#endif /* SWSS_PORTSORCH_H */

