#include <unordered_map>
#include "pfcactionhandler.h"
#include "logger.h"
#include "sai_serialize.h"
#include "portsorch.h"
#include <vector>
#include <inttypes.h>

#define PFC_WD_QUEUE_STATUS             "PFC_WD_STATUS"
#define PFC_WD_QUEUE_STATUS_OPERATIONAL "operational"
#define PFC_WD_QUEUE_STATUS_STORMED     "stormed"

#define PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED "PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED"
#define PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED "PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED"

#define PFC_WD_QUEUE_STATS_TX_PACKETS "PFC_WD_QUEUE_STATS_TX_PACKETS"
#define PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS "PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS"
#define PFC_WD_QUEUE_STATS_RX_PACKETS "PFC_WD_QUEUE_STATS_RX_PACKETS"
#define PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS "PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS"

#define PFC_WD_QUEUE_STATS_TX_PACKETS_LAST "PFC_WD_QUEUE_STATS_TX_PACKETS_LAST"
#define PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST "PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST"
#define PFC_WD_QUEUE_STATS_RX_PACKETS_LAST "PFC_WD_QUEUE_STATS_RX_PACKETS_LAST"
#define PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST "PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST"

extern sai_object_id_t gSwitchId;
extern PortsOrch *gPortsOrch;
extern SwitchOrch *gSwitchOrch;
extern AclOrch * gAclOrch;
extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_buffer_api_t *sai_buffer_api;

PfcWdActionHandler::PfcWdActionHandler(sai_object_id_t port, sai_object_id_t queue,
        uint8_t queueId, shared_ptr<Table> countersTable):
    m_port(port),
    m_queue(queue),
    m_queueId(queueId),
    m_countersTable(countersTable)
{
    SWSS_LOG_ENTER();
}

PfcWdActionHandler::~PfcWdActionHandler(void)
{
    SWSS_LOG_ENTER();

}

void PfcWdActionHandler::initCounters(void)
{
    SWSS_LOG_ENTER();

    if (!getHwCounters(m_hwStats))
    {
        return;
    }

    auto wdQueueStats = getQueueStats(m_countersTable, sai_serialize_object_id(m_queue));
    // initCounters() is called when the event channel receives
    // a storm signal. This can happen when there is a true new storm or
    // when there is an existing storm ongoing before warm-reboot. In the latter case,
    // we treat the storm as an old storm. In particular,
    // we do not increment the detectCount so as to clamp the
    // gap between detectCount and restoreCount by 1 at maximum
    if (!(wdQueueStats.detectCount > wdQueueStats.restoreCount))
    {
        wdQueueStats.detectCount++;

        wdQueueStats.txPktLast = 0;
        wdQueueStats.txDropPktLast = 0;
        wdQueueStats.rxPktLast = 0;
        wdQueueStats.rxDropPktLast = 0;
    }
    wdQueueStats.operational = false;

    updateWdCounters(sai_serialize_object_id(m_queue), wdQueueStats);
}

void PfcWdActionHandler::commitCounters(bool periodic /* = false */)
{
    SWSS_LOG_ENTER();

    PfcWdHwStats hwStats;

    if (!getHwCounters(hwStats))
    {
        return;
    }

    auto finalStats = getQueueStats(m_countersTable, sai_serialize_object_id(m_queue));

    if (!periodic)
    {
        finalStats.restoreCount++;
    }
    finalStats.operational = !periodic;

    finalStats.txPktLast += hwStats.txPkt - m_hwStats.txPkt;
    finalStats.txDropPktLast += hwStats.txDropPkt - m_hwStats.txDropPkt;
    finalStats.rxPktLast += hwStats.rxPkt - m_hwStats.rxPkt;
    finalStats.rxDropPktLast += hwStats.rxDropPkt - m_hwStats.rxDropPkt;

    finalStats.txPkt += hwStats.txPkt - m_hwStats.txPkt;
    finalStats.txDropPkt += hwStats.txDropPkt - m_hwStats.txDropPkt;
    finalStats.rxPkt += hwStats.rxPkt - m_hwStats.rxPkt;
    finalStats.rxDropPkt += hwStats.rxDropPkt - m_hwStats.rxDropPkt;

    m_hwStats = hwStats;

    updateWdCounters(sai_serialize_object_id(m_queue), finalStats);
}

