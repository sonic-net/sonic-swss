#include "ut_helper.h"
#include "switchorch.h"
#include "bufferorch.h"
#include "vnetorch.h"
#include "vxlanorch.h"
#include "swssnet.h"
#include "tokenize.h"
#include "subscriberstatetable.h"
#include "mock_orchagent_main.h"

namespace intfsorch_test
{
    using namespace std;
    using namespace swss;

    struct MockIntfsOrch
    {
        IntfsOrch *m_intfsOrch;
        DBConnector *m_appl_db;

        MockIntfsOrch(DBConnector *appl_db, IntfsOrch *intfsOrch) :
            m_appl_db(appl_db),
            m_intfsOrch(intfsOrch)
        {
        }

        ~MockIntfsOrch()
        {
        }

        operator IntfsOrch *()
        {
            return m_intfsOrch;
        }

        void doTask(const deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(new ConsumerStateTable(m_appl_db, APP_INTF_TABLE_NAME, 1, 1), m_intfsOrch, APP_INTF_TABLE_NAME));

            Portal::ConsumerInternal::addToSync(consumer.get(), entries);
            static_cast<Orch *>(m_intfsOrch)->doTask(*consumer);
        }

        void doTask(const deque<KeyOpFieldsValuesTuple> &entries, const string &command)
        {
            deque<KeyOpFieldsValuesTuple> tmp(entries);

            for (auto it = tmp.begin(); it != tmp.end(); ++it)
            {
                get<1>(*it) = command;
            }

            doTask(tmp);
        }
    };

    class IntfsOrchTest : public ::testing::Test
    {
    public:
        bool initBridgePorts();
        bool createVRF(const string &vrf_name);
        bool createVlan(const string &vlan_name, const int mtu);
        bool addVlanMember(const string &vlan_name, const string &intf_name, const string &tagging_mode);
        bool createLag(const string &lag_name, const int mtu);
        bool addLagMember(const string &lag_name, const string &intf_name);
        bool createVxlan(const string &tunnel_name, const string &src_ip, const string &dst_ip);
        bool createVnet(const string &vnet_name, uint32_t vni, const string &vxlan_name);
        sai_object_id_t getRouterInterfaceId(const string &alias);
        sai_object_id_t getVirtualRouterId(sai_object_id_t rif_id);
        sai_object_id_t getCpuPortId();
        bool isRouteExist(sai_object_id_t vr_id, const IpPrefix &p);
        bool getRoutePacketAction(sai_object_id_t vr_id, const IpPrefix &p, int *packet_action);
        sai_object_id_t getRouteNexthopId(sai_object_id_t vr_id, const IpPrefix &p);
        bool getNeighborDstMac(sai_object_id_t rif_id, const IpAddress &ip, MacAddress &mac);
        uint32_t getRouterIntfMtu(sai_object_id_t rif_id);

        shared_ptr<DBConnector> m_configDb;
        shared_ptr<DBConnector> m_applDb;
        uint32_t m_baseIpv4RouteCounter;
        uint32_t m_baseIpv6RouteCounter;

        void SetUp() override
        {

            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            auto status = ut_helper::initSaiApi(profile);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            // Create switch
            sai_attribute_t attr;
            vector<sai_attribute_t> attrs;
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            attrs.push_back(attr);
            status = sai_switch_api->create_switch(&gSwitchId, (uint32_t)attrs.size(), attrs.data());
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            // Get switch source MAC address
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gMacAddress = attr.value.mac;
            gVxlanMacAddress = attr.value.mac;

            // Get virtual router id
            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gVirtualRouterId = attr.value.oid;

            // Create a loopback underlay router interface
            vector<sai_attribute_t> underlay_intf_attrs;
            sai_attribute_t underlay_intf_attr;

            underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
            underlay_intf_attr.value.oid = gVirtualRouterId;
            underlay_intf_attrs.push_back(underlay_intf_attr);

            underlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
            underlay_intf_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
            underlay_intf_attrs.push_back(underlay_intf_attr);

            status = sai_router_intfs_api->create_router_interface(&gUnderlayIfId, gSwitchId, (uint32_t)underlay_intf_attrs.size(), underlay_intf_attrs.data());
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            // create db connectors
            m_configDb = make_shared<swss::DBConnector>(CONFIG_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
            m_applDb = make_shared<swss::DBConnector>(APPL_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);

            // Create all required orchs

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_applDb.get(), APP_SWITCH_TABLE_NAME);
            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_configDb.get(), CFG_CRM_TABLE_NAME);

            const int portsorch_base_pri = 40;
            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_applDb.get(), ports_tables);

            vector<string> buffer_tables = {
                CFG_BUFFER_POOL_TABLE_NAME,
                CFG_BUFFER_PROFILE_TABLE_NAME,
                CFG_BUFFER_QUEUE_TABLE_NAME,
                CFG_BUFFER_PG_TABLE_NAME,
                CFG_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                CFG_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
            };

            ASSERT_EQ(gBufferOrch, nullptr);
            gBufferOrch = new BufferOrch(m_configDb.get(), buffer_tables);

            ASSERT_EQ(gDirectory.get<VNetOrch *>(), nullptr);
            VNetOrch *vnet_orch = new VNetOrch(m_applDb.get(), APP_VNET_TABLE_NAME);
            gDirectory.set(vnet_orch);

            vector<string> vnet_tables = {
                APP_VNET_RT_TABLE_NAME,
                APP_VNET_RT_TUNNEL_TABLE_NAME
            };

            ASSERT_EQ(gDirectory.get<VNetRouteOrch *>(), nullptr);
            VNetRouteOrch *vnet_rt_orch = new VNetRouteOrch(m_applDb.get(), vnet_tables, vnet_orch);
            gDirectory.set(vnet_rt_orch);

            ASSERT_EQ(gDirectory.get<VxlanTunnelOrch *>(), nullptr);
            VxlanTunnelOrch *vxlan_tunnel_orch = new VxlanTunnelOrch(m_configDb.get(), CFG_VXLAN_TUNNEL_TABLE_NAME);
            gDirectory.set(vxlan_tunnel_orch);

            ASSERT_EQ(gDirectory.get<VxlanTunnelMapOrch *>(), nullptr);
            VxlanTunnelMapOrch *vxlan_tunnel_map_orch = new VxlanTunnelMapOrch(m_configDb.get(), CFG_VXLAN_TUNNEL_MAP_TABLE_NAME);
            gDirectory.set(vxlan_tunnel_map_orch);

            ASSERT_EQ(gDirectory.get<VxlanVrfMapOrch *>(), nullptr);
            VxlanVrfMapOrch *vxlan_vrf_map_orch = new VxlanVrfMapOrch(m_applDb.get(), APP_VXLAN_VRF_TABLE_NAME);
            gDirectory.set(vxlan_vrf_map_orch);

            ASSERT_EQ(gVrfOrch, nullptr);
            ASSERT_EQ(gDirectory.get<VRFOrch *>(), nullptr);
            gVrfOrch = new VRFOrch(m_applDb.get(), APP_VRF_TABLE_NAME);
            gDirectory.set(gVrfOrch);

            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_applDb.get(), APP_INTF_TABLE_NAME, gVrfOrch);

            ASSERT_EQ(gNeighOrch, nullptr);
            gNeighOrch = new NeighOrch(m_applDb.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch);

            ASSERT_EQ(gRouteOrch, nullptr);
            gRouteOrch = new RouteOrch(m_applDb.get(), APP_ROUTE_TABLE_NAME, gNeighOrch);

            // Create all bridge ports and initial portsorch
            ASSERT_TRUE(initBridgePorts());

            // Crm counters may not start from zero
            rebaseCrmResourceCounters(gCrmOrch);
        }

        void TearDown() override
        {
            delete gSwitchOrch;
            gSwitchOrch = nullptr;
            delete gPortsOrch;
            gPortsOrch = nullptr;
            delete gIntfsOrch;
            gIntfsOrch = nullptr;
            delete gNeighOrch;
            gNeighOrch = nullptr;
            delete gRouteOrch;
            gRouteOrch = nullptr;
            delete gCrmOrch;
            gCrmOrch = nullptr;
            delete gBufferOrch;
            gBufferOrch = nullptr;
            delete gVrfOrch;
            gVrfOrch = nullptr;

            if (gDirectory.get<VNetOrch *>())
            {
                delete gDirectory.get<VNetOrch *>();
            }

            if (gDirectory.get<VNetRouteOrch *>())
            {
                delete gDirectory.get<VNetRouteOrch *>();
            }

            if (gDirectory.get<VxlanTunnelOrch *>())
            {
                delete gDirectory.get<VxlanTunnelOrch *>();
            }

            if (gDirectory.get<VxlanTunnelMapOrch *>())
            {
                delete gDirectory.get<VxlanTunnelMapOrch *>();
            }

            if (gDirectory.get<VxlanVrfMapOrch *>())
            {
                delete gDirectory.get<VxlanVrfMapOrch *>();
            }

            Portal::DirectoryInternal::clearValues(&gDirectory);

            sai_status_t status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_TRUE(status == SAI_STATUS_SUCCESS);
            gSwitchId = 0;

            ut_helper::uninitSaiApi();
        }

        shared_ptr<MockIntfsOrch> createIntfsOrch()
        {
            return make_shared<MockIntfsOrch>(m_applDb.get(), gIntfsOrch);
        }

        void rebaseCrmResourceCounters(CrmOrch *crmOrch)
        {
            auto resource_map = Portal::CrmOrchInternal::getResourceMap(crmOrch);

            m_baseIpv4RouteCounter = resource_map.at(CrmResourceType::CRM_IPV4_ROUTE).countersMap["STATS"].usedCounter;
            m_baseIpv6RouteCounter = resource_map.at(CrmResourceType::CRM_IPV6_ROUTE).countersMap["STATS"].usedCounter;
        }

        bool validateResourceCountWithCrm(IntfsOrch *intfsOrch, CrmOrch *crmOrch)
        {
            auto resource_map = Portal::CrmOrchInternal::getResourceMap(crmOrch);
            uint32_t crm_ipv4_route_cnt = resource_map.at(CrmResourceType::CRM_IPV4_ROUTE).countersMap["STATS"].usedCounter;
            uint32_t crm_ipv6_route_cnt = resource_map.at(CrmResourceType::CRM_IPV6_ROUTE).countersMap["STATS"].usedCounter;
            uint32_t intfs_ipv4_route_cnt = m_baseIpv4RouteCounter, intfs_ipv6_route_cnt = m_baseIpv6RouteCounter;
            auto intfs_table = intfsOrch->getSyncdIntfses();

            for (auto intf : intfs_table)
            {
                uint32_t amount = (intf.first == "lo") ? 1 : 2;
                for (auto ip : intf.second.ip_addresses)
                {
                    if (ip.isV4())
                    {
                        intfs_ipv4_route_cnt += amount;
                    }
                    else
                    {
                        intfs_ipv6_route_cnt += amount;
                    }
                }
            }

            return (intfs_ipv4_route_cnt == crm_ipv4_route_cnt && intfs_ipv6_route_cnt == crm_ipv6_route_cnt);
        }

        bool validateResourceWithSai(IntfsOrch *intfsOrch, PortsOrch *portsOrch)
        {
            auto intfs_table = intfsOrch->getSyncdIntfses();

            for (auto intf : intfs_table)
            {
                sai_object_id_t vr_id = gVirtualRouterId;
                Port port;

                if (intf.first != "lo")
                {
                    if (!portsOrch->getPort(intf.first, port))
                    {
                        return false;
                    }
                    vr_id = port.m_vr_id;
                }

                for (auto ip_prefix : intf.second.ip_addresses)
                {
                    if (intf.first != "lo")
                    {
                        int packet_action;

                        if (!getRoutePacketAction(vr_id, ip_prefix, &packet_action))
                        {
                            return false;
                        }

                        if (port.m_rif_id == SAI_NULL_OBJECT_ID ||
                            getVirtualRouterId(port.m_rif_id) != vr_id ||
                            packet_action != SAI_PACKET_ACTION_FORWARD ||
                            getRouteNexthopId(vr_id, ip_prefix) != port.m_rif_id)
                        {
                            return false;
                        }
                    }

                    IpPrefix ip_addr(ip_prefix.getIp().to_string() + (ip_prefix.isV4() ? "/32" : "/128"));
                    int packet_action;

                    if (!getRoutePacketAction(vr_id, ip_addr, &packet_action))
                    {
                        return false;
                    }

                    if (packet_action != SAI_PACKET_ACTION_FORWARD ||
                        getRouteNexthopId(vr_id, ip_addr) != getCpuPortId())
                    {
                        return false;
                    }

                    if (intf.first.substr(0, 4) == VLAN_PREFIX && ip_prefix.isV4())
                    {
                        MacAddress mac;

                        if (!getNeighborDstMac(port.m_rif_id, ip_prefix.getBroadcastIp(), mac) ||
                            mac != MacAddress("ff:ff:ff:ff:ff:ff"))
                        {
                            return false;
                        }
                    }
                }
            }

            return true;
        }

        // Validate consistency between orchagent and CRM/SAI
        bool validateLowerLayerDb(const MockIntfsOrch *orch)
        {
            if (!validateResourceCountWithCrm(orch->m_intfsOrch, gCrmOrch))
            {
                return false;
            }

            if (!validateResourceWithSai(orch->m_intfsOrch, gPortsOrch))
            {
                return false;
            }

            return true;
        }

        // Validate consistency between orchagent and consumer data
        bool validateOrchAgentByConfOp(IntfsOrch *intfsOrch, PortsOrch *portsOrch, VRFOrch *vrfOrch, const deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto intfs_table = intfsOrch->getSyncdIntfses();
            set<IpPrefix> all_routes = intfsOrch->getSubnetRoutes();
            uint32_t addr_count = 0;

            for (auto intf : intfs_table)
            {
                addr_count += (uint32_t)intf.second.ip_addresses.size();
            }

            if (addr_count != all_routes.size())
            {
                return false;
            }

            for (auto t : entries)
            {
                vector<string> keys = tokenize(kfvKey(t), ':');
                string intf_name = keys[0];

                if (intfs_table.find(intf_name) == intfs_table.end())
                {
                    return false;
                }

                if (keys.size() > 1)
                {
                    IpPrefix ip_prefix(kfvKey(t).substr(kfvKey(t).find(':') + 1));

                    if (all_routes.find(ip_prefix) == all_routes.end() ||
                        !intfs_table[intf_name].ip_addresses.count(ip_prefix))
                    {
                        return false;
                    }

                    all_routes.erase(ip_prefix);
                }

                if (intf_name == "lo")
                {
                    continue;
                }

                const vector<FieldValueTuple> &data = kfvFieldsValues(t);
                string vrf_name = "", vnet_name = "";

                for (auto idx : data)
                {
                    const auto &field = fvField(idx);
                    const auto &value = fvValue(idx);
                    if (field == "vrf_name")
                    {
                        vrf_name = value;
                    }
                    else if (field == "vnet_name")
                    {
                        vnet_name = value;
                    }
                }

                sai_object_id_t vrf_id = gVirtualRouterId;
                if (!vnet_name.empty())
                {
                    VNetOrch *vnet_orch = gDirectory.get<VNetOrch *>();

                    if (!vnet_orch->isVnetExists(vnet_name))
                    {
                        return false;
                    }

                    auto *vnet_obj = vnet_orch->getTypePtr<VNetVrfObject>(vnet_name);

                    vrf_id = vnet_obj->getVRid();
                }
                else if (!vrf_name.empty())
                {
                    if (!vrfOrch->isVRFexists(vrf_name))
                    {
                        return false;
                    }

                    vrf_id = vrfOrch->getVRFid(vrf_name);
                }

                Port port;
                if (!portsOrch->getPort(intf_name, port) ||
                    port.m_vr_id != vrf_id ||
                    port.m_rif_id != intfsOrch->getRouterIntfsId(intf_name) ||
                    port.m_rif_id == SAI_NULL_OBJECT_ID)
                {
                    return false;
                }
            }

            if (!all_routes.empty())
            {
                return false;
            }

            return true;
        }
    };

    bool IntfsOrchTest::initBridgePorts()
    {
        vector<sai_object_id_t> port_list;
        sai_attribute_t attr;
        sai_status_t status;
        uint32_t port_count;

        auto consumer = unique_ptr<Consumer>(new Consumer(new ConsumerStateTable(m_applDb.get(), APP_PORT_TABLE_NAME, 1, 1), gPortsOrch, APP_PORT_TABLE_NAME));

        attr.id = SAI_SWITCH_ATTR_PORT_NUMBER;
        status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            return false;
        }
        port_count = attr.value.u32;

        port_list.resize(port_count);
        attr.id = SAI_SWITCH_ATTR_PORT_LIST;
        attr.value.objlist.count = port_count;
        attr.value.objlist.list = port_list.data();
        status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
        if (status != SAI_STATUS_SUCCESS || attr.value.objlist.count != port_count)
        {
            return false;
        }

        deque<KeyOpFieldsValuesTuple> port_init_tuple;
        for (uint32_t i = 0; i < port_count; ++i)
        {
            string lanes_str = "";
            sai_uint32_t lanes[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
            attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
            attr.value.u32list.count = 8;
            attr.value.u32list.list = lanes;
            status = sai_port_api->get_port_attribute(port_list[i], 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                return false;
            }

            for (uint32_t j = 0; j < attr.value.u32list.count; ++j)
            {
                lanes_str += (j == 0) ? "" : ",";
                lanes_str += to_string(attr.value.u32list.list[j]);
            }

            port_init_tuple.push_back(
                { FRONT_PANEL_PORT_PREFIX + to_string(i),
                  SET_COMMAND,
                  { { "lanes", lanes_str },
                    { "speed", "1000" },
                    { "mtu", "6000" },
                    { "admin_status", "up" } } });
        }

        port_init_tuple.push_back({ "PortConfigDone", SET_COMMAND, { { "count", to_string(port_count) } } });

        Portal::ConsumerInternal::addToSync(consumer.get(), port_init_tuple);
        static_cast<Orch *>(gPortsOrch)->doTask(*consumer.get());

        Portal::ConsumerInternal::addToSync(consumer.get(), { { "PortInitDone", EMPTY_PREFIX, { { "", "" } } } });
        static_cast<Orch *>(gPortsOrch)->doTask(*consumer.get());

        return gPortsOrch->isInitDone();
    }

    bool IntfsOrchTest::createVRF(const string &vrf_name)
    {
        auto consumer = unique_ptr<Consumer>(new Consumer(new ConsumerStateTable(m_applDb.get(), APP_VRF_TABLE_NAME, 1, 1), gVrfOrch, APP_VRF_TABLE_NAME));
        auto vrf_data = deque<KeyOpFieldsValuesTuple>(
            { { vrf_name,
                SET_COMMAND,
                { { "v4", "true" },
                  { "v6", "true" },
                  { "src_mac", gMacAddress.to_string() },
                  { "ttl_action", "trap" },
                  { "ip_opt_action", "drop" },
                  { "l3_mc_action", "forward" } } } });

        Portal::ConsumerInternal::addToSync(consumer.get(), vrf_data);
        static_cast<Orch *>(gVrfOrch)->doTask(*consumer.get());

        return gVrfOrch->isVRFexists(vrf_name);
    }

    bool IntfsOrchTest::createVlan(const string &vlan_name, const int mtu = 9100)
    {
        auto consumer = unique_ptr<Consumer>(new Consumer(new ConsumerStateTable(m_applDb.get(), APP_VLAN_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0), gPortsOrch, APP_VLAN_TABLE_NAME));
        auto vlan_data = deque<KeyOpFieldsValuesTuple>(
            { { vlan_name,
                SET_COMMAND,
                { { "mtu", to_string(mtu) } } } });

        Portal::ConsumerInternal::addToSync(consumer.get(), vlan_data);
        gPortsOrch->doVlanTask(*consumer.get());

        Port p;
        if (!gPortsOrch->getPort(vlan_name, p) || p.m_vlan_info.vlan_oid == SAI_NULL_OBJECT_ID)
        {
            return false;
        }

        return true;
    }

    bool IntfsOrchTest::addVlanMember(const string &vlan_name, const string &intf_name, const string &tagging_mode = "untagged")
    {
        auto consumer = unique_ptr<Consumer>(new Consumer(new ConsumerStateTable(m_applDb.get(), APP_VLAN_MEMBER_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0), gPortsOrch, APP_VLAN_MEMBER_TABLE_NAME));
        auto vlan_member_data = deque<KeyOpFieldsValuesTuple>(
            { { vlan_name + ":" + intf_name,
                SET_COMMAND,
                { { "tagging_mode", tagging_mode } } } });

        Portal::ConsumerInternal::addToSync(consumer.get(), vlan_member_data);
        gPortsOrch->doVlanMemberTask(*consumer.get());

        Port v, p;
        if (!gPortsOrch->getPort(vlan_name, v) ||
            !gPortsOrch->getPort(intf_name, p) ||
            v.m_members.find(intf_name) == v.m_members.end())
        {
            return false;
        }

        return true;
    }

    bool IntfsOrchTest::createLag(const string &lag_name, const int mtu = 9100)
    {
        auto consumer = unique_ptr<Consumer>(new Consumer(new ConsumerStateTable(m_applDb.get(), APP_LAG_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0), gPortsOrch, APP_LAG_TABLE_NAME));
        auto lag_data = deque<KeyOpFieldsValuesTuple>(
            { { lag_name,
                SET_COMMAND,
                { { "mtu", to_string(mtu) } } } });

        Portal::ConsumerInternal::addToSync(consumer.get(), lag_data);
        gPortsOrch->doLagTask(*consumer.get());

        Port p;
        if (!gPortsOrch->getPort(lag_name, p) || p.m_lag_id == SAI_NULL_OBJECT_ID)
        {
            return false;
        }

        return true;
    }

    bool IntfsOrchTest::addLagMember(const string &lag_name, const string &intf_name)
    {
        auto consumer = unique_ptr<Consumer>(new Consumer(new ConsumerStateTable(m_applDb.get(), APP_LAG_MEMBER_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0), gPortsOrch, APP_LAG_MEMBER_TABLE_NAME));
        auto vlan_data = deque<KeyOpFieldsValuesTuple>(
            { { lag_name + ":" + intf_name,
                SET_COMMAND,
                { { "status", "enabled" } } } });

        Portal::ConsumerInternal::addToSync(consumer.get(), vlan_data);
        gPortsOrch->doLagMemberTask(*consumer.get());

        Port l, p;
        if (!gPortsOrch->getPort(lag_name, l) ||
            !gPortsOrch->getPort(intf_name, p) ||
            l.m_members.find(intf_name) == l.m_members.end())
        {
            return false;
        }

        return true;
    }

    bool IntfsOrchTest::createVxlan(const string &tunnel_name, const string &src_ip, const string &dst_ip)
    {
        VxlanTunnelOrch *vxlan_orch = gDirectory.get<VxlanTunnelOrch *>();
        auto consumer = unique_ptr<Consumer>(new Consumer(new SubscriberStateTable(m_configDb.get(), CFG_VXLAN_TUNNEL_TABLE_NAME, 1, 1), vxlan_orch, CFG_VXLAN_TUNNEL_TABLE_NAME));
        auto vxlanData = deque<KeyOpFieldsValuesTuple>(
            { { tunnel_name,
                SET_COMMAND,
                { { "src_ip", src_ip },
                  { "dst_ip", dst_ip } } } });

        Portal::ConsumerInternal::addToSync(consumer.get(), vxlanData);
        static_cast<Orch *>(vxlan_orch)->doTask(*consumer.get());

        return vxlan_orch->isTunnelExists(tunnel_name);
    }

    bool IntfsOrchTest::createVnet(const string &vnet_name, uint32_t vni, const string &vxlan_name)
    {
        VNetOrch *vnet_orch = gDirectory.get<VNetOrch *>();
        VxlanTunnelOrch *vxlan_orch = gDirectory.get<VxlanTunnelOrch *>();
        auto consumer = unique_ptr<Consumer>(new Consumer(new ConsumerStateTable(m_applDb.get(), APP_VNET_TABLE_NAME, 1, 1), vnet_orch, APP_VNET_TABLE_NAME));
        auto vnetData = deque<KeyOpFieldsValuesTuple>(
            { { vnet_name,
                SET_COMMAND,
                { { "src_mac", gMacAddress.to_string() },
                  { "vni", to_string(vni) },
                  { "vxlan_tunnel", vxlan_name } } } });

        if (!vxlan_orch->isTunnelExists(vxlan_name))
        {
            return false;
        }

        Portal::ConsumerInternal::addToSync(consumer.get(), vnetData);
        static_cast<Orch *>(vnet_orch)->doTask(*consumer.get());

        return vnet_orch->isVnetExists(vnet_name);
    }

    sai_object_id_t IntfsOrchTest::getRouterInterfaceId(const string &alias)
    {
        Port p;

        if (!gPortsOrch->getPort(alias, p))
        {
            return SAI_NULL_OBJECT_ID;
        }

        return p.m_rif_id;
    }

    sai_object_id_t IntfsOrchTest::getVirtualRouterId(sai_object_id_t rif_id)
    {
        sai_attribute_t attr;
        sai_status_t status;

        if (rif_id == SAI_NULL_OBJECT_ID)
        {
            return SAI_NULL_OBJECT_ID;
        }

        memset(&attr, 0, sizeof(sai_attribute_t));
        attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
        status = sai_router_intfs_api->get_router_interface_attribute(rif_id, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            return SAI_NULL_OBJECT_ID;
        }

        return attr.value.oid;
    }

    sai_object_id_t IntfsOrchTest::getCpuPortId()
    {
        Port cpu_port;

        gPortsOrch->getCpuPort(cpu_port);

        return cpu_port.m_port_id;
    }

    // Check route existence by getting its packet action
    bool IntfsOrchTest::isRouteExist(sai_object_id_t vr_id, const IpPrefix &p)
    {
        int packet_action;

        return getRoutePacketAction(vr_id, p, &packet_action);
    }

    bool IntfsOrchTest::getRoutePacketAction(sai_object_id_t vr_id, const IpPrefix &p, int *packet_action)
    {
        sai_attribute_t attr;
        sai_route_entry_t unicast_route_entry;
        sai_status_t status;

        unicast_route_entry.switch_id = gSwitchId;
        unicast_route_entry.vr_id = vr_id;
        copy(unicast_route_entry.destination, p);
        subnet(unicast_route_entry.destination, unicast_route_entry.destination);

        memset(&attr, 0, sizeof(sai_attribute_t));
        attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        status = sai_route_api->get_route_entry_attribute(&unicast_route_entry, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            return false;
        }

        *packet_action = attr.value.s32;
        return true;
    }

    sai_object_id_t IntfsOrchTest::getRouteNexthopId(sai_object_id_t vr_id, const IpPrefix &p)
    {
        sai_attribute_t attr;
        sai_route_entry_t unicast_route_entry;
        sai_status_t status;

        unicast_route_entry.switch_id = gSwitchId;
        unicast_route_entry.vr_id = vr_id;
        copy(unicast_route_entry.destination, p);
        subnet(unicast_route_entry.destination, unicast_route_entry.destination);

        memset(&attr, 0, sizeof(sai_attribute_t));
        attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        status = sai_route_api->get_route_entry_attribute(&unicast_route_entry, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            return SAI_NULL_OBJECT_ID;
        }

        return attr.value.oid;
    }

    bool IntfsOrchTest::getNeighborDstMac(sai_object_id_t rif_id, const IpAddress &ip, MacAddress &mac)
    {
        sai_neighbor_entry_t neighbor_entry;
        sai_attribute_t attr;
        sai_status_t status;

        neighbor_entry.rif_id = rif_id;
        neighbor_entry.switch_id = gSwitchId;
        copy(neighbor_entry.ip_address, ip);

        memset(&attr, 0, sizeof(sai_attribute_t));
        attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
        status = sai_neighbor_api->get_neighbor_entry_attribute(&neighbor_entry, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            return false;
        }

        mac = MacAddress(attr.value.mac);

        return true;
    }

    uint32_t IntfsOrchTest::getRouterIntfMtu(sai_object_id_t rif_id)
    {
        sai_attribute_t attr;
        sai_status_t status;

        memset(&attr, 0, sizeof(sai_attribute_t));
        attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
        status = sai_router_intfs_api->get_router_interface_attribute(rif_id, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            return 0;
        }

        return attr.value.u32;
    }

    // Create and remove interfaces with/without VRF/Vnet
    TEST_F(IntfsOrchTest, Interfaces_Create_Remove)
    {
        ASSERT_TRUE(createVlan("Vlan1"));
        ASSERT_TRUE(addVlanMember("Vlan1", "Ethernet1"));
        ASSERT_TRUE(createVlan("Vlan2"));
        ASSERT_TRUE(addVlanMember("Vlan2", "Ethernet2"));
        ASSERT_TRUE(createVlan("Vlan3"));
        ASSERT_TRUE(addVlanMember("Vlan3", "Ethernet3"));
        ASSERT_TRUE(createLag("PortChannel1"));
        ASSERT_TRUE(addLagMember("PortChannel1", "Ethernet4"));
        ASSERT_TRUE(createVRF("Vrf-blue"));
        ASSERT_TRUE(createVxlan("Vxlan-1", "11.11.11.1", "12.12.12.1"));
        ASSERT_TRUE(createVnet("Vnet-1", 168, "Vxlan-1"));

        auto orch = createIntfsOrch();
        auto set_data = deque<KeyOpFieldsValuesTuple>(
            {
                { "Vlan1", SET_COMMAND, {} },
                { "Vlan2", SET_COMMAND, { { "vrf_name", "Vrf-blue" } } },
                { "Vlan3", SET_COMMAND, { { "vnet_name", "Vnet-1" } } },
                { "PortChannel1", SET_COMMAND, {} },
                { "Ethernet5", SET_COMMAND, {} },
            });
        orch->doTask(set_data);

        ASSERT_TRUE(validateOrchAgentByConfOp(orch->m_intfsOrch, gPortsOrch, gVrfOrch, set_data));
        ASSERT_TRUE(validateLowerLayerDb(orch.get()));

        sai_object_id_t rif_id_v1 = getRouterInterfaceId("Vlan1");
        sai_object_id_t rif_id_v2 = getRouterInterfaceId("Vlan2");
        sai_object_id_t rif_id_v3 = getRouterInterfaceId("Vlan3");
        sai_object_id_t rif_id_lag = getRouterInterfaceId("PortChannel1");
        sai_object_id_t rif_id_eth = getRouterInterfaceId("Ethernet5");

        orch->doTask(set_data, DEL_COMMAND);

        ASSERT_TRUE(validateOrchAgentByConfOp(orch->m_intfsOrch, gPortsOrch, gVrfOrch, {}));
        ASSERT_TRUE(validateLowerLayerDb(orch.get()));

        ASSERT_EQ(getRouterInterfaceId("Vlan1"), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getRouterInterfaceId("Vlan2"), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getRouterInterfaceId("Vlan3"), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getRouterInterfaceId("PortChannel1"), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getRouterInterfaceId("Ethernet5"), SAI_NULL_OBJECT_ID);

        ASSERT_EQ(getVirtualRouterId(rif_id_v1), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getVirtualRouterId(rif_id_v2), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getVirtualRouterId(rif_id_v3), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getVirtualRouterId(rif_id_lag), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getVirtualRouterId(rif_id_eth), SAI_NULL_OBJECT_ID);
    }

    // Add and remove IP addresses on interfaces with/without VRF/Vnet
    TEST_F(IntfsOrchTest, Interfaces_Create_Remove_Ip_Addresses)
    {
        ASSERT_TRUE(createVlan("Vlan1"));
        ASSERT_TRUE(addVlanMember("Vlan1", "Ethernet1"));
        ASSERT_TRUE(createVlan("Vlan2"));
        ASSERT_TRUE(addVlanMember("Vlan2", "Ethernet2"));
        ASSERT_TRUE(createVlan("Vlan3"));
        ASSERT_TRUE(addVlanMember("Vlan3", "Ethernet3"));
        ASSERT_TRUE(createLag("PortChannel1"));
        ASSERT_TRUE(addLagMember("PortChannel1", "Ethernet4"));
        ASSERT_TRUE(createVRF("Vrf-blue"));
        ASSERT_TRUE(createVxlan("Vxlan-1", "11.11.11.1", "12.12.12.1"));
        ASSERT_TRUE(createVnet("Vnet-1", 168, "Vxlan-1"));

        auto orch = createIntfsOrch();
        auto set_data = deque<KeyOpFieldsValuesTuple>(
            {
                { "Vlan1", SET_COMMAND, {} },
                { "Vlan1:10.1.1.1/24", SET_COMMAND, {} },
                { "Vlan1:10.1.2.1/24", SET_COMMAND, {} },
                { "Vlan2", SET_COMMAND, { { "vrf_name", "Vrf-blue" } } },
                { "Vlan2:10.1.3.1/24", SET_COMMAND, { { "vrf_name", "Vrf-blue" } } },
                { "Vlan1:2002::1d9d:d699:dab5:3e77/64", SET_COMMAND, {} },
                { "Vlan2:2003::f84d:2e90:4f40:f881/64", SET_COMMAND, { { "vrf_name", "Vrf-blue" } } },
                { "Vlan3", SET_COMMAND, { { "vnet_name", "Vnet-1" } } },
                { "Vlan3:10.1.4.1/24", SET_COMMAND, { { "vnet_name", "Vnet-1" } } },
                { "PortChannel1", SET_COMMAND, {} },
                { "PortChannel1:10.1.5.1/24", SET_COMMAND, {} },
                { "Ethernet5", SET_COMMAND, {} },
                { "Ethernet5:10.1.6.1/24", SET_COMMAND, {} },
                { "lo:10.10.10.1/30", SET_COMMAND, {} },
            });

        orch->doTask(set_data);

        ASSERT_TRUE(validateOrchAgentByConfOp(orch->m_intfsOrch, gPortsOrch, gVrfOrch, set_data));
        ASSERT_TRUE(validateLowerLayerDb(orch.get()));

        sai_object_id_t rif_id_v1 = getRouterInterfaceId("Vlan1");
        sai_object_id_t rif_id_v2 = getRouterInterfaceId("Vlan2");
        sai_object_id_t rif_id_v3 = getRouterInterfaceId("Vlan3");
        sai_object_id_t rif_id_lag = getRouterInterfaceId("PortChannel1");
        sai_object_id_t rif_id_eth = getRouterInterfaceId("Ethernet5");

        orch->doTask(set_data, DEL_COMMAND);

        ASSERT_TRUE(validateOrchAgentByConfOp(orch->m_intfsOrch, gPortsOrch, gVrfOrch, {}));
        ASSERT_TRUE(validateLowerLayerDb(orch.get()));

        ASSERT_EQ(getRouterInterfaceId("Vlan1"), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getRouterInterfaceId("Vlan2"), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getRouterInterfaceId("Vlan3"), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getRouterInterfaceId("PortChannel1"), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getRouterInterfaceId("Ethernet5"), SAI_NULL_OBJECT_ID);

        ASSERT_EQ(getVirtualRouterId(rif_id_v1), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getVirtualRouterId(rif_id_v2), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getVirtualRouterId(rif_id_v3), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getVirtualRouterId(rif_id_lag), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(getVirtualRouterId(rif_id_eth), SAI_NULL_OBJECT_ID);
    }

    // Bind interface to non exist VRF
    TEST_F(IntfsOrchTest, Interface_Non_Exist_Vrf)
    {
        ASSERT_TRUE(createVlan("Vlan1"));
        ASSERT_TRUE(addVlanMember("Vlan1", "Ethernet1"));
        ASSERT_TRUE(createVRF("Vrf-blue"));

        auto orch = createIntfsOrch();
        auto set_data = deque<KeyOpFieldsValuesTuple>(
            { { "Vlan1", SET_COMMAND, { { "vrf_name", "Vrf-non-exist" } } } });
        orch->doTask(set_data);

        ASSERT_TRUE(getRouterInterfaceId("Vlan1") == SAI_NULL_OBJECT_ID);
    }

    // Bind interface to non exist Vnet
    TEST_F(IntfsOrchTest, Interface_Non_Exist_Vnet)
    {
        ASSERT_TRUE(createVlan("Vlan1"));
        ASSERT_TRUE(addVlanMember("Vlan1", "Ethernet1"));
        ASSERT_TRUE(createVxlan("Vxlan-1", "11.11.11.1", "12.12.12.1"));
        ASSERT_TRUE(createVnet("Vnet-1", 168, "Vxlan-1"));

        auto orch = createIntfsOrch();
        auto set_data = deque<KeyOpFieldsValuesTuple>(
            { { "Vlan1", SET_COMMAND, { { "vnet_name", "Vnet-non-exist" } } } });
        orch->doTask(set_data);

        ASSERT_TRUE(getRouterInterfaceId("Vlan1") == SAI_NULL_OBJECT_ID);
    }

    // Set interface MTU
    TEST_F(IntfsOrchTest, Interface_Set_Mtu)
    {
        ASSERT_TRUE(createVlan("Vlan1"));
        ASSERT_TRUE(addVlanMember("Vlan1", "Ethernet1"));

        auto orch = createIntfsOrch();
        auto set_data = deque<KeyOpFieldsValuesTuple>(
            { { "Vlan1", SET_COMMAND, {} } });

        orch->doTask(set_data);

        Port p;
        ASSERT_TRUE(gPortsOrch->getPort("Vlan1", p));

        p.m_mtu = 2019;
        static_cast<IntfsOrch *>(*orch.get())->setRouterIntfsMtu(p);

        ASSERT_EQ(getRouterIntfMtu(getRouterInterfaceId("Vlan1")), 2019);
    }

    // Increase/decrease interface reference count
    TEST_F(IntfsOrchTest, Interface_Increate_Decrease_Reference_Count)
    {
        ASSERT_TRUE(createVlan("Vlan1"));
        ASSERT_TRUE(addVlanMember("Vlan1", "Ethernet1"));

        auto orch = createIntfsOrch();
        auto set_data = deque<KeyOpFieldsValuesTuple>(
            { { "Vlan1", SET_COMMAND, {} } });

        orch->doTask(set_data);

        IntfsTable intfs_table = static_cast<IntfsOrch *>(*orch.get())->getSyncdIntfses();

        ASSERT_EQ(intfs_table["Vlan1"].ref_count, 0);

        static_cast<IntfsOrch *>(*orch.get())->increaseRouterIntfsRefCount("Vlan1");
        intfs_table = static_cast<IntfsOrch *>(*orch.get())->getSyncdIntfses();
        ASSERT_EQ(intfs_table["Vlan1"].ref_count, 1);

        static_cast<IntfsOrch *>(*orch.get())->decreaseRouterIntfsRefCount("Vlan1");
        intfs_table = static_cast<IntfsOrch *>(*orch.get())->getSyncdIntfses();
        ASSERT_EQ(intfs_table["Vlan1"].ref_count, 0);
    }
}
