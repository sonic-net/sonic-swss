#include "gtest/gtest.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <csignal>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../mock_table.h"
#include "redisutility.h"
#include "timer.h"

#define private public
#include "macsecmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;

std::map<FILE *, int> wpaPipeStatuses;
size_t wpaProcessCount = 0;
int wpaSignal = 0;

extern "C" FILE *__wrap_popen(const char *command, const char *)
{
    ++wpaProcessCount;
    std::string cmd(command);
    const std::string redirect = " 2>&1";
    if (cmd.size() >= redirect.size() &&
        cmd.compare(cmd.size() - redirect.size(), redirect.size(), redirect) == 0)
    {
        cmd.erase(cmd.size() - redirect.size());
    }
    std::string output;
    const int status = callback(cmd, output);
    FILE *pipe = tmpfile();
    if (pipe == nullptr)
    {
        return nullptr;
    }
    fwrite(output.data(), 1, output.size(), pipe);
    rewind(pipe);
    wpaPipeStatuses[pipe] = wpaSignal != 0 ? wpaSignal : (status < 0 ? -1 : status << 8);
    return pipe;
}

extern "C" int __wrap_pclose(FILE *pipe)
{
    const int status = wpaPipeStatuses.at(pipe);
    wpaPipeStatuses.erase(pipe);
    fclose(pipe);
    return status;
}

namespace macsecmgr_ut
{

using namespace std;
using namespace swss;

const string PRIMARY_CKN =
    "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
const string NEW_PRIMARY_CKN =
    "112233445566778899aabbccddeeff00112233445566778899aabbccddeeff00";
const string FALLBACK_CKN =
    "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";
const string NEW_FALLBACK_CKN =
    "eeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100ff";
const string LATEST_FALLBACK_CKN =
    "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899";
const string LATEST_PRIMARY_CKN =
    "bbccddeeff00112233445566778899aabbccddeeff00112233445566778899aa";
const string ENCODED_CAK = "00" + string(64, '0');
const string NEW_ENCODED_CAK = "00" + string(64, '1');

string participant(
    int index,
    const string &ckn,
    bool primary,
    bool principal,
    uint32_t livePeers,
    uint32_t potentialPeers)
{
    return
        "participant_idx=" + to_string(index) + "\n"
        "ckn=" + ckn + "\n"
        "mi=102030405060708090a0b0c0\n"
        "mn=482\n"
        "active=Yes\n"
        "retain=No\n"
        "is_principal=" + string(principal ? "Yes\n" : "No\n") +
        "is_primary=" + string(primary ? "Yes\n" : "No\n") +
        "live_peers=" + to_string(livePeers) + "\n"
        "potential_peers=" + to_string(potentialPeers) + "\n"
        "is_key_server=Yes\n"
        "is_elected=Yes\n";
}

string statusOutput(
    const string &primaryCkn,
    const string &fallbackCkn = FALLBACK_CKN,
    bool fallbackPrincipal = false,
    bool authenticated = false,
    bool secured = true,
    uint32_t primaryLivePeers = 1,
    uint32_t primaryPotentialPeers = 0,
    uint32_t fallbackLivePeers = 1,
    uint32_t fallbackPotentialPeers = 0)
{
    string output =
        "PAE KaY status=Active\n"
        "Authenticated=" + string(authenticated ? "Yes\n" : "No\n") +
        "Secured=" + string(secured ? "Yes\n" : "No\n") +
        "Failed=No\n"
        "Actor Priority=16\n"
        "Key Server Priority=16\n"
        "Is Key Server=Yes\n"
        "Number of Keys Distributed=7\n"
        "Number of Keys Received=0\n"
        "MKA Hello Time=2000\n"
        "actor_sci=00:11:22:33:44:55@1\n"
        "key_server_sci=00:11:22:33:44:55@1\n";
    int index = 0;
    if (!primaryCkn.empty())
    {
        output += participant(
            index++,
            primaryCkn,
            true,
            !fallbackPrincipal,
            primaryLivePeers,
            primaryPotentialPeers);
    }
    if (!fallbackCkn.empty())
    {
        output += participant(
            index,
            fallbackCkn,
            false,
            fallbackPrincipal,
            fallbackLivePeers,
            fallbackPotentialPeers);
    }
    return output;
}

bool getField(
    Table &table,
    const string &key,
    const string &field,
    string &value)
{
    vector<FieldValueTuple> values;
    if (!table.get(key, values))
    {
        return false;
    }
    const auto fieldValue = fvsGetValue(values, field, true);
    if (!fieldValue)
    {
        return false;
    }
    value = fieldValue.get();
    return true;
}

struct CommandState
{
    struct PeerCounts
    {
        uint32_t primaryLive = 1;
        uint32_t primaryPotential = 0;
        uint32_t fallbackLive = 1;
        uint32_t fallbackPotential = 0;
    };

    string primaryCkn = PRIMARY_CKN;
    string fallbackCkn = FALLBACK_CKN;
    map<string, string> primaryCknByPort;
    map<string, string> fallbackCknByPort;
    map<string, string> networkPrimaryCknByPort;
    map<string, string> networkFallbackCknByPort;
    map<string, PeerCounts> peerCountsByPort;
    map<string, size_t> queryCountByPort;
    map<string, map<size_t, string>> queryRepliesByPort;
    map<string, set<size_t>> failedQueriesByPort;
    set<string> peersAppearOnSecondQueryPorts;
    set<string> primaryPeerLostOnSecondQueryPorts;
    set<string> replacementAppearsAfterRemovePorts;
    map<string, bool> lastRemoveWasPrimaryByPort;
    set<string> failFirstPostAddQueryPorts;
    set<string> omitFirstPostAddReplacementPorts;
    set<string> postAddQueryConsumedPorts;
    map<string, bool> lastAddWasPrimaryByPort;
    bool failQuery = false;
    string failQueryPort;
    set<string> failQueryPorts;
    bool failRemove = false;
    bool removeAppliedButFailed = false;
    bool failFallbackAdd = false;
    bool newParticipantsPeerless = false;
    bool addAppliedButFailed = false;
    bool authenticated = false;
    bool secured = true;
    int addCalls = 0;
    string failOperation;
    string failureOutput;
    int failureStatus = 0;
};

CommandState commandState;

string encodeTestCak(unsigned int salt, const string &key)
{
    const string salts = "dsfd;kfoA,.iyewrkldJKDHSUBsgvca69834ncxv9873254k;fg87";
    ostringstream output;
    output << dec << setw(2) << setfill('0') << salt;
    for (size_t index = 0; index < key.size(); ++index)
    {
        output << hex << setw(2) << setfill('0') <<
            (static_cast<unsigned int>(key[index]) ^
             static_cast<unsigned int>(salts[(salt + index) % salts.size()]));
    }
    return output.str();
}

void replaceField(MACsecMgr::TaskArgs &args, const string &field, const string &value)
{
    for (auto &entry : args)
    {
        if (fvField(entry) == field)
        {
            fvValue(entry) = value;
            return;
        }
    }
    args.emplace_back(field, value);
}

string commandValue(const string &cmd, const string &prefix)
{
    const auto start = cmd.find(prefix);
    if (start == string::npos)
    {
        return "";
    }
    const auto valueStart = start + prefix.size();
    const auto end = cmd.find_first_of("\"' ", valueStart);
    return cmd.substr(valueStart, end - valueStart);
}

string commandPort(const string &cmd)
{
    return commandValue(cmd, "IFNAME=");
}

int commandCallback(const string &cmd, string &output)
{
    mockCallArgs.push_back(cmd);
    if (!commandState.failOperation.empty() &&
        cmd.find(commandState.failOperation) != string::npos)
    {
        output = commandState.failureOutput;
        return commandState.failureStatus;
    }
    if (cmd.find("add_network") != string::npos)
    {
        output = "0\n";
        return 0;
    }
    const auto port = commandPort(cmd);
    auto &primaryCkn = commandState.primaryCknByPort.count(port) ?
        commandState.primaryCknByPort[port] :
        commandState.primaryCkn;
    auto &fallbackCkn = commandState.fallbackCknByPort.count(port) ?
        commandState.fallbackCknByPort[port] :
        commandState.fallbackCkn;
    auto peerCounts = commandState.peerCountsByPort[port];

    if (cmd.find("macsec_mka_list") != string::npos)
    {
        const auto queryCount = ++commandState.queryCountByPort[port];
        if (commandState.failedQueriesByPort[port].count(queryCount) != 0)
        {
            output = "FAIL\n";
            return 1;
        }
        const auto scripted = commandState.queryRepliesByPort[port].find(queryCount);
        if (scripted != commandState.queryRepliesByPort[port].end())
        {
            output = scripted->second;
            MKASessionStatus observed;
            string error;
            if (parseMKAStatus(output, observed, error))
            {
                primaryCkn.clear();
                fallbackCkn.clear();
                for (const auto &entry : observed.participants)
                {
                    (entry.isPrimary ? primaryCkn : fallbackCkn) = entry.ckn;
                }
            }
            return 0;
        }
        if (queryCount >= 2 &&
            commandState.peersAppearOnSecondQueryPorts.count(port) != 0)
        {
            peerCounts.primaryLive = 1;
        }
        if (queryCount >= 2 &&
            commandState.primaryPeerLostOnSecondQueryPorts.count(port) != 0)
        {
            peerCounts.primaryLive = 0;
        }
        if (commandState.failQuery ||
            (!commandState.failQueryPort.empty() &&
             port == commandState.failQueryPort) ||
            commandState.failQueryPorts.count(port) != 0)
        {
            output = "FAIL\n";
            return 1;
        }
        if (commandState.lastRemoveWasPrimaryByPort.count(port) != 0 &&
            commandState.replacementAppearsAfterRemovePorts.erase(port) != 0)
        {
            if (commandState.lastRemoveWasPrimaryByPort[port])
            {
                primaryCkn = NEW_PRIMARY_CKN;
            }
            else
            {
                fallbackCkn = NEW_FALLBACK_CKN;
            }
        }
        if (commandState.lastAddWasPrimaryByPort.count(port) != 0 &&
            commandState.postAddQueryConsumedPorts.count(port) == 0)
        {
            commandState.postAddQueryConsumedPorts.insert(port);
            if (commandState.failFirstPostAddQueryPorts.count(port) != 0)
            {
                output = "FAIL\n";
                return 1;
            }
            if (commandState.omitFirstPostAddReplacementPorts.count(port) != 0)
            {
                output = statusOutput(
                    commandState.lastAddWasPrimaryByPort[port] ? "" : primaryCkn,
                    commandState.lastAddWasPrimaryByPort[port] ? fallbackCkn : "",
                    false,
                    commandState.authenticated,
                    commandState.secured,
                    peerCounts.primaryLive,
                    peerCounts.primaryPotential,
                    peerCounts.fallbackLive,
                    peerCounts.fallbackPotential);
                return 0;
            }
        }
        output = statusOutput(
            primaryCkn,
            fallbackCkn,
            primaryCkn.empty(),
            commandState.authenticated,
            commandState.secured,
            peerCounts.primaryLive,
            peerCounts.primaryPotential,
            peerCounts.fallbackLive,
            peerCounts.fallbackPotential);
        return 0;
    }
    if (cmd.find("macsec_del_mka") != string::npos)
    {
        if (commandState.failRemove)
        {
            output = "FAIL\n";
            return 0;
        }
        if (!primaryCkn.empty() && cmd.find(primaryCkn) != string::npos)
        {
            primaryCkn.clear();
            commandState.lastRemoveWasPrimaryByPort[port] = true;
        }
        else
        {
            fallbackCkn.clear();
            commandState.lastRemoveWasPrimaryByPort[port] = false;
        }
        output = commandState.removeAppliedButFailed ? "FAIL\n" : "OK\n";
        return 0;
    }
    if (cmd.find("macsec_add_mka") != string::npos)
    {
        ++commandState.addCalls;
        if (commandState.failFallbackAdd && cmd.find("fallback=1") != string::npos)
        {
            output = "FAIL\n";
            return 0;
        }
        commandState.postAddQueryConsumedPorts.erase(port);
        if (cmd.find("fallback=1") != string::npos)
        {
            fallbackCkn = commandValue(cmd, "ckn=");
            commandState.lastAddWasPrimaryByPort[port] = false;
            if (commandState.newParticipantsPeerless)
            {
                commandState.peerCountsByPort[port].fallbackLive = 0;
                commandState.peerCountsByPort[port].fallbackPotential = 0;
            }
        }
        else
        {
            primaryCkn = commandValue(cmd, "ckn=");
            commandState.lastAddWasPrimaryByPort[port] = true;
            if (commandState.newParticipantsPeerless)
            {
                commandState.peerCountsByPort[port].primaryLive = 0;
                commandState.peerCountsByPort[port].primaryPotential = 0;
            }
        }
        output = commandState.addAppliedButFailed ? "FAIL\n" : "OK\n";
        return 0;
    }
    if (cmd.find("set_network") != string::npos &&
        cmd.find("mka_ckn") != string::npos)
    {
        const auto end = cmd.find_last_not_of(" \"");
        const auto start = cmd.find_last_of(" \"", end);
        const auto ckn = cmd.substr(start + 1, end - start);
        if (cmd.find("mka_ckn_fallback") != string::npos)
        {
            commandState.networkFallbackCknByPort[port] = ckn;
        }
        else
        {
            commandState.networkPrimaryCknByPort[port] = ckn;
        }
    }
    output = "OK\n";
    return 0;
}

struct MACsecMgrTest : public ::testing::Test
{
    shared_ptr<DBConnector> configDb;
    shared_ptr<DBConnector> stateDb;
    unique_ptr<MACsecMgr> manager;

