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

    sai_queue_api_t ut_sai_queue_api;
    sai_queue_api_t *pold_sai_queue_api;
    sai_buffer_api_t ut_sai_buffer_api;
    sai_buffer_api_t *pold_sai_buffer_api;

    string _ut_stub_queue_key;
    sai_status_t _ut_stub_sai_get_queue_attribute(
        _In_ sai_object_id_t queue_id,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
    {
        if (attr_count == 1 && attr_list[0].id == SAI_QUEUE_ATTR_BUFFER_PROFILE_ID)
        {
            auto &typemapQueue = (*gBufferOrch->m_buffer_type_maps[APP_BUFFER_QUEUE_TABLE_NAME]);
            auto &profileName = typemapQueue["Ethernet0:3-4"].m_objsReferencingByMe["profile"];
            auto profileNameVec = tokenize(profileName, ':');
            auto &typemapProfile = (*gBufferOrch->m_buffer_type_maps[APP_BUFFER_PROFILE_TABLE_NAME]);
            attr_list[0].value.oid = typemapProfile[profileNameVec[1]].m_saiObjectId;
            return SAI_STATUS_SUCCESS;
        }
        else
        {
            return pold_sai_queue_api->get_queue_attribute(queue_id, attr_count, attr_list);
        }
    }

    sai_status_t _ut_stub_sai_get_ingress_priority_group_attribute(
        _In_ sai_object_id_t ingress_priority_group_id,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
    {
        if (attr_count == 1 && attr_list[0].id == SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE)
        {
            auto &typemapPg = (*gBufferOrch->m_buffer_type_maps[APP_BUFFER_PG_TABLE_NAME]);
            auto &profileName = typemapPg["Ethernet0:3-4"].m_objsReferencingByMe["profile"];
            auto profileNameVec = tokenize(profileName, ':');
            auto &typemapProfile = (*gBufferOrch->m_buffer_type_maps[APP_BUFFER_PROFILE_TABLE_NAME]);
            attr_list[0].value.oid = typemapProfile[profileNameVec[1]].m_saiObjectId;
            return SAI_STATUS_SUCCESS;
        }
        else
        {
            return pold_sai_buffer_api->get_ingress_priority_group_attribute(ingress_priority_group_id, attr_count, attr_list);
        }
    }

    int _sai_create_buffer_pool_count = 0;
    sai_status_t _ut_stub_sai_create_buffer_pool(
        _Out_ sai_object_id_t *buffer_pool_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        auto status = pold_sai_buffer_api->create_buffer_pool(buffer_pool_id, switch_id, attr_count, attr_list);
        if (SAI_STATUS_SUCCESS == status)
            _sai_create_buffer_pool_count++;
        return status;
    }

    int _sai_remove_buffer_pool_count = 0;
    sai_status_t _ut_stub_sai_remove_buffer_pool(
        _In_ sai_object_id_t buffer_pool_id)
    {
        auto status = pold_sai_buffer_api->remove_buffer_pool(buffer_pool_id);
        if (SAI_STATUS_SUCCESS == status)
            _sai_remove_buffer_pool_count++;
        return status;
    }

    void _hook_sai_buffer_and_queue_api()
    {
        ut_sai_buffer_api = *sai_buffer_api;
        pold_sai_buffer_api = sai_buffer_api;
        ut_sai_buffer_api.create_buffer_pool = _ut_stub_sai_create_buffer_pool;
        ut_sai_buffer_api.remove_buffer_pool = _ut_stub_sai_remove_buffer_pool;
        ut_sai_buffer_api.get_ingress_priority_group_attribute = _ut_stub_sai_get_ingress_priority_group_attribute;
        sai_buffer_api = &ut_sai_buffer_api;

        ut_sai_queue_api = *sai_queue_api;
        pold_sai_queue_api = sai_queue_api;
        ut_sai_queue_api.get_queue_attribute = _ut_stub_sai_get_queue_attribute;
        sai_queue_api = &ut_sai_queue_api;
    }

    void _unhook_sai_buffer_and_queue_api()
    {
        sai_buffer_api = pold_sai_buffer_api;
        sai_queue_api = pold_sai_queue_api;
    }

    void clear_pfcwd_zero_buffer_handler()
    {
        auto &zeroProfile = PfcWdZeroBufferHandler::ZeroBufferProfile::getInstance();        
        zeroProfile.m_zeroIngressBufferPool = SAI_NULL_OBJECT_ID;
        zeroProfile.m_zeroEgressBufferPool = SAI_NULL_OBJECT_ID;
        zeroProfile.m_zeroIngressBufferProfile = SAI_NULL_OBJECT_ID;
        zeroProfile.m_zeroEgressBufferProfile = SAI_NULL_OBJECT_ID;
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

            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());

            vector<string> flex_counter_tables = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

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

    TEST_F(PortsOrchTest, PfcZeroBufferHandlerLocksPortPgAndQueue)
    {
        _hook_sai_buffer_and_queue_api();
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

        // Create test buffer pool
        poolTable.set(
            "ingress_pool",
            {
                { "type", "ingress" },
                { "mode", "dynamic" },
                { "size", "4200000" },
            });
        poolTable.set(
            "egress_pool",
            {
                { "type", "egress" },
                { "mode", "dynamic" },
                { "size", "4200000" },
            });

        // Create test buffer profile
        profileTable.set("test_profile", { { "pool", "ingress_pool" },
                                           { "xon", "14832" },
                                           { "xoff", "14832" },
                                           { "size", "35000" },
                                           { "dynamic_th", "0" } });
        profileTable.set("ingress_profile", { { "pool", "ingress_pool" },
                                              { "xon", "14832" },
                                              { "xoff", "14832" },
                                              { "size", "35000" },
                                              { "dynamic_th", "0" } });
        profileTable.set("egress_profile", { { "pool", "egress_pool" },
                                             { "size", "0" },
                                             { "dynamic_th", "0" } });

        // Apply profile on PGs 3-4 all ports
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

        // process pool, profile and PGs
        static_cast<Orch *>(gBufferOrch)->doTask();

        auto countersTable = make_shared<Table>(m_counters_db.get(), COUNTERS_TABLE);
        auto current_create_buffer_pool_count = _sai_create_buffer_pool_count;
        auto dropHandler = make_unique<PfcWdZeroBufferHandler>(port.m_port_id, port.m_queue_ids[3], 3, countersTable);

        current_create_buffer_pool_count += 2;
        ASSERT_TRUE(current_create_buffer_pool_count == _sai_create_buffer_pool_count);
        ASSERT_TRUE(PfcWdZeroBufferHandler::ZeroBufferProfile::getInstance().getPool(true) == gBufferOrch->m_ingressZeroBufferPool);
        ASSERT_TRUE(PfcWdZeroBufferHandler::ZeroBufferProfile::getInstance().getPool(false) == gBufferOrch->m_egressZeroBufferPool);
        ASSERT_TRUE(gBufferOrch->m_ingressZeroPoolRefCount == 1);
        ASSERT_TRUE(gBufferOrch->m_egressZeroPoolRefCount == 1);

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0:3-4", "SET", {{ "profile", "test_profile"}}});
        auto pgConsumer = static_cast<Consumer*>(gBufferOrch->getExecutor(APP_BUFFER_PG_TABLE_NAME));
        pgConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();

        // Port should have been updated by BufferOrch->doTask
        gPortsOrch->getPort("Ethernet0", port);
        auto profile_id = (*BufferOrch::m_buffer_type_maps["BUFFER_PROFILE_TABLE"])[string("test_profile")].m_saiObjectId;
        ASSERT_TRUE(profile_id != SAI_NULL_OBJECT_ID);
        ASSERT_TRUE(port.m_priority_group_pending_profile[3] == profile_id);
        ASSERT_TRUE(port.m_priority_group_pending_profile[4] == SAI_NULL_OBJECT_ID);

        pgConsumer = static_cast<Consumer*>(gBufferOrch->getExecutor(APP_BUFFER_PG_TABLE_NAME));
        pgConsumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty()); // PG is stored in m_priority_group_pending_profile
        ts.clear();

        // Create a zero buffer pool after PFC storm
        entries.push_back({"ingress_zero_pool", "SET", {{ "type", "ingress" },
                                                        { "mode", "static" },
                                                        { "size", "0" }}});
        auto poolConsumer = static_cast<Consumer*>(gBufferOrch->getExecutor(APP_BUFFER_POOL_TABLE_NAME));
        poolConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        // Reference increased
        ASSERT_TRUE(gBufferOrch->m_ingressZeroPoolRefCount == 2);
        // Didn't create buffer pool again
        ASSERT_TRUE(_sai_create_buffer_pool_count == current_create_buffer_pool_count);

        entries.push_back({"ingress_zero_pool", "DEL", {}});
        poolConsumer->addToSync(entries);
        entries.clear();
        auto current_remove_buffer_pool_count = _sai_remove_buffer_pool_count;
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_TRUE(gBufferOrch->m_ingressZeroPoolRefCount == 1);
        ASSERT_TRUE(_sai_remove_buffer_pool_count == current_remove_buffer_pool_count);

        // release zero buffer drop handler
        dropHandler.reset();

        // re-fetch the port
        gPortsOrch->getPort("Ethernet0", port);

        // pending profile should be cleared
        ASSERT_TRUE(port.m_priority_group_pending_profile[3] == SAI_NULL_OBJECT_ID);
        ASSERT_TRUE(port.m_priority_group_pending_profile[4] == SAI_NULL_OBJECT_ID);

        // process PGs
        static_cast<Orch *>(gBufferOrch)->doTask();

        pgConsumer = static_cast<Consumer*>(gBufferOrch->getExecutor(APP_BUFFER_PG_TABLE_NAME));
        pgConsumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty()); // PG should be processed now
        ts.clear();
        clear_pfcwd_zero_buffer_handler();
        _unhook_sai_buffer_and_queue_api();
    }

    TEST_F(PortsOrchTest, PfcZeroBufferHandlerLocksPortWithZeroPoolCreated)
    {
        _hook_sai_buffer_and_queue_api();
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

        // Create test buffer pool
        poolTable.set("ingress_pool",
                      {
                          { "type", "ingress" },
                          { "mode", "dynamic" },
                          { "size", "4200000" },
                      });
        poolTable.set("egress_pool",
                      {
                          { "type", "egress" },
                          { "mode", "dynamic" },
                          { "size", "4200000" },
                      });
        poolTable.set("ingress_zero_pool",
                      {
                          { "type", "ingress" },
                          { "mode", "static" },
                          { "size", "0" }
                      });
        auto poolConsumer = static_cast<Consumer*>(gBufferOrch->getExecutor(APP_BUFFER_POOL_TABLE_NAME));

        // Create test buffer profile
        profileTable.set("ingress_profile", { { "pool", "ingress_pool" },
                                              { "xon", "14832" },
                                              { "xoff", "14832" },
                                              { "size", "35000" },
                                              { "dynamic_th", "0" } });
        profileTable.set("egress_profile", { { "pool", "egress_pool" },
                                             { "size", "0" },
                                             { "dynamic_th", "0" } });

        // Apply profile on PGs 3-4 all ports
        for (const auto &it : ports)
        {
            std::ostringstream oss;
            oss << it.first << ":3-4";
            pgTable.set(oss.str(), { { "profile", "ingress_profile" } });
            queueTable.set(oss.str(), { {"profile", "egress_profile" } });
        }

        gBufferOrch->addExistingData(&poolTable);
        gBufferOrch->addExistingData(&profileTable);
        gBufferOrch->addExistingData(&pgTable);
        gBufferOrch->addExistingData(&queueTable);

        auto current_create_buffer_pool_count = _sai_create_buffer_pool_count + 3; // call SAI API create_buffer_pool for each pool
        ASSERT_TRUE(gBufferOrch->m_ingressZeroPoolRefCount == 0);
        ASSERT_TRUE(gBufferOrch->m_egressZeroPoolRefCount == 0);
        ASSERT_TRUE(gBufferOrch->m_ingressZeroBufferPool == SAI_NULL_OBJECT_ID);
        ASSERT_TRUE(gBufferOrch->m_egressZeroBufferPool == SAI_NULL_OBJECT_ID);

        // process pool, profile and PGs
        static_cast<Orch *>(gBufferOrch)->doTask();

        ASSERT_TRUE(current_create_buffer_pool_count == _sai_create_buffer_pool_count);
        ASSERT_TRUE(gBufferOrch->m_ingressZeroPoolRefCount == 1);
        ASSERT_TRUE(gBufferOrch->m_egressZeroPoolRefCount == 0);
        ASSERT_TRUE(gBufferOrch->m_ingressZeroBufferPool != SAI_NULL_OBJECT_ID);
        ASSERT_TRUE(gBufferOrch->m_egressZeroBufferPool == SAI_NULL_OBJECT_ID);

        auto countersTable = make_shared<Table>(m_counters_db.get(), COUNTERS_TABLE);
        auto dropHandler = make_unique<PfcWdZeroBufferHandler>(port.m_port_id, port.m_queue_ids[3], 3, countersTable);

        current_create_buffer_pool_count++; // Increased for egress zero pool
        ASSERT_TRUE(current_create_buffer_pool_count == _sai_create_buffer_pool_count);
        ASSERT_TRUE(PfcWdZeroBufferHandler::ZeroBufferProfile::getInstance().getPool(true) == gBufferOrch->m_ingressZeroBufferPool);
        ASSERT_TRUE(PfcWdZeroBufferHandler::ZeroBufferProfile::getInstance().getPool(false) == gBufferOrch->m_egressZeroBufferPool);
        ASSERT_TRUE(gBufferOrch->m_ingressZeroPoolRefCount == 2);
        ASSERT_TRUE(gBufferOrch->m_egressZeroPoolRefCount == 1);

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"ingress_zero_pool", "DEL", {}});
        poolConsumer->addToSync(entries);
        entries.clear();
        auto current_remove_buffer_pool_count = _sai_remove_buffer_pool_count;
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_TRUE(gBufferOrch->m_ingressZeroPoolRefCount == 1);
        ASSERT_TRUE(_sai_remove_buffer_pool_count == current_remove_buffer_pool_count);

        // release zero buffer drop handler
        dropHandler.reset();
        clear_pfcwd_zero_buffer_handler();
        _unhook_sai_buffer_and_queue_api();
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