PfcWdActionHandler::PfcWdQueueStats PfcWdActionHandler::getQueueStats(shared_ptr<Table> countersTable, const string &queueIdStr)
{
    SWSS_LOG_ENTER();

    PfcWdQueueStats stats;
    memset(&stats, 0, sizeof(PfcWdQueueStats));
    stats.operational = true;
    vector<FieldValueTuple> fieldValues;

    if (!countersTable->get(queueIdStr, fieldValues))
    {
        return stats;
    }

    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);

        if (field == PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED)
        {
            stats.detectCount = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED)
        {
            stats.restoreCount = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATUS)
        {
            stats.operational = value == PFC_WD_QUEUE_STATUS_OPERATIONAL ? true : false;
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_PACKETS)
        {
            stats.txPkt = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS)
        {
            stats.txDropPkt = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_PACKETS)
        {
            stats.rxPkt = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS)
        {
            stats.rxDropPkt = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_PACKETS_LAST)
        {
            stats.txPktLast = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST)
        {
            stats.txDropPktLast = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_PACKETS_LAST)
        {
            stats.rxPktLast = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST)
        {
            stats.rxDropPktLast = stoul(value);
        }
    }

    return stats;
}

void PfcWdActionHandler::initWdCounters(shared_ptr<Table> countersTable, const string &queueIdStr)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> resultFvValues;

    auto stats = getQueueStats(countersTable, queueIdStr);

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, to_string(stats.detectCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, to_string(stats.restoreCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATUS, PFC_WD_QUEUE_STATUS_OPERATIONAL);

    countersTable->set(queueIdStr, resultFvValues);
}

void PfcWdActionHandler::updateWdCounters(const string& queueIdStr, const PfcWdQueueStats& stats)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> resultFvValues;

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, to_string(stats.detectCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, to_string(stats.restoreCount));

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_PACKETS, to_string(stats.txPkt));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS, to_string(stats.txDropPkt));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_PACKETS, to_string(stats.rxPkt));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS, to_string(stats.rxDropPkt));

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_PACKETS_LAST, to_string(stats.txPktLast));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST, to_string(stats.txDropPktLast));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_PACKETS_LAST, to_string(stats.rxPktLast));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST, to_string(stats.rxDropPktLast));

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATUS, stats.operational ?
                                                     PFC_WD_QUEUE_STATUS_OPERATIONAL :
                                                     PFC_WD_QUEUE_STATUS_STORMED);

    m_countersTable->set(queueIdStr, resultFvValues);
}

PfcWdSaiDlrInitHandler::PfcWdSaiDlrInitHandler(sai_object_id_t port, sai_object_id_t queue,
                                               uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdZeroBufferHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_PFC_DLR_INIT;
    attr.value.booldata = true;

    // Set DLR init to true to start PFC deadlock recovery
    sai_status_t status = sai_queue_api->set_queue_attribute(queue, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set PFC DLR INIT on port 0x%" PRIx64 " queue 0x%" PRIx64
                       " queueId %d : %d",
                       port, queue, queueId, status);
        return;
    }
}

PfcWdSaiDlrInitHandler::~PfcWdSaiDlrInitHandler(void)
{
    SWSS_LOG_ENTER();

    sai_object_id_t port = getPort();
    sai_object_id_t queue = getQueue();
    uint8_t queueId = getQueueId();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_PFC_DLR_INIT;
    attr.value.booldata = false;

    // Set DLR init to false to stop PFC deadlock recovery
    sai_status_t status = sai_queue_api->set_queue_attribute(getQueue(), &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to clear PFC DLR INIT on port 0x%" PRIx64 " queue 0x%" PRIx64
                       " queueId %d : %d", port, queue, queueId, status);
        return;
    }
}

PfcWdDlrHandler::PfcWdDlrHandler(sai_object_id_t port, sai_object_id_t queue,
                                               uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdLossyHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_PFC_DLR_INIT;
    attr.value.booldata = true;

    // Set DLR init to true to start PFC deadlock recovery
    sai_status_t status = sai_queue_api->set_queue_attribute(queue, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set PFC DLR INIT on port 0x%" PRIx64 " queue 0x%" PRIx64
                       " queueId %d : %d",
                       port, queue, queueId, status);
        return;
    }
}

PfcWdDlrHandler::~PfcWdDlrHandler(void)
{
    SWSS_LOG_ENTER();

    sai_object_id_t port = getPort();
    sai_object_id_t queue = getQueue();
    uint8_t queueId = getQueueId();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_PFC_DLR_INIT;
    attr.value.booldata = false;

    // Set DLR init to false to stop PFC deadlock recovery
    sai_status_t status = sai_queue_api->set_queue_attribute(getQueue(), &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to clear PFC DLR INIT on port 0x%" PRIx64 " queue 0x%" PRIx64
                       " queueId %d : %d", port, queue, queueId, status);
        return;
    }
}

