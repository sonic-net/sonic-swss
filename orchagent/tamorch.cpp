#include "tamorch.h"

#include "saiextensions.h"
#include "logger.h"
#include "schema.h"
#include "converter.h"
#include "ipprefix.h"
#include "swssnet.h"
#include "directory.h"
#include "saihelper.h"
#include "aclorch.h"
#include "observer.h"
#include "portsorch.h"
#include "neighorch.h"
#include "routeorch.h"
#include "fdborch.h"
#include "intfsorch.h"
#include "macaddress.h"
#include "tokenize.h"

using namespace std;
using namespace swss;

extern sai_tam_api_t* sai_tam_api;
extern sai_port_api_t* sai_port_api;
extern sai_switch_api_t   *sai_switch_api;
extern sai_object_id_t gSwitchId;
extern MacAddress gMacAddress;
extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;
extern IntfsOrch *gIntfsOrch;


static acl_rule_attr_lookup_t aclL3ActionLookup =
{
    { ACTION_PACKET_ACTION,        SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION },
    { ACTION_TAM_INT_OBJECT,       SAI_ACL_ENTRY_ATTR_ACTION_TAM_INT_OBJECT },
    { ACTION_TAM_OBJECT,           SAI_ACL_ENTRY_ATTR_ACTION_TAM_OBJECT },
    { ACTION_INT_INSERT,           SAI_ACL_ENTRY_ATTR_ACTION_INT_INSERT },
    { ACTION_INT_DELETE,           SAI_ACL_ENTRY_ATTR_ACTION_INT_DELETE },
};


static acl_rule_attr_lookup_t aclMatchLookup =
{
    { MATCH_TAM_INT_TYPE,          SAI_ACL_ENTRY_ATTR_FIELD_TAM_INT_TYPE },
};


static acl_tam_int_type_lookup_t tamIntTypeLookup =
{
    { MATCH_TAM_INT_TYPE_IFA2,     SAI_TAM_INT_TYPE_IFA2 },
};

TamTransportEntry::TamTransportEntry(uint32_t transportType=SAI_TAM_TRANSPORT_TYPE_NONE,
        uint16_t l4SrcPort=0,
        uint16_t l4DstPort=0,
        const MacAddress& srcMac=MacAddress("00:00:00:00:00:00"),
        const MacAddress& dstMac=MacAddress("00:00:00:00:00:00"),
        sai_object_id_t transportObjId=SAI_NULL_OBJECT_ID):
    transportType(transportType),
    l4SrcPort(l4SrcPort),
    l4DstPort(l4DstPort),
    srcMac(srcMac),
    dstMac(dstMac),
    transportObjId(transportObjId)
{
    refCount = 0;
}

TamCollectorEntry::TamCollectorEntry(const string& collectorName):
    collectorName(collectorName)
{
    dscp = 32;
    l4DstPort = 0;
    collectorObjId = SAI_NULL_OBJECT_ID;
    transportObjId = SAI_NULL_OBJECT_ID;
    string alias = "";
    nexthopInfo.prefix = IpPrefix("0.0.0.0/0");
    nexthopInfo.nexthop = NextHopKey("0.0.0.0", alias);
    refCount = 0;
    resolved = false;
}

bool TamCollectorEntry::validConfig()
{
    return(!srcIp.isZero() && !dstIp.isZero() && l4DstPort != 0);
}

void TamOrch::tamTransportIncRefCountById(sai_object_id_t transportObjId) 
{
    auto transportIter = m_transportTables.find(transportObjId);
    if (transportIter == m_transportTables.end()) {
        SWSS_LOG_ERROR("transportObjId:%" PRIx64 " does not exist", transportObjId);
        return;
    }

    auto transportEntry = transportIter->second;
    transportEntry->refCount ++;
    SWSS_LOG_DEBUG("transportObj:%" PRIx64 " refCount:%d", transportObjId, transportEntry->refCount);
}

void TamOrch::tamTransportDecRefCountById(sai_object_id_t transportObjId) {
    auto transportIter = m_transportTables.find(transportObjId);
    if (transportIter == m_transportTables.end()) {
        SWSS_LOG_ERROR("transportObjId:%" PRIx64 " does not exist", transportObjId);
        return;
    }

    auto transportEntry = transportIter->second;
    if (transportEntry->refCount <= 0) {
        SWSS_LOG_ERROR("Invalid refCount transportObjId:%" PRIx64 " refCount:%d", transportObjId, transportEntry->refCount);
        return;
    }

    transportEntry->refCount --;
    SWSS_LOG_DEBUG("transportObj:%" PRIx64 " refCount:%d", transportObjId, transportEntry->refCount);
    if (transportEntry->refCount == 0) {
        tamTransportDelete(transportObjId);
        m_transportTables.erase(transportObjId);
    }
}

/**
 * @brief Increment reference count for all collectors used by a drop monitor session
 *
 * When a drop monitor session is created, it references one or more collectors.
 * This function increments the reference count for each collector to track how many
 * sessions are using them. This prevents collectors from being deleted while still in use.
 *
 * @param sessionEntry Shared pointer to the TAM session entry containing collector references
 */
void TamOrch::tamDropMonitorIncRefCountForCollector(
        std::shared_ptr<TamSessionEntry> sessionEntry)
{
    // For all the collectors referred to by the DropMonitor session,
    // increment the refCount
    for (const auto& collectorName : sessionEntry->dropSession.collectorNames)
    {
        assert(m_collectorTables.find(collectorName) != m_collectorTables.end());
        tamCollectorIncRefCountByName(collectorName);
        tamCollectorIncRefCountByName(collectorName);
    }
}

/**
 * @brief Decrement reference count for all collectors used by a drop monitor session
 *
 * When a drop monitor session is deleted, this function decrements the reference count
 * for each collector it was using. When a collector's reference count reaches zero,
 * it can be safely deleted.
 *
 * @param sessionEntry Shared pointer to the TAM session entry containing collector references
 */
void TamOrch::tamDropMonitorDecRefCountForCollector(
        std::shared_ptr<TamSessionEntry> sessionEntry)
{
    // For all the collectors referred to by the DropMonitor session,
    // decrement the refCount
    for (const auto& collectorName : sessionEntry->dropSession.collectorNames)
    {
        assert(m_collectorTables.find(collectorName) != m_collectorTables.end());
        tamCollectorDecRefCountByName(collectorName);
        tamCollectorDecRefCountByName(collectorName);
    }
}

void TamOrch::tamCollectorIncRefCountByName(const string& collectorName) {
    auto collectorIter = m_collectorTables.find(collectorName);
    if (collectorIter == m_collectorTables.end()) {
        SWSS_LOG_ERROR("Collector:%s does not exist", collectorName.c_str());
        return;
    }

    auto collectorEntry = collectorIter->second;
    collectorEntry->refCount ++;
    SWSS_LOG_DEBUG("Collector:%s refCount:%d", collectorName.c_str(), collectorEntry->refCount);
}

void TamOrch::tamCollectorDecRefCountByName(const string& collectorName) {
    auto collectorIter = m_collectorTables.find(collectorName);
    if (collectorIter == m_collectorTables.end()) {
        SWSS_LOG_ERROR("Collector:%s does not exist", collectorName.c_str());
        return;
    }

    auto collectorEntry = collectorIter->second;
    assert((collectorEntry->refCount-count) > 0);

    collectorEntry->refCount --;
    SWSS_LOG_DEBUG("Collector:%s refCount:%d", collectorName.c_str(), collectorEntry->refCount);
    if (collectorEntry->refCount == 0) {
        tamCollectorDelete(collectorEntry);
    }
}

AclRuleTam::AclRuleTam(AclOrch *aclOrch, string rule, string table, bool createCounter) :
    AclRule(aclOrch, rule, table, createCounter)
{
}

bool AclRuleTam::setMatch(sai_acl_entry_attr_t matchId, sai_acl_field_data_t matchData)
{
    SWSS_LOG_ENTER();

    /* Broadcom SAI bug where SAI_ACL_ENTRY_ATTR_FIELD_TAM_INT_TYPE when added to the table
     * will throw an error.  Catch that here and bypass the normal validateAclRuleAction() logic
     * that is otherwise taken
     */
    if (matchId == SAI_ACL_ENTRY_ATTR_FIELD_TAM_INT_TYPE)
    {
        SWSS_LOG_DEBUG("overriding AclRule::setMatch() for SAI_ACL_ENTRY_ATTR_FIELD_TAM_INT_TYPE");
        sai_attribute_t attr;
        attr.id = matchId;
        attr.value.aclfield = matchData;

        m_matches[matchId] = SaiAttrWrapper(SAI_OBJECT_TYPE_ACL_ENTRY, attr);
        return true;
    }

    return AclRule::setMatch(matchId, matchData);
}

bool AclRuleTam::setAction(sai_acl_entry_attr_t actionId, sai_acl_action_data_t actionData)
{
    SWSS_LOG_ENTER();

    /* isActionSupported(SAI_ACL_ENTRY_ATTR_ACTION_INT_INSERT) returns false, because it
     * calls into isAclActionSupported() which searches actionList.find(action) and comes
     * up empty.  This list is populated from sai_switch_api->get_switch_attribute() to enumerate
     * all supported ACL attribute ... whoops, another broadcom bug */
    if (actionId == SAI_ACL_ENTRY_ATTR_ACTION_INT_INSERT) {
        SWSS_LOG_DEBUG("overriding AclRule::setAction() for SAI_ACL_ENTRY_ATTR_ACTION_INT_INSERT");
        sai_attribute_t attr;
        attr.id = actionId;
        attr.value.aclaction = actionData;
        m_actions[actionId] = SaiAttrWrapper(SAI_OBJECT_TYPE_ACL_ENTRY, attr);
        return true;
    }

    return AclRule::setAction(actionId, actionData);
}

bool AclRuleTam::validateAddAction(string attrName, string attrValue)
{
    /* No valid actions with string values */
    return false;
}

bool AclRuleTam::validateAddAction(string attrName)
{
    SWSS_LOG_ENTER();
    sai_acl_action_data_t actionData;

    auto actionStr = attrName;

    if (attrName == ACTION_INT_INSERT)
    {
        actionData.parameter.booldata = true;
    }
    else if (attrName == ACTION_INT_DELETE)
    {
        actionData.parameter.booldata = true;
    }
    else
    {
        return false;
    }

    actionData.enable = true;

    return setAction(aclL3ActionLookup[actionStr], actionData);
}

bool AclRuleTam::validateAddAction(string attrName, sai_object_id_t attrValue)
{
    SWSS_LOG_ENTER();
    sai_acl_action_data_t actionData;

    auto actionStr = attrName;

    if (attrName == ACTION_TAM_INT_OBJECT || attrName == ACTION_TAM_OBJECT)
    {
        actionData.parameter.oid = attrValue;
    }
    else
    {
        return false;
    }

    actionData.enable = true;

    return setAction(aclL3ActionLookup[actionStr], actionData);
}

bool AclRuleTam::validateAddMatch(string attrName, string attrValueInput)
{
    SWSS_LOG_ENTER();
    string attrValue = to_upper(attrValueInput);
    sai_acl_field_data_t matchData{};

    if (aclMatchLookup.find(attrName) == aclMatchLookup.end())
    {
        SWSS_LOG_DEBUG("Delegating lookup to AclRule::validateAddMatch()");
        return AclRule::validateAddMatch(attrName, attrValue);
    }

    matchData.enable = true;

    try
    {
        if (attrName == MATCH_TAM_INT_TYPE)
        {
            if (tamIntTypeLookup.find(attrValue) == tamIntTypeLookup.end()) {
                SWSS_LOG_ERROR("Failed to parse %s attribute %s value. Unknown value.",
                               attrName.c_str(), attrValue.c_str());
                return false;
            }
            matchData.data.u8 = tamIntTypeLookup[attrValue];
            matchData.enable = true;
            matchData.mask.u8 = 0xFF;
        }
    }
    catch (exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse %s attribute %s value. Error: %s",
                       attrName.c_str(), attrValue.c_str(), e.what());
        return false;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Failed to parse %s attribute %s value.", attrName.c_str(), attrValue.c_str());
        return false;
    }

    return setMatch(aclMatchLookup[attrName], matchData);
}

bool AclRuleTam::validateAddPriority(string attrName, string attrValue)
{
    return AclRule::validateAddPriority(attrName, attrValue);
}

void AclRuleTam::onUpdate(SubjectType, void *)
{
    // Do nothing
}

bool AclRuleTam::validate()
{
    SWSS_LOG_ENTER();

    // TODO: add more validations
    if (m_matches.empty() || m_actions.size() < 1)
    {
        return false;
    }

    return true;
}

TamOrch::TamOrch(DBConnector *db, vector<string> tableNames, TableConnector stateDbConnector,
        AclOrch *aclOrch, PortsOrch *portOrch, VRFOrch *vrfOrch, RouteOrch *routeOrch,
        NeighOrch *neighOrch, FdbOrch *fdbOrch) :
    Orch(db, tableNames),
    m_aclOrch(aclOrch),
    m_portsOrch(portOrch),
    m_vrfOrch(vrfOrch),
    m_routeOrch(routeOrch),
    m_neighOrch(neighOrch),
    m_fdbOrch(fdbOrch),
    m_stateDbDropMonitorSessionTable(stateDbConnector.first, stateDbConnector.second)
{
    m_neighOrch->attach(this);

    // Clean up stale STATE_DB drop monitor session entries on startup/config reload
    vector<string> keys;
    m_stateDbDropMonitorSessionTable.getKeys(keys);
    for (const auto& key : keys)
    {
        m_stateDbDropMonitorSessionTable.del(key);
    }
}

TamOrch::~TamOrch()
{
}

uint32_t TamOrch::getSystemUniqueId()
{
    const uint8_t *mac = gMacAddress.getMac();

    /* Use last 2 bytes of the mac address */
    return static_cast<uint32_t>(mac[4] << 8 | mac[5]);
}

uint32_t TamOrch::getDeviceId()
{
    if (globalSettings.find("device-id") == globalSettings.end()) {
        return getSystemUniqueId();
    }
    return to_uint<uint32_t>(globalSettings["device-id"]);
}

uint32_t TamOrch::getEnterpriseId()
{
    if (globalSettings.find("enterprise-id") == globalSettings.end()) {
        return getSystemUniqueId();
    }
    return to_uint<uint32_t>(globalSettings["enterprise-id"]);
}

bool TamOrch::getIfaEnabled()
{
    if (globalSettings.find("ifa") == globalSettings.end()) {
        return false;
    }
    return globalSettings["ifa"] == "true";
}

