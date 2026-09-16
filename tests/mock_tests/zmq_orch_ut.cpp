#include "gtest/gtest.h"
#include <stdlib.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <unistd.h>
#include "schema.h"
#include "ut_helper.h"
#include "orch_zmq_config.h"
#include "dbconnector.h"
#include "mock_table.h"

#define protected public
#include "orch.h"
#include "zmqorch.h"
#undef protected

#define MAX_RETRY     10

using namespace std;

TEST(ZmqOrchTest, CreateZmqOrchWitTableNames)
{   
    vector<table_name_with_pri_t> tables = {
        { "TABLE_1", 1 },
        { "TABLE_2", 2 },
        { "TABLE_3", 3 }
    };

    auto app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    auto zmq_orch = make_shared<ZmqOrch>(app_db.get(), tables, nullptr);
    
    EXPECT_EQ(zmq_orch->getSelectables().size(), tables.size());
}

TEST(ZmqOrchTest, CreateZmqRouteOrchWithTableNamesAndPri)
{
    vector<table_name_with_pri_t> tables = {
        { "ROUTE_TABLE_1", 1 },
        { "ROUTE_TABLE_2", 2 },
        { "ROUTE_TABLE_3", 3 }
    };

    auto app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    auto zmq_route_orch = make_shared<ZmqRouteOrch>(app_db.get(), tables, nullptr);

    EXPECT_EQ(zmq_route_orch->getSelectables().size(), tables.size());
}

TEST(ZmqOrchTest, CreateZmqRouteOrchWithTableNames)
{
    vector<string> tables = { "ROUTE_TABLE_A", "ROUTE_TABLE_B" };

    auto app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    auto zmq_route_orch = make_shared<ZmqRouteOrch>(app_db.get(), tables, nullptr);

    EXPECT_EQ(zmq_route_orch->getSelectables().size(), tables.size());
}

TEST(ZmqOrchTest, ZmqRouteConsumerExecuteEmpty)
{
    string zmq_server_address = "tcp://127.0.0.1:18100";
    auto zmq_server = swss::create_zmq_route_server(zmq_server_address);

    auto app_db = make_shared<swss::DBConnector>("APPL_DB", 0);

    // Hosting ZmqRouteOrch — used only as the parent Orch pointer for the
    // ZmqRouteConsumer below (nullptr server so it doesn't register a real
    // ZMQ consumer of its own).
    vector<table_name_with_pri_t> empty_tables;
    auto host_orch = make_shared<ZmqRouteOrch>(app_db.get(), empty_tables, nullptr);

    // Construct ZmqRouteConsumerStateTable with dbPersistence=false so no
    // AsyncDBUpdater / Redis activity happens.
    auto* cst = new swss::ZmqRouteConsumerStateTable(
        app_db.get(), "ROUTE_TABLE_E", *zmq_server,
        /*pri=*/1, /*dbPersistence=*/false);
    auto* consumer = new ZmqRouteConsumer(cst, host_orch.get(), "ROUTE_TABLE_E");

    // With no messages received, pops() returns empty, addToSync returns 0,
    // the do-while exits after one iteration, and drain() is a no-op.
    consumer->execute();

    delete consumer;
}

TEST(ZmqOrchTest, GetZMQPort)
{
    const char* backup_nsid = getenv("NAMESPACE_ID");

    // ZMQ port for global namespace
    unsetenv("NAMESPACE_ID");
    int port = swss::get_zmq_port();
    EXPECT_EQ(port, ORCH_ZMQ_PORT);

    // ZMQ port for namespace 0
    setenv("NAMESPACE_ID", "0", true);
    port = swss::get_zmq_port();
    EXPECT_EQ(port, ORCH_ZMQ_PORT+1);

    if (backup_nsid == nullptr)
    {
        unsetenv("NAMESPACE_ID");
    }
    else
    {
        setenv("NAMESPACE_ID", backup_nsid, true);
    }
}

class ZmqHandler : public ZmqMessageHandler
{
public:
    ZmqHandler()
    {
        received = false;
    }

    virtual void handleReceivedData(const std::vector<std::shared_ptr<KeyOpFieldsValuesTuple>>& kcos)
    {
        received = true;
    }

    bool received;
};

TEST(ZmqOrchTest, CreateZmqClient)
{
    string zmq_server_address = "tcp://127.0.0.1";
    ZmqHandler zmq_handler;
    auto zmq_server = swss::create_zmq_server(zmq_server_address);
    auto zmq_client = swss::create_zmq_client(zmq_server_address);

    zmq_server->registerMessageHandler("test_db", "test_table", &zmq_handler);
    zmq_server->bind();
    
    std::vector<KeyOpFieldsValuesTuple> value;
    zmq_client->sendMsg("test_db", "test_table", value);

    int retry = 0;
    while (retry < MAX_RETRY)
    {
        if (zmq_handler.received)
        {
            break;
        }

        sleep(1);
    }

    EXPECT_TRUE(zmq_handler.received);
}

TEST(ZmqOrchTest, GetFeatureStatus)
{
    DBConnector config_db("CONFIG_DB", 0);
    config_db.hset("DEVICE_METADATA|localhost", ORCH_NORTHBOND_DASH_ZMQ_ENABLED, "true");
    auto enabled = swss::get_feature_status(ORCH_NORTHBOND_DASH_ZMQ_ENABLED, false);
    EXPECT_TRUE(enabled);

    config_db.hdel("DEVICE_METADATA|localhost", ORCH_NORTHBOND_DASH_ZMQ_ENABLED);
    enabled = swss::get_feature_status(ORCH_NORTHBOND_DASH_ZMQ_ENABLED, false);
    EXPECT_FALSE(enabled);

    config_db.hset("DEVICE_METADATA|localhost", ORCH_NORTHBOND_DASH_ZMQ_ENABLED, "false");
    enabled = swss::get_feature_status(ORCH_NORTHBOND_DASH_ZMQ_ENABLED, true);
    EXPECT_FALSE(enabled);

    config_db.hdel("DEVICE_METADATA|localhost", ORCH_NORTHBOND_DASH_ZMQ_ENABLED);
    enabled = swss::get_feature_status(ORCH_NORTHBOND_DASH_ZMQ_ENABLED, true);
    EXPECT_TRUE(enabled);
}

