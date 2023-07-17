#pragma once

#include "gearboxutils.h"

#include <string>
#include "orch.h"

#define IS_ATTR_ID_IN_RANGE(attrId, objectType, attrPrefix) \
    ((attrId) >= SAI_ ## objectType ## _ATTR_ ## attrPrefix ## _START && (attrId) <= SAI_ ## objectType ## _ATTR_ ## attrPrefix ## _END)

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
