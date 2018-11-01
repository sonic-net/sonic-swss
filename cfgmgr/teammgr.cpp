#include <unistd.h>

#include "exec.h"
#include "teammgr.h"
#include "logger.h"
#include "shellcmd.h"
#include "tokenize.h"

#include <algorithm>
#include <sstream>
#include <thread>

using namespace std;
using namespace swss;

#define DEFAULT_ADMIN_STATUS_STR    "up"
#define DEFAULT_MTU_STR             "9100"

TeamMgr::TeamMgr(DBConnector *confDb, DBConnector *applDb, DBConnector *statDb,
        const vector<TableConnector> &tables) :
    Orch(tables),
    m_cfgMetadataTable(confDb, CFG_DEVICE_METADATA_TABLE_NAME),
    m_cfgPortTable(confDb, CFG_PORT_TABLE_NAME),
    m_cfgLagTable(confDb, CFG_LAG_TABLE_NAME),
    m_cfgLagMemberTable(confDb, CFG_LAG_MEMBER_TABLE_NAME),
    m_appPortTable(applDb, APP_PORT_TABLE_NAME),
    m_appLagTable(applDb, APP_LAG_TABLE_NAME),
    m_statePortTable(statDb, STATE_PORT_TABLE_NAME),
    m_stateLagTable(statDb, STATE_LAG_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    // Clean up state database LAG entries
    vector<string> keys;
    m_stateLagTable.getKeys(keys);

    for (auto alias : keys)
    {
        m_stateLagTable.del(alias);
    }

    // Get the MAC address from configuration database
    vector<FieldValueTuple> fvs;
    m_cfgMetadataTable.get("localhost", fvs);
    auto it = find_if(fvs.begin(), fvs.end(), [](const FieldValueTuple &fv) {
            return fv.first == "mac";
            });

    if (it == fvs.end())
    {
        throw runtime_error("Failed to get MAC address from configuration database");
    }

    m_mac = MacAddress(it->second);
}

bool TeamMgr::isPortStateOk(const string &alias)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> temp;

    if (!m_statePortTable.get(alias, temp))
    {
        SWSS_LOG_INFO("Port %s is not ready", alias.c_str());
        return false;
    }

    return true;
}

bool TeamMgr::isLagStateOk(const string &alias)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> temp;

    if (!m_stateLagTable.get(alias, temp))
    {
        SWSS_LOG_INFO("Lag %s is not ready", alias.c_str());
        return false;
    }

    return true;
}

void TeamMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto table = consumer.getTableName();

    SWSS_LOG_INFO("Get task from table %s", table.c_str());

    if (table == CFG_LAG_TABLE_NAME)
    {
        doLagTask(consumer);
    }
    else if (table == CFG_LAG_MEMBER_TABLE_NAME)
    {
        doLagMemberTask(consumer);
    }
    else if (table == STATE_PORT_TABLE_NAME)
    {
        doPortUpdateTask(consumer);
    }
}

void TeamMgr::doLagTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            int min_links = 0;
            bool fallback = false;
            string admin_status = DEFAULT_ADMIN_STATUS_STR;
            string mtu = DEFAULT_MTU_STR;

            for (auto i : kfvFieldsValues(t))
            {
                // min_links and fallback attributes cannot be changed
                // after the LAG is created.
                if (fvField(i) == "min_links")
                {
                    min_links = stoi(fvValue(i));
                    SWSS_LOG_INFO("Get min_links value %d", min_links);
                }
                else if (fvField(i) == "fallback")
                {
                    fallback = fvValue(i) == "true";
                    SWSS_LOG_INFO("Get fallback option %s",
                            fallback ? "true" : "false");
                }
                else if (fvField(i) == "admin_status")
                {
                    admin_status = fvValue(i);;
                    SWSS_LOG_INFO("Get admin_status %s",
                            admin_status.c_str());
                }
                else if (fvField(i) == "mtu")
                {
                    mtu = fvValue(i);
                    SWSS_LOG_INFO("Get MTU %s", mtu.c_str());
                }
            }

            if (m_lagList.find(alias) == m_lagList.end())
            {
                addLag(alias, min_links, fallback);
                m_lagList.insert(alias);
            }

            setLagAdminStatus(alias, admin_status);
            setLagMtu(alias, mtu);
        }
        else if (op == DEL_COMMAND)
        {
            if (m_lagList.find(alias) != m_lagList.end())
            {
                removeLag(alias);
                m_lagList.erase(alias);
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

void TeamMgr::doLagMemberTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        auto tokens = tokenize(kfvKey(t), '|');
        auto lag = tokens[0];
        auto member = tokens[1];

        auto op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            if (!isPortStateOk(member) || !isLagStateOk(lag))
            {
                it++;
                continue;
            }

            addLagMember(lag, member);
        }
        else if (op == DEL_COMMAND)
        {
            removeLagMember(lag, member);
        }

        it = consumer.m_toSync.erase(it);
    }
}

