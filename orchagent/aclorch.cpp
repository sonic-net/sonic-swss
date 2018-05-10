#include <limits.h>
#include <unordered_map>
#include <algorithm>
#include "aclorch.h"
#include "logger.h"
#include "schema.h"
#include "ipprefix.h"
#include "converter.h"
#include "timer.h"
#include "crmorch.h"

using namespace std;
using namespace swss;

mutex AclOrch::m_countersMutex;
map<acl_range_properties_t, AclRange*> AclRange::m_ranges;
condition_variable AclOrch::m_sleepGuard;
bool AclOrch::m_bCollectCounters = true;
sai_uint32_t AclRule::m_minPriority = 0;
sai_uint32_t AclRule::m_maxPriority = 0;

swss::DBConnector AclOrch::m_db(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
swss::Table AclOrch::m_countersTable(&m_db, "COUNTERS");

extern sai_acl_api_t*    sai_acl_api;
extern sai_port_api_t*   sai_port_api;
extern sai_switch_api_t* sai_switch_api;
extern sai_object_id_t   gSwitchId;
extern PortsOrch*        gPortsOrch;
extern CrmOrch *gCrmOrch;

acl_rule_attr_lookup_t aclMatchLookup =
{
    { MATCH_SRC_IP,            SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP },
    { MATCH_DST_IP,            SAI_ACL_ENTRY_ATTR_FIELD_DST_IP },
    { MATCH_SRC_IPV6,          SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6 },
    { MATCH_DST_IPV6,          SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6 },
    { MATCH_L4_SRC_PORT,       SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT },
    { MATCH_L4_DST_PORT,       SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT },
    { MATCH_ETHER_TYPE,        SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE },
    { MATCH_IP_PROTOCOL,       SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL },
    { MATCH_TCP_FLAGS,         SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS },
    { MATCH_IP_TYPE,           SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE },
    { MATCH_DSCP,              SAI_ACL_ENTRY_ATTR_FIELD_DSCP },
    { MATCH_TC,                SAI_ACL_ENTRY_ATTR_FIELD_TC },
    { MATCH_L4_SRC_PORT_RANGE, (sai_acl_entry_attr_t)SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE },
    { MATCH_L4_DST_PORT_RANGE, (sai_acl_entry_attr_t)SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE },
};

acl_rule_attr_lookup_t aclL3ActionLookup =
{
    { PACKET_ACTION_FORWARD,  SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION },
    { PACKET_ACTION_DROP,     SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION },
    { PACKET_ACTION_REDIRECT, SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT }
};

static acl_table_type_lookup_t aclTableTypeLookUp =
{
    { TABLE_TYPE_L3,        ACL_TABLE_L3 },
    { TABLE_TYPE_L3V6,      ACL_TABLE_L3V6 },
    { TABLE_TYPE_MIRROR,    ACL_TABLE_MIRROR },
    { TABLE_TYPE_CTRLPLANE, ACL_TABLE_CTRLPLANE }
};

static acl_stage_type_lookup_t aclStageLookUp =
{
    {TABLE_INGRESS, ACL_STAGE_INGRESS },
    {TABLE_EGRESS,  ACL_STAGE_EGRESS }
};

static acl_ip_type_lookup_t aclIpTypeLookup =
{
    { IP_TYPE_ANY,         SAI_ACL_IP_TYPE_ANY },
    { IP_TYPE_IP,          SAI_ACL_IP_TYPE_IP },
    { IP_TYPE_NON_IP,      SAI_ACL_IP_TYPE_NON_IP },
    { IP_TYPE_IPv4ANY,     SAI_ACL_IP_TYPE_IPV4ANY },
    { IP_TYPE_NON_IPv4,    SAI_ACL_IP_TYPE_NON_IPV4 },
    { IP_TYPE_IPv6ANY,     SAI_ACL_IP_TYPE_IPV6ANY },
    { IP_TYPE_NON_IPv6,    SAI_ACL_IP_TYPE_NON_IPV6 },
    { IP_TYPE_ARP,         SAI_ACL_IP_TYPE_ARP },
    { IP_TYPE_ARP_REQUEST, SAI_ACL_IP_TYPE_ARP_REQUEST },
    { IP_TYPE_ARP_REPLY,   SAI_ACL_IP_TYPE_ARP_REPLY }
};

inline string toUpper(const string& str)
{
    string uppercase = str;

    transform(uppercase.begin(), uppercase.end(), uppercase.begin(), ::toupper);

    return uppercase;
}

inline string trim(const std::string& str, const std::string& whitespace = " \t")
{
    const auto strBegin = str.find_first_not_of(whitespace);
    if (strBegin == std::string::npos)
        return "";

    const auto strEnd = str.find_last_not_of(whitespace);
    const auto strRange = strEnd - strBegin + 1;

    return str.substr(strBegin, strRange);
}

AclRule::AclRule(AclOrch *aclOrch, string rule, string table, acl_table_type_t type) :
        m_pAclOrch(aclOrch),
        m_id(rule),
        m_tableId(table),
        m_tableType(type),
        m_tableOid(SAI_NULL_OBJECT_ID),
        m_ruleOid(SAI_NULL_OBJECT_ID),
        m_counterOid(SAI_NULL_OBJECT_ID),
        m_priority(0)
{
    m_tableOid = aclOrch->getTableById(m_tableId);
}

bool AclRule::validateAddPriority(string attr_name, string attr_value)
{
    bool status = false;

    if (attr_name == RULE_PRIORITY)
    {
        char *endp = NULL;
        errno = 0;
        m_priority = (uint32_t)strtol(attr_value.c_str(), &endp, 0);
        // chack conversion was successfull and the value is within the allowed range
        status = (errno == 0) &&
                 (endp == attr_value.c_str() + attr_value.size()) &&
                 (m_priority >= m_minPriority) &&
                 (m_priority <= m_maxPriority);
    }

    return status;
}

bool AclRule::validateAddMatch(string attr_name, string attr_value)
{
    SWSS_LOG_ENTER();

    sai_attribute_value_t value;

    try
    {
        if (aclMatchLookup.find(attr_name) == aclMatchLookup.end())
        {
            return false;
        }
        else if(attr_name == MATCH_IP_TYPE)
        {
            if (!processIpType(attr_value, value.aclfield.data.u32))
            {
                SWSS_LOG_ERROR("Invalid IP type %s", attr_value.c_str());
                return false;
            }

            value.aclfield.mask.u32 = 0xFFFFFFFF;
        }
        else if(attr_name == MATCH_TCP_FLAGS)
        {
            vector<string> flagsData;
            string flags, mask;
            int val;
            char *endp = NULL;
            errno = 0;

            split(attr_value, flagsData, '/');

            if (flagsData.size() != 2) // expect two parts flags and mask separated with '/'
            {
                SWSS_LOG_ERROR("Invalid TCP flags format %s", attr_value.c_str());
                return false;
            }

            flags = trim(flagsData[0]);
            mask = trim(flagsData[1]);

            val = (uint32_t)strtol(flags.c_str(), &endp, 0);
            if (errno || (endp != flags.c_str() + flags.size()) ||
                (val < 0) || (val > UCHAR_MAX))
            {
                SWSS_LOG_ERROR("TCP flags parse error, value: %s(=%d), errno: %d", flags.c_str(), val, errno);
                return false;
            }
            value.aclfield.data.u8 = (uint8_t)val;

            val = (uint32_t)strtol(mask.c_str(), &endp, 0);
            if (errno || (endp != mask.c_str() + mask.size()) ||
                (val < 0) || (val > UCHAR_MAX))
            {
                SWSS_LOG_ERROR("TCP mask parse error, value: %s(=%d), errno: %d", mask.c_str(), val, errno);
                return false;
            }
            value.aclfield.mask.u8 = (uint8_t)val;
        }
        else if(attr_name == MATCH_ETHER_TYPE || attr_name == MATCH_L4_SRC_PORT || attr_name == MATCH_L4_DST_PORT)
        {
            value.aclfield.data.u16 = to_uint<uint16_t>(attr_value);
            value.aclfield.mask.u16 = 0xFFFF;
        }
        else if(attr_name == MATCH_DSCP)
        {
            value.aclfield.data.u8 = to_uint<uint8_t>(attr_value, 0, 0x3F);
            value.aclfield.mask.u8 = 0x3F;
        }
        else if(attr_name == MATCH_IP_PROTOCOL)
        {
            value.aclfield.data.u8 = to_uint<uint8_t>(attr_value);
            value.aclfield.mask.u8 = 0xFF;
        }
        else if (attr_name == MATCH_SRC_IP || attr_name == MATCH_DST_IP)
        {
            IpPrefix ip(attr_value);

            if (!ip.isV4())
            {
                SWSS_LOG_ERROR("IP type is not v4 type");
                return false;
            }
            value.aclfield.data.ip4 = ip.getIp().getV4Addr();
            value.aclfield.mask.ip4 = ip.getMask().getV4Addr();
        }
        else if (attr_name == MATCH_SRC_IPV6 || attr_name == MATCH_DST_IPV6)
        {
            IpPrefix ip(attr_value);
            if (ip.isV4())
            {
                SWSS_LOG_ERROR("IP type is not v6 type");
                return false;
            }
            memcpy(value.aclfield.data.ip6, ip.getIp().getV6Addr(), 16);
            memcpy(value.aclfield.mask.ip6, ip.getMask().getV6Addr(), 16);
        }
        else if ((attr_name == MATCH_L4_SRC_PORT_RANGE) || (attr_name == MATCH_L4_DST_PORT_RANGE))
        {
            if (sscanf(attr_value.c_str(), "%d-%d", &value.u32range.min, &value.u32range.max) != 2)
            {
                SWSS_LOG_ERROR("Range parse error. Attribute: %s, value: %s", attr_name.c_str(), attr_value.c_str());
                return false;
            }

            // check boundaries
            if ((value.u32range.min > USHRT_MAX) ||
                (value.u32range.max > USHRT_MAX) ||
                (value.u32range.min > value.u32range.max))
            {
                SWSS_LOG_ERROR("Range parse error. Invalid range value. Attribute: %s, value: %s", attr_name.c_str(), attr_value.c_str());
                return false;
            }
        }
        else if(attr_name == MATCH_TC)
        {
            value.aclfield.data.u8 = to_uint<uint8_t>(attr_value);
            value.aclfield.mask.u8 = 0xFF;
        }

    }
    catch (exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse %s attribute %s value. Error: %s", attr_name.c_str(), attr_value.c_str(), e.what());
        return false;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Failed to parse %s attribute %s value.", attr_name.c_str(), attr_value.c_str());
        return false;
    }

    m_matches[aclMatchLookup[attr_name]] = value;

    return true;
}

bool AclRule::processIpType(string type, sai_uint32_t &ip_type)
{
    SWSS_LOG_ENTER();

    auto it = aclIpTypeLookup.find(toUpper(type));

    if (it == aclIpTypeLookup.end())
    {
        return false;
    }

    ip_type = it->second;

    return true;
}

bool AclRule::create()
{
    SWSS_LOG_ENTER();

    sai_object_id_t table_oid = m_pAclOrch->getTableById(m_tableId);
    vector<sai_attribute_t> rule_attrs;
    sai_object_id_t range_objects[2];
    sai_object_list_t range_object_list = {0, range_objects};

    sai_attribute_t attr;
    sai_status_t status;

    if (!createCounter())
    {
        return false;
    }

    SWSS_LOG_INFO("Created counter for the rule %s in table %s", m_id.c_str(), m_tableId.c_str());

    // store table oid this rule belongs to
    attr.id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    attr.value.oid = table_oid;
    rule_attrs.push_back(attr);

    attr.id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    attr.value.u32 = m_priority;
    rule_attrs.push_back(attr);

    attr.id = SAI_ACL_ENTRY_ATTR_ADMIN_STATE;
    attr.value.booldata = true;
    rule_attrs.push_back(attr);

    // add reference to the counter
    attr.id = SAI_ACL_ENTRY_ATTR_ACTION_COUNTER;
    attr.value.aclaction.parameter.oid = m_counterOid;
    attr.value.aclaction.enable = true;
    rule_attrs.push_back(attr);

    // store matches
    for (auto it : m_matches)
    {
        // collect ranges and add them later as a list
        if (((sai_acl_range_type_t)it.first == SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE) ||
            ((sai_acl_range_type_t)it.first == SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE))
        {
            SWSS_LOG_INFO("Creating range object %u..%u", it.second.u32range.min, it.second.u32range.max);

            AclRange *range = AclRange::create((sai_acl_range_type_t)it.first, it.second.u32range.min, it.second.u32range.max);
            if (!range)
            {
                // release already created range if any
                AclRange::remove(range_objects, range_object_list.count);
                return false;
            }
            else
            {
                range_objects[range_object_list.count++] = range->getOid();
            }
        }
        else
        {
            attr.id = it.first;
            attr.value = it.second;
            attr.value.aclfield.enable = true;
            rule_attrs.push_back(attr);
        }
    }

    // store ranges if any
    if (range_object_list.count > 0)
    {
        attr.id = SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE;
        attr.value.aclfield.enable = true;
        attr.value.aclfield.data.objlist = range_object_list;
        rule_attrs.push_back(attr);
    }

    // store actions
    for (auto it : m_actions)
    {
        attr.id = it.first;
        attr.value = it.second;
        rule_attrs.push_back(attr);
    }

    status = sai_acl_api->create_acl_entry(&m_ruleOid, gSwitchId, (uint32_t)rule_attrs.size(), rule_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ACL rule");
        AclRange::remove(range_objects, range_object_list.count);
        decreaseNextHopRefCount();
    }

    gCrmOrch->incCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_ENTRY, m_tableOid);

    return (status == SAI_STATUS_SUCCESS);
}

