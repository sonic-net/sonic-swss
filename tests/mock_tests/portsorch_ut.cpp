#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"

#include <sstream>

namespace portsorch_test
{

    using namespace std;

    struct PortsOrchTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;

        PortsOrchTest()
        {
            // FIXME: move out from constructor
            m_app_db = make_shared<swss::DBConnector>(
                APPL_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
            m_config_db = make_shared<swss::DBConnector>(
                CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
            m_state_db = make_shared<swss::DBConnector>(
                STATE_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
        }

        virtual void SetUp() override
        {
            ::testing_db::reset();
        }

        virtual void TearDown() override
        {
            ::testing_db::reset();
            delete gPortsOrch;
            gPortsOrch = nullptr;
            delete gBufferOrch;
            gBufferOrch = nullptr;
        }

        static void SetUpTestCase()
        {
            // Init switch and create dependencies

            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            auto status = ut_helper::initSaiApi(profile);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            sai_attribute_t attr;

            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            // Get switch source MAC address
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);

            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gMacAddress = attr.value.mac;

            // Get the default virtual router ID
            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);

            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gVirtualRouterId = attr.value.oid;
        }

        static void TearDownTestCase()
        {
            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gSwitchId = 0;

            ut_helper::uninitSaiApi();
        }
    };

    TEST_F(PortsOrchTest, PortReadinessColdBoot)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table pgTable = Table(m_config_db.get(), CFG_BUFFER_PG_TABLE_NAME);
        Table profileTable = Table(m_config_db.get(), CFG_BUFFER_PROFILE_TABLE_NAME);
        Table poolTable = Table(m_config_db.get(), CFG_BUFFER_POOL_TABLE_NAME);

        // Get SAI default ports to populate DB

        auto ports = ut_helper::getInitialSaiPorts();

        // Create test buffer pool
        poolTable.set(
            "test_pool",
            {
                { "type", "ingress" },
                { "mode", "dynamic" },
                { "size", "4200000" },
            });

        // Create test buffer profile
        profileTable.set("test_profile", { { "pool", "[BUFFER_POOL|test_pool]" },
                                           { "xon", "14832" },
                                           { "xoff", "14832" },
                                           { "size", "35000" },
                                           { "dynamic_th", "0" } });

        // Apply profile on PGs 3-4 all ports
        for (const auto &it : ports)
        {
            std::ostringstream oss;
            oss << it.first << "|3-4";
            pgTable.set(oss.str(), { { "profile", "[BUFFER_PROFILE|test_profile]" } });
        }

        // Create dependencies ...

        const int portsorch_base_pri = 40;

        vector<table_name_with_pri_t> ports_tables = {
            { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
            { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
            { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
            { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
            { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
        };

        ASSERT_EQ(gPortsOrch, nullptr);
        gPortsOrch = new PortsOrch(m_app_db.get(), ports_tables);
        vector<string> buffer_tables = { CFG_BUFFER_POOL_TABLE_NAME,
                                         CFG_BUFFER_PROFILE_TABLE_NAME,
                                         CFG_BUFFER_QUEUE_TABLE_NAME,
                                         CFG_BUFFER_PG_TABLE_NAME,
                                         CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                         CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };

        ASSERT_EQ(gBufferOrch, nullptr);
        gBufferOrch = new BufferOrch(m_config_db.get(), buffer_tables);

        // Populate pot table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);
        gBufferOrch->addExistingData(&pgTable);
        gBufferOrch->addExistingData(&poolTable);
        gBufferOrch->addExistingData(&profileTable);

        // Apply configuration :
        //  create ports

        static_cast<Orch *>(gBufferOrch)->doTask();

        static_cast<Orch *>(gPortsOrch)->doTask();

        // Ports are not ready yet

        ASSERT_FALSE(gPortsOrch->allPortsReady());

        // Ports host interfaces are created

        portTable.set("PortInitDone", { { "lanes", "0" } });
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        //  configure buffers
        //          ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Since init done is set now, apply buffers
        static_cast<Orch *>(gBufferOrch)->doTask();

        // Ports are not ready yet, mtu, speed left
        ASSERT_FALSE(gPortsOrch->allPortsReady());

        static_cast<Orch *>(gPortsOrch)->doTask();
        ASSERT_TRUE(gPortsOrch->allPortsReady());

        // No more tasks

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        ts.clear();

        gBufferOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
    }

    TEST_F(PortsOrchTest, PortReadinessWarmBoot)
    {

        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table pgTable = Table(m_config_db.get(), CFG_BUFFER_PG_TABLE_NAME);
        Table profileTable = Table(m_config_db.get(), CFG_BUFFER_PROFILE_TABLE_NAME);
        Table poolTable = Table(m_config_db.get(), CFG_BUFFER_POOL_TABLE_NAME);

        // Get SAI default ports to populate DB

        auto ports = ut_helper::getInitialSaiPorts();

        // Create test buffer pool
        poolTable.set(
            "test_pool",
            {
                { "type", "ingress" },
                { "mode", "dynamic" },
                { "size", "4200000" },
            });

        // Create test buffer profile
        profileTable.set("test_profile", { { "pool", "[BUFFER_POOL|test_pool]" },
                                           { "xon", "14832" },
                                           { "xoff", "14832" },
                                           { "size", "35000" },
                                           { "dynamic_th", "0" } });

        // Apply profile on PGs 3-4 all ports
        for (const auto &it : ports)
        {
            std::ostringstream oss;
            oss << it.first << "|3-4";
            pgTable.set(oss.str(), { { "profile", "[BUFFER_PROFILE|test_profile]" } });
        }

        // Populate pot table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone

        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        // Create dependencies ...

        const int portsorch_base_pri = 40;

        vector<table_name_with_pri_t> ports_tables = {
            { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
            { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
            { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
            { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
            { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
        };

        ASSERT_EQ(gPortsOrch, nullptr);
        gPortsOrch = new PortsOrch(m_app_db.get(), ports_tables);
        vector<string> buffer_tables = { CFG_BUFFER_POOL_TABLE_NAME,
                                         CFG_BUFFER_PROFILE_TABLE_NAME,
                                         CFG_BUFFER_QUEUE_TABLE_NAME,
                                         CFG_BUFFER_PG_TABLE_NAME,
                                         CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                         CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };

        ASSERT_EQ(gBufferOrch, nullptr);
        gBufferOrch = new BufferOrch(m_config_db.get(), buffer_tables);

        // warm start, bake fill refill consumer

        gBufferOrch->bake();
        gPortsOrch->bake();

        // Create ports, BufferOrch skips processing
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Ports should not be ready here, buffers not applied,
        // BufferOrch depends on ports to be created

        ASSERT_FALSE(gPortsOrch->allPortsReady());

        // Drain remaining

        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Now ports should be ready

        ASSERT_TRUE(gPortsOrch->allPortsReady());

        // No more tasks

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        ts.clear();

        gBufferOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
    }
}
