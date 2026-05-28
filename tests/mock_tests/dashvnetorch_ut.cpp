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
#include "dash_api/appliance.pb.h"
#include "dash_api/route_type.pb.h"
#include "dash_api/eni.pb.h"
#include "dash_api/qos.pb.h"
#include "dash_api/eni_route.pb.h"
#include "gtest/gtest.h"
#include "crmorch.h"
#include "table.h"

#include <deque>

EXTERN_MOCK_FNS

namespace dashvnetorch_test
{
    DEFINE_SAI_API_MOCK(dash_outbound_ca_to_pa, outbound_ca_to_pa);
    DEFINE_SAI_API_MOCK(dash_pa_validation, pa_validation);
    DEFINE_SAI_GENERIC_API_OBJECT_BULK_MOCK(dash_vnet, vnet)
    using namespace mock_orch_test;
    using ::testing::Return;
    using ::testing::Throw;
    using ::testing::DoAll;
    using ::testing::SetArrayArgument;
    using ::testing::SetArgPointee;
    using ::testing::InSequence;

    class DashVnetOrchTest : public MockDashOrchTest
    {
    protected:
        int GetCrmUsedCount(CrmResourceType type)
        {
            CrmOrch::CrmResourceEntry entry = CrmOrch::CrmResourceEntry("", CrmThresholdType::CRM_PERCENTAGE, 0, 1);
            gCrmOrch->getResAvailability(type, entry);
            return entry.countersMap["STATS"].usedCounter;
        }

        void ProcessDashEntries(const std::string &table_name,
                                const std::deque<swss::KeyOpFieldsValuesTuple> &entries,
                                bool expect_empty = true)
        {
            Orch *target_orch = *(dash_table_orch_map.at(table_name));
            auto consumer = std::make_unique<Consumer>(
                new swss::ConsumerStateTable(m_app_db.get(), table_name),
                target_orch, table_name);
            consumer->addToSync(entries);
            target_orch->doTask(*consumer.get());

            if (expect_empty)
            {
                EXPECT_EQ(consumer->m_toSync.begin(), consumer->m_toSync.end());
            }
            else
            {
                EXPECT_NE(consumer->m_toSync.begin(), consumer->m_toSync.end());
            }
        }

        void ProcessDashTupleRaw(const std::string &table_name,
                                 const std::string &key,
                                 const std::string &op,
                                 const std::vector<swss::FieldValueTuple> &fvs,
                                 bool expect_empty = true)
        {
            ProcessDashEntries(table_name, {swss::KeyOpFieldsValuesTuple(key, op, fvs)}, expect_empty);
        }

        bool DashResultExists(const std::string &table_name, const std::string &key)
        {
            swss::Table table(m_dpu_app_state_db.get(), table_name);
            std::vector<swss::FieldValueTuple> values;
            return table.get(key, values);
        }

        uint32_t GetDashResult(const std::string &table_name, const std::string &key)
        {
            swss::Table table(m_dpu_app_state_db.get(), table_name);
            std::vector<swss::FieldValueTuple> values;
            EXPECT_TRUE(table.get(key, values));
            for (const auto &fv : values)
            {
                if (fvField(fv) == "result")
                {
                    return static_cast<uint32_t>(std::stoul(fvValue(fv)));
                }
            }

            ADD_FAILURE() << "Result field not found for key " << key;
            return 0xffffffffu;
        }

        void ApplySaiMock() override
        {
            INIT_SAI_API_MOCK(dash_vnet);
            INIT_SAI_API_MOCK(dash_outbound_ca_to_pa);
            INIT_SAI_API_MOCK(dash_pa_validation);
            MockSaiApis();
        }