void AclRule::decreaseNextHopRefCount()
{
    if (!m_redirect_target_next_hop.empty())
    {
        m_pAclOrch->m_neighOrch->decreaseNextHopRefCount(IpAddress(m_redirect_target_next_hop));
        m_redirect_target_next_hop.clear();
    }
    if (!m_redirect_target_next_hop_group.empty())
    {
        IpAddresses target = IpAddresses(m_redirect_target_next_hop_group);
        m_pAclOrch->m_routeOrch->decreaseNextHopRefCount(target);
        // remove next hop group in case it's not used by anything else
        if (m_pAclOrch->m_routeOrch->isRefCounterZero(target))
        {
            if (m_pAclOrch->m_routeOrch->removeNextHopGroup(target))
            {
                SWSS_LOG_DEBUG("Removed acl redirect target next hop group '%s'", m_redirect_target_next_hop_group.c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Failed to remove unused next hop group '%s'", m_redirect_target_next_hop_group.c_str());
                // FIXME: what else could we do here?
            }
        }
        m_redirect_target_next_hop_group.clear();
    }

    return;
}

bool AclRule::remove()
{
    SWSS_LOG_ENTER();
    sai_status_t res;

    if (sai_acl_api->remove_acl_entry(m_ruleOid) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete ACL rule");
        return false;
    }

    gCrmOrch->decCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_ENTRY, m_tableOid);

    m_ruleOid = SAI_NULL_OBJECT_ID;

    decreaseNextHopRefCount();

    res = removeRanges();
    res &= removeCounter();

    return res;
}