bool TamOrch::doIfaTamReportCreate()
{
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;

    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("TAM: Creating TAM Report object");

    attr.id = SAI_TAM_REPORT_ATTR_TYPE;
    attr.value.s32 = SAI_TAM_REPORT_TYPE_IPFIX;
    attrs.push_back(attr);
    /* TODO: Is SAI_TAM_REPORT_ATTR_REPORT_MODE needed? */
    attr.id = SAI_TAM_REPORT_ATTR_REPORT_MODE;
    attr.value.s32 = SAI_TAM_REPORT_MODE_BULK;
    attrs.push_back(attr);
    attr.id = SAI_TAM_REPORT_ATTR_ENTERPRISE_NUMBER;
    attr.value.u32 = getEnterpriseId();
    attrs.push_back(attr);
    status = sai_tam_api->create_tam_report(&ifaTamReportObj, gSwitchId,
                                            (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error creating TAM REPORT id");
        auto taskStatus = handleSaiCreateStatus(SAI_API_TAM, status);
        if (taskStatus != task_success) {
            return parseHandleSaiStatusFailure(taskStatus);
        }
    }

    return true;
}

bool TamOrch::tamCollectorDelete(std::shared_ptr<TamCollectorEntry> collectorEntry)
{
    // If the objId is not valid, just return true since there is nothing to delete
    if (collectorEntry->collectorObjId == SAI_NULL_OBJECT_ID) {
        SWSS_LOG_ERROR("tamCollectorDelete: Invalid objId, returning true");
        return true;
    }

    auto status = sai_tam_api->remove_tam_collector(collectorEntry->collectorObjId);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error deleting TAM Collector object");
        if (handleSaiRemoveStatus(SAI_API_MIRROR, status) != task_success) {
            return false;
        }
    }

    // Clear the collectorObjId
    collectorEntry->collectorObjId = SAI_NULL_OBJECT_ID;

    // Since the collector is now deleted, dec ref on the transport object
    tamTransportDecRefCountById(collectorEntry->transportObjId);
    collectorEntry->transportObjId = SAI_NULL_OBJECT_ID;

    SWSS_LOG_NOTICE("Deleted tamCollector obj:%" PRIx64, collectorEntry->collectorObjId);

    return true;
}

bool TamOrch::tamCollectorCreate(
        std::shared_ptr<TamCollectorEntry> collectorEntry,
        sai_object_id_t tamTransportObj)
{
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;
    sai_object_id_t tamCollectorObj = SAI_NULL_OBJECT_ID;

    SWSS_LOG_ENTER();

    // Mandatory attributes
    attr.id = SAI_TAM_COLLECTOR_ATTR_SRC_IP;
    copy(attr.value.ipaddr, collectorEntry->srcIp);
    attrs.push_back(attr);

    attr.id = SAI_TAM_COLLECTOR_ATTR_DST_IP;
    copy(attr.value.ipaddr, collectorEntry->dstIp);
    attrs.push_back(attr);

    attr.id = SAI_TAM_COLLECTOR_ATTR_TRANSPORT;
    attr.value.oid = tamTransportObj;
    attrs.push_back(attr);

    // Optional attributes
    attr.id = SAI_TAM_COLLECTOR_ATTR_DSCP_VALUE;
    // TODO - There is a bug in BCM SAI where this DSCP value is not being
    // used correctly in the TOS field of the packet. Working around for the
    // issue here for now.  Once SAI fixes this, we can remove the workaround
    uint8_t tos = collectorEntry->dscp << 2;
    attr.value.u8 = tos;
    attrs.push_back(attr);

    status = sai_tam_api->create_tam_collector(&tamCollectorObj, gSwitchId,
            (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error creating TAM Collector object");
        auto taskStatus = handleSaiCreateStatus(SAI_API_TAM, status);
        if (taskStatus != task_success) {
            return parseHandleSaiStatusFailure(taskStatus);
        }
    }

    SWSS_LOG_NOTICE("TAM: Created TAM Collector object %" PRIx64, tamCollectorObj);
    collectorEntry->collectorObjId = tamCollectorObj;
    collectorEntry->transportObjId = tamTransportObj;
    tamCollectorIncRefCountByName(collectorEntry->collectorName);

    handleCollectorChange(collectorEntry->collectorName);
    return true;
}

bool TamOrch::tamTransportDelete(sai_object_id_t transportObjId)
{
    auto status = sai_tam_api->remove_tam_transport(transportObjId);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error deleting TAM transport object");
        auto taskStatus = handleSaiCreateStatus(SAI_API_TAM, status);
        if (taskStatus != task_success) {
            return parseHandleSaiStatusFailure(taskStatus);
        }
    }

    SWSS_LOG_NOTICE("Deleted tamTransport:%" PRIx64, transportObjId);
    return true;
}

bool TamOrch::tamTransportCreate(uint16_t l4SrcPort, uint16_t l4DstPort,
        const MacAddress& srcMac, const MacAddress& dstMac,
        sai_object_id_t& transportObjId)
{
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;

    SWSS_LOG_DEBUG("Creating TAM Transport l4SrcPort=%d, l4DstPort=%d",
            l4SrcPort, l4DstPort);

    attr.id = SAI_TAM_TRANSPORT_ATTR_TRANSPORT_TYPE;
    attr.value.s32 = SAI_TAM_TRANSPORT_TYPE_PORT;
    attrs.push_back(attr);

    attr.id = SAI_TAM_TRANSPORT_ATTR_SRC_PORT;
    attr.value.u32 = l4SrcPort;
    attrs.push_back(attr);

    attr.id = SAI_TAM_TRANSPORT_ATTR_DST_PORT;
    attr.value.u32 = l4DstPort;
    attrs.push_back(attr);

    attr.id = SAI_TAM_TRANSPORT_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, srcMac.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_TAM_TRANSPORT_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, dstMac.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    status = sai_tam_api->create_tam_transport(&transportObjId, gSwitchId,
                                               (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error creating TAM Transport object");
        auto taskStatus = handleSaiCreateStatus(SAI_API_TAM, status);
        if (taskStatus != task_success) {
            return parseHandleSaiStatusFailure(taskStatus);
        }
    }

    // Add the transport to the collection of transports
    auto transportEntry = make_shared<TamTransportEntry>(SAI_TAM_TRANSPORT_TYPE_PORT,
            l4SrcPort, l4DstPort, srcMac,
            dstMac, transportObjId);
    m_transportTables[ transportObjId ] = transportEntry;
    tamTransportIncRefCountById(transportObjId);

    SWSS_LOG_DEBUG("Created TAM Transport object %" PRIx64, transportObjId);
    return true;
}

bool TamOrch::doIfaTamReportDelete()
{
    sai_status_t status;
    SWSS_LOG_ENTER();
    if (!ifaTamReportObj)
    {
        return true;
    }

    status = sai_tam_api->remove_tam_report(ifaTamReportObj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error destroying TAM REPORT");
        return false;
    }

    ifaTamReportObj = SAI_NULL_OBJECT_ID;
    return true;
}

bool TamOrch::doIfaTamIntCreate()
{
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;

    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("TAM: Creating TAM INT object");

    attr.id = SAI_TAM_INT_ATTR_TYPE;
    attr.value.s32 = SAI_TAM_INT_TYPE_IFA2;
    attrs.push_back(attr);
    attr.id = SAI_TAM_INT_ATTR_DEVICE_ID;
    attr.value.u32 = getDeviceId();
    attrs.push_back(attr);
    attr.id = SAI_TAM_INT_ATTR_INLINE;
    attr.value.booldata = false;
    attrs.push_back(attr);
    attr.id = SAI_TAM_INT_ATTR_INT_PRESENCE_TYPE;
    attr.value.s32 = SAI_TAM_INT_PRESENCE_TYPE_L3_PROTOCOL;
    attrs.push_back(attr);
    attr.id = SAI_TAM_INT_ATTR_INT_PRESENCE_L3_PROTOCOL;
    attr.value.u8 = TAM_IFA_L3_PROTOCOL;
    attrs.push_back(attr);
    attr.id = SAI_TAM_INT_ATTR_EXTENSIONS_REPORT_ID;
    attr.value.oid = ifaTamReportObj;
    attrs.push_back(attr);
    status = sai_tam_api->create_tam_int(&ifaTamIntObj, gSwitchId,
                                         (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error creating TAM INT id");
        auto taskStatus = handleSaiCreateStatus(SAI_API_TAM, status);
        if (taskStatus != task_success) {
            return parseHandleSaiStatusFailure(taskStatus);
        }
    }
    return true;
}

bool TamOrch::doIfaTamIntDelete()
{
    sai_status_t status;
    SWSS_LOG_ENTER();

    if (!ifaTamIntObj)
    {
        return true;
    }

    status = sai_tam_api->remove_tam_int(ifaTamIntObj);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error destroying TAM INT");
        return false;
    }
    ifaTamIntObj = SAI_NULL_OBJECT_ID;
    return true;
}

bool TamOrch::doIfaAclTableCreate()
{
    SWSS_LOG_ENTER();

    AclTable tamAclTable(m_aclOrch, TAM_IFA_ACL_TABLE);

    SWSS_LOG_DEBUG("TAM: Initializing ACL TAM Table");
    static const auto tamTableType = AclTableTypeBuilder()
        .withBindPointType(SAI_ACL_BIND_POINT_TYPE_SWITCH)
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL))
        /* NOTE: The creation fails with this set.  This seems like a bug as the validator then fails when we try
         *       to specify this field in the ACL Rule.  So we have to do a hack to bypass that...
         * .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_TAM_INT_TYPE))
         */
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IP))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IP))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT))
        .withAction(SAI_ACL_ACTION_TYPE_TAM_INT_OBJECT)
        .build();

    if (!tamAclTable.validateAddType(tamTableType))
    {
        SWSS_LOG_ERROR("Failed to configure TAM IFA table type");
        return false;
    }

    if (!tamAclTable.validateAddStage(ACL_STAGE_INGRESS))
    {
        SWSS_LOG_ERROR("Failed to configure TAM IFA table stage");
        return false;
    }

    tamAclTable.setDescription("TAM IFA Table");

    if (!tamAclTable.validate())
    {
        SWSS_LOG_ERROR("Failed to validate TAM IFA table");
        return false;
    }

    if (!m_aclOrch->addAclTable(tamAclTable))
    {
        SWSS_LOG_ERROR("Failed to create TAM IFA table in SAI");
        return false;
    }

    if (!gSwitchOrch->bindAclTableToSwitch(ACL_STAGE_INGRESS, tamAclTable.getOid()))
    {
        SWSS_LOG_ERROR("Failed to bind TAM IFA table to switch");
        return false;
    }

    ifaAclTableCreated = true;

    return true;
}

bool TamOrch::doIfaAclTableDelete()
{
    SWSS_LOG_ENTER();
    if (!ifaAclTableCreated)
    {
        /* Nothing to do */
        return true;
    }

    SWSS_LOG_DEBUG("TAM: IFA ACL Table Destroy");
    if (!gSwitchOrch->unbindAclTableFromSwitch(ACL_STAGE_INGRESS, m_aclOrch->getTableById(TAM_IFA_ACL_TABLE)))
    {
        SWSS_LOG_ERROR("Failed to unbind IFA ACL table from switch");
        return false;
    }
    if (!m_aclOrch->removeAclTable(TAM_IFA_ACL_TABLE))
    {
        SWSS_LOG_ERROR("Failed to remove IFA ACL table from switch");
        return false;
    }

    ifaAclTableCreated = false;
    return true;
}

bool TamOrch::doIfaAclRuleCreate()
{
    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("TAM: ACL Rule Create");

    std::shared_ptr<AclRuleTam> tamRule = std::make_shared<AclRuleTam>(
            m_aclOrch, TAM_IFA_ACL_RULE, TAM_IFA_ACL_TABLE, false /* counter not supported */);

    SWSS_LOG_DEBUG("TAM: ACL Rule validateAddPriority");

    if (!tamRule->validateAddPriority(RULE_PRIORITY, "100")) {
        SWSS_LOG_ERROR("TAM ERROR: TamRule failed to add priority");
        return false;
    }

    SWSS_LOG_DEBUG("TAM: ACL Rule validateAddMatch MATCH_TAM_INT_TYPE");

    if (!tamRule->validateAddMatch(MATCH_TAM_INT_TYPE, MATCH_TAM_INT_TYPE_IFA2)) {
        SWSS_LOG_ERROR("TAM ERROR: TamRule failed to add int type");
        return false;
    }

    SWSS_LOG_DEBUG("TAM: ACL Rule validateAddAction ACTION_INT_INSERT");

    if (!tamRule->validateAddAction(ACTION_INT_INSERT)) {
        SWSS_LOG_ERROR("TAM ERROR: TamRule failed to add packet action");
        return false;
    }

    SWSS_LOG_DEBUG("TAM: ACL Rule validateAddAction ACTION_TAM_INT_OBJECT");

    if (!tamRule->validateAddAction(ACTION_TAM_INT_OBJECT, ifaTamIntObj)) {
        SWSS_LOG_ERROR("TAM ERROR: TamRule failed to add tam int object");
        return false;
    }

    SWSS_LOG_DEBUG("TAM: ACL Rule add to table");

    if (!m_aclOrch->addAclRule(tamRule, TAM_IFA_ACL_TABLE)) {
        SWSS_LOG_ERROR("TAM ERROR: TamTable failed to add rule to table");
        return false;
    }

    ifaAclRuleCreated = true;
    return true;
}

bool TamOrch::doIfaAclRuleDelete()
{
    if (!ifaAclRuleCreated)
    {
        /* Nothing to do */
        return true;
    }

    if (!m_aclOrch->removeAclRule(TAM_IFA_ACL_TABLE, TAM_IFA_ACL_RULE))
    {
        return false;
    }

    ifaAclRuleCreated = false;
    return true;
}

bool TamOrch::doIfaTransitCreate()
{
    SWSS_LOG_ENTER();

    if (!doIfaAclTableCreate())
    {
        SWSS_LOG_DEBUG("TAM: Failed to create ACL Table");
        return false;
    }

    if (!doIfaTamReportCreate()) {
        SWSS_LOG_DEBUG("TAM: Failed to create IFA TAM Report");
        return false;
    }

    if (!doIfaTamIntCreate()) {
        SWSS_LOG_DEBUG("TAM: Failed to create IFA INT Report");
        return false;
    }

    if (!doIfaAclRuleCreate()) {
        SWSS_LOG_DEBUG("TAM: Failed to create IFA ACL Rule");
        return false;
    }

    return true;
}

bool TamOrch::doIfaTransitDelete()
{
    SWSS_LOG_ENTER();

    if (!doIfaAclRuleDelete())
    {
        SWSS_LOG_DEBUG("TAM: IFA clean up failed to delete ACL rule");
        return false;
    }

    if (!doIfaTamIntDelete())
    {
        SWSS_LOG_DEBUG("TAM: IFA clean up failed to delete TAM INT");
        return false;
    }

    if (!doIfaTamReportDelete())
    {
        SWSS_LOG_DEBUG("TAM: IFA clean up failed to delete TAM Report");
        return false;
    }

    if (!doIfaAclTableDelete())
    {
        SWSS_LOG_DEBUG("TAM: IFA clean up failed to delete ACL Table");
        return false;
    }

    return true;
}

