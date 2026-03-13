/*
 * Copyright 2019 Broadcom.  The term "Broadcom" refers to Broadcom Inc.
 * and/or its subsidiaries.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SWSS_MLAGORCH_H
#define SWSS_MLAGORCH_H

#include <set>
#include <string>
#include "orchagent/orch.h"
#include "orchagent/port.h"

struct MlagIfUpdate
{
    string if_name;
    bool is_add;
};

struct MlagIslUpdate
{
    string isl_name;
    bool is_add;
};

class MlagOrch: public Orch, public Observer, public Subject
{
public:
    MlagOrch(DBConnector *db, vector<string> &tableNames);
    ~MlagOrch();
    void update(SubjectType type, void *cntx);
    bool isMlagInterface(string if_name);
    bool isIslInterface(string if_name);

    const std::set<std::string>&
    getMlagIntfs() const
    {
        return m_mlagIntfs;
    }

private:
    std::string m_isl_name;
    std::set<std::string> m_mlagIntfs;

    void doTask(Consumer &consumer);
    void doMlagDomainTask(Consumer &consumer);
    void doMlagInterfaceTask(Consumer &consumer);
    bool addIslInterface(string isl_name);
    bool delIslInterface();
    bool addMlagInterface(string if_name);
    bool delMlagInterface(string if_name);
};

#endif /* SWSS_MLAGORCH_H */
