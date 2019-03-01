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
#define LOOPBACK_PREFIX     "Loopback"
#define VNET_PREFIX         "Vnet"

IntfMgr::IntfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgIntfTable(cfgDb, CFG_INTF_TABLE_NAME),
        m_cfgVlanIntfTable(cfgDb, CFG_VLAN_INTF_TABLE_NAME),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
        m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME),
        m_stateVrfTable(stateDb, STATE_VRF_TABLE_NAME),
        m_stateIntfTable(stateDb, STATE_INTERFACE_TABLE_NAME),
        m_appIntfTableProducer(appDb, APP_INTF_TABLE_NAME)
{
}

void IntfMgr::setIntfIp(const string &alias, const string &opCmd,
                        const string &ipPrefixStr, const bool ipv4)
{
    stringstream cmd;
    string res;

    if (ipv4)
    {
        cmd << IP_CMD << " address " << opCmd << " " << ipPrefixStr << " dev " << alias;
    }
    else
    {
        cmd << IP_CMD << " -6 address " << opCmd << " " << ipPrefixStr << " dev " << alias;
    }
    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
    }
}

void IntfMgr::setIntfVrf(const string &alias, const string vrfName)
{
    stringstream cmd;
    string res;

    if (!vrfName.empty())
    {
        cmd << IP_CMD << " link set " << alias << " master " << vrfName;
    }
    else
    {
        cmd << IP_CMD << " link set " << alias << " nomaster";
    }
    EXEC_WITH_ERROR_THROW(cmd.str(), res);
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
    else if (!alias.compare(0, strlen(VNET_PREFIX), VNET_PREFIX))
    {
        if (m_stateVrfTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Vnet %s is ready", alias.c_str());
            return true;
        }
    }
    else if (m_statePortTable.get(alias, temp))
    {
        SWSS_LOG_DEBUG("Port %s is ready", alias.c_str());
        return true;
    }
    else if (!alias.compare(0, strlen(LOOPBACK_PREFIX), LOOPBACK_PREFIX))
    {
        return true;
    }

    return false;
}

bool IntfMgr::doIntfGeneralTask(const vector<string>& keys,
        const vector<FieldValueTuple>& data,
        const string& op)
{
    SWSS_LOG_ENTER();

    string alias(keys[0]);
    string vrf_name = "";
    bool is_lo = !alias.compare(0, strlen(LOOPBACK_PREFIX), LOOPBACK_PREFIX);

    for (auto idx : data)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);
        if (field == "vnet_name" || field == "vrf_name")
        {
            vrf_name = value;
        }
    }

    if (op == SET_COMMAND)
    {
        if (!isIntfStateOk(alias))
        {
            SWSS_LOG_DEBUG("Interface is not ready, skipping %s", alias.c_str());
            return false;
        }

        if (!vrf_name.empty() && !isIntfStateOk(vrf_name))
        {
            SWSS_LOG_DEBUG("VRF is not ready, skipping %s", vrf_name.c_str());
            return false;
        }

        // Set Interface VRF except for lo
        if (!is_lo)
        {
            setIntfVrf(alias, vrf_name);
            m_appIntfTableProducer.set(alias, data);
        }
        else
        {
            m_appIntfTableProducer.set("lo", data);
        }
    }
    else if (op == DEL_COMMAND)
    {
        // Set Interface VRF except for lo
        if (!is_lo)
        {
            setIntfVrf(alias, "");
            m_appIntfTableProducer.del(alias);
        }
        else
        {
            m_appIntfTableProducer.del("lo");
        }
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
    }

    return true;
}

bool IntfMgr::doIntfAddrTask(const vector<string>& keys,
        const vector<FieldValueTuple>& data,
        const string& op)
{
    SWSS_LOG_ENTER();

    string alias(keys[0]);
    IpPrefix ip_prefix(keys[1]);
    bool is_lo = !alias.compare(0, strlen(LOOPBACK_PREFIX), LOOPBACK_PREFIX);
    string appKey = (is_lo ? "lo" : keys[0]) + ":" + keys[1];

    if (op == SET_COMMAND)
    {
        /*
         * Don't proceed if port/LAG/VLAN is not ready yet.
         * The pending task will be checked periodically and retried.
         */
        if (!isIntfStateOk(alias))
        {
            SWSS_LOG_DEBUG("Interface is not ready, skipping %s", alias.c_str());
            return false;
        }

        // Set Interface IP except for lo
        if (!is_lo)
        {
            setIntfIp(alias, "add", ip_prefix.to_string(), ip_prefix.isV4());
        }

        std::vector<FieldValueTuple> fvVector;
        FieldValueTuple f("family", ip_prefix.isV4() ? IPV4_NAME : IPV6_NAME);

        if (ip_prefix.getIp().getAddrScope() == IpAddress::AddrScope::LINK_SCOPE)
        {
            FieldValueTuple s("scope", "local");
            fvVector.push_back(s);
        }
        else
        {
            FieldValueTuple s("scope", "global");
            fvVector.push_back(s);
        }

        fvVector.push_back(f);

        m_appIntfTableProducer.set(appKey, fvVector);
        m_stateIntfTable.hset(keys[0] + state_db_key_delimiter + keys[1], "state", "ok");
    }
    else if (op == DEL_COMMAND)
    {
        // Set Interface IP except for lo
        if (!is_lo)
        {
            setIntfIp(alias, "del", ip_prefix.to_string(), ip_prefix.isV4());
        }
        m_appIntfTableProducer.del(appKey);
        m_stateIntfTable.del(keys[0] + state_db_key_delimiter + keys[1]);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
    }

    return true;
}

void IntfMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        vector<string> keys = tokenize(kfvKey(t), config_db_key_delimiter);
        const vector<FieldValueTuple>& data = kfvFieldsValues(t);
        string op = kfvOp(t);

        if (keys.size() == 1)
        {
            if (!doIntfGeneralTask(keys, data, op))
            {
                it++;
                continue;
            }
        }
        else if (keys.size() == 2)
        {
            if (!doIntfAddrTask(keys, data, op))
            {
                it++;
                continue;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Invalid key %s", kfvKey(t).c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}
