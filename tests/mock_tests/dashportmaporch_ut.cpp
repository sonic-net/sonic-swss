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

#include "dash_api/outbound_port_map.pb.h"

EXTERN_MOCK_FNS

namespace dashportmaporch_test
{
    DEFINE_SAI_API_COMBINED_MOCK(dash_outbound_port_map, outbound_port_map, outbound_port_map_port_range)
    using namespace mock_orch_test;
    using ::testing::DoAll;
    using ::testing::Return;
    using ::testing::SetArgPointee;
    using ::testing::SetArrayArgument;
    using ::testing::SaveArg;
    using ::testing::SaveArgPointee;
    using ::testing::Invoke;
    using ::testing::InSequence;

    class DashPortMapOrchTest : public MockDashOrchTest
    {
        void ApplySaiMock()
        {
            INIT_SAI_API_MOCK(dash_outbound_port_map);
            MockSaiApis();
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_outbound_port_map);
        }

        protected:
            std::string port_map1 = "PORT_MAP_1";
            int port_map1_start_port = 1000;
            int port_map1_end_port = 2000;
    };

    TEST_F(DashPortMapOrchTest, AddRemovePortMapEntry)
    {
        dash::outbound_port_map::OutboundPortMap port_map;

        std::vector<sai_status_t> exp_status = {SAI_STATUS_SUCCESS};
        sai_object_id_t fake_oid = 0x1234;
        sai_object_id_t actual_removed_oid;
        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, create_outbound_port_maps).WillOnce(
            DoAll(
                SetArgPointee<5>(fake_oid),
                SetArrayArgument<6>(exp_status.begin(), exp_status.end()),
                Return(SAI_STATUS_SUCCESS)
            )
        );
        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, remove_outbound_port_maps).WillOnce(
            DoAll(
                SaveArgPointee<1>(&actual_removed_oid),
                SetArrayArgument<3>(exp_status.begin(), exp_status.end()),
                Return(SAI_STATUS_SUCCESS)
            )
        );
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME, port_map1, port_map);
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME, port_map1, port_map, false);

        EXPECT_EQ(actual_removed_oid, fake_oid);
    }

    TEST_F(DashPortMapOrchTest, AddDuplicatePortMap)
    {
        dash::outbound_port_map::OutboundPortMap port_map;

        std::vector<sai_status_t> exp_status = {SAI_STATUS_SUCCESS};
        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, create_outbound_port_maps).Times(1);
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME, port_map1, port_map);
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME, port_map1, port_map);
    }

    TEST_F(DashPortMapOrchTest, RemoveNonexistPortMap)
    {
        dash::outbound_port_map::OutboundPortMap port_map;
        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, remove_outbound_port_maps).Times(0);
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME, port_map1, port_map, false);
    }
}