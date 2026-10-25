#include "gtest/gtest.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <set>
#include <sstream>
#include <iostream>
#include <unistd.h>
#include <sys/stat.h>
#include "../mock_table.h"
#include "shellcmd.h"
#include "macaddress.h"
#define private public
#include "warm_restart.h"
#include "intfmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;
extern swss::MacAddress gMacAddress;
extern swss::MacAddress gSagMacAddress;

bool Ethernet0IPv6Set = false;
bool FailBridgeFdbCommand = false;
bool FailAddressAdd = false;
bool FailIpv6Enable = false;
bool FailAddressDelete = false;
int AddressQueryResult = 0;
bool ThrowAddressQuery = false;
std::string AddressQueryOutput;
unsigned int InterfaceIndex = 1;
int InterfaceIndexError = 0;
std::vector<std::string> interfaceLookups;
std::set<std::string> MissingSubInterfaces;
bool FailSubIntfCreation = false;
std::map<std::string, std::pair<std::string, std::string>> KernelSubIntfs;
std::map<std::string, std::string> SubIntfQueryOverrides;
int SubIntfQueryResult = 0;
bool ThrowSubIntfQuery = false;
bool FailSubIntfMtu = false;
bool FailSubIntfAdmin = false;
bool ThrowAdminOperation = false;
int AdminQueryResult = 0;
bool ThrowAdminQuery = false;
bool ThrowAdminQueryInvalidArgument = false;
std::string AdminQueryOutput;
int IntfIpCount = 0;

static std::string quotedArgument(const std::string &cmd, const std::string &marker)
{
    auto start = cmd.find(marker + "\"");
    if (start == std::string::npos)
        return "";
    start += marker.size() + 1;
    return cmd.substr(start, cmd.find('"', start) - start);
}

static std::string subIntfLine(const std::string &alias, const std::string &parent,
                               const std::string &vlan)
{
    return "7: " + alias + "@" + parent + ": <BROADCAST,UP> mtu 1500\\    "
           "link/ether 02:03:04:05:06:07\\    vlan protocol 802.1Q id " + vlan + " <REORDER_HDR>\n";
}

extern "C" unsigned int __wrap_if_nametoindex(const char *ifname)
{
    interfaceLookups.push_back(ifname);
    if (MissingSubInterfaces.count(ifname))
    {
        errno = ENODEV;
        return 0;
    }
    errno = InterfaceIndexError;
    return InterfaceIndex;
}

int cb(const std::string &cmd, std::string &stdout){
    mockCallArgs.push_back(cmd);
    if (cmd.find("/sbin/ip link add link ") == 0)
    {
        auto alias = quotedArgument(cmd, "name ");
        if (FailSubIntfCreation || (KernelSubIntfs.count(alias) && !MissingSubInterfaces.count(alias)))
            return 1;
        MissingSubInterfaces.erase(alias);
        KernelSubIntfs[alias] = {quotedArgument(cmd, "link add link "), quotedArgument(cmd, "id ")};
        return 0;
    }
    if (cmd.find("/sbin/ip link del ") == 0)
    {
        auto alias = quotedArgument(cmd, "link del ");
        if (MissingSubInterfaces.count(alias) || !KernelSubIntfs.count(alias))
            return 1;
        KernelSubIntfs.erase(alias);
        MissingSubInterfaces.insert(alias);
        return 0;
    }
    if (cmd.find("/sbin/ip -d -o link show dev ") == 0)
    {
        auto alias = quotedArgument(cmd, "dev ");
        if (ThrowSubIntfQuery)
            throw std::runtime_error("link query unavailable");
        if (SubIntfQueryResult)
            return SubIntfQueryResult;
        if (MissingSubInterfaces.count(alias))
            return 1;
        if (SubIntfQueryOverrides.count(alias))
        {
            stdout = SubIntfQueryOverrides.at(alias);
            return 0;
        }
        if (!KernelSubIntfs.count(alias))
            return 1;
        stdout = subIntfLine(alias, KernelSubIntfs.at(alias).first, KernelSubIntfs.at(alias).second);
        return 0;
    }
    if (cmd.find("/sbin/ip -o link show dev ") == 0)
    {
        if (ThrowAdminQueryInvalidArgument)
            throw std::invalid_argument("invalid admin query");
        if (ThrowAdminQuery)
            throw std::runtime_error("admin query unavailable");
        const auto alias = quotedArgument(cmd, "dev ");
        stdout = AdminQueryOutput;
        return MissingSubInterfaces.count(alias) ? 1 : AdminQueryResult;
    }
    if (cmd.find("/sbin/ip -o address show dev ") == 0)
    {
        if (ThrowAddressQuery)
            throw std::runtime_error("address query unavailable");
        const auto alias = quotedArgument(cmd, "dev ");
        stdout = AddressQueryOutput;
        return MissingSubInterfaces.count(alias) ? 1 : AddressQueryResult;
    }
    if (ThrowAdminOperation && cmd.find("/sbin/ip link set ") == 0 &&
        (cmd.find("\"up\"") != std::string::npos || cmd.find("\"down\"") != std::string::npos))
        throw std::runtime_error("admin operation unavailable");
    for (const auto &entry : KernelSubIntfs)
    {
        if (cmd.find("/sbin/ip link set \"" + entry.first + "\"") == 0 &&
            ((FailSubIntfMtu && cmd.find(" mtu ") != std::string::npos) ||
             (FailSubIntfAdmin && (cmd.find("\"up\"") != std::string::npos ||
                                  cmd.find("\"down\"") != std::string::npos))))
            return 1;
    }
    for (const auto &alias : MissingSubInterfaces)
    {
        if (cmd.find("/sbin/ip link set \"" + alias + "\"") == 0 ||
            cmd.find(" dev \"" + alias + "\"") != std::string::npos)
            return 1;
    }
    if (cmd == "sysctl -w net.ipv6.conf.\"Ethernet0\".disable_ipv6=0" ||
        cmd == "sysctl -w net.ipv6.conf.\"PortChannel1\".disable_ipv6=0")
    {
        if (FailIpv6Enable)
        {
            return 1;
        }
        Ethernet0IPv6Set = true;
        return 0;
    }
    else if (cmd.find("/sbin/ip -6 address \"add\"") == 0) {
        return Ethernet0IPv6Set && !FailAddressAdd ? 0 : 2;
    }
    else if (cmd.find("/sbin/ip address \"add\"") == 0) {
        return FailAddressAdd ? 2 : 0;
    }
    else if (cmd.find("/sbin/ip address \"del\"") == 0 ||
             cmd.find("/sbin/ip -6 address \"del\"") == 0) {
        return FailAddressDelete ? 2 : 0;
    }
    else if (cmd == "/sbin/ip link set \"Ethernet64.10\" \"up\""){
        return 1;
    }
    else if (cmd.find("/sbin/ip address show ") == 0) {
        stdout = std::to_string(IntfIpCount) + "\n";
        return 0;
    }
    else if (cmd.find("bridge fdb") == 0) {
        return FailBridgeFdbCommand ? 1 : 0;
    }
    else {
        return 0;
    }
    return 0;
}