// When a port gets removed and created again, notification is triggered
// when state dabatabase gets updated. In this situation, the port needs
// to be enslaved into the LAG again.
void TeamMgr::doPortUpdateTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        auto alias = kfvKey(t);
        auto op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            SWSS_LOG_INFO("Received port %s state update", alias.c_str());

            vector<string> keys;
            m_cfgLagMemberTable.getKeys(keys);

            for (auto key : keys)
            {
                auto tokens = tokenize(key, '|');

                auto lag = tokens[0];
                auto member = tokens[1];

                // Find the master of the port
                if (alias == member)
                {
                    addLagMember(lag, alias);
                    break;
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Received port %s state removal", alias.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool TeamMgr::setLagAdminStatus(const string &alias, const string &admin_status)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    // ip link set dev <port_channel_name> [up|down]
    cmd << IP_CMD << " link set dev " << alias << " " << admin_status;
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    SWSS_LOG_NOTICE("Set port channel %s admin status to %s",
            alias.c_str(), admin_status.c_str());

    return true;
}

bool TeamMgr::setLagMtu(const string &alias, const string &mtu)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    // ip link set dev <port_channel_name> mtu <mtu_value>
    cmd << IP_CMD << " link set dev " << alias << " mtu " << mtu;
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    vector<FieldValueTuple> fvs;
    FieldValueTuple fv("mtu", mtu);
    fvs.push_back(fv);
    m_appLagTable.set(alias, fvs);

    vector<string> keys;
    m_cfgLagMemberTable.getKeys(keys);

    for (auto key : keys)
    {
        auto tokens = tokenize(key, '|');
        auto lag = tokens[0];
        auto member = tokens[1];

        if (alias == lag)
        {
            m_appPortTable.set(member, fvs);
        }
    }

    SWSS_LOG_NOTICE("Set port channel %s MTU to %s",
            alias.c_str(), mtu.c_str());

    return true;
}

bool TeamMgr::addLag(const string &alias, int min_links, bool fallback)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    stringstream conf;
    conf << "'{\"device\":\"" << alias << "\","
         << "\"hwaddr\":\"" << m_mac.to_string() << "\","
         << "\"runner\":{"
         << "\"active\":\"true\","
         << "\"name\":\"lacp\"";

    if (min_links != 0)
    {
        conf << ",\"min_ports\":" << min_links;
    }

    if (fallback)
    {
        conf << ",\"fallback\":\"true\"";
    }

    conf << "}}'";

    SWSS_LOG_INFO("Port channel %s teamd configuration: %s",
            alias.c_str(), conf.str().c_str());

    cmd << TEAMD_CMD << " -r -t " << alias << " -c " << conf.str() << " -d";
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    SWSS_LOG_NOTICE("Start port channel %s with teamd", alias.c_str());

    return true;
}

bool TeamMgr::removeLag(const string &alias)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    cmd << TEAMD_CMD << " -k -t " << alias;
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    SWSS_LOG_NOTICE("Stop port channel %s", alias.c_str());

    return true;
}

