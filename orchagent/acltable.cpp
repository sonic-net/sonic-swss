#include "logger.h"

#include "port.h"
#include "acltable.h"
#include "aclorch.h"
#include "crmorch.h"

using namespace std;

extern sai_acl_api_t*    sai_acl_api;
extern sai_object_id_t   gSwitchId;
extern PortsOrch*        gPortsOrch;
extern CrmOrch*          gCrmOrch;

bool AclTable::validate()
{
    if (type == ACL_TABLE_CTRLPLANE)
        return true;

    if (type == ACL_TABLE_UNKNOWN || stage == ACL_STAGE_UNKNOWN)
        return false;

    if (portSet.empty() && pendingPortSet.empty())
        return false;

    return true;
}

bool AclTable::create()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> table_attrs;
    vector<int32_t> bpoint_list;

    // PFC watch dog ACLs are only applied to port
    if (type == ACL_TABLE_PFCWD)
    {
        bpoint_list = { SAI_ACL_BIND_POINT_TYPE_PORT };
    }
    else
    {
        bpoint_list = { SAI_ACL_BIND_POINT_TYPE_PORT, SAI_ACL_BIND_POINT_TYPE_LAG };
    }

    attr.id = SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST;
    attr.value.s32list.count = static_cast<uint32_t>(bpoint_list.size());
    attr.value.s32list.list = bpoint_list.data();
    table_attrs.push_back(attr);

    if (type == ACL_TABLE_PFCWD)
    {
        attr.id = SAI_ACL_TABLE_ATTR_FIELD_TC;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_ACL_STAGE;
        attr.value.s32 = stage == ACL_STAGE_INGRESS ? SAI_ACL_STAGE_INGRESS : SAI_ACL_STAGE_EGRESS;
        table_attrs.push_back(attr);

        sai_status_t status = sai_acl_api->create_acl_table(&m_oid, gSwitchId, (uint32_t)table_attrs.size(), table_attrs.data());

        if (status == SAI_STATUS_SUCCESS)
        {
            gCrmOrch->incCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE, (sai_acl_stage_t) attr.value.s32, SAI_ACL_BIND_POINT_TYPE_PORT);
        }

        return status == SAI_STATUS_SUCCESS;
    }

    if (type == ACL_TABLE_MIRROR_DSCP)
    {
        attr.id = SAI_ACL_TABLE_ATTR_FIELD_DSCP;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_ACL_STAGE;
        attr.value.s32 = stage == ACL_STAGE_INGRESS
            ? SAI_ACL_STAGE_INGRESS : SAI_ACL_STAGE_EGRESS;
        table_attrs.push_back(attr);

        sai_status_t status = sai_acl_api->create_acl_table(
                &m_oid, gSwitchId, (uint32_t)table_attrs.size(), table_attrs.data());

        if (status == SAI_STATUS_SUCCESS)
        {
            gCrmOrch->incCrmAclUsedCounter(
                    CrmResourceType::CRM_ACL_TABLE, (sai_acl_stage_t)attr.value.s32, SAI_ACL_BIND_POINT_TYPE_PORT);
        }

        return status == SAI_STATUS_SUCCESS;
    }

    if (type != ACL_TABLE_MIRRORV6)
    {
        attr.id = SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE;
        attr.value.booldata = true;
        table_attrs.push_back(attr);
    }

    attr.id = SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    attr.id = SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL;
    attr.value.booldata = true;
    table_attrs.push_back(attr);

    /*
     * Type of Tables and Supported Match Types (ASIC database)
     * |------------------------------------------------------------------|
     * |                   | TABLE_MIRROR | TABLE_MIRROR | TABLE_MIRRORV6 |
     * |    Match Type     |----------------------------------------------|
     * |                   |   combined   |          separated            |
     * |------------------------------------------------------------------|
     * | MATCH_SRC_IP      |      √       |      √       |                |
     * | MATCH_DST_IP      |      √       |      √       |                |
     * |------------------------------------------------------------------|
     * | MATCH_ICMP_TYPE   |      √       |      √       |                |
     * | MATCH_ICMP_CODE   |      √       |      √       |                |
     * |------------------------------------------------------------------|
     * | MATCH_SRC_IPV6    |      √       |              |       √        |
     * | MATCH_DST_IPV6    |      √       |              |       √        |
     * |------------------------------------------------------------------|
     * | MATCH_ICMPV6_TYPE |      √       |             |        √        |
     * | MATCH_ICMPV6_CODE |      √       |             |        √        |
     * |------------------------------------------------------------------|
     * | MARTCH_ETHERTYPE  |      √       |      √       |                |
     * |------------------------------------------------------------------|
     */

    if (type == ACL_TABLE_MIRROR)
    {
        attr.id = SAI_ACL_TABLE_ATTR_FIELD_SRC_IP;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_FIELD_DST_IP;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        // If the switch supports v6 and requires one single table
        if (m_pAclOrch->m_mirrorTableCapabilities[ACL_TABLE_MIRRORV6] &&
                m_pAclOrch->m_isCombinedMirrorV6Table)
        {
            attr.id = SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6;
            attr.value.booldata = true;
            table_attrs.push_back(attr);

            attr.id = SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6;
            attr.value.booldata = true;
            table_attrs.push_back(attr);

            attr.id = SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE;
            attr.value.booldata = true;
            table_attrs.push_back(attr);

            attr.id = SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE;
            attr.value.booldata = true;
            table_attrs.push_back(attr);
        }
    }
    else if (type == ACL_TABLE_L3V6 || type == ACL_TABLE_MIRRORV6) // v6 only
    {
        attr.id = SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE;
        attr.value.booldata = true;
        table_attrs.push_back(attr);
    }
    else // v4 only
    {
        attr.id = SAI_ACL_TABLE_ATTR_FIELD_SRC_IP;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_FIELD_DST_IP;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE;
        attr.value.booldata = true;
        table_attrs.push_back(attr);

        attr.id = SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE;
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

    int32_t range_types_list[] = { SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE, SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE };
    attr.id = SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE;
    attr.value.s32list.count = (uint32_t)(sizeof(range_types_list) / sizeof(range_types_list[0]));
    attr.value.s32list.list = range_types_list;
    table_attrs.push_back(attr);

    sai_acl_stage_t acl_stage;
    attr.id = SAI_ACL_TABLE_ATTR_ACL_STAGE;
    acl_stage = (stage == ACL_STAGE_INGRESS) ? SAI_ACL_STAGE_INGRESS : SAI_ACL_STAGE_EGRESS;
    attr.value.s32 = acl_stage;
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
        gCrmOrch->incCrmAclUsedCounter(CrmResourceType::CRM_ACL_TABLE, acl_stage, SAI_ACL_BIND_POINT_TYPE_PORT);
    }

    return status == SAI_STATUS_SUCCESS;
}

