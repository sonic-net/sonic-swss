#ifndef SWSS_COPPORCH_H
#define SWSS_COPPORCH_H

#include <map>
#include <set>
#include <memory>
#include "dbconnector.h"
#include "orch.h"
#include "flex_counter_manager.h"
#include "producertable.h"
#include "table.h"
#include "selectabletimer.h"

using namespace swss;

#define HOSTIF_TRAP_COUNTER_FLEX_COUNTER_GROUP "HOSTIF_TRAP_FLOW_COUNTER"

// trap fields
const std::string copp_trap_id_list                = "trap_ids";
const std::string copp_trap_action_field           = "trap_action";
const std::string copp_trap_priority_field         = "trap_priority";

const std::string copp_queue_field                 = "queue";

// policer fields
const std::string copp_policer_meter_type_field    = "meter_type";
const std::string copp_policer_mode_field          = "mode";
const std::string copp_policer_color_field         = "color";
const std::string copp_policer_cbs_field           = "cbs";
const std::string copp_policer_cir_field           = "cir";
const std::string copp_policer_pbs_field           = "pbs";
const std::string copp_policer_pir_field           = "pir";
const std::string copp_policer_action_green_field  = "green_action";
const std::string copp_policer_action_red_field    = "red_action";
const std::string copp_policer_action_yellow_field = "yellow_action";

// genetlink fields
const std::string copp_genetlink_name              = "genetlink_name";
const std::string copp_genetlink_mcgrp_name        = "genetlink_mcgrp_name";

typedef std::map<sai_attr_id_t, sai_attribute_value_t> TrapIdAttribs;
struct copp_trap_objects
{
    sai_object_id_t trap_obj;
    sai_object_id_t trap_group_obj;
    sai_hostif_trap_type_t trap_type;
};

/* TrapGroupPolicerTable: trap group ID, policer ID */
typedef std::map<sai_object_id_t, sai_object_id_t> TrapGroupPolicerTable;
/* TrapIdTrapObjectsTable: trap ID, copp trap objects */
typedef std::map<sai_hostif_trap_type_t, copp_trap_objects> TrapIdTrapObjectsTable;
/* TrapGroupHostIfMap: trap group ID, host interface ID */
typedef std::map<sai_object_id_t, sai_object_id_t> TrapGroupHostIfMap;
/* TrapIdHostIfTableMap: trap type, host table entry ID*/
typedef std::map<sai_hostif_trap_type_t, sai_object_id_t> TrapIdHostIfTableMap;
/* Trap group to trap ID attributes */
typedef std::map<std::string, TrapIdAttribs> TrapGroupTrapIdAttribs;
/* Trap OID to trap name*/
typedef std::map<sai_object_id_t, std::string> TrapObjectTrapNameMap;

class CoppOrch : public Orch
{
public:
    CoppOrch(swss::DBConnector* db, std::string tableName);
    void generateHostIfTrapCounterIdList();
    void clearHostIfTrapCounterIdList();

    inline object_map getTrapGroupMap()
    {
        return m_trap_group_map;
    }

    inline TrapGroupHostIfMap getTrapGroupHostIfMap()
    {
        return m_trap_group_hostif_map;
    }
protected:
    object_map m_trap_group_map;

    TrapGroupPolicerTable m_trap_group_policer_map;
    TrapIdTrapObjectsTable m_syncdTrapIds;

    TrapGroupHostIfMap m_trap_group_hostif_map;
    TrapIdHostIfTableMap m_trapid_hostif_table_map;
    TrapGroupTrapIdAttribs m_trap_group_trap_id_attrs;
    TrapObjectTrapNameMap m_trap_obj_name_map;
    std::map<sai_object_id_t, std::string> m_pendingAddToFlexCntr;

    std::shared_ptr<DBConnector> m_counter_db;
    std::shared_ptr<DBConnector> m_flex_db;
    std::shared_ptr<DBConnector> m_asic_db;
    std::unique_ptr<Table> m_counter_table;
    std::unique_ptr<Table> m_vidToRidTable;
    std::unique_ptr<ProducerTable> m_flex_counter_group_table;

    FlexCounterManager m_trap_counter_manager;

    bool m_trap_rate_plugin_loaded = false;

    SelectableTimer* m_FlexCounterUpdTimer = nullptr;

    void initDefaultHostIntfTable();
    void initDefaultTrapGroup();
    void initDefaultTrapIds();
    void initTrapRatePlugin();

    task_process_status processCoppRule(Consumer& consumer);
    bool isValidList(std::vector<std::string> &trap_id_list, std::vector<std::string> &all_items) const;
    void getTrapIdList(std::vector<std::string> &trap_id_name_list, std::vector<sai_hostif_trap_type_t> &trap_id_list) const;
    bool applyAttributesToTrapIds(sai_object_id_t trap_group_id, const std::vector<sai_hostif_trap_type_t> &trap_id_list, std::vector<sai_attribute_t> &trap_id_attribs);

    bool createPolicer(std::string trap_group, std::vector<sai_attribute_t> &policer_attribs);
    bool removePolicer(std::string trap_group_name);

    sai_object_id_t getPolicer(std::string trap_group_name);

    bool createGenetlinkHostIf(std::string trap_group_name, std::vector<sai_attribute_t> &hostif_attribs);
    bool removeGenetlinkHostIf(std::string trap_group_name);
    bool createGenetlinkHostIfTable(std::vector<sai_hostif_trap_type_t> &trap_id_list);
    bool removeGenetlinkHostIfTable(std::vector<sai_hostif_trap_type_t> &trap_id_list);
    void getTrapAddandRemoveList(std::string trap_group_name, std::vector<sai_hostif_trap_type_t> &trap_ids,
                                 std::vector<sai_hostif_trap_type_t> &add_trap_ids,
                                 std::vector<sai_hostif_trap_type_t> &rem_trap_ids);

    void getTrapIdsFromTrapGroup (sai_object_id_t trap_group_obj,
                                  std::vector<sai_hostif_trap_type_t> &trap_ids);

    bool trapGroupProcessTrapIdChange (std::string trap_group_name,
                                       std::vector<sai_hostif_trap_type_t> &add_trap_ids,
                                       std::vector<sai_hostif_trap_type_t> &rem_trap_ids);

    bool processTrapGroupDel (std::string trap_group_name);

    bool getAttribsFromTrapGroup (std::vector<swss::FieldValueTuple> &fv_tuple,
                                  std::vector<sai_attribute_t> &trap_gr_attribs,
                                  std::vector<sai_attribute_t> &trap_id_attribs,
                                  std::vector<sai_attribute_t> &policer_attribs,
                                  std::vector<sai_attribute_t> &genetlink_attribs);

    bool trapGroupUpdatePolicer (std::string trap_group_name, std::vector<sai_attribute_t> &policer_attribs);

    bool removeTrap(sai_object_id_t hostif_trap_id);

    bool bindTrapCounter(sai_object_id_t hostif_trap_id, sai_hostif_trap_type_t trap_type);
    void unbindTrapCounter(sai_object_id_t hostif_trap_id);

    virtual void doTask(Consumer& consumer);
    void doTask(swss::SelectableTimer&) override;
};
#endif /* SWSS_COPPORCH_H */

