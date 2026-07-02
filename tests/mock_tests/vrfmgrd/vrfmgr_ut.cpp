#include "gtest/gtest.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../mock_table.h"

#define private public
#include "warm_restart.h"
#include "vrfmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;

namespace
{

    constexpr auto KERNEL_VRF_FALLBACK_TABLE = "KERNEL_VRF_FALLBACK";
    constexpr auto KERNEL_VRF_FALLBACK_KEY = "GLOBAL";
    constexpr auto SENTINEL_METRIC = "4278198272";

    std::string existingVrfOutput;
    std::string failingCommand;
    std::string throwingCommand;
    int remainingFailures;
    std::string routeShowOutput;
    int routeShowReturnCode;

    int commandCallback(const std::string &cmd, std::string &output)
    {
        mockCallArgs.push_back(cmd);
        output.clear();

        if (cmd == "/sbin/ip -d link show type vrf")
        {
            output = existingVrfOutput;
            return 0;
        }
        if (cmd == "/sbin/ip rule | grep '^0:'")
        {
            return 1;
        }
        if (cmd.find(" route show table ") != std::string::npos)
        {
            output = routeShowOutput;
            return routeShowReturnCode;
        }
        if (!throwingCommand.empty() && cmd == throwingCommand)
        {
            throw std::runtime_error("injected command failure");
        }
        if (remainingFailures > 0 && cmd == failingCommand)
        {
            --remainingFailures;
            return 2;
        }

        return 0;
    }

    bool commandWasIssued(const std::string &command)
    {
        return std::find(mockCallArgs.begin(), mockCallArgs.end(), command) != mockCallArgs.end();
    }

    class TestableVrfMgr : public swss::VrfMgr
    {
    public:
        using VrfMgr::VrfMgr;

        swss::Consumer *getConsumer(const std::string &tableName)
        {
            return dynamic_cast<swss::Consumer *>(getExecutor(tableName));
        }
    };

    class VrfMgrFallbackTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            testing_db::reset();
            mockCallArgs.clear();
            existingVrfOutput.clear();
            failingCommand.clear();
            throwingCommand.clear();
            remainingFailures = 0;
            routeShowOutput.clear();
            routeShowReturnCode = 0;
            callback = commandCallback;
            swss::WarmStart::getInstance().m_enabled = false;

