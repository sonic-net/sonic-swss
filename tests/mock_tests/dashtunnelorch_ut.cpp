#include <algorithm>
#include <sstream>

#define private public
#include "dashtunnelorch.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_dash_orch_test.h"
#include "dash_api/tunnel.pb.h"
#include "table.h"

EXTERN_MOCK_FNS

namespace dashtunnelorch_test
{
    DEFINE_SAI_GENERIC_APIS_MOCK(dash_tunnel, dash_tunnel, dash_tunnel_member, dash_tunnel_next_hop)

    using namespace mock_orch_test;
    using ::testing::DoAll;
    using ::testing::InSequence;
    using ::testing::Invoke;
    using ::testing::Return;

    class DashTunnelOrchTest : public MockDashOrchTest
    {
    protected:
        void ApplySaiMock() override
        {
            INIT_SAI_API_MOCK(dash_tunnel);
            MockSaiApis();
        }

        void PostSetUp() override
        {
            CreateApplianceEntry();
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_tunnel);
        }

        dash::tunnel::Tunnel BuildTunnel(const std::vector<std::string> &endpoints,
                                        dash::route_type::EncapType encapType = dash::route_type::ENCAP_TYPE_VXLAN)
        {
            dash::tunnel::Tunnel tunnel;
            tunnel.set_encap_type(encapType);
            tunnel.set_vni(5555);
            for (const auto &endpoint : endpoints)
            {
                auto *ip = tunnel.add_endpoints();
                ip->set_ipv4(swss::IpAddress(endpoint).getV4Addr());
            }
            return tunnel;
        }

        dash::tunnel::Tunnel BuildTunnelWithUnsetEndpoints(size_t endpointCount)
        {
            dash::tunnel::Tunnel tunnel;
            tunnel.set_encap_type(dash::route_type::ENCAP_TYPE_VXLAN);
            tunnel.set_vni(5555);
            for (size_t i = 0; i < endpointCount; ++i)
            {
                tunnel.add_endpoints();
            }
            return tunnel;
        }

        std::unique_ptr<Consumer> MakeTunnelConsumer()
        {
            return std::make_unique<Consumer>(
                new swss::ConsumerStateTable(m_app_db.get(), APP_DASH_TUNNEL_TABLE_NAME),
                m_DashTunnelOrch, APP_DASH_TUNNEL_TABLE_NAME);
        }

        bool HasTunnelResult(const std::string &tunnelName)
        {
            swss::Table resultTable(m_dpu_app_state_db.get(), APP_DASH_TUNNEL_TABLE_NAME);
            std::vector<swss::FieldValueTuple> fvs;
            return resultTable.get(tunnelName, fvs);
        }

        std::string GetTunnelResult(const std::string &tunnelName)
        {
            swss::Table resultTable(m_dpu_app_state_db.get(), APP_DASH_TUNNEL_TABLE_NAME);
            std::string result;
            EXPECT_TRUE(resultTable.hget(tunnelName, "result", result));
            return result;
        }

