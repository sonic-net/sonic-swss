extern "C"
{
#include "sai.h"
}

#include <map>
#include <string>

#include "portsorch.h"

#define PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS 1000
#define PORT_BUFFER_DROP_STAT_POLLING_INTERVAL_MS 60000
#define QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS 10000

PortsOrch::PortsOrch(DBConnector *db, DBConnector *stateDb, vector<table_name_with_pri_t> &tableNames,
                     DBConnector *chassisAppDb)
    : Orch(db, tableNames), m_portStateTable(stateDb, STATE_PORT_TABLE_NAME),
      port_stat_manager(PORT_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                        PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, true),
      port_buffer_drop_stat_manager(PORT_BUFFER_DROP_STAT_FLEX_COUNTER_GROUP, StatsMode::READ,
                                    PORT_BUFFER_DROP_STAT_POLLING_INTERVAL_MS, true),
      queue_stat_manager(QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                         QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, true)
{
}

bool PortsOrch::allPortsReady()
{
    return true;
}

bool PortsOrch::isInitDone()
{
    return true;
}

bool PortsOrch::isConfigDone()
{
    return true;
}

bool PortsOrch::isPortAdminUp(const string &alias)
{
    return true;
}

std::map<string, Port> &PortsOrch::getAllPorts()
{
    return m_portList;
}

bool PortsOrch::bake()
{
    return true;
}

void PortsOrch::cleanPortTable(const vector<string> &keys)
{
}

bool PortsOrch::getBridgePort(sai_object_id_t id, Port &port)
{
    return true;
}

bool PortsOrch::setBridgePortLearningFDB(Port &port, sai_bridge_port_fdb_learning_mode_t mode)
{
    return true;
}

bool PortsOrch::getPort(string alias, Port &port)
{
    if (m_portList.find(alias) == m_portList.end())
    {
        return false;
    }
    port = m_portList[alias];
    return true;
}

bool PortsOrch::getPort(sai_object_id_t id, Port &port)
{
    for (const auto &p : m_portList)
    {
        if (p.second.m_port_id == id)
        {
            port = p.second;
            return true;
        }
    }
    return false;
}

void PortsOrch::increasePortRefCount(const string &alias)
{
}

void PortsOrch::decreasePortRefCount(const string &alias)
{
}

bool PortsOrch::getPortByBridgePortId(sai_object_id_t bridge_port_id, Port &port)
{
    return true;
}

void PortsOrch::setPort(string alias, Port port)
{
    m_portList[alias] = port;
}

void PortsOrch::getCpuPort(Port &port)
{
}

bool PortsOrch::getInbandPort(Port &port)
{
    return true;
}

bool PortsOrch::getVlanByVlanId(sai_vlan_id_t vlan_id, Port &vlan)
{
    return true;
}

bool PortsOrch::setHostIntfsOperStatus(const Port &port, bool up) const
{
    return true;
}

void PortsOrch::updateDbPortOperStatus(const Port &port, sai_port_oper_status_t status) const
{
}

bool PortsOrch::createVlanHostIntf(Port &vl, string hostif_name)
{
    return true;
}

bool PortsOrch::removeVlanHostIntf(Port vl)
{
    return true;
}

bool PortsOrch::createBindAclTableGroup(sai_object_id_t port_oid, sai_object_id_t acl_table_oid,
                                        sai_object_id_t &group_oid, acl_stage_type_t acl_stage)
{
    return true;
}

bool PortsOrch::unbindRemoveAclTableGroup(sai_object_id_t port_oid, sai_object_id_t acl_table_oid,
                                          acl_stage_type_t acl_stage)
{
    return true;
}

bool PortsOrch::bindAclTable(sai_object_id_t id, sai_object_id_t table_oid, sai_object_id_t &group_member_oid,
                             acl_stage_type_t acl_stage)
{
    return true;
}

bool PortsOrch::unbindAclTable(sai_object_id_t port_oid, sai_object_id_t acl_table_oid,
                               sai_object_id_t acl_group_member_oid, acl_stage_type_t acl_stage)
{
    return true;
}

