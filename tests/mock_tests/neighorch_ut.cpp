#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#define private public
#include "routeorch.h"
#undef private
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
#include "subscriberstatetable.h"

EXTERN_MOCK_FNS

extern std::string gMySwitchType;
extern std::string gMyHostName;
extern std::string gMyAsicName;
extern bool gMultiAsicVoq;

namespace neighorch_test
{
    DEFINE_SAI_API_MOCK(neighbor);
    DEFINE_SAI_GENERIC_API_OBJECT_BULK_MOCK(next_hop, next_hop);
    using namespace std;
    using namespace mock_orch_test;
    using ::testing::DoAll;
    using ::testing::Return;
    using ::testing::SetArgPointee;
    using ::testing::SetArrayArgument;
    using ::testing::Throw;

    static const string TEST_IP = "10.10.10.10";
    static const string VRF_3000 = "Vrf3000";
    static const NeighborEntry VLAN1000_NEIGH = NeighborEntry(TEST_IP, VLAN_1000);
    static const NeighborEntry VLAN2000_NEIGH = NeighborEntry(TEST_IP, VLAN_2000);
    static const NeighborEntry VLAN3000_NEIGH = NeighborEntry(TEST_IP, VLAN_3000);
    static const NeighborEntry VLAN4000_NEIGH = NeighborEntry(TEST_IP, VLAN_4000);

    struct VoqGlobalsGuard
    {
        string switch_type = gMySwitchType;
        string host_name = gMyHostName;
        string asic_name = gMyAsicName;
        bool multi_asic_voq = gMultiAsicVoq;

        ~VoqGlobalsGuard()
        {
            gMySwitchType = switch_type;
            gMyHostName = host_name;
            gMyAsicName = asic_name;
            gMultiAsicVoq = multi_asic_voq;
        }
    };

    struct PortListGuard
    {
        string alias;
        bool port_exists;
        Port port;

        explicit PortListGuard(const string &alias) : alias(alias)
        {
            auto port_it = gPortsOrch->m_portList.find(alias);
            port_exists = (port_it != gPortsOrch->m_portList.end());
            if (port_exists)
            {
                port = port_it->second;
            }
        }

        ~PortListGuard()
        {
            if (port_exists)
            {
                gPortsOrch->m_portList[alias] = port;
            }
            else
            {
                gPortsOrch->m_portList.erase(alias);
            }
        }
    };

    class NeighOrchTest : public MockOrchTest
    {
    protected:
        sai_bulk_object_create_fn old_object_create;

        void SetAndAssertMuxState(std::string interface, std::string state)
        {
            MuxCable *muxCable = m_MuxOrch->getMuxCable(interface);
            muxCable->setState(state);
            EXPECT_EQ(state, muxCable->getState());
        }

        void LearnNeighbor(std::string vlan, std::string ip, std::string mac)
        {
            Table neigh_table = Table(m_app_db.get(), APP_NEIGH_TABLE_NAME);
            string key = vlan + neigh_table.getTableNameSeparator() + ip;
            neigh_table.set(key, { { "neigh", mac }, { "family", "IPv4" } });
            gNeighOrch->addExistingData(&neigh_table);
            static_cast<Orch *>(gNeighOrch)->doTask();
            neigh_table.del(key);
        }

        std::unique_ptr<Consumer> CreateVoqSystemNeighConsumer()
        {
            return std::unique_ptr<Consumer>(new Consumer(
                new swss::SubscriberStateTable(
                    m_chassis_app_db.get(),
                    CHASSIS_APP_SYSTEM_NEIGH_TABLE_NAME,
                    swss::TableConsumable::DEFAULT_POP_BATCH_SIZE,
                    0),
                gNeighOrch,
                CHASSIS_APP_SYSTEM_NEIGH_TABLE_NAME));
        }

        void AddVoqSystemNeighTask(Consumer &consumer, const string &alias)
        {
            string key = alias + consumer.getConsumerTable()->getTableNameSeparator() + TEST_IP;
            consumer.addToSync({ key, SET_COMMAND, { { "encap_index", "1" }, { "neigh", MAC1 } } });
        }