void AclTable::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    // Only interested in port change
    if (type != SUBJECT_TYPE_PORT_CHANGE)
    {
        return;
    }

    PortUpdate *update = static_cast<PortUpdate *>(cntx);

    Port &port = update->port;
    if (update->add)
    {
        if (pendingPortSet.find(port.m_alias) != pendingPortSet.end())
        {
            sai_object_id_t bind_port_id;
            if (gPortsOrch->getAclBindPortId(port.m_alias, bind_port_id))
            {
                link(bind_port_id);
                bind(bind_port_id);

                pendingPortSet.erase(port.m_alias);
                portSet.emplace(port.m_alias);

                SWSS_LOG_NOTICE("Bound port %s to ACL table %s",
                        port.m_alias.c_str(), id.c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Failed to get port %s bind port ID",
                        port.m_alias.c_str());
                return;
            }
        }
    }
    else
    {
        // TODO: deal with port removal scenario
    }
}

// TODO: make bind/unbind symmetric
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
        SWSS_LOG_ERROR("Failed to unbind table %" PRIu64 " as member %" PRIu64 " from ACL table: %d",
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
            SWSS_LOG_NOTICE("Successfully deleted ACL rule %s in table %s",
                    rule_id.c_str(), id.c_str());
        }
    }

    if (newRule->create())
    {
        rules[rule_id] = newRule;
        SWSS_LOG_NOTICE("Successfully created ACL rule %s in table %s",
                rule_id.c_str(), id.c_str());
        return true;
    }
    else
    {
        SWSS_LOG_ERROR("Failed to create ACL rule %s in table %s",
                rule_id.c_str(), id.c_str());
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
            SWSS_LOG_NOTICE("Successfully deleted ACL rule %s in table %s",
                    rule_id.c_str(), id.c_str());
            return true;
        }
        else
        {
            SWSS_LOG_ERROR("Failed to delete ACL rule %s in table %s",
                    rule_id.c_str(), id.c_str());
            return false;
        }
    }
    else
    {
        SWSS_LOG_WARN("Skip deleting unknown ACL rule %s in table %s",
                rule_id.c_str(), id.c_str());
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
            SWSS_LOG_ERROR("Failed to delete ACL rule %s when removing the ACL table %s",
                    rule.getId().c_str(), id.c_str());
            return false;
        }
    }
    rules.clear();
    return true;
}
