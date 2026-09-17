#include "macsecmgr.h"

#include <exec.h>
#include <shellcmd.h>
#include <timer.h>
#include <swss/stringutility.h>
#include <swss/redisutility.h>
#include <boost/algorithm/string/predicate.hpp>

#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <string.h>
#include <error.h>
#include <string>
#include <vector>
#include <map>
#include <tuple>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <cctype>
#include <ctime>
#include <functional>
#include <iomanip>
#include <set>


using namespace std;
using namespace swss;

#define WPA_SUPPLICANT_CMD "/sbin/wpa_supplicant"
#define WPA_CLI_CMD        "/sbin/wpa_cli"
#define WPA_CONF           "/etc/wpa_supplicant.conf"
#define SOCK_DIR           "/var/run/"

constexpr std::uint64_t RETRY_TIME = 30;

/* retry interval, in millisecond */
constexpr std::uint64_t RETRY_INTERVAL = 100;
constexpr std::uint64_t MKA_STATUS_SWEEP_INTERVAL_SECONDS = 20;

#define TIMEOUT_CMD "/usr/bin/timeout"

/*
 * The input cipher_str is the encoded string which can be either of length 66 bytes or 130 bytes.
 *
 * 66 bytes of length, for 128-byte cipher suite
 *   - first 2 bytes of the string will be the index from the magic salt string.
 *   - remaining 64 bytes will be encoded string from the 32-byte plain text CAK input string.
 *
 * 130 bytes of length, for 256-byte cipher suite
 *   - first 2 bytes of the string will be the index from the magic salt string.
 *   - remaining 128 bytes will be encoded string from the 32 byte plain text CAK input string.
*/
constexpr std::size_t AES_LEN_128_BYTE = 66;
constexpr std::size_t AES_LEN_256_BYTE = 130;

static void lexical_convert(const std::string &policy_str, MACsecMgr::MACsecProfile::Policy & policy)
{
    SWSS_LOG_ENTER();

    if (boost::iequals(policy_str, "integrity_only"))
    {
        policy = MACsecMgr::MACsecProfile::Policy::INTEGRITY_ONLY;
    }
    else if (boost::iequals(policy_str, "security"))
    {
        policy = MACsecMgr::MACsecProfile::Policy::SECURITY;
    }
    else
    {
        throw std::invalid_argument("Invalid policy : " + policy_str);
    }
}

static void lexical_convert(const std::string &cipher_str, MACsecMgr::MACsecProfile::CipherSuite & cipher_suite)
{
    SWSS_LOG_ENTER();

    if (boost::iequals(cipher_str, "GCM-AES-128"))
    {
        cipher_suite = MACsecMgr::MACsecProfile::CipherSuite::GCM_AES_128;
    }
    else if (boost::iequals(cipher_str, "GCM-AES-256"))
    {
        cipher_suite = MACsecMgr::MACsecProfile::CipherSuite::GCM_AES_256;
    }
    else if (boost::iequals(cipher_str, "GCM-AES-XPN-128"))
    {
        cipher_suite = MACsecMgr::MACsecProfile::CipherSuite::GCM_AES_XPN_128;
    }
    else if (boost::iequals(cipher_str, "GCM-AES-XPN-256"))
    {
        cipher_suite = MACsecMgr::MACsecProfile::CipherSuite::GCM_AES_XPN_256;
    }
    else
    {
        throw std::invalid_argument("Invalid cipher_suite : " + cipher_str);
    }
}



/* Decodes a Type 7 encoded input.
 *
 * The Type 7 encoding consists of two decimal digits(encoding the salt), followed a series of hexadecimal characters,
 * two for every byte in the encoded password. An example encoding(of "password") is 044B0A151C36435C0D.
 * This has a salt/offset of 4 (04 in the example), and encodes password via 4B0A151C36435C0D.
 *
 * The algorithm is a straightforward XOR Cipher that relies on the following ascii-encoded 53-byte constant:
 *    "dsfd;kfoA,.iyewrkldJKDHSUBsgvca69834ncxv9873254k;fg87"
 *
 * Decode()
 *    Get the salt index from the first 2 chars
 *    For each byte in the provided text after the encoded salt:
 *        j = (salt index + 1) % 53
 *        XOR the i'th byte of the password with the j'th byte of the magic constant.
 *        append to the decoded string.
 */
