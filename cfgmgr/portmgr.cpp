#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "portmgr.h"
#include "exec.h"
#include "shellcmd.h"
#include <swss/redisutility.h>

using namespace std;
using namespace swss;

PortMgr::PortMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgPortTable(cfgDb, CFG_PORT_TABLE_NAME),
        m_cfgLagMemberTable(cfgDb, CFG_LAG_MEMBER_TABLE_NAME),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_appPortTable(appDb, APP_PORT_TABLE_NAME)
{
}

bool PortMgr::setPortMtu(const string &alias, const string &mtu)
{
    stringstream cmd;
    string res;

    // ip link set dev <port_name> mtu <mtu>
    cmd << IP_CMD << " link set dev " << shellquote(alias) << " mtu " << shellquote(mtu);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    // Set the port MTU in application database to update both
    // the port MTU and possibly the port based router interface MTU
    return writeConfigToAppDb(alias, "mtu", mtu);
}

bool PortMgr::setPortAdminStatus(const string &alias, const bool up)
{
    stringstream cmd;
    string res;

    // ip link set dev <port_name> [up|down]
    cmd << IP_CMD << " link set dev " << shellquote(alias) << (up ? " up" : " down");
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    return writeConfigToAppDb(alias, "admin_status", (up ? "up" : "down"));
}

bool PortMgr::isPortStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (m_statePortTable.get(alias, temp))
    {
        auto state_opt = swss::fvsGetValue(temp, "state", true);
        if (!state_opt)
        {
            return false;
        }

        SWSS_LOG_INFO("Port %s is ready", alias.c_str());
        return true;
    }

    return false;
}

void PortMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    std::vector<FieldValueTuple> retryFields;
    auto table = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            /* portOk=true indicates that the port has been created in kernel.
             * We should not call any ip command if portOk=false. However, it is
             * valid to put port configuration to APP DB which will trigger port creation in kernel.
             */
            bool portOk = isPortStateOk(alias);

            string admin_status, mtu;
            std::vector<FieldValueTuple> field_values;

            bool configured = (m_portList.find(alias) != m_portList.end());

            /* If this is the first time we set port settings
             * assign default admin status and mtu
             */
            if (!configured)
            {
                admin_status = DEFAULT_ADMIN_STATUS_STR;
                mtu = DEFAULT_MTU_STR;

                m_portList.insert(alias);
            }

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "mtu")
                {
                    mtu = fvValue(i);
                }
                else if (fvField(i) == "admin_status")
                {
                    admin_status = fvValue(i);
                }
                else 
                {
                    field_values.emplace_back(i);
                }
            }

            if (!mtu.empty())
            {
                if (portOk)
                {
                    removeFromRetry(alias, "mtu");
                    setPortMtu(alias, mtu);
                }
                else
                {
                    if (addToRetry(alias, "mtu", mtu))
                    {
                        writeConfigToAppDb(alias, "mtu", mtu);
                    }
                    retryFields.emplace_back("mtu", mtu);
                }
                SWSS_LOG_NOTICE("Configure %s MTU to %s", alias.c_str(), mtu.c_str());
            }

            if (!admin_status.empty())
            {
                if (portOk)
                {
                    removeFromRetry(alias, "admin_status");
                    setPortAdminStatus(alias, admin_status == "up");
                }
                else
                {
                    if (addToRetry(alias, "admin_status", admin_status))
                    {
                         writeConfigToAppDb(alias, "admin_status", admin_status);
                    }
                    retryFields.emplace_back("admin_status", admin_status);
                }
                SWSS_LOG_NOTICE("Configure %s admin status to %s", alias.c_str(), admin_status.c_str());
            }

            for (auto &entry : field_values)
            {
                writeConfigToAppDb(alias, fvField(entry), fvValue(entry));
                SWSS_LOG_NOTICE("Configure %s %s to %s", alias.c_str(), fvField(entry).c_str(), fvValue(entry).c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("Delete Port: %s", alias.c_str());
            m_appPortTable.del(alias);
            m_portList.erase(alias);
        }

        if (retryFields.empty()) // TODO: test delete & retry
        {
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            /* There are some fields require retry due to set failure. This is usually because 
             * port has not been created in kernel so that mtu and admin_status configuration 
             * cannot be synced between kernel and  ASIC for now. In this case, we put the retry fields 
             * back to m_toSync and wait for re-visit later.
             */
            it->second = KeyOpFieldsValuesTuple{alias, SET_COMMAND, retryFields};
            ++it;
            retryFields.clear();
        }
    }
}

bool PortMgr::writeConfigToAppDb(const std::string &alias, const std::string &field, const std::string &value)
{
    vector<FieldValueTuple> fvs;
    FieldValueTuple fv(field, value);
    fvs.push_back(fv);
    m_appPortTable.set(alias, fvs);

    return true;
}

bool PortMgr::addToRetry(const std::string &alias, const std::string &field, const std::string &value)
{
    auto iter = m_retryMap.find(alias);
    if (iter == m_retryMap.end())
    {
        m_retryMap.emplace(alias, std::map<std::string, std::string>{{field, value}});
        return true;
    }

    auto ret = iter->second.emplace(field, value);
    if (ret.second)
    {
        return true;
    }
    else
    {
        if (ret.first->second == value)
        {
            return false;
        }

        ret.first->second = value;
        return true;
    }
}

void PortMgr::removeFromRetry(const std::string &alias, const std::string &field)
{
    auto iter = m_retryMap.find(alias);
    if (iter != m_retryMap.end())
    {
        auto innerIter = iter->second.find(field);
        if (innerIter != iter->second.end())
        {
            iter->second.erase(innerIter);
            if (iter->second.empty())
            {
                m_retryMap.erase(iter);
            }
        }
    }
}