    void SetUp() override
    {
        testing_db::reset();
        mockCallArgs.clear();
        commandState = CommandState();
        wpaProcessCount = 0;
        wpaSignal = 0;
        callback = commandCallback;
        configDb = make_shared<DBConnector>("CONFIG_DB", 0);
        stateDb = make_shared<DBConnector>("STATE_DB", 0);
        manager.reset(new MACsecMgr(
            configDb.get(),
            stateDb.get(),
            {CFG_MACSEC_PROFILE_TABLE_NAME, CFG_PORT_TABLE_NAME}));
    }

    void TearDown() override
    {
        manager->m_macsec_ports.clear();
        manager.reset();
        callback = nullptr;
    }

    MACsecMgr::MACsecProfile profile(const string &primaryCkn = PRIMARY_CKN)
    {
        MACsecMgr::MACsecProfile value;
        value.primary_cak = primaryCkn == PRIMARY_CKN ? ENCODED_CAK : NEW_ENCODED_CAK;
        value.primary_ckn = primaryCkn;
        value.fallback_cak = ENCODED_CAK;
        value.fallback_ckn = FALLBACK_CKN;
        return value;
    }

    MACsecMgr::TaskArgs profileArgs(
        const string &primaryCkn = PRIMARY_CKN,
        const string &fallbackCkn = FALLBACK_CKN)
    {
        return {
            {"cipher_suite", "GCM-AES-128"},
            {"primary_cak", primaryCkn == PRIMARY_CKN ? ENCODED_CAK : NEW_ENCODED_CAK},
            {"primary_ckn", primaryCkn},
            {"fallback_cak", fallbackCkn == FALLBACK_CKN ? ENCODED_CAK : NEW_ENCODED_CAK},
            {"fallback_ckn", fallbackCkn},
        };
    }

    MACsecMgr::MACsecProfile primaryOnlyProfile(const string &primaryCkn = PRIMARY_CKN)
    {
        auto value = profile(primaryCkn);
        value.fallback_cak.clear();
        value.fallback_ckn.clear();
        return value;
    }

    MACsecMgr::TaskArgs primaryOnlyProfileArgs(const string &primaryCkn = PRIMARY_CKN)
    {
        return {
            {"cipher_suite", "GCM-AES-128"},
            {"primary_cak", primaryCkn == PRIMARY_CKN ? ENCODED_CAK : NEW_ENCODED_CAK},
            {"primary_ckn", primaryCkn},
        };
    }

    MACsecMgr::MKASession &session(const string &port)
    {
        auto &value = manager->m_macsec_ports[port];
        value.profile_name = "profile";
        value.sock = "/var/run/" + port;
        value.network_id = "0";
        value.applied_profile = profile();
        manager->m_profiles["profile"] = value.applied_profile;
        commandState.primaryCknByPort[port] = PRIMARY_CKN;
        commandState.fallbackCknByPort[port] = FALLBACK_CKN;
        commandState.networkPrimaryCknByPort[port] = PRIMARY_CKN;
        commandState.networkFallbackCknByPort[port] = FALLBACK_CKN;
        return value;
    }

    MACsecMgr::MKASession &session()
    {
        return session("Ethernet0");
    }

    Executor *executor(const string &name)
    {
        for (const auto selectable : manager->getSelectables())
        {
            auto item = dynamic_cast<Executor *>(selectable);
            if (item != nullptr && item->getName() == name)
            {
                return item;
            }
        }
        return nullptr;
    }
};

TEST(MKAStatusParser, AcceptsFinalFrozenInterfaceWithoutParticipantField)
{
    MKASessionStatus status;
    string error;
    string uppercaseCkn = PRIMARY_CKN;
    transform(
        uppercaseCkn.begin(),
        uppercaseCkn.end(),
        uppercaseCkn.begin(),
        [](unsigned char c) { return static_cast<char>(toupper(c)); });
    ASSERT_TRUE(parseMKAStatus(statusOutput(uppercaseCkn), status, error)) << error;
    EXPECT_EQ("active", status.kayStatus);
    EXPECT_FALSE(status.authenticated);
    EXPECT_TRUE(status.secured);
    EXPECT_EQ("0011223344550001", status.actorSci);
    ASSERT_EQ(2u, status.participants.size());
    EXPECT_EQ(PRIMARY_CKN, status.participants[0].ckn);
    EXPECT_TRUE(status.participants[0].isPrimary);
    EXPECT_FALSE(status.participants[1].isPrimary);
}

TEST(MKAStatusParser, RejectsPartialParticipant)
{
    MKASessionStatus status;
    string error;
    auto output = statusOutput(PRIMARY_CKN);
    const auto field = output.find("live_peers=1\n");
    output.erase(field, string("live_peers=1\n").size());
    EXPECT_FALSE(parseMKAStatus(output, status, error));
    EXPECT_NE(string::npos, error.find("missing field"));
}

TEST_F(MACsecMgrTest, LegacySaltProfilesLoadFromConfigOnManagerStartup)
{
    Table configProfiles(configDb.get(), CFG_MACSEC_PROFILE_TABLE_NAME);
    for (const auto salt : {0u, 52u, 53u, 99u})
    {
        for (const auto keyLength : {32u, 64u})
        {
            auto args = primaryOnlyProfileArgs();
            const auto plainKey = string(keyLength, 'a');
            const auto encodedKey = encodeTestCak(salt, plainKey);
            replaceField(args, "primary_cak", encodedKey);
            replaceField(args, "cipher_suite", keyLength == 32 ? "GCM-AES-128" : "GCM-AES-256");
            const auto name = "legacy-" + to_string(salt) + "-" + to_string(keyLength);
            configProfiles.set(name, args);
        }
    }

    manager.reset(new MACsecMgr(configDb.get(), stateDb.get(),
        {CFG_MACSEC_PROFILE_TABLE_NAME, CFG_PORT_TABLE_NAME}));
    manager->addExistingData(&configProfiles);
    manager->doTask();
    EXPECT_EQ(8u, manager->m_profiles.size());
    for (const auto &entry : manager->m_profiles)
    {
        MACsecMgr::MKASession runtime;
        runtime.sock = "/var/run/Ethernet8";
        ASSERT_TRUE(manager->configureMACsec("Ethernet8", runtime, entry.second));
    }

    const auto decodedWrites = count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("mka_cak") != string::npos &&
            (cmd.find(string(32, 'a')) != string::npos || cmd.find(string(64, 'a')) != string::npos);
    });
    EXPECT_EQ(8, decodedWrites);
}

TEST_F(MACsecMgrTest, LegacySaltStillRequiresValidFormatAndCipherLength)
{
    const auto valid = encodeTestCak(99, string(32, 'a'));
    for (const auto &invalid : {
             string("x9") + valid.substr(2),
             string("-1") + valid.substr(2),
             string("9 ") + valid.substr(2),
             valid.substr(0, valid.size() - 1),
             valid + "00",
             valid.substr(0, 4) + "gg" + valid.substr(6)})
    {
        auto args = primaryOnlyProfileArgs();
        replaceField(args, "primary_cak", invalid);
        EXPECT_EQ(task_invalid_entry, manager->loadProfile("bad", args));
        EXPECT_TRUE(manager->m_profiles.empty());
    }
    auto args = primaryOnlyProfileArgs();
    replaceField(args, "primary_cak", valid);
    replaceField(args, "cipher_suite", "GCM-AES-256");
    EXPECT_EQ(task_invalid_entry, manager->loadProfile("bad", args));
    EXPECT_TRUE(manager->m_profiles.empty());
}

TEST_F(MACsecMgrTest, ConfigAndWpaUseSameCknNormalization)
{
    const vector<string> valid = {"AB", "aBcD", string(128, 'A')};
    const vector<string> invalid = {"", "A", "0g", "ab cd", " 00", string(130, 'a')};
    for (const auto &input : valid)
    {
        string normalized;
        ASSERT_TRUE(normalizeCkn(input, normalized));
        auto args = primaryOnlyProfileArgs(input);
        MACsecMgr::MACsecProfile configured;
        ASSERT_TRUE(configured.update(args));
        MKASessionStatus status;
        string error;
        ASSERT_TRUE(parseMKAStatus(statusOutput(input, ""), status, error)) << error;
        ASSERT_EQ(1u, status.participants.size());
        EXPECT_EQ(normalized, configured.primary_ckn);
        EXPECT_EQ(normalized, status.participants[0].ckn);
    }
    for (const auto &input : invalid)
    {
        string normalized;
        EXPECT_FALSE(normalizeCkn(input, normalized));
        MACsecMgr::MACsecProfile configured;
        EXPECT_FALSE(configured.update(primaryOnlyProfileArgs(input)));
        MKASessionStatus status;
        string error;
        EXPECT_FALSE(parseMKAStatus(
            statusOutput("", "") + participant(0, input, true, true, 0, 0), status, error));
    }

    auto duplicate = PRIMARY_CKN;
    transform(duplicate.begin(), duplicate.end(), duplicate.begin(),
        [](unsigned char c) { return static_cast<char>(toupper(c)); });
    MACsecMgr::MACsecProfile configured;
    EXPECT_FALSE(configured.update(profileArgs(PRIMARY_CKN, duplicate)));
    MKASessionStatus status;
    string error;
    EXPECT_FALSE(parseMKAStatus(statusOutput(PRIMARY_CKN, duplicate), status, error));
    EXPECT_EQ("status contains duplicate participant CKN", error);
}

