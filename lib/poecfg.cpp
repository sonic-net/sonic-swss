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

#include "poecfg.h"
#include "tokenize.h"

namespace swss {

std::string PoeConfig::parseKey(const std::string &key_str)
{
    std::vector<std::string> token = tokenize(key_str, ':');
    std::string type, idx;

    if (token.size() == 1)
    {
        if (token[0].rfind("Ethernet", 0) != 0)
        {
            SWSS_LOG_WARN("Failed to parse key '%s'", key_str.c_str());
            return "";
        }
        type = "port";
        idx = token.at(0);
    }
    else if (token.size() == 2)
    {
        type = token.at(0);
        idx = token.at(1);
    }
    else
    {
        SWSS_LOG_WARN("Failed to parse key '%s'", key_str.c_str());
        return "";
    }

    SWSS_LOG_NOTICE("Parsed key: '%s', Return: (%s)(%s)", key_str.c_str(), type.c_str(), idx.c_str());

    return type;
}

PoeConfig::PoeConfig(Table *poe_table)
{
    poeTable = poe_table;
}

PoeConfig::PoeConfig(Table &poe_table)
{
    poeTable = &poe_table;
}

bool PoeConfig::isConfigDone()
{
    std::vector<FieldValueTuple> tuples;

    if (!poeTable->get("PoeConfigDone", tuples))
    {
        return false;
    }

    for (auto& el : tuples)
    {
        if (el.first == "success" && el.second == "true")
        {
            return true;
        }
    }
    return false;
}

bool PoeConfig::platformHasPoe()
{
    /* check if file exists */
    return access("/usr/share/sonic/hwsku/poe_config.json", F_OK) != -1;
}

bool PoeConfig::isPoeEnabled()
{
    int count_s = 10;

    SWSS_LOG_ENTER();
    if (!platformHasPoe())
    {
        return false;
    }

    while (count_s > 0)  /* wait for 10s */
    {
        if (isConfigDone())
        {
            return true;
        }
        sleep(1);
        count_s--;
    }

    return false;
}

bool PoeConfig::loadDeviceConfig(const std::vector<FieldValueTuple> &ovalues)
{
    poe_device_t dev = {};

    for (auto &val : ovalues)
    {
        try
        {
            if (val.first == "oid")
            {
                dev.oid = val.second;
            }
            else if (val.first == "id")
            {
                    dev.id = (uint32_t)std::stoul(val.second);
            }
            else if (val.first == "hw_info")
            {
                dev.hwInfo = val.second;
            }
            else if (val.first == "power_limit_mode")
            {
                dev.powerLimitMode = val.second;
            }
            else
            {
                SWSS_LOG_ERROR("Unknown config key '%s'", val.first.c_str());
                return false;
            }
        }
        catch (std::invalid_argument const&)
        {
            SWSS_LOG_ERROR("Failed to parse value '%s' for key '%s'", val.second.c_str(), val.first.c_str());
            return false;
        }
    }
    poeDeviceMap[dev.id] = dev;
    return true;
}

bool PoeConfig::loadPseConfig(const std::vector<FieldValueTuple> &ovalues)
{
    poe_pse_t pse = {};

    for (auto &val : ovalues)
    {
        try
        {
            if (val.first == "oid")
            {
                pse.oid = val.second;
            }
            else if (val.first == "device_id")
            {
                    pse.deviceId = (uint32_t)std::stoul(val.second);
            }
            else if (val.first == "pse_index")
            {
                pse.pseIndex = (uint32_t)std::stoul(val.second);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown config key '%s'", val.first.c_str());
                return false;
            }
        }
        catch (std::invalid_argument const&)
        {
            SWSS_LOG_ERROR("Failed to parse value '%s' for key '%s'", val.second.c_str(), val.first.c_str());
            return false;
        }
    }
    poePseMap[pse.pseIndex] = pse;
    return true;
}

bool PoeConfig::loadPortConfig(const std::vector<FieldValueTuple> &ovalues)
{
    poe_port_t port = {};

    for (auto &val : ovalues)
    {
        try
        {
            if (val.first == "oid")
            {
                port.oid = val.second;
            }
            else if (val.first == "device_id")
            {
                port.deviceId = (uint32_t)std::stoul(val.second);
            }
            else if (val.first == "front_panel_index")
            {
                port.frontPanelIndex = (uint32_t)std::stoul(val.second);
            }
            else if (val.first == "interface")
            {
                port.interface = val.second;
            }
            else if (val.first == "pwr_limit")
            {
                port.powerLimit = (uint32_t)std::stoul(val.second);
            }
            else if (val.first == "priority")
            {
                port.powerPriority = val.second;
            }
            else if (val.first == "enabled")
            {
                port.enable = val.second == "enable";
            }
            else
            {
                SWSS_LOG_ERROR("Unknown config key '%s'", val.first.c_str());
                return false;
            }
        }
        catch (std::invalid_argument const&)
        {
            SWSS_LOG_ERROR("Failed to parse value '%s' for key '%s'", val.second.c_str(), val.first.c_str());
            return false;
        }
    }
    poePortMap[port.frontPanelIndex] = port;
    return true;
}

bool PoeConfig::loadConfig()
{
    std::vector<FieldValueTuple> ovalues;
    std::vector<std::string> keys;

    SWSS_LOG_ENTER();

    poeTable->getKeys(keys);

    if (keys.empty())
    {
        SWSS_LOG_ERROR("No PoE records in ApplDB!");
        return false;
    }

    for (auto &k : keys)
    {
        if (k == "PoeConfigDone")
        {
            /* ignore this key */
            continue;
        }

        std::string type = parseKey(k);
        poeTable->get(k, ovalues);

        if (type == "dev")
        {
            if (!loadDeviceConfig(ovalues))
            {
                SWSS_LOG_ERROR("Failed to parse PoE device config");
                return false;
            }
        }
        else if (type == "pse")
        {
            if (!loadPseConfig(ovalues))
            {
                SWSS_LOG_ERROR("Failed to parse PoE pse config");
                return false;
            }
        }
        else if (type == "port")
        {
            if (!loadPortConfig(ovalues))
            {
                SWSS_LOG_ERROR("Failed to parse PoE port config");
                return false;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown PoE type '%s'", type.c_str());
            return false;
        }
    }
    SWSS_LOG_NOTICE("poeDeviceMap size: %lu", poeDeviceMap.size());
    SWSS_LOG_NOTICE("poePseMap size: %lu", poePseMap.size());
    SWSS_LOG_NOTICE("poePortMap size: %lu", poePortMap.size());
    return true;
}

}
