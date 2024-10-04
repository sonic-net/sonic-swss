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

#ifndef __EVPNMHORCH_H__
#define __EVPNMHORCH_H__

#include "orch.h"
#include "observer.h"

#include <isolationgrouporch.h>

#define DF_MODE_FIELD  "df"

class EvpnMhOrch : public Orch
{
public:
    EvpnMhOrch(DBConnector *db, const vector<string> &tableNames);

    ~EvpnMhOrch();

private:
    void doTask(Consumer &consumer);

    void doDfTask(Consumer& consumer);

    bool setDfElection(string lag_port, bool non_df_mode);
};


#endif /* __EVPNMHORCH_H__ */