TEST_F(MACsecMgrTest, DebugAndErrorDiagnosticsNeverEchoKeys)
{
    const string plainKey = "0123456789abcdef0123456789abcdef";
    const string encodedKey = encodeTestCak(99, plainKey);
    auto args = profileArgs();
    replaceField(args, "primary_cak", encodedKey);
    replaceField(args, "fallback_cak", encodedKey);
    MACsecMgr::MACsecProfile configured;
    ASSERT_TRUE(configured.update(args));
    MACsecMgr::MKASession runtime;
    runtime.sock = "/var/run/Ethernet0";
    runtime.network_id = "0";
    runtime.profile_name = "profile";
    runtime.applied_profile = configured;
    manager->m_profiles["profile"] = configured;

    const auto priorPriority = Logger::getMinPrio();
    Logger::swssOutputNotify("macsecmgrd", "STDOUT");
    Logger::setMinPrio(Logger::SWSS_DEBUG);
    testing::internal::CaptureStdout();

    EXPECT_TRUE(manager->configureMACsec("Ethernet0", runtime, configured));
    EXPECT_TRUE(manager->collectMKAStatus("Ethernet0", runtime));
    for (const auto &operation : {"mka_cak", "mka_cak_fallback", "macsec_add_mka"})
    {
        for (const int exitCode : {0, 1})
        {
            commandState.failOperation = operation;
            commandState.failureOutput = "FAIL " + plainKey + " " + encodedKey;
            commandState.failureStatus = exitCode;
            if (string(operation) != "macsec_add_mka")
            {
                EXPECT_FALSE(manager->configureMACsec("Ethernet0", runtime, configured));
                EXPECT_FALSE(manager->updateNetworkParticipant(
                    "Ethernet0", runtime, configured, string(operation) == "mka_cak"));
            }
            else
            {
                EXPECT_FALSE(manager->addParticipant("Ethernet0", runtime, configured, true));
                EXPECT_FALSE(manager->addParticipant("Ethernet0", runtime, configured, false));
            }
        }
    }
    commandState.failOperation = "add_network";
    commandState.failureOutput = "unexpected response: " + plainKey + " " + encodedKey;
    commandState.failureStatus = 0;
    EXPECT_FALSE(manager->configureMACsec("Ethernet0", runtime, configured));
    commandState.failOperation = "interface_remove";
    commandState.failureOutput = "FAIL " + plainKey + " " + encodedKey;
    EXPECT_TRUE(manager->unconfigureMACsec("Ethernet0", runtime));
    commandState.failureStatus = 1;
    EXPECT_FALSE(manager->unconfigureMACsec("Ethernet0", runtime));
    commandState.failOperation = "macsec_mka_list";
    commandState.failureOutput = "Authenticated=" + plainKey + "\ncak=" + encodedKey;
    commandState.failureStatus = 0;
    EXPECT_FALSE(manager->collectMKAStatus("Ethernet0", runtime));

    auto invalid = args;
    replaceField(invalid, "policy", encodedKey);
    try
    {
        configured.update(invalid);
        ADD_FAILURE() << "Invalid policy accepted";
    }
    catch (const invalid_argument &error)
    {
        EXPECT_EQ(string::npos, string(error.what()).find(encodedKey));
    }
    EXPECT_EQ(task_failed, manager->loadProfile("bad-policy", invalid));
    replaceField(invalid, "policy", "security");
    replaceField(invalid, "cipher_suite", plainKey);
    EXPECT_EQ(task_failed, manager->loadProfile("bad-cipher", invalid));
    replaceField(invalid, "cipher_suite", "GCM-AES-128");
    replaceField(invalid, "priority", encodedKey);
    EXPECT_EQ(task_success, manager->loadProfile("bad-priority", invalid));

    const string logs = testing::internal::GetCapturedStdout();
    Logger::setMinPrio(priorPriority);
    Logger::swssOutputNotify("macsecmgrd", "SYSLOG");
    EXPECT_EQ(string::npos, logs.find(encodedKey));
    EXPECT_EQ(string::npos, logs.find(plainKey));
    EXPECT_NE(string::npos, logs.find("WPA control mka_cak"));
    EXPECT_NE(string::npos, logs.find("command failed"));
    EXPECT_NE(string::npos, logs.find("invalid response"));
    EXPECT_NE(string::npos, logs.find("Cannot convert MACsec field 'priority'"));
    EXPECT_GT(wpaProcessCount, 0u);
    EXPECT_TRUE(wpaPipeStatuses.empty());
    for (const auto &command : mockCallArgs)
    {
        if (command.find("macsec_mka_list") != string::npos)
        {
            EXPECT_EQ(0u, command.find("/usr/bin/timeout --signal=KILL 2s "));
        }
        else
        {
            EXPECT_EQ(string::npos, command.find("/usr/bin/timeout"));
        }
    }

    for (auto *table : {&manager->m_stateMkaSessionTable, &manager->m_stateMkaParticipantTable})
    {
        vector<string> keys;
        table->getKeys(keys);
        for (const auto &key : keys)
        {
            vector<FieldValueTuple> values;
            ASSERT_TRUE(table->get(key, values));
            for (const auto &fieldValue : values)
            {
                EXPECT_EQ(string::npos, fvValue(fieldValue).find(encodedKey));
                EXPECT_EQ(string::npos, fvValue(fieldValue).find(plainKey));
            }
        }
    }
    const auto consumer = dynamic_cast<Consumer *>(executor(CFG_MACSEC_PROFILE_TABLE_NAME));
    ASSERT_NE(nullptr, consumer);
    EXPECT_FALSE(consumer->isRecordable());
}

TEST_F(MACsecMgrTest, MissingProfileConsumerFailsStartupWithLoggedError)
{
    const auto priorPriority = Logger::getMinPrio();
    Logger::swssOutputNotify("macsecmgrd", "STDOUT");
    Logger::setMinPrio(Logger::SWSS_ERROR);
    testing::internal::CaptureStdout();

    EXPECT_THROW(
        MACsecMgr(configDb.get(), stateDb.get(), {CFG_PORT_TABLE_NAME}),
        runtime_error);

    const string logs = testing::internal::GetCapturedStdout();
    Logger::setMinPrio(priorPriority);
    Logger::swssOutputNotify("macsecmgrd", "SYSLOG");
    EXPECT_NE(string::npos, logs.find("MACsec profile consumer is missing or has the wrong type"));
    EXPECT_EQ(0u, wpaProcessCount);
}

TEST_F(MACsecMgrTest, WpaDiagnosticsUseDecodedExitAndSignalStatuses)
{
    MACsecMgr::MKASession runtime;
    runtime.sock = "/var/run/Ethernet0";
    const auto configured = profile();
    commandState.failOperation = "mka_cak";
    commandState.failureOutput = "FAIL";
    const auto priorPriority = Logger::getMinPrio();
    Logger::swssOutputNotify("macsecmgrd", "STDOUT");
    Logger::setMinPrio(Logger::SWSS_DEBUG);
    testing::internal::CaptureStdout();

    commandState.failureStatus = 7;
    EXPECT_FALSE(manager->configureMACsec("Ethernet0", runtime, configured));
    commandState.failureStatus = 124;
    EXPECT_FALSE(manager->configureMACsec("Ethernet0", runtime, configured));
    commandState.failureStatus = 0;
    wpaSignal = SIGTERM;
    EXPECT_FALSE(manager->configureMACsec("Ethernet0", runtime, configured));
    wpaSignal = 0;

    const string logs = testing::internal::GetCapturedStdout();
    Logger::setMinPrio(priorPriority);
    Logger::swssOutputNotify("macsecmgrd", "SYSLOG");
    EXPECT_NE(string::npos, logs.find("command failed (exit 7)"));
    EXPECT_EQ(string::npos, logs.find("1792"));
    EXPECT_NE(string::npos, logs.find("command timed out"));
    EXPECT_NE(string::npos, logs.find("command terminated by signal " + to_string(SIGTERM)));
    EXPECT_TRUE(wpaPipeStatuses.empty());
}

