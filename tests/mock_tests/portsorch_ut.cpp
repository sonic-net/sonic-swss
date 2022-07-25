#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private

#include "json.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "notifier.h"
#define private public
#include "pfcactionhandler.h"
#undef private

#include <sstream>

extern redisReply *mockReply;

namespace portsorch_test
{

    using namespace std;

    sai_port_api_t ut_sai_port_api;
    sai_port_api_t *pold_sai_port_api;

    bool not_support_fetching_fec;
    vector<sai_port_fec_mode_t> mock_port_fec_modes = {SAI_PORT_FEC_MODE_RS, SAI_PORT_FEC_MODE_FC};

    sai_status_t _ut_stub_sai_get_port_attribute(
        _In_ sai_object_id_t port_id,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
    {
        sai_status_t status;
        if (attr_count == 1 && attr_list[0].id == SAI_PORT_ATTR_SUPPORTED_FEC_MODE)
        {
            if (not_support_fetching_fec)
            {
                status = SAI_STATUS_NOT_IMPLEMENTED;
            }
            else
            {
                uint32_t i;
                for (i = 0; i < attr_list[0].value.s32list.count && i < mock_port_fec_modes.size(); i++)
                {
                    attr_list[0].value.s32list.list[i] = mock_port_fec_modes[i];
                }
                attr_list[0].value.s32list.count = i;
                status = SAI_STATUS_SUCCESS;
            }
        }
        else
        {
            status = pold_sai_port_api->get_port_attribute(port_id, attr_count, attr_list);
        }
        return status;
    }

    uint32_t _sai_set_port_fec_count;
    int32_t _sai_port_fec_mode;
    sai_status_t _ut_stub_sai_set_port_attribute(
        _In_ sai_object_id_t port_id,
        _In_ const sai_attribute_t *attr)
    {
        if (attr[0].id == SAI_PORT_ATTR_FEC_MODE)
        {
            _sai_set_port_fec_count++;
            _sai_port_fec_mode = attr[0].value.s32;
        }
        return pold_sai_port_api->set_port_attribute(port_id, attr);
    }

    void _hook_sai_port_api()
    {
        ut_sai_port_api = *sai_port_api;
        pold_sai_port_api = sai_port_api;
        ut_sai_port_api.get_port_attribute = _ut_stub_sai_get_port_attribute;
        ut_sai_port_api.set_port_attribute = _ut_stub_sai_set_port_attribute;
        sai_port_api = &ut_sai_port_api;
    }

    void _unhook_sai_port_api()
    {
        sai_port_api = pold_sai_port_api;
    }

    struct PortsOrchTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_counters_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;
        shared_ptr<swss::DBConnector> m_asic_db;

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
            m_chassis_app_db = make_shared<swss::DBConnector>(
                "CHASSIS_APP_DB", 0);
            m_asic_db = make_shared<swss::DBConnector>(
                "ASIC_DB", 0);
        }