AclRuleCounters AclRule::getCounters()
{
    SWSS_LOG_ENTER();

    sai_attribute_t counter_attr[2];
    counter_attr[0].id = SAI_ACL_COUNTER_ATTR_PACKETS;
    counter_attr[1].id = SAI_ACL_COUNTER_ATTR_BYTES;

    if (sai_acl_api->get_acl_counter_attribute(m_counterOid, 2, counter_attr) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get counters for %s rule", m_id.c_str());
        return AclRuleCounters();
    }

    return AclRuleCounters(counter_attr[0].value.u64, counter_attr[1].value.u64);
}

shared_ptr<AclRule> AclRule::makeShared(acl_table_type_t type, AclOrch *acl, MirrorOrch *mirror, const string& rule, const string& table, const KeyOpFieldsValuesTuple& data)
{
    string action;
    bool action_found = false;
    /* Find action configured by user. Based on action type create rule. */
    for (const auto& itr : kfvFieldsValues(data))
    {
        string attr_name = toUpper(fvField(itr));
        string attr_value = fvValue(itr);
        if (attr_name == ACTION_PACKET_ACTION || attr_name == ACTION_MIRROR_ACTION)
        {
            action_found = true;
            action = attr_name;
            break;
        }
    }

    if (!action_found)
    {
        throw runtime_error("ACL rule action is not found in rule " + rule);
    }

    if (type != ACL_TABLE_L3 && type != ACL_TABLE_L3V6 && type != ACL_TABLE_MIRROR)
    {
        throw runtime_error("Unknown table type.");
    }

    /* Mirror rules can exist in both tables*/
    if (action == ACTION_MIRROR_ACTION)
    {
        return make_shared<AclRuleMirror>(acl, mirror, rule, table, type);
    }
    /* L3 rules can exist only in L3 table */
    else if (type == ACL_TABLE_L3)
    {
        return make_shared<AclRuleL3>(acl, rule, table, type);
    }
    /* L3V6 rules can exist only in L3V6 table */
    else if (type == ACL_TABLE_L3V6)
    {
        return make_shared<AclRuleL3V6>(acl, rule, table, type);
    }

    throw runtime_error("Wrong combination of table type and action in rule " + rule);
}

bool AclRule::createCounter()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> counter_attrs;

    attr.id = SAI_ACL_COUNTER_ATTR_TABLE_ID;
    attr.value.oid = m_tableOid;
    counter_attrs.push_back(attr);

    attr.id = SAI_ACL_COUNTER_ATTR_ENABLE_BYTE_COUNT;
    attr.value.booldata = true;
    counter_attrs.push_back(attr);

    attr.id = SAI_ACL_COUNTER_ATTR_ENABLE_PACKET_COUNT;
    attr.value.booldata = true;
    counter_attrs.push_back(attr);

    if (sai_acl_api->create_acl_counter(&m_counterOid, gSwitchId, (uint32_t)counter_attrs.size(), counter_attrs.data()) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create counter for the rule %s in table %s", m_id.c_str(), m_tableId.c_str());
        return false;
    }

    gCrmOrch->incCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_COUNTER, m_tableOid);

    return true;
}

bool AclRule::removeRanges()
{
    SWSS_LOG_ENTER();
    for (auto it : m_matches)
    {
        if (((sai_acl_range_type_t)it.first == SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE) ||
            ((sai_acl_range_type_t)it.first == SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE))
        {
            return AclRange::remove((sai_acl_range_type_t)it.first, it.second.u32range.min, it.second.u32range.max);
        }
    }
    return true;
}