TEST_F(MACsecMgrTest, PreDeleteQueryFailureDefersOnlyAffectedPortUntilTimerRecovery)
{
    auto &blocked = session("Ethernet0");
    auto &ready = session("Ethernet4");
    commandState.failQueryPorts.insert("Ethernet0");
    auto consumer = dynamic_cast<Consumer *>(executor(CFG_MACSEC_PROFILE_TABLE_NAME));
    ASSERT_NE(nullptr, consumer);
    consumer->addToSync(KeyOpFieldsValuesTuple("profile", SET_COMMAND, profileArgs(NEW_PRIMARY_CKN)));
    manager->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(NEW_PRIMARY_CKN, manager->m_profiles["profile"].primary_ckn);
    EXPECT_EQ(PRIMARY_CKN, blocked.applied_profile.primary_ckn);
    EXPECT_EQ(PRIMARY_CKN, commandState.primaryCknByPort["Ethernet0"]);
    EXPECT_TRUE(blocked.pending_old_ckn.empty());
    EXPECT_EQ(NEW_PRIMARY_CKN, ready.applied_profile.primary_ckn);

    const auto forks = wpaProcessCount;
    const auto blockedWrites = testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet0");
    const auto readyWrites = testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet4");
    for (int timeout = 0; timeout < 40; ++timeout)
    {
        manager->doTask();
    }
    EXPECT_EQ(forks, wpaProcessCount);
    EXPECT_EQ(blockedWrites, testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet0"));
    EXPECT_EQ(readyWrites, testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet4"));

    itimerspec timerState = {};
    ASSERT_EQ(0, timerfd_gettime(manager->m_mkaStatusTimer->getFd(), &timerState));
    EXPECT_EQ(20, timerState.it_interval.tv_sec);
    EXPECT_EQ(0, timerState.it_interval.tv_nsec);
    auto timer = dynamic_cast<ExecutableTimer *>(executor("MACSEC_MKA_STATUS_TIMER"));
    ASSERT_NE(nullptr, timer);
    timer->execute();
    EXPECT_EQ(forks + 3, wpaProcessCount);
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_FALSE(sessionTable.get("Ethernet0", values));
    EXPECT_GT(testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet4"), readyWrites);

    commandState.failQueryPorts.clear();
    timer->execute();
    EXPECT_EQ(NEW_PRIMARY_CKN, blocked.applied_profile.primary_ckn);
    EXPECT_EQ(2, commandState.addCalls);
    EXPECT_TRUE(sessionTable.get("Ethernet0", values));
    EXPECT_TRUE(consumer->m_toSync.empty());
}

TEST_F(MACsecMgrTest, PeerlessAlternateDoesNotDeferCommandsOrRequireTimer)
{
    auto &runtime = session();
    commandState.peerCountsByPort["Ethernet0"].fallbackLive = 0;
    auto consumer = dynamic_cast<Consumer *>(executor(CFG_MACSEC_PROFILE_TABLE_NAME));
    ASSERT_NE(nullptr, consumer);
    consumer->addToSync(KeyOpFieldsValuesTuple("profile", SET_COMMAND, profileArgs(NEW_PRIMARY_CKN)));
    manager->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(1, commandState.addCalls);
    EXPECT_EQ(NEW_PRIMARY_CKN, runtime.applied_profile.primary_ckn);
    const auto writes = testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet0");
    const auto forks = wpaProcessCount;
    for (int timeout = 0; timeout < 20; ++timeout)
    {
        manager->doTask();
    }
    EXPECT_EQ(forks, wpaProcessCount);
    EXPECT_EQ(writes, testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet0"));

    auto timer = dynamic_cast<ExecutableTimer *>(executor("MACSEC_MKA_STATUS_TIMER"));
    ASSERT_NE(nullptr, timer);
    timer->execute();
    EXPECT_EQ(NEW_PRIMARY_CKN, runtime.applied_profile.primary_ckn);
    EXPECT_EQ(1, commandState.addCalls);

    consumer->addToSync(KeyOpFieldsValuesTuple(
        "profile", SET_COMMAND, profileArgs(NEW_PRIMARY_CKN, NEW_FALLBACK_CKN)));
    manager->doTask();
    EXPECT_EQ(NEW_FALLBACK_CKN, runtime.applied_profile.fallback_ckn);
    EXPECT_EQ(2, commandState.addCalls);
}

TEST_F(MACsecMgrTest, UnsupportedRoleSwapIsObservedOnTimerWithoutRetryChurn)
{
    auto &runtime = session();
    auto consumer = dynamic_cast<Consumer *>(executor(CFG_MACSEC_PROFILE_TABLE_NAME));
    ASSERT_NE(nullptr, consumer);
    consumer->addToSync(KeyOpFieldsValuesTuple(
        "profile", SET_COMMAND, profileArgs(FALLBACK_CKN, PRIMARY_CKN)));
    manager->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(PRIMARY_CKN, runtime.applied_profile.primary_ckn);
    EXPECT_EQ(FALLBACK_CKN, runtime.applied_profile.fallback_ckn);
    EXPECT_EQ(0, commandState.addCalls);
    const auto forks = wpaProcessCount;
    const auto writes = testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet0");
    for (int timeout = 0; timeout < 20; ++timeout)
    {
        manager->doTask();
    }
    EXPECT_EQ(forks, wpaProcessCount);
    EXPECT_EQ(writes, testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet0"));

    auto timer = dynamic_cast<ExecutableTimer *>(executor("MACSEC_MKA_STATUS_TIMER"));
    ASSERT_NE(nullptr, timer);
    timer->execute();
    EXPECT_EQ(forks + 1, wpaProcessCount);
    EXPECT_GT(testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet0"), writes);
    EXPECT_EQ(0, commandState.addCalls);
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string value;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", value));
    EXPECT_EQ("degraded", value);
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "query_status", value));
    EXPECT_EQ("ok", value);
}

TEST_F(MACsecMgrTest, FailedFallbackAddRecoversOnTimerWithoutConfigReplay)
{
    auto &runtime = session();
    commandState.failFallbackAdd = true;
    auto consumer = dynamic_cast<Consumer *>(executor(CFG_MACSEC_PROFILE_TABLE_NAME));
    ASSERT_NE(nullptr, consumer);
    consumer->addToSync(KeyOpFieldsValuesTuple(
        "profile", SET_COMMAND, profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    manager->doTask();
    ASSERT_TRUE(consumer->m_toSync.empty());
    ASSERT_EQ(FALLBACK_CKN, runtime.pending_old_ckn);
    ASSERT_TRUE(runtime.applied_profile.fallback_ckn.empty());
    const auto forks = wpaProcessCount;
    const auto writes = testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet0");
    for (int timeout = 0; timeout < 20; ++timeout)
    {
        manager->doTask();
    }
    EXPECT_EQ(forks, wpaProcessCount);
    EXPECT_EQ(writes, testing_db::writeCount(
        STATE_DB, STATE_MACSEC_MKA_SESSION_TABLE_NAME, "Ethernet0"));

    commandState.failFallbackAdd = false;
    auto timer = dynamic_cast<ExecutableTimer *>(executor("MACSEC_MKA_STATUS_TIMER"));
    ASSERT_NE(nullptr, timer);
    timer->execute();
    EXPECT_EQ(NEW_FALLBACK_CKN, runtime.applied_profile.fallback_ckn);
    EXPECT_TRUE(runtime.pending_old_ckn.empty());
    EXPECT_EQ(2, commandState.addCalls);
    EXPECT_EQ(1, count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &command) {
        return command.find("macsec_del_mka") != string::npos;
    }));
}

TEST_F(MACsecMgrTest, RejectedNetworkUpdateRetriesInPlaceWithoutRemovingAgain)
{
    auto &runtime = session();
    commandState.failOperation = "mka_ckn_fallback";
    commandState.failureOutput = "FAIL\n";

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(FALLBACK_CKN, runtime.pending_old_ckn);
    EXPECT_EQ(0, commandState.addCalls);
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string status;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", status));
    EXPECT_EQ("degraded", status);

    commandState.failOperation.clear();
    manager->sweepMKAStatus();
    EXPECT_EQ(NEW_FALLBACK_CKN, runtime.applied_profile.fallback_ckn);
    EXPECT_EQ(1, commandState.addCalls);
    EXPECT_EQ(1, count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", status));
    EXPECT_EQ("in-sync", status);
}

TEST_F(MACsecMgrTest, QueryFailureDeletesOnlyFailedPortAndRecoveryRepublishes)
{
    auto &mkaSession = session();
    auto &otherSession = session("Ethernet4");
    ASSERT_TRUE(manager->collectMKAStatus("Ethernet0", mkaSession));
    ASSERT_TRUE(manager->collectMKAStatus("Ethernet4", otherSession));

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    Table participantTable(stateDb.get(), STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME);
    commandState.failQueryPort = "Ethernet0";
    EXPECT_FALSE(manager->collectMKAStatus("Ethernet0", mkaSession));

    vector<FieldValueTuple> values;
    EXPECT_FALSE(sessionTable.get("Ethernet0", values));
    EXPECT_FALSE(participantTable.get("Ethernet0|" + PRIMARY_CKN, values));
    EXPECT_FALSE(participantTable.get("Ethernet0|" + FALLBACK_CKN, values));
    EXPECT_TRUE(sessionTable.get("Ethernet4", values));
    EXPECT_TRUE(participantTable.get("Ethernet4|" + PRIMARY_CKN, values));
    EXPECT_FALSE(mkaSession.participants_verified);
    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);

    commandState.failQueryPort.clear();
    EXPECT_TRUE(manager->collectMKAStatus("Ethernet0", mkaSession));
    string value;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "query_status", value));
    EXPECT_EQ("ok", value);
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", value));
    EXPECT_EQ("in-sync", value);
    EXPECT_TRUE(participantTable.get("Ethernet0|" + PRIMARY_CKN, values));
    EXPECT_TRUE(participantTable.get("Ethernet0|" + FALLBACK_CKN, values));
}

TEST_F(MACsecMgrTest, IncompleteStatusDeletesRowsWithoutLosingPendingReplacement)
{
    auto &mkaSession = session();
    auto &otherSession = session("Ethernet4");
    otherSession.profile_name = "other";
    manager->m_profiles["other"] = profile();
    ASSERT_TRUE(manager->collectMKAStatus("Ethernet0", mkaSession));
    ASSERT_TRUE(manager->collectMKAStatus("Ethernet4", otherSession));

    commandState.failFallbackAdd = true;
    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    ASSERT_EQ(FALLBACK_CKN, mkaSession.pending_old_ckn);
    ASSERT_EQ(NEW_FALLBACK_CKN, mkaSession.pending_new_ckn);
    ASSERT_EQ(1, commandState.addCalls);

    commandState.failOperation = "macsec_mka_list";
    commandState.failureOutput = statusOutput(PRIMARY_CKN) +
        "participant_idx=2\nckn=" + NEW_PRIMARY_CKN + "\n";
    commandState.failureStatus = 0;
    EXPECT_FALSE(manager->collectMKAStatus("Ethernet0", mkaSession));

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    Table participantTable(stateDb.get(), STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_FALSE(sessionTable.get("Ethernet0", values));
    EXPECT_FALSE(participantTable.get("Ethernet0|" + PRIMARY_CKN, values));
    EXPECT_FALSE(participantTable.get("Ethernet0|" + FALLBACK_CKN, values));
    EXPECT_TRUE(sessionTable.get("Ethernet4", values));
    EXPECT_TRUE(participantTable.get("Ethernet4|" + PRIMARY_CKN, values));
    EXPECT_EQ(FALLBACK_CKN, mkaSession.pending_old_ckn);
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.pending_new_ckn);
    EXPECT_EQ(1, commandState.addCalls);

    commandState.failOperation.clear();
    commandState.failFallbackAdd = false;
    manager->sweepMKAStatus();
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(2, commandState.addCalls);
    EXPECT_TRUE(sessionTable.get("Ethernet0", values));
    EXPECT_TRUE(participantTable.get("Ethernet0|" + NEW_FALLBACK_CKN, values));
    EXPECT_TRUE(sessionTable.get("Ethernet4", values));
}

TEST_F(MACsecMgrTest, UnexpectedParticipantIsPublishedButNotReportedInSync)
{
    auto &mkaSession = session();
    commandState.primaryCknByPort["Ethernet0"] = NEW_PRIMARY_CKN;

    EXPECT_TRUE(manager->collectMKAStatus("Ethernet0", mkaSession));
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    Table participantTable(stateDb.get(), STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME);
    string value;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "query_status", value));
    EXPECT_EQ("ok", value);
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", value));
    EXPECT_EQ("degraded", value);
    vector<FieldValueTuple> fields;
    EXPECT_TRUE(participantTable.get("Ethernet0|" + NEW_PRIMARY_CKN, fields));

    commandState.primaryCknByPort["Ethernet0"] = PRIMARY_CKN;
    EXPECT_TRUE(manager->collectMKAStatus("Ethernet0", mkaSession));
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", value));
    EXPECT_EQ("in-sync", value);
}

