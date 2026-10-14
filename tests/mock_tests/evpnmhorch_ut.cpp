// NOTE: Using #define private public is a known SONiC test pattern to access internal members
// for testing purposes. However, this is technically undefined behavior in C++ and should be
// used cautiously. Consider using friend declarations or proper test fixtures in new code.
#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#define private public
#include "portsorch.h"
#include "l2nhgorch.h"
#include "evpnmhorch.h"
#undef private

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "bulker.h"
#include "logger.h"

#include "saispy.h"

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

        sai_bridge_api_t ut_sai_bridge_api;
        sai_bridge_api_t *org_sai_bridge_api = nullptr;

        void _hook_sai_bridge_api()
        {
            ut_sai_bridge_api = *sai_bridge_api;
            org_sai_bridge_api = sai_bridge_api;
            sai_bridge_api = &ut_sai_bridge_api;
        }

        void _unhook_sai_bridge_api()
        {
            sai_bridge_api = org_sai_bridge_api;
        }

        /* Check in the SAI that the port has a bridge port and exactly one member in the VLAN. */
        void CheckBridgePortAndVlanMember(const string &port_alias, const string &vlan_alias)
        {
            Port port, vlan;
            ASSERT_TRUE(gPortsOrch->getPort(port_alias, port));
            ASSERT_TRUE(gPortsOrch->getPort(vlan_alias, vlan));
            ASSERT_NE(port.m_bridge_port_id, SAI_NULL_OBJECT_ID);
            ASSERT_EQ(vlan.m_members.count(port_alias), 1u);

            sai_attribute_t attr;
            attr.id = SAI_BRIDGE_PORT_ATTR_PORT_ID;
            ASSERT_EQ(sai_bridge_api->get_bridge_port_attribute(port.m_bridge_port_id, 1, &attr), SAI_STATUS_SUCCESS);
            ASSERT_EQ(attr.value.oid, port.m_port_id);

            vector<sai_object_id_t> members(16);
            attr.id = SAI_VLAN_ATTR_MEMBER_LIST;
            attr.value.objlist.count = (uint32_t)members.size();
            attr.value.objlist.list = members.data();
            ASSERT_EQ(sai_vlan_api->get_vlan_attribute(vlan.m_vlan_info.vlan_oid, 1, &attr), SAI_STATUS_SUCCESS);

            uint32_t found = 0;
            for (uint32_t i = 0; i < attr.value.objlist.count; i++)
            {
                sai_attribute_t member_attr;
                member_attr.id = SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID;
                ASSERT_EQ(sai_vlan_api->get_vlan_member_attribute(members[i], 1, &member_attr), SAI_STATUS_SUCCESS);
                if (member_attr.value.oid == port.m_bridge_port_id)
                {
                    found++;
                }
            }
            ASSERT_EQ(found, 1u);
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

            TableConnector appDbEsBackupNhgTable(m_appl_db.get(), "EVPN_ES_BACKUP_NHG_TABLE");

            vector<TableConnector> evpn_df_es_table_connectors = {
                appDbDfTable,
                confDbEvpnEsTable,
                appDbEsBackupNhgTable,
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

        bool df_attr_found = false;
        bool df_attr_value = false;
        bool df_attr_in_create = false;

        auto vlanSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN_MEMBER>(&sai_vlan_api->create_vlan_member);
        vlanSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t *attrs) -> sai_status_t {
            uint32_t i;

            for (i = 0; i < count && !df_attr_in_create; i++)
            {
                // TODO: Use the correct attribute once its available in SAI
                if (attrs[i].id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP)
                {
                    df_attr_in_create = true;
                }
            }

            return org_sai_vlan_api->create_vlan_member(oid, swoid, count, attrs);
        });

        auto vlanMemberAttrSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN_MEMBER>(&sai_vlan_api->set_vlan_member_attribute);
        vlanMemberAttrSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
            // TODO: Use the correct attribute once its available in SAI
            if (attr->id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP)
            {
                df_attr_found = true;
                df_attr_received = attr->id;
                // This is inverted from the currently proposed NON_DF SAI attribute
                df_attr_value = attr->value.booldata;
            }

            return org_sai_vlan_api->set_vlan_member_attribute(oid, attr);
        });

        ApplyDualTorConfigsForSingleVlan();

        ASSERT_TRUE(gEvpnMhOrch->isPortInterfaceAssociatedToEs("Ethernet1"));
        ASSERT_TRUE(gEvpnMhOrch->isPortAndVlanAssociatedToEs("Ethernet1", 10));
        ASSERT_FALSE(gEvpnMhOrch->isInterfaceDF("Ethernet1", 10));
        /* The DF role is applied after create so an unimplemented attribute cannot
         * fail the VLAN member itself. */
        ASSERT_FALSE(df_attr_in_create);
        ASSERT_TRUE(df_attr_found);
        ASSERT_FALSE(df_attr_value);

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

    TEST_F(EvpnMhOrchTest, ESDfAttrUnsupported)
    {
        /* A SAI that implements neither DF attribute still gets the ES port's bridge port and
         * VLAN member. The attributes are rejected in a create too, where they used to be. */
        _hook_sai_vlan_api();
        _hook_sai_bridge_api();

        Table evpnEsIntfTable = Table(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");
        evpnEsIntfTable.set(TEST_INTERFACE_1, { { "df_pref", "32767" },
                                                { "esi", "AUTO" },
                                                { "type", "TYPE_3_MAC_BASED" } });
        gEvpnMhOrch->addExistingData(&evpnEsIntfTable);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        uint32_t bp_df_sets = 0;
        uint32_t vm_df_sets = 0;

        auto bpCreateSpy = SpyOn<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT>(&sai_bridge_api->create_bridge_port);
        bpCreateSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t *attrs) -> sai_status_t {
            for (uint32_t i = 0; i < count; i++)
            {
                if (attrs[i].id == SAI_BRIDGE_PORT_ATTR_EGRESS_FILTERING)
                {
                    /* SAI_STATUS_ATTR_NOT_IMPLEMENTED_0 offset by the attribute index */
                    return static_cast<sai_status_t>(SAI_STATUS_CODE(0x00030000L + i));
                }
            }
            return org_sai_bridge_api->create_bridge_port(oid, swoid, count, attrs);
        });
        auto bpSetSpy = SpyOn<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT>(&sai_bridge_api->set_bridge_port_attribute);
        bpSetSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
            if (attr->id == SAI_BRIDGE_PORT_ATTR_EGRESS_FILTERING)
            {
                bp_df_sets++;
                return SAI_STATUS_ATTR_NOT_IMPLEMENTED_0;
            }
            return org_sai_bridge_api->set_bridge_port_attribute(oid, attr);
        });
        auto vmCreateSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN_MEMBER>(&sai_vlan_api->create_vlan_member);
        vmCreateSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t *attrs) -> sai_status_t {
            for (uint32_t i = 0; i < count; i++)
            {
                if (attrs[i].id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP)
                {
                    return static_cast<sai_status_t>(SAI_STATUS_CODE(0x00030000L + i));
                }
            }
            return org_sai_vlan_api->create_vlan_member(oid, swoid, count, attrs);
        });
        auto vmSetSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN_MEMBER>(&sai_vlan_api->set_vlan_member_attribute);
        vmSetSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
            if (attr->id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP)
            {
                vm_df_sets++;
                return SAI_STATUS_NOT_SUPPORTED;
            }
            return org_sai_vlan_api->set_vlan_member_attribute(oid, attr);
        });

        ApplyDualTorConfigsForSingleVlan();

        ASSERT_NO_FATAL_FAILURE(CheckBridgePortAndVlanMember(TEST_INTERFACE_1, VLAN_NAME_1));
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_VLAN_MEMBER_TABLE_NAME));
        ASSERT_TRUE(consumer->m_toSync.empty());
        /* Both DF roles were requested by a set, and rejected. */
        ASSERT_EQ(bp_df_sets, 1u);
        ASSERT_EQ(vm_df_sets, 1u);

        _unhook_sai_bridge_api();
        _unhook_sai_vlan_api();
    }

    TEST_F(EvpnMhOrchTest, ESDfAttrSetRetry)
    {
        /* A retryable status from a DF set removes the object just created, so the next pass
         * creates it again rather than a second one. */
        _hook_sai_vlan_api();
        _hook_sai_bridge_api();

        Table evpnEsIntfTable = Table(m_config_db.get(), "EVPN_ETHERNET_SEGMENT");
        evpnEsIntfTable.set(TEST_INTERFACE_1, { { "df_pref", "32767" },
                                                { "esi", "AUTO" },
                                                { "type", "TYPE_3_MAC_BASED" } });
        gEvpnMhOrch->addExistingData(&evpnEsIntfTable);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();

        uint32_t bp_creates = 0, bp_removes = 0, bp_df_sets = 0;
        uint32_t vm_creates = 0, vm_removes = 0, vm_df_sets = 0;

        auto bpCreateSpy = SpyOn<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT>(&sai_bridge_api->create_bridge_port);
        bpCreateSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t *attrs) -> sai_status_t {
            bp_creates++;
            return org_sai_bridge_api->create_bridge_port(oid, swoid, count, attrs);
        });
        auto bpRemoveSpy = SpyOn<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT>(&sai_bridge_api->remove_bridge_port);
        bpRemoveSpy->callFake([&](sai_object_id_t oid) -> sai_status_t {
            bp_removes++;
            return org_sai_bridge_api->remove_bridge_port(oid);
        });
        auto bpSetSpy = SpyOn<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT>(&sai_bridge_api->set_bridge_port_attribute);
        bpSetSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
            if (attr->id == SAI_BRIDGE_PORT_ATTR_EGRESS_FILTERING && bp_df_sets++ == 0)
            {
                return SAI_STATUS_INSUFFICIENT_RESOURCES;
            }
            return org_sai_bridge_api->set_bridge_port_attribute(oid, attr);
        });
        auto vmCreateSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN_MEMBER>(&sai_vlan_api->create_vlan_member);
        vmCreateSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t *attrs) -> sai_status_t {
            vm_creates++;
            return org_sai_vlan_api->create_vlan_member(oid, swoid, count, attrs);
        });
        auto vmRemoveSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN_MEMBER>(&sai_vlan_api->remove_vlan_member);
        vmRemoveSpy->callFake([&](sai_object_id_t oid) -> sai_status_t {
            vm_removes++;
            return org_sai_vlan_api->remove_vlan_member(oid);
        });
        auto vmSetSpy = SpyOn<SAI_API_VLAN, SAI_OBJECT_TYPE_VLAN_MEMBER>(&sai_vlan_api->set_vlan_member_attribute);
        vmSetSpy->callFake([&](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
            if (attr->id == SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP && vm_df_sets++ == 0)
            {
                return SAI_STATUS_TABLE_FULL;
            }
            return org_sai_vlan_api->set_vlan_member_attribute(oid, attr);
        });

        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_VLAN_MEMBER_TABLE_NAME));

        /* Pass 1: the bridge port DF set asks for a retry, so the bridge port is removed. */
        ApplyDualTorConfigsForSingleVlan();
        ASSERT_EQ(bp_creates, 1u);
        ASSERT_EQ(bp_removes, 1u);
        ASSERT_EQ(vm_creates, 0u);
        ASSERT_FALSE(consumer->m_toSync.empty());

        /* Pass 2: the bridge port is created again and kept; the VLAN member DF set asks
         * for a retry, so the member is removed. */
        static_cast<Orch *>(gPortsOrch)->doTask();
        ASSERT_EQ(bp_creates, 2u);
        ASSERT_EQ(bp_removes, 1u);
        ASSERT_EQ(vm_creates, 1u);
        ASSERT_EQ(vm_removes, 1u);
        ASSERT_FALSE(consumer->m_toSync.empty());

        /* Pass 3: the VLAN member is created again and kept. */
        static_cast<Orch *>(gPortsOrch)->doTask();
        ASSERT_EQ(bp_creates, 2u);
        ASSERT_EQ(vm_creates, 2u);
        ASSERT_EQ(vm_removes, 1u);
        ASSERT_TRUE(consumer->m_toSync.empty());
        ASSERT_NO_FATAL_FAILURE(CheckBridgePortAndVlanMember(TEST_INTERFACE_1, VLAN_NAME_1));

        _unhook_sai_bridge_api();
        _unhook_sai_vlan_api();
    }

    /*
     * ES backup next hop group (EVPN_ES_BACKUP_NHG_TABLE): the group becomes the ES
     * port bridge port's protection group, and the port's oper state drives the
     * switchover.
     */
    static const string ES_BACKUP_NHG = "536870913";
    static const sai_object_id_t ES_BACKUP_NHG_OID = 0x5000000000abcULL;
    static const string ES_BACKUP_NHG_2 = "9200";
    static const sai_object_id_t ES_BACKUP_NHG_2_OID = 0x5000000000abdULL;

    static int es_flush_calls;
    static sai_fdb_api_t es_ut_fdb_api;
    static sai_fdb_api_t *es_org_fdb_api;

    static sai_status_t _ut_count_fdb_flush(sai_object_id_t, uint32_t, const sai_attribute_t *)
    {
        es_flush_calls++;
        return SAI_STATUS_SUCCESS;
    }

    struct EsBackupSetAttr
    {
        sai_attr_id_t id;
        bool value;
        sai_object_id_t oid;
    };

    /* A bridge port SAI call: a set of attribute id, or a remove (id 0). */
    struct EsBridgePortCall
    {
        bool remove;
        sai_object_id_t bridge_port_id;
        sai_attr_id_t id;
    };

    struct EvpnMhEsBackupTest : public EvpnMhOrchTest
    {
        std::vector<EsBackupSetAttr> set_calls;
        std::vector<EsBridgePortCall> bp_calls;
        /* A bridge port the test made up: its calls succeed without reaching the SAI. */
        sai_object_id_t fake_bp = SAI_NULL_OBJECT_ID;
        int fail_switchover = 0; /* SET_SWITCHOVER calls to refuse */
        std::shared_ptr<SaiSpyFunctor<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT, sai_status_t,
                                      sai_object_id_t, const sai_attribute_t *>> bpSpy;

        void SetUp() override
        {
            EvpnMhOrchTest::SetUp();

            vector<string> mlag_tables = { CFG_MCLAG_TABLE_NAME, CFG_MCLAG_INTF_TABLE_NAME };
            ASSERT_EQ(gMlagOrch, nullptr);
            gMlagOrch = new MlagOrch(m_config_db.get(), mlag_tables);
            ut_orch_list.push_back((Orch **)&gMlagOrch);

            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME, FdbOrch::fdborch_pri },
                { APP_VXLAN_FDB_TABLE_NAME, FdbOrch::fdborch_pri },
                { APP_MCLAG_FDB_TABLE_NAME, FdbOrch::fdborch_pri }
            };
            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            ASSERT_EQ(gFdbOrch, nullptr);
            gFdbOrch = new FdbOrch(m_appl_db.get(), app_fdb_tables, stateDbFdb, stateMclagDbFdb,
                                   gPortsOrch, m_config_db.get());
            ut_orch_list.push_back((Orch **)&gFdbOrch);

            ASSERT_EQ(gL2NhgOrch, nullptr);
            gL2NhgOrch = new L2NhgOrch(m_appl_db.get(), APP_L2_NEXTHOP_GROUP_TABLE_NAME);
            ut_orch_list.push_back((Orch **)&gL2NhgOrch);

            es_org_fdb_api = sai_fdb_api;
            es_ut_fdb_api = *sai_fdb_api;
            es_ut_fdb_api.flush_fdb_entries = _ut_count_fdb_flush;
            sai_fdb_api = &es_ut_fdb_api;
            es_flush_calls = 0;

            bpSpy = SpyOn<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT>(
                &sai_bridge_api->set_bridge_port_attribute);
            auto orig = bpSpy->original_fn;
            bpSpy->callFake([this, orig](sai_object_id_t oid, const sai_attribute_t *attr) -> sai_status_t {
                bp_calls.push_back({ false, oid, attr->id });
                if (oid == fake_bp && fake_bp != SAI_NULL_OBJECT_ID)
                {
                    return SAI_STATUS_SUCCESS;
                }
                if (attr->id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER && fail_switchover > 0)
                {
                    fail_switchover--;
                    return SAI_STATUS_FAILURE;
                }
                if (attr->id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID ||
                    attr->id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER)
                {
                    set_calls.push_back({ attr->id, attr->value.booldata, attr->value.oid });
                    return SAI_STATUS_SUCCESS;
                }
                return orig(oid, attr);
            });

            /* The capability query is not answered by the mock SAI. */
            gEvpnMhOrch->m_esBackupCapChecked = true;
            gEvpnMhOrch->m_esBackupSupported = true;

            ApplyDualTorConfigs();
            ASSERT_NE(gPortsOrch->m_portList[TEST_INTERFACE_1].m_bridge_port_id, SAI_NULL_OBJECT_ID);
            /* EVPN_ETHERNET_SEGMENT (CONFIG_DB) entry of the ES port. */
            gEvpnMhOrch->m_esIntfMap[TEST_INTERFACE_1] = true;
            setOper(SAI_PORT_OPER_STATUS_UP);
            es_flush_calls = 0;
        }

        void TearDown() override
        {
            unhookBridgePortRemoval();
            bpSpy.reset();
            sai_fdb_api = es_org_fdb_api;
            EvpnMhOrchTest::TearDown();
        }

        /* For PortsOrch::removeBridgePort(): StpOrch, host interfaces, and a bridge port
         * remove that refuse_remove makes the SAI refuse (without removing anything). */
        bool refuse_remove = false;
        std::shared_ptr<SaiSpyFunctor<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT, sai_status_t,
                                      sai_object_id_t>> removeBpSpy;
        std::shared_ptr<SaiSpyFunctor<SAI_API_HOSTIF, SAI_OBJECT_TYPE_HOSTIF, sai_status_t,
                                      sai_object_id_t, const sai_attribute_t *>> hostifSpy;

        void hookBridgePortRemoval()
        {
            removeBpSpy = SpyOn<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT>(&sai_bridge_api->remove_bridge_port);
            removeBpSpy->callFake([this](sai_object_id_t oid) -> sai_status_t {
                bp_calls.push_back({ true, oid, 0 });
                return refuse_remove ? SAI_STATUS_OBJECT_IN_USE : SAI_STATUS_SUCCESS;
            });
            hostifSpy = SpyOn<SAI_API_HOSTIF, SAI_OBJECT_TYPE_HOSTIF>(&sai_hostif_api->set_hostif_attribute);
            hostifSpy->callFake([](sai_object_id_t, const sai_attribute_t *) -> sai_status_t {
                return SAI_STATUS_SUCCESS;
            });
            vector<string> stp_tables = { "STP_TABLE", "STP_VLAN_INSTANCE_TABLE", "STP_PORT_STATE_TABLE",
                                          "STP_FASTAGEING_FLUSH_TABLE", "STP_INST_PORT_FLUSH_TABLE" };
            gStpOrch = new StpOrch(m_appl_db.get(), m_state_db.get(), stp_tables);
        }

        void unhookBridgePortRemoval()
        {
            removeBpSpy.reset();
            hostifSpy.reset();
            delete gStpOrch;
            gStpOrch = nullptr;
        }

        /* The group's bridge port, as L2NhgOrch creates it, with a made-up bridge port id. */
        Port addGroupBridgePort()
        {
            fake_bp = 0x3a00000000abcULL;
            Port nhgPort(gL2NhgOrch->getNextHopGroupPortName(ES_BACKUP_NHG), Port::NEXTHOP_GROUP);
            nhgPort.m_nexthop_group_id = ES_BACKUP_NHG_OID;
            nhgPort.m_bridge_port_id = fake_bp;
            gPortsOrch->m_portList[nhgPort.m_alias] = nhgPort;
            return nhgPort;
        }

        void activateBackupNhg(const string &nhg = ES_BACKUP_NHG, sai_object_id_t oid = ES_BACKUP_NHG_OID)
        {
            gL2NhgOrch->m_nhg_nh[nhg].oid = oid;
            gL2NhgOrch->m_nhg_nh[nhg].is_active = true;
        }

        /* What L2NhgOrch::deleteL2NextHopGroup() does first: remove the group's bridge port. */
        void removeGroupBridgePort(const string &nhg = ES_BACKUP_NHG, sai_object_id_t oid = ES_BACKUP_NHG_OID)
        {
            Port nhgPort(gL2NhgOrch->getNextHopGroupPortName(nhg), Port::NEXTHOP_GROUP);
            nhgPort.m_nexthop_group_id = oid;
            PortUpdate update = { nhgPort, false };
            gEvpnMhOrch->update(SUBJECT_TYPE_BRIDGE_PORT_CHANGE, &update);
        }

        void pushBackup(const string &op, const string &port = TEST_INTERFACE_1,
                        const string &nhg = ES_BACKUP_NHG)
        {
            auto consumer = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_ES_BACKUP_NHG_TABLE"));
            ASSERT_NE(consumer, nullptr);
            std::deque<KeyOpFieldsValuesTuple> entries;
            if (op == "SET")
            {
                entries.push_back({ port, "SET", { { "nexthop_group", nhg } } });
            }
            else
            {
                entries.push_back({ port, "DEL", { {} } });
            }
            consumer->addToSync(entries);
            static_cast<Orch *>(gEvpnMhOrch)->doTask();
            ASSERT_TRUE(consumer->m_toSync.empty());
        }

        /* The switchback timer fires with every port's hold passed. */
        void expireSwitchbackHold()
        {
            for (auto &kv : gEvpnMhOrch->m_esBackupSwitchback)
            {
                kv.second = std::chrono::steady_clock::time_point::min();
            }
            gEvpnMhOrch->onEsBackupSwitchbackTimer();
        }

        /* What PortsOrch::updatePortOperStatus() does for the observers. */
        void setOper(sai_port_oper_status_t status)
        {
            gPortsOrch->m_portList[TEST_INTERFACE_1].m_oper_status = status;
            PortOperStateUpdate update = { gPortsOrch->m_portList[TEST_INTERFACE_1], status };
            gFdbOrch->update(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, &update);
            gEvpnMhOrch->update(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, &update);
        }
    };

    TEST_F(EvpnMhEsBackupTest, SwitchoverFollowsPortOperState)
    {
        activateBackupNhg();
        pushBackup("SET");

        ASSERT_EQ(set_calls.size(), 1u);
        ASSERT_EQ(set_calls[0].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID);
        ASSERT_EQ(set_calls[0].oid, ES_BACKUP_NHG_OID);
        ASSERT_TRUE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));

        /* Down: switch over, and keep the port's MACs (they forward through the group). */
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_EQ(set_calls[1].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER);
        ASSERT_TRUE(set_calls[1].value);
        ASSERT_EQ(es_flush_calls, 0);

        /* Up: switched back only when the hold timer fires. */
        setOper(SAI_PORT_OPER_STATUS_UP);
        ASSERT_EQ(set_calls.size(), 2u);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 3u);
        ASSERT_EQ(set_calls[2].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER);
        ASSERT_FALSE(set_calls[2].value);

        /* DEL takes the group off the bridge port. */
        pushBackup("DEL");
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_EQ(set_calls[3].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        ASSERT_FALSE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));

        /* Without a backup group a down port is flushed as before. */
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(es_flush_calls, 1);
        ASSERT_EQ(set_calls.size(), 4u);
    }

    TEST_F(EvpnMhEsBackupTest, FlapWithinTheHoldStaysSwitchedOver)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 2u);

        setOper(SAI_PORT_OPER_STATUS_UP);
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 2u);

        setOper(SAI_PORT_OPER_STATUS_UP);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 3u);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_EQ(es_flush_calls, 0);
    }

    /* The timer can fire for another port's hold: a port switches back only after its own. */
    TEST_F(EvpnMhEsBackupTest, SwitchbackWaitsForTheHoldOfItsOwnPort)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        setOper(SAI_PORT_OPER_STATUS_UP);
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_TRUE(gEvpnMhOrch->m_esBackupSwitchback.count(TEST_INTERFACE_1));

        gEvpnMhOrch->onEsBackupSwitchbackTimer();
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_TRUE(gEvpnMhOrch->m_esBackupSwitchback.count(TEST_INTERFACE_1));

        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 3u);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_TRUE(gEvpnMhOrch->m_esBackupSwitchback.empty());
    }

    TEST_F(EvpnMhEsBackupTest, WaitsForTheGroupAndSwitchesOverOnAttach)
    {
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        es_flush_calls = 0;

        pushBackup("SET");
        ASSERT_TRUE(set_calls.empty());
        ASSERT_FALSE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));

        /* L2NhgOrch activates the group later and tells EvpnMhOrch at once, in the
         * same pass (a warm restart's view must have the protection group). */
        activateBackupNhg();
        gEvpnMhOrch->onL2NhgActive(ES_BACKUP_NHG);
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_EQ(set_calls[0].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID);
        ASSERT_EQ(set_calls[1].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER);
        ASSERT_TRUE(set_calls[1].value);

        static_cast<Orch *>(gEvpnMhOrch)->doTask();
        ASSERT_EQ(set_calls.size(), 2u);
    }

    TEST_F(EvpnMhEsBackupTest, DetachedBeforeTheGroupBridgePortGoes)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_EQ(es_flush_calls, 0);

        /* L2NhgOrch removes the group's bridge port before the group. */
        removeGroupBridgePort();

        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_EQ(set_calls[2].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_EQ(set_calls[3].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        /* Nothing protects the down port any more: its MACs are flushed. */
        ASSERT_EQ(es_flush_calls, 1);

        gL2NhgOrch->m_nhg_nh.erase(ES_BACKUP_NHG);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();
        ASSERT_EQ(set_calls.size(), 4u);

        /* The group comes back (same id): attached and switched over again. */
        activateBackupNhg();
        static_cast<Orch *>(gEvpnMhOrch)->doTask();
        ASSERT_EQ(set_calls.size(), 6u);
        ASSERT_EQ(set_calls[4].oid, ES_BACKUP_NHG_OID);
        ASSERT_TRUE(set_calls[5].value);
    }

    /*
     * Through PortsOrch::removeBridgePort(), as L2NhgOrch removes the group: the ES port
     * is switched back and the group taken off it while the group's bridge port still
     * exists. A SAI may refuse to remove a group bridge port while a port is switched over to it.
     */
    TEST_F(EvpnMhEsBackupTest, DetachedWhileTheGroupBridgePortStillExists)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 2u);
        const sai_object_id_t es_bp = gPortsOrch->m_portList[TEST_INTERFACE_1].m_bridge_port_id;

        /* The group's bridge port, as L2NhgOrch creates it. */
        fake_bp = 0x3a00000000abcULL;
        Port nhgPort(gL2NhgOrch->getNextHopGroupPortName(ES_BACKUP_NHG), Port::NEXTHOP_GROUP);
        nhgPort.m_nexthop_group_id = ES_BACKUP_NHG_OID;
        nhgPort.m_bridge_port_id = fake_bp;
        gPortsOrch->m_portList[nhgPort.m_alias] = nhgPort;

        auto removeSpy = SpyOn<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT>(&sai_bridge_api->remove_bridge_port);
        auto origRemove = removeSpy->original_fn;
        removeSpy->callFake([this, origRemove](sai_object_id_t oid) -> sai_status_t {
            bp_calls.push_back({ true, oid, 0 });
            return oid == fake_bp ? SAI_STATUS_SUCCESS : origRemove(oid);
        });
        vector<string> stp_tables = { "STP_TABLE", "STP_VLAN_INSTANCE_TABLE", "STP_PORT_STATE_TABLE",
                                      "STP_FASTAGEING_FLUSH_TABLE", "STP_INST_PORT_FLUSH_TABLE" };
        ASSERT_EQ(gStpOrch, nullptr);
        gStpOrch = new StpOrch(m_appl_db.get(), m_state_db.get(), stp_tables);
        bp_calls.clear();

        bool removed = gPortsOrch->removeBridgePort(nhgPort);

        delete gStpOrch;
        gStpOrch = nullptr;
        removeSpy.reset();
        ASSERT_TRUE(removed);

        int switchback_at = -1, detach_at = -1, remove_at = -1;
        for (int i = 0; i < (int)bp_calls.size(); i++)
        {
            const auto &c = bp_calls[i];
            if (!c.remove && c.bridge_port_id == es_bp && c.id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER)
            {
                switchback_at = i;
            }
            if (!c.remove && c.bridge_port_id == es_bp &&
                c.id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID)
            {
                detach_at = i;
            }
            if (c.remove && c.bridge_port_id == fake_bp)
            {
                remove_at = i;
            }
        }
        ASSERT_GE(switchback_at, 0);
        ASSERT_GE(detach_at, 0);
        ASSERT_GE(remove_at, 0);
        ASSERT_LT(switchback_at, detach_at);
        ASSERT_LT(detach_at, remove_at);
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        ASSERT_FALSE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));
        ASSERT_EQ(gPortsOrch->m_portList[nhgPort.m_alias].m_bridge_port_id, SAI_NULL_OBJECT_ID);
    }

    /*
     * The SAI refuses to remove the ES port's bridge port: the group was taken off it
     * before the removal, and is attached and switched over again on the bridge port that stays.
     */
    TEST_F(EvpnMhEsBackupTest, RefusedPortBridgePortRemovalKeepsTheGroup)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 2u);

        hookBridgePortRemoval();
        refuse_remove = true;
        Port port = gPortsOrch->m_portList[TEST_INTERFACE_1];
        bool removed = gPortsOrch->removeBridgePort(port);
        unhookBridgePortRemoval();

        ASSERT_FALSE(removed);
        ASSERT_EQ(set_calls.size(), 6u);
        ASSERT_EQ(set_calls[2].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        ASSERT_EQ(set_calls[4].oid, ES_BACKUP_NHG_OID);
        ASSERT_EQ(set_calls[5].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER);
        ASSERT_TRUE(set_calls[5].value);
        ASSERT_TRUE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));
    }

    /* While L2NhgOrch retries a refused removal of the group, the group is not attached again. */
    TEST_F(EvpnMhEsBackupTest, GroupBeingRemovedIsNotAttachedAgain)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 2u);
        addGroupBridgePort();

        hookBridgePortRemoval();
        refuse_remove = true;
        bool deleted = gL2NhgOrch->deleteL2NextHopGroup(ES_BACKUP_NHG);
        unhookBridgePortRemoval();

        ASSERT_FALSE(deleted);
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);

        static_cast<Orch *>(gEvpnMhOrch)->doTask();
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_FALSE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));
    }

    TEST_F(EvpnMhEsBackupTest, IgnoredWhenTheSaiLacksProtection)
    {
        gEvpnMhOrch->m_esBackupSupported = false;
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);

        ASSERT_TRUE(set_calls.empty());
        ASSERT_FALSE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));
        ASSERT_EQ(es_flush_calls, 1);
    }

    /*
     * A DEL while the port is switched over and down is held: the group keeps carrying
     * the port's MACs until the port is back and the switchback hold has passed.
     */
    TEST_F(EvpnMhEsBackupTest, DeleteWhileSwitchedOverIsHeldUntilTheSwitchback)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 2u);

        pushBackup("DEL");
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_TRUE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));
        ASSERT_EQ(es_flush_calls, 0);
        /* A second oper down (FdbOrch's flush site) keeps the MACs too. */
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(es_flush_calls, 0);
        /* A retry or a second DEL does not end the hold. */
        gEvpnMhOrch->m_esBackupPending.insert(TEST_INTERFACE_1);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();
        pushBackup("DEL");
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_TRUE(gEvpnMhOrch->m_esBackup.at(TEST_INTERFACE_1).deleted);

        /* A flap within the hold: still held. */
        setOper(SAI_PORT_OPER_STATUS_UP);
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 2u);

        /* Back up: switched back after the hold, then detached; nothing is flushed. */
        setOper(SAI_PORT_OPER_STATUS_UP);
        ASSERT_EQ(set_calls.size(), 2u);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_EQ(set_calls[2].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_EQ(set_calls[3].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        ASSERT_EQ(es_flush_calls, 0);
        ASSERT_FALSE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));
        ASSERT_EQ(gEvpnMhOrch->m_esBackup.count(TEST_INTERFACE_1), 0u);

        /* Gone for good: a later down flushes as without a group. */
        static_cast<Orch *>(gEvpnMhOrch)->doTask();
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_EQ(es_flush_calls, 1);
    }

    TEST_F(EvpnMhEsBackupTest, DeleteDuringTheSwitchbackHoldWaitsForIt)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        setOper(SAI_PORT_OPER_STATUS_UP);
        ASSERT_EQ(set_calls.size(), 2u);

        pushBackup("DEL");
        ASSERT_EQ(set_calls.size(), 2u);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        ASSERT_EQ(es_flush_calls, 0);
        ASSERT_EQ(gEvpnMhOrch->m_esBackup.count(TEST_INTERFACE_1), 0u);
    }

    /* A DEL + SET (a group replace) while down moves the port to the new group in place. */
    TEST_F(EvpnMhEsBackupTest, SetAfterAHeldDeleteReplacesTheGroupInPlace)
    {
        activateBackupNhg();
        activateBackupNhg(ES_BACKUP_NHG_2, ES_BACKUP_NHG_2_OID);
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 2u);

        pushBackup("DEL");
        pushBackup("SET", TEST_INTERFACE_1, ES_BACKUP_NHG_2);
        ASSERT_EQ(set_calls.size(), 3u);
        ASSERT_EQ(set_calls[2].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID);
        ASSERT_EQ(set_calls[2].oid, ES_BACKUP_NHG_2_OID);
        ASSERT_EQ(es_flush_calls, 0);

        /* No longer held: the first group's removal does not touch the port. */
        removeGroupBridgePort(ES_BACKUP_NHG);
        ASSERT_EQ(set_calls.size(), 3u);

        /* Re-attaching the same group while held sends nothing. */
        pushBackup("DEL");
        pushBackup("SET", TEST_INTERFACE_1, ES_BACKUP_NHG_2);
        ASSERT_EQ(set_calls.size(), 3u);

        setOper(SAI_PORT_OPER_STATUS_UP);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_FALSE(set_calls[3].value);
        ASSERT_TRUE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));
    }

    TEST_F(EvpnMhEsBackupTest, HeldDeleteEndsWhenTheGroupBridgePortGoes)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        pushBackup("DEL");
        ASSERT_EQ(set_calls.size(), 2u);

        removeGroupBridgePort();
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_EQ(set_calls[2].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_EQ(set_calls[3].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        ASSERT_EQ(es_flush_calls, 1);
        ASSERT_EQ(gEvpnMhOrch->m_esBackup.count(TEST_INTERFACE_1), 0u);

        ASSERT_TRUE(gEvpnMhOrch->m_esBackupPending.empty());
    }

    TEST_F(EvpnMhEsBackupTest, HeldDeleteEndsWhenThePortBridgePortGoes)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        pushBackup("DEL");
        ASSERT_EQ(set_calls.size(), 2u);

        /* Taken off the bridge port before PortsOrch removes it. */
        PortUpdate removed = { gPortsOrch->m_portList[TEST_INTERFACE_1], false };
        gEvpnMhOrch->update(SUBJECT_TYPE_BRIDGE_PORT_CHANGE, &removed);
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        ASSERT_FALSE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));
        ASSERT_EQ(gEvpnMhOrch->m_esBackup.count(TEST_INTERFACE_1), 0u);

        ASSERT_EQ(gEvpnMhOrch->m_esBackupPending.count(TEST_INTERFACE_1), 0u);
    }

    /* The port leaves its ES (EVPN_ETHERNET_SEGMENT DEL): a held DEL completes. */
    TEST_F(EvpnMhEsBackupTest, HeldDeleteEndsWhenThePortLeavesItsEs)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        pushBackup("DEL");
        ASSERT_EQ(set_calls.size(), 2u);

        auto consumer = dynamic_cast<Consumer *>(gEvpnMhOrch->getExecutor("EVPN_ETHERNET_SEGMENT"));
        ASSERT_NE(consumer, nullptr);
        std::deque<KeyOpFieldsValuesTuple> entries = { { TEST_INTERFACE_1, "DEL", { {} } } };
        consumer->addToSync(entries);
        static_cast<Orch *>(gEvpnMhOrch)->doTask();
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        ASSERT_EQ(es_flush_calls, 1);
        ASSERT_EQ(gEvpnMhOrch->m_esBackup.count(TEST_INTERFACE_1), 0u);
    }

    /* A port that is not an ES any more is not held: detached and flushed at once. */
    TEST_F(EvpnMhEsBackupTest, DeleteOnAPortThatLeftItsEsIsNotHeld)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        gEvpnMhOrch->m_esIntfMap.erase(TEST_INTERFACE_1);

        pushBackup("DEL");
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_EQ(es_flush_calls, 1);
        ASSERT_EQ(gEvpnMhOrch->m_esBackup.count(TEST_INTERFACE_1), 0u);
    }

    /* A replacement group that is not active yet: the old one stays until it is. */
    TEST_F(EvpnMhEsBackupTest, SetOfAGroupNotActiveYetKeepsTheOldOneWhileSwitchedOver)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 2u);

        pushBackup("SET", TEST_INTERFACE_1, ES_BACKUP_NHG_2);
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_TRUE(gEvpnMhOrch->isEsBackupAttached(TEST_INTERFACE_1));
        ASSERT_EQ(es_flush_calls, 0);

        activateBackupNhg(ES_BACKUP_NHG_2, ES_BACKUP_NHG_2_OID);
        gEvpnMhOrch->onL2NhgActive(ES_BACKUP_NHG_2);
        ASSERT_EQ(set_calls.size(), 3u);
        ASSERT_EQ(set_calls[2].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID);
        ASSERT_EQ(set_calls[2].oid, ES_BACKUP_NHG_2_OID);
        ASSERT_EQ(es_flush_calls, 0);
    }

    /* The old group goes while the new one is not active: detached, as without a replacement. */
    TEST_F(EvpnMhEsBackupTest, OldGroupRemovedBeforeTheNewOneIsActive)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        pushBackup("SET", TEST_INTERFACE_1, ES_BACKUP_NHG_2);
        ASSERT_EQ(set_calls.size(), 2u);

        removeGroupBridgePort(ES_BACKUP_NHG, ES_BACKUP_NHG_OID);
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        ASSERT_EQ(es_flush_calls, 1);

        activateBackupNhg(ES_BACKUP_NHG_2, ES_BACKUP_NHG_2_OID);
        gEvpnMhOrch->onL2NhgActive(ES_BACKUP_NHG_2);
        ASSERT_EQ(set_calls.size(), 6u);
        ASSERT_EQ(set_calls[4].oid, ES_BACKUP_NHG_2_OID);
        ASSERT_TRUE(set_calls[5].value);
    }

    /* A refused switchback while held is retried, then the group is detached. */
    TEST_F(EvpnMhEsBackupTest, HeldDeleteRetriesARefusedSwitchback)
    {
        activateBackupNhg();
        pushBackup("SET");
        setOper(SAI_PORT_OPER_STATUS_DOWN);
        pushBackup("DEL");
        setOper(SAI_PORT_OPER_STATUS_UP);

        fail_switchover = 1;
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_TRUE(gEvpnMhOrch->m_esBackupPending.count(TEST_INTERFACE_1));

        static_cast<Orch *>(gEvpnMhOrch)->doTask();
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_FALSE(set_calls[2].value);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        ASSERT_EQ(es_flush_calls, 0);
        ASSERT_EQ(gEvpnMhOrch->m_esBackup.count(TEST_INTERFACE_1), 0u);
    }

    /* Not switched over (the port is up): a DEL detaches at once, as before. */
    TEST_F(EvpnMhEsBackupTest, DeleteWhileUpDetachesAtOnce)
    {
        activateBackupNhg();
        pushBackup("SET");
        pushBackup("DEL");
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_EQ(set_calls[1].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID);
        ASSERT_EQ(set_calls[1].oid, SAI_NULL_OBJECT_ID);
        ASSERT_EQ(gEvpnMhOrch->m_esBackup.count(TEST_INTERFACE_1), 0u);
        ASSERT_EQ(es_flush_calls, 0);
    }

    /*
     * An ES LAG (PortChannel1 = TEST_INTERFACE_1 + TEST_INTERFACE_2, both up) switches
     * over when its last member stops forwarding, before the LAG goes oper down.
     */
    static const string ES_LAG = "PortChannel1";
    static const sai_object_id_t ES_LAG_OID = 0x2000000000001ULL;
    static const sai_object_id_t ES_LAG_BRIDGE_PORT_OID = 0x3a000000000fffULL;

    struct EvpnMhEsBackupLagTest : public EvpnMhEsBackupTest
    {
        std::shared_ptr<SaiSpyFunctor<SAI_API_LAG, SAI_OBJECT_TYPE_LAG_MEMBER, sai_status_t,
                                      sai_object_id_t, const sai_attribute_t *>> lagMemberSpy;

        void SetUp() override
        {
            EvpnMhEsBackupTest::SetUp();

            Port lag(ES_LAG, Port::LAG);
            lag.m_lag_id = ES_LAG_OID;
            lag.m_bridge_port_id = ES_LAG_BRIDGE_PORT_OID;
            lag.m_members = { TEST_INTERFACE_1, TEST_INTERFACE_2 };
            lag.m_oper_status = SAI_PORT_OPER_STATUS_UP;
            gPortsOrch->m_portList[ES_LAG] = lag;
            gEvpnMhOrch->m_esIntfMap[ES_LAG] = true;
            sai_object_id_t member_oid = 0x1b000000000001ULL;
            for (const auto &alias : { TEST_INTERFACE_1, TEST_INTERFACE_2 })
            {
                auto &member = gPortsOrch->m_portList[alias];
                member.m_lag_id = ES_LAG_OID;
                member.m_lag_member_id = member_oid++;
                member.m_oper_status = SAI_PORT_OPER_STATUS_UP;
            }

            lagMemberSpy = SpyOn<SAI_API_LAG, SAI_OBJECT_TYPE_LAG_MEMBER>(&sai_lag_api->set_lag_member_attribute);
            lagMemberSpy->callFake([](sai_object_id_t, const sai_attribute_t *) -> sai_status_t {
                return SAI_STATUS_SUCCESS;
            });
        }

        void TearDown() override
        {
            lagMemberSpy.reset();
            gPortsOrch->m_portList.erase(ES_LAG);
            for (const auto &alias : { TEST_INTERFACE_1, TEST_INTERFACE_2 })
            {
                gPortsOrch->m_portList[alias].m_lag_id = SAI_NULL_OBJECT_ID;
                gPortsOrch->m_portList[alias].m_lag_member_id = SAI_NULL_OBJECT_ID;
            }
            EvpnMhEsBackupTest::TearDown();
        }

        /* PortsOrch programming a member's distribution (egress enable). */
        void setDistribution(const string &alias, bool enable)
        {
            Port member = gPortsOrch->m_portList[alias];
            ASSERT_TRUE(gPortsOrch->setDistributionOnLagMember(member, enable));
        }

        /* PortsOrch::updatePortOperStatus(): observers run before m_portList is updated. */
        void setPortOper(const string &alias, sai_port_oper_status_t status)
        {
            Port port = gPortsOrch->m_portList[alias];
            port.m_oper_status = status;
            PortOperStateUpdate update = { port, status };
            gEvpnMhOrch->update(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, &update);
            gPortsOrch->m_portList[alias] = port;
        }

        size_t switchoverCalls(bool value)
        {
            size_t n = 0;
            for (const auto &c : set_calls)
            {
                n += c.id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER && c.value == value;
            }
            return n;
        }
    };

    TEST_F(EvpnMhEsBackupLagTest, SwitchesOverWhenTheLastMemberStopsDistributing)
    {
        activateBackupNhg();
        pushBackup("SET", ES_LAG);
        ASSERT_EQ(set_calls.size(), 1u);
        ASSERT_EQ(set_calls[0].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID);

        /* The other member still forwards. */
        setDistribution(TEST_INTERFACE_1, false);
        ASSERT_EQ(set_calls.size(), 1u);

        /* The last one stops: switched over while the LAG is still oper up. */
        setDistribution(TEST_INTERFACE_2, false);
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_EQ(switchoverCalls(true), 1u);

        setPortOper(ES_LAG, SAI_PORT_OPER_STATUS_DOWN);
        setPortOper(TEST_INTERFACE_1, SAI_PORT_OPER_STATUS_DOWN);
        setPortOper(TEST_INTERFACE_2, SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 2u);

        /* Up again, but no member distributes yet: the hold does not start. */
        setPortOper(TEST_INTERFACE_1, SAI_PORT_OPER_STATUS_UP);
        setPortOper(ES_LAG, SAI_PORT_OPER_STATUS_UP);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 2u);

        setDistribution(TEST_INTERFACE_1, true);
        ASSERT_EQ(set_calls.size(), 2u);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 3u);
        ASSERT_EQ(switchoverCalls(false), 1u);
    }

    TEST_F(EvpnMhEsBackupLagTest, SwitchesOverWhenTheLastMemberGoesDown)
    {
        activateBackupNhg();
        pushBackup("SET", ES_LAG);
        ASSERT_EQ(set_calls.size(), 1u);

        setPortOper(TEST_INTERFACE_1, SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 1u);
        /* Link down, before teamd disables the member or the LAG goes down. */
        setPortOper(TEST_INTERFACE_2, SAI_PORT_OPER_STATUS_DOWN);
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_EQ(switchoverCalls(true), 1u);

        /* A flap that teamd never saw: back up, switched back after the hold. */
        setPortOper(TEST_INTERFACE_2, SAI_PORT_OPER_STATUS_UP);
        ASSERT_EQ(set_calls.size(), 2u);
        setPortOper(TEST_INTERFACE_2, SAI_PORT_OPER_STATUS_DOWN);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 2u);
        setPortOper(TEST_INTERFACE_2, SAI_PORT_OPER_STATUS_UP);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 3u);
        ASSERT_EQ(switchoverCalls(false), 1u);
    }

    TEST_F(EvpnMhEsBackupLagTest, MembersDisabledBeforeTheGroupIsAttached)
    {
        setDistribution(TEST_INTERFACE_1, false);
        setDistribution(TEST_INTERFACE_2, false);
        ASSERT_TRUE(set_calls.empty());

        activateBackupNhg();
        pushBackup("SET", ES_LAG);
        ASSERT_EQ(set_calls.size(), 2u);
        ASSERT_EQ(set_calls[0].id, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID);
        ASSERT_EQ(switchoverCalls(true), 1u);
    }

    TEST_F(EvpnMhEsBackupLagTest, RemovedMemberIsNoLongerTrackedAsDisabled)
    {
        activateBackupNhg();
        pushBackup("SET", ES_LAG);
        setDistribution(TEST_INTERFACE_1, false);

        /* Removed and re-added: PortsOrch programs its distribution again from scratch. */
        LagMemberUpdate removed = { gPortsOrch->m_portList[ES_LAG], gPortsOrch->m_portList[TEST_INTERFACE_1], false };
        gEvpnMhOrch->update(SUBJECT_TYPE_LAG_MEMBER_CHANGE, &removed);
        ASSERT_FALSE(gEvpnMhOrch->m_lagMembersNotDistributing.count(TEST_INTERFACE_1));

        setDistribution(TEST_INTERFACE_2, false);
        ASSERT_EQ(set_calls.size(), 1u);
    }

    TEST_F(EvpnMhEsBackupLagTest, MemberAddedDistributingSwitchesBack)
    {
        activateBackupNhg();
        pushBackup("SET", ES_LAG);
        setDistribution(TEST_INTERFACE_1, false);
        setDistribution(TEST_INTERFACE_2, false);
        ASSERT_EQ(switchoverCalls(true), 1u);

        /* TEST_INTERFACE_1 leaves the LAG: still nothing forwards. */
        Port lag = gPortsOrch->m_portList[ES_LAG];
        lag.m_members = { TEST_INTERFACE_2 };
        gPortsOrch->m_portList[ES_LAG] = lag;
        LagMemberUpdate removed = { lag, gPortsOrch->m_portList[TEST_INTERFACE_1], false };
        gEvpnMhOrch->update(SUBJECT_TYPE_LAG_MEMBER_CHANGE, &removed);
        expireSwitchbackHold();
        ASSERT_EQ(switchoverCalls(false), 0u);

        /* Re-added already distributing (created with distribution on): the LAG forwards again. */
        lag.m_members = { TEST_INTERFACE_1, TEST_INTERFACE_2 };
        gPortsOrch->m_portList[ES_LAG] = lag;
        LagMemberUpdate added = { lag, gPortsOrch->m_portList[TEST_INTERFACE_1], true };
        gEvpnMhOrch->update(SUBJECT_TYPE_LAG_MEMBER_CHANGE, &added);
        ASSERT_EQ(switchoverCalls(false), 0u);
        expireSwitchbackHold();
        ASSERT_EQ(switchoverCalls(false), 1u);
    }

    TEST_F(EvpnMhEsBackupLagTest, MemberCreatedNotDistributingDoesNotForward)
    {
        activateBackupNhg();
        pushBackup("SET", ES_LAG);
        setDistribution(TEST_INTERFACE_1, false);

        /* TEST_INTERFACE_2 is re-created not distributing: PortsOrch reports that just
         * before the member add. */
        Port lag = gPortsOrch->m_portList[ES_LAG];
        LagMemberDistributionUpdate distribution = { gPortsOrch->m_portList[TEST_INTERFACE_2], false };
        gEvpnMhOrch->update(SUBJECT_TYPE_LAG_MEMBER_DISTRIBUTION_CHANGE, &distribution);
        LagMemberUpdate added = { lag, gPortsOrch->m_portList[TEST_INTERFACE_2], true };
        gEvpnMhOrch->update(SUBJECT_TYPE_LAG_MEMBER_CHANGE, &added);

        ASSERT_TRUE(gEvpnMhOrch->m_lagMembersNotDistributing.count(TEST_INTERFACE_2));
        ASSERT_EQ(switchoverCalls(true), 1u);
        expireSwitchbackHold();
        ASSERT_EQ(switchoverCalls(false), 0u);
    }

    TEST_F(EvpnMhEsBackupLagTest, HoldDoesNotSwitchBackOntoALagThatDoesNotForward)
    {
        activateBackupNhg();
        pushBackup("SET", ES_LAG);
        setDistribution(TEST_INTERFACE_1, false);
        setDistribution(TEST_INTERFACE_2, false);
        ASSERT_EQ(switchoverCalls(true), 1u);

        /* The hold expires while the LAG is oper up but no member distributes. */
        gEvpnMhOrch->m_esBackupSwitchback[ES_LAG] = std::chrono::steady_clock::now();
        expireSwitchbackHold();
        ASSERT_EQ(switchoverCalls(false), 0u);
    }

    /*
     * A DEL after the last member stopped distributing but before the LAG went oper
     * down: held, so FdbOrch's flush at the LAG oper down keeps the MACs as well.
     */
    TEST_F(EvpnMhEsBackupLagTest, DeleteBeforeTheLagGoesDownKeepsItsMacs)
    {
        activateBackupNhg();
        pushBackup("SET", ES_LAG);
        setDistribution(TEST_INTERFACE_1, false);
        setDistribution(TEST_INTERFACE_2, false);
        ASSERT_EQ(set_calls.size(), 2u);

        pushBackup("DEL", ES_LAG);
        ASSERT_EQ(set_calls.size(), 2u);

        Port lag = gPortsOrch->m_portList[ES_LAG];
        lag.m_oper_status = SAI_PORT_OPER_STATUS_DOWN;
        PortOperStateUpdate update = { lag, SAI_PORT_OPER_STATUS_DOWN };
        gFdbOrch->update(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, &update);
        gEvpnMhOrch->update(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, &update);
        gPortsOrch->m_portList[ES_LAG] = lag;
        ASSERT_EQ(es_flush_calls, 0);
        ASSERT_EQ(set_calls.size(), 2u);

        /* Back: switched back after the hold and detached, nothing flushed. */
        setPortOper(ES_LAG, SAI_PORT_OPER_STATUS_UP);
        setDistribution(TEST_INTERFACE_1, true);
        expireSwitchbackHold();
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_EQ(switchoverCalls(false), 1u);
        ASSERT_EQ(set_calls[3].oid, SAI_NULL_OBJECT_ID);
        ASSERT_EQ(es_flush_calls, 0);
        ASSERT_EQ(gEvpnMhOrch->m_esBackup.count(ES_LAG), 0u);
    }

    /* The group goes while a DEL is held in that window: the flush follows the LAG's forwarding. */
    TEST_F(EvpnMhEsBackupLagTest, HeldDeleteFlushesALagThatDoesNotForward)
    {
        activateBackupNhg();
        pushBackup("SET", ES_LAG);
        setDistribution(TEST_INTERFACE_1, false);
        setDistribution(TEST_INTERFACE_2, false);
        pushBackup("DEL", ES_LAG);
        ASSERT_EQ(set_calls.size(), 2u);

        removeGroupBridgePort();
        ASSERT_EQ(set_calls.size(), 4u);
        ASSERT_EQ(es_flush_calls, 1);
        ASSERT_EQ(gEvpnMhOrch->m_esBackup.count(ES_LAG), 0u);
    }
}
