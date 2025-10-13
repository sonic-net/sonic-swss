/*
 * Copyright 2020 Broadcom Inc.
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

#include "gearparserbase.h"
#include <iostream>
#include <fstream>
#include <sstream>

void
GearParserBase::init() 
{
    m_writeToDb = false;
    m_rootInit = false;
    m_applDb = std::unique_ptr<swss::DBConnector>{new swss::DBConnector("APPL_DB", 0)};
    m_producerStateTable = std::unique_ptr<swss::ProducerStateTable>{new swss::ProducerStateTable(m_applDb.get(), APP_GEARBOX_TABLE_NAME)};
}

GearParserBase::GearParserBase() 
{
    init();
}

GearParserBase::~GearParserBase() 
{
}

json & GearParserBase::getJSONRoot()
{
    // lazy instantiate

    if (m_rootInit == false) 
    {
        std::ifstream infile(getConfigPath());
        if (infile.is_open())
        {
            std::string jsonBuffer;
            std::string line;

            while (getline(infile, line)) 
            {
                jsonBuffer += line;
            }
            infile.close();

            m_root = json::parse(jsonBuffer.c_str());
            m_rootInit = true;
        }
    }
    return m_root;
}

bool GearParserBase::writeToDb(std::string &key, std::vector<swss::FieldValueTuple> &attrs)
{
    m_producerStateTable.get()->set(key.c_str(), attrs); 
    return true;
}
