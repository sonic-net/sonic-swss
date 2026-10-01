#include "gtest/gtest.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../mock_table.h"
#include "redisutility.h"

#define private public
#include "macsecmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;

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
    map<string, PeerCounts> peerCountsByPort;
    map<string, size_t> queryCountByPort;
    set<string> peersAppearOnSecondQueryPorts;
    set<string> failFirstPostAddQueryPorts;
    set<string> omitFirstPostAddReplacementPorts;
    set<string> postAddQueryConsumedPorts;
    map<string, bool> lastAddWasPrimaryByPort;
    bool failQuery = false;
    string failQueryPort;
    set<string> failQueryPorts;
    bool failRemove = false;
    bool addAppliedButFailed = false;
    bool authenticated = false;
    bool secured = true;
    int addCalls = 0;
};

CommandState commandState;

string commandPort(const string &cmd)
{
    const auto start = cmd.find("IFNAME=");
    if (start == string::npos)
    {
        return "";
    }
    const auto valueStart = start + string("IFNAME=").size();
    const auto end = cmd.find_first_of("\"' ", valueStart);
    return cmd.substr(valueStart, end - valueStart);
}

int commandCallback(const string &cmd, string &output)
{
    mockCallArgs.push_back(cmd);
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
        if (queryCount >= 2 &&
            commandState.peersAppearOnSecondQueryPorts.count(port) != 0)
        {
            peerCounts.primaryLive = 1;
        }
        if (commandState.failQuery ||
            (!commandState.failQueryPort.empty() &&
             port == commandState.failQueryPort) ||
            commandState.failQueryPorts.count(port) != 0)
        {
            output = "FAIL\n";
            return 1;
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
        }
        else
        {
            fallbackCkn.clear();
        }
        output = "OK\n";
        return 0;
    }
    if (cmd.find("macsec_add_mka") != string::npos)
    {
        ++commandState.addCalls;
        commandState.postAddQueryConsumedPorts.erase(port);
        if (cmd.find("fallback=1") != string::npos)
        {
            fallbackCkn = NEW_FALLBACK_CKN;
            commandState.lastAddWasPrimaryByPort[port] = false;
        }
        else
        {
            primaryCkn = NEW_PRIMARY_CKN;
            commandState.lastAddWasPrimaryByPort[port] = true;
        }
        output = commandState.addAppliedButFailed ? "FAIL\n" : "OK\n";
        return 0;
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

    MACsecMgr::TaskArgs profileArgs(const string &primaryCkn = PRIMARY_CKN)
    {
        return {
            {"cipher_suite", "GCM-AES-128"},
            {"primary_cak", primaryCkn == PRIMARY_CKN ? ENCODED_CAK : NEW_ENCODED_CAK},
            {"primary_ckn", primaryCkn},
            {"fallback_cak", ENCODED_CAK},
            {"fallback_ckn", FALLBACK_CKN},
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
        return value;
    }

    MACsecMgr::MKASession &session()
    {
        return session("Ethernet0");
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

TEST_F(MACsecMgrTest, QueryFailurePreservesLastSuccessfulSnapshot)
{
    auto &mkaSession = session();
    ASSERT_TRUE(manager->collectMKAStatus("Ethernet0", mkaSession));

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    Table participantTable(stateDb.get(), STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME);
    string lastUpdated;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "last_updated", lastUpdated));

    commandState.failQuery = true;
    EXPECT_FALSE(manager->collectMKAStatus("Ethernet0", mkaSession));

    string value;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "query_status", value));
    EXPECT_EQ("error", value);
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "last_updated", value));
    EXPECT_EQ(lastUpdated, value);
    vector<FieldValueTuple> participantValues;
    EXPECT_TRUE(participantTable.get("Ethernet0|" + PRIMARY_CKN, participantValues));
    EXPECT_FALSE(fvsGetValue(participantValues, "participant", true));
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
    ASSERT_TRUE(getField(sessionTable, "Ethernet104", "query_status", value));
    EXPECT_EQ("error", value);
    ASSERT_TRUE(getField(sessionTable, "Ethernet104", "last_updated", value));
    EXPECT_EQ("2000-01-01T00:00:00Z", value);

    ASSERT_TRUE(getField(sessionTable, "Ethernet136", "query_status", value));
    EXPECT_EQ("ok", value);
    ASSERT_TRUE(getField(sessionTable, "Ethernet136", "last_updated", value));
    EXPECT_NE("2000-01-01T00:00:00Z", value);
}