TEST_F(MACsecMgrTest, TimerRestoresParticipantMissingAfterPreviouslyVerifiedState)
{
    auto &mkaSession = session();
    ASSERT_TRUE(manager->collectMKAStatus("Ethernet0", mkaSession));
    commandState.primaryCknByPort["Ethernet0"].clear();

    manager->sweepMKAStatus();

    EXPECT_EQ(PRIMARY_CKN, commandState.primaryCknByPort["Ethernet0"]);
    EXPECT_EQ(1, commandState.addCalls);
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos ||
               cmd.find("interface_remove") != string::npos;
    }));
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string status;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", status));
    EXPECT_EQ("in-sync", status);
}

TEST_F(MACsecMgrTest, TimerReplacesOutOfBandCknDriftWithoutRestart)
{
    auto &mkaSession = session();
    ASSERT_TRUE(manager->collectMKAStatus("Ethernet0", mkaSession));
    commandState.primaryCknByPort["Ethernet0"] = NEW_PRIMARY_CKN;

    manager->sweepMKAStatus();

    EXPECT_EQ(PRIMARY_CKN, commandState.primaryCknByPort["Ethernet0"]);
    EXPECT_EQ(1, commandState.addCalls);
    EXPECT_EQ(1, count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos &&
               cmd.find(NEW_PRIMARY_CKN) != string::npos;
    }));
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("interface_remove") != string::npos ||
               cmd.find("interface_add") != string::npos;
    }));
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string status;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", status));
    EXPECT_EQ("in-sync", status);
}

TEST_F(MACsecMgrTest, TimerRetriesMissingParticipantAddWithoutRepeatedRemoval)
{
    auto &mkaSession = session();
    ASSERT_TRUE(manager->collectMKAStatus("Ethernet0", mkaSession));
    commandState.fallbackCknByPort["Ethernet0"].clear();
    commandState.failFallbackAdd = true;

    manager->sweepMKAStatus();
    EXPECT_EQ(1, commandState.addCalls);
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string status;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", status));
    EXPECT_EQ("degraded", status);

    commandState.failFallbackAdd = false;
    manager->sweepMKAStatus();
    EXPECT_EQ(2, commandState.addCalls);
    EXPECT_EQ(FALLBACK_CKN, commandState.fallbackCknByPort["Ethernet0"]);
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", status));
    EXPECT_EQ("in-sync", status);
}

TEST_F(MACsecMgrTest, TimerObservesAmbiguousRestorationBeforeAddingAgain)
{
    auto &mkaSession = session();
    ASSERT_TRUE(manager->collectMKAStatus("Ethernet0", mkaSession));
    commandState.fallbackCknByPort["Ethernet0"].clear();
    commandState.addAppliedButFailed = true;

    manager->sweepMKAStatus();
    EXPECT_EQ(1, commandState.addCalls);
    commandState.addAppliedButFailed = false;
    manager->sweepMKAStatus();
    EXPECT_EQ(1, commandState.addCalls);
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string status;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", status));
    EXPECT_EQ("in-sync", status);
}

TEST_F(MACsecMgrTest, ExplicitDisableDeletesOperationalRows)
{
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    Table participantTable(stateDb.get(), STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME);
    sessionTable.set("Ethernet0", {{"query_status", "ok"}});
    participantTable.set("Ethernet0|" + PRIMARY_CKN, {{"active", "true"}});

    EXPECT_EQ(task_success, manager->disableMACsec("Ethernet0", {}));

    vector<FieldValueTuple> values;
    EXPECT_FALSE(sessionTable.get("Ethernet0", values));
    EXPECT_FALSE(participantTable.get("Ethernet0|" + PRIMARY_CKN, values));
}

TEST_F(MACsecMgrTest, TimerSweepQueriesAllPortsAndIsolatesFailure)
{
    const vector<string> ports = {
        "Ethernet0",
        "Ethernet100",
        "Ethernet104",
        "Ethernet108",
        "Ethernet112",
        "Ethernet136",
    };
    manager->m_profiles["profile"] = profile();
    for (const auto &port : ports)
    {
        auto &mkaSession = manager->m_macsec_ports[port];
        mkaSession.profile_name = "profile";
        mkaSession.sock = "/var/run/" + port;
        mkaSession.applied_profile = manager->m_profiles["profile"];
        ASSERT_TRUE(manager->collectMKAStatus(port, mkaSession));
    }

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    for (const auto &port : ports)
    {
        sessionTable.hset(port, "last_updated", "2000-01-01T00:00:00Z");
    }

    commandState.failQueryPort = "Ethernet104";
    mockCallArgs.clear();
    ASSERT_NE(nullptr, manager->m_mkaStatusTimer);
    manager->doTask(*manager->m_mkaStatusTimer);

    vector<string> queriedPorts;
    for (const auto &command : mockCallArgs)
    {
        if (command.find("macsec_mka_list") == string::npos)
        {
            continue;
        }
        EXPECT_NE(string::npos, command.find("/usr/bin/timeout --signal=KILL 2s"));
        for (const auto &port : ports)
        {
            if (command.find("IFNAME=" + port) != string::npos)
            {
                queriedPorts.push_back(port);
                break;
            }
        }
    }
    EXPECT_EQ(ports, queriedPorts);

    string value;
    vector<FieldValueTuple> values;
    EXPECT_FALSE(sessionTable.get("Ethernet104", values));
    Table participantTable(stateDb.get(), STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME);
    EXPECT_FALSE(participantTable.get("Ethernet104|" + PRIMARY_CKN, values));
    EXPECT_TRUE(sessionTable.get("Ethernet100", values));
    EXPECT_TRUE(participantTable.get("Ethernet100|" + PRIMARY_CKN, values));

    ASSERT_TRUE(getField(sessionTable, "Ethernet136", "query_status", value));
    EXPECT_EQ("ok", value);
    ASSERT_TRUE(getField(sessionTable, "Ethernet136", "last_updated", value));
    EXPECT_NE("2000-01-01T00:00:00Z", value);
}

TEST(MACsecMgrRestart, ConstructorPurgesStaleRowsBeforeFirstPublication)
{
    testing_db::reset();
    auto configDb = make_shared<DBConnector>("CONFIG_DB", 0);
    auto stateDb = make_shared<DBConnector>("STATE_DB", 0);
    Table configPortTable(configDb.get(), CFG_PORT_TABLE_NAME);
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    Table participantTable(stateDb.get(), STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME);
    Table unrelatedTable(stateDb.get(), STATE_PORT_TABLE_NAME);

    configPortTable.set("Ethernet0", {{"macsec", "profile"}});
    sessionTable.set("Ethernet0", {{"query_status", "ok"}});
    sessionTable.set("Ethernet4", {{"query_status", "ok"}});
    participantTable.set("Ethernet0|" + PRIMARY_CKN, {{"active", "true"}});
    participantTable.set("Ethernet4|" + PRIMARY_CKN, {{"active", "true"}});
    participantTable.set("Ethernet8|" + PRIMARY_CKN, {{"active", "true"}});
    unrelatedTable.set("Ethernet0", {{"state", "ok"}});

    MACsecMgr manager(
        configDb.get(),
        stateDb.get(),
        {CFG_MACSEC_PROFILE_TABLE_NAME, CFG_PORT_TABLE_NAME});

    vector<FieldValueTuple> values;
    EXPECT_FALSE(sessionTable.get("Ethernet0", values));
    EXPECT_FALSE(participantTable.get("Ethernet0|" + PRIMARY_CKN, values));
    EXPECT_FALSE(sessionTable.get("Ethernet4", values));
    EXPECT_FALSE(participantTable.get("Ethernet4|" + PRIMARY_CKN, values));
    EXPECT_FALSE(participantTable.get("Ethernet8|" + PRIMARY_CKN, values));
    EXPECT_TRUE(unrelatedTable.get("Ethernet0", values));
}

TEST(MACsecMgrRestart, GracefulShutdownPurgesDaemonOwnedRows)
{
    testing_db::reset();
    auto configDb = make_shared<DBConnector>("CONFIG_DB", 0);
    auto stateDb = make_shared<DBConnector>("STATE_DB", 0);
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    Table participantTable(stateDb.get(), STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME);
    Table unrelatedTable(stateDb.get(), STATE_PORT_TABLE_NAME);
    {
        MACsecMgr manager(
            configDb.get(), stateDb.get(),
            {CFG_MACSEC_PROFILE_TABLE_NAME, CFG_PORT_TABLE_NAME});
        sessionTable.set("Ethernet0", {{"query_status", "ok"}});
        participantTable.set("Ethernet0|" + PRIMARY_CKN, {{"active", "true"}});
        participantTable.set("Ethernet4|" + PRIMARY_CKN, {{"active", "true"}});
        unrelatedTable.set("Ethernet0", {{"state", "ok"}});
    }
    vector<FieldValueTuple> values;
    EXPECT_FALSE(sessionTable.get("Ethernet0", values));
    EXPECT_FALSE(participantTable.get("Ethernet0|" + PRIMARY_CKN, values));
    EXPECT_FALSE(participantTable.get("Ethernet4|" + PRIMARY_CKN, values));
    EXPECT_TRUE(unrelatedTable.get("Ethernet0", values));
}

TEST_F(MACsecMgrTest, PrimaryRolloverRemovesBeforeAddingWithoutRestart)
{
    auto &mkaSession = session();
    auto desired = profile(NEW_PRIMARY_CKN);
    manager->m_profiles["profile"] = desired;

    ASSERT_TRUE(manager->reconcilePort("Ethernet0", mkaSession, desired));

    const auto remove = find_if(
        mockCallArgs.begin(), mockCallArgs.end(),
        [](const string &cmd) { return cmd.find("macsec_del_mka") != string::npos; });
    const auto add = find_if(
        mockCallArgs.begin(), mockCallArgs.end(),
        [](const string &cmd) { return cmd.find("macsec_add_mka") != string::npos; });
    ASSERT_NE(mockCallArgs.end(), remove);
    ASSERT_NE(mockCallArgs.end(), add);
    EXPECT_LT(distance(mockCallArgs.begin(), remove), distance(mockCallArgs.begin(), add));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("interface_remove") != string::npos ||
               cmd.find("interface_add") != string::npos ||
               cmd.find("enable_network") != string::npos;
    }));
    ASSERT_NE(mockCallArgs.begin(), remove);
    EXPECT_NE(string::npos, (remove - 1)->find("macsec_mka_list"));
}

TEST_F(MACsecMgrTest, AuthenticatedUnsecuredSessionStillRotatesInPlace)
{
    auto &mkaSession = session();
    auto desired = profile(NEW_PRIMARY_CKN);
    manager->m_profiles["profile"] = desired;
    commandState.authenticated = true;
    commandState.secured = false;
    commandState.peerCountsByPort["Ethernet0"].fallbackLive = 0;

    EXPECT_TRUE(manager->reconcilePort("Ethernet0", mkaSession, desired));
    EXPECT_EQ(1, commandState.addCalls);
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(any_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_del_mka") != string::npos;
        }));
}

