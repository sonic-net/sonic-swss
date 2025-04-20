#define protected public
#include "orch.h"
#undef protected
#include "mock_orch_test.h"
#include "mock_table.h"
#include "mock_sai_api.h"

#include "dash_api/ha_set.pb.h"
#include "dash_api/ha_scope.pb.h"
#include "dash_api/types.pb.h"

#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "dash/dashhaorch.h"

using namespace ::testing;

EXTERN_MOCK_FNS

namespace dashhaorch_ut 
{
    DEFINE_SAI_GENERIC_API_MOCK(dash_ha, ha_set, ha_scope);
    using namespace mock_orch_test;

    class DashHaOrchTest : public MockOrchTest
    {
    protected:

        void ApplySaiMock()
        {
            INIT_SAI_API_MOCK(dash_ha);
            MockSaiApis();
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_ha);
        }

        void CreateHaSet()
        {
            Table ha_set_table = Table(m_app_db.get(), APP_DASH_HA_SET_TABLE_NAME);
            dash::ha_set::HaSet ha_set = dash::ha_set::HaSet();
            swss::IpAddress vip_v4("1.1.1.1");
            swss::IpAddress npu_ip("2.2.2.2");
            swss::IpAddress local_ip("3.3.3.3");
            swss::IpAddress peer_ip("4.4.4.4");

            ha_set.set_version("1");
            ha_set.set_scope(dash::types::SCOPE_DPU);
            ha_set.mutable_vip_v4()->set_ipv4(vip_v4.getV4Addr());
            ha_set.mutable_local_npu_ip()->set_ipv4(npu_ip.getV4Addr());
            ha_set.mutable_local_ip()->set_ipv4(local_ip.getV4Addr());
            ha_set.mutable_peer_ip()->set_ipv4(peer_ip.getV4Addr());
            ha_set.set_cp_data_channel_port(100);
            ha_set.set_dp_channel_dst_port(200);
            ha_set.set_dp_channel_src_port_min(0);
            ha_set.set_dp_channel_src_port_max(1000);
            ha_set.set_dp_channel_probe_interval_ms(1000);
            ha_set.set_dp_channel_probe_fail_threshold(3);
            ha_set_table.set("HA_SET_1", { { "pb", ha_set.SerializeAsString() } });
            m_dashHaOrch->addExistingData(&ha_set_table);
            static_cast<Orch *>(m_dashHaOrch)->doTask();
        }

        void CreateHaScope()
        {
            Table ha_scope_table = Table(m_app_db.get(), APP_DASH_HA_SCOPE_TABLE_NAME);
            dash::ha_scope::HaScope ha_scope = dash::ha_scope::HaScope();
            ha_scope.set_version("1");
            ha_scope.set_ha_role(dash::types::HA_SCOPE_ROLE_DEAD);
            ha_scope_table.set("HA_SET_1", { { "pb", ha_scope.SerializeAsString() } });
            m_dashHaOrch->addExistingData(&ha_scope_table);
            static_cast<Orch *>(m_dashHaOrch)->doTask();
        }
    };

    TEST_F(DashHaOrchTest, AddHaSet)
    {
        CreateHaSet();
        EXPECT_CALL(*mock_sai_dash_ha_api, create_ha_set)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));

    }

    TEST_F(DashHaOrchTest, AddHaScope)
    {
        CreateHaSet();
        CreateHaScope();
        EXPECT_CALL(*mock_sai_dash_ha_api, create_ha_scope)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));
    }
}