// Once a port is enslaved into a port channel, the port's MTU will
// be inherited from the master's MTU while the port's admin status
// will still be controlled separately.
bool TeamMgr::addLagMember(const string &lag, const string &member)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    // Set admin down LAG member (required by teamd) and enslave it
    // ip link set dev <member> down;
    // teamdctl <port_channel_name> port add <member>;
    cmd << IP_CMD << " link set dev " << member << " down; ";
    cmd << TEAMDCTL_CMD << " " << lag << " port add " << member;

    // teamdctl port add command will fail when the member port is not
    // set to admin status down; it is possible that some other processes
    // or users are executing the command to bring up the member port
    // while adding this port into the port channel. This piece of retry
    // logic will try three times to ensure that a port could be added
    // into the port channel.
    int retry = 0;
    while (retry < 3)
    {
        int ret = exec(cmd.str(), res);
        if (ret == 0)
            break;

        SWSS_LOG_NOTICE("Failed to enslave %s to %s, wait 1 second and retry...",
                member.c_str(), lag.c_str());

        sleep(1);
        retry++;
    }

    if (retry == 3)
    {
        SWSS_LOG_ERROR("Failed to enslave %s to %s",
                member.c_str(), lag.c_str());
        return false;
    }

    vector<FieldValueTuple> fvs;
    m_cfgPortTable.get(member, fvs);

    // Get the member admin status (by default up)
    auto it = find_if(fvs.begin(), fvs.end(), [](const FieldValueTuple &fv) {
            return fv.first == "admin_status";
            });

    string admin_status = DEFAULT_ADMIN_STATUS_STR;
    if (it != fvs.end())
    {
        admin_status = it->second;
    }

    // Get the LAG MTU (by default 9100)
    // Member port will inherit master's MTU attribute
    m_cfgLagTable.get(lag, fvs);
    it = find_if(fvs.begin(), fvs.end(), [](const FieldValueTuple &fv) {
            return fv.first == "mtu";
            });

    string mtu = DEFAULT_MTU_STR;
    if (it != fvs.end())
    {
        mtu = it->second;
    }

    // ip link set dev <member> [up|down]
    cmd.str(string());
    cmd << IP_CMD << " link set dev " << member << " " << admin_status;
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    fvs.clear();
    FieldValueTuple fv("admin_status", admin_status);
    fvs.push_back(fv);
    fv = FieldValueTuple("mtu", mtu);
    fvs.push_back(fv);
    m_appPortTable.set(member, fvs);

    SWSS_LOG_NOTICE("Add %s to port channel %s", member.c_str(), lag.c_str());

    return true;
}

// Once a port is removed from from the master, both the admin status and the
// MTU will be re-set to its original value.
bool TeamMgr::removeLagMember(const string &lag, const string &member)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    // teamdctl <port_channel_name> port remove <member>;
    cmd << TEAMDCTL_CMD << " " << lag << " port remove " << member << "; ";

    vector<FieldValueTuple> fvs;
    m_cfgPortTable.get(member, fvs);

    // Re-configure port MTU and admin status (by default 9100 and up)
    string admin_status = DEFAULT_ADMIN_STATUS_STR;
    string mtu = DEFAULT_MTU_STR;
    for (auto i : fvs)
    {
        if (fvField(i) == "admin_status")
        {
            admin_status = fvValue(i);
        }
        else if (fvField(i) == "mtu")
        {
            mtu = fvValue(i);
        }
    }

    // ip link set dev <port_name> [up|down];
    // ip link set dev <port_name> mtu
    cmd << IP_CMD << " link set dev " << member << " " << admin_status << "; ";
    cmd << IP_CMD << " link set dev " << member << " mtu " << mtu;

    EXEC_WITH_ERROR_THROW(cmd.str(), res);
    fvs.clear();
    FieldValueTuple fv("admin_status", admin_status);
    fvs.push_back(fv);
    fv = FieldValueTuple("mtu", mtu);
    fvs.push_back(fv);
    m_appPortTable.set(member, fvs);

    SWSS_LOG_NOTICE("Remove %s from port channel %s", member.c_str(), lag.c_str());

    return true;
}
