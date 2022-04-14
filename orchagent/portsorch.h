#ifndef SWSS_PORTSORCH_H
#define SWSS_PORTSORCH_H

#include <map>

#include "acltable.h"
#include "orch.h"
#include "port.h"
#include "observer.h"
#include "macaddress.h"
#include "producertable.h"
#include "flex_counter_manager.h"
#include "gearboxutils.h"
#include "saihelper.h"
#include "lagid.h"
#include "flexcounterorch.h"


#define FCS_LEN 4
#define VLAN_TAG_LEN 4
#define PORT_STAT_COUNTER_FLEX_COUNTER_GROUP "PORT_STAT_COUNTER"
#define PORT_RATE_COUNTER_FLEX_COUNTER_GROUP "PORT_RATE_COUNTER"
#define PORT_BUFFER_DROP_STAT_FLEX_COUNTER_GROUP "PORT_BUFFER_DROP_STAT"
#define QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP "QUEUE_STAT_COUNTER"
#define QUEUE_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP "QUEUE_WATERMARK_STAT_COUNTER"
#define PG_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP "PG_WATERMARK_STAT_COUNTER"
#define PG_DROP_STAT_COUNTER_FLEX_COUNTER_GROUP "PG_DROP_STAT_COUNTER"

typedef std::vector<sai_uint32_t> PortSupportedSpeeds;

static const map<sai_port_oper_status_t, string> oper_status_strings =
{
    { SAI_PORT_OPER_STATUS_UNKNOWN,     "unknown" },
    { SAI_PORT_OPER_STATUS_UP,          "up" },
    { SAI_PORT_OPER_STATUS_DOWN,        "down" },
    { SAI_PORT_OPER_STATUS_TESTING,     "testing" },
    { SAI_PORT_OPER_STATUS_NOT_PRESENT, "not present" }
};

static const unordered_map<string, sai_port_oper_status_t> string_oper_status =
{
    { "unknown",     SAI_PORT_OPER_STATUS_UNKNOWN },
    { "up",          SAI_PORT_OPER_STATUS_UP },
    { "down",        SAI_PORT_OPER_STATUS_DOWN },
    { "testing",     SAI_PORT_OPER_STATUS_TESTING },
    { "not present", SAI_PORT_OPER_STATUS_NOT_PRESENT }
};

struct PortUpdate
{
    Port port;
    bool add;
};

struct PortOperStateUpdate
{
    Port port;
    sai_port_oper_status_t operStatus;
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
    PortsOrch(DBConnector *db, DBConnector *stateDb, vector<table_name_with_pri_t> &tableNames, DBConnector *chassisAppDb);

    bool allPortsReady();
    bool isInitDone();
    bool isConfigDone();
    bool isPortAdminUp(const string &alias);

    map<string, Port>& getAllPorts();
    bool bake() override;
    void cleanPortTable(const vector<string>& keys);
    bool getBridgePort(sai_object_id_t id, Port &port);
    bool setBridgePortLearningFDB(Port &port, sai_bridge_port_fdb_learning_mode_t mode);
    bool getPort(string alias, Port &port);
    bool getPort(sai_object_id_t id, Port &port);
    void increasePortRefCount(const string &alias);
    void decreasePortRefCount(const string &alias);
    bool getPortByBridgePortId(sai_object_id_t bridge_port_id, Port &port);
    void setPort(string alias, Port port);
    void getCpuPort(Port &port);
    bool getInbandPort(Port &port);
    bool getVlanByVlanId(sai_vlan_id_t vlan_id, Port &vlan);

    bool setHostIntfsOperStatus(const Port& port, bool up) const;
    void updateDbPortOperStatus(const Port& port, sai_port_oper_status_t status) const;

    bool createVlanHostIntf(Port& vl, string hostif_name);
    bool removeVlanHostIntf(Port vl);

