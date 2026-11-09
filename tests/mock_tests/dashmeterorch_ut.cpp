#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_dash_orch_test.h"
#include "dash_api/meter_policy.pb.h"
#include "dash_api/meter_rule.pb.h"
#include "dash_api/types.pb.h"

EXTERN_MOCK_FNS

namespace dashmeterorch_test
{
    DEFINE_SAI_GENERIC_APIS_MOCK(dash_meter, meter_policy, meter_rule)

    using namespace mock_orch_test;
    using ::testing::DoAll;
    using ::testing::Return;
    using ::testing::SaveArg;
    using ::testing::SaveArgPointee;
    using ::testing::SetArgPointee;
    using ::testing::SetArrayArgument;
    using ::testing::Throw;

    class DashMeterOrchTest : public MockDashOrchTest
    {
    protected:
        std::string meterPolicy1 = "METER_POLICY_1";

        void ApplySaiMock() override
        {
            INIT_SAI_API_MOCK(dash_meter);
            MockSaiApis();
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_meter);
        }

        dash::meter_policy::MeterPolicy BuildMeterPolicy(bool ipv4 = true)
        {
            dash::meter_policy::MeterPolicy meter_policy;
            meter_policy.set_ip_version(ipv4 ? dash::types::IP_VERSION_IPV4 : dash::types::IP_VERSION_IPV6);
            return meter_policy;
        }

        dash::meter_rule::MeterRule BuildMeterRule(const std::string &ip = "10.0.0.0",
                                                   const std::string &mask = "255.255.255.0",
                                                   uint32_t meteringClass = 1,
                                                   uint32_t priority = 10)
        {
            dash::meter_rule::MeterRule meter_rule;
            meter_rule.mutable_ip_prefix()->mutable_ip()->set_ipv4(swss::IpAddress(ip).getV4Addr());
            meter_rule.mutable_ip_prefix()->mutable_mask()->set_ipv4(swss::IpAddress(mask).getV4Addr());
            meter_rule.set_metering_class(meteringClass);
            meter_rule.set_priority(priority);
            return meter_rule;
        }

        std::string MeterRuleKey(uint32_t ruleNum) const
        {
            return meterPolicy1 + ":" + std::to_string(ruleNum);
        }