// Test Fixture
namespace intfmgr_ut
{
    struct IntfMgrTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;
        std::vector<std::string> cfg_intf_tables;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);

            swss::WarmStart::initialize("intfmgrd", "swss");
            swss::WarmStart::checkWarmStart("intfmgrd", "swss");

            std::vector<std::string> tables = {
                CFG_INTF_TABLE_NAME,
                CFG_LAG_INTF_TABLE_NAME,
                CFG_VLAN_INTF_TABLE_NAME,
                CFG_LOOPBACK_INTERFACE_TABLE_NAME,
                CFG_VLAN_SUB_INTF_TABLE_NAME,
                CFG_VOQ_INBAND_INTERFACE_TABLE_NAME,
            };
            cfg_intf_tables = tables;
            mockCallArgs.clear();
            callback = cb;
            Ethernet0IPv6Set = false;
            FailBridgeFdbCommand = false;
            FailAddressAdd = false;
            FailIpv6Enable = false;
            FailAddressDelete = false;
            AddressQueryResult = 0;
            ThrowAddressQuery = false;
            AddressQueryOutput.clear();
            InterfaceIndex = 1;
            InterfaceIndexError = 0;
            interfaceLookups.clear();
            MissingSubInterfaces.clear();
            FailSubIntfCreation = false;
            KernelSubIntfs.clear();
            SubIntfQueryOverrides.clear();
            SubIntfQueryResult = 0;
            ThrowSubIntfQuery = false;
            FailSubIntfMtu = FailSubIntfAdmin = false;
            ThrowAdminOperation = false;
            AdminQueryResult = 0;
            ThrowAdminQuery = false;
            ThrowAdminQueryInvalidArgument = false;
            AdminQueryOutput.clear();
            IntfIpCount = 0;
        }

        void TearDown() override
        {
            swss::Table enable(m_state_db.get(), "WARM_RESTART_ENABLE_TABLE");
            enable.hset("swss", "enable", "false");
            // Production checks once per process; isolate its latched flags between test cases.
            swss::WarmStart::getInstance().m_enabled = false;
            swss::WarmStart::getInstance().m_systemWarmRebootEnabled = false;
            EXPECT_FALSE(swss::WarmStart::isWarmStart());
        }

        void enableWarmStart()
        {
            swss::Table enable(m_state_db.get(), "WARM_RESTART_ENABLE_TABLE");
            enable.hset("swss", "enable", "true");
            swss::Table state(m_state_db.get(), STATE_WARM_RESTART_TABLE_NAME);
            state.hset("intfmgrd", "restore_count", "0");
            swss::WarmStart::checkWarmStart("intfmgrd", "swss");
            swss::WarmStart::setWarmStartState("intfmgrd", swss::WarmStart::INITIALIZED);
            ASSERT_TRUE(swss::WarmStart::isWarmStart());
        }

        swss::WarmStart::WarmStartState warmState()
        {
            swss::WarmStart::WarmStartState state;
            swss::WarmStart::getWarmStartState("intfmgrd", state);
            return state;
        }

        void verifyFailedAddressProgrammingRemainsPending(const std::string &prefix,
                                                          bool failIpv6Enable = false)
        {
            swss::IntfMgr intfmgr(
                m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
            const std::string alias = "PortChannel1";
            const std::string key = alias + "|" + prefix;
            const std::string appKey = alias + ":" + prefix;

            notifyLag(intfmgr, alias, SET_COMMAND, {{"state", "ok"}});
            intfmgr.m_stateIntfTable.set(alias, {{"vrf", ""}}, "SET", "");

            auto *consumer = dynamic_cast<Consumer *>(
                intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
            ASSERT_NE(consumer, nullptr);
            consumer->m_toSync.emplace(
                key, swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, {}));

            testing_db::resetOperationCounters();
            FailAddressAdd = true;
            FailIpv6Enable = failIpv6Enable;
            intfmgr.doTask(*consumer);

            swss::Table appIntfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
            std::vector<swss::FieldValueTuple> values;
            EXPECT_EQ(consumer->m_toSync.count(key), 1u);
            EXPECT_FALSE(appIntfTable.get(appKey, values));
            EXPECT_FALSE(intfmgr.m_stateIntfTable.get(key, values));
            EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 0u);

            FailAddressAdd = false;
            FailIpv6Enable = false;
            intfmgr.doTask(*consumer);

            EXPECT_TRUE(consumer->m_toSync.empty());
            EXPECT_TRUE(appIntfTable.get(appKey, values));
            std::string state;
            ASSERT_TRUE(intfmgr.m_stateIntfTable.hget(key, "state", state));
            EXPECT_EQ(state, "ok");
            EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 1u);

            const auto commandCount = mockCallArgs.size();
            intfmgr.doTask(*consumer);
            EXPECT_EQ(mockCallArgs.size(), commandCount);
            EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 1u);
        }

        void notifyLag(swss::IntfMgr &intfmgr, const std::string &alias,
                       const std::string &op, const std::vector<swss::FieldValueTuple> &data = {})
        {
            if (op == DEL_COMMAND)
                intfmgr.m_stateLagTable.del(alias);
            else
                intfmgr.m_stateLagTable.set(alias, data);
            auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(STATE_LAG_TABLE_NAME));
            ASSERT_NE(consumer, nullptr);
            consumer->addToSync(swss::KeyOpFieldsValuesTuple(alias, op, data));
            intfmgr.doTask(*consumer);
        }

        Consumer *applyChild(swss::IntfMgr &intfmgr, const std::string &alias = "PortChannel1.20",
                             const std::string &prefix = "192.0.2.1/31")
        {
            swss::Table config(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
            config.set(alias, {{"mtu", "1500"}, {"admin_status", "up"}});
            if (!prefix.empty())
                config.set(alias + "|" + prefix, {{"NULL", "NULL"}});
            notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                      {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
            auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_VLAN_SUB_INTF_TABLE_NAME));
            if (!consumer)
                throw std::runtime_error("Missing subinterface consumer");
            consumer->refillToSync(&config);
            intfmgr.doTask(*consumer);
            return consumer;
        }
    };

    static bool commandWasIssued(const std::string &needle)
    {
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    static bool getFieldValue(const std::vector<swss::FieldValueTuple> &values,
                              const std::string &field,
                              std::string &value)
    {
        for (const auto &fv : values)
        {
            if (fvField(fv) == field)
            {
                value = fvValue(fv);
                return true;
            }
        }
        return false;
    }

    static std::string addressLine(const std::string &prefix)
    {
        return "7: PortChannel1    " + std::string(swss::IpPrefix(prefix).isV4() ? "inet " : "inet6 ") +
               prefix + " scope global PortChannel1\\       valid_lft forever preferred_lft forever\n";
    }

    static std::map<std::string, std::vector<swss::FieldValueTuple>> tableSnapshot(swss::Table &table)
    {
        std::map<std::string, std::vector<swss::FieldValueTuple>> result;
        std::vector<std::string> keys;
        table.getKeys(keys);
        for (const auto &key : keys)
            table.get(key, result[key]);
        return result;
    }

    TEST_F(IntfMgrTest, testAddressCommandSuccessDoesNotQuery)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        Ethernet0IPv6Set = true;
        AddressQueryResult = 1;
        InterfaceIndex = 0;
        InterfaceIndexError = ENODEV;
        mockCallArgs.clear();
        for (const std::string prefix : {"192.0.2.1/31", "2001:db8::1/64"})
        {
            EXPECT_TRUE(intfmgr.setIntfIp("PortChannel1", "add", swss::IpPrefix(prefix)));
            EXPECT_TRUE(intfmgr.setIntfIp("PortChannel1", "del", swss::IpPrefix(prefix)));
        }
        EXPECT_EQ(mockCallArgs.size(), 4u);
        EXPECT_FALSE(commandWasIssued(" address show "));
        EXPECT_TRUE(interfaceLookups.empty());
    }

    TEST_F(IntfMgrTest, testFailedAddressCommandsUseExactLocalPrefix)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        FailAddressAdd = FailAddressDelete = true;
        Ethernet0IPv6Set = true;
        struct TestCase
        {
            std::string desired;
            std::string output;
            bool present;
        };
        const std::vector<TestCase> cases = {
            {"192.0.2.1/31", addressLine("192.0.2.1/31"), true},
            {"192.0.2.1/31", addressLine("192.0.2.1/30"), false},
            {"192.0.2.1/31", addressLine("192.0.2.0/31"), false},
            {"192.0.2.1/31", addressLine("::ffff:192.0.2.1/127"), false},
            {"192.0.2.1/31", "", false},
            {"192.0.2.1/31", addressLine("192.0.2.3/31") + addressLine("192.0.2.1/31"), true},
            {"192.0.2.1/31", "7: PortChannel1 inet 192.0.2.0 peer 192.0.2.1/31 scope global\n", false},
            {"2001:db8::1/64", addressLine("2001:db8::1/64"), true},
            {"2001:0db8:0000:0000:0000:0000:0000:0001/64", addressLine("2001:db8::1/64"), true},
            {"2001:db8::1/64", addressLine("2001:db8::1/128"), false},
            {"2001:db8::1/64", addressLine("2001:db8::2/64"), false},
            {"2001:db8::1/64", addressLine("192.0.2.1/31"), false},
            {"2001:db8::1/64", "", false},
            {"2001:db8::1/64", addressLine("fe80::1/64") + addressLine("2001:db8::1/64"), true},
            {"2001:db8::1/64", "7: PortChannel1 inet6 2001:db8::2 peer 2001:db8::1/64 scope global\n", false}
        };
        for (const auto &test : cases)
        {
            SCOPED_TRACE(test.desired + " output: " + test.output);
            AddressQueryOutput = test.output;
            mockCallArgs.clear();
            EXPECT_EQ(intfmgr.setIntfIp("PortChannel1", "add", swss::IpPrefix(test.desired)), test.present);
            EXPECT_EQ(intfmgr.setIntfIp("PortChannel1", "del", swss::IpPrefix(test.desired)), !test.present);
            EXPECT_EQ(std::count(mockCallArgs.begin(), mockCallArgs.end(),
                                 "/sbin/ip -o address show dev \"PortChannel1\""), 2);
        }
        EXPECT_TRUE(interfaceLookups.empty());
    }

    TEST_F(IntfMgrTest, testFailedAddressCommandsRecognizeMissingInterface)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        FailAddressAdd = FailAddressDelete = true;
        AddressQueryResult = 1;
        for (const std::string prefix : {"192.0.2.1/31", "2001:db8::1/64"})
        {
            SCOPED_TRACE(prefix);
            EXPECT_FALSE(intfmgr.setIntfIp("PortChannel1", "add", swss::IpPrefix(prefix)));
            EXPECT_TRUE(intfmgr.setIntfIp("PortChannel1", "del", swss::IpPrefix(prefix)));
        }
        EXPECT_TRUE(interfaceLookups.empty());
    }

    TEST_F(IntfMgrTest, testFailedAddressQueryReturnsNotPresent)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        FailAddressAdd = FailAddressDelete = true;
        AddressQueryResult = 1;
        for (bool throws : {false, true})
        {
            ThrowAddressQuery = throws;
            for (const std::string prefix : {"192.0.2.1/31", "2001:db8::1/64"})
            {
                SCOPED_TRACE(prefix);
                AddressQueryOutput = addressLine(prefix);
                EXPECT_FALSE(intfmgr.isIntfIpPresent("PortChannel1", swss::IpPrefix(prefix)));
                EXPECT_FALSE(intfmgr.setIntfIp("PortChannel1", "add", swss::IpPrefix(prefix)));
                EXPECT_TRUE(intfmgr.setIntfIp("PortChannel1", "del", swss::IpPrefix(prefix)));
            }
        }
        EXPECT_TRUE(interfaceLookups.empty());
    }

    TEST_F(IntfMgrTest, testUnmatchedAddressOutputReturnsNotPresent)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        FailAddressAdd = FailAddressDelete = true;
        for (const std::string output : {
                "invalid\n",
                "7: PortChannel1 inet invalid/31\n",
                "7: PortChannel1 inet6 192.0.2.1/31\n",
                "7: PortChannel1 inet 192.0.2.1 scope global\n",
                "7: PortChannel1 inet 192.0.2.1 peer invalid/31\n",
                "7: PortChannel1 inet 192.0.2.1 peer 192.0.2.0\n"})
        {
            SCOPED_TRACE(output);
            AddressQueryOutput = output;
            EXPECT_FALSE(intfmgr.setIntfIp("PortChannel1", "add", swss::IpPrefix("192.0.2.1/31")));
            EXPECT_TRUE(intfmgr.setIntfIp("PortChannel1", "del", swss::IpPrefix("192.0.2.1/31")));
        }
    }

    TEST_F(IntfMgrTest, testAlreadyPresentAddressIsPublishedAndConsumedOnce)
    {
        for (const std::string prefix : {"192.0.2.1/31", "2001:db8::1/64"})
        {
            SCOPED_TRACE(prefix);
            swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
            notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
            intfmgr.m_stateIntfTable.set("PortChannel1", {{"vrf", ""}});
            auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
            ASSERT_NE(consumer, nullptr);
            const auto key = "PortChannel1|" + prefix;
            consumer->addToSync(swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, {}));
            FailAddressAdd = true;
            AddressQueryOutput = addressLine(prefix);
            testing_db::resetOperationCounters();
            intfmgr.doTask(*consumer);
            EXPECT_TRUE(consumer->m_toSync.empty());
            swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
            std::vector<swss::FieldValueTuple> values;
            EXPECT_TRUE(app.get("PortChannel1:" + prefix, values));
            EXPECT_TRUE(intfmgr.m_stateIntfTable.get(key, values));
            EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 1u);
            const auto commands = mockCallArgs.size();
            intfmgr.doTask(*consumer);
            EXPECT_EQ(mockCallArgs.size(), commands);
            EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 1u);
        }
    }

    TEST_F(IntfMgrTest, testWrongPrefixAfterFailedAddRemainsPending)
    {
        AddressQueryOutput = addressLine("192.0.2.1/30");
        verifyFailedAddressProgrammingRemainsPending("192.0.2.1/31");
        AddressQueryOutput = addressLine("2001:db8::1/128");
        verifyFailedAddressProgrammingRemainsPending("2001:db8::1/64");
    }

    TEST_F(IntfMgrTest, testIpv6FinalStateAfterEnableAndRetry)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        FailAddressAdd = true;
        for (bool enableFails : {false, true})
        {
            FailIpv6Enable = enableFails;
            for (bool present : {false, true})
            {
                SCOPED_TRACE("enable failure: " + std::to_string(enableFails) +
                             " address present: " + std::to_string(present));
                Ethernet0IPv6Set = false;
                AddressQueryOutput = present ? addressLine("2001:db8::1/64") : "";
                mockCallArgs.clear();
                EXPECT_EQ(intfmgr.setIntfIp("PortChannel1", "add", swss::IpPrefix("2001:db8::1/64")), present);
                ASSERT_EQ(mockCallArgs.size(), enableFails ? 3u : 4u);
                EXPECT_EQ(mockCallArgs[1], "sysctl -w net.ipv6.conf.\"PortChannel1\".disable_ipv6=0");
                EXPECT_EQ(mockCallArgs.back(), "/sbin/ip -o address show dev \"PortChannel1\"");
            }
        }
        FailAddressAdd = FailIpv6Enable = Ethernet0IPv6Set = false;
        AddressQueryResult = 1;
        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.setIntfIp("PortChannel1", "add", swss::IpPrefix("2001:db8::1/64")));
        EXPECT_EQ(mockCallArgs.size(), 3u);
        EXPECT_FALSE(commandWasIssued(" address show "));
        EXPECT_TRUE(interfaceLookups.empty());
    }

    TEST_F(IntfMgrTest, testConfigDeleteRemainsBestEffortBeforeFollowingSet)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
        intfmgr.m_stateIntfTable.set("PortChannel1", {{"vrf", ""}});
        intfmgr.m_stateIntfTable.set("PortChannel1|192.0.2.1/31", {{"state", "ok"}});
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        FailAddressDelete = true;
        AddressQueryOutput = addressLine("192.0.2.1/31");
        EXPECT_FALSE(intfmgr.setIntfIp("PortChannel1", "del", swss::IpPrefix("192.0.2.1/31")));
        consumer->addToSync(swss::KeyOpFieldsValuesTuple("PortChannel1|192.0.2.1/31", DEL_COMMAND, {}));
        consumer->addToSync(swss::KeyOpFieldsValuesTuple("PortChannel1|192.0.2.1/31", SET_COMMAND, {}));
        ASSERT_EQ(consumer->m_toSync.size(), 2u);
        testing_db::resetOperationCounters();
        mockCallArgs.clear();
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(mockCallArgs.size(), 3u);
        EXPECT_EQ(mockCallArgs[0], "/sbin/ip address \"del\" \"192.0.2.1/31\" dev \"PortChannel1\"");
        EXPECT_EQ(mockCallArgs[1], "/sbin/ip -o address show dev \"PortChannel1\"");
        EXPECT_EQ(mockCallArgs[2], "/sbin/ip address \"add\" \"192.0.2.1/31\" dev \"PortChannel1\"");
        EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 1u);
        EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 1u);
    }

    TEST_F(IntfMgrTest, testLagReopenAlreadyPresentAddressIsConsumedOnce)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        intfmgr.m_cfgLagIntfTable.set("PortChannel1", {{"NULL", "NULL"}});
        intfmgr.m_cfgLagIntfTable.set("PortChannel1|192.0.2.1/31", {{"NULL", "NULL"}});
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        FailAddressAdd = true;
        AddressQueryOutput = addressLine("192.0.2.1/31");
        for (int interval = 0; interval < 2; ++interval)
        {
            testing_db::resetOperationCounters();
            notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
            intfmgr.doTask(*consumer);
            EXPECT_TRUE(consumer->m_toSync.empty());
            EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 2u);
            mockCallArgs.clear();
            notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
            intfmgr.doTask(*consumer);
            EXPECT_TRUE(mockCallArgs.empty());
            EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 2u);
            notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
            EXPECT_TRUE(mockCallArgs.empty());
            EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 0u);
        }
    }

    TEST_F(IntfMgrTest, testLagIntfConfigRequiredUsesCurrentConfig)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        intfmgr.m_stateIntfTable.set("PortChannel1", {{"vrf", ""}});
        EXPECT_FALSE(intfmgr.isLagIntfConfigRequired("PortChannel1"));
        for (auto *table : {&intfmgr.m_cfgLagIntfTable, &intfmgr.m_cfgSubIntfTable})
        {
            const std::string alias = table == &intfmgr.m_cfgLagIntfTable ?
                "PortChannel1" : "PortChannel1.20";
            for (const auto &key : {alias, alias + "|192.0.2.1/31"})
            {
                EXPECT_FALSE(intfmgr.isLagIntfConfigRequired(key));
                table->set(key, {{"NULL", "NULL"}});
                EXPECT_TRUE(intfmgr.isLagIntfConfigRequired(key));
                table->del(key);
                EXPECT_FALSE(intfmgr.isLagIntfConfigRequired(key));
            }
        }
    }

    TEST_F(IntfMgrTest, testLagReadinessUsesRowPresence)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        swss::Table appIntfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        const std::string alias = "PortChannel1";
        const std::string key = alias + "|10.0.0.1/31";
        intfmgr.m_cfgLagIntfTable.set(alias, {{"NULL", "NULL"}});
        intfmgr.m_cfgLagIntfTable.set(key, {{"NULL", "NULL"}});

        EXPECT_FALSE(intfmgr.isIntfStateOk(alias));
        notifyLag(intfmgr, alias, SET_COMMAND, {{"system_mac", "02:03:04:05:06:07"}});
        EXPECT_TRUE(intfmgr.isIntfStateOk(alias));
        ASSERT_EQ(consumer->m_toSync.size(), 2u);
        EXPECT_EQ(consumer->m_toSync.begin()->first, alias);
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        std::vector<swss::FieldValueTuple> values;
        EXPECT_TRUE(appIntfTable.get(alias, values));
        EXPECT_TRUE(appIntfTable.get(alias + ":10.0.0.1/31", values));
        EXPECT_TRUE(intfmgr.m_stateIntfTable.get(alias, values));
        EXPECT_TRUE(intfmgr.m_stateIntfTable.get(key, values));

        notifyLag(intfmgr, alias, SET_COMMAND, {{"state", "pending"}});
        EXPECT_TRUE(intfmgr.isIntfStateOk(alias));
        EXPECT_TRUE(consumer->m_toSync.empty());
        notifyLag(intfmgr, alias, DEL_COMMAND);
        EXPECT_FALSE(intfmgr.isIntfStateOk(alias));

        for (const std::string childAlias : {"PortChannel1.20", "Po1.20"})
        {
            EXPECT_FALSE(intfmgr.isIntfStateOk(childAlias));
            intfmgr.m_stateLagTable.set(childAlias, {{"metadata", "present"}});
            EXPECT_TRUE(intfmgr.isIntfStateOk(childAlias));
            intfmgr.m_stateLagTable.del(childAlias);
            EXPECT_FALSE(intfmgr.isIntfStateOk(childAlias));
        }
    }

    TEST_F(IntfMgrTest, testLagDeleteCancelsSetsAndWithdrawsReadiness)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        swss::Table appIntfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
        for (const std::string key : {"PortChannel1", "PortChannel1|10.0.0.1/31",
                                     "PortChannel10", "PortChannel10|10.0.0.3/31"})
        {
            consumer->addToSync(swss::KeyOpFieldsValuesTuple(key, DEL_COMMAND, {}));
            consumer->addToSync(swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, {{"nat_zone", "1"}}));
            intfmgr.m_pendingReplayIntfList.insert(key);
            intfmgr.m_cfgLagIntfTable.set(key, {{"NULL", "NULL"}});
            intfmgr.m_stateIntfTable.set(key, key.find('|') == std::string::npos ?
                std::vector<swss::FieldValueTuple>{{"vrf", "VrfRed"}} :
                std::vector<swss::FieldValueTuple>{{"state", "ok"}, {"owner", "retained"}});
            auto appKey = key;
            auto delimiter = appKey.find('|');
            if (delimiter != std::string::npos)
                appKey.replace(delimiter, 1, ":");
            appIntfTable.set(appKey, {{"scope", "global"}});
        }
        appIntfTable.set("PortChannel1:2001:db8::1/64", {{"family", "IPv6"}});
        intfmgr.m_stateIntfTable.set("PortChannel1|2001:db8::2/64", {{"state", "ok"}});
        intfmgr.m_intfLLAddresses["PortChannel1"].insert("fe80::1/64");
        intfmgr.m_intfLLAddresses["PortChannel10"].insert("fe80::10/64");
        intfmgr.m_ipv6LinkLocalModeList.insert("PortChannel1");
        intfmgr.m_ipv6LinkLocalModeList.insert("PortChannel10");

        mockCallArgs.clear();
        testing_db::resetOperationCounters();
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        EXPECT_TRUE(mockCallArgs.empty());
        ASSERT_EQ(consumer->m_toSync.count("PortChannel1"), 1u);
        EXPECT_EQ(consumer->m_toSync.count("PortChannel1|10.0.0.1/31"), 1u);
        EXPECT_EQ(kfvOp(consumer->m_toSync.find("PortChannel1")->second), DEL_COMMAND);
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList.count("PortChannel1"), 1u);
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList.count("PortChannel1|10.0.0.1/31"), 1u);
        EXPECT_EQ(intfmgr.m_intfLLAddresses.count("PortChannel1"), 1u);
        EXPECT_EQ(intfmgr.m_ipv6LinkLocalModeList.count("PortChannel1"), 1u);
        EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 0u);
        std::vector<swss::FieldValueTuple> values;
        for (const std::string key : {"PortChannel1", "PortChannel1:10.0.0.1/31",
                                     "PortChannel1:2001:db8::1/64"})
            EXPECT_TRUE(appIntfTable.get(key, values));
        EXPECT_FALSE(appIntfTable.get("PortChannel1:2001:db8::2/64", values));
        std::string value;
        ASSERT_TRUE(intfmgr.m_stateIntfTable.hget("PortChannel1", "vrf", value));
        EXPECT_EQ(value, "VrfRed");
        EXPECT_FALSE(intfmgr.m_stateIntfTable.hget("PortChannel1|10.0.0.1/31", "state", value));
        EXPECT_FALSE(intfmgr.m_stateIntfTable.get("PortChannel1|10.0.0.1/31", values));
        EXPECT_FALSE(intfmgr.m_stateIntfTable.get("PortChannel1|2001:db8::2/64", values));
        for (const std::string key : {"PortChannel10", "PortChannel10|10.0.0.3/31"})
        {
            EXPECT_EQ(consumer->m_toSync.count(key), 2u);
            EXPECT_EQ(intfmgr.m_pendingReplayIntfList.count(key), 1u);
            EXPECT_TRUE(intfmgr.m_stateIntfTable.get(key, values));
        }
        EXPECT_TRUE(appIntfTable.get("PortChannel10:10.0.0.3/31", values));
        EXPECT_EQ(intfmgr.m_intfLLAddresses.count("PortChannel10"), 1u);
        EXPECT_EQ(intfmgr.m_ipv6LinkLocalModeList.count("PortChannel10"), 1u);
        std::vector<std::string> configKeys;
        intfmgr.m_cfgLagIntfTable.getKeys(configKeys);
        EXPECT_EQ(configKeys.size(), 4u);
        EXPECT_TRUE(intfmgr.m_cfgLagIntfTable.get("PortChannel1", values));
        EXPECT_TRUE(intfmgr.m_cfgLagIntfTable.get("PortChannel1|10.0.0.1/31", values));
    }

    TEST_F(IntfMgrTest, testWarmInitialNotReadyRetainsObligationsUntilSuccess)
    {
        swss::Table config(m_config_db.get(), CFG_LAG_INTF_TABLE_NAME);
        config.set("PortChannel1", {{"NULL", "NULL"}});
        config.set("PortChannel1|192.0.2.1/31", {{"NULL", "NULL"}});
        enableWarmStart();
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->refillToSync(&config);
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList.size(), 2u);
        EXPECT_FALSE(intfmgr.m_replayDone);
        EXPECT_EQ(warmState(), swss::WarmStart::INITIALIZED);

        FailAddressAdd = true;
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
        intfmgr.doTask(*consumer);
        EXPECT_EQ(consumer->m_toSync.size(), 1u);
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList.size(), 1u);
        EXPECT_FALSE(intfmgr.m_replayDone);
        EXPECT_NE(warmState(), swss::WarmStart::RECONCILED);
        std::string value;
        EXPECT_FALSE(intfmgr.m_stateIntfTable.hget("PortChannel1|192.0.2.1/31", "state", value));

        FailAddressAdd = false;
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(intfmgr.m_pendingReplayIntfList.empty());
        EXPECT_TRUE(intfmgr.m_replayDone);
        EXPECT_EQ(warmState(), swss::WarmStart::RECONCILED);
    }

    TEST_F(IntfMgrTest, testWarmLagDelRetainsObligationsAcrossFailedReplay)
    {
        swss::Table config(m_config_db.get(), CFG_LAG_INTF_TABLE_NAME);
        swss::Table subConfig(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        config.set("PortChannel1", {{"NULL", "NULL"}});
        config.set("PortChannel1|192.0.2.1/31", {{"NULL", "NULL"}});
        subConfig.set("PortChannel1.20", {{"mtu", "1500"}});
        subConfig.set("PortChannel1.20|198.51.100.1/31", {{"NULL", "NULL"}});
        enableWarmStart();
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *parent = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        auto *child = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_VLAN_SUB_INTF_TABLE_NAME));
        ASSERT_NE(parent, nullptr);
        ASSERT_NE(child, nullptr);
        FailAddressAdd = true;
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        intfmgr.doTask(*parent);
        intfmgr.doTask(*child);
        ASSERT_EQ(intfmgr.m_pendingReplayIntfList.size(), 3u);
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        EXPECT_TRUE(parent->m_toSync.empty());
        EXPECT_TRUE(child->m_toSync.empty());
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList.size(), 3u);
        EXPECT_FALSE(intfmgr.m_replayDone);
        EXPECT_NE(warmState(), swss::WarmStart::RECONCILED);

        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        intfmgr.doTask(*parent);
        intfmgr.doTask(*child);
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList.size(), 3u);
        EXPECT_FALSE(intfmgr.m_replayDone);
        EXPECT_NE(warmState(), swss::WarmStart::RECONCILED);
        FailAddressAdd = false;
        intfmgr.doTask(*parent);
        intfmgr.doTask(*child);
        EXPECT_TRUE(parent->m_toSync.empty());
        EXPECT_TRUE(child->m_toSync.empty());
        EXPECT_TRUE(intfmgr.m_pendingReplayIntfList.empty());
        EXPECT_TRUE(intfmgr.m_replayDone);
        EXPECT_EQ(warmState(), swss::WarmStart::RECONCILED);
    }

    TEST_F(IntfMgrTest, testWarmReplayObligationsOnlyAddLagChildren)
    {
        swss::Table config(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        for (const std::string key : {"Ethernet0.20", "Po1.20", "PortChannel1.30"})
            config.set(key, {{"vlan", "20"}});
        enableWarmStart();
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList, (std::set<std::string>{"Po1.20", "PortChannel1.30"}));
        EXPECT_FALSE(intfmgr.m_replayDone);
        EXPECT_EQ(warmState(), swss::WarmStart::INITIALIZED);
    }

    TEST_F(IntfMgrTest, testWarmConfiguredChildWithL2ParentWaitsForReady)
    {
        swss::Table config(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        config.set("PortChannel1.20", {{"mtu", "1500"}});
        config.set("PortChannel1.20|192.0.2.1/31", {{"NULL", "NULL"}});
        enableWarmStart();
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *child = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_VLAN_SUB_INTF_TABLE_NAME));
        ASSERT_NE(child, nullptr);
        child->refillToSync(&config);
        intfmgr.doTask(*child);
        EXPECT_TRUE(child->m_toSync.empty());
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList.size(), 2u);
        EXPECT_FALSE(intfmgr.m_replayDone);
        EXPECT_NE(warmState(), swss::WarmStart::RECONCILED);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        intfmgr.doTask(*child);
        EXPECT_TRUE(child->m_toSync.empty());
        EXPECT_TRUE(intfmgr.m_pendingReplayIntfList.empty());
        EXPECT_EQ(warmState(), swss::WarmStart::RECONCILED);
    }

    TEST_F(IntfMgrTest, testWarmConfirmedConfigDeletionRetiresCancelledObligations)
    {
        swss::Table config(m_config_db.get(), CFG_LAG_INTF_TABLE_NAME);
        config.set("PortChannel1", {{"NULL", "NULL"}});
        config.set("PortChannel1|192.0.2.1/31", {{"NULL", "NULL"}});
        enableWarmStart();
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->refillToSync(&config);
        intfmgr.doTask(*consumer);
        ASSERT_EQ(intfmgr.m_pendingReplayIntfList.size(), 2u);
        config.del("PortChannel1|192.0.2.1/31");
        config.del("PortChannel1");
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(intfmgr.m_pendingReplayIntfList.empty());
        EXPECT_TRUE(intfmgr.m_replayDone);
        EXPECT_EQ(warmState(), swss::WarmStart::RECONCILED);
    }

    TEST_F(IntfMgrTest, testWarmDeletedConfigBeforeNotReadySetDoesNotBlock)
    {
        swss::Table config(m_config_db.get(), CFG_LAG_INTF_TABLE_NAME);
        config.set("PortChannel1", {{"NULL", "NULL"}});
        config.set("PortChannel1|192.0.2.1/31", {{"NULL", "NULL"}});
        enableWarmStart();
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->refillToSync(&config);
        ASSERT_EQ(intfmgr.m_pendingReplayIntfList.size(), 2u);
        config.del("PortChannel1|192.0.2.1/31");
        config.del("PortChannel1");
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(intfmgr.m_pendingReplayIntfList.empty());
        EXPECT_EQ(warmState(), swss::WarmStart::RECONCILED);
    }

    TEST_F(IntfMgrTest, testWarmConfigDelDrainsWhileParentClosed)
    {
        swss::Table config(m_config_db.get(), CFG_LAG_INTF_TABLE_NAME);
        config.set("PortChannel1", {{"NULL", "NULL"}});
        config.set("PortChannel1|192.0.2.1/31", {{"NULL", "NULL"}});
        enableWarmStart();
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        FailAddressAdd = true;
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
        intfmgr.doTask(*consumer);
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        EXPECT_FALSE(intfmgr.m_replayDone);
        for (const std::string key : {"PortChannel1|192.0.2.1/31", "PortChannel1"})
        {
            config.del(key);
            consumer->addToSync(swss::KeyOpFieldsValuesTuple(key, DEL_COMMAND, {}));
        }
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(intfmgr.m_pendingReplayIntfList.empty());
        EXPECT_TRUE(intfmgr.m_replayDone);
        EXPECT_EQ(warmState(), swss::WarmStart::RECONCILED);
        EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 2u);
    }

    struct IntfMgrWarmDeleteTest : IntfMgrTest, ::testing::WithParamInterface<bool>
    {
    };

    TEST_P(IntfMgrWarmDeleteTest, RetainedDeleteWaitsForReplacementSet)
    {
        const std::string table = GetParam() ? CFG_VLAN_SUB_INTF_TABLE_NAME : CFG_LAG_INTF_TABLE_NAME;
        const std::string alias = GetParam() ? "PortChannel1.20" : "PortChannel1";
        const std::string address = alias + "|192.0.2.1/31";
        const std::vector<swss::FieldValueTuple> data = {{"mtu", "1500"}, {"admin_status", "up"}};
        swss::Table config(m_config_db.get(), table);
        config.set(alias, data);
        config.set(address, {{"NULL", "NULL"}});
        enableWarmStart();
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(table));
        ASSERT_NE(consumer, nullptr);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        for (const auto &key : {alias, address})
        {
            consumer->addToSync(swss::KeyOpFieldsValuesTuple(key, DEL_COMMAND, {}));
            consumer->addToSync(swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, {}));
            ASSERT_EQ(consumer->m_toSync.count(key), 2u);
        }

        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        if (GetParam())
            MissingSubInterfaces.insert(alias);
        for (const auto &key : {alias, address})
        {
            ASSERT_EQ(consumer->m_toSync.count(key), 1u);
            EXPECT_EQ(kfvOp(consumer->m_toSync.find(key)->second), DEL_COMMAND);
        }
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList, (std::set<std::string>{alias, address}));
        EXPECT_FALSE(intfmgr.m_replayDone);
        EXPECT_EQ(warmState(), swss::WarmStart::INITIALIZED);

        FailAddressAdd = true;
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        intfmgr.doTask(*consumer);
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList, (std::set<std::string>{address}));
        EXPECT_FALSE(intfmgr.m_replayDone);
        EXPECT_EQ(warmState(), swss::WarmStart::INITIALIZED);

        FailAddressAdd = false;
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(intfmgr.m_pendingReplayIntfList.empty());
        EXPECT_TRUE(intfmgr.m_replayDone);
        EXPECT_EQ(warmState(), swss::WarmStart::RECONCILED);
    }

    INSTANTIATE_TEST_SUITE_P(ParentAndChild, IntfMgrWarmDeleteTest, ::testing::Bool());

    TEST_F(IntfMgrTest, testLagReopenReadsCurrentConfigOnce)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        const std::string alias = "PortChannel1";
        const std::string oldKey = alias + "|10.0.0.1/31";
        const std::string newKey = alias + "|10.0.0.5/31";
        intfmgr.m_cfgLagIntfTable.set(alias, {{"nat_zone", "1"}, {"mpls", "enable"}});
        intfmgr.m_cfgLagIntfTable.set(oldKey, {{"NULL", "NULL"}});
        notifyLag(intfmgr, alias, SET_COMMAND, {{"state", "ok"}});
        intfmgr.doTask(*consumer);
        ASSERT_TRUE(consumer->m_toSync.empty());
        notifyLag(intfmgr, alias, DEL_COMMAND);

        intfmgr.m_cfgLagIntfTable.del(alias);
        intfmgr.m_cfgLagIntfTable.set(alias, {{"nat_zone", "2"}});
        intfmgr.m_cfgLagIntfTable.del(oldKey);
        intfmgr.m_cfgLagIntfTable.set(newKey, {{"NULL", "NULL"}});
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(alias, SET_COMMAND, {{"nat_zone", "2"}}));
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(oldKey, DEL_COMMAND, {}));
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(newKey, SET_COMMAND, {}));
        mockCallArgs.clear();
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(commandWasIssued("address \"del\" \"10.0.0.1/31\""));
        EXPECT_FALSE(commandWasIssued("address \"add\""));

        // Residual work must be replaced, not merged with the new CONFIG_DB snapshot.
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(alias, DEL_COMMAND, {}));
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(alias, SET_COMMAND, {{"mpls", "enable"}}));
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(oldKey, SET_COMMAND, {}));
        notifyLag(intfmgr, alias, SET_COMMAND, {{"system_mac", "02:03:04:05:06:07"}});
        EXPECT_TRUE(intfmgr.isIntfStateOk(alias));
        notifyLag(intfmgr, alias, SET_COMMAND, {{"state", "ok"}});
        ASSERT_EQ(consumer->m_toSync.size(), 3u);
        EXPECT_EQ(consumer->m_toSync.begin()->first, alias);
        EXPECT_EQ(kfvOp(consumer->m_toSync.begin()->second), DEL_COMMAND);
        std::string value;
        auto data = kfvFieldsValues(std::next(consumer->m_toSync.begin())->second);
        EXPECT_TRUE(getFieldValue(data, "nat_zone", value));
        EXPECT_EQ(value, "2");
        EXPECT_FALSE(getFieldValue(data, "mpls", value));
        EXPECT_EQ(consumer->m_toSync.count(oldKey), 0u);
        EXPECT_EQ(consumer->m_toSync.count(newKey), 1u);
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(newKey, SET_COMMAND, {{"retry_marker", "kept"}}));
        const auto pending = consumer->m_toSync;
        notifyLag(intfmgr, alias, SET_COMMAND, {{"state", "ok"}});
        EXPECT_EQ(consumer->m_toSync, pending);

        testing_db::resetOperationCounters();
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 2u);
        std::vector<swss::FieldValueTuple> values;
        EXPECT_FALSE(intfmgr.m_stateIntfTable.get(oldKey, values));
        EXPECT_TRUE(intfmgr.m_stateIntfTable.get(newKey, values));
        mockCallArgs.clear();
        notifyLag(intfmgr, alias, SET_COMMAND, {{"state", "ok"}});
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(mockCallArgs.empty());
        EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 2u);

        notifyLag(intfmgr, alias, DEL_COMMAND);
        intfmgr.m_cfgLagIntfTable.del(alias);
        intfmgr.m_cfgLagIntfTable.del(newKey);
        notifyLag(intfmgr, alias, SET_COMMAND, {{"state", "ok"}});
        EXPECT_EQ(consumer->m_toSync.size(), 2u);
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
    }

    TEST_F(IntfMgrTest, testLagReopenUsesNormalLinkLocalPathAndPreservesPortUpdates)
    {
        Ethernet0IPv6Set = true;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        auto *subConsumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_VLAN_SUB_INTF_TABLE_NAME));
        ASSERT_NE(subConsumer, nullptr);
        swss::Table subConfig(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        subConfig.set("PortChannel1.10", {{"mtu", "9100"}, {"admin_status", "up"}});
        intfmgr.m_cfgLagIntfTable.set("PortChannel1", {{"NULL", "NULL"}});
        intfmgr.m_cfgLagIntfTable.set("PortChannel1|fe80::1/64", {{"NULL", "NULL"}});
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        intfmgr.doTask(*consumer);
        intfmgr.doTask(*subConsumer);
        ASSERT_EQ(intfmgr.m_intfLLAddresses.count("PortChannel1"), 1u);
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        MissingSubInterfaces.insert("PortChannel1.10");
        mockCallArgs.clear();
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "1500"}});
        EXPECT_FALSE(commandWasIssued("address \"add\""));
        EXPECT_FALSE(commandWasIssued("link set \"PortChannel1.10\""));
        EXPECT_EQ(intfmgr.m_intfLLAddresses.count("PortChannel1"), 0u);
        mockCallArgs.clear();
        intfmgr.doTask(*consumer);
        EXPECT_EQ(std::count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const std::string &cmd) {
            return cmd.find("address \"add\" \"fe80::1/64\"") != std::string::npos;
        }), 1);
        intfmgr.doTask(*subConsumer);
        EXPECT_TRUE(commandWasIssued("link add link \"PortChannel1\" name \"PortChannel1.10\""));
        EXPECT_TRUE(commandWasIssued("link set \"PortChannel1.10\" \"up\""));
        EXPECT_TRUE(commandWasIssued("link set \"PortChannel1.10\" mtu \"1500\""));
        mockCallArgs.clear();
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"admin_status", "up"}});
        EXPECT_EQ(std::count_if(mockCallArgs.begin(), mockCallArgs.end(), [](const std::string &cmd) {
            return cmd.find("address \"add\" \"fe80::1/64\"") != std::string::npos;
        }), 1);
        EXPECT_TRUE(consumer->m_toSync.empty());
    }

    TEST_F(IntfMgrTest, testLagReopenDiscardsSubscriberStartupSnapshot)
    {
        swss::Table config(m_config_db.get(), CFG_LAG_INTF_TABLE_NAME);
        config.set("PortChannel1", {{"nat_zone", "1"}, {"mpls", "enable"}});
        config.set("PortChannel1|10.0.0.1/31", {{"NULL", "NULL"}});
        config.set("PortChannel10", {{"NULL", "NULL"}});
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);

        config.del("PortChannel1");
        config.set("PortChannel1", {{"nat_zone", "2"}});
        config.del("PortChannel1|10.0.0.1/31");
        config.set("PortChannel1|10.0.0.5/31", {{"NULL", "NULL"}});
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        EXPECT_EQ(consumer->m_toSync.count("PortChannel10"), 1u);
        EXPECT_EQ(consumer->m_toSync.count("PortChannel1"), 0u);
        EXPECT_EQ(consumer->m_toSync.count("PortChannel1|10.0.0.1/31"), 0u);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});

        consumer->execute();
        std::vector<swss::FieldValueTuple> values;
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        ASSERT_TRUE(app.get("PortChannel1", values));
        std::string value;
        ASSERT_TRUE(getFieldValue(values, "nat_zone", value));
        EXPECT_EQ(value, "2");
        EXPECT_FALSE(getFieldValue(values, "mpls", value));
        EXPECT_FALSE(app.get("PortChannel1:10.0.0.1/31", values));
        EXPECT_TRUE(app.get("PortChannel1:10.0.0.5/31", values));
    }

    TEST_F(IntfMgrTest, testLagSubinterfaceDeleteRetainsApplicationAndCache)
    {
        Ethernet0IPv6Set = true;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        swss::Table subConfig(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        auto *parent = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        auto *child = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_VLAN_SUB_INTF_TABLE_NAME));
        ASSERT_NE(parent, nullptr);
        ASSERT_NE(child, nullptr);
        for (const std::string alias : {"PortChannel1", "PortChannel10"})
        {
            const auto subAlias = alias + (alias == "PortChannel1" ? ".20" : ".2");
            intfmgr.m_cfgLagIntfTable.set(alias, {{"NULL", "NULL"}});
            intfmgr.m_cfgLagIntfTable.set(alias + "|10.0.0.1/31", {{"NULL", "NULL"}});
            subConfig.set(subAlias, {{"mtu", "9100"}, {"ipv6_use_link_local_only", "enable"}});
            for (const std::string prefix : {"192.0.2.1/31", "2001:db8:20::1/64", "fe80::20/64"})
                subConfig.set(subAlias + "|" + prefix, {{"NULL", "NULL"}});
            notifyLag(intfmgr, alias, SET_COMMAND, {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        }
        intfmgr.doTask(*parent);
        child->refillToSync(&subConfig);
        intfmgr.doTask(*child);
        ASSERT_TRUE(parent->m_toSync.empty());
        ASSERT_TRUE(child->m_toSync.empty());
        ASSERT_EQ(intfmgr.m_subIntfList.count("PortChannel1.20"), 1u);
        ASSERT_EQ(intfmgr.m_intfLLAddresses.count("PortChannel1.20"), 1u);
        ASSERT_TRUE(intfmgr.isIntfStateOk("PortChannel1.20"));
        const auto parentConfigBefore = tableSnapshot(intfmgr.m_cfgLagIntfTable);
        const auto subConfigBefore = tableSnapshot(subConfig);
        for (auto *consumer : {parent, child})
        {
            const auto &config = consumer == parent ? parentConfigBefore : subConfigBefore;
            for (const auto &row : config)
            {
                consumer->addToSync(swss::KeyOpFieldsValuesTuple(row.first, DEL_COMMAND, {}));
                consumer->addToSync(swss::KeyOpFieldsValuesTuple(row.first, SET_COMMAND, row.second));
                intfmgr.m_pendingReplayIntfList.insert(row.first);
            }
        }

        mockCallArgs.clear();
        testing_db::resetOperationCounters();
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        EXPECT_TRUE(mockCallArgs.empty());
        EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 0u);
        EXPECT_EQ(intfmgr.m_subIntfList.count("PortChannel1.20"), 1u);
        EXPECT_EQ(intfmgr.m_intfLLAddresses.count("PortChannel1.20"), 1u);
        EXPECT_EQ(intfmgr.m_ipv6LinkLocalModeList.count("PortChannel1.20"), 1u);
        std::vector<swss::FieldValueTuple> values;
        EXPECT_FALSE(intfmgr.m_stateLagTable.get("PortChannel1.20", values));
        for (auto *consumer : {parent, child})
        {
            const auto &config = consumer == parent ? parentConfigBefore : subConfigBefore;
            for (const auto &row : config)
            {
                const bool related = row.first.find("PortChannel1|") == 0 ||
                    row.first.find("PortChannel1.") == 0 || row.first == "PortChannel1";
                EXPECT_EQ(consumer->m_toSync.count(row.first), related ? 1u : 2u);
                EXPECT_EQ(intfmgr.m_pendingReplayIntfList.count(row.first), 1u);
                EXPECT_EQ(intfmgr.m_stateIntfTable.get(row.first, values),
                          !related || row.first.find('|') == std::string::npos);
                auto appKey = row.first;
                auto separator = appKey.find('|');
                if (separator != std::string::npos)
                    appKey[separator] = ':';
                EXPECT_TRUE(app.get(appKey, values));
                if (related)
                {
                    EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, row.first), 0u);
                }
            }
        }
        EXPECT_EQ(intfmgr.m_subIntfList.count("PortChannel10.2"), 1u);
        EXPECT_EQ(intfmgr.m_intfLLAddresses.count("PortChannel10.2"), 1u);
        EXPECT_EQ(intfmgr.m_ipv6LinkLocalModeList.count("PortChannel10.2"), 1u);
        EXPECT_TRUE(intfmgr.m_stateLagTable.get("PortChannel10.2", values));
        EXPECT_TRUE(intfmgr.m_stateLagTable.get("PortChannel10", values));
        EXPECT_EQ(tableSnapshot(intfmgr.m_cfgLagIntfTable), parentConfigBefore);
        EXPECT_EQ(tableSnapshot(subConfig), subConfigBefore);
    }

    TEST_F(IntfMgrTest, testLagSubinterfaceReopenReadsCurrentConfigInOrder)
    {
        Ethernet0IPv6Set = true;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        swss::Table subConfig(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        auto *parent = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        auto *child = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_VLAN_SUB_INTF_TABLE_NAME));
        auto *state = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(STATE_LAG_TABLE_NAME));
        ASSERT_NE(parent, nullptr);
        ASSERT_NE(child, nullptr);
        ASSERT_NE(state, nullptr);
        const std::string alias = "PortChannel1.20";
        intfmgr.m_cfgLagIntfTable.set("PortChannel1", {{"NULL", "NULL"}});
        intfmgr.m_cfgLagIntfTable.set("PortChannel1|10.0.0.1/31", {{"NULL", "NULL"}});
        subConfig.set(alias, {{"mtu", "9100"}, {"ipv6_use_link_local_only", "enable"}});
        for (const std::string prefix : {"192.0.2.1/31", "2001:db8:20::1/64", "fe80::20/64"})
            subConfig.set(alias + "|" + prefix, {{"NULL", "NULL"}});
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        intfmgr.doTask(*parent);
        child->refillToSync(&subConfig);
        intfmgr.doTask(*child);
        ASSERT_TRUE(child->m_toSync.empty());
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        MissingSubInterfaces.insert(alias);
        subConfig.del(alias);
        subConfig.set(alias, {{"mtu", "1500"}, {"admin_status", "down"}});
        subConfig.del(alias + "|192.0.2.1/31");
        subConfig.del(alias + "|fe80::20/64");
        subConfig.set(alias + "|192.0.2.9/31", {{"NULL", "NULL"}});
        subConfig.set(alias + "|fe80::21/64", {{"NULL", "NULL"}});
        child->addToSync(swss::KeyOpFieldsValuesTuple(alias, DEL_COMMAND, {}));
        child->addToSync(swss::KeyOpFieldsValuesTuple(alias, SET_COMMAND, {{"mtu", "1500"}}));
        child->addToSync(swss::KeyOpFieldsValuesTuple(alias + "|192.0.2.1/31", DEL_COMMAND, {}));
        child->addToSync(swss::KeyOpFieldsValuesTuple(alias + "|fe80::21/64", SET_COMMAND, {}));
        mockCallArgs.clear();
        intfmgr.doTask(*child);
        EXPECT_TRUE(child->m_toSync.empty());
        EXPECT_TRUE(commandWasIssued("link del \"PortChannel1.20\""));
        EXPECT_FALSE(commandWasIssued("address \"add\""));
        mockCallArgs.clear();
        child->addToSync(swss::KeyOpFieldsValuesTuple(alias, SET_COMMAND, {{"ipv6_use_link_local_only", "enable"}}));
        child->addToSync(swss::KeyOpFieldsValuesTuple(alias + "|192.0.2.1/31", SET_COMMAND, {}));
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        EXPECT_TRUE(mockCallArgs.empty());
        ASSERT_EQ(child->m_toSync.size(), 5u);
        EXPECT_EQ(child->m_toSync.begin()->first, alias);
        EXPECT_EQ(child->m_toSync.count(alias + "|192.0.2.1/31"), 0u);
        intfmgr.doTask(*child);
        EXPECT_TRUE(commandWasIssued("address \"del\" \"fe80::20/64\""));
        EXPECT_FALSE(commandWasIssued("address \"add\""));
        EXPECT_EQ(child->m_toSync.size(), 4u);
        FailAddressAdd = true;
        intfmgr.doTask(*parent);
        ASSERT_EQ(parent->m_toSync.size(), 1u);
        mockCallArgs.clear();
        intfmgr.doTask(*child);
        EXPECT_TRUE(mockCallArgs.empty());

        FailAddressAdd = false;
        intfmgr.doTask(*parent);
        intfmgr.doTask(*child);
        EXPECT_TRUE(parent->m_toSync.empty());
        EXPECT_TRUE(child->m_toSync.empty());
        auto position = [](const std::string &needle) {
            return std::find_if(mockCallArgs.begin(), mockCallArgs.end(), [&](const std::string &cmd) {
                return cmd.find(needle) != std::string::npos;
            }) - mockCallArgs.begin();
        };
        const auto parentAddress = position("address \"add\" \"10.0.0.1/31\"");
        const auto childCreate = position("link add link \"PortChannel1\" name \"PortChannel1.20\"");
        const auto childMtu = position("link set \"PortChannel1.20\" mtu \"1500\"");
        const auto childAdmin = position("link set \"PortChannel1.20\" \"down\"");
        const auto childAddress = position("address \"add\" \"192.0.2.9/31\"");
        EXPECT_LT(parentAddress, childCreate);
        EXPECT_LT(childCreate, childMtu);
        EXPECT_LT(childMtu, childAddress);
        EXPECT_LT(childAdmin, childAddress);
        EXPECT_TRUE(commandWasIssued("address \"add\" \"192.0.2.9/31\""));
        EXPECT_TRUE(commandWasIssued("address \"add\" \"2001:db8:20::1/64\""));
        EXPECT_TRUE(commandWasIssued("address \"add\" \"fe80::21/64\""));
        EXPECT_FALSE(commandWasIssued("192.0.2.1/31"));
        EXPECT_FALSE(commandWasIssued("fe80::20/64"));
        EXPECT_EQ(intfmgr.m_subIntfList.at(alias).mtu, "1500");
        EXPECT_EQ(intfmgr.m_subIntfList.at(alias).adminStatus, "down");
        EXPECT_EQ(intfmgr.m_intfLLAddresses.at(alias), (std::set<std::string>{"fe80::21/64"}));
        EXPECT_EQ(intfmgr.m_ipv6LinkLocalModeList.count(alias), 0u);
        std::vector<swss::FieldValueTuple> values;
        EXPECT_FALSE(app.get(alias + ":192.0.2.1/31", values));
        EXPECT_FALSE(intfmgr.m_stateIntfTable.get(alias + "|fe80::20/64", values));
        EXPECT_TRUE(app.get(alias + ":fe80::21/64", values));
        EXPECT_TRUE(intfmgr.m_stateIntfTable.get(alias + "|2001:db8:20::1/64", values));
        EXPECT_TRUE(intfmgr.isIntfStateOk(alias));

        const auto publications = testing_db::getProducerSetCount(APP_INTF_TABLE_NAME);
        mockCallArgs.clear();
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
        // A delayed child notification must not open/close a separate parent epoch.
        state->addToSync(swss::KeyOpFieldsValuesTuple(alias, DEL_COMMAND, {}));
        state->addToSync(swss::KeyOpFieldsValuesTuple(alias, SET_COMMAND, {{"state", "ok"}}));
        intfmgr.doTask(*state);
        intfmgr.doTask(*parent);
        intfmgr.doTask(*child);
        EXPECT_TRUE(mockCallArgs.empty());
        EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), publications);
        EXPECT_TRUE(app.get(alias, values));
        EXPECT_TRUE(intfmgr.isIntfStateOk(alias));
    }

    TEST_F(IntfMgrTest, testLagSubinterfaceMissingChildAndL2Parent)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        swss::Table subConfig(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        auto *child = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_VLAN_SUB_INTF_TABLE_NAME));
        ASSERT_NE(child, nullptr);
        const std::string alias = "PortChannel1.20";
        subConfig.set(alias, {{"mtu", "1500"}, {"admin_status", "up"}});
        subConfig.set(alias + "|192.0.2.1/31", {{"NULL", "NULL"}});
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        child->refillToSync(&subConfig);
        intfmgr.doTask(*child);
        ASSERT_EQ(intfmgr.m_subIntfList.count(alias), 1u);
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        MissingSubInterfaces.insert(alias);
        mockCallArgs.clear();
        testing_db::resetOperationCounters();
        EXPECT_NO_THROW(notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                        {{"system_mac", "02:03:04:05:06:07"}, {"admin_status", "up"}, {"mtu", "9000"}}));
        EXPECT_TRUE(mockCallArgs.empty());
        EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 0u);
        EXPECT_NO_THROW(notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                        {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9000"}}));
        EXPECT_TRUE(mockCallArgs.empty());
        FailSubIntfCreation = true;
        EXPECT_NO_THROW(intfmgr.doTask(*child));
        EXPECT_EQ(child->m_toSync.size(), 2u);
        EXPECT_EQ(intfmgr.m_subIntfList.count(alias), 0u);
        EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 0u);
        std::vector<swss::FieldValueTuple> values;
        EXPECT_TRUE(app.get(alias, values));
        EXPECT_FALSE(intfmgr.m_stateLagTable.get(alias, values));
        mockCallArgs.clear();
        EXPECT_NO_THROW(notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"admin_status", "up"}, {"mtu", "9100"}}));
        EXPECT_TRUE(mockCallArgs.empty());
        FailSubIntfCreation = false;
        intfmgr.doTask(*child);
        EXPECT_TRUE(child->m_toSync.empty());
        EXPECT_TRUE(app.get(alias + ":192.0.2.1/31", values));
        EXPECT_TRUE(intfmgr.isIntfStateOk(alias));

        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        subConfig.del(alias);
        subConfig.del(alias + "|192.0.2.1/31");
        mockCallArgs.clear();
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
        intfmgr.doTask(*child);
        EXPECT_TRUE(child->m_toSync.empty());
        EXPECT_TRUE(commandWasIssued("link del \"PortChannel1.20\""));
        EXPECT_FALSE(app.get(alias, values));
        EXPECT_EQ(intfmgr.m_subIntfList.count(alias), 0u);
    }

    TEST_F(IntfMgrTest, testLagSubinterfaceShortNameUsesExactParent)
    {
        swss::Table config(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        config.set("Po1.20", {{"vlan", "20"}});
        config.set("Po1.30", {{"vlan", "30"}});
        config.set("Po10.20", {{"vlan", "20"}});
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *child = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_VLAN_SUB_INTF_TABLE_NAME));
        ASSERT_NE(child, nullptr);
        config.del("Po1.30");
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        notifyLag(intfmgr, "PortChannel10", SET_COMMAND, {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        child->execute();
        ASSERT_EQ(intfmgr.m_subIntfList.count("Po1.20"), 1u);
        ASSERT_EQ(intfmgr.m_subIntfList.count("Po10.20"), 1u);
        EXPECT_EQ(intfmgr.m_subIntfList.count("Po1.30"), 0u);
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        EXPECT_EQ(intfmgr.m_subIntfList.count("Po1.20"), 1u);
        EXPECT_EQ(intfmgr.m_subIntfList.count("Po10.20"), 1u);
        EXPECT_FALSE(intfmgr.isIntfStateOk("Po1.20"));
        EXPECT_TRUE(intfmgr.isIntfStateOk("Po10.20"));
        config.set("Po1.30", {{"vlan", "30"}});
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        ASSERT_EQ(child->m_toSync.size(), 2u);
        EXPECT_EQ(child->m_toSync.begin()->first, "Po1.20");
        intfmgr.doTask(*child);
        EXPECT_TRUE(child->m_toSync.empty());
        EXPECT_TRUE(intfmgr.isIntfStateOk("Po1.20"));
        EXPECT_TRUE(intfmgr.isIntfStateOk("Po1.30"));
    }

    TEST_F(IntfMgrTest, testHybridDelPreservesConfigDeleteAndApplication)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = applyChild(intfmgr);
        ASSERT_TRUE(consumer->m_toSync.empty());
        swss::Table config(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        const std::string alias = "PortChannel1.20";
        const std::string key = alias + "|192.0.2.1/31";
        const auto application = tableSnapshot(app);
        config.del(alias);
        config.del(key);
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(alias, DEL_COMMAND, {}));
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(key, DEL_COMMAND, {}));
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, {}));
        intfmgr.m_pendingReplayIntfList.insert(alias);
        intfmgr.m_pendingReplayIntfList.insert(key);
        testing_db::resetOperationCounters();
        mockCallArgs.clear();
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        EXPECT_TRUE(mockCallArgs.empty());
        EXPECT_EQ(tableSnapshot(app), application);
        EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 0u);
        EXPECT_EQ(intfmgr.m_subIntfList.count(alias), 1u);
        EXPECT_EQ(consumer->m_toSync.count(alias), 1u);
        ASSERT_EQ(consumer->m_toSync.count(key), 1u);
        EXPECT_EQ(kfvOp(consumer->m_toSync.find(key)->second), DEL_COMMAND);
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList.count(key), 0u);
        std::vector<swss::FieldValueTuple> values;
        EXPECT_TRUE(intfmgr.m_stateIntfTable.get(alias, values));
        EXPECT_FALSE(intfmgr.m_stateIntfTable.get(key, values));
        EXPECT_FALSE(intfmgr.m_stateLagTable.get(alias, values));

        MissingSubInterfaces.insert(alias);
        EXPECT_NO_THROW(intfmgr.doTask(*consumer));
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(commandWasIssued("link del \"PortChannel1.20\""));
        EXPECT_FALSE(app.get(alias, values));
        EXPECT_FALSE(app.get(alias + ":192.0.2.1/31", values));
        EXPECT_EQ(intfmgr.m_subIntfList.count(alias), 0u);
        EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 2u);
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 0u);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
        EXPECT_TRUE(consumer->m_toSync.empty());
    }

    TEST_F(IntfMgrTest, testReadinessWithdrawalRemovesOwnedRowsAndPreservesBaseMetadata)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *child = applyChild(intfmgr);
        ASSERT_TRUE(child->m_toSync.empty());
        const std::string alias = "PortChannel1.20";
        const std::string key = alias + "|192.0.2.1/31";
        intfmgr.m_stateIntfTable.hset(alias, "metadata", "keep-base");
        intfmgr.m_stateIntfTable.hset(key, "metadata", "keep-address");
        intfmgr.m_stateLagTable.hset(alias, "metadata", "keep-child");
        const auto base = tableSnapshot(intfmgr.m_stateIntfTable).at(alias);
        const auto upstream = tableSnapshot(intfmgr.m_stateLagTable).at("PortChannel1");
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        const auto application = tableSnapshot(app);
        const auto kernel = KernelSubIntfs;
        mockCallArgs.clear();
        // Dispatch the notification without erasing the upstream-owned hash in the fixture.
        intfmgr.doLagTableTask("PortChannel1", {}, DEL_COMMAND);
        auto state = tableSnapshot(intfmgr.m_stateIntfTable);
        auto lagState = tableSnapshot(intfmgr.m_stateLagTable);
        EXPECT_EQ(state[alias], base);
        EXPECT_EQ(lagState["PortChannel1"], upstream);
        std::string value;
        EXPECT_EQ(state.count(key), 0u);
        EXPECT_EQ(lagState.count(alias), 0u);
        EXPECT_FALSE(intfmgr.isIntfStateOk(alias));
        EXPECT_EQ(tableSnapshot(app), application);
        EXPECT_EQ(KernelSubIntfs, kernel);
        EXPECT_TRUE(mockCallArgs.empty());
        EXPECT_EQ(intfmgr.m_subIntfList.count(alias), 1u);

        MissingSubInterfaces.insert(alias);
        KernelSubIntfs.erase(alias);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        EXPECT_TRUE(mockCallArgs.empty());
        FailSubIntfMtu = true;
        intfmgr.doTask(*child);
        EXPECT_TRUE(commandWasIssued("link add link \"PortChannel1\" name \"PortChannel1.20\""));
        EXPECT_FALSE(intfmgr.m_stateIntfTable.hget(key, "state", value));
        EXPECT_FALSE(intfmgr.m_stateLagTable.hget(alias, "state", value));
        FailSubIntfMtu = false;
        intfmgr.doTask(*child);
        EXPECT_TRUE(child->m_toSync.empty());
        ASSERT_TRUE(intfmgr.m_stateIntfTable.hget(key, "state", value));
        EXPECT_EQ(value, "ok");
        ASSERT_TRUE(intfmgr.m_stateLagTable.hget(alias, "state", value));
        EXPECT_EQ(value, "ok");
        EXPECT_FALSE(intfmgr.m_stateIntfTable.hget(key, "metadata", value));
        EXPECT_FALSE(intfmgr.m_stateLagTable.hget(alias, "metadata", value));
        ASSERT_TRUE(intfmgr.m_stateIntfTable.hget(alias, "metadata", value));
        EXPECT_EQ(value, "keep-base");
    }

    TEST_F(IntfMgrTest, testHybridRetainedBaseGatesAddressAndChildReplay)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        const std::string alias = "PortChannel1";
        const std::string key = alias + "|10.0.0.1/31";
        intfmgr.m_cfgLagIntfTable.set(alias, {{"NULL", "NULL"}});
        intfmgr.m_cfgLagIntfTable.set(key, {{"NULL", "NULL"}});
        auto *child = applyChild(intfmgr);
        auto *parent = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(parent, nullptr);
        intfmgr.doTask(*parent);
        intfmgr.doTask(*child);
        ASSERT_TRUE(parent->m_toSync.empty());
        ASSERT_TRUE(child->m_toSync.empty());
        notifyLag(intfmgr, alias, DEL_COMMAND);
        ASSERT_TRUE(intfmgr.isIntfCreated(alias));
        intfmgr.m_cfgLagIntfTable.hset(alias, "mpls", "invalid");
        notifyLag(intfmgr, alias, SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        testing_db::resetOperationCounters();
        mockCallArgs.clear();
        intfmgr.doTask(*parent);
        intfmgr.doTask(*child);
        EXPECT_EQ(parent->m_toSync.size(), 2u);
        EXPECT_EQ(child->m_toSync.size(), 2u);
        EXPECT_FALSE(commandWasIssued("address \"add\""));
        EXPECT_FALSE(commandWasIssued("link set \"PortChannel1.20\""));
        EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 0u);
        std::string value;
        EXPECT_FALSE(intfmgr.m_stateIntfTable.hget(key, "state", value));
        EXPECT_FALSE(intfmgr.m_stateLagTable.hget("PortChannel1.20", "state", value));
        parent->addToSync(swss::KeyOpFieldsValuesTuple(alias, SET_COMMAND, {{"mpls", "disable"}}));
        intfmgr.m_cfgLagIntfTable.hset(alias, "mpls", "disable");
        intfmgr.doTask(*parent);
        intfmgr.doTask(*child);
        EXPECT_TRUE(parent->m_toSync.empty());
        EXPECT_TRUE(child->m_toSync.empty());
        ASSERT_TRUE(intfmgr.m_stateIntfTable.hget(key, "state", value));
        EXPECT_EQ(value, "ok");
        ASSERT_TRUE(intfmgr.m_stateLagTable.hget("PortChannel1.20", "state", value));
        EXPECT_EQ(value, "ok");
    }

    TEST_F(IntfMgrTest, testHybridRetainedVrfRejectsRebindUntilConfigDelete)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *parent = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        auto *child = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_VLAN_SUB_INTF_TABLE_NAME));
        ASSERT_NE(parent, nullptr);
        ASSERT_NE(child, nullptr);
        intfmgr.m_stateVrfTable.set("VrfRed", {{"state", "ok"}});
        intfmgr.m_stateVrfTable.set("VrfBlue", {{"state", "ok"}});
        intfmgr.m_cfgLagIntfTable.set("PortChannel1", {{"vrf_name", "VrfRed"}});
        intfmgr.m_cfgSubIntfTable.set("PortChannel1.20", {{"vrf_name", "VrfRed"}, {"mtu", "1500"}});
        for (int interval = 0; interval < 3; ++interval)
        {
            if (interval)
                notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
            if (interval == 2)
            {
                intfmgr.m_cfgLagIntfTable.hset("PortChannel1", "vrf_name", "VrfBlue");
                intfmgr.m_cfgSubIntfTable.hset("PortChannel1.20", "vrf_name", "VrfBlue");
            }
            mockCallArgs.clear();
            notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                      {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
            intfmgr.doTask(*parent);
            intfmgr.doTask(*child);
            EXPECT_TRUE(parent->m_toSync.empty());
            EXPECT_TRUE(child->m_toSync.empty());
            for (const std::string alias : {"PortChannel1", "PortChannel1.20"})
            {
                SCOPED_TRACE(alias + ":" + std::to_string(interval));
                std::string vrf;
                ASSERT_TRUE(intfmgr.m_stateIntfTable.hget(alias, "vrf", vrf));
                EXPECT_EQ(vrf, "VrfRed");
                EXPECT_FALSE(commandWasIssued("link set \"" + alias + "\" master \"VrfBlue\""));
                if (interval < 2)
                {
                    EXPECT_TRUE(commandWasIssued("link set \"" + alias + "\" master \"VrfRed\""));
                }
            }
        }
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        for (const auto &entry : std::vector<std::pair<Consumer *, std::string>>{
                 {child, "PortChannel1.20"}, {parent, "PortChannel1"}})
        {
            entry.first->addToSync(swss::KeyOpFieldsValuesTuple(entry.second, DEL_COMMAND, {}));
            intfmgr.doTask(*entry.first);
            EXPECT_FALSE(intfmgr.isIntfCreated(entry.second));
            std::vector<swss::FieldValueTuple> values;
            EXPECT_FALSE(app.get(entry.second, values));
        }
        mockCallArgs.clear();
        parent->addToSync(swss::KeyOpFieldsValuesTuple("PortChannel1", SET_COMMAND, {{"vrf_name", "VrfBlue"}}));
        child->addToSync(swss::KeyOpFieldsValuesTuple("PortChannel1.20", SET_COMMAND, {{"vrf_name", "VrfBlue"}}));
        intfmgr.doTask(*parent);
        intfmgr.doTask(*child);
        for (const std::string alias : {"PortChannel1", "PortChannel1.20"})
        {
            std::string vrf;
            ASSERT_TRUE(intfmgr.m_stateIntfTable.hget(alias, "vrf", vrf));
            EXPECT_EQ(vrf, "VrfBlue");
            EXPECT_TRUE(commandWasIssued("link set \"" + alias + "\" master \"VrfBlue\""));
        }
    }

    TEST_F(IntfMgrTest, testHybridReopenAdoptsExactSurvivingChild)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = applyChild(intfmgr);
        ASSERT_TRUE(consumer->m_toSync.empty());
        const std::string alias = "PortChannel1.20";
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        const auto application = tableSnapshot(app);
        FailSubIntfCreation = true;
        FailAddressAdd = true;
        AddressQueryOutput = "7: PortChannel1.20 inet 192.0.2.1/31 scope global\n";
        testing_db::resetOperationCounters();
        for (std::size_t interval = 1; interval <= 2; ++interval)
        {
            mockCallArgs.clear();
            notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
            notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
            EXPECT_TRUE(mockCallArgs.empty());
            EXPECT_EQ(tableSnapshot(app), application);
            EXPECT_EQ(intfmgr.m_subIntfList.count(alias), 1u);
            EXPECT_FALSE(intfmgr.isIntfStateOk(alias));
            notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                      {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
            EXPECT_TRUE(mockCallArgs.empty());
            intfmgr.doTask(*consumer);
            EXPECT_TRUE(consumer->m_toSync.empty());
            EXPECT_TRUE(commandWasIssued("-d -o link show dev \"PortChannel1.20\""));
            EXPECT_FALSE(commandWasIssued("link add"));
            EXPECT_FALSE(commandWasIssued("link del"));
            EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 0u);
            EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), interval * 2);
            EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), interval);
            EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias + "|192.0.2.1/31"), interval);
            EXPECT_EQ(testing_db::getTableSetCount(STATE_LAG_TABLE_NAME, alias), interval);
            mockCallArgs.clear();
            notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
            intfmgr.doTask(*consumer);
            EXPECT_TRUE(mockCallArgs.empty());
            EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), interval);
        }
    }

    TEST_F(IntfMgrTest, testFailedSubinterfaceAddRequiresExactObject)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        const std::string alias = "PortChannel1.20";
        FailSubIntfCreation = true;
        const std::vector<std::pair<std::string, bool>> cases = {
            {subIntfLine(alias, "PortChannel1", "20"), true},
            {subIntfLine(alias, "PortChannel10", "20"), false},
            {subIntfLine(alias, "PortChannel1", "21"), false},
            {"7: PortChannel1.20@PortChannel1: <UP> mtu 1500\\    dummy\n", false},
            {"7: PortChannel1.20@PortChannel1: <UP> vlan protocol 802.1ad id 20\n", false},
            {subIntfLine(alias, "PortChannel1", "20bad"), false},
            {"", false}
        };
        for (const auto &entry : cases)
        {
            SCOPED_TRACE(entry.first);
            SubIntfQueryOverrides[alias] = entry.first;
            EXPECT_EQ(intfmgr.isSubIntfPresent("PortChannel1", alias, "20"), entry.second);
            mockCallArgs.clear();
            if (entry.second)
            {
                EXPECT_NO_THROW(intfmgr.addHostSubIntf("PortChannel1", alias, "20"));
            }
            else
            {
                EXPECT_THROW(intfmgr.addHostSubIntf("PortChannel1", alias, "20"), std::runtime_error);
            }
            ASSERT_GE(mockCallArgs.size(), 2u);
            EXPECT_EQ(mockCallArgs.front(), "/sbin/ip link add link \"PortChannel1\" name \"PortChannel1.20\" type vlan id \"20\"");
            EXPECT_EQ(mockCallArgs[1], "/sbin/ip -d -o link show dev \"PortChannel1.20\"");
        }
        SubIntfQueryOverrides[alias] = subIntfLine(alias, "PortChannel1", "20");
        EXPECT_TRUE(intfmgr.isSubIntfPresent("PortChannel1", alias, "020"));
        SubIntfQueryResult = 1;
        EXPECT_THROW(intfmgr.addHostSubIntf("PortChannel1", alias, "20"), std::runtime_error);
        FailSubIntfCreation = false;
        mockCallArgs.clear();
        EXPECT_NO_THROW(intfmgr.addHostSubIntf("PortChannel1", alias, "20"));
        EXPECT_EQ(mockCallArgs.size(), 1u);
    }

    TEST_F(IntfMgrTest, testHybridCachedChildNonReadyAttemptsCreation)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = applyChild(intfmgr);
        ASSERT_TRUE(consumer->m_toSync.empty());
        const std::string alias = "PortChannel1.20";
        const auto applied = intfmgr.m_subIntfList.at(alias);
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        const auto application = tableSnapshot(app);
        const std::vector<std::string> outputs = {
            subIntfLine(alias, "PortChannel10", "20"),
            subIntfLine(alias, "PortChannel1", "30"),
            "7: PortChannel1.20@PortChannel1: <UP> dummy\n",
            "invalid\n",
            subIntfLine(alias, "PortChannel1", "20"),
            subIntfLine(alias, "PortChannel1", "20")
        };
        for (std::size_t i = 0; i < outputs.size(); ++i)
        {
            SCOPED_TRACE(i);
            intfmgr.m_subIntfList[alias] = applied;
            notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
            SubIntfQueryOverrides[alias] = outputs[i];
            SubIntfQueryResult = i == 4 ? 1 : 0;
            ThrowSubIntfQuery = i == 5;
            notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                      {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
            testing_db::resetOperationCounters();
            mockCallArgs.clear();
            intfmgr.doTask(*consumer);
            EXPECT_EQ(consumer->m_toSync.size(), 2u);
            EXPECT_EQ(intfmgr.m_subIntfList.count(alias), 0u);
            EXPECT_EQ(tableSnapshot(app), application);
            EXPECT_FALSE(intfmgr.isIntfStateOk(alias));
            EXPECT_TRUE(intfmgr.isIntfCreated(alias));
            EXPECT_TRUE(commandWasIssued("link add"));
            EXPECT_FALSE(commandWasIssued("link del"));
            EXPECT_FALSE(commandWasIssued("address \"add\""));
            EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 0u);
            EXPECT_EQ(testing_db::getTableSetCount(STATE_LAG_TABLE_NAME, alias), 0u);
            EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 0u);
        }
        SubIntfQueryResult = 0;
        ThrowSubIntfQuery = false;
        SubIntfQueryOverrides.clear();
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(intfmgr.isIntfStateOk(alias));
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 1u);
    }

    TEST_F(IntfMgrTest, testHybridCachedChildQueryFailureRecreatesMissingChild)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = applyChild(intfmgr);
        ASSERT_TRUE(consumer->m_toSync.empty());
        const std::string alias = "PortChannel1.20";
        for (bool throws : {false, true})
        {
            SCOPED_TRACE(throws);
            notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
            KernelSubIntfs.erase(alias);
            SubIntfQueryResult = 1;
            ThrowSubIntfQuery = throws;
            notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                      {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
            mockCallArgs.clear();
            interfaceLookups.clear();
            intfmgr.doTask(*consumer);
            EXPECT_TRUE(consumer->m_toSync.empty());
            EXPECT_EQ(intfmgr.m_subIntfList.count(alias), 1u);
            ASSERT_EQ(KernelSubIntfs.count(alias), 1u);
            EXPECT_EQ(KernelSubIntfs.at(alias), std::make_pair(std::string("PortChannel1"), std::string("20")));
            EXPECT_TRUE(intfmgr.isIntfStateOk(alias));
            EXPECT_TRUE(commandWasIssued("link add"));
            EXPECT_FALSE(commandWasIssued("link del"));
            EXPECT_TRUE(interfaceLookups.empty());
        }
    }

    TEST_F(IntfMgrTest, testHybridStateWaitsForChildMtuAndAdmin)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = applyChild(intfmgr);
        ASSERT_TRUE(consumer->m_toSync.empty());
        const std::string alias = "PortChannel1.20";
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        const auto application = tableSnapshot(app);
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        testing_db::resetOperationCounters();
        for (bool failMtu : {true, false})
        {
            FailSubIntfMtu = failMtu;
            FailSubIntfAdmin = !failMtu;
            mockCallArgs.clear();
            intfmgr.doTask(*consumer);
            EXPECT_TRUE(commandWasIssued("link set \"PortChannel1.20\" mtu \"1500\""));
            if (failMtu)
            {
                EXPECT_FALSE(commandWasIssued("link set \"PortChannel1.20\" \"up\""));
            }
            else
            {
                EXPECT_TRUE(commandWasIssued("link set \"PortChannel1.20\" \"up\""));
            }
            EXPECT_EQ(consumer->m_toSync.size(), 2u);
            EXPECT_EQ(tableSnapshot(app), application);
            EXPECT_FALSE(intfmgr.isIntfStateOk(alias));
            EXPECT_TRUE(intfmgr.isIntfCreated(alias));
            EXPECT_EQ(testing_db::getTableSetCount(STATE_LAG_TABLE_NAME, alias), 0u);
            EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 0u);
            EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias + "|192.0.2.1/31"), 0u);
        }
        FailSubIntfMtu = FailSubIntfAdmin = false;
        mockCallArgs.clear();
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(commandWasIssued("link set \"PortChannel1.20\" mtu \"1500\""));
        EXPECT_TRUE(commandWasIssued("link set \"PortChannel1.20\" \"up\""));
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_EQ(testing_db::getTableSetCount(STATE_LAG_TABLE_NAME, alias), 1u);
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 1u);
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias + "|192.0.2.1/31"), 1u);
    }

    TEST_F(IntfMgrTest, testHybridReopenCleansDeletedConfigFromRetainedProjection)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = applyChild(intfmgr);
        applyChild(intfmgr, "PortChannel1.30", "192.0.2.3/31");
        ASSERT_TRUE(consumer->m_toSync.empty());
        swss::Table config(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        ASSERT_EQ(intfmgr.m_subIntfList.count("PortChannel1.20"), 1u);
        config.del("PortChannel1.20");
        config.del("PortChannel1.20|192.0.2.1/31");
        testing_db::resetOperationCounters();
        mockCallArgs.clear();
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        EXPECT_TRUE(mockCallArgs.empty());
        ASSERT_EQ(consumer->m_toSync.count("PortChannel1.20"), 1u);
        EXPECT_EQ(kfvOp(consumer->m_toSync.find("PortChannel1.20")->second), DEL_COMMAND);
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(commandWasIssued("link del \"PortChannel1.20\""));
        EXPECT_FALSE(commandWasIssued("link del \"PortChannel1.30\""));
        EXPECT_FALSE(commandWasIssued("link add"));
        EXPECT_EQ(intfmgr.m_subIntfList.count("PortChannel1.20"), 0u);
        EXPECT_EQ(intfmgr.m_subIntfList.count("PortChannel1.30"), 1u);
        std::vector<swss::FieldValueTuple> values;
        EXPECT_FALSE(app.get("PortChannel1.20", values));
        EXPECT_FALSE(app.get("PortChannel1.20:192.0.2.1/31", values));
        EXPECT_TRUE(app.get("PortChannel1.30:192.0.2.3/31", values));
        EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 2u);
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, "PortChannel1.30"), 1u);
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, "PortChannel1.20"), 0u);
    }

    TEST_F(IntfMgrTest, testHybridRetainedGeneralDeleteBlocksFollowingSet)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = applyChild(intfmgr, "PortChannel1.20", "");
        ASSERT_TRUE(consumer->m_toSync.empty());
        const std::string alias = "PortChannel1.20";
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(alias, DEL_COMMAND, {}));
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(alias, SET_COMMAND, {}));
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        ASSERT_EQ(consumer->m_toSync.size(), 1u);
        IntfIpCount = 1;
        intfmgr.doTask(*consumer);
        EXPECT_EQ(consumer->m_toSync.size(), 1u);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        ASSERT_EQ(consumer->m_toSync.size(), 2u);
        intfmgr.m_pendingReplayIntfList.insert(alias);
        testing_db::resetOperationCounters();
        intfmgr.doTask(*consumer);
        EXPECT_EQ(consumer->m_toSync.size(), 2u);
        EXPECT_TRUE(intfmgr.isIntfCreated(alias));
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 0u);
        EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 0u);
        IntfIpCount = 0;
        FailSubIntfCreation = true;
        intfmgr.doTask(*consumer);
        EXPECT_EQ(consumer->m_toSync.size(), 1u);
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList.count(alias), 1u);
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 0u);
        FailSubIntfCreation = false;
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_EQ(intfmgr.m_pendingReplayIntfList.count(alias), 0u);
        EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 1u);
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 1u);
        EXPECT_EQ(testing_db::getTableSetCount(STATE_LAG_TABLE_NAME, alias), 1u);
    }

    TEST_F(IntfMgrTest, testPartialSetPreservesLinkLocalModeWithoutBaseMetadata)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        const std::string alias = "PortChannel1";
        intfmgr.m_cfgLagIntfTable.set(alias, {{"ipv6_use_link_local_only", "enable"}});
        notifyLag(intfmgr, alias, SET_COMMAND, {{"state", "ok"}});
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_LAG_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        intfmgr.doTask(*consumer);
        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(intfmgr.m_ipv6LinkLocalModeList.count(alias), 1u);

        intfmgr.m_stateIntfTable.del(alias);
        intfmgr.m_cfgLagIntfTable.hset(alias, "nat_zone", "2");
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(alias, SET_COMMAND, {{"nat_zone", "2"}}));
        intfmgr.doTask(*consumer);

        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_EQ(intfmgr.m_ipv6LinkLocalModeList.count(alias), 1u);
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(app.get(alias, values));
        std::string value;
        ASSERT_TRUE(getFieldValue(values, "ipv6_use_link_local_only", value));
        EXPECT_EQ(value, "enable");
        ASSERT_TRUE(getFieldValue(values, "nat_zone", value));
        EXPECT_EQ(value, "2");
    }

    TEST_F(IntfMgrTest, testHybridReopenResetsRemovedMtuAndLinkLocalMode)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        const std::string alias = "PortChannel1.20";
        swss::Table config(m_config_db.get(), CFG_VLAN_SUB_INTF_TABLE_NAME);
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        config.set(alias, {{"mtu", "1500"}, {"admin_status", "up"}, {"ipv6_use_link_local_only", "enable"}});
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(CFG_VLAN_SUB_INTF_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        consumer->refillToSync(&config);
        intfmgr.doTask(*consumer);
        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_EQ(intfmgr.m_ipv6LinkLocalModeList.count(alias), 1u);
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        // Constructed CONFIG changes; readiness withdrawal does not remove these fields.
        config.hdel(alias, "mtu");
        config.hdel(alias, "ipv6_use_link_local_only");
        testing_db::resetOperationCounters();
        mockCallArgs.clear();
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        FailSubIntfMtu = true;
        intfmgr.doTask(*consumer);
        EXPECT_EQ(consumer->m_toSync.size(), 1u);
        EXPECT_EQ(intfmgr.m_ipv6LinkLocalModeList.count(alias), 1u);
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 0u);
        FailSubIntfMtu = false;
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(commandWasIssued("link set \"PortChannel1.20\" mtu \"9100\""));
        EXPECT_FALSE(commandWasIssued("link add"));
        EXPECT_EQ(intfmgr.m_subIntfList.at(alias).mtu, "0");
        EXPECT_EQ(intfmgr.m_ipv6LinkLocalModeList.count(alias), 0u);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(app.get(alias, values));
        std::string value;
        ASSERT_TRUE(getFieldValue(values, "mtu", value));
        EXPECT_EQ(value, "9100");
        ASSERT_TRUE(getFieldValue(values, "ipv6_use_link_local_only", value));
        EXPECT_EQ(value, "disable");
        EXPECT_FALSE(config.hget(alias, "mtu", value));
        EXPECT_FALSE(config.hget(alias, "ipv6_use_link_local_only", value));
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 1u);
        EXPECT_EQ(testing_db::getProducerDelCount(APP_INTF_TABLE_NAME), 0u);

        mockCallArgs.clear();
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"mtu", "8500"}});
        EXPECT_TRUE(commandWasIssued("link set \"PortChannel1.20\" mtu \"8500\""));
        ASSERT_TRUE(app.get(alias, values));
        ASSERT_TRUE(getFieldValue(values, "mtu", value));
        EXPECT_EQ(value, "8500");
        EXPECT_EQ(intfmgr.m_subIntfList.at(alias).mtu, "0");
        EXPECT_EQ(testing_db::getProducerSetCount(APP_LAG_TABLE_NAME), 0u);
    }

    TEST_F(IntfMgrTest, testHybridExplicitMtuWaitsForParentMetadata)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = applyChild(intfmgr);
        ASSERT_TRUE(consumer->m_toSync.empty());
        const std::string alias = "PortChannel1.20";
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        const auto application = tableSnapshot(app);
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"state", "ok"}});
        EXPECT_TRUE(intfmgr.isIntfStateOk("PortChannel1"));
        EXPECT_EQ(intfmgr.getIntfMtu("PortChannel1"), "0");
        testing_db::resetOperationCounters();
        mockCallArgs.clear();
        intfmgr.doTask(*consumer);
        EXPECT_EQ(consumer->m_toSync.size(), 2u);
        EXPECT_FALSE(commandWasIssued("link set \"PortChannel1.20\" mtu"));
        EXPECT_EQ(tableSnapshot(app), application);
        EXPECT_FALSE(intfmgr.isIntfStateOk(alias));
        EXPECT_TRUE(intfmgr.isIntfCreated(alias));
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 0u);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND, {{"admin_status", "up"}, {"mtu", "9100"}});
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(commandWasIssued("link set \"PortChannel1.20\" mtu \"1500\""));
        EXPECT_FALSE(commandWasIssued("link set \"PortChannel1.20\" mtu \"0\""));
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias), 1u);
        EXPECT_EQ(testing_db::getTableSetCount(STATE_INTERFACE_TABLE_NAME, alias + "|192.0.2.1/31"), 1u);
    }

    TEST_F(IntfMgrTest, testSettingIpv6Flag){
        Ethernet0IPv6Set = false;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        /* Set portStateTable */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        /* Set m_stateIntfTable */
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");
        /* Set Ipv6 prefix */
        const std::vector<std::string>& keys = {"Ethernet0", "2001::8/64"};
        const std::vector<swss::FieldValueTuple> data;
        ASSERT_TRUE(intfmgr.doIntfAddrTask(keys, data, "SET"));
        int ip_cmd_called = 0;
        for (auto cmd : mockCallArgs){
            if (cmd.find("/sbin/ip -6 address \"add\"") == 0){
                ip_cmd_called++;
            }
        }
        ASSERT_EQ(ip_cmd_called, 2);
    }

    TEST_F(IntfMgrTest, testNoSettingIpv6Flag){
        Ethernet0IPv6Set = true; // Assuming it is already set by SDK
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        /* Set portStateTable */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        /* Set m_stateIntfTable */
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");
        /* Set Ipv6 prefix */
        const std::vector<std::string>& keys = {"Ethernet0", "2001::8/64"};
        const std::vector<swss::FieldValueTuple> data;
        ASSERT_TRUE(intfmgr.doIntfAddrTask(keys, data, "SET"));
        int ip_cmd_called = 0;
        for (auto cmd : mockCallArgs){
            if (cmd.find("/sbin/ip -6 address \"add\"") == 0){
                ip_cmd_called++;
            }
        }
        ASSERT_EQ(ip_cmd_called, 1);
    }

    TEST_F(IntfMgrTest, testHostSubIntfAdminStatusReturnsOnlyUpOrDown)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        const std::string alias = "Ethernet0.10";
        mockCallArgs.clear();
        EXPECT_EQ(intfmgr.setHostSubIntfAdminStatus(alias, "up", "up"), "up");
        EXPECT_EQ(intfmgr.setHostSubIntfAdminStatus(alias, "down", "up"), "down");
        EXPECT_EQ(intfmgr.setHostSubIntfAdminStatus(alias, "down", "down"), "down");
        ASSERT_EQ(mockCallArgs.size(), 3u);
        EXPECT_EQ(intfmgr.setHostSubIntfAdminStatus(alias, "up", "down"), "down");
        EXPECT_EQ(mockCallArgs.size(), 3u);
        for (const std::string status : {"", "invalid"})
            EXPECT_THROW(intfmgr.setHostSubIntfAdminStatus(alias, status, "up"), std::runtime_error);
        EXPECT_EQ(mockCallArgs.size(), 3u);
    }

    TEST_F(IntfMgrTest, testAdminStatusCheckUsesExactAdminFlag)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        const std::string alias = "Ethernet0.10";
        const std::vector<std::pair<std::string, bool>> cases = {
            {"<UP,BROADCAST,LOWER_UP> state DOWN", true},
            {"<BROADCAST,UP,LOWER_UP> state UNKNOWN", true},
            {"<BROADCAST,UP> state DOWN", true},
            {"<UP> state UNKNOWN", true},
            {"<BROADCAST,LOWER_UP> state UP", false},
            {"<> state DOWN", false},
        };
        for (const auto &entry : cases)
        {
            SCOPED_TRACE(entry.first);
            AdminQueryOutput = "7: " + alias + "@Ethernet0: " + entry.first + "\n";
            EXPECT_EQ(intfmgr.isIntfAdminStatusSet(alias, "up"), entry.second);
            EXPECT_EQ(intfmgr.isIntfAdminStatusSet(alias, "down"), !entry.second);
        }
        for (const std::string output : {"", "7: Ethernet0.10: <BROADCAST", "7: Ethernet0.10: state DOWN"})
        {
            AdminQueryOutput = output;
            EXPECT_FALSE(intfmgr.isIntfAdminStatusSet(alias, "up"));
            EXPECT_FALSE(intfmgr.isIntfAdminStatusSet(alias, "down"));
        }
        AdminQueryOutput = "7: " + alias + ": <UP> state DOWN\n";
        for (const std::string status : {"up", "down"})
        {
            MissingSubInterfaces.insert(alias);
            EXPECT_FALSE(intfmgr.isIntfAdminStatusSet(alias, status));
            MissingSubInterfaces.clear();
            AdminQueryResult = 1;
            EXPECT_FALSE(intfmgr.isIntfAdminStatusSet(alias, status));
            AdminQueryResult = 0;
            ThrowAdminQuery = true;
            EXPECT_FALSE(intfmgr.isIntfAdminStatusSet(alias, status));
            ThrowAdminQuery = false;
            ThrowAdminQueryInvalidArgument = true;
            EXPECT_FALSE(intfmgr.isIntfAdminStatusSet(alias, status));
            ThrowAdminQueryInvalidArgument = false;
        }
    }

    TEST_F(IntfMgrTest, testAdminFailureChecksKernelStateRegardlessOfReadiness)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        const std::string alias = "Ethernet0.10";
        KernelSubIntfs[alias] = {"Ethernet0", "10"};
        FailSubIntfAdmin = true;
        for (bool ready : {false, true})
        {
            SCOPED_TRACE(ready);
            if (ready)
                intfmgr.m_statePortTable.set(alias, {{"state", "ok"}});
            else
                intfmgr.m_statePortTable.del(alias);
            for (const std::string status : {"up", "down"})
            {
                SCOPED_TRACE(status);
                AdminQueryOutput = "7: " + alias + ": <BROADCAST" +
                    (status == "up" ? ",UP" : "") + "> state UNKNOWN\n";
                mockCallArgs.clear();
                EXPECT_EQ(intfmgr.setHostSubIntfAdminStatus(alias, status, "up"), status);
                ASSERT_EQ(mockCallArgs.size(), 2u);
                EXPECT_EQ(mockCallArgs[0], "/sbin/ip link set \"" + alias + "\" \"" + status + "\"");
                EXPECT_EQ(mockCallArgs[1], "/sbin/ip -o link show dev \"" + alias + "\"");

                AdminQueryOutput = "7: " + alias + ": <BROADCAST" +
                    (status == "down" ? ",UP" : "") + "> state UNKNOWN\n";
                EXPECT_THROW(intfmgr.setHostSubIntfAdminStatus(alias, status, "up"), std::runtime_error);
                AdminQueryOutput.clear();
                EXPECT_THROW(intfmgr.setHostSubIntfAdminStatus(alias, status, "up"), std::runtime_error);
                ThrowAdminQuery = true;
                EXPECT_THROW(intfmgr.setHostSubIntfAdminStatus(alias, status, "up"), std::runtime_error);
                ThrowAdminQuery = false;
            }
        }
    }

    TEST_F(IntfMgrTest, testAdminOperationReturnsBooleanForCommandAndLaunchFailures)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        const std::string alias = "Ethernet0.10";
        KernelSubIntfs[alias] = {"Ethernet0", "10"};
        for (bool ready : {false, true})
        {
            SCOPED_TRACE(ready);
            if (ready)
                intfmgr.m_statePortTable.set(alias, {{"state", "ok"}});
            else
                intfmgr.m_statePortTable.del(alias);
            for (const std::string status : {"up", "down"})
            {
                SCOPED_TRACE(status);
                EXPECT_TRUE(intfmgr.setIntfAdminStatus(alias, status));
                FailSubIntfAdmin = true;
                EXPECT_FALSE(intfmgr.setIntfAdminStatus(alias, status));
                FailSubIntfAdmin = false;
                ThrowAdminOperation = true;
                EXPECT_FALSE(intfmgr.setIntfAdminStatus(alias, status));
                ThrowAdminOperation = false;
            }
        }
        EXPECT_FALSE(commandWasIssued("-o link show"));
    }

    TEST_F(IntfMgrTest, testAdminLaunchFailureUsesStateCheckBeforeThrowing)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        const std::string alias = "Ethernet0.10";
        ThrowAdminOperation = true;
        for (const std::string status : {"up", "down"})
        {
            SCOPED_TRACE(status);
            AdminQueryOutput = "7: " + alias + ": <BROADCAST" +
                (status == "up" ? ",UP" : "") + "> state UNKNOWN\n";
            mockCallArgs.clear();
            EXPECT_EQ(intfmgr.setHostSubIntfAdminStatus(alias, status, "up"), status);
            ASSERT_EQ(mockCallArgs.size(), 2u);
            EXPECT_EQ(mockCallArgs[0], "/sbin/ip link set \"" + alias + "\" \"" + status + "\"");
            EXPECT_EQ(mockCallArgs[1], "/sbin/ip -o link show dev \"" + alias + "\"");
            AdminQueryOutput.clear();
            EXPECT_THROW(intfmgr.setHostSubIntfAdminStatus(alias, status, "up"), std::runtime_error);
            ThrowAdminQueryInvalidArgument = true;
            EXPECT_THROW(intfmgr.setHostSubIntfAdminStatus(alias, status, "up"), std::runtime_error);
            ThrowAdminQueryInvalidArgument = false;
        }
    }

    TEST_F(IntfMgrTest, testLoopbackAdminFailureRemainsBestEffortWithoutStateCheck)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        for (int failure : {0, 1, 2})
        {
            SCOPED_TRACE(failure);
            const std::string alias = "Loopback" + std::to_string(failure);
            intfmgr.m_loopbackIntfList.insert(alias);
            if (failure == 1)
                MissingSubInterfaces.insert(alias);
            ThrowAdminOperation = failure == 2;
            AdminQueryOutput = "7: " + alias + ": <UP> state UNKNOWN\n";
            mockCallArgs.clear();
            EXPECT_TRUE(intfmgr.doIntfGeneralTask({alias}, {}, SET_COMMAND));
            EXPECT_TRUE(commandWasIssued("link set \"" + alias + "\" \"up\""));
            EXPECT_FALSE(commandWasIssued("-o link show"));
            EXPECT_TRUE(intfmgr.isIntfCreated(alias));
            std::string status;
            EXPECT_EQ(app.hget(alias, "admin_status", status), failure == 0);
            if (failure == 0)
            {
                EXPECT_EQ(status, "up");
            }
        }
    }

    TEST_F(IntfMgrTest, testConfigAdminFailureAcceptsConfirmedKernelState)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto *consumer = applyChild(intfmgr);
        ASSERT_TRUE(consumer->m_toSync.empty());
        const std::string alias = "PortChannel1.20";
        notifyLag(intfmgr, "PortChannel1", DEL_COMMAND);
        notifyLag(intfmgr, "PortChannel1", SET_COMMAND,
                  {{"state", "ok"}, {"admin_status", "up"}, {"mtu", "9100"}});
        FailSubIntfAdmin = true;
        AdminQueryOutput = "7: " + alias + "@PortChannel1: <BROADCAST,UP> state DOWN\n";
        mockCallArgs.clear();
        intfmgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(commandWasIssued("link set \"" + alias + "\" \"up\""));
        EXPECT_TRUE(commandWasIssued("-o link show dev \"" + alias + "\""));
        EXPECT_EQ(intfmgr.m_subIntfList.at(alias).currAdminStatus, "up");
        EXPECT_TRUE(intfmgr.isIntfStateOk(alias));
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        std::string status;
        ASSERT_TRUE(app.hget(alias, "admin_status", status));
        EXPECT_EQ(status, "up");
    }

    TEST_F(IntfMgrTest, testParentAdminFailurePublishesConfirmedKernelState)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        const std::string parent = "Ethernet0";
        const std::string alias = "Ethernet0.10";
        intfmgr.m_subIntfList[alias] = {"10", "1500", "up", "down"};
        KernelSubIntfs[alias] = {parent, "10"};
        intfmgr.m_statePortTable.set(parent, {{"state", "ok"}, {"admin_status", "up"}});
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(STATE_PORT_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(parent, SET_COMMAND, {{"admin_status", "up"}}));
        FailSubIntfAdmin = true;
        AdminQueryOutput = "7: " + alias + "@Ethernet0: <BROADCAST,UP> state DOWN\n";
        testing_db::resetOperationCounters();
        EXPECT_NO_THROW(intfmgr.doTask(*consumer));
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_EQ(intfmgr.m_subIntfList.at(alias).currAdminStatus, "up");
        EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 1u);
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        std::string status;
        ASSERT_TRUE(app.hget(alias, "admin_status", status));
        EXPECT_EQ(status, "up");
    }

    TEST_F(IntfMgrTest, testAdminUpdateExceptionRetainsNotificationForRetry)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        const std::string parent = "Ethernet0";
        const std::string alias = "Ethernet0.10";
        intfmgr.m_subIntfList[alias] = {"10", "1500", "up", "down"};
        KernelSubIntfs[alias] = {parent, "10"};
        intfmgr.m_statePortTable.set(parent, {{"state", "ok"}, {"admin_status", "up"}});
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        app.set(alias, {{"admin_status", "down"}});
        auto *consumer = dynamic_cast<Consumer *>(intfmgr.getConsumerBase(STATE_PORT_TABLE_NAME));
        ASSERT_NE(consumer, nullptr);
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(parent, SET_COMMAND, {{"admin_status", "up"}}));
        testing_db::resetOperationCounters();
        FailSubIntfAdmin = true;

        EXPECT_THROW(intfmgr.doTask(*consumer), std::runtime_error);
        EXPECT_EQ(consumer->m_toSync.count(parent), 1u);
        EXPECT_EQ(intfmgr.m_subIntfList.at(alias).currAdminStatus, "down");
        EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 0u);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(app.get(alias, values));
        std::string value;
        ASSERT_TRUE(getFieldValue(values, "admin_status", value));
        EXPECT_EQ(value, "down");

        FailSubIntfAdmin = false;
        EXPECT_NO_THROW(intfmgr.doTask(*consumer));
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_EQ(intfmgr.m_subIntfList.at(alias).currAdminStatus, "up");
        EXPECT_EQ(testing_db::getProducerSetCount(APP_INTF_TABLE_NAME), 1u);
        ASSERT_TRUE(app.get(alias, values));
        ASSERT_TRUE(getFieldValue(values, "admin_status", value));
        EXPECT_EQ(value, "up");
    }

    // A programming failure throws even when the subinterface is not ready.
    TEST_F(IntfMgrTest, testSetAdminStatusFailToNotOkSubInt){
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        EXPECT_THROW(intfmgr.setHostSubIntfAdminStatus("Ethernet64.10", "up", "up"), std::runtime_error);
    }

    //This test except runtime error when the set admin status command failed
    //and the subinterface has ok status
    TEST_F(IntfMgrTest, testSetAdminStatusFailToOkSubInt){
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        /* Set portStateTable */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet64.10", values, "SET", "");
        EXPECT_THROW(intfmgr.setHostSubIntfAdminStatus("Ethernet64.10", "up", "up"), std::runtime_error);
    }

    TEST_F(IntfMgrTest, testReplayLLIpv6AddressOnAdminUp){
        Ethernet0IPv6Set = true;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        /* Set portStateTable and stateIntfTable so doIntfAddrTask proceeds */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");

        /* Add an IPv6 link-local address via doIntfAddrTask to populate the cache */
        const std::vector<std::string> llKeys = {"Ethernet0", "fe80::1/64"};
        const std::vector<swss::FieldValueTuple> emptyData;
        intfmgr.doIntfAddrTask(llKeys, emptyData, "SET");

        /* Also add a global IPv6 address — this should NOT be replayed */
        const std::vector<std::string> globalKeys = {"Ethernet0", "2001::8/64"};
        intfmgr.doIntfAddrTask(globalKeys, emptyData, "SET");

        /* Also add an IPv4 address — this should NOT be replayed */
        const std::vector<std::string> ipv4Keys = {"Ethernet0", "10.0.0.1/31"};
        intfmgr.doIntfAddrTask(ipv4Keys, emptyData, "SET");

        mockCallArgs.clear();

        /* Simulate admin up by calling doPortTableTask */
        std::vector<swss::FieldValueTuple> portData;
        portData.emplace_back("admin_status", "up");
        intfmgr.doPortTableTask("Ethernet0", portData, "SET");

        /* Verify that only IPv6 link-local address add was called */
        int ipv6_ll_add_called = 0;
        int ipv6_global_add_called = 0;
        int ipv4_add_called = 0;
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos &&
                cmd.find("fe80::1/64") != std::string::npos)
            {
                ipv6_ll_add_called++;
            }
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos &&
                cmd.find("2001::8/64") != std::string::npos)
            {
                ipv6_global_add_called++;
            }
            if (cmd.find("/sbin/ip address \"add\"") != std::string::npos &&
                cmd.find("10.0.0.1/31") != std::string::npos)
            {
                ipv4_add_called++;
            }
        }
        ASSERT_EQ(ipv6_ll_add_called, 1);
        ASSERT_EQ(ipv6_global_add_called, 0);
        ASSERT_EQ(ipv4_add_called, 0);

        /* Now delete the link-local address and verify it is no longer replayed */
        intfmgr.doIntfAddrTask(llKeys, emptyData, "DEL");
        ASSERT_EQ(intfmgr.m_intfLLAddresses.count("Ethernet0"), 0u);

        mockCallArgs.clear();
        intfmgr.doPortTableTask("Ethernet0", portData, "SET");

        ipv6_ll_add_called = 0;
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos &&
                cmd.find("fe80::1/64") != std::string::npos)
            {
                ipv6_ll_add_called++;
            }
        }
        ASSERT_EQ(ipv6_ll_add_called, 0);
    }

    TEST_F(IntfMgrTest, testNoReplayLLOnAdminDown){
        Ethernet0IPv6Set = true;
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        /* Set portStateTable and stateIntfTable so doIntfAddrTask proceeds */
        std::vector<swss::FieldValueTuple> values;
        values.emplace_back("state", "ok");
        intfmgr.m_statePortTable.set("Ethernet0", values, "SET", "");
        values.clear();
        values.emplace_back("vrf", "");
        intfmgr.m_stateIntfTable.set("Ethernet0", values, "SET", "");

        /* Add an IPv6 link-local address via doIntfAddrTask to populate the cache */
        const std::vector<std::string> llKeys = {"Ethernet0", "fe80::1/64"};
        const std::vector<swss::FieldValueTuple> emptyData;
        intfmgr.doIntfAddrTask(llKeys, emptyData, "SET");

        mockCallArgs.clear();

        /* Simulate admin down — should NOT trigger replay */
        std::vector<swss::FieldValueTuple> portData;
        portData.emplace_back("admin_status", "down");
        intfmgr.doPortTableTask("Ethernet0", portData, "SET");

        int ipv6_add_called = 0;
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find("/sbin/ip -6 address \"add\"") != std::string::npos)
            {
                ipv6_add_called++;
            }
        }
        ASSERT_EQ(ipv6_add_called, 0);
    }

    TEST_F(IntfMgrTest, testFailedAddressProgrammingRemainsPending)
    {
        verifyFailedAddressProgrammingRemainsPending("10.10.192.33/31");
    }

    TEST_F(IntfMgrTest, testFailedIpv6AddressProgrammingRemainsPending)
    {
        verifyFailedAddressProgrammingRemainsPending("2001:db8::1/128");
    }

    TEST_F(IntfMgrTest, testIpv6EnableFailureRetainsTask)
    {
        verifyFailedAddressProgrammingRemainsPending("2001:db8::1/128", true);
    }

    TEST_F(IntfMgrTest, testSetSagFdbEntryValidationAndBridgeCommand){
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        mockCallArgs.clear();
        intfmgr.setSagFdbEntry("update", "Vlan100", "02:03:04:05:06:07");
        intfmgr.setSagFdbEntry("replace", "Ethernet0", "02:03:04:05:06:07");
        intfmgr.setSagFdbEntry("replace", "VlanABC", "02:03:04:05:06:07");
        intfmgr.setSagFdbEntry("replace", "Vlan100", gMacAddress.to_string());
        EXPECT_TRUE(mockCallArgs.empty());

        FailBridgeFdbCommand = true;
        intfmgr.setSagFdbEntry("replace", "Vlan100", "02:03:04:05:06:07");
        ASSERT_EQ(mockCallArgs.size(), 1u);
        EXPECT_EQ(mockCallArgs[0], "bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent");
    }

    TEST_F(IntfMgrTest, testUpdateSagMacProgramsSagVlans){
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        gSagMacAddress = swss::MacAddress("00:aa:bb:cc:dd:ee");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        intfmgr.m_cfgVlanIntfTable.set("Vlan100", {
            {"static_anycast_gateway", "true"},
            {"proxy_arp", "enabled"}
        });
        intfmgr.m_cfgVlanIntfTable.set("Vlan200", {
            {"static_anycast_gateway", "false"}
        });
        intfmgr.m_cfgVlanIntfTable.set("Vlan300|10.0.0.1/24", {
            {"static_anycast_gateway", "true"}
        });

        mockCallArgs.clear();
        intfmgr.updateSagMac("02:03:04:05:06:07");

        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" down"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 02:03:04:05:06:07"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" up"));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 00:aa:bb:cc:dd:ee dev Bridge vlan 100 permanent"));
        EXPECT_TRUE(commandWasIssued("bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));
        EXPECT_FALSE(commandWasIssued("Vlan200 address"));
        EXPECT_FALSE(commandWasIssued("Vlan300"));

        swss::Table appIntfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(appIntfTable.get("Vlan100", values));
        std::string mac;
        ASSERT_TRUE(getFieldValue(values, "mac_addr", mac));
        EXPECT_EQ(mac, "02:03:04:05:06:07");
    }

    TEST_F(IntfMgrTest, testDoSagTaskSetAndDelete){
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        gSagMacAddress = swss::MacAddress("00:00:00:00:00:00");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        intfmgr.m_cfgVlanIntfTable.set("Vlan100", {
            {"static_anycast_gateway", "true"}
        });

        const std::vector<std::string> keys = {"GLOBAL"};
        mockCallArgs.clear();
        intfmgr.doSagTask(keys, {}, SET_COMMAND);
        EXPECT_TRUE(mockCallArgs.empty());

        intfmgr.doSagTask(keys, {{"gateway_mac", "02:03:04:05:06:07"}}, SET_COMMAND);
        EXPECT_TRUE(commandWasIssued("bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));

        swss::Table appSagTable(m_app_db.get(), APP_SAG_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(appSagTable.get("GLOBAL", values));
        std::string mac;
        ASSERT_TRUE(getFieldValue(values, "gateway_mac", mac));
        EXPECT_EQ(mac, "02:03:04:05:06:07");

        mockCallArgs.clear();
        intfmgr.doSagTask(keys, {}, DEL_COMMAND);
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 00:11:22:33:44:55"));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));
        EXPECT_FALSE(commandWasIssued("bridge fdb replace 00:11:22:33:44:55"));
        EXPECT_FALSE(appSagTable.get("GLOBAL", values));

        intfmgr.doSagTask(keys, {}, "UNKNOWN");
    }

    TEST_F(IntfMgrTest, testDoIntfGeneralTaskStaticAnycastGateway)
    {
        gMacAddress = swss::MacAddress("00:11:22:33:44:55");
        gSagMacAddress = swss::MacAddress("00:aa:bb:cc:dd:ee");
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);

        intfmgr.m_stateVlanTable.set("Vlan100", {{"state", "ok"}}, "SET", "");
        intfmgr.m_stateVlanTable.set("Vlan200", {{"state", "ok"}}, "SET", "");
        intfmgr.m_cfgSagTable.set("GLOBAL", {{"gateway_mac", "02:03:04:05:06:07"}});

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan100"}, {{"static_anycast_gateway", "true"}}, SET_COMMAND));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" down"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 02:03:04:05:06:07"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set \"Vlan100\" up"));
        EXPECT_TRUE(commandWasIssued("bridge fdb replace 02:03:04:05:06:07 dev Bridge vlan 100 permanent"));
        EXPECT_TRUE(intfmgr.m_sagIntfList.at("Vlan100"));

        swss::Table appIntfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
        std::vector<swss::FieldValueTuple> values;
        ASSERT_TRUE(appIntfTable.get("Vlan100", values));
        std::string mac;
        ASSERT_TRUE(getFieldValue(values, "mac_addr", mac));
        EXPECT_EQ(mac, "02:03:04:05:06:07");

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan100"}, {}, DEL_COMMAND));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 00:aa:bb:cc:dd:ee dev Bridge vlan 100 permanent"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan100 address 00:11:22:33:44:55"));
        EXPECT_EQ(intfmgr.m_sagIntfList.count("Vlan100"), 0u);

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan200"}, {{"static_anycast_gateway", "false"}}, SET_COMMAND));
        EXPECT_TRUE(commandWasIssued("bridge fdb del 00:aa:bb:cc:dd:ee dev Bridge vlan 200 permanent"));
        EXPECT_TRUE(commandWasIssued("/sbin/ip link set Vlan200 address 00:11:22:33:44:55"));
        ASSERT_TRUE(appIntfTable.get("Vlan200", values));
        ASSERT_TRUE(getFieldValue(values, "mac_addr", mac));
        EXPECT_EQ(mac, swss::MacAddress().to_string());

        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Vlan200"}, {{"static_anycast_gateway", "invalid"}}, SET_COMMAND));
        EXPECT_FALSE(commandWasIssued("bridge fdb"));
    }

}