        void SetVoqInbandPortReady()
        {
            string inband_alias = "Vlan4094";
            Port inband_port;
            inband_port.m_alias = inband_alias;
            inband_port.m_type = Port::VLAN;
            gPortsOrch->m_portList[inband_alias] = inband_port;
            gPortsOrch->m_inbandPortName = inband_alias;
        }

        void AddRemoteSystemPort(const string &alias)
        {
            Port remote_system_port;
            remote_system_port.m_alias = alias;
            remote_system_port.m_type = Port::SYSTEM;
            remote_system_port.m_rif_id = SAI_NULL_OBJECT_ID;
            remote_system_port.m_system_port_info.alias = alias;
            remote_system_port.m_system_port_info.type = SAI_SYSTEM_PORT_TYPE_REMOTE;
            gPortsOrch->m_portList[alias] = remote_system_port;
        }

        void ApplyInitialConfigs()
        {
            Table port_table = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            Table vlan_table = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table = Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
            Table neigh_table = Table(m_app_db.get(), APP_NEIGH_TABLE_NAME);
            Table intf_table = Table(m_app_db.get(), APP_INTF_TABLE_NAME);
            Table fdb_table = Table(m_app_db.get(), APP_FDB_TABLE_NAME);
            Table vrf_table = Table(m_app_db.get(), APP_VRF_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(ETHERNET0, ports[ETHERNET0]);
            port_table.set(ETHERNET4, ports[ETHERNET4]);
            port_table.set(ETHERNET8, ports[ETHERNET8]);
            port_table.set("PortConfigDone", { { "count", to_string(1) } });
            port_table.set("PortInitDone", { {} });

            vrf_table.set(VRF_3000, { {"NULL", "NULL"} });

            vlan_table.set(VLAN_1000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_table.set(VLAN_2000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "aa:11:bb:22:cc:33" } });
            vlan_table.set(VLAN_3000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "99:ff:88:ee:77:dd" } });
            vlan_table.set(VLAN_4000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "99:ff:88:ee:77:dd" } });
            vlan_member_table.set(
                VLAN_1000 + vlan_member_table.getTableNameSeparator() + ETHERNET0,
                { { "tagging_mode", "untagged" } });

            vlan_member_table.set(
                VLAN_2000 + vlan_member_table.getTableNameSeparator() + ETHERNET4,
                { { "tagging_mode", "untagged" } });

            vlan_member_table.set(
                VLAN_3000 + vlan_member_table.getTableNameSeparator() + ETHERNET8,
                { { "tagging_mode", "untagged" } });

            vlan_member_table.set(
                VLAN_4000 + vlan_member_table.getTableNameSeparator() + ETHERNET12,
                { { "tagging_mode", "untagged" } });

            intf_table.set(VLAN_1000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });

            intf_table.set(VLAN_2000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });

            intf_table.set(VLAN_3000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "vrf_name", VRF_3000 },
                                        { "mac_addr", "00:00:00:00:00:00" } });

            intf_table.set(VLAN_4000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "vrf_name", VRF_3000 },
                                        { "mac_addr", "00:00:00:00:00:00" } });

            intf_table.set(
                VLAN_1000 + neigh_table.getTableNameSeparator() + "192.168.0.1/24", {
                                                                                        { "scope", "global" },
                                                                                        { "family", "IPv4" },
                                                                                    });

            intf_table.set(
                VLAN_2000 + neigh_table.getTableNameSeparator() + "192.168.2.1/24", {
                                                                                        { "scope", "global" },
                                                                                        { "family", "IPv4" },
                                                                                    });
            intf_table.set(
                VLAN_3000 + neigh_table.getTableNameSeparator() + "192.168.3.1/24", {
                                                                                        { "scope", "global" },
                                                                                        { "family", "IPv4" },
                                                                                    });

            intf_table.set(
                VLAN_4000 + neigh_table.getTableNameSeparator() + "192.168.3.1/24", {
                                                                                        { "scope", "global" },
                                                                                        { "family", "IPv4" },
                                                                                    });

            gPortsOrch->addExistingData(&port_table);
            gPortsOrch->addExistingData(&vlan_table);
            gPortsOrch->addExistingData(&vlan_member_table);
            static_cast<Orch *>(gPortsOrch)->doTask();

            gVrfOrch->addExistingData(&vrf_table);
            static_cast<Orch *>(gVrfOrch)->doTask();

            gIntfsOrch->addExistingData(&intf_table);
            static_cast<Orch *>(gIntfsOrch)->doTask();

            fdb_table.set(
                VLAN_1000 + fdb_table.getTableNameSeparator() + MAC1,
                { { "type", "dynamic" },
                  { "port", ETHERNET0 } });

            fdb_table.set(
                VLAN_2000 + fdb_table.getTableNameSeparator() + MAC2,
                { { "type", "dynamic" },
                  { "port", ETHERNET4 } });

            fdb_table.set(
                VLAN_1000 + fdb_table.getTableNameSeparator() + MAC3,
                { { "type", "dynamic" },
                  { "port", ETHERNET0 } });

            fdb_table.set(
                VLAN_3000 + fdb_table.getTableNameSeparator() + MAC4,
                { { "type", "dynamic" },
                  { "port", ETHERNET8 } });

            fdb_table.set(
                VLAN_4000 + fdb_table.getTableNameSeparator() + MAC5,
                { { "type", "dynamic" },
                  { "port", ETHERNET12 } });

            gFdbOrch->addExistingData(&fdb_table);
            static_cast<Orch *>(gFdbOrch)->doTask();
        }

        void PostSetUp() override
        {
            INIT_SAI_API_MOCK(neighbor);
            INIT_SAI_API_MOCK(next_hop);
            MockSaiApis();
            old_object_create = gNeighOrch->gNextHopBulker.create_entries;
            gNeighOrch->gNextHopBulker.create_entries = mock_create_next_hops;
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
            gNeighOrch->gNextHopBulker.create_entries = old_object_create;
            DEINIT_SAI_API_MOCK(next_hop);
        }
    };

    TEST_F(NeighOrchTest, SystemNeighFromDifferentAsicOnSameHost)
    {
        VoqGlobalsGuard guard;
        gMySwitchType = "voq";
        gMyHostName = "Linecard1";
        gMyAsicName = "Asic0";
        gMultiAsicVoq = true;
        SetVoqInbandPortReady();

        auto consumer = CreateVoqSystemNeighConsumer();
        string remote_asic_alias = gMyHostName + "|Asic1|Ethernet999";
        PortListGuard port_guard(remote_asic_alias);
        AddRemoteSystemPort(remote_asic_alias);
        AddVoqSystemNeighTask(*consumer, remote_asic_alias);

        gNeighOrch->doVoqSystemNeighTask(*consumer);

        ASSERT_EQ(consumer->m_toSync.size(), 1u);
        EXPECT_EQ(kfvKey(consumer->m_toSync.begin()->second),
                  remote_asic_alias + consumer->getConsumerTable()->getTableNameSeparator() + TEST_IP);
    }

    TEST_F(NeighOrchTest, SystemNeighFromSameAsicOnSameHost)
    {
        VoqGlobalsGuard guard;
        gMySwitchType = "voq";
        gMyHostName = "Linecard1";
        gMyAsicName = "Asic0";
        gMultiAsicVoq = true;
        SetVoqInbandPortReady();

        auto consumer = CreateVoqSystemNeighConsumer();
        string local_asic_alias = gMyHostName + "|asic0|Ethernet999";
        AddVoqSystemNeighTask(*consumer, local_asic_alias);

        gNeighOrch->doVoqSystemNeighTask(*consumer);

        ASSERT_TRUE(consumer->m_toSync.empty());
    }

    TEST_F(NeighOrchTest, MultiVlanDuplicateNeighbor)
    {
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry);
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_2000, TEST_IP, MAC2);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 0);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN2000_NEIGH), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry);
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC3);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN2000_NEIGH), 0);
    }

    TEST_F(NeighOrchTest, BulkNextHopFailureDoesNotInstallOrAccountNullNextHop)
    {
        NeighborContext ctx(VLAN1000_NEIGH, true);
        ctx.mac = MacAddress(MAC1);
        NextHopKey nexthop(VLAN1000_NEIGH);
        auto& counter = gCrmOrch->m_resourcesMap.at(CrmResourceType::CRM_IPV4_NEXTHOP)
                            .countersMap["STATS"].usedCounter;
        uint32_t initial_counter = counter;
        int initial_rif_ref_count = gIntfsOrch->getSyncdIntfses().at(VLAN_1000).ref_count;
        std::vector<sai_object_id_t> returned_ids = {0x101};
        std::vector<sai_status_t> returned_statuses = {SAI_STATUS_TABLE_FULL};

        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hops)
            .WillOnce(DoAll(
                SetArrayArgument<5>(returned_ids.begin(), returned_ids.end()),
                SetArrayArgument<6>(returned_statuses.begin(), returned_statuses.end()),
                Return(SAI_STATUS_FAILURE)));

        ASSERT_TRUE(gNeighOrch->addNextHop(ctx));
        EXPECT_EQ(ctx.nexthop_status, SAI_STATUS_NOT_EXECUTED);

        gNeighOrch->gNextHopBulker.flush();

        EXPECT_EQ(ctx.next_hop_id, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(ctx.nexthop_status, SAI_STATUS_TABLE_FULL);
        EXPECT_FALSE(gNeighOrch->processBulkAddNextHop(ctx));
        EXPECT_EQ(gNeighOrch->m_syncdNextHops.count(nexthop), 0);
        EXPECT_EQ(counter, initial_counter);
        EXPECT_EQ(gIntfsOrch->getSyncdIntfses().at(VLAN_1000).ref_count, initial_rif_ref_count);
    }

    TEST_F(NeighOrchTest, UnwrittenBulkNextHopStatusDoesNotFinalizeNeighbor)
    {
        NeighborContext ctx(VLAN1000_NEIGH, true);
        ctx.mac = MacAddress(MAC1);
        NextHopKey nexthop(VLAN1000_NEIGH);
        auto& counter = gCrmOrch->m_resourcesMap.at(CrmResourceType::CRM_IPV4_NEXTHOP)
                            .countersMap["STATS"].usedCounter;
        uint32_t initial_counter = counter;
        int initial_rif_ref_count = gIntfsOrch->getSyncdIntfses().at(VLAN_1000).ref_count;
        std::vector<sai_object_id_t> returned_ids = {0x101};

        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hops)
            .WillOnce(DoAll(
                SetArrayArgument<5>(returned_ids.begin(), returned_ids.end()),
                Return(SAI_STATUS_FAILURE)));

        ASSERT_TRUE(gNeighOrch->addNextHop(ctx));
        gNeighOrch->gNextHopBulker.flush();

        EXPECT_EQ(ctx.next_hop_id, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(ctx.nexthop_status, SAI_STATUS_NOT_EXECUTED);
        EXPECT_FALSE(gNeighOrch->processBulkAddNextHop(ctx));
        EXPECT_EQ(gNeighOrch->m_syncdNextHops.count(nexthop), 0);
        EXPECT_EQ(counter, initial_counter);
        EXPECT_EQ(gIntfsOrch->getSyncdIntfses().at(VLAN_1000).ref_count, initial_rif_ref_count);
    }

    TEST_F(NeighOrchTest, MultiVlanUnableToRemoveNeighbor)
    {
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        NextHopKey nexthop = { TEST_IP, VLAN_1000 };
        gNeighOrch->m_syncdNextHops[nexthop].ref_count = 1;

        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry).Times(0);
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry).Times(0);
        LearnNeighbor(VLAN_2000, TEST_IP, MAC2);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN2000_NEIGH), 0);
    }

    TEST_F(NeighOrchTest, MultiVlanDifferentVrfDuplicateNeighbor)
    {
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry).Times(0);
        LearnNeighbor(VLAN_3000, TEST_IP, MAC4);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN3000_NEIGH), 1);
    }

    TEST_F(NeighOrchTest, MultiVlanSameVrfDuplicateNeighbor)
    {
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_3000, TEST_IP, MAC4);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN3000_NEIGH), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry);
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_4000, TEST_IP, MAC5);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN3000_NEIGH), 0);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN4000_NEIGH), 1);
    }

    TEST_F(NeighOrchTest, MultiVlanDuplicateNeighborMissingExistingVlanPort)
    {
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);

        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry).Times(0);
        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry).Times(0);
        gPortsOrch->m_portList.erase(VLAN_1000);
        LearnNeighbor(VLAN_2000, TEST_IP, MAC2);
    }

    TEST_F(NeighOrchTest, MultiVlanDuplicateNeighborMissingNewVlanPort)
    {
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);

        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry).Times(0);
        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry).Times(0);
        gPortsOrch->m_portList.erase(VLAN_2000);
        LearnNeighbor(VLAN_2000, TEST_IP, MAC2);
    }

    TEST_F(NeighOrchTest, SkipHostInterfaceUsb0)
    {
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry).Times(0);
        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry).Times(0);
        LearnNeighbor("usb0", TEST_IP, MAC1);
        /* Literal "usb0" can overload-resolve to NextHopKey(str, bool overlay) vs (str, str). */
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(NeighborEntry(TEST_IP, std::string("usb0"))), 0);
    }

    /* --- IPinIP tunnel NextHopKey tests --- */

    TEST(NextHopKeyTunnelTest, TunnelNextHopKeyConstructor)
    {
        IpAddress ip("10.1.0.32");
        NextHopKey nh(ip, string("MuxTunnel0"), true /*tunnel_nh*/, 0 /*tag*/);

        EXPECT_TRUE(nh.isTunnelNextHop());
        EXPECT_EQ(nh.ip_address, ip);
        EXPECT_EQ(nh.tunnel_name, "MuxTunnel0");
        EXPECT_EQ(nh.alias, "");
        EXPECT_EQ(nh.vni, 0u);
        EXPECT_FALSE(nh.isSrv6NextHop());
        EXPECT_FALSE(nh.isMplsNextHop());
    }

    TEST(NextHopKeyTunnelTest, TunnelNextHopKeyToStringRoundtrip)
    {
        IpAddress ip("192.168.1.1");
        NextHopKey original(ip, string("IPINIP_TUNNEL"), true /*tunnel_nh*/, 0 /*tag*/);

        string str = original.to_string();
        EXPECT_EQ(str, "tunnel:IPINIP_TUNNEL@192.168.1.1");

        NextHopKey parsed(str);
        EXPECT_TRUE(parsed.isTunnelNextHop());
        EXPECT_EQ(parsed.tunnel_name, "IPINIP_TUNNEL");
        EXPECT_EQ(parsed.ip_address, ip);
        EXPECT_EQ(original, parsed);
    }

    TEST(NextHopKeyTunnelTest, TunnelNextHopKeyComparison)
    {
        NextHopKey nh_a(IpAddress("10.0.0.1"), string("TunA"), true, 0);
        NextHopKey nh_b(IpAddress("10.0.0.1"), string("TunB"), true, 0);
        NextHopKey nh_same(IpAddress("10.0.0.1"), string("TunA"), true, 0);

        EXPECT_EQ(nh_a, nh_same);
        EXPECT_NE(nh_a, nh_b);

        NextHopKey regular_nh(IpAddress("10.0.0.1"), string("Ethernet0"));
        EXPECT_NE(nh_a, regular_nh);
    }

    TEST(NextHopKeyTunnelTest, TunnelNextHopKeyInvalidParseFails)
    {
        EXPECT_THROW(NextHopKey("tunnel:@10.0.0.1@extra"), std::invalid_argument);
        EXPECT_THROW(NextHopKey("tunnel:OnlyName"), std::invalid_argument);
    }

    // Multiple producers can register the same tunnel NH key; the entry
    // must survive until the last registrant unregisters.
    TEST_F(NeighOrchTest, IpinipTunnelNextHopMultiProducerRegistration)
    {
        IpAddress ip("10.2.0.1");
        NextHopKey nh(ip, string("MuxTunnel0"), true /*tunnel_nh*/, 0 /*tag*/);
        const sai_object_id_t tunnel_id = 0x5000;
        const sai_object_id_t oid = 0x1001;
        sai_object_id_t nh_id;

        // First producer registers the key: SAI object created.
        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hop)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(oid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_EQ(gNeighOrch->addIpinipTunnelNextHop(nh, tunnel_id, nh_id), TunnelNhOpStatus::CREATED);
        EXPECT_EQ(nh_id, oid);
        ASSERT_EQ(gNeighOrch->m_syncdNextHops.count(nh), 1);
        EXPECT_EQ(gNeighOrch->m_syncdNextHops[nh].next_hop_id, oid);
        EXPECT_EQ(gNeighOrch->m_ipinipTunnelNextHopRegRefs[nh], 1u);

        // Second producer registers the same key: reused, no SAI call.
        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hop).Times(0);
        EXPECT_EQ(gNeighOrch->addIpinipTunnelNextHop(nh, tunnel_id, nh_id), TunnelNhOpStatus::REUSED);
        EXPECT_EQ(nh_id, oid);
        EXPECT_EQ(gNeighOrch->m_ipinipTunnelNextHopRegRefs[nh], 2u);

        // First producer tears down: entry must survive, no SAI call.
        EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hop).Times(0);
        EXPECT_EQ(gNeighOrch->removeIpinipTunnelNextHop(nh), TunnelNhOpStatus::OTHER_REGISTRANTS_REMAIN);
        EXPECT_EQ(gNeighOrch->m_syncdNextHops.count(nh), 1);
        EXPECT_EQ(gNeighOrch->m_ipinipTunnelNextHopRegRefs[nh], 1u);

        // Last producer tears down: SAI object deleted, entry erased.
        EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hop)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));
        EXPECT_EQ(gNeighOrch->removeIpinipTunnelNextHop(nh), TunnelNhOpStatus::REMOVED);
        EXPECT_EQ(gNeighOrch->m_syncdNextHops.count(nh), 0);
        EXPECT_EQ(gNeighOrch->m_ipinipTunnelNextHopRegRefs.count(nh), 0);

        // Removing an already-gone key is idempotent and touches no SAI object.
        EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hop).Times(0);
        EXPECT_EQ(gNeighOrch->removeIpinipTunnelNextHop(nh), TunnelNhOpStatus::REMOVED);
    }

    // create_next_hop() failing on first registration must leave nothing
    // registered, so the caller's normal retry path can safely call again.
    TEST_F(NeighOrchTest, IpinipTunnelNextHopCreateFailureIsRetryable)
    {
        IpAddress ip("10.2.0.2");
        NextHopKey nh(ip, string("MuxTunnel0"), true /*tunnel_nh*/, 0 /*tag*/);
        const sai_object_id_t tunnel_id = 0x5000;
        const sai_object_id_t oid = 0x1002;
        sai_object_id_t nh_id;

        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hop)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_FAILURE));
        EXPECT_EQ(gNeighOrch->addIpinipTunnelNextHop(nh, tunnel_id, nh_id), TunnelNhOpStatus::SAI_FAILED);
        EXPECT_EQ(gNeighOrch->m_syncdNextHops.count(nh), 0);
        EXPECT_EQ(gNeighOrch->m_ipinipTunnelNextHopRegRefs.count(nh), 0);

        // Retry succeeds.
        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hop)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(oid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_EQ(gNeighOrch->addIpinipTunnelNextHop(nh, tunnel_id, nh_id), TunnelNhOpStatus::CREATED);
        EXPECT_EQ(nh_id, oid);

        // Cleanup.
        EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hop)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));
        EXPECT_EQ(gNeighOrch->removeIpinipTunnelNextHop(nh), TunnelNhOpStatus::REMOVED);
    }

    TEST_F(NeighOrchTest, ProcessFDBAdd_EnableNeighbor)
    {
        // Setup: Learn a neighbor first
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        // Disable the neighbor to simulate it being disabled
        EXPECT_TRUE(gNeighOrch->disableNeighbor(VLAN1000_NEIGH));
        EXPECT_FALSE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));

        // Create FDB entry to trigger processFDBAdd
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBAdd - should re-enable the neighbor
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        gNeighOrch->processFDBAdd(fdb_entry);

        // Verify neighbor is enabled
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
    }

    TEST_F(NeighOrchTest, ProcessFDBAdd_InvalidVlanId)
    {
        // Setup: Learn a neighbor first
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        // Create FDB entry with invalid VLAN ID
        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = 0x999999; // Invalid VLAN ID
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBAdd with invalid VLAN - should not crash or affect neighbors
        gNeighOrch->processFDBAdd(fdb_entry);

        // Verify neighbor state unchanged
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
    }

    TEST_F(NeighOrchTest, ProcessFDBDelete_DisableNeighbor)
    {
        // Setup: Learn a neighbor first
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));

        // Create FDB entry to trigger processFDBDelete
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBDelete - should disable the neighbor
        gNeighOrch->processFDBDelete(fdb_entry);

        // Verify neighbor is disabled but still in cache
        EXPECT_FALSE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
    }

    TEST_F(NeighOrchTest, ProcessFDBDelete_NoMatchingNeighbor)
    {
        // Setup: Learn a neighbor with MAC1
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        // Create FDB entry with different MAC
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC2); // Different MAC
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBDelete with non-matching MAC - should not affect neighbor
        gNeighOrch->processFDBDelete(fdb_entry);

        // Verify neighbor state unchanged
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
    }

    TEST_F(NeighOrchTest, ProcessFDBResolve_TriggerArpResolution)
    {
        // Setup: Learn a neighbor first
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        // Create FDB entry to trigger processFDBResolve
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBResolve - should trigger ARP resolution
        gNeighOrch->processFDBResolve(fdb_entry);

        // Verify neighbor entry is still present (ARP resolve doesn't remove it)
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
    }

    TEST_F(NeighOrchTest, ProcessFDBResolve_InvalidVlanId)
    {
        // Setup: Learn a neighbor first
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        // Create FDB entry with invalid VLAN ID
        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = 0x888888; // Invalid VLAN ID
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBResolve with invalid VLAN - should not crash
        gNeighOrch->processFDBResolve(fdb_entry);

        // Verify neighbor state unchanged
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
    }

    TEST_F(NeighOrchTest, ProcessFDBFunctions_MultipleNeighborsOnSameVlan)
    {
        const string TEST_IP2 = "10.10.10.11";
        const NeighborEntry VLAN1000_NEIGH2 = NeighborEntry(TEST_IP2, VLAN_1000);

        // Setup: Learn two neighbors on the same VLAN
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry).Times(2);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        LearnNeighbor(VLAN_1000, TEST_IP2, MAC3);

        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH2), 1);

        // Create FDB entry for first neighbor's MAC
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBDelete - should only affect the matching neighbor
        gNeighOrch->processFDBDelete(fdb_entry);

        // Verify only first neighbor is disabled, second remains enabled
        EXPECT_FALSE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH2));
    }

    TEST_F(NeighOrchTest, ProcessFDBFunctions_DifferentVlansSameMac)
    {
        // Setup: Learn neighbors on different VLANs with different MACs
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_2000, TEST_IP, MAC2); // Different MAC, different VLAN
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN2000_NEIGH), 1);

        // Create FDB entry for VLAN_1000
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBDelete - should only affect VLAN_1000 neighbor
        gNeighOrch->processFDBDelete(fdb_entry);

        // Verify only VLAN_1000 neighbor is disabled, VLAN_2000 remains enabled
        EXPECT_FALSE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN2000_NEIGH));
    }
}
