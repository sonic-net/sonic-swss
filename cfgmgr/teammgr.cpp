#include "exec.h"
#include "teammgr.h"
#include "logger.h"
#include "shellcmd.h"
#include "tokenize.h"
#include "warm_restart.h"
#include "portmgr.h"

#include <algorithm>
#include <sstream>
#include <fstream>
#include <thread>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>

#define PID_FILE_PATH "/var/run/teamd/"

using namespace std;
using namespace swss;


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


#define MAX_PID_STRLEN 10
int TeamMgr::getTeamPid(const string &alias)
{
    SWSS_LOG_ENTER();
    int pid = 0;

    string file = string(PID_FILE_PATH) + alias + string(".pid");
    ifstream infile(file);
    if (!infile.is_open())
    {
        SWSS_LOG_WARN("The LAG PID file: %s is not readable", file.c_str());
        return 0;
    }

    string line;
    getline(infile, line);
    if (line.empty())
    {
        SWSS_LOG_WARN("The LAG PID file: %s is empty", file.c_str());
    }
    else 
    {
        /*Store the PID value */
        pid = stoi(line, nullptr, 10);
    }

    /* Close the file and return */
    infile.close();

    return pid;
}


#define MAX_PID_STRLEN 10
void TeamMgr::addLagPid(const string &alias)
{
    SWSS_LOG_ENTER();
    m_lagPIDList[alias] = getTeamPid(alias);
}

void TeamMgr::removeLagPid(const string &alias)
{
    SWSS_LOG_ENTER();
    m_lagPIDList.erase(alias);
}