bool PortsOrch::bindUnbindAclTableGroup(Port &port, bool ingress, bool bind)
{
    return true;
}

bool PortsOrch::getPortPfc(sai_object_id_t portId, uint8_t *pfc_bitmask)
{
    return true;
}

bool PortsOrch::setPortPfc(sai_object_id_t portId, uint8_t pfc_bitmask)
{
    return true;
}

void PortsOrch::generateQueueMap()
{
}

void PortsOrch::generatePriorityGroupMap()
{
}

void PortsOrch::generatePortCounterMap()
{
}

void PortsOrch::generatePortBufferDropCounterMap()
{
}

void PortsOrch::refreshPortStatus()
{
}

bool PortsOrch::removeAclTableGroup(const Port &p)
{
    return true;
}

bool PortsOrch::addSubPort(Port &port, const string &alias, const string &vlan, const bool &adminUp,
                           const uint32_t &mtu)
{
    return true;
}

bool PortsOrch::removeSubPort(const string &alias)
{
    return true;
}

bool PortsOrch::updateL3VniStatus(uint16_t vlan_id, bool status)
{
    return true;
}

void PortsOrch::getLagMember(Port &lag, vector<Port> &portv)
{
}

void PortsOrch::updateChildPortsMtu(const Port &p, const uint32_t mtu)
{
}

bool PortsOrch::addTunnel(string tunnel, sai_object_id_t, bool learning)
{
    return true;
}

bool PortsOrch::removeTunnel(Port tunnel)
{
    return true;
}

bool PortsOrch::addBridgePort(Port &port)
{
    return true;
}

bool PortsOrch::removeBridgePort(Port &port)
{
    return true;
}

bool PortsOrch::addVlanMember(Port &vlan, Port &port, string &tagging_mode, string end_point_ip)
{
    return true;
}

bool PortsOrch::removeVlanMember(Port &vlan, Port &port, string end_point_ip)
{
    return true;
}

bool PortsOrch::isVlanMember(Port &vlan, Port &port, string end_point_ip)
{
    return true;
}

bool PortsOrch::addVlanFloodGroups(Port &vlan, Port &port, string end_point_ip)
{
    return true;
}

bool PortsOrch::removeVlanEndPointIp(Port &vlan, Port &port, string end_point_ip)
{
    return true;
}

void PortsOrch::increaseBridgePortRefCount(Port &port)
{
}

void PortsOrch::decreaseBridgePortRefCount(Port &port)
{
}

bool PortsOrch::getBridgePortReferenceCount(Port &port)
{
    return true;
}

bool PortsOrch::isInbandPort(const string &alias)
{
    return true;
}

bool PortsOrch::setVoqInbandIntf(string &alias, string &type)
{
    return true;
}

bool PortsOrch::getRecircPort(Port &p, string role)
{
    return true;
}

const gearbox_phy_t *PortsOrch::getGearboxPhy(const Port &port)
{
    return nullptr;
}

bool PortsOrch::getPortIPG(sai_object_id_t port_id, uint32_t &ipg)
{
    return true;
}

bool PortsOrch::setPortIPG(sai_object_id_t port_id, uint32_t ipg)
{
    return true;
}

bool PortsOrch::getPortOperStatus(const Port &port, sai_port_oper_status_t &status) const
{
    status = port.m_oper_status;
    return true;
}

std::string PortsOrch::getQueueWatermarkFlexCounterTableKey(std::string s)
{
    return "";
}

std::string PortsOrch::getPriorityGroupWatermarkFlexCounterTableKey(std::string s)
{
    return "";
}

std::string PortsOrch::getPriorityGroupDropPacketsFlexCounterTableKey(std::string s)
{
    return "";
}

std::string PortsOrch::getPortRateFlexCounterTableKey(std::string s)
{
    return "";
}

void PortsOrch::doTask()
{
}