    bool createBindAclTableGroup(sai_object_id_t  port_oid,
                   sai_object_id_t  acl_table_oid,
                   sai_object_id_t  &group_oid,
                   acl_stage_type_t acl_stage = ACL_STAGE_EGRESS);
    bool unbindRemoveAclTableGroup(sai_object_id_t  port_oid,
                                   sai_object_id_t  acl_table_oid,
                                   acl_stage_type_t acl_stage);
    bool bindAclTable(sai_object_id_t  id,
                      sai_object_id_t  table_oid,
                      sai_object_id_t  &group_member_oid,
                      acl_stage_type_t acl_stage = ACL_STAGE_INGRESS);
    bool unbindAclTable(sai_object_id_t  port_oid,
                        sai_object_id_t  acl_table_oid,
                        sai_object_id_t  acl_group_member_oid,
                        acl_stage_type_t acl_stage);
    bool bindUnbindAclTableGroup(Port &port,
                                 bool ingress,
                                 bool bind);
    bool getPortPfc(sai_object_id_t portId, uint8_t *pfc_bitmask);
    bool setPortPfc(sai_object_id_t portId, uint8_t pfc_bitmask);

    bool setPortPfcWatchdogStatus(sai_object_id_t portId, uint8_t pfc_bitmask);
    bool getPortPfcWatchdogStatus(sai_object_id_t portId, uint8_t *pfc_bitmask);

    void generateQueueMap();
    void generatePriorityGroupMap();
    void generatePortCounterMap();
    void generatePortBufferDropCounterMap();

    void refreshPortStatus();
    bool removeAclTableGroup(const Port &p);

    bool addSubPort(Port &port, const string &alias, const string &vlan, const bool &adminUp = true, const uint32_t &mtu = 0);
    bool removeSubPort(const string &alias);
    bool updateL3VniStatus(uint16_t vlan_id, bool status);
    void getLagMember(Port &lag, vector<Port> &portv);
    void updateChildPortsMtu(const Port &p, const uint32_t mtu);

    bool addTunnel(string tunnel,sai_object_id_t, bool learning=true);
    bool removeTunnel(Port tunnel);
    bool addBridgePort(Port &port);
    bool removeBridgePort(Port &port);
    bool addVlanMember(Port &vlan, Port &port, string& tagging_mode, string end_point_ip = "");
    bool removeVlanMember(Port &vlan, Port &port, string end_point_ip = "");
    bool isVlanMember(Port &vlan, Port &port, string end_point_ip = "");
    bool addVlanFloodGroups(Port &vlan, Port &port, string end_point_ip);
    bool removeVlanEndPointIp(Port &vlan, Port &port, string end_point_ip);
    void increaseBridgePortRefCount(Port &port);
    void decreaseBridgePortRefCount(Port &port);
    bool getBridgePortReferenceCount(Port &port);

    string m_inbandPortName = "";
    bool isInbandPort(const string &alias);
    bool setVoqInbandIntf(string &alias, string &type);
    bool getPortVlanMembers(Port &port, vlan_members_t &vlan_members);

    bool getRecircPort(Port &p, string role);

    const gearbox_phy_t* getGearboxPhy(const Port &port);

    bool getPortIPG(sai_object_id_t port_id, uint32_t &ipg);
    bool setPortIPG(sai_object_id_t port_id, uint32_t ipg);

    bool getPortOperStatus(const Port& port, sai_port_oper_status_t& status) const;

private:
    unique_ptr<Table> m_counterTable;
    unique_ptr<Table> m_counterLagTable;
    unique_ptr<Table> m_portTable;
    unique_ptr<Table> m_gearboxTable;
    unique_ptr<Table> m_queueTable;
    unique_ptr<Table> m_queuePortTable;
    unique_ptr<Table> m_queueIndexTable;
    unique_ptr<Table> m_queueTypeTable;
    unique_ptr<Table> m_pgTable;
    unique_ptr<Table> m_pgPortTable;
    unique_ptr<Table> m_pgIndexTable;
    unique_ptr<Table> m_stateBufferMaximumValueTable;
    unique_ptr<ProducerTable> m_flexCounterTable;
    unique_ptr<ProducerTable> m_flexCounterGroupTable;
    Table m_portStateTable;

    std::string getQueueWatermarkFlexCounterTableKey(std::string s);
    std::string getPriorityGroupWatermarkFlexCounterTableKey(std::string s);
    std::string getPriorityGroupDropPacketsFlexCounterTableKey(std::string s);
    std::string getPortRateFlexCounterTableKey(std::string s);

    shared_ptr<DBConnector> m_counter_db;
    shared_ptr<DBConnector> m_flex_db;
    shared_ptr<DBConnector> m_state_db;

    FlexCounterManager port_stat_manager;
    FlexCounterManager port_buffer_drop_stat_manager;
    FlexCounterManager queue_stat_manager;

