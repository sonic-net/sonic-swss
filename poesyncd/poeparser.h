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

#pragma once

#include <vector>
#include <string>
#include "dbconnector.h"
#include "producerstatetable.h"

// #pragma GCC diagnostic push
// #pragma GCC diagnostic ignored "-Wshadow"
#include <nlohmann/json.hpp>
// #pragma GCC diagnostic pop

using json = nlohmann::json;

class PoeParser
{
public:
    PoeParser(const std::string &configPath);
    bool loadConfig();
    bool storeConfigToDb();
    bool writeToDb(swss::ProducerStateTable &table,
                   const std::string &key,
                   const std::vector<swss::FieldValueTuple> &vals);
    void notifyConfigDone(bool success);
private:
    swss::DBConnector m_applDb;
    swss::DBConnector m_cfgDb;
    swss::ProducerStateTable m_poeTable;
    std::string m_cfgPath;
    json m_cfg;
};