void PortsOrch::doTask(Consumer &consumer)
{
}

void PortsOrch::doPortTask(Consumer &consumer)
{
}

void PortsOrch::doVlanTask(Consumer &consumer)
{
}

void PortsOrch::doVlanMemberTask(Consumer &consumer)
{
}

void PortsOrch::doLagTask(Consumer &consumer)
{
}

void PortsOrch::doLagMemberTask(Consumer &consumer)
{
}

void PortsOrch::doTask(NotificationConsumer &consumer)
{
}

void PortsOrch::removePortFromLanesMap(string alias)
{
}

void PortsOrch::removePortFromPortListMap(sai_object_id_t port_id)
{
}

void PortsOrch::removeDefaultVlanMembers()
{
}

void PortsOrch::removeDefaultBridgePorts()
{
}

bool PortsOrch::initializePort(Port &port)
{
    return true;
}

void PortsOrch::initializePriorityGroups(Port &port)
{
}

void PortsOrch::initializePortBufferMaximumParameters(Port &port)
{
}

void PortsOrch::initializeQueues(Port &port)
{
}

bool PortsOrch::addHostIntfs(Port &port, string alias, sai_object_id_t &host_intfs_id)
{
    return true;
}

bool PortsOrch::setHostIntfsStripTag(Port &port, sai_hostif_vlan_tag_t strip)
{
    return true;
}

bool PortsOrch::setBridgePortLearnMode(Port &port, string learn_mode)
{
    return true;
}

bool PortsOrch::addVlan(string vlan)
{
    return true;
}

bool PortsOrch::removeVlan(Port vlan)
{
    return true;
}

bool PortsOrch::addLag(string lag, uint32_t spa_id, int32_t switch_id)
{
    return true;
}

bool PortsOrch::removeLag(Port lag)
{
    return true;
}

bool PortsOrch::setLagTpid(sai_object_id_t id, sai_uint16_t tpid)
{
    return true;
}

bool PortsOrch::addLagMember(Port &lag, Port &port, bool enableForwarding)
{
    return true;
}

bool PortsOrch::removeLagMember(Port &lag, Port &port)
{
    return true;
}

bool PortsOrch::setCollectionOnLagMember(Port &lagMember, bool enableCollection)
{
    return true;
}

bool PortsOrch::setDistributionOnLagMember(Port &lagMember, bool enableDistribution)
{
    return true;
}

bool PortsOrch::addPort(const set<int> &lane_set, uint32_t speed, int an, string fec)
{
    return true;
}

sai_status_t PortsOrch::removePort(sai_object_id_t port_id)
{
    return SAI_STATUS_SUCCESS;
}

bool PortsOrch::initPort(const string &alias, const string &role, const int index, const set<int> &lane_set)
{
    return true;
}

void PortsOrch::deInitPort(string alias, sai_object_id_t port_id)
{
}

bool PortsOrch::setPortAdminStatus(Port &port, bool up)
{
    return true;
}

bool PortsOrch::getPortAdminStatus(sai_object_id_t id, bool &up)
{
    return true;
}

bool PortsOrch::setPortMtu(sai_object_id_t id, sai_uint32_t mtu)
{
    return true;
}

bool PortsOrch::setPortTpid(sai_object_id_t id, sai_uint16_t tpid)
{
    return true;
}

bool PortsOrch::setPortPvid(Port &port, sai_uint32_t pvid)
{
    return true;
}

bool PortsOrch::getPortPvid(Port &port, sai_uint32_t &pvid)
{
    return true;
}

bool PortsOrch::setPortFec(Port &port, sai_port_fec_mode_t mode)
{
    return true;
}

bool PortsOrch::setPortPfcAsym(Port &port, string pfc_asym)
{
    return true;
}

bool PortsOrch::getDestPortId(sai_object_id_t src_port_id, dest_port_type_t port_type, sai_object_id_t &des_port_id)
{
    return true;
}

bool PortsOrch::setBridgePortAdminStatus(sai_object_id_t id, bool up)
{
    return true;
}

