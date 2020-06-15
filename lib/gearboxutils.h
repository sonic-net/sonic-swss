/*
 * Copyright 2019-2020 Broadcom Inc.
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

#ifndef SWSS_COMMON_GEARBOX_UTILS_H
#define SWSS_COMMON_GEARBOX_UTILS_H

#include <unistd.h>
#include <sstream>
#include <iostream>
#include <vector>
#include <map>
#include <tuple>
#include <set>

#include "tokenize.h"
#include "table.h"

namespace swss {

typedef struct
{
    int phy_id;
    std::string phy_oid;
    std::string name;
    std::string lib_name;
    std::string firmware;
    std::string firmware_major_version;
    std::string sai_init_config_file;
    std::string config_file;
    std::string access;
    uint32_t address;
    uint32_t bus_id;
} gearbox_phy_t;

typedef struct
{
    int index;
    int phy_id;
    std::set<int> line_lanes;
    std::set<int> system_lanes;
} gearbox_interface_t;

typedef struct
{
    int index;
    bool system_side;
    int tx_polarity;
    int rx_polarity;
    int line_tx_lanemap;
    int line_rx_lanemap;
    int line_to_system_lanemap;
    std::string mdio_addr;
} gearbox_lane_t;

typedef struct
{
    int index;
    std::string mdio_addr;
    int system_speed;
	std::string system_fec;
    bool system_auto_neg;
	std::string system_loopback;
	bool system_training;
    int line_speed;
	std::string line_fec;
    bool line_auto_neg;
	std::string line_media_type;
	std::string line_intf_type;
	std::string line_loopback;
	bool line_training;
    std::set<int> line_adver_speed;
    std::set<int> line_adver_fec;
	bool line_adver_auto_neg;
	bool line_adver_asym_pause;
	std::string line_adver_media_type;
} gearbox_port_t;

class GearboxUtils
{
    private:
        std::map<int, gearbox_phy_t> gearboxPhyMap;
        std::map<int, gearbox_interface_t> gearboxInterfaceMap;
        std::map<int, gearbox_lane_t> gearboxLaneMap;
        std::map<int, gearbox_port_t> gearboxPortMap;
        std::tuple <std::string, std::string, std::string> parseGearboxKey(std::string key_str);
    public:
        bool platformHasGearbox();
        bool isGearboxConfigDone(Table &gearboxTable);
        bool isGearboxConfigDone(Table *gearboxTable);
        bool isGearboxEnabled(Table *gearboxTable);
        std::map<int, gearbox_phy_t> loadPhyMap(Table *gearboxTable);
        std::map<int, gearbox_interface_t> loadInterfaceMap(Table *gearboxTable);
        std::map<int, gearbox_lane_t> loadLaneMap(Table *gearboxTable);
        std::map<int, gearbox_port_t> loadPortMap(Table *gearboxTable);
};

}

#endif /* SWSS_COMMON_GEARBOX_UTILS_H */