bool AclRule::removeCounter()
{
    SWSS_LOG_ENTER();

    if (m_counterOid == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    if (sai_acl_api->remove_acl_counter(m_counterOid) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove ACL counter for rule %s in table %s", m_id.c_str(), m_tableId.c_str());
        return false;
    }

    gCrmOrch->decCrmAclTableUsedCounter(CrmResourceType::CRM_ACL_COUNTER, m_tableOid);

    SWSS_LOG_INFO("Removing record about the counter %lX from the DB", m_counterOid);
    AclOrch::getCountersTable().del(getTableId() + ":" + getId());

    m_counterOid = SAI_NULL_OBJECT_ID;

    return true;
}

AclRuleL3::AclRuleL3(AclOrch *aclOrch, string rule, string table, acl_table_type_t type) :
        AclRule(aclOrch, rule, table, type)
{
}

bool AclRuleL3::validateAddAction(string attr_name, string _attr_value)
{
    SWSS_LOG_ENTER();

    string attr_value = toUpper(_attr_value);
    sai_attribute_value_t value;

    if (attr_name != ACTION_PACKET_ACTION)
    {
        return false;
    }

    if (attr_value == PACKET_ACTION_FORWARD)
    {
        value.aclaction.parameter.s32 = SAI_PACKET_ACTION_FORWARD;
    }
    else if (attr_value == PACKET_ACTION_DROP)
    {
        value.aclaction.parameter.s32 = SAI_PACKET_ACTION_DROP;
    }
    else if (attr_value.find(PACKET_ACTION_REDIRECT) != string::npos)
    {
        // resize attr_value to remove argument, _attr_value still has the argument
        attr_value.resize(string(PACKET_ACTION_REDIRECT).length());

        sai_object_id_t param_id = getRedirectObjectId(_attr_value);
        if (param_id == SAI_NULL_OBJECT_ID)
        {
            return false;
        }
        value.aclaction.parameter.oid = param_id;
    }
    else
    {
        return false;
    }
    value.aclaction.enable = true;

    m_actions[aclL3ActionLookup[attr_value]] = value;

    return true;
}

// This method should return sai attribute id of the redirect destination
sai_object_id_t AclRuleL3::getRedirectObjectId(const string& redirect_value)
{
    // check that we have a colon after redirect rule
    size_t colon_pos = string(PACKET_ACTION_REDIRECT).length();
    if (redirect_value[colon_pos] != ':')
    {
        SWSS_LOG_ERROR("Redirect action rule must have ':' after REDIRECT");
        return SAI_NULL_OBJECT_ID;
    }

    if (colon_pos + 1 == redirect_value.length())
    {
        SWSS_LOG_ERROR("Redirect action rule must have a target after 'REDIRECT:' action");
        return SAI_NULL_OBJECT_ID;
    }

    string target = redirect_value.substr(colon_pos + 1);

    // Try to parse physical port and LAG first
    Port port;
    if (gPortsOrch->getPort(target, port))
    {
        if (port.m_type == Port::PHY)
        {
            return port.m_port_id;
        }
        else if (port.m_type == Port::LAG)
        {
            return port.m_lag_id;
        }
        else
        {
            SWSS_LOG_ERROR("Wrong port type for REDIRECT action. Only physical ports and LAG ports are supported");
            return SAI_NULL_OBJECT_ID;
        }
    }

    // Try to parse nexthop ip address
    try
    {
        IpAddress ip(target);
        if (!m_pAclOrch->m_neighOrch->hasNextHop(ip))
        {
            SWSS_LOG_ERROR("ACL Redirect action target next hop ip: '%s' doesn't exist on the switch", ip.to_string().c_str());
            return SAI_NULL_OBJECT_ID;
        }

        m_redirect_target_next_hop = target;
        m_pAclOrch->m_neighOrch->increaseNextHopRefCount(ip);
        return m_pAclOrch->m_neighOrch->getNextHopId(ip);
    }
    catch (...)
    {
        // no error, just try next variant
    }

    // try to parse nh group ip addresses
    try
    {
        IpAddresses ips(target);
        if (!m_pAclOrch->m_routeOrch->hasNextHopGroup(ips))
        {
            SWSS_LOG_INFO("ACL Redirect action target next hop group: '%s' doesn't exist on the switch. Creating it.", ips.to_string().c_str());

            if(!m_pAclOrch->m_routeOrch->addNextHopGroup(ips))
            {
                SWSS_LOG_ERROR("Can't create required target next hop group '%s'", ips.to_string().c_str());
                return SAI_NULL_OBJECT_ID;
            }
            SWSS_LOG_DEBUG("Created acl redirect target next hop group '%s'", ips.to_string().c_str());
        }

        m_redirect_target_next_hop_group = target;
        m_pAclOrch->m_routeOrch->increaseNextHopRefCount(ips);
        return m_pAclOrch->m_routeOrch->getNextHopGroupId(ips);
    }
    catch (...)
    {
        // no error, just try next variant
    }

    SWSS_LOG_ERROR("ACL Redirect action target '%s' wasn't recognized", target.c_str());

    return SAI_NULL_OBJECT_ID;
}

bool AclRuleL3::validateAddMatch(string attr_name, string attr_value)
{
    if (attr_name == MATCH_DSCP)
    {
        SWSS_LOG_ERROR("DSCP match is not supported for the tables of type L3");
        return false;
    }
    if (attr_name == MATCH_SRC_IPV6 || attr_name == MATCH_DST_IPV6)
    {
        SWSS_LOG_ERROR("IPv6 address match is not supported for the tables of type L3");
        return false;
    }

    return AclRule::validateAddMatch(attr_name, attr_value);
}

bool AclRuleL3::validate()
{
    SWSS_LOG_ENTER();

    if (m_matches.size() == 0 || m_actions.size() != 1)
    {
        return false;
    }

    return true;
}

void AclRuleL3::update(SubjectType, void *)
{
    // Do nothing
}

AclRuleL3V6::AclRuleL3V6(AclOrch *aclOrch, string rule, string table, acl_table_type_t type) :
        AclRuleL3(aclOrch, rule, table, type)
{
}


bool AclRuleL3V6::validateAddMatch(string attr_name, string attr_value)
{
    if (attr_name == MATCH_DSCP)
    {
        SWSS_LOG_ERROR("DSCP match is not supported for the tables of type L3V6");
        return false;
    }
    if (attr_name == MATCH_SRC_IP || attr_name == MATCH_DST_IP)
    {
        SWSS_LOG_ERROR("IPv4 address match is not supported for the tables of type L3V6");
        return false;
    }

    return AclRule::validateAddMatch(attr_name, attr_value);
}

AclRuleMirror::AclRuleMirror(AclOrch *aclOrch, MirrorOrch *mirror, string rule, string table, acl_table_type_t type) :
        AclRule(aclOrch, rule, table, type),
        m_state(false),
        m_pMirrorOrch(mirror)
{
}

bool AclRuleMirror::validateAddAction(string attr_name, string attr_value)
{
    SWSS_LOG_ENTER();

    if (attr_name != ACTION_MIRROR_ACTION)
    {
        return false;
    }

    if (!m_pMirrorOrch->sessionExists(attr_value))
    {
        return false;
    }

    m_sessionName = attr_value;

    return true;
}

bool AclRuleMirror::validateAddMatch(string attr_name, string attr_value)
{
    if ((m_tableType == ACL_TABLE_L3 || m_tableType == ACL_TABLE_L3V6)
	&& attr_name == MATCH_DSCP)
    {
        SWSS_LOG_ERROR("DSCP match is not supported for the tables of type L3");
        return false;
    }

    return AclRule::validateAddMatch(attr_name, attr_value);
}

bool AclRuleMirror::validate()
{
    SWSS_LOG_ENTER();

    if (m_matches.size() == 0 || m_sessionName.empty())
    {
        return false;
    }

    return true;
}

bool AclRuleMirror::create()
{
    SWSS_LOG_ENTER();

    sai_attribute_value_t value;
    bool state = false;
    sai_object_id_t oid = SAI_NULL_OBJECT_ID;

    if (!m_pMirrorOrch->getSessionState(m_sessionName, state))
    {
        throw runtime_error("Failed to get mirror session state");
    }

    // Increase session reference count regardless of state to deny
    // attempt to remove mirror session with attached ACL rules.
    if (!m_pMirrorOrch->increaseRefCount(m_sessionName))
    {
        throw runtime_error("Failed to increase mirror session reference count");
    }

    if (!state)
    {
        return true;
    }

    if (!m_pMirrorOrch->getSessionOid(m_sessionName, oid))
    {
        throw runtime_error("Failed to get mirror session OID");
    }

    value.aclaction.enable = true;
    value.aclaction.parameter.objlist.list = &oid;
    value.aclaction.parameter.objlist.count = 1;

    m_actions.clear();
    m_actions[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS] = value;

    if (!AclRule::create())
    {
        return false;
    }

    m_state = true;

    return true;
}

bool AclRuleMirror::remove()
{
    if (!m_state)
    {
        return true;
    }

    if (!AclRule::remove())
    {
        return false;
    }

    if (!m_pMirrorOrch->decreaseRefCount(m_sessionName))
    {
        throw runtime_error("Failed to decrease mirror session reference count");
    }

    m_state = false;

    return true;
}

void AclRuleMirror::update(SubjectType type, void *cntx)
{
    if (type != SUBJECT_TYPE_MIRROR_SESSION_CHANGE)
    {
        return;
    }

    MirrorSessionUpdate *update = static_cast<MirrorSessionUpdate *>(cntx);

    if (m_sessionName != update->name)
    {
        return;
    }

    if (update->active)
    {
        SWSS_LOG_INFO("Activating mirroring ACL %s for session %s", m_id.c_str(), m_sessionName.c_str());
        create();
    }
    else
    {
        // Store counters before deactivating ACL rule
        counters += getCounters();

        SWSS_LOG_INFO("Deactivating mirroring ACL %s for session %s", m_id.c_str(), m_sessionName.c_str());
        remove();
    }
}

bool AclTable::validate()
{
    // Control plane ACLs are handled by a separate process
    if (type == ACL_TABLE_UNKNOWN || type == ACL_TABLE_CTRLPLANE) return false;
    if (ports.empty()) return false;
    return true;
}

bool AclTable::create()
{
    SWSS_LOG_ENTER();

    if (stage == ACL_STAGE_UNKNOWN)
    {
        SWSS_LOG_ERROR("Unknown ACL stage for ACL table %s", id.c_str());
        return false;
    }

    sai_attribute_t attr;
    vector<sai_attribute_t> table_attrs;

    int32_t range_types_list[] =
        { SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE,
          SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE
        };

    set<sai_acl_bind_point_type_t> binds;
    for (const auto& portid_pair : ports)
    {
        Port port;
        if (!gPortsOrch->getPort(portid_pair.first, port))
        {
         continue;
        }

        switch (port.m_type)
        {
        case Port::PHY:
            binds.insert(SAI_ACL_BIND_POINT_TYPE_PORT);
            break;
        case Port::VLAN:
            binds.insert(SAI_ACL_BIND_POINT_TYPE_VLAN);
            break;
        case Port::LAG:
            binds.insert(SAI_ACL_BIND_POINT_TYPE_LAG);
            break;
        default:
            return SAI_STATUS_FAILURE;
        }
    }

    vector<int32_t> bpoint_list;
    for (auto bind : binds)
    {
        bpoint_list.push_back(bind);
    }

    attr.id = SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST;
    attr.value.s32list.count = static_cast<uint32_t>(bpoint_list.size());
    attr.value.s32list.list = bpoint_list.data();
    table_attrs.push_back(attr);

    attr.id = SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id = SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id = SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    if (type == ACL_TABLE_L3V6)
    {
        attr.id = SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6;
        attr.value.booldata = true;
        table_attrs.push_back(attr);
    }
    else
    {
        attr.id = SAI_ACL_TABLE_ATTR_FIELD_SRC_IP;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_FIELD_DST_IP;
        attr.value.booldata = true;
        table_attrs.push_back(attr);
    }
    attr.id = SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id = SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id = SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id = SAI_ACL_TABLE_ATTR_FIELD_TC;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    if(stage == ACL_STAGE_INGRESS)
    {
        attr.id = SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE;
        attr.value.s32list.count = (uint32_t)(sizeof(range_types_list) / sizeof(range_types_list[0]));
        attr.value.s32list.list = range_types_list;
        table_attrs.push_back(attr);
    }

    attr.id = SAI_ACL_TABLE_ATTR_ACL_STAGE;
    attr.value.s32 = stage == ACL_STAGE_INGRESS ? SAI_ACL_STAGE_INGRESS : SAI_ACL_STAGE_EGRESS;
    table_attrs.push_back(attr);

    if (type == ACL_TABLE_MIRROR)
    {
        attr.id = SAI_ACL_TABLE_ATTR_FIELD_DSCP;
        attr.value.booldata = true;
        table_attrs.push_back(attr);
    }

    sai_status_t status = sai_acl_api->create_acl_table(&m_oid, gSwitchId, (uint32_t)table_attrs.size(), table_attrs.data());

    if (status == SAI_STATUS_SUCCESS)
    {
        gCrmOrch->incCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE, (sai_acl_stage_t) attr.value.s32, SAI_ACL_BIND_POINT_TYPE_PORT);
    }

    return status == SAI_STATUS_SUCCESS;
}

