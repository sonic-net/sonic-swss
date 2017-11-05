#include <string.h>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "intfmgr.h"
#include "exec.h"
#include "shellcmd.h"

using namespace std;
using namespace swss;

#define VLAN_PREFIX         "Vlan"
#define LAG_PREFIX          "PortChannel"

IntfMgr::IntfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgIntfTable(cfgDb, CFG_INTF_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_cfgVlanIntfTable(cfgDb, CFG_VLAN_INTF_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_appIntfTableProducer(appDb, APP_INTF_TABLE_NAME)
{
}

bool IntfMgr::setIntfIp(const string &alias, const string &opCmd, const string &ipPrefixStr)
{
    stringstream cmd;
    string res;

    cmd << IP_CMD << " address " << opCmd << " " << ipPrefixStr << " dev " << alias;;
    int ret = swss::exec(cmd.str(), res);
    return (ret == 0);
}

bool IntfMgr::isIntfStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        if (m_stateVlanTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Vlan %s is ready", alias.c_str());
            return true;
        }
    }
    else if (!alias.compare(0, strlen(LAG_PREFIX), LAG_PREFIX))
    {
        if (m_stateLagTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Lag %s is ready", alias.c_str());
            return true;
        }
    }
    else if (m_statePortTable.get(alias, temp))
    {
        SWSS_LOG_DEBUG("Port %s is ready", alias.c_str());
        return true;
    }

    return false;
}
void IntfMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string keySeparator = CONFIGDB_KEY_SEPARATOR;
        vector<string> keys = tokenize(kfvKey(t), keySeparator[0]);
        string alias(keys[0]);

        if (alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
        {
            /* handle IP over vlan Only for now, skip the rest */
            it = consumer.m_toSync.erase(it);
            continue;
        }

        size_t pos = kfvKey(t).find(CONFIGDB_KEY_SEPARATOR);
        if (pos == string::npos)
        {
            SWSS_LOG_DEBUG("Invalid key %s", kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        IpPrefix ip_prefix(kfvKey(t).substr(pos+1));

        SWSS_LOG_DEBUG("intfs doTask: %s", (dumpTuple(consumer, t)).c_str());

        string op = kfvOp(t);
        if (op == SET_COMMAND)
        {
            /*
             * Don't proceed if port/lag/VLAN is not ready yet.
             * The pending task will be checked periodially and retried.
             * TODO: Subscribe to stateDB for port/lag/VLAN state and retry
             * pending tasks immediately upon state change.
             */
            if (!isIntfStateOk(alias))
            {
                SWSS_LOG_DEBUG("Interface is not ready, skipping %s", kfvKey(t).c_str());
                it++;
                continue;
            }
            string opCmd("add");
            string ipPrefixStr = ip_prefix.to_string();
            setIntfIp(alias, opCmd, ipPrefixStr);
        }
        else if (op == DEL_COMMAND)
        {
            string opCmd("del");
            string ipPrefixStr = ip_prefix.to_string();
            setIntfIp(alias, opCmd, ipPrefixStr);
        }

        it = consumer.m_toSync.erase(it);
        continue;
    }
}
