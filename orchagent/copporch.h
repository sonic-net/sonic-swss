#ifndef SWSS_COPPORCH_H
#define SWSS_COPPORCH_H

#include <map>
#include <set>
#include "orch.h"

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

struct copp_trap_objects
{
    sai_object_id_t trap_obj;
    sai_object_id_t trap_group_obj;
};

/* TrapGroupPolicerTable: trap group ID, policer ID */
typedef std::map<sai_object_id_t, sai_object_id_t> TrapGroupPolicerTable;
/* TrapIdTrapObjectsTable: trap ID, copp trap objects */
typedef std::map<sai_hostif_trap_type_t, copp_trap_objects> TrapIdTrapObjectsTable;
/* TrapGroupHostIfMap: trap group ID, host interface ID */
typedef std::map<sai_object_id_t, sai_object_id_t> TrapGroupHostIfMap;
/* TrapIdHostIfTableMap: trap type, host table entry ID*/
typedef std::map<sai_hostif_trap_type_t, sai_object_id_t> TrapIdHostIfTableMap;

class CoppOrch : public Orch
{
public:
    CoppOrch(std::vector<TableConnector> &tableConnectors);
protected:
    object_map m_trap_group_map;
    bool       enable_sflow_trap;

    TrapGroupPolicerTable m_trap_group_policer_map;
    TrapIdTrapObjectsTable m_syncdTrapIds;

    TrapGroupHostIfMap m_trap_group_hostif_map;
    TrapIdHostIfTableMap m_trapid_hostif_table_map;

    void initDefaultHostIntfTable();
    void initDefaultTrapGroup();
    void initDefaultTrapIds();

    task_process_status processCoppRule(Consumer& consumer);
    bool isValidList(std::vector<std::string> &trap_id_list, std::vector<std::string> &all_items) const;
    void getTrapIdList(std::vector<std::string> &trap_id_name_list, std::vector<sai_hostif_trap_type_t> &trap_id_list) const;
    bool applyTrapIds(sai_object_id_t trap_group, std::vector<std::string> &trap_id_name_list, std::vector<sai_attribute_t> &trap_id_attribs);
    bool applyAttributesToTrapIds(sai_object_id_t trap_group_id, const std::vector<sai_hostif_trap_type_t> &trap_id_list, std::vector<sai_attribute_t> &trap_id_attribs);

    bool createPolicer(std::string trap_group, std::vector<sai_attribute_t> &policer_attribs);
    bool removePolicer(std::string trap_group_name);

    sai_object_id_t getPolicer(std::string trap_group_name);

    bool createGenetlinkHostIf(std::string trap_group_name, std::vector<sai_attribute_t> &hostif_attribs);
    bool removeGenetlinkHostIf(std::string trap_group_name);
    bool createGenetlinkHostifTable(std::vector<std::string> &trap_id_name_list);
    void coppProcessSflow(Consumer& consumer);

    virtual void doTask(Consumer& consumer);
};
#endif /* SWSS_COPPORCH_H */