bool AclTable::bind(sai_object_id_t portOid)
{
    SWSS_LOG_ENTER();

    assert(ports.find(portOid) != ports.end());

    sai_object_id_t group_member_oid;
    if (!gPortsOrch->bindAclTable(portOid, m_oid, group_member_oid, stage))
    {
        return false;
    }

    ports[portOid] = group_member_oid;

    return true;
}

bool AclTable::unbind(sai_object_id_t portOid)
{
    SWSS_LOG_ENTER();

    sai_object_id_t member = ports[portOid];
    sai_status_t status = sai_acl_api->remove_acl_table_group_member(member);
    if (status != SAI_STATUS_SUCCESS) {
        SWSS_LOG_ERROR("Failed to unbind table %lu as member %lu from ACL table: %d",
                m_oid, member, status);
        return false;
    }
    return true;
}

bool AclTable::bind()
{
    SWSS_LOG_ENTER();

    for (const auto& portpair: ports)
    {
        sai_object_id_t portOid = portpair.first;
        bool suc = bind(portOid);
        if (!suc) return false;
    }
    return true;
}

bool AclTable::unbind()
{
    SWSS_LOG_ENTER();

    for (const auto& portpair: ports)
    {
        sai_object_id_t portOid = portpair.first;
        bool suc = unbind(portOid);
        if (!suc) return false;
    }
    return true;
}

