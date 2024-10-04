/*
 * Copyright 2024 GlobalLogic.
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

#ifndef __SHLORCH_H__
#define __SHLORCH_H__

#include "orch.h"
#include "observer.h"

#include <isolationgrouporch.h>

#define SHL_VTEPS       "vteps"
#define ISO_GRP_PREFIX  "IsoGrp_"

class ShlOrch : public Orch
{
public:
    ShlOrch(DBConnector *db, const vector<string> &tableNames);

    ~ShlOrch();

private:
    void doTask(Consumer &consumer);

    void doShlTask(Consumer& consumer);
    void doDfTask(Consumer& consumer);

    shared_ptr<IsolationGroup>
    getIsolationGroup(string name_iso_grp);

    bool addIsolationGroupMember(string own_port, string member_port);
    bool delIsolationGroupMember(string own_port, string member_port);

    bool addSplitHorizonList(string lag_name, string vteps);
    bool delSplitHorizonList(string key);

    bool addDfElection(string lag_name, bool df_mode);
    bool delDfElection(string key);

    map<string, shared_ptr<IsolationGroup>> m_ShlGrps;
    map<string, vector<string>> m_ShlGrp_members;
};


#endif /* __SHLORCH_H__ */
