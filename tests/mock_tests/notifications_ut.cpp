#include "gtest/gtest.h"

#include <csignal>
#include <memory>

#include "notifications.h"
#include "orch.h"
#include "sai_serialize.h"
#include "saiextensions.h"
#include "sairedis.h"
#include "swss/table.h"

struct IcmpSaiSessionHandler
{
    static void on_state_change(uint32_t count, sai_icmp_echo_session_state_notification_t *data);
};

extern sai_redis_communication_mode_t gRedisCommunicationMode;
extern sai_object_id_t gSwitchId;
extern volatile sig_atomic_t gOrchShutdownRequested;

namespace notifications_test
{

using namespace std;
using namespace swss;

class TestNotificationOrch : public Orch
{
public:
    TestNotificationOrch()
        : Orch()
    {
    }
};

static void drainSaiNotificationQueue()
{
    auto queue = getSaiNotificationQueue();
    std::deque<KeyOpFieldsValuesTuple> entries;
    while (queue->hasData())
    {
        queue->pops(entries);
        entries.clear();
    }
}

class SaiNotificationZmqTest : public ::testing::Test
{
protected:
    sai_redis_communication_mode_t m_oldMode;

    void SetUp() override
    {
        m_oldMode = gRedisCommunicationMode;
        gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC;
        drainSaiNotificationQueue();
    }

    void TearDown() override
    {
        drainSaiNotificationQueue();
        gRedisCommunicationMode = m_oldMode;
    }
};

TEST_F(SaiNotificationZmqTest, EnqueueDroppedDuringShutdown)
{
    gOrchShutdownRequested = SIGTERM;

    enqueueSaiNotification("fdb_event", "data", std::vector<FieldValueTuple>());

    EXPECT_EQ(getSaiNotificationQueue()->size(), 0u);

    gOrchShutdownRequested = 0;
}

TEST(SaiNotificationQueueTest, EnqueueAndPop)
{
    SaiNotificationQueue queue(100, 2);
    std::vector<FieldValueTuple> values;

    ASSERT_FALSE(queue.hasData());

    queue.enqueue("port_state_change", "data1", values);
    queue.enqueue("port_state_change", "data2", values);
    queue.enqueue("port_state_change", "data3", values);

    ASSERT_TRUE(queue.hasData());
    ASSERT_TRUE(queue.hasCachedData());

    std::deque<KeyOpFieldsValuesTuple> entries;
    queue.pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(2));
    EXPECT_EQ(kfvOp(entries[0]), "port_state_change");
    EXPECT_EQ(kfvKey(entries[0]), "data1");
    EXPECT_EQ(kfvOp(entries[1]), "port_state_change");
    EXPECT_EQ(kfvKey(entries[1]), "data2");
    ASSERT_TRUE(queue.hasData());

    queue.pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), "port_state_change");
    EXPECT_EQ(kfvKey(entries[0]), "data3");
    ASSERT_FALSE(queue.hasData());
}

TEST(SaiNotificationQueueTest, DispatcherInvokesRegisteredHandler)
{
    SaiNotificationDispatcher dispatcher;
    std::vector<FieldValueTuple> values;
    KeyOpFieldsValuesTuple entry("payload", "port_state_change", values);
    bool called = false;
    std::string payload;

    dispatcher.registerHandler(
        "port_state_change",
        [&](KeyOpFieldsValuesTuple &dispatchedEntry)
        {
            called = true;
            payload = kfvKey(dispatchedEntry);
        });

    dispatcher.dispatch(entry);

    EXPECT_TRUE(called);
    EXPECT_EQ(payload, "payload");
}