static std::string decodeKey(const std::string &cipher_str, const MACsecMgr::MACsecProfile::CipherSuite & cipher_suite)
{
    int salts[] = { 0x64, 0x73, 0x66, 0x64, 0x3B, 0x6B, 0x66, 0x6F, 0x41, 0x2C, 0x2E, 0x69, 0x79, 0x65, 0x77, 0x72, 0x6B, 0x6C, 0x64, 0x4A, 0x4B, 0x44, 0x48, 0x53, 0x55, 0x42, 0x73, 0x67, 0x76, 0x63, 0x61, 0x36, 0x39, 0x38, 0x33, 0x34, 0x6E, 0x63, 0x78, 0x76, 0x39, 0x38, 0x37, 0x33, 0x32, 0x35, 0x34, 0x6B, 0x3B, 0x66, 0x67, 0x38, 0x37 };

    std::string decodedPassword = std::string("");
    std::string cipher_hex_str = std::string("");
    unsigned int hex_int, saltIdx;

    if ((cipher_suite == MACsecMgr::MACsecProfile::CipherSuite::GCM_AES_128) ||
        (cipher_suite == MACsecMgr::MACsecProfile::CipherSuite::GCM_AES_XPN_128))
    {
        if (cipher_str.length() != AES_LEN_128_BYTE)
            throw std::invalid_argument("Invalid encoded CAK length");
    }
    else if ((cipher_suite == MACsecMgr::MACsecProfile::CipherSuite::GCM_AES_256) ||
             (cipher_suite == MACsecMgr::MACsecProfile::CipherSuite::GCM_AES_XPN_256))
    {
        if (cipher_str.length() != AES_LEN_256_BYTE)
            throw std::invalid_argument("Invalid encoded CAK length");
    }

    // Get the salt index from the cipher_str
    saltIdx = (unsigned int) stoi(cipher_str.substr(0,2));

    // Convert the hex string (eg: "aabbcc") to hex integers (eg: 0xaa, 0xbb, 0xcc) taking a substring of 2 chars at a time
    // and do xor with the magic salt string
    for (size_t i = 2; i < cipher_str.length(); i += 2) {
        std::stringstream ss;
        ss << std::hex << cipher_str.substr(i,2);
        ss >> hex_int;
        decodedPassword += (char)(hex_int ^ salts[saltIdx++ % (sizeof(salts)/sizeof(salts[0]))]);
    }

    return decodedPassword;
}

