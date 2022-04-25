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

map<std::string, std::string> portDefaultConfig = {
    {"mtu", DEFAULT_MTU_STR},
    {"admin_status", DEFAULT_ADMIN_STATUS_STR}
};

PortMgr::PortMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgPortTable(cfgDb, CFG_PORT_TABLE_NAME),
        m_cfgLagMemberTable(cfgDb, CFG_LAG_MEMBER_TABLE_NAME),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_appPortTable(appDb, APP_PORT_TABLE_NAME),
        m_configCache(std::bind(&PortMgr::onPortConfigChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5))
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

            /* If this is the first time we set port settings
             * assign default admin status and mtu
             */
            bool exist = m_configCache.exist(alias);

            for (auto i : kfvFieldsValues(t))
            {
                m_configCache.config(alias, fvField(i), fvValue(i), (void *)&portOk);
            }

            if (!exist)
            {
                // The port does not exist before, it means that it is the first time
                // to configure this port, try to apply default configuration. Please
                // not that applyDefault function will NOT override existing configuration
                // with default value.
                m_configCache.applyDefault(alias, portDefaultConfig, (void *)&portOk);
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("Delete Port: %s", alias.c_str());
            m_appPortTable.del(alias);
            m_configCache.remove(alias);
        }

        if (m_retryFields.empty())
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
            it->second = KeyOpFieldsValuesTuple{alias, SET_COMMAND, m_retryFields};
            ++it;
            m_retryFields.clear();
        }
    }
}

bool PortMgr::onPortConfigChanged(const std::string &alias, const std::string &field, const std::string &old_value, const std::string &new_value, void *context)
{
    bool portOk = *((bool *)context);
    if (field == "mtu" && portOk)
    {
        setPortMtu(alias, new_value);
    }
    else if (field == "admin_status" && portOk)
    {
        setPortAdminStatus(alias, new_value == "up");
    }
    else
    {
        /* For mtu and admin_status, if portOk=false we still save it to APP DB which is 
         * the same behavior as portsyncd before. 
         */
        writeConfigToAppDb(alias, field, new_value);
    }

    if (!portOk && (field == "mtu" || field == "admin_status"))
    {
        m_retryFields.emplace_back(field, new_value);
        return false;
    }

    SWSS_LOG_NOTICE("Configure %s %s from %s to %s", alias.c_str(), field.c_str(), old_value.c_str(), new_value.c_str());
    return true;
}

bool PortMgr::writeConfigToAppDb(const std::string &alias, const std::string &field, const std::string &value)
{
    vector<FieldValueTuple> fvs;
    FieldValueTuple fv(field, value);
    fvs.push_back(fv);
    m_appPortTable.set(alias, fvs);

    return true;
}
