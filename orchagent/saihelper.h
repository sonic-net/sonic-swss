#pragma once

#include "gearboxutils.h"

#include <string>

#define IS_ATTR_ID_IN_RANGE(attrId, objectType, attrPrefix) \
    ((attrId) >= SAI_ ## objectType ## _ATTR_ ## attrPrefix ## _START && (attrId) <= SAI_ ## objectType ## _ATTR_ ## attrPrefix ## _END)

void initSaiApi();
void initSaiRedis(const std::string &record_location, const std::string &record_filename);
sai_status_t initSaiPhyApi(swss::gearbox_phy_t *phy);
