#include "macsecmgr.h"

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
#include <set>
#include <tuple>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iomanip>


using namespace std;
using namespace swss;

#define WPA_SUPPLICANT_CMD "/sbin/wpa_supplicant"
#define WPA_CLI_CMD        "/sbin/wpa_cli"
#define WPA_CONF           "/etc/wpa_supplicant.conf"
#define SOCK_DIR           "/var/run/"
// How long enableMACsec waits for orchagent to consume the DEL of a stale
// MACSEC_PORT_TABLE entry before giving up and retrying the task later.
#define CLEAR_STALE_TIMEOUT_MS  10000
#define CLEAR_STALE_POLL_MS     100

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
        throw std::invalid_argument("Invalid MACsec policy");
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
        throw std::invalid_argument("Invalid MACsec cipher suite");
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

    return true;
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

static bool selectPrimaryReplacement(
    const MACsecMgr::MKASession &session,
    const MACsecMgr::MACsecProfile &desired)
{
    if (!session.pending_old_ckn.empty())
    {
        return session.pending_primary;
    }

    const auto &applied = session.applied_profile;
    const bool primaryChanged =
        applied.primary_ckn != desired.primary_ckn ||
        applied.primary_cak != desired.primary_cak;
    const bool fallbackChanged =
        applied.fallback_ckn != desired.fallback_ckn ||
        applied.fallback_cak != desired.fallback_cak;
    if (!primaryChanged || !fallbackChanged)
    {
        return primaryChanged;
    }

    return desired.primary_ckn != applied.fallback_ckn;
}

