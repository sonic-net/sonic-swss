#ifndef SWSS_PORTSORCH_H
#define SWSS_PORTSORCH_H

#include <map>
#include <unordered_set>

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
#include "events.h"

#include "port/port_capabilities.h"
#include "port/porthlpr.h"
#include "port/portschema.h"

#include "high_frequency_telemetry/counternameupdater.h"

#define FCS_LEN 4
#define VLAN_TAG_LEN 4
#define MAX_MACSEC_SECTAG_SIZE 32
#define PORT_STAT_COUNTER_FLEX_COUNTER_GROUP "PORT_STAT_COUNTER"
#define PORT_RATE_COUNTER_FLEX_COUNTER_GROUP "PORT_RATE_COUNTER"
#define PORT_BUFFER_DROP_STAT_FLEX_COUNTER_GROUP "PORT_BUFFER_DROP_STAT"
#define QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP "QUEUE_STAT_COUNTER"
#define QUEUE_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP "QUEUE_WATERMARK_STAT_COUNTER"
#define PG_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP "PG_WATERMARK_STAT_COUNTER"
#define PG_DROP_STAT_COUNTER_FLEX_COUNTER_GROUP "PG_DROP_STAT_COUNTER"
#define QUEUE_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS "60000"
#define PG_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS    "60000"
#define PG_DROP_FLEX_STAT_COUNTER_POLL_MSECS         "10000"
#define PORT_RATE_FLEX_COUNTER_POLLING_INTERVAL_MS   "1000"
#define WRED_QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP "WRED_ECN_QUEUE_STAT_COUNTER"
#define WRED_PORT_STAT_COUNTER_FLEX_COUNTER_GROUP "WRED_ECN_PORT_STAT_COUNTER"

typedef std::vector<sai_uint32_t> PortSupportedSpeeds;
typedef std::set<sai_port_fec_mode_t> PortSupportedFecModes;

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

static const std::map<std::string, sai_port_serdes_attr_t> tx_fir_strings_system_side =
{
    {"system_tx_fir_pre1", SAI_PORT_SERDES_ATTR_TX_FIR_PRE1},
    {"system_tx_fir_pre2", SAI_PORT_SERDES_ATTR_TX_FIR_PRE2},
    {"system_tx_fir_pre3", SAI_PORT_SERDES_ATTR_TX_FIR_PRE3},
    {"system_tx_fir_post1", SAI_PORT_SERDES_ATTR_TX_FIR_POST1},
    {"system_tx_fir_post2", SAI_PORT_SERDES_ATTR_TX_FIR_POST2},
    {"system_tx_fir_post3", SAI_PORT_SERDES_ATTR_TX_FIR_POST3},
    {"system_tx_fir_main", SAI_PORT_SERDES_ATTR_TX_FIR_MAIN}
};