TEST(ZmqOrchTest, GetFeatureStatusException)
{
    DBConnector config_db("CONFIG_DB", 0);
    config_db.hset("DEVICE_METADATA|localhost", HGET_THROW_EXCEPTION_FIELD_NAME, "false");
    auto enabled = swss::get_feature_status(HGET_THROW_EXCEPTION_FIELD_NAME, true);
    EXPECT_TRUE(enabled);

    config_db.hset("DEVICE_METADATA|localhost", HGET_THROW_EXCEPTION_FIELD_NAME, "true");
    enabled = swss::get_feature_status(HGET_THROW_EXCEPTION_FIELD_NAME, false);
    EXPECT_FALSE(enabled);
}

TEST(ZmqOrchTest, GetRoutePerfZmqEnabled)
{
    DBConnector config_db("CONFIG_DB", 0);

    // Test: enabled
    config_db.hset(SYSTEM_DEFAULTS_SWSS_ZMQ_KEY, SYSTEM_DEFAULTS_STATUS_FIELD, "enabled");
    EXPECT_TRUE(swss::get_route_perf_zmq_enabled());

    // Test: disabled
    config_db.hset(SYSTEM_DEFAULTS_SWSS_ZMQ_KEY, SYSTEM_DEFAULTS_STATUS_FIELD, "disabled");
    EXPECT_FALSE(swss::get_route_perf_zmq_enabled());

    // Test: missing key → default false
    config_db.del(SYSTEM_DEFAULTS_SWSS_ZMQ_KEY);
    EXPECT_FALSE(swss::get_route_perf_zmq_enabled());
}

// The ZMQ route path and warm/fast restart are mutually exclusive: the coalescer
// owns the ZMQ route writes for the life of the process, and warm-restart
// reconcile writes the same tables directly. Both daemons resolve this from the
// same helper, so producer and consumer cannot disagree. An unsupported
// combination must refuse to start rather than run with two writers.
TEST(ZmqOrchTest, RoutePerfZmqRefusesWarmOrFastRestart)
{
    DBConnector config_db("CONFIG_DB", 0);
    DBConnector state_db("STATE_DB", 0);
    config_db.hset(SYSTEM_DEFAULTS_SWSS_ZMQ_KEY, SYSTEM_DEFAULTS_STATUS_FIELD, "enabled");

    const std::vector<std::string> keys = {
        "WARM_RESTART_ENABLE_TABLE|system",
        "WARM_RESTART_ENABLE_TABLE|bgp",
        "WARM_RESTART_ENABLE_TABLE|swss",
        "FAST_RESTART_ENABLE_TABLE|system",
    };

    for (const auto &key : keys)
    {
        state_db.hset(key, "enable", "true");
        EXPECT_THROW(swss::validate_route_perf_zmq_supported(), std::runtime_error) << key;
        // The getter itself stays a pure read of the knob and never throws.
        EXPECT_TRUE(swss::get_route_perf_zmq_enabled()) << key;
        state_db.hdel(key, "enable");
        // With the key cleared the same call must pass, so the throw is
        // attributable to that key alone.
        EXPECT_NO_THROW(swss::validate_route_perf_zmq_supported()) << key;
    }

    // "enable" set to anything other than "true" is not armed.
    state_db.hset("WARM_RESTART_ENABLE_TABLE|system", "enable", "false");
    EXPECT_NO_THROW(swss::validate_route_perf_zmq_supported());
    state_db.hdel("WARM_RESTART_ENABLE_TABLE|system", "enable");

    // Warm restart armed while ZMQ is disabled stays the normal, supported case.
    config_db.hset(SYSTEM_DEFAULTS_SWSS_ZMQ_KEY, SYSTEM_DEFAULTS_STATUS_FIELD, "disabled");
    state_db.hset("WARM_RESTART_ENABLE_TABLE|system", "enable", "true");
    EXPECT_FALSE(swss::get_route_perf_zmq_enabled());
    EXPECT_NO_THROW(swss::validate_route_perf_zmq_supported());

    state_db.hdel("WARM_RESTART_ENABLE_TABLE|system", "enable");
    config_db.del(SYSTEM_DEFAULTS_SWSS_ZMQ_KEY);
}

TEST(ZmqOrchTest, CreateRoutePerfZmqClient)
{
    DBConnector config_db("CONFIG_DB", 0);

    // When enabled: returns a non-null ZmqClient
    config_db.hset(SYSTEM_DEFAULTS_SWSS_ZMQ_KEY, SYSTEM_DEFAULTS_STATUS_FIELD, "enabled");
    auto client = swss::create_route_perf_zmq_client();
    EXPECT_NE(client, nullptr);

    // When disabled: returns nullptr
    config_db.hset(SYSTEM_DEFAULTS_SWSS_ZMQ_KEY, SYSTEM_DEFAULTS_STATUS_FIELD, "disabled");
    client = swss::create_route_perf_zmq_client();
    EXPECT_EQ(client, nullptr);

    // When key missing: returns nullptr
    config_db.del(SYSTEM_DEFAULTS_SWSS_ZMQ_KEY);
    client = swss::create_route_perf_zmq_client();
    EXPECT_EQ(client, nullptr);
}