        void ProcessDashTupleRaw(const std::string &tableName,
                                 const std::string &key,
                                 const std::string &op,
                                 const std::vector<swss::FieldValueTuple> &fvs)
        {
            Orch *target_orch = *(dash_table_orch_map.at(tableName));
            auto consumer = std::make_unique<Consumer>(
                new swss::ConsumerStateTable(m_app_db.get(), tableName),
                target_orch, tableName);
            consumer->addToSync(swss::KeyOpFieldsValuesTuple(key, op, fvs));
            target_orch->doTask(*consumer.get());
            EXPECT_EQ(consumer->m_toSync.begin(), consumer->m_toSync.end());
        }
    };

    TEST_F(DashMeterOrchTest, AddRemoveMeterPolicy)
    {
        auto meterPolicy = BuildMeterPolicy();
        sai_object_id_t createdOid = 0x1111;
        sai_object_id_t removedOid = SAI_NULL_OBJECT_ID;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(createdOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_policy)
            .WillOnce(DoAll(SaveArg<0>(&removedOid), Return(SAI_STATUS_SUCCESS)));

        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);
        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyOid(meterPolicy1), createdOid);

        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy, false);
        EXPECT_EQ(removedOid, createdOid);
        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyOid(meterPolicy1), SAI_NULL_OBJECT_ID);
    }

    TEST_F(DashMeterOrchTest, AddDuplicateMeterPolicy)
    {
        auto meterPolicy = BuildMeterPolicy();
        sai_object_id_t createdOid = 0x1112;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(createdOid), Return(SAI_STATUS_SUCCESS)));

        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);
        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyOid(meterPolicy1), createdOid);
    }

    TEST_F(DashMeterOrchTest, RemoveNonExistentMeterPolicy)
    {
        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_policy).Times(0);
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, BuildMeterPolicy(), false);
        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyOid(meterPolicy1), SAI_NULL_OBJECT_ID);
    }

    TEST_F(DashMeterOrchTest, RemoveBoundMeterPolicy)
    {
        auto meterPolicy = BuildMeterPolicy();
        sai_object_id_t createdOid = 0x1113;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(createdOid), Return(SAI_STATUS_SUCCESS)));
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);

        m_DashMeterOrch->incrMeterPolicyEniBindCount(meterPolicy1);
        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyEniBindCount(meterPolicy1), 1);

        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_policy).Times(0);
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy, false);
        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyOid(meterPolicy1), createdOid);
    }

    TEST_F(DashMeterOrchTest, RemoveMeterPolicyWithRules)
    {
        auto meterPolicy = BuildMeterPolicy();
        auto meterRule = BuildMeterRule();
        std::vector<sai_status_t> successStatus = {SAI_STATUS_SUCCESS};
        sai_object_id_t policyOid = 0x1114;
        sai_object_id_t ruleOid = 0x2114;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(policyOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_rules)
            .WillOnce(DoAll(SetArgPointee<5>(ruleOid), SetArrayArgument<6>(successStatus.begin(), successStatus.end()), Return(SAI_STATUS_SUCCESS)));

        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule);

        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_policy).Times(0);
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy, false);
        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyOid(meterPolicy1), policyOid);
    }

    TEST_F(DashMeterOrchTest, MeterPolicySaiCreateFailure)
    {
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(Return(SAI_STATUS_INSUFFICIENT_RESOURCES));

        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, BuildMeterPolicy(), true, true);
        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyOid(meterPolicy1), SAI_NULL_OBJECT_ID);
    }

    TEST_F(DashMeterOrchTest, MeterPolicySaiRemoveFailure)
    {
        auto meterPolicy = BuildMeterPolicy();
        sai_object_id_t createdOid = 0x1115;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(createdOid), Return(SAI_STATUS_SUCCESS)));
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);

        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_policy)
            .WillOnce(Return(SAI_STATUS_INVALID_PARAMETER));
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy, false, true);

        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyOid(meterPolicy1), createdOid);
    }

    TEST_F(DashMeterOrchTest, MissingProtobufMeterPolicy)
    {
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy).Times(0);
        SetDashTableRaw(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, {}, true, true);
        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyOid(meterPolicy1), SAI_NULL_OBJECT_ID);
    }

    TEST_F(DashMeterOrchTest, AddRemoveMeterRule)
    {
        auto meterPolicy = BuildMeterPolicy();
        auto meterRule = BuildMeterRule();
        std::vector<sai_status_t> successStatus = {SAI_STATUS_SUCCESS};
        sai_object_id_t policyOid = 0x1116;
        sai_object_id_t ruleOid = 0x2116;
        sai_object_id_t removedRuleOid = SAI_NULL_OBJECT_ID;
        sai_object_id_t removedPolicyOid = SAI_NULL_OBJECT_ID;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(policyOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_rules)
            .WillOnce(DoAll(SetArgPointee<5>(ruleOid), SetArrayArgument<6>(successStatus.begin(), successStatus.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_rules)
            .WillOnce(DoAll(SaveArgPointee<1>(&removedRuleOid), SetArrayArgument<3>(successStatus.begin(), successStatus.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_policy)
            .WillOnce(DoAll(SaveArg<0>(&removedPolicyOid), Return(SAI_STATUS_SUCCESS)));

        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule, false);
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy, false);

        EXPECT_EQ(removedRuleOid, ruleOid);
        EXPECT_EQ(removedPolicyOid, policyOid);
    }

    TEST_F(DashMeterOrchTest, MeterRuleMissingPolicy)
    {
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_rules).Times(0);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), BuildMeterRule(), true, true);
    }

    TEST_F(DashMeterOrchTest, MeterRuleBoundPolicy)
    {
        auto meterPolicy = BuildMeterPolicy();
        sai_object_id_t policyOid = 0x1117;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(policyOid), Return(SAI_STATUS_SUCCESS)));
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);

        m_DashMeterOrch->incrMeterPolicyEniBindCount(meterPolicy1);

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_rules).Times(0);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), BuildMeterRule(), true, true);
        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyOid(meterPolicy1), policyOid);
    }

    TEST_F(DashMeterOrchTest, MeterRuleSaiCreateFailure)
    {
        auto meterPolicy = BuildMeterPolicy();
        auto meterRule = BuildMeterRule();
        std::vector<sai_status_t> createStatus = {SAI_STATUS_INSUFFICIENT_RESOURCES};
        sai_object_id_t policyOid = 0x1118;
        sai_object_id_t removedPolicyOid = SAI_NULL_OBJECT_ID;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(policyOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_rules)
            .WillOnce(DoAll(SetArgPointee<5>(SAI_NULL_OBJECT_ID), SetArrayArgument<6>(createStatus.begin(), createStatus.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_policy)
            .WillOnce(DoAll(SaveArg<0>(&removedPolicyOid), Return(SAI_STATUS_SUCCESS)));

        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule, true, true);
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy, false);

        EXPECT_EQ(removedPolicyOid, policyOid);
    }

    TEST_F(DashMeterOrchTest, MeterRuleSaiRemoveFailure)
    {
        auto meterPolicy = BuildMeterPolicy();
        auto meterRule = BuildMeterRule();
        std::vector<sai_status_t> successStatus = {SAI_STATUS_SUCCESS};
        std::vector<sai_status_t> removeStatus = {SAI_STATUS_INVALID_PARAMETER};
        sai_object_id_t policyOid = 0x1119;
        sai_object_id_t ruleOid = 0x2119;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(policyOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_rules)
            .WillOnce(DoAll(SetArgPointee<5>(ruleOid), SetArrayArgument<6>(successStatus.begin(), successStatus.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_rules)
            .WillOnce(DoAll(SetArrayArgument<3>(removeStatus.begin(), removeStatus.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_policy).Times(0);

        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule, false, true);
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy, false, true);

        EXPECT_EQ(m_DashMeterOrch->getMeterPolicyOid(meterPolicy1), policyOid);
    }

    TEST_F(DashMeterOrchTest, MeterRuleSaiRemoveNotExecuted)
    {
        auto meterPolicy = BuildMeterPolicy();
        auto meterRule = BuildMeterRule();
        std::vector<sai_status_t> successStatus = {SAI_STATUS_SUCCESS};
        std::vector<sai_status_t> removeStatus = {SAI_STATUS_NOT_EXECUTED};
        sai_object_id_t policyOid = 0x1120;
        sai_object_id_t ruleOid = 0x2120;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(policyOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_rules)
            .WillOnce(DoAll(SetArgPointee<5>(ruleOid), SetArrayArgument<6>(successStatus.begin(), successStatus.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_rules)
            .WillOnce(DoAll(SetArrayArgument<3>(removeStatus.begin(), removeStatus.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_policy).Times(0);

        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule, false, true);
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy, false, true);
    }

    TEST_F(DashMeterOrchTest, MeterRuleSaiRemoveItemNotFound)
    {
        auto meterPolicy = BuildMeterPolicy();
        auto meterRule = BuildMeterRule();
        std::vector<sai_status_t> successStatus = {SAI_STATUS_SUCCESS};
        std::vector<sai_status_t> removeStatus = {SAI_STATUS_ITEM_NOT_FOUND};
        sai_object_id_t policyOid = 0x1121;
        sai_object_id_t ruleOid = 0x2121;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(policyOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_rules)
            .WillOnce(DoAll(SetArgPointee<5>(ruleOid), SetArrayArgument<6>(successStatus.begin(), successStatus.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_rules)
            .WillOnce(DoAll(SetArrayArgument<3>(removeStatus.begin(), removeStatus.end()), Return(SAI_STATUS_SUCCESS)));

        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule);
        SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule, false, true);
    }

    TEST_F(DashMeterOrchTest, MissingProtobufMeterRule)
    {
        auto meterPolicy = BuildMeterPolicy();
        sai_object_id_t policyOid = 0x1122;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(policyOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_rules).Times(0);

        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);
        SetDashTableRaw(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), {}, true, true);
    }

    TEST_F(DashMeterOrchTest, MeterPolicyCreateDeleteChurn)
    {
        auto meterPolicy = BuildMeterPolicy();

        for (int i = 0; i < 3; ++i)
        {
            sai_object_id_t createdOid = 0x1200 + static_cast<sai_object_id_t>(i);
            EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
                .WillOnce(DoAll(SetArgPointee<0>(createdOid), Return(SAI_STATUS_SUCCESS)));
            SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);

            EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_policy)
                .WillOnce(Return(SAI_STATUS_SUCCESS));
            SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy, false);
        }
    }

    TEST_F(DashMeterOrchTest, MeterRuleCreateDeleteChurn)
    {
        auto meterPolicy = BuildMeterPolicy();
        auto meterRule = BuildMeterRule();
        std::vector<sai_status_t> successStatus = {SAI_STATUS_SUCCESS};
        sai_object_id_t policyOid = 0x1123;

        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(DoAll(SetArgPointee<0>(policyOid), Return(SAI_STATUS_SUCCESS)));
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy);

        for (int i = 0; i < 3; ++i)
        {
            sai_object_id_t ruleOid = 0x2200 + static_cast<sai_object_id_t>(i);
            EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_rules)
                .WillOnce(DoAll(SetArgPointee<5>(ruleOid), SetArrayArgument<6>(successStatus.begin(), successStatus.end()), Return(SAI_STATUS_SUCCESS)));
            SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule);

            EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_rules)
                .WillOnce(DoAll(SetArrayArgument<3>(successStatus.begin(), successStatus.end()), Return(SAI_STATUS_SUCCESS)));
            SetDashTable(APP_DASH_METER_RULE_TABLE_NAME, MeterRuleKey(0), meterRule, false);
        }

        EXPECT_CALL(*mock_sai_dash_meter_api, remove_meter_policy)
            .WillOnce(Return(SAI_STATUS_SUCCESS));
        SetDashTable(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, meterPolicy, false);
    }

    TEST_F(DashMeterOrchTest, MeterPolicyUnknownOpAndException)
    {
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_policy)
            .WillOnce(Throw(std::runtime_error("meter policy failure")));

        ProcessDashTupleRaw(APP_DASH_METER_POLICY_TABLE_NAME, meterPolicy1, "UNKNOWN", {});
        ProcessDashTupleRaw(APP_DASH_METER_POLICY_TABLE_NAME,
                            meterPolicy1,
                            SET_COMMAND,
                            {{"pb", BuildMeterPolicy().SerializeAsString()}});
    }

    TEST_F(DashMeterOrchTest, MeterRuleUnknownOpAndInvalidKey)
    {
        EXPECT_CALL(*mock_sai_dash_meter_api, create_meter_rules).Times(0);

        ProcessDashTupleRaw(APP_DASH_METER_RULE_TABLE_NAME,
                            MeterRuleKey(0),
                            "UNKNOWN",
                            {{"pb", BuildMeterRule().SerializeAsString()}});
        ProcessDashTupleRaw(APP_DASH_METER_RULE_TABLE_NAME,
                            meterPolicy1 + ":invalid",
                            SET_COMMAND,
                            {{"pb", BuildMeterRule().SerializeAsString()}});
    }
}