bool PortsOrch::isSpeedSupported(const std::string &alias, sai_object_id_t port_id, sai_uint32_t speed)
{
    return true;
}

void PortsOrch::getPortSupportedSpeeds(const std::string &alias, sai_object_id_t port_id,
                                       PortSupportedSpeeds &supported_speeds)
{
}

void PortsOrch::initPortSupportedSpeeds(const std::string &alias, sai_object_id_t port_id)
{
}

task_process_status PortsOrch::setPortSpeed(Port &port, sai_uint32_t speed)
{
    return task_success;
}

bool PortsOrch::getPortSpeed(sai_object_id_t port_id, sai_uint32_t &speed)
{
    return true;
}

bool PortsOrch::setGearboxPortsAttr(Port &port, sai_port_attr_t id, void *value)
{
    return true;
}

bool PortsOrch::setGearboxPortAttr(Port &port, dest_port_type_t port_type, sai_port_attr_t id, void *value)
{
    return true;
}

task_process_status PortsOrch::setPortAdvSpeeds(sai_object_id_t port_id, std::vector<sai_uint32_t> &speed_list)
{
    return task_success;
}

bool PortsOrch::getQueueTypeAndIndex(sai_object_id_t queue_id, string &type, uint8_t &index)
{
    return true;
}

void PortsOrch::generateQueueMapPerPort(const Port &port)
{
}

void PortsOrch::generatePriorityGroupMapPerPort(const Port &port)
{
}

task_process_status PortsOrch::setPortAutoNeg(sai_object_id_t id, int an)
{
    return task_success;
}

bool PortsOrch::setPortFecMode(sai_object_id_t id, int fec)
{
    return true;
}

task_process_status PortsOrch::setPortInterfaceType(sai_object_id_t id, sai_port_interface_type_t interface_type)
{
    return task_success;
}

task_process_status PortsOrch::setPortAdvInterfaceTypes(sai_object_id_t id, std::vector<uint32_t> &interface_types)
{
    return task_success;
}

void PortsOrch::updatePortOperStatus(Port &port, sai_port_oper_status_t status)
{
}

bool PortsOrch::getPortOperSpeed(const Port &port, sai_uint32_t &speed) const
{
    return true;
}

void PortsOrch::updateDbPortOperSpeed(Port &port, sai_uint32_t speed)
{
}

void PortsOrch::getPortSerdesVal(const std::string &s, std::vector<uint32_t> &lane_values)
{
}

bool PortsOrch::getPortAdvSpeedsVal(const std::string &s, std::vector<uint32_t> &speed_values)
{
    return true;
}

bool PortsOrch::getPortInterfaceTypeVal(const std::string &s, sai_port_interface_type_t &interface_type)
{
    return true;
}

bool PortsOrch::getPortAdvInterfaceTypesVal(const std::string &s, std::vector<uint32_t> &type_values)
{
    return true;
}

void PortsOrch::removePortSerdesAttribute(sai_object_id_t port_id)
{
}

bool PortsOrch::getSaiAclBindPointType(Port::Type type, sai_acl_bind_point_type_t &sai_acl_bind_type)
{
    return true;
}

void PortsOrch::initGearbox()
{
}

bool PortsOrch::initGearboxPort(Port &port)
{
    return true;
}

bool PortsOrch::getSystemPorts()
{
    return true;
}

bool PortsOrch::addSystemPorts()
{
    return true;
}

void PortsOrch::voqSyncAddLag(Port &lag)
{
}

void PortsOrch::voqSyncDelLag(Port &lag)
{
}

void PortsOrch::voqSyncAddLagMember(Port &lag, Port &port)
{
}

void PortsOrch::voqSyncDelLagMember(Port &lag, Port &port)
{
}

std::unordered_set<std::string> PortsOrch::generateCounterStats(const string &type, bool gearbox)
{
    return {};
}

void PortsOrch::doTask(swss::SelectableTimer &timer)
{
}