void AclTable::link(sai_object_id_t portOid)
{
    SWSS_LOG_ENTER();

    ports.emplace(portOid, SAI_NULL_OBJECT_ID);
}

bool AclTable::add(shared_ptr<AclRule> newRule)
{
    SWSS_LOG_ENTER();

    string rule_id = newRule->getId();
    auto ruleIter = rules.find(rule_id);
    if (ruleIter != rules.end())
    {
        // If ACL rule already exists, delete it first
        if (ruleIter->second->remove())
        {
            rules.erase(ruleIter);
            SWSS_LOG_NOTICE("Successfully deleted ACL rule: %s", rule_id.c_str());
        }
    }

    if (newRule->create())
    {
        rules[rule_id] = newRule;
        SWSS_LOG_NOTICE("Successfully created ACL rule %s in table %s", rule_id.c_str(), id.c_str());
        return true;
    }
    else
    {
        SWSS_LOG_ERROR("Failed to create rule in table %s", id.c_str());
        return false;
    }
}

bool AclTable::remove(string rule_id)
{
    SWSS_LOG_ENTER();

    auto ruleIter = rules.find(rule_id);
    if (ruleIter != rules.end())
    {
        if (ruleIter->second->remove())
        {
            rules.erase(ruleIter);
            SWSS_LOG_NOTICE("Successfully deleted ACL rule %s", rule_id.c_str());
            return true;
        }
        else
        {
            SWSS_LOG_ERROR("Failed to delete ACL rule: %s", rule_id.c_str());
            return false;
        }
    }
    else
    {
        SWSS_LOG_WARN("Skip deleting ACL rule. Unknown rule %s", rule_id.c_str());
        return true;
    }
}

bool AclTable::clear()
{
    SWSS_LOG_ENTER();

    for (auto& rulepair: rules)
    {
        auto& rule = *rulepair.second;
        bool suc = rule.remove();
        if (!suc)
        {
            SWSS_LOG_ERROR("Failed to delete existing ACL rule %s when removing the ACL table %s", rule.getId().c_str(), id.c_str());
            return false;
        }
    }
    rules.clear();
    return true;
}

AclRuleCounters AclRuleMirror::getCounters()
{
    AclRuleCounters cnt(counters);

    if (m_state)
    {
        cnt += AclRule::getCounters();
    }

    return cnt;
}

AclRange::AclRange(sai_acl_range_type_t type, sai_object_id_t oid, int min, int max):
    m_oid(oid), m_refCnt(0), m_min(min), m_max(max), m_type(type)
{
    SWSS_LOG_ENTER();
}

AclRange *AclRange::create(sai_acl_range_type_t type, int min, int max)
{
    SWSS_LOG_ENTER();
    sai_status_t status;
    sai_object_id_t range_oid = SAI_NULL_OBJECT_ID;

    acl_range_properties_t rangeProperties = make_tuple(type, min, max);
    auto range_it = m_ranges.find(rangeProperties);
    if(range_it == m_ranges.end())
    {
        sai_attribute_t attr;
        vector<sai_attribute_t> range_attrs;

        // work around to avoid syncd termination on SAI error due to max count of ranges reached
        // can be removed when syncd start passing errors to the SAI callers
        char *platform = getenv("platform");
        if (platform && strstr(platform, MLNX_PLATFORM_SUBSTRING))
        {
            if (m_ranges.size() >= MLNX_MAX_RANGES_COUNT)
            {
                SWSS_LOG_ERROR("Maximum numbers of ACL ranges reached");
                return NULL;
            }
        }

        attr.id = SAI_ACL_RANGE_ATTR_TYPE;
        attr.value.s32 = type;
        range_attrs.push_back(attr);

        attr.id = SAI_ACL_RANGE_ATTR_LIMIT;
        attr.value.u32range.min = min;
        attr.value.u32range.max = max;
        range_attrs.push_back(attr);

        status = sai_acl_api->create_acl_range(&range_oid, gSwitchId, (uint32_t)range_attrs.size(), range_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create range object");
            return NULL;
        }

        SWSS_LOG_INFO("Created ACL Range object. Type: %d, range %d-%d, oid: %lX", type, min, max, range_oid);
        m_ranges[rangeProperties] = new AclRange(type, range_oid, min, max);

        range_it = m_ranges.find(rangeProperties);
    }
    else
    {
        SWSS_LOG_INFO("Reusing range object oid %lX ref count increased to %d", range_it->second->m_oid, range_it->second->m_refCnt);
    }

    // increase range reference count
    range_it->second->m_refCnt++;

    return range_it->second;
}

bool AclRange::remove(sai_acl_range_type_t type, int min, int max)
{
    SWSS_LOG_ENTER();

    auto range_it = m_ranges.find(make_tuple(type, min, max));

    if(range_it == m_ranges.end())
    {
        return false;
    }

    return range_it->second->remove();
}

bool AclRange::remove(sai_object_id_t *oids, int oidsCnt)
{
    SWSS_LOG_ENTER();

    for (int oidIdx = 0; oidIdx < oidsCnt; oidsCnt++)
    {
        for (auto it : m_ranges)
        {
            if (it.second->m_oid == oids[oidsCnt])
            {
                return it.second->remove();
            }
        }
    }

    return false;
}

bool AclRange::remove()
{
    SWSS_LOG_ENTER();

    if ((--m_refCnt) < 0)
    {
        throw runtime_error("Invalid ACL Range refCnt!");
    }

    if (m_refCnt == 0)
    {
        SWSS_LOG_INFO("Range object oid %lX ref count is %d, removing..", m_oid, m_refCnt);
        if (sai_acl_api->remove_acl_range(m_oid) != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to delete ACL Range object oid: %lX", m_oid);
            return false;
        }
        auto range_it = m_ranges.find(make_tuple(m_type, m_min, m_max));

        m_ranges.erase(range_it);
        delete this;
    }
    else
    {
        SWSS_LOG_INFO("Range object oid %lX ref count decreased to %d", m_oid, m_refCnt);
    }

    return true;
}

AclOrch::AclOrch(DBConnector *db, vector<string> tableNames, PortsOrch *portOrch, MirrorOrch *mirrorOrch, NeighOrch *neighOrch, RouteOrch *routeOrch) :
        Orch(db, tableNames),
        m_mirrorOrch(mirrorOrch),
        m_neighOrch(neighOrch),
        m_routeOrch(routeOrch)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attrs[2];
    attrs[0].id = SAI_SWITCH_ATTR_ACL_ENTRY_MINIMUM_PRIORITY;
    attrs[1].id = SAI_SWITCH_ATTR_ACL_ENTRY_MAXIMUM_PRIORITY;

    sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 2, attrs);
    if (status == SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("Get ACL entry priority values, min: %u, max: %u", attrs[0].value.u32, attrs[1].value.u32);
        AclRule::setRulePriorities(attrs[0].value.u32, attrs[1].value.u32);
    }
    else
    {
        SWSS_LOG_ERROR("Failed to get ACL entry priority min/max values, rv:%d", status);
        throw "AclOrch initialization failure";
    }

    m_mirrorOrch->attach(this);

    // Should be initialized last to guaranty that object is
    // initialized before thread start.
    auto interv = timespec { .tv_sec = COUNTERS_READ_INTERVAL, .tv_nsec = 0 };
    auto timer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(timer, this);
    Orch::addExecutor("", executor);
    timer->start();
}

