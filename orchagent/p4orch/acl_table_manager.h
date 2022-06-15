#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "orch.h"
#include "p4orch/acl_util.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "response_publisher_interface.h"
#include "return_code.h"

extern "C"
{
#include "sai.h"
}

namespace p4orch
{
namespace test
{
class AclManagerTest;
} // namespace test

class AclTableManager : public ObjectManagerInterface
{
  public:
    explicit AclTableManager(P4OidMapper *p4oidMapper, ResponsePublisherInterface *publisher);
    virtual ~AclTableManager();

    void enqueue(const swss::KeyOpFieldsValuesTuple &entry) override;
    void drain() override;

    // Get ACL table definition by table name in cache. Return nullptr if not
    // found.
    P4AclTableDefinition *getAclTable(const std::string &acl_table_name);

  private:
    // Validate ACL table definition APP_DB entry.
    ReturnCode validateAclTableDefinitionAppDbEntry(const P4AclTableDefinitionAppDbEntry &app_db_entry);

    // Deserializes an entry from table APP_P4RT_ACL_TABLE_DEFINITION_NAME.
    ReturnCodeOr<P4AclTableDefinitionAppDbEntry> deserializeAclTableDefinitionAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes);

    // Create new ACL table definition.
    ReturnCode createAclTable(P4AclTableDefinition &acl_table, sai_object_id_t *acl_table_oid,
                              sai_object_id_t *acl_group_member_oid);

    // Remove ACL table by table name. Caller should verify reference count is
    // zero before calling the method.
    ReturnCode removeAclTable(P4AclTableDefinition &acl_table);

    // Create UDF groups and UDFs for the ACL table. If any of the UDF and UDF
    // group fails to create, then clean up all created ones
    ReturnCode createUdfGroupsAndUdfsForAclTable(const P4AclTableDefinition &acl_table);

    // Create new ACL UDF group based on the UdfField. Callers should verify no
    // UDF group with the same name exists
    ReturnCode createUdfGroup(const P4UdfField &udf_field);

    // Remove ACL UDF group by group id,
    ReturnCode removeUdfGroup(const std::string &udf_group_id);

    // Create the default UDF match with name P4_UDF_MATCH_DEFAULT.
    // The attributes values for the UDF match are all wildcard matches.
    ReturnCode createDefaultUdfMatch();

    // Remove the default UDF match if no UDFs depend on it.
    ReturnCode removeDefaultUdfMatch();

    // Create UDF with group_oid, base and offset defined in udf_field and the
    // default udf_match_oid. Callers should verify no UDF with the same name
    // exists
    ReturnCode createUdf(const P4UdfField &udf_fields);

    // Remove UDF by id string and group id string if no ACL rules depends on it
    ReturnCode removeUdf(const std::string &udf_id, const std::string &udf_group_id);

    // Process add request on ACL table definition. If the table is
    // created successfully, a new consumer will be added in
    // p4orch to process requests for ACL rules for the table.
    ReturnCode processAddTableRequest(const P4AclTableDefinitionAppDbEntry &app_db_entry);

    // Process delete request on ACL table definition.
    ReturnCode processDeleteTableRequest(const std::string &acl_table_name);

    // Create ACL group member for given ACL table.
    ReturnCode createAclGroupMember(const P4AclTableDefinition &acl_table, sai_object_id_t *acl_grp_mem_oid);

    // Remove ACL group member for given ACL table.
    ReturnCode removeAclGroupMember(const std::string &acl_table_name);

    P4OidMapper *m_p4OidMapper;
    ResponsePublisherInterface *m_publisher;
    P4AclTableDefinitions m_aclTableDefinitions;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;
    std::map<sai_acl_stage_t, std::vector<std::string>> m_aclTablesByStage;

    friend class p4orch::test::AclManagerTest;
};

} // namespace p4orch
