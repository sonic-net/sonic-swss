#include <string.h>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "vrfmgr.h"
#include "exec.h"
#include "shellcmd.h"

#define VRF_TABLE_START 1001
#define VRF_TABLE_END 2000
#define OBJ_PREFIX "OBJ"

using namespace swss;

VrfMgr::VrfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_appVrfTableProducer(appDb, APP_VRF_TABLE_NAME),
        m_appVnetTableProducer(appDb, APP_VNET_TABLE_NAME),
        m_stateVrfTable(stateDb, STATE_VRF_TABLE_NAME)
{
    for (uint32_t i = VRF_TABLE_START; i < VRF_TABLE_END; i++)
    {
        m_freeTables.emplace(i);
    }

    /* Get existing VRFs from Linux */
    stringstream cmd;
    string res;

    cmd << IP_CMD << " -d link show type vrf";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    enum IpShowRowType
    {
        LINK_ROW,
        MAC_ROW,
        DETAILS_ROW,
    };

    string vrfName;
    uint32_t table;
    IpShowRowType rowType = LINK_ROW;
    const auto& rows = tokenize(res, '\n');
    for (const auto& row : rows)
    {
        const auto& items = tokenize(row, ' ');
        switch(rowType)
        {
            case LINK_ROW:
                vrfName = items[1];
                vrfName.pop_back();
                rowType = MAC_ROW;
                break;
            case MAC_ROW:
                rowType = DETAILS_ROW;
                break;
            case DETAILS_ROW:
                table = static_cast<uint32_t>(stoul(items[6]));
                m_vrfTableMap[vrfName] = table;
                m_freeTables.erase(table);
                rowType = LINK_ROW;
                break;
        }
    }

    cmd.str("");
    cmd.clear();
    cmd << IP_CMD << " rule | grep '^0:'";
    if (swss::exec(cmd.str(), res) == 0)
    {
        cmd.str("");
        cmd.clear();
        cmd << IP_CMD << " rule add pref 1001 table local && " << IP_CMD << " rule del pref 0 && "
            << IP_CMD << " -6 rule add pref 1001 table local && " << IP_CMD << " -6 rule del pref 0";
        EXEC_WITH_ERROR_THROW(cmd.str(), res);
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
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

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
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    m_vrfTableMap.emplace(vrfName, table);

    cmd.str("");
    cmd.clear();
    cmd << IP_CMD << " link set " << vrfName << " up";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    return true;
}

bool VrfMgr::isVrfObjExist(const string& vrfName)
{
    vector<FieldValueTuple> temp;
    string key = string(OBJ_PREFIX) + state_db_key_delimiter + vrfName;

    if (m_stateVrfTable.get(key, temp))
    {
        SWSS_LOG_DEBUG("Vrf %s object exist", vrfName.c_str());
        return true;
    }

    return false;
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

            vector<FieldValueTuple> fvVector;
            fvVector.emplace_back("state", "ok");
            m_stateVrfTable.set(vrfName, fvVector);

            SWSS_LOG_NOTICE("Created vrf netdev %s", vrfName.c_str());
            if (consumer.getTableName() == CFG_VRF_TABLE_NAME)
            {
                m_appVrfTableProducer.set(vrfName, kfvFieldsValues(t));
            }
            else
            {
                m_appVnetTableProducer.set(vrfName, kfvFieldsValues(t));
            }
        }
        else if (op == DEL_COMMAND)
        {
            vector<FieldValueTuple> temp;

            if (m_stateVrfTable.get(vrfName, temp))
            {
                if (!isVrfObjExist(vrfName))
                {
                    it++;
                    continue;
                }

                m_stateVrfTable.del(vrfName);

                if (consumer.getTableName() == CFG_VRF_TABLE_NAME)
                {
                    m_appVrfTableProducer.del(vrfName);
                }
                else
                {
                    m_appVnetTableProducer.del(vrfName);
                }
            }

            if (isVrfObjExist(vrfName))
            {
                it++;
                continue;
            }

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