AclOrch::~AclOrch()
{
    m_mirrorOrch->detach(this);

    m_bCollectCounters = false;
    m_sleepGuard.notify_all();
}

void AclOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    if (type != SUBJECT_TYPE_MIRROR_SESSION_CHANGE)
    {
        return;
    }

    unique_lock<mutex> lock(m_countersMutex);

    for (const auto& table : m_AclTables)
    {
        for (auto& rule : table.second.rules)
        {
            rule.second->update(type, cntx);
        }
    }
}

void AclOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->isInitDone())
    {
        return;
    }

    string table_name = consumer.getTableName();

    if (table_name == CFG_ACL_TABLE_NAME)
    {
        unique_lock<mutex> lock(m_countersMutex);
        doAclTableTask(consumer);
    }
    else if (table_name == CFG_ACL_RULE_TABLE_NAME)
    {
        unique_lock<mutex> lock(m_countersMutex);
        doAclRuleTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Invalid table %s", table_name.c_str());
    }
}

bool AclOrch::addAclTable(AclTable &newTable, string table_id)
{
    SWSS_LOG_ENTER();

    sai_object_id_t table_oid = getTableById(table_id);

    if (table_oid != SAI_NULL_OBJECT_ID)
    {
        /* If ACL table exists, remove the table first.*/
        if (!removeAclTable(table_id))
        {
            SWSS_LOG_ERROR("Fail to remove the exsiting ACL table %s when try to add the new one.", table_id.c_str());
            return false;
        }
    }

    if (createBindAclTable(newTable, table_oid))
    {
        m_AclTables[table_oid] = newTable;
        SWSS_LOG_NOTICE("Successfully created ACL table %s, oid: %lX", newTable.description.c_str(), table_oid);
        return true;
    }
    else
    {
        SWSS_LOG_ERROR("Failed to create table %s", table_id.c_str());
        return false;
    }
}

bool AclOrch::removeAclTable(string table_id)
{
    SWSS_LOG_ENTER();

    sai_object_id_t table_oid = getTableById(table_id);
    if (table_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("Skip deleting ACL table %s. Table does not exist.", table_id.c_str());
        return true;
    }

    /* If ACL rules associate with this table, remove the rules first.*/
    bool suc = m_AclTables[table_oid].clear();
    if (!suc) return false;

    if (deleteUnbindAclTable(table_oid) == SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("Successfully deleted ACL table %s", table_id.c_str());
        m_AclTables.erase(table_oid);

        sai_acl_stage_t stage = (m_AclTables[table_oid].stage == ACL_STAGE_INGRESS) ? SAI_ACL_STAGE_INGRESS : SAI_ACL_STAGE_EGRESS;
        gCrmOrch->decCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE, stage, SAI_ACL_BIND_POINT_TYPE_PORT, table_oid);

        return true;
    }
    else
    {
        SWSS_LOG_ERROR("Failed to delete ACL table %s.", table_id.c_str());
        return false;
    }
}

bool AclOrch::addAclRule(shared_ptr<AclRule> newRule, string table_id)
{
    sai_object_id_t table_oid = getTableById(table_id);
    if (table_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to add ACL rule in ACL table %s. Table doesn't exist", table_id.c_str());
        return false;
    }

    return m_AclTables[table_oid].add(newRule);
}

bool AclOrch::removeAclRule(string table_id, string rule_id)
{
    sai_object_id_t table_oid = getTableById(table_id);
    if (table_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("Skip removing rule %s from ACL table %s. Table does not exist", rule_id.c_str(), table_id.c_str());
        return true;
    }

    return m_AclTables[table_oid].remove(rule_id);
}