static bool normalizeCkn(std::string &ckn)
{
    if (ckn.empty() || ckn.size() > 128 || (ckn.size() % 2) != 0 ||
        !std::all_of(ckn.begin(), ckn.end(), [](unsigned char c) { return std::isxdigit(c); }))
    {
        return false;
    }
    std::transform(ckn.begin(), ckn.end(), ckn.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return true;
}

static bool validateEncodedCak(const std::string &cak, size_t expectedLength)
{
    if (cak.size() != expectedLength ||
        !std::isdigit(static_cast<unsigned char>(cak[0])) ||
        !std::isdigit(static_cast<unsigned char>(cak[1])) ||
        !std::all_of(
            cak.begin() + 2,
            cak.end(),
            [](unsigned char c) { return std::isxdigit(c); }))
    {
        return false;
    }

    return std::stoul(cak.substr(0, 2)) < 53;
}

static bool sameNonKeyProfile(const MACsecMgr::MACsecProfile &lhs, const MACsecMgr::MACsecProfile &rhs)
{
    return lhs.priority == rhs.priority &&
           lhs.cipher_suite == rhs.cipher_suite &&
           lhs.policy == rhs.policy &&
           static_cast<bool>(lhs.enable_replay_protect) == static_cast<bool>(rhs.enable_replay_protect) &&
           lhs.replay_window == rhs.replay_window &&
           static_cast<bool>(lhs.send_sci) == static_cast<bool>(rhs.send_sci) &&
           lhs.rekey_period == rhs.rekey_period;
}

static bool sameProfile(const MACsecMgr::MACsecProfile &lhs, const MACsecMgr::MACsecProfile &rhs)
{
    return sameNonKeyProfile(lhs, rhs) &&
           lhs.primary_cak == rhs.primary_cak &&
           lhs.primary_ckn == rhs.primary_ckn &&
           lhs.fallback_cak == rhs.fallback_cak &&
           lhs.fallback_ckn == rhs.fallback_ckn;
}

static const MKAParticipantStatus *findParticipant(
    const MKASessionStatus &status,
    const std::string &ckn)
{
    const auto participant = std::find_if(
        status.participants.begin(),
        status.participants.end(),
        [&](const MKAParticipantStatus &entry) { return entry.ckn == ckn; });
    return participant == status.participants.end() ? nullptr : &*participant;
}

static std::string utcTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    struct tm utc = {};
    gmtime_r(&time, &utc);
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

template<class T>
static bool get_value(
    const MACsecMgr::TaskArgs & ta,
    const std::string & field,
    T & value)
{
    SWSS_LOG_ENTER();

    auto value_opt = swss::fvsGetValue(ta, field, true);
    if (!value_opt)
    {
        SWSS_LOG_DEBUG("Cannot find field : %s", field.c_str());
        return false;
    }

    try
    {
        lexical_convert(*value_opt, value);
    }
    catch(const boost::bad_lexical_cast &e)
    {
        SWSS_LOG_ERROR("Cannot convert value(%s) in field(%s)", value_opt->c_str(), field.c_str());
        return false;
    }

    return true;
}

static void wpa_cli_commands(std::ostringstream & ostream)
{
    // Intentionally emtpy function to adapt
    // the recursively calling of wpa_cli_commands
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
        wpa_cli_commands(ostream, "set_network", network_id);
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

template<typename...Args>
static void wpa_cli_exec_sensitive_and_check(
    const std::string &sock,
    const std::string &port_name,
    const std::string &network_id,
    Args && ... args)
{
    std::ostringstream stream;
    std::string result;
    wpa_cli_commands(
        stream,
        sock,
        port_name,
        network_id,
        std::forward<Args>(args)...);
    if (swss::exec(stream.str(), result) != 0 || result.find("OK") != 0)
    {
        throw std::runtime_error("sensitive WPA control command failed");
    }
}

static std::string query_mka_status_with_timeout(
    const std::string &sock,
    const std::string &port_name)
{
    std::ostringstream command;
    std::string output;
    const std::string networkId;
    command << TIMEOUT_CMD << " --signal=KILL 2s ";
    wpa_cli_commands(command, sock, port_name, networkId, "macsec_mka_list");
    EXEC_WITH_ERROR_THROW(command.str(), output);
    return output;
}

MACsecMgr::MACsecMgr(
    DBConnector *cfgDb,
    DBConnector *stateDb,
    const vector<std::string> &tables) :
        Orch(cfgDb, tables),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_cfgPortTable(cfgDb, CFG_PORT_TABLE_NAME),
        m_stateMkaSessionTable(stateDb, STATE_MACSEC_MKA_SESSION_TABLE_NAME),
        m_stateMkaParticipantTable(stateDb, STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME)
{
    const auto interval = timespec {
        .tv_sec = MKA_STATUS_SWEEP_INTERVAL_SECONDS,
        .tv_nsec = 0,
    };
    m_mkaStatusTimer = new SelectableTimer(interval);
    auto executor = new ExecutableTimer(
        m_mkaStatusTimer,
        this,
        "MACSEC_MKA_STATUS_TIMER");
    Orch::addExecutor(executor);
    m_mkaStatusTimer->start();

    std::vector<std::string> keys;
    m_stateMkaSessionTable.getKeys(keys);
    for (const auto &key : keys)
    {
        m_stateMkaSessionTable.hset(key, "query_status", "error");
    }
}

MACsecMgr::~MACsecMgr()
{
    while (!m_macsec_ports.empty())
    {
        auto port = m_macsec_ports.begin();
        markMKAQueryError(port->first, port->second, "manager stopped");
        unconfigureMACsec(port->first, port->second);
        stopWPASupplicant(port->second.wpa_supplicant_pid);
        m_macsec_ports.erase(port);
    }
}

void MACsecMgr::doTask()
{
    Orch::doTask();

    if (!m_startupStateReconciled)
    {
        reconcileStartupState();
        m_startupStateReconciled = true;
    }

}

void MACsecMgr::doTask(SelectableTimer &timer)
{
    if (&timer != m_mkaStatusTimer)
    {
        SWSS_LOG_WARN("Unknown timer passed to MACsecMgr");
        return;
    }

    sweepMKAStatus();
}

void MACsecMgr::sweepMKAStatus()
{
    for (auto &port : m_macsec_ports)
    {
        collectMKAStatus(port.first, port.second);
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

    *this = MACsecProfile();

    // The following fields are optional
    const bool hasFallbackCak = GetValue(ta, fallback_cak);
    const bool hasFallbackCkn = GetValue(ta, fallback_ckn);
    if (hasFallbackCak != hasFallbackCkn)
    {
        return false;
    }
    if (!GetValue(ta, enable_replay_protect))
    {
        enable_replay_protect = false;
    }
    if (!GetValue(ta, replay_window))
    {
        replay_window = 0;
    }
    if (!GetValue(ta, send_sci))
    {
        send_sci = true;
    }
    if (!GetValue(ta, rekey_period))
    {
        rekey_period = 0;
    }
    if (!GetValue(ta, priority))
    {
        priority = 255;
    }
    if (!GetValue(ta, policy))
    {
        policy = Policy::SECURITY;
    }

    // The following fields are necessary
    if (!GetValue(ta, cipher_suite) ||
        !GetValue(ta, primary_cak) ||
        !GetValue(ta, primary_ckn) ||
        !normalizeCkn(primary_ckn) ||
        (hasFallbackCkn && !normalizeCkn(fallback_ckn)) ||
        (hasFallbackCkn && primary_ckn == fallback_ckn))
    {
        return false;
    }

    const auto expectedCakLength =
        cipher_suite == CipherSuite::GCM_AES_128 ||
        cipher_suite == CipherSuite::GCM_AES_XPN_128 ?
        AES_LEN_128_BYTE : AES_LEN_256_BYTE;
    return validateEncodedCak(primary_cak, expectedCakLength) &&
           (!hasFallbackCak || validateEncodedCak(fallback_cak, expectedCakLength));
}

task_process_status MACsecMgr::loadProfile(
    const std::string & profile_name,
    const TaskArgs & profile_attr)
{
    SWSS_LOG_ENTER();

    MACsecProfile desired;
    try
    {
        if (!desired.update(profile_attr))
        {
            SWSS_LOG_WARN("The MACsec profile '%s' is invalid", profile_name.c_str());
            return task_invalid_entry;
        }

        const auto existing = m_profiles.find(profile_name);
        if (existing == m_profiles.end())
        {
            m_profiles.emplace(profile_name, desired);
            SWSS_LOG_NOTICE("The MACsec profile '%s' is loaded", profile_name.c_str());
            return task_success;
        }

        existing->second = desired;
        std::vector<std::reference_wrapper<std::pair<const std::string, MKASession>>> attachedPorts;
        for (auto &port : m_macsec_ports)
        {
            if (port.second.profile_name == profile_name &&
                !sameProfile(port.second.applied_profile, desired))
            {
                attachedPorts.emplace_back(port);
            }
        }

        for (auto &portRef : attachedPorts)
        {
            auto &port = portRef.get();
            if (!preflightRollover(port.first, port.second, desired))
            {
                return task_need_retry;
            }
        }

        for (auto &portRef : attachedPorts)
        {
            auto &port = portRef.get();
            if (!reconcilePort(port.first, port.second, desired))
            {
                return task_need_retry;
            }
        }

        SWSS_LOG_NOTICE("The MACsec profile '%s' is reconciled", profile_name.c_str());
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

    // Handle existing macsec profile
    auto port_itr = m_macsec_ports.find(port_name);
    if (port_itr != m_macsec_ports.end())
    {
        if (port_itr->second.profile_name == profile_name)
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
                port_itr->second.profile_name.c_str(),
                port_name.c_str(),
                profile_name.c_str());
            auto result = disableMACsec(port_name, port_attr);
            if (result != task_success)
            {
                return result;
            }
        }
    }
    // Create MKA Session object
    auto port = m_macsec_ports.emplace(
        std::piecewise_construct,
        std::make_tuple(port_name),
        std::make_tuple());
    auto & session = port.first->second;
    session.profile_name = profile_name;
    session.applied_profile = profile;
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
    collectMKAStatus(port_name, session);
    return task_success;
}

task_process_status MACsecMgr::disableMACsec(
    const std::string & port_name,
    const TaskArgs & port_attr)
{
    SWSS_LOG_ENTER();

    deleteOperationalState(port_name);
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
    std::string oper_status;

    if (m_statePortTable.get(port_name, temp)
        && get_value(temp, "state", state)
        && state == "ok"
        && get_value(temp, "netdev_oper_status", oper_status)
        && oper_status == "up")
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
    MKASession & session,
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
        session.network_id = network_id;

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

        wpa_cli_exec_sensitive_and_check(
            session.sock,
            port_name,
            network_id,
            "mka_cak",
            decodeKey(profile.primary_cak, profile.cipher_suite));

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "mka_ckn",
            profile.primary_ckn);

        if (!profile.fallback_ckn.empty())
        {
            wpa_cli_exec_sensitive_and_check(
                session.sock,
                port_name,
                network_id,
                "mka_cak_fallback",
                decodeKey(profile.fallback_cak, profile.cipher_suite));

            wpa_cli_exec_and_check(
                session.sock,
                port_name,
                network_id,
                "mka_ckn_fallback",
                profile.fallback_ckn);
        }

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "mka_priority",
            profile.priority);

        if (profile.rekey_period)
        {
            wpa_cli_exec_and_check(
                session.sock,
                port_name,
                network_id,
                "mka_rekey_period",
                profile.rekey_period);
        }

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "macsec_ciphersuite",
            profile.cipher_suite);

        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            network_id,
            "macsec_include_sci",
            (profile.send_sci ? 1 : 0));

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

    // Retry interface_remove a few times in case wpa_supplicant is slow to
    // respond. This specifically targets the "command timed out" condition
    // seen in the field, to reduce spurious Task PORT - SET failures.
    static constexpr int MAX_INTERFACE_REMOVE_RETRIES = 3;

    for (int attempt = 1; attempt <= MAX_INTERFACE_REMOVE_RETRIES; ++attempt)
    {
        try
        {
            wpa_cli_exec_and_check(
                session.sock,
                "",
                "",
                "interface_remove",
                port_name);

            // Success on this attempt: no need to retry further.
            return true;
        }
        catch (const std::runtime_error &e)
        {
            const std::string error_message = e.what();
            // Best-effort cleanup semantics for interface_remove:
            //
            // 1. If wpa_cli returns "FAIL" for interface_remove, it typically means
            //    the interface is already gone from wpa_supplicant. From
            //    macsecmgr's perspective this is equivalent to a successful
            //    unconfigure, so treat it as success to avoid spurious
            //    Task PORT - SET failures.
            if (error_message.find("-> FAIL") != std::string::npos)
            {
                SWSS_LOG_NOTICE(
                    "interface_remove for port '%s' reported error '%s'; "
                    "treating MACsec unconfigure as best-effort success",
                    port_name.c_str(),
                    error_message.c_str());
                return true;
            }

            // 2. If the command times out, retry up to
            //    MAX_INTERFACE_REMOVE_RETRIES times. If all retries still time
            //    out, fall back to best-effort semantics: stopWPASupplicant()
            //    will still be invoked by the caller and will tear down the
            //    wpa_supplicant process (and its interfaces).
            if (error_message.find("command timed out") != std::string::npos)
            {
                if (attempt < MAX_INTERFACE_REMOVE_RETRIES)
                {
                    SWSS_LOG_WARN(
                        "interface_remove for port '%s' attempt %d/%d timed out: '%s'; retrying after 10 seconds",
                        port_name.c_str(),
                        attempt,
                        MAX_INTERFACE_REMOVE_RETRIES,
                        error_message.c_str());
                    std::this_thread::sleep_for(std::chrono::seconds(10));
                    continue;
                }

                SWSS_LOG_NOTICE(
                    "interface_remove for port '%s' timed out after %d attempts: '%s'; "
                    "ignoring timeouts and treating MACsec unconfigure as best-effort success",
                    port_name.c_str(),
                    MAX_INTERFACE_REMOVE_RETRIES,
                    error_message.c_str());
                return true;
            }

            // Any other error is treated as a real failure.
            SWSS_LOG_WARN("Disable MACsec fail : %s", error_message.c_str());
            return false;
        }
    }
    return true;
}

