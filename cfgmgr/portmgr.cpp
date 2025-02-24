#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "portmgr.h"
#include "exec.h"
#include "shellcmd.h"
#include <swss/redisutility.h>
#include <unordered_map>

using namespace std;
using namespace swss;

PortMgr::PortMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgPortTable(cfgDb, CFG_PORT_TABLE_NAME),
        m_cfgSendToIngressPortTable(cfgDb, CFG_SEND_TO_INGRESS_PORT_TABLE_NAME),
        m_cfgLagMemberTable(cfgDb, CFG_LAG_MEMBER_TABLE_NAME),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_appSendToIngressPortTable(appDb, APP_SEND_TO_INGRESS_PORT_TABLE_NAME),
        m_appPortTable(appDb, APP_PORT_TABLE_NAME)
{
}

bool PortMgr::isLagMember(const std::string &alias, std::unordered_map<std::string, int> &lagMembers)
{
    /* Lag members are lazily loaded on the first call to isLagMember and cached
     * within a variable inside of doTask() for future calls.
     */
    if (lagMembers.empty())
    {
        vector<string> keys;
        m_cfgLagMemberTable.getKeys(keys);
        for (auto key: keys)
        {
            auto tokens = tokenize(key, config_db_key_delimiter);
            std::string member = tokens[1];
            if (!member.empty()) {
                lagMembers[member] = 1;
            }
        }

        /* placeholder to state we already read lagmembers even though there are
         * none */
        if (lagMembers.empty()) {
            lagMembers["none"] = 1;
        }
    }

    if (lagMembers.find(alias) != lagMembers.end())
    {
        return true;
    }

    return false;
}

bool PortMgr::setPortMtu(const string &alias, const string &mtu, std::unordered_map<std::string, int> &lagMembers)
{
    stringstream cmd;
    string res, cmd_str;

    // ip link set dev <port_name> mtu <mtu>
    cmd << IP_CMD << " link set dev " << shellquote(alias) << " mtu " << shellquote(mtu);
    cmd_str = cmd.str();
    int ret = swss::exec(cmd_str, res);
    if (!ret)
    {
        // Set the port MTU in application database to update both
        // the port MTU and possibly the port based router interface MTU
        return writeConfigToAppDb(alias, "mtu", mtu);
    }
    else if (!isPortStateOk(alias))
    {
        // Can happen when a DEL notification is sent by portmgrd immediately followed by a new SET notif
        SWSS_LOG_WARN("Setting mtu to alias:%s netdev failed with cmd:%s, rc:%d, error:%s", alias.c_str(), cmd_str.c_str(), ret, res.c_str());
        return false;
    }
    else if (isLagMember(alias, lagMembers))
    {
        // Catch user improperly specified an MTU on the PortChannel
        SWSS_LOG_WARN("Setting mtu to alias:%s which is a member of a PortChannel is invalid", alias.c_str());
        return false;
    }
    else
    {
        throw runtime_error(cmd_str + " : " + res);
    }
    return true;
}

bool PortMgr::setPortAdminStatus(const string &alias, const bool up)
{
    stringstream cmd;
    string res, cmd_str;

    // ip link set dev <port_name> [up|down]
    cmd << IP_CMD << " link set dev " << shellquote(alias) << (up ? " up" : " down");
    cmd_str = cmd.str();
    int ret = swss::exec(cmd_str, res);
    if (!ret)
    {
        return writeConfigToAppDb(alias, "admin_status", (up ? "up" : "down"));
    }
    else if (!isPortStateOk(alias))
    {
        // Can happen when a DEL notification is sent by portmgrd immediately followed by a new SET notification
        SWSS_LOG_WARN("Setting admin_status to alias:%s netdev failed with cmd%s, rc:%d, error:%s", alias.c_str(), cmd_str.c_str(), ret, res.c_str());
        return false;
    }
    else
    {
        throw runtime_error(cmd_str + " : " + res);
    }
    return true;
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

void PortMgr::doSendToIngressPortTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);
        auto fvs = kfvFieldsValues(t);

        if (op == SET_COMMAND)
        {
            SWSS_LOG_NOTICE("Add SendToIngress Port: %s",
                            alias.c_str());
            m_appSendToIngressPortTable.set(alias, fvs);
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("Removing SendToIngress Port: %s",
                                alias.c_str());
            m_appSendToIngressPortTable.del(alias);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }

}


void PortMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    /* Variable to lazily cache lag members upon first call into isLagMember(). We
     * don't want to always query for lag members if not needed, and we also don't
     * want to query it for each call to isLagMember() which may be on every port.
     */
    std::unordered_map<std::string, int> lagMembers;

    auto table = consumer.getTableName();
    if (table == CFG_SEND_TO_INGRESS_PORT_TABLE_NAME)
    {
        doSendToIngressPortTask(consumer);
        return;
    }

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
            bool   isMtuSet = false;
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
            else if (!portOk)
            {
                it++;
                continue;
            }

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "mtu")
                {
                    mtu = fvValue(i);
                    /* mtu read might be "", so we can't just use .empty() to
                     * know if its set.  There's test cases that depend on this
                     * logic ... */
                    isMtuSet = true;
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

            /* Clear default MTU if LAG member as this will otherwise fail. */
            if (!isMtuSet && isLagMember(alias, lagMembers)) {
                mtu = "";
            }

            if (!portOk)
            {
                // Port configuration is handled by the orchagent. If the configuration is written to the APP DB using
                // multiple Redis write commands, the orchagent may receive a partial configuration and create a port
                // with incorrect settings.
                field_values.emplace_back("mtu", mtu);
                field_values.emplace_back("admin_status", admin_status);
            }

            if (field_values.size())
            {
                writeConfigToAppDb(alias, field_values);
            }

            if (!portOk)
            {
                SWSS_LOG_INFO("Port %s is not ready, pending...", alias.c_str());

                /* Retry setting these params after the netdev is created */
                field_values.clear();
                field_values.emplace_back("mtu", mtu);
                field_values.emplace_back("admin_status", admin_status);
                it->second = KeyOpFieldsValuesTuple{alias, SET_COMMAND, field_values};
                it++;
                continue;
            }

            if (!mtu.empty())
            {
                SWSS_LOG_NOTICE("Configure %s MTU to %s", alias.c_str(), mtu.c_str());
                setPortMtu(alias, mtu, lagMembers);
            }

            if (!admin_status.empty())
            {
                SWSS_LOG_NOTICE("Configure %s admin status to %s", alias.c_str(), admin_status.c_str());
                setPortAdminStatus(alias, admin_status == "up");
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("Delete Port: %s", alias.c_str());
            m_appPortTable.del(alias);
            m_portList.erase(alias);
        }

        it = consumer.m_toSync.erase(it);
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

bool PortMgr::writeConfigToAppDb(const std::string &alias, std::vector<FieldValueTuple> &field_values)
{
    m_appPortTable.set(alias, field_values);
    return true;
}
