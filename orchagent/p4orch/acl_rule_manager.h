#pragma once

#include <map>
#include <string>
#include <vector>

#include "copporch.h"
#include "orch.h"
#include "p4orch/acl_util.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "response_publisher_interface.h"
#include "return_code.h"
#include "vrforch.h"

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

class AclRuleManager : public ObjectManagerInterface
{
  public:
    explicit AclRuleManager(P4OidMapper *p4oidMapper, VRFOrch *vrfOrch, CoppOrch *coppOrch,
                            ResponsePublisherInterface *publisher)
        : m_p4OidMapper(p4oidMapper), m_vrfOrch(vrfOrch), m_publisher(publisher), m_coppOrch(coppOrch),
          m_countersDb(std::make_unique<swss::DBConnector>("COUNTERS_DB", 0)),
          m_countersTable(std::make_unique<swss::Table>(
              m_countersDb.get(), std::string(COUNTERS_TABLE) + DEFAULT_KEY_SEPARATOR + APP_P4RT_TABLE_NAME))
    {
        SWSS_LOG_ENTER();
        assert(m_p4OidMapper != nullptr);
    }
    virtual ~AclRuleManager() = default;

    void enqueue(const swss::KeyOpFieldsValuesTuple &entry) override;
    void drain() override;

    // Update counters stats for every rule in each ACL table in COUNTERS_DB, if
    // counters are enabled in rules.
    void doAclCounterStatsTask();

  private:
    // Deserializes an entry in a dynamically created ACL table.
    ReturnCodeOr<P4AclRuleAppDbEntry> deserializeAclRuleAppDbEntry(
        const std::string &acl_table_name, const std::string &key,
        const std::vector<swss::FieldValueTuple> &attributes);

    // Validate an ACL rule APP_DB entry.
    ReturnCode validateAclRuleAppDbEntry(const P4AclRuleAppDbEntry &app_db_entry);

    // Get ACL rule by table name and rule key. Return nullptr if not found.
    P4AclRule *getAclRule(const std::string &acl_table_name, const std::string &acl_rule_key);

    // Processes add operation for an ACL rule.
    ReturnCode processAddRuleRequest(const std::string &acl_rule_key, const P4AclRuleAppDbEntry &app_db_entry);

    // Processes delete operation for an ACL rule.
    ReturnCode processDeleteRuleRequest(const std::string &acl_table_name, const std::string &acl_rule_key);

    // Processes update operation for an ACL rule.
    ReturnCode processUpdateRuleRequest(const P4AclRuleAppDbEntry &app_db_entry, const P4AclRule &old_acl_rule);

    // Set counters stats for an ACL rule in COUNTERS_DB.
    ReturnCode setAclRuleCounterStats(const P4AclRule &acl_rule);

    // Create an ACL rule.
    ReturnCode createAclRule(P4AclRule &acl_rule);

    // Create an ACL counter.
    ReturnCode createAclCounter(const std::string &acl_table_name, const std::string &counter_key,
                                const P4AclCounter &p4_acl_counter, sai_object_id_t *counter_oid);

    // Create an ACL meter.
    ReturnCode createAclMeter(const P4AclMeter &p4_acl_meter, const std::string &meter_key, sai_object_id_t *meter_oid);

    // Remove an ACL counter.
    ReturnCode removeAclCounter(const std::string &acl_table_name, const std::string &counter_key);

    // Update ACL meter.
    ReturnCode updateAclMeter(const P4AclMeter &new_acl_meter, const P4AclMeter &old_acl_meter);

    // Update ACL rule.
    ReturnCode updateAclRule(const P4AclRule &new_acl_rule, const P4AclRule &old_acl_rule,
                             std::vector<sai_attribute_t> &acl_entry_attrs,
                             std::vector<sai_attribute_t> &rollback_attrs);

    // Remove an ACL meter.
    ReturnCode removeAclMeter(const std::string &meter_key);

    // Remove the ACL rule by key in the given ACL table.
    ReturnCode removeAclRule(const std::string &acl_table_name, const std::string &acl_rule_key);

    // Set Meter value in ACL rule.
    ReturnCode setMeterValue(const P4AclTableDefinition *acl_table, const P4AclRuleAppDbEntry &app_db_entry,
                             P4AclMeter &acl_meter);

    // Validate and set all match attributes in an ACL rule.
    ReturnCode setAllMatchFieldValues(const P4AclRuleAppDbEntry &app_db_entry, const P4AclTableDefinition *acl_table,
                                      P4AclRule &acl_rules);

    // Validate and set all action attributes in an ACL rule.
    ReturnCode setAllActionFieldValues(const P4AclRuleAppDbEntry &app_db_entry, const P4AclTableDefinition *acl_table,
                                       P4AclRule &acl_rule);

    // Validate and set a match attribute in an ACL rule.
    ReturnCode setMatchValue(const acl_entry_attr_union_t attr_name, const std::string &attr_value,
                             sai_attribute_value_t *value, P4AclRule *acl_rule,
                             const std::string &ip_type_bit_type = EMPTY_STRING);

    // Validate and set an action attribute in an ACL rule.
    ReturnCode setActionValue(const acl_entry_attr_union_t attr_name, const std::string &attr_value,
                              sai_attribute_value_t *value, P4AclRule *acl_rule);

    // Get port object id by name for redirect action.
    ReturnCode getRedirectActionPortOid(const std::string &target, sai_object_id_t *rediect_oid);

    // Get next hop object id by name for redirect action.
    ReturnCode getRedirectActionNextHopOid(const std::string &target, sai_object_id_t *rediect_oid);

    // Create user defined trap for each cpu queue/trap group and program user
    // defined traps in hostif. Save the user defined trap oids in m_p4OidMapper
    // and default ref count is 1.
    ReturnCode setUpUserDefinedTraps();

    // Clean up user defined traps created for cpu queues. Callers need to make
    // sure ref count on user defined traps in m_userDefinedTraps are ones before
    // clean up.
    ReturnCode cleanUpUserDefinedTraps();

    P4OidMapper *m_p4OidMapper;
    ResponsePublisherInterface *m_publisher;
    P4AclRuleTables m_aclRuleTables;
    VRFOrch *m_vrfOrch;
    CoppOrch *m_coppOrch;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;
    std::unique_ptr<swss::DBConnector> m_countersDb;
    std::unique_ptr<swss::Table> m_countersTable;
    std::vector<P4UserDefinedTrapHostifTableEntry> m_userDefinedTraps;

    friend class AclTableManager;
    friend class p4orch::test::AclManagerTest;
};

} // namespace p4orch
