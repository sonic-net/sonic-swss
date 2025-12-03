#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
#include "check.h"

EXTERN_MOCK_FNS

/* 
    This test provides a framework to mock create_acl_entry & remove_acl_entry API's
*/
namespace aclorch_rule_test
{
    DEFINE_SAI_GENERIC_API_MOCK(acl, acl_entry);
    /* To mock Redirect Action functionality */
    DEFINE_SAI_GENERIC_API_MOCK(next_hop, next_hop);
    
    using namespace ::testing;
    using namespace std;
    using namespace saimeta;
    using namespace swss;
    using namespace mock_orch_test;

    struct SaiMockState
    {
        /* Add extra attributes on demand */
        vector<sai_attribute_t> create_attrs;
        sai_status_t create_status = SAI_STATUS_SUCCESS;
        sai_status_t remove_status = SAI_STATUS_SUCCESS;
        sai_object_id_t remove_oid;
        sai_object_id_t create_oid;

        sai_status_t handleCreate(sai_object_id_t *sai, sai_object_id_t switch_id, uint32_t attr_count, const sai_attribute_t *attr_list)
        {
            *sai = create_oid;
            create_attrs.clear();
            for (uint32_t i = 0; i < attr_count; ++i)
            {
                create_attrs.emplace_back(attr_list[i]);
            }
            return create_status;
        }

        sai_status_t handleRemove(sai_object_id_t oid)
        {
            EXPECT_EQ(oid, remove_oid);
            return remove_status;
        } 
    };

    struct AclOrchRuleTest : public MockOrchTest
    {   
        unique_ptr<SaiMockState> aclMockState;

