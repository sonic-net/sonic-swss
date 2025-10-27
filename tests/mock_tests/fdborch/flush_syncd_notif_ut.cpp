#include "../ut_helper.h"
#include "../mock_orchagent_main.h"
#include "../mock_table.h"
#include "port.h"
#define private public // Need to modify internal cache
#include "portsorch.h"
#include "fdborch.h"
#include "crmorch.h"
#undef private

#define ETH0 "Ethernet0"
#define VLAN40 "Vlan40"
#define VXLAN_REMOTE "Vxlan_1.1.1.1"

extern CrmOrch*  gCrmOrch;

/*
Test Fixture 
*/
namespace fdb_syncd_flush_test
{

    sai_fdb_api_t ut_sai_fdb_api;
    sai_fdb_api_t *pold_sai_fdb_api;

    sai_status_t _ut_stub_sai_create_fdb_entry (
        _In_ const sai_fdb_entry_t *fdb_entry,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        return SAI_STATUS_SUCCESS;
    }
    void _hook_sai_fdb_api()
    {
        ut_sai_fdb_api = *sai_fdb_api;
        pold_sai_fdb_api = sai_fdb_api;
        ut_sai_fdb_api.create_fdb_entry = _ut_stub_sai_create_fdb_entry;
        sai_fdb_api = &ut_sai_fdb_api;
    }
    void _unhook_sai_fdb_api()
    {
        sai_fdb_api = pold_sai_fdb_api;
    }
    struct FdbOrchTest : public ::testing::Test
    {   
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;
        std::shared_ptr<swss::DBConnector> m_asic_db;
        std::shared_ptr<swss::DBConnector> m_chassis_app_db;
        std::shared_ptr<PortsOrch> m_portsOrch;
        std::shared_ptr<FdbOrch> m_fdborch;

        virtual void SetUp() override
        {   

            testing_db::reset();

            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);

            /* Create Switch */
            sai_attribute_t attr;
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            auto status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_asic_db = std::make_shared<swss::DBConnector>("ASIC_DB", 0);

            // Construct dependencies
            // 1) SwitchOrch
            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);

            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            // 2) Portsorch
            const int portsorch_base_pri = 40;

            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            m_portsOrch = std::make_shared<PortsOrch>(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());

