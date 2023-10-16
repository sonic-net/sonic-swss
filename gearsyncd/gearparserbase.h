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

#pragma once

#include "dbconnector.h"
#include "producerstatetable.h"
#include <string>
#include <memory>
#include <vector>


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <nlohmann/json.hpp>
#pragma GCC diagnostic pop

using json = nlohmann::json;

class GearParserBase
{
public:
    GearParserBase();
    virtual ~GearParserBase();
    virtual bool parse() = 0;
    void setWriteToDb(bool val) {m_writeToDb = val;}
    bool getWriteToDb() {return m_writeToDb;}
    void setConfigPath(std::string &path) {m_cfgPath = path;}
    const std::string getConfigPath() {return m_cfgPath;}
    std::unique_ptr<swss::ProducerStateTable> &getProducerStateTable() {return m_producerStateTable;}

protected:
    bool writeToDb(std::string &key, std::vector<swss::FieldValueTuple> &attrs);
    json &getJSONRoot();

private:
    void init();
    std::unique_ptr<swss::DBConnector> m_cfgDb;
    std::unique_ptr<swss::DBConnector> m_applDb;
    std::unique_ptr<swss::DBConnector> m_stateDb;
    std::unique_ptr<swss::ProducerStateTable> m_producerStateTable;
    std::string m_cfgPath;
    bool m_writeToDb;
    json m_root;
    bool m_rootInit;
};