TEST_F(SaiNotificationZmqTest, PortStateChangeCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    sai_port_oper_status_notification_t port_oper_status;
    memset(&port_oper_status, 0, sizeof(port_oper_status));
    port_oper_status.port_id = 0x1000000000019;
    port_oper_status.port_state = SAI_PORT_OPER_STATUS_UP;
    port_oper_status.port_error_status = SAI_PORT_ERROR_STATUS_CLEAR;

    on_port_state_change(1, &port_oper_status);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), "port_state_change");

    uint32_t count = 0;
    sai_port_oper_status_notification_t *deserialized_status = nullptr;
    sai_deserialize_port_oper_status_ntf(kfvKey(entries[0]), count, &deserialized_status);

    ASSERT_EQ(count, static_cast<uint32_t>(1));
    EXPECT_EQ(deserialized_status[0].port_id, port_oper_status.port_id);
    EXPECT_EQ(deserialized_status[0].port_state, port_oper_status.port_state);
    EXPECT_EQ(deserialized_status[0].port_error_status, port_oper_status.port_error_status);

    sai_deserialize_free_port_oper_status_ntf(count, deserialized_status);
    ASSERT_FALSE(queue->hasData());
}

TEST_F(SaiNotificationZmqTest, FdbEventCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    sai_fdb_event_notification_data_t fdb_data;
    memset(&fdb_data, 0, sizeof(fdb_data));
    fdb_data.event_type = SAI_FDB_EVENT_LEARNED;
    fdb_data.fdb_entry.switch_id = 0x21000000000000;
    fdb_data.fdb_entry.bv_id = 0x26000000000a6c;
    uint8_t mac[] = {0x52, 0x54, 0x00, 0x11, 0x22, 0x33};
    memcpy(fdb_data.fdb_entry.mac_address, mac, sizeof(mac));
    fdb_data.attr_count = 0;
    fdb_data.attr = nullptr;

    on_fdb_event(1, &fdb_data);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), "fdb_event");

    uint32_t count = 0;
    sai_fdb_event_notification_data_t *deserialized = nullptr;
    sai_deserialize_fdb_event_ntf(kfvKey(entries[0]), count, &deserialized);

    ASSERT_EQ(count, static_cast<uint32_t>(1));
    EXPECT_EQ(deserialized[0].event_type, fdb_data.event_type);
    EXPECT_EQ(deserialized[0].fdb_entry.switch_id, fdb_data.fdb_entry.switch_id);
    EXPECT_EQ(deserialized[0].fdb_entry.bv_id, fdb_data.fdb_entry.bv_id);
    EXPECT_EQ(memcmp(deserialized[0].fdb_entry.mac_address, mac, sizeof(mac)), 0);

    sai_deserialize_free_fdb_event_ntf(count, deserialized);
    ASSERT_FALSE(queue->hasData());
}

TEST(SaiNotificationQueueTest, PeekFrontOp)
{
    SaiNotificationQueue queue(100, 1);
    std::vector<FieldValueTuple> values;
    std::string op;

    ASSERT_FALSE(queue.peekFrontOp(op));

    queue.enqueue("port_state_change", "data1", values);
    queue.enqueue("fdb_event", "data2", values);

    ASSERT_TRUE(queue.peekFrontOp(op));
    EXPECT_EQ(op, "port_state_change");

    std::deque<KeyOpFieldsValuesTuple> entries;
    queue.pops(entries);
    ASSERT_EQ(entries.size(), static_cast<size_t>(1));

    ASSERT_TRUE(queue.peekFrontOp(op));
    EXPECT_EQ(op, "fdb_event");
}

TEST(SaiNotificationQueueTest, ReadinessPredicateReportsNotReady)
{
    SaiNotificationDispatcher dispatcher;

    dispatcher.registerHandler(
        "port_state_change",
        [](KeyOpFieldsValuesTuple &)
        {
        },
        []() { return false; });

    EXPECT_FALSE(dispatcher.isReady("port_state_change"));

    dispatcher.registerHandler(
        "port_state_change",
        [](KeyOpFieldsValuesTuple &)
        {
        },
        []() { return true; });

    EXPECT_TRUE(dispatcher.isReady("port_state_change"));
}

