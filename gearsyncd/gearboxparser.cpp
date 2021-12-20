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

#include "gearboxparser.h"
#include "phyparser.h"
#include <vector>

void GearboxParser::notifyGearboxConfigDone(bool success)
{
    swss::ProducerStateTable *p = getProducerStateTable().get();

    swss::FieldValueTuple finish_notice("success", std::to_string(success));
    std::vector<swss::FieldValueTuple> attrs = { finish_notice };

    p->set("GearboxConfigDone", attrs);
}

bool GearboxParser::parse()
{
    json root;

    try 
    {
        root = getJSONRoot();
    } 
    catch (const std::exception& e) 
    {
        SWSS_LOG_ERROR("JSON root not parseable");
        return false;
    }

    json phys, phy, interfaces, interface, val, lanes;

    std::vector<swss::FieldValueTuple> attrs;

    try 
    {
        phys = root["phys"];
        if (phys.size() == 0) 
        {
            SWSS_LOG_ERROR("zero-sized 'phys' field in gearbox configuration");
            return false;
        }
    } 
    catch (const std::exception& e) 
    {
        SWSS_LOG_ERROR("unable to read gearbox configuration (invalid format)");
        return false;
    }

    for (uint32_t iter = 0; iter < phys.size(); iter++) 
    {
        phy = phys[iter];
        try 
        {
            attrs.clear();
            swss::FieldValueTuple attr;
            if (phy.find("phy_id") == phy.end()) 
            {
                SWSS_LOG_ERROR("missing 'phy_id' field in 'phys' item %d in gearbox configuration", iter);
                return false;
            }
            val = phy["phy_id"];
            int phyId = val.get<int>();
            attr = std::make_pair("phy_id", std::to_string(phyId));
            attrs.push_back(attr);
            if (phy.find("name") == phy.end()) 
            {
                SWSS_LOG_ERROR("missing 'name' field in 'phys' item %d in gearbox configuration", iter);
                return false;
            }
            val = phy["name"];
            std::string name(val.get<std::string>());
            attr = std::make_pair("name", std::string(val.get<std::string>()));
            attrs.push_back(attr);
            if (phy.find("address") == phy.end()) {
                SWSS_LOG_ERROR("missing 'address' field in 'phys' item %d in gearbox configuration", iter);
                return false;
            }
            val = phy["address"];
            attr = std::make_pair("address", std::string(val.get<std::string>()));
            attrs.push_back(attr);
            if (phy.find("lib_name") == phy.end()) 
            {
                SWSS_LOG_ERROR("missing 'lib_name' field in 'phys' item %d in gearbox configuration", iter);
                return false;
            }
            val = phy["lib_name"];
            attr = std::make_pair("lib_name", std::string(val.get<std::string>()));
            attrs.push_back(attr);
            if (phy.find("firmware_path") == phy.end()) 
            {
                SWSS_LOG_ERROR("missing 'firmware_path' field in 'phys' item %d in gearbox configuration", iter);
                return false;
            }
            val = phy["firmware_path"];
            attr = std::make_pair("firmware_path", std::string(val.get<std::string>()));
            attrs.push_back(attr);
            if (phy.find("config_file") == phy.end()) 
            {
                SWSS_LOG_ERROR("missing 'config_file' field in 'phys' item %d in gearbox configuration", iter);
                return false;
            }
            val = phy["config_file"];
            std::string cfgFile(val.get<std::string>());
            attr = std::make_pair("config_file", cfgFile);
            attrs.push_back(attr);
            if (phy.find("sai_init_config_file") == phy.end()) 
            {
                SWSS_LOG_ERROR("missing 'sai_init_config_file' field in 'phys' item %d in gearbox configuration", iter);
                return false;
            }
            val = phy["sai_init_config_file"];
            std::string bcmCfgFile(std::string(val.get<std::string>()));
            attr = std::make_pair("sai_init_config_file", bcmCfgFile);
            attrs.push_back(attr);
            if (phy.find("phy_access") == phy.end()) 
            {
                SWSS_LOG_ERROR("missing 'phy_access' field in 'phys' item %d in gearbox configuration", iter);
                return false;
            }
            val = phy["phy_access"];
            attr = std::make_pair("phy_access", std::string(val.get<std::string>()));
            attrs.push_back(attr);
            if (phy.find("bus_id") == phy.end()) 
            {
                SWSS_LOG_ERROR("missing 'bus_id' field in 'phys' item %d in gearbox configuration", iter);
                return false;
            }
            val = phy["bus_id"];
            attr = std::make_pair("bus_id", std::to_string(val.get<int>()));
            attrs.push_back(attr);
            if (phy.find("context_id") == phy.end())
            {
                SWSS_LOG_ERROR("missing 'context_id' field in 'phys' item %d in gearbox configuration", iter);
                return false;
            }
            val = phy["context_id"];
            attr = std::make_pair("context_id", std::to_string(val.get<int>()));
            attrs.push_back(attr);
            if (phy.find("macsec_ipg") != phy.end())
            {
                val = phy["macsec_ipg"];
                attr = std::make_pair("macsec_ipg", std::to_string(val.get<int>()));
                attrs.push_back(attr);
            }
            if (phy.find("hwinfo") == phy.end())
            {
                SWSS_LOG_ERROR("missing 'hwinfo' field in 'phys' item %d in gearbox configuration", iter);
                return false;
            }
            val = phy["hwinfo"];
            attr = std::make_pair("hwinfo", std::string(val.get<std::string>()));
            attrs.push_back(attr);
            std::string key;
            key = "phy:" + std::to_string(phyId);
            if (getWriteToDb() == true) 
            {
                writeToDb(key, attrs);
            }
            PhyParser p;
            p.setPhyId(phyId);
            p.setWriteToDb(getWriteToDb());
            p.setConfigPath(cfgFile);
            if (p.parse() == false) 
            {
                SWSS_LOG_ERROR("phy parser failed to parse item %d in gearbox configuration", iter);
                return false;
            }
        } 
        catch (const std::exception& e) 
        {
            SWSS_LOG_ERROR("unable to read 'phys' item %d in gearbox configuration (invalid format)", iter);
            return false;
        }
    } 

    if (root.find("interfaces") != root.end()) 
    {
        interfaces = root["interfaces"]; // vec
        if (interfaces.size() == 0) 
        {
            SWSS_LOG_ERROR("zero-sized 'interfaces' field in gearbox configuration");
            return false;
        }
        for (uint32_t iter = 0; iter < interfaces.size(); iter++) 
        {
            attrs.clear();
            interface = interfaces[iter];
            try 
            {
                swss::FieldValueTuple attr;

                if (interface.find("name") == interface.end()) 
                {
                    SWSS_LOG_ERROR("missing 'name' field in 'interfaces' item %d in gearbox configuration", iter);
                    return false;
                }
                val = interface["name"];
                attr = std::make_pair("name", std::string(val.get<std::string>()));
                attrs.push_back(attr);

                if (interface.find("index") == interface.end()) 
                {
                    SWSS_LOG_ERROR("missing 'index' field in 'interfaces' item %d in gearbox configuration", iter);
                    return false;
                }
                val = interface["index"];
                int index = val.get<int>();
                attr = std::make_pair("index", std::to_string(index));
                attrs.push_back(attr);

                if (interface.find("phy_id") == interface.end()) 
                {
                    SWSS_LOG_ERROR("missing 'phy_id' field in 'interfaces' item %d in gearbox configuration", iter);
                    return false;
                }
                val = interface["phy_id"];
                attr = std::make_pair("phy_id", std::to_string(val.get<int>()));
                attrs.push_back(attr);

                if (interface.find("system_lanes") != interface.end()) 
                {
                    lanes = interface["system_lanes"]; // vec
                    std::string laneStr("");
                    if (lanes.size() == 0) 
                    {
                        SWSS_LOG_ERROR("zero-sized 'system_lanes' field in 'interfaces' item %d in gearbox configuration", iter);
                        return false;
                    }
                    for (uint32_t iter2 = 0; iter2 < lanes.size(); iter2++) 
                    {
                        val = lanes[iter2];
                        if (laneStr.length() > 0) 
                        {
                            laneStr += ",";
                        }
                        laneStr += std::to_string(val.get<int>());
                    }
                    attr = std::make_pair("system_lanes", laneStr);
                    attrs.push_back(attr);
                } 
                else 
                {
                    SWSS_LOG_ERROR("missing 'system_lanes' field in 'interfaces' item %d in gearbox configuration", iter);
                    return false;
                }

                if (interface.find("line_lanes") != interface.end()) 
                {
                    lanes = interface["line_lanes"]; // vec
                    std::string laneStr("");
                    if (lanes.size() == 0) 
                    {
                        SWSS_LOG_ERROR("zero-sized 'line_lanes' field in 'interfaces' item %d in gearbox configuration", iter);
                        return false;
                    }
                    for (uint32_t iter2 = 0; iter2 < lanes.size(); iter2++) 
                    {
                        val = lanes[iter2];
                        if (laneStr.length() > 0) 
                        {
                            laneStr += ",";
                        }
                        laneStr += std::to_string(val.get<int>());
                    }
                    attr = std::make_pair("line_lanes", laneStr);
                    attrs.push_back(attr);
                } 
                else 
                {
                    SWSS_LOG_ERROR("missing 'line_lanes' field in 'interfaces' item %d in gearbox configuration", iter);
                    return false;
                }
                std::string key;
                key = "interface:" + std::to_string(index);
                if (getWriteToDb() == true) 
                {
                    writeToDb(key, attrs);
                }
            } 
            catch (const std::exception& e) 
            {
                SWSS_LOG_ERROR("unable to read 'interfaces' item %d in gearbox configuration (invalid format)", iter);
                return false;
            }
        }
    } 
    else 
    {
        SWSS_LOG_ERROR("unable to read 'interfaces' item in gearbox configuration");
        return false;
    }
    return true;
}
