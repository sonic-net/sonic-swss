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

#include "gearboxutils.h"

namespace swss {

std::tuple <std::string, std::string, std::string> GearboxUtils::parseGearboxKey(std::string key_str)
{
    /*
     * The Gearbox key string has a few formats based on the number of tokens.
     * Parse and return accordingly;
     * phy:<phy_id>:ports:<index> rtn: ports<phy_id><index>
     * phy:<phy_id>:lanes:<index> rtn: lanes<phy_id><index>
     * phy:<phy_id>               rtn: phy<phy_id>
     * interface:<name>           rtn: interface<name>
     */

    std::string str1, str2, str3;
    std::vector<std::string> token = tokenize(key_str, ':');

    if (token.size() == 4)
    {
        str1 = token.at(2);
        str2 = token.at(1);
        str3 = token.at(3);
    }
    else if (token.size() == 2)
    {
        str1 = token.at(0);
        str2 = token.at(1);
    }

    SWSS_LOG_DEBUG("Parsed key:%s, Return:(%s)(%s)(%s)", key_str.c_str(), str1.c_str(), str2.c_str(), str3.c_str());

    return make_tuple(str1, str2, str3);
}

bool GearboxUtils::platformHasGearbox()
{
    bool ret = false;

    if (access("/usr/share/sonic/hwsku/gearbox_config.json", F_OK) != -1)
    {
        ret = true;
    }
    return ret;
}

bool GearboxUtils::isGearboxConfigDone(Table &gearboxTable)
{
    std::vector<FieldValueTuple> tuples;
    bool gearboxConfigDone = false;

    // this will return false if GearboxConfigDone is not set in the database

    gearboxConfigDone = gearboxTable.get("GearboxConfigDone", tuples);
    return gearboxConfigDone;
}

bool GearboxUtils::isGearboxConfigDone(Table *gearboxTable)
{
    std::vector<FieldValueTuple> tuples;
    bool gearboxConfigDone = false;

    // this will return false if GearboxConfigDone is not set in the database

    gearboxConfigDone = gearboxTable->get("GearboxConfigDone", tuples);
    return gearboxConfigDone;
}

bool GearboxUtils::isGearboxEnabled(Table *gearboxTable)
{
    int count = 10;      // 10 seconds is more than enough time
    bool gearboxDone = false;

    SWSS_LOG_ENTER();

    if (platformHasGearbox() != true)
    {
        return gearboxDone;
    }

    // wait until gearbox table is ready
    while (count > 0)
    {
        if (!isGearboxConfigDone(gearboxTable))
        {
            sleep(1);
            count--;
            continue;
        }
        else
        {
            gearboxDone = true;
            break;
        }
    }

    return gearboxDone;
}

std::map<int, gearbox_phy_t> GearboxUtils::loadPhyMap(Table *gearboxTable)
{
    std::vector<FieldValueTuple> ovalues;
    std::tuple <std::string, std::string, std::string> keyt;
    std::vector<std::string> keys;

    SWSS_LOG_ENTER();

    gearboxTable->getKeys(keys);

    if (keys.empty())
    {
        SWSS_LOG_ERROR("No Gearbox records in ApplDB!");
        return gearboxPhyMap;
    }

    for (auto &k : keys)
    {
        keyt = parseGearboxKey(k);

        if (std::get<0>(keyt).compare("phy") == 0)
        {
            gearbox_phy_t phy = {};

            gearboxTable->get(k, ovalues);
            for (auto &val : ovalues)
            {
                if (val.first == "phy_id")
                {
                    phy.phy_id = std::stoi(val.second);
                }
                else if (val.first == "phy_oid")
                {
                    // oid is from create_switch (not config)
                    phy.phy_oid = val.second;
                }
                else if (val.first == "name")
                {
                    phy.name = val.second;
                }
                else if (val.first == "lib_name")
                {
                    phy.lib_name = val.second;
                }
                else if (val.first == "firmware_path")
                {
                    phy.firmware = val.second;
                }
                else if (val.first == "config_file")
                {
                    phy.config_file = val.second;
                }
                else if (val.first == "sai_init_config_file")
                {
                    phy.sai_init_config_file = val.second;
                }
                else if (val.first == "phy_access")
                {
                    phy.access = val.second;
                }
                else if (val.first == "address")
                {
                    phy.address = std::stoi(val.second);
                }
                else if (val.first == "bus_id")
                {
                    phy.bus_id = std::stoi(val.second);
                }
            }
            gearboxPhyMap[phy.phy_id] = phy;
        }
    }

    return gearboxPhyMap;
}

std::map<int, gearbox_interface_t> GearboxUtils::loadInterfaceMap(Table *gearboxTable)
{
    std::vector<FieldValueTuple> ovalues;
    std::tuple <std::string, std::string, std::string> keyt;
    std::vector<std::string> keys;

    SWSS_LOG_ENTER();

    gearboxTable->getKeys(keys);

    if (keys.empty())
    {
        SWSS_LOG_ERROR("No Gearbox records in ApplDB!");
        return gearboxInterfaceMap;
    }

    for (auto &k : keys)
    {
        keyt = parseGearboxKey(k);

        if (std::get<0>(keyt).compare("interface") == 0)
        {
            gearbox_interface_t interface = {};

            gearboxTable->get(k, ovalues);
            for (auto &val : ovalues)
            {
                if (val.first == "index")
                {
                    interface.index = std::stoi(val.second);
                    SWSS_LOG_DEBUG("BOX interface = %d", interface.index);
                }
                else if (val.first == "phy_id")
                {
                    interface.phy_id = std::stoi(val.second);
                    SWSS_LOG_DEBUG("BOX phy_id = %d", interface.phy_id);
                }
               else if (val.first == "line_lanes")
                {
                    std::stringstream ss(val.second);

                    for (int i; ss >> i;)
                    {
                        SWSS_LOG_DEBUG("Parsed key:%s, val:%s,inserted %d", val.first.c_str(), val.second.c_str(), i);
                        interface.line_lanes.insert(i);
                        if (ss.peek() == ',')
                        {
                            ss.ignore();
                        }
                    }
                }
                else if (val.first == "system_lanes")
                {
                    std::stringstream ss(val.second);

                    for (int i; ss >> i;)
                    {
                        SWSS_LOG_DEBUG("Parsed key:%s, val:%s,inserted %d", val.first.c_str(), val.second.c_str(), i);
                        interface.system_lanes.insert(i);
                        if (ss.peek() == ',')
                        {
                            ss.ignore();
                        }
                    }
                }
            }
            gearboxInterfaceMap[interface.index] = interface;
        }
    }

    return gearboxInterfaceMap;
}

std::map<int, gearbox_lane_t> GearboxUtils::loadLaneMap(Table *gearboxTable)
{
    std::vector<FieldValueTuple> ovalues;
    std::tuple <std::string, std::string, std::string> keyt;
    std::vector<std::string> keys;

    SWSS_LOG_ENTER();

    gearboxTable->getKeys(keys);

    if (keys.empty())
    {
        SWSS_LOG_ERROR("No Gearbox records in ApplDB!");
        return gearboxLaneMap;
    }

    for (auto &k : keys)
    {
        keyt = parseGearboxKey(k);

        if (std::get<0>(keyt).compare("lanes") == 0)
        {
            gearbox_lane_t lane = {};

            gearboxTable->get(k, ovalues);
            for (auto &val : ovalues)
            {
                if (val.first == "index")
                {
                    lane.index = std::stoi(val.second);
                }
                else if (val.first == "tx_polarity")
                {
                    lane.tx_polarity = std::stoi(val.second);
                }
                else if (val.first == "rx_polarity")
                {
                    lane.rx_polarity = std::stoi(val.second);
                }
                else if (val.first == "line_tx_lanemap")
                {
                    lane.line_tx_lanemap = std::stoi(val.second);
                }
                else if (val.first == "line_rx_lanemap")
                {
                    lane.line_rx_lanemap = std::stoi(val.second);
                }
                else if (val.first == "line_to_system_lanemap")
                {
                    lane.line_to_system_lanemap = std::stoi(val.second);
                }
                else if (val.first == "mdio_addr")
                {
                    lane.mdio_addr = val.second;
                }
                else if (val.first == "system_side")
                {
                    lane.system_side = (val.second == "true") ? true : false;
                }
            }
            gearboxLaneMap[lane.index] = lane;
        }
    }

    return gearboxLaneMap;
}

std::map<int, gearbox_port_t> GearboxUtils::loadPortMap(Table *gearboxTable)
{
    std::vector<FieldValueTuple> ovalues;
    std::tuple <std::string, std::string, std::string> keyt;
    std::vector<std::string> keys;

    SWSS_LOG_ENTER();

    gearboxTable->getKeys(keys);

    if (keys.empty())
    {
        SWSS_LOG_ERROR("No Gearbox records in ApplDB!");
        return gearboxPortMap;
    }

    for (auto &k : keys)
    {
        keyt = parseGearboxKey(k);

        if (std::get<0>(keyt).compare("ports") == 0)
        {
            gearbox_port_t port = {};

            gearboxTable->get(k, ovalues);
            for (auto &val : ovalues)
            {
                if (val.first == "index")
                {
                    port.index = std::stoi(val.second);
                }
                else if (val.first == "mdio_addr")
                {
                    port.mdio_addr = val.second;
                }
                else if (val.first == "system_speed")
                {
                    port.system_speed = std::stoi(val.second);
                }
                else if (val.first == "system_fec")
                {
                    port.system_fec = val.second;
                }
                else if (val.first == "system_auto_neg")
                {
                    port.system_auto_neg = (val.second == "true") ? true : false;
                }
                else if (val.first == "system_loopback")
                {
                    port.system_loopback = val.second;
                }
                else if (val.first == "system_training")
                {
                    port.system_training = (val.second == "true") ? true : false;
                }
                else if (val.first == "line_speed")
                {
                    port.line_speed = std::stoi(val.second);
                }
                else if (val.first == "line_fec")
                {
                    port.line_fec = val.second;
                }
                else if (val.first == "line_auto_neg")
                {
                    port.line_auto_neg = (val.second == "true") ? true : false;
                }
                else if (val.first == "line_media_type")
                {
                    port.line_media_type = val.second;
                }
                else if (val.first == "line_intf_type")
                {
                    port.line_intf_type = val.second;
                }
                else if (val.first == "line_loopback")
                {
                    port.line_loopback = val.second;
                }
                else if (val.first == "line_training")
                {
                    port.line_training = (val.second == "true") ? true : false;
                }
                else if (val.first == "line_adver_speed")
                {
                    std::stringstream ss(val.second);
                    for (int i; ss >> i;)
                    {
                        port.line_adver_speed.insert(i);
                        if (ss.peek() == ',')
                        {
                            ss.ignore();
                        }
                    }
                }
                else if (val.first == "line_adver_fec")
                {
                    std::stringstream ss(val.second);
                    for (int i; ss >> i;)
                    {
                        port.line_adver_fec.insert(i);
                        if (ss.peek() == ',')
                        {
                            ss.ignore();
                        }
                    }
                }
                else if (val.first == "line_adver_auto_neg")
                {
                    port.line_adver_auto_neg = (val.second == "true") ? true : false;
                }
                else if (val.first == "line_adver_asym_pause")
                {
                    port.line_adver_asym_pause = (val.second == "true") ? true : false;
                }
                else if (val.first == "line_adver_media_type")
                {
                    port.line_adver_media_type = val.second;
                }
            }
            gearboxPortMap[port.index] = port;
        }
    }

    return gearboxPortMap;
}

}