/**
 * @brief Recreate all active drop monitor sessions
 *
 * Since SAI layer prevents direct attribute updates on active TAM objects,
 * this method implements a tear-down and recreation approach:
 * 1. Collects all active drop monitor sessions
 * 2. Tears down all sessions in a batch
 * 3. Recreates all sessions with the new device-id/enterprise-id
 *
 * @return true if all sessions were successfully recreated, false if any errors occurred
 */
bool TamOrch::recreateDropMonitorSessions()
{
    SWSS_LOG_ENTER();

    // Collect all active drop monitor sessions that need recreation
    std::vector<std::shared_ptr<TamSessionEntry>> sessionsToRecreate;

    for (auto& sessionPair : m_sessionTables)
    {
        auto sessionEntry = sessionPair.second;
        if (sessionEntry->sessionType == tamSessionTypeDropMonitor && sessionEntry->active)
        {
            sessionsToRecreate.push_back(sessionEntry);
            SWSS_LOG_NOTICE("TAM: Session '%s' marked for recreation due to config change",
                           sessionEntry->sessionName.c_str());
        }
    }

    if (sessionsToRecreate.empty())
    {
        SWSS_LOG_DEBUG("TAM: No active drop monitor sessions to recreate");
        return true;
    }

    const size_t totalSessions = sessionsToRecreate.size();
    SWSS_LOG_NOTICE("TAM: Recreating %zu drop monitor session(s) due to config change", totalSessions);

    // Phase 1: Tear down all active sessions
    // Remove sessions that fail to delete from the list (they can't be safely recreated)
    auto it = sessionsToRecreate.begin();
    while (it != sessionsToRecreate.end())
    {
        auto& sessionEntry = *it;
        SWSS_LOG_NOTICE("TAM: Tearing down session '%s'", sessionEntry->sessionName.c_str());

        if (!tamDropMonitorSessionDelete(sessionEntry))
        {
            SWSS_LOG_ERROR("TAM: Failed to delete session '%s'", sessionEntry->sessionName.c_str());
            it = sessionsToRecreate.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Phase 2: Recreate all sessions that were successfully deleted
    // The new values are already in globalSettings, so tamDropMonitorSessionCreate
    // will pick them up via getDeviceId() and getEnterpriseId()
    size_t recreatedCount = 0;
    for (auto& sessionEntry : sessionsToRecreate)
    {
        SWSS_LOG_NOTICE("TAM: Recreating session '%s'", sessionEntry->sessionName.c_str());

        // Validate session before recreation
        if (!validateTamSession(sessionEntry))
        {
            SWSS_LOG_ERROR("TAM: Session '%s' validation failed during recreation, session will remain inactive",
                          sessionEntry->sessionName.c_str());
            continue;
        }

        if (!tamDropMonitorSessionCreate(sessionEntry))
        {
            SWSS_LOG_ERROR("TAM: Failed to recreate session '%s', session will remain inactive",
                          sessionEntry->sessionName.c_str());
            continue;
        }

        recreatedCount++;
    }

    SWSS_LOG_NOTICE("TAM: Recreated %zu of %zu drop monitor session(s)", recreatedCount, totalSessions);

    return (recreatedCount == totalSessions);
}

bool TamOrch::doIfaTransitUpdate()
{
    SWSS_LOG_ENTER();

    if (!doIfaTransitDelete())
    {
        SWSS_LOG_DEBUG("TAM: Failed to clean up prior IFA state");
        return false;
    }

    if (getIfaEnabled() && !doIfaTransitCreate())
    {
        SWSS_LOG_DEBUG("TAM: Failed to initialize IFA state");
        return false;
    }

    return true;
}

void TamOrch::doTamDeviceTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    // Track if device-id or enterprise-id changed across all updates in this batch
    // We batch the detection to avoid multiple recreations if both fields change together
    bool deviceIdChanged = false;
    bool enterpriseIdChanged = false;
    uint32_t oldDeviceId = getDeviceId();
    uint32_t oldEnterpriseId = getEnterpriseId();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string tableAttr = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            SWSS_LOG_DEBUG("TAM SET %s", tableAttr.c_str());
            if (tableAttr == "device")
            {
                // Partial update support: merge incoming fields with existing globalSettings
                // instead of clearing all settings. This preserves enterprise-id when only
                // device-id is updated, and vice versa.
                for (auto i : kfvFieldsValues(t))
                {
                    const std::string& field = fvField(i);
                    const std::string& value = fvValue(i);

                    // Check if device-id is changing
                    if (field == "device-id" &&
		        oldDeviceId != to_uint<uint32_t>(value.c_str())) {
                        deviceIdChanged = true;
                        SWSS_LOG_NOTICE("TAM: device-id changing from '%d' to '%s'",
                                         oldDeviceId, value.c_str());
                    }
                    // Check if enterprise-id is changing
                    else if (field == "enterprise-id" &&
		             oldEnterpriseId != to_uint<uint32_t>(value.c_str())) {
                        enterpriseIdChanged = true;
                        SWSS_LOG_NOTICE("TAM: enterprise-id changing from '%d' to '%s'",
                                         oldEnterpriseId, value.c_str());
                    }

                    globalSettings[field] = value;
                    SWSS_LOG_DEBUG("TAM SET %s key %s=%s", tableAttr.c_str(), field.c_str(), value.c_str());
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_DEBUG("TAM DEL %s", tableAttr.c_str());
            if (tableAttr == "device")
            {
                deviceIdChanged = true;
                enterpriseIdChanged = true;
                globalSettings.clear();
            }
        }

        /* Consume */
        it = consumer.m_toSync.erase(it);
    }

    // Recreate drop monitor sessions if device-id or enterprise-id changed
    // This is done after processing all updates to batch the recreation
    // (i.e., if both device-id and enterprise-id change, sessions are only recreated once)
    if (deviceIdChanged || enterpriseIdChanged)
    {
        SWSS_LOG_NOTICE("TAM: Device config changed (device-id: %s, enterprise-id: %s), "
                       "recreating affected drop monitor sessions",
                       deviceIdChanged ? "changed" : "unchanged",
                       enterpriseIdChanged ? "changed" : "unchanged");

        if (!recreateDropMonitorSessions())
        {
            SWSS_LOG_ERROR("TAM: Failed to recreate some drop monitor sessions after device config change");
            // Continue - sessions that failed will remain inactive but config is updated
        }
    }

    doIfaTransitUpdate();
}

void TamOrch::doTamCollectorTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string tableAttr = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            SWSS_LOG_NOTICE("TAM_COLLECTOR SET %s", tableAttr.c_str());
            createTamCollectorEntry(tableAttr, kfvFieldsValues(t));
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("TAM_COLLECTOR DEL %s", tableAttr.c_str());
            deleteTamCollectorEntry(tableAttr);
        }

        /* Consume */
        it = consumer.m_toSync.erase(it);
    }
}


void TamOrch::doTamSessionTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string tableAttr = kfvKey(t);
        string op = kfvOp(t);
        string tamSessionType = getTamSessionType(kfvFieldsValues(t));

        if (op == SET_COMMAND)
        {
            SWSS_LOG_DEBUG("TAM SET %s", tableAttr.c_str());
            if (tamSessionType == SESSION_TYPE_DROP_MONITOR) {
                createDropMonitorSession(tableAttr, kfvFieldsValues(t));
            } else {
                SWSS_LOG_ERROR("Invalid TAM Session name:%s type:%s",
                                tableAttr.c_str(), tamSessionType.c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_DEBUG("TAM DEL %s", tableAttr.c_str());
            auto sessionIter = m_sessionTables.find(tableAttr);
            if (sessionIter == m_sessionTables.end()) {
                // The session does not even exist.  Ignore
                SWSS_LOG_ERROR("Session:%s does not exist", tableAttr.c_str());
            } else {
                // Sessin exists
                auto sessionEntry = sessionIter->second;
                if (sessionEntry->sessionType == tamSessionTypeDropMonitor) {
                    // Drop-monitor session
                    deleteDropMonitorSession(sessionEntry);
                } else {
                    SWSS_LOG_ERROR("Invalid TAM Session name:%s type:%d",
                                    tableAttr.c_str(), sessionEntry->sessionType);
                }
            }
        }

        /* Consume */
        it = consumer.m_toSync.erase(it);
    }
}

void TamOrch::doTamFlowGroupTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);
        auto values = kfvFieldsValues(t);

        // Parse key to determine if it's a flow group table or rule
        size_t pipePos = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        bool isRule = (pipePos != string::npos);

        if (op == SET_COMMAND)
        {
            SWSS_LOG_DEBUG("TAM SET %s", key.c_str());
            map<string, string> config;
            for (auto &value : values)
            {
                config[fvField(value)] = fvValue(value);
            }

            if (isRule)
            {
                string groupKey = key.substr(0, pipePos);
                string ruleKey = key.substr(pipePos + 1);

                if (validateFlowGroupRuleConfig(config))
                {
                    if (doTamFlowGroupRuleCreate(groupKey, ruleKey, config))
                    {
                        SWSS_LOG_DEBUG("TAM flow group rule %s created successfully", key.c_str());
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to create TAM flow group rule %s", key.c_str());
                    }
                }
            }
            else
            {
                if (validateFlowGroupTableConfig(config))
                {
                    if (doTamFlowGroupTableCreate(key, config))
                    {
                        SWSS_LOG_DEBUG("TAM flow group table %s created successfully", key.c_str());
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to create TAM flow group table %s", key.c_str());
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_DEBUG("TAM DEL %s", key.c_str());
            if (isRule)
            {
                string groupKey = key.substr(0, pipePos);
                string ruleKey = key.substr(pipePos + 1);

                if (doTamFlowGroupRuleDelete(groupKey, ruleKey))
                {
                    SWSS_LOG_DEBUG("TAM flow group rule %s deleted successfully", key.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to delete TAM flow group rule %s", key.c_str());
                }
            }
            else
            {
                if (doTamFlowGroupTableDelete(key))
                {
                    SWSS_LOG_DEBUG("TAM flow group table %s deleted successfully", key.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to delete TAM flow group table %s", key.c_str());
                }
            }
        }

        /* Consume */
        it = consumer.m_toSync.erase(it);
    }
}

bool TamOrch::doTamFlowGroupTableCreate(const string &key, const map<string, string> &config)
{
    SWSS_LOG_ENTER();

    TamFlowGroupTable flowGroupTable;

    auto agingIt = config.find("aging_interval");
    if (agingIt != config.end())
    {
        flowGroupTable.agingInterval = agingIt->second;
    }

    // Create dedicated ACL table for this flow group
    string aclTableName = "TAM_FLOW_GROUP_" + key;
    AclTable aclTable(m_aclOrch, aclTableName);

    auto portsIt = config.find("ports");
    if (portsIt != config.end())
    {
        string portsStr = portsIt->second;
        auto portList = tokenize(portsStr, ',');

        for (const auto &portName : portList)
        {
            Port port;
            if (!gPortsOrch->getPort(portName, port))
            {
                SWSS_LOG_ERROR("Failed to locate port %s for TAM flow group table %s",
                               portName.c_str(), key.c_str());
                return false;
            }

            sai_object_id_t portOid;
            if (!m_aclOrch->getAclBindPortId(port, portOid))
            {
                SWSS_LOG_ERROR("Failed to get bind port ID for port %s in TAM flow group table %s",
                               portName.c_str(), key.c_str());
                return false;
            }

	    flowGroupTable.ports.push_back(portName);
            aclTable.ports.emplace(portOid, SAI_NULL_OBJECT_ID);
        }
    }

    auto tableType = AclTableTypeBuilder()
        .withBindPointType(SAI_ACL_BIND_POINT_TYPE_PORT)
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_SRC_IP))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_DST_IP))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT))
        .withMatch(make_shared<AclTableMatch>(SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT))
        .withAction(SAI_ACL_ACTION_TYPE_TAM_OBJECT)
        .build();

    aclTable.type = tableType;

    // Create ACL table through AclOrch
    if (m_aclOrch->addAclTable(aclTable))
    {
        flowGroupTable.aclTableId = aclTableName;
        flowGroupTable.active = true;
        tamFlowGroupTables[key] = flowGroupTable;

        SWSS_LOG_DEBUG("TAM flow group table %s created with ACL table ID %s",
                       key.c_str(), flowGroupTable.aclTableId.c_str());
        return true;
    }

    return false;
}

bool TamOrch::doTamFlowGroupTableDelete(const string &key)
{
    SWSS_LOG_ENTER();

    auto it = tamFlowGroupTables.find(key);
    if (it == tamFlowGroupTables.end())
    {
        SWSS_LOG_ERROR("TAM flow group table %s not found", key.c_str());
        return false;
    }

    // Delete all rules first
    if (!it->second.aclTableId.empty())
    {
        for (auto &rulePair : it->second.rules)
        {
            if (rulePair.second.aclRule)
            {
                m_aclOrch->removeAclRule(it->second.aclTableId, rulePair.second.aclRule->getId());
            }
        }

        // Delete ACL table
        m_aclOrch->removeAclTable(it->second.aclTableId);
    }

    tamFlowGroupTables.erase(it);
    return true;
}

bool TamOrch::hasDropMonitorSessionForFlowGroup(const string &flowGroupName)
{
    SWSS_LOG_ENTER();

    for (const auto &sessionPair : m_sessionTables)
    {
        const auto &sessionEntry = sessionPair.second;
        if (sessionEntry->sessionType == tamSessionTypeDropMonitor &&
            sessionEntry->dropSession.flowGroupName == flowGroupName)
        {
            SWSS_LOG_DEBUG("Found DropMonitorSession %s for flow group %s",
                          sessionPair.first.c_str(), flowGroupName.c_str());
            return true;
        }
    }

    SWSS_LOG_DEBUG("No DropMonitorSession found for flow group %s", flowGroupName.c_str());
    return false;
}

sai_object_id_t TamOrch::getTamObjectIdForFlowGroup(const string &flowGroupName)
{
    SWSS_LOG_ENTER();

    for (const auto &sessionPair : m_sessionTables)
    {
        const auto &sessionEntry = sessionPair.second;
        if (sessionEntry->sessionType == tamSessionTypeDropMonitor &&
            sessionEntry->dropSession.flowGroupName == flowGroupName &&
            sessionEntry->active)
        {
            SWSS_LOG_DEBUG("Found active DropMonitorSession %s for flow group %s with TAM object ID %" PRIx64,
                          sessionPair.first.c_str(), flowGroupName.c_str(), sessionEntry->dropSession.tamObjId);
            return sessionEntry->dropSession.tamObjId;
        }
    }

    SWSS_LOG_DEBUG("No active DropMonitorSession found for flow group %s", flowGroupName.c_str());
    return SAI_NULL_OBJECT_ID;
}