static const std::map<std::string, sai_port_serdes_attr_t> tx_fir_strings_line_side =
{
    {"line_tx_fir_pre1", SAI_PORT_SERDES_ATTR_TX_FIR_PRE1},
    {"line_tx_fir_pre2", SAI_PORT_SERDES_ATTR_TX_FIR_PRE2},
    {"line_tx_fir_pre3", SAI_PORT_SERDES_ATTR_TX_FIR_PRE3},
    {"line_tx_fir_post1", SAI_PORT_SERDES_ATTR_TX_FIR_POST1},
    {"line_tx_fir_post2", SAI_PORT_SERDES_ATTR_TX_FIR_POST2},
    {"line_tx_fir_post3", SAI_PORT_SERDES_ATTR_TX_FIR_POST3},
    {"line_tx_fir_main", SAI_PORT_SERDES_ATTR_TX_FIR_MAIN}
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

struct queueInfo
{
    // SAI_QUEUE_ATTR_TYPE
    sai_queue_type_t type;
    // SAI_QUEUE_ATTR_INDEX
    sai_uint8_t index;
};

struct systemPortMapInfo
{
    sai_object_id_t system_port_id;
    SystemPortInfo system_port_info;
    bool info_valid = false;
};

template<typename T>
struct PortCapability
{
    bool supported = false;
    T data;
};

typedef PortCapability<PortSupportedFecModes> PortFecModeCapability_t;

class PortsOrch : public Orch, public Subject
{
public:
    PortsOrch(DBConnector *db, DBConnector *stateDb, vector<table_name_with_pri_t> &tableNames, DBConnector *chassisAppDb);

    bool allPortsReady();
    bool isInitDone();
    bool isConfigDone();
    bool isGearboxEnabled();
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
    void initHostTxReadyState(Port &port);
    void initializePortOperErrors(Port &port);
    bool getInbandPort(Port &port);
    bool getVlanByVlanId(sai_vlan_id_t vlan_id, Port &vlan);

    bool setHostIntfsOperStatus(const Port& port, bool up) const;
    void updateDbPortOperStatus(const Port& port, sai_port_oper_status_t status) const;
    void updateDbPortFlapCount(Port& port, sai_port_oper_status_t pstatus);
    void updateDbPortOperError(Port& port, PortOperErrorEvent *pevent);

    bool createVlanHostIntf(Port& vl, string hostif_name);
    bool removeVlanHostIntf(Port vl);

    unordered_set<string>& getAllVlans();

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

    void generateQueueMap(map<string, FlexCounterQueueStates> queuesStateVector);
    uint32_t getNumberOfPortSupportedQueueCounters(string port);
    void createPortBufferQueueCounters(const Port &port, string queues, bool skip_host_tx_queue=true);
    void addPortBufferQueueCounters(const Port &port, uint32_t startIndex, uint32_t endIndex, bool skip_host_tx_queue=true);
    void removePortBufferQueueCounters(const Port &port, string queues, bool skip_host_tx_queue=true);
    void deletePortBufferQueueCounters(const Port &port, uint32_t startIndex, uint32_t endIndex, bool skip_host_tx_queue=true);
    void addQueueFlexCounters(map<string, FlexCounterQueueStates> queuesStateVector);
    void addQueueWatermarkFlexCounters(map<string, FlexCounterQueueStates> queuesStateVector);
    void addWredQueueFlexCounters(map<string, FlexCounterQueueStates> queuesStateVector);

    void generatePriorityGroupMap(map<string, FlexCounterPgStates> pgsStateVector);
    uint32_t getNumberOfPortSupportedPgCounters(string port);
    void createPortBufferPgCounters(const Port &port, string pgs);
    void addPortBufferPgCounters(const Port& port, uint32_t startIndex, uint32_t endIndex);
    void removePortBufferPgCounters(const Port& port, string pgs);
    void deletePortBufferPgCounters(const Port& port, uint32_t startIndex, uint32_t endIndex);
    void addPriorityGroupFlexCounters(map<string, FlexCounterPgStates> pgsStateVector);
    void addPriorityGroupWatermarkFlexCounters(map<string, FlexCounterPgStates> pgsStateVector);

    void generatePortCounterMap();
    void generatePortBufferDropCounterMap();

    void generateWredPortCounterMap();
    void generateWredQueueCounterMap();

    void flushCounters();

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

    bool getRecircPort(Port &p, Port::Role role);

    const gearbox_phy_t* getGearboxPhy(const Port &port);

    bool getPortIPG(sai_object_id_t port_id, uint32_t &ipg);
    bool setPortIPG(sai_object_id_t port_id, uint32_t ipg);

    bool getPortOperStatus(const Port& port, sai_port_oper_status_t& status) const;

    void updateGearboxPortOperStatus(const Port& port);

    bool decrFdbCount(const string& alias, int count);

    void setMACsecEnabledState(sai_object_id_t port_id, bool enabled);
    bool isMACsecPort(sai_object_id_t port_id) const;
    vector<sai_object_id_t> getPortVoQIds(Port& port);

    bool setPortPtIntfId(const Port& port, sai_uint16_t intf_id);
    bool setPortPtTimestampTemplate(const Port& port, sai_port_path_tracing_timestamp_type_t ts_type);

private:
    unique_ptr<CounterNameMapUpdater> m_counterNameMapUpdater;
    unique_ptr<Table> m_counterSysPortTable;
    unique_ptr<Table> m_counterLagTable;
    unique_ptr<Table> m_portTable;
    unique_ptr<Table> m_sendToIngressPortTable;
    unique_ptr<Table> m_systemPortTable;
    unique_ptr<Table> m_gearboxTable;
    unique_ptr<CounterNameMapUpdater> m_queueCounterNameMapUpdater;
    unique_ptr<Table> m_voqTable;
    unique_ptr<Table> m_queuePortTable;
    unique_ptr<Table> m_queueIndexTable;
    unique_ptr<Table> m_queueTypeTable;
    unique_ptr<CounterNameMapUpdater> m_pgCounterNameMapUpdater;
    unique_ptr<Table> m_pgPortTable;
    unique_ptr<Table> m_pgIndexTable;
    unique_ptr<Table> m_stateBufferMaximumValueTable;
    Table m_portStateTable;
    Table m_portOpErrTable;

    std::string getQueueWatermarkFlexCounterTableKey(std::string s);
    std::string getPriorityGroupWatermarkFlexCounterTableKey(std::string s);
    std::string getPriorityGroupDropPacketsFlexCounterTableKey(std::string s);
    std::string getPortRateFlexCounterTableKey(std::string s);
    std::string getWredQueueFlexCounterTableKey(std::string s);

    shared_ptr<DBConnector> m_counter_db;
    shared_ptr<DBConnector> m_state_db;
    shared_ptr<DBConnector> m_notificationsDb;

    FlexCounterTaggedCachedManager<void> port_stat_manager;
    FlexCounterTaggedCachedManager<void> port_buffer_drop_stat_manager;
    FlexCounterTaggedCachedManager<sai_queue_type_t> queue_stat_manager;
    FlexCounterTaggedCachedManager<sai_queue_type_t> queue_watermark_manager;
    FlexCounterTaggedCachedManager<void> pg_watermark_manager;
    FlexCounterTaggedCachedManager<void> pg_drop_stat_manager;
    FlexCounterTaggedCachedManager<void> wred_port_stat_manager;
    FlexCounterTaggedCachedManager<sai_queue_type_t> wred_queue_stat_manager;

    std::vector<std::reference_wrapper<FlexCounterCachedManager>> counter_managers;

    FlexCounterManager gb_port_stat_manager;
    shared_ptr<DBConnector> m_gb_counter_db;
    unique_ptr<Table> m_gbcounterTable;

    // Supported speeds on the system side.
    std::map<sai_object_id_t, PortSupportedSpeeds> m_portSupportedSpeeds;
    // Supported FEC modes on the system side.
    std::map<sai_object_id_t, PortFecModeCapability_t> m_portSupportedFecModes;

    bool m_initDone = false;
    bool m_isSendToIngressPortConfigured = false;
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

    unordered_set<string> m_vlanPorts;
    port_config_state_t m_portConfigState = PORT_CONFIG_MISSING;
    sai_uint32_t m_portCount;
    map<set<uint32_t>, sai_object_id_t> m_portListLaneMap;
    map<set<uint32_t>, PortConfig> m_lanesAliasSpeedMap;
    map<string, Port> m_portList;
    map<string, Port> m_pluggedModulesPort;
    map<string, vlan_members_t> m_portVlanMember;
    map<string, std::vector<sai_object_id_t>> m_port_voq_ids;
    /* mapping from SAI object ID to Name for faster
     * retrieval of Port/VLAN from object ID for events
     * coming from SAI
     */
    unordered_map<sai_object_id_t, string> saiOidToAlias;
    unordered_map<sai_object_id_t, uint16_t> m_portOidToIndex;
    map<string, uint32_t> m_port_ref_count;
    unordered_set<string> m_pendingPortSet;
    const uint32_t max_flood_control_types = 4;
    set<sai_vlan_flood_control_type_t> uuc_sup_flood_control_type;
    set<sai_vlan_flood_control_type_t> bc_sup_flood_control_type;
    map<string, uint32_t> m_bridge_port_ref_count;

    NotificationConsumer* m_portStatusNotificationConsumer;
    NotificationConsumer* m_portHostTxReadyNotificationConsumer;

    bool fec_override_sup = false;
    bool oper_fec_sup = false;
    bool saiHwTxSignalSupported = false;
    bool saiTxReadyNotifySupported = false;
    bool m_supportsHostIfTxQueue = false;

    swss::SelectableTimer *m_port_state_poller = nullptr;

    bool m_cmisModuleAsicSyncSupported = false;

    void doTask() override;
    void onWarmBootEnd() override;
    void doTask(Consumer &consumer);
    void doPortTask(Consumer &consumer);
    void doSendToIngressPortTask(Consumer &consumer);
    void doVlanTask(Consumer &consumer);
    void doVlanMemberTask(Consumer &consumer);
    void doLagTask(Consumer &consumer);
    void doLagMemberTask(Consumer &consumer);
    void doTransceiverPresenceCheck(Consumer &consumer);

    void doTask(NotificationConsumer &consumer);
    void handleNotification(NotificationConsumer &consumer, KeyOpFieldsValuesTuple& entry);
    void doTask(swss::SelectableTimer &timer);

    void removePortFromLanesMap(string alias);
    void removePortFromPortListMap(sai_object_id_t port_id);
    void removeDefaultVlanMembers();
    void removeDefaultBridgePorts();

    bool initializePorts(std::vector<Port>& ports);
    void initializePriorityGroupsBulk(std::vector<Port>& ports);
    void initializeQueuesBulk(std::vector<Port>& ports);
    void initializeSchedulerGroupsBulk(std::vector<Port>& ports);
    void initializePortHostTxReadyBulk(std::vector<Port>& ports);
    void initializePortMtuBulk(std::vector<Port>& ports);

    void initializePortBufferMaximumParameters(const Port &port);
    void initializeVoqs(Port &port);

    bool addHostIntfs(Port &port, string alias, sai_object_id_t &host_intfs_id, bool isUp);
    bool setHostIntfsStripTag(Port &port, sai_hostif_vlan_tag_t strip);

    bool setBridgePortLearnMode(Port &port, sai_bridge_port_fdb_learning_mode_t learn_mode);

    bool addVlan(string vlan);
    bool removeVlan(Port vlan);

    bool addLag(string lag, uint32_t spa_id, int32_t switch_id);
    bool removeLag(Port lag);
    bool setLagTpid(sai_object_id_t id, sai_uint16_t tpid);
    bool addLagMember(Port &lag, Port &port, string status);
    bool removeLagMember(Port &lag, Port &port);
    bool setCollectionOnLagMember(Port &lagMember, bool enableCollection);
    bool setDistributionOnLagMember(Port &lagMember, bool enableDistribution);

    sai_status_t removePort(sai_object_id_t port_id);
    bool initExistingPort(const PortConfig &port);
    bool initPortsBulk(std::vector<Port>& ports);
    void registerPort(Port &p);
    
    void deInitPort(string alias, sai_object_id_t port_id);

    void initPortCapAutoNeg(Port &port);
    void initPortCapLinkTraining(Port &port);

    void postPortInit(Port &p);

    bool setPortAdminStatus(Port &port, bool up);
    bool getPortAdminStatus(sai_object_id_t id, bool& up);
    bool getPortMtu(const Port& port, sai_uint32_t &mtu);
    bool getPortHostTxReady(const Port& port, bool &hostTxReadyVal);
    bool setPortMtu(const Port& port, sai_uint32_t mtu);
    bool setPortTpid(Port &port, sai_uint16_t tpid);
    bool setPortPvid (Port &port, sai_uint32_t pvid);
    bool getPortPvid(Port &port, sai_uint32_t &pvid);
    bool setPortFec(Port &port, sai_port_fec_mode_t fec_mode, bool override_fec);
    bool setPortFecOverride(sai_object_id_t port_obj, bool override_fec);
    bool setPortPfcAsym(Port &port, sai_port_priority_flow_control_mode_t pfc_asym);
    bool getDestPortId(sai_object_id_t src_port_id, dest_port_type_t port_type, sai_object_id_t &des_port_id);
    bool setPortMediaType(Port& port, const string &media_type);

    bool setBridgePortAdminStatus(sai_object_id_t id, bool up);

    bool setSaiHostTxSignal(const Port &port, bool enable);

    void setHostTxReady(Port port, const std::string &status);
    // Get supported speeds on system side
    bool isSpeedSupported(const std::string& alias, sai_object_id_t port_id, sai_uint32_t speed);
    void getPortSupportedSpeeds(const std::string& alias, sai_object_id_t port_id, PortSupportedSpeeds &supported_speeds);
    void initPortSupportedSpeeds(const std::string& alias, sai_object_id_t port_id);
    // Get supported FEC modes on system side
    bool isFecModeSupported(const Port &port, sai_port_fec_mode_t fec_mode);
    sai_status_t getPortSupportedFecModes(PortSupportedFecModes &supported_fecmodes, sai_object_id_t port_id);
    void initPortSupportedFecModes(const std::string& alias, sai_object_id_t port_id);
    task_process_status setPortSpeed(Port &port, sai_uint32_t speed);
    bool getPortSpeed(sai_object_id_t id, sai_uint32_t &speed);
    bool setGearboxPortsAttr(const Port &port, sai_port_attr_t id, void *value, bool override_fec=true);
    bool setGearboxPortAttr(const Port &port, dest_port_type_t port_type, sai_port_attr_t id, void *value, bool override_fec);

    bool getPortAdvSpeeds(const Port& port, bool remote, std::vector<sai_uint32_t>& speed_list);
    bool getPortAdvSpeeds(const Port& port, bool remote, string& adv_speeds);
    task_process_status setPortAdvSpeeds(Port &port, std::set<sai_uint32_t> &speed_list);

    bool getQueueTypeAndIndex(sai_object_id_t queue_id, sai_queue_type_t &type, uint8_t &index);

    bool m_isQueueMapGenerated = false;
    void generateQueueMapPerPort(const Port& port, FlexCounterQueueStates& queuesState, bool voq);
    bool m_isQueueFlexCountersAdded = false;
    void addQueueFlexCountersPerPort(const Port& port, FlexCounterQueueStates& queuesState);
    void addQueueFlexCountersPerPortPerQueueIndex(const Port& port, size_t queueIndex, bool voq, sai_queue_type_t queueType);

    bool m_isQueueWatermarkFlexCountersAdded = false;
    void addQueueWatermarkFlexCountersPerPort(const Port& port, FlexCounterQueueStates& queuesState);
    void addQueueWatermarkFlexCountersPerPortPerQueueIndex(const Port& port, size_t queueIndex, sai_queue_type_t queueType);

    bool m_isWredQueueCounterMapGenerated = false;
    void addWredQueueFlexCountersPerPort(const Port& port, FlexCounterQueueStates& queuesState);
    void addWredQueueFlexCountersPerPortPerQueueIndex(const Port& port, size_t queueIndex, bool voq, sai_queue_type_t queueType);

    bool m_isPriorityGroupMapGenerated = false;
    void generatePriorityGroupMapPerPort(const Port& port, FlexCounterPgStates& pgsState);
    bool m_isPriorityGroupFlexCountersAdded = false;
    void addPriorityGroupFlexCountersPerPort(const Port& port, FlexCounterPgStates& pgsState);
    void addPriorityGroupFlexCountersPerPortPerPgIndex(const Port& port, size_t pgIndex);

    bool m_isPriorityGroupWatermarkFlexCountersAdded = false;
    void addPriorityGroupWatermarkFlexCountersPerPort(const Port& port, FlexCounterPgStates& pgsState);
    void addPriorityGroupWatermarkFlexCountersPerPortPerPgIndex(const Port& port, size_t pgIndex);

    bool m_isPortCounterMapGenerated = false;
    bool m_isPortBufferDropCounterMapGenerated = false;

    bool isAutoNegEnabled(sai_object_id_t id);
    task_process_status setPortAutoNeg(Port &port, bool autoneg);
    task_process_status setPortUnreliableLOS(Port &port, bool enabled);
    task_process_status setPortInterfaceType(Port &port, sai_port_interface_type_t interface_type);
    task_process_status setPortAdvInterfaceTypes(Port &port, std::set<sai_port_interface_type_t> &interface_types);
    task_process_status setPortLinkTraining(const Port& port, bool state);

    ReturnCode setPortLinkEventDampingAlgorithm(Port &port,
                                                sai_redis_link_event_damping_algorithm_t &link_event_damping_algorithm);
    ReturnCode setPortLinkEventDampingAiedConfig(Port &port,
                                                 sai_redis_link_event_damping_algo_aied_config_t &config);

    void updatePortOperStatus(Port &port, sai_port_oper_status_t status);

    bool getPortOperSpeed(const Port& port, sai_uint32_t& speed) const;
    void updateDbPortOperSpeed(Port &port, sai_uint32_t speed);

    bool getPortLinkTrainingRxStatus(const Port &port, sai_port_link_training_rx_status_t &rx_status);
    bool getPortLinkTrainingFailure(const Port &port, sai_port_link_training_failure_status_t &failure);

    typedef enum {
        PORT_STATE_POLL_NONE = 0,
        PORT_STATE_POLL_AN   = 0x00000001, /* Auto Negotiation */
        PORT_STATE_POLL_LT   = 0x00000002  /* Link Trainig */
    } port_state_poll_t;

    map<string, uint32_t> m_port_state_poll;
    void updatePortStatePoll(const Port &port, port_state_poll_t type, bool active);
    void refreshPortStateAutoNeg(const Port &port);
    void refreshPortStateLinkTraining(const Port &port);

    void getPortSerdesVal(const std::string& s, std::vector<uint32_t> &lane_values, int base = 16);
    bool setPortSerdesAttribute(sai_object_id_t port_id, sai_object_id_t switch_id,
                                std::map<sai_port_serdes_attr_t, SerdesValue> &serdes_attr);

    void removePortSerdesAttribute(sai_object_id_t port_id);

    bool getSaiAclBindPointType(Port::Type                type,
                                sai_acl_bind_point_type_t &sai_acl_bind_type);

    ReturnCode addSendToIngressHostIf(const std::string &send_to_ingress_name);
    ReturnCode removeSendToIngressHostIf();
    void initGearbox();
    bool initGearboxPort(Port &port);
    bool getPortOperFec(const Port& port, sai_port_fec_mode_t &fec_mode) const;
    void updateDbPortOperFec(Port &port, string fec_str);
    void updatePortErrorStatus(Port &port, sai_port_error_status_t port_oper_eror);

    map<string, Port::Role> m_recircPortRole;

    //map key is tuple of <attached_switch_id, core_index, core_port_index>
    map<tuple<int, int, int>, systemPortMapInfo> m_systemPortOidMap;
    sai_uint32_t m_systemPortCount;
    bool getSystemPorts();
    bool addSystemPorts();
    void updateSystemPort(Port &port);
    unique_ptr<Table> m_tableVoqSystemLagTable;
    unique_ptr<Table> m_tableVoqSystemLagMemberTable;
    void voqSyncAddLag(Port &lag);
    void voqSyncDelLag(Port &lag);
    void voqSyncAddLagMember(Port &lag, Port &port, string status);
    void voqSyncDelLagMember(Port &lag, Port &port);
    unique_ptr<LagIdAllocator> m_lagIdAllocator;
    set<sai_object_id_t> m_macsecEnabledPorts;

    template <typename T>
    std::unordered_set<std::string> generateCounterStats(const vector<T> &counterIds, std::string (*serializer)(const T));

    map<sai_object_id_t, struct queueInfo> m_queueInfo;

    /* Protoypes for Path tracing */
    bool setPortPtTam(const Port& port, sai_object_id_t tam_id);

private:
    void initializeCpuPort();
    void initializePorts();

    auto getPortConfigState() const -> port_config_state_t;
    void setPortConfigState(port_config_state_t value);

    bool addPortBulk(const std::vector<PortConfig> &portList, std::vector<Port>& addedPorts);
    bool removePortBulk(const std::vector<sai_object_id_t> &portList);

    /* Prototypes for Path Tracing */
    bool checkPathTracingCapability();
    bool createPtTam();
    bool removePtTam(sai_object_id_t tam_id);
    bool createAndSetPortPtTam(const Port &p);
    bool unsetPortPtTam(const Port &p);
    sai_object_id_t m_ptTamReport = SAI_NULL_OBJECT_ID;
    sai_object_id_t m_ptTamInt = SAI_NULL_OBJECT_ID;
    sai_object_id_t m_ptTam = SAI_NULL_OBJECT_ID;
    uint32_t m_ptTamRefCount = 0;
    map<string, sai_object_id_t> m_portPtTam;
    // Define whether the switch supports or not Path Tracing
    bool m_isPathTracingSupported = false;
    void initCounterCapabilities(sai_object_id_t switchId);
    bool m_isWredPortCounterMapGenerated = false;
    std::unique_ptr<swss::Table> m_queueCounterCapabilitiesTable = nullptr;
    std::unique_ptr<swss::Table> m_portCounterCapabilitiesTable = nullptr;

private:
    // Port config aggregator
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_portConfigMap;

    // Port OA capabilities
    PortCapabilities m_portCap;

    // Port OA helper
    PortHelper m_portHlpr;
    bool m_isWarmRestoreStage = false;
};
#endif /* SWSS_PORTSORCH_H */