TEST_F(MACsecMgrTest, QueryFailureOnOnePortDoesNotBlockOtherPorts)
{
    auto &unsafeSession = session("Ethernet0");
    auto &safeSession = session("Ethernet4");
    commandState.failQueryPorts.insert("Ethernet0");

    EXPECT_EQ(task_success,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(NEW_PRIMARY_CKN, manager->m_profiles["profile"].primary_ckn);
    EXPECT_EQ(PRIMARY_CKN, unsafeSession.applied_profile.primary_ckn);
    EXPECT_EQ(NEW_PRIMARY_CKN, safeSession.applied_profile.primary_ckn);
    EXPECT_EQ(PRIMARY_CKN, commandState.primaryCknByPort["Ethernet0"]);
    EXPECT_EQ(NEW_PRIMARY_CKN, commandState.primaryCknByPort["Ethernet4"]);
    EXPECT_TRUE(none_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return commandPort(cmd) == "Ethernet0" &&
                   cmd.find("macsec_del_mka") != string::npos;
        }));
    EXPECT_TRUE(any_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return commandPort(cmd) == "Ethernet4" &&
                   cmd.find("macsec_add_mka") != string::npos;
        }));

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_FALSE(sessionTable.get("Ethernet0", values));
    EXPECT_TRUE(sessionTable.get("Ethernet4", values));
}

TEST_F(MACsecMgrTest, MultipleSafePortsAllRotate)
{
    auto &ethernet0 = session("Ethernet0");
    auto &ethernet4 = session("Ethernet4");
    auto &ethernet100 = session("Ethernet100");

    EXPECT_EQ(
        task_success,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(NEW_PRIMARY_CKN, ethernet0.applied_profile.primary_ckn);
    EXPECT_EQ(NEW_PRIMARY_CKN, ethernet4.applied_profile.primary_ckn);
    EXPECT_EQ(NEW_PRIMARY_CKN, ethernet100.applied_profile.primary_ckn);
    EXPECT_EQ(3, commandState.addCalls);
}

TEST_F(MACsecMgrTest, PendingPortObservesAmbiguousAddWithoutReapplyingCompletedPort)
{
    auto &deferredSession = session("Ethernet0");
    auto &completedSession = session("Ethernet4");
    commandState.failFirstPostAddQueryPorts.insert("Ethernet0");

    EXPECT_EQ(task_success,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));
    ASSERT_EQ(2, commandState.addCalls);
    ASSERT_EQ(NEW_PRIMARY_CKN, completedSession.applied_profile.primary_ckn);

    commandState.failFirstPostAddQueryPorts.clear();
    mockCallArgs.clear();
    EXPECT_EQ(
        task_success,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(NEW_PRIMARY_CKN, deferredSession.applied_profile.primary_ckn);
    EXPECT_EQ(NEW_PRIMARY_CKN, completedSession.applied_profile.primary_ckn);
    EXPECT_EQ(2, commandState.addCalls);
    EXPECT_TRUE(none_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return commandPort(cmd) == "Ethernet4" &&
                   (cmd.find("macsec_del_mka") != string::npos ||
                    cmd.find("macsec_add_mka") != string::npos);
        }));
}

TEST_F(MACsecMgrTest, DesiredProfileIsCachedWithoutRuntimeSession)
{
    Table configPortTable(configDb.get(), CFG_PORT_TABLE_NAME);
    configPortTable.set("Ethernet8", {
        {"macsec", "profile"},
        {"admin_status", "down"},
    });
    manager->m_profiles["profile"] = profile();

    EXPECT_EQ(
        task_success,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(NEW_PRIMARY_CKN, manager->m_profiles["profile"].primary_ckn);
    EXPECT_TRUE(manager->m_macsec_ports.empty());
    EXPECT_TRUE(mockCallArgs.empty());
}

TEST_F(MACsecMgrTest, FallbackPresenceChangesRemainUnsupported)
{
    auto &mkaSession = session();
    ASSERT_TRUE(manager->collectMKAStatus("Ethernet0", mkaSession));
    auto desired = primaryOnlyProfile();
    manager->m_profiles["profile"] = desired;

    EXPECT_FALSE(manager->reconcilePort("Ethernet0", mkaSession, desired));
    EXPECT_EQ(0, commandState.addCalls);
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string status;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", status));
    EXPECT_EQ("degraded", status);

    mkaSession.applied_profile = primaryOnlyProfile();
    desired = profile();
    manager->m_profiles["profile"] = desired;
    EXPECT_FALSE(manager->reconcilePort("Ethernet0", mkaSession, desired));
    EXPECT_EQ(0, commandState.addCalls);
}

TEST_F(MACsecMgrTest, PrimaryOnlyProfileRotatesWithoutManagerAlternateGuarantee)
{
    auto &mkaSession = session();
    mkaSession.applied_profile = primaryOnlyProfile();
    manager->m_profiles["profile"] = mkaSession.applied_profile;
    commandState.fallbackCknByPort["Ethernet0"].clear();

    EXPECT_EQ(task_success,
        manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(NEW_PRIMARY_CKN, manager->m_profiles["profile"].primary_ckn);
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(any_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_mka_list") != string::npos;
        }));
    EXPECT_TRUE(any_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_del_mka") != string::npos;
        }));

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string configStatus;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", configStatus));
    EXPECT_EQ("in-sync", configStatus);
}

TEST_F(MACsecMgrTest, PeerlessPrimaryOnlyDesiredChangeAppliesImmediately)
{
    auto &mkaSession = session();
    mkaSession.applied_profile = primaryOnlyProfile();
    manager->m_profiles["profile"] = mkaSession.applied_profile;
    commandState.fallbackCknByPort["Ethernet0"].clear();
    commandState.peerCountsByPort["Ethernet0"] = {};
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.authenticated = true;
    commandState.secured = false;

    EXPECT_EQ(
        task_success,
        manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(NEW_PRIMARY_CKN, manager->m_profiles["profile"].primary_ckn);
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(NEW_PRIMARY_CKN, commandState.primaryCknByPort["Ethernet0"]);
    EXPECT_TRUE(any_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_del_mka") != string::npos;
        }));
    EXPECT_TRUE(any_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_add_mka") != string::npos;
        }));
}

TEST_F(MACsecMgrTest, PotentialPeerDoesNotVetoLiveRekey)
{
    auto &mkaSession = session();
    mkaSession.applied_profile = primaryOnlyProfile();
    manager->m_profiles["profile"] = mkaSession.applied_profile;
    commandState.fallbackCknByPort["Ethernet0"].clear();
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.peerCountsByPort["Ethernet0"].primaryPotential = 1;

    EXPECT_EQ(task_success,
        manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(any_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_add_mka") != string::npos;
        }));
}

TEST_F(MACsecMgrTest, QueryFailureDefersPrimaryOnlyRemovalWithoutClaimingConvergence)
{
    auto &mkaSession = session();
    mkaSession.applied_profile = primaryOnlyProfile();
    manager->m_profiles["profile"] = mkaSession.applied_profile;
    commandState.fallbackCknByPort["Ethernet0"].clear();
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.failQueryPorts.insert("Ethernet0");

    EXPECT_EQ(task_success,
        manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
    EXPECT_EQ(PRIMARY_CKN, commandState.primaryCknByPort["Ethernet0"]);
    EXPECT_TRUE(none_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_add_mka") != string::npos;
        }));
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_FALSE(sessionTable.get("Ethernet0", values));
    commandState.failQueryPorts.clear();
    EXPECT_EQ(task_success, manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(1, commandState.addCalls);
    string status;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "query_status", status));
    EXPECT_EQ("ok", status);
}

TEST_F(MACsecMgrTest, PeerAppearingDuringObservationDoesNotVetoCommands)
{
    auto &mkaSession = session();
    mkaSession.applied_profile = primaryOnlyProfile();
    manager->m_profiles["profile"] = mkaSession.applied_profile;
    commandState.fallbackCknByPort["Ethernet0"].clear();
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.peersAppearOnSecondQueryPorts.insert("Ethernet0");

    EXPECT_EQ(task_success,
        manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(3u, commandState.queryCountByPort["Ethernet0"]);
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(any_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_add_mka") != string::npos;
        }));
}

TEST_F(MACsecMgrTest, PeerlessDualParticipantUpdateCompletesInOneReconciliation)
{
    auto &mkaSession = session();
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.peerCountsByPort["Ethernet0"].fallbackLive = 0;
    auto desiredArgs = profileArgs(NEW_PRIMARY_CKN);
    for (auto &fieldValue : desiredArgs)
    {
        if (fvField(fieldValue) == "fallback_cak")
        {
            fvValue(fieldValue) = NEW_ENCODED_CAK;
        }
        else if (fvField(fieldValue) == "fallback_ckn")
        {
            fvValue(fieldValue) = NEW_FALLBACK_CKN;
        }
    }

    EXPECT_EQ(
        task_success,
        manager->loadProfile("profile", desiredArgs));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(2, commandState.addCalls);
    EXPECT_EQ(NEW_PRIMARY_CKN, commandState.networkPrimaryCknByPort["Ethernet0"]);
    EXPECT_EQ(NEW_FALLBACK_CKN, commandState.networkFallbackCknByPort["Ethernet0"]);
    EXPECT_GE(commandState.queryCountByPort["Ethernet0"], 4u);
}

TEST_F(MACsecMgrTest, PeerlessRemoveFailureDoesNotClaimSuccess)
{
    auto &mkaSession = session();
    mkaSession.applied_profile = primaryOnlyProfile();
    manager->m_profiles["profile"] = mkaSession.applied_profile;
    commandState.fallbackCknByPort["Ethernet0"].clear();
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.failRemove = true;

    EXPECT_EQ(task_success,
        manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));

    EXPECT_TRUE(mkaSession.applied_profile.primary_ckn.empty());
    EXPECT_EQ(PRIMARY_CKN, mkaSession.pending_old_ckn);
    EXPECT_EQ(0, commandState.addCalls);
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string configStatus;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", configStatus));
    EXPECT_EQ("degraded", configStatus);
}

TEST_F(MACsecMgrTest, RemoveFailureNeverAddsReplacement)
{
    auto &mkaSession = session();
    auto desired = profile(NEW_PRIMARY_CKN);
    manager->m_profiles["profile"] = desired;
    commandState.failRemove = true;

    EXPECT_FALSE(manager->reconcilePort("Ethernet0", mkaSession, desired));
    EXPECT_EQ(0, commandState.addCalls);
    EXPECT_TRUE(mkaSession.applied_profile.primary_ckn.empty());
    EXPECT_EQ(PRIMARY_CKN, mkaSession.pending_old_ckn);

    manager->sweepMKAStatus();
    EXPECT_EQ(0, commandState.addCalls);
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string status;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", status));
    EXPECT_EQ("degraded", status);

    commandState.failRemove = false;
    manager->sweepMKAStatus();
    EXPECT_EQ(1, commandState.addCalls);
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", status));
    EXPECT_EQ("in-sync", status);
}

TEST_F(MACsecMgrTest, AmbiguousRemoveIsObservedBeforeAddingReplacement)
{
    auto &mkaSession = session();
    auto desired = profile(NEW_PRIMARY_CKN);
    manager->m_profiles["profile"] = desired;
    commandState.removeAppliedButFailed = true;

    EXPECT_TRUE(manager->reconcilePort("Ethernet0", mkaSession, desired));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(1, commandState.addCalls);

    const auto remove = find_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    });
    ASSERT_NE(mockCallArgs.end(), remove);
    const auto observation = find_if(remove + 1, mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_mka_list") != string::npos;
    });
    const auto add = find_if(remove + 1, mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_add_mka") != string::npos;
    });
    ASSERT_NE(mockCallArgs.end(), observation);
    ASSERT_NE(mockCallArgs.end(), add);
    EXPECT_LT(distance(mockCallArgs.begin(), observation), distance(mockCallArgs.begin(), add));
    EXPECT_EQ(1, count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
}