void TamOrch::activateFlowGroupRulesForDropMonitor(const string &flowGroupName)
{
    SWSS_LOG_ENTER();

    auto groupIt = tamFlowGroupTables.find(flowGroupName);
    if (groupIt == tamFlowGroupTables.end())
    {
        SWSS_LOG_DEBUG("No flow group table found for %s", flowGroupName.c_str());
        return;
    }

    SWSS_LOG_INFO("Activating flow group rules for DropMonitorSession: %s", flowGroupName.c_str());

    for (auto &rulePair : groupIt->second.rules)
    {
        auto &rule = rulePair.second;
        const string &ruleKey = rulePair.first;

        // Skip rules that are already active
        if (rule.active && rule.aclRuleOid != SAI_NULL_OBJECT_ID)
        {
            continue;
        }

        // Create AclRuleTam for inactive rule
        auto aclRule = make_shared<AclRuleTam>(m_aclOrch, ruleKey, groupIt->second.aclTableId, false);

        // Configure rule matches
        if (!rule.srcIp.empty())
        {
            aclRule->validateAddMatch(MATCH_SRC_IP, rule.srcIp);
        }
        if (!rule.dstIp.empty())
        {
            aclRule->validateAddMatch(MATCH_DST_IP, rule.dstIp);
        }
        if (!rule.ipProtocol.empty())
        {
            aclRule->validateAddMatch(MATCH_IP_PROTOCOL, rule.ipProtocol);
        }
        if (!rule.srcL4Port.empty())
        {
            aclRule->validateAddMatch(MATCH_L4_SRC_PORT, rule.srcL4Port);
        }
        if (!rule.dstL4Port.empty())
        {
            aclRule->validateAddMatch(MATCH_L4_DST_PORT, rule.dstL4Port);
        }

        // Add ACL action for drop monitoring - reference the TAM object
        sai_object_id_t tamObjId = getTamObjectIdForFlowGroup(flowGroupName);
        if (tamObjId == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("No active TAM object found for flow group %s", flowGroupName.c_str());
            continue;  // Skip this rule but continue with others
        }

        if (!aclRule->validateAddAction(ACTION_TAM_OBJECT, tamObjId))
        {
            SWSS_LOG_ERROR("Failed to add TAM object action for TAM flow group rule %s", ruleKey.c_str());
            continue;  // Skip this rule but continue with others
        }

        // Create ACL rule through AclOrch
        if (m_aclOrch->addAclRule(aclRule, groupIt->second.aclTableId))
        {
            rule.aclRule = aclRule;
            rule.aclRuleOid = aclRule->getOid();
            rule.active = true;

            SWSS_LOG_DEBUG("Activated TAM flow group rule %s for DropMonitorSession %s",
                           ruleKey.c_str(), flowGroupName.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Failed to activate TAM flow group rule %s for DropMonitorSession %s",
                          ruleKey.c_str(), flowGroupName.c_str());
        }
    }
}

void TamOrch::deactivateFlowGroupRulesForDropMonitor(const string &flowGroupName)
{
    SWSS_LOG_ENTER();

    auto groupIt = tamFlowGroupTables.find(flowGroupName);
    if (groupIt == tamFlowGroupTables.end())
    {
        SWSS_LOG_DEBUG("No flow group table found for %s", flowGroupName.c_str());
        return;
    }

    SWSS_LOG_INFO("Deactivating flow group rules for DropMonitorSession: %s", flowGroupName.c_str());

    for (auto &rulePair : groupIt->second.rules)
    {
        auto &rule = rulePair.second;
        const string &ruleKey = rulePair.first;

        // Skip rules that are already inactive
        if (!rule.active || rule.aclRuleOid == SAI_NULL_OBJECT_ID)
        {
            continue;
        }

        // Remove ACL rule
        if (rule.aclRule && !groupIt->second.aclTableId.empty())
        {
            if (m_aclOrch->removeAclRule(groupIt->second.aclTableId, rule.aclRule->getId()))
            {
                SWSS_LOG_DEBUG("Deactivated TAM flow group rule %s for DropMonitorSession %s",
                               ruleKey.c_str(), flowGroupName.c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Failed to remove ACL rule for TAM flow group rule %s", ruleKey.c_str());
            }
        }

        // Mark rule as inactive but keep the configuration
        rule.aclRule = nullptr;
        rule.aclRuleOid = SAI_NULL_OBJECT_ID;
        rule.active = false;
    }
}

bool TamOrch::doTamFlowGroupRuleCreate(const string &groupKey, const string &ruleKey,
                                       const map<string, string> &config)
{
    SWSS_LOG_ENTER();

    auto groupIt = tamFlowGroupTables.find(groupKey);
    if (groupIt == tamFlowGroupTables.end())
    {
        SWSS_LOG_ERROR("TAM flow group table %s not found for rule %s", groupKey.c_str(), ruleKey.c_str());
        return false;
    }

    TamFlowGroupRule rule;

    // Parse configuration fields
    rule.srcIp = config.find("src_ip_prefix")->second;
    rule.dstIp = config.find("dst_ip_prefix")->second;

    auto protocolIt = config.find("ip_protocol");
    if (protocolIt != config.end())
    {
        rule.ipProtocol = protocolIt->second;
    }

    auto srcPortIt = config.find("l4_src_port");
    if (srcPortIt != config.end())
    {
        rule.srcL4Port = srcPortIt->second;
    }
    auto dstPortIt = config.find("l4_dst_port");
    if (dstPortIt != config.end())
    {
        rule.dstL4Port = dstPortIt->second;
    }

    // Check if a DropMonitorSession exists with flowGroupName == groupKey
    bool hasDropMonitorSession = hasDropMonitorSessionForFlowGroup(groupKey);

    if (hasDropMonitorSession)
    {
        // Create AclRuleTam and add to ACL table
        auto aclRule = make_shared<AclRuleTam>(
                m_aclOrch, ruleKey, groupIt->second.aclTableId, false /* counter not supported */);

        // Configure rule matches
        if (!rule.srcIp.empty())
        {
            aclRule->validateAddMatch(MATCH_SRC_IP, rule.srcIp);
        }
        if (!rule.dstIp.empty())
        {
            aclRule->validateAddMatch(MATCH_DST_IP, rule.dstIp);
        }
        if (!rule.ipProtocol.empty())
        {
            aclRule->validateAddMatch(MATCH_IP_PROTOCOL, rule.ipProtocol);
        }
        if (!rule.srcL4Port.empty())
        {
            aclRule->validateAddMatch(MATCH_L4_SRC_PORT, rule.srcL4Port);
        }
        if (!rule.dstL4Port.empty())
        {
            aclRule->validateAddMatch(MATCH_L4_DST_PORT, rule.dstL4Port);
        }

        // Add ACL action for drop monitoring - reference the TAM object
        sai_object_id_t tamObjId = getTamObjectIdForFlowGroup(groupKey);
        if (tamObjId == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("No active TAM object found for flow group %s", groupKey.c_str());
            return false;
        }

        if (!aclRule->validateAddAction(ACTION_TAM_OBJECT, tamObjId))
        {
            SWSS_LOG_ERROR("Failed to add TAM object action for TAM flow group rule %s", ruleKey.c_str());
            return false;
        }

        // Create ACL rule through AclOrch
        if (m_aclOrch->addAclRule(aclRule, groupIt->second.aclTableId))
        {
            rule.aclRule = aclRule;
            rule.aclRuleOid = aclRule->getOid();
            rule.active = true;

            groupIt->second.rules[ruleKey] = rule;
            SWSS_LOG_DEBUG("TAM flow group rule %s created with ACL rule for group %s",
                            ruleKey.c_str(), groupKey.c_str());
            return true;
        }
        else
        {
            SWSS_LOG_ERROR("Failed to create ACL rule for TAM flow group rule %s", ruleKey.c_str());
            return false;
        }
    }
    else
    {
        // No DropMonitorSession found - save rule without ACL rule
        rule.aclRule = nullptr;
        rule.aclRuleOid = SAI_NULL_OBJECT_ID;
        rule.active = false;  // Mark as inactive since no ACL rule is created

        groupIt->second.rules[ruleKey] = rule;
        SWSS_LOG_DEBUG("TAM flow group rule %s saved without ACL rule (no DropMonitorSession for group %s)",
                       ruleKey.c_str(), groupKey.c_str());
        return true;
    }
}

bool TamOrch::doTamFlowGroupRuleDelete(const string &groupKey, const string &ruleKey)
{
    auto groupIt = tamFlowGroupTables.find(groupKey);
    if (groupIt == tamFlowGroupTables.end())
    {
        return false;
    }

    auto ruleIt = groupIt->second.rules.find(ruleKey);
    if (ruleIt == groupIt->second.rules.end())
    {
        return false;
    }

    if (ruleIt->second.aclRule && !groupIt->second.aclTableId.empty())
    {
        m_aclOrch->removeAclRule(groupIt->second.aclTableId, ruleIt->second.aclRule->getId());
    }

    groupIt->second.rules.erase(ruleIt);
    return true;
}

bool TamOrch::validateFlowGroupTableConfig(const map<string, string> &config)
{
    SWSS_LOG_ENTER();

    // Validate required fields
    auto it = config.find("ports");
    if (it == config.end() || it->second.empty())
    {
        SWSS_LOG_ERROR("TAM flow group table 'ports' field is missing or empty");
        return false;
    }

    return true;
}

bool TamOrch::validateFlowGroupRuleConfig(const map<string, string> &config)
{
    SWSS_LOG_ENTER();

    // Validate required fields
    if (config.find("src_ip_prefix") == config.end() || config.find("dst_ip_prefix") == config.end())
    {
        SWSS_LOG_ERROR("TAM flow group rule missing required IP prefix fields");
        return false;
    }

    return true;
}

void TamOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string tableName = consumer.getTableName();

    if (tableName == CFG_TAM_TABLE_NAME)
    {
        doTamDeviceTableTask(consumer);
    }
    else if (tableName == CFG_TAM_COLLECTOR_TABLE_NAME)
    {
        doTamCollectorTableTask(consumer);
    }
    else if (tableName == CFG_TAM_FLOW_GROUP_TABLE_NAME)
    {
        doTamFlowGroupTableTask(consumer);
    }
    else if (tableName == CFG_TAM_SESSION_TABLE_NAME)
    {
        doTamSessionTableTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("TAM ERROR: Invalid table %s", tableName.c_str());
    }
}

bool TamOrch::deleteTamCollectorEntry(const string& collectorName)
{
    auto collectorIter = m_collectorTables.find(collectorName);
    if (collectorIter == m_collectorTables.end()) {
        SWSS_LOG_ERROR("Collector:%s does not exist", collectorName.c_str());
        return false;
    }

    // Mark the collector as unresolved and call the notifiers so that the entry
    // can be freed
    auto collectorEntry = collectorIter->second;
    collectorEntry->resolved = false;
    tamCollectorDecRefCountByName(collectorName);
    handleCollectorChange(collectorName);

    // By now the refcount should be 0
    if (collectorEntry->refCount != 0) {
        SWSS_LOG_ERROR("Refcount for Collector %s is still %d", collectorName.c_str(), collectorEntry->refCount);
        return false;
    }

    m_collectorTables.erase(collectorName);
    return true;
}

string TamOrch::getTamSessionType(const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    auto iter = std::find_if(data.begin(), data.end(), [&](const auto& pair) {
        return pair.first == "type";
    });

    if (iter != data.end()) {
        // Type is found
        return iter->second;
    }

    return "";
}

DropMonitorSession::DropMonitorSession()
{
    reportType = "";
    eventType = SAI_TAM_EVENT_TYPE_PACKET_DROP;
    dropStage = DROP_MONITOR_STAGE_ALL;

    tamReportObjId = SAI_NULL_OBJECT_ID;
    tamEventActionObjId = SAI_NULL_OBJECT_ID;
    ingressEventObjId = SAI_NULL_OBJECT_ID;
    egressEventObjId = SAI_NULL_OBJECT_ID;
    tmEventObjId = SAI_NULL_OBJECT_ID;
    tamObjId = SAI_NULL_OBJECT_ID;
}

/**
 * @brief Validate a drop monitor session configuration before creation
 *
 * Performs validation checks on a drop monitor session to ensure it can be created:
 * - Report type must be "ipfix" (currently the only supported type)
 * - Event type must be SAI_TAM_EVENT_TYPE_PACKET_DROP
 * - At least one collector must be resolved and have a valid SAI object ID
 *
 * @param dropSession The drop monitor session configuration to validate
 * @return true if the session configuration is valid and can be created, false otherwise
 */
bool TamOrch::validateDropMonitorSession(const DropMonitorSession& dropSession)
{
    bool validReportType = false, validEventType = false, validCollector = false;

    // Validate report type (currently only IPFIX supported)
    validReportType = (dropSession.reportType == "ipfix");

    // Validate event type (must be packet drop)
    validEventType = (dropSession.eventType == SAI_TAM_EVENT_TYPE_PACKET_DROP);

    // Check if at least one collector is resolved and has a valid SAI object
    // Session can be created with partial collector list; remaining collectors
    // will require session recreation when they become available
    for (const auto& collectorName : dropSession.collectorNames)
    {
        auto collectorIter = m_collectorTables.find(collectorName);
        if (collectorIter != m_collectorTables.end()) {
            // Collector must be resolved (nexthop/neighbor known) and have SAI object created
            validCollector = (validCollector || (
               collectorIter->second->resolved && collectorIter->second->collectorObjId != SAI_NULL_OBJECT_ID));

            // If we have at least one collector resolved.
            if (validCollector)
            {
                break;
            }
        }
    }

    SWSS_LOG_DEBUG("validReportType:%d validEventType:%d validCollector:%d",
                     validReportType, validEventType, validCollector);
    return validReportType && validEventType && validCollector;
}

TamSessionEntry::TamSessionEntry(const string& sessionName, const TamSessionType sessionType):
    sessionName(sessionName),
    sessionType(sessionType),
    dropSession()
{
    active = false;
}

bool TamOrch::validateTamSession(std::shared_ptr<TamSessionEntry> sessionEntry)
{
    if (sessionEntry->sessionType == tamSessionTypeDropMonitor) {
        return validateDropMonitorSession(sessionEntry->dropSession);
    } else {
        SWSS_LOG_ERROR("Unknown sessionType:%d", sessionEntry->sessionType);
        return false;
    }
}

/**
 * @brief Delete a TAM report SAI object
 *
 * Removes a previously created TAM report object from the SAI layer.
 * TAM reports define the format and content of telemetry data sent to collectors.
 *
 * @param reportObjId SAI object ID of the TAM report to delete (passed by reference)
 * @return true if deletion succeeded, false otherwise
 */
bool TamOrch::tamReportDelete(sai_object_id_t &reportObjId)
{
    SWSS_LOG_DEBUG("reportObjId:%" PRIx64, reportObjId);

    auto status = sai_tam_api->remove_tam_report(reportObjId);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Error deleting TAM REPORT object");
        if (handleSaiRemoveStatus(SAI_API_MIRROR, status) != task_success) {
            return false;
        }
    }
    SWSS_LOG_DEBUG("Delted TAM Report %" PRIx64, reportObjId);
    return true;
}