TEST(SaiNotificationQueueExecutorTest, SaiNotificationQueueExecutor)
{
    SaiNotificationQueue queue(100, 10);
    SaiNotificationDispatcher dispatcher;
    TestNotificationOrch orch;
    std::vector<FieldValueTuple> values;
    bool called = false;

    dispatcher.registerHandler(
        "bfd_session_state_change",
        [&](KeyOpFieldsValuesTuple &)
        {
            called = true;
        });

    queue.enqueue("bfd_session_state_change", "bfd", values);

    std::unique_ptr<Executor> executor(
        createSaiNotificationQueueExecutor(&queue, &orch, &dispatcher, "TEST_EXECUTOR"));

    ASSERT_TRUE(queue.hasData());
    executor->execute();

    EXPECT_TRUE(called);
    EXPECT_FALSE(queue.hasData());
}

TEST(SaiNotificationQueueExecutorTest, SaiNotificationQueueExecutorHeadOfLineBlocksUntilReady)
{
    SaiNotificationQueue queue(100, 10);
    SaiNotificationDispatcher dispatcher;
    TestNotificationOrch orch;
    std::vector<FieldValueTuple> values;
    bool portsReady = false;
    int fdbCalls = 0;
    int bfdCalls = 0;
    std::deque<std::string> dispatchOrder;

    dispatcher.registerHandler(
        "fdb_event",
        [&](KeyOpFieldsValuesTuple &)
        {
            fdbCalls++;
            dispatchOrder.push_back("fdb_event");
        },
        [&]() { return portsReady; });

    dispatcher.registerHandler(
        "bfd_session_state_change",
        [&](KeyOpFieldsValuesTuple &)
        {
            bfdCalls++;
            dispatchOrder.push_back("bfd_session_state_change");
        });

    queue.enqueue("fdb_event", "fdb", values);
    queue.enqueue("bfd_session_state_change", "bfd", values);

    std::unique_ptr<Executor> executor(
        createSaiNotificationQueueExecutor(&queue, &orch, &dispatcher, "TEST_EXECUTOR"));

    executor->execute();

    EXPECT_EQ(fdbCalls, 0);
    EXPECT_EQ(bfdCalls, 0);
    EXPECT_EQ(queue.size(), 2u);
    EXPECT_TRUE(dispatchOrder.empty());

    portsReady = true;
    executor->execute();

    EXPECT_EQ(fdbCalls, 1);
    EXPECT_EQ(bfdCalls, 1);
    EXPECT_FALSE(queue.hasData());
    ASSERT_EQ(dispatchOrder.size(), 2u);
    EXPECT_EQ(dispatchOrder[0], "fdb_event");
    EXPECT_EQ(dispatchOrder[1], "bfd_session_state_change");
}

TEST(SaiNotificationQueueTest, MissingHandlerLogsWarning)
{
    SaiNotificationDispatcher dispatcher;
    std::vector<FieldValueTuple> values;
    KeyOpFieldsValuesTuple entry("payload", "unknown_op", values);

    dispatcher.dispatch(entry);
    SUCCEED();
}

TEST_F(SaiNotificationZmqTest, PortHostTxReadyCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    on_port_host_tx_ready(0x1000000000001, 0x1000000000019, SAI_PORT_HOST_TX_READY_STATUS_READY);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), "port_host_tx_ready");

    sai_object_id_t switch_id = 0;
    sai_object_id_t port_id = 0;
    sai_port_host_tx_ready_status_t status = SAI_PORT_HOST_TX_READY_STATUS_NOT_READY;
    sai_deserialize_port_host_tx_ready_ntf(kfvKey(entries[0]), switch_id, port_id, status);

    EXPECT_EQ(switch_id, static_cast<sai_object_id_t>(0x1000000000001));
    EXPECT_EQ(port_id, static_cast<sai_object_id_t>(0x1000000000019));
    EXPECT_EQ(status, SAI_PORT_HOST_TX_READY_STATUS_READY);
    ASSERT_FALSE(queue->hasData());
}

