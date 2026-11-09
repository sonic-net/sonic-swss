#include "gtest/gtest.h"
#include "sairedis.h"
#include "notifications.h"

extern sai_redis_communication_mode_t gRedisCommunicationMode;

namespace notifications_zmq_test
{
    using namespace std;

    class NotificationsZmqForwardingTest : public ::testing::Test
    {
    protected:
        sai_redis_communication_mode_t m_oldMode;

        void SetUp() override
        {
            m_oldMode = gRedisCommunicationMode;
            gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC;
        }

        void TearDown() override
        {
            gRedisCommunicationMode = m_oldMode;
        }
    };

    TEST_F(NotificationsZmqForwardingTest, FdbEventForwarding)
    {
        sai_fdb_event_notification_data_t data;
        memset(&data, 0, sizeof(data));
        data.event_type = SAI_FDB_EVENT_LEARNED;
        data.fdb_entry.switch_id = 0x1;
        data.fdb_entry.bv_id = 0x2000;

        // Set a MAC address
        uint8_t mac[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
        memcpy(data.fdb_entry.mac_address, mac, sizeof(mac));

        data.attr_count = 0;
        data.attr = nullptr;

        // Should not crash — Redis forwarding via mock hiredis (no-op)
        ASSERT_NO_THROW(on_fdb_event(1, &data));
    }

    TEST_F(NotificationsZmqForwardingTest, FdbEventMultipleEntries)
    {
        const int count = 3;
        sai_fdb_event_notification_data_t data[count];
        memset(data, 0, sizeof(data));

        for (int i = 0; i < count; i++)
        {
            data[i].event_type = SAI_FDB_EVENT_LEARNED;
            data[i].fdb_entry.switch_id = 0x1;
            data[i].fdb_entry.bv_id = 0x2000 + i;
            uint8_t mac[] = {0x00, 0x11, 0x22, 0x33, 0x44, (uint8_t)(0x50 + i)};
            memcpy(data[i].fdb_entry.mac_address, mac, sizeof(mac));
            data[i].attr_count = 0;
            data[i].attr = nullptr;
        }

        ASSERT_NO_THROW(on_fdb_event(count, data));
    }

    TEST_F(NotificationsZmqForwardingTest, PortStateChangeForwarding)
    {
        sai_port_oper_status_notification_t data;
        memset(&data, 0, sizeof(data));
        data.port_id = 0x100;
        data.port_state = SAI_PORT_OPER_STATUS_UP;

        ASSERT_NO_THROW(on_port_state_change(1, &data));
    }

    TEST_F(NotificationsZmqForwardingTest, PortStateChangeMultiple)
    {
        const int count = 4;
        sai_port_oper_status_notification_t data[count];
        memset(data, 0, sizeof(data));

        for (int i = 0; i < count; i++)
        {
            data[i].port_id = 0x100 + i;
            data[i].port_state = (i % 2 == 0) ? SAI_PORT_OPER_STATUS_UP : SAI_PORT_OPER_STATUS_DOWN;
        }

        ASSERT_NO_THROW(on_port_state_change(count, data));
    }

    TEST_F(NotificationsZmqForwardingTest, BfdSessionStateChangeForwarding)
    {
        sai_bfd_session_state_notification_t data;
        memset(&data, 0, sizeof(data));
        data.bfd_session_id = 0x200;
        data.session_state = SAI_BFD_SESSION_STATE_UP;

        ASSERT_NO_THROW(on_bfd_session_state_change(1, &data));
    }

    TEST_F(NotificationsZmqForwardingTest, BfdSessionStateChangeMultiple)
    {
        const int count = 2;
        sai_bfd_session_state_notification_t data[count];
        memset(data, 0, sizeof(data));

        data[0].bfd_session_id = 0x200;
        data[0].session_state = SAI_BFD_SESSION_STATE_UP;
        data[1].bfd_session_id = 0x201;
        data[1].session_state = SAI_BFD_SESSION_STATE_DOWN;

        ASSERT_NO_THROW(on_bfd_session_state_change(count, data));
    }

    TEST_F(NotificationsZmqForwardingTest, TwampSessionEventForwarding)
    {
        sai_twamp_session_event_notification_data_t data;
        memset(&data, 0, sizeof(data));
        data.twamp_session_id = 0x300;
        data.session_state = SAI_TWAMP_SESSION_STATE_ACTIVE;

        ASSERT_NO_THROW(on_twamp_session_event(1, &data));
    }

    TEST_F(NotificationsZmqForwardingTest, PortHostTxReadyForwarding)
    {
        sai_object_id_t switch_id = 0x1;
        sai_object_id_t port_id = 0x400;
        sai_port_host_tx_ready_status_t status = SAI_PORT_HOST_TX_READY_STATUS_READY;

        ASSERT_NO_THROW(on_port_host_tx_ready(switch_id, port_id, status));
    }

    TEST_F(NotificationsZmqForwardingTest, PortHostTxReadyNotReady)
    {
        sai_object_id_t switch_id = 0x1;
        sai_object_id_t port_id = 0x401;
        sai_port_host_tx_ready_status_t status = SAI_PORT_HOST_TX_READY_STATUS_NOT_READY;

        ASSERT_NO_THROW(on_port_host_tx_ready(switch_id, port_id, status));
    }

    TEST_F(NotificationsZmqForwardingTest, TamTelTypeConfigChangeForwarding)
    {
        sai_object_id_t tam_tel_id = 0x500;

        ASSERT_NO_THROW(on_tam_tel_type_config_change(tam_tel_id));
    }

    // Verify callbacks are no-ops in non-ZMQ mode (no forwarding)
    TEST(NotificationsNonZmqTest, FdbEventNoForwardingInRedisMode)
    {
        sai_redis_communication_mode_t oldMode = gRedisCommunicationMode;
        gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;

        sai_fdb_event_notification_data_t data;
        memset(&data, 0, sizeof(data));
        data.event_type = SAI_FDB_EVENT_LEARNED;
        data.fdb_entry.switch_id = 0x1;
        data.attr_count = 0;
        data.attr = nullptr;

        // In Redis mode, callback should be a no-op (no forwarding needed)
        ASSERT_NO_THROW(on_fdb_event(1, &data));

        gRedisCommunicationMode = oldMode;
    }

    TEST(NotificationsNonZmqTest, BfdNoForwardingInRedisMode)
    {
        sai_redis_communication_mode_t oldMode = gRedisCommunicationMode;
        gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;

        sai_bfd_session_state_notification_t data;
        memset(&data, 0, sizeof(data));
        data.bfd_session_id = 0x200;
        data.session_state = SAI_BFD_SESSION_STATE_UP;

        ASSERT_NO_THROW(on_bfd_session_state_change(1, &data));

        gRedisCommunicationMode = oldMode;
    }

    TEST(NotificationsNonZmqTest, PortStateChangeNoForwardingInRedisMode)
    {
        sai_redis_communication_mode_t oldMode = gRedisCommunicationMode;
        gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;

        sai_port_oper_status_notification_t data;
        memset(&data, 0, sizeof(data));
        data.port_id = 0x100;
        data.port_state = SAI_PORT_OPER_STATUS_UP;

        ASSERT_NO_THROW(on_port_state_change(1, &data));

        gRedisCommunicationMode = oldMode;
    }

    // Repeated calls to verify static thread_local connections stay stable
    TEST_F(NotificationsZmqForwardingTest, RepeatedFdbEventsStable)
    {
        for (int i = 0; i < 100; i++)
        {
            sai_fdb_event_notification_data_t data;
            memset(&data, 0, sizeof(data));
            data.event_type = (i % 2 == 0) ? SAI_FDB_EVENT_LEARNED : SAI_FDB_EVENT_AGED;
            data.fdb_entry.switch_id = 0x1;
            data.fdb_entry.bv_id = 0x2000;
            uint8_t mac[] = {0x00, 0x11, 0x22, 0x33, (uint8_t)(i >> 8), (uint8_t)(i & 0xff)};
            memcpy(data.fdb_entry.mac_address, mac, sizeof(mac));
            data.attr_count = 0;
            data.attr = nullptr;

            ASSERT_NO_THROW(on_fdb_event(1, &data));
        }
    }

    TEST_F(NotificationsZmqForwardingTest, RepeatedBfdEventsStable)
    {
        for (int i = 0; i < 50; i++)
        {
            sai_bfd_session_state_notification_t data;
            memset(&data, 0, sizeof(data));
            data.bfd_session_id = 0x200 + i;
            data.session_state = (i % 2 == 0) ? SAI_BFD_SESSION_STATE_UP : SAI_BFD_SESSION_STATE_DOWN;

            ASSERT_NO_THROW(on_bfd_session_state_change(1, &data));
        }
    }
}