        void PostSetUp() override
        {
            INIT_SAI_API_MOCK(acl);
            INIT_SAI_API_MOCK(next_hop);
            MockSaiApis();

            aclMockState = make_unique<SaiMockState>();
            /* Port init done is a pre-req for Aclorch */
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_app_db.get(), APP_PORT_TABLE_NAME, 1, 1), gPortsOrch, APP_PORT_TABLE_NAME));
            consumer->addToSync({ { "PortInitDone", EMPTY_PREFIX, { { "", "" } } } });
            static_cast<Orch *>(gPortsOrch)->doTask(*consumer.get());
        }

        void PreTearDown() override
        {
            aclMockState.reset();
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(next_hop);
            DEINIT_SAI_API_MOCK(acl);
        }

        void doAclTableTypeTask(const deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_config_db.get(), CFG_ACL_TABLE_TYPE_TABLE_NAME, 1, 1), 
                    gAclOrch, CFG_ACL_TABLE_TYPE_TABLE_NAME));
            consumer->addToSync(entries);
            static_cast<Orch *>(gAclOrch)->doTask(*consumer);
        }

        void doAclTableTask(const deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_config_db.get(), CFG_ACL_TABLE_TABLE_NAME, 1, 1), 
                    gAclOrch, CFG_ACL_TABLE_TABLE_NAME));
            consumer->addToSync(entries);
            static_cast<Orch *>(gAclOrch)->doTask(*consumer);
        }

        void doAclRuleTask(const deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_config_db.get(), CFG_ACL_RULE_TABLE_NAME, 1, 1), 
                    gAclOrch, CFG_ACL_RULE_TABLE_NAME));
            consumer->addToSync(entries);
            static_cast<Orch *>(gAclOrch)->doTask(*consumer);
        }
    };

    struct AclRedirectActionTest : public AclOrchRuleTest
    {    
        string acl_table_type = "TEST_ACL_TABLE_TYPE";
        string acl_table = "TEST_ACL_TABLE";
        string acl_rule = "TEST_ACL_RULE";

        string mock_tunnel_name = "tunnel0";
        string mock_invalid_tunnel_name = "tunnel1";
        string mock_src_ip = "20.0.0.1";
        string mock_nh_ip_str = "20.0.0.3";
        string mock_invalid_nh_ip_str = "20.0.0.4";
        sai_object_id_t nh_oid = 0x400000000064d;

        void PostSetUp() override
        {
            AclOrchRuleTest::PostSetUp();

            /* Create a tunnel */
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME, 1, 1),
                                             m_VxlanTunnelOrch, APP_VXLAN_TUNNEL_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                            {
                                mock_tunnel_name,
                                SET_COMMAND,
                                {
                                    { "src_ip", mock_src_ip }
                                }
                            }
                    }
            ));
            static_cast<Orch2*>(m_VxlanTunnelOrch)->doTask(*consumer.get());

            populateAclTale();
            setDefaultMockState();
        }

        void PreTearDown() override
        {
            AclOrchRuleTest::PreTearDown();

            /* Delete the Tunnel Object */
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME, 1, 1),
                                             m_VxlanTunnelOrch, APP_VXLAN_TUNNEL_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                            {
                                mock_tunnel_name,
                                DEL_COMMAND,
                                { }
                            }
                    }
            ));
            static_cast<Orch2*>(m_VxlanTunnelOrch)->doTask(*consumer.get());
        }

        void createTunnelNH(string ip, uint32_t vni)
        {
            IpAddress mock_nh_ip(ip);
            ASSERT_EQ(m_VxlanTunnelOrch->createNextHopTunnel(mock_tunnel_name, mock_nh_ip, MacAddress(), vni), nh_oid);
        }

        void populateAclTale()
        {
            /* Create a Table type and Table */
            doAclTableTypeTask({
                {
                    acl_table_type,
                    SET_COMMAND,
                    {
                        { ACL_TABLE_TYPE_MATCHES, string(MATCH_DST_IP) + "," + MATCH_TUNNEL_TERM },
                        { ACL_TABLE_TYPE_ACTIONS, ACTION_REDIRECT_ACTION },
                    } 
                }
            });
            doAclTableTask({
                {
                    acl_table,
                    SET_COMMAND,
                    {
                        { ACL_TABLE_TYPE, acl_table_type },
                        { ACL_TABLE_STAGE, STAGE_INGRESS },
                    } 
                }
            });
        }

        void addTunnelNhRule(string ip, string tunnel_name, string vni)
        {
            string redirect_str = ip + "@" + tunnel_name;
            if (!vni.empty())
            {
                redirect_str = redirect_str + ',' + vni;
            }

            /* Create a rule */
            doAclRuleTask({
                {
                    acl_table + "|" + acl_rule,
                    SET_COMMAND,
                    {
                        { RULE_PRIORITY, "9999" },
                        { MATCH_DST_IP, "10.0.0.1/24" },
                        { MATCH_TUNNEL_TERM, "true" },
                        { ACTION_REDIRECT_ACTION, redirect_str }
                    } 
                }
            });
        }

        void delTunnelNhRule()
        {
            doAclRuleTask(
            {
                {
                    acl_table + "|" + acl_rule,
                    DEL_COMMAND,
                    { } 
                }
            });
        }

        void setDefaultMockState()
        {
            aclMockState->create_status = SAI_STATUS_SUCCESS;
            aclMockState->remove_status = SAI_STATUS_SUCCESS;
            aclMockState->create_oid = nh_oid;
            aclMockState->remove_oid = nh_oid;
        }
    };

    TEST_F(AclRedirectActionTest, TunnelNH)
    {
        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hop).WillOnce(DoAll(SetArgPointee<0>(nh_oid),
                Return(SAI_STATUS_SUCCESS)
        ));
        EXPECT_CALL(*mock_sai_acl_api, create_acl_entry).WillOnce(testing::Invoke(aclMockState.get(), &SaiMockState::handleCreate));     
        addTunnelNhRule(mock_nh_ip_str, mock_tunnel_name, "1000");

        /* Verify SAI attributes and if the rule is created */
        SaiAttributeList attr_list(SAI_OBJECT_TYPE_ACL_ENTRY, vector<swss::FieldValueTuple>({ 
              { "SAI_ACL_ENTRY_ATTR_TABLE_ID", sai_serialize_object_id(gAclOrch->getTableById(acl_table)) },
              { "SAI_ACL_ENTRY_ATTR_PRIORITY", "9999" },
              { "SAI_ACL_ENTRY_ATTR_ADMIN_STATE", "true" },
              { "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER", "oid:0xfffffffffff"},
              { "SAI_ACL_ENTRY_ATTR_FIELD_DST_IP", "10.0.0.1&mask:255.255.255.0"},
              { "SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_TERMINATED", "true"},
              { "SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT", sai_serialize_object_id(nh_oid) }
        }), false);
        vector<bool> skip_list = {false, false, false, true, false, false, false}; /* skip checking counter */
        ASSERT_TRUE(Check::AttrListSubset(SAI_OBJECT_TYPE_ACL_ENTRY, aclMockState->create_attrs, attr_list, skip_list));
        ASSERT_TRUE(gAclOrch->getAclRule(acl_table, acl_rule));

        /* ACLRule is deleted along with Nexthop */
        EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hop).Times(1).WillOnce(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(*mock_sai_acl_api, remove_acl_entry).WillOnce(testing::Invoke(aclMockState.get(), &SaiMockState::handleRemove));
        delTunnelNhRule();
        ASSERT_FALSE(gAclOrch->getAclRule(acl_table, acl_rule));
    }

    TEST_F(AclRedirectActionTest, TunnelNH_ExistingNhObject)
    {
        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hop).WillOnce(DoAll(SetArgPointee<0>(nh_oid),
                Return(SAI_STATUS_SUCCESS)
        ));
        EXPECT_CALL(*mock_sai_acl_api, create_acl_entry).WillOnce(testing::Invoke(aclMockState.get(), &SaiMockState::handleCreate));
        createTunnelNH(mock_nh_ip_str, 1000);
        addTunnelNhRule(mock_nh_ip_str, mock_tunnel_name, "1000");
        ASSERT_TRUE(gAclOrch->getAclRule(acl_table, acl_rule));

        /* ACL Rule is deleted but nexthop is not deleted */
        EXPECT_CALL(*mock_sai_acl_api, remove_acl_entry).WillOnce(testing::Invoke(aclMockState.get(), &SaiMockState::handleRemove));
        EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hop).Times(0);
        delTunnelNhRule();
        ASSERT_FALSE(gAclOrch->getAclRule(acl_table, acl_rule));
    }

    TEST_F(AclRedirectActionTest, TunnelNH_InvalidTunnel)
    {
        EXPECT_CALL(*mock_sai_acl_api, create_acl_entry).Times(0);
        addTunnelNhRule(mock_nh_ip_str, mock_invalid_tunnel_name, "");
        ASSERT_FALSE(gAclOrch->getAclRule(acl_table, acl_rule));
    }

    TEST_F(AclRedirectActionTest, TunnelNH_InvalidNextHop)
    {
        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hop).WillOnce(
                Return(SAI_STATUS_FAILURE) /* create next hop fails */
        );
        EXPECT_CALL(*mock_sai_acl_api, create_acl_entry).Times(0);
        addTunnelNhRule(mock_invalid_nh_ip_str, mock_tunnel_name, "");
        ASSERT_FALSE(gAclOrch->getAclRule(acl_table, acl_rule));
    }
}
