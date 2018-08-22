#include <string>

#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "portmgr.h"
#include "exec.h"
#include "shellcmd.h"

using namespace std;
using namespace swss;

PortMgr::PortMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgPortTable(cfgDb, CFG_PORT_TABLE_NAME),
        m_cfgLagTable(cfgDb, CFG_LAG_TABLE_NAME),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
        m_appPortTable(appDb, APP_PORT_TABLE_NAME),
        m_appLagTable(appDb, APP_LAG_TABLE_NAME)
{
}

bool PortMgr::setPortMtu(const string &table, const string &alias, const string &mtu)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link set dev " << alias << " mtu " << mtu;
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    if (table == CFG_PORT_TABLE_NAME)
    {
        // Set the port MTU in application database to update both
        // the port MTU and possibly the port based router interface MTU
        vector<FieldValueTuple> fvs;
        FieldValueTuple fv("mtu", mtu);
        fvs.push_back(fv);
        m_appPortTable.set(alias, fvs);
    }
    else if (table == CFG_LAG_TABLE_NAME)
    {
        // Set the port channel MTU in application database to update
        // the LAG based router interface MTU in orchagent
        vector<FieldValueTuple> fvs;
        FieldValueTuple fv("mtu", mtu);
        fvs.push_back(fv);
        m_appLagTable.set(alias, fvs);

        m_cfgLagTable.get(alias, fvs);
        for (auto fv: fvs)
        {
            // Set the port channel members MTU in application database
            // to update the port MTU in orchagent
            if (fvField(fv) == "members")
            {
                for (auto member : tokenize(fvValue(fv), ','))
                {
                    vector<FieldValueTuple> member_fvs;
                    FieldValueTuple member_fv("mtu", mtu);
                    member_fvs.push_back(member_fv);
                    m_appPortTable.set(member, member_fvs);
                }
            }
        }
    }

    return true;
}

bool PortMgr::setPortAdminStatus(const string &alias, const bool up)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " link set dev " << alias << (up ? " up" : " down");
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    return true;
}

bool PortMgr::isPortStateOk(const string &table, const string &alias)
{
    vector<FieldValueTuple> temp;

    if (table == CFG_PORT_TABLE_NAME)
    {
        if (m_statePortTable.get(alias, temp))
        {
            SWSS_LOG_INFO("Port %s is ready", alias.c_str());
            return true;
        }
    }
    else if (table == CFG_LAG_TABLE_NAME)
    {
        if (m_stateLagTable.get(alias, temp))
        {
            SWSS_LOG_INFO("Lag %s is ready", alias.c_str());
            return true;
        }
    }

    return false;
}

void PortMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto table = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            if (!isPortStateOk(table, alias))
            {
                SWSS_LOG_INFO("Port %s is not ready, pending...", alias.c_str());
                it++;
                continue;
            }

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "mtu")
                {
                    auto mtu = fvValue(i);
                    setPortMtu(table, alias, mtu);
                    SWSS_LOG_NOTICE("Configure %s MTU to %s",
                                    alias.c_str(), mtu.c_str());
                }
                else if (fvField(i) == "admin_status")
                {
                    auto status = fvValue(i);
                    setPortAdminStatus(alias, status == "up");
                    SWSS_LOG_NOTICE("Configure %s %s",
                            alias.c_str(), status.c_str());
                }
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}
