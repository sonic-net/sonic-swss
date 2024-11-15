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

#include <string>
#include <vector>

#include "evpnmhorch.h"
#include "intfsorch.h"
#include "tokenize.h"

using namespace std;
using namespace swss;

extern PortsOrch*           gPortsOrch;
extern sai_bridge_api_t*    sai_bridge_api;

EvpnMhOrch::EvpnMhOrch(DBConnector *db, const vector<string> &tableNames) : Orch(db, tableNames)
{
    SWSS_LOG_ENTER();
}

EvpnMhOrch::~EvpnMhOrch()
{
    SWSS_LOG_ENTER();
}

void EvpnMhOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    string table_name = consumer.getTableName();
    if (table_name == APP_EVPN_DF_TABLE_NAME)
    {
        doDfTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("EvpnMhOrch receives invalid table %s", table_name.c_str());
    }
}

void EvpnMhOrch::doDfTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    string attr_value;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string op = kfvOp(t);
        string key = kfvKey(t);

        size_t sep_loc = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        string name = key.substr(0, sep_loc);

        if (op == SET_COMMAND)
        {
            for (auto i : kfvFieldsValues(t))
            {
                string attr_name = fvField(i);
                if (attr_name == DF_MODE_FIELD)
                {
                    attr_value = fvValue(i);
                    break;
                }
            }
            if (!attr_value.empty())
            {
                if (setDfElection(name, attr_value=="false"?true:false))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
                it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (setDfElection(name, false))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool EvpnMhOrch::setDfElection(string lag_port, bool non_df_mode) 
{
    SWSS_LOG_ENTER();
    bool status = true;
    sai_attribute_t attr;
    sai_status_t    sai_status;

    do {
        try {
            Port port;
            if (!gPortsOrch->getPort(lag_port, port))
            {
                SWSS_LOG_ERROR("Failed to locate port %s", lag_port.c_str());
                status = false;
                break;
            }

            if (port.m_bridge_port_id) {
                attr.id = SAI_BRIDGE_PORT_ATTR_NON_DF;
                attr.value.booldata = non_df_mode;
                sai_status = sai_bridge_api->set_bridge_port_attribute(port.m_bridge_port_id, &attr);
                if (sai_status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to set df-mode attr to bridge port id 0x%lx "
                                   "status %d",
                                   port.m_bridge_port_id,
                                   sai_status);
                    status = false;
                    break;
                }
            }
        }
        catch (exception &e)
        {
            SWSS_LOG_ERROR("Exception: %s", e.what());
            status = false;
        }
    } while(0);

    return status;
}