PfcWdAclHandler::PfcWdAclHandler(sai_object_id_t port, sai_object_id_t queue,
        uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdLossyHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    string table_type;

    string queuestr = to_string(queueId);
    m_strRule = "Rule_PfcWdAclHandler_" + queuestr;

    // Ingress table/rule creation
    table_type = TABLE_TYPE_DROP;
    m_strIngressTable = INGRESS_TABLE_DROP;
    auto found = m_aclTables.find(m_strIngressTable);
    if (found == m_aclTables.end())
    {
        // First time of handling PFC for this queue, create ACL table, and bind
        createPfcAclTable(port, m_strIngressTable, true);
        shared_ptr<AclRulePacket> newRule = make_shared<AclRulePacket>(gAclOrch, m_strRule, m_strIngressTable);
        createPfcAclRule(newRule, queueId, m_strIngressTable, port);
    }
    else
    {
        AclRule* rule = gAclOrch->getAclRule(m_strIngressTable, m_strRule);
        if (rule == nullptr)
        {
            shared_ptr<AclRulePacket> newRule = make_shared<AclRulePacket>(gAclOrch, m_strRule, m_strIngressTable);
            createPfcAclRule(newRule, queueId, m_strIngressTable, port);
        } 
        else 
        {
            gAclOrch->updateAclRule(m_strIngressTable, m_strRule, MATCH_IN_PORTS, &port, RULE_OPER_ADD);
        }
    }

    // Egress table/rule creation
    table_type = TABLE_TYPE_PFCWD;

    // Use shared egress acl table for BRCM DNX platform.
    string platform = getenv("platform") ? getenv("platform") : "";
    string sub_platform = getenv("sub_platform") ? getenv("sub_platform") : "";
    shared_egress_acl_table = (platform == BRCM_PLATFORM_SUBSTRING &&
                               sub_platform == BRCM_DNX_PLATFORM_SUBSTRING);

    if (shared_egress_acl_table)
    {
        Port p;
        if (!gPortsOrch->getPort(port, p))
        {
            SWSS_LOG_ERROR("Failed to get port structure from port oid 0x%" PRIx64, port);
            return;
        }
        m_strEgressRule = "Egress_Rule_PfcWdAclHandler_" + p.m_alias + "_" + queuestr;
        m_strEgressTable = "EgressTable_PfcWdAclHandler";
        found = m_aclTables.find(m_strEgressTable);
        if (found == m_aclTables.end())
        {
            // First time of handling PFC, create ACL table and also ACL rule.
            createPfcAclTable(port, m_strEgressTable, false);
            shared_ptr<AclRulePacket> newRule = make_shared<AclRulePacket>(gAclOrch, m_strEgressRule, m_strEgressTable);
            createPfcAclRule(newRule, queueId, m_strEgressTable, port);
        }
        else
        {
            // ACL table already exists. Add ACL rule if needed.
            AclRule* rule = gAclOrch->getAclRule(m_strEgressTable, m_strEgressRule);
            if (rule == nullptr)
            {
                shared_ptr<AclRulePacket> newRule = make_shared<AclRulePacket>(gAclOrch, m_strEgressRule, m_strEgressTable);
                createPfcAclRule(newRule, queueId, m_strEgressTable, port);
            }
        }
    }
    else
    {
        m_strEgressTable = "EgressTable_PfcWdAclHandler_" + queuestr;
        found = m_aclTables.find(m_strEgressTable);
        if (found == m_aclTables.end())
        {
            // First time of handling PFC for this queue, create ACL table, and bind
            createPfcAclTable(port, m_strEgressTable, false);
            shared_ptr<AclRulePacket> newRule = make_shared<AclRulePacket>(gAclOrch, m_strRule, m_strEgressTable);
            createPfcAclRule(newRule, queueId, m_strEgressTable, port);
        }
        else
        {
            // Otherwise just bind ACL table with the port
            found->second.bind(port);
        }
    }
}