bool MACsecMgr::queryMKAStatus(
    const std::string &port_name,
    const MKASession &session,
    MKASessionStatus &status,
    std::string &error) const
{
    try
    {
        const auto output = query_mka_status_with_timeout(
            session.sock,
            port_name);
        return parseMKAStatus(output, status, error);
    }
    catch (const std::exception &)
    {
        error = "WPA status query failed";
        return false;
    }
}

bool MACsecMgr::validateExpectedParticipants(
    const MKASession &session,
    const MKASessionStatus &status,
    const MACsecProfile *desired,
    bool allowDesiredReplacement,
    std::string &error) const
{
    std::map<std::string, bool> fixed;
    if (!session.applied_profile.primary_ckn.empty())
    {
        fixed.emplace(session.applied_profile.primary_ckn, true);
    }
    if (!session.applied_profile.fallback_ckn.empty())
    {
        fixed.emplace(session.applied_profile.fallback_ckn, false);
    }

    std::map<std::string, bool> alternatives;
    if (allowDesiredReplacement && !session.pending_old_ckn.empty())
    {
        alternatives.emplace(session.pending_old_ckn, session.pending_primary);
        if (desired != nullptr)
        {
            alternatives.emplace(
                session.pending_primary ? desired->primary_ckn : desired->fallback_ckn,
                session.pending_primary);
        }
    }

    size_t alternativeCount = 0;
    for (const auto &participant : status.participants)
    {
        const auto expected = fixed.find(participant.ckn);
        if (expected != fixed.end())
        {
            if (participant.isPrimary != expected->second)
            {
                error = "runtime participant role differs from applied state";
                return false;
            }
            continue;
        }

        const auto alternative = alternatives.find(participant.ckn);
        if (alternative == alternatives.end() ||
            participant.isPrimary != alternative->second)
        {
            error = "runtime participant set differs from applied state";
            return false;
        }
        ++alternativeCount;
    }

    for (const auto &expected : fixed)
    {
        if (findParticipant(status, expected.first) == nullptr)
        {
            error = "runtime participant set is incomplete";
            return false;
        }
    }

    if (alternativeCount > 1 ||
        status.participants.size() != fixed.size() + alternativeCount)
    {
        error = "runtime participant set is ambiguous";
        return false;
    }

    return true;
}