        virtual void SetUp() override
        {
            ::testing_db::reset();

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

            vector<string> flex_counter_tables = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());
            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };

            ASSERT_EQ(gBufferOrch, nullptr);
            gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_app_db.get(), APP_INTF_TABLE_NAME, gVrfOrch, m_chassis_app_db.get());

            const int fdborch_pri = 20;

            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri},
                { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri},
                { APP_MCLAG_FDB_TABLE_NAME,  fdborch_pri}
            };

            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            ASSERT_EQ(gFdbOrch, nullptr);
            gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch);

            ASSERT_EQ(gNeighOrch, nullptr);
            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, gPortsOrch, m_chassis_app_db.get());
        }

        virtual void TearDown() override
        {
            ::testing_db::reset();

            auto buffer_maps = BufferOrch::m_buffer_type_maps;
            for (auto &i : buffer_maps)
            {
                i.second->clear();
            }

            delete gNeighOrch;
            gNeighOrch = nullptr;
            delete gFdbOrch;
            gFdbOrch = nullptr;
            delete gIntfsOrch;
            gIntfsOrch = nullptr;
            delete gPortsOrch;
            gPortsOrch = nullptr;
            delete gBufferOrch;
            gBufferOrch = nullptr;

            // clear orchs saved in directory
            gDirectory.m_values.clear();
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

    TEST_F(PortsOrchTest, PortSupportedFecModes)
    {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        not_support_fetching_fec = false;
        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_port_fec_count;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"fec", "rs"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        ASSERT_EQ(_sai_set_port_fec_count, ++current_sai_api_call_count);
        ASSERT_EQ(_sai_port_fec_mode, SAI_PORT_FEC_MODE_RS);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"fec", "none"}
                           }});
        consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();

        ASSERT_EQ(_sai_set_port_fec_count, current_sai_api_call_count);
        ASSERT_EQ(_sai_port_fec_mode, SAI_PORT_FEC_MODE_RS);

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 0);

        _unhook_sai_port_api();
    }

    /*
     * Test case: SAI_PORT_ATTR_SUPPORTED_FEC_MODE is not supported by vendor
     **/
    TEST_F(PortsOrchTest, PortNotSupportedFecModes)
    {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        not_support_fetching_fec = true;
        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_port_fec_count;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"fec", "rs"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        ASSERT_EQ(_sai_set_port_fec_count, ++current_sai_api_call_count);
        ASSERT_EQ(_sai_port_fec_mode, SAI_PORT_FEC_MODE_RS);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _unhook_sai_port_api();
    }

    /*
     * Test case: Fetching SAI_PORT_ATTR_SUPPORTED_FEC_MODE is supported but no FEC mode is supported on the port
     **/
    TEST_F(PortsOrchTest, PortSupportNoFecModes)
    {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        not_support_fetching_fec = false;
        auto old_mock_port_fec_modes = mock_port_fec_modes;
        mock_port_fec_modes.clear();
        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_port_fec_count;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"fec", "rs"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        ASSERT_EQ(_sai_set_port_fec_count, current_sai_api_call_count);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        mock_port_fec_modes = old_mock_port_fec_modes;
        _unhook_sai_port_api();
    }

    TEST_F(PortsOrchTest, PortReadinessColdBoot)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table pgTable = Table(m_app_db.get(), APP_BUFFER_PG_TABLE_NAME);
        Table pgTableCfg = Table(m_config_db.get(), CFG_BUFFER_PG_TABLE_NAME);
        Table profileTable = Table(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);
        Table poolTable = Table(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);

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
        profileTable.set("test_profile", { { "pool", "test_pool" },
                                           { "xon", "14832" },
                                           { "xoff", "14832" },
                                           { "size", "35000" },
                                           { "dynamic_th", "0" } });

        // Apply profile on PGs 3-4 all ports
        for (const auto &it : ports)
        {
            std::ostringstream ossAppl, ossCfg;
            ossAppl << it.first << ":3-4";
            pgTable.set(ossAppl.str(), { { "profile", "test_profile" } });
            ossCfg << it.first << "|3-4";
            pgTableCfg.set(ossCfg.str(), { { "profile", "test_profile" } });
        }

        // Recreate buffer orch to read populated data
        vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                         APP_BUFFER_PROFILE_TABLE_NAME,
                                         APP_BUFFER_QUEUE_TABLE_NAME,
                                         APP_BUFFER_PG_TABLE_NAME,
                                         APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                         APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };

        gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

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
        Table pgTable = Table(m_app_db.get(), APP_BUFFER_PG_TABLE_NAME);
        Table profileTable = Table(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);
        Table poolTable = Table(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);

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
        profileTable.set("test_profile", { { "pool", "test_pool" },
                                           { "xon", "14832" },
                                           { "xoff", "14832" },
                                           { "size", "35000" },
                                           { "dynamic_th", "0" } });

        // Apply profile on PGs 3-4 all ports
        for (const auto &it : ports)
        {
            std::ostringstream oss;
            oss << it.first << ":3-4";
            pgTable.set(oss.str(), { { "profile", "test_profile" } });
        }

        // Populate pot table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone

        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

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

    TEST_F(PortsOrchTest, PfcZeroBufferHandler)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table pgTable = Table(m_app_db.get(), APP_BUFFER_PG_TABLE_NAME);
        Table profileTable = Table(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);
        Table poolTable = Table(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);
        Table queueTable = Table(m_app_db.get(), APP_BUFFER_QUEUE_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

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
            "egress_pool",
            {
                { "type", "egress" },
                { "mode", "dynamic" },
                { "size", "4200000" },
            });
        poolTable.set(
            "ingress_pool",
            {
                { "type", "ingress" },
                { "mode", "dynamic" },
                { "size", "4200000" },
            });

        // Create test buffer profile
        profileTable.set("ingress_profile", { { "pool", "ingress_pool" },
                                              { "xon", "14832" },
                                              { "xoff", "14832" },
                                              { "size", "35000" },
                                              { "dynamic_th", "0" } });
        profileTable.set("egress_profile", { { "pool", "egress_pool" },
                                             { "size", "0" },
                                             { "dynamic_th", "0" } });

        // Apply profile on Queue and PGs 3-4 all ports
        for (const auto &it : ports)
        {
            std::ostringstream oss;
            oss << it.first << ":3-4";
            pgTable.set(oss.str(), { { "profile", "ingress_profile" } });
            queueTable.set(oss.str(), { {"profile", "egress_profile" } });
        }
        gBufferOrch->addExistingData(&pgTable);
        gBufferOrch->addExistingData(&poolTable);
        gBufferOrch->addExistingData(&profileTable);
        gBufferOrch->addExistingData(&queueTable);

        // process pool, profile and Q's
        static_cast<Orch *>(gBufferOrch)->doTask();

        auto queueConsumer = static_cast<Consumer*>(gBufferOrch->getExecutor(APP_BUFFER_QUEUE_TABLE_NAME));
        queueConsumer->dumpPendingTasks(ts);
        ASSERT_FALSE(ts.empty()); // Queue is skipped
        ts.clear();

        auto pgConsumer = static_cast<Consumer*>(gBufferOrch->getExecutor(APP_BUFFER_PG_TABLE_NAME));
        pgConsumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty()); // PG Notification is not skipped
        ts.clear();

        // release zero buffer drop handler
        dropHandler.reset();

        // process queue
        static_cast<Orch *>(gBufferOrch)->doTask();

        queueConsumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty()); // queue should be processed now
        ts.clear();
    }

    /* This test checks that a LAG member validation happens on orchagent level
     * and no SAI call is executed in case a port requested to be a LAG member
     * is already a LAG member.
     */
    TEST_F(PortsOrchTest, LagMemberDoesNotCallSAIApiWhenPortIsAlreadyALagMember)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table lagTable = Table(m_app_db.get(), APP_LAG_TABLE_NAME);
        Table lagMemberTable = Table(m_app_db.get(), APP_LAG_MEMBER_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        /*
         * Next we will prepare some configuration data to be consumed by PortsOrch
         * 32 Ports, 2 LAGs, 1 port is LAG member.
         */

        // Populate pot table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { } });

        lagTable.set("PortChannel999",
            {
                {"admin_status", "up"},
                {"mtu", "9100"}
            }
        );
        lagTable.set("PortChannel0001",
            {
                {"admin_status", "up"},
                {"mtu", "9100"}
            }
        );
        lagMemberTable.set(
            std::string("PortChannel999") + lagMemberTable.getTableNameSeparator() + ports.begin()->first,
            { {"status", "enabled"} });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);
        gPortsOrch->addExistingData(&lagTable);
        gPortsOrch->addExistingData(&lagMemberTable);

        static_cast<Orch *>(gPortsOrch)->doTask();

        // check LAG, VLAN tasks were processed
        // port table may require one more doTask iteration
        for (auto tableName: {APP_LAG_TABLE_NAME, APP_LAG_MEMBER_TABLE_NAME})
        {
            vector<string> ts;
            auto exec = gPortsOrch->getExecutor(tableName);
            auto consumer = static_cast<Consumer*>(exec);
            ts.clear();
            consumer->dumpPendingTasks(ts);
            ASSERT_TRUE(ts.empty());
        }

        // Set first port as a LAG member while this port is still a member of different LAG.
        lagMemberTable.set(
            std::string("PortChannel0001") + lagMemberTable.getTableNameSeparator() + ports.begin()->first,
            { {"status", "enabled"} });

        // save original api since we will spy
        auto orig_lag_api = sai_lag_api;
        sai_lag_api = new sai_lag_api_t();
        memcpy(sai_lag_api, orig_lag_api, sizeof(*sai_lag_api));

        bool lagMemberCreateCalled = false;

        auto lagSpy = SpyOn<SAI_API_LAG, SAI_OBJECT_TYPE_LAG_MEMBER>(&sai_lag_api->create_lag_member);
        lagSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t * attrs) -> sai_status_t
            {
                lagMemberCreateCalled = true;
                return orig_lag_api->create_lag_member(oid, swoid, count, attrs);
            }
        );

        gPortsOrch->addExistingData(&lagMemberTable);

        static_cast<Orch *>(gPortsOrch)->doTask();
        sai_lag_api = orig_lag_api;

        // verify there is a pending task to do.
        vector<string> ts;
        auto exec = gPortsOrch->getExecutor(APP_LAG_MEMBER_TABLE_NAME);
        auto consumer = static_cast<Consumer*>(exec);
        ts.clear();
        consumer->dumpPendingTasks(ts);
        ASSERT_FALSE(ts.empty());

        // verify there was no SAI call executed.
        ASSERT_FALSE(lagMemberCreateCalled);
    }

    /*
    * The scope of this test is a negative test which verify that:
    * if port operational status is up but operational speed is 0, the port speed should not be
    * updated to DB.
    */
    TEST_F(PortsOrchTest, PortOperStatusIsUpAndOperSpeedIsZero)
    {   
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

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
        // Apply configuration : create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Get first port, expect the oper status is not UP
        Port port;
        gPortsOrch->getPort("Ethernet0", port);
        ASSERT_TRUE(port.m_oper_status != SAI_PORT_OPER_STATUS_UP);
        
        // save original api since we will spy
        auto orig_port_api = sai_port_api;
        sai_port_api = new sai_port_api_t();
        memcpy(sai_port_api, orig_port_api, sizeof(*sai_port_api));

        // mock SAI API sai_port_api->get_port_attribute
        auto portSpy = SpyOn<SAI_API_PORT, SAI_OBJECT_TYPE_PORT>(&sai_port_api->get_port_attribute);
        portSpy->callFake([&](sai_object_id_t oid, uint32_t count, sai_attribute_t * attrs) -> sai_status_t {
                if (attrs[0].id == SAI_PORT_ATTR_OPER_STATUS)
                {
                    attrs[0].value.u32 = (uint32_t)SAI_PORT_OPER_STATUS_UP;
                }
                else if (attrs[0].id == SAI_PORT_ATTR_OPER_SPEED)
                {
                    // Return 0 for port operational speed
                    attrs[0].value.u32 = 0;
                }
                
                return (sai_status_t)SAI_STATUS_SUCCESS;
            }
        );

        auto exec = static_cast<Notifier *>(gPortsOrch->getExecutor("PORT_STATUS_NOTIFICATIONS"));
        auto consumer = exec->getNotificationConsumer();
        
        // mock a redis reply for notification, it notifies that Ehernet0 is going to up
        mockReply = (redisReply *)calloc(sizeof(redisReply), 1);
        mockReply->type = REDIS_REPLY_ARRAY;
        mockReply->elements = 3; // REDIS_PUBLISH_MESSAGE_ELEMNTS
        mockReply->element = (redisReply **)calloc(sizeof(redisReply *), mockReply->elements);
        mockReply->element[2] = (redisReply *)calloc(sizeof(redisReply), 1);
        mockReply->element[2]->type = REDIS_REPLY_STRING;
        sai_port_oper_status_notification_t port_oper_status;
        port_oper_status.port_id = port.m_port_id;
        port_oper_status.port_state = SAI_PORT_OPER_STATUS_UP;
        std::string data = sai_serialize_port_oper_status_ntf(1, &port_oper_status);
        std::vector<FieldValueTuple> notifyValues;
        FieldValueTuple opdata("port_state_change", data);
        notifyValues.push_back(opdata);
        std::string msg = swss::JSon::buildJson(notifyValues);
        mockReply->element[2]->str = (char*)calloc(1, msg.length() + 1);
        memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());

        // trigger the notification
        consumer->readData();
        gPortsOrch->doTask(*consumer);
        mockReply = nullptr; 

        gPortsOrch->getPort("Ethernet0", port);
        ASSERT_TRUE(port.m_oper_status == SAI_PORT_OPER_STATUS_UP);

        std::vector<FieldValueTuple> values;
        portTable.get("Ethernet0", values);
        for (auto &valueTuple : values)
        {
            if (fvField(valueTuple) == "speed")
            {
                ASSERT_TRUE(fvValue(valueTuple) != "0");
            }
        }

        gPortsOrch->refreshPortStatus();
        for (const auto &it : ports)
        {
            gPortsOrch->getPort(it.first, port);
            ASSERT_TRUE(port.m_oper_status == SAI_PORT_OPER_STATUS_UP);

            std::vector<FieldValueTuple> values;
            portTable.get(it.first, values);
            for (auto &valueTuple : values)
            {
                if (fvField(valueTuple) == "speed")
                {
                    ASSERT_TRUE(fvValue(valueTuple) != "0");
                }
            }
        }

        sai_port_api = orig_port_api;
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

        // check LAG, VLAN tasks were processed
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