/**
 * @brief Create a TAM report SAI object
 *
 * Creates a TAM report object that defines the format and structure of telemetry data.
 * Currently supports IPFIX report type for drop monitoring.
 * The report includes the enterprise ID for vendor-specific information.
 *
 * @param reportType Type of report to create (e.g., SAI_TAM_REPORT_TYPE_IPFIX)
 * @param reportObjId Output parameter that receives the created SAI object ID
 * @return true if creation succeeded, false otherwise
 */
bool TamOrch::tamReportCreate(sai_tam_report_type_t reportType,
                               sai_object_id_t &reportObjId)
{
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;

    SWSS_LOG_DEBUG("TAM: Creating TAM REPORT object");

    // Set report type (e.g., IPFIX for drop monitoring)
    attr.id = SAI_TAM_REPORT_ATTR_TYPE;
    attr.value.s32 = reportType;
    attrs.push_back(attr);

    // Set enterprise ID for telemetry data
    attr.id = SAI_TAM_REPORT_ATTR_ENTERPRISE_NUMBER;
    attr.value.u32 = getEnterpriseId();
    attrs.push_back(attr);

    // Create the SAI report object
    status = sai_tam_api->create_tam_report(&reportObjId, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error creating TAM REPORT");
        auto taskStatus = handleSaiCreateStatus(SAI_API_TAM, status); 
        if (taskStatus != task_success)
        {
            return parseHandleSaiStatusFailure(taskStatus);
        }
    }
    SWSS_LOG_NOTICE("Created TAM REPORT:%" PRIx64, reportObjId);
    return true;
}

/**
 * @brief Delete a TAM event action SAI object
 *
 * Removes a previously created TAM event action object from the SAI layer.
 * Event actions define what happens when a TAM event is triggered (e.g., generate a report).
 *
 * @param eventActionObjId SAI object ID of the TAM event action to delete (passed by reference)
 * @return true if deletion succeeded, false otherwise
 */
bool TamOrch::tamEventActionDelete(sai_object_id_t &eventActionObjId)
{
    SWSS_LOG_DEBUG("eventActionObjId:%" PRIx64, eventActionObjId);

    auto status = sai_tam_api->remove_tam_event_action(eventActionObjId);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Error deleting TAM EVENT ACTION object");
        if (handleSaiRemoveStatus(SAI_API_MIRROR, status) != task_success) {
            return false;
        }
    }

    SWSS_LOG_NOTICE("Deleted TAM Event Action %" PRIx64, eventActionObjId);
    return true;
}

/**
 * @brief Create a TAM event action SAI object
 *
 * Creates a TAM event action that links a TAM event to a report.
 * When the event is triggered (e.g., packet drop detected), the associated
 * report is generated and sent to collectors.
 *
 * @param reportObjId SAI object ID of the TAM report to associate with this action
 * @param eventActionObjId Output parameter that receives the created SAI object ID
 * @return true if creation succeeded, false otherwise
 */
bool TamOrch::tamEventActionCreate(sai_object_id_t reportObjId,
                                    sai_object_id_t &eventActionObjId)
{
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;

    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("TAM: Creating TAM EVENT ACTION object");

    // Link the action to the report - when event triggers, generate this report
    attr.id = SAI_TAM_EVENT_ACTION_ATTR_REPORT_TYPE;
    attr.value.oid = reportObjId;
    attrs.push_back(attr);

    // Create the SAI event action object
    status = sai_tam_api->create_tam_event_action(&eventActionObjId, gSwitchId,
                                                  (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error creating TAM EVENT ACTION");
        auto taskStatus = handleSaiCreateStatus(SAI_API_TAM, status);
        if (taskStatus != task_success) {
            return parseHandleSaiStatusFailure(taskStatus);
        }
    }

    SWSS_LOG_NOTICE("Created TAM EVENT ACTION %" PRIx64, eventActionObjId);
    return true;
}

/**
 * @brief Delete a TAM drop event SAI object
 *
 * Removes a previously created TAM event object that monitors packet drops.
 * This is called when tearing down a drop monitor session.
 *
 * @param eventObjId SAI object ID of the TAM event to delete (passed by reference)
 * @return true if deletion succeeded, false otherwise
 */
bool TamOrch::tamDropEventDelete(std::shared_ptr<TamSessionEntry> sessionEntry, sai_object_id_t &eventObjId)
{
    SWSS_LOG_DEBUG("eventObjId:%" PRIx64, eventObjId);

    auto status = sai_tam_api->remove_tam_event(eventObjId);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Error deleting TAM EVENT object");
        if (handleSaiRemoveStatus(SAI_API_MIRROR, status) != task_success) {
            return false;
        }
    }

    SWSS_LOG_NOTICE("Deleted TAM Event %" PRIx64, eventObjId);

    // Decrement the refcount to the collector
    tamDropMonitorDecRefCountForCollector(sessionEntry);

    return true;
}

/**
 * @brief Create a TAM event for monitoring packet drops at a specific pipeline stage
 *
 * Creates a TAM event object that monitors packet drops at ingress, egress, or MMU (TM) stage.
 * The event is configured based on the drop stage and drop type (stateful/stateless).
 * When drops occur, the event triggers the associated action to generate reports.
 *
 * Supported drop stages:
 * - DROP_MONITOR_STAGE_INGRESS: Monitors ingress pipeline drops
 * - DROP_MONITOR_STAGE_EGRESS: Monitors egress pipeline drops
 * - DROP_MONITOR_STAGE_TM: Monitors MMU/traffic manager drops
 *
 * @param sessionEntry Shared pointer to the TAM session containing drop configuration
 * @param dropStage Pipeline stage to monitor (ingress/egress/MMU)
 * @param eventObjId Output parameter that receives the created SAI object ID
 * @return true if creation succeeded, false otherwise
 */
bool TamOrch::tamDropEventCreate(std::shared_ptr<TamSessionEntry> sessionEntry,
                                  uint8_t dropStage, sai_object_id_t &eventObjId)
{
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;

    SWSS_LOG_ENTER();

    // Set event type (packet drop monitoring)
    attr.id = SAI_TAM_EVENT_ATTR_TYPE;
    attr.value.oid = sessionEntry->dropSession.eventType;
    attrs.push_back(attr);

    // Configure drop types based on pipeline stage
    std::vector<int32_t> dropList;
    if (dropStage == DROP_MONITOR_STAGE_INGRESS) {
        attr.id = SAI_TAM_EVENT_ATTR_PACKET_DROP_TYPE_INGRESS;
        dropList.push_back(SAI_PACKET_DROP_TYPE_INGRESS_ALL);  // Monitor all ingress drops
    }
    else if (dropStage == DROP_MONITOR_STAGE_EGRESS)
    {
        attr.id = SAI_TAM_EVENT_ATTR_PACKET_DROP_TYPE_EGRESS;
        dropList.push_back(SAI_PACKET_DROP_TYPE_EGRESS_ALL);   // Monitor all egress drops
    }
    else if (dropStage == DROP_MONITOR_STAGE_TM)
    {
        attr.id = SAI_TAM_EVENT_ATTR_PACKET_DROP_TYPE_MMU;
        dropList.push_back(SAI_PACKET_DROP_TYPE_MMU_ALL);      // Monitor all MMU/TM drops
    }

    attr.value.s32list.count = (uint32_t) dropList.size();
    attr.value.s32list.list = dropList.data();
    attrs.push_back(attr);

    // Validate that collectors are configured.  Currently we support only 1
    if (sessionEntry->dropSession.collectorNames.size() != 1) {
        SWSS_LOG_ERROR("Invalid number of collectors :%ld", sessionEntry->dropSession.collectorNames.size());
        return false;
    }

    std::vector<sai_object_id_t> collectorList = {};
    std::vector<string> collectorNames = {};

    // Build list of resolved collector SAI object IDs
    for (const auto& collectorName : sessionEntry->dropSession.collectorNames)
    {
        auto collectorIter = m_collectorTables.find(collectorName);
        if (collectorIter == m_collectorTables.end()) {
            SWSS_LOG_DEBUG("Collector %s is not available yet", collectorName.c_str());
            continue;
        }
        collectorList.push_back(collectorIter->second->collectorObjId);
        collectorNames.push_back(collectorName);
    }

    // Cannot create event without at least one resolved collector
    if (collectorList.size() == 0) {
        SWSS_LOG_DEBUG("No collector available yet");
        return false;
    }

    // Set standard collector list attribute
    attr.id = SAI_TAM_EVENT_ATTR_COLLECTOR_LIST;
    attr.value.objlist.count = (uint32_t) collectorList.size();
    attr.value.objlist.list = collectorList.data();
    attrs.push_back(attr);

    // Set extensions collector list (vendor-specific attribute)
    attr.id = SAI_TAM_EVENT_ATTR_EXTENSIONS_COLLECTOR_LIST;
    attr.value.objlist.count = (uint32_t) collectorList.size();
    attr.value.objlist.list = collectorList.data();
    attrs.push_back(attr);

    // Set unique event ID per drop stage
    attr.id = SAI_TAM_EVENT_ATTR_EVENT_ID;
    attr.value.s32 = TAM_DROP_EVENT_ID_BASE + dropStage;
    attrs.push_back(attr);

    // Set device ID for multi-device scenarios
    attr.id = SAI_TAM_EVENT_ATTR_DEVICE_ID;
    attr.value.u32 = getDeviceId();
    attrs.push_back(attr);

    // Link event to the action (which generates the report)
    std::vector<sai_object_id_t> eventActionList =  { sessionEntry->dropSession.tamEventActionObjId };
    attr.id = SAI_TAM_EVENT_ATTR_ACTION_LIST;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = eventActionList.data();
    attrs.push_back(attr);

    // Create the SAI event object
    status = sai_tam_api->create_tam_event(&eventObjId, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error creating TAM EVENT id");
        auto taskStatus = handleSaiCreateStatus(SAI_API_TAM, status);
        if (taskStatus != task_success) {
            return parseHandleSaiStatusFailure(taskStatus);
        }
    }

    // Increment refcount to the collector
    tamDropMonitorIncRefCountForCollector(sessionEntry);

    SWSS_LOG_NOTICE("Created TAM EVENT %" PRIx64 " for stage:%d", eventObjId, dropStage);
    return true;
}

bool TamOrch::unbindPortFromTam(sai_object_id_t& portId, sai_object_id_t& tamObjId)
{
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;

    // TODO decref count for the tam object
	attr.id = SAI_PORT_ATTR_TAM_OBJECT;
	attr.value.objlist.count = 0;

	status = sai_port_api->set_port_attribute(portId, &attr);
    if (status != SAI_STATUS_SUCCESS) {
	    SWSS_LOG_ERROR("Failed set_port_attribute port:%" PRIx64, portId);
		if (handleSaiSetStatus(SAI_API_PORT, status) != task_success) {
		    return false;
		}
	}

	SWSS_LOG_NOTICE("Unbinding port:%" PRIx64 " from tam:%" PRIx64, portId, tamObjId);

	return true;
}

bool TamOrch::bindPortToTam(sai_object_id_t& portId, sai_object_id_t& tamObjId)
{
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;

	SWSS_LOG_DEBUG("Binding port:%" PRIx64 " to tam:%" PRIx64, portId, tamObjId);

    // TODO incref count for the tam object
	std::vector<sai_object_id_t> tamObjList{ tamObjId };
	attr.id = SAI_PORT_ATTR_TAM_OBJECT;
	attr.value.objlist.count = (uint32_t)tamObjList.size();
	attr.value.objlist.list = tamObjList.data();

	status = sai_port_api->set_port_attribute(portId, &attr);
    if (status != SAI_STATUS_SUCCESS) {
	    SWSS_LOG_ERROR("Binding port:%" PRIx64 " to tamObj:%" PRIx64 " failed",
		                portId, tamObjId);
		if (handleSaiSetStatus(SAI_API_PORT, status) != task_success) {
		    return false;
		}
	}
	return true;
}

void TamOrch::handleTamCreate(std::shared_ptr<TamSessionEntry> sessionEntry)
{
    SWSS_LOG_ENTER();

    // If this is a mirror session, we may need to program MTP port with this TAM
    if (sessionEntry->sessionType == tamSessionTypeDropMonitor)
    {
	    // Get the collector name and lookup to see if it has valid port id
	    assert(sessionEntry->dropSession.collectorNames.size() != 0);
	    for (const auto& collectorName : sessionEntry->dropSession.collectorNames)
		{
            auto collectorIter = m_collectorTables.find(collectorName);
     	    assert(collectorIter != m_collectorTables.end());

            // We need valid port id
	    	auto collectorEntry = collectorIter->second;
		    auto portId = collectorEntry->neighborInfo.portId;
		    assert(portId != SAI_NULL_OBJECT_ID);

		    // Bind that port to TAM
		    bindPortToTam(portId, sessionEntry->dropSession.tamObjId);
		}

        // Activate flow group ACL rules if configured
        if (!sessionEntry->dropSession.flowGroupName.empty())
        {
            SWSS_LOG_INFO("Activating flow group rules for DropMonitorSession %s with flow group %s",
                         sessionEntry->sessionName.c_str(), sessionEntry->dropSession.flowGroupName.c_str());
            activateFlowGroupRulesForDropMonitor(sessionEntry->dropSession.flowGroupName);
        } else {
            // Since Flow group is not configured, this is flow-unaware MOD. Lets tie the TAM
            // object to the switch
            bindTamToSwitch(sessionEntry->dropSession.tamObjId);
        }
    }
}

bool TamOrch::tamDelete(sai_object_id_t &tamObjId)
{
    SWSS_LOG_DEBUG("tamObjId:%" PRIx64, tamObjId);

    auto status = sai_tam_api->remove_tam(tamObjId);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Error deleting TAM EVENT object");
        if (handleSaiRemoveStatus(SAI_API_MIRROR, status) != task_success) {
            return false;
        }
    }
    SWSS_LOG_NOTICE("Deleted TAM %" PRIx64, tamObjId);
    return true;
}

bool TamOrch::tamCreate(std::shared_ptr<TamSessionEntry> sessionEntry,
                         sai_object_id_t &tamObjId)
{
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;

    std::vector<sai_object_id_t> eventList =  {};

    // Add all available events
    auto events = { sessionEntry->dropSession.ingressEventObjId,
                    sessionEntry->dropSession.egressEventObjId,
                    sessionEntry->dropSession.tmEventObjId };

    // Populate valid events
    for (auto event : events) {
        if (event != SAI_NULL_OBJECT_ID) {
            eventList.push_back(event);
        }
    }

    attr.id = SAI_TAM_ATTR_EVENT_OBJECTS_LIST;
    attr.value.objlist.count = (uint32_t) eventList.size();
    attr.value.objlist.list = eventList.data();
    attrs.push_back(attr);

    std::vector<sai_tam_bind_point_type_t> bindTypeList = { SAI_TAM_BIND_POINT_TYPE_PORT, 
                                                            SAI_TAM_BIND_POINT_TYPE_LAG, 
                                                            SAI_TAM_BIND_POINT_TYPE_SWITCH };
    attr.id = SAI_TAM_ATTR_TAM_BIND_POINT_TYPE_LIST;
    attr.value.s32list.count = (uint32_t) bindTypeList.size();
    attr.value.s32list.list = (int32_t*) bindTypeList.data();
    attrs.push_back(attr);

    status = sai_tam_api->create_tam(&tamObjId, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("TAM ERROR: Error creating TAM Object");
        auto taskStatus = handleSaiCreateStatus(SAI_API_TAM, status);
        if (taskStatus != task_success) {
            return parseHandleSaiStatusFailure(taskStatus);
        }
    }

    SWSS_LOG_NOTICE("Created TAM %" PRIx64 , tamObjId);
    return true;
}