static bool usePendingReplacement(
    const MACsecMgr::MKASession &session,
    const MACsecMgr::MACsecProfile &desired,
    bool primary)
{
    const auto &alternateCkn =
        primary ? session.applied_profile.fallback_ckn : session.applied_profile.primary_ckn;
    const auto &desiredCkn = primary ? desired.primary_ckn : desired.fallback_ckn;
    return !session.pending_old_ckn.empty() &&
           session.pending_primary == primary &&
           !session.pending_new_ckn.empty() &&
           !session.pending_new_cak.empty() &&
           desiredCkn == alternateCkn &&
           session.pending_new_ckn != alternateCkn &&
           session.pending_new_ckn != (primary ? desired.fallback_ckn : desired.primary_ckn);
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
        SWSS_LOG_ERROR("Cannot convert MACsec field '%s'", field.c_str());
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

// swss::exec logs the full command at DEBUG, including WPA key arguments.
template<typename...Args>
static std::string wpa_cli_exec(
    const std::string &sock,
    const std::string &port_name,
    const std::string &network_id,
    const char *operation,
    Args && ... args)
{
    std::ostringstream command;
    if (strcmp(operation, "macsec_mka_list") == 0)
    {
        command << TIMEOUT_CMD << " --signal=KILL 2s ";
    }
    wpa_cli_commands(
        command,
        sock,
        port_name,
        network_id,
        operation,
        std::forward<Args>(args)...);
    const std::string context =
        std::string("WPA control ") + operation + " on port '" + port_name + "'";
    SWSS_LOG_DEBUG("%s", context.c_str());
    FILE *pipe = popen((command.str() + " 2>&1").c_str(), "r");
    if (pipe == nullptr)
    {
        throw std::runtime_error(context + ": cannot execute command");
    }

    std::string output;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        output += buffer;
    }
    const bool readFailed = ferror(pipe) != 0;
    const int status = pclose(pipe);
    if (readFailed || status == -1)
    {
        throw std::runtime_error(context + ": command I/O failed");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        const bool timedOut = output.find("command timed out") != std::string::npos ||
            (WIFEXITED(status) && WEXITSTATUS(status) == 124) ||
            (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        if (timedOut)
        {
            throw std::runtime_error(context + ": command timed out");
        }
        if (WIFEXITED(status))
        {
            throw std::runtime_error(
                context + ": command failed (exit " + std::to_string(WEXITSTATUS(status)) + ")");
        }
        if (WIFSIGNALED(status))
        {
            throw std::runtime_error(
                context + ": command terminated by signal " + std::to_string(WTERMSIG(status)));
        }
        throw std::runtime_error(context + ": unexpected command wait status");
    }
    return output;
}

template<typename...Args>
static void wpa_cli_exec_and_check(
    const std::string & sock,
    const std::string & port_name,
    const std::string & network_id,
    const char *operation,
    Args && ... args)
{
    std::string res = wpa_cli_exec(
        sock,
        port_name,
        network_id,
        operation,
        std::forward<Args>(args)...);
    if (res.find("OK") != 0)
    {
        const std::string reason =
            res.find("command timed out") != std::string::npos ? "command timed out" :
            (res.find("FAIL") == 0 ? "-> FAIL" : "invalid response");
        throw std::runtime_error(
            std::string("WPA control ") + operation + " on port '" + port_name + "': " + reason);
    }
}

MACsecMgr::MACsecMgr(
    DBConnector *cfgDb,
    DBConnector *appDb,
    DBConnector *stateDb,
    const vector<std::string> &tables) :
        Orch(cfgDb, tables),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateMkaSessionTable(stateDb, STATE_MACSEC_MKA_SESSION_TABLE_NAME),
        m_stateMkaParticipantTable(stateDb, STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME),
        m_clearStaleTimeoutMs(CLEAR_STALE_TIMEOUT_MS)
{
    auto profileConsumer = dynamic_cast<Consumer *>(getExecutor(CFG_MACSEC_PROFILE_TABLE_NAME));
    if (profileConsumer == nullptr)
    {
        SWSS_LOG_THROW("MACsec profile consumer is missing or has the wrong type");
    }
    profileConsumer->setRecordable(false);

    clearMKAState();

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

    // SA and SC entries first, the PORT entry last: the PORT DEL is the one
    // orchagent acts on (it cascades to every SC and SA under the port), so it
    // must be the last thing written and the one waited for.
    for (const auto &name : {APP_MACSEC_EGRESS_SA_TABLE_NAME,
                             APP_MACSEC_INGRESS_SA_TABLE_NAME,
                             APP_MACSEC_EGRESS_SC_TABLE_NAME,
                             APP_MACSEC_INGRESS_SC_TABLE_NAME,
                             APP_MACSEC_PORT_TABLE_NAME})
    {
        AppMACsecTable t;
        t.visible.reset(new Table(appDb, name));
        t.producer.reset(new ProducerStateTable(appDb, name));
        // A SET a producer has written but the consumer has not popped lives in
        // the staging hash under the table name prefixed by the state hash prefix.
        t.staged.reset(new Table(appDb, t.producer->getStateHashPrefix() + name));
        m_appMACsecTables.push_back(std::move(t));
    }
}

MACsecMgr::~MACsecMgr()
{
    clearMKAState();
    while (!m_macsec_ports.empty())
    {
        auto port = m_macsec_ports.begin();
        unconfigureMACsec(port->first, port->second);
        stopWPASupplicant(port->second.wpa_supplicant_pid);
        m_macsec_ports.erase(port);
    }
}

void MACsecMgr::doTask()
{
    Orch::doTask();
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
        const auto desired = m_profiles.find(port.second.profile_name);
        if (desired != m_profiles.end() &&
            (!sameProfile(port.second.applied_profile, desired->second) ||
             !port.second.pending_old_ckn.empty()))
        {
            if (!reconcilePort(port.first, port.second, desired->second) &&
                port.second.pending_old_ckn.empty())
            {
                collectMKAStatus(port.first, port.second);
            }
        }
        else
        {
            MKASessionStatus status;
            if (collectMKAStatus(port.first, port.second, &status) &&
                !port.second.participants_verified &&
                desired != m_profiles.end())
            {
                reconcileObservedParticipants(
                    port.first, port.second, desired->second, status);
            }
        }
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
        !normalizeCkn(primary_ckn, primary_ckn) ||
        (hasFallbackCkn && !normalizeCkn(fallback_ckn, fallback_ckn)) ||
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

        // Accept desired state; the timer owns retries for deferred runtime.
        existing->second = desired;
        for (auto &port : m_macsec_ports)
        {
            if (port.second.profile_name == profile_name &&
                (!sameProfile(port.second.applied_profile, desired) ||
                 !port.second.pending_old_ckn.empty()))
            {
                reconcilePort(port.first, port.second, desired);
            }
        }

        SWSS_LOG_NOTICE("The MACsec profile '%s' desired state is cached", profile_name.c_str());
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
    // A previous wpa_supplicant that was killed without running its deinit
    // leaves its APPL_DB entries, and with them orchagent's SAI objects,
    // behind. They have to go before the new session writes, or orchagent
    // sees SETs on objects it already holds and keeps the old key.
    if (!clearStaleMACsecState(port_name))
    {
        return task_need_retry;
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

bool MACsecMgr::clearStaleMACsecState(const std::string & port_name)
{
    SWSS_LOG_ENTER();

    // Keys are "<port>" in the PORT table, "<port>:<sci>" in the SC tables
    // and "<port>:<sci>:<an>" in the SA tables. The ':' keeps Ethernet1 from
    // matching Ethernet10.
    const std::string prefix = port_name + ":";
    auto belongs_to_port = [&](const std::string & key)
    {
        return key == port_name || key.compare(0, prefix.size(), prefix) == 0;
    };

    bool port_entry_found = false;
    for (auto & table : m_appMACsecTables)
    {
        // Both what orchagent has already consumed and what the dead
        // supplicant wrote but orchagent has not popped yet: a SET still in the
        // staging hash would otherwise be applied on top of the new session.
        std::set<std::string> keys;
        for (auto * t : {table.visible.get(), table.staged.get()})
        {
            std::vector<std::string> found;
            t->getKeys(found);
            keys.insert(found.begin(), found.end());
        }
        const std::string & name = table.visible->getTableName();
        for (const auto & key : keys)
        {
            if (!belongs_to_port(key))
            {
                continue;
            }
            SWSS_LOG_NOTICE("Removing stale %s entry '%s' left by a previous MKA session on the port '%s'",
                name.c_str(), key.c_str(), port_name.c_str());
            // Through the producer: it drops the staged SET, if any, in the same
            // step and queues a DEL for orchagent. For the PORT entry the DEL is
            // what tears the SAI port, SCs, SAs, flows and ACL entries down.
            table.producer->del(key);
            if (name == APP_MACSEC_PORT_TABLE_NAME)
            {
                port_entry_found = true;
            }
        }
    }
    if (!port_entry_found)
    {
        return true;
    }

    // Wait until orchagent has popped the DEL: the consumer removes the key
    // from the table and the staging hash when it does. Only then can the new
    // supplicant write its SET without ProducerStateTable folding the two into
    // a single SET, which would reproduce the stale state this clears.
    auto & port_table = m_appMACsecTables.back();
    std::vector<FieldValueTuple> fvs;
    for (unsigned int waited = 0; waited < m_clearStaleTimeoutMs; waited += CLEAR_STALE_POLL_MS)
    {
        if (!port_table.visible->get(port_name, fvs) && !port_table.staged->get(port_name, fvs))
        {
            SWSS_LOG_NOTICE("Stale MACsec state on the port '%s' has been cleared", port_name.c_str());
            return true;
        }
        usleep(CLEAR_STALE_POLL_MS * 1000);
    }
    SWSS_LOG_WARN("orchagent did not clear the stale MACsec state on the port '%s' within %u ms",
        port_name.c_str(), m_clearStaleTimeoutMs);
    return false;
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
            throw std::runtime_error("Cannot add MACsec network: invalid response");
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

        wpa_cli_exec_and_check(
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
            wpa_cli_exec_and_check(
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
        const auto output = wpa_cli_exec(
            session.sock,
            port_name,
            "",
            "macsec_mka_list");
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

        error = "runtime participant set differs from applied state";
        return false;
    }

    for (const auto &expected : fixed)
    {
        if (findParticipant(status, expected.first) == nullptr)
        {
            error = "runtime participant set is incomplete";
            return false;
        }
    }

    if (status.participants.size() != fixed.size())
    {
        error = "runtime participant set is ambiguous";
        return false;
    }

    return true;
}

bool MACsecMgr::collectMKAStatus(
    const std::string &port_name,
    MKASession &session,
    MKASessionStatus *statusResult)
{
    MKASessionStatus status;
    std::string error;

    if (!queryMKAStatus(port_name, session, status, error))
    {
        handleMKAQueryFailure(port_name, session, error);
        return false;
    }

    if (!session.pending_old_ckn.empty())
    {
        const auto replacement = findParticipant(status, session.pending_new_ckn);
        if (replacement != nullptr &&
            replacement->isPrimary == session.pending_primary &&
            findParticipant(status, session.pending_old_ckn) == nullptr)
        {
            if (session.pending_primary)
            {
                session.applied_profile.primary_ckn = session.pending_new_ckn;
                session.applied_profile.primary_cak = session.pending_new_cak;
            }
            else
            {
                session.applied_profile.fallback_ckn = session.pending_new_ckn;
                session.applied_profile.fallback_cak = session.pending_new_cak;
            }
            session.pending_old_ckn.clear();
            session.pending_primary = false;
            session.pending_new_ckn.clear();
            session.pending_new_cak.clear();
        }
    }

    session.participants_verified =
        session.pending_old_ckn.empty() &&
        validateExpectedParticipants(session, status, error);
    publishMKAStatus(port_name, session, status);
    if (!session.participants_verified && session.pending_old_ckn.empty())
    {
        setConfigState(port_name, session, error);
    }
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
    m_stateMkaSessionTable.del(port_name);
    session.status_published = false;
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

    auto sessionValues = status.toFieldValues();
    sessionValues.emplace_back("profile", session.profile_name);
    sessionValues.emplace_back("query_status", "ok");
    sessionValues.emplace_back("last_updated", utcTimestamp());
    m_stateMkaSessionTable.set(port_name, sessionValues);
    session.status_published = true;
    setConfigState(port_name, session, "");
}

void MACsecMgr::handleMKAQueryFailure(
    const std::string &port_name,
    MKASession &session,
    const std::string &reason)
{
    session.participants_verified = false;
    session.status_published = false;
    deleteOperationalState(port_name);
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
        session.participants_verified &&
        session.pending_old_ckn.empty() &&
        sameProfile(session.applied_profile, desired->second);

    if (inSync)
    {
        session.config_error.clear();
        if (session.status_published)
        {
            m_stateMkaSessionTable.hset(port_name, "config_status", "in-sync");
            m_stateMkaSessionTable.hdel(port_name, "config_error");
        }
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

    if (session.status_published)
    {
        m_stateMkaSessionTable.set(port_name, {
            {"config_status", "degraded"},
            {"config_error", session.config_error},
        });
    }
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

void MACsecMgr::clearMKAState()
{
    // The two MKA tables are owned solely by macsecmgrd in this STATE_DB
    // namespace; never clear other state tables or another DB connector.
    std::vector<std::string> sessionKeys;
    m_stateMkaSessionTable.getKeys(sessionKeys);
    for (const auto &port : sessionKeys)
    {
        m_stateMkaSessionTable.del(port);
    }

    std::vector<std::string> participantKeys;
    m_stateMkaParticipantTable.getKeys(participantKeys);
    for (const auto &key : participantKeys)
    {
        m_stateMkaParticipantTable.del(key);
    }
}

bool MACsecMgr::selectParticipantToUpdate(
    const std::string &port_name,
    MKASession &session,
    const MACsecProfile &desired,
    bool &primary)
{
    const bool hasAppliedFallback =
        !session.applied_profile.fallback_ckn.empty() ||
        (!session.pending_old_ckn.empty() && !session.pending_primary);
    if (!sameNonKeyProfile(session.applied_profile, desired) ||
        hasAppliedFallback != !desired.fallback_ckn.empty())
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
    if ((primaryChanged &&
         session.applied_profile.primary_ckn == desired.primary_ckn &&
         session.applied_profile.primary_cak != desired.primary_cak) ||
        (fallbackChanged &&
         session.applied_profile.fallback_ckn == desired.fallback_ckn &&
         session.applied_profile.fallback_cak != desired.fallback_cak))
    {
        setConfigState(port_name, session, "profile update is not a supported CKN replacement");
        return false;
    }

    if (sameProfile(session.applied_profile, desired))
    {
        return true;
    }

    primary = selectPrimaryReplacement(session, desired);
    const auto &replacementCkn =
        usePendingReplacement(session, desired, primary) ?
        session.pending_new_ckn :
        (primary ? desired.primary_ckn : desired.fallback_ckn);
    const auto &alternateCkn =
        primary ? session.applied_profile.fallback_ckn : session.applied_profile.primary_ckn;
    if (replacementCkn == alternateCkn)
    {
        setConfigState(port_name, session, "replacement CKN conflicts with the applied alternate");
        return false;
    }

    return true;
}

bool MACsecMgr::reconcilePort(
    const std::string &port_name,
    MKASession &session,
    const MACsecProfile &desired)
{
    // A superseded pending target may need a bridge before the two final roles.
    for (size_t step = 0; step < 3; ++step)
    {
        if (!reconcileParticipant(port_name, session, desired))
        {
            return false;
        }
        if (sameProfile(session.applied_profile, desired) && session.pending_old_ckn.empty())
        {
            return true;
        }
    }
    return false;
}

bool MACsecMgr::reconcileObservedParticipants(
    const std::string &port_name,
    MKASession &session,
    const MACsecProfile &desired,
    const MKASessionStatus &status)
{
    for (const auto &participant : status.participants)
    {
        const auto &expected = participant.isPrimary ?
            desired.primary_ckn : desired.fallback_ckn;
        if (participant.ckn != expected)
        {
            if (!removeParticipant(port_name, session, participant.ckn))
            {
                setConfigState(port_name, session, "failed to remove unexpected participant");
                return false;
            }
        }
    }

    for (const bool primary : {true, false})
    {
        const auto &ckn = primary ? desired.primary_ckn : desired.fallback_ckn;
        if (ckn.empty())
        {
            continue;
        }
        const auto observed = findParticipant(status, ckn);
        if (observed != nullptr && observed->isPrimary == primary)
        {
            continue;
        }
        if (!updateNetworkParticipant(port_name, session, desired, primary))
        {
            setConfigState(port_name, session, "failed to update participant network configuration");
            return false;
        }
        if (!addParticipant(port_name, session, desired, primary))
        {
            setConfigState(port_name, session, "failed to add missing participant");
            return false;
        }
    }

    return collectMKAStatus(port_name, session);
}

bool MACsecMgr::reconcileParticipant(
    const std::string &port_name,
    MKASession &session,
    const MACsecProfile &desired)
{
    bool primary = false;
    MKASessionStatus status;
    bool haveStatus = false;
    if (!session.pending_old_ckn.empty())
    {
        if (!collectMKAStatus(port_name, session, &status))
        {
            return false;
        }
        haveStatus = true;
    }
    if (!selectParticipantToUpdate(port_name, session, desired, primary))
    {
        return false;
    }
    if (sameProfile(session.applied_profile, desired) && session.pending_old_ckn.empty())
    {
        setConfigState(port_name, session, "");
        return true;
    }

    MACsecProfile replacement = desired;
    if (usePendingReplacement(session, desired, primary))
    {
        if (primary)
        {
            replacement.primary_ckn = session.pending_new_ckn;
            replacement.primary_cak = session.pending_new_cak;
        }
        else
        {
            replacement.fallback_ckn = session.pending_new_ckn;
            replacement.fallback_cak = session.pending_new_cak;
        }
    }

    const auto oldCkn =
        !session.pending_old_ckn.empty() ?
        session.pending_old_ckn :
        (primary ?
         session.applied_profile.primary_ckn :
         session.applied_profile.fallback_ckn);
    const bool pending = !session.pending_old_ckn.empty();
    if (!oldCkn.empty() &&
        (!pending || (haveStatus && findParticipant(status, oldCkn) != nullptr)))
    {
        const bool removeAccepted = removeParticipant(port_name, session, oldCkn);
        session.pending_old_ckn = oldCkn;
        session.pending_primary = primary;
        session.pending_new_ckn = primary ? replacement.primary_ckn : replacement.fallback_ckn;
        session.pending_new_cak = primary ? replacement.primary_cak : replacement.fallback_cak;
        session.participants_verified = false;
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
        setConfigState(
            port_name, session,
            removeAccepted ? "selected participant removed; replacement pending" :
                             "selected participant removal is unverified");

        MKASessionStatus postRemoveStatus;
        if (collectMKAStatus(port_name, session, &postRemoveStatus))
        {
            if (findParticipant(postRemoveStatus, oldCkn) != nullptr)
            {
                setConfigState(port_name, session, "selected participant removal is not yet verified");
                return false;
            }
            if (session.pending_old_ckn.empty())
            {
                return true;
            }
        }
        else if (!removeAccepted)
        {
            return false;
        }
    }

    session.pending_new_ckn = primary ? replacement.primary_ckn : replacement.fallback_ckn;
    session.pending_new_cak = primary ? replacement.primary_cak : replacement.fallback_cak;
    session.participants_verified = false;
    if (!updateNetworkParticipant(port_name, session, replacement, primary))
    {
        setConfigState(port_name, session, "failed to update participant network configuration");
        return false;
    }
    if (!addParticipant(port_name, session, replacement, primary))
    {
        setConfigState(port_name, session, "failed to add replacement participant");
        return false;
    }

    if (!collectMKAStatus(port_name, session))
    {
        return false;
    }
    if (!session.pending_old_ckn.empty())
    {
        setConfigState(
            port_name,
            session,
            "replacement participant is not yet verified");
        return false;
    }

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
        wpa_cli_exec_and_check(
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
            wpa_cli_exec_and_check(
                session.sock,
                port_name,
                "",
                "macsec_add_mka",
                "ckn=" + ckn,
                "cak=" + decodeKey(cak, desired.cipher_suite));
        }
        else
        {
            wpa_cli_exec_and_check(
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
