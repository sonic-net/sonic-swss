#ifndef __POLICERMGR__
#define __POLICERMGR__

#include "dbconnector.h"
#include "orch.h"

#include <map>
#include <string>
#include <vector>

#ifndef CFG_POLICER_TABLE_NAME
#define CFG_POLICER_TABLE_NAME              "POLICER"
#endif
#ifndef STATE_POLICER_TABLE_NAME
#define STATE_POLICER_TABLE_NAME            "POLICER_TABLE"
#endif
#ifndef CFG_PORT_STORM_CONTROL_TABLE_NAME
#define CFG_PORT_STORM_CONTROL_TABLE_NAME   "PORT_STORM_CONTROL"
#endif

/* POLICER field names (lowercase, per sonic-policer.yang) */
#define POLICER_FIELD_METER_TYPE        "meter_type"
#define POLICER_FIELD_MODE              "mode"
#define POLICER_FIELD_COLOR             "color"
#define POLICER_FIELD_CIR               "cir"
#define POLICER_FIELD_CBS               "cbs"
#define POLICER_FIELD_PIR               "pir"
#define POLICER_FIELD_PBS               "pbs"
#define POLICER_FIELD_GREEN_ACTION      "green_packet_action"
#define POLICER_FIELD_RED_ACTION        "red_packet_action"
#define POLICER_FIELD_YELLOW_ACTION     "yellow_packet_action"

/* PORT_STORM_CONTROL field */
#define STORM_CONTROL_FIELD_KBPS        "kbps"

namespace swss {

/*
 * PolicerMgr — switchdev policer/meter manager.
 *
 * Reads CONFIG_DB POLICER (meter_type/mode/color/cir/cbs/pir/pbs/actions),
 * validates it, and writes STATE_DB POLICER_TABLE. The kernel tc police action
 * is applied by the referencing daemon (aclmgrd/mirrormgrd) via
 * kernutil::policerToTcPolice. Also handles per-port storm control from the
 * PORT_STORM_CONTROL table, programming tc police filters directly.
 */
class PolicerMgr : public Orch
{
public:
    PolicerMgr(DBConnector *cfgDb, DBConnector *stateDb,
               const std::vector<std::string> &tableNames);
    using Orch::doTask;

    bool policerExists(const std::string &name);
    bool increaseRefCount(const std::string &name);
    bool decreaseRefCount(const std::string &name);

private:
    Table m_statePolicerTable;
    std::map<std::string, int> m_policerRefCounts;   // policer name -> refcount
    std::map<std::string, uint32_t> m_stormPrio;     // port|storm_type -> tc prio
    uint32_t m_nextStormPrio = 200;

    void doTask(Consumer &consumer);
    void doPolicerTask(Consumer &consumer);
    void doPortStormControlTask(Consumer &consumer);

    void addStormControlFilter(const std::string &iface, const std::string &stormType,
                               const std::string &kbps, uint32_t prio);
    void removeStormControlFilter(const std::string &iface, uint32_t prio);
};

} // namespace swss

#endif /* __POLICERMGR__ */
