#include "aclorch.h"

#define ACL_COUNTER_DEFAULT_POLLING_INTERVAL_MS 10000  // ms
#define ACL_COUNTER_DEFAULT_ENABLED_STATE false

AclOrch::AclOrch(vector<TableConnector>& connectors, DBConnector* stateDb,
                 SwitchOrch* switchOrch, PortsOrch* portOrch,
                 MirrorOrch* mirrorOrch, NeighOrch* neighOrch,
                 RouteOrch* routeOrch, DTelOrch* dtelOrch)
    : Orch(connectors),
      m_aclStageCapabilityTable(stateDb, STATE_ACL_STAGE_CAPABILITY_TABLE_NAME),
      m_aclTableStateTable(stateDb, STATE_ACL_TABLE_TABLE_NAME),
      m_aclRuleStateTable(stateDb, STATE_ACL_RULE_TABLE_NAME),
      m_switchOrch(switchOrch),
      m_mirrorOrch(mirrorOrch),
      m_neighOrch(neighOrch),
      m_routeOrch(routeOrch),
      m_dTelOrch(dtelOrch),
      m_flex_counter_manager(ACL_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                             ACL_COUNTER_DEFAULT_POLLING_INTERVAL_MS,
                             ACL_COUNTER_DEFAULT_ENABLED_STATE) {
  SWSS_LOG_ENTER();
}

bool AclOrch::isAclActionListMandatoryOnTableCreation(
    acl_stage_type_t stage) const {
  return true;
}

bool AclOrch::isAclActionSupported(acl_stage_type_t stage,
                                   sai_acl_action_type_t action) const {
  return true;
}

AclOrch::~AclOrch() {}

void AclOrch::doTask(Consumer& consumer) {}

void AclOrch::update(SubjectType type, void* cntx) {}