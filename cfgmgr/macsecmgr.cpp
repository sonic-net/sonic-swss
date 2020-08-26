#include "macsecmgr.h"

#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <string.h>
#include <error.h>

#include <exec.h>
#include <shellcmd.h>

#include <string>
#include <vector>
#include <map>
#include <tuple>
#include <algorithm>
#include <sstream>
#include <cctype>


using namespace std;
using namespace swss;

#define WPA_SUPPLICANT_CMD "./wpa_supplicant"
#define WPA_CLI_CMD        "./wpa_cli"
#define WPA_CONF           "./wpa.conf"
// #define SOCK_DIR           "/var/run/macsec/"
#define SOCK_DIR           "./"

constexpr std::uint64_t RETRY_TIME = 30;
/* retry interval, in millisecond */
constexpr std::uint64_t RETRY_INTERVAL = 100;

static std::istringstream& operator>>(
    std::istringstream &istream,
    MACsecMgr::MACsecProfile::Policy & policy)
{
    SWSS_LOG_ENTER();

    std::string policy_str = istream.str();
    std::transform(
        policy_str.begin(),
        policy_str.begin(),
        policy_str.end(),
        ::tolower);
    if (policy_str == "bypass")
    {
        policy = MACsecMgr::MACsecProfile::Policy::BYPASS;
    }
    else if (policy_str == "integrity_only")
    {
        policy = MACsecMgr::MACsecProfile::Policy::INTEGRITY_ONLY;
    }
    else if (policy_str == "security")
    {
        policy = MACsecMgr::MACsecProfile::Policy::SECURITY;
    }
    else
    {
        throw std::invalid_argument("Invalid policy : " + policy_str);
    }
    return istream;
}

template<class T>
static bool get_value(
    const MACsecMgr::TaskArgs & ta,
    const std::string & field,
    T & value)
{
    SWSS_LOG_ENTER();

    std::string target_field = field;
    std::transform(
        target_field.begin(),
        target_field.begin(),
        target_field.end(),
        ::tolower);
    auto itr = std::find_if(
        ta.begin(),
        ta.end(),
        [&](const MACsecMgr::TaskArgs::value_type & entry)
        {
            std::string field = fvField(entry);
            std::transform(
                field.begin(),
                field.begin(),
                field.end(),
                ::tolower);
            return field == target_field;
        });
    if (itr != ta.end())
    {
        std::istringstream istream(fvValue(*itr));
        istream >> value;
        SWSS_LOG_DEBUG(
            "Set field '%s' as '%s'",
            field.c_str(),
            fvValue(*itr).c_str());
        return true;
    }
    SWSS_LOG_WARN("Cannot find field : %s", field.c_str());
    return false;
}


static void wpa_cli_commands(std::ostringstream & ostream)
{
}

template<typename T, typename...Args>
static void wpa_cli_commands(
    std::ostringstream & ostream,
    T && t,
    Args && ... args)
{
    ostream << " " << t;
    wpa_cli_commands(ostream, args...);
}

template<typename...Args>
static void wpa_cli_commands(
    std::ostringstream & ostream,
    const std::string & t,
    Args && ... args)
{
    ostream << shellquote(t) << " ";
    wpa_cli_commands(ostream, args...);
}

template<typename...Args>
static void wpa_cli_commands(
    std::ostringstream & ostream,
    const std::string & sock,
    const std::string & port_name,
    const std::string & network_id,
    Args && ... args)
{
    ostream << WPA_CLI_CMD;
    wpa_cli_commands(ostream, "-g", sock);
    if (!port_name.empty())
    {
        wpa_cli_commands(ostream, "IFNAME=" + port_name);
    }
    if (!network_id.empty())
    {
        wpa_cli_commands(ostream, "set_network", port_name);
    }
    wpa_cli_commands(ostream, args...);
}

template<typename...Args>
static std::string wpa_cli_exec(
    const std::string & sock,
    const std::string & port_name,
    const std::string & network_id,
    Args && ... args)
{
    std::ostringstream ostream;
    std::string res;
    wpa_cli_commands(
        ostream,
        sock,
        port_name,
        network_id,
        std::forward<Args>(args)...);
    EXEC_WITH_ERROR_THROW(ostream.str(), res);
    return res;
}

template<typename...Args>
static void wpa_cli_exec_and_check(
    const std::string & sock,
    const std::string & port_name,
    const std::string & network_id,
    Args && ... args)
{
    std::string res = wpa_cli_exec(
        sock,
        port_name,
        network_id,
        std::forward<Args>(args)...);
    if (res.find("OK") != 0)
    {
        std::ostringstream ostream;
        wpa_cli_commands(
            ostream,
            sock,
            port_name,
            network_id,
            std::forward<Args>(args)...);
        throw std::runtime_error(
            "Wpa_cli command : " + ostream.str() + " -> " +res);
    }
}

