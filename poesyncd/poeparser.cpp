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

#include "poeparser.h"
#include "schema.h"
#include <fstream>


PoeParser::PoeParser(const std::string &configPath) :
        m_cfgPath(configPath),
        m_applDb(swss::DBConnector(APPL_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0)),
        m_cfgDb(swss::DBConnector(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0)),
        m_poeTable(swss::ProducerStateTable(&m_applDb, APP_POE_TABLE_NAME))
{
}

void PoeParser::notifyConfigDone(bool success)
{
    swss::FieldValueTuple notice("success", success ? "true" : "false");
    std::vector<swss::FieldValueTuple> attrs = { notice };
    writeToDb(m_poeTable, "PoeConfigDone", attrs);
}

bool PoeParser::writeToDb(swss::ProducerStateTable &table,
                          const std::string &key,
                          const std::vector<swss::FieldValueTuple> &vals)
{
    table.set(key.c_str(), vals);
    return true;
}

bool PoeParser::loadConfig()
{
    std::ifstream file(m_cfgPath);

    if (!file.is_open())
    {
        return false;
    }
    std::string contents;

    file.seekg(0, std::ios::end);
    contents.resize(file.tellg());
    file.seekg(0, std::ios::beg);
    file.read(&contents[0], contents.size());
    file.close();

    m_cfg = json::parse(contents.c_str());
    return true;
}

bool PoeParser::storeConfigToDb()
{
    std::string sep = m_poeTable.getTableNameSeparator();
    std::vector<swss::FieldValueTuple> attrs;
    json devs, dev, ports, port, val;

    if (!m_cfg.is_array())
    {
        SWSS_LOG_ERROR("Expected PoE config root to be an array");
        return false;
    }

    for (uint32_t devId = 0; devId < m_cfg.size(); devId++)
    {
        swss::FieldValueTuple attr;
        attrs.clear();

        dev = m_cfg[devId];

        attr = std::make_pair("id", std::to_string(devId));
        attrs.push_back(attr);

        if (dev.find("hw_info") == dev.end() || dev["hw_info"].empty())
        {
            SWSS_LOG_ERROR("Missing mandatory field 'hw_info'");
            return false;
        }
        attr = std::make_pair("hw_info", dev["hw_info"]);
        attrs.push_back(attr);

        if (dev.find("power_limit_mode") != dev.end())
        {
            attr = std::make_pair("power_limit_mode", dev["power_limit_mode"]);
            attrs.push_back(attr);
        }

        writeToDb(m_poeTable, "dev" + sep + std::to_string(devId), attrs);

        for (auto& pse : dev["pse_list"])
        {
            attrs.clear();

            attr = std::make_pair("device_id", std::to_string(devId));
            attrs.push_back(attr);

            if (pse.find("pse_index") == pse.end())
            {
                SWSS_LOG_ERROR("Missing mandatory field 'pse_index'");
                return false;
            }
            auto pseIndex = pse["pse_index"].get<uint32_t>();
            attr = std::make_pair("pse_index", std::to_string(pseIndex));
            attrs.push_back(attr);

            writeToDb(m_poeTable, "pse" + sep + std::to_string(pseIndex), attrs);
        }

        for (auto& port : dev["port_mapping_list"])
        {
            attrs.clear();

            attr = std::make_pair("device_id", std::to_string(devId));
            attrs.push_back(attr);

            if (port.find("front_panel_index") == port.end())
            {
                SWSS_LOG_ERROR("Missing mandatory field 'front_panel_index'");
                return false;
            }
            auto frontPanelIndex = port["front_panel_index"].get<uint32_t>();
            attr = std::make_pair("front_panel_index", std::to_string(frontPanelIndex));
            attrs.push_back(attr);

            if (port.find("interface") == port.end())
            {
                SWSS_LOG_ERROR("Missing mandatory field 'interface'");
                return false;
            }
            auto ifname = port["interface"].get<std::string>();
            attr = std::make_pair("interface", ifname);
            attrs.push_back(attr);

            if (port.find("power_limit") != port.end())
            {
                attr = std::make_pair("pwr_limit", std::to_string(port["power_limit"].get<uint32_t>()));
                attrs.push_back(attr);
            }

            if (port.find("power_priority") != port.end())
            {
                attr = std::make_pair("priority", port["power_priority"]);
                attrs.push_back(attr);
            }

            writeToDb(m_poeTable, ifname, attrs);
        }
    }
    return true;
}