PfcWdAclHandler::~PfcWdAclHandler(void)
{
    SWSS_LOG_ENTER();

    AclRule* rule = gAclOrch->getAclRule(m_strIngressTable, m_strRule);
    if (rule == nullptr)
    {
        SWSS_LOG_THROW("ACL Rule does not exist for rule %s", m_strRule.c_str());
    }

    vector<sai_object_id_t> port_set = rule->getInPorts();
    sai_object_id_t port = getPort();

    if ((port_set.size() == 1) && (port_set[0] == port))
    {
        gAclOrch->removeAclRule(m_strIngressTable, m_strRule);
    }
    else 
    {
        gAclOrch->updateAclRule(m_strIngressTable, m_strRule, MATCH_IN_PORTS, &port, RULE_OPER_DELETE);
    } 

    if (shared_egress_acl_table)
    {
        rule = gAclOrch->getAclRule(m_strEgressTable, m_strEgressRule);
        if (rule == nullptr)
        {
            SWSS_LOG_THROW("Egress ACL Rule does not exist for rule %s", m_strEgressRule.c_str());
        }
        gAclOrch->removeAclRule(m_strEgressTable, m_strEgressRule);
    }
    else
    {
        auto found = m_aclTables.find(m_strEgressTable);
        found->second.unbind(port);
    }
}

void PfcWdAclHandler::clear()
{
    SWSS_LOG_ENTER();

    for (auto& tablepair: m_aclTables)
    {
        auto& table = tablepair.second;
        gAclOrch->removeAclTable(table.getId());
    }
}

void PfcWdAclHandler::createPfcAclTable(sai_object_id_t port, string strTable, bool ingress)
{
    SWSS_LOG_ENTER();

    auto inserted = m_aclTables.emplace(piecewise_construct,
        std::forward_as_tuple(strTable),
        std::forward_as_tuple(gAclOrch, strTable));

    assert(inserted.second);

    AclTable& aclTable = inserted.first->second;

    sai_object_id_t table_oid = gAclOrch->getTableById(strTable);
    if (ingress && table_oid != SAI_NULL_OBJECT_ID)
    {
        // DROP ACL table is already created
        SWSS_LOG_NOTICE("ACL table %s exists, reuse the same", strTable.c_str());
        aclTable = *(gAclOrch->getTableByOid(table_oid));
        return;
    }

    // Link port only for ingress ACL table or unshared egress ACL table.
    if (ingress || !shared_egress_acl_table)
    {
        aclTable.link(port);
    }

    if (ingress) 
    {
        auto dropType = gAclOrch->getAclTableType(TABLE_TYPE_DROP);
        assert(dropType);
        aclTable.validateAddType(*dropType);
        aclTable.stage = ACL_STAGE_INGRESS;
    } 
    else 
    {
        auto pfcwdType = gAclOrch->getAclTableType(TABLE_TYPE_PFCWD);
        assert(pfcwdType);
        aclTable.validateAddType(*pfcwdType);
        aclTable.stage = ACL_STAGE_EGRESS;
    }
    
    gAclOrch->addAclTable(aclTable);
}

void PfcWdAclHandler::createPfcAclRule(shared_ptr<AclRulePacket> rule, uint8_t queueId, string strTable, sai_object_id_t portOid)
{
    SWSS_LOG_ENTER();

    string attr_name, attr_value;

    attr_name = RULE_PRIORITY;
    attr_value = "999";
    rule->validateAddPriority(attr_name, attr_value);

    attr_name = MATCH_TC;
    attr_value = to_string(queueId);
    rule->validateAddMatch(attr_name, attr_value);

    // Add MATCH_IN_PORTS as match criteria for ingress table and MATCH_OUT_PORT as match creiteria for shared egress table.
    if (strTable == INGRESS_TABLE_DROP || shared_egress_acl_table)
    {
        Port p;
        if (strTable == INGRESS_TABLE_DROP) 
        {
            attr_name = MATCH_IN_PORTS;
        }
        else if (shared_egress_acl_table) {
            attr_name = MATCH_OUT_PORT;
        }
    
        if (!gPortsOrch->getPort(portOid, p))
        {
            SWSS_LOG_ERROR("Failed to get port structure from port oid 0x%" PRIx64, portOid);
            return;
        }
    
        attr_value = p.m_alias;
        rule->validateAddMatch(attr_name, attr_value);
    }

    attr_name = ACTION_PACKET_ACTION;
    attr_value = PACKET_ACTION_DROP;
    rule->validateAddAction(attr_name, attr_value);

    gAclOrch->addAclRule(rule, strTable);
}

std::map<std::string, AclTable> PfcWdAclHandler::m_aclTables;