bool MACsecMgr::collectMKAStatus(
    const std::string &port_name,
    MKASession &session,
    MKASessionStatus *statusResult,
    bool allowDesiredReplacement)
{
    MKASessionStatus status;
    std::string error;
    const auto desired = m_profiles.find(session.profile_name);
    const MACsecProfile *desiredProfile =
        desired == m_profiles.end() ? nullptr : &desired->second;

    if (!queryMKAStatus(port_name, session, status, error) ||
        !validateExpectedParticipants(
            session,
            status,
            desiredProfile,
            allowDesiredReplacement,
            error))
    {
        markMKAQueryError(port_name, session, error);
        return false;
    }

    if (allowDesiredReplacement &&
        desiredProfile != nullptr &&
        !session.pending_old_ckn.empty())
    {
        const auto &replacementCkn =
            session.pending_primary ?
            desiredProfile->primary_ckn :
            desiredProfile->fallback_ckn;
        if (findParticipant(status, replacementCkn) != nullptr)
        {
            if (session.pending_primary)
            {
                session.applied_profile.primary_ckn = desiredProfile->primary_ckn;
                session.applied_profile.primary_cak = desiredProfile->primary_cak;
            }
            else
            {
                session.applied_profile.fallback_ckn = desiredProfile->fallback_ckn;
                session.applied_profile.fallback_cak = desiredProfile->fallback_cak;
            }
            session.pending_old_ckn.clear();
            session.pending_primary = false;
        }
    }

    publishMKAStatus(port_name, session, status);
    if (statusResult != nullptr)
    {
        *statusResult = status;
    }
    return true;
}