    std::map<sai_object_id_t, PortSupportedSpeeds> m_portSupportedSpeeds;

    bool m_initDone = false;
    Port m_cpuPort;
    // TODO: Add Bridge/Vlan class
    sai_object_id_t m_default1QBridge;
    sai_object_id_t m_defaultVlan;

    typedef enum
    {
        PORT_CONFIG_MISSING,
        PORT_CONFIG_RECEIVED,
        PORT_CONFIG_DONE,
    } port_config_state_t;

    typedef enum
    {
        MAC_PORT_TYPE,
        PHY_PORT_TYPE,
        LINE_PORT_TYPE,
    } dest_port_type_t;

    bool m_gearboxEnabled = false;
    map<int, gearbox_phy_t> m_gearboxPhyMap;
    map<int, gearbox_interface_t> m_gearboxInterfaceMap;
    map<int, gearbox_lane_t> m_gearboxLaneMap;
    map<int, gearbox_port_t> m_gearboxPortMap;
    map<sai_object_id_t, tuple<sai_object_id_t, sai_object_id_t>> m_gearboxPortListLaneMap;

    port_config_state_t m_portConfigState = PORT_CONFIG_MISSING;
    sai_uint32_t m_portCount;
    map<set<int>, sai_object_id_t> m_portListLaneMap;
    map<set<int>, tuple<string, uint32_t, int, string, int, string>> m_lanesAliasSpeedMap;
    map<string, Port> m_portList;
    map<string, vlan_members_t> m_portVlanMember;
    /* mapping from SAI object ID to Name for faster
     * retrieval of Port/VLAN from object ID for events
     * coming from SAI
     */
    unordered_map<sai_object_id_t, string> saiOidToAlias;
    unordered_map<sai_object_id_t, int> m_portOidToIndex;
    map<string, uint32_t> m_port_ref_count;
    unordered_set<string> m_pendingPortSet;
    const uint32_t max_flood_control_types = 4;
    set<sai_vlan_flood_control_type_t> uuc_sup_flood_control_type;
    set<sai_vlan_flood_control_type_t> bc_sup_flood_control_type;
    map<string, uint32_t> m_bridge_port_ref_count;

    NotificationConsumer* m_portStatusNotificationConsumer;

    void doTask() override;
    void doTask(Consumer &consumer);
    void doPortTask(Consumer &consumer);
    void doVlanTask(Consumer &consumer);
    void doVlanMemberTask(Consumer &consumer);
    void doLagTask(Consumer &consumer);
    void doLagMemberTask(Consumer &consumer);

    void doTask(NotificationConsumer &consumer);

    void removePortFromLanesMap(string alias);
    void removePortFromPortListMap(sai_object_id_t port_id);
    void removeDefaultVlanMembers();
    void removeDefaultBridgePorts();

    bool initializePort(Port &port);
    void initializePriorityGroups(Port &port);
    void initializePortBufferMaximumParameters(Port &port);
    void initializeQueues(Port &port);

    bool addHostIntfs(Port &port, string alias, sai_object_id_t &host_intfs_id);
    bool setHostIntfsStripTag(Port &port, sai_hostif_vlan_tag_t strip);

    bool setBridgePortLearnMode(Port &port, string learn_mode);

    bool addVlan(string vlan);
    bool removeVlan(Port vlan);

    bool addLag(string lag, uint32_t spa_id, int32_t switch_id);
    bool removeLag(Port lag);
    bool setLagTpid(sai_object_id_t id, sai_uint16_t tpid);
    bool addLagMember(Port &lag, Port &port, bool enableForwarding);
    bool removeLagMember(Port &lag, Port &port);
    bool setCollectionOnLagMember(Port &lagMember, bool enableCollection);
    bool setDistributionOnLagMember(Port &lagMember, bool enableDistribution);

    bool addPort(const set<int> &lane_set, uint32_t speed, int an=0, string fec="");
    sai_status_t removePort(sai_object_id_t port_id);
    bool initPort(const string &alias, const string &role, const int index, const set<int> &lane_set);
    void deInitPort(string alias, sai_object_id_t port_id);

