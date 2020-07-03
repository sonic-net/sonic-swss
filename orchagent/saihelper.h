#pragma once

#include "gearboxutils.h"

#include <string>

void initSaiApi();
void initSaiRedis(const std::string &record_location);
sai_status_t initSaiPhyApi(swss::gearbox_phy_t *phy);