TEST_F(SaiNotificationZmqTest, BfdSessionStateChangeCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    sai_bfd_session_state_notification_t bfd_session_state;
    memset(&bfd_session_state, 0, sizeof(bfd_session_state));
    bfd_session_state.bfd_session_id = 0x1000000000020;
    bfd_session_state.session_state = SAI_BFD_SESSION_STATE_UP;

    on_bfd_session_state_change(1, &bfd_session_state);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), "bfd_session_state_change");

    uint32_t count = 0;
    sai_bfd_session_state_notification_t *deserialized = nullptr;
    sai_deserialize_bfd_session_state_ntf(kfvKey(entries[0]), count, &deserialized);

    ASSERT_EQ(count, static_cast<uint32_t>(1));
    EXPECT_EQ(deserialized[0].bfd_session_id, bfd_session_state.bfd_session_id);
    EXPECT_EQ(deserialized[0].session_state, bfd_session_state.session_state);

    sai_deserialize_free_bfd_session_state_ntf(count, deserialized);
    ASSERT_FALSE(queue->hasData());
}

TEST_F(SaiNotificationZmqTest, IcmpEchoSessionStateChangeCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    sai_icmp_echo_session_state_notification_t icmp_session_state;
    memset(&icmp_session_state, 0, sizeof(icmp_session_state));
    icmp_session_state.icmp_echo_session_id = 0x1000000000021;
    icmp_session_state.session_state = SAI_ICMP_ECHO_SESSION_STATE_UP;

    IcmpSaiSessionHandler::on_state_change(1, &icmp_session_state);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), "icmp_echo_session_state_change");

    uint32_t count = 0;
    sai_icmp_echo_session_state_notification_t *deserialized = nullptr;
    sai_deserialize_icmp_echo_session_state_ntf(kfvKey(entries[0]), count, &deserialized);

    ASSERT_EQ(count, static_cast<uint32_t>(1));
    EXPECT_EQ(deserialized[0].icmp_echo_session_id, icmp_session_state.icmp_echo_session_id);
    EXPECT_EQ(deserialized[0].session_state, icmp_session_state.session_state);

    sai_deserialize_free_icmp_echo_session_state_ntf(count, deserialized);
    ASSERT_FALSE(queue->hasData());
}

TEST_F(SaiNotificationZmqTest, TwampSessionEventCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    sai_twamp_session_event_notification_data_t twamp_session;
    memset(&twamp_session, 0, sizeof(twamp_session));
    twamp_session.twamp_session_id = 0x1000000000022;
    twamp_session.session_state = SAI_TWAMP_SESSION_STATE_ACTIVE;

    on_twamp_session_event(1, &twamp_session);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), "twamp_session_event");

    uint32_t count = 0;
    sai_twamp_session_event_notification_data_t *deserialized = nullptr;
    sai_deserialize_twamp_session_event_ntf(kfvKey(entries[0]), count, &deserialized);

    ASSERT_EQ(count, static_cast<uint32_t>(1));
    EXPECT_EQ(deserialized[0].twamp_session_id, twamp_session.twamp_session_id);
    EXPECT_EQ(deserialized[0].session_state, twamp_session.session_state);

    sai_deserialize_free_twamp_session_event_ntf(count, deserialized);
    ASSERT_FALSE(queue->hasData());
}

TEST_F(SaiNotificationZmqTest, HaSetEventCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    sai_ha_set_event_data_t ha_set_event;
    memset(&ha_set_event, 0, sizeof(ha_set_event));
    ha_set_event.ha_set_id = 0x1000000000030;
    ha_set_event.event_type = SAI_HA_SET_EVENT_DP_CHANNEL_UP;

    on_ha_set_event(1, &ha_set_event);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), SAI_SWITCH_NOTIFICATION_NAME_HA_SET_EVENT);

    uint32_t count = 0;
    sai_ha_set_event_data_t *deserialized = nullptr;
    sai_deserialize_ha_set_event_ntf(kfvKey(entries[0]), count, &deserialized);

    ASSERT_EQ(count, static_cast<uint32_t>(1));
    EXPECT_EQ(deserialized[0].ha_set_id, ha_set_event.ha_set_id);
    EXPECT_EQ(deserialized[0].event_type, ha_set_event.event_type);

    sai_deserialize_free_ha_set_event_ntf(count, deserialized);
    ASSERT_FALSE(queue->hasData());
}