TEST_F(MACsecMgrTest, AmbiguousRemoveWaitsForStatusThenAddsWithoutRemovingAgain)
{
    auto &mkaSession = session();
    commandState.removeAppliedButFailed = true;
    commandState.failedQueriesByPort["Ethernet0"].insert(2);

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(FALLBACK_CKN, mkaSession.pending_old_ckn);
    EXPECT_EQ(0, commandState.addCalls);
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_FALSE(sessionTable.get("Ethernet0", values));
    EXPECT_EQ(FALLBACK_CKN, mkaSession.pending_old_ckn);

    commandState.failQuery = true;
    manager->sweepMKAStatus();
    EXPECT_EQ(0, commandState.addCalls);
    commandState.failQuery = false;
    manager->sweepMKAStatus();
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(1, commandState.addCalls);
    EXPECT_EQ(1, count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
    string status;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", status));
    EXPECT_EQ("in-sync", status);
}

TEST_F(MACsecMgrTest, FallbackRolloverKeepsPrimaryAndAddsBestEffort)
{
    auto &mkaSession = session();
    auto desired = profile();
    desired.fallback_ckn = NEW_FALLBACK_CKN;
    desired.fallback_cak = NEW_ENCODED_CAK;
    manager->m_profiles["profile"] = desired;

    ASSERT_TRUE(manager->reconcilePort("Ethernet0", mkaSession, desired));

    EXPECT_EQ(PRIMARY_CKN, commandState.primaryCknByPort["Ethernet0"]);
    EXPECT_EQ(NEW_FALLBACK_CKN, commandState.fallbackCknByPort["Ethernet0"]);
    const auto add = find_if(
        mockCallArgs.begin(), mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_add_mka") != string::npos &&
                   cmd.find("fallback=1") != string::npos;
        });
    EXPECT_NE(mockCallArgs.end(), add);
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("interface_remove") != string::npos ||
               cmd.find("interface_add") != string::npos;
    }));
}

TEST_F(MACsecMgrTest, RetryRecognizesReplacementAfterAmbiguousAddFailure)
{
    auto &mkaSession = session();
    auto desired = profile(NEW_PRIMARY_CKN);
    manager->m_profiles["profile"] = desired;
    commandState.addAppliedButFailed = true;

    EXPECT_FALSE(manager->reconcilePort("Ethernet0", mkaSession, desired));
    EXPECT_EQ(1, commandState.addCalls);

    commandState.addAppliedButFailed = false;
    EXPECT_TRUE(manager->reconcilePort("Ethernet0", mkaSession, desired));
    EXPECT_EQ(1, commandState.addCalls);
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
}

TEST_F(MACsecMgrTest, PostAddQueryFailureKeepsPendingUntilReplacementObserved)
{
    auto &mkaSession = session();
    commandState.failFirstPostAddQueryPorts.insert("Ethernet0");

    EXPECT_EQ(task_success,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));

    EXPECT_TRUE(mkaSession.applied_profile.primary_ckn.empty());
    EXPECT_EQ(PRIMARY_CKN, mkaSession.pending_old_ckn);
    EXPECT_TRUE(mkaSession.pending_primary);
    EXPECT_EQ(1, commandState.addCalls);

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    vector<FieldValueTuple> values;
    EXPECT_FALSE(sessionTable.get("Ethernet0", values));

    EXPECT_EQ(
        task_success,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
    EXPECT_EQ(1, commandState.addCalls);
    string value;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", value));
    EXPECT_EQ("in-sync", value);
}

TEST_F(MACsecMgrTest, MissingReplacementAfterAddRemainsPendingUntilRetryObservation)
{
    auto &mkaSession = session();
    commandState.omitFirstPostAddReplacementPorts.insert("Ethernet0");

    EXPECT_EQ(task_success,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));

    EXPECT_TRUE(mkaSession.applied_profile.primary_ckn.empty());
    EXPECT_EQ(PRIMARY_CKN, mkaSession.pending_old_ckn);
    EXPECT_EQ(1, commandState.addCalls);

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string value;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "query_status", value));
    EXPECT_EQ("ok", value);
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", value));
    EXPECT_EQ("degraded", value);
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_error", value));
    EXPECT_EQ("replacement participant is not yet verified", value);

    EXPECT_EQ(
        task_success,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
    EXPECT_EQ(1, commandState.addCalls);
}

TEST_F(MACsecMgrTest, QueuedPrimaryThenFallbackUsesAppliedAlternate)
{
    auto &mkaSession = session();
    commandState.failQuery = true;
    ASSERT_EQ(task_success, manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));
    commandState.failQuery = false;
    commandState.newParticipantsPeerless = true;

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(NEW_PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(2, commandState.addCalls);

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(NEW_PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(2, commandState.addCalls);
}

TEST_F(MACsecMgrTest, QueuedFallbackThenPrimaryConvergesWithoutPeerChecks)
{
    auto &mkaSession = session();
    commandState.failQuery = true;
    ASSERT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    commandState.failQuery = false;
    commandState.peerCountsByPort["Ethernet0"].fallbackLive = 0;
    commandState.newParticipantsPeerless = true;

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(NEW_PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(2, commandState.addCalls);

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(NEW_PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(2, commandState.addCalls);
    EXPECT_EQ(NEW_PRIMARY_CKN, commandState.networkPrimaryCknByPort["Ethernet0"]);
    EXPECT_EQ(NEW_FALLBACK_CKN, commandState.networkFallbackCknByPort["Ethernet0"]);
}

TEST_F(MACsecMgrTest, PendingFallbackAddRetriesBeforeQueuedPrimary)
{
    auto &mkaSession = session();
    commandState.failFallbackAdd = true;
    ASSERT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    ASSERT_TRUE(mkaSession.applied_profile.fallback_ckn.empty());
    ASSERT_EQ(FALLBACK_CKN, mkaSession.pending_old_ckn);

    commandState.failFallbackAdd = false;
    commandState.newParticipantsPeerless = true;
    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(NEW_PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(NEW_PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(3, commandState.addCalls);
}

TEST_F(MACsecMgrTest, AbsentPendingFallbackRestoresAfterAlternateLosesLivePeer)
{
    auto &mkaSession = session();
    commandState.failFallbackAdd = true;
    ASSERT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    ASSERT_TRUE(commandState.fallbackCknByPort["Ethernet0"].empty());
    ASSERT_EQ(FALLBACK_CKN, mkaSession.pending_old_ckn);

    commandState.failFallbackAdd = false;
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.peerCountsByPort["Ethernet0"].primaryPotential = 1;
    commandState.authenticated = true;
    commandState.secured = false;
    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(NEW_FALLBACK_CKN, commandState.networkFallbackCknByPort["Ethernet0"]);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
    EXPECT_EQ(2, commandState.addCalls);
    EXPECT_EQ(1, count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string value;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", value));
    EXPECT_EQ("in-sync", value);
}

TEST_F(MACsecMgrTest, StillPresentPendingFallbackIsRemovedAgainWithoutPeerChecks)
{
    auto &mkaSession = session();
    commandState.failFallbackAdd = true;
    ASSERT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    ASSERT_EQ(FALLBACK_CKN, mkaSession.pending_old_ckn);

    commandState.failFallbackAdd = false;
    commandState.fallbackCknByPort["Ethernet0"] = FALLBACK_CKN;
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.peerCountsByPort["Ethernet0"].primaryPotential = 1;
    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
    EXPECT_EQ(2, commandState.addCalls);
    EXPECT_EQ(2, count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
}

TEST_F(MACsecMgrTest, SupersededPendingFallbackCollisionUsesAttemptedTargetAsBridge)
{
    auto &mkaSession = session();
    commandState.failFallbackAdd = true;
    ASSERT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    ASSERT_TRUE(commandState.fallbackCknByPort["Ethernet0"].empty());

    commandState.failFallbackAdd = false;
    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(NEW_PRIMARY_CKN, PRIMARY_CKN)));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(NEW_PRIMARY_CKN, commandState.networkPrimaryCknByPort["Ethernet0"]);
    EXPECT_EQ(PRIMARY_CKN, commandState.networkFallbackCknByPort["Ethernet0"]);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
    EXPECT_EQ(4, commandState.addCalls);
    EXPECT_EQ(3, count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
}

TEST_F(MACsecMgrTest, BridgeRestorationConvergesEvenWithPeerlessParticipants)
{
    auto &mkaSession = session();
    commandState.failFallbackAdd = true;
    ASSERT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));

    commandState.failFallbackAdd = false;
    commandState.newParticipantsPeerless = true;
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.peerCountsByPort["Ethernet0"].primaryPotential = 1;
    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(NEW_PRIMARY_CKN, PRIMARY_CKN)));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
    EXPECT_EQ(3, count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
}

TEST_F(MACsecMgrTest, PendingBridgeCannotCollideWithFinalAlternateTarget)
{
    auto &mkaSession = session();
    commandState.failFallbackAdd = true;
    ASSERT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));

    commandState.failFallbackAdd = false;
    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(NEW_FALLBACK_CKN, PRIMARY_CKN)));
    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(mkaSession.applied_profile.fallback_ckn.empty());
    EXPECT_EQ(FALLBACK_CKN, mkaSession.pending_old_ckn);
    EXPECT_EQ(1, commandState.addCalls);
}

TEST_F(MACsecMgrTest, PendingFallbackQueryRecoversBeforeQueuedPrimary)
{
    auto &mkaSession = session();
    commandState.failFirstPostAddQueryPorts.insert("Ethernet0");
    commandState.newParticipantsPeerless = true;
    ASSERT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    ASSERT_TRUE(mkaSession.applied_profile.fallback_ckn.empty());
    ASSERT_EQ(1, commandState.addCalls);

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(NEW_PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_TRUE(mkaSession.applied_profile.primary_ckn.empty());
    EXPECT_EQ(PRIMARY_CKN, mkaSession.pending_old_ckn);
    EXPECT_EQ(2, commandState.addCalls);

    commandState.failFirstPostAddQueryPorts.clear();
    commandState.peerCountsByPort["Ethernet0"].fallbackLive = 1;
    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(NEW_PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
    EXPECT_EQ(2, commandState.addCalls);
}

TEST_F(MACsecMgrTest, SupersededPendingFallbackIsObservedThenReplaced)
{
    auto &mkaSession = session();
    commandState.failFirstPostAddQueryPorts.insert("Ethernet0");
    ASSERT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    ASSERT_EQ(NEW_FALLBACK_CKN, commandState.fallbackCknByPort["Ethernet0"]);
    ASSERT_TRUE(mkaSession.applied_profile.fallback_ckn.empty());

    commandState.failFirstPostAddQueryPorts.clear();
    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, LATEST_FALLBACK_CKN)));
    EXPECT_EQ(LATEST_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(LATEST_FALLBACK_CKN, commandState.fallbackCknByPort["Ethernet0"]);
    EXPECT_EQ(LATEST_FALLBACK_CKN, commandState.networkFallbackCknByPort["Ethernet0"]);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
    EXPECT_EQ(2, commandState.addCalls);
}

