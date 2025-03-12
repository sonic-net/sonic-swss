#pragma once

#include "gearboxutils.h"

#include <string>
#include "orch.h"
#include "producertable.h"

#define IS_ATTR_ID_IN_RANGE(attrId, objectType, attrPrefix) \
    ((attrId) >= SAI_ ## objectType ## _ATTR_ ## attrPrefix ## _START && (attrId) <= SAI_ ## objectType ## _ATTR_ ## attrPrefix ## _END)

void initFlexCounterTables();
void initSaiApi();
void initSaiRedis();
sai_status_t initSaiPhyApi(swss::gearbox_phy_t *phy);

/* Handling SAI status*/
task_process_status handleSaiCreateStatus(sai_api_t api, sai_status_t status, void *context = nullptr);
task_process_status handleSaiSetStatus(sai_api_t api, sai_status_t status, void *context = nullptr);
task_process_status handleSaiRemoveStatus(sai_api_t api, sai_status_t status, void *context = nullptr);
task_process_status handleSaiGetStatus(sai_api_t api, sai_status_t status, void *context = nullptr);
bool parseHandleSaiStatusFailure(task_process_status status);
void handleSaiFailure(bool abort_on_failure);

void setFlexCounterGroupParameter(const std::string &group,
                                  const std::string &poll_interval,
                                  const std::string &stats_mode,
                                  const std::string &plugin_name="",
                                  const std::string &plugins="",
                                  const std::string &operation="",
                                  bool is_gearbox=false);
void setFlexCounterGroupPollInterval(const std::string &group,
                                     const std::string &poll_interval,
                                     bool is_gearbox=false);
void setFlexCounterGroupOperation(const std::string &group,
                                  const std::string &operation,
                                  bool is_gearbox=false);
void setFlexCounterGroupStatsMode(const std::string &group,
                                  const std::string &stats_mode,
                                  bool is_gearbox=false);

void delFlexCounterGroup(const std::string &group,
                         bool is_gearbox=false);

void startFlexCounterPolling(sai_object_id_t switch_oid,
                             const std::string &key,
                             const std::string &counter_ids,
                             const std::string &counter_field_name,
                             const std::string &stats_mode="");
void stopFlexCounterPolling(sai_object_id_t switch_oid,
                            const std::string &key);

std::vector<sai_stat_id_t> queryAvailableCounterStats(const sai_object_type_t);