void MACsecMgr::publishMKAStatus(
    const std::string &port_name,
    MKASession &session,
    const MKASessionStatus &status)
{
    auto sessionValues = status.toFieldValues();
    sessionValues.emplace_back("profile", session.profile_name);
    sessionValues.emplace_back("query_status", "ok");
    sessionValues.emplace_back("last_updated", utcTimestamp());
    m_stateMkaSessionTable.set(port_name, sessionValues);

    std::set<std::string> publishedKeys;
    for (const auto &participant : status.participants)
    {
        const auto key = port_name + "|" + participant.ckn;
        publishedKeys.insert(key);
        m_stateMkaParticipantTable.set(key, participant.toFieldValues());
    }

    std::vector<std::string> existingKeys;
    m_stateMkaParticipantTable.getKeys(existingKeys);
    const auto prefix = port_name + "|";
    for (const auto &key : existingKeys)
    {
        if (key.compare(0, prefix.size(), prefix) == 0 &&
            publishedKeys.find(key) == publishedKeys.end())
        {
            m_stateMkaParticipantTable.del(key);
        }
    }

    setConfigState(port_name, session, "");
}

void MACsecMgr::markMKAQueryError(
    const std::string &port_name,
    MKASession &session,
    const std::string &reason)
{
    m_stateMkaSessionTable.set(port_name, {
        {"profile", session.profile_name},
        {"query_status", "error"},
    });
    setConfigState(port_name, session, "");
    SWSS_LOG_WARN("MKA status query for port '%s' failed: %s",
                  port_name.c_str(), reason.c_str());
}

void MACsecMgr::setConfigState(
    const std::string &port_name,
    MKASession &session,
    const std::string &error)
{
    const auto desired = m_profiles.find(session.profile_name);
    const bool inSync =
        desired != m_profiles.end() &&
        session.pending_old_ckn.empty() &&
        sameProfile(session.applied_profile, desired->second);

    if (inSync)
    {
        session.config_error.clear();
        m_stateMkaSessionTable.hset(port_name, "config_status", "in-sync");
        m_stateMkaSessionTable.hdel(port_name, "config_error");
        return;
    }

    if (!error.empty())
    {
        session.config_error = error;
    }
    else if (session.config_error.empty())
    {
        session.config_error = "desired profile is not applied";
    }

    m_stateMkaSessionTable.set(port_name, {
        {"config_status", "degraded"},
        {"config_error", session.config_error},
    });
}

void MACsecMgr::deleteOperationalState(const std::string &port_name)
{
    m_stateMkaSessionTable.del(port_name);
    std::vector<std::string> keys;
    m_stateMkaParticipantTable.getKeys(keys);
    const auto prefix = port_name + "|";
    for (const auto &key : keys)
    {
        if (key.compare(0, prefix.size(), prefix) == 0)
        {
            m_stateMkaParticipantTable.del(key);
        }
    }
}