TEST_F(MACsecMgrTest, SupersededMissingFallbackAddsLatestDesired)
{
    auto &mkaSession = session();
    commandState.failFallbackAdd = true;
    ASSERT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    ASSERT_TRUE(commandState.fallbackCknByPort["Ethernet0"].empty());

    commandState.failFallbackAdd = false;
    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, LATEST_FALLBACK_CKN)));
    EXPECT_EQ(LATEST_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(LATEST_FALLBACK_CKN, commandState.fallbackCknByPort["Ethernet0"]);
    EXPECT_EQ(LATEST_FALLBACK_CKN, commandState.networkFallbackCknByPort["Ethernet0"]);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
}

TEST_F(MACsecMgrTest, SupersededPendingPrimaryIsObservedBeforeBothRolesConverge)
{
    auto &mkaSession = session();
    commandState.failFirstPostAddQueryPorts.insert("Ethernet0");
    commandState.newParticipantsPeerless = true;
    ASSERT_EQ(task_success, manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));
    ASSERT_TRUE(mkaSession.applied_profile.primary_ckn.empty());
    ASSERT_EQ(NEW_PRIMARY_CKN, commandState.primaryCknByPort["Ethernet0"]);

    commandState.failFirstPostAddQueryPorts.clear();
    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(LATEST_PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(LATEST_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(3, commandState.addCalls);

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(LATEST_PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(3, commandState.addCalls);
    EXPECT_EQ(LATEST_PRIMARY_CKN, commandState.networkPrimaryCknByPort["Ethernet0"]);
    EXPECT_EQ(NEW_FALLBACK_CKN, commandState.networkFallbackCknByPort["Ethernet0"]);
}

TEST_F(MACsecMgrTest, LosingAlternatePeerDoesNotVetoFallbackUpdate)
{
    auto &mkaSession = session();
    commandState.primaryPeerLostOnSecondQueryPorts.insert("Ethernet0");

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(3u, commandState.queryCountByPort["Ethernet0"]);
    EXPECT_EQ(1, commandState.addCalls);
    EXPECT_TRUE(any_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string value;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", value));
    EXPECT_EQ("in-sync", value);
}

TEST_F(MACsecMgrTest, QueuedReplacementAvoidsCollisionWithAppliedAlternate)
{
    auto &mkaSession = session();

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(FALLBACK_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(FALLBACK_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(2, commandState.addCalls);
    const auto firstRemove = find_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    });
    ASSERT_NE(mockCallArgs.end(), firstRemove);
    EXPECT_EQ(FALLBACK_CKN, commandValue(*firstRemove, "ckn="));
}

TEST_F(MACsecMgrTest, SupersededPendingFallbackCanReturnToOriginalCkn)
{
    auto &mkaSession = session();
    commandState.failFirstPostAddQueryPorts.insert("Ethernet0");
    ASSERT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    ASSERT_TRUE(mkaSession.applied_profile.fallback_ckn.empty());

    commandState.failFirstPostAddQueryPorts.clear();
    EXPECT_EQ(task_success, manager->loadProfile("profile", profileArgs()));
    EXPECT_EQ(FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_EQ(ENCODED_CAK, mkaSession.applied_profile.fallback_cak);
    EXPECT_EQ(FALLBACK_CKN, commandState.networkFallbackCknByPort["Ethernet0"]);
    EXPECT_EQ(2, commandState.addCalls);
}

TEST_F(MACsecMgrTest, ReplacementObservedAfterRemoveIsNotAddedTwice)
{
    auto &mkaSession = session();
    commandState.networkFallbackCknByPort["Ethernet0"] = NEW_FALLBACK_CKN;
    commandState.replacementAppearsAfterRemovePorts.insert("Ethernet0");

    EXPECT_EQ(task_success, manager->loadProfile(
        "profile", profileArgs(PRIMARY_CKN, NEW_FALLBACK_CKN)));
    EXPECT_EQ(NEW_FALLBACK_CKN, mkaSession.applied_profile.fallback_ckn);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
    EXPECT_EQ(0, commandState.addCalls);
}

TEST_F(MACsecMgrTest, RotationQueryFailurePreventsDeleteAndPreservesAppliedRole)
{
    auto &runtime = session();
    commandState.failQuery = true;
    EXPECT_EQ(task_success, manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));
    EXPECT_EQ(PRIMARY_CKN, runtime.applied_profile.primary_ckn);
    EXPECT_TRUE(runtime.pending_old_ckn.empty());
    EXPECT_EQ(0, commandState.addCalls);
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
    vector<FieldValueTuple> values;
    EXPECT_FALSE(manager->m_stateMkaSessionTable.get("Ethernet0", values));
}

TEST_F(MACsecMgrTest, RotationChangedTargetRoleDefersWithoutDelete)
{
    auto &runtime = session();
    commandState.queryRepliesByPort["Ethernet0"][1] =
        statusOutput(FALLBACK_CKN, PRIMARY_CKN);
    EXPECT_EQ(task_success, manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));
    EXPECT_EQ(PRIMARY_CKN, runtime.applied_profile.primary_ckn);
    EXPECT_EQ(0, commandState.addCalls);
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
    EXPECT_EQ("participant role changed before removal", runtime.config_error);
    string value;
    ASSERT_TRUE(getField(manager->m_stateMkaSessionTable, "Ethernet0", "config_status", value));
    EXPECT_EQ("degraded", value);
}

TEST_F(MACsecMgrTest, MalformedPreDeleteSnapshotClearsRowsWithoutMutation)
{
    auto &runtime = session();
    ASSERT_TRUE(manager->collectMKAStatus("Ethernet0", runtime));
    auto partial = statusOutput(PRIMARY_CKN);
    const string field = "live_peers=1\n";
    partial.erase(partial.find(field), field.size());
    commandState.queryRepliesByPort["Ethernet0"][2] = partial;

    EXPECT_EQ(task_success, manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));
    EXPECT_EQ(PRIMARY_CKN, runtime.applied_profile.primary_ckn);
    EXPECT_TRUE(runtime.pending_old_ckn.empty());
    EXPECT_EQ(0, commandState.addCalls);
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
    vector<FieldValueTuple> values;
    EXPECT_FALSE(manager->m_stateMkaSessionTable.get("Ethernet0", values));
    EXPECT_FALSE(manager->m_stateMkaParticipantTable.get("Ethernet0|" + PRIMARY_CKN, values));
}

TEST_F(MACsecMgrTest, RotationObservesAlreadyReplacedTargetWithoutDeleteOrAdd)
{
    auto &runtime = session();
    commandState.queryRepliesByPort["Ethernet0"][1] = statusOutput(NEW_PRIMARY_CKN);
    commandState.networkPrimaryCknByPort["Ethernet0"] = NEW_PRIMARY_CKN;
    EXPECT_EQ(task_success, manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));
    EXPECT_EQ(NEW_PRIMARY_CKN, runtime.applied_profile.primary_ckn);
    EXPECT_TRUE(runtime.pending_old_ckn.empty());
    EXPECT_EQ(0, commandState.addCalls);
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
}

TEST_F(MACsecMgrTest, DriftRecoveryDoesNotRemoveParticipantFromObsoleteSnapshot)
{
    auto &runtime = session();
    MKASessionStatus stale;
    string error;
    ASSERT_TRUE(parseMKAStatus(statusOutput(NEW_PRIMARY_CKN), stale, error));

    EXPECT_TRUE(manager->reconcileObservedParticipants(
        "Ethernet0", runtime, manager->m_profiles.at("profile"), stale));
    EXPECT_EQ(0, commandState.addCalls);
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
    EXPECT_TRUE(runtime.participants_verified);
}

TEST_F(MACsecMgrTest, DriftRecoveryRoleChangeDefersWithoutDelete)
{
    auto &runtime = session();
    MKASessionStatus stale;
    string error;
    ASSERT_TRUE(parseMKAStatus(statusOutput(NEW_PRIMARY_CKN), stale, error));
    commandState.queryRepliesByPort["Ethernet0"][1] =
        statusOutput(PRIMARY_CKN, NEW_PRIMARY_CKN);

    EXPECT_FALSE(manager->reconcileObservedParticipants(
        "Ethernet0", runtime, manager->m_profiles.at("profile"), stale));
    EXPECT_EQ(0, commandState.addCalls);
    EXPECT_TRUE(none_of(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
    EXPECT_EQ("participant role changed before removal", runtime.config_error);
}

TEST_F(MACsecMgrTest, DriftRecoveryObservesEachTargetImmediatelyBeforeDelete)
{
    auto &runtime = session();
    MKASessionStatus stale;
    string error;
    ASSERT_TRUE(parseMKAStatus(
        statusOutput(NEW_PRIMARY_CKN, NEW_FALLBACK_CKN), stale, error));
    commandState.primaryCknByPort["Ethernet0"] = NEW_PRIMARY_CKN;
    commandState.fallbackCknByPort["Ethernet0"] = NEW_FALLBACK_CKN;

    EXPECT_TRUE(manager->reconcileObservedParticipants(
        "Ethernet0", runtime, manager->m_profiles.at("profile"), stale));
    size_t removes = 0;
    for (size_t index = 0; index < mockCallArgs.size(); ++index)
    {
        if (mockCallArgs[index].find("macsec_del_mka") != string::npos)
        {
            ++removes;
            ASSERT_GT(index, 0u);
            EXPECT_NE(string::npos, mockCallArgs[index - 1].find("macsec_mka_list"));
        }
    }
    EXPECT_EQ(2u, removes);
    EXPECT_EQ(2, commandState.addCalls);
    EXPECT_TRUE(runtime.participants_verified);
}

TEST_F(MACsecMgrTest, DriftRecoveryFailedSecondTargetObservationStopsFurtherMutation)
{
    auto &runtime = session();
    MKASessionStatus stale;
    string error;
    ASSERT_TRUE(parseMKAStatus(
        statusOutput(NEW_PRIMARY_CKN, NEW_FALLBACK_CKN), stale, error));
    commandState.primaryCknByPort["Ethernet0"] = NEW_PRIMARY_CKN;
    commandState.fallbackCknByPort["Ethernet0"] = NEW_FALLBACK_CKN;
    commandState.failedQueriesByPort["Ethernet0"].insert(3);

    EXPECT_FALSE(manager->reconcileObservedParticipants(
        "Ethernet0", runtime, manager->m_profiles.at("profile"), stale));
    EXPECT_EQ(1, count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const string &cmd) {
        return cmd.find("macsec_del_mka") != string::npos;
    }));
    EXPECT_EQ(0, commandState.addCalls);
    vector<FieldValueTuple> values;
    EXPECT_FALSE(manager->m_stateMkaSessionTable.get("Ethernet0", values));
    EXPECT_FALSE(manager->m_stateMkaParticipantTable.get("Ethernet0|" + NEW_FALLBACK_CKN, values));
}

}