            configDb = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            appDb = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            stateDb = std::make_shared<swss::DBConnector>("STATE_DB", 0);
            swss::WarmStart::initialize("vrfmgrd", "swss");
        }

        void TearDown() override
        {
            swss::WarmStart::getInstance().m_enabled = false;
            callback = nullptr;
            testing_db::reset();
        }

        std::unique_ptr<TestableVrfMgr> makeManager()
        {
            const std::vector<std::string> tables = {
                CFG_VRF_TABLE_NAME,
                CFG_VNET_TABLE_NAME,
                CFG_VXLAN_EVPN_NVO_TABLE_NAME,
                CFG_MGMT_VRF_CONFIG_TABLE_NAME,
                KERNEL_VRF_FALLBACK_TABLE,
            };
            return std::make_unique<TestableVrfMgr>(configDb.get(), appDb.get(), stateDb.get(), tables);
        }

        void setFallbackConfig(const std::string &status)
        {
            swss::Table table(configDb.get(), KERNEL_VRF_FALLBACK_TABLE);
            table.set(KERNEL_VRF_FALLBACK_KEY, { { "status", status } });
        }

        void enqueueFallbackEvent(TestableVrfMgr &manager, const std::string &op,
                                  const std::vector<swss::FieldValueTuple> &values = {},
                                  const std::string &key = KERNEL_VRF_FALLBACK_KEY)
        {
            auto *consumer = manager.getConsumer(KERNEL_VRF_FALLBACK_TABLE);
            ASSERT_NE(consumer, nullptr);
            consumer->m_toSync[key] = swss::KeyOpFieldsValuesTuple(key, op, values);
            manager.doTask();
        }

        std::shared_ptr<swss::DBConnector> configDb;
        std::shared_ptr<swss::DBConnector> appDb;
        std::shared_ptr<swss::DBConnector> stateDb;
    };

    TEST_F(VrfMgrFallbackTest, AbsentConfigInstallsDualStackSentinels)
    {
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        mockCallArgs.clear();

        EXPECT_TRUE(manager->reconcileKernelVrf("VrfBlue"));

        EXPECT_TRUE(commandWasIssued("/sbin/ip route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_TRUE(commandWasIssued("/sbin/ip -6 route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
    }

    TEST_F(VrfMgrFallbackTest, EnabledConfigDeletesOnlyExactDualStackSentinels)
    {
        setFallbackConfig("enabled");
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        mockCallArgs.clear();

        EXPECT_TRUE(manager->reconcileKernelVrf("VrfBlue"));

        EXPECT_TRUE(commandWasIssued("/sbin/ip route del table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_TRUE(commandWasIssued("/sbin/ip -6 route del table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_FALSE(commandWasIssued("/sbin/ip route del table 1001 default"));
    }

    TEST_F(VrfMgrFallbackTest, ConfigDeleteRestoresDefaultDisabledState)
    {
        setFallbackConfig("enabled");
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        mockCallArgs.clear();

        enqueueFallbackEvent(*manager, DEL_COMMAND);

        EXPECT_FALSE(manager->m_kernelVrfFallbackEnabled);
        EXPECT_TRUE(commandWasIssued("/sbin/ip route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_TRUE(commandWasIssued("/sbin/ip -6 route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
    }

    TEST_F(VrfMgrFallbackTest, ExplicitDisabledConfigReconcilesAllVrfs)
    {
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        mockCallArgs.clear();

        enqueueFallbackEvent(*manager, SET_COMMAND, { { "status", "disabled" } });

        EXPECT_FALSE(manager->m_kernelVrfFallbackEnabled);
        EXPECT_TRUE(commandWasIssued("/sbin/ip route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_TRUE(commandWasIssued("/sbin/ip -6 route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
    }

    TEST_F(VrfMgrFallbackTest, ExplicitEnabledConfigRemovesSentinelsFromAllVrfs)
    {
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        mockCallArgs.clear();

        enqueueFallbackEvent(*manager, SET_COMMAND, { { "status", "enabled" } });

        EXPECT_TRUE(manager->m_kernelVrfFallbackEnabled);
        EXPECT_TRUE(commandWasIssued("/sbin/ip route del table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_TRUE(commandWasIssued("/sbin/ip -6 route del table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
    }

    TEST_F(VrfMgrFallbackTest, InvalidConfigUsesDefaultDisabledState)
    {
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        mockCallArgs.clear();

        enqueueFallbackEvent(*manager, SET_COMMAND, { { "status", "invalid" } });

        EXPECT_FALSE(manager->m_kernelVrfFallbackEnabled);
        EXPECT_TRUE(commandWasIssued("/sbin/ip route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
    }

    TEST_F(VrfMgrFallbackTest, MissingStatusUsesDefaultDisabledState)
    {
        setFallbackConfig("enabled");
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        mockCallArgs.clear();

        enqueueFallbackEvent(*manager, SET_COMMAND, { { "other", "value" } });

        EXPECT_FALSE(manager->m_kernelVrfFallbackEnabled);
        EXPECT_TRUE(commandWasIssued("/sbin/ip route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
    }

    TEST_F(VrfMgrFallbackTest, NonGlobalKeyIsIgnored)
    {
        setFallbackConfig("enabled");
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        mockCallArgs.clear();

        enqueueFallbackEvent(*manager, SET_COMMAND, { { "status", "disabled" } }, "NOT_GLOBAL");

        EXPECT_TRUE(manager->m_kernelVrfFallbackEnabled);
        EXPECT_TRUE(mockCallArgs.empty());
    }

    TEST_F(VrfMgrFallbackTest, GlobalReconciliationSkipsManagementVrf)
    {
        auto manager = makeManager();
        manager->m_vrfTableMap["mgmt"] = 6000;
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        mockCallArgs.clear();

        manager->reconcileAllKernelVrfs();

        EXPECT_TRUE(commandWasIssued("/sbin/ip route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        for (const auto &command : mockCallArgs)
        {
            EXPECT_EQ(command.find("table 6000"), std::string::npos);
        }
    }

    TEST_F(VrfMgrFallbackTest, NewVrfReceivesSentinelsInDefaultState)
    {
        auto manager = makeManager();
        auto *consumer = manager->getConsumer(CFG_VRF_TABLE_NAME);
        ASSERT_NE(consumer, nullptr);
        mockCallArgs.clear();
        consumer->m_toSync["VrfNew"] =
            swss::KeyOpFieldsValuesTuple("VrfNew", SET_COMMAND, { { "empty", "empty" } });

        manager->doTask();

        EXPECT_TRUE(commandWasIssued("/sbin/ip route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_TRUE(commandWasIssued("/sbin/ip -6 route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
    }

    TEST_F(VrfMgrFallbackTest, NewVnetReceivesSentinelsInDefaultState)
    {
        auto manager = makeManager();
        auto *consumer = manager->getConsumer(CFG_VNET_TABLE_NAME);
        ASSERT_NE(consumer, nullptr);
        mockCallArgs.clear();
        consumer->m_toSync["VnetNew"] =
            swss::KeyOpFieldsValuesTuple("VnetNew", SET_COMMAND, { { "empty", "empty" } });

        manager->doTask();

        EXPECT_TRUE(commandWasIssued("/sbin/ip route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_TRUE(commandWasIssued("/sbin/ip -6 route replace table 1001 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
    }

    TEST_F(VrfMgrFallbackTest, RepeatedDisabledReconciliationIsIdempotent)
    {
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        const std::string ipv4Command =
            "/sbin/ip route replace table 1001 unreachable default metric " +
            std::string(SENTINEL_METRIC);
        const std::string ipv6Command =
            "/sbin/ip -6 route replace table 1001 unreachable default metric " +
            std::string(SENTINEL_METRIC);
        mockCallArgs.clear();

        EXPECT_TRUE(manager->reconcileKernelVrf("VrfBlue"));
        EXPECT_TRUE(manager->reconcileKernelVrf("VrfBlue"));

        EXPECT_EQ(std::count(mockCallArgs.begin(), mockCallArgs.end(), ipv4Command), 2);
        EXPECT_EQ(std::count(mockCallArgs.begin(), mockCallArgs.end(), ipv6Command), 2);
        EXPECT_TRUE(manager->m_pendingKernelVrfReconcile.empty());
    }

    TEST_F(VrfMgrFallbackTest, StartupReadsEnabledConfigBeforeReconcilingRetainedVrf)
    {
        setFallbackConfig("enabled");
        existingVrfOutput =
            "5: VrfRetained: <NOARP,MASTER,UP> mtu 65536 qdisc noqueue state UP mode DEFAULT\n"
            "    link/ether 02:42:ac:11:00:02 brd ff:ff:ff:ff:ff:ff\n"
            "    vrf table 1007 addrgenmode eui64 numtxqueues 1 numrxqueues 1\n";
        swss::WarmStart::getInstance().m_enabled = true;

        auto manager = makeManager();

        EXPECT_EQ(manager->m_vrfTableMap.at("VrfRetained"), 1007u);
        EXPECT_TRUE(commandWasIssued("/sbin/ip route del table 1007 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_TRUE(commandWasIssued("/sbin/ip -6 route del table 1007 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_FALSE(commandWasIssued("/sbin/ip route replace table 1007 unreachable default metric " +
                                      std::string(SENTINEL_METRIC)));
    }

    TEST_F(VrfMgrFallbackTest, StartupUsesDefaultDisabledForRetainedVrfWhenConfigAbsent)
    {
        existingVrfOutput =
            "5: VrfRetained: <NOARP,MASTER,UP> mtu 65536 qdisc noqueue state UP mode DEFAULT\n"
            "    link/ether 02:42:ac:11:00:02 brd ff:ff:ff:ff:ff:ff\n"
            "    vrf table 1007 addrgenmode eui64 numtxqueues 1 numrxqueues 1\n";
        swss::WarmStart::getInstance().m_enabled = true;

        auto manager = makeManager();

        EXPECT_FALSE(manager->m_kernelVrfFallbackEnabled);
        EXPECT_TRUE(commandWasIssued("/sbin/ip route replace table 1007 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_TRUE(commandWasIssued("/sbin/ip -6 route replace table 1007 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
    }

    TEST_F(VrfMgrFallbackTest, FailedRouteOperationIsRetried)
    {
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        failingCommand = "/sbin/ip -6 route replace table 1001 unreachable default metric " +
                         std::string(SENTINEL_METRIC);
        remainingFailures = 1;
        mockCallArgs.clear();

        EXPECT_FALSE(manager->reconcileKernelVrf("VrfBlue"));
        EXPECT_EQ(manager->m_pendingKernelVrfReconcile.count("VrfBlue"), 1u);

        manager->retryPendingKernelVrfs();

        EXPECT_TRUE(manager->m_pendingKernelVrfReconcile.empty());
        EXPECT_EQ(std::count(mockCallArgs.begin(), mockCallArgs.end(), failingCommand), 2);
    }

    TEST_F(VrfMgrFallbackTest, RetryUsesCurrentTableAndDesiredState)
    {
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        failingCommand = "/sbin/ip -6 route replace table 1001 unreachable default metric " +
                         std::string(SENTINEL_METRIC);
        remainingFailures = 1;

        EXPECT_FALSE(manager->reconcileKernelVrf("VrfBlue"));
        manager->m_vrfTableMap["VrfBlue"] = 1002;
        manager->m_kernelVrfFallbackEnabled = true;
        mockCallArgs.clear();

        manager->retryPendingKernelVrfs();

        EXPECT_TRUE(commandWasIssued("/sbin/ip route del table 1002 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_TRUE(commandWasIssued("/sbin/ip -6 route del table 1002 unreachable default metric " +
                                     std::string(SENTINEL_METRIC)));
        EXPECT_TRUE(manager->m_pendingKernelVrfReconcile.empty());
    }

    TEST_F(VrfMgrFallbackTest, MissingExactSentinelIsSuccessfulDuringDelete)
    {
        setFallbackConfig("enabled");
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        failingCommand = "/sbin/ip route del table 1001 unreachable default metric " +
                         std::string(SENTINEL_METRIC);
        remainingFailures = 1;
        mockCallArgs.clear();

        EXPECT_TRUE(manager->reconcileKernelVrf("VrfBlue"));

        EXPECT_TRUE(commandWasIssued("/sbin/ip route show table 1001 type unreachable"));
        EXPECT_TRUE(manager->m_pendingKernelVrfReconcile.empty());
    }

    TEST_F(VrfMgrFallbackTest, DifferentMetricUnreachableRouteIsNotManaged)
    {
        setFallbackConfig("enabled");
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        failingCommand = "/sbin/ip route del table 1001 unreachable default metric " +
                         std::string(SENTINEL_METRIC);
        remainingFailures = 1;
        routeShowOutput = "unreachable default metric 1234\n";
        mockCallArgs.clear();

        EXPECT_TRUE(manager->reconcileKernelVrf("VrfBlue"));

        EXPECT_TRUE(manager->m_pendingKernelVrfReconcile.empty());
    }

    TEST_F(VrfMgrFallbackTest, RouteShowFailureLeavesVrfPendingForRetry)
    {
        setFallbackConfig("enabled");
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        failingCommand = "/sbin/ip route del table 1001 unreachable default metric " +
                         std::string(SENTINEL_METRIC);
        remainingFailures = 1;
        routeShowReturnCode = 2;
        mockCallArgs.clear();

        EXPECT_FALSE(manager->reconcileKernelVrf("VrfBlue"));

        EXPECT_EQ(manager->m_pendingKernelVrfReconcile.count("VrfBlue"), 1u);
    }

    TEST_F(VrfMgrFallbackTest, ExactSentinelStillPresentLeavesVrfPendingForRetry)
    {
        setFallbackConfig("enabled");
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        failingCommand = "/sbin/ip route del table 1001 unreachable default metric " +
                         std::string(SENTINEL_METRIC);
        remainingFailures = 1;
        routeShowOutput = "unreachable default metric 4278198272\n";
        mockCallArgs.clear();

        EXPECT_FALSE(manager->reconcileKernelVrf("VrfBlue"));

        EXPECT_EQ(manager->m_pendingKernelVrfReconcile.count("VrfBlue"), 1u);
    }

    TEST_F(VrfMgrFallbackTest, CommandExceptionLeavesVrfPendingForTimerRetry)
    {
        auto manager = makeManager();
        manager->m_vrfTableMap["VrfBlue"] = 1001;
        throwingCommand = "/sbin/ip route replace table 1001 unreachable default metric " +
                          std::string(SENTINEL_METRIC);
        mockCallArgs.clear();

        bool result = true;
        EXPECT_NO_THROW(result = manager->reconcileKernelVrf("VrfBlue"));

        EXPECT_FALSE(result);
        EXPECT_EQ(manager->m_pendingKernelVrfReconcile.count("VrfBlue"), 1u);
    }

} // namespace