/**
 * @brief Delete a drop monitor session and all associated SAI objects
 *
 * Tears down a complete drop monitor session by:
 * 1. Deactivating any associated flow group ACL rules
 * 2. Unbinding ports from the TAM object
 * 3. Deleting the TAM object
 * 4. Deleting TAM events (ingress/egress/MMU)
 * 5. Deleting the event action
 * 6. Deleting the report
 * 7. Decrementing collector reference counts
 *
 * This is the reverse of tamDropMonitorSessionCreate() and ensures proper cleanup
 * of all SAI resources.
 *
 * @param sessionEntry Shared pointer to the TAM session to delete
 * @return true if deletion succeeded, false otherwise
 */
bool TamOrch::tamDropMonitorSessionDelete(std::shared_ptr<TamSessionEntry> sessionEntry)
{
    // Deactivate any flow group rules associated with this DropMonitorSession
    if (!sessionEntry->dropSession.flowGroupName.empty())
    {
        SWSS_LOG_INFO("Deactivating flow group rules for DropMonitorSession %s with flow group %s",
                     sessionEntry->sessionName.c_str(), sessionEntry->dropSession.flowGroupName.c_str());
        deactivateFlowGroupRulesForDropMonitor(sessionEntry->dropSession.flowGroupName);
    } else {
        unbindTamFromSwitch();
    }

    // Get the collector name and lookup to see if it has valid port id
    for (const auto& collectorName : sessionEntry->dropSession.collectorNames)
	{
        auto collectorIter = m_collectorTables.find(collectorName);
        assert(collectorIter != m_collectorTables.end());

        // We need valid port id
	    auto collectorEntry = collectorIter->second;
		auto portId = collectorEntry->neighborInfo.portId;
		assert(portId != SAI_NULL_OBJECT_ID);

		// Unbind port from the TAM object
		unbindPortFromTam(portId, sessionEntry->dropSession.tamObjId);
    }

    // Delete TAM object
    assert(sessionEntry->dropSession.tamObjId != SAI_NULL_OBJECT_ID);
    tamDelete(sessionEntry->dropSession.tamObjId);
    sessionEntry->dropSession.tamObjId = SAI_NULL_OBJECT_ID;

    // Delete all valid events
    if (sessionEntry->dropSession.ingressEventObjId != SAI_NULL_OBJECT_ID) {
        tamDropEventDelete(sessionEntry, sessionEntry->dropSession.ingressEventObjId);
        sessionEntry->dropSession.ingressEventObjId = SAI_NULL_OBJECT_ID;
    }

    if (sessionEntry->dropSession.egressEventObjId != SAI_NULL_OBJECT_ID) {
        tamDropEventDelete(sessionEntry, sessionEntry->dropSession.egressEventObjId);
        sessionEntry->dropSession.egressEventObjId = SAI_NULL_OBJECT_ID;
    }

    if (sessionEntry->dropSession.tmEventObjId != SAI_NULL_OBJECT_ID) {
        tamDropEventDelete(sessionEntry, sessionEntry->dropSession.tmEventObjId);
        sessionEntry->dropSession.tmEventObjId = SAI_NULL_OBJECT_ID;
    }

    // Event action must be valid
    assert(sessionEntry->dropSession.tamEventActionObjId);
    tamEventActionDelete(sessionEntry->dropSession.tamEventActionObjId);
    sessionEntry->dropSession.tamEventActionObjId = SAI_NULL_OBJECT_ID;

    // Report must be valid
    assert(sessionEntry->dropSession.tamReportObjId != SAI_NULL_OBJECT_ID);
    tamReportDelete(sessionEntry->dropSession.tamReportObjId);
    sessionEntry->dropSession.tamReportObjId = SAI_NULL_OBJECT_ID;

    sessionEntry->active = false;

    // Update STATE_DB with inactive status
    setDropMonitorSessionState(sessionEntry);

    SWSS_LOG_NOTICE("Deleted Drop Monitor session");

    return true;
}

bool TamOrch::bindTamToSwitch(const sai_object_id_t tamObjId)
{
    SWSS_LOG_NOTICE("Binding tamObj 0x%" PRIx64 " to switch", tamObjId);

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;

    std::vector<sai_object_id_t> tamObjList = {tamObjId};
    attr.id = SAI_SWITCH_ATTR_TAM_OBJECT_ID;
	attr.value.objlist.count = (uint32_t)tamObjList.size();
	attr.value.objlist.list = tamObjList.data();
    attrs.push_back(attr);

    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to bind TAM to switch");
        return false;
    }
    
    return true;
}

bool TamOrch::unbindTamFromSwitch()
{
    SWSS_LOG_NOTICE("Unbinding tamObj from switch");

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    sai_status_t status;

    attr.id = SAI_SWITCH_ATTR_TAM_OBJECT_ID;
	attr.value.objlist.count = 0;
    attrs.push_back(attr);

    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to unbind TAM from switch");
        return false;
    }
    
    return true;
}

/**
 * @brief Create a complete drop monitor session with all required SAI objects
 *
 * Creates a full drop monitoring session by building the TAM object hierarchy:
 * 1. TAM Report - defines the telemetry report format (IPFIX)
 * 2. TAM Event Action - links events to the report
 * 3. TAM Events - one for each enabled drop stage (ingress/egress/MMU)
 * 4. TAM Object - aggregates collectors and events
 * 5. Binds ports to the TAM object for telemetry export
 * 6. Activates flow group ACL rules if configured
 * 7. Increments collector reference counts
 *
 * The session is marked as active upon successful creation.
 *
 * @param sessionEntry Shared pointer to the TAM session entry to create
 * @return true if creation succeeded, false otherwise
 */
bool TamOrch::tamDropMonitorSessionCreate(std::shared_ptr<TamSessionEntry> sessionEntry)
{
    sai_object_id_t reportObj = SAI_NULL_OBJECT_ID, eventActionObj = SAI_NULL_OBJECT_ID;
    sai_object_id_t ingressEventObj = SAI_NULL_OBJECT_ID, egressEventObj = SAI_NULL_OBJECT_ID;
    sai_object_id_t tmEventObj = SAI_NULL_OBJECT_ID, tamObj = SAI_NULL_OBJECT_ID;

    // Create TAM report (defines telemetry format)
    assert(sessionEntry->dropSession.reportType == SESSION_REPORT_TYPE_IPFIX);
    if (!tamReportCreate(SAI_TAM_REPORT_TYPE_IPFIX, reportObj))
    {
        SWSS_LOG_ERROR("Failed to create TAM REPORT for drop-monitor");
        return false;
    }
    sessionEntry->dropSession.tamReportObjId = reportObj;

    // Create event action (links events to report)
    if (!tamEventActionCreate(reportObj, eventActionObj)) {
        SWSS_LOG_ERROR("Failed to create event action for drop-monitor");
        goto cleanupAndReturn;
    }
    sessionEntry->dropSession.tamEventActionObjId = eventActionObj;

    // Create drop events for each enabled pipeline stage
    if (sessionEntry->dropSession.dropStage & DROP_MONITOR_STAGE_INGRESS) {
        if (!tamDropEventCreate(sessionEntry, DROP_MONITOR_STAGE_INGRESS, ingressEventObj)) {
            SWSS_LOG_ERROR("Failed to create ingress_drop event for drop-monitor");
            goto cleanupAndReturn;
        }
        sessionEntry->dropSession.ingressEventObjId = ingressEventObj;
    }

    if (sessionEntry->dropSession.dropStage & DROP_MONITOR_STAGE_EGRESS) {
        if (!tamDropEventCreate(sessionEntry, DROP_MONITOR_STAGE_EGRESS, egressEventObj)) {
            SWSS_LOG_ERROR("Failed to create egress_drop event for drop-monitor");
            goto cleanupAndReturn;
        }
        sessionEntry->dropSession.egressEventObjId = egressEventObj;
    }

    if (sessionEntry->dropSession.dropStage & DROP_MONITOR_STAGE_TM) {
        if (!tamDropEventCreate(sessionEntry, DROP_MONITOR_STAGE_TM, tmEventObj)) {
            SWSS_LOG_ERROR("Failed to create mmu_drop event for drop-monitor");
            goto cleanupAndReturn;
        }
        sessionEntry->dropSession.tmEventObjId = tmEventObj;
    }

    // Create main TAM object (aggregates events and collectors)
    if (!tamCreate(sessionEntry, tamObj)) {
       SWSS_LOG_ERROR("Failed to create mmu_drop event for drop-monitor");
       goto cleanupAndReturn;
    }
    sessionEntry->dropSession.tamObjId = tamObj;
    sessionEntry->active = true;

    // Bind ports to TAM and increment collector refcounts
    handleTamCreate(sessionEntry);

    // Update STATE_DB with session status
    setDropMonitorSessionState(sessionEntry);

    SWSS_LOG_NOTICE("Created Drop Monitor session");

    return true;

cleanupAndReturn:
    SWSS_LOG_NOTICE("Drop Monitor session creation failed");

    // Delete TAM Report if valid
    if (sessionEntry->dropSession.tamReportObjId != SAI_NULL_OBJECT_ID) 
    {
        tamReportDelete(sessionEntry->dropSession.tamReportObjId);
        sessionEntry->dropSession.tamReportObjId = SAI_NULL_OBJECT_ID;
    }

    // Delete TAM Event Action if valid
    if (sessionEntry->dropSession.tamEventActionObjId != SAI_NULL_OBJECT_ID)
    {
        tamEventActionDelete(sessionEntry->dropSession.tamEventActionObjId);
        sessionEntry->dropSession.tamEventActionObjId = SAI_NULL_OBJECT_ID;
    }

    // Delete TAM IP Event if valid
    if (sessionEntry->dropSession.ingressEventObjId != SAI_NULL_OBJECT_ID) {
        tamDropEventDelete(sessionEntry, sessionEntry->dropSession.ingressEventObjId);
        sessionEntry->dropSession.ingressEventObjId = SAI_NULL_OBJECT_ID;
    }

    // Delete TAM EP Event if valid
    if (sessionEntry->dropSession.egressEventObjId != SAI_NULL_OBJECT_ID) {
        tamDropEventDelete(sessionEntry, sessionEntry->dropSession.egressEventObjId);
        sessionEntry->dropSession.egressEventObjId = SAI_NULL_OBJECT_ID;
    }

    // Delete TAM TM Event if valid
    if (sessionEntry->dropSession.tmEventObjId != SAI_NULL_OBJECT_ID) {
        tamDropEventDelete(sessionEntry, sessionEntry->dropSession.tmEventObjId);
        sessionEntry->dropSession.tmEventObjId = SAI_NULL_OBJECT_ID;
    }

    return false;
}

void TamOrch::handleCollectorChange(const string& collectorName)
{
	SWSS_LOG_NOTICE("collectorName:%s", collectorName.c_str());

    // Check if any mirror session used this collector
    for (auto &iter : m_sessionTables) {
        auto collectorUsed = false;
        auto &sessionEntry = iter.second;
        if (sessionEntry->sessionType == tamSessionTypeDropMonitor) {
            for (const auto& name : sessionEntry->dropSession.collectorNames) {
                if (name == collectorName) {
				    SWSS_LOG_DEBUG("DropMonitor session:%s uses collector:%s",
					                 sessionEntry->sessionName.c_str(),
									 collectorName.c_str());
                    collectorUsed = true;
                    break;
                }
            }

            if (!collectorUsed) {
                // Drop monitor does not care about this collector
                continue;
            }

            // Drop monitor session is using this collector.
            bool isValidSession = validateTamSession(sessionEntry);
            if (!sessionEntry->active && isValidSession) {
                // This session was previously not active, program it now
                tamDropMonitorSessionCreate(sessionEntry);
            } else if (sessionEntry->active) {
                // Session is active, something changed.  Remove the old instance and recreate it
                tamDropMonitorSessionDelete(sessionEntry);
            }
        }
    }
}

/**
 * @brief Create or update a drop monitor session from configuration data
 *
 * Parses configuration from CONFIG_DB and creates a drop monitor session.
 * Handles updates by deleting the existing session first (delete + add pattern).
 *
 * Configuration fields parsed:
 * - SESSION_TYPE: Must be "drop-monitor"
 * - SESSION_REPORT_TYPE: Report format (e.g., "ipfix")
 * - SESSION_FLOW_GROUP: Optional flow group name for ACL-based filtering
 * - SESSION_COLLECTOR: Comma-separated list of collector names
 * - SESSION_DROP_TYPE: Drop type (stateful/stateless)
 * - SESSION_DROP_STAGE: Pipeline stages to monitor (ingress/egress/tm)
 *
 * The session is validated before creation. If validation fails, the session
 * is stored but not activated until all dependencies (collectors) are resolved.
 *
 * @param dropSessionName Name of the drop monitor session
 * @param data Vector of field-value tuples containing session configuration
 * @return true if session was created/updated successfully, false otherwise
 */
bool TamOrch::createDropMonitorSession(const string& dropSessionName, const vector<FieldValueTuple>& data)
{
    // Handle updates as delete + add pattern
    auto sessionIter = m_sessionTables.find(dropSessionName);
    if (sessionIter != m_sessionTables.end()) {
        // Session exists.  Make sure it is of correct type
        auto sessionEntry = sessionIter->second;
        if (sessionEntry->sessionType == tamSessionTypeDropMonitor) {
            deleteDropMonitorSession(sessionEntry);
        } else {
            SWSS_LOG_ERROR("Invalid session type name:%s type:%d", 
                sessionEntry->sessionName.c_str(), sessionEntry->sessionType);
            return false;
        }
    }

    // Create new session entry
    auto dropEntry = std::make_shared<TamSessionEntry>(dropSessionName, tamSessionTypeDropMonitor);

    // Parse configuration from CONFIG_DB
    for (auto i : data)
    {
        try {
            SWSS_LOG_DEBUG("Session name:%s Key:%s Value:%s", dropSessionName.c_str(),
                    fvField(i).c_str(), fvValue(i).c_str());
            if (fvField(i) == SESSION_TYPE)
            {
                // Validate session type
                assert(fvValue(i) == SESSION_TYPE_DROP_MONITOR);
            }
            else if (fvField(i) == SESSION_REPORT_TYPE)
            {
                // Report format (e.g., "ipfix")
                dropEntry->dropSession.reportType = fvValue(i);
            }
            else if (fvField(i) == SESSION_FLOW_GROUP)
            {
                // Optional ACL flow group for filtering
                dropEntry->dropSession.flowGroupName = fvValue(i);
            }
            else if (fvField(i) == SESSION_COLLECTOR)
            {
                // Parse comma-separated collector list
                auto collectorList = tokenize(fvValue(i), ',');

                for (const auto &collectorName : collectorList)
                {
                    dropEntry->dropSession.collectorNames.push_back(collectorName);
                }
            }
        }
        catch (const exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse dropSession %s attribute %s error: %s.",
                           dropSessionName.c_str(), fvField(i).c_str(), e.what());
            return false;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to parse dropSession %s attribute %s Unknown error has occurred.",
                            dropSessionName.c_str(), fvField(i).c_str());
            return false;
        }
    }

    // Store session in table
    m_sessionTables[ dropSessionName ] = dropEntry;

    // Initialize STATE_DB with inactive status (will be updated to active if creation succeeds)
    setDropMonitorSessionState(dropEntry);

    // Validate and create session if all dependencies are resolved
    if (!dropEntry->active && validateTamSession(dropEntry))
    {
        tamDropMonitorSessionCreate(dropEntry);
    }
    else
    {
        return false;
    }
    return true;
}