PfcWdLossyHandler::PfcWdLossyHandler(sai_object_id_t port, sai_object_id_t queue,
        uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdActionHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    string platform = getenv("platform") ? getenv("platform") : "";
    if (platform == CISCO_8000_PLATFORM_SUBSTRING || ((platform == BRCM_PLATFORM_SUBSTRING) && (gSwitchOrch->checkPfcDlrInitEnable())))
    {
        SWSS_LOG_DEBUG("Skipping in constructor PfcWdLossyHandler for platform %s on port 0x%" PRIx64,
                       platform.c_str(), port);
        return;
    }

    uint8_t pfcMask = 0;

    if (!gPortsOrch->getPortPfc(port, &pfcMask))
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port 0x%" PRIx64, port);
    }

    pfcMask = static_cast<uint8_t>(pfcMask & ~(1 << queueId));

    if (!gPortsOrch->setPortPfc(port, pfcMask))
    {
        SWSS_LOG_ERROR("Failed to set PFC mask on port 0x%" PRIx64, port);
    }
}

PfcWdLossyHandler::~PfcWdLossyHandler(void)
{
    SWSS_LOG_ENTER();

    string platform = getenv("platform") ? getenv("platform") : "";
    if (platform == CISCO_8000_PLATFORM_SUBSTRING || ((platform == BRCM_PLATFORM_SUBSTRING) && (gSwitchOrch->checkPfcDlrInitEnable())))
    {
        SWSS_LOG_DEBUG("Skipping in destructor PfcWdLossyHandler for platform %s on port 0x%" PRIx64,
                       platform.c_str(), getPort());
        return;
    }

    uint8_t pfcMask = 0;

    if (!gPortsOrch->getPortPfc(getPort(), &pfcMask))
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port 0x%" PRIx64, getPort());
    }

    pfcMask = static_cast<uint8_t>(pfcMask | (1 << getQueueId()));

    if (!gPortsOrch->setPortPfc(getPort(), pfcMask))
    {
        SWSS_LOG_ERROR("Failed to set PFC mask on port 0x%" PRIx64, getPort());
    }
}

bool PfcWdLossyHandler::getHwCounters(PfcWdHwStats& counters)
{
    SWSS_LOG_ENTER();

    static const vector<sai_stat_id_t> queueStatIds =
    {
        SAI_QUEUE_STAT_PACKETS,
        SAI_QUEUE_STAT_DROPPED_PACKETS,
    };

    static const vector<sai_stat_id_t> pgStatIds =
    {
        SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS,
        SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS,
    };

    vector<uint64_t> queueStats;
    queueStats.resize(queueStatIds.size());

    sai_status_t status = sai_queue_api->get_queue_stats(
            getQueue(),
            static_cast<uint32_t>(queueStatIds.size()),
            queueStatIds.data(),
            queueStats.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to fetch queue 0x%" PRIx64 " stats: %d", getQueue(), status);
        return false;
    }

    // PG counters not yet supported in Mellanox platform
    Port portInstance;
    if (!gPortsOrch->getPort(getPort(), portInstance))
    {
        SWSS_LOG_ERROR("Cannot get port by ID 0x%" PRIx64, getPort());
        return false;
    }

    sai_object_id_t pg = portInstance.m_priority_group_ids[static_cast <size_t> (getQueueId())];
    vector<uint64_t> pgStats;
    pgStats.resize(pgStatIds.size());

    status = sai_buffer_api->get_ingress_priority_group_stats(
            pg,
            static_cast<uint32_t>(pgStatIds.size()),
            pgStatIds.data(),
            pgStats.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to fetch pg 0x%" PRIx64 " stats: %d", pg, status);
        return false;
    }

    counters.txPkt = queueStats[0];
    counters.txDropPkt = queueStats[1];
    counters.rxPkt = pgStats[0];
    counters.rxDropPkt = pgStats[1];

    return true;
}

PfcWdZeroBufferHandler::PfcWdZeroBufferHandler(sai_object_id_t port,
        sai_object_id_t queue, uint8_t queueId, shared_ptr<Table> countersTable):
    PfcWdLossyHandler(port, queue, queueId, countersTable)
{
    SWSS_LOG_ENTER();

    Port portInstance;
    if (!gPortsOrch->getPort(port, portInstance))
    {
        SWSS_LOG_ERROR("Cannot get port by ID 0x%" PRIx64, port);
        return;
    }

    setQueueLockFlag(portInstance, true);

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;

    // Get queue's buffer profile ID
    sai_status_t status = sai_queue_api->get_queue_attribute(queue, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get buffer profile ID on queue 0x%" PRIx64 ": %d", queue, status);
        return;
    }

    sai_object_id_t oldQueueProfileId = attr.value.oid;

    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;
    attr.value.oid = ZeroBufferProfile::getZeroBufferProfile();

    // Set our zero buffer profile
    status = sai_queue_api->set_queue_attribute(queue, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set buffer profile ID on queue 0x%" PRIx64 ": %d", queue, status);
        return;
    }

    // Save original buffer profile
    m_originalQueueBufferProfile = oldQueueProfileId;
}

