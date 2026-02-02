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

        FlexCounterOrch *m_FlexCounterOrch;
        sai_vlan_api_t ut_sai_vlan_api;
        sai_vlan_api_t *org_sai_vlan_api;

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
        sai_attr_id_t df_attr_received[2];
        bool df_attr_value[2];

        ApplyDualTorConfigs();
        Table evpnEsIntfTable = Table(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");
        evpnEsIntfTable.set("Ethernet1", {
            {"df_pref", "32767"},
            {"esi", "AUTO"},
            {"type", "TYPE_3_MAC_BASED"}
        });

        df_attr_received[0] = 0;
        df_attr_received[1] = 0;
        df_attr_value[0] = false;
        df_attr_value[1] = false;
        iter = 0;
        auto vlanSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN>(&sai_vlan_api->set_vlan_member_attribute);
        vlanSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t * attr) -> sai_status_t {
            df_attr_received[iter] = attr->id;
            df_attr_value[iter] = attr->value.booldata;
            iter++;

            return org_sai_vlan_api->set_vlan_member_attribute(oid, attr);
        });

        gEvpnMhOrch->addExistingData(&evpnEsIntfTable);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_TRUE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet1"));
        ASSERT_TRUE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet1", 10));
        ASSERT_TRUE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet1", 20));
        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));
        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 20));
        ASSERT_EQ(df_attr_received[0], SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP);
        ASSERT_EQ(df_attr_value[0], gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));
        ASSERT_EQ(df_attr_received[1], SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP);
        ASSERT_EQ(df_attr_value[1], gEvpnMhOrch->isInterfaceDF("Ethernet1", 20));

        df_attr_received[0] = 0;
        df_attr_received[1] = 0;
        df_attr_value[0] = false;
        df_attr_value[1] = false;
        iter = 0;

        vlanSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t * attr) -> sai_status_t {
            df_attr_received[iter] = attr->id;
            df_attr_value[iter] = attr->value.booldata;
            iter++;

            return org_sai_vlan_api->set_vlan_member_attribute(oid, attr);
        });

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.clear();
        entries.push_back({"Vlan10:Ethernet1", "SET", {{"df", "true"} }});
        consumer_df->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        ASSERT_TRUE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));
        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 20));
        ASSERT_EQ(df_attr_received[0], SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP);
        ASSERT_EQ(df_attr_value[0], gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));
        _unhook_sai_vlan_api();
    }

    TEST_F(EvpnMhOrchTest, ESProgramAndAddVlanMember)
    {
        _hook_sai_vlan_api();

        auto consumer_df = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_DF_TABLE"));
        sai_attr_id_t df_attr_received;
        // bool df_attr_value[2];

        Table evpnEsIntfTable = Table(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");
        evpnEsIntfTable.set("Ethernet1", { { "df_pref", "32767" },
                                           { "esi", "AUTO" },
                                           { "type", "TYPE_3_MAC_BASED" } });
        gEvpnMhOrch->addExistingData(&evpnEsIntfTable);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        bool df_attr_found;
        bool df_attr_value;

        auto vlanSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN_MEMBER>(&sai_vlan_api->create_vlan_member);
        vlanSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t *attrs) -> sai_status_t {
            uint32_t i;

            for (i = 0; i < count && !df_attr_found; i++)
            {
                // TODO: Use the correct attribute once its available in SAI
                if (attrs[i].id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP)
                {
                    df_attr_found = true;
                    // This is inverted from the currently proposed NON_DF SAI attribute
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
        ASSERT_EQ(df_attr_value, gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));

        _unhook_sai_vlan_api();
    }
}
