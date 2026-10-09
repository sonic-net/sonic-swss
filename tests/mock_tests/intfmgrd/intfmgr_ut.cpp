#include "gtest/gtest.h"
#include <iostream>
#include <fstream>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include "../mock_table.h"
#include "warm_restart.h"
#include "macaddress.h"
#include <cerrno>
#define private public
#include "intfmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;
extern swss::MacAddress gMacAddress;
extern swss::MacAddress gSagMacAddress;

bool Ethernet0IPv6Set = false;
unsigned MockIfIndex = 42;
int MockIfIndexError = ENODEV;
int KernelBindingResult = 0;
extern "C" unsigned __wrap_if_nametoindex(const char *)
{
    errno = MockIfIndex ? 0 : MockIfIndexError;
    return MockIfIndex;
}
bool FailBridgeFdbCommand = false;
int LinkLocalFlushResult = 0;

int cb(const std::string &cmd, std::string &stdout){
    mockCallArgs.push_back(cmd);
    if (cmd.find(" nomaster") != std::string::npos || cmd.find(" master ") != std::string::npos)
    {
        return KernelBindingResult;
    }
    if (cmd == "sysctl -w net.ipv6.conf.\"Ethernet0\".disable_ipv6=0") Ethernet0IPv6Set = true;
    else if (cmd.find("/sbin/ip -6 address \"add\"") == 0) {
        return Ethernet0IPv6Set ? 0 : 2;
    }
    else if (cmd == "/sbin/ip link set \"Ethernet64.10\" \"up\""){
        return 1;
    }
    else if (cmd.find("/sbin/ip address show ") == 0) {
        stdout = "0\n";
        return 0;
    }
    else if (cmd.find("bridge fdb") == 0) {
        return FailBridgeFdbCommand ? 1 : 0;
    }
    else if (cmd.find("/sbin/ip -6 neigh flush") == 0) {
        return LinkLocalFlushResult;
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
            MockIfIndex = 42;
            MockIfIndexError = ENODEV;
            KernelBindingResult = 0;
            FailBridgeFdbCommand = false;
            LinkLocalFlushResult = 0;
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

    TEST_F(IntfMgrTest, RetryPacingMatchingAckDoesNotRepublishOnKernelFailure)
    {
        for (const std::string status : {"guarded", "retired"})
        {
            SCOPED_TRACE(status);
            testing_db::reset();
            swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
            mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
            mgr.m_stateVrfTable.set("VrfBlue", {{"state", "ok"}});
            auto consumer = dynamic_cast<Consumer *>(mgr.getConsumerBase(CFG_INTF_TABLE_NAME));
            ASSERT_NE(nullptr, consumer);
            consumer->addToSync(KeyOpFieldsValuesTuple{"Ethernet0", SET_COMMAND, {{"vrf_name", "VrfBlue"}}});
            mgr.doTask(*consumer);
            ASSERT_EQ(1u, consumer->m_toSync.size());
            swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
            std::string id, value;
            ASSERT_TRUE(guard.hget("Ethernet0", "request_id", id));
            guard.set("Ethernet0", {{"id", id}, {"state", status}});
            const auto published = testing_db::getProducerSetCount("INTF_GUARD_TABLE");
            KernelBindingResult = 2;
            for (unsigned retry = 0; retry < 4; ++retry)
            {
                mockCallArgs.clear();
                mgr.doTask(*consumer);
                EXPECT_EQ(1u, consumer->m_toSync.size());
                EXPECT_TRUE(commandWasIssued(" master "));
                EXPECT_EQ(published, testing_db::getProducerSetCount("INTF_GUARD_TABLE"));
                ASSERT_TRUE(guard.hget("Ethernet0", "request_id", value));
                EXPECT_EQ(id, value);
            }
            // No notification delivered: the next ordinary sweep reads retained STATE.
            KernelBindingResult = 0;
            mgr.doTask(*consumer);
            EXPECT_TRUE(consumer->m_toSync.empty());
            ASSERT_TRUE(guard.hget("Ethernet0", "action", value));
            EXPECT_EQ("applied", value);
            ASSERT_TRUE(guard.hget("Ethernet0", "applied_id", value));
            EXPECT_EQ(id, value);
            EXPECT_EQ(published + 1, testing_db::getProducerSetCount("INTF_GUARD_TABLE"));
            ASSERT_TRUE(mgr.m_stateIntfTable.hget("Ethernet0", "vrf", value));
            EXPECT_EQ("VrfBlue", value);
        }
    }

    TEST_F(IntfMgrTest, RetryPacingAbsentStaleAckAndNewGenerationStillPublish)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        std::string id, value;
        EXPECT_FALSE(mgr.prepareBinding("Ethernet0", "VrfBlue"));
        ASSERT_TRUE(guard.hget("Ethernet0", "request_id", id));
        EXPECT_EQ("1", id);
        EXPECT_EQ(1u, testing_db::getProducerSetCount("INTF_GUARD_TABLE"));
        for (unsigned retry = 0; retry < 3; ++retry)
            EXPECT_FALSE(mgr.prepareBinding("Ethernet0", "VrfBlue"));
        EXPECT_EQ(4u, testing_db::getProducerSetCount("INTF_GUARD_TABLE"));
        guard.set("Ethernet0", {{"id", "0"}, {"state", "guarded"}});
        for (unsigned retry = 0; retry < 3; ++retry)
            EXPECT_FALSE(mgr.prepareBinding("Ethernet0", "VrfBlue"));
        EXPECT_EQ(7u, testing_db::getProducerSetCount("INTF_GUARD_TABLE"));
        guard.set("Ethernet0", {{"id", id}, {"state", "cancelled"}});
        EXPECT_FALSE(mgr.prepareBinding("Ethernet0", "VrfBlue"));
        EXPECT_EQ(8u, testing_db::getProducerSetCount("INTF_GUARD_TABLE"));
        guard.set("Ethernet0", {{"state", "guarded"}});
        // Old valid ack must not acknowledge a changed target.
        EXPECT_FALSE(mgr.prepareBinding("Ethernet0", "VrfRed"));
        ASSERT_TRUE(guard.hget("Ethernet0", "request_id", value));
        EXPECT_EQ("2", value);
        EXPECT_EQ(9u, testing_db::getProducerSetCount("INTF_GUARD_TABLE"));
        guard.set("Ethernet0", {{"id", value}, {"state", "retired"}, {"action", "applied"}});
        EXPECT_FALSE(mgr.prepareBinding("Ethernet0", "VrfRed"));
        ASSERT_TRUE(guard.hget("Ethernet0", "request_id", value));
        EXPECT_EQ("3", value);
        EXPECT_EQ(10u, testing_db::getProducerSetCount("INTF_GUARD_TABLE"));
    }

    TEST_F(IntfMgrTest, RetryPacingRestartRetainedAckWithoutWakeConverges)
    {
        swss::Table config(m_config_db.get(), CFG_INTF_TABLE_NAME);
        swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        const std::vector<swss::FieldValueTuple> data{{"vrf_name", "VrfBlue"}};
        std::string id, value;
        {
            swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
            mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
            mgr.m_stateVrfTable.set("VrfBlue", {{"state", "ok"}});
            config.set("Ethernet0", data);
            EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
            ASSERT_TRUE(guard.hget("Ethernet0", "request_id", id));
            guard.set("Ethernet0", {{"id", id}, {"state", "guarded"}});
            KernelBindingResult = 2;
            EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        }
        // Reconstruct only the manager; CONFIG and STATE survive and no wake is sent.
        swss::IntfMgr restarted(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto consumer = dynamic_cast<Consumer *>(restarted.getConsumerBase(CFG_INTF_TABLE_NAME));
        ASSERT_NE(nullptr, consumer);
        consumer->addToSync(KeyOpFieldsValuesTuple{"Ethernet0", SET_COMMAND, data});
        const auto published = testing_db::getProducerSetCount("INTF_GUARD_TABLE");
        restarted.doTask(*consumer);
        EXPECT_EQ(1u, consumer->m_toSync.size());
        EXPECT_EQ(published, testing_db::getProducerSetCount("INTF_GUARD_TABLE"));
        KernelBindingResult = 0;
        restarted.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_EQ(published + 1, testing_db::getProducerSetCount("INTF_GUARD_TABLE"));
        ASSERT_TRUE(guard.hget("Ethernet0", "applied_id", value));
        EXPECT_EQ(id, value);
        ASSERT_TRUE(guard.hget("Ethernet0", "kernel_pending", value));
        EXPECT_EQ("0", value);
        ASSERT_TRUE(restarted.m_stateIntfTable.hget("Ethernet0", "vrf", value));
        EXPECT_EQ("VrfBlue", value);
    }

    TEST_F(IntfMgrTest, RestartSameVrfReaddCancelsUnstartedRemovalPreservingRetirement)
    {
        swss::Table config(m_config_db.get(), CFG_INTF_TABLE_NAME);
        swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        swss::Table appGuard(m_app_db.get(), "INTF_GUARD_TABLE");
        const std::vector<swss::FieldValueTuple> data{{"vrf_name", "VrfBlue"}};
        std::string bindId, removeId, value;
        {
            swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
            mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
            mgr.m_stateVrfTable.set("VrfBlue", {{"state", "ok"}});
            config.set("Ethernet0", data);
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
            ASSERT_TRUE(guard.hget("Ethernet0", "request_id", bindId));
            guard.set("Ethernet0", {{"id", bindId}, {"state", "guarded"}, {"retired_id", "0"}});
            ASSERT_TRUE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
            config.del("Ethernet0");
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, {}, DEL_COMMAND));
            ASSERT_TRUE(guard.hget("Ethernet0", "request_id", removeId));
            ASSERT_NE(bindId, removeId);
            guard.set("Ethernet0", {{"id", removeId}, {"state", "guarded"}});
            // Restart after the ack, before the first kernel removal attempt.
            config.set("Ethernet0", data);
        }
        mockCallArgs.clear();
        swss::IntfMgr restarted(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        ASSERT_TRUE(restarted.isIntfCreated("Ethernet0"));
        ASSERT_TRUE(guard.hget("Ethernet0", "kernel_pending", value));
        ASSERT_EQ("0", value);
        ASSERT_TRUE(restarted.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        ASSERT_TRUE(guard.hget("Ethernet0", "action", value));
        EXPECT_EQ("cancel", value);
        ASSERT_TRUE(appGuard.hget("Ethernet0", "action", value));
        EXPECT_EQ("cancel", value);
        ASSERT_TRUE(appGuard.hget("Ethernet0", "id", value));
        EXPECT_EQ(removeId, value);
        ASSERT_TRUE(guard.hget("Ethernet0", "request_id", value));
        EXPECT_EQ(removeId, value);
        ASSERT_TRUE(guard.hget("Ethernet0", "applied_id", value));
        EXPECT_EQ(bindId, value);
        ASSERT_TRUE(guard.hget("Ethernet0", "applied_vrf", value));
        EXPECT_EQ("VrfBlue", value); // Cancellation cannot erase earlier retirement work.
        ASSERT_TRUE(guard.hget("Ethernet0", "retired_id", value));
        EXPECT_EQ("0", value); // IntfMgr must not forge an orchagent retirement ack.
        ASSERT_TRUE(restarted.m_stateIntfTable.hget("Ethernet0", "vrf", value));
        EXPECT_EQ("VrfBlue", value);
        EXPECT_FALSE(commandWasIssued(" nomaster"));
    }

    TEST_F(IntfMgrTest, RestartSameVrfReaddCompletesUncertainKernelMutationBeforeApplied)
    {
        swss::Table config(m_config_db.get(), CFG_INTF_TABLE_NAME);
        swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        swss::Table appGuard(m_app_db.get(), "INTF_GUARD_TABLE");
        const std::vector<swss::FieldValueTuple> data{{"vrf_name", "VrfBlue"}};
        std::string bindId, removeId, replayId, value;
        {
            swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
            mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
            mgr.m_stateVrfTable.set("VrfBlue", {{"state", "ok"}});
            config.set("Ethernet0", data);
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
            ASSERT_TRUE(guard.hget("Ethernet0", "request_id", bindId));
            guard.set("Ethernet0", {{"id", bindId}, {"state", "guarded"}, {"retired_id", "0"}});
            ASSERT_TRUE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
            config.del("Ethernet0");
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, {}, DEL_COMMAND));
            ASSERT_TRUE(guard.hget("Ethernet0", "request_id", removeId));
            guard.set("Ethernet0", {{"id", removeId}, {"state", "guarded"}});
            mgr.m_ipv6LinkLocalModeList.insert("Ethernet0");
            LinkLocalFlushResult = 1;
            mockCallArgs.clear();
            // nomaster succeeds, but checked cleanup fails before STATE deletion.
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, {}, DEL_COMMAND));
            ASSERT_TRUE(commandWasIssued(" nomaster"));
            ASSERT_TRUE(mgr.isIntfCreated("Ethernet0"));
            ASSERT_TRUE(guard.hget("Ethernet0", "kernel_pending", value));
            ASSERT_EQ("1", value);
            config.set("Ethernet0", data);
        }
        LinkLocalFlushResult = 0;
        mockCallArgs.clear();
        testing_db::resetOperationCounters();
        swss::IntfMgr restarted(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        ASSERT_TRUE(restarted.isIntfCreated("Ethernet0"));
        // Existing STATE must not suppress the fence for unfinished kernel work.
        ASSERT_FALSE(restarted.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        EXPECT_FALSE(commandWasIssued(" master "));
        ASSERT_TRUE(guard.hget("Ethernet0", "request_id", replayId));
        EXPECT_NE(removeId, replayId); // Old DEL ack cannot authorize the new target.
        ASSERT_TRUE(guard.hget("Ethernet0", "target_vrf", value));
        EXPECT_EQ("VrfBlue", value);
        guard.set("Ethernet0", {{"id", replayId}, {"state", "guarded"}});
        KernelBindingResult = 2;
        ASSERT_FALSE(restarted.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        EXPECT_TRUE(commandWasIssued(" master "));
        ASSERT_TRUE(guard.hget("Ethernet0", "action", value));
        EXPECT_EQ("prepare", value);
        ASSERT_TRUE(appGuard.hget("Ethernet0", "action", value));
        EXPECT_EQ("prepare", value);
        ASSERT_TRUE(guard.hget("Ethernet0", "applied_id", value));
        EXPECT_EQ(bindId, value);
        ASSERT_TRUE(guard.hget("Ethernet0", "retired_id", value));
        EXPECT_EQ("0", value);
        ASSERT_TRUE(guard.hget("Ethernet0", "kernel_pending", value));
        EXPECT_EQ("1", value);
        EXPECT_EQ(0u, testing_db::getProducerSetCount(APP_INTF_TABLE_NAME));
        KernelBindingResult = 0;
        ASSERT_TRUE(restarted.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        ASSERT_TRUE(guard.hget("Ethernet0", "action", value));
        EXPECT_EQ("applied", value);
        ASSERT_TRUE(appGuard.hget("Ethernet0", "action", value));
        EXPECT_EQ("applied", value);
        ASSERT_TRUE(appGuard.hget("Ethernet0", "id", value));
        EXPECT_EQ(replayId, value);
        ASSERT_TRUE(guard.hget("Ethernet0", "applied_id", value));
        EXPECT_EQ(replayId, value);
        ASSERT_TRUE(guard.hget("Ethernet0", "retired_id", value));
        EXPECT_EQ("0", value); // Supersession preserves the retirement obligation.
        ASSERT_TRUE(guard.hget("Ethernet0", "kernel_pending", value));
        EXPECT_EQ("0", value);
        ASSERT_TRUE(restarted.m_stateIntfTable.hget("Ethernet0", "vrf", value));
        EXPECT_EQ("VrfBlue", value);
        EXPECT_EQ(1u, testing_db::getProducerSetCount(APP_INTF_TABLE_NAME));
    }

    TEST_F(IntfMgrTest, NamedBindWaitsForGuardNotForRetirement)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        mgr.m_stateVrfTable.set("VrfBlue", {{"state", "ok"}});
        const std::vector<swss::FieldValueTuple> data{{"vrf_name", "VrfBlue"}};
        EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        EXPECT_FALSE(commandWasIssued(" master "));
        std::string id;
        ASSERT_TRUE(mgr.m_stateIntfGuardTable.hget("Ethernet0", "request_id", id));
        mgr.m_stateIntfGuardTable.set("Ethernet0", {{"id", id}, {"state", "guarded"}});
        EXPECT_TRUE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
        EXPECT_TRUE(commandWasIssued(" master "));
        std::string vrf;
        ASSERT_TRUE(mgr.m_stateIntfTable.hget("Ethernet0", "vrf", vrf));
        EXPECT_EQ("VrfBlue", vrf);
        ASSERT_TRUE(mgr.m_stateIntfGuardTable.hget("Ethernet0", "state", vrf));
        EXPECT_EQ("guarded", vrf); // No retirement acknowledgment was needed.
    }

    TEST_F(IntfMgrTest, NamedRemovalFinishesWithoutFutureBindOrRetirementAck)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", "VrfBlue"}});
        EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, {}, DEL_COMMAND));
        std::string id;
        ASSERT_TRUE(mgr.m_stateIntfGuardTable.hget("Ethernet0", "request_id", id));
        mgr.m_stateIntfGuardTable.set("Ethernet0", {{"id", id}, {"state", "guarded"}});
        EXPECT_TRUE(mgr.doIntfGeneralTask({"Ethernet0"}, {}, DEL_COMMAND));
        std::vector<swss::FieldValueTuple> fields;
        EXPECT_FALSE(mgr.m_stateIntfTable.get("Ethernet0", fields));
        std::string applied;
        ASSERT_TRUE(mgr.m_stateIntfGuardTable.hget("Ethernet0", "applied_id", applied));
        EXPECT_EQ(id, applied);
    }

    TEST_F(IntfMgrTest, RestartResumesStandaloneRemovalBeforeFirstKernelAttempt)
    {
        swss::Table config(m_config_db.get(), CFG_INTF_TABLE_NAME);
        swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        const std::vector<swss::FieldValueTuple> data{
            {"vrf_name", "VrfBlue"}, {"ipv6_use_link_local_only", "enable"}};
        std::string bindId, removeId, value;
        {
            swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
            mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
            mgr.m_stateVrfTable.set("VrfBlue", {{"state", "ok"}});
            config.set("Ethernet0", data);
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
            ASSERT_TRUE(guard.hget("Ethernet0", "request_id", bindId));
            guard.set("Ethernet0", {{"id", bindId}, {"state", "guarded"}});
            ASSERT_TRUE(mgr.doIntfGeneralTask({"Ethernet0"}, data, SET_COMMAND));
            guard.set("Ethernet0", {{"state", "retired"}, {"retired_id", bindId}});
            config.del("Ethernet0");
            mockCallArgs.clear();
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, {}, DEL_COMMAND));
            ASSERT_FALSE(commandWasIssued(" nomaster"));
            ASSERT_TRUE(guard.hget("Ethernet0", "request_id", removeId));
            ASSERT_NE(bindId, removeId);
            ASSERT_TRUE(guard.hget("Ethernet0", "kernel_pending", value));
            ASSERT_EQ("0", value);
        }
        // No successor CONFIG command and no in-memory DEL survives this restart.
        mockCallArgs.clear();
        swss::IntfMgr restarted(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto consumer = dynamic_cast<Consumer *>(restarted.getConsumerBase(CFG_INTF_TABLE_NAME));
        ASSERT_NE(nullptr, consumer);
        ASSERT_EQ(1u, consumer->m_toSync.size());
        restarted.doTask(*consumer);
        ASSERT_EQ(1u, consumer->m_toSync.size());
        EXPECT_FALSE(commandWasIssued(" nomaster"));
        guard.set("Ethernet0", {{"id", removeId}, {"state", "guarded"}});
        LinkLocalFlushResult = 2;
        restarted.doTask(*consumer);
        EXPECT_EQ(1u, consumer->m_toSync.size());
        EXPECT_TRUE(restarted.isIntfCreated("Ethernet0"));
        EXPECT_TRUE(commandWasIssued("neigh flush"));
        LinkLocalFlushResult = 0;
        restarted.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_FALSE(restarted.isIntfCreated("Ethernet0"));
        EXPECT_TRUE(commandWasIssued(" nomaster"));
        ASSERT_TRUE(guard.hget("Ethernet0", "applied_id", value));
        EXPECT_EQ(removeId, value);
        ASSERT_TRUE(guard.hget("Ethernet0", "retired_id", value));
        EXPECT_EQ(bindId, value); // No ASIC-completion acknowledgment was fabricated.
        std::vector<swss::FieldValueTuple> fields;
        EXPECT_FALSE(config.get("Ethernet0", fields));
    }

    TEST_F(IntfMgrTest, RestartCompletesRemovalBeforeDifferentVrfSuccessor)
    {
        swss::Table config(m_config_db.get(), CFG_INTF_TABLE_NAME);
        swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        const std::vector<swss::FieldValueTuple> oldData{{"vrf_name", "VrfBlue"}};
        const std::vector<swss::FieldValueTuple> newData{{"vrf_name", "VrfRed"}};
        std::string bindId, removeId, successorId, value;
        {
            swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
            mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
            mgr.m_stateVrfTable.set("VrfBlue", {{"state", "ok"}});
            config.set("Ethernet0", oldData);
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, oldData, SET_COMMAND));
            ASSERT_TRUE(guard.hget("Ethernet0", "request_id", bindId));
            guard.set("Ethernet0", {{"id", bindId}, {"state", "guarded"}});
            ASSERT_TRUE(mgr.doIntfGeneralTask({"Ethernet0"}, oldData, SET_COMMAND));
            guard.set("Ethernet0", {{"state", "retired"}, {"retired_id", bindId}});
            config.del("Ethernet0");
            ASSERT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, {}, DEL_COMMAND));
            ASSERT_TRUE(guard.hget("Ethernet0", "request_id", removeId));
            guard.set("Ethernet0", {{"id", removeId}, {"state", "guarded"}});
            config.set("Ethernet0", newData);
        }
        mockCallArgs.clear();
        swss::IntfMgr restarted(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto consumer = dynamic_cast<Consumer *>(restarted.getConsumerBase(CFG_INTF_TABLE_NAME));
        ASSERT_NE(nullptr, consumer);
        consumer->addToSync(KeyOpFieldsValuesTuple{"Ethernet0", SET_COMMAND, newData});
        restarted.doTask(*consumer);
        // The old removal completes independently even while the future VRF is absent.
        EXPECT_TRUE(commandWasIssued(" nomaster"));
        EXPECT_FALSE(restarted.isIntfCreated("Ethernet0"));
        ASSERT_EQ(1u, consumer->m_toSync.size());
        EXPECT_EQ(SET_COMMAND, kfvOp(consumer->m_toSync.begin()->second));
        restarted.m_stateVrfTable.set("VrfRed", {{"state", "ok"}});
        restarted.doTask(*consumer);
        ASSERT_EQ(1u, consumer->m_toSync.size());
        ASSERT_TRUE(guard.hget("Ethernet0", "request_id", successorId));
        EXPECT_NE(removeId, successorId);
        guard.set("Ethernet0", {{"id", successorId}, {"state", "guarded"}});
        KernelBindingResult = 2;
        restarted.doTask(*consumer);
        EXPECT_EQ(1u, consumer->m_toSync.size());
        EXPECT_FALSE(restarted.isIntfCreated("Ethernet0"));
        KernelBindingResult = 0;
        restarted.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        ASSERT_TRUE(restarted.m_stateIntfTable.hget("Ethernet0", "vrf", value));
        EXPECT_EQ("VrfRed", value);
        ASSERT_TRUE(guard.hget("Ethernet0", "applied_id", value));
        EXPECT_EQ(successorId, value);
        ASSERT_TRUE(guard.hget("Ethernet0", "retired_id", value));
        EXPECT_EQ(bindId, value);
    }

    TEST_F(IntfMgrTest, VerifiedMissingDeviceDischargesOnlyKernelCleanup)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_stateIntfTable.set("Vlan100", {{"vrf", ""}});
        mgr.m_ipv6LinkLocalModeList.insert("Vlan100");
        MockIfIndex = 0;
        KernelBindingResult = 2;
        LinkLocalFlushResult = 2;
        EXPECT_TRUE(mgr.doIntfGeneralTask({"Vlan100"}, {}, DEL_COMMAND));
        EXPECT_FALSE(commandWasIssued("neigh flush"));
        std::vector<swss::FieldValueTuple> fields;
        EXPECT_FALSE(mgr.m_stateIntfTable.get("Vlan100", fields));
    }

    TEST_F(IntfMgrTest, RestartCancelsOrphanPrepareWithoutInventingLinuxWork)
    {
        swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        guard.set("Ethernet0", {{"request_id", "1"}, {"action", "prepare"}, {"target_vrf", "VrfBlue"}});
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        std::string action;
        ASSERT_TRUE(guard.hget("Ethernet0", "action", action));
        EXPECT_EQ("cancel", action);
        EXPECT_FALSE(commandWasIssued(" master "));
        EXPECT_FALSE(commandWasIssued(" nomaster"));
    }

    TEST_F(IntfMgrTest, RestartRetainsPartiallyAppliedKernelRemoval)
    {
        swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        guard.set("Ethernet0", {{"request_id", "1"}, {"action", "prepare"},
                              {"target_vrf", "VrfBlue"}, {"kernel_pending", "1"}});
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto consumer = dynamic_cast<Consumer *>(mgr.getConsumerBase(CFG_INTF_TABLE_NAME));
        ASSERT_NE(nullptr, consumer);
        ASSERT_EQ(1u, consumer->m_toSync.size());
        mgr.doTask(*consumer);
        ASSERT_EQ(1u, consumer->m_toSync.size());
        std::string id;
        ASSERT_TRUE(guard.hget("Ethernet0", "request_id", id));
        guard.set("Ethernet0", {{"id", id}, {"state", "guarded"}});
        mgr.doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        EXPECT_TRUE(commandWasIssued(" nomaster"));
        std::string pending;
        ASSERT_TRUE(guard.hget("Ethernet0", "kernel_pending", pending));
        EXPECT_EQ("0", pending);
    }

    TEST_F(IntfMgrTest, SameKeyDeleteThenSetPreservesSuccessor)
    {
        swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        mgr.m_statePortTable.set("Ethernet0", {{"state", "ok"}});
        mgr.m_stateVrfTable.set("VrfBlue", {{"state", "ok"}});
        mgr.m_stateIntfTable.set("Ethernet0", {{"vrf", "VrfRed"}});
        auto consumer = dynamic_cast<Consumer *>(mgr.getConsumerBase(CFG_INTF_TABLE_NAME));
        ASSERT_NE(nullptr, consumer);
        std::deque<KeyOpFieldsValuesTuple> entries{
            {"Ethernet0", DEL_COMMAND, {}},
            {"Ethernet0", SET_COMMAND, {{"vrf_name", "VrfBlue"}}},
            {"Ethernet0|192.0.2.1/24", SET_COMMAND, {}}
        };
        consumer->addToSync(entries);
        swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        for (unsigned pass = 0; pass < 5; ++pass)
        {
            mgr.doTask(*consumer);
            std::string id;
            if (guard.hget("Ethernet0", "request_id", id))
            {
                guard.set("Ethernet0", {{"id", id}, {"state", "guarded"}});
            }
        }
        EXPECT_TRUE(consumer->m_toSync.empty());
        std::string vrf;
        ASSERT_TRUE(mgr.m_stateIntfTable.hget("Ethernet0", "vrf", vrf));
        EXPECT_EQ("VrfBlue", vrf);
        EXPECT_TRUE(commandWasIssued("address \"add\""));
    }

    TEST_F(IntfMgrTest, GuardedCleanupFailureRetainsRootAndMode)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        intfmgr.m_ipv6LinkLocalModeList.insert("Ethernet0");
        intfmgr.m_stateIntfTable.set("Ethernet0", {{"vrf", "VrfBlue"}});
        swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        guard.set("Ethernet0", {{"request_id", "1"}, {"action", "prepare"},
                               {"target_vrf", ""}, {"id", "1"}, {"state", "guarded"}});
        intfmgr.m_neighTable.set("Ethernet0:fe80::2", {{"neigh", "00:11:22:33:44:55"}});
        LinkLocalFlushResult = 2;

        EXPECT_FALSE(intfmgr.doIntfGeneralTask({"Ethernet0"}, {}, DEL_COMMAND));
        EXPECT_EQ(1u, intfmgr.m_ipv6LinkLocalModeList.count("Ethernet0"));
        std::vector<swss::FieldValueTuple> fields;
        EXPECT_TRUE(intfmgr.m_stateIntfTable.get("Ethernet0", fields));
        EXPECT_TRUE(commandWasIssued("-6 neigh flush"));
        EXPECT_FALSE(commandWasIssued("-6 neigh show"));
    }

    TEST_F(IntfMgrTest, NeighborRelearnDoesNotBlockRemovalAfterSuccessfulFlush)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        intfmgr.m_ipv6LinkLocalModeList.insert("Ethernet0");
        intfmgr.m_stateIntfTable.set("Ethernet0", {{"vrf", ""}});

        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Ethernet0"}, {}, DEL_COMMAND));
        EXPECT_EQ(0u, intfmgr.m_ipv6LinkLocalModeList.count("Ethernet0"));
        std::vector<swss::FieldValueTuple> fields;
        EXPECT_FALSE(intfmgr.m_stateIntfTable.get("Ethernet0", fields));
        EXPECT_TRUE(commandWasIssued("-6 neigh flush"));
        EXPECT_TRUE(commandWasIssued("-4 neigh flush"));
        EXPECT_FALSE(commandWasIssued("neigh show"));
    }

    TEST_F(IntfMgrTest, CleanupTargetsExactDeviceAndIpv6LinkLocalScope)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        intfmgr.m_ipv6LinkLocalModeList.insert("Ethernet0");
        intfmgr.m_neighTable.set("Ethernet00:fe80::2", {{"neigh", "00:11:22:33:44:55"}});
        mockCallArgs.clear();
        EXPECT_TRUE(intfmgr.doIntfGeneralTask({"Ethernet0"}, {}, DEL_COMMAND));
        std::vector<std::string> cleanupCommands;
        for (const auto &command : mockCallArgs)
        {
            if (command.find(" neigh ") != std::string::npos)
            {
                cleanupCommands.push_back(command);
            }
        }
        const std::vector<std::string> expected = {
            "/sbin/ip -6 neigh flush dev \"Ethernet0\" to fe80::/10 nud all",
            "/sbin/ip -4 neigh flush dev \"Ethernet0\" to 169.254.0.0/16 nud all"
        };
        EXPECT_EQ(expected, cleanupCommands);
        EXPECT_FALSE(commandWasIssued("Ethernet00"));
    }

    TEST_F(IntfMgrTest, CleanupRunsBeforeDeletingSubinterfaceDevice)
    {
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        intfmgr.m_ipv6LinkLocalModeList.insert("Ethernet0.100");
        intfmgr.m_stateIntfTable.set("Ethernet0.100", {{"vrf", "VrfBlue"}});
        swss::Table guard(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        guard.set("Ethernet0.100", {{"request_id", "1"}, {"action", "prepare"},
                                   {"target_vrf", ""}, {"id", "1"}, {"state", "guarded"}});
        LinkLocalFlushResult = 2;
        EXPECT_FALSE(intfmgr.doIntfGeneralTask({"Ethernet0.100"}, {}, DEL_COMMAND));
        EXPECT_FALSE(commandWasIssued("link del"));
        EXPECT_EQ(1u, intfmgr.m_ipv6LinkLocalModeList.count("Ethernet0.100"));
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
        intfmgr.doIntfAddrTask(keys, data, "SET");
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
        intfmgr.doIntfAddrTask(keys, data, "SET");
        int ip_cmd_called = 0;
        for (auto cmd : mockCallArgs){
            if (cmd.find("/sbin/ip -6 address \"add\"") == 0){
                ip_cmd_called++;
            }
        }
        ASSERT_EQ(ip_cmd_called, 1);
    }

    //This test except no runtime error when the set admin status command failed
    //and the subinterface has not ok status (for example not existing subinterface)
    TEST_F(IntfMgrTest, testSetAdminStatusFailToNotOkSubInt){
        swss::IntfMgr intfmgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        intfmgr.setHostSubIntfAdminStatus("Ethernet64.10", "up", "up");
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

namespace intfmgr_ut
{
    TEST_F(IntfMgrTest, DefaultCleanupFailureDoesNotStrandDeletedRootAfterRestart)
    {
        callback = [](const std::string &cmd, std::string &out) {
            if (cmd.find(" neigh ") != std::string::npos) return 2;
            return cb(cmd, out);
        };
        swss::Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        swss::Table state(m_state_db.get(), STATE_INTERFACE_TABLE_NAME);
        app.set("Ethernet0", {{"ipv6_use_link_local_only", "enable"}});
        state.set("Ethernet0", {{"vrf", ""}});
        {
            swss::IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
            mgr.m_ipv6LinkLocalModeList.insert("Ethernet0");
            mgr.m_neighTable.set("Ethernet0:fe80::2", {{"neigh", "00:11:22:33:44:55"}});
            auto consumer = dynamic_cast<Consumer *>(mgr.getConsumerBase(CFG_INTF_TABLE_NAME));
            consumer->addToSync(KeyOpFieldsValuesTuple{"Ethernet0", DEL_COMMAND, {}});
            mgr.doTask(*consumer);
        }
        callback = cb;
        swss::IntfMgr restarted(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        auto consumer = dynamic_cast<Consumer *>(restarted.getConsumerBase(CFG_INTF_TABLE_NAME));
        restarted.doTask(*consumer);
        std::vector<swss::FieldValueTuple> fields;
        EXPECT_FALSE(app.get("Ethernet0", fields));
        EXPECT_FALSE(state.get("Ethernet0", fields));
    }
}

namespace intfmgr_ut
{
    TEST_F(IntfMgrTest, ColdDefaultRootSupersedesRetainedNamedBinding)
    {
        Table config(m_config_db.get(), CFG_INTF_TABLE_NAME);
        config.set("Ethernet0", {{"NULL", "NULL"}});
        Table ports(m_state_db.get(), STATE_PORT_TABLE_NAME);
        ports.set("Ethernet0", {{"state", "ok"}});
        Table state(m_state_db.get(), "INTERFACE_GUARD_TABLE");
        // A cold SWSS restart clears APP and INTERFACE_TABLE, but retains this.
        state.set("Ethernet0", {{"request_id", "1"}, {"action", "applied"},
            {"target_vrf", "VrfBlue"}, {"applied_id", "1"},
            {"id", "1"}, {"state", "released"}, {"kernel_pending", "0"}});
        Table app(m_app_db.get(), APP_INTF_TABLE_NAME);
        IntfMgr mgr(m_config_db.get(), m_app_db.get(), m_state_db.get(), cfg_intf_tables);
        std::vector<FieldValueTuple> fields;
        EXPECT_FALSE(mgr.doIntfGeneralTask({"Ethernet0"}, {{"NULL", "NULL"}}, SET_COMMAND));
        EXPECT_FALSE(app.get("Ethernet0", fields));
        std::string id, action, target;
        ASSERT_TRUE(state.hget("Ethernet0", "request_id", id));
        EXPECT_EQ("2", id);
        state.set("Ethernet0", {{"id", id}, {"state", "guarded"}});
        EXPECT_TRUE(mgr.doIntfGeneralTask({"Ethernet0"}, {{"NULL", "NULL"}}, SET_COMMAND));
        EXPECT_TRUE(app.get("Ethernet0", fields));
        ASSERT_TRUE(state.hget("Ethernet0", "action", action));
        ASSERT_TRUE(state.hget("Ethernet0", "target_vrf", target));
        EXPECT_EQ("applied", action);
        EXPECT_TRUE(target.empty());
        EXPECT_TRUE(std::any_of(mockCallArgs.begin(), mockCallArgs.end(),
            [](const std::string &cmd) { return cmd.find(" nomaster") != std::string::npos; }));
    }
}