PfcWdZeroBufferHandler::~PfcWdZeroBufferHandler(void)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_BUFFER_PROFILE_ID;
    attr.value.oid = m_originalQueueBufferProfile;

    // Set our zero buffer profile on a queue
    sai_status_t status = sai_queue_api->set_queue_attribute(getQueue(), &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set buffer profile ID on queue 0x%" PRIx64 ": %d", getQueue(), status);
        return;
    }

    Port portInstance;
    if (!gPortsOrch->getPort(getPort(), portInstance))
    {
        SWSS_LOG_ERROR("Cannot get port by ID 0x%" PRIx64, getPort());
        return;
    }

    setQueueLockFlag(portInstance, false);
}

void PfcWdZeroBufferHandler::setQueueLockFlag(Port& port, bool isLocked) const
{
    // set lock bits on queue
    for (size_t i = 0; i < port.m_queue_ids.size(); ++i)
    {
        if (port.m_queue_ids[i] == getQueue())
        {
            port.m_queue_lock[i] = isLocked;
        }
    }
    gPortsOrch->setPort(port.m_alias, port);
}

PfcWdZeroBufferHandler::ZeroBufferProfile::ZeroBufferProfile(void)
{
    SWSS_LOG_ENTER();
}

PfcWdZeroBufferHandler::ZeroBufferProfile::~ZeroBufferProfile(void)
{
    SWSS_LOG_ENTER();

    // Destroy egress profiles and pools
    destroyZeroBufferProfile();
}

PfcWdZeroBufferHandler::ZeroBufferProfile &PfcWdZeroBufferHandler::ZeroBufferProfile::getInstance(void)
{
    SWSS_LOG_ENTER();

    static ZeroBufferProfile instance;

    return instance;
}

sai_object_id_t PfcWdZeroBufferHandler::ZeroBufferProfile::getZeroBufferProfile()
{
    SWSS_LOG_ENTER();

    if (getInstance().getProfile() == SAI_NULL_OBJECT_ID)
    {
        getInstance().createZeroBufferProfile();
    }

    return getInstance().getProfile();
}

void PfcWdZeroBufferHandler::ZeroBufferProfile::createZeroBufferProfile()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attribs;
    sai_status_t status;

    // Create zero pool
    attr.id = SAI_BUFFER_POOL_ATTR_SIZE;
    attr.value.u64 = 0;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_POOL_ATTR_TYPE;
    attr.value.u32 = SAI_BUFFER_POOL_TYPE_EGRESS;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_POOL_ATTR_THRESHOLD_MODE;
    attr.value.u32 = SAI_BUFFER_POOL_THRESHOLD_MODE_DYNAMIC;
    attribs.push_back(attr);

    status = sai_buffer_api->create_buffer_pool(
        &getPool(),
        gSwitchId,
        static_cast<uint32_t>(attribs.size()),
        attribs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create dynamic zero buffer pool for PFC WD: %d", status);
        return;
    }

    // Create zero profile
    attribs.clear();

    attr.id = SAI_BUFFER_PROFILE_ATTR_POOL_ID;
    attr.value.oid = getPool();
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE;
    attr.value.u32 = SAI_BUFFER_PROFILE_THRESHOLD_MODE_DYNAMIC;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
    attr.value.u64 = 0;
    attribs.push_back(attr);

    attr.id = SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH;
    attr.value.s8 = -8;
    attribs.push_back(attr);

    status = sai_buffer_api->create_buffer_profile(
            &getProfile(),
            gSwitchId,
            static_cast<uint32_t>(attribs.size()),
            attribs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create dynamic zero buffer profile for PFC WD: %d", status);
        return;
    }
}

void PfcWdZeroBufferHandler::ZeroBufferProfile::destroyZeroBufferProfile()
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_buffer_api->remove_buffer_profile(getProfile());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove static zero buffer profile for PFC WD: %d", status);
        return;
    }

    status = sai_buffer_api->remove_buffer_pool(getPool());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove static zero buffer pool for PFC WD: %d", status);
    }
}
