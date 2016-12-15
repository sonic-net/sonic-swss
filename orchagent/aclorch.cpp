#include <algorithm>
#include "aclorch.h"
#include "logger.h"
#include "schema.h"
#include "ipprefix.h"

#undef SWSS_LOG_DEBUG
#define SWSS_LOG_DEBUG SWSS_LOG_ERROR

std::mutex AclOrch::m_countersMutex;
std::condition_variable AclOrch::m_sleepGuard;
bool AclOrch::m_bCollectCounters = true;

extern sai_acl_api_t*    sai_acl_api;
extern sai_port_api_t*   sai_port_api;
extern sai_switch_api_t* sai_switch_api;

acl_rule_attr_lookup_t aclMatchLookup =
{
    { MATCH_SRC_IP,            SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP },
    { MATCH_DST_IP,            SAI_ACL_ENTRY_ATTR_FIELD_DST_IP },
    { MATCH_L4_SRC_PORT,       SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT },
    { MATCH_L4_DST_PORT,       SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT },
    { MATCH_ETHER_TYPE,        SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE },
    { MATCH_IP_PROTOCOL,       SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL },
    { MATCH_TCP_FLAGS,         SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS },
    { MATCH_IP_TYPE,           SAI_ACL_ENTRY_ATTR_FIELD_IP_TYPE },
    { MATCH_DSCP,              SAI_ACL_ENTRY_ATTR_FIELD_DSCP },
    { MATCH_L4_SRC_PORT_RANGE, SAI_ACL_ENTRY_ATTR_FIELD_RANGE },
    { MATCH_L4_DST_PORT_RANGE, SAI_ACL_ENTRY_ATTR_FIELD_RANGE },
};

acl_rule_attr_lookup_t aclActionLookup =
{
    { ACTION_PACKET_ACTION, SAI_ACL_ENTRY_ATTR_PACKET_ACTION },
    { ACTION_MIRROR,        SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS }
};

static acl_table_type_lookup_t aclTableTypeLookUp =
{
    { TABLE_TYPE_L3,     ACL_TABLE_L3 },
    { TABLE_TYPE_MIRROR, ACL_TABLE_MIRROR }
};

static acl_ip_type_lookup_t aclIpTypeLookup =
{
    { IP_TYPE_ANY,         SAI_ACL_IP_TYPE_ANY },
    { IP_TYPE_IP,          SAI_ACL_IP_TYPE_IP },
    { IP_TYPE_NON_IP,      SAI_ACL_IP_TYPE_NON_IP },
    { IP_TYPE_IPv4ANY,     SAI_ACL_IP_TYPE_IPv4ANY },
    { IP_TYPE_NON_IPv4,    SAI_ACL_IP_TYPE_NON_IPv4 },
    { IP_TYPE_IPv6ANY,     SAI_ACL_IP_TYPE_IPv6ANY },
    { IP_TYPE_NON_IPv6,    SAI_ACL_IP_TYPE_NON_IPv6 },
    { IP_TYPE_ARP,         SAI_ACL_IP_TYPE_ARP },
    { IP_TYPE_ARP_REQUEST, SAI_ACL_IP_TYPE_ARP_REQUEST },
    { IP_TYPE_ARP_REPLY,   SAI_ACL_IP_TYPE_ARP_REPLY }
};

AclOrch::AclOrch(DBConnector *db, vector<string> tableNames, PortsOrch *portOrch) :
        Orch(db, tableNames),
        std::thread(AclOrch::collectCountersThread, this),
        m_portOrch(portOrch)
{
    SWSS_LOG_ENTER();
}

AclOrch::~AclOrch()
{
    SWSS_LOG_ENTER();

    m_bCollectCounters = false;
    m_sleepGuard.notify_all();
    join();
}

void AclOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.m_consumer->getTableName();

    if (table_name == APP_ACL_TABLE_NAME)
    {
        doAclTableTask(consumer);
    }
    else if (table_name == APP_ACL_RULE_TABLE_NAME)
    {
        doAclRuleTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Invalid table %s\n", table_name.c_str());
    }
}