void MACsecMgr::reconcileStartupState()
{
    std::set<std::string> configuredPorts;
    std::vector<std::string> portKeys;
    m_cfgPortTable.getKeys(portKeys);
    for (const auto &port : portKeys)
    {
        std::vector<FieldValueTuple> values;
        std::string profile;
        if (m_cfgPortTable.get(port, values) &&
            get_value(values, "macsec", profile) &&
            !profile.empty())
        {
            configuredPorts.insert(port);
        }
    }

    std::vector<std::string> sessionKeys;
    m_stateMkaSessionTable.getKeys(sessionKeys);
    for (const auto &port : sessionKeys)
    {
        if (configuredPorts.find(port) == configuredPorts.end())
        {
            deleteOperationalState(port);
        }
        else
        {
            m_stateMkaSessionTable.hset(port, "query_status", "error");
        }
    }

    std::vector<std::string> participantKeys;
    m_stateMkaParticipantTable.getKeys(participantKeys);
    for (const auto &key : participantKeys)
    {
        const auto separator = key.find('|');
        const auto port = key.substr(0, separator);
        if (separator == std::string::npos ||
            configuredPorts.find(port) == configuredPorts.end())
        {
            m_stateMkaParticipantTable.del(key);
        }
    }

    for (auto &port : m_macsec_ports)
    {
        collectMKAStatus(port.first, port.second);
    }
}

bool MACsecMgr::preflightRollover(
    const std::string &port_name,
    MKASession &session,
    const MACsecProfile &desired)
{
    if (!sameNonKeyProfile(session.applied_profile, desired) ||
        session.applied_profile.fallback_ckn.empty() != desired.fallback_ckn.empty())
    {
        setConfigState(port_name, session, "profile update is not a single-key rollover");
        return false;
    }

    const bool primaryChanged =
        session.applied_profile.primary_ckn != desired.primary_ckn ||
        session.applied_profile.primary_cak != desired.primary_cak ||
        (session.pending_primary && !session.pending_old_ckn.empty());
    const bool fallbackChanged =
        session.applied_profile.fallback_ckn != desired.fallback_ckn ||
        session.applied_profile.fallback_cak != desired.fallback_cak ||
        (!session.pending_primary && !session.pending_old_ckn.empty());

    if (!primaryChanged && !fallbackChanged)
    {
        setConfigState(port_name, session, "");
        return true;
    }
    if (primaryChanged == fallbackChanged ||
        (primaryChanged &&
         session.applied_profile.primary_ckn == desired.primary_ckn &&
         session.applied_profile.primary_cak != desired.primary_cak) ||
        (fallbackChanged &&
         session.applied_profile.fallback_ckn == desired.fallback_ckn &&
         session.applied_profile.fallback_cak != desired.fallback_cak))
    {
        setConfigState(port_name, session, "profile update is not a supported CKN replacement");
        return false;
    }

    const bool rotatingPrimary = primaryChanged;
    const auto &alternateCkn =
        rotatingPrimary ? desired.fallback_ckn : desired.primary_ckn;
    if (alternateCkn.empty())
    {
        setConfigState(port_name, session, "no alternate participant is configured");
        return false;
    }

    MKASessionStatus status;
    if (!collectMKAStatus(port_name, session, &status, true))
    {
        setConfigState(port_name, session, "fresh MKA status is unavailable");
        return false;
    }

    if (sameProfile(session.applied_profile, desired))
    {
        return true;
    }

    if (status.kayStatus != "active" ||
        status.authenticated ||
        !status.secured ||
        status.failed)
    {
        setConfigState(port_name, session, "MKA session is not healthy");
        return false;
    }

    const auto alternate = findParticipant(status, alternateCkn);
    if (alternate == nullptr ||
        alternate->isPrimary == rotatingPrimary ||
        !alternate->active ||
        alternate->livePeers == 0)
    {
        setConfigState(port_name, session, "alternate participant is not live and role-correct");
        return false;
    }

    const auto &selectedCkn =
        !session.pending_old_ckn.empty() ?
        session.pending_old_ckn :
        (rotatingPrimary ?
         session.applied_profile.primary_ckn :
         session.applied_profile.fallback_ckn);
    const auto selected = findParticipant(status, selectedCkn);
    if (selected != nullptr && selected->isPrimary != rotatingPrimary)
    {
        setConfigState(port_name, session, "selected participant role is inconsistent");
        return false;
    }

    return true;
}