TEST_F(SaiNotificationZmqTest, HaScopeEventCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    sai_ha_scope_event_data_t ha_scope_event;
    memset(&ha_scope_event, 0, sizeof(ha_scope_event));
    ha_scope_event.ha_scope_id = 0x1000000000031;
    ha_scope_event.event_type = SAI_HA_SCOPE_EVENT_STATE_CHANGED;
    ha_scope_event.ha_state = SAI_DASH_HA_STATE_ACTIVE;

    on_ha_scope_event(1, &ha_scope_event);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), SAI_SWITCH_NOTIFICATION_NAME_HA_SCOPE_EVENT);

    uint32_t count = 0;
    sai_ha_scope_event_data_t *deserialized = nullptr;
    sai_deserialize_ha_scope_event_ntf(kfvKey(entries[0]), count, &deserialized);

    ASSERT_EQ(count, static_cast<uint32_t>(1));
    EXPECT_EQ(deserialized[0].ha_scope_id, ha_scope_event.ha_scope_id);
    EXPECT_EQ(deserialized[0].event_type, ha_scope_event.event_type);

    sai_deserialize_free_ha_scope_event_ntf(count, deserialized);
    ASSERT_FALSE(queue->hasData());
}

TEST_F(SaiNotificationZmqTest, FlowBulkGetSessionEventCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    sai_flow_bulk_get_session_event_data_t flow_event;
    memset(&flow_event, 0, sizeof(flow_event));
    flow_event.event_type = SAI_FLOW_BULK_GET_SESSION_EVENT_FINISHED;

    on_flow_bulk_get_session_event(0x1000000000032, 1, &flow_event);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), SAI_SWITCH_NOTIFICATION_NAME_FLOW_BULK_GET_SESSION_EVENT);

    sai_object_id_t session_id = SAI_NULL_OBJECT_ID;
    uint32_t count = 0;
    sai_flow_bulk_get_session_event_data_t *deserialized = nullptr;
    sai_deserialize_flow_bulk_get_session_event_ntf(kfvKey(entries[0]), session_id, count, &deserialized);

    EXPECT_EQ(session_id, static_cast<sai_object_id_t>(0x1000000000032));
    ASSERT_EQ(count, static_cast<uint32_t>(1));
    EXPECT_EQ(deserialized[0].event_type, flow_event.event_type);

    sai_deserialize_free_flow_bulk_get_session_event_ntf(count, deserialized);
    ASSERT_FALSE(queue->hasData());
}

TEST_F(SaiNotificationZmqTest, SwitchMacsecPostStatusCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    on_switch_macsec_post_status_notify(gSwitchId, SAI_SWITCH_MACSEC_POST_STATUS_PASS);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), SAI_SWITCH_NOTIFICATION_NAME_SWITCH_MACSEC_POST_STATUS);

    sai_object_id_t switch_id = SAI_NULL_OBJECT_ID;
    sai_switch_macsec_post_status_t status = SAI_SWITCH_MACSEC_POST_STATUS_UNKNOWN;
    sai_deserialize_switch_macsec_post_status_ntf(kfvKey(entries[0]), switch_id, status);

    EXPECT_EQ(switch_id, gSwitchId);
    EXPECT_EQ(status, SAI_SWITCH_MACSEC_POST_STATUS_PASS);
    ASSERT_FALSE(queue->hasData());
}

TEST_F(SaiNotificationZmqTest, MacsecPostStatusCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    sai_object_id_t macsec_id = 0x1000000000033;

    on_macsec_post_status_notify(macsec_id, SAI_MACSEC_POST_STATUS_PASS);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), SAI_SWITCH_NOTIFICATION_NAME_MACSEC_POST_STATUS);

    sai_object_id_t deserialized_macsec_id = SAI_NULL_OBJECT_ID;
    sai_macsec_post_status_t status = SAI_MACSEC_POST_STATUS_UNKNOWN;
    sai_deserialize_macsec_post_status_ntf(kfvKey(entries[0]), deserialized_macsec_id, status);

    EXPECT_EQ(deserialized_macsec_id, macsec_id);
    EXPECT_EQ(status, SAI_MACSEC_POST_STATUS_PASS);
    ASSERT_FALSE(queue->hasData());
}