        void ExpectTunnelResult(const std::string &tunnelName, uint32_t expectedResult)
        {
            EXPECT_EQ(GetTunnelResult(tunnelName), std::to_string(expectedResult));
        }
    };

    class DashTunnelOrchNoApplianceTest : public DashTunnelOrchTest
    {
    protected:
        void PostSetUp() override
        {
        }
    };

    TEST_F(DashTunnelOrchTest, AddRemoveTunnel)
    {
        auto tunnel = BuildTunnel({"2.2.2.2"});
        sai_object_id_t createdTunnelOid = 0x101;
        sai_object_id_t removedTunnelOid = SAI_NULL_OBJECT_ID;
        swss::IpAddress expectedEndpoint("2.2.2.2");
        swss::IpAddress expectedVip("1.1.1.1");

        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels)
            .WillOnce(Invoke([&](sai_object_id_t, uint32_t object_count, const uint32_t *attr_count,
                                 const sai_attribute_t **attr_list, sai_bulk_op_error_mode_t,
                                 sai_object_id_t *object_ids, sai_status_t *object_statuses) {
                EXPECT_EQ(object_count, 1u);
                EXPECT_EQ(attr_count[0], 5u);

                std::map<int32_t, sai_attribute_t> attrs;
                for (uint32_t i = 0; i < attr_count[0]; ++i)
                {
                    attrs[attr_list[0][i].id] = attr_list[0][i];
                }

                EXPECT_EQ(attrs[SAI_DASH_TUNNEL_ATTR_MAX_MEMBER_SIZE].value.u32, 1u);
                EXPECT_EQ(attrs[SAI_DASH_TUNNEL_ATTR_DASH_ENCAPSULATION].value.u32,
                          SAI_DASH_ENCAPSULATION_VXLAN);
                EXPECT_EQ(attrs[SAI_DASH_TUNNEL_ATTR_TUNNEL_KEY].value.u32, 5555u);
                EXPECT_EQ(attrs[SAI_DASH_TUNNEL_ATTR_SIP].value.ipaddr.addr_family,
                          SAI_IP_ADDR_FAMILY_IPV4);
                EXPECT_EQ(attrs[SAI_DASH_TUNNEL_ATTR_SIP].value.ipaddr.addr.ip4,
                          expectedVip.getV4Addr());
                EXPECT_EQ(attrs[SAI_DASH_TUNNEL_ATTR_DIP].value.ipaddr.addr_family,
                          SAI_IP_ADDR_FAMILY_IPV4);
                EXPECT_EQ(attrs[SAI_DASH_TUNNEL_ATTR_DIP].value.ipaddr.addr.ip4,
                          expectedEndpoint.getV4Addr());

                object_ids[0] = createdTunnelOid;
                object_statuses[0] = SAI_STATUS_SUCCESS;
                return SAI_STATUS_SUCCESS;
            }));
        EXPECT_CALL(*mock_sai_dash_tunnel_api, remove_dash_tunnels)
            .WillOnce(Invoke([&](uint32_t object_count, const sai_object_id_t *object_ids,
                                 sai_bulk_op_error_mode_t, sai_status_t *object_statuses) {
                EXPECT_EQ(object_count, 1u);
                removedTunnelOid = object_ids[0];
                object_statuses[0] = SAI_STATUS_SUCCESS;
                return SAI_STATUS_SUCCESS;
            }));

        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, tunnel);
        EXPECT_EQ(m_DashTunnelOrch->getTunnelOid(tunnel1), createdTunnelOid);
        ExpectTunnelResult(tunnel1, DASH_RESULT_SUCCESS);

        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, dash::tunnel::Tunnel(), false);
        EXPECT_EQ(removedTunnelOid, createdTunnelOid);
        EXPECT_EQ(m_DashTunnelOrch->getTunnelOid(tunnel1), SAI_NULL_OBJECT_ID);
        EXPECT_FALSE(HasTunnelResult(tunnel1));
    }

    TEST_F(DashTunnelOrchNoApplianceTest, AddTunnelMissingAppliance)
    {
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members).Times(0);

        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, BuildTunnel({"2.2.2.2"}));
        EXPECT_EQ(m_DashTunnelOrch->getTunnelOid(tunnel1), SAI_NULL_OBJECT_ID);
        ExpectTunnelResult(tunnel1, DASH_RESULT_FAILURE);
    }

    TEST_F(DashTunnelOrchTest, DuplicateTunnelSetClearsContextOnParseFailure)
    {
        auto consumer = MakeTunnelConsumer();
        auto invalidTunnel = BuildTunnel({"2.2.2.2"}, dash::route_type::ENCAP_TYPE_UNSPECIFIED);

        consumer->addToSync(swss::KeyOpFieldsValuesTuple(
            tunnel1, SET_COMMAND, {{"pb", invalidTunnel.SerializeAsString()}}));
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(tunnel1, SET_COMMAND, {}));

        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members).Times(0);

        m_DashTunnelOrch->doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        ExpectTunnelResult(tunnel1, DASH_RESULT_FAILURE);
    }

    TEST_F(DashTunnelOrchTest, TunnelSaiCreateFailure)
    {
        std::vector<sai_status_t> createStatuses = {SAI_STATUS_INSUFFICIENT_RESOURCES};

        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels)
            .WillOnce(Invoke([&](sai_object_id_t, uint32_t object_count, const uint32_t *,
                                 const sai_attribute_t **, sai_bulk_op_error_mode_t,
                                 sai_object_id_t *object_ids, sai_status_t *object_statuses) {
                EXPECT_EQ(object_count, 1u);
                object_ids[0] = SAI_NULL_OBJECT_ID;
                std::copy(createStatuses.begin(), createStatuses.end(), object_statuses);
                return SAI_STATUS_SUCCESS;
            }));

        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, BuildTunnel({"2.2.2.2"}));
        EXPECT_EQ(m_DashTunnelOrch->getTunnelOid(tunnel1), SAI_NULL_OBJECT_ID);
        ExpectTunnelResult(tunnel1, DASH_RESULT_FAILURE);
    }

    TEST_F(DashTunnelOrchTest, MissingProtobufTunnel)
    {
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members).Times(0);

        SetDashTableRaw(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, {}, true, true);
        ExpectTunnelResult(tunnel1, DASH_RESULT_FAILURE);
    }

    TEST_F(DashTunnelOrchTest, TunnelCreateDeleteChurn)
    {
        auto tunnel = BuildTunnel({"2.2.2.2"});

        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members).Times(0);
        for (int i = 0; i < 3; ++i)
        {
            sai_object_id_t tunnelOid = static_cast<sai_object_id_t>(0x200 + i);
            EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels)
                .WillOnce(Invoke([=](sai_object_id_t, uint32_t object_count, const uint32_t *,
                                     const sai_attribute_t **, sai_bulk_op_error_mode_t,
                                     sai_object_id_t *object_ids, sai_status_t *object_statuses) {
                    EXPECT_EQ(object_count, 1u);
                    object_ids[0] = tunnelOid;
                    object_statuses[0] = SAI_STATUS_SUCCESS;
                    return SAI_STATUS_SUCCESS;
                }));
            SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, tunnel);
            EXPECT_EQ(m_DashTunnelOrch->getTunnelOid(tunnel1), tunnelOid);

            EXPECT_CALL(*mock_sai_dash_tunnel_api, remove_dash_tunnels)
                .WillOnce(Invoke([=](uint32_t object_count, const sai_object_id_t *object_ids,
                                     sai_bulk_op_error_mode_t, sai_status_t *object_statuses) {
                    EXPECT_EQ(object_count, 1u);
                    EXPECT_EQ(object_ids[0], tunnelOid);
                    object_statuses[0] = SAI_STATUS_SUCCESS;
                    return SAI_STATUS_SUCCESS;
                }));
            SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, dash::tunnel::Tunnel(), false);
            EXPECT_EQ(m_DashTunnelOrch->getTunnelOid(tunnel1), SAI_NULL_OBJECT_ID);
        }
    }

    TEST_F(DashTunnelOrchTest, TunnelRemoveSaiInUse)
    {
        auto tunnel = BuildTunnel({"2.2.2.2"});
        sai_object_id_t tunnelOid = 0x301;

        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels)
            .WillOnce(Invoke([&](sai_object_id_t, uint32_t, const uint32_t *, const sai_attribute_t **,
                                 sai_bulk_op_error_mode_t, sai_object_id_t *object_ids,
                                 sai_status_t *object_statuses) {
                object_ids[0] = tunnelOid;
                object_statuses[0] = SAI_STATUS_SUCCESS;
                return SAI_STATUS_SUCCESS;
            }));
        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, tunnel);

        EXPECT_CALL(*mock_sai_dash_tunnel_api, remove_dash_tunnels)
            .WillOnce(Invoke([&](uint32_t, const sai_object_id_t *, sai_bulk_op_error_mode_t,
                                 sai_status_t *object_statuses) {
                object_statuses[0] = SAI_STATUS_OBJECT_IN_USE;
                return SAI_STATUS_SUCCESS;
            }));
        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, dash::tunnel::Tunnel(), false);

        EXPECT_EQ(m_DashTunnelOrch->getTunnelOid(tunnel1), tunnelOid);
        ExpectTunnelResult(tunnel1, DASH_RESULT_SUCCESS);
    }

    TEST_F(DashTunnelOrchTest, TunnelRemoveSaiFailure)
    {
        auto tunnel = BuildTunnel({"2.2.2.2"});
        sai_object_id_t tunnelOid = 0x302;

        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels)
            .WillOnce(Invoke([&](sai_object_id_t, uint32_t, const uint32_t *, const sai_attribute_t **,
                                 sai_bulk_op_error_mode_t, sai_object_id_t *object_ids,
                                 sai_status_t *object_statuses) {
                object_ids[0] = tunnelOid;
                object_statuses[0] = SAI_STATUS_SUCCESS;
                return SAI_STATUS_SUCCESS;
            }));
        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, tunnel);

        EXPECT_CALL(*mock_sai_dash_tunnel_api, remove_dash_tunnels)
            .WillOnce(Invoke([&](uint32_t, const sai_object_id_t *, sai_bulk_op_error_mode_t,
                                 sai_status_t *object_statuses) {
                object_statuses[0] = SAI_STATUS_INVALID_PARAMETER;
                return SAI_STATUS_SUCCESS;
            }));
        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, dash::tunnel::Tunnel(), false);

        EXPECT_EQ(m_DashTunnelOrch->getTunnelOid(tunnel1), tunnelOid);
        EXPECT_FALSE(HasTunnelResult(tunnel1));
    }

    TEST_F(DashTunnelOrchTest, TunnelUnknownOp)
    {
        auto tunnel = BuildTunnel({"2.2.2.2"});
        auto consumer = MakeTunnelConsumer();
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(
            tunnel1, SET_COMMAND, {{"pb", tunnel.SerializeAsString()}}));

        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members).Times(0);
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels)
            .WillOnce(Invoke([&](sai_object_id_t, uint32_t, const uint32_t *, const sai_attribute_t **,
                                 sai_bulk_op_error_mode_t, sai_object_id_t *object_ids,
                                 sai_status_t *object_statuses) {
                object_ids[0] = 0x401;
                object_statuses[0] = SAI_STATUS_SUCCESS;
                consumer->addToSync(swss::KeyOpFieldsValuesTuple("UNKNOWN_TUNNEL", "UNKNOWN", {}));
                return SAI_STATUS_SUCCESS;
            }));

        m_DashTunnelOrch->doTask(*consumer);
        ASSERT_EQ(consumer->m_toSync.size(), 1u);
        EXPECT_EQ(kfvKey(consumer->m_toSync.begin()->second), "UNKNOWN_TUNNEL");
        EXPECT_EQ(kfvOp(consumer->m_toSync.begin()->second), "UNKNOWN");
        ExpectTunnelResult(tunnel1, DASH_RESULT_SUCCESS);
        consumer->m_toSync.clear();
    }

    TEST_F(DashTunnelOrchTest, AddTunnelMultiEndpoint)
    {
        auto tunnel = BuildTunnel({"2.2.2.2", "3.3.3.3"});
        std::vector<sai_object_id_t> nhopOids = {0x501, 0x502};
        std::vector<sai_object_id_t> memberOids = {0x601, 0x602};
        sai_object_id_t tunnelOid = 0x503;

        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels)
                .WillOnce(Invoke([&](sai_object_id_t, uint32_t object_count, const uint32_t *,
                                     const sai_attribute_t **, sai_bulk_op_error_mode_t,
                                     sai_object_id_t *object_ids, sai_status_t *object_statuses) {
                    EXPECT_EQ(object_count, 1u);
                    object_ids[0] = tunnelOid;
                    object_statuses[0] = SAI_STATUS_SUCCESS;
                    return SAI_STATUS_SUCCESS;
                }));
            EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops)
                .WillOnce(Invoke([&](sai_object_id_t, uint32_t object_count, const uint32_t *,
                                     const sai_attribute_t **, sai_bulk_op_error_mode_t,
                                     sai_object_id_t *object_ids, sai_status_t *object_statuses) {
                    EXPECT_EQ(object_count, nhopOids.size());
                    std::copy(nhopOids.begin(), nhopOids.end(), object_ids);
                    std::fill(object_statuses, object_statuses + object_count, SAI_STATUS_SUCCESS);
                    return SAI_STATUS_SUCCESS;
                }));
            EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members)
                .WillOnce(Invoke([&](sai_object_id_t, uint32_t object_count, const uint32_t *,
                                     const sai_attribute_t **, sai_bulk_op_error_mode_t,
                                     sai_object_id_t *object_ids, sai_status_t *object_statuses) {
                    EXPECT_EQ(object_count, memberOids.size());
                    std::copy(memberOids.begin(), memberOids.end(), object_ids);
                    std::fill(object_statuses, object_statuses + object_count, SAI_STATUS_SUCCESS);
                    return SAI_STATUS_SUCCESS;
                }));
        }

        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, tunnel);

        EXPECT_EQ(m_DashTunnelOrch->getTunnelOid(tunnel1), tunnelOid);
        ASSERT_EQ(m_DashTunnelOrch->tunnel_table_[tunnel1].endpoints.size(), 2u);
        EXPECT_EQ(m_DashTunnelOrch->tunnel_table_[tunnel1].endpoints["2.2.2.2"].tunnel_nhop_oid, nhopOids[0]);
        EXPECT_EQ(m_DashTunnelOrch->tunnel_table_[tunnel1].endpoints["2.2.2.2"].tunnel_member_oid, memberOids[0]);
        EXPECT_EQ(m_DashTunnelOrch->tunnel_table_[tunnel1].endpoints["3.3.3.3"].tunnel_nhop_oid, nhopOids[1]);
        EXPECT_EQ(m_DashTunnelOrch->tunnel_table_[tunnel1].endpoints["3.3.3.3"].tunnel_member_oid, memberOids[1]);
        ExpectTunnelResult(tunnel1, DASH_RESULT_SUCCESS);
    }

    TEST_F(DashTunnelOrchTest, TunnelMemberCreateFailure)
    {
        auto tunnel = BuildTunnel({"2.2.2.2", "3.3.3.3"});
        std::vector<sai_object_id_t> nhopOids = {0x511, 0x512};
        std::vector<sai_object_id_t> memberOids = {SAI_NULL_OBJECT_ID, SAI_NULL_OBJECT_ID};
        std::vector<sai_status_t> memberStatuses = {SAI_STATUS_FAILURE, SAI_STATUS_FAILURE};
        sai_object_id_t tunnelOid = 0x513;

        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels)
                .WillOnce(Invoke([&](sai_object_id_t, uint32_t, const uint32_t *, const sai_attribute_t **,
                                     sai_bulk_op_error_mode_t, sai_object_id_t *object_ids,
                                     sai_status_t *object_statuses) {
                    object_ids[0] = tunnelOid;
                    object_statuses[0] = SAI_STATUS_SUCCESS;
                    return SAI_STATUS_SUCCESS;
                }));
            EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops)
                .WillOnce(Invoke([&](sai_object_id_t, uint32_t object_count, const uint32_t *,
                                     const sai_attribute_t **, sai_bulk_op_error_mode_t,
                                     sai_object_id_t *object_ids, sai_status_t *object_statuses) {
                    std::copy(nhopOids.begin(), nhopOids.end(), object_ids);
                    std::fill(object_statuses, object_statuses + object_count, SAI_STATUS_SUCCESS);
                    return SAI_STATUS_SUCCESS;
                }));
            EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members)
                .WillOnce(Invoke([&](sai_object_id_t, uint32_t object_count, const uint32_t *,
                                     const sai_attribute_t **, sai_bulk_op_error_mode_t,
                                     sai_object_id_t *object_ids, sai_status_t *object_statuses) {
                    std::copy(memberOids.begin(), memberOids.end(), object_ids);
                    std::copy(memberStatuses.begin(), memberStatuses.end(), object_statuses);
                    EXPECT_EQ(object_count, memberOids.size());
                    return SAI_STATUS_SUCCESS;
                }));
        }

        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, tunnel);

        EXPECT_EQ(m_DashTunnelOrch->tunnel_table_[tunnel1].endpoints["2.2.2.2"].tunnel_member_oid,
                  SAI_NULL_OBJECT_ID);
        EXPECT_EQ(m_DashTunnelOrch->tunnel_table_[tunnel1].endpoints["3.3.3.3"].tunnel_member_oid,
                  SAI_NULL_OBJECT_ID);
        ExpectTunnelResult(tunnel1, DASH_RESULT_SUCCESS);
    }

    TEST_F(DashTunnelOrchTest, TunnelEndpointRemovalFailure)
    {
        auto tunnel = BuildTunnel({"2.2.2.2", "3.3.3.3"});
        std::vector<sai_object_id_t> nhopOids = {0x521, 0x522};
        std::vector<sai_object_id_t> memberOids = {0x621, 0x622};
        sai_object_id_t tunnelOid = 0x523;

        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels)
                .WillOnce(Invoke([&](sai_object_id_t, uint32_t, const uint32_t *, const sai_attribute_t **,
                                     sai_bulk_op_error_mode_t, sai_object_id_t *object_ids,
                                     sai_status_t *object_statuses) {
                    object_ids[0] = tunnelOid;
                    object_statuses[0] = SAI_STATUS_SUCCESS;
                    return SAI_STATUS_SUCCESS;
                }));
            EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops)
                .WillOnce(Invoke([&](sai_object_id_t, uint32_t object_count, const uint32_t *,
                                     const sai_attribute_t **, sai_bulk_op_error_mode_t,
                                     sai_object_id_t *object_ids, sai_status_t *object_statuses) {
                    std::copy(nhopOids.begin(), nhopOids.end(), object_ids);
                    std::fill(object_statuses, object_statuses + object_count, SAI_STATUS_SUCCESS);
                    return SAI_STATUS_SUCCESS;
                }));
            EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members)
                .WillOnce(Invoke([&](sai_object_id_t, uint32_t object_count, const uint32_t *,
                                     const sai_attribute_t **, sai_bulk_op_error_mode_t,
                                     sai_object_id_t *object_ids, sai_status_t *object_statuses) {
                    std::copy(memberOids.begin(), memberOids.end(), object_ids);
                    std::fill(object_statuses, object_statuses + object_count, SAI_STATUS_SUCCESS);
                    return SAI_STATUS_SUCCESS;
                }));
        }
        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, tunnel);

        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_tunnel_api, remove_dash_tunnel_members)
                .WillOnce(Invoke([&](uint32_t object_count, const sai_object_id_t *,
                                     sai_bulk_op_error_mode_t, sai_status_t *object_statuses) {
                    EXPECT_EQ(object_count, 2u);
                    std::fill(object_statuses, object_statuses + object_count, SAI_STATUS_SUCCESS);
                    return SAI_STATUS_SUCCESS;
                }));
            EXPECT_CALL(*mock_sai_dash_tunnel_api, remove_dash_tunnels)
                .WillOnce(Invoke([&](uint32_t object_count, const sai_object_id_t *object_ids,
                                     sai_bulk_op_error_mode_t, sai_status_t *object_statuses) {
                    EXPECT_EQ(object_count, 1u);
                    EXPECT_EQ(object_ids[0], tunnelOid);
                    object_statuses[0] = SAI_STATUS_SUCCESS;
                    return SAI_STATUS_SUCCESS;
                }));
            EXPECT_CALL(*mock_sai_dash_tunnel_api, remove_dash_tunnel_next_hops)
                .WillOnce(Invoke([&](uint32_t object_count, const sai_object_id_t *,
                                     sai_bulk_op_error_mode_t, sai_status_t *object_statuses) {
                    EXPECT_EQ(object_count, 2u);
                    std::fill(object_statuses, object_statuses + object_count, SAI_STATUS_FAILURE);
                    return SAI_STATUS_SUCCESS;
                }));
        }

        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, dash::tunnel::Tunnel(), false);

        EXPECT_EQ(m_DashTunnelOrch->getTunnelOid(tunnel1), tunnelOid);
        ASSERT_EQ(m_DashTunnelOrch->tunnel_table_[tunnel1].endpoints.size(), 2u);
        // Both endpoints remain because nhop removal failed; member OIDs are cleared
        for (auto it = m_DashTunnelOrch->tunnel_table_[tunnel1].endpoints.begin();
             it != m_DashTunnelOrch->tunnel_table_[tunnel1].endpoints.end(); ++it)
        {
            EXPECT_EQ(it->second.tunnel_member_oid, SAI_NULL_OBJECT_ID);
            EXPECT_NE(it->second.tunnel_nhop_oid, SAI_NULL_OBJECT_ID);
        }
        ExpectTunnelResult(tunnel1, DASH_RESULT_SUCCESS);
    }

    TEST_F(DashTunnelOrchTest, TunnelDeletePreOpException)
    {
        DashTunnelEntry entry;
        entry.tunnel_oid = SAI_NULL_OBJECT_ID;
        m_DashTunnelOrch->tunnel_table_[tunnel1] = entry;

        auto consumer = MakeTunnelConsumer();
        consumer->addToSync(swss::KeyOpFieldsValuesTuple(tunnel1, DEL_COMMAND, {}));

        EXPECT_CALL(*mock_sai_dash_tunnel_api, remove_dash_tunnels).Times(0);

        m_DashTunnelOrch->doTask(*consumer);
        EXPECT_TRUE(consumer->m_toSync.empty());
        ExpectTunnelResult(tunnel1, DASH_RESULT_FAILURE);
    }

    TEST_F(DashTunnelOrchTest, TunnelPostProcessingException)
    {
        auto tunnel = BuildTunnelWithUnsetEndpoints(2);

        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnels)
            .WillOnce(Invoke([&](sai_object_id_t, uint32_t, const uint32_t *, const sai_attribute_t **,
                                 sai_bulk_op_error_mode_t, sai_object_id_t *object_ids,
                                 sai_status_t *object_statuses) {
                object_ids[0] = 0x701;
                object_statuses[0] = SAI_STATUS_SUCCESS;
                return SAI_STATUS_SUCCESS;
            }));
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_next_hops)
            .WillOnce(Invoke([&](sai_object_id_t, uint32_t object_count, const uint32_t *,
                                 const sai_attribute_t **, sai_bulk_op_error_mode_t,
                                 sai_object_id_t *object_ids, sai_status_t *object_statuses) {
                EXPECT_EQ(object_count, 2u);
                object_ids[0] = 0x702;
                object_ids[1] = 0x703;
                object_statuses[0] = SAI_STATUS_SUCCESS;
                object_statuses[1] = SAI_STATUS_SUCCESS;
                return SAI_STATUS_SUCCESS;
            }));
        EXPECT_CALL(*mock_sai_dash_tunnel_api, create_dash_tunnel_members).Times(0);

        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, "BROKEN_TUNNEL", tunnel);
        EXPECT_FALSE(HasTunnelResult("BROKEN_TUNNEL"));
    }
}