TEST(MACsecMgrRestart, RetainsConfiguredRowsAndDeletesOrphans)
{
    testing_db::reset();
    auto configDb = make_shared<DBConnector>("CONFIG_DB", 0);
    auto stateDb = make_shared<DBConnector>("STATE_DB", 0);
    Table configPortTable(configDb.get(), CFG_PORT_TABLE_NAME);
    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    Table participantTable(stateDb.get(), STATE_MACSEC_MKA_PARTICIPANT_TABLE_NAME);

    configPortTable.set("Ethernet0", {{"macsec", "profile"}});
    sessionTable.set("Ethernet0", {{"query_status", "ok"}});
    sessionTable.set("Ethernet4", {{"query_status", "ok"}});
    participantTable.set("Ethernet0|" + PRIMARY_CKN, {{"active", "true"}});
    participantTable.set("Ethernet4|" + PRIMARY_CKN, {{"active", "true"}});

    MACsecMgr manager(
        configDb.get(),
        stateDb.get(),
        {CFG_MACSEC_PROFILE_TABLE_NAME, CFG_PORT_TABLE_NAME});
    manager.reconcileStartupState();

    vector<FieldValueTuple> values;
    string queryStatus;
    EXPECT_TRUE(sessionTable.get("Ethernet0", values));
    EXPECT_TRUE(getField(sessionTable, "Ethernet0", "query_status", queryStatus));
    EXPECT_EQ("error", queryStatus);
    EXPECT_TRUE(participantTable.get("Ethernet0|" + PRIMARY_CKN, values));
    EXPECT_FALSE(sessionTable.get("Ethernet4", values));
    EXPECT_FALSE(participantTable.get("Ethernet4|" + PRIMARY_CKN, values));
}

TEST_F(MACsecMgrTest, SecuredSessionPrimaryRolloverRemovesBeforeAdding)
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
}

TEST_F(MACsecMgrTest, AuthenticatedButUnsecuredSessionFailsRolloverPreflight)
{
    auto &mkaSession = session();
    auto desired = profile(NEW_PRIMARY_CKN);
    manager->m_profiles["profile"] = desired;
    commandState.authenticated = true;
    commandState.secured = false;

    EXPECT_FALSE(manager->reconcilePort("Ethernet0", mkaSession, desired));
    EXPECT_EQ(0, commandState.addCalls);
    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(none_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_del_mka") != string::npos;
        }));
}

TEST_F(MACsecMgrTest, UnsafeFirstPortDoesNotBlockLaterSafePort)
{
    auto &unsafeSession = session("Ethernet0");
    auto &safeSession = session("Ethernet4");
    commandState.failQueryPorts.insert("Ethernet0");

    EXPECT_EQ(
        task_need_retry,
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
    string configStatus;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", configStatus));
    EXPECT_EQ("degraded", configStatus);
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

TEST_F(MACsecMgrTest, DeferredPortRecoversWithoutReapplyingCompletedPort)
{
    auto &deferredSession = session("Ethernet0");
    auto &completedSession = session("Ethernet4");
    commandState.failQueryPorts.insert("Ethernet0");

    EXPECT_EQ(
        task_need_retry,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));
    ASSERT_EQ(1, commandState.addCalls);
    ASSERT_EQ(NEW_PRIMARY_CKN, completedSession.applied_profile.primary_ckn);

    commandState.failQueryPorts.clear();
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

TEST_F(MACsecMgrTest, PrimaryOnlyDesiredChangeIsCachedButRuntimeMutationIsDeferred)
{
    auto &mkaSession = session();
    mkaSession.applied_profile = primaryOnlyProfile();
    manager->m_profiles["profile"] = mkaSession.applied_profile;
    commandState.fallbackCknByPort["Ethernet0"].clear();

    EXPECT_EQ(
        task_need_retry,
        manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(NEW_PRIMARY_CKN, manager->m_profiles["profile"].primary_ckn);
    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(any_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_mka_list") != string::npos;
        }));
    EXPECT_TRUE(none_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_del_mka") != string::npos ||
                   cmd.find("macsec_add_mka") != string::npos;
        }));

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string configStatus;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", configStatus));
    EXPECT_EQ("degraded", configStatus);
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

