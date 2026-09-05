#ifndef __POLICERMGR__
#define __POLICERMGR__

#include "dbconnector.h"
#include "orch.h"

#include <set>
#include <string>
#include <vector>

#ifndef CFG_POLICER_TABLE_NAME
#define CFG_POLICER_TABLE_NAME          "POLICER"
#endif
#ifndef STATE_POLICER_TABLE_NAME
#define STATE_POLICER_TABLE_NAME        "POLICER_TABLE"
#endif

#define POLICER_FIELD_METER_TYPE        "meter_type"
#define POLICER_FIELD_MODE              "mode"
#define POLICER_FIELD_CIR               "cir"
#define POLICER_FIELD_CBS               "cbs"
#define POLICER_FIELD_PIR               "pir"
#define POLICER_FIELD_PBS               "pbs"
#define POLICER_FIELD_GREEN_ACTION      "green_packet_action"
#define POLICER_FIELD_RED_ACTION        "red_packet_action"
#define POLICER_FIELD_YELLOW_ACTION     "yellow_packet_action"

namespace swss {

/*
 * PolicerMgr — switchdev policer (meter) manager.
 *
 * Reads CONFIG_DB POLICER (meter_type / cir / cbs / pir / pbs / actions),
 * validates the config and writes status to STATE_DB POLICER_TABLE. A policer
 * is a meter object (no port), so the kernel tc police action is applied by the
 * referencing daemon (aclmgrd/mirrormgrd); this daemon validates + registers it.
 */
class PolicerMgr : public Orch
{
public:
    PolicerMgr(DBConnector *cfgDb, DBConnector *stateDb,
               const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    Table m_statePolicerTable;
    std::set<std::string> m_registeredPolicers;

    void doTask(Consumer &consumer);
    void doPolicerTask(Consumer &consumer);
};

} // namespace swss

#endif /* __POLICERMGR__ */