    bool setPortAdminStatus(Port &port, bool up);
    bool getPortAdminStatus(sai_object_id_t id, bool& up);
    bool setPortMtu(sai_object_id_t id, sai_uint32_t mtu);
    bool setPortTpid(sai_object_id_t id, sai_uint16_t tpid);
    bool setPortPvid (Port &port, sai_uint32_t pvid);
    bool getPortPvid(Port &port, sai_uint32_t &pvid);
    bool setPortFec(Port &port, sai_port_fec_mode_t mode);
    bool setPortPfcAsym(Port &port, string pfc_asym);
    bool getDestPortId(sai_object_id_t src_port_id, dest_port_type_t port_type, sai_object_id_t &des_port_id);

    bool setBridgePortAdminStatus(sai_object_id_t id, bool up);

    bool isSpeedSupported(const std::string& alias, sai_object_id_t port_id, sai_uint32_t speed);
    void getPortSupportedSpeeds(const std::string& alias, sai_object_id_t port_id, PortSupportedSpeeds &supported_speeds);
    void initPortSupportedSpeeds(const std::string& alias, sai_object_id_t port_id);
    task_process_status setPortSpeed(Port &port, sai_uint32_t speed);
    bool getPortSpeed(sai_object_id_t id, sai_uint32_t &speed);
    bool setGearboxPortsAttr(Port &port, sai_port_attr_t id, void *value);
    bool setGearboxPortAttr(Port &port, dest_port_type_t port_type, sai_port_attr_t id, void *value);

    task_process_status setPortAdvSpeeds(sai_object_id_t port_id, std::vector<sai_uint32_t>& speed_list);

    bool getQueueTypeAndIndex(sai_object_id_t queue_id, string &type, uint8_t &index);

    bool m_isQueueMapGenerated = false;
    void generateQueueMapPerPort(const Port& port);
    void removeQueueMapPerPort(const Port& port);

    bool m_isPriorityGroupMapGenerated = false;
    void generatePriorityGroupMapPerPort(const Port& port);
    void removePriorityGroupMapPerPort(const Port& port);

    bool m_isPortCounterMapGenerated = false;
    bool m_isPortBufferDropCounterMapGenerated = false;

    task_process_status setPortAutoNeg(sai_object_id_t id, int an);
    bool setPortFecMode(sai_object_id_t id, int fec);
    task_process_status setPortInterfaceType(sai_object_id_t id, sai_port_interface_type_t interface_type);
    task_process_status setPortAdvInterfaceTypes(sai_object_id_t id, std::vector<uint32_t> &interface_types);

    void updatePortOperStatus(Port &port, sai_port_oper_status_t status);

    bool getPortOperSpeed(const Port& port, sai_uint32_t& speed) const;
    void updateDbPortOperSpeed(Port &port, sai_uint32_t speed);

    void getPortSerdesVal(const std::string& s, std::vector<uint32_t> &lane_values);
    bool getPortAdvSpeedsVal(const std::string &s, std::vector<uint32_t> &speed_values);
    bool getPortInterfaceTypeVal(const std::string &s, sai_port_interface_type_t &interface_type);
    bool getPortAdvInterfaceTypesVal(const std::string &s, std::vector<uint32_t> &type_values);

    bool setPortSerdesAttribute(sai_object_id_t port_id,
                                std::map<sai_port_serdes_attr_t, std::vector<uint32_t>> &serdes_attr);


    void removePortSerdesAttribute(sai_object_id_t port_id);

    bool getSaiAclBindPointType(Port::Type                type,
                                sai_acl_bind_point_type_t &sai_acl_bind_type);
    void initGearbox();
    bool initGearboxPort(Port &port);

    map<string, string> m_recircPortRole;

    //map key is tuple of <attached_switch_id, core_index, core_port_index>
    map<tuple<int, int, int>, sai_object_id_t> m_systemPortOidMap;
    sai_uint32_t m_systemPortCount;
    bool getSystemPorts();
    bool addSystemPorts();
    unique_ptr<Table> m_tableVoqSystemLagTable;
    unique_ptr<Table> m_tableVoqSystemLagMemberTable;
    void voqSyncAddLag(Port &lag);
    void voqSyncDelLag(Port &lag);
    void voqSyncAddLagMember(Port &lag, Port &port);
    void voqSyncDelLagMember(Port &lag, Port &port);
    unique_ptr<LagIdAllocator> m_lagIdAllocator;

    std::unordered_set<std::string> generateCounterStats(const string& type);

};
#endif /* SWSS_PORTSORCH_H */