void AclOrch::doAclTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(':');
        string table_id = key.substr(0, found);
        string op = kfvOp(t);

        SWSS_LOG_DEBUG("OP: %s, TABLE_ID: %s\n", op.c_str(), table_id.c_str());

        if (op == SET_COMMAND)
        {
            AclTable newTable;
            bool bAllAttributesOk = true;

            for (auto itp : kfvFieldsValues(t))
            {
                newTable.id = table_id;

                string attr_name = toUpper(fvField(itp));
                string attr_value = fvValue(itp);

                SWSS_LOG_DEBUG("TABLE ATTRIBUTE: %s : %s\n", attr_name.c_str(), attr_value.c_str());

                if (attr_name == TABLE_DESCRIPTION)
                {
                    newTable.description = attr_value;
                }
                else if (attr_name == TABLE_TYPE)
                {
                    if (!processAclTableType(attr_value, newTable.type))
                    {
                        SWSS_LOG_ERROR("Failed to process table type for table %s\n", table_id.c_str());
                    }
                }
                else if (attr_name == TABLE_PORTS)
                {
                    if (!processPorts(attr_value, newTable.ports))
                    {
                        SWSS_LOG_ERROR("Failed to process table ports for table %s\n", table_id.c_str());
                    }
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown table attribute '%s'\n", attr_name.c_str());
                    bAllAttributesOk = false;
                    break;
                }
            }
            // validate and create ACL Table
            if (bAllAttributesOk && validateAclTable(newTable))
            {
                sai_object_id_t table_oid = getTableById(table_id);

                if (table_oid != SAI_NULL_OBJECT_ID)
                {
                    // table already exists, delete it first
                    if (deleteUnbindAclTable(table_oid) == SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_INFO("Successfully deleted ACL table %s\n", table_id.c_str());
                        m_AclTables.erase(table_oid);
                    }
                }
                if (createBindAclTable(newTable, table_oid) == SAI_STATUS_SUCCESS)
                {
                    m_AclTables[table_oid] = newTable;
                    SWSS_LOG_INFO("Successfully created ACL table %s, oid: %lX\n", newTable.description.c_str(), table_oid);
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to create table %s\n", table_id.c_str());
                }
            }
            else
            {
                SWSS_LOG_ERROR("Failed to create ACL table. Table configuration is invalid\n");
            }
        }
        else if (op == DEL_COMMAND)
        {
            sai_object_id_t table_oid = getTableById(table_id);
            if (table_oid != SAI_NULL_OBJECT_ID)
            {
                if (deleteUnbindAclTable(table_oid) == SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_INFO("Successfully deleted ACL table %s\n", table_id.c_str());
                    m_AclTables.erase(table_oid);
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to delete ACL table %s\n", table_id.c_str());
                }
            }
            else
            {
                SWSS_LOG_ERROR("Failed to delete ACL table. Table %s does not exist\n", table_id.c_str());
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
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
        size_t found = key.find(':');
        string table_id = key.substr(0, found);
        string rule_id = key.substr(found + 1);
        string op = kfvOp(t);

        SWSS_LOG_DEBUG("OP: %s, TABLE_ID: %s, RULE_ID: %s\n", op.c_str(), table_id.c_str(), rule_id.c_str());

        if (op == SET_COMMAND)
        {
            AclRule newRule;
            bool bAllAttributesOk = true;

            for (auto itr : kfvFieldsValues(t))
            {
                newRule.id = rule_id;
                newRule.table_id = table_id;

                string attr_name = toUpper(fvField(itr));
                string attr_value = fvValue(itr);

                SWSS_LOG_DEBUG("ATTRIBUTE: %s %s\n", attr_name.c_str(), attr_value.c_str());

                if (validateAddPriority(newRule, attr_name, attr_value))
                {
                    SWSS_LOG_INFO("Added priority attribute\n");
                }
                else if (validateAddMatch(newRule, attr_name, attr_value))
                {
                    SWSS_LOG_INFO("Added match attribute '%s'\n", attr_name.c_str());
                }
                else if (validateAddAction(newRule, attr_name, attr_value))
                {
                    SWSS_LOG_INFO("Added action attribute '%s'\n", attr_name.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown or invalid rule attribute '%s : %s'\n", attr_name.c_str(), attr_value.c_str());
                    bAllAttributesOk = false;
                    break;
                }
            }
            // validate and create ACL rule
            if (bAllAttributesOk && validateAclRule(newRule))
            {
                sai_object_id_t table_oid = getTableById(table_id);
                sai_object_id_t rule_oid = getRuleById(table_id, rule_id);
                if (rule_oid != SAI_NULL_OBJECT_ID)
                {
                    // rule already exists - delete it first
                    if (deleteAclRule(m_AclTables[table_oid].rules[rule_oid]) == SAI_STATUS_SUCCESS)
                    {
                        m_AclTables[table_oid].rules.erase(rule_oid);
                        SWSS_LOG_INFO("Successfully deleted ACL rule: %s\n", rule_id.c_str());
                    }
                }
                if (createAclRule(newRule, rule_oid) == SAI_STATUS_SUCCESS)
                {
                    m_AclTables[table_oid].rules[rule_oid] = newRule;
                    SWSS_LOG_INFO("Successfully created ACL rule %s in table %s\n", rule_id.c_str(), table_id.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to create rule in table %s\n", table_id.c_str());
                }
            }
            else
            {
                SWSS_LOG_ERROR("Failed to create ACL rule. Rule configuration is invalid\n");
            }
        }
        else if (op == DEL_COMMAND)
        {
            sai_object_id_t table_oid = getTableById(table_id);
            if (table_oid != SAI_NULL_OBJECT_ID)
            {
                sai_object_id_t rule_oid = getRuleById(table_id, rule_id);
                if (rule_oid != SAI_NULL_OBJECT_ID)
                {
                    if (deleteAclRule(m_AclTables[table_oid].rules[rule_oid]) == SAI_STATUS_SUCCESS)
                    {
                        m_AclTables[table_oid].rules.erase(rule_oid);
                        SWSS_LOG_INFO("Successfully deleted ACL rule %s\n", rule_id.c_str());
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to delete ACL rule: %s\n", table_id.c_str());
                    }
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to delete ACL rule. Unknown rule %s\n", table_id.c_str());
                }
            }
            else
            {
                SWSS_LOG_ERROR("Failed to delete rule %s from ACL table %s. Table does not exist\n", rule_id.c_str(), table_id.c_str());
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }
}

bool AclOrch::processPorts(string portsList, ports_list_t& out)
{
    SWSS_LOG_ENTER();

    vector<string> strList;

    SWSS_LOG_INFO("Processing ACL table port list %s\n", portsList.c_str());

    split(portsList, strList, ',');

    set<string> strSet(strList.begin(), strList.end());

    if (strList.size() != strSet.size())
    {
        SWSS_LOG_ERROR("Failed to process port list. Duplicate port entry.\n");
        return false;
    }

    if (strList.empty())
    {
        SWSS_LOG_ERROR("Failed to process port list. List is empty.\n");
        return false;
    }

    for (const auto& alias : strList)
    {
        Port port;
        if (!m_portOrch->getPort(alias, port))
        {
            SWSS_LOG_ERROR("Failed to process port. Port %s doesn't exist.\n", alias.c_str());
            return false;
        }

        if (port.m_type != Port::PHY)
        {
            SWSS_LOG_ERROR("Failed to process port. Incorrect port %s type %d\n", alias.c_str(), port.m_type);
            return false;
        }

        out.push_back(port.m_port_id);
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

bool AclOrch::processIpType(string type, sai_uint32_t &ip_type)
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

sai_object_id_t AclOrch::getRuleById(string table_id, string rule_id)
{
    SWSS_LOG_ENTER();

    sai_object_id_t table_oid = getTableById(table_id);

    if (table_oid == SAI_NULL_OBJECT_ID)
    {
        return SAI_NULL_OBJECT_ID;
    }

    for (auto it : m_AclTables[table_oid].rules)
    {
        if (it.second.id == rule_id)
            return it.first;
    }

    return SAI_NULL_OBJECT_ID;
}

bool AclOrch::validateAddPriority(AclRule &aclRule, string attr_name, string attr_value)
{
    bool status = false;

    if (attr_name == RULE_PRIORITY)
    {
        sai_attribute_t attrs[] =
        {
            { SAI_SWITCH_ATTR_ACL_ENTRY_MINIMUM_PRIORITY },
            { SAI_SWITCH_ATTR_ACL_ENTRY_MAXIMUM_PRIORITY }
        };
        // get min/max allowed priority
        if (sai_switch_api->get_switch_attribute(sizeof(attrs)/sizeof(attrs[0]), attrs) == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_DEBUG("ACL entry priority min/max: %d/%d\n", attrs[0].value.u32, attrs[1].value.u32);
            char *endp = NULL;
            errno = 0;
            aclRule.priority = strtol(attr_value.c_str(), &endp, 0);
            // chack conversion was successfull and the value is within the allowed range
            status = (errno == 0) &&
                     (endp == attr_value.c_str() + attr_value.size()) &&
                     (aclRule.priority >= attrs[0].value.u32) &&
                     (aclRule.priority <= attrs[1].value.u32);
        }
        else
        {
            SWSS_LOG_ERROR("Failed to get ACL entry priority min/max values\n");
        }
    }

    return status;
}

bool AclOrch::validateAddMatch(AclRule &aclRule, string attr_name, string attr_value)
{
    SWSS_LOG_ENTER();

    sai_attribute_value_t value;

    if (aclMatchLookup.find(attr_name) == aclMatchLookup.end())
    {
        return false;
    }
    else if(attr_name == MATCH_IP_TYPE)
    {
        if (!processIpType(attr_value, value.aclfield.data.u32))
        {
            SWSS_LOG_DEBUG("Invalid IP type %s\n", attr_value.c_str());
            return false;
        }

        value.aclfield.enable = true;
        value.aclfield.mask.u32 = 0xFFFFFFFF;
    }
    else if((attr_name == MATCH_ETHER_TYPE)  || (attr_name == MATCH_DSCP)        ||
            (attr_name == MATCH_L4_SRC_PORT) || (attr_name == MATCH_L4_DST_PORT) ||
            (attr_name == MATCH_IP_PROTOCOL) || (attr_name == MATCH_TCP_FLAGS))
    {
        char *endp = NULL;
        errno = 0;
        value.aclfield.data.u32 = strtol(attr_value.c_str(), &endp, 0);
        if (errno || (endp != attr_value.c_str() + attr_value.size()))
        {
            SWSS_LOG_DEBUG("Attr: %s, val: %s(=%d), err=%d\n", attr_name.c_str(), attr_value.c_str(), value.aclfield.data.u32, errno);
            return false;
        }

        value.aclfield.enable = true;
        value.aclfield.mask.u32 = 0xFFFFFFFF;
    }
    else if (attr_name == MATCH_SRC_IP || attr_name == MATCH_DST_IP)
    {
        try
        {
            IpPrefix ip(attr_value);

            if (ip.isV4())
            {
                value.aclfield.data.ip4 = ip.getIp().getV4Addr();
                value.aclfield.mask.ip4 = ip.getMask().getV4Addr();
            }
            else
            {
                memcpy(value.aclfield.data.ip6, ip.getIp().getV6Addr(), 16);
                memcpy(value.aclfield.mask.ip6, ip.getMask().getV6Addr(), 16);
            }
        }
        catch(...)
        {
            SWSS_LOG_DEBUG("Attr: %s, val: %s IpPrefix exception...\n", attr_name.c_str(), attr_value.c_str());
            return false;
        }
    }

    aclRule.matches[aclMatchLookup[attr_name]] = value;

    return true;
}

bool AclOrch::validateAddAction(AclRule &aclRule, string attr_name, string _attr_value)
{
    SWSS_LOG_ENTER();

    string attr_value = toUpper(_attr_value);
    sai_attribute_value_t value;

    if (aclActionLookup.find(attr_name) == aclActionLookup.end())
        return false;

    if (attr_name == ACTION_PACKET_ACTION)
    {
        if (attr_value == PACKET_ACTION_FORWARD)
        {
            value.aclaction.parameter.s32 = SAI_PACKET_ACTION_FORWARD;
        }
        else if (attr_value == PACKET_ACTION_DROP)
        {
            value.aclaction.parameter.s32 = SAI_PACKET_ACTION_DROP;
        }
        else
        {
            return false;
        }
        value.aclaction.enable = true;
    }
    else if (attr_name == ACTION_MIRROR)
    {
        // parse 'attr_value', fill in 'value'
        return false;
    }

    aclRule.actions[aclActionLookup[attr_name]] = value;

    return true;
}

bool AclOrch::validateAclTable(AclTable &aclTable)
{
    SWSS_LOG_ENTER();

    if (aclTable.type == ACL_TABLE_UNKNOWN || aclTable.ports.size() == 0)
    {
        return false;
    }

    return true;
}

bool AclOrch::validateAclRule(AclRule &aclRule)
{
    SWSS_LOG_ENTER();

    if (getTableById(aclRule.table_id) == SAI_NULL_OBJECT_ID ||
        aclRule.matches.size() == 0 || aclRule.actions.size() != 1)
    {
        return false;
    }

    return true;
}

sai_status_t AclOrch::createBindAclTable(AclTable &aclTable, sai_object_id_t &table_oid)
{
    SWSS_LOG_ENTER();

    std::unique_lock<std::mutex> lock(m_countersMutex);

    sai_status_t status;
    sai_attribute_t attr;
    vector<sai_attribute_t> table_attrs;

    attr.id =  SAI_ACL_TABLE_ATTR_STAGE;
    attr.value.s32 = SAI_ACL_STAGE_INGRESS;
    table_attrs.push_back(attr);

    attr.id =  SAI_ACL_TABLE_ATTR_PRIORITY;
    attr.value.u32 = DEFAULT_TABLE_PRIORITY;
    table_attrs.push_back(attr);

    attr.id =  SAI_ACL_TABLE_ATTR_SIZE;
    attr.value.u32 = 0;
    table_attrs.push_back(attr);

    attr.id =  SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id =  SAI_ACL_TABLE_ATTR_FIELD_IP_TYPE;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id =  SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id =  SAI_ACL_TABLE_ATTR_FIELD_SRC_IP;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id =  SAI_ACL_TABLE_ATTR_FIELD_DST_IP;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id =  SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id =  SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id =  SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    status = sai_acl_api->create_acl_table(&table_oid, table_attrs.size(), table_attrs.data());

    if (status == SAI_STATUS_SUCCESS)
    {
        if ((status = bindAclTable(table_oid, aclTable)) != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to bind table %s to ports\n", aclTable.description.c_str());
        }
    }

    return status;
}

sai_status_t AclOrch::createAclRule(AclRule &aclRule, sai_object_id_t &rule_oid)
{
    SWSS_LOG_ENTER();

    std::unique_lock<std::mutex> lock(m_countersMutex);

    sai_object_id_t table_oid = getTableById(aclRule.table_id);
    vector<sai_attribute_t> rule_attrs;
    sai_object_id_t counter_oid;
    sai_attribute_t attr;
    sai_status_t status;

    if ((status = createAclCounter(counter_oid, table_oid)) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create counter for the rule %s in table %s\n",
                       aclRule.id.c_str(), aclRule.table_id.c_str());
        return status;
    }
    else
    {
        SWSS_LOG_DEBUG("Created counter for the rule %s in table %s\n", aclRule.id.c_str(), aclRule.table_id.c_str());
    }

    aclRule.counter_oid = counter_oid;

    // store table oid this rule belongs to
    attr.id =  SAI_ACL_ENTRY_ATTR_TABLE_ID;
    attr.value.oid = table_oid;
    rule_attrs.push_back(attr);

    attr.id =  SAI_ACL_ENTRY_ATTR_PRIORITY;
    attr.value.u32 = aclRule.priority;
    rule_attrs.push_back(attr);

    attr.id =  SAI_ACL_ENTRY_ATTR_ADMIN_STATE;
    attr.value.booldata = true;
    rule_attrs.push_back(attr);

    // add reference to the counter
    attr.id =  SAI_ACL_ENTRY_ATTR_ACTION_COUNTER;
    attr.value.aclaction.parameter.oid = aclRule.counter_oid;
    attr.value.aclaction.enable = true;
    rule_attrs.push_back(attr);

    // store matches
    for (auto it : aclRule.matches)
    {
        attr.id = it.first;
        attr.value = it.second;
        rule_attrs.push_back(attr);
    }

    // store actions
    for (auto it : aclRule.actions)
    {
        attr.id = it.first;
        attr.value = it.second;
        rule_attrs.push_back(attr);
    }

    return sai_acl_api->create_acl_entry(&rule_oid, rule_attrs.size(), rule_attrs.data());
}

sai_status_t AclOrch::createAclCounter(sai_object_id_t &counter_oid, sai_object_id_t table_oid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> counter_attrs;

    attr.id =  SAI_ACL_COUNTER_ATTR_TABLE_ID;
    attr.value.oid = table_oid;
    counter_attrs.push_back(attr);

    attr.id =  SAI_ACL_COUNTER_ATTR_ENABLE_BYTE_COUNT;
    attr.value.booldata = true;
    counter_attrs.push_back(attr);

    attr.id =  SAI_ACL_COUNTER_ATTR_ENABLE_PACKET_COUNT;
    attr.value.booldata = true;
    counter_attrs.push_back(attr);

    return sai_acl_api->create_acl_counter(&counter_oid, counter_attrs.size(), counter_attrs.data());
}

sai_status_t AclOrch::deleteUnbindAclTable(sai_object_id_t table_oid)
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    std::unique_lock<std::mutex> lock(m_countersMutex);

    if ((status = bindAclTable(table_oid, m_AclTables[table_oid], false)) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to unbind table %s\n", m_AclTables[table_oid].description.c_str());
        return status;

    }

    return sai_acl_api->delete_acl_table(table_oid);
}

sai_status_t AclOrch::deleteAclRule(AclRule &aclRule)
{
    SWSS_LOG_ENTER();
    sai_object_id_t rule_oid;
    sai_status_t status;

    std::unique_lock<std::mutex> lock(m_countersMutex);

    if ((rule_oid = getRuleById(aclRule.table_id, aclRule.id)) == SAI_NULL_OBJECT_ID)
    {
        return SAI_STATUS_FAILURE;
    }

    if ((status = sai_acl_api->delete_acl_entry(rule_oid)) != SAI_STATUS_SUCCESS)
    {
        return status;
    }

    return deleteAclCounter(aclRule.counter_oid);
}

sai_status_t AclOrch::deleteAclCounter(sai_object_id_t counter_oid)
{
    SWSS_LOG_ENTER();

    return sai_acl_api->delete_acl_counter(counter_oid);
}

void AclOrch::collectCountersThread(AclOrch* pAclOrch)
{
    SWSS_LOG_ENTER();
    sai_attribute_t counter_attr[2];
    counter_attr[0].id = SAI_ACL_COUNTER_ATTR_PACKETS;
    counter_attr[1].id = SAI_ACL_COUNTER_ATTR_BYTES;

    swss::DBConnector db(COUNTERS_DB, "localhost", 6379, 0);
    swss::Table countersTable(&db, "COUNTERS");

    while(m_bCollectCounters)
    {
        std::unique_lock<std::mutex> lock(m_countersMutex);

        for (auto itt : pAclOrch->m_AclTables)
        {

            std::vector<swss::FieldValueTuple> values;

            for (auto itr : itt.second.rules)
            {
                sai_acl_api->get_acl_counter_attribute(itr.second.counter_oid, 2, counter_attr);

                swss::FieldValueTuple fvtp("Packets", std::to_string(counter_attr[0].value.u64));
                values.push_back(fvtp);
                swss::FieldValueTuple fvtb("Bytes", std::to_string(counter_attr[1].value.u64));
                values.push_back(fvtb);

                SWSS_LOG_DEBUG("Counter %lX, value %ld/%ld\n", itr.second.counter_oid, counter_attr[0].value.u64, counter_attr[1].value.u64);
                countersTable.set(itt.second.id + ":" + itr.second.id, values, "");
            }
            values.clear();
        }

        m_sleepGuard.wait_for(lock, std::chrono::seconds(COUNTERS_READ_INTERVAL));
    }

}

sai_status_t AclOrch::bindAclTable(sai_object_id_t table_oid, AclTable &aclTable, bool bind)
{
    SWSS_LOG_ENTER();

    sai_status_t status = SAI_STATUS_SUCCESS;

    SWSS_LOG_INFO("%s table to ports\n", bind ? "Bind" : "Unbind", aclTable.id.c_str());

    if (aclTable.ports.empty())
    {
        if (bind)
        {
            SWSS_LOG_ERROR("Port list is not configured for %s table\n", aclTable.id.c_str());
            return SAI_STATUS_FAILURE;
        }
        else
        {
            return SAI_STATUS_SUCCESS;
        }
    }

    for (const auto& portOid : aclTable.ports)
    {
        auto& portAcls = m_portBind[portOid];

        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_INGRESS_ACL_LIST;

        if (bind)
        {
            portAcls.push_back(table_oid);
        }
        else
        {
            auto iter = portAcls.begin();
            while (iter != portAcls.end())
            {
                if (*iter == table_oid)
                {
                    portAcls.erase(iter);
                    break;
                }
            }
        }

        attr.value.objlist.list = portAcls.data();
        attr.value.objlist.count = portAcls.size();

        status = sai_port_api->set_port_attribute(portOid, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to %s ACL table %s %s port %lu\n",
                           bind ? "bind" : "unbind", aclTable.id.c_str(),
                           bind ? "to" : "from",
                           portOid);
            return status;
        }
    }

    return status;
}

string AclOrch::toUpper(string str)
{
    string uppercase = str;

    transform(uppercase.begin(), uppercase.end(), uppercase.begin(), ::toupper);

    return uppercase;
}