TEST_F(MACsecMgrTest, PotentialPeerPreventsNoPeerBypass)
{
    auto &mkaSession = session();
    mkaSession.applied_profile = primaryOnlyProfile();
    manager->m_profiles["profile"] = mkaSession.applied_profile;
    commandState.fallbackCknByPort["Ethernet0"].clear();
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.peerCountsByPort["Ethernet0"].primaryPotential = 1;

    EXPECT_EQ(
        task_need_retry,
        manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(none_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_del_mka") != string::npos ||
                   cmd.find("macsec_add_mka") != string::npos;
        }));
}

TEST_F(MACsecMgrTest, QueryFailureDoesNotTriggerNoPeerBypass)
{
    auto &mkaSession = session();
    mkaSession.applied_profile = primaryOnlyProfile();
    manager->m_profiles["profile"] = mkaSession.applied_profile;
    commandState.fallbackCknByPort["Ethernet0"].clear();
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.failQueryPorts.insert("Ethernet0");

    EXPECT_EQ(
        task_need_retry,
        manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(none_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_del_mka") != string::npos ||
                   cmd.find("macsec_add_mka") != string::npos;
        }));
}

TEST_F(MACsecMgrTest, PeerAppearingOnImmediateRecheckPreventsRemoval)
{
    auto &mkaSession = session();
    mkaSession.applied_profile = primaryOnlyProfile();
    manager->m_profiles["profile"] = mkaSession.applied_profile;
    commandState.fallbackCknByPort["Ethernet0"].clear();
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.peersAppearOnSecondQueryPorts.insert("Ethernet0");

    EXPECT_EQ(
        task_need_retry,
        manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(2u, commandState.queryCountByPort["Ethernet0"]);
    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(none_of(
        mockCallArgs.begin(),
        mockCallArgs.end(),
        [](const string &cmd) {
            return cmd.find("macsec_del_mka") != string::npos ||
                   cmd.find("macsec_add_mka") != string::npos;
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
    EXPECT_GE(commandState.queryCountByPort["Ethernet0"], 6u);
}

TEST_F(MACsecMgrTest, PeerlessRemoveFailureDoesNotClaimSuccess)
{
    auto &mkaSession = session();
    mkaSession.applied_profile = primaryOnlyProfile();
    manager->m_profiles["profile"] = mkaSession.applied_profile;
    commandState.fallbackCknByPort["Ethernet0"].clear();
    commandState.peerCountsByPort["Ethernet0"].primaryLive = 0;
    commandState.failRemove = true;

    EXPECT_EQ(
        task_need_retry,
        manager->loadProfile("profile", primaryOnlyProfileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
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
    EXPECT_EQ(PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
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

    EXPECT_EQ(
        task_need_retry,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));

    EXPECT_TRUE(mkaSession.applied_profile.primary_ckn.empty());
    EXPECT_EQ(PRIMARY_CKN, mkaSession.pending_old_ckn);
    EXPECT_TRUE(mkaSession.pending_primary);
    EXPECT_EQ(1, commandState.addCalls);

    Table sessionTable(stateDb.get(), STATE_MACSEC_MKA_SESSION_TABLE_NAME);
    string value;
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "query_status", value));
    EXPECT_EQ("error", value);
    ASSERT_TRUE(getField(sessionTable, "Ethernet0", "config_status", value));
    EXPECT_EQ("degraded", value);

    EXPECT_EQ(
        task_success,
        manager->loadProfile("profile", profileArgs(NEW_PRIMARY_CKN)));

    EXPECT_EQ(NEW_PRIMARY_CKN, mkaSession.applied_profile.primary_ckn);
    EXPECT_TRUE(mkaSession.pending_old_ckn.empty());
    EXPECT_EQ(1, commandState.addCalls);
}

TEST_F(MACsecMgrTest, MissingReplacementAfterAddRemainsPendingUntilRetryObservation)
{
    auto &mkaSession = session();
    commandState.omitFirstPostAddReplacementPorts.insert("Ethernet0");

    EXPECT_EQ(
        task_need_retry,
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

}