void TeamMgr::cleanTeamProcesses(int signo)
{
    int pid = 0;

    for (const auto& it: m_lagList)
    {
        pid = m_lagPIDList[it];
        if(!pid) {
            SWSS_LOG_WARN("Invalid PID found for LaG %s ", it.c_str());

            /* Try to get the PID again */
            pid = getTeamPid(it);
        }

        if(pid > 0)
        {
            SWSS_LOG_WARN("Cleaning Process(PID: %d) for LaG %s ", pid, it.c_str());
            kill(pid, signo);
        }
    }

    return;
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
            string learn_mode;

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
                else if (fvField(i) == "learn_mode")
                {
                    learn_mode = fvValue(i);
                    SWSS_LOG_INFO("Get learn_mode %s",
                            learn_mode.c_str());
                }
            }

            if (m_lagList.find(alias) == m_lagList.end())
            {
                if (addLag(alias, min_links, fallback) == task_need_retry)
                {
                    it++;
                    continue;
                }

                m_lagList.insert(alias);
                addLagPid(alias);
            }

            setLagAdminStatus(alias, admin_status);
            setLagMtu(alias, mtu);
            if (!learn_mode.empty())
            {
                setLagLearnMode(alias, learn_mode);
                SWSS_LOG_NOTICE("Configure %s MAC learn mode to %s", alias.c_str(), learn_mode.c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_lagList.find(alias) != m_lagList.end())
            {
                removeLag(alias);
                m_lagList.erase(alias);
                removeLagPid(alias);
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

        auto tokens = tokenize(kfvKey(t), config_db_key_delimiter);
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

            if (addLagMember(lag, member) == task_need_retry)
            {
                it++;
                continue;
            }
        }
        else if (op == DEL_COMMAND)
        {
            removeLagMember(lag, member);
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool TeamMgr::checkPortIffUp(const string &port)
{
    SWSS_LOG_ENTER();

    struct ifreq ifr;
    memcpy(ifr.ifr_name, port.c_str(), strlen(port.c_str()));
    ifr.ifr_name[strlen(port.c_str())] = 0;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd == -1 || ioctl(fd, SIOCGIFFLAGS, &ifr) == -1)
    {
        SWSS_LOG_ERROR("Failed to get port %s flags", port.c_str());
        return false;
    }

    SWSS_LOG_INFO("Get port %s flags %i", port.c_str(), ifr.ifr_flags);

    return ifr.ifr_flags & IFF_UP;
}

bool TeamMgr::isPortEnslaved(const string &port)
{
    SWSS_LOG_ENTER();

    struct stat buf;
    string path = "/sys/class/net/" + port + "/master";

    return lstat(path.c_str(), &buf) == 0;
}

bool TeamMgr::findPortMaster(string &master, const string &port)
{
    SWSS_LOG_ENTER();

    vector<string> keys;
    m_cfgLagMemberTable.getKeys(keys);

    for (auto key: keys)
    {
        auto tokens = tokenize(key, config_db_key_delimiter);
        auto lag = tokens[0];
        auto member = tokens[1];

        if (port == member)
        {
            master = lag;
            return true;
        }
    }

    return false;
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

            string lag;
            if (findPortMaster(lag, alias))
            {
                if (addLagMember(lag, alias) == task_need_retry)
                {
                    it++;
                    continue;
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
    cmd << IP_CMD << " link set dev " << shellquote(alias) << " " << shellquote(admin_status);
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
    cmd << IP_CMD << " link set dev " << shellquote(alias) << " mtu " << shellquote(mtu);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    vector<FieldValueTuple> fvs;
    FieldValueTuple fv("mtu", mtu);
    fvs.push_back(fv);
    m_appLagTable.set(alias, fvs);

    vector<string> keys;
    m_cfgLagMemberTable.getKeys(keys);

    for (auto key : keys)
    {
        auto tokens = tokenize(key, config_db_key_delimiter);
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

bool TeamMgr::setLagLearnMode(const string &alias, const string &learn_mode)
{
    // Set the port MAC learn mode in application database
    vector<FieldValueTuple> fvs;
    FieldValueTuple fv("learn_mode", learn_mode);
    fvs.push_back(fv);
    m_appLagTable.set(alias, fvs);

    return true;
}

task_process_status TeamMgr::addLag(const string &alias, int min_links, bool fallback)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    stringstream conf;
    conf << "'{\"device\":\"" << alias << "\","
         << "\"hwaddr\":\"" << m_mac.to_string() << "\","
         << "\"runner\":{"
         << "\"active\":true,"
         << "\"name\":\"lacp\"";

    if (min_links != 0)
    {
        conf << ",\"min_ports\":" << min_links;
    }

    if (fallback)
    {
        conf << ",\"fallback\":true";
    }

    conf << "}}'";

    SWSS_LOG_INFO("Port channel %s teamd configuration: %s",
            alias.c_str(), conf.str().c_str());

    string warmstart_flag = WarmStart::isWarmStart() ? " -w -o " : " -r ";
    const string dump_path = "/var/warmboot/teamd/";

    cmd << TEAMD_CMD
        << warmstart_flag
        << " -t " << alias
        << " -c " << conf.str()
        << " -L " << dump_path
        << " -g -d";

    if (exec(cmd.str(), res) != 0)
    {
        SWSS_LOG_INFO("Failed to start port channel %s with teamd, retry...",
                alias.c_str());
        return task_need_retry;
    }

    SWSS_LOG_NOTICE("Start port channel %s with teamd", alias.c_str());

    return task_success;
}

bool TeamMgr::removeLag(const string &alias)
{
    SWSS_LOG_ENTER();

    stringstream cmd;
    string res;

    cmd << TEAMD_CMD << " -k -t " << shellquote(alias);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    SWSS_LOG_NOTICE("Stop port channel %s", alias.c_str());

    return true;
}

// Once a port is enslaved into a port channel, the port's MTU will
// be inherited from the master's MTU while the port's admin status
// will still be controlled separately.
task_process_status TeamMgr::addLagMember(const string &lag, const string &member)
{
    SWSS_LOG_ENTER();

    // If port is already enslaved, ignore this operation
    // TODO: check the current master if it is the same as to be configured
    if (isPortEnslaved(member))
    {
        return task_ignore;
    }

    stringstream cmd;
    string res;

    // Set admin down LAG member (required by teamd) and enslave it
    // ip link set dev <member> down;
    // teamdctl <port_channel_name> port add <member>;
    cmd << IP_CMD << " link set dev " << shellquote(member) << " down; ";
    cmd << TEAMDCTL_CMD << " " << shellquote(lag) << " port add " << shellquote(member);

    if (exec(cmd.str(), res) != 0)
    {
        // teamdctl port add command will fail when the member port is not
        // set to admin status down; it is possible that some other processes
        // or users (e.g. portmgrd) are executing the command to bring up the
        // member port while adding this port into the port channel. This piece
        // of code will check if the port is set to admin status up. If yes,
        // it will retry to add the port into the port channel.
        if (checkPortIffUp(member))
        {
            SWSS_LOG_INFO("Failed to add %s to port channel %s, retry...",
                    member.c_str(), lag.c_str());
            return task_need_retry;
        }
        else
        {
            SWSS_LOG_ERROR("Failed to add %s to port channel %s",
                    member.c_str(), lag.c_str());
            return task_failed;
        }
    }

    vector<FieldValueTuple> fvs;
    m_cfgPortTable.get(member, fvs);

    // Get the member admin status
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
    cmd << IP_CMD << " link set dev " << shellquote(member) << " " << shellquote(admin_status);
    EXEC_WITH_ERROR_THROW(cmd.str(), res);

    fvs.clear();
    FieldValueTuple fv("mtu", mtu);
    fvs.push_back(fv);
    m_appPortTable.set(member, fvs);

    SWSS_LOG_NOTICE("Add %s to port channel %s", member.c_str(), lag.c_str());

    return task_success;
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
    cmd << IP_CMD << " link set dev " << shellquote(member) << " " << shellquote(admin_status) << "; ";
    cmd << IP_CMD << " link set dev " << shellquote(member) << " mtu " << shellquote(mtu);

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
