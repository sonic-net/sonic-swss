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
            int port_map1_backend_port_base = 5000;
            swss::IpAddress port_map1_backend_ip = swss::IpAddress("1.2.3.4");
            dash::outbound_port_map_range::OutboundPortMapRange BuildOutboundPortMapRange()
            {
                dash::outbound_port_map_range::OutboundPortMapRange port_map_range;
                port_map_range.mutable_backend_ip()->set_ipv4(port_map1_backend_ip.getV4Addr());
                port_map_range.set_action(dash::outbound_port_map_range::PortMapRangeAction::ACTION_MAP_PRIVATE_LINK_SERVICE);
                port_map_range.set_backend_port_base(port_map1_backend_port_base);
                return port_map_range;
            }
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

    TEST_F(DashPortMapOrchTest, AddRemovePortMapRange)
    {
        uint32_t num_attrs;
        const sai_attribute_t* attr_start;
        std::vector<sai_attribute_t> actual_attrs;
        sai_outbound_port_map_port_range_entry_t actual_entry;
        sai_outbound_port_map_port_range_entry_t removed_entry;
        sai_object_id_t fake_oid = 0x1234;
        std::vector<sai_status_t> success_status = {SAI_STATUS_SUCCESS};

        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, create_outbound_port_maps)
            .WillOnce(DoAll(
                SetArgPointee<5>(fake_oid),
                SetArrayArgument<6>(success_status.begin(), success_status.end()),
                Return(SAI_STATUS_SUCCESS)
            ));

        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, create_outbound_port_map_port_range_entries)
            .WillOnce(DoAll(
                SaveArgPointee<1>(&actual_entry),
                SaveArgPointee<2>(&num_attrs),
                SaveArgPointee<3>(&attr_start),
                Return(SAI_STATUS_SUCCESS)
            ));

        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, remove_outbound_port_map_port_range_entries)
            .WillOnce(DoAll(
                SaveArgPointee<1>(&removed_entry),
                SetArrayArgument<3>(success_status.begin(), success_status.end()),
                Return(SAI_STATUS_SUCCESS)
            ));

        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME, port_map1, dash::outbound_port_map::OutboundPortMap());

        std::stringstream key_stream;
        key_stream << port_map1 << ":" << port_map1_start_port << "-" << port_map1_end_port;
        std::string key = key_stream.str();

        dash::outbound_port_map_range::OutboundPortMapRange port_map_range = BuildOutboundPortMapRange();
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_RANGE_TABLE_NAME, key, port_map_range);

        EXPECT_EQ(actual_entry.outbound_port_map_id, fake_oid);
        EXPECT_EQ(actual_entry.dst_port_range.min, 1000);
        EXPECT_EQ(actual_entry.dst_port_range.max, 2000);

        actual_attrs.assign(attr_start, attr_start + num_attrs);

        for (auto attr : actual_attrs)
        {
            if (attr.id == SAI_OUTBOUND_PORT_MAP_PORT_RANGE_ENTRY_ATTR_ACTION)
            {
                EXPECT_EQ(attr.value.s32, SAI_OUTBOUND_PORT_MAP_PORT_RANGE_ENTRY_ACTION_MAP_TO_PRIVATE_LINK_SERVICE);
            }
            else if (attr.id == SAI_OUTBOUND_PORT_MAP_PORT_RANGE_ENTRY_ATTR_BACKEND_IP)
            {
                EXPECT_EQ(attr.value.ipaddr.addr.ip4, port_map1_backend_ip.getV4Addr());
            }
            else if (attr.id == SAI_OUTBOUND_PORT_MAP_PORT_RANGE_ENTRY_ATTR_MATCH_PORT_BASE)
            {
                EXPECT_EQ(attr.value.u32, port_map1_start_port);
            }
            else if (attr.id == SAI_OUTBOUND_PORT_MAP_PORT_RANGE_ENTRY_ATTR_BACKEND_PORT_BASE)
            {
                EXPECT_EQ(attr.value.u32, port_map1_backend_port_base);
            }
        }

        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_RANGE_TABLE_NAME, key, port_map_range, false);
        EXPECT_EQ(removed_entry.outbound_port_map_id, fake_oid);
        EXPECT_EQ(removed_entry.dst_port_range.min, 1000);
        EXPECT_EQ(removed_entry.dst_port_range.max, 2000);
    }

        TEST_F(DashPortMapOrchTest, AddDuplicatePortMapRange)
    {
        auto port_map_range = BuildOutboundPortMapRange();
        std::stringstream key_stream;
        key_stream << port_map1 << ":" << port_map1_start_port << "-" << port_map1_end_port;
        std::string key = key_stream.str();

        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, create_outbound_port_map_port_range_entries).Times(2);
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME, port_map1, dash::outbound_port_map::OutboundPortMap());
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_RANGE_TABLE_NAME, key, port_map_range);
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_RANGE_TABLE_NAME, key, port_map_range, true, false);
    }

    TEST_F(DashPortMapOrchTest, RemoveNonexistPortMapRange)
    {
        std::stringstream key_stream;
        key_stream << port_map1 << ":" << port_map1_start_port << "-" << port_map1_end_port;
        std::string port_map_range_key = key_stream.str();

        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, remove_outbound_port_map_port_range_entries);
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME, port_map1, dash::outbound_port_map::OutboundPortMap());
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_RANGE_TABLE_NAME, port_map_range_key, dash::outbound_port_map_range::OutboundPortMapRange(), false, false);
    }

    TEST_F(DashPortMapOrchTest, AddPortRangeWithoutPortMap)
    {
        auto port_map_range = BuildOutboundPortMapRange();
        std::stringstream key_stream;
        key_stream << port_map1 << ":" << port_map1_start_port << "-" << port_map1_end_port;
        std::string key = key_stream.str();
        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, create_outbound_port_map_port_range_entries).Times(0);
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_RANGE_TABLE_NAME, key, port_map_range, true, false);
    }

    TEST_F(DashPortMapOrchTest, RemoveInUsePortMap)
    {
        auto port_map_range = BuildOutboundPortMapRange();
        std::stringstream key_stream;
        key_stream << port_map1 << ":" << port_map1_start_port << "-" << port_map1_end_port;
        std::string port_map_range_key = key_stream.str();

        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, create_outbound_port_maps);
        EXPECT_CALL(*mock_sai_dash_outbound_port_map_api, create_outbound_port_map_port_range_entries);

        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME, port_map1, dash::outbound_port_map::OutboundPortMap());
        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_RANGE_TABLE_NAME, port_map_range_key, port_map_range);

        SetDashTable(APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME, port_map1, dash::outbound_port_map::OutboundPortMap(), false, false);

    }
}