void AclOrch::doAclTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find('|');
        string table_id = key.substr(0, found);
        string op = kfvOp(t);

        SWSS_LOG_DEBUG("OP: %s, TABLE_ID: %s", op.c_str(), table_id.c_str());

        if (op == SET_COMMAND)
        {
            AclTable newTable;
            bool bAllAttributesOk = true;

            for (auto itp : kfvFieldsValues(t))
            {
                newTable.id = table_id;

                string attr_name = toUpper(fvField(itp));
                string attr_value = fvValue(itp);

                SWSS_LOG_DEBUG("TABLE ATTRIBUTE: %s : %s", attr_name.c_str(), attr_value.c_str());

                if (attr_name == TABLE_DESCRIPTION)
                {
                    newTable.description = attr_value;
                }
                else if (attr_name == TABLE_TYPE)
                {
                    if (!processAclTableType(attr_value, newTable.type))
                    {
                        SWSS_LOG_ERROR("Failed to process table type for table %s", table_id.c_str());
                        bAllAttributesOk = false;
                        break;
                    }
                }
                else if (attr_name == TABLE_PORTS)
                {
                    bool suc = processPorts(attr_value, [&](sai_object_id_t portOid) {
                        newTable.link(portOid);
                    });

                    if (!suc)
                    {
                        SWSS_LOG_ERROR("Failed to process table ports for table %s", table_id.c_str());
                        bAllAttributesOk = false;
                        break;
                    }
                }
                else if (attr_name == TABLE_STAGE)
                {
                   if (!processAclTableStage(attr_value, newTable.stage))
                   {
                       SWSS_LOG_ERROR("Failed to process table stage for table %s", table_id.c_str());
                       bAllAttributesOk = false;
                       break;
                   }
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown table attribute '%s'", attr_name.c_str());
                    bAllAttributesOk = false;
                    break;
                }
            }
            // validate and create ACL Table
            if (bAllAttributesOk && newTable.validate())
            {
                if (addAclTable(newTable, table_id))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
            {
                it = consumer.m_toSync.erase(it);
                SWSS_LOG_ERROR("Failed to create ACL table. Table configuration is invalid");
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (removeAclTable(table_id))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else
        {
            it = consumer.m_toSync.erase(it);
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
    }
}

void AclOrch::doAclRuleTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find('|');
        string table_id = key.substr(0, found);
        string rule_id = key.substr(found + 1);
        string op = kfvOp(t);

        SWSS_LOG_INFO("OP: %s, TABLE_ID: %s, RULE_ID: %s", op.c_str(), table_id.c_str(), rule_id.c_str());

        if (op == SET_COMMAND)
        {
            bool bAllAttributesOk = true;
            shared_ptr<AclRule> newRule;
            sai_object_id_t table_oid = getTableById(table_id);

            /* ACL table is not yet created */
            /* TODO: Remove ACL_TABLE_UNKNOWN as a table with this type cannot be successfully created */
            if (table_oid == SAI_NULL_OBJECT_ID || m_AclTables[table_oid].type == ACL_TABLE_UNKNOWN)
            {
                SWSS_LOG_INFO("Wait for ACL table %s to be created", table_id.c_str());
                it++;
                continue;
            }

            newRule = AclRule::makeShared(m_AclTables[table_oid].type, this, m_mirrorOrch, rule_id, table_id, t);

            for (const auto& itr : kfvFieldsValues(t))
            {
                string attr_name = toUpper(fvField(itr));
                string attr_value = fvValue(itr);

                SWSS_LOG_INFO("ATTRIBUTE: %s %s", attr_name.c_str(), attr_value.c_str());

                if (newRule->validateAddPriority(attr_name, attr_value))
                {
                    SWSS_LOG_INFO("Added priority attribute");
                }
                else if (newRule->validateAddMatch(attr_name, attr_value))
                {
                    SWSS_LOG_INFO("Added match attribute '%s'", attr_name.c_str());
                }
                else if (newRule->validateAddAction(attr_name, attr_value))
                {
                    SWSS_LOG_INFO("Added action attribute '%s'", attr_name.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown or invalid rule attribute '%s : %s'", attr_name.c_str(), attr_value.c_str());
                    bAllAttributesOk = false;
                    break;
                }
            }

            // validate and create ACL rule
            if (bAllAttributesOk && newRule->validate())
            {
                if(addAclRule(newRule, table_id))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
            {
                it = consumer.m_toSync.erase(it);
                SWSS_LOG_ERROR("Failed to create ACL rule. Rule configuration is invalid");
            }
        }
        else if (op == DEL_COMMAND)
        {
            if(removeAclRule(table_id, rule_id))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else
        {
            it = consumer.m_toSync.erase(it);
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
    }
}

bool AclOrch::processPorts(string portsList, std::function<void (sai_object_id_t)> inserter)
{
    SWSS_LOG_ENTER();

    vector<string> strList;

    SWSS_LOG_INFO("Processing ACL table port list %s", portsList.c_str());

    split(portsList, strList, ',');

    set<string> strSet(strList.begin(), strList.end());

    if (strList.size() != strSet.size())
    {
        SWSS_LOG_ERROR("Failed to process port list. Duplicate port entry");
        return false;
    }

    if (strList.empty())
    {
        SWSS_LOG_ERROR("Failed to process port list. List is empty");
        return false;
    }

    for (const auto& alias : strList)
    {
        Port port;
        if (!gPortsOrch->getPort(alias, port))
        {
            SWSS_LOG_ERROR("Failed to process port. Port %s doesn't exist", alias.c_str());
            return false;
        }

        switch (port.m_type)
        {
        case Port::PHY:
            if (port.m_lag_member_id != SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_ERROR("Failed to process port. Bind table to LAG member %s is not allowed", alias.c_str());
                return false;
            }
            inserter(port.m_port_id);
            break;
        case Port::LAG:
            inserter(port.m_lag_id);
            break;
        case Port::VLAN:
            inserter(port.m_vlan_info.vlan_oid);
            break;
        default:
          SWSS_LOG_ERROR("Failed to process port. Incorrect port %s type %d", alias.c_str(), port.m_type);
          return false;
      }
    }

    return true;
}

bool AclOrch::processAclTableType(string type, acl_table_type_t &table_type)
{
    SWSS_LOG_ENTER();

    auto tt = aclTableTypeLookUp.find(toUpper(type));

    if (tt == aclTableTypeLookUp.end())
    {
        return false;
    }

    table_type = tt->second;

    return true;
}

bool AclOrch::processAclTableStage(string stage, acl_stage_type_t &acl_stage)
{
    SWSS_LOG_ENTER();

    auto iter = aclStageLookUp.find(toUpper(stage));

    if (iter == aclStageLookUp.end())
    {
        acl_stage = ACL_STAGE_UNKNOWN;
        return false;
    }

    acl_stage = iter->second;

    return true;
}



sai_object_id_t AclOrch::getTableById(string table_id)
{
    SWSS_LOG_ENTER();

    for (auto it : m_AclTables)
    {
        if (it.second.id == table_id)
        {
            return it.first;
        }
    }

    return SAI_NULL_OBJECT_ID;
}

bool AclOrch::createBindAclTable(AclTable &aclTable, sai_object_id_t &table_oid)
{
    SWSS_LOG_ENTER();

    bool suc = aclTable.create();
    if (!suc) return false;

    table_oid = aclTable.getOid();
    sai_status_t status = bindAclTable(table_oid, aclTable);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to bind table %s to ports", aclTable.description.c_str());
        return false;
    }
    return true;
}

sai_status_t AclOrch::deleteUnbindAclTable(sai_object_id_t table_oid)
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    if ((status = bindAclTable(table_oid, m_AclTables[table_oid], false)) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to unbind table %s", m_AclTables[table_oid].description.c_str());
        return status;
    }

    return sai_acl_api->remove_acl_table(table_oid);
}

void AclOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    for (auto& table_it : m_AclTables)
    {
        vector<swss::FieldValueTuple> values;

        for (auto rule_it : table_it.second.rules)
        {
            AclRuleCounters cnt = rule_it.second->getCounters();

            swss::FieldValueTuple fvtp("Packets", to_string(cnt.packets));
            values.push_back(fvtp);
            swss::FieldValueTuple fvtb("Bytes", to_string(cnt.bytes));
            values.push_back(fvtb);

            AclOrch::getCountersTable().set(table_it.second.id + ":" + rule_it.second->getId(), values, "");
        }
        values.clear();
    }
}

sai_status_t AclOrch::bindAclTable(sai_object_id_t table_oid, AclTable &aclTable, bool bind)
{
    SWSS_LOG_ENTER();

    sai_status_t status = SAI_STATUS_SUCCESS;

    SWSS_LOG_INFO("%s table %s to ports", bind ? "Bind" : "Unbind", aclTable.id.c_str());

    if (aclTable.ports.empty())
    {
        if (bind)
        {
            SWSS_LOG_ERROR("Port list is not configured for %s table", aclTable.id.c_str());
            return SAI_STATUS_FAILURE;
        }
        else
        {
            return SAI_STATUS_SUCCESS;
        }
    }

    if (bind)
    {
        aclTable.bind();
    }
    else
    {
        aclTable.unbind();
    }

    return status;
}