            // 3) Crmorch
            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);
            VxlanTunnelOrch *vxlan_tunnel_orch_1 = new VxlanTunnelOrch(m_state_db.get(), m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
            gDirectory.set(vxlan_tunnel_orch_1);

            // Construct fdborch
            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri},
                { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri},
                { APP_MCLAG_FDB_TABLE_NAME,  FdbOrch::fdborch_pri}
            };

            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);

            m_fdborch = std::make_shared<FdbOrch>(m_app_db.get(), 
                                                  app_fdb_tables, 
                                                  stateDbFdb,
                                                  stateMclagDbFdb, 
                                                  m_portsOrch.get());

            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);

            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_app_db.get(), APP_INTF_TABLE_NAME, gVrfOrch, m_chassis_app_db.get());

            ASSERT_EQ(gNeighOrch, nullptr);
            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, m_fdborch.get(), m_portsOrch.get(), m_chassis_app_db.get());
        }

        virtual void TearDown() override {
            delete gSwitchOrch;
            gSwitchOrch = nullptr;
            delete gCrmOrch;
            gCrmOrch = nullptr;
            gDirectory.m_values.clear();

            delete gVrfOrch;
            gVrfOrch = nullptr;

            delete gIntfsOrch;
            gIntfsOrch = nullptr;

            delete gNeighOrch;
            gNeighOrch = nullptr;

            ut_helper::uninitSaiApi();
        }
    };

    /* Helper Methods */
    void setUpVlan(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for Vlan40 */
        std::string alias = VLAN40;
        sai_object_id_t oid = 0x26000000000796;

        Port vlan(alias, Port::VLAN);
        vlan.m_vlan_info.vlan_oid = oid;
        vlan.m_vlan_info.vlan_id = 40;
        vlan.m_members = set<string>();

        m_portsOrch->m_portList[alias] = vlan;
        m_portsOrch->m_port_ref_count[alias] = 0;
        m_portsOrch->saiOidToAlias[oid] = alias;
    }

    void setUpPort(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for Ethernet0 */
        std::string alias = ETH0;
        sai_object_id_t oid = 0x10000000004a4;

        Port port(alias, Port::PHY);
        port.m_index = 1;
        port.m_port_id = oid;
        port.m_hif_id = 0xd00000000056e;

        m_portsOrch->m_portList[alias] = port;
        m_portsOrch->saiOidToAlias[oid] =  alias;
    }

    void setUpVxlanPort(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for Ethernet0 */
        std::string alias = VXLAN_REMOTE;
        sai_object_id_t oid = 0x10000000004a5;

        Port port(alias, Port::PHY);
        m_portsOrch->m_portList[alias] = port;
        m_portsOrch->saiOidToAlias[oid] =  alias;
    }


    void setUpVlanMember(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for adding Ethernet0 into Vlan40 */
        sai_object_id_t bridge_port_id = 0x3a000000002c33;
        
        /* Add Bridge Port */
        m_portsOrch->m_portList[ETH0].m_bridge_port_id = bridge_port_id;
        m_portsOrch->saiOidToAlias[bridge_port_id] = ETH0;
        m_portsOrch->m_portList[VLAN40].m_members.insert(ETH0);
    }

    void setUpVxlanMember(PortsOrch* m_portsOrch){
        /* Updates portsOrch internal cache for adding Ethernet0 into Vlan40 */
        sai_object_id_t bridge_port_id = 0x3a000000002c34;

        /* Add Bridge Port */
        m_portsOrch->m_portList[VXLAN_REMOTE].m_bridge_port_id = bridge_port_id;
        m_portsOrch->saiOidToAlias[bridge_port_id] = VXLAN_REMOTE;
        m_portsOrch->m_portList[VLAN40].m_members.insert(VXLAN_REMOTE);
    }

    void triggerUpdate(FdbOrch* m_fdborch,
                       sai_fdb_event_t type,
                       vector<uint8_t> mac_addr,
                       sai_object_id_t bridge_port_id,
                       sai_object_id_t bv_id){
        sai_fdb_entry_t entry;
        for (int i = 0; i < (int)mac_addr.size(); i++){
            *(entry.mac_address+i) = mac_addr[i];
        }
        entry.bv_id = bv_id;
        m_fdborch->update(type, &entry, bridge_port_id, SAI_FDB_ENTRY_TYPE_DYNAMIC);
    }
}

namespace fdb_syncd_flush_test
{
    /* Test Consolidated Flush Per Vlan and Per Port */
    TEST_F(FdbOrchTest, ConsolidatedFlushVlanandPort)
    {   
        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());