/**
 * @brief Delete a drop monitor session by name
 *
 * High-level function to remove a drop monitor session from the system.
 * If the session is active (SAI objects created), it calls tamDropMonitorSessionDelete()
 * to tear down all SAI resources before removing the session from the session table.
 *
 * This function is idempotent - calling it on a non-existent session is a no-op.
 *
 * @param sessionName Name of the drop monitor session to delete
 * @return true if session was deleted, false if session didn't exist
 */
bool TamOrch::deleteDropMonitorSession(std::shared_ptr<TamSessionEntry> sessionEntry)
{
    SWSS_LOG_ENTER();

    if (sessionEntry->active)
    {
        tamDropMonitorSessionDelete(sessionEntry);
    }

    // Remove session status from STATE_DB
    removeDropMonitorSessionState(sessionEntry->sessionName);

    m_sessionTables.erase(sessionEntry->sessionName);

    return true;
}

/**
 * @brief Write drop monitor session status and configuration to STATE_DB
 *
 * Populates STATE_DB with the operational state and configuration of a drop monitor
 * session, enabling CLI commands like "show tam mod sessions" to display session information.
 *
 * @param sessionEntry Shared pointer to the session entry containing configuration
 */
void TamOrch::setDropMonitorSessionState(std::shared_ptr<TamSessionEntry> sessionEntry)
{
    SWSS_LOG_ENTER();

    const string& sessionName = sessionEntry->sessionName;
    vector<FieldValueTuple> fvVector;

    // Operational status
    string status = sessionEntry->active ? "active" : "inactive";
    fvVector.emplace_back("status", status);

    // Report type
    fvVector.emplace_back("report_type", sessionEntry->dropSession.reportType);

    // Event type
    string eventType = (sessionEntry->dropSession.eventType == SAI_TAM_EVENT_TYPE_PACKET_DROP)
                       ? "packet-drop-stateless" : "packet-drop-stateful";
    fvVector.emplace_back("event_type", eventType);

    // Drop stages as comma-separated list
    string dropStages;
    uint8_t stage = sessionEntry->dropSession.dropStage;
    if (stage & DROP_MONITOR_STAGE_INGRESS) {
        dropStages += "ingress";
    }
    if (stage & DROP_MONITOR_STAGE_EGRESS) {
        if (!dropStages.empty()) dropStages += ",";
        dropStages += "egress";
    }
    if (stage & DROP_MONITOR_STAGE_TM) {
        if (!dropStages.empty()) dropStages += ",";
        dropStages += "tm";
    }
    fvVector.emplace_back("drop_stages", dropStages);

    // Flow group name (empty string if not configured)
    fvVector.emplace_back("flow_group", sessionEntry->dropSession.flowGroupName);

    // Collectors as comma-separated list
    string collectors;
    for (const auto& collectorName : sessionEntry->dropSession.collectorNames) {
        if (!collectors.empty()) collectors += ",";
        collectors += collectorName;
    }
    fvVector.emplace_back("collectors", collectors);

    // If inactive, add status detail explaining why
    if (!sessionEntry->active) {
        string statusDetail;
        // Check if any collector is unresolved (specific known reason)
        bool hasUnresolvedCollector = false;
        for (const auto& collectorName : sessionEntry->dropSession.collectorNames) {
            auto collectorIter = m_collectorTables.find(collectorName);
            if (collectorIter == m_collectorTables.end() || !collectorIter->second->resolved) {
                hasUnresolvedCollector = true;
                break;
            }
        }
        if (hasUnresolvedCollector) {
            // Specific known reason: collector not reachable
            statusDetail = "Collector not reachable";
        } else {
            // Non-specific/unknown reason
            statusDetail = "Configuration error";
        }
        fvVector.emplace_back("status_detail", statusDetail);
    }

    // Delete existing entry first to ensure stale fields (like status_detail) are removed
    // when transitioning from inactive to active
    m_stateDbDropMonitorSessionTable.del(sessionName);
    m_stateDbDropMonitorSessionTable.set(sessionName, fvVector);
    SWSS_LOG_NOTICE("TAM: Updated STATE_DB for drop monitor session '%s' status=%s",
                   sessionName.c_str(), status.c_str());
}

/**
 * @brief Remove drop monitor session status from STATE_DB
 *
 * @param sessionName Name of the session to remove
 */
void TamOrch::removeDropMonitorSessionState(const string& sessionName)
{
    SWSS_LOG_ENTER();

    m_stateDbDropMonitorSessionTable.del(sessionName);
    SWSS_LOG_NOTICE("TAM: Removed STATE_DB entry for drop monitor session '%s'",
                   sessionName.c_str());
}

bool TamOrch::createTamCollectorEntry(const string& collectorName, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    // We handle collector updates as delete + add.  So, first attempt to delete the old entry
    // If the entry does not exist, it is a noop.
    deleteTamCollectorEntry(collectorName);

    // Now process the config
    auto collectorEntry = std::make_shared<TamCollectorEntry>(collectorName);

    for (auto i : data)
    {
        try {
            SWSS_LOG_DEBUG("Collector name:%s Key:%s Value:%s", collectorName.c_str(),
                    fvField(i).c_str(), fvValue(i).c_str());
            if (fvField(i) == COLLECTOR_SRC_IP)
            {
                collectorEntry->srcIp = fvValue(i);
            }
            else if (fvField(i) == COLLECTOR_DST_IP)
            {
                collectorEntry->dstIp = fvValue(i);
            }
            else if (fvField(i) == COLLECTOR_L4_DST_PORT)
            {
                collectorEntry->l4DstPort = (uint16_t) atoi(fvValue(i).c_str());
            }
            else if (fvField(i) == COLLECTOR_DSCP_VALUE)
            {
                collectorEntry->dscp = (uint8_t) atoi(fvValue(i).c_str());
            }
            else if (fvField(i) == COLLECTOR_VRF)
            {
                collectorEntry->vrfName = fvValue(i);
            }
        }
        catch (const exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse collector %s attribute %s error: %s.", collectorName.c_str(),
                    fvField(i).c_str(), e.what());
            return false;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to parse collector %s attribute %s. Unknown error has occurred",
                    collectorName.c_str(), fvField(i).c_str());
            return false;
        }
    }

    // If the all the relevant information is present, attach to routeOrch to get the
    // nexthop for the collector destination IP address
    if (collectorEntry->validConfig())
    {
        // If the VRF does not exist, return failure
        if (collectorEntry->vrfName != "default" &&
                (!collectorEntry->vrfName.empty() &&
                  (!m_vrfOrch->isVRFexists(collectorEntry->vrfName)))) {
            SWSS_LOG_ERROR("VRF %s does not exist", collectorEntry->vrfName.c_str());
            return false;
        }

        // Get the VRF id for the VRF and attach to routeOrch to resolve the collector destination IP
        auto vrfId = m_vrfOrch->getVRFid(collectorEntry->vrfName);
        SWSS_LOG_DEBUG("Attaching to m_routeOrch to resolve %s on VRF %s id %" PRIx64,
                collectorEntry->dstIp.to_string().c_str(),
                collectorEntry->vrfName.c_str(), vrfId);
        m_collectorTables[ collectorName ] = collectorEntry;
        m_routeOrch->attach(this, collectorEntry->dstIp, vrfId);
    }
    else
    {
        return false;
    }

    return true;
}

/**
 * @brief Retrieve neighbor information for a TAM collector
 *
 * Attempts to resolve the neighbor (MAC address and interface) for a TAM collector's
 * destination IP address. This is required before a TAM collector SAI object can be
 * created, as the transport layer needs the destination MAC address.
 *
 * The function tries two approaches in order:
 * 1. Direct neighbor lookup using the collector's destination IP
 * 2. If direct lookup fails and a nexthop is configured, lookup using the nexthop IP
 *
 * If passive lookup fails, this function will trigger active neighbor resolution
 * via ARP/NDP and return false. The caller should retry later after the neighbor
 * has been resolved.
 *
 * @param collectorEntry Shared pointer to the collector entry to resolve
 * @return true if neighbor information was successfully retrieved, false otherwise
 *
 * @note The retrieved neighbor information is stored in collectorEntry->neighborInfo:
 *       - neighborInfo.neighbor: NeighborEntry with IP and interface alias
 *       - neighborInfo.mac: Destination MAC address for the collector
 */
bool TamOrch::getNeighborInfo(std::shared_ptr<TamCollectorEntry> collectorEntry)
{
    SWSS_LOG_ENTER();

    NeighborEntry neighborEntry;
    MacAddress macAddress;
    IpAddress ipToResolve;
    string interfaceAlias;
    bool found = false;

    // First attempt: Try direct neighbor lookup using collector's destination IP
    // This works when the collector is directly connected (same subnet)
    if (m_neighOrch->getNeighborEntry(collectorEntry->dstIp,
                                       neighborEntry,
                                       macAddress))
    {
        found = true;
        ipToResolve = collectorEntry->dstIp;
    }
    // Second attempt: If direct lookup fails, try using the nexthop IP
    // This handles cases where collector is reachable through a gateway
    else if (!collectorEntry->nexthopInfo.nexthop.ip_address.isZero() &&
             m_neighOrch->getNeighborEntry(collectorEntry->nexthopInfo.nexthop,
                                            neighborEntry,
                                            macAddress))
    {
        found = true;
        ipToResolve = collectorEntry->nexthopInfo.nexthop.ip_address;
    }

    if (found)
    {
        // Success - neighbor is resolved
        collectorEntry->neighborInfo.neighbor = neighborEntry;
        collectorEntry->neighborInfo.mac = macAddress;

        SWSS_LOG_NOTICE("Neighbor resolved for collector %s: %s -> %s on %s",
                     collectorEntry->collectorName.c_str(),
                     ipToResolve.to_string().c_str(),
                     macAddress.to_string().c_str(),
                     neighborEntry.alias.c_str());

        return true;
    }

    // Neighbor not found - trigger active resolution

    // Determine which IP and interface to use for resolution
    if (!collectorEntry->nexthopInfo.nexthop.ip_address.isZero())
    {
        // Use nexthop IP and interface
        ipToResolve = collectorEntry->nexthopInfo.nexthop.ip_address;
        interfaceAlias = collectorEntry->nexthopInfo.nexthop.alias;
    }
    else
    {
        // Use direct IP - need to find the router interface
        ipToResolve = collectorEntry->dstIp;
        interfaceAlias = gIntfsOrch->getRouterIntfsAlias(ipToResolve);

        if (interfaceAlias.empty())
        {
            SWSS_LOG_ERROR("Cannot resolve neighbor for collector %s: "
                          "no router interface found for %s",
                          collectorEntry->collectorName.c_str(),
                          ipToResolve.to_string().c_str());
            return false;
        }
    }

    // Create NeighborEntry and trigger active resolution (ARP/NDP request)
    NeighborEntry entryToResolve(ipToResolve, interfaceAlias);

    SWSS_LOG_NOTICE("Triggering ARP/NDP resolution for collector %s: %s on %s",
                   collectorEntry->collectorName.c_str(),
                   ipToResolve.to_string().c_str(),
                   interfaceAlias.c_str());

    m_neighOrch->resolveNeighbor(entryToResolve);

    // Return false - Callback from NeighOrch is going retry this code path
    return false;
}

/**
 * @brief Update a TAM collector's state and SAI objects based on current neighbor/nexthop information
 *
 * This function is called when neighbor or nexthop information changes that may affect a TAM collector.
 * It re-evaluates the collector's neighbor resolution state and updates the SAI collector object accordingly.
 *
 * The function implements a state machine with the following transitions:
 * - Resolved → Unresolved: Delete the SAI collector object
 * - Resolved → Resolved (changed): Delete and recreate the SAI collector object
 * - Unresolved → Resolved: Create the SAI collector object
 * - Unresolved → Unresolved: No action (waiting for resolution)
 *
 * When creating a collector, this function attempts to resolve neighbor information (MAC address
 * and interface), retrieves port information from PortsOrch, handles special cases like LAG ports
 * (uses first LAG member), creates a SAI transport object with random source port, and finally
 * creates the SAI collector object.
 *
 * @param collectorEntry Shared pointer to the collector entry to update
 *
 * @note This function is called from:
 *       - handleNeighborUpdate() when ARP/NDP entries change
 *       - handleNextHopUpdate() when routing table changes
 *       - createTamCollectorEntry() during initial collector configuration
 *
 * @note For LAG ports, the function uses the first member of the LAG to obtain the physical port ID.
 *       This is required because SAI transport objects need a physical port, not a LAG object.
 *
 * @note The function uses a random ephemeral source port (49152-65535) for the transport object.
 *       This follows RFC 6335 recommendations for dynamic/private ports.
 *
 * @note State transitions that involve deletion use handleCollectorChange() to clean up
 *       all dependent objects (sessions, reports, etc.) before recreating the collector.
 */
