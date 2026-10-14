// NOTE: Using #define private public is a known SONiC test pattern to access internal members
// for testing purposes. However, this is technically undefined behavior in C++ and should be
// used cautiously. Consider using friend declarations or proper test fixtures in new code.
#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "bulker.h"
#include "logger.h"

#include "evpnmhorch.h"

namespace evpnmhorch_test
{
    using namespace std;

    static std::map<std::string, std::vector<swss::FieldValueTuple>> defaultPortList;
    static const string TEST_INTERFACE_1 = "Ethernet1";
    static const string TEST_INTERFACE_2 = "Ethernet2";
    static const string VLAN_NAME_1 = "Vlan10";
    static const string VLAN_NAME_2 = "Vlan20";

    struct EvpnMhOrchTest : public ::testing::Test
    {
        int iter = 0;
        std::vector<Orch **> ut_orch_list;
        shared_ptr<swss::DBConnector> m_appl_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;

        FlexCounterOrch *m_FlexCounterOrch = nullptr;
        sai_vlan_api_t ut_sai_vlan_api;
        sai_vlan_api_t *org_sai_vlan_api = nullptr;

        void _hook_sai_vlan_api()
        {
            ut_sai_vlan_api = *sai_vlan_api;
            org_sai_vlan_api = sai_vlan_api;
            sai_vlan_api = &ut_sai_vlan_api;
        }

        void _unhook_sai_vlan_api()
        {
            sai_vlan_api = org_sai_vlan_api;
        }

        EvpnMhOrchTest()
        {
        }

       void ApplyDualTorConfigs()
        {
            Table port_table = Table(m_appl_db.get(), APP_PORT_TABLE_NAME);
            Table vlan_table = Table(m_appl_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table = Table(m_appl_db.get(), APP_VLAN_MEMBER_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(TEST_INTERFACE_1, ports[TEST_INTERFACE_1]);
            port_table.set(TEST_INTERFACE_2, ports[TEST_INTERFACE_2]);
            port_table.set("PortConfigDone", { { "count", to_string(1) } });
            port_table.set("PortInitDone", { {} });

            vlan_table.set(VLAN_NAME_1, { { "admin_status", "up" },
                                          { "mtu", "9100" },
                                          { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_table.set(VLAN_NAME_2, { { "admin_status", "up" },
                                          { "mtu", "9100" },
                                          { "mac", "00:aa:bb:cc:dd:ee" } });

            vlan_member_table.set(
                VLAN_NAME_1 + vlan_member_table.getTableNameSeparator() + TEST_INTERFACE_1,
                { { "tagging_mode", "untagged" } });

            vlan_member_table.set(
                VLAN_NAME_2 + vlan_member_table.getTableNameSeparator() + TEST_INTERFACE_1,
                { { "tagging_mode", "untagged" } });

            gPortsOrch->addExistingData(&port_table);
            gPortsOrch->addExistingData(&vlan_table);
            gPortsOrch->addExistingData(&vlan_member_table);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

       void PrepareSai()
        {
            // Init switch and create dependencies
            sai_attribute_t attr;

            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            sai_status_t status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
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

            // Get SAI default ports
            defaultPortList = ut_helper::getInitialSaiPorts();
            ASSERT_TRUE(!defaultPortList.empty());
        }

        void SetUp() override
        {
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);

            m_appl_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);

            PrepareSai();

            // Create dependencies ...
            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector app_switch_table(m_appl_db.get(), APP_SWITCH_TABLE_NAME);
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);

            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_appl_db.get(), switch_tables, stateDbSwitchTable);
            gDirectory.set(gSwitchOrch);
            ut_orch_list.push_back((Orch **)&gSwitchOrch);

            vector<string> flex_counter_tables = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };

            m_FlexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(m_FlexCounterOrch);
            ut_orch_list.push_back((Orch **)&m_FlexCounterOrch);

            const int portsorch_base_pri = 40;

            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_appl_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());
            gDirectory.set(gPortsOrch);
            ut_orch_list.push_back((Orch **)&gPortsOrch);

            // Create EvpnMhOrch early so its ES/DF state is available when PortsOrch
            // processes bridge ports and VLAN members (matches production code order)
            TableConnector appDbDfTable(m_appl_db.get(), "EVPN_DF_TABLE");
            TableConnector confDbEvpnEsTable(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");

            vector<TableConnector> evpn_df_es_table_connectors = {
                appDbDfTable,
                confDbEvpnEsTable,
            };

            ASSERT_EQ(gEvpnMhOrch, nullptr);
            gEvpnMhOrch = new EvpnMhOrch(evpn_df_es_table_connectors);
            gDirectory.set(gEvpnMhOrch);
            ut_orch_list.push_back((Orch **)&gEvpnMhOrch);

            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };

            ASSERT_EQ(gBufferOrch, nullptr);
            gBufferOrch = new BufferOrch(m_appl_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);
            gDirectory.set(gBufferOrch);
            ut_orch_list.push_back((Orch **)&gBufferOrch);
        }