MACsecMgr::MACsecMgr(
    DBConnector *cfgDb,
    DBConnector *appDb,
    DBConnector *stateDb,
    const vector<std::string> &tables) :
        Orch(cfgDb, tables),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME)
{
}

MACsecMgr::~MACsecMgr()
{
    // Disable MACsec for all ports
    while (!m_macsec_ports.empty())
    {
        auto port = m_macsec_ports.begin();
        const TaskArgs temp;
        disableMACsec(port->first, temp);
    }
}

void MACsecMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    using TaskType = std::tuple<const std::string,const std::string>;
    using TaskFunc = task_process_status (MACsecMgr::*)(const std::string &, const TaskArgs &);
    const static std::map<TaskType, TaskFunc > TaskMap = {
        { { CFG_MACSEC_PROFILE_TABLE_NAME, SET_COMMAND }, &MACsecMgr::loadProfile},
        { { CFG_MACSEC_PROFILE_TABLE_NAME, DEL_COMMAND }, &MACsecMgr::removeProfile},
        { { CFG_PORT_TABLE_NAME, SET_COMMAND }, &MACsecMgr::enableMACsec},
        { { CFG_PORT_TABLE_NAME, DEL_COMMAND }, &MACsecMgr::disableMACsec},
    };

    const std::string & table_name = consumer.getTableName();
    auto itr = consumer.m_toSync.begin();
    while (itr != consumer.m_toSync.end())
    {
        task_process_status task_done = task_failed;
        auto & message = itr->second;
        const std::string & op = kfvOp(message);

        auto task = TaskMap.find(std::make_tuple(table_name, op));
        if (task != TaskMap.end())
        {
            task_done = (this->*task->second)(
                kfvKey(message),
                kfvFieldsValues(message));
        }
        else
        {
            SWSS_LOG_ERROR(
                "Unknown task : %s - %s",
                table_name.c_str(),
                op.c_str());
        }

        if (task_done == task_need_retry)
        {
            SWSS_LOG_DEBUG(
                "Task %s - %s need retry",
                table_name.c_str(),
                op.c_str());
            ++itr;
        }
        else
        {
            if (task_done != task_success)
            {
                SWSS_LOG_WARN("Task %s - %s fail",
                    table_name.c_str(),
                    op.c_str());
            }
            else
            {
                SWSS_LOG_DEBUG(
                    "Task %s - %s success",
                    table_name.c_str(),
                    op.c_str());
            }

            itr = consumer.m_toSync.erase(itr);
        }
    }
}

#define GetValue(args, name) (get_value(args, #name, name))

bool MACsecMgr::MACsecProfile::update(const TaskArgs & ta)
{
    SWSS_LOG_ENTER();

    return GetValue(ta, priority)
        && GetValue(ta, cipher_suite)
        && GetValue(ta, primary_cak)
        && GetValue(ta, primary_ckn)
        && GetValue(ta, fallback_cak)
        && GetValue(ta, fallback_ckn)
        && GetValue(ta, policy)
        && GetValue(ta, enable_replay_protect)
        && GetValue(ta, replay_window)
        && GetValue(ta, send_sci)
        && GetValue(ta, rekey_period);
}

task_process_status MACsecMgr::loadProfile(
    const std::string & profile_name,
    const TaskArgs & profile_attr)
{
    SWSS_LOG_ENTER();

    auto profile = m_profiles.emplace(
        std::piecewise_construct,
        std::make_tuple(profile_name),
        std::make_tuple());
    try
    {
        if (profile.first->second.update(profile_attr))
        {
            SWSS_LOG_NOTICE(
                "The MACsec profile '%s' is loaded",
                profile_name.c_str());
        }
        // If the profile has been used
        if (profile.second)
        {
            for (auto & port : m_macsec_ports)
            {
                if (port.second.profile_name == profile_name)
                {
                    // Hot update
                    SWSS_LOG_DEBUG("Hot update");
                }
            }
        }
        return task_success;
    }
    catch(const std::invalid_argument & e)
    {
        SWSS_LOG_WARN("%s", e.what());
        return task_failed;
    }
}

