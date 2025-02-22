/*
 * Copyright 2024
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SWSS_COMMON_POE_CONFIG_H
#define SWSS_COMMON_POE_CONFIG_H

#include <map>
#include <set>

#include "table.h"

namespace swss {

typedef struct {
    std::string oid;
    uint32_t id;
    std::string hwInfo;
    std::string powerLimitMode;
} poe_device_t;

typedef struct {
    std::string oid;
    uint32_t deviceId;
    uint32_t pseIndex;
} poe_pse_t;

typedef struct {
    std::string oid;
    uint32_t deviceId;
    uint32_t frontPanelIndex;
    std::string interface;
    bool enable;
    uint32_t powerLimit;
    std::string powerPriority;
} poe_port_t;

class PoeConfig
{
    private:
        std::map<uint32_t, poe_device_t> poeDeviceMap;
        std::map<uint32_t, poe_pse_t> poePseMap;
        std::map<uint32_t, poe_port_t> poePortMap;
        Table *poeTable;
        bool loadDeviceConfig(const std::vector<FieldValueTuple> &ovalues);
        bool loadPseConfig(const std::vector<FieldValueTuple> &ovalues);
        bool loadPortConfig(const std::vector<FieldValueTuple> &ovalues);
        std::string parseKey(const std::string &key_str);
    public:
        PoeConfig(Table *table);
        PoeConfig(Table &table);
        bool platformHasPoe();
        bool isPoeEnabled();
        bool isConfigDone();
        bool loadConfig();
        std::map<uint32_t, poe_device_t> getDeviceMap() { return poeDeviceMap; };
        std::map<uint32_t, poe_pse_t> getPseMap() { return poePseMap; };
        std::map<uint32_t, poe_port_t> getPortMap() { return poePortMap; };
};

}

#endif /* SWSS_COMMON_POE_CONFIG_H */