void TamOrch::updateCollector(std::shared_ptr<TamCollectorEntry> collectorEntry)
{
    auto collectorName = collectorEntry->collectorName;
    bool prevResolved = collectorEntry->resolved;

    // Attempt to resolve neighbor information (MAC address and interface) for the collector
    // This may trigger active ARP/NDP resolution if the neighbor is not yet in the table
    if (!getNeighborInfo(collectorEntry)) {
        SWSS_LOG_DEBUG("getNeighborInfo failed for collector %s", collectorName.c_str());
        collectorEntry->resolved = false;
    } else {
        SWSS_LOG_DEBUG("getNeighborInfo successful for collector %s", collectorName.c_str());
        collectorEntry->resolved = true;
    }

    SWSS_LOG_DEBUG("Collector %s resolution state: prev=%d current=%d",
                  collectorName.c_str(), prevResolved, collectorEntry->resolved);

    // Handle state transitions based on previous and current resolution state

    // If the collector was resolved but is now unresolved (neighbor/nexthop deleted), or
    // if the collector was resolved and remains resolved (neighbor/nexthop changed), then
    // the existing SAI collector object must be deleted
    if ((prevResolved && !collectorEntry->resolved) ||
        (prevResolved && collectorEntry->resolved)) {
        // Delete all dependent objects (sessions, reports, etc.) and the collector itself
        handleCollectorChange(collectorName);
        tamCollectorDecRefCountByName(collectorName);
    }
    // If the collector was unresolved and remains unresolved, there's nothing to do
    // as we're still waiting for neighbor/nexthop resolution
    else if (!prevResolved && !collectorEntry->resolved) {
        SWSS_LOG_DEBUG("Collector %s remains unresolved, waiting for neighbor resolution",
                      collectorName.c_str());
        return;
    }
    // If the collector was unresolved and is now resolved, proceed to create SAI objects below

    // If the collector is still unresolved after the update, nothing more to do
    if (!collectorEntry->resolved) {
        SWSS_LOG_DEBUG("Collector %s is unresolved, skipping SAI object creation",
                      collectorName.c_str());
        return;
    }

    // Collector is now resolved - retrieve port information for the neighbor's interface
    m_portsOrch->getPort(collectorEntry->neighborInfo.neighbor.alias, collectorEntry->neighborInfo.port);

    // Determine the physical port ID based on the port type
    // SAI transport objects require a physical port, not a logical port like LAG
    switch (collectorEntry->neighborInfo.port.m_type) {
        case Port::PHY:
            // Physical port - use the port ID directly
            SWSS_LOG_DEBUG("Collector %s uses physical port %s",
                          collectorName.c_str(),
                          collectorEntry->neighborInfo.port.m_alias.c_str());
            collectorEntry->neighborInfo.portId = collectorEntry->neighborInfo.port.m_port_id;
            break;

        case Port::LAG:
            {
                // LAG port - need to use the first member's physical port ID
                // SAI transport objects cannot be created on LAG objects directly
                SWSS_LOG_DEBUG("Collector %s uses LAG port %s, selecting first member",
                              collectorName.c_str(),
                              collectorEntry->neighborInfo.port.m_alias.c_str());
                Port member;
                string first_member_alias = *collectorEntry->neighborInfo.port.m_members.begin();
                m_portsOrch->getPort(first_member_alias, member);
                collectorEntry->neighborInfo.portId = member.m_port_id;
                SWSS_LOG_DEBUG("Selected LAG member %s for collector %s",
                              first_member_alias.c_str(), collectorName.c_str());
                break;
            }

        case Port::VLAN:
        case Port::SYSTEM:
        case Port::TUNNEL:
        case Port::SUBPORT:
        case Port::LOOPBACK:
        case Port::MGMT:
        case Port::UNKNOWN:
        default:
            // Unsupported port types for TAM collectors
            SWSS_LOG_ERROR("Invalid port type %d for collector %s on interface %s",
                          collectorEntry->neighborInfo.port.m_type,
                          collectorName.c_str(),
                          collectorEntry->neighborInfo.neighbor.alias.c_str());
            return;
    }

    // Log the complete collector information before creating SAI objects
    SWSS_LOG_DEBUG("Creating SAI objects for collector %s: dest=%s mac=%s port=%s portId=%" PRIx64,
            collectorName.c_str(),
            collectorEntry->dstIp.to_string().c_str(),
            collectorEntry->neighborInfo.mac.to_string().c_str(),
            collectorEntry->neighborInfo.port.m_alias.c_str(),
            collectorEntry->neighborInfo.portId);

    // Create SAI transport object
    // Use a random ephemeral source port (49152-65535) per RFC 6335
    uint16_t l4SrcPort = (uint16_t) (49152 + rand()%(65535 - 49152));
    sai_object_id_t transportObjId = SAI_NULL_OBJECT_ID;

    if (tamTransportCreate(l4SrcPort, collectorEntry->l4DstPort,
                gMacAddress, collectorEntry->neighborInfo.mac,
                transportObjId)) {
        // Transport object created successfully - now create the TAM collector object
        if (!tamCollectorCreate(collectorEntry, transportObjId)) {
            SWSS_LOG_ERROR("Failed to create SAI collector object for collector %s",
                          collectorName.c_str());
            // Clean up the transport object since collector creation failed
            tamTransportDecRefCountById(transportObjId);
            collectorEntry->transportObjId = SAI_NULL_OBJECT_ID;
            return;
        }
        SWSS_LOG_NOTICE("Created TAM collector object for %s",
                       collectorName.c_str());
    } else {
        SWSS_LOG_ERROR("Failed to create TAM transport for collector %s",
                      collectorName.c_str());
    }
}

/**
 * @brief Observer callback for neighbor (ARP/NDP) table changes
 *
 * This function is called by NeighOrch when a neighbor entry is added, modified, or deleted
 * in the neighbor table. It implements the Observer pattern to receive notifications about
 * ARP (IPv4) and NDP (IPv6) changes that may affect TAM collectors.
 *
 * The function iterates through all configured TAM collectors and updates those that are
 * affected by the neighbor change. A collector is considered affected if the neighbor IP
 * matches the collector's destination IP (directly connected collector), or if the neighbor
 * IP matches the collector's nexthop IP (collector via gateway).
 *
 * When a matching collector is found, updateCollector() is called to re-resolve the neighbor
 * information (MAC address and interface), update or recreate the SAI collector object if the
 * neighbor information changed, and handle neighbor deletion by cleaning up the SAI collector
 * object.
 *
 * @param update NeighborUpdate structure containing:
 *               - entry: NeighborEntry with IP address and interface alias
 *               - mac: MAC address of the neighbor
 *               - add: true if neighbor was added/updated, false if deleted
 *
 * @note This function is called from TamOrch::update() when SUBJECT_TYPE_NEIGH_CHANGE
 *       notifications are received from NeighOrch.
 *
 * @note TamOrch registers as an observer of NeighOrch in its constructor via:
 *       m_neighOrch->attach(this)
 *
 * @note This callback is crucial for the active neighbor resolution feature. When
 *       getNeighborInfo() triggers ARP/NDP resolution via resolveNeighbor(), the kernel
 *       sends an ARP Request or NDP Solicitation. When the reply is received, NeighOrch
 *       adds the neighbor to the table, and this callback is invoked, allowing the
 *       collector to be created.
 *
 * @note Common scenarios that trigger this callback:
 *       - ARP/NDP reply received after active resolution
 *       - Neighbor entry aged out and re-learned
 *       - Neighbor MAC address changed (e.g., failover, VM migration)
 *       - Neighbor entry manually added/deleted via "ip neigh" command
 *       - Interface flap causing neighbor table flush
 *
 * @see handleNextHopUpdate() for routing table change notifications
 * @see updateCollector() for the actual collector update logic
 * @see NeighOrch::doTask() for where neighbor changes are detected and notified
 */
void TamOrch::handleNeighborUpdate(const NeighborUpdate& update)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Received neighbor update: IP=%s MAC=%s interface=%s action=%s",
                 update.entry.ip_address.to_string().c_str(),
                 update.mac.to_string().c_str(),
                 update.entry.alias.c_str(),
                 update.add ? "ADD/UPDATE" : "DELETE");

    // Iterate through all configured TAM collectors to find those affected by this neighbor change
    for (auto it = m_collectorTables.begin(); it != m_collectorTables.end(); it++)
    {
        auto collectorEntry = it->second;

        SWSS_LOG_DEBUG("Checking collector %s: dstIp=%s nexthop=%s against update IP=%s",
                collectorEntry->collectorName.c_str(),
                collectorEntry->dstIp.to_string().c_str(),
                collectorEntry->nexthopInfo.nexthop.ip_address.to_string().c_str(),
                update.entry.ip_address.to_string().c_str());

        // Check if this collector is affected by the neighbor update. A collector is affected
        // if it's directly connected (destination IP matches neighbor IP) or if it's reachable
        // via a gateway (nexthop IP matches neighbor IP)
        if (collectorEntry->dstIp != update.entry.ip_address &&
                collectorEntry->nexthopInfo.nexthop.ip_address != update.entry.ip_address) {
            // This collector is not affected by this neighbor change - skip it
            continue;
        }

        // This collector is affected - update its state and SAI objects
        SWSS_LOG_NOTICE("Neighbor update affects collector %s, triggering update",
                       collectorEntry->collectorName.c_str());
        updateCollector(collectorEntry);
    }
}

/**
 * @brief Observer callback for routing table (nexthop) changes
 *
 * This function is called by RouteOrch when a route entry is added, modified, or deleted
 * in the routing table. It implements the Observer pattern to receive notifications about
 * routing changes that may affect TAM collectors.
 *
 * The function iterates through all configured TAM collectors and updates those whose
 * destination IP matches the route's destination. This is necessary because TAM collectors
 * may be on remote networks reachable via gateways, the nexthop (gateway) IP is needed to
 * resolve the neighbor MAC address, routing changes can affect which gateway is used to
 * reach the collector, and ECMP (Equal-Cost Multi-Path) routes may have multiple nexthops.
 *
 * When a matching collector is found, the function updates the collector's prefix information,
 * selects an appropriate nexthop from the nexthop group (for ECMP), handles route deletion
 * by clearing the nexthop, and calls updateCollector() to re-resolve neighbor and update
 * SAI objects.
 *
 * @param update NextHopUpdate structure containing:
 *               - vrf_id: VRF object ID for the route
 *               - destination: Destination IP address of the route
 *               - prefix: IP prefix of the route (e.g., 10.0.0.0/24)
 *               - nexthopGroup: Set of nexthops (gateways) for this route
 *
 * @note This function is called from TamOrch::update() when SUBJECT_TYPE_NEXTHOP_CHANGE
 *       notifications are received from RouteOrch.
 *
 * @note For ECMP routes with multiple nexthops, this function implements a simple selection
 *       strategy. If the current nexthop is still in the ECMP group, it keeps using it.
 *       Otherwise, it selects the first nexthop from the group. This ensures stability
 *       (avoids unnecessary collector recreation) while handling nexthop changes.
 *
 * @note When a route is deleted (nexthopGroup is empty), the nexthop is set to 0.0.0.0 (IPv4)
 *       or :: (IPv6) with empty interface. This triggers collector cleanup in updateCollector().
 *
 * @note Common scenarios that trigger this callback:
 *       - Static route added/deleted via "ip route" command
 *       - Dynamic route learned/withdrawn via BGP/OSPF
 *       - Route nexthop changed due to link failure
 *       - ECMP group membership changed
 *       - Route metric changed causing best path selection change
 *
 * @note Interaction with active neighbor resolution: When a new route is added, this callback
 *       updates the nexthop. Then updateCollector() calls getNeighborInfo() with the new nexthop,
 *       which triggers ARP/NDP resolution for the gateway. When resolution completes,
 *       handleNeighborUpdate() is called, and the collector is created with the resolved gateway MAC.
 *
 * @see handleNeighborUpdate() for neighbor table change notifications
 * @see updateCollector() for the actual collector update logic
 * @see RouteOrch::addRoute() and RouteOrch::removeRoute() for where route changes are detected
 */
void TamOrch::handleNextHopUpdate(const NextHopUpdate &update)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Received nexthop update: destination=%s prefix=%s nexthopGroup=%s",
                 update.destination.to_string().c_str(),
                 update.prefix.to_string().c_str(),
                 update.nexthopGroup.to_string().c_str());

    // Iterate over all the collectors and handle the impacted collectors
    for (auto it = m_collectorTables.begin(); it != m_collectorTables.end(); it++)
    {
        auto collectorEntry = it->second;

        SWSS_LOG_DEBUG("Checking collector %s: dstIp=%s against update destination=%s prefix=%s",
                collectorEntry->collectorName.c_str(),
                collectorEntry->dstIp.to_string().c_str(),
                update.destination.to_string().c_str(),
                update.prefix.to_string().c_str());

        // Check if this collector is affected by the route update
        // Only process if the collector's destination IP matches the route's destination
        if (collectorEntry->dstIp != update.destination)
        {
            // This collector is not affected by this route change - skip it
            continue;
        }

        // Update the collector's prefix information
        collectorEntry->nexthopInfo.prefix = update.prefix;

        // Check if the route has valid nexthops (route is reachable)
        if (update.nexthopGroup != NextHopGroupKey())
        {
            // Route has one or more nexthops (may be ECMP with multiple nexthops)
            SWSS_LOG_DEBUG("Route has nexthops: %s", update.nexthopGroup.to_string().c_str());

            // Optimization: If we already have a nexthop programmed and it's still part
            // of the ECMP group, keep using it to avoid unnecessary collector recreation
            if (update.nexthopGroup.getNextHops().count(collectorEntry->nexthopInfo.nexthop)) {
                SWSS_LOG_DEBUG("Collector %s nexthop %s is still valid in ECMP group, no change needed",
                              collectorEntry->collectorName.c_str(),
                              collectorEntry->nexthopInfo.nexthop.to_string().c_str());
                continue;
            }

            // Either this is a new route, or the old nexthop we had is no longer valid.
            // Select the first nexthop from the ECMP group.
            collectorEntry->nexthopInfo.nexthop = *update.nexthopGroup.getNextHops().begin();
            SWSS_LOG_NOTICE("Collector %s nexthop changed to %s",
                           collectorEntry->collectorName.c_str(),
                           collectorEntry->nexthopInfo.nexthop.to_string().c_str());
        } else {
            // Route has no nexthops - route was deleted or became unreachable.
            // Set nexthop to zero address to trigger collector cleanup.
            SWSS_LOG_NOTICE("Route to collector %s destination %s is no longer reachable, clearing nexthop",
                           collectorEntry->collectorName.c_str(),
                           collectorEntry->dstIp.to_string().c_str());
            string alias = "";
            collectorEntry->nexthopInfo.nexthop =
                collectorEntry->dstIp.isV4() ? NextHopKey("0.0.0.0", alias) : NextHopKey("::", alias);
        }

        // Update the collector's state and SAI objects based on the new nexthop information
        // This will trigger neighbor resolution for the new nexthop (if any)
        updateCollector(collectorEntry);
    }
}

void TamOrch::update(SubjectType type, void *ctx)
{
    SWSS_LOG_ENTER();
    assert(ctx);

    switch(type) {
        case SUBJECT_TYPE_NEXTHOP_CHANGE:
            {
                NextHopUpdate *update = static_cast<NextHopUpdate *>(ctx);
                handleNextHopUpdate(*update);
                break;
            }

            case SUBJECT_TYPE_NEIGH_CHANGE:
            {
                NeighborUpdate *update = static_cast<NeighborUpdate *>(ctx);
                handleNeighborUpdate(*update);
                break;
            }

            case SUBJECT_TYPE_FDB_CHANGE:
            SWSS_LOG_DEBUG("SUBJECT_TYPE_NEIGH_CHANGE");
            break;

        default:
            SWSS_LOG_DEBUG("Unknown update %d", type);
            break;
    }
}
