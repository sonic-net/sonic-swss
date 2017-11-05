#include <vector>

extern "C" {
#include "sai.h"
}

#include "port.h"
#include "swss/logger.h"

using namespace std;

extern sai_port_api_t *sai_port_api;
extern sai_acl_api_t* sai_acl_api;
extern sai_object_id_t gSwitchId;

namespace swss {

sai_status_t Port::bindAclTable(sai_object_id_t& group_member_oid, sai_object_id_t table_oid)
{
    sai_status_t status;
    sai_object_id_t groupOid;

    // If port ACL table group does not exist, create one
    if (m_acl_table_group_id == 0)
    {
        sai_object_id_t bp_list[] = { SAI_ACL_BIND_POINT_TYPE_PORT };

        vector<sai_attribute_t> group_attrs;
        sai_attribute_t group_attr;

        group_attr.id = SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE;
        group_attr.value.s32 = SAI_ACL_STAGE_INGRESS; // TODO: double check
        group_attrs.push_back(group_attr);

        group_attr.id = SAI_ACL_TABLE_GROUP_ATTR_ACL_BIND_POINT_TYPE_LIST;
        group_attr.value.objlist.count = 1;
        group_attr.value.objlist.list = bp_list;
        group_attrs.push_back(group_attr);

        group_attr.id = SAI_ACL_TABLE_GROUP_ATTR_TYPE;
        group_attr.value.s32 = SAI_ACL_TABLE_GROUP_TYPE_PARALLEL;
        group_attrs.push_back(group_attr);

        status = sai_acl_api->create_acl_table_group(&groupOid, gSwitchId, (uint32_t)group_attrs.size(), group_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create ACL table group, rv:%d", status);
            return status;
        }

        m_acl_table_group_id = groupOid;

        // Bind this ACL group to port OID
        sai_attribute_t port_attr;
        port_attr.id = SAI_PORT_ATTR_INGRESS_ACL;
        port_attr.value.oid = groupOid;

        status = sai_port_api->set_port_attribute(m_port_id, &port_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to bind port %lx(%s) to ACL table group %lx, rv:%d",
                    m_port_id, m_alias.c_str(), groupOid, status);
            return status;
        }

        SWSS_LOG_NOTICE("Create ACL table group and bind port %s to it", m_alias.c_str());
    }
    else
    {
        groupOid = m_acl_table_group_id;
    }

    // Create an ACL group member with table_oid and groupOid
    vector<sai_attribute_t> member_attrs;

    sai_attribute_t member_attr;
    member_attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID;
    member_attr.value.oid = groupOid;
    member_attrs.push_back(member_attr);

    member_attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID;
    member_attr.value.oid = table_oid;
    member_attrs.push_back(member_attr);

    member_attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY;
    member_attr.value.u32 = 100; // TODO: double check!
    member_attrs.push_back(member_attr);

    status = sai_acl_api->create_acl_table_group_member(&group_member_oid, gSwitchId, (uint32_t)member_attrs.size(), member_attrs.data());
    if (status != SAI_STATUS_SUCCESS) {
        SWSS_LOG_ERROR("Failed to create member in ACL table group %lx for ACL table group %lx, rv:%d",
                table_oid, groupOid, status);
        return status;
    }

    return SAI_STATUS_SUCCESS;
}

}
