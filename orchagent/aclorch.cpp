#include <algorithm>
#include "aclorch.h"
#include "logger.h"
#include "schema.h"
#include "ipprefix.h"

extern sai_acl_api_t*   sai_acl_api;
extern sai_port_api_t*  sai_port_api;

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

AclOrch::AclOrch(DBConnector *db, vector<string> tableNames, PortsOrch *portOrch) :
        Orch(db, tableNames),
        m_portOrch(portOrch)
{
    SWSS_LOG_ENTER();
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
                    if (!processToAclTableType(attr_value, newTable.type))
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
                }
            }
            // validate and create ACL Table
            if (validateAclTable(newTable))
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
            if (table_id == "*")
            {
                deleteAllAclObjects();
            }
            else
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

            for (auto itr : kfvFieldsValues(t))
            {
                newRule.id = rule_id;
                newRule.table_id = table_id;

                string attr_name = toUpper(fvField(itr));
                string attr_value = fvValue(itr);

                SWSS_LOG_DEBUG("RULE: %s %s\n", attr_name.c_str(), attr_value.c_str());

                if (attr_name == RULE_PRIORITY)
                {
                    newRule.priority = std::stoi(attr_value);
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
                    break;
                }
            }
            // validate and create ACL rule
            if (validateAclRule(newRule))
            {
                sai_object_id_t table_oid = getTableById(table_id);
                sai_object_id_t rule_oid = getRuleById(table_id, rule_id);
                if (rule_oid != SAI_NULL_OBJECT_ID)
                {
                    // rule already exists - delete it first
                    if (deleteAclRule(rule_oid) == SAI_STATUS_SUCCESS)
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
            // TODO delete behavior?
            sai_object_id_t table_oid = getTableById(table_id);
            if (table_oid != SAI_NULL_OBJECT_ID)
            {
                sai_object_id_t rule_oid = getRuleById(table_id, rule_id);
                if (rule_oid != SAI_NULL_OBJECT_ID)
                {
                    if (deleteAclRule(rule_oid) == SAI_STATUS_SUCCESS)
                    {
                        m_AclTables[table_oid].rules.erase(rule_oid);
                        SWSS_LOG_INFO("Successfully deleted ACL rule %s\n", rule_id.c_str());
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to delete ACL rule %s\n", table_id.c_str());
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

bool AclOrch::processToAclTableType(string _type, acl_table_type_t &acl_type)
{
    SWSS_LOG_ENTER();

    string type = toUpper(_type);

    // TODO combine if=assign
    if (aclTableTypeLookUp.find(type) == aclTableTypeLookUp.end())
    {
        acl_type = ACL_TABLE_UNKNOWN;
        return false;
    }
    else
    {
        acl_type = aclTableTypeLookUp[type];
        return true;
    }
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

bool AclOrch::validateAddMatch(AclRule &aclRule, string attr_name, string attr_value)
{
    SWSS_LOG_ENTER();

    sai_attribute_value_t value;

    if (aclMatchLookup.find(attr_name) == aclMatchLookup.end())
    {
        return false;
    }
    else if((attr_name == MATCH_ETHER_TYPE)  || (attr_name == MATCH_IP_TYPE)     ||
            (attr_name == MATCH_L4_SRC_PORT) || (attr_name == MATCH_L4_DST_PORT) ||
            (attr_name == MATCH_IP_PROTOCOL) || (attr_name == MATCH_TCP_FLAGS)   ||
            (attr_name == MATCH_DSCP))
    {
        value.aclfield.data.u32 = std::stoi(attr_value);  // TODO can throw? // u32 - safe?
        value.aclfield.enable = true;
        value.aclfield.mask.u32 = 0xFFFFFFFF;
    }
    else if (attr_name == MATCH_SRC_IP || attr_name == MATCH_DST_IP)
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

    sai_object_id_t table_oid = getTableById(aclRule.table_id);
    sai_attribute_t entry_attrs[MAX_RULE_ATTRIBUTES];
    uint32_t attrs_num = 0;

    // store table oid this rule belongs to
    entry_attrs[attrs_num].id = SAI_ACL_ENTRY_ATTR_TABLE_ID;
    entry_attrs[attrs_num++].value.oid = table_oid;

    entry_attrs[attrs_num].id = SAI_ACL_ENTRY_ATTR_PRIORITY;
    entry_attrs[attrs_num++].value.u32 = aclRule.priority;

    entry_attrs[attrs_num].id = SAI_ACL_ENTRY_ATTR_ADMIN_STATE;
    entry_attrs[attrs_num++].value.booldata = true;

    SWSS_LOG_DEBUG("Creating rule in table %s\n", aclRule.table_id.c_str());

    // store matches
    for (auto it : aclRule.matches)
    {
        entry_attrs[attrs_num].id = it.first;
        entry_attrs[attrs_num++].value = it.second;
    }
    // store actions
    for (auto it : aclRule.actions)
    {
        entry_attrs[attrs_num].id = it.first;
        entry_attrs[attrs_num++].value = it.second;
    }

    return sai_acl_api->create_acl_entry(&rule_oid, attrs_num, entry_attrs);
}

sai_status_t AclOrch::deleteUnbindAclTable(sai_object_id_t table_oid)
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    if ((status = bindAclTable(table_oid, m_AclTables[table_oid], false)) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to unbind table %s\n", m_AclTables[table_oid].description.c_str());
        return status;

    }

    return sai_acl_api->delete_acl_table(table_oid);
}

sai_status_t AclOrch::deleteAclRule(sai_object_id_t rule_oid)
{
    SWSS_LOG_ENTER();

    return sai_acl_api->delete_acl_entry(rule_oid);
}

// TODO for debug purposes
sai_status_t AclOrch::deleteAllAclObjects()
{
    SWSS_LOG_ENTER();

    auto it = m_AclTables.begin();
    while (it != m_AclTables.end())
    {
        auto it2 = it->second.rules.begin();
        while (it2 != it->second.rules.end())
        {
            if (deleteAclRule(it2->first) == SAI_STATUS_SUCCESS)
            {
                it2 = it->second.rules.erase(it2);
                SWSS_LOG_INFO("Successfully deleted ACL rule %s in table %s\n", it2->second.id.c_str(), it->second.id.c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Failed to delete ACL rule %s in table %s\n", it2->second.id.c_str(), it->second.id.c_str());
                break;
            }
        }
        if (deleteUnbindAclTable(it->first) == SAI_STATUS_SUCCESS) {
            SWSS_LOG_INFO("Successfully deleted ACL table %s\n", it->second.id.c_str());
            it = m_AclTables.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Failed to delete ACL table %s\n", it->second.id.c_str());
            return SAI_STATUS_FAILURE;
            break;
        }
    }

    return SAI_STATUS_SUCCESS;
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
