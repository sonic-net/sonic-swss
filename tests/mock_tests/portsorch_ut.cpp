#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "pfcactionhandler.h"

#include <sstream>

namespace portsorch_test
{

    using namespace std;

    struct PortsOrchTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_counters_db;

        PortsOrchTest()
        {
            // FIXME: move out from constructor
            m_app_db = make_shared<swss::DBConnector>(
                "APPL_DB", 0);
            m_counters_db = make_shared<swss::DBConnector>(
                "COUNTERS_DB", 0);
            m_config_db = make_shared<swss::DBConnector>(
                "CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>(
                "STATE_DB", 0);
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

    TEST_F(PortsOrchTest, PfcZeroBufferHandlerLocksPortPgAndQueue)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table pgTable = Table(m_config_db.get(), CFG_BUFFER_PG_TABLE_NAME);
        Table profileTable = Table(m_config_db.get(), CFG_BUFFER_PROFILE_TABLE_NAME);
        Table poolTable = Table(m_config_db.get(), CFG_BUFFER_POOL_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

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

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports

        static_cast<Orch *>(gPortsOrch)->doTask();

        // Apply configuration
        //          ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        ASSERT_TRUE(gPortsOrch->allPortsReady());

        // No more tasks
        vector<string> ts;
        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ts.clear();

        // Simulate storm drop handler started on Ethernet0 TC 3
        Port port;
        gPortsOrch->getPort("Ethernet0", port);

        auto countersTable = make_shared<Table>(m_counters_db.get(), COUNTERS_TABLE);
        auto dropHandler = make_unique<PfcWdZeroBufferHandler>(port.m_port_id, port.m_queue_ids[3], 3, countersTable);

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
        gBufferOrch->addExistingData(&pgTable);
        gBufferOrch->addExistingData(&poolTable);
        gBufferOrch->addExistingData(&profileTable);

        // process pool, profile and PGs
        static_cast<Orch *>(gBufferOrch)->doTask();

        // Port should have been updated by BufferOrch->doTask
        gPortsOrch->getPort("Ethernet0", port);
        auto profile_id = (*BufferOrch::m_buffer_type_maps["BUFFER_PROFILE"])[string("test_profile")].m_saiObjectId;
        ASSERT_TRUE(profile_id != SAI_NULL_OBJECT_ID);
        ASSERT_TRUE(port.m_priority_group_pending_profile[3] == profile_id);
        ASSERT_TRUE(port.m_priority_group_pending_profile[4] == SAI_NULL_OBJECT_ID);

        auto pgConsumer = static_cast<Consumer*>(gBufferOrch->getExecutor(CFG_BUFFER_PG_TABLE_NAME));
        pgConsumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty()); // PG is stored in m_priority_group_pending_profile
        ts.clear();

        // release zero buffer drop handler
        dropHandler.reset();

        // re-fetch the port
        gPortsOrch->getPort("Ethernet0", port);

        // pending profile should be cleared
        ASSERT_TRUE(port.m_priority_group_pending_profile[3] == SAI_NULL_OBJECT_ID);
        ASSERT_TRUE(port.m_priority_group_pending_profile[4] == SAI_NULL_OBJECT_ID);

        // process PGs
        static_cast<Orch *>(gBufferOrch)->doTask();

        pgConsumer = static_cast<Consumer*>(gBufferOrch->getExecutor(CFG_BUFFER_PG_TABLE_NAME));
        pgConsumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty()); // PG should be proceesed now
        ts.clear();
    }

    /*
    * The scope of this test is to verify that LAG member is
    * added to a LAG before any other object on LAG is created, like RIF, bridge port in warm mode.
    * For objects like RIF which are created by a different Orch we know that they will wait until
    * allPortsReady(), so we can guaranty they won't be created if PortsOrch can process ports, lags,
    * vlans in single doTask().
    * If objects are created in PortsOrch, like bridge port, we will spy on SAI API to verify they are
    * not called before create_lag_member.
    * This is done like this because of limitation on Mellanox platform that does not allow to create objects
    * on LAG before at least one LAG members is added in warm reboot. Later this will be fixed.
    *
    */
    TEST_F(PortsOrchTest, LagMemberIsCreatedBeforeOtherObjectsAreCreatedOnLag)
    {

        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table lagTable = Table(m_app_db.get(), APP_LAG_TABLE_NAME);
        Table lagMemberTable = Table(m_app_db.get(), APP_LAG_MEMBER_TABLE_NAME);
        Table vlanTable = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
        Table vlanMemberTable = Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

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

        /*
         * Next we will prepare some configuration data to be consumed by PortsOrch
         * 32 Ports, 1 LAG, 1 port is LAG member and LAG is in Vlan.
         */

        // Populate pot table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { } });

        lagTable.set("PortChannel0001",
            {
                {"admin_status", "up"},
                {"mtu", "9100"}
            }
        );
        lagMemberTable.set(
            std::string("PortChannel0001") + lagMemberTable.getTableNameSeparator() + ports.begin()->first,
            { {"status", "enabled"} });
        vlanTable.set("Vlan5",
            {
                {"admin_status", "up"},
                {"mtu", "9100"}
            }
        );
        vlanMemberTable.set(
            std::string("Vlan5") + vlanMemberTable.getTableNameSeparator() + std::string("PortChannel0001"),
            { {"tagging_mode", "untagged"} }
        );

        // refill consumer
        gPortsOrch->addExistingData(&portTable);
        gPortsOrch->addExistingData(&lagTable);
        gPortsOrch->addExistingData(&lagMemberTable);
        gPortsOrch->addExistingData(&vlanTable);
        gPortsOrch->addExistingData(&vlanMemberTable);

        // save original api since we will spy
        auto orig_lag_api = sai_lag_api;
        sai_lag_api = new sai_lag_api_t();
        memcpy(sai_lag_api, orig_lag_api, sizeof(*sai_lag_api));

        auto orig_bridge_api = sai_bridge_api;
        sai_bridge_api = new sai_bridge_api_t();
        memcpy(sai_bridge_api, orig_bridge_api, sizeof(*sai_bridge_api));

        bool bridgePortCalled = false;
        bool bridgePortCalledBeforeLagMember = false;

        auto lagSpy = SpyOn<SAI_API_LAG, SAI_OBJECT_TYPE_LAG_MEMBER>(&sai_lag_api->create_lag_member);
        lagSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t * attrs) -> sai_status_t {
                if (bridgePortCalled) {
                    bridgePortCalledBeforeLagMember = true;
                }
                return orig_lag_api->create_lag_member(oid, swoid, count, attrs);
            }
        );

        auto bridgeSpy = SpyOn<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT>(&sai_bridge_api->create_bridge_port);
        bridgeSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t * attrs) -> sai_status_t {
                bridgePortCalled = true;
                return orig_bridge_api->create_bridge_port(oid, swoid, count, attrs);
            }
        );

        static_cast<Orch *>(gPortsOrch)->doTask();

        vector<string> ts;

        // check LAG, VLAN tasks were proceesed
        // port table may require one more doTask iteration
        for (auto tableName: {
                APP_LAG_TABLE_NAME,
                APP_LAG_MEMBER_TABLE_NAME,
                APP_VLAN_TABLE_NAME,
                APP_VLAN_MEMBER_TABLE_NAME})
        {
            auto exec = gPortsOrch->getExecutor(tableName);
            auto consumer = static_cast<Consumer*>(exec);
            ts.clear();
            consumer->dumpPendingTasks(ts);
            ASSERT_TRUE(ts.empty());
        }

        ASSERT_FALSE(bridgePortCalledBeforeLagMember); // bridge port created on lag before lag member was created
    }

}
