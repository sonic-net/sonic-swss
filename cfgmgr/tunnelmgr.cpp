#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <net/if.h>

#include "logger.h"
#include "tunnelmgr.h"

using namespace std;
using namespace swss;

TunnelMgr::TunnelMgr(DBConnector *cfgDb, DBConnector *appDb, const vector<std::string> &tables) :
        Orch(cfgDb, tables),
        m_appIpInIpTunnelTable(appDb, APP_TUNNEL_DECAP_TABLE_NAME)
{
}

void TunnelMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        bool task_result = false;

        KeyOpFieldsValuesTuple t = it->second;
        const vector<FieldValueTuple>& data = kfvFieldsValues(t);

        const std::string & op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            for (auto idx : data)
            {
                const auto &field = fvField(idx);
                const auto &value = fvValue(idx);
                if (field == "tunnel_type")
                {
                    if (value == "IPINIP")
                    {
                        task_result = doIpInIpTunnelTask(t);
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            /* TODO: Handle Tunnel delete for other tunnel types */
            task_result = doIpInIpTunnelTask(t);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation: '%s'", op.c_str());
        }

        if (task_result == true)
        {
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool TunnelMgr::doIpInIpTunnelTask(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();

    const std::string & TunnelName = kfvKey(t);
    const std::string & op = kfvOp(t);

    if (op == SET_COMMAND)
    {
        m_appIpInIpTunnelTable.set(TunnelName, kfvFieldsValues(t));

    }
    else
    {
        m_appIpInIpTunnelTable.del(TunnelName);
    }
    
    SWSS_LOG_NOTICE("Tunnel %s task, op %s", TunnelName.c_str(), op.c_str());
    return true;
}