bool MACsecMgr::reconcilePort(
    const std::string &port_name,
    MKASession &session,
    const MACsecProfile &desired)
{
    if (!preflightRollover(port_name, session, desired))
    {
        return false;
    }
    if (sameProfile(session.applied_profile, desired))
    {
        setConfigState(port_name, session, "");
        return true;
    }

    const bool primaryChanged =
        session.applied_profile.primary_ckn != desired.primary_ckn ||
        session.applied_profile.primary_cak != desired.primary_cak ||
        (session.pending_primary && !session.pending_old_ckn.empty());
    const bool primary = primaryChanged;
    MKASessionStatus status;
    if (!collectMKAStatus(port_name, session, &status, true))
    {
        setConfigState(port_name, session, "fresh MKA status is unavailable");
        return false;
    }

    const auto oldCkn =
        !session.pending_old_ckn.empty() ?
        session.pending_old_ckn :
        (primary ?
         session.applied_profile.primary_ckn :
         session.applied_profile.fallback_ckn);
    if (!oldCkn.empty() && findParticipant(status, oldCkn) != nullptr)
    {
        if (!removeParticipant(port_name, session, oldCkn))
        {
            setConfigState(port_name, session, "failed to remove selected participant");
            return false;
        }

        session.pending_old_ckn = oldCkn;
        session.pending_primary = primary;
        if (primary)
        {
            session.applied_profile.primary_ckn.clear();
            session.applied_profile.primary_cak.clear();
        }
        else
        {
            session.applied_profile.fallback_ckn.clear();
            session.applied_profile.fallback_cak.clear();
        }
        setConfigState(port_name, session, "selected participant removed; replacement pending");

        MKASessionStatus postRemoveStatus;
        if (!collectMKAStatus(port_name, session, &postRemoveStatus, true) ||
            findParticipant(postRemoveStatus, oldCkn) != nullptr)
        {
            setConfigState(port_name, session, "selected participant removal is not yet verified");
            return false;
        }
    }

    if (!updateNetworkParticipant(port_name, session, desired, primary))
    {
        setConfigState(port_name, session, "failed to update participant network configuration");
        return false;
    }
    if (!addParticipant(port_name, session, desired, primary))
    {
        setConfigState(port_name, session, "failed to add replacement participant");
        return false;
    }

    if (primary)
    {
        session.applied_profile.primary_ckn = desired.primary_ckn;
        session.applied_profile.primary_cak = desired.primary_cak;
    }
    else
    {
        session.applied_profile.fallback_ckn = desired.fallback_ckn;
        session.applied_profile.fallback_cak = desired.fallback_cak;
    }
    session.pending_old_ckn.clear();
    session.pending_primary = false;
    session.config_error.clear();
    setConfigState(port_name, session, "");
    collectMKAStatus(port_name, session);
    return true;
}

bool MACsecMgr::removeParticipant(
    const std::string &port_name,
    const MKASession &session,
    const std::string &ckn) const
{
    try
    {
        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            "",
            "macsec_del_mka",
            "ckn=" + ckn);
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool MACsecMgr::updateNetworkParticipant(
    const std::string &port_name,
    const MKASession &session,
    const MACsecProfile &desired,
    bool primary) const
{
    try
    {
        const auto &cak = primary ? desired.primary_cak : desired.fallback_cak;
        const auto &ckn = primary ? desired.primary_ckn : desired.fallback_ckn;
        wpa_cli_exec_sensitive_and_check(
            session.sock,
            port_name,
            session.network_id,
            primary ? "mka_cak" : "mka_cak_fallback",
            decodeKey(cak, desired.cipher_suite));
        wpa_cli_exec_and_check(
            session.sock,
            port_name,
            session.network_id,
            primary ? "mka_ckn" : "mka_ckn_fallback",
            ckn);
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool MACsecMgr::addParticipant(
    const std::string &port_name,
    const MKASession &session,
    const MACsecProfile &desired,
    bool primary) const
{
    try
    {
        const auto &cak = primary ? desired.primary_cak : desired.fallback_cak;
        const auto &ckn = primary ? desired.primary_ckn : desired.fallback_ckn;
        if (primary)
        {
            wpa_cli_exec_sensitive_and_check(
                session.sock,
                port_name,
                "",
                "macsec_add_mka",
                "ckn=" + ckn,
                "cak=" + decodeKey(cak, desired.cipher_suite));
        }
        else
        {
            wpa_cli_exec_sensitive_and_check(
                session.sock,
                port_name,
                "",
                "macsec_add_mka",
                "ckn=" + ckn,
                "cak=" + decodeKey(cak, desired.cipher_suite),
                "fallback=1");
        }
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}
