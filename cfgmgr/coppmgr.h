#pragma once

#include "dbconnector.h"
#include "orch.h"
#include "producerstatetable.h"
#include <unistd.h>

#include <map>
#include <set>
#include <string>

namespace swss {

/* COPP Trap Table Fields */
#define COPP_TRAP_ID_LIST_FIELD                "trap_ids"
#define COPP_TRAP_GROUP_FIELD                  "trap_group"
#define COPP_ALWAYS_ENABLED_FIELD              "always_enabled"

/* COPP Group Table Fields */
#define COPP_GROUP_QUEUE_FIELD                 "queue"
#define COPP_GROUP_TRAP_ACTION_FIELD           "trap_action"
#define COPP_GROUP_TRAP_PRIORITY_FIELD         "trap_priority"
#define COPP_GROUP_POLICER_METER_TYPE_FIELD    "meter_type"
#define COPP_GROUP_POLICER_MODE_FIELD          "mode"
#define COPP_GROUP_POLICER_COLOR_FIELD         "color"
#define COPP_GROUP_POLICER_CBS_FIELD           "cbs"
#define COPP_GROUP_POLICER_CIR_FIELD           "cir"
#define COPP_GROUP_POLICER_PBS_FIELD           "pbs"
#define COPP_GROUP_POLICER_PIR_FIELD           "pir"
#define COPP_GROUP_POLICER_ACTION_GREEN_FIELD  "green_action"
#define COPP_GROUP_POLICER_ACTION_RED_FIELD    "red_action"
#define COPP_GROUP_POLICER_ACTION_YELLOW_FIELD "yellow_action"

/* sflow genetlink fields */
#define COPP_GROUP_GENETLINK_NAME_FIELD        "genetlink_name"
#define COPP_GROUP_GENETLINK_MCGRP_NAME_FIELD  "genetlink_mcgrp_name"

#define COPP_TRAP_TYPE_SAMPLEPACKET            "sample_packet"

#define COPP_INIT_FILE "/etc/sonic/copp_cfg.json"

struct CoppTrapConf
{
    std::string         trap_ids;
    std::string         trap_group;
    std::string         is_always_enabled;
};

/* TrapName to TrapConf map  */
typedef std::map<std::string, CoppTrapConf> CoppTrapConfMap;

/* TrapID to Trap group name map  */
typedef std::map<std::string, std::string> CoppTrapIdTrapGroupMap;

/* Key to Field value Tuple map */
typedef std::map<std::string, std::vector<FieldValueTuple>> CoppCfg;

/* Restricted Copp group key to Field value map's map */
typedef std::map<std::string, std::map<std::string, std::string>> CoppGroupFvs;

class CoppMgr : public Orch
{
public:
    CoppMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb,
        const std::vector<std::string> &tableNames);

    using Orch::doTask;
private:
    Table                  m_cfgCoppTrapTable, m_cfgCoppGroupTable, m_cfgFeatureTable, m_coppTable;
    Table                  m_stateCoppTrapTable, m_stateCoppGroupTable;
    ProducerStateTable     m_appCoppTable;
    CoppTrapConfMap        m_coppTrapConfMap;
    CoppTrapIdTrapGroupMap m_coppTrapIdTrapGroupMap;
    CoppGroupFvs           m_coppGroupFvs;
    CoppCfg                m_coppGroupInitCfg;
    CoppCfg                m_coppTrapInitCfg;
    CoppCfg                m_featuresCfgTable;


    void doTask(Consumer &consumer);
    void doCoppGroupTask(Consumer &consumer);
    void doCoppTrapTask(Consumer &consumer);
    void doFeatureTask(Consumer &consumer);

    void getTrapGroupTrapIds(std::string trap_group, std::string &trap_ids);
    void removeTrapIdsFromTrapGroup(std::string trap_group, std::string trap_ids);
    void addTrapIdsToTrapGroup(std::string trap_group, std::string trap_ids);
    bool isTrapIdDisabled(std::string trap_id);
    void setFeatureTrapIdsStatus(std::string feature, bool enable);
    bool checkTrapGroupPending(std::string trap_group_name);

    void setCoppGroupStateOk(std::string alias);
    void delCoppGroupStateOk(std::string alias);

    void setCoppTrapStateOk(std::string alias);
    void delCoppTrapStateOk(std::string alias);
    void coppGroupGetModifiedFvs(std::string key, std::vector<FieldValueTuple> &trap_group_fvs,
                                 std::vector<FieldValueTuple> &modified_fvs);
    void parseInitFile(void);
    bool isTrapGroupInstalled(std::string key);
    bool isFeatureEnabled(std::string feature);
    void mergeConfig(CoppCfg &init_cfg, CoppCfg &m_cfg, std::vector<std::string> &cfg_keys, Table &cfgTable);
    bool isDupEntry(const std::string &key, std::vector<FieldValueTuple> &fvs);

    void removeTrap(std::string key);
    void addTrap(std::string trap_ids, std::string trap_group);


};

}