task_process_status MACsecMgr::removeProfile(
    const std::string & profile_name,
    const TaskArgs & profile_attr)
{
    SWSS_LOG_ENTER();

    auto profile = m_profiles.find(profile_name);
    if (profile == m_profiles.end())
    {
        SWSS_LOG_WARN(
            "The MACsec profile '%s' wasn't loaded",
            profile_name.c_str());
        return task_invalid_entry;
    }

    // The MACsec profile cannot be removed if it is occupied
    auto port = std::find_if(
        m_macsec_ports.begin(),
        m_macsec_ports.end(),
        [&](const decltype(m_macsec_ports)::value_type & pair)
        {
            return pair.second.profile_name == profile_name;
        });
    if (port != m_macsec_ports.end())
    {
        // This MACsec profile is occupied by some ports
        // remove it after all ports disable MACsec
        SWSS_LOG_DEBUG(
            "The MACsec profile '%s' is used by the port '%s'",
            profile_name.c_str(),
            port->first.c_str());
        return task_need_retry;
    }
    SWSS_LOG_NOTICE("The MACsec profile '%s' is removed", profile_name.c_str());
    m_profiles.erase(profile);
    return task_success;
}

task_process_status MACsecMgr::enableMACsec(
    const std::string & port_name,
    const TaskArgs & port_attr)
{
    SWSS_LOG_ENTER();

    std::string profile_name;
    if (!get_value(port_attr, "macsec", profile_name)
        || profile_name.empty())
    {
        SWSS_LOG_DEBUG("MACsec field of port '%s' is empty", port_name.c_str());
        return disableMACsec(port_name, port_attr);
    }

    // If the MACsec profile is ready
    auto itr = m_profiles.find(profile_name);
    if (itr == m_profiles.end())
    {
        SWSS_LOG_DEBUG(
            "The MACsec profile '%s' for the port '%s' isn't ready",
            profile_name.c_str(),
            port_name.c_str());
        return task_need_retry;
    }
    auto & profile = itr->second;

    // If the port is ready
    if (!isPortStateOk(port_name))
    {
        SWSS_LOG_DEBUG("The port '%s' isn't ready", port_name.c_str());
        return task_need_retry;
    }

    // Create MKA Session object
    auto port = m_macsec_ports.emplace(
        std::piecewise_construct,
        std::make_tuple(port_name),
        std::make_tuple());
    if (!port.second)
    {
        if (port.first->second.profile_name == profile_name)
        {
            SWSS_LOG_NOTICE(
                "The MACsec profile '%s' on the port '%s' has been loaded",
                profile_name.c_str(),
                port_name.c_str());
            return task_success;
        }
        else
        {
            SWSS_LOG_NOTICE(
                "The MACsec profile '%s' on the port '%s' "
                "will be replaced by the MACsec profile '%s'",
                port.first->second.profile_name.c_str(),
                port_name.c_str(),
                profile_name.c_str());
            auto result = disableMACsec(port_name, port_attr);
            if (result != task_success)
            {
                return result;
            }
        }
    }
    auto & session = port.first->second;
    session.profile_name = profile_name;
    ostringstream ostream;
    ostream << SOCK_DIR << port_name;
    session.sock = ostream.str();
    session.wpa_supplicant_pid = startWPASupplicant(session.sock);
    if (session.wpa_supplicant_pid < 0)
    {
        SWSS_LOG_WARN("Cannot start the wpa_supplicant of the port '%s' : %s",
            port_name.c_str(),
            strerror(errno));
        m_macsec_ports.erase(port.first);
        return task_need_retry;
    }
    else if (session.wpa_supplicant_pid == 0)
    {
        SWSS_LOG_WARN("Cannot start the wpa_supplicant of the port '%s' : %s",
        port_name.c_str(),
        strerror(errno));
        m_macsec_ports.erase(port.first);
        return task_failed;
    }

    // Enable MACsec
    if (!configureMACsec(port_name, session, profile))
    {
        SWSS_LOG_WARN("The MACsec profile '%s' on the port '%s' loading fail",
            profile_name.c_str(),
            port_name.c_str());
        return disableMACsec(port_name, port_attr);
    }
    SWSS_LOG_NOTICE("The MACsec profile '%s' on the port '%s' loading success",
        profile_name.c_str(),
        port_name.c_str());
    return task_success;
}

task_process_status MACsecMgr::disableMACsec(
    const std::string & port_name,
    const TaskArgs & port_attr)
{
    SWSS_LOG_ENTER();

    auto itr = m_macsec_ports.find(port_name);
    if (itr == m_macsec_ports.end())
    {
        SWSS_LOG_NOTICE("The MACsec was not enabled on the port '%s'",
            port_name.c_str());
        return task_success;
    }
    auto & session = itr->second;
    task_process_status ret = task_success;
    if (!unconfigureMACsec(port_name, session))
    {
        SWSS_LOG_WARN(
            "Cannot stop MKA session on the port '%s'",
            port_name.c_str());
        ret = task_failed;
    }
    if (!stopWPASupplicant(session.wpa_supplicant_pid))
    {
        SWSS_LOG_WARN(
            "Cannot stop WPA_SUPPLICANT process of the port '%s'",
            port_name.c_str());
        ret = task_failed;
    }
    if (ret == task_success)
    {
        SWSS_LOG_NOTICE("The MACsec profile '%s' on the port '%s' is removed",
            itr->second.profile_name.c_str(),
            port_name.c_str());
    }
    m_macsec_ports.erase(itr);
    return ret;
}