        void TearDown() override
        {
            ::testing_db::reset();

            auto buffer_maps = BufferOrch::m_buffer_type_maps;
            for (auto &i : buffer_maps)
            {
                i.second->clear();
            }

            for (std::vector<Orch **>::reverse_iterator rit = ut_orch_list.rbegin(); rit != ut_orch_list.rend(); ++rit)
            {
                Orch **orch = *rit;
                delete *orch;
                *orch = nullptr;
            }

            gDirectory.m_values.clear();

            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gSwitchId = 0;

            ut_helper::uninitSaiApi();
        }

        void ApplyDualTorConfigsForSingleVlan()
        {
            Table port_table = Table(m_appl_db.get(), APP_PORT_TABLE_NAME);
            Table vlan_table = Table(m_appl_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table = Table(m_appl_db.get(), APP_VLAN_MEMBER_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(TEST_INTERFACE_1, ports[TEST_INTERFACE_1]);
            port_table.set(TEST_INTERFACE_2, ports[TEST_INTERFACE_2]);
            port_table.set("PortConfigDone", { { "count", to_string(1) } });
            port_table.set("PortInitDone", { {} });

            vlan_table.set(VLAN_NAME_1, { { "admin_status", "up" },
                                          { "mtu", "9100" },
                                          { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_member_table.set(
                VLAN_NAME_1 + vlan_member_table.getTableNameSeparator() + TEST_INTERFACE_1,
                { { "tagging_mode", "untagged" } });

            gPortsOrch->addExistingData(&port_table);
            gPortsOrch->addExistingData(&vlan_table);
            gPortsOrch->addExistingData(&vlan_member_table);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }
    };

    TEST_F(EvpnMhOrchTest, ESCacheDFRole)
    {
        ApplyDualTorConfigs();

        Table evpnEsIntfTable = Table(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");
        evpnEsIntfTable.set("Ethernet1", { { "df_pref", "32767" },
                                           { "esi", "AUTO" },
                                           { "type", "TYPE_3_MAC_BASED" } });
        evpnEsIntfTable.set("Ethernet2", { { "df_pref", "32767" },
                                           { "esi", "AUTO" },
                                           { "type", "TYPE_3_MAC_BASED" } });

        Table evpnDFTable = Table(m_appl_db.get(), "EVPN_DF_TABLE");
        evpnDFTable.set("Vlan10:Ethernet1", {
                                                { "df", "true" },
                                            });
        evpnDFTable.set("Vlan20:Ethernet1", {
                                                { "df", "false" },
                                            });

        gEvpnMhOrch->addExistingData(&evpnEsIntfTable);
        gEvpnMhOrch->addExistingData(&evpnDFTable);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_TRUE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet1"));
        ASSERT_TRUE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet1", 10));
        ASSERT_TRUE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));

        ASSERT_TRUE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet1", 20));
        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 20));

        ASSERT_FALSE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet3"));
        ASSERT_FALSE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet3", 10));

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ "Vlan20:Ethernet1", "DEL", { {} } });

