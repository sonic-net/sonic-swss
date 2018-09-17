#include <string.h>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "vrfmgr.h"
#include "exec.h"
#include "shellcmd.h"

#define VRF_TABLE_START 100
#define VRF_TABLE_END 164

using namespace swss;

VrfMgr::VrfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames)
{
    for (uint32_t i = VRF_TABLE_START; i < VRF_TABLE_END; i++)
    {
        m_freeTables.emplace(i);
    }
}

uint32_t VrfMgr::getFreeTable(void)
{
    SWSS_LOG_ENTER();

    if (m_freeTables.empty())
    {
        return 0;
    }

    uint32_t table = *m_freeTables.begin();
    m_freeTables.erase(table);

    return table;
}

void VrfMgr::recycleTable(uint32_t table)
{
    SWSS_LOG_ENTER();

    m_freeTables.emplace(table);
}

bool VrfMgr::delLink(const string& vrfName)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    if (m_vrfTableMap.find(vrfName) == m_vrfTableMap.end())
    {
        return false;
    }

    cmd << IP_CMD << " link del " << vrfName;
    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Failed command %s, rc:%d", cmd.str().c_str(), ret);
        return false;
    }

    recycleTable(m_vrfTableMap[vrfName]);
    m_vrfTableMap.erase(vrfName);

    return true;
}

bool VrfMgr::setLink(const string& vrfName)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    if (m_vrfTableMap.find(vrfName) != m_vrfTableMap.end())
    {
        return true;
    }

    uint32_t table = getFreeTable();
    if (table == 0)
    {
        return false;
    }

    cmd << IP_CMD << " link add " << vrfName << " type vrf table " << table;
    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Failed command %s, rc:%d", cmd.str().c_str(), ret);
        return false;
    }

    m_vrfTableMap.emplace(vrfName, table);

    cmd.str("");
    cmd.clear();
    cmd << IP_CMD << " link set " << vrfName << " up";
    ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Failed command %s, rc:%d", cmd.str().c_str(), ret);
        return false;
    }

    return true;
}

void VrfMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        auto vrfName = kfvKey(t);

        string op = kfvOp(t);
        if (op == SET_COMMAND)
        {
            if (!setLink(vrfName))
            {
                SWSS_LOG_ERROR("Failed to create vrf netdev %s", vrfName.c_str());
            }

            SWSS_LOG_NOTICE("Created vrf netdev %s", vrfName.c_str());
        }
        else if (op == DEL_COMMAND)
        {
            if (!delLink(vrfName))
            {
                SWSS_LOG_ERROR("Failed to remove vrf netdev %s", vrfName.c_str());
            }

            SWSS_LOG_NOTICE("Removed vrf netdev %s", vrfName.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}