bool MACsecMgr::isPortStateOk(const std::string & port_name)
{
    SWSS_LOG_ENTER();

    std::vector<FieldValueTuple> temp;
    std::string state;

    if (m_statePortTable.get(port_name, temp)
        && get_value(temp, "state", state)
        && state == "ok")
    {
        SWSS_LOG_DEBUG("Port '%s' is ready", port_name.c_str());
        return true;
    }
    SWSS_LOG_DEBUG("Port '%s' is not ready", port_name.c_str());
    return false;
}

pid_t MACsecMgr::startWPASupplicant(const std::string & sock) const
{
    SWSS_LOG_ENTER();

    pid_t wpa_supplicant_pid = fork();
    if (wpa_supplicant_pid == 0)
    {
        exit(execl(
            WPA_SUPPLICANT_CMD,
            WPA_SUPPLICANT_CMD,
            "-s",
            "-D", "macsec_sonic",
            "-g", sock.c_str(),
            NULL));
    }
    else if (wpa_supplicant_pid > 0)
    {
        // Wait wpa_supplicant ready
        bool wpa_supplicant_loading = false;
        auto retry_time = RETRY_TIME;
        while(!wpa_supplicant_loading && retry_time > 0)
        {
            try
            {
                wpa_cli_exec(sock, "", "", "status");
                wpa_supplicant_loading = true;
            }
            catch(const std::runtime_error&)
            {
                retry_time--;
                std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_INTERVAL));
            }
        }
        if (wpa_supplicant_loading)
        {
            SWSS_LOG_DEBUG("Start wpa_supplicant success");
        }
        else
        {
            stopWPASupplicant(wpa_supplicant_pid);
            wpa_supplicant_pid = 0;
            SWSS_LOG_WARN("Cannot connect to wpa_supplicant.");
        }
    }
    return wpa_supplicant_pid;
}

bool MACsecMgr::stopWPASupplicant(pid_t pid) const
{
    SWSS_LOG_ENTER();

    if(kill(pid, SIGINT) != 0)
    {
        SWSS_LOG_WARN("Cannot stop wpa_supplicant(%d)", pid);
        return false;
    }
    int status = 0;
    waitpid(pid, &status, 0);
    SWSS_LOG_DEBUG(
        "Stop wpa_supplicant(%d) with return value (%d)",
        pid,
        status);
    return status == 0;
}

bool MACsecMgr::configureMACsec(
    const std::string & port_name,
    const MKASession & session,
    const MACsecProfile & profile) const
{
    SWSS_LOG_ENTER();

    try
    {
        wpa_cli_exec_and_check(
            session.sock,
            "",
            "",
            "interface_add",
            port_name,
            WPA_CONF,
            "macsec_sonic");

        const std::string res = wpa_cli_exec(
            session.sock,
            port_name,
            "",
            "add_network");
        const std::string network_id(
            res.begin(),
            std::find_if_not(
                res.begin(),
                res.end(),
                [](unsigned char c)
                {
                    return std::isdigit(c);
                }
            )
        );
        if (network_id.empty())
        {
            throw std::runtime_error("Cannot add network : " + res);
        }

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "key_mgmt",
            "NONE");

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "eapol_flags",
            0);

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "macsec_policy",
            1);

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "macsec_integ_only",
            (profile.policy == MACsecProfile::Policy::INTEGRITY_ONLY ? 1 : 0));

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "mka_cak",
            profile.primary_cak);

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "mka_ckn",
            profile.primary_ckn);

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "mka_priority",
            profile.priority);

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "macsec_replay_protect",
            (profile.enable_replay_protect ? 1 : 0));

        if (profile.enable_replay_protect)
        {
            wpa_cli_exec_and_check(
                session.sock,
                port_name,
                network_id,
                "macsec_replay_window",
                profile.replay_window);
        }

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            "",
            "enable_network",
            network_id);
    }
    catch(const std::runtime_error & e)
    {
        SWSS_LOG_WARN("Enable MACsec fail : %s", e.what());
        return false;
    }
    return true;
}

bool MACsecMgr::unconfigureMACsec(
    const std::string & port_name,
    const MKASession & session) const
{
    SWSS_LOG_ENTER();
    try
    {
        wpa_cli_exec_and_check(
            session.sock,
            "",
            "",
            "interface_remove",
            port_name);
    }
    catch(const std::runtime_error & e)
    {
        SWSS_LOG_WARN("Disable MACsec fail : %s", e.what());
        return false;
    }
    return true;
}