TEST_F(SaiNotificationZmqTest, TamTelTypeConfigChangeCallbackEnqueuesInZmqMode)
{
    auto queue = getSaiNotificationQueue();

    sai_object_id_t tam_tel_id = 0x1000000000034;

    on_tam_tel_type_config_change(tam_tel_id);

    ASSERT_TRUE(queue->hasData());
    std::deque<KeyOpFieldsValuesTuple> entries;
    queue->pops(entries);

    ASSERT_EQ(entries.size(), static_cast<size_t>(1));
    EXPECT_EQ(kfvOp(entries[0]), SAI_SWITCH_NOTIFICATION_NAME_TAM_TEL_TYPE_CONFIG_CHANGE);

    sai_object_id_t deserialized_tam_tel_id = SAI_NULL_OBJECT_ID;
    sai_deserialize_object_id(kfvKey(entries[0]), deserialized_tam_tel_id);

    EXPECT_EQ(deserialized_tam_tel_id, tam_tel_id);
    ASSERT_FALSE(queue->hasData());
}

TEST(NotificationsNonZmqTest, PortStateChangeNoEnqueueInRedisMode)
{
    sai_redis_communication_mode_t oldMode = gRedisCommunicationMode;
    gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;
    drainSaiNotificationQueue();

    sai_port_oper_status_notification_t port_oper_status;
    memset(&port_oper_status, 0, sizeof(port_oper_status));
    port_oper_status.port_id = 0x100;
    port_oper_status.port_state = SAI_PORT_OPER_STATUS_UP;

    on_port_state_change(1, &port_oper_status);

    EXPECT_FALSE(getSaiNotificationQueue()->hasData());

    gRedisCommunicationMode = oldMode;
}

TEST(NotificationsNonZmqTest, TamTelTypeConfigChangeNoEnqueueInRedisMode)
{
    sai_redis_communication_mode_t oldMode = gRedisCommunicationMode;
    gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;
    drainSaiNotificationQueue();

    on_tam_tel_type_config_change(0x500);

    EXPECT_FALSE(getSaiNotificationQueue()->hasData());

    gRedisCommunicationMode = oldMode;
}

TEST(NotificationsNonZmqTest, FdbEventNoEnqueueInRedisMode)
{
    sai_redis_communication_mode_t oldMode = gRedisCommunicationMode;
    gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;
    drainSaiNotificationQueue();

    sai_fdb_event_notification_data_t fdb_data;
    memset(&fdb_data, 0, sizeof(fdb_data));
    fdb_data.event_type = SAI_FDB_EVENT_LEARNED;
    fdb_data.fdb_entry.switch_id = 0x1;
    fdb_data.attr_count = 0;
    fdb_data.attr = nullptr;

    on_fdb_event(1, &fdb_data);

    EXPECT_FALSE(getSaiNotificationQueue()->hasData());

    gRedisCommunicationMode = oldMode;
}

TEST(NotificationsNonZmqTest, BfdSessionStateChangeNoEnqueueInRedisMode)
{
    sai_redis_communication_mode_t oldMode = gRedisCommunicationMode;
    gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;
    drainSaiNotificationQueue();

    sai_bfd_session_state_notification_t bfd_session_state;
    memset(&bfd_session_state, 0, sizeof(bfd_session_state));
    bfd_session_state.bfd_session_id = 0x200;
    bfd_session_state.session_state = SAI_BFD_SESSION_STATE_UP;

    on_bfd_session_state_change(1, &bfd_session_state);

    EXPECT_FALSE(getSaiNotificationQueue()->hasData());

    gRedisCommunicationMode = oldMode;
}

} // namespace notifications_test