        void PostSetUp() override
        {
            CreateApplianceEntry();
        }
        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_outbound_ca_to_pa);
            DEINIT_SAI_API_MOCK(dash_pa_validation);
            DEINIT_SAI_API_MOCK(dash_vnet);
        }

    };

    TEST_F(DashVnetOrchTest, AddRemoveVnet)
    {
        std::vector<sai_status_t> exp_status = {SAI_STATUS_SUCCESS};
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        AddPLRoutingType();
        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets).Times(1);
            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).WillOnce(DoAll(
                Return(SAI_STATUS_SUCCESS)
            ));
            EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(1);
            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).WillOnce(DoAll(
                Return(SAI_STATUS_SUCCESS)
            ));
            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries).Times(2).WillRepeatedly(DoAll(
                Return(SAI_STATUS_SUCCESS)
            ));
            EXPECT_CALL(*mock_sai_dash_pa_validation_api, remove_pa_validation_entries).Times(1);
            EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets).Times(1);
        }

        CreateVnet();
        AddVnetMap();
        AddPortMap();
        AddVnetMapPL();

        RemoveVnetMap();
        RemoveVnetMapPL();
        RemoveVnet();
    }

    TEST_F(DashVnetOrchTest, AddVnetMapMissingVnetFails)
    {
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(0);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries)
            .Times(0);
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        AddVnetMap(true);
    }

    TEST_F(DashVnetOrchTest, AddExistingOutboundCaToPaSuccessful)
    {
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        AddVnetMap();
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_ALREADY_EXISTS};

        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddVnetMap(); 
        int actualUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
        EXPECT_EQ(expectedUsed, actualUsed);
    }

    TEST_F(DashVnetOrchTest, RemoveNonexistVnetMapFails)
    {
        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_NOT_FOUND};
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveVnetMap(); 
        int actualUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
        EXPECT_EQ(expectedUsed, actualUsed);
    }

    TEST_F(DashVnetOrchTest, InvalidEncapVnetMapFails)
    {
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_UNSPECIFIED);
        CreateVnet();
        AddVnetMap();
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(0);
        AddVnetMap();
    }

    TEST_F(DashVnetOrchTest, AddExistPaValidationSuccessful)
    {
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_ALREADY_EXISTS};
        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddVnetMap();
        int actualUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        EXPECT_EQ(expectedUsed, actualUsed);
    }

    TEST_F(DashVnetOrchTest, RemovePaValidationInUseFails)
    {
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        AddVnetMap();

        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        std::vector<sai_status_t> exp_status = {SAI_STATUS_OBJECT_IN_USE};

        EXPECT_CALL(*mock_sai_dash_pa_validation_api, remove_pa_validation_entries)
            .Times(1).WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        RemoveVnet(true);

        int actualUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);
        EXPECT_EQ(expectedUsed, actualUsed);
    }

    TEST_F(DashVnetOrchTest, VnetSaiCreateFailureNotRetried)
    {
        std::vector<sai_object_id_t> exp_oids = {SAI_NULL_OBJECT_ID};
        EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets)
            .WillOnce(DoAll(SetArrayArgument<5>(exp_oids.begin(), exp_oids.end()), Return(SAI_STATUS_INSUFFICIENT_RESOURCES)));
        CreateVnet();
    }

    TEST_F(DashVnetOrchTest, VnetMapSaiCreateInvalidParameterNotRetried)
    {
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        std::vector<sai_status_t> exp_status = {SAI_STATUS_INVALID_PARAMETER};
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        AddVnetMap(true);
    }

    TEST_F(DashVnetOrchTest, RemoveNonExistentVnet)
    {
        EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets).Times(0);
        RemoveVnet(true);
    }

    TEST_F(DashVnetOrchTest, VnetRemoveItemNotFoundSuccess)
    {
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_NOT_FOUND};

        CreateVnet();
        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_VNET);

        EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets)
            .Times(1)
            .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));

        RemoveVnet();

        EXPECT_EQ(expectedUsed, GetCrmUsedCount(CrmResourceType::CRM_DASH_VNET));
        EXPECT_FALSE(DashResultExists(APP_DASH_VNET_TABLE_NAME, vnet1));
    }

    TEST_F(DashVnetOrchTest, VnetUnknownOpConsumed)
    {
        dash::vnet::Vnet vnet;
        vnet.set_vni(5555);

        EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets).Times(0);
        EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets).Times(0);

        ProcessDashTupleRaw(APP_DASH_VNET_TABLE_NAME, vnet1, "UNKNOWN",
                            {{"pb", vnet.SerializeAsString()}});

        EXPECT_FALSE(DashResultExists(APP_DASH_VNET_TABLE_NAME, vnet1));
    }

    TEST_F(DashVnetOrchTest, VnetDuplicateDeleteNoopClearsBulkContext)
    {
        EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets).Times(0);

        ProcessDashEntries(
            APP_DASH_VNET_TABLE_NAME,
            {
                swss::KeyOpFieldsValuesTuple(vnet1, DEL_COMMAND, {}),
                swss::KeyOpFieldsValuesTuple(vnet1, DEL_COMMAND, {})
            });
    }

    TEST_F(DashVnetOrchTest, VnetMapMissingRouteTypeActions)
    {
        std::string key = vnet1 + ":" + vnet_map_ip1;

        CreateVnet();

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(0);

        AddVnetMap();

        EXPECT_EQ(GetDashResult(APP_DASH_VNET_MAPPING_TABLE_NAME, key), 1u);
    }

    TEST_F(DashVnetOrchTest, VnetMapDuplicateEntry)
    {
        std::vector<sai_status_t> exp_status = {SAI_STATUS_ITEM_ALREADY_EXISTS};

        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        AddVnetMap();

        int expectedCaUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);
        int expectedPaUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION);

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(1)
            .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(0);

        AddVnetMap();

        EXPECT_EQ(expectedCaUsed, GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA));
        EXPECT_EQ(expectedPaUsed, GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_PA_VALIDATION));
    }

    TEST_F(DashVnetOrchTest, VnetMapMissingPortMap)
    {
        std::string key = vnet1 + ":" + vnet_map_ip2;

        AddPLRoutingType();
        CreateVnet();

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(0);

        AddVnetMapPL();

        EXPECT_EQ(GetDashResult(APP_DASH_VNET_MAPPING_TABLE_NAME, key), 1u);
    }

    TEST_F(DashVnetOrchTest, VnetMapPaValidationSaiFailure)
    {
        std::vector<sai_status_t> create_status = {SAI_STATUS_SUCCESS};
        std::vector<sai_status_t> pa_validation_status = {SAI_STATUS_INVALID_PARAMETER};
        std::string key = vnet1 + ":" + vnet_map_ip1;

        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries)
            .Times(1)
            .WillOnce(DoAll(SetArrayArgument<5>(create_status.begin(), create_status.end()), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries)
            .Times(1)
            .WillOnce(DoAll(SetArrayArgument<5>(pa_validation_status.begin(), pa_validation_status.end()), Return(SAI_STATUS_SUCCESS)));

        AddVnetMap();

        EXPECT_EQ(GetDashResult(APP_DASH_VNET_MAPPING_TABLE_NAME, key), 1u);
    }

    TEST_F(DashVnetOrchTest, VnetMapRemoveSaiNotExecuted)
    {
        std::vector<sai_status_t> exp_status = {SAI_STATUS_NOT_EXECUTED};
        std::string key = vnet1 + ":" + vnet_map_ip1;

        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        AddVnetMap();

        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries)
            .Times(1)
            .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));

        RemoveVnetMap();

        EXPECT_EQ(expectedUsed, GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA));
        EXPECT_TRUE(DashResultExists(APP_DASH_VNET_MAPPING_TABLE_NAME, key));
    }

    TEST_F(DashVnetOrchTest, VnetMapRemoveSaiFailure)
    {
        std::vector<sai_status_t> exp_status = {SAI_STATUS_INVALID_PARAMETER};
        std::string key = vnet1 + ":" + vnet_map_ip1;

        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();
        AddVnetMap();

        int expectedUsed = GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA);

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries)
            .Times(1)
            .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));

        RemoveVnetMap();

        EXPECT_EQ(expectedUsed, GetCrmUsedCount(CrmResourceType::CRM_DASH_IPV4_OUTBOUND_CA_TO_PA));
        EXPECT_TRUE(DashResultExists(APP_DASH_VNET_MAPPING_TABLE_NAME, key));
    }

    TEST_F(DashVnetOrchTest, VnetMapUnknownOpConsumed)
    {
        dash::vnet_mapping::VnetMapping vnet_map;
        // Use a unique key to avoid collision with result entries left by prior tests
        std::string key = vnet1 + ":9.9.9.9";

        vnet_map.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_ENCAP);
        vnet_map.mutable_underlay_ip()->set_ipv4(swss::IpAddress("7.7.7.7").getV4Addr());

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(0);

        ProcessDashTupleRaw(APP_DASH_VNET_MAPPING_TABLE_NAME, key, "UNKNOWN",
                            {{"pb", vnet_map.SerializeAsString()}});

        EXPECT_FALSE(DashResultExists(APP_DASH_VNET_MAPPING_TABLE_NAME, key));
    }

    TEST_F(DashVnetOrchTest, VnetMapDuplicateFailedAddClearsBulkContext)
    {
        dash::vnet_mapping::VnetMapping vnet_map;
        std::string key = vnet1 + ":" + vnet_map_ip1;

        CreateVnet();
        vnet_map.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_ENCAP);
        vnet_map.mutable_underlay_ip()->set_ipv4(swss::IpAddress("7.7.7.7").getV4Addr());

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(0);

        ProcessDashEntries(
            APP_DASH_VNET_MAPPING_TABLE_NAME,
            {
                swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, {{"pb", vnet_map.SerializeAsString()}}),
                swss::KeyOpFieldsValuesTuple(key, SET_COMMAND, {{"pb", vnet_map.SerializeAsString()}})
            });

        EXPECT_EQ(GetDashResult(APP_DASH_VNET_MAPPING_TABLE_NAME, key), 1u);
    }

    TEST_F(DashVnetOrchTest, VnetMapDeprecatedActionTypeFallback)
    {
        dash::vnet_mapping::VnetMapping vnet_map;
        std::string key = vnet1 + ":" + vnet_map_ip1;

        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        vnet_map.set_action_type(dash::route_type::ROUTING_TYPE_VNET_ENCAP);
#pragma GCC diagnostic pop
        vnet_map.mutable_underlay_ip()->set_ipv4(swss::IpAddress("7.7.7.7").getV4Addr());

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(1);
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(1);

        ProcessDashTupleRaw(APP_DASH_VNET_MAPPING_TABLE_NAME, key, SET_COMMAND,
                            {{"pb", vnet_map.SerializeAsString()}});

        EXPECT_EQ(GetDashResult(APP_DASH_VNET_MAPPING_TABLE_NAME, key), 0u);
    }

    class DashVnetOrchNoApplianceTest : public MockDashOrchTest
    {
    protected:
        int GetCrmUsedCount(CrmResourceType type)
        {
            CrmOrch::CrmResourceEntry entry = CrmOrch::CrmResourceEntry("", CrmThresholdType::CRM_PERCENTAGE, 0, 1);
            gCrmOrch->getResAvailability(type, entry);
            return entry.countersMap["STATS"].usedCounter;
        }

        void ApplySaiMock() override
        {
            INIT_SAI_API_MOCK(dash_vnet);
            INIT_SAI_API_MOCK(dash_outbound_ca_to_pa);
            INIT_SAI_API_MOCK(dash_pa_validation);
            MockSaiApis();
        }

        void PostSetUp() override
        {
            // Do NOT create appliance — tests need to verify behavior without it
        }
        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_outbound_ca_to_pa);
            DEINIT_SAI_API_MOCK(dash_pa_validation);
            DEINIT_SAI_API_MOCK(dash_vnet);
        }
    };

    TEST_F(DashVnetOrchNoApplianceTest, CreateVnetMissingApplianceNotRetried)
    {
        EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets).Times(0);
        dash::vnet::Vnet vnet = dash::vnet::Vnet();
        vnet.set_vni(5555);
        SetDashTable(APP_DASH_VNET_TABLE_NAME, "VNET_1", vnet, true, true);
    }

    TEST_F(DashVnetOrchTest, MissingProtobufVnet)
    {
        EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets).Times(0);
        SetDashTableRaw(APP_DASH_VNET_TABLE_NAME, "VNET_TEST", {}, true, true);
    }

    TEST_F(DashVnetOrchTest, InvalidProtobufVnetMap)
    {
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        CreateVnet();
        SetDashTableRaw(APP_DASH_VNET_MAPPING_TABLE_NAME, vnet1 + ":1.2.3.4", {{ "pb", "garbage" }}, true, true);
    }

    TEST_F(DashVnetOrchTest, InvalidKeyVnetMap)
    {
        // Invalid keys should be caught per-item and consumed without throwing.
        CreateVnet();
        dash::vnet_mapping::VnetMapping vnet_map = dash::vnet_mapping::VnetMapping();
        vnet_map.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_ENCAP);
        vnet_map.mutable_underlay_ip()->set_ipv4(swss::IpAddress("7.7.7.7").getV4Addr());
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        EXPECT_NO_THROW(
            SetDashTable(APP_DASH_VNET_MAPPING_TABLE_NAME, vnet1 + ":not_an_ip", vnet_map, true, true));
    }

    TEST_F(DashVnetOrchTest, VnetMapKeyMissingIp)
    {
        // Key should be "vnet:ip" — send just vnet without IP
        CreateVnet();
        dash::vnet_mapping::VnetMapping vnet_map = dash::vnet_mapping::VnetMapping();
        vnet_map.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_ENCAP);
        vnet_map.mutable_underlay_ip()->set_ipv4(swss::IpAddress("7.7.7.7").getV4Addr());
        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        EXPECT_NO_THROW(
            SetDashTable(APP_DASH_VNET_MAPPING_TABLE_NAME, vnet1, vnet_map, true, true));
    }

    TEST_F(DashVnetOrchTest, RemoveVnetSaiFailureWritesFailureResult)
    {
        // When SAI vnet remove returns an unrecoverable error code, the orch should
        // not retry — the consumer entry is consumed and a failure result is recorded.
        // Exercises lines 162-167 in dashvnetorch.cpp.
        std::vector<sai_status_t> exp_status = {SAI_STATUS_INVALID_PARAMETER};
        CreateVnet();
        EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets)
            .Times(1)
            .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_SUCCESS)));

        RemoveVnet();
        EXPECT_TRUE(DashResultExists(APP_DASH_VNET_TABLE_NAME, vnet1));
    }

    TEST_F(DashVnetOrchTest, VnetMapTunnelNotFoundConsumed)
    {
        // VnetMap entry references a tunnel that does not exist — addOutboundCaToPa
        // should set pre_op_result to FAILURE and return true so the entry is consumed
        // before reaching the bulker.  Covers lines 377-379 in dashvnetorch.cpp.
        std::string key = vnet1 + ":" + vnet_map_ip1;

        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();

        dash::vnet_mapping::VnetMapping vnet_map = dash::vnet_mapping::VnetMapping();
        vnet_map.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_ENCAP);
        vnet_map.mutable_underlay_ip()->set_ipv4(swss::IpAddress("7.7.7.7").getV4Addr());
        vnet_map.set_tunnel("NONEXISTENT_TUNNEL");

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        SetDashTable(APP_DASH_VNET_MAPPING_TABLE_NAME, key, vnet_map, true, true);
        EXPECT_EQ(GetDashResult(APP_DASH_VNET_MAPPING_TABLE_NAME, key), 1u);
    }

    TEST_F(DashVnetOrchTest, VnetMapInvalidEncapTypeConsumed)
    {
        // VnetMap with a routing type whose encap type is invalid (not VXLAN/NVGRE) —
        // addOutboundCaToPa should set pre_op_result to FAILURE and return true.
        // Covers the "Invalid encap type" path in addOutboundCaToPa.
        std::string key = vnet1 + ":" + vnet_map_ip1;
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_UNSPECIFIED);
        CreateVnet();

        dash::vnet_mapping::VnetMapping vnet_map = dash::vnet_mapping::VnetMapping();
        vnet_map.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_ENCAP);
        vnet_map.mutable_underlay_ip()->set_ipv4(swss::IpAddress("7.7.7.7").getV4Addr());

        EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(0);
        SetDashTable(APP_DASH_VNET_MAPPING_TABLE_NAME, key, vnet_map, true, true);
        EXPECT_EQ(GetDashResult(APP_DASH_VNET_MAPPING_TABLE_NAME, key), 1u);
    }

    TEST_F(DashVnetOrchTest, VnetCreateDeleteChurn)
    {
        for (int i = 0; i < 3; i++)
        {
            EXPECT_CALL(*mock_sai_dash_vnet_api, create_vnets).Times(1);
            CreateVnet();

            EXPECT_CALL(*mock_sai_dash_vnet_api, remove_vnets).Times(1);
            RemoveVnet();
        }
    }

    TEST_F(DashVnetOrchTest, VnetMapCreateDeleteChurn)
    {
        AddVnetEncapRoutingType(dash::route_type::ENCAP_TYPE_VXLAN);
        CreateVnet();

        // PA validation is per-VNET underlay IP, only created on first add
        EXPECT_CALL(*mock_sai_dash_pa_validation_api, create_pa_validation_entries).Times(1);

        for (int i = 0; i < 3; i++)
        {
            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, create_outbound_ca_to_pa_entries).Times(1);
            AddVnetMap();

            EXPECT_CALL(*mock_sai_dash_outbound_ca_to_pa_api, remove_outbound_ca_to_pa_entries).Times(1);
            RemoveVnetMap();
        }
    }
}