        auto consumer = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_DF_TABLE"));
        consumer->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_TRUE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet1", 10));
        ASSERT_TRUE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet1"));
        ASSERT_TRUE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet2"));

        entries.clear();
        consumer = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_ETHERNET_SEGMENT"));
        entries.push_back({ "Ethernet2", "DEL", { {} } });
        consumer->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_FALSE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet2"));

        entries.clear();
        consumer = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_DF_TABLE"));
        entries.push_back({ "Vlan10:Ethernet1", "DEL", { {} } });
        consumer->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_TRUE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet1"));

        entries.clear();
        consumer = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_ETHERNET_SEGMENT"));
        entries.push_back({ "Ethernet1", "DEL", { {} } });
        consumer->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_FALSE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet1"));
        ASSERT_FALSE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet1", 10));
        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));
        ASSERT_FALSE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet1", 20));
        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 20));
        ASSERT_FALSE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet2"));
    }

    TEST_F(EvpnMhOrchTest, ESProgramDFAfterPort)
    {
        _hook_sai_vlan_api();

        auto consumer_df = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_DF_TABLE"));
        auto consumer_es = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_ETHERNET_SEGMENT"));
        std::vector<sai_attr_id_t> df_attr_received;
        std::vector<bool> df_attr_value;

        ApplyDualTorConfigs();
        Table evpnEsIntfTable = Table(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");
        evpnEsIntfTable.set("Ethernet1", {
            {"df_pref", "32767"},
            {"esi", "AUTO"},
            {"type", "TYPE_3_MAC_BASED"}
        });

        auto vlanSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN>(&sai_vlan_api->set_vlan_member_attribute);
        vlanSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t * attr) -> sai_status_t {
            df_attr_received.push_back(attr->id);
            df_attr_value.push_back(attr->value.booldata);

            return org_sai_vlan_api->set_vlan_member_attribute(oid, attr);
        });

        gEvpnMhOrch->addExistingData(&evpnEsIntfTable);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_TRUE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet1"));
        ASSERT_TRUE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet1", 10));
        ASSERT_TRUE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet1", 20));
        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));
        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 20));
        /* No DF entry yet: the ES member does not drop tunnel-terminated BUM. */
        ASSERT_EQ(df_attr_value.size(), 2u);
        ASSERT_EQ(df_attr_received[0], SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP);
        ASSERT_FALSE(df_attr_value[0]);
        ASSERT_EQ(df_attr_received[1], SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP);
        ASSERT_FALSE(df_attr_value[1]);

        df_attr_received.clear();
        df_attr_value.clear();
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Vlan10:Ethernet1", "SET", {{"df", "true"} }});
        entries.push_back({"Vlan20:Ethernet1", "SET", {{"df", "false"} }});
        consumer_df->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        /* The DF forwards tunnel-terminated BUM, the non-DF drops it. */
        ASSERT_TRUE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));
        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 20));
        ASSERT_EQ(df_attr_value.size(), 2u);
        ASSERT_EQ(df_attr_received[0], SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP);
        ASSERT_FALSE(df_attr_value[0]);
        ASSERT_EQ(df_attr_received[1], SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP);
        ASSERT_TRUE(df_attr_value[1]);

        /* Removing the non-DF entry of a port still in the ES clears the drop. */
        df_attr_value.clear();
        entries.clear();
        entries.push_back({"Vlan20:Ethernet1", "DEL", { {} }});
        consumer_df->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_EQ(df_attr_value.size(), 1u);
        ASSERT_FALSE(df_attr_value[0]);

        /* A new non-DF entry sets it again. */
        df_attr_value.clear();
        entries.clear();
        entries.push_back({"Vlan20:Ethernet1", "SET", {{"df", "false"} }});
        consumer_df->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_EQ(df_attr_value.size(), 1u);
        ASSERT_TRUE(df_attr_value[0]);

        /* Leaving the ES clears the drop on every VLAN, also where a non-DF entry is still cached. */
        df_attr_value.clear();
        entries.clear();
        entries.push_back({"Ethernet1", "DEL", { {} }});
        consumer_es->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_FALSE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet1"));
        ASSERT_EQ(df_attr_value.size(), 2u);
        ASSERT_FALSE(df_attr_value[0]);
        ASSERT_FALSE(df_attr_value[1]);

        /* The DF entry removed after the port left the ES keeps the drop cleared. */
        df_attr_value.clear();
        entries.clear();
        entries.push_back({"Vlan20:Ethernet1", "DEL", { {} }});
        consumer_df->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_EQ(df_attr_value.size(), 1u);
        ASSERT_FALSE(df_attr_value[0]);
        _unhook_sai_vlan_api();
    }

    TEST_F(EvpnMhOrchTest, ESProgramAndAddVlanMember)
    {
        _hook_sai_vlan_api();

        auto consumer_df = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_DF_TABLE"));
        sai_attr_id_t df_attr_received = 0;
        // bool df_attr_value[2];

        Table evpnEsIntfTable = Table(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");
        evpnEsIntfTable.set("Ethernet1", { { "df_pref", "32767" },
                                           { "esi", "AUTO" },
                                           { "type", "TYPE_3_MAC_BASED" } });
        gEvpnMhOrch->addExistingData(&evpnEsIntfTable);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        bool df_attr_found = false;
        bool df_attr_value = false;

        auto vlanSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN_MEMBER>(&sai_vlan_api->create_vlan_member);
        vlanSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t *attrs) -> sai_status_t {
            uint32_t i;

            for (i = 0; i < count && !df_attr_found; i++)
            {
                // TODO: Use the correct attribute once its available in SAI
                if (attrs[i].id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP)
                {
                    df_attr_found = true;
                    // TUNNEL_TERM_BUM_TX_DROP=true on the non-DF
                    df_attr_value = attrs[i].value.booldata;
                }
            }

            return org_sai_vlan_api->create_vlan_member(oid, swoid, count, attrs);
        });

        ApplyDualTorConfigsForSingleVlan();

        ASSERT_TRUE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet1"));
        ASSERT_TRUE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet1", 10));
        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));
        ASSERT_TRUE(df_attr_found);
        /* No DF entry yet: the ES member does not drop tunnel-terminated BUM. */
        ASSERT_FALSE(df_attr_value);

        auto vlanMemberAttrSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN>(&sai_vlan_api->set_vlan_member_attribute);
        vlanMemberAttrSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
            df_attr_received = attr->id;
            df_attr_value = attr->value.booldata;

            return org_sai_vlan_api->set_vlan_member_attribute(oid, attr);
        });

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.clear();
        entries.push_back({ "Vlan10:Ethernet1", "SET", { { "df", "true" } } });
        consumer_df->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_TRUE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));
        ASSERT_EQ(df_attr_received, SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP);
        ASSERT_FALSE(df_attr_value);

        df_attr_received = 0;
        entries.clear();
        entries.push_back({ "Vlan10:Ethernet1", "SET", { { "df", "false" } } });
        consumer_df->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));
        ASSERT_EQ(df_attr_received, SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP);
        ASSERT_TRUE(df_attr_value);

        _unhook_sai_vlan_api();
    }

    /* DF entries that arrive before their VLAN are applied when the VLAN member is created. */
    TEST_F(EvpnMhOrchTest, ESMemberCreatedAfterDfEntry)
    {
        _hook_sai_vlan_api();

        auto consumer_df = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_DF_TABLE"));
        std::map<sai_object_id_t, bool> create_value;
        std::map<sai_object_id_t, bool> set_value;

        auto createSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN_MEMBER>(&sai_vlan_api->create_vlan_member);
        createSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count,
                                const sai_attribute_t *attrs) -> sai_status_t {
            sai_object_id_t vlan_oid = SAI_NULL_OBJECT_ID;
            for (uint32_t i = 0; i < count; i++)
            {
                if (attrs[i].id == SAI_VLAN_MEMBER_ATTR_VLAN_ID)
                {
                    vlan_oid = attrs[i].value.oid;
                }
            }
            for (uint32_t i = 0; i < count; i++)
            {
                if (attrs[i].id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP)
                {
                    create_value[vlan_oid] = attrs[i].value.booldata;
                }
            }
            return org_sai_vlan_api->create_vlan_member(oid, swoid, count, attrs);
        });
        auto setSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN>(&sai_vlan_api->set_vlan_member_attribute);
        setSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
            if (attr->id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP)
            {
                set_value[oid] = attr->value.booldata;
            }
            return org_sai_vlan_api->set_vlan_member_attribute(oid, attr);
        });

        Table evpnEsIntfTable = Table(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");
        evpnEsIntfTable.set("Ethernet1", { { "df_pref", "32767" },
                                           { "esi", "AUTO" },
                                           { "type", "TYPE_3_MAC_BASED" } });
        gEvpnMhOrch->addExistingData(&evpnEsIntfTable);
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ "Vlan10:Ethernet1", "SET", { { "df", "false" } } });
        entries.push_back({ "Vlan20:Ethernet1", "SET", { { "df", "true" } } });
        consumer_df->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ApplyDualTorConfigs();

        Port vlan10, vlan20;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_NAME_1, vlan10));
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_NAME_2, vlan20));
        /* The cached roles reach the members at create: drop on the non-DF, not on the DF. */
        ASSERT_EQ(create_value.count(vlan10.m_vlan_info.vlan_oid), 1u);
        ASSERT_TRUE(create_value[vlan10.m_vlan_info.vlan_oid]);
        ASSERT_EQ(create_value.count(vlan20.m_vlan_info.vlan_oid), 1u);
        ASSERT_FALSE(create_value[vlan20.m_vlan_info.vlan_oid]);
        _unhook_sai_vlan_api();
    }

    /* A DF entry for a port outside any ES sets no drop; the ES config then applies the cached role. */
    TEST_F(EvpnMhOrchTest, ESConfiguredAfterDfEntry)
    {
        _hook_sai_vlan_api();

        auto consumer_df = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_DF_TABLE"));
        auto consumer_es = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_ETHERNET_SEGMENT"));
        std::map<sai_object_id_t, bool> set_value;

        ApplyDualTorConfigs();

        Port vlan10, vlan20;
        sai_object_id_t member10, member20;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_NAME_1, vlan10));
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_NAME_2, vlan20));
        ASSERT_TRUE(gPortsOrch->getVlanMember(TEST_INTERFACE_1, vlan10, member10));
        ASSERT_TRUE(gPortsOrch->getVlanMember(TEST_INTERFACE_1, vlan20, member20));

        auto setSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN>(&sai_vlan_api->set_vlan_member_attribute);
        setSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
            if (attr->id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP)
            {
                set_value[oid] = attr->value.booldata;
            }
            return org_sai_vlan_api->set_vlan_member_attribute(oid, attr);
        });

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ "Vlan10:Ethernet1", "SET", { { "df", "false" } } });
        entries.push_back({ "Vlan20:Ethernet1", "SET", { { "df", "true" } } });
        consumer_df->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_EQ(set_value.size(), 2u);
        ASSERT_FALSE(set_value[member10]);
        ASSERT_FALSE(set_value[member20]);

        set_value.clear();
        entries.clear();
        entries.push_back({ "Ethernet1", "SET", { { "df_pref", "32767" },
                                                  { "esi", "AUTO" },
                                                  { "type", "TYPE_3_MAC_BASED" } } });
        consumer_es->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_EQ(set_value.size(), 2u);
        ASSERT_TRUE(set_value[member10]);
        ASSERT_FALSE(set_value[member20]);

        _unhook_sai_vlan_api();
    }

    /*
     * A VLAN that the ES port joins after its DF election (in FRR: a VLAN mapped to a VNI
     * after the port joined the ES) gets no EVPN_DF_TABLE entry until zebra sends one. It
     * must not drop tunnel-terminated BUM meanwhile, while the port's other VLANs keep their
     * DF role, also when the ES is applied again.
     */
    TEST_F(EvpnMhOrchTest, ESVlanAddedLaterWithoutDfEntryDoesNotDrop)
    {
        _hook_sai_vlan_api();

        auto consumer_df = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_DF_TABLE"));
        auto consumer_es = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_ETHERNET_SEGMENT"));
        std::map<sai_object_id_t, bool> set_value;
        std::vector<bool> create_value;

        ApplyDualTorConfigsForSingleVlan();

        auto setSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN>(&sai_vlan_api->set_vlan_member_attribute);
        setSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
            if (attr->id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP)
            {
                set_value[oid] = attr->value.booldata;
            }
            return org_sai_vlan_api->set_vlan_member_attribute(oid, attr);
        });
        auto createSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN_MEMBER>(&sai_vlan_api->create_vlan_member);
        createSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count,
                                const sai_attribute_t *attrs) -> sai_status_t {
            for (uint32_t i = 0; i < count; i++)
            {
                if (attrs[i].id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP)
                {
                    create_value.push_back(attrs[i].value.booldata);
                }
            }
            return org_sai_vlan_api->create_vlan_member(oid, swoid, count, attrs);
        });

        Table evpnEsIntfTable = Table(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");
        evpnEsIntfTable.set("Ethernet1", { { "df_pref", "32767" },
                                           { "esi", "AUTO" },
                                           { "type", "TYPE_3_MAC_BASED" } });
        gEvpnMhOrch->addExistingData(&evpnEsIntfTable);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ "Vlan10:Ethernet1", "SET", { { "df", "false" } } });
        consumer_df->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        Port vlan10;
        sai_object_id_t member10;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_NAME_1, vlan10));
        ASSERT_TRUE(gPortsOrch->getVlanMember(TEST_INTERFACE_1, vlan10, member10));
        ASSERT_EQ(set_value.count(member10), 1u);
        ASSERT_TRUE(set_value[member10]);

        /* Vlan20 and its ES member are created later; no DF entry for Vlan20 arrives. */
        Table vlan_table = Table(m_appl_db.get(), APP_VLAN_TABLE_NAME);
        Table vlan_member_table = Table(m_appl_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
        vlan_table.set(VLAN_NAME_2, { { "admin_status", "up" },
                                      { "mtu", "9100" },
                                      { "mac", "00:aa:bb:cc:dd:ee" } });
        vlan_member_table.set(
            VLAN_NAME_2 + vlan_member_table.getTableNameSeparator() + TEST_INTERFACE_1,
            { { "tagging_mode", "tagged" } });
        gPortsOrch->addExistingData(&vlan_table);
        gPortsOrch->addExistingData(&vlan_member_table);
        static_cast<Orch *>(gPortsOrch)->doTask();

        Port vlan20;
        sai_object_id_t member20;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_NAME_2, vlan20));
        ASSERT_TRUE(gPortsOrch->getVlanMember(TEST_INTERFACE_1, vlan20, member20));
        ASSERT_EQ(create_value.size(), 1u);
        ASSERT_FALSE(create_value[0]);

        /* Applying the ES again (as after a restart or a df_pref change) keeps both values. */
        set_value.clear();
        entries.clear();
        entries.push_back({ "Ethernet1", "SET", { { "df_pref", "100" },
                                                  { "esi", "AUTO" },
                                                  { "type", "TYPE_3_MAC_BASED" } } });
        consumer_es->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_EQ(set_value.size(), 2u);
        ASSERT_TRUE(set_value[member10]);
        ASSERT_FALSE(set_value[member20]);

        /* zebra's non-DF entry for Vlan20 turns the drop on there. */
        set_value.clear();
        entries.clear();
        entries.push_back({ "Vlan20:Ethernet1", "SET", { { "df", "false" } } });
        consumer_df->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_EQ(set_value.size(), 1u);
        ASSERT_EQ(set_value.count(member20), 1u);
        ASSERT_TRUE(set_value[member20]);

        _unhook_sai_vlan_api();
    }
}