        /* Event 1: Learn a dynamic FDB Entry */
        // 7c:fe:90:12:22:ec
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_LEARNED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        string port;
        string entry_type;

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Make sure state db is updated as expected */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), true);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), true);
        
        ASSERT_EQ(port, "Ethernet0");
        ASSERT_EQ(entry_type, "dynamic");

        /* Event 2: Generate a FDB Flush per port and per vlan */
        vector<uint8_t> flush_mac_addr = {0, 0, 0, 0, 0, 0};
        for (map<FdbEntry, FdbData>::iterator it = m_fdborch->m_entries.begin(); it != m_fdborch->m_entries.end(); it++)
        {
            it->second.is_flush_pending = true;
        }
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_FLUSHED, flush_mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        /* make sure fdb_counters are decremented */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 0);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 0);

        /* Make sure state db is cleared */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), false);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), false);
    }

    /* Test Consolidated Flush All */
    TEST_F(FdbOrchTest, ConsolidatedFlushAll)
    {   
        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());

        /* Event 1: Learn a dynamic FDB Entry */
        // 7c:fe:90:12:22:ec
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_LEARNED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);
        
        string port;
        string entry_type;

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Make sure state db is updated as expected */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), true);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), true);
        
        ASSERT_EQ(port, "Ethernet0");
        ASSERT_EQ(entry_type, "dynamic");

        /* Event2: Send a Consolidated Flush response from syncd */
        vector<uint8_t> flush_mac_addr = {0, 0, 0, 0, 0, 0};
        for (map<FdbEntry, FdbData>::iterator it = m_fdborch->m_entries.begin(); it != m_fdborch->m_entries.end(); it++)
        {
            it->second.is_flush_pending = true;
        }
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_FLUSHED, flush_mac_addr, SAI_NULL_OBJECT_ID,
                      SAI_NULL_OBJECT_ID);

        /* make sure fdb_counters are decremented */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 0);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 0);

        /* Make sure state db is cleared */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), false);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), false);
    }

    /* Test Consolidated Flush per VLAN BV_ID */
    TEST_F(FdbOrchTest, ConsolidatedFlushVlan)
    {   
        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());

        /* Event 1: Learn a dynamic FDB Entry */
        // 7c:fe:90:12:22:ec
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_LEARNED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);
        
        string port;
        string entry_type;

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Make sure state db is updated as expected */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), true);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), true);
        
        ASSERT_EQ(port, "Ethernet0");
        ASSERT_EQ(entry_type, "dynamic");

        /* Event2: Send a Consolidated Flush response from syncd for vlan */
        vector<uint8_t> flush_mac_addr = {0, 0, 0, 0, 0, 0};
        for (map<FdbEntry, FdbData>::iterator it = m_fdborch->m_entries.begin(); it != m_fdborch->m_entries.end(); it++)
        {
            it->second.is_flush_pending = true;
        }
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_FLUSHED, flush_mac_addr, SAI_NULL_OBJECT_ID,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        /* make sure fdb_counters are decremented */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 0);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 0);

        /* Make sure state db is cleared */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), false);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), false);
    }

    /* Test Consolidated Flush per bridge port id */
    TEST_F(FdbOrchTest, ConsolidatedFlushPort)
    {   
        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());

        /* Event 1: Learn a dynamic FDB Entry */
        // 7c:fe:90:12:22:ec
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_LEARNED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);
        
        string port;
        string entry_type;

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Make sure state db is updated as expected */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), true);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), true);
        
        ASSERT_EQ(port, "Ethernet0");
        ASSERT_EQ(entry_type, "dynamic");

        /* Event2: Send a Consolidated Flush response from syncd for a port */
        vector<uint8_t> flush_mac_addr = {0, 0, 0, 0, 0, 0};
        for (map<FdbEntry, FdbData>::iterator it = m_fdborch->m_entries.begin(); it != m_fdborch->m_entries.end(); it++)
        {
            it->second.is_flush_pending = true;
        }
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_FLUSHED, flush_mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      SAI_NULL_OBJECT_ID);

        /* make sure fdb_counters are decremented */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 0);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 0);

        /* Make sure state db is cleared */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), false);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), false);
    }

    //* Test Consolidated Flush Per Vlan and Per Port, but the bridge_port_id from the internal cache is already deleted */
    TEST_F(FdbOrchTest, ConsolidatedFlushVlanandPortBridgeportDeleted)
    {
        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());

        /* Event 1: Learn a dynamic FDB Entry */
        // 7c:fe:90:12:22:ec
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_LEARNED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        string port;
        string entry_type;

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Make sure state db is updated as expected */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), true);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), true);

        ASSERT_EQ(port, "Ethernet0");
        ASSERT_EQ(entry_type, "dynamic");

        auto bridge_port_oid = m_portsOrch->m_portList[ETH0].m_bridge_port_id;

        /* Delete the bridge_port_oid in the internal OA cache */
        m_portsOrch->m_portList[ETH0].m_bridge_port_id = SAI_NULL_OBJECT_ID;
        m_portsOrch->saiOidToAlias.erase(bridge_port_oid);

        /* Event 2: Generate a FDB Flush per port and per vlan */
        vector<uint8_t> flush_mac_addr = {0, 0, 0, 0, 0, 0};
        for (map<FdbEntry, FdbData>::iterator it = m_fdborch->m_entries.begin(); it != m_fdborch->m_entries.end(); it++)
        {
            it->second.is_flush_pending = true;
        }
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_FLUSHED, flush_mac_addr, bridge_port_oid,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        /* make sure fdb_counter for Vlan is decremented */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 0);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 0);

        /* Make sure state db is cleared */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), false);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), false);
    }

    /* Test Flush Per Vlan and Per Port */
    TEST_F(FdbOrchTest, NonConsolidatedFlushVlanandPort)
    {   
        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(ETH0), m_portsOrch->m_portList.end());
        setUpVlanMember(m_portsOrch.get());

        /* Event 1: Learn a dynamic FDB Entry */
        // 7c:fe:90:12:22:ec
        vector<uint8_t> mac_addr = {124, 254, 144, 18, 34, 236};
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_LEARNED, mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        string port;
        string entry_type;

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 1);

        /* Make sure state db is updated as expected */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), true);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), true);
        
        ASSERT_EQ(port, "Ethernet0");
        ASSERT_EQ(entry_type, "dynamic");

        /* Event 2: Generate a non-consilidated FDB Flush per port and per vlan */
        vector<uint8_t> flush_mac_addr = {124, 254, 144, 18, 34, 236};
        for (map<FdbEntry, FdbData>::iterator it = m_fdborch->m_entries.begin(); it != m_fdborch->m_entries.end(); it++)
        {
            it->second.is_flush_pending = true;
        }
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_FLUSHED, flush_mac_addr, m_portsOrch->m_portList[ETH0].m_bridge_port_id,
                      m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid);

        /* make sure fdb_counters are decremented */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 0);
        ASSERT_EQ(m_portsOrch->m_portList[ETH0].m_fdb_count, 0);

        /* Make sure state db is cleared */
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "port", port), false);
        ASSERT_EQ(m_fdborch->m_fdbStateTable.hget("Vlan40:7c:fe:90:12:22:ec", "type", entry_type), false);
    }

    /* Test Consolidated Flush with origin VXLAN */
    TEST_F(FdbOrchTest, ConsolidatedFlushAllVxLAN)
    {
        _hook_sai_fdb_api();
        ASSERT_NE(m_portsOrch, nullptr);
        setUpVlan(m_portsOrch.get());
        setUpVxlanPort(m_portsOrch.get());
        ASSERT_NE(m_portsOrch->m_portList.find(VLAN40), m_portsOrch->m_portList.end());
        ASSERT_NE(m_portsOrch->m_portList.find(VXLAN_REMOTE), m_portsOrch->m_portList.end());
        setUpVxlanMember(m_portsOrch.get());

        FdbData fdbData;
        fdbData.bridge_port_id = SAI_NULL_OBJECT_ID;
        fdbData.type = "dynamic";
        fdbData.origin = FDB_ORIGIN_VXLAN_ADVERTIZED;
        fdbData.dest_type = VTEP;
        fdbData.dest_value = "1.1.1.1";
        fdbData.esi = "";
        fdbData.vni = 100;
        FdbEntry entry;

        MacAddress mac1 = MacAddress("52:54:00:ac:3a:99");
        entry.mac = mac1;
        entry.port_name = VXLAN_REMOTE;

        entry.bv_id = m_portsOrch->m_portList[VLAN40].m_vlan_info.vlan_oid;
        m_fdborch->addFdbEntry(entry, VXLAN_REMOTE, fdbData);

        /* Make sure fdb_count is incremented as expected */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[VXLAN_REMOTE].m_fdb_count, 1);

        /* Event2: Send a Consolidated Flush response from syncd */
        vector<uint8_t> flush_mac_addr = {0, 0, 0, 0, 0, 0};
        triggerUpdate(m_fdborch.get(), SAI_FDB_EVENT_FLUSHED, flush_mac_addr, SAI_NULL_OBJECT_ID,
                      SAI_NULL_OBJECT_ID);

        /* make sure fdb_counters are decremented */
        ASSERT_EQ(m_portsOrch->m_portList[VLAN40].m_fdb_count, 1);
        ASSERT_EQ(m_portsOrch->m_portList[VXLAN_REMOTE].m_fdb_count, 1);
        _unhook_sai_fdb_api();
    }
}
