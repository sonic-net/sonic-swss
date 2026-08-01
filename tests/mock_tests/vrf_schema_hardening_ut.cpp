#include "gtest/gtest.h"

#define private public
#define protected public
#include "orch.h"
#include "vrforch.h"
#undef protected
#undef private

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "flowcounterrouteorch.h"
#include "directory.h"
#include "vrf_appl_fields.h"
#include "vrf_schema_hardening.h"

extern sai_virtual_router_api_t *sai_virtual_router_api;

/*
 * Local VRF schema hardening tests (UPSW-6663).
 * Own translation unit so upstream vrforch_ut.cpp downmerges cleanly.
 */
namespace vrf_schema_hardening_test
{
    using namespace std;

    sai_virtual_router_api_t *pold_sai_vr_api;
    sai_virtual_router_api_t ut_sai_vr_api;

    int set_vr_attr_count = 0;
    sai_status_t set_vr_attr_status = SAI_STATUS_SUCCESS;

    sai_status_t _ut_set_virtual_router_attribute(
            _In_ sai_object_id_t virtual_router_id,
            _In_ const sai_attribute_t *attr)
    {
        ++set_vr_attr_count;
        return set_vr_attr_status;
    }

    sai_status_t _ut_create_virtual_router(
            _Out_ sai_object_id_t *vr_id,
            _In_ sai_object_id_t switch_id,
            _In_ uint32_t attr_count,
            _In_ const sai_attribute_t *attr_list)
    {
        *vr_id = 0x3000000000099;
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_remove_virtual_router(
            _In_ sai_object_id_t vr_id)
    {
        return SAI_STATUS_SUCCESS;
    }

    struct VRFSchemaHardeningTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;

        void SetUp() override
        {
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);

            pold_sai_vr_api = sai_virtual_router_api;
            ut_sai_vr_api = *sai_virtual_router_api;
            ut_sai_vr_api.create_virtual_router = _ut_create_virtual_router;
            ut_sai_vr_api.set_virtual_router_attribute = _ut_set_virtual_router_attribute;
            ut_sai_vr_api.remove_virtual_router = _ut_remove_virtual_router;
            sai_virtual_router_api = &ut_sai_vr_api;

            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);

            sai_attribute_t attr;
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            auto status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gMacAddress = attr.value.mac;

            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gVirtualRouterId = attr.value.oid;

            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            static const vector<string> route_pattern_tables = {
                CFG_FLOW_COUNTER_ROUTE_PATTERN_TABLE_NAME,
            };
            ASSERT_EQ(gFlowCounterRouteOrch, nullptr);
            gFlowCounterRouteOrch = new FlowCounterRouteOrch(m_config_db.get(), route_pattern_tables);

            set_vr_attr_count = 0;
            set_vr_attr_status = SAI_STATUS_SUCCESS;
        }

        void TearDown() override
        {
            sai_virtual_router_api = pold_sai_vr_api;

            delete gFlowCounterRouteOrch;
            gFlowCounterRouteOrch = nullptr;

            delete gCrmOrch;
            gCrmOrch = nullptr;

            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gSwitchId = 0;

            ut_helper::uninitSaiApi();
        }
    };

    TEST_F(VRFSchemaHardeningTest, VrfUnknownAttrDoesNotDiscardRow)
    {
        VRFOrch vrfOrch(m_app_db.get(), APP_VRF_TABLE_NAME,
                        m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);

        sai_object_id_t fake_vr_id = 0x3000000000102;
        vrfOrch.vrf_table_["Vrf_tenant-5"].vrf_id = fake_vr_id;
        vrfOrch.vrf_table_["Vrf_tenant-5"].ref_count = 0;

        ASSERT_TRUE(vrfOrch.isVRFexists("Vrf_tenant-5"));

        set_vr_attr_status = SAI_STATUS_SUCCESS;
        set_vr_attr_count = 0;

        /*
         * 'rd' is BGP metadata that used to be copied onto the APPL_DB row.
         * Under strict parsing it threw before addOperation() ran and the whole
         * row was discarded, so ttl_action was silently never applied.
         */
        auto consumer = dynamic_cast<Consumer *>(vrfOrch.getExecutor(APP_VRF_TABLE_NAME));
        swss::KeyOpFieldsValuesTuple kco_update("Vrf_tenant-5", "SET",
            { { "rd", "20005:1" }, { "ttl_action", "forward" } });
        consumer->addToSync({ kco_update });
        static_cast<Orch *>(&vrfOrch)->doTask(*consumer);

        ASSERT_EQ(set_vr_attr_count, 1);
        ASSERT_TRUE(vrfOrch.isVRFexists("Vrf_tenant-5"));
    }

    TEST(VRFApplSchema, VrfApplForwardFieldsMatchOrchSchema)
    {
        std::set<std::string> orch_attrs;
        for (const auto& attr : request_description.attr_item_types)
        {
            orch_attrs.insert(attr.first);
        }

        EXPECT_EQ(swss::vrfApplForwardFields(), orch_attrs)
            << "cfgmgr/vrf_appl_fields.h drifted from orchagent/vrforch.h; "
               "a VRF field accepted by one and not the other is dropped silently";
    }

    TEST(VRFApplSchema, MetadataOnlyUpdateSkipsPublish)
    {
        std::vector<swss::FieldValueTuple> filtered;

        /* Existing VRF + only BGP metadata -> do not publish (avoids vni=0 clear). */
        EXPECT_FALSE(swss::filterVrfApplFields(
            { { "rd", "20005:1" }, { "redistribute_connected", "true" } },
            filtered,
            /*publish_placeholder_if_empty=*/false));
        EXPECT_TRUE(filtered.empty());

        /* First create with only metadata -> placeholder so the row materializes. */
        EXPECT_TRUE(swss::filterVrfApplFields(
            { { "rd", "20005:1" } },
            filtered,
            /*publish_placeholder_if_empty=*/true));
        ASSERT_EQ(filtered.size(), 1u);
        EXPECT_EQ(fvField(filtered[0]), "NULL");

        /* Orchagent fields are forwarded. */
        EXPECT_TRUE(swss::filterVrfApplFields(
            { { "rd", "20005:1" }, { "vni", "10010" }, { "ttl_action", "forward" } },
            filtered,
            /*publish_placeholder_if_empty=*/false));
        ASSERT_EQ(filtered.size(), 2u);
    }
}
