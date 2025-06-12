#include "gtest/gtest.h"
#include <stdlib.h>
#include <string>
#include "schema.h"
#include "ut_helper.h"
#include "orch_zmq_config.h"
#include "dbconnector.h"

#define protected public
#include "orch.h"
#include "zmqorch.h"
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
    config_db.hset("DEVICE_METADATA|localhost", "TEST_FEATURE", "true");
    auto enabled = swss::get_feature_status("TEST_FEATURE", false);
    EXPECT_TRUE(enabled);

    config_db.hdel("DEVICE_METADATA|localhost", "TEST_FEATURE");
    enabled = swss::get_feature_status("TEST_FEATURE", false);
    EXPECT_FALSE(enabled);

    config_db.hset("DEVICE_METADATA|localhost", "TEST_FEATURE", "false");
    enabled = swss::get_feature_status("TEST_FEATURE", true);
    EXPECT_FALSE(enabled);

    config_db.hdel("DEVICE_METADATA|localhost", "TEST_FEATURE");
    enabled = swss::get_feature_status("TEST_FEATURE", true);
    EXPECT_TRUE(enabled);
}